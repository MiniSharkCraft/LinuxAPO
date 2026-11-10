#include "Runtime.h"
#include "RuntimeAudioGate.h"
#include "TransitionMetricsGeneration.h"
#include "../platform/PlatformChannels.h"
#include "../platform/RealtimeAudit.h"
#include "../platform/Settings.h"
#include "DeviceManager.h"
#include "DeviceSelection.h"
#include "DefaultSinkVolumeMonitor.h"
#include "PlaybackGraph.h"
#include "Engine.h"
#include "LoudnessVolumeProvider.h"
#include "../platform/ConfigWatcher.h"
#include "helpers/StringHelper.h"
#include <pipewire/extensions/metadata.h>
#include <pipewire/keys.h>
#include <pipewire/permission.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <fcntl.h>
#include <vector>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/port-config.h>
#include <spa/param/props.h>
#include <spa/utils/result.h>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <thread>
#include <unistd.h>

namespace {
constexpr unsigned MaxFrames = 65536, MaxChannels = 8;
static_assert(std::atomic<double>::is_always_lock_free &&
                  std::atomic<uint64_t>::is_always_lock_free &&
                  std::atomic<Engine *>::is_always_lock_free &&
                  std::atomic<unsigned>::is_always_lock_free &&
                  std::atomic<bool>::is_always_lock_free,
              "Realtime atomics must be lock free");
uint64_t now() {
  timespec t{};
  clock_gettime(CLOCK_MONOTONIC, &t);
  return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}
std::string readControlRequest(int fd) {
  std::string frame;
  std::array<char, settings::ipc::MaxRequestBytes + 1> buffer{};
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  for (;;) {
    pollfd ready{fd, POLLIN, 0};
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    const int result = poll(&ready, 1, std::max<int64_t>(0, remaining.count()));
    if (result <= 0)
      throw std::runtime_error("malformed request frame (incomplete/timeout)");
    const auto count = recv(fd, buffer.data(), buffer.size(), 0);
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        continue;
      throw std::runtime_error("cannot read daemon IPC request");
    }
    if (frame.size() + static_cast<size_t>(count) >
        settings::ipc::MaxRequestBytes)
      throw std::runtime_error("request frame exceeds size limit");
    frame.append(buffer.data(), static_cast<size_t>(count));
  }
  return frame;
}
void sendControlResponse(int fd, bool ok, const std::string &payload) {
  const auto frame = settings::ipc::responseFrame(ok, payload);
  size_t sent = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (sent < frame.size()) {
    const auto count =
        send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
    if (count > 0) {
      sent += static_cast<size_t>(count);
      continue;
    }
    if (count < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
      pollfd ready{fd, POLLOUT, 0};
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      if (poll(&ready, 1, std::max<int64_t>(0, remaining.count())) > 0)
        continue;
    }
    break;
  }
}
std::string get(const spa_dict *p, const char *key) {
  auto *v = spa_dict_lookup(p, key);
  return v ? v : "";
}
struct PersistedMetadataValue {
  bool present{};
  std::string type;
  std::string value;
};
struct PersistedDefaultRoute {
  PersistedMetadataValue active;
  PersistedMetadataValue configured;
};
bool matchesDefaultMetadata(const bool present, const std::string &type,
                            const std::string &value,
                            const PersistedMetadataValue &saved) {
  if (present != saved.present)
    return false;
  if (!present)
    return true;
  if (type != saved.type)
    return false;
  if (type == "Spa:String:JSON") {
    std::string currentName;
    std::string savedName;
    if (skyapo::pipewire::DefaultSinkVolumeMonitor::parseDefaultNodeMetadata(
            value.c_str(), currentName) &&
        skyapo::pipewire::DefaultSinkVolumeMonitor::parseDefaultNodeMetadata(
            saved.value.c_str(), savedName))
      return currentName == savedName;
  }
  return value == saved.value;
}
std::optional<std::filesystem::path> defaultRouteSnapshotPath() {
  if (const char *stateHome = std::getenv("XDG_STATE_HOME");
      stateHome && *stateHome) {
    std::filesystem::path base(stateHome);
    if (base.is_absolute())
      return base / "skyapo" / "playback-default-route-v1";
  }
  if (const char *home = std::getenv("HOME"); home && *home) {
    std::filesystem::path base(home);
    if (base.is_absolute())
      return base / ".local" / "state" / "skyapo" / "playback-default-route-v1";
  }
  return std::nullopt;
}
void appendRouteSnapshotField(std::string &result, const std::string &value) {
  result += std::to_string(value.size());
  result.push_back('\n');
  result.append(value);
  result.push_back('\n');
}
bool readRouteSnapshotField(const std::string &data, size_t &offset,
                            std::string &value) {
  const size_t lineEnd = data.find('\n', offset);
  if (lineEnd == std::string::npos || lineEnd - offset > 8)
    return false;
  char *end = nullptr;
  errno = 0;
  const auto length = std::strtoull(data.c_str() + offset, &end, 10);
  if (errno || end != data.c_str() + lineEnd || length > 16384)
    return false;
  offset = lineEnd + 1;
  if (length > data.size() - offset ||
      data.size() - offset - static_cast<size_t>(length) < 1)
    return false;
  value.assign(data.data() + offset, static_cast<size_t>(length));
  offset += static_cast<size_t>(length);
  if (data[offset++] != '\n')
    return false;
  return true;
}
std::string encodeDefaultRouteSnapshot(const PersistedDefaultRoute &snapshot) {
  std::string result = "SKYAPO_PLAYBACK_ROUTE 1\n";
  for (const auto *value : {&snapshot.active, &snapshot.configured}) {
    result += value->present ? "1\n" : "0\n";
    appendRouteSnapshotField(result, value->type);
    appendRouteSnapshotField(result, value->value);
  }
  return result;
}
bool decodeDefaultRouteSnapshot(const std::string &data,
                                PersistedDefaultRoute &snapshot) {
  if (data.size() > 65536 ||
      data.compare(0, std::string("SKYAPO_PLAYBACK_ROUTE 1\n").size(),
                   "SKYAPO_PLAYBACK_ROUTE 1\n") != 0)
    return false;
  size_t offset = std::string("SKYAPO_PLAYBACK_ROUTE 1\n").size();
  for (auto *value : {&snapshot.active, &snapshot.configured}) {
    if (offset + 2 > data.size() || data[offset + 1] != '\n' ||
        (data[offset] != '0' && data[offset] != '1'))
      return false;
    value->present = data[offset] == '1';
    offset += 2;
    if (!readRouteSnapshotField(data, offset, value->type) ||
        !readRouteSnapshotField(data, offset, value->value))
      return false;
    if (!value->present && (!value->type.empty() || !value->value.empty()))
      return false;
  }
  return offset == data.size();
}
bool writeAll(int fd, const char *data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const auto count = ::write(fd, data + offset, size - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}
bool writeDefaultRouteSnapshot(const std::filesystem::path &path,
                               const PersistedDefaultRoute &snapshot,
                               std::string &error) {
  namespace fs = std::filesystem;
  const auto directory = path.parent_path();
  std::error_code fsError;
  fs::create_directories(directory, fsError);
  if (fsError || fs::is_symlink(fs::symlink_status(directory, fsError)) ||
      fsError) {
    error = "cannot safely create the playback route state directory";
    return false;
  }
  if (::chmod(directory.c_str(), 0700) < 0) {
    error = "cannot secure the playback route state directory";
    return false;
  }
  std::string pattern = path.string() + ".tmp-XXXXXX";
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  const int fd = ::mkstemp(temporary.data());
  if (fd < 0) {
    error = "cannot create the playback route state file";
    return false;
  }
  bool fdOpen = true;
  bool renamed = false;
  const auto fail = [&](const char *message) {
    error = message;
    if (fdOpen)
      ::close(fd);
    if (!renamed)
      ::unlink(temporary.data());
    return false;
  };
  if (::fchmod(fd, 0600) < 0)
    return fail("cannot secure the playback route state file");
  const std::string bytes = encodeDefaultRouteSnapshot(snapshot);
  if (!writeAll(fd, bytes.data(), bytes.size()) || ::fsync(fd) < 0)
    return fail("cannot write the playback route state file");
  const int closeResult = ::close(fd);
  fdOpen = false;
  if (closeResult < 0)
    return fail("cannot close the playback route state file");
  if (::rename(temporary.data(), path.c_str()) < 0)
    return fail("cannot atomically install the playback route state file");
  renamed = true;
  const int directoryFd =
      ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directoryFd < 0) {
    error = "cannot open the playback route state directory for sync";
    ::unlink(path.c_str());
    return false;
  }
  const int syncResult = ::fsync(directoryFd);
  ::close(directoryFd);
  if (syncResult < 0) {
    error = "cannot sync the playback route state directory";
    ::unlink(path.c_str());
    return false;
  }
  return true;
}
bool readDefaultRouteSnapshot(const std::filesystem::path &path,
                              PersistedDefaultRoute &snapshot, bool &exists,
                              std::string &error) {
  exists = false;
  struct stat pathInfo {};
  if (::lstat(path.c_str(), &pathInfo) < 0) {
    if (errno == ENOENT)
      return true;
    error = "cannot inspect the playback route state file";
    return false;
  }
  exists = true;
  if (!S_ISREG(pathInfo.st_mode) || pathInfo.st_uid != ::getuid() ||
      (pathInfo.st_mode & 0077) != 0 || pathInfo.st_size < 0 ||
      pathInfo.st_size > 65536) {
    error =
        "playback route state file has unsafe ownership, permissions, or size";
    return false;
  }
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    error = "cannot open the playback route state file safely";
    return false;
  }
  struct stat fileInfo {};
  if (::fstat(fd, &fileInfo) < 0 || !S_ISREG(fileInfo.st_mode) ||
      fileInfo.st_uid != ::getuid() || (fileInfo.st_mode & 0077) != 0 ||
      fileInfo.st_size != pathInfo.st_size) {
    ::close(fd);
    error = "playback route state file changed while opening";
    return false;
  }
  std::string data(static_cast<size_t>(fileInfo.st_size), '\0');
  size_t offset = 0;
  while (offset < data.size()) {
    const auto count = ::read(fd, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      ::close(fd);
      error = "cannot read the playback route state file";
      return false;
    }
    offset += static_cast<size_t>(count);
  }
  ::close(fd);
  if (!decodeDefaultRouteSnapshot(data, snapshot)) {
    error = "playback route state file is malformed or unsupported";
    return false;
  }
  return true;
}
struct Runtime {
  volatile sig_atomic_t &stopping;
  AudioDevice device;
  std::string config, error, path;
  std::vector<AudioPort> ports;
  pw_main_loop *main{};
  pw_context *context{};
  pw_core *core{};
  pw_registry *registry{};
  pw_metadata *defaultMetadata{};
  uint32_t defaultMetadataId{SPA_ID_INVALID};
  uint32_t defaultMetadataPermissions{};
  spa_hook defaultMetadataHook{};
  pw_filter *filter{};
  spa_hook coreHook{}, registryHook{}, filterHook{};
  spa_source *rateEvent{}, *transitionEvent{}, *statusEvent{},
      *pluginLatencyEvent{}, *processFaultEvent{}, *sigint{}, *sigterm{},
      *selectionTimer{}, *playbackRecoveryTimer{}, *defaultRouteTimer{},
      *configEvent{}, *reloadTimer{}, *linkRetryTimer{},
      *virtualMicActivationTimer{};
  std::unique_ptr<skyapo::platform::ConfigWatcher> configWatcher;
  int server = -1;
  bool ownsSocket = false;
  bool cleaning = false;
  bool initialized = false;
  using AudioMode = skyapo::pipewire::RuntimeAudioMode;
  std::array<void *, MaxChannels> inputs{}, outputs{};
  std::array<uint32_t, MaxChannels> ownInputs{};
  std::array<pw_proxy *, MaxChannels> links{};
  struct LinkContext {
    Runtime *owner;
    unsigned channel;
  };
  std::array<LinkContext, MaxChannels> linkContexts{};
  std::array<spa_hook, MaxChannels> linkHooks{};
  std::array<uint32_t, MaxChannels> linkGlobalIds{};
  std::array<bool, MaxChannels> linked{};
  bool outputPortConfigPending{};
  bool wirePlumberPresent{};
  bool virtualMicConfiguredByPolicy{};
  bool virtualMicActive{};
  std::vector<float> work;
  std::unique_ptr<Engine> activeEngine;
  std::unique_ptr<Engine> pendingEngine;
  std::vector<std::unique_ptr<Engine>> retiredEngines;
  std::shared_ptr<const ConfigSourceSnapshot> lastGoodConfigSources;
  std::unique_ptr<skyapo::pipewire::DefaultSinkVolumeMonitor>
      renderVolumeMonitor;
  std::unique_ptr<skyapo::pipewire::PlaybackGraph> playbackGraph;
  bool defaultMetadataSnapshotReady{};
  std::optional<std::filesystem::path> defaultRouteSnapshotFile;
  bool defaultRouteSnapshotChecked{};
  bool defaultMetadataPresent{};
  bool configuredDefaultMetadataPresent{};
  bool originalDefaultMetadataCaptured{};
  bool originalConfiguredDefaultCaptured{};
  bool defaultRouteActiveConfirmed{};
  bool defaultRouteConfiguredConfirmed{};
  bool defaultRouteSyncConfirmed{};
  bool defaultRouteAttempted{};
  bool defaultRoutePending{};
  bool defaultRouteOwned{};
  // Route metadata may have been changed before ownership is confirmed. Keep
  // cleanup eligibility separate so shutdown can safely undo our own writes.
  bool defaultRouteRestoreEligible{};
  bool defaultRouteSetRequested{};
  bool defaultRouteRestoring{};
  bool defaultRouteOverridden{};
  bool defaultRouteRecoveryPending{};
  int nextCoreSequence{};
  int defaultMetadataSnapshotSequence{-1};
  int defaultRouteSequence{-1};
  int defaultRestoreSequence{-1};
  int lastCoreDoneSequence{-1};
  std::string defaultMetadataType;
  std::string defaultMetadataValue;
  std::string defaultMetadataName;
  std::string configuredDefaultMetadataType;
  std::string configuredDefaultMetadataValue;
  std::string configuredDefaultMetadataName;
  std::chrono::steady_clock::time_point defaultRouteLastWrite{};
  std::chrono::steady_clock::time_point defaultRouteLastMetadataChange{};
  std::chrono::steady_clock::time_point defaultRouteConfirmationDeadline{};
  std::chrono::steady_clock::time_point defaultRouteRecoveryReadySince{};
  std::string originalDefaultMetadataType;
  std::string originalDefaultMetadataValue;
  bool originalDefaultMetadataPresent{};
  std::string originalConfiguredDefaultMetadataType;
  std::string originalConfiguredDefaultMetadataValue;
  bool originalConfiguredDefaultMetadataPresent{};
  std::string defaultRouteError;
  settings::DeviceSelection outputSelection;
  std::string playbackError;
  skyapo::pipewire::DefaultSinkVolumeSnapshot renderVolume;
  std::atomic<Engine *> active{nullptr};
  std::atomic<Engine *> pending{nullptr};
  std::atomic<unsigned> callbacksInFlight{0};
  std::atomic<AudioMode> audioMode{AudioMode::Running};
  // Fault is sticky across temporary quiesce/resume operations and format
  // rebuilds. Only an explicitly validated config reload clears it.
  std::atomic<bool> pluginLatencyFaulted{false};
  std::atomic<bool> callbackProcessingFaulted{false};
  std::atomic<bool> processFaultEventQueued{false};
  std::atomic<bool> transitionComplete{false};
  std::atomic<bool> pluginLatencyEventQueued{false};
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
  std::atomic<bool> testFormatRebuildDeferred{false};
  bool testLatencyDrainFailureInjected{};
  bool testDefaultRouteSyncFailureInjected{};
#endif
  // A queued completion event can outlive its graph; this prevents it from
  // resetting telemetry for a more recently installed graph.
  skyapo::pipewire::TransitionMetricsGeneration transitionMetrics;
  unsigned transitionCounter{};
  unsigned transitionLength{};
  unsigned transitionDurationMs{10};
  uint64_t formatRebuildsDuringTransition{};
  unsigned pluginLatencyRefreshes{};
  std::atomic<bool> resetMetrics{false};
  std::atomic<unsigned> rate{0}, quantum{0}, requestedRate{0},
      requestedQuantum{0};
  std::atomic<uint64_t> blocks{0}, samples{0}, totalNs{0}, maxNs{0},
      overruns{0};
  std::atomic<double> rawEnergy{0}, processedEnergy{0};
  double rawSum = 0, processedSum = 0;
  unsigned channels = 0;
  std::string configError;
  std::string pluginLatencyError;
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
  bool testThrowProcessExceptionOnce{};
  bool testThrowProcessExceptionAfterTransition{};
#endif
  pw_filter_state state = PW_FILTER_STATE_UNCONNECTED;
  explicit Runtime(volatile sig_atomic_t &stop) : stopping(stop) {
    defaultRouteSnapshotFile = defaultRouteSnapshotPath();
    ownInputs.fill(SPA_ID_INVALID);
    linkGlobalIds.fill(SPA_ID_INVALID);
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    if (const char *value = std::getenv("SKYAPO_TEST_TRANSITION_MS")) {
      char *end = nullptr;
      errno = 0;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (errno || end == value || *end != '\0' || parsed < 10 || parsed > 5000)
        throw std::runtime_error(
            "SKYAPO_TEST_TRANSITION_MS must be an integer from 10 to 5000");
      transitionDurationMs = static_cast<unsigned>(parsed);
    }
    if (const char *value =
            std::getenv("SKYAPO_TEST_THROW_PROCESS_EXCEPTION_ONCE")) {
      if (value[0] == '1' && value[1] == '\0') {
        testThrowProcessExceptionOnce = true;
      } else if (std::strcmp(value, "after-transition") == 0) {
        testThrowProcessExceptionOnce = true;
        testThrowProcessExceptionAfterTransition = true;
      } else {
        throw std::runtime_error(
            "SKYAPO_TEST_THROW_PROCESS_EXCEPTION_ONCE must be 1 or "
            "after-transition");
      }
    }
#endif
  }
  void stopStatusServer() noexcept {
    if (main && statusEvent) {
      pw_loop_destroy_source(pw_main_loop_get_loop(main), statusEvent);
      statusEvent = nullptr;
    }
    if (server >= 0) {
      close(server);
      server = -1;
    }
    if (ownsSocket) {
      unlink(path.c_str());
      ownsSocket = false;
    }
  }
  ~Runtime() {
    cleaning = true;
    // Route restoration synchronously iterates the PipeWire loop. Stop status
    // requests first so they cannot inspect dead PipeWire proxies while the
    // core sync is being drained during teardown after a server restart.
    stopStatusServer();
    skyapo::platform::LoudnessVolumeProvider::publish(false, 0.0f);
    // Release registry-bound proxies/listeners while their core is still live.
    restoreDefaultRouteAndWait();
    playbackGraph.reset();
    renderVolumeMonitor.reset();
    if (defaultMetadata) {
      spa_hook_remove(&defaultMetadataHook);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(defaultMetadata));
      defaultMetadata = nullptr;
    }
    if (filter)
      pw_filter_destroy(filter);
    for (auto *link : links)
      if (link)
        pw_proxy_destroy(link);
    if (registry)
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    if (core)
      pw_core_disconnect(core);
    if (main) {
      auto *l = pw_main_loop_get_loop(main);
      for (auto *s :
           {rateEvent, transitionEvent, statusEvent, pluginLatencyEvent,
            processFaultEvent, sigint, sigterm, selectionTimer,
            playbackRecoveryTimer, defaultRouteTimer, configEvent, reloadTimer,
            linkRetryTimer, virtualMicActivationTimer})
        if (s)
          pw_loop_destroy_source(l, s);
    }
    if (context)
      pw_context_destroy(context);
    if (main)
      pw_main_loop_destroy(main);
    if (initialized)
      pw_deinit();
  }
  void fail(const std::string &e) {
    error = e;
    std::cerr << "skyapod: fatal PipeWire runtime error: " << e << '\n';
    pw_main_loop_quit(main);
  }
  int requestCoreSync() {
    if (!core)
      return -ENOTCONN;
    const int requested = ++nextCoreSequence;
    return pw_core_sync(core, PW_ID_CORE, requested);
  }
  void removeDefaultRouteSnapshot() noexcept {
    if (!defaultRouteSnapshotFile)
      return;
    if (::unlink(defaultRouteSnapshotFile->c_str()) < 0 && errno != ENOENT) {
      std::cerr << "skyapod: cannot remove stale playback route state: "
                << std::strerror(errno) << '\n';
      return;
    }
    const int directoryFd =
        ::open(defaultRouteSnapshotFile->parent_path().c_str(),
               O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directoryFd >= 0) {
      if (::fsync(directoryFd) < 0)
        std::cerr << "skyapod: cannot sync playback route state removal: "
                  << std::strerror(errno) << '\n';
      ::close(directoryFd);
    }
  }
  PersistedDefaultRoute currentOriginalDefaultRoute() const {
    PersistedDefaultRoute snapshot;
    snapshot.active = {originalDefaultMetadataPresent,
                       originalDefaultMetadataType,
                       originalDefaultMetadataValue};
    snapshot.configured = {originalConfiguredDefaultMetadataPresent,
                           originalConfiguredDefaultMetadataType,
                           originalConfiguredDefaultMetadataValue};
    return snapshot;
  }
  bool recoverPersistedDefaultRoute() {
    if (defaultRouteSnapshotChecked)
      return false;
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    const char *holdRecoveryFile =
        std::getenv("SKYAPO_TEST_HOLD_DEFAULT_ROUTE_RECOVERY_FILE");
    if (holdRecoveryFile && *holdRecoveryFile &&
        ::access(holdRecoveryFile, F_OK) == 0)
      return true;
#endif
    defaultRouteSnapshotChecked = true;
    if (!defaultRouteSnapshotFile)
      return false;

    PersistedDefaultRoute snapshot;
    bool exists = false;
    std::string readError;
    if (!readDefaultRouteSnapshot(*defaultRouteSnapshotFile, snapshot, exists,
                                  readError)) {
      std::cerr << "skyapod: ignoring playback route recovery state: "
                << readError << '\n';
      if (exists)
        removeDefaultRouteSnapshot();
      return false;
    }
    if (!exists)
      return false;

    const bool activeSkyApo = defaultMetadataName == "skyapo.playback";
    const bool configuredSkyApo =
        wirePlumberPresent &&
        configuredDefaultMetadataName == "skyapo.playback";
    const bool activeOriginal =
        matchesDefaultMetadata(defaultMetadataPresent, defaultMetadataType,
                               defaultMetadataValue, snapshot.active);
    const bool configuredOriginal = matchesDefaultMetadata(
        configuredDefaultMetadataPresent, configuredDefaultMetadataType,
        configuredDefaultMetadataValue, snapshot.configured);
    const bool configuredCompatible =
        !wirePlumberPresent || configuredSkyApo || configuredOriginal;
    if (!activeSkyApo && !configuredSkyApo && activeOriginal &&
        (!wirePlumberPresent || configuredOriginal)) {
      // The session manager restored the saved route while SkyAPO was down.
      // Discard the stale lease and let this daemon claim the same original
      // route again, taking a fresh snapshot.
      removeDefaultRouteSnapshot();
      return false;
    }
    if ((!activeSkyApo && !activeOriginal) || !configuredCompatible) {
      // At least one default changed to a value that is neither the saved
      // original nor SkyAPO while the daemon was down. Do not overwrite it.
      removeDefaultRouteSnapshot();
      defaultRouteAttempted = true;
      defaultRouteOverridden = true;
      defaultRouteError = "playback default changed while SkyAPO was stopped; "
                          "leaving it untouched (active='" +
                          defaultMetadataName + "', configured='" +
                          configuredDefaultMetadataName + "')";
      std::cerr << "skyapod: " << defaultRouteError << '\n';
      return true;
    }

    originalDefaultMetadataPresent = snapshot.active.present;
    originalDefaultMetadataType = snapshot.active.type;
    originalDefaultMetadataValue = snapshot.active.value;
    originalDefaultMetadataCaptured = true;
    originalConfiguredDefaultMetadataPresent = snapshot.configured.present;
    originalConfiguredDefaultMetadataType = snapshot.configured.type;
    originalConfiguredDefaultMetadataValue = snapshot.configured.value;
    originalConfiguredDefaultCaptured = true;
    defaultRouteAttempted = true;
    defaultRouteRestoreEligible = true;
    defaultRouteActiveConfirmed = activeSkyApo;
    defaultRouteConfiguredConfirmed = !wirePlumberPresent || configuredSkyApo;
    if (!PW_PERM_IS_W(defaultMetadataPermissions)) {
      defaultRouteOwned = false;
      defaultRoutePending = false;
      defaultRouteSetRequested = false;
      defaultRouteError =
          "PipeWire default metadata is not writable; playback route "
          "recovery remains inactive";
      std::cerr << "skyapod: " << defaultRouteError << '\n';
      return true;
    }
    if (defaultRouteActiveConfirmed && defaultRouteConfiguredConfirmed) {
      defaultRouteOwned = true;
      defaultRouteError.clear();
      std::cerr << "skyapod: recovered playback route ownership from the "
                   "saved pre-crash default\n";
      return true;
    }

    defaultRouteOwned = false;
    std::cerr << "skyapod: completing playback route recovery from the "
                 "saved pre-crash default\n";
    reassertOwnedDefaultRoute();
    return true;
  }
  static int metadataProperty(void *data, uint32_t subject, const char *key,
                              const char *type, const char *value) {
    auto &r = *static_cast<Runtime *>(data);
    if (subject != PW_ID_CORE || !key)
      return 0;
    const bool configuredDefault =
        std::strcmp(key, "default.configured.audio.sink") == 0;
    if (!configuredDefault && std::strcmp(key, "default.audio.sink") != 0)
      return 0;
    if (configuredDefault)
      r.wirePlumberPresent = true;
    auto &metadataPresent = configuredDefault
                                ? r.configuredDefaultMetadataPresent
                                : r.defaultMetadataPresent;
    auto &metadataType = configuredDefault ? r.configuredDefaultMetadataType
                                           : r.defaultMetadataType;
    auto &metadataValue = configuredDefault ? r.configuredDefaultMetadataValue
                                            : r.defaultMetadataValue;
    auto &metadataName = configuredDefault ? r.configuredDefaultMetadataName
                                           : r.defaultMetadataName;
    const auto keyName = configuredDefault ? "default.configured.audio.sink"
                                           : "default.audio.sink";
    const auto previousName = metadataName;
    metadataPresent = value != nullptr;
    metadataType = type ? type : "";
    metadataValue = value ? value : "";
    std::string name;
    metadataName = value && skyapo::pipewire::DefaultSinkVolumeMonitor::
                                parseDefaultNodeMetadata(value, name)
                       ? std::move(name)
                       : std::string{};
    if (metadataName != previousName)
      std::cerr << "skyapod: PipeWire " << keyName << " changed from '"
                << previousName << "' to '" << metadataName
                << "' (route-owned=" << r.defaultRouteOwned
                << ", route-write-pending=" << r.defaultRouteSetRequested
                << ")\n";

    if (r.defaultRouteSetRequested) {
      r.defaultRouteLastMetadataChange = std::chrono::steady_clock::now();
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
      const char *suppressRouteConfirmation =
          std::getenv("SKYAPO_TEST_SUPPRESS_DEFAULT_ROUTE_CONFIRMATION");
      const bool suppressConfirmation =
          suppressRouteConfirmation &&
          std::strcmp(suppressRouteConfirmation, "1") == 0;
#else
      constexpr bool suppressConfirmation = false;
#endif
      if (!suppressConfirmation) {
        if (configuredDefault)
          r.defaultRouteConfiguredConfirmed = metadataName == "skyapo.playback";
        else
          r.defaultRouteActiveConfirmed = metadataName == "skyapo.playback";
      }
      r.confirmDefaultRouteOwnership();
      if (r.defaultRouteSetRequested && r.defaultRouteSyncConfirmed &&
          r.defaultRouteTimer) {
        timespec delay{0, 100000000};
        pw_loop_update_timer(pw_main_loop_get_loop(r.main), r.defaultRouteTimer,
                             &delay, nullptr, false);
      }
    }

    if (configuredDefault) {
      if (r.defaultRouteOwned && !r.defaultRouteRestoring &&
          metadataName != "skyapo.playback") {
        const bool routeWriteSettling =
            r.defaultRouteLastWrite.time_since_epoch().count() != 0 &&
            std::chrono::steady_clock::now() - r.defaultRouteLastWrite <
                std::chrono::seconds(1);
        const bool playbackUnavailable =
            r.playbackGraph && !r.playbackGraph->ready();
        if (playbackUnavailable || r.defaultRouteRecoveryPending ||
            routeWriteSettling) {
          if (playbackUnavailable) {
            r.defaultRouteRecoveryPending = true;
            r.defaultRouteRecoveryReadySince = {};
          }
          std::cerr << "skyapod: retaining SkyAPO as WirePlumber's configured "
                       "playback route while the graph settles\n";
          r.reassertOwnedDefaultRoute();
          if (r.defaultRouteRecoveryPending)
            r.scheduleDefaultRouteRecoveryCheck();
          return 0;
        }
        r.defaultRouteOwned = false;
        r.defaultRouteSetRequested = false;
        r.defaultRouteOverridden = true;
      }
      return 0;
    }

    if (r.defaultRouteOwned && !r.defaultRouteRestoring &&
        r.defaultMetadataName != "skyapo.playback") {
      const bool routeWriteSettling =
          r.defaultRouteLastWrite.time_since_epoch().count() != 0 &&
          std::chrono::steady_clock::now() - r.defaultRouteLastWrite <
              std::chrono::seconds(1);
      const bool playbackUnavailable =
          r.playbackGraph && !r.playbackGraph->ready();
      if (playbackUnavailable) {
        r.defaultRouteRecoveryPending = true;
        r.defaultRouteRecoveryReadySince = {};
      }
      if (playbackUnavailable || r.defaultRouteRecoveryPending ||
          routeWriteSettling) {
        std::cerr << "skyapod: retaining SkyAPO as the default playback route "
                     "while PipeWire/WirePlumber settles its route\n";
        r.reassertOwnedDefaultRoute();
        if (r.defaultRouteRecoveryPending)
          r.scheduleDefaultRouteRecoveryCheck();
        return 0;
      }
      r.defaultRouteOwned = false;
      r.defaultRouteSetRequested = false;
      r.defaultRouteOverridden = true;
    }
    if (!r.playbackGraph)
      r.schedulePlaybackRecovery();
    return 0;
  }
  void schedulePlaybackRecovery() {
    if (playbackGraph || cleaning || stopping || !playbackRecoveryTimer ||
        !main)
      return;
    timespec delay{0, 50000000};
    pw_loop_update_timer(pw_main_loop_get_loop(main), playbackRecoveryTimer,
                         &delay, nullptr, false);
  }
  static void coreDone(void *data, uint32_t id, int sequence) {
    auto &r = *static_cast<Runtime *>(data);
    if (id != PW_ID_CORE)
      return;
    r.lastCoreDoneSequence = sequence;
    if (r.defaultMetadataSnapshotSequence == sequence) {
      r.defaultMetadataSnapshotReady = true;
      r.defaultMetadataSnapshotSequence = -1;
      r.scheduleDefaultRouteUpdate();
    }
    if (r.defaultRouteSequence == sequence) {
      // The core sync can arrive before the metadata subscription delivers
      // the writes. Keep the route pending until both sources confirm it.
      r.defaultRouteSyncConfirmed = true;
      r.defaultRouteSequence = -1;
      r.confirmDefaultRouteOwnership();
      if (r.defaultRouteSetRequested && r.defaultRouteTimer) {
        timespec delay{0, 100000000};
        pw_loop_update_timer(pw_main_loop_get_loop(r.main), r.defaultRouteTimer,
                             &delay, nullptr, false);
      }
    }
    if (r.defaultRestoreSequence == sequence)
      r.defaultRestoreSequence = -1;
  }
  void confirmDefaultRouteOwnership() {
    if (!defaultRouteSetRequested || !defaultRouteSyncConfirmed ||
        !defaultRouteActiveConfirmed || !defaultRouteConfiguredConfirmed)
      return;
    if (std::chrono::steady_clock::now() - defaultRouteLastMetadataChange <
        std::chrono::milliseconds(100))
      return;
    defaultRoutePending = false;
    defaultRouteSetRequested = false;
    defaultRouteOwned = true;
    defaultRouteError.clear();
    std::cerr << "skyapod: PipeWire and WirePlumber confirmed SkyAPO as the "
                 "default playback route\n";
  }
  void waitForCoreSync(int sequence) {
    if (sequence < 0 || !main)
      return;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (lastCoreDoneSequence != sequence &&
           std::chrono::steady_clock::now() < deadline) {
      pw_loop_iterate(pw_main_loop_get_loop(main), 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
  void updateDefaultRoute() {
    if (!playbackGraph || !playbackGraph->ready() || !defaultMetadata ||
        !defaultMetadataSnapshotReady || defaultRouteAttempted ||
        defaultRoutePending || defaultRouteOwned || defaultRouteOverridden)
      return;

    if (recoverPersistedDefaultRoute())
      return;

    if (!originalDefaultMetadataCaptured) {
      originalDefaultMetadataPresent = defaultMetadataPresent;
      originalDefaultMetadataType = defaultMetadataType;
      originalDefaultMetadataValue = defaultMetadataValue;
      originalDefaultMetadataCaptured = true;
    }
    if (wirePlumberPresent && !originalConfiguredDefaultCaptured) {
      originalConfiguredDefaultMetadataPresent =
          configuredDefaultMetadataPresent;
      originalConfiguredDefaultMetadataType = configuredDefaultMetadataType;
      originalConfiguredDefaultMetadataValue = configuredDefaultMetadataValue;
      originalConfiguredDefaultCaptured = true;
    }

    const bool activeIsSkyApo = defaultMetadataName == "skyapo.playback";
    const bool configuredIsSkyApo =
        !wirePlumberPresent ||
        configuredDefaultMetadataName == "skyapo.playback";
    if (activeIsSkyApo && configuredIsSkyApo) {
      defaultRouteAttempted = true;
      defaultRouteError =
          "SkyAPO Playback is already the system default; this daemon did not "
          "claim the route and will not change it on shutdown";
      return;
    }

    defaultRouteAttempted = true;
    if (!PW_PERM_IS_W(defaultMetadataPermissions)) {
      defaultRouteError =
          "PipeWire default metadata is not writable; ordinary application "
          "playback remains on the existing default sink";
      std::cerr << "skyapod: " << defaultRouteError << '\n';
      return;
    }

    if (!defaultRouteSnapshotFile) {
      defaultRouteError =
          "cannot locate an XDG state directory; leaving the playback default "
          "unchanged because crash recovery cannot be saved";
      std::cerr << "skyapod: " << defaultRouteError << '\n';
      return;
    }
    std::string snapshotError;
    if (!writeDefaultRouteSnapshot(*defaultRouteSnapshotFile,
                                   currentOriginalDefaultRoute(),
                                   snapshotError)) {
      defaultRouteError =
          snapshotError + "; leaving the playback default unchanged";
      std::cerr << "skyapod: " << defaultRouteError << '\n';
      return;
    }

    reassertOwnedDefaultRoute();
  }
  void reassertOwnedDefaultRoute() {
    if (!defaultMetadata)
      return;
    defaultRouteOwned = false;
    defaultRouteSyncConfirmed = false;
    defaultRouteLastMetadataChange = std::chrono::steady_clock::now();
    defaultRouteConfirmationDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const bool previousDefaultPresent = defaultMetadataPresent;
    const std::string previousDefaultType = defaultMetadataType;
    const std::string previousDefaultValue = defaultMetadataValue;
    defaultRoutePending = true;
    defaultRouteSetRequested = true;
    defaultRouteActiveConfirmed = defaultMetadataName == "skyapo.playback";
    defaultRouteConfiguredConfirmed =
        !wirePlumberPresent ||
        configuredDefaultMetadataName == "skyapo.playback";
    const int result = pw_metadata_set_property(
        defaultMetadata, PW_ID_CORE, "default.audio.sink", "Spa:String:JSON",
        "{\"name\":\"skyapo.playback\"}");
    if (result < 0) {
      defaultRoutePending = false;
      defaultRouteSetRequested = false;
      defaultRouteOwned = false;
      defaultRouteError =
          std::string("cannot set PipeWire default sink metadata: ") +
          spa_strerror(result);
      std::cerr << "skyapod: " << defaultRouteError << '\n';
      return;
    }
    defaultRouteRestoreEligible = true;
    if (wirePlumberPresent) {
      int configuredResult = 0;
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
      const char *failConfiguredWrite =
          std::getenv("SKYAPO_TEST_FAIL_CONFIGURED_DEFAULT_WRITE");
      const char *failConfiguredWriteFile =
          std::getenv("SKYAPO_TEST_FAIL_CONFIGURED_DEFAULT_WRITE_FILE");
      const bool failConfiguredWriteOnce =
          failConfiguredWriteFile && *failConfiguredWriteFile &&
          ::access(failConfiguredWriteFile, F_OK) == 0;
      if (failConfiguredWriteOnce) {
        ::unlink(failConfiguredWriteFile);
        configuredResult = -EIO;
      } else if (failConfiguredWrite &&
                 std::strcmp(failConfiguredWrite, "1") == 0) {
        configuredResult = -EIO;
      } else
#endif
        configuredResult = pw_metadata_set_property(
            defaultMetadata, PW_ID_CORE, "default.configured.audio.sink",
            "Spa:String:JSON", "{\"name\":\"skyapo.playback\"}");
      if (configuredResult < 0) {
        defaultRoutePending = false;
        defaultRouteSetRequested = false;
        defaultRouteOwned = false;
        const std::string configuredError =
            std::string("cannot update WirePlumber's configured playback "
                        "default: ") +
            spa_strerror(configuredResult);
        const int rollbackResult = pw_metadata_set_property(
            defaultMetadata, PW_ID_CORE, "default.audio.sink",
            previousDefaultPresent ? previousDefaultType.c_str() : nullptr,
            previousDefaultPresent ? previousDefaultValue.c_str() : nullptr);
        if (rollbackResult < 0) {
          defaultRouteError = configuredError +
                              "; cannot restore previous PipeWire default: " +
                              spa_strerror(rollbackResult);
        } else {
          defaultRouteError = configuredError +
                              "; requested restoration of the previous "
                              "PipeWire default";
          const int syncResult = requestCoreSync();
          if (syncResult < 0)
            defaultRouteError +=
                "; cannot confirm PipeWire default rollback: " +
                std::string(spa_strerror(syncResult));
        }
        std::cerr << "skyapod: " << defaultRouteError << '\n';
        return;
      }
      std::cerr << "skyapod: requesting WirePlumber configured default sink "
                   "skyapo.playback\n";
    }
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    const char *revertActiveBeforeConfirm =
        std::getenv("SKYAPO_TEST_REVERT_ACTIVE_BEFORE_ROUTE_SYNC");
    if (revertActiveBeforeConfirm &&
        std::strcmp(revertActiveBeforeConfirm, "1") == 0) {
      const int revertResult = pw_metadata_set_property(
          defaultMetadata, PW_ID_CORE, "default.audio.sink",
          previousDefaultPresent ? previousDefaultType.c_str() : nullptr,
          previousDefaultPresent ? previousDefaultValue.c_str() : nullptr);
      if (revertResult < 0) {
        defaultRoutePending = false;
        defaultRouteSetRequested = false;
        defaultRouteError = std::string("test route override failed: ") +
                            spa_strerror(revertResult);
        return;
      }
      if (wirePlumberPresent) {
        const int configuredRevertResult = pw_metadata_set_property(
            defaultMetadata, PW_ID_CORE, "default.configured.audio.sink",
            originalConfiguredDefaultMetadataPresent
                ? originalConfiguredDefaultMetadataType.c_str()
                : nullptr,
            originalConfiguredDefaultMetadataPresent
                ? originalConfiguredDefaultMetadataValue.c_str()
                : nullptr);
        if (configuredRevertResult < 0) {
          defaultRoutePending = false;
          defaultRouteSetRequested = false;
          defaultRouteError =
              std::string("test configured-route override failed: ") +
              spa_strerror(configuredRevertResult);
          return;
        }
      }
    }
#endif
    defaultRouteLastWrite = std::chrono::steady_clock::now();

#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    const char *failRouteSync =
        std::getenv("SKYAPO_TEST_FAIL_DEFAULT_ROUTE_SYNC_ONCE");
    if (!testDefaultRouteSyncFailureInjected && failRouteSync &&
        std::strcmp(failRouteSync, "1") == 0) {
      testDefaultRouteSyncFailureInjected = true;
      defaultRouteSequence = -EIO;
    } else
#endif
      defaultRouteSequence = requestCoreSync();
    if (defaultRouteSequence < 0) {
      defaultRoutePending = false;
      defaultRouteSetRequested = false;
      defaultRouteSyncConfirmed = false;
      defaultRouteError = "cannot confirm the PipeWire default sink update";
      std::cerr << "skyapod: " << defaultRouteError << '\n';
    }
  }
  void scheduleDefaultRouteUpdate() {
    if (!defaultRouteTimer || !main || cleaning || stopping ||
        defaultRouteAttempted || defaultRouteOwned || defaultRoutePending ||
        defaultRouteOverridden)
      return;
    // Let the session manager finish reconciling its initial default route
    // after the PipeWire graph appears before claiming the route. Without this
    // settling window WirePlumber may immediately restore its startup value,
    // which looks indistinguishable from an intentional user override.
    timespec delay{1, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(main), defaultRouteTimer, &delay,
                         nullptr, false);
  }
  static void defaultRouteReady(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    if (r.defaultRouteSetRequested && r.defaultRouteSyncConfirmed) {
      r.confirmDefaultRouteOwnership();
      const auto now = std::chrono::steady_clock::now();
      if (!r.defaultRouteSetRequested) {
        r.updateDefaultRoute();
        r.scheduleDefaultRouteUpdate();
        return;
      }
      if (now >= r.defaultRouteConfirmationDeadline) {
        r.defaultRoutePending = false;
        r.defaultRouteSetRequested = false;
        r.defaultRouteOwned = false;
        r.defaultRouteError =
            "PipeWire/WirePlumber did not confirm SkyAPO as the default "
            "playback route";
        std::cerr << "skyapod: " << r.defaultRouteError << '\n';
      } else {
        timespec delay{0, 100000000};
        pw_loop_update_timer(pw_main_loop_get_loop(r.main), r.defaultRouteTimer,
                             &delay, nullptr, false);
        return;
      }
    }
    if (r.defaultRouteRecoveryPending) {
      const auto now = std::chrono::steady_clock::now();
      if (!r.playbackGraph || !r.playbackGraph->ready()) {
        r.defaultRouteRecoveryReadySince = {};
      } else if (r.defaultRouteRecoveryReadySince.time_since_epoch().count() ==
                 0) {
        r.defaultRouteRecoveryReadySince = now;
      } else if (now - r.defaultRouteRecoveryReadySince >=
                 std::chrono::seconds(1)) {
        r.defaultRouteRecoveryPending = false;
        r.defaultRouteRecoveryReadySince = {};
      }
      if (r.defaultRouteRecoveryPending) {
        r.scheduleDefaultRouteRecoveryCheck();
        return;
      }
    }
    r.updateDefaultRoute();
    r.scheduleDefaultRouteUpdate();
  }
  void scheduleDefaultRouteRecoveryCheck() {
    if (!defaultRouteTimer || !main || cleaning || stopping ||
        !defaultRouteRecoveryPending)
      return;
    timespec delay{0, 100000000};
    pw_loop_update_timer(pw_main_loop_get_loop(main), defaultRouteTimer, &delay,
                         nullptr, false);
  }
  std::string defaultRouteStatus() const {
    if (!playbackGraph)
      return "unavailable (playback graph is not active)";
    if (defaultRouteOwned)
      return "active (ordinary application playback routes through SkyAPO; "
             "previous default is restored on clean shutdown)";
    if (defaultRouteOverridden)
      return "inactive (system default was changed externally; SkyAPO left it "
             "untouched)";
    if (defaultRoutePending)
      return "waiting for PipeWire to confirm the SkyAPO default route";
    if (!defaultRouteError.empty())
      return "inactive (" + defaultRouteError + ")";
    if (!playbackGraph->ready())
      return "waiting for playback graph links to become active";
    if (!defaultMetadata)
      return "waiting for PipeWire default metadata";
    if (!defaultMetadataSnapshotReady)
      return "waiting for PipeWire default metadata snapshot";
    return "waiting for default route application";
  }
  void restoreDefaultRouteAndWait() noexcept {
    if ((!defaultRouteOwned && !defaultRouteRestoreEligible) ||
        !defaultMetadata || !originalDefaultMetadataCaptured)
      return;

    defaultRouteRestoring = true;
    // Drain accepted but unconfirmed writes first. Then restore only metadata
    // that still names SkyAPO, so an intervening user choice is preserved.
    const int inspectSequence = requestCoreSync();
    defaultRestoreSequence = inspectSequence;
    if (inspectSequence < 0) {
      defaultRouteRestoring = false;
      std::cerr << "skyapod: cannot inspect PipeWire default metadata before "
                   "restoration (core sync: "
                << spa_strerror(inspectSequence)
                << "); retaining playback route recovery state\n";
      return;
    }
    waitForCoreSync(inspectSequence);
    if (lastCoreDoneSequence != inspectSequence) {
      defaultRestoreSequence = -1;
      defaultRouteRestoring = false;
      std::cerr << "skyapod: cannot inspect PipeWire default metadata before "
                   "restoration (core sync timed out at sequence "
                << inspectSequence
                << "); retaining playback route recovery "
                   "state\n";
      return;
    }
    defaultRestoreSequence = -1;

    const bool restoreActive = defaultMetadataName == "skyapo.playback";
    const bool restoreConfigured =
        originalConfiguredDefaultCaptured && wirePlumberPresent &&
        configuredDefaultMetadataName == "skyapo.playback";
    if (!restoreActive && !restoreConfigured) {
      defaultRouteRestoring = false;
      defaultRouteOwned = false;
      defaultRouteRestoreEligible = false;
      removeDefaultRouteSnapshot();
      return;
    }

    int result = 0;
    if (restoreActive)
      result = pw_metadata_set_property(
          defaultMetadata, PW_ID_CORE, "default.audio.sink",
          originalDefaultMetadataPresent ? originalDefaultMetadataType.c_str()
                                         : nullptr,
          originalDefaultMetadataPresent ? originalDefaultMetadataValue.c_str()
                                         : nullptr);
    if (result >= 0 && restoreConfigured)
      result = pw_metadata_set_property(
          defaultMetadata, PW_ID_CORE, "default.configured.audio.sink",
          originalConfiguredDefaultMetadataPresent
              ? originalConfiguredDefaultMetadataType.c_str()
              : nullptr,
          originalConfiguredDefaultMetadataPresent
              ? originalConfiguredDefaultMetadataValue.c_str()
              : nullptr);
    if (result < 0) {
      defaultRouteRestoring = false;
      std::cerr << "skyapod: cannot restore PipeWire default sink metadata: "
                << spa_strerror(result)
                << "; retaining playback route recovery state\n";
      return;
    }

    const int restoreSequence = requestCoreSync();
    defaultRestoreSequence = restoreSequence;
    if (restoreSequence < 0) {
      defaultRouteRestoring = false;
      std::cerr << "skyapod: cannot confirm PipeWire default sink restoration; "
                   "retaining playback route recovery state\n";
      return;
    }
    waitForCoreSync(restoreSequence);
    if (lastCoreDoneSequence != restoreSequence) {
      defaultRestoreSequence = -1;
      defaultRouteRestoring = false;
      std::cerr << "skyapod: PipeWire default sink restoration timed out at "
                   "core sync sequence "
                << restoreSequence
                << "; retaining playback route recovery state\n";
      return;
    }
    defaultRestoreSequence = -1;
    defaultRouteRestoring = false;
    defaultRouteOwned = false;
    const PersistedMetadataValue activeOriginal{originalDefaultMetadataPresent,
                                                originalDefaultMetadataType,
                                                originalDefaultMetadataValue};
    const bool activeMetadataNotRestored =
        restoreActive &&
        !matchesDefaultMetadata(defaultMetadataPresent, defaultMetadataType,
                                defaultMetadataValue, activeOriginal);
    const PersistedMetadataValue configuredOriginal{
        originalConfiguredDefaultMetadataPresent,
        originalConfiguredDefaultMetadataType,
        originalConfiguredDefaultMetadataValue};
    const bool configuredMetadataNotRestored =
        restoreConfigured &&
        !matchesDefaultMetadata(
            configuredDefaultMetadataPresent, configuredDefaultMetadataType,
            configuredDefaultMetadataValue, configuredOriginal);
    if (activeMetadataNotRestored || configuredMetadataNotRestored)
      std::cerr << "skyapod: PipeWire did not confirm restoration of the "
                   "previous playback default metadata\n";
    else {
      removeDefaultRouteSnapshot();
      defaultRouteRestoreEligible = false;
    }
  }
  bool sameFormat(const Engine &a, const Engine &b) const noexcept {
    return a.sampleRate() == b.sampleRate() && a.channels() == b.channels() &&
           a.maxFrames() == b.maxFrames();
  }
  void promoteCompletedTransition() {
    if (!transitionComplete.load(std::memory_order_seq_cst))
      return;
    if (!pendingEngine)
      return;
    if (activeEngine)
      retiredEngines.push_back(std::move(activeEngine));
    activeEngine = std::move(pendingEngine);
    transitionCounter = 0;
    transitionLength = 0;
    transitionComplete.store(false, std::memory_order_seq_cst);
  }
  void reclaimRetired() {
    promoteCompletedTransition();
    if (callbacksInFlight.load(std::memory_order_seq_cst) == 0)
      retiredEngines.clear();
  }
  void waitForTransition() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (pending.load(std::memory_order_seq_cst)) {
      if (skyapo::pipewire::configReloadNeedsValidation(
              pluginLatencyFaulted.load(std::memory_order_seq_cst),
              callbackProcessingFaulted.load(std::memory_order_seq_cst),
              pluginLatencyEventQueued.load(std::memory_order_seq_cst)))
        return;
      reclaimRetired();
      if (!pending.load(std::memory_order_seq_cst))
        return;
      if (std::chrono::steady_clock::now() >= deadline)
        throw std::runtime_error(
            "timed out waiting for previous DSP graph transition");
      std::this_thread::yield();
    }
    reclaimRetired();
  }
  bool quiesceAndDrain() {
    const auto mode = audioMode.load(std::memory_order_seq_cst);
    if (mode == AudioMode::Stopping)
      throw std::runtime_error("audio runtime is shutting down");
    audioMode.store(AudioMode::Quiescing, std::memory_order_seq_cst);
    if (waitForCallbacksToDrain())
      return true;
    resumeAudio();
    return false;
  }
  void discardFaultedTransition() {
    if (!callbackProcessingFaulted.load(std::memory_order_seq_cst))
      return;
    if (!quiesceAndDrain())
      throw std::runtime_error(
          "timed out draining callbacks after a DSP processing exception");
    // A fault may interrupt a crossfade after either Engine has been
    // partially processed. Drop that transition only after callbacks have
    // drained; a subsequent validated config reload installs a fresh graph.
    pending.store(nullptr, std::memory_order_seq_cst);
    pendingEngine.reset();
    transitionCounter = 0;
    transitionLength = 0;
    transitionComplete.store(false, std::memory_order_seq_cst);
    transitionMetrics.graphReplaced();
    reclaimRetired();
    audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
  }
  void pauseAfterConfigCandidateForTest() {
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    const char *base = std::getenv("SKYAPO_TEST_PAUSE_AFTER_CONFIG_CANDIDATE");
    if (!base || !*base)
      return;
    const std::filesystem::path ready(std::string(base) + ".ready");
    const std::filesystem::path release(std::string(base) + ".release");
    {
      std::ofstream marker(ready, std::ios::out | std::ios::trunc);
      if (!marker)
        throw std::runtime_error(
            "cannot create test config-candidate barrier marker");
      marker << "candidate-built\n";
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (!std::filesystem::exists(release) &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!std::filesystem::exists(release))
      throw std::runtime_error("timed out at test config-candidate barrier");
#endif
  }
  void consumePendingConfigReload() {
    // An explicit reload reads the latest file contents, so an already queued
    // inotify debounce would only build and transition to the same graph a
    // second time. Drain it and cancel that timer before the explicit reload.
    configWatcher->consumeEvents();
    timespec disarmed{};
    pw_loop_update_timer(pw_main_loop_get_loop(main), reloadTimer, &disarmed,
                         nullptr, false);
  }
  void resumeAudio() noexcept {
    auto mode = audioMode.load(std::memory_order_seq_cst);
    while (mode != AudioMode::Stopping) {
      const auto next = skyapo::pipewire::resumeRuntimeAudioGate(
          mode, pluginLatencyFaulted.load(std::memory_order_seq_cst),
          callbackProcessingFaulted.load(std::memory_order_seq_cst),
          pluginLatencyEventQueued.load(std::memory_order_seq_cst));
      if (!audioMode.compare_exchange_weak(
              mode, next, std::memory_order_seq_cst, std::memory_order_seq_cst))
        continue;
      if (next != AudioMode::Running ||
          !skyapo::pipewire::configReloadNeedsValidation(
              pluginLatencyFaulted.load(std::memory_order_seq_cst),
              callbackProcessingFaulted.load(std::memory_order_seq_cst),
              pluginLatencyEventQueued.load(std::memory_order_seq_cst)))
        return;
      mode = AudioMode::Running;
    }
  }
  void buildEngine(unsigned hz, unsigned blockFrames) {
    if (hz < 8000 || hz > 384000)
      throw std::runtime_error("unsupported graph rate " + std::to_string(hz));
    if (!blockFrames || blockFrames > MaxFrames)
      throw std::runtime_error("unsupported graph quantum " +
                               std::to_string(blockFrames));
    // A format change cannot use FilterConfiguration's transition because the
    // preallocated configurations have different rate/block capacities. If a
    // same-format reload was mid-fade when PipeWire renegotiated, choose that
    // already accepted pending config before rebuilding at the new format.
    if (pending.load(std::memory_order_seq_cst)) {
      ++formatRebuildsDuringTransition;
      if (!quiesceAndDrain())
        throw std::runtime_error(
            "timed out draining callbacks for PipeWire format rebuild");
      Engine *next = pending.load(std::memory_order_seq_cst);
      if (next) {
        active.store(next, std::memory_order_seq_cst);
        pending.store(nullptr, std::memory_order_seq_cst);
        transitionComplete.store(true, std::memory_order_seq_cst);
      }
      reclaimRetired();
      resumeAudio();
    }
    auto *current = active.load(std::memory_order_acquire);
    if (current && current->sampleRate() == hz &&
        current->supportsFrameCount(blockFrames))
      return;
    if (skyapo::pipewire::formatRebuildMustDefer(
            pluginLatencyFaulted.load(std::memory_order_seq_cst),
            callbackProcessingFaulted.load(std::memory_order_seq_cst),
            pluginLatencyEventQueued.load(std::memory_order_seq_cst))) {
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
      if (std::getenv("SKYAPO_TEST_FORMAT_REBUILD_BEFORE_PDC_EVENT"))
        testFormatRebuildDeferred.store(true, std::memory_order_seq_cst);
#endif
      return;
    }
    std::vector<std::wstring> names;
    for (auto &p : ports)
      names.push_back(eapoChannel(p.channel));
    const auto createEngine =
        [&](const std::shared_ptr<const ConfigSourceSnapshot> &sources) {
          auto candidate = std::make_unique<Engine>(
              hz, channels, blockFrames, names, true, true,
              deviceMatchText(device),
              StringHelper::toWString(device.description, 65001),
              StringHelper::toWString(device.name, 65001));
          if (sources)
            candidate->loadConfig(config, sources);
          else
            candidate->loadConfig(config, true);
          return candidate;
        };
    // Validate the existing sidecars before the active graph is allowed to
    // overwrite them with a fresh snapshot. If corrupt state/plugin load is
    // rejected, this candidate fails and the active graph and bytes survive.
    auto engine = createEngine(lastGoodConfigSources);
    auto acceptedSources = engine->configSourceSnapshot();
    const bool rebuildingFaultedGraph =
        skyapo::pipewire::formatRebuildMustDefer(
            pluginLatencyFaulted.load(std::memory_order_seq_cst),
            callbackProcessingFaulted.load(std::memory_order_seq_cst),
            pluginLatencyEventQueued.load(std::memory_order_seq_cst));
    if (!rebuildingFaultedGraph) {
      const auto saved = saveCurrentPluginStates();
      if (saved)
        std::cerr << "skyapod: saved state for " << saved
                  << " plugin instance(s) before graph rebuild\n";
      if (saved) {
        // The first candidate validated the old sidecar state. Once the
        // active graph has written its fresh snapshot, discard that candidate
        // before reconstructing from the new state to avoid holding two full
        // replacement graphs alongside the active Engine.
        engine.reset();
        engine = createEngine(acceptedSources);
      }
    }
    if (!quiesceAndDrain())
      throw std::runtime_error(
          "timed out draining callbacks before PipeWire format commit");
    if (skyapo::pipewire::formatRebuildMustDefer(
            pluginLatencyFaulted.load(std::memory_order_seq_cst),
            callbackProcessingFaulted.load(std::memory_order_seq_cst),
            pluginLatencyEventQueued.load(std::memory_order_seq_cst))) {
      // A latency event may arrive while the replacement Engine is being
      // prepared. Keep the old PDC state authoritative and the callback gate
      // fail-closed; the latency event will validate or fault it, after which
      // PipeWire's format mismatch requests a fresh rebuild if still needed.
      resumeAudio();
      return;
    }
    try {
      configWatcher->update(engine->configFiles());
      installEngine(std::move(engine));
      lastGoodConfigSources = std::move(acceptedSources);
    } catch (...) {
      resumeAudio();
      throw;
    }
    auto *pointer = active.load(std::memory_order_acquire);
    std::cerr << "skyapod: DSP ready at " << hz << " Hz, " << channels
              << " channels, " << pointer->filterCount() << " filters\n";
    resumeAudio();
  }
  void installEngine(std::unique_ptr<Engine> replacement,
                     bool forceHardSwap = false) {
    promoteCompletedTransition();
    if (pendingEngine || pending.load(std::memory_order_seq_cst))
      throw std::runtime_error("another DSP graph transition is still active");
    for (const auto &notice : replacement->configNotices())
      std::cerr << "skyapod: config warning " << notice.file.string() << ':'
                << notice.line << ": " << notice.message << '\n';
    // Every accepted replacement invalidates queued completion telemetry,
    // including hard swaps after a PipeWire format change.
    const auto generation = transitionMetrics.graphReplaced();
    auto *current = active.load(std::memory_order_seq_cst);
    if (!forceHardSwap && current && sameFormat(*current, *replacement)) {
      // Keep both owners on the control thread. The callback only reads the
      // published raw pointer and never destroys or mutates ownership.
      retiredEngines.reserve(retiredEngines.size() + 1);
      transitionCounter = 0;
      transitionLength =
          std::max(1u, replacement->sampleRate() * transitionDurationMs / 1000);
      auto *next = replacement.get();
      pendingEngine = std::move(replacement);
      transitionMetrics.transitionStarted(generation);
      pending.store(next, std::memory_order_seq_cst);
      return;
    }
    // Format changes keep the old graph alive until every callback that could
    // have loaded it has left; incompatible formats use a hard rebuild.
    if (activeEngine) {
      retiredEngines.reserve(retiredEngines.size() + 1);
      retiredEngines.push_back(std::move(activeEngine));
    }
    auto *pointer = replacement.get();
    active.store(pointer, std::memory_order_seq_cst);
    activeEngine = std::move(replacement);
    reclaimRetired();
  }
  void reserveEngineInstall() {
    promoteCompletedTransition();
    if (pendingEngine || pending.load(std::memory_order_seq_cst))
      throw std::runtime_error("another DSP graph transition is still active");
    if (activeEngine)
      retiredEngines.reserve(retiredEngines.size() + 1);
  }
  bool waitForCallbacksToDrain() {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (callbacksInFlight.load(std::memory_order_seq_cst) != 0) {
      if (std::chrono::steady_clock::now() >= deadline)
        return false;
      std::this_thread::yield();
    }
    return true;
  }
  unsigned saveCurrentPluginStates() {
    if (!quiesceAndDrain()) {
      throw std::runtime_error(
          "timed out waiting for realtime callbacks before plugin state save");
    }
    try {
      if (skyapo::pipewire::configReloadNeedsValidation(
              pluginLatencyFaulted.load(std::memory_order_seq_cst),
              callbackProcessingFaulted.load(std::memory_order_seq_cst),
              pluginLatencyEventQueued.load(std::memory_order_seq_cst))) {
        // A callback may have queued PDC validation while the config candidate
        // was being built. Do not call into that potentially inconsistent live
        // plugin to save state; the caller will validate and commit a fresh
        // candidate after the same callback-drain boundary.
        resumeAudio();
        return 0;
      }
      promoteCompletedTransition();
      Engine *current = active.load(std::memory_order_seq_cst);
      const auto saved = current ? current->savePersistentPluginStates() : 0;
      resumeAudio();
      return saved;
    } catch (...) {
      resumeAudio();
      throw;
    }
  }
  void shutdownAudio() noexcept {
    // Explicit shutdown path: disconnect/destroy PipeWire first, then drain
    // callbacks before making any main-thread plugin state calls. Never save in
    // Runtime/Engine/plugin destructors.
    cleaning = true;
    audioMode.store(AudioMode::Stopping, std::memory_order_seq_cst);
    if (filter) {
      pw_filter_disconnect(filter);
      pw_filter_destroy(filter);
      filter = nullptr;
    }
    if (!waitForCallbacksToDrain()) {
      std::cerr << "skyapod: realtime callback drain exceeded 500 ms after "
                   "filter destruction; waiting before Engine teardown\n";
      // The filter has already been disconnected and destroyed, so no new
      // callback may start. Continue draining rather than freeing an Engine
      // that a stuck callback could still be using. State save remains skipped.
      while (callbacksInFlight.load(std::memory_order_seq_cst) != 0)
        std::this_thread::yield();
    }
    // A callback may have completed the fade just before filter destruction;
    // resolve owner movement only after the in-flight barrier.
    if (pending.load(std::memory_order_seq_cst)) {
      Engine *next = pending.load(std::memory_order_seq_cst);
      active.store(next, std::memory_order_seq_cst);
      pending.store(nullptr, std::memory_order_seq_cst);
      transitionComplete.store(true, std::memory_order_seq_cst);
    }
    promoteCompletedTransition();
    Engine *current = active.exchange(nullptr, std::memory_order_seq_cst);
    if (!current)
      return;
    try {
      const auto count = current->savePersistentPluginStates();
      if (count)
        std::cerr << "skyapod: saved state for " << count
                  << " plugin instance(s)\n";
    } catch (const std::exception &error) {
      std::cerr << "skyapod: plugin state save failed during shutdown: "
                << error.what() << '\n';
    }
  }
  void reloadConfig() {
    // Only one pair of configurations may be in flight. A very short wait on
    // the control thread lets realtime finish and retire the accepted graph
    // before another candidate is prepared.
    discardFaultedTransition();
    bool recoveringFaultedGraph = skyapo::pipewire::configReloadNeedsValidation(
        pluginLatencyFaulted.load(std::memory_order_seq_cst),
        callbackProcessingFaulted.load(std::memory_order_seq_cst),
        pluginLatencyEventQueued.load(std::memory_order_seq_cst));
    if (!recoveringFaultedGraph)
      waitForTransition();
    auto *current = active.load(std::memory_order_acquire);
    const unsigned reportedRate = requestedRate.load(std::memory_order_acquire);
    const unsigned reportedQuantum =
        requestedQuantum.load(std::memory_order_acquire);
    const unsigned hz = reportedRate ? reportedRate
                        : current    ? current->sampleRate()
                                     : 0;
    const unsigned blockFrames = reportedQuantum ? reportedQuantum
                                 : current       ? current->maxFrames()
                                                 : 0;
    std::vector<std::wstring> names;
    for (auto &p : ports)
      names.push_back(eapoChannel(p.channel));
    if (!hz || !blockFrames)
      throw std::runtime_error(
          "cannot reload before PipeWire format negotiation");
    const auto createEngine =
        [&](const std::shared_ptr<const ConfigSourceSnapshot> &sources) {
          auto candidate = std::make_unique<Engine>(
              hz, channels, blockFrames, names, true, true,
              deviceMatchText(device),
              StringHelper::toWString(device.description, 65001),
              StringHelper::toWString(device.name, 65001));
          if (sources)
            candidate->loadConfig(config, sources);
          else
            candidate->loadConfig(config, true);
          return candidate;
        };
    // Read/validate persisted state first. A malformed sidecar or plugin load
    // rejection must not be silently overwritten by saving the live graph.
    std::unique_ptr<Engine> replacement;
    try {
      replacement = createEngine(nullptr);
      pauseAfterConfigCandidateForTest();
    } catch (const Engine::ConfigError &error) {
      // Keep last-good dependency watches and additionally watch paths from
      // the failed candidate, so creating a missing Include retries reload.
      configWatcher->extend(error.attemptedFiles());
      throw;
    }
    // A plugin that changed its internal latency may no longer match the
    // active PDC schedule. Do not call it again to save state or crossfade it
    // against a replacement; the faulted graph stays quiesced until a fresh
    // Engine has been constructed successfully.
    if (!recoveringFaultedGraph) {
      const auto saved = saveCurrentPluginStates();
      if (saved)
        std::cerr << "skyapod: saved state for " << saved
                  << " plugin instance(s) before config reload\n";
      if (saved) {
        // The first candidate validated the old sidecar state. After saving
        // current plugin state, release it before loading the updated
        // sidecars so peak reload memory stays at active + one candidate.
        const auto replacementSources = replacement->configSourceSnapshot();
        replacement.reset();
        replacement = createEngine(replacementSources);
      }
    }
    const auto replacementSources = replacement->configSourceSnapshot();
    configWatcher->update(replacement->configFiles());
    const unsigned count = replacement->filterCount();
    // Prepare every operation that can allocate before asking the playback
    // graph to commit its own replacement. If playback rejects this config,
    // capture must keep its last accepted Engine too.
    if (!quiesceAndDrain())
      throw std::runtime_error(
          "timed out draining callbacks before config commit");
    try {
      // The initial state snapshot can become stale while the candidate is
      // being parsed or initialized. A realtime callback may discover a new
      // plugin latency or processing fault during that interval. Quiesce and
      // drain at the commit boundary, then validate against the state that is
      // actually about to be replaced.
      recoveringFaultedGraph = skyapo::pipewire::configReloadNeedsValidation(
          pluginLatencyFaulted.load(std::memory_order_seq_cst),
          callbackProcessingFaulted.load(std::memory_order_seq_cst),
          pluginLatencyEventQueued.load(std::memory_order_seq_cst));
      if (recoveringFaultedGraph)
        replacement->validateForActivation();

      // A fault can interrupt an in-flight crossfade. Once callbacks have
      // drained, discard that unvalidated pending graph before replacing the
      // active Engine; otherwise it could remain pending forever while the
      // fail-closed callback intentionally emits silence.
      reclaimRetired();
      if (recoveringFaultedGraph && pending.load(std::memory_order_seq_cst)) {
        pending.store(nullptr, std::memory_order_seq_cst);
        pendingEngine.reset();
        transitionCounter = 0;
        transitionLength = 0;
        transitionComplete.store(false, std::memory_order_seq_cst);
        transitionMetrics.graphReplaced();
        reclaimRetired();
      }

      // With callbacks drained and any faulted transition discarded, reserve
      // the final owner-retirement slot before the paired playback graph can
      // commit. No callback can complete another transition and consume this
      // capacity during the remaining commit sequence.
      reserveEngineInstall();

      if (playbackGraph) {
        try {
          playbackGraph->reloadConfig(config, replacementSources);
          playbackError.clear();
        } catch (const std::exception &error) {
          playbackError = error.what();
          throw std::runtime_error(
              std::string("playback config reload failed: ") + error.what());
        }
      } else {
        // Keep the two processing stages transactional even while a selected
        // playback target is temporarily absent. Otherwise the capture Engine
        // could accept a config whose post-mix branch is invalid, and the later
        // playback recovery would only discover the error after capture had
        // already changed. Validate that branch on the control thread using the
        // last known output identity and the currently negotiated graph shape;
        // the real playback Engine is still rebuilt against its negotiated
        // format and sink ports when the target returns.
        const std::string playbackName = !outputSelection.name.empty()
                                             ? outputSelection.name
                                             : defaultMetadataName;
        const auto playbackNameWide =
            StringHelper::toWString(playbackName, 65001);
        Engine playbackCandidate(hz, channels, blockFrames, names, true, true,
                                 playbackNameWide, playbackNameWide,
                                 playbackNameWide, L"post-mix");
        playbackCandidate.loadConfig(config, replacementSources);
        playbackCandidate.validateForActivation();
      }

      installEngine(std::move(replacement), recoveringFaultedGraph);
      lastGoodConfigSources = replacementSources;
      resetMetrics.store(true, std::memory_order_release);
      configError.clear();
      if (recoveringFaultedGraph) {
        pluginLatencyError.clear();
        pluginLatencyFaulted.store(false, std::memory_order_seq_cst);
        callbackProcessingFaulted.store(false, std::memory_order_seq_cst);
      }
      resumeAudio();
    } catch (const std::exception &) {
      resumeAudio();
      throw;
    } catch (...) {
      resumeAudio();
      throw;
    }
    std::cerr << "skyapod: config reload succeeded (" << count << " filters)\n";
  }
  static void configReady(void *data, int, uint32_t) {
    auto &r = *static_cast<Runtime *>(data);
    if (r.configWatcher->consumeEvents()) {
      timespec debounce{0, 150000000};
      pw_loop_update_timer(pw_main_loop_get_loop(r.main), r.reloadTimer,
                           &debounce, nullptr, false);
    }
  }
  static void reloadReady(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    try {
      r.reloadConfig();
    } catch (const std::exception &e) {
      r.configError = e.what();
      std::cerr << "skyapod: config reload failed; "
                << (r.audioMode.load(std::memory_order_seq_cst) ==
                            AudioMode::Faulted
                        ? "DSP remains paused and outputting silence: "
                        : "retaining last valid graph: ")
                << e.what() << '\n';
    }
  }
  bool tryCreatePlaybackGraph(const Devices &devices) {
    if (playbackGraph)
      return true;
    outputSelection = settings::outputDeviceSelection();
    const AudioDevice *defaultPlaybackDevice = nullptr;
    const auto defaultSink = std::find_if(
        devices.sinks.begin(), devices.sinks.end(),
        [](const AudioDevice &candidate) {
          return candidate.isDefault == std::optional<bool>(true) &&
                 candidate.name != "skyapo.playback";
        });
    if (defaultSink != devices.sinks.end())
      defaultPlaybackDevice = &*defaultSink;
    const AudioDevice *playbackDevice = nullptr;
    if (!outputSelection.name.empty()) {
      playbackDevice = findSelectedSink(devices, outputSelection.name,
                                        outputSelection.stableProperty);
      if (!playbackDevice) {
        playbackError =
            "selected playback sink unavailable: " + outputSelection.name;
        return false;
      }
    } else if (defaultPlaybackDevice) {
      playbackDevice = defaultPlaybackDevice;
    } else {
      playbackError = "PipeWire default playback sink is unavailable";
      return false;
    }

    std::vector<AudioPort> playbackPorts;
    for (const auto &port : devices.ports)
      if (port.node == playbackDevice->id && port.direction == "in")
        playbackPorts.push_back(port);
    try {
      playbackGraph = std::make_unique<skyapo::pipewire::PlaybackGraph>(
          core, pw_main_loop_get_loop(main), registry, *playbackDevice,
          std::move(playbackPorts), config,
          wirePlumberPresent || virtualMicConfiguredByPolicy);
      if (renderVolumeMonitor) {
        renderVolumeMonitor->setSinkOverride(playbackDevice->name);
        renderVolume = renderVolumeMonitor->snapshot();
        skyapo::platform::LoudnessVolumeProvider::publish(
            renderVolume.uniformChannelGainAvailable,
            renderVolume.uniformChannelGainDb);
      }
      playbackError.clear();
      scheduleDefaultRouteUpdate();
      std::cerr << "skyapod: playback graph recovered for "
                << playbackDevice->name << '\n';
      return true;
    } catch (const std::exception &error) {
      playbackError = error.what();
      std::cerr << "skyapod: playback graph unavailable: " << error.what()
                << '\n';
      return false;
    }
  }
  static void playbackRecovery(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    if (r.playbackGraph || r.cleaning || r.stopping)
      return;
    try {
      const auto devices = enumerateDevices();
      r.tryCreatePlaybackGraph(devices);
    } catch (const std::exception &error) {
      r.playbackError = error.what();
      std::cerr << "skyapod: playback recovery discovery failed: "
                << error.what() << '\n';
    }
  }
  std::string refreshPlaybackOutput() {
    const auto selection = settings::outputDeviceSelection();
    if (selection.name.empty())
      throw std::runtime_error(
          "no explicit playback target is selected; restart skyapod to use the "
          "system default sink");
    auto devices = enumerateDevices();
    const auto *selected =
        findSelectedSink(devices, selection.name, selection.stableProperty);
    if (!selected)
      throw std::runtime_error("selected playback sink is unavailable: " +
                               selection.name);
    if (!playbackGraph) {
      if (!tryCreatePlaybackGraph(devices))
        throw std::runtime_error(playbackError.empty()
                                     ? "playback graph could not be created"
                                     : playbackError);
      return "Playback output graph created for " + selected->name + '\n';
    }
    std::vector<AudioPort> selectedPorts;
    for (const auto &port : devices.ports)
      if (port.node == selected->id && port.direction == "in")
        selectedPorts.push_back(port);
    playbackGraph->selectSink(*selected, std::move(selectedPorts));
    outputSelection = selection;
    playbackError.clear();
    if (renderVolumeMonitor) {
      renderVolumeMonitor->setSinkOverride(selected->name);
      renderVolume = renderVolumeMonitor->snapshot();
      skyapo::platform::LoudnessVolumeProvider::publish(
          renderVolume.uniformChannelGainAvailable,
          renderVolume.uniformChannelGainDb);
    }
    std::cerr << "skyapod: playback target changed in-place to "
              << selected->name << '\n';
    return "Playback output switched to " + selected->name +
           " without restarting the audio graph\n";
  }
  static void rateChanged(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    try {
      r.reclaimRetired();
      r.buildEngine(r.requestedRate.load(), r.requestedQuantum.load());
    } catch (const std::exception &e) {
      if (skyapo::pipewire::formatRebuildMustDefer(
              r.pluginLatencyFaulted.load(std::memory_order_seq_cst),
              r.callbackProcessingFaulted.load(std::memory_order_seq_cst),
              r.pluginLatencyEventQueued.load(std::memory_order_seq_cst))) {
        r.configError = e.what();
        std::cerr << "skyapod: PipeWire format rebuild deferred while PDC "
                     "validation is unresolved and output is silent: "
                  << e.what() << '\n';
        return;
      }
      r.fail(e.what());
    }
  }
  static void processFaultReady(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    r.processFaultEventQueued.store(false, std::memory_order_release);
    if (!r.callbackProcessingFaulted.load(std::memory_order_seq_cst))
      return;
    try {
      r.discardFaultedTransition();
    } catch (const std::exception &error) {
      r.configError = error.what();
      std::cerr << "skyapod: realtime DSP exception; output is silenced, but "
                   "callback drain failed: "
                << error.what() << '\n';
      return;
    }
    r.configError =
        "DSP processing threw inside the PipeWire callback; output silenced "
        "until a valid config reload";
    std::cerr << "skyapod: " << r.configError << '\n';
  }
  static void transitionReady(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    r.reclaimRetired();
    // A delayed event from an earlier transition must not reset telemetry
    // collected for a newer graph. The callback publishes completion before
    // signaling this event; duplicate event delivery is harmless.
    if (r.transitionMetrics.shouldResetMetrics()) {
      // Restart signal-level statistics after the graph crossfade completes.
      r.resetMetrics.store(true, std::memory_order_release);
    }
  }
  static void process(void *data, spa_io_position *position) {
    realtime::Scope audit;
    auto &r = *static_cast<Runtime *>(data);
    struct FlightGuard {
      std::atomic<unsigned> &count;
      explicit FlightGuard(std::atomic<unsigned> &value) : count(value) {
        count.fetch_add(1, std::memory_order_seq_cst);
      }
      ~FlightGuard() {
        count.fetch_sub(1, std::memory_order_seq_cst);
      }
    } flight(r.callbacksInFlight);
    const uint64_t begin = now();
    const auto frames = position->clock.duration;
    const unsigned hz = position->clock.rate.num ? position->clock.rate.denom /
                                                       position->clock.rate.num
                                                 : 0;
    if (!frames)
      return;
    if (r.resetMetrics.exchange(false, std::memory_order_acq_rel)) {
      r.rawSum = 0;
      r.processedSum = 0;
      r.samples.store(0, std::memory_order_relaxed);
      r.blocks.store(0, std::memory_order_relaxed);
      r.totalNs.store(0, std::memory_order_relaxed);
      r.maxNs.store(0, std::memory_order_relaxed);
      r.overruns.store(0, std::memory_order_relaxed);
      r.rawEnergy.store(0, std::memory_order_relaxed);
      r.processedEnergy.store(0, std::memory_order_relaxed);
    }
    r.rate.store(hz, std::memory_order_relaxed);
    r.quantum.store(frames, std::memory_order_relaxed);
    std::array<float *, MaxChannels> in{}, out{};
    for (unsigned c = 0; c < r.channels; ++c) {
      in[c] =
          static_cast<float *>(pw_filter_get_dsp_buffer(r.inputs[c], frames));
      out[c] =
          static_cast<float *>(pw_filter_get_dsp_buffer(r.outputs[c], frames));
    }
    Engine *engine = r.active.load(std::memory_order_seq_cst);
    const bool formatMismatch = !engine || engine->sampleRate() != hz ||
                                !engine->supportsFrameCount(frames);
    if (formatMismatch) {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      const bool rateChanged = r.requestedRate.exchange(hz) != hz;
      const bool quantumChanged = r.requestedQuantum.exchange(frames) != frames;
      if (rateChanged || quantumChanged ||
          (formatMismatch &&
           !skyapo::pipewire::formatRebuildMustDefer(
               r.pluginLatencyFaulted.load(std::memory_order_seq_cst),
               r.callbackProcessingFaulted.load(std::memory_order_seq_cst),
               r.pluginLatencyEventQueued.load(std::memory_order_seq_cst))))
        pw_loop_signal_event(pw_main_loop_get_loop(r.main), r.rateEvent);
      return;
    }
    if (skyapo::pipewire::audioOutputMustBeSilent(
            r.audioMode.load(std::memory_order_seq_cst),
            r.pluginLatencyEventQueued.load(std::memory_order_seq_cst))) {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      return;
    }
    Engine *nextEngine = r.pending.load(std::memory_order_seq_cst);
    const auto failClosedForPluginLatency = [&] {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      // Quiesce even if an event is already queued. The queue bit only
      // deduplicates control notifications; it must never govern the audio
      // safety state.
      r.audioMode.store(AudioMode::Quiescing, std::memory_order_seq_cst);
      bool expected = false;
      if (r.pluginLatencyEventQueued.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel,
              std::memory_order_relaxed)) {
        pw_loop_signal_event(pw_main_loop_get_loop(r.main),
                             r.pluginLatencyEvent);
      }
    };
    double raw = 0, processed = 0;
    try {
      for (uint64_t offset = 0; offset < frames; offset += MaxFrames) {
        const unsigned n = std::min<uint64_t>(MaxFrames, frames - offset);
        for (unsigned f = 0; f < n; ++f)
          for (unsigned c = 0; c < r.channels; ++c) {
            float s = in[c] ? in[c][offset + f] : 0.0f;
            r.work[f * r.channels + c] = s;
            raw += double(s) * s;
          }
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
        struct TestProcessException {};
        if (r.testThrowProcessExceptionOnce &&
            (!r.testThrowProcessExceptionAfterTransition ||
             (nextEngine && r.transitionCounter > 0))) {
          r.testThrowProcessExceptionOnce = false;
          throw TestProcessException{};
        }
#endif
        if (nextEngine) {
          r.transitionCounter = engine->processTransitionTo(
              *nextEngine, r.work.data(), n, r.transitionCounter,
              r.transitionLength);
          if (r.transitionCounter >= r.transitionLength) {
            engine = nextEngine;
            r.active.store(engine, std::memory_order_seq_cst);
            r.transitionMetrics.transitionCompleted();
            r.pending.store(nullptr, std::memory_order_seq_cst);
            r.transitionComplete.store(true, std::memory_order_seq_cst);
            nextEngine = nullptr;
            if (r.transitionEvent)
              pw_loop_signal_event(pw_main_loop_get_loop(r.main),
                                   r.transitionEvent);
          }
        } else {
          engine->process(r.work.data(), n);
        }
        if (engine->pluginLatencyRefreshPending() ||
            (nextEngine && nextEngine->pluginLatencyRefreshPending())) {
          // A changed plugin delay invalidates the current PDC plan at this
          // exact block boundary. Do not publish even the candidate chunk:
          // mute the complete PipeWire quantum and let the control thread
          // drain/rebuild without performing waits or ownership changes here.
          failClosedForPluginLatency();
          return;
        }
        for (unsigned f = 0; f < n; ++f)
          for (unsigned c = 0; c < r.channels; ++c) {
            float s = r.work[f * r.channels + c];
            if (out[c])
              out[c][offset + f] = s;
            processed += double(s) * s;
          }
      }
    } catch (...) {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      r.callbackProcessingFaulted.store(true, std::memory_order_seq_cst);
      r.audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
      bool expected = false;
      if (r.processFaultEvent &&
          r.processFaultEventQueued.compare_exchange_strong(
              expected, true, std::memory_order_acq_rel,
              std::memory_order_relaxed))
        pw_loop_signal_event(pw_main_loop_get_loop(r.main),
                             r.processFaultEvent);
      return;
    }
    r.rawSum += raw;
    r.processedSum += processed;
    r.rawEnergy.store(r.rawSum, std::memory_order_relaxed);
    r.processedEnergy.store(r.processedSum, std::memory_order_relaxed);
    r.samples.fetch_add(frames * r.channels, std::memory_order_relaxed);
    r.blocks.fetch_add(1, std::memory_order_relaxed);
    const uint64_t elapsed = now() - begin;
    r.totalNs.fetch_add(elapsed, std::memory_order_relaxed);
    if (elapsed > r.maxNs.load(std::memory_order_relaxed))
      r.maxNs.store(elapsed, std::memory_order_relaxed);
    if (hz && elapsed > frames * 1000000000 / hz)
      r.overruns.fetch_add(1, std::memory_order_relaxed);

    // A plugin may report a changed latency at the end of run(). Its new DSP
    // delay applies to subsequent blocks, so stop processing at this block
    // boundary and let the control loop rebuild the PDC schedule while audio
    // callbacks are drained. The quiescing callback path writes silence; it
    // does not allocate, inspect files, or wait for the control loop.
    Engine *processedEngine = r.active.load(std::memory_order_seq_cst);
    Engine *candidate = r.pending.load(std::memory_order_seq_cst);
    if ((processedEngine && processedEngine->pluginLatencyRefreshPending()) ||
        (candidate && candidate->pluginLatencyRefreshPending())) {
      // Close the race with asynchronous host notifications arriving after
      // the per-chunk check but before this callback returns.
      failClosedForPluginLatency();
    }
  }
  static void stateChanged(void *data, pw_filter_state old,
                           pw_filter_state state, const char *error) {
    auto &r = *static_cast<Runtime *>(data);
    r.state = state;
    std::cerr << "skyapod: virtual mic " << pw_filter_state_as_string(state)
              << '\n';
    if (state == PW_FILTER_STATE_PAUSED && r.outputPortConfigPending)
      r.rebuildOutputPortsForPortConfig();
    if (state == PW_FILTER_STATE_PAUSED && r.virtualMicConfiguredByPolicy &&
        !r.outputPortConfigPending)
      activateVirtualMic(r, "WirePlumber PortConfig");
    if (state == PW_FILTER_STATE_ERROR)
      r.fail(error ? error : "filter error");
    if (state == PW_FILTER_STATE_UNCONNECTED &&
        old != PW_FILTER_STATE_UNCONNECTED && !r.cleaning && !r.stopping)
      r.fail("virtual source disconnected");
  }
  static void refreshPluginLatency(Runtime &r) {
    r.reclaimRetired();
    Engine *candidate = r.pending.load(std::memory_order_seq_cst);
    Engine *current = r.active.load(std::memory_order_seq_cst);
    if (candidate && (candidate->pluginLatencyRefreshPending() ||
                      (current && current->pluginLatencyRefreshPending()))) {
      if (!r.quiesceAndDrain()) {
        r.pluginLatencyError =
            "timed out draining callbacks after plugin latency changed "
            "during graph transition";
        r.pluginLatencyFaulted.store(true, std::memory_order_seq_cst);
        r.audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
        return;
      }
      r.reclaimRetired();
      if (r.pending.load(std::memory_order_seq_cst)) {
        // The in-flight candidate has not been proven safe with its observed
        // plugin latency. Destroy it only after the callback barrier and keep
        // the previously accepted graph muted until a validated reload.
        r.pending.store(nullptr, std::memory_order_seq_cst);
        r.pendingEngine.reset();
        r.transitionCounter = 0;
        r.transitionLength = 0;
        r.transitionComplete.store(false, std::memory_order_seq_cst);
        r.transitionMetrics.graphReplaced();
        r.pluginLatencyError =
            "plugin latency changed during graph transition; candidate "
            "discarded and output remains silent until config reload";
        r.pluginLatencyFaulted.store(true, std::memory_order_seq_cst);
        r.audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
        return;
      }
    }
    auto *engine = r.active.load(std::memory_order_seq_cst);
    if (!engine || !engine->pluginLatencyRefreshPending()) {
      r.resumeAudio();
      return;
    }
    // Keep the callback gate closed if draining times out. quiesceAndDrain()
    // resumes audio on timeout, which is unsafe while a plugin's PDC state is
    // unresolved.
    r.audioMode.store(AudioMode::Quiescing, std::memory_order_seq_cst);
    bool callbacksDrained = r.waitForCallbacksToDrain();
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    bool injectedDrainTimeout = false;
    if (const char *inject = std::getenv("SKYAPO_TEST_FAIL_LATENCY_DRAIN");
        inject && std::strcmp(inject, "1") == 0 &&
        !r.testLatencyDrainFailureInjected) {
      r.testLatencyDrainFailureInjected = true;
      injectedDrainTimeout = true;
      callbacksDrained = false;
    }
#endif
    if (!callbacksDrained) {
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
      r.pluginLatencyError = injectedDrainTimeout
                                 ? "injected callback drain timeout"
                                 : "timed out draining callbacks for plugin "
                                   "latency refresh";
#else
      r.pluginLatencyError =
          "timed out draining callbacks for plugin latency refresh";
#endif
      r.pluginLatencyFaulted.store(true, std::memory_order_seq_cst);
      r.audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
      std::cerr << "skyapod: " << r.pluginLatencyError
                << "; keeping the daemon alive and output silent\n";
      return;
    }
    try {
      r.reclaimRetired();
      engine = r.active.load(std::memory_order_seq_cst);
      const unsigned count = engine ? engine->refreshPluginLatencies() : 0;
      if (count) {
        r.pluginLatencyRefreshes += count;
        r.pluginLatencyError.clear();
        std::cerr << "skyapod: refreshed latency for " << count
                  << " plugin instance(s)\n";
        r.resetMetrics.store(true, std::memory_order_release);
      }
    } catch (const std::exception &e) {
      r.pluginLatencyError = e.what();
      std::cerr << "skyapod: plugin latency refresh failed: "
                << r.pluginLatencyError << '\n';
      // The plugin may already have changed its internal latency, but the
      // transactional PDC rebuild leaves the previous alignment schedule
      // untouched on failure. Keep publishing silence and preserve the
      // control socket so the user can install a fresh valid config. A
      // portable rollback of arbitrary plugin-internal state is unavailable.
      r.pluginLatencyFaulted.store(true, std::memory_order_seq_cst);
      r.audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
      return;
    } catch (...) {
      r.pluginLatencyError =
          "unknown non-standard exception during plugin latency refresh";
      std::cerr << "skyapod: " << r.pluginLatencyError << '\n';
      r.pluginLatencyFaulted.store(true, std::memory_order_seq_cst);
      r.audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
      return;
    }
    r.resumeAudio();
  }
  static void pluginLatencyChanged(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    if (std::getenv("SKYAPO_TEST_FORMAT_REBUILD_BEFORE_PDC_EVENT")) {
      // Reproduce ordering where a format rebuild reaches the control loop
      // before the queued latency notification is serviced. Keep the queue
      // latch set while buildEngine runs, as it is in the real race window.
      constexpr unsigned testQuantum = 2048;
      const unsigned previousQuantum =
          r.requestedQuantum.load(std::memory_order_seq_cst);
      r.requestedQuantum.store(testQuantum, std::memory_order_seq_cst);
      rateChanged(&r, 0);
      r.requestedQuantum.store(previousQuantum, std::memory_order_seq_cst);
    }
#endif
    r.pluginLatencyEventQueued.store(false, std::memory_order_release);
    refreshPluginLatency(r);
  }
  static void selectionChanged(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    refreshPluginLatency(r);
    r.updateDefaultRoute();
    try {
      const auto selection = settings::deviceSelection();
      if (!isSelectedDevice(r.device, selection.name, selection.stableProperty))
        r.fail("device selection changed");
    } catch (const std::exception &e) {
      r.fail(e.what());
    }
  }
  void rebuildOutputPortsForPortConfig() noexcept {
    if (!outputPortConfigPending || state != PW_FILTER_STATE_PAUSED ||
        !filter || cleaning || stopping)
      return;

    const auto previousAudioMode = audioMode.load(std::memory_order_seq_cst);
    const bool resumeAfterRebuild = previousAudioMode == AudioMode::Running;
    if (resumeAfterRebuild) {
      if (!quiesceAndDrain()) {
        fail("timed out draining audio callbacks for PipeWire PortConfig");
        return;
      }
    } else if (!waitForCallbacksToDrain()) {
      audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
      fail("timed out draining audio callbacks for PipeWire PortConfig");
      return;
    }

    try {
      for (unsigned channel = 0; channel < channels; ++channel) {
        if (outputs[channel]) {
          const int result = pw_filter_remove_port(outputs[channel]);
          outputs[channel] = nullptr;
          if (result < 0)
            throw std::runtime_error("cannot remove virtual-source output " +
                                     ports[channel].channel + ": " +
                                     std::to_string(result));
        }
      }
      for (unsigned channel = 0; channel < channels; ++channel) {
        const auto name = "capture_" + ports[channel].channel;
        auto *properties = pw_properties_new(
            PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
            name.c_str(), PW_KEY_AUDIO_CHANNEL, ports[channel].channel.c_str(),
            nullptr);
        if (!properties)
          throw std::runtime_error("cannot allocate virtual-source output " +
                                   ports[channel].channel + " properties");
        outputs[channel] = pw_filter_add_port(filter, PW_DIRECTION_OUTPUT,
                                              PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                              1, properties, nullptr, 0);
        if (!outputs[channel])
          throw std::runtime_error("cannot recreate virtual-source output " +
                                   ports[channel].channel);
      }
      outputPortConfigPending = false;
      std::cerr << "skyapod: refreshed fixed virtual-microphone output ports "
                   "for PipeWire PortConfig\n";
    } catch (const std::exception &exception) {
      audioMode.store(AudioMode::Faulted, std::memory_order_seq_cst);
      fail(exception.what());
      return;
    }

    if (resumeAfterRebuild)
      resumeAudio();
  }
  static void paramChanged(void *data, void *port, uint32_t id,
                           const spa_pod *param) {
    auto &r = *static_cast<Runtime *>(data);
    if (port || !param)
      return;
    if (id != SPA_PARAM_Format && id != SPA_PARAM_PortConfig)
      return;
    if (id == SPA_PARAM_Format) {
      spa_audio_info_raw info{};
      if (spa_format_audio_raw_parse(param, &info) < 0 ||
          info.channels != r.channels ||
          (info.format != SPA_AUDIO_FORMAT_F32P &&
           info.format != SPA_AUDIO_FORMAT_F32)) {
        r.fail("unsupported virtual-source format request");
        return;
      }
    }
    if (id == SPA_PARAM_PortConfig) {
      // WirePlumber's si-audio-adapter completes DSP PortConfig only after a
      // ports-changed notification. This filter keeps a fixed channel layout,
      // so explicitly recreate its source ports while the node is suspended.
      r.outputPortConfigPending = true;
      r.virtualMicConfiguredByPolicy = true;
      if (r.playbackGraph)
        r.playbackGraph->sessionManagerDetected();
      if (r.state == PW_FILTER_STATE_PAUSED)
        r.rebuildOutputPortsForPortConfig();
    }
    // Fixed DSP ports are already configured. Acknowledge the node format
    // and signal Props so WirePlumber can finish its adapter transaction.
    pw_filter_update_params(r.filter, nullptr, &param, 1);
    if (id == SPA_PARAM_PortConfig) {
      uint8_t bytes[512];
      std::array<float, MaxChannels> volumes;
      volumes.fill(1.0f);
      spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
      const spa_pod *props = static_cast<spa_pod *>(spa_pod_builder_add_object(
          &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
          SPA_POD_Float(1.0f), SPA_PROP_mute, SPA_POD_Bool(false),
          SPA_PROP_channelVolumes,
          SPA_POD_Array(sizeof(float), SPA_TYPE_Float, r.channels,
                        volumes.data())));
      pw_filter_update_params(r.filter, nullptr, &props, 1);
      if (!r.outputPortConfigPending && r.state == PW_FILTER_STATE_PAUSED)
        activateVirtualMic(r, "WirePlumber PortConfig");
    }
  }
  static void activateVirtualMic(Runtime &r, const char *reason) noexcept {
    if (r.virtualMicActive || !r.filter || r.cleaning || r.stopping ||
        r.audioMode.load(std::memory_order_seq_cst) == AudioMode::Faulted)
      return;
    const int result = pw_filter_set_active(r.filter, true);
    if (result < 0) {
      r.fail("cannot activate virtual microphone after " + std::string(reason) +
             ": " + std::to_string(result));
      return;
    }
    r.virtualMicActive = true;
    std::cerr << "skyapod: virtual microphone activated after " << reason
              << '\n';
  }
  static void virtualMicActivationTimeout(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    // A running session manager owns device-format negotiation. Keep the
    // source inactive until its PortConfig transaction completes. Without a
    // session manager, fixed DSP ports can be started directly after a short
    // startup grace period.
    if (!r.wirePlumberPresent && !r.virtualMicConfiguredByPolicy)
      activateVirtualMic(r, "standalone startup fallback");
  }
  static void coreError(void *data, uint32_t id, int, int res,
                        const char *message) {
    auto &r = *static_cast<Runtime *>(data);
    std::cerr << "skyapod: PipeWire error for object " << id << " (" << res
              << "): " << (message ? message : "") << '\n';
    // A node can disappear while an asynchronous enum/subscribe request is
    // still in flight. PipeWire reports that stale object request on the core
    // proxy as "unknown resource"; it is not a core disconnect and must not
    // tear down unrelated capture processing.
    const bool staleObjectRequest =
        id == PW_ID_CORE && res == -ENOENT && message &&
        std::strncmp(message, "unknown resource ", 17) == 0;
    if (id == PW_ID_CORE && !staleObjectRequest)
      r.fail(message ? message : "PipeWire disconnected");
    else if (staleObjectRequest)
      std::cerr << "skyapod: PipeWire object disappeared during an async "
                   "request; keeping remaining graphs active\n";
    if (r.defaultMetadata && id == pw_proxy_get_id(reinterpret_cast<pw_proxy *>(
                                       r.defaultMetadata))) {
      r.defaultRoutePending = false;
      r.defaultRouteError =
          message ? message : "PipeWire metadata request failed";
    }
    for (auto *link : r.links)
      if (link && id == pw_proxy_get_id(link))
        r.fail(message ? message : "capture link error");
  }
  static void linkInfo(void *data, const pw_link_info *info) {
    auto &c = *static_cast<LinkContext *>(data);
    c.owner->linkGlobalIds[c.channel] = info->id;
    c.owner->linked[c.channel] = info->state == PW_LINK_STATE_ACTIVE;
    if (info->state == PW_LINK_STATE_ERROR)
      c.owner->fail(info->error ? info->error : "capture link failed");
  }
  void createCaptureLink(unsigned channel) {
    if (channel >= channels || ownInputs[channel] == SPA_ID_INVALID ||
        links[channel])
      return;
    linkGlobalIds[channel] = SPA_ID_INVALID;
    auto *properties = pw_properties_new(nullptr, nullptr);
    if (!properties) {
      fail("cannot allocate capture-link properties");
      return;
    }
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", device.id);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       ports[channel].id);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u",
                       pw_filter_get_node_id(filter));
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       ownInputs[channel]);
    links[channel] = reinterpret_cast<pw_proxy *>(
        pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                              PW_VERSION_LINK, &properties->dict, 0));
    pw_properties_free(properties);
    if (!links[channel]) {
      fail("cannot create capture link for " + ports[channel].channel);
      return;
    }
    static const auto events = [] {
      pw_link_events e{};
      e.version = PW_VERSION_LINK_EVENTS;
      e.info = linkInfo;
      return e;
    }();
    linkContexts[channel] = {this, channel};
    const int listenerResult = pw_link_add_listener(
        reinterpret_cast<pw_link *>(links[channel]), &linkHooks[channel],
        &events, &linkContexts[channel]);
    if (listenerResult < 0) {
      fail("cannot listen to capture link: " + std::to_string(listenerResult));
      return;
    }
    std::cerr << "skyapod: linking physical port " << ports[channel].id
              << " -> " << ownInputs[channel] << " (" << ports[channel].channel
              << ", link proxy " << pw_proxy_get_id(links[channel]) << ")\n";
  }
  void scheduleLinkRetry() {
    if (cleaning || stopping || !linkRetryTimer)
      return;
    timespec delay{0, 100000000};
    pw_loop_update_timer(pw_main_loop_get_loop(main), linkRetryTimer, &delay,
                         nullptr, false);
  }
  static void retryLinks(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    for (unsigned c = 0; c < r.channels; ++c)
      r.createCaptureLink(c);
  }
  static void global(void *data, uint32_t id, uint32_t permissions,
                     const char *type, uint32_t version,
                     const spa_dict *props) {
    auto &r = *static_cast<Runtime *>(data);
    if (type && props && std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
      const auto application = get(props, "application.name");
      const auto binary = get(props, "application.process.binary");
      if (application.find("WirePlumber") != std::string::npos ||
          application.find("wireplumber") != std::string::npos ||
          binary.find("wireplumber") != std::string::npos) {
        r.wirePlumberPresent = true;
        std::cerr << "skyapod: detected WirePlumber session manager client\n";
        if (r.playbackGraph)
          r.playbackGraph->sessionManagerDetected();
      }
    }
    if (!r.playbackGraph && type && props &&
        std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0 &&
        get(props, PW_KEY_MEDIA_CLASS) == "Audio/Sink")
      r.schedulePlaybackRecovery();
    if (type && props && std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
        r.defaultMetadataId == SPA_ID_INVALID) {
      const char *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
      if (name && std::strcmp(name, "default") == 0) {
        r.defaultMetadata = static_cast<pw_metadata *>(pw_registry_bind(
            r.registry, id, PW_TYPE_INTERFACE_Metadata,
            std::min(version, static_cast<uint32_t>(PW_VERSION_METADATA)), 0));
        if (!r.defaultMetadata) {
          r.defaultRouteError = "cannot bind PipeWire default metadata";
          return;
        }
        r.defaultMetadataId = id;
        r.defaultMetadataPermissions = permissions;
        static const auto events = [] {
          pw_metadata_events e{};
          e.version = PW_VERSION_METADATA_EVENTS;
          e.property = metadataProperty;
          return e;
        }();
        pw_metadata_add_listener(r.defaultMetadata, &r.defaultMetadataHook,
                                 &events, &r);
        r.defaultMetadataSnapshotSequence = r.requestCoreSync();
        if (r.defaultMetadataSnapshotSequence < 0) {
          r.defaultRouteError = "cannot enumerate PipeWire default metadata";
          r.defaultMetadataSnapshotSequence = -1;
        }
      }
      return;
    }
    if (!props || strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
      return;
    if (get(props, PW_KEY_NODE_ID) !=
            std::to_string(pw_filter_get_node_id(r.filter)) ||
        get(props, PW_KEY_PORT_DIRECTION) != "in")
      return;
    auto channel = get(props, PW_KEY_AUDIO_CHANNEL);
    for (unsigned c = 0; c < r.channels; ++c)
      if (channel == r.ports[c].channel && r.ownInputs[c] == SPA_ID_INVALID) {
        r.ownInputs[c] = id;
        r.createCaptureLink(c);
      }
  }
  static void removed(void *data, uint32_t id) {
    auto &r = *static_cast<Runtime *>(data);
    for (unsigned c = 0; c < r.channels; ++c) {
      if (id != r.linkGlobalIds[c])
        continue;
      spa_hook_remove(&r.linkHooks[c]);
      if (r.links[c])
        pw_proxy_destroy(r.links[c]);
      r.links[c] = nullptr;
      r.linkGlobalIds[c] = SPA_ID_INVALID;
      r.linked[c] = false;
      std::cerr << "skyapod: capture link removed for " << r.ports[c].channel
                << "; scheduling recovery\n";
      r.scheduleLinkRetry();
    }
    if (id == r.defaultMetadataId) {
      if (r.defaultMetadata) {
        spa_hook_remove(&r.defaultMetadataHook);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(r.defaultMetadata));
        r.defaultMetadata = nullptr;
      }
      r.defaultMetadataId = SPA_ID_INVALID;
      r.defaultMetadataSnapshotReady = false;
      r.defaultRouteSnapshotChecked = false;
      r.originalDefaultMetadataCaptured = false;
      r.originalConfiguredDefaultCaptured = false;
      r.defaultRouteActiveConfirmed = false;
      r.defaultRouteConfiguredConfirmed = false;
      r.defaultRouteSyncConfirmed = false;
      r.defaultRouteOwned = false;
      r.defaultRoutePending = false;
      r.defaultRouteAttempted = false;
      r.defaultRouteSetRequested = false;
      r.defaultRouteRestoring = false;
      r.defaultRouteOverridden = false;
      r.originalDefaultMetadataPresent = false;
      r.originalDefaultMetadataType.clear();
      r.originalDefaultMetadataValue.clear();
      r.originalConfiguredDefaultMetadataPresent = false;
      r.originalConfiguredDefaultMetadataType.clear();
      r.originalConfiguredDefaultMetadataValue.clear();
      r.defaultMetadataPresent = false;
      r.defaultMetadataType.clear();
      r.defaultMetadataValue.clear();
      r.defaultMetadataName.clear();
      r.configuredDefaultMetadataPresent = false;
      r.configuredDefaultMetadataType.clear();
      r.configuredDefaultMetadataValue.clear();
      r.configuredDefaultMetadataName.clear();
      r.defaultRouteError = "PipeWire default metadata disappeared";
    }
    if (id == r.device.id)
      r.fail("selected capture device disappeared");
    for (auto &p : r.ports)
      if (id == p.id)
        r.fail("physical capture port disappeared");
  }
  static void signal(void *data, int) {
    auto &r = *static_cast<Runtime *>(data);
    r.stopping = 1;
    pw_main_loop_quit(r.main);
  }
  static void renderVolumeChanged(
      void *data, const skyapo::pipewire::DefaultSinkVolumeSnapshot &snapshot) {
    auto &r = *static_cast<Runtime *>(data);
    r.renderVolume = snapshot;
    skyapo::platform::LoudnessVolumeProvider::publish(
        snapshot.uniformChannelGainAvailable, snapshot.uniformChannelGainDb);
  }
  std::string status() {
    std::ostringstream s;
    const auto n = samples.load(), b = blocks.load();
    auto *e = active.load();
    const auto dspMode = audioMode.load(std::memory_order_seq_cst);
    const auto activeRate = rate.load(std::memory_order_relaxed);
    const auto activeQuantum = quantum.load(std::memory_order_relaxed);
    const bool engineFormatReady = e && activeRate && activeQuantum &&
                                   e->sampleRate() == activeRate &&
                                   e->supportsFrameCount(activeQuantum);
    const char *dspState =
        dspMode == AudioMode::Faulted &&
                callbackProcessingFaulted.load(std::memory_order_seq_cst)
            ? "paused (realtime processing exception; output silence)"
        : dspMode == AudioMode::Faulted
            ? "paused (plugin latency refresh failed; output silence)"
        : dspMode == AudioMode::Stopping  ? "stopping"
        : dspMode == AudioMode::Quiescing ? "quiescing (output silence)"
        : (!e || !activeRate || !activeQuantum)
            ? "initializing (awaiting negotiated format)"
        : !engineFormatReady ? "reconfiguring (format change; output silence)"
                             : "running";
    s << "Daemon: " << pw_filter_state_as_string(state)
      << "\nDSP state: " << dspState << "\nSelected device: " << device.name
      << "\nCapture node: " << device.id << " (" << device.description
      << ")\nVirtual microphone: SkyAPO Virtual Mic\nVirtual node: "
      << pw_filter_get_node_id(filter)
      << " (skyapo.virtual_mic)\nFormat: F32 planar DSP\nChannels: " << channels
      << "\nGraph transition: "
      << (pending.load(std::memory_order_seq_cst) ? "crossfading" : "stable")
      << "\nFormat rebuilds during transition: "
      << formatRebuildsDuringTransition;
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    if (std::getenv("SKYAPO_TEST_FORMAT_REBUILD_BEFORE_PDC_EVENT"))
      s << "\nTest PDC format rebuild: "
        << (testFormatRebuildDeferred.load(std::memory_order_seq_cst)
                ? "deferred"
                : "not deferred");
#endif
    s << "\nPlugin latency refreshes: " << pluginLatencyRefreshes
      << "\nChannel positions:";
    for (auto &p : ports)
      s << ' ' << p.channel;
    s << "\nSample rate: ";
    if (rate.load())
      s << rate.load() << " Hz";
    else
      s << "unknown (awaiting graph)";
    s << "\nQuantum: ";
    if (quantum.load())
      s << quantum.load();
    else
      s << "unknown";
    s << "\nLoudness volume endpoint: ";
    if (!renderVolume.available) {
      s << (renderVolume.stableName.empty()
                ? "unavailable (no default audio sink volume)"
                : renderVolume.stableName + " (volume unavailable)");
    } else {
      s << renderVolume.stableName << " (effective ";
      if (renderVolume.muted)
        s << "muted, ";
      s << renderVolume.effectiveDb << " dB)";
    }
    s << "\nLoudness volume input: ";
    if (renderVolume.uniformChannelGainAvailable)
      s << renderVolume.uniformChannelGainDb << " dB uniform channel gain";
    else
      s << "unavailable (channel gains are not a positive uniform scalar)";
    s << "\nFilters: ";
    if (e)
      s << e->filterCount();
    else
      s << "unknown (DSP engine not initialized)";
    s << "\nFilter chain:";
    if (!e)
      s << "\n  (DSP engine not initialized)";
    else if (e->filterDescriptions().empty())
      s << "\n  (none)";
    else
      for (const auto &filter : e->filterDescriptions())
        s << "\n  " << filter;
    if (e && !e->configNotices().empty()) {
      s << "\nConfig warnings:";
      for (const auto &notice : e->configNotices())
        s << "\n  " << notice.file.string() << ':' << notice.line << ": "
          << notice.message;
    }
    if (playbackGraph)
      s << playbackGraph->status()
        << "\nPlayback routing policy: " << defaultRouteStatus()
        << "\nPipeWire default sink: "
        << (defaultMetadataName.empty() ? "unknown" : defaultMetadataName)
        << '\n';
    else
      s << "Playback graph: unavailable"
        << (playbackError.empty() ? " (no usable default/output selection)"
                                  : " (" + playbackError + ")")
        << '\n';
    s << "\nPlugin-reported latency sum: ";
    const auto pluginLatency =
        e ? e->pluginLatencySamples() : std::optional<uint64_t>{};
    if (pluginLatency)
      s << *pluginLatency << " samples (sum only; not end-to-end latency)";
    else
      s << "unknown";
    s << "\nPlugin latency compensation: "
      << (e && e->pluginLatencyCompensationActive() ? "active" : "inactive")
      << " (plugin-reported latency only)";
    s << "\nConfig: " << config << "\nProcessed blocks: " << b
      << "\nOverruns: " << overruns.load();
    s << "\nActive capture links: "
      << std::count(linked.begin(), linked.begin() + channels, true) << '/'
      << channels;
    if (b)
      s << "\nProcess average: " << double(totalNs.load()) / b / 1000
        << " us\nProcess maximum: " << double(maxNs.load()) / 1000 << " us";
    if (n) {
      s << "\nInput RMS: " << std::sqrt(rawEnergy.load() / n)
        << "\nOutput RMS: " << std::sqrt(processedEnergy.load() / n);
      if (rawEnergy.load() > 0)
        s << "\nDSP amplitude ratio: "
          << std::sqrt(processedEnergy.load() / rawEnergy.load());
      else
        s << "\nDSP amplitude ratio: unavailable (silent input)";
    }
    s << "\nCallback allocations: " << realtime::allocations.load()
      << "\nCallback deallocations: " << realtime::deallocations.load()
      << "\nAudit scope: executable C++ and wrapped C allocation calls; "
         "shared-library C allocators excluded\n";
    if (!configError.empty())
      s << "Config reload error: " << configError << '\n';
    if (callbackProcessingFaulted.load(std::memory_order_seq_cst))
      s << "DSP callback error: processing exception; valid config reload "
           "required\n";
    if (!pluginLatencyError.empty())
      s << "Plugin latency update error: " << pluginLatencyError << '\n';
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    if (testLatencyDrainFailureInjected)
      s << "Test latency callback drain timeout: injected\n";
#endif
    s << "Plugin failures:";
    const auto failures =
        e ? e->failedPluginDescriptions() : std::vector<std::string>{};
    if (failures.empty()) {
      s << " (none)\n";
    } else {
      for (const auto &failure : failures)
        s << "\n  " << failure;
      s << '\n';
    }
    return s.str();
  }
  static void statusReady(void *data, int, uint32_t) {
    auto &r = *static_cast<Runtime *>(data);
    if (r.cleaning || r.server < 0)
      return;
    for (;;) {
      int client =
          accept4(r.server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (client < 0)
        break;
      settings::ipc::DecodedRequest request;
      try {
        request = settings::ipc::decodeRequest(readControlRequest(client));
      } catch (const std::exception &e) {
        sendControlResponse(client, false, std::string(e.what()) + "\n");
        close(client);
        continue;
      }
      if (request.command == settings::ipc::Command::Invalid) {
        sendControlResponse(client, false, request.error + "\n");
        close(client);
        continue;
      }
      std::string text;
      if (request.command == settings::ipc::Command::Status) {
        text = r.status();
      } else if (request.command == settings::ipc::Command::Reload) {
        try {
          r.consumePendingConfigReload();
          r.reloadConfig();
          text = "Config reload succeeded\n";
        } catch (const std::exception &e) {
          r.configError = e.what();
          const bool faulted =
              r.audioMode.load(std::memory_order_seq_cst) == AudioMode::Faulted;
          text = std::string("Config reload failed; ") +
                 (faulted ? "DSP remains paused and outputting silence: "
                          : "keeping last valid graph: ") +
                 e.what() + "\n";
          sendControlResponse(client, false, text);
          close(client);
          continue;
        }
      } else if (request.command == settings::ipc::Command::RefreshOutput) {
        try {
          text = r.refreshPlaybackOutput();
        } catch (const std::exception &e) {
          r.playbackError = e.what();
          sendControlResponse(client, false, std::string(e.what()) + "\n");
          close(client);
          continue;
        }
      } else if (request.command ==
                 settings::ipc::Command::SetPluginParameter) {
        try {
          if (r.pending.load(std::memory_order_seq_cst))
            throw std::runtime_error(
                "plugin controls are unavailable during the brief DSP graph "
                "transition; retry shortly");
          auto *engine = r.active.load(std::memory_order_acquire);
          if (!engine)
            throw std::runtime_error("audio graph is not active");
          engine->setPluginParameter(request.pluginId, request.parameter,
                                     request.value);
          text = "Updated " + request.pluginId + " parameter " +
                 request.parameter + " to " + std::to_string(request.value) +
                 "\n";
        } catch (const std::exception &e) {
          sendControlResponse(client, false, std::string(e.what()) + "\n");
          close(client);
          continue;
        }
      } else if (request.command == settings::ipc::Command::SetPluginBypass) {
        try {
          if (r.pending.load(std::memory_order_seq_cst))
            throw std::runtime_error(
                "plugin controls are unavailable during the brief DSP graph "
                "transition; retry shortly");
          auto *engine = r.active.load(std::memory_order_acquire);
          if (!engine)
            throw std::runtime_error("audio graph is not active");
          engine->setPluginBypass(request.pluginId, request.bypassed);
          text = std::string(request.bypassed ? "Bypassed " : "Unbypassed ") +
                 request.pluginId + "\n";
        } catch (const std::exception &e) {
          sendControlResponse(client, false, std::string(e.what()) + "\n");
          close(client);
          continue;
        }
      } else if (request.command == settings::ipc::Command::Stop) {
        r.stopping = 1;
        text = "Stopping skyapod\n";
      }
      sendControlResponse(client, true, text);
      close(client);
      if (r.stopping) {
        pw_main_loop_quit(r.main);
        break;
      }
    }
  }
  void init(const std::string &selected, const std::string &stableProperty,
            const std::string &cfg) {
    config = cfg;
    auto all = enumerateDevices();
    const auto *selectedDevice =
        findSelectedDevice(all, selected, stableProperty);
    if (!selectedDevice)
      throw std::runtime_error(stableProperty.empty()
                                   ? "selected device unavailable: " + selected
                                   : "selected device identity unavailable: " +
                                         stableProperty);
    device = *selectedDevice;
    for (auto &p : all.ports)
      if (p.node == device.id && p.direction == "out")
        ports.push_back(p);
    if (ports.empty() || ports.size() > MaxChannels)
      throw std::runtime_error(
          "selected source has no usable ports or more than 8 channels");
    const std::vector<std::string> order = {"MONO", "FL", "FR", "FC", "LFE",
                                            "RL",   "RR", "SL", "SR"};
    std::sort(ports.begin(), ports.end(), [&](auto &a, auto &b) {
      return std::find(order.begin(), order.end(), a.channel) <
             std::find(order.begin(), order.end(), b.channel);
    });
    for (auto &p : ports)
      if (p.channel.empty())
        throw std::runtime_error("source port lacks channel position");
    channels = ports.size();
    work.resize(channels * MaxFrames);
    pw_init(nullptr, nullptr);
    initialized = true;
    main = pw_main_loop_new(nullptr);
    if (!main)
      throw std::runtime_error("cannot create PipeWire main loop");
    auto *loop = pw_main_loop_get_loop(main);
    context = pw_context_new(loop, nullptr, 0);
    if (!context)
      throw std::runtime_error("cannot create PipeWire context");
    core = pw_context_connect(context, nullptr, 0);
    if (!core)
      throw std::runtime_error("PipeWire connect failed");
    static const auto ce = [] {
      pw_core_events e{};
      e.version = PW_VERSION_CORE_EVENTS;
      e.done = coreDone;
      e.error = coreError;
      return e;
    }();
    pw_core_add_listener(core, &coreHook, &ce, this);
    rateEvent = pw_loop_add_event(loop, rateChanged, this);
    transitionEvent = pw_loop_add_event(loop, transitionReady, this);
    pluginLatencyEvent = pw_loop_add_event(loop, pluginLatencyChanged, this);
    processFaultEvent = pw_loop_add_event(loop, processFaultReady, this);
    sigint = pw_loop_add_signal(loop, SIGINT, signal, this);
    sigterm = pw_loop_add_signal(loop, SIGTERM, signal, this);
    selectionTimer = pw_loop_add_timer(loop, selectionChanged, this);
    playbackRecoveryTimer = pw_loop_add_timer(loop, playbackRecovery, this);
    defaultRouteTimer = pw_loop_add_timer(loop, defaultRouteReady, this);
    reloadTimer = pw_loop_add_timer(loop, reloadReady, this);
    linkRetryTimer = pw_loop_add_timer(loop, retryLinks, this);
    virtualMicActivationTimer =
        pw_loop_add_timer(loop, virtualMicActivationTimeout, this);
    configWatcher = std::make_unique<skyapo::platform::ConfigWatcher>(config);
    configEvent = pw_loop_add_io(loop, configWatcher->fileDescriptor(),
                                 SPA_IO_IN, false, configReady, this);
    if (!rateEvent || !transitionEvent || !pluginLatencyEvent ||
        !processFaultEvent || !sigint || !sigterm || !selectionTimer ||
        !playbackRecoveryTimer || !defaultRouteTimer || !reloadTimer ||
        !linkRetryTimer || !virtualMicActivationTimer || !configEvent)
      throw std::runtime_error("cannot create PipeWire loop events");
    timespec interval{1, 0};
    pw_loop_update_timer(loop, selectionTimer, &interval, &interval, false);
    auto *props = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.virtual_mic", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO Virtual Mic", PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_AUTOCONNECT, "true",
        PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_NODE_PAUSE_ON_IDLE, "false",
        nullptr);
    pw_properties_setf(props, PW_KEY_AUDIO_CHANNELS, "%u", channels);
    // DSP ports already exist and must not be rebuilt by a session manager.
    std::string positions = "[ ";
    for (auto &p : ports)
      positions += p.channel + " ";
    positions += "]";
    pw_properties_set(props, SPA_KEY_AUDIO_POSITION, positions.c_str());
    filter = pw_filter_new(core, "SkyAPO Virtual Mic", props);
    if (!filter)
      throw std::runtime_error("cannot create PipeWire filter");
    static const auto fe = [] {
      pw_filter_events e{};
      e.version = PW_VERSION_FILTER_EVENTS;
      e.state_changed = stateChanged;
      e.param_changed = paramChanged;
      e.process = process;
      return e;
    }();
    pw_filter_add_listener(filter, &filterHook, &fe, this);
    for (unsigned c = 0; c < channels; ++c) {
      auto inName = "input_" + ports[c].channel,
           outName = "capture_" + ports[c].channel;
      inputs[c] = pw_filter_add_port(
          filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, inName.c_str(),
                            PW_KEY_AUDIO_CHANNEL, ports[c].channel.c_str(),
                            nullptr),
          nullptr, 0);
      outputs[c] = pw_filter_add_port(
          filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, outName.c_str(),
                            PW_KEY_AUDIO_CHANNEL, ports[c].channel.c_str(),
                            nullptr),
          nullptr, 0);
      if (!inputs[c] || !outputs[c])
        throw std::runtime_error("cannot create DSP ports");
    }
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    static const auto re = [] {
      pw_registry_events e{};
      e.version = PW_VERSION_REGISTRY_EVENTS;
      e.global = global;
      e.global_remove = removed;
      return e;
    }();
    pw_registry_add_listener(registry, &registryHook, &re, this);
    renderVolumeMonitor =
        std::make_unique<skyapo::pipewire::DefaultSinkVolumeMonitor>(
            core, registry, renderVolumeChanged, this);
    renderVolume = renderVolumeMonitor->snapshot();
    skyapo::platform::LoudnessVolumeProvider::publish(
        renderVolume.uniformChannelGainAvailable,
        renderVolume.uniformChannelGainDb);
    tryCreatePlaybackGraph(all);
    uint8_t bytes[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;
    info.channels = channels;
    const std::vector<std::pair<std::string, uint32_t>> positionsMap = {
        {"MONO", SPA_AUDIO_CHANNEL_MONO}, {"FL", SPA_AUDIO_CHANNEL_FL},
        {"FR", SPA_AUDIO_CHANNEL_FR},     {"FC", SPA_AUDIO_CHANNEL_FC},
        {"LFE", SPA_AUDIO_CHANNEL_LFE},   {"RL", SPA_AUDIO_CHANNEL_RL},
        {"RR", SPA_AUDIO_CHANNEL_RR},     {"SL", SPA_AUDIO_CHANNEL_SL},
        {"SR", SPA_AUDIO_CHANNEL_SR}};
    for (unsigned c = 0; c < channels; ++c) {
      auto p = std::find_if(
          positionsMap.begin(), positionsMap.end(),
          [&](auto &pair) { return pair.first == ports[c].channel; });
      if (p == positionsMap.end())
        throw std::runtime_error("unsupported channel position " +
                                 ports[c].channel);
      info.position[c] = p->second;
    }
    std::array<float, MaxChannels> unityVolumes;
    unityVolumes.fill(1.0f);
    // Advertise the rates the DSP core can rebuild for, rather than pinning
    // the PipeWire graph to the old prototype's 48 kHz default. Keep F32P and
    // the selected node's channel positions fixed; the session manager chooses
    // one of these rates and process() observes it through clock.rate.
    spa_pod_frame formatFrame;
    spa_pod_builder_push_object(&b, &formatFrame, SPA_TYPE_OBJECT_Format,
                                SPA_PARAM_EnumFormat);
    spa_pod_builder_add(
        &b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_AUDIO_format, SPA_POD_Id(info.format), SPA_FORMAT_AUDIO_rate,
        SPA_POD_CHOICE_ENUM_Int(3, 48000, 44100, 96000),
        SPA_FORMAT_AUDIO_channels, SPA_POD_Int(channels), 0);
    if (!SPA_FLAG_IS_SET(info.flags, SPA_AUDIO_FLAG_UNPOSITIONED))
      spa_pod_builder_add(
          &b, SPA_FORMAT_AUDIO_position,
          SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, channels, info.position),
          0);
    const spa_pod *enumFormat =
        static_cast<spa_pod *>(spa_pod_builder_pop(&b, &formatFrame));
    const spa_pod *params[] = {
        enumFormat, static_cast<spa_pod *>(spa_pod_builder_add_object(
                        &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                        SPA_PROP_volume, SPA_POD_Float(1.0f), SPA_PROP_mute,
                        SPA_POD_Bool(false), SPA_PROP_channelVolumes,
                        SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                                      unityVolumes.data())))};
    if (pw_filter_connect(
            filter,
            static_cast<pw_filter_flags>(PW_FILTER_FLAG_RT_PROCESS |
                                         PW_FILTER_FLAG_INACTIVE),
            params, 2) < 0)
      throw std::runtime_error("filter connection failed");
    timespec activationGrace{0, 500000000};
    pw_loop_update_timer(loop, virtualMicActivationTimer, &activationGrace,
                         nullptr, false);
    path = settings::socketPath();
    server = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (server < 0)
      throw std::runtime_error("status socket failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path))
      throw std::runtime_error("socket path too long");
    std::copy(path.begin(), path.end(), address.sun_path);
    if (bind(server, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
        0)
      throw std::runtime_error(
          "status socket already exists; another daemon or stale socket");
    ownsSocket = true;
    chmod(path.c_str(), 0600);
    if (listen(server, 8) < 0)
      throw std::runtime_error("socket listen failed");
    statusEvent =
        pw_loop_add_io(loop, server, SPA_IO_IN, false, statusReady, this);
    if (!statusEvent)
      throw std::runtime_error("cannot attach status socket to loop");
  }
};
} // namespace
void runPipeWire(const std::string &device, const std::string &stableProperty,
                 const std::string &config, volatile sig_atomic_t &stopping) {
  Runtime r(stopping);
  r.init(device, stableProperty, config);
  pw_main_loop_run(r.main);
  r.shutdownAudio();
  if (!r.error.empty())
    throw std::runtime_error(r.error);
}
