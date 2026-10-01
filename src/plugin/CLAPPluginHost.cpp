#include "CLAPPluginHost.h"
#include "IPluginFailureState.h"
#include "IPluginLatencyState.h"
#include "IPluginLatencyRefresh.h"
#include "IPluginParameterControl.h"
#include "IPluginBypassControl.h"
#include "IPluginSourceContext.h"
#include "IPluginStatePersistence.h"

#include "IFilter.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include <clap/clap.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>

#ifndef SKYAPO_VERSION
#define SKYAPO_VERSION "0.1.0"
#endif

namespace fs = std::filesystem;
static_assert(std::atomic<bool>::is_always_lock_free,
              "CLAP failure latch must be lock-free on the audio thread");
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "CLAP parameter revisions must be lock-free on the audio thread");
static_assert(std::atomic<float>::is_always_lock_free,
              "CLAP parameter values must be lock-free on the audio thread");

namespace {
constexpr size_t MaxClapStateBytes = 16 * 1024 * 1024;
constexpr std::array<char, 8> StateMagic{'S', 'K', 'Y', 'C', 'L', 'A', 'P', '1'};

struct MemoryStream {
  const std::vector<uint8_t> *input{};
  std::vector<uint8_t> output;
  size_t cursor{};
  bool failed{};
};

int64_t CLAP_ABI stateRead(const clap_istream_t *stream, void *buffer,
                           uint64_t size) {
  auto &state = *static_cast<MemoryStream *>(stream->ctx);
  if ((!buffer && size) || size > static_cast<uint64_t>(INT64_MAX)) {
    state.failed = true;
    return -1;
  }
  if (!state.input || state.cursor >= state.input->size() || !size)
    return 0;
  const size_t count = std::min<uint64_t>(
      size, state.input->size() - state.cursor);
  std::memcpy(buffer, state.input->data() + state.cursor, count);
  state.cursor += count;
  return static_cast<int64_t>(count);
}

int64_t CLAP_ABI stateWrite(const clap_ostream_t *stream, const void *buffer,
                            uint64_t size) {
  auto &state = *static_cast<MemoryStream *>(stream->ctx);
  if ((!buffer && size) || size > MaxClapStateBytes ||
      state.output.size() > MaxClapStateBytes - size) {
    state.failed = true;
    return -1;
  }
  if (!size)
    return 0;
  const auto *bytes = static_cast<const uint8_t *>(buffer);
  try {
    state.output.insert(state.output.end(), bytes, bytes + size);
  } catch (...) {
    state.failed = true;
    return -1;
  }
  return static_cast<int64_t>(size);
}

std::string stateIdentity(const fs::path &source, unsigned line,
                          const std::string &pluginId) {
  return source.generic_string() + "\n" + std::to_string(line) + "\n" +
         pluginId;
}

uint64_t stableHash(std::string_view value) {
  uint64_t hash = 14695981039346656037ull;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

uint64_t stateChecksum(const std::string &identity,
                       const std::vector<uint8_t> &payload) {
  uint64_t hash = stableHash(identity);
  hash ^= 0xff;
  hash *= 1099511628211ull;
  for (const uint8_t byte : payload) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

fs::path stateDirectory() {
  if (const char *xdg = std::getenv("XDG_STATE_HOME")) {
    fs::path base(xdg);
    if (base.is_absolute())
      return base / "skyapo" / "clap-state";
  }
  if (const char *home = std::getenv("HOME"))
    return fs::path(home) / ".local" / "state" / "skyapo" / "clap-state";
  throw std::runtime_error(
      "CLAP state persistence needs absolute XDG_STATE_HOME or HOME");
}

fs::path statePathFor(const std::string &identity) {
  std::ostringstream name;
  name << std::hex << std::setw(16) << std::setfill('0')
       << stableHash(identity) << ".clapstate";
  return stateDirectory() / name.str();
}

void appendU32(std::vector<uint8_t> &out, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void appendU64(std::vector<uint8_t> &out, uint64_t value) {
  for (unsigned i = 0; i < 8; ++i)
    out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
bool takeU32(const std::vector<uint8_t> &in, size_t &at, uint32_t &value) {
  if (in.size() - at < 4)
    return false;
  value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= uint32_t(in[at++]) << (i * 8);
  return true;
}
bool takeU64(const std::vector<uint8_t> &in, size_t &at, uint64_t &value) {
  if (in.size() - at < 8)
    return false;
  value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= uint64_t(in[at++]) << (i * 8);
  return true;
}

std::vector<uint8_t> encodeState(const std::string &identity,
                                 const std::vector<uint8_t> &payload) {
  if (identity.size() > 4096 || payload.size() > MaxClapStateBytes)
    throw std::runtime_error("CLAP state exceeds supported size limit");
  std::vector<uint8_t> out;
  out.reserve(StateMagic.size() + 4 + 8 + 8 + 8 + identity.size() +
              payload.size());
  out.insert(out.end(), StateMagic.begin(), StateMagic.end());
  appendU32(out, 1);
  appendU64(out, identity.size());
  appendU64(out, payload.size());
  appendU64(out, stateChecksum(identity, payload));
  out.insert(out.end(), identity.begin(), identity.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

std::vector<uint8_t> decodeState(const std::vector<uint8_t> &data,
                                 const std::string &identity) {
  size_t at = StateMagic.size();
  uint32_t version{};
  uint64_t identitySize{}, payloadSize{}, checksum{};
  if (data.size() < StateMagic.size() ||
      !std::equal(StateMagic.begin(), StateMagic.end(), data.begin()) ||
      !takeU32(data, at, version) || version != 1 ||
      !takeU64(data, at, identitySize) || identitySize > 4096 ||
      !takeU64(data, at, payloadSize) || payloadSize > MaxClapStateBytes ||
      !takeU64(data, at, checksum) ||
      identitySize > data.size() - at ||
      payloadSize != data.size() - at - identitySize)
    throw std::runtime_error("corrupt CLAP state sidecar");
  const std::string storedIdentity(
      reinterpret_cast<const char *>(data.data() + at), identitySize);
  at += static_cast<size_t>(identitySize);
  if (storedIdentity != identity)
    throw std::runtime_error("CLAP state sidecar identity mismatch");
  std::vector<uint8_t> payload(data.begin() + at, data.end());
  if (checksum != stateChecksum(identity, payload))
    throw std::runtime_error("CLAP state sidecar checksum mismatch");
  return payload;
}

struct ScopedFd {
  int value{-1};
  explicit ScopedFd(int fd) : value(fd) {}
  ~ScopedFd() {
    if (value >= 0)
      close(value);
  }
  int release() noexcept {
    const int fd = value;
    value = -1;
    return fd;
  }
  ScopedFd(const ScopedFd &) = delete;
  ScopedFd &operator=(const ScopedFd &) = delete;
};

std::optional<std::vector<uint8_t>> readStateFile(const fs::path &path,
                                                  const std::string &identity) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    if (errno == ENOENT)
      return std::nullopt;
    throw std::runtime_error("cannot open CLAP state sidecar: " +
                             path.string() + ": " + std::strerror(errno));
  }
  ScopedFd owner(fd);
  struct stat status {};
  if (fstat(fd, &status) < 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
      (status.st_mode & 0077) != 0 || status.st_uid != geteuid() ||
      static_cast<uint64_t>(status.st_size) > MaxClapStateBytes + 8192) {
    throw std::runtime_error("invalid or oversized CLAP state sidecar: " +
                             path.string());
  }
  std::vector<uint8_t> data(static_cast<size_t>(status.st_size));
  size_t offset = 0;
  while (offset < data.size()) {
    const auto count = read(fd, data.data() + offset, data.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("short read from CLAP state sidecar: " +
                               path.string());
    offset += static_cast<size_t>(count);
  }
  return decodeState(data, identity);
}

void writeAll(int fd, const uint8_t *data, size_t size) {
  size_t offset = 0;
  while (offset < size) {
    const auto count = write(fd, data + offset, size - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      throw std::runtime_error("cannot write CLAP state sidecar: " +
                               std::string(std::strerror(errno)));
    offset += static_cast<size_t>(count);
  }
}

void atomicWriteState(const fs::path &path, const std::string &identity,
                      const std::vector<uint8_t> &payload) {
  auto data = encodeState(identity, payload);
  const auto directory = path.parent_path();
  std::error_code ec;
  fs::create_directories(directory, ec);
  if (ec)
    throw std::runtime_error("cannot create CLAP state directory: " +
                             directory.string() + ": " + ec.message());
  if (fs::is_symlink(fs::symlink_status(directory, ec)) || ec)
    throw std::runtime_error("CLAP state directory must not be a symlink: " +
                             directory.string());
  if (chmod(directory.c_str(), 0700) < 0)
    throw std::runtime_error("cannot secure CLAP state directory: " +
                             directory.string() + ": " + std::strerror(errno));
  std::string pattern = (path.string() + ".tmp-XXXXXX");
  std::vector<char> temp(pattern.begin(), pattern.end());
  temp.push_back('\0');
  const int rawFd = mkstemp(temp.data());
  if (rawFd < 0)
    throw std::runtime_error("cannot create CLAP state temporary file: " +
                             std::string(std::strerror(errno)));
  ScopedFd owner(rawFd);
  bool renamed = false;
  try {
    if (fchmod(owner.value, 0600) < 0)
      throw std::runtime_error("cannot set CLAP state file permissions");
    writeAll(owner.value, data.data(), data.size());
    if (fsync(owner.value) < 0)
      throw std::runtime_error("cannot sync CLAP state sidecar");
    const int fd = owner.release();
    if (close(fd) < 0)
      throw std::runtime_error("cannot close CLAP state sidecar");
    if (rename(temp.data(), path.c_str()) < 0)
      throw std::runtime_error("cannot atomically replace CLAP state sidecar: " +
                               std::string(std::strerror(errno)));
    renamed = true;
    const int dirFd = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirFd < 0)
      throw std::runtime_error("cannot open CLAP state directory for sync: " +
                               std::string(std::strerror(errno)));
    ScopedFd dirOwner(dirFd);
    if (fsync(dirFd) < 0)
      throw std::runtime_error("cannot sync CLAP state directory: " +
                               std::string(std::strerror(errno)));
  } catch (...) {
    if (!renamed)
      unlink(temp.data());
    throw;
  }
}

struct ClapLibrary {
  void *handle{};
  const clap_plugin_entry_t *entry{};
  const clap_plugin_factory_t *factory{};
  fs::path path;
  bool initialized = false;
  ~ClapLibrary() {
    if (initialized)
      entry->deinit();
    if (handle)
      dlclose(handle);
  }
};

std::vector<fs::path> searchPaths() {
  std::vector<fs::path> paths;
  if (const char *env = std::getenv("CLAP_PATH")) {
    std::string list(env);
    size_t begin = 0;
    while (begin <= list.size()) {
      const size_t end = list.find(':', begin);
      if (end != begin)
        paths.emplace_back(list.substr(begin, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - begin));
      if (end == std::string::npos)
        break;
      begin = end + 1;
    }
  }
  const char *pathsOnly = std::getenv("SKYAPO_CLAP_PATHS_ONLY");
  if (!pathsOnly || std::string_view(pathsOnly) != "1") {
    if (const char *home = std::getenv("HOME"))
      paths.emplace_back(fs::path(home) / ".clap");
    paths.emplace_back("/usr/lib/clap");
    paths.emplace_back("/usr/local/lib/clap");
    paths.emplace_back("/usr/lib64/clap");
  }
  return paths;
}

std::vector<fs::path> pluginFiles() {
  std::vector<fs::path> result;
  for (const auto &root : searchPaths()) {
    std::error_code ec;
    if (!fs::exists(root, ec))
      continue;
    if (fs::is_regular_file(root, ec) && root.extension() == ".clap") {
      result.push_back(root);
      continue;
    }
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (it->is_regular_file(ec) && it->path().extension() == ".clap")
        result.push_back(it->path());
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::shared_ptr<ClapLibrary> openLibrary(const fs::path &path) {
  auto library = std::make_shared<ClapLibrary>();
  library->path = path;
  library->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library->handle)
    return {};
  library->entry = static_cast<const clap_plugin_entry_t *>(
      dlsym(library->handle, "clap_entry"));
  if (!library->entry ||
      !clap_version_is_compatible(library->entry->clap_version))
    return {};
  if (!library->entry->init(path.c_str()))
    return {};
  library->initialized = true;
  library->factory = static_cast<const clap_plugin_factory_t *>(
      library->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!library->factory)
    return {};
  return library;
}

thread_local const clap_host_t *currentClapAudioHost = nullptr;
struct ClapHostContext {
  pthread_t mainThread{};
  std::atomic<bool> restartRequested{false};
  std::atomic<bool> latencyChanged{false};
};
bool CLAP_ABI hostIsMainThread(const clap_host_t *host) {
  return host && host->host_data &&
         pthread_equal(
             static_cast<const ClapHostContext *>(host->host_data)->mainThread,
             pthread_self());
}
bool CLAP_ABI hostIsAudioThread(const clap_host_t *host) {
  return host && currentClapAudioHost == host;
}
const clap_host_thread_check_t threadCheck{hostIsMainThread, hostIsAudioThread};
void CLAP_ABI hostLatencyChanged(const clap_host_t *host) {
  if (host && host->host_data)
    static_cast<ClapHostContext *>(host->host_data)
        ->latencyChanged.store(true, std::memory_order_release);
}
void CLAP_ABI hostRequestRestart(const clap_host_t *host) {
  if (host && host->host_data)
    static_cast<ClapHostContext *>(host->host_data)
        ->restartRequested.store(true, std::memory_order_release);
}
const clap_host_latency_t hostLatency{hostLatencyChanged};
const void *CLAP_ABI hostExtension(const clap_host_t *, const char *id) {
  if (!id)
    return nullptr;
  if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0)
    return &threadCheck;
  if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
    return &hostLatency;
  return nullptr;
}
void CLAP_ABI hostRequest(const clap_host_t *) {}
bool CLAP_ABI rejectEvent(const clap_output_events_t *,
                          const clap_event_header_t *) {
  return false;
}

class CLAPInstance final : public IPluginInstance,
                           public IPluginParameterControl,
                           public IPluginLatencyRefresh,
                           public IPluginStatePersistence {
public:
  CLAPInstance(std::shared_ptr<ClapLibrary> lib,
               const clap_plugin_descriptor_t *descriptor, std::string id,
               float sampleRate, unsigned maxFrames,
               const std::vector<std::wstring> &channels,
               const std::vector<PluginParameterValue> &overrides,
               std::string stateIdentity = {})
      : library(std::move(lib)), pluginId(std::move(id)),
        sampleRate(sampleRate), maxFrameCount(maxFrames),
        inputChannels(channels.size()), outputChannels(channels.size()),
        persistentIdentity(std::move(stateIdentity)) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || !maxFrames ||
        channels.empty())
      throw std::runtime_error("invalid CLAP audio configuration");
    host.clap_version = CLAP_VERSION;
    hostContext.mainThread = pthread_self();
    host.host_data = &hostContext;
    host.name = "SkyAPO";
    host.vendor = "SkyAPO project";
    host.url = "https://github.com/skyapo/skyapo";
    host.version = SKYAPO_VERSION;
    host.get_extension = hostExtension;
    host.request_restart = hostRequestRestart;
    host.request_process = hostRequest;
    host.request_callback = hostRequest;

    plugin = library->factory->create_plugin(library->factory, &host,
                                              descriptor->id);
    if (!plugin)
      throw std::runtime_error("CLAP plugin create failed: " + pluginId);
    if (!plugin->init(plugin)) {
      plugin->destroy(plugin);
      plugin = nullptr;
      throw std::runtime_error("CLAP plugin init failed: " + pluginId);
    }
    auto cleanupPlugin = [this](const clap_plugin_t *instance) {
      if (processing) {
        currentClapAudioHost = &host;
        instance->stop_processing(instance);
        currentClapAudioHost = nullptr;
      }
      if (active)
        instance->deactivate(instance);
      instance->destroy(instance);
      plugin = nullptr;
      processing = false;
      active = false;
    };
    std::unique_ptr<const clap_plugin_t, decltype(cleanupPlugin)> cleanup(
        plugin, cleanupPlugin);
    stateExtension = static_cast<const clap_plugin_state_t *>(
        plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (!persistentIdentity.empty()) {
      statePath = statePathFor(persistentIdentity);
      if (auto saved = readStateFile(statePath, persistentIdentity)) {
        if (!stateExtension)
          throw std::runtime_error(
              "CLAP state exists but plugin does not implement clap.state: " +
              pluginId);
        MemoryStream stream{&*saved};
        clap_istream_t input{&stream, stateRead};
        if (!stateExtension->load(plugin, &input) || stream.failed ||
            stream.cursor != saved->size())
          throw std::runtime_error("CLAP plugin rejected saved state: " +
                                   pluginId);
      }
    }
    ports = static_cast<const clap_plugin_audio_ports_t *>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (!ports || ports->count(plugin, true) != 1 ||
        ports->count(plugin, false) != 1 ||
        !ports->get(plugin, 0, true, &inputInfo) ||
        !ports->get(plugin, 0, false, &outputInfo) ||
        !(inputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) ||
        !(outputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) ||
        inputInfo.channel_count != channels.size() ||
        outputInfo.channel_count != channels.size()) {
      throw std::runtime_error("CLAP plugin must expose one main input and "
                               "output matching the SkyAPO channel count: " +
                               pluginId);
    }

    parameterExtension = static_cast<const clap_plugin_params_t *>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    if (!parameterExtension && !overrides.empty()) {
      throw std::runtime_error("CLAP plugin does not expose parameters: " +
                               pluginId);
    }
    if (parameterExtension) {
      const uint32_t count = parameterExtension->count(plugin);
      if (count > 4096) {
        throw std::runtime_error("CLAP plugin declares too many parameters: " +
                                 pluginId);
      }
      clapParameters.reserve(count);
      parameterInfo.reserve(count);
      liveParameters = std::make_unique<LiveParameter[]>(count);
      consumedRevisions = std::make_unique<uint32_t[]>(count);
      hasStaticOverride.assign(count, false);
      for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!parameterExtension->get_info(plugin, i, &info) ||
            !std::isfinite(info.min_value) || !std::isfinite(info.max_value) ||
            !std::isfinite(info.default_value) ||
            !std::isfinite(static_cast<float>(info.min_value)) ||
            !std::isfinite(static_cast<float>(info.max_value)) ||
            !std::isfinite(static_cast<float>(info.default_value)) ||
            info.min_value > info.max_value ||
            info.default_value < info.min_value ||
            info.default_value > info.max_value) {
          throw std::runtime_error("CLAP plugin supplied invalid parameter "
                                   "metadata: " + pluginId);
        }
        double value = info.default_value;
        (void)parameterExtension->get_value(plugin, info.id, &value);
        if (!std::isfinite(value) || value < info.min_value ||
            value > info.max_value)
          value = info.default_value;
        clapParameters.push_back(info);
        parameterInfo.push_back({std::to_string(info.id), info.name,
                                 static_cast<float>(info.default_value),
                                 static_cast<float>(info.min_value),
                                 static_cast<float>(info.max_value),
                                 static_cast<float>(value)});
      }
    }
    for (const auto &override : overrides) {
      if (!std::isfinite(override.value))
        throw std::runtime_error("CLAP parameter value must be finite");
      size_t match = clapParameters.size();
      for (size_t i = 0; i < clapParameters.size(); ++i) {
        const auto &info = clapParameters[i];
        if (override.symbol == std::to_string(info.id) ||
            override.symbol == info.name) {
          if (match != clapParameters.size())
            throw std::runtime_error("ambiguous CLAP parameter '" +
                                     override.symbol + "' (use its numeric id)");
          match = i;
        }
      }
      if (match == clapParameters.size())
        throw std::runtime_error("unknown CLAP parameter '" + override.symbol +
                                 "'");
      const auto &info = clapParameters[match];
      if (info.flags & CLAP_PARAM_IS_READONLY)
        throw std::runtime_error("CLAP parameter '" + override.symbol +
                                 "' is read-only");
      if (override.value < info.min_value || override.value > info.max_value)
        throw std::runtime_error("CLAP parameter '" + override.symbol +
                                 "' value is outside its declared range");
      clap_event_param_value_t event{};
      event.header.size = sizeof(event);
      event.header.time = 0;
      event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      event.header.type = CLAP_EVENT_PARAM_VALUE;
      event.header.flags = CLAP_EVENT_DONT_RECORD;
      event.param_id = info.id;
      event.cookie = info.cookie;
      event.note_id = -1;
      event.port_index = -1;
      event.channel = -1;
      event.key = -1;
      event.value = override.value;
      parameterEvents.push_back(event);
      parameterInfo[match].value = override.value;
      hasStaticOverride[match] = true;
      staticOverrideIndices.push_back(match);
    }
    eventBatch.resize(parameterEvents.size() + clapParameters.size());
    if (!plugin->activate(plugin, sampleRate, 1, maxFrames)) {
      throw std::runtime_error("CLAP plugin activation failed: " + pluginId);
    }
    active = true;
    latencyExtension = static_cast<const clap_plugin_latency_t *>(
        plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    latency = latencyExtension ? latencyExtension->get(plugin) : 0;
    if (!plugin->start_processing(plugin)) {
      throw std::runtime_error("CLAP plugin start_processing failed: " +
                               pluginId);
    }
    processing = true;
    inputBuffer.data32 = inputChannels.data();
    inputBuffer.channel_count = static_cast<uint32_t>(inputChannels.size());
    outputBuffer.data32 = outputChannels.data();
    outputBuffer.channel_count = static_cast<uint32_t>(outputChannels.size());
    emptyInputEvents.ctx = this;
    emptyInputEvents.size = parameterEventCount;
    emptyInputEvents.get = parameterEventAt;
    outputEvents.try_push = rejectEvent;
    clapProcess.in_events = &emptyInputEvents;
    clapProcess.out_events = &outputEvents;
    clapProcess.audio_inputs = &inputBuffer;
    clapProcess.audio_outputs = &outputBuffer;
    clapProcess.audio_inputs_count = 1;
    clapProcess.audio_outputs_count = 1;
    clapProcess.transport = nullptr;
    clapProcess.steady_time = -1;
    cleanup.release();
  }

  ~CLAPInstance() override {
    if (plugin) {
      if (processing) {
        currentClapAudioHost = &host;
        plugin->stop_processing(plugin);
        currentClapAudioHost = nullptr;
      }
      if (active)
        plugin->deactivate(plugin);
      plugin->destroy(plugin);
    }
  }

  const std::string &uri() const noexcept override {
    return pluginId;
  }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return parameterInfo;
  }
  uint32_t latencySamples() const noexcept override {
    return latency.load(std::memory_order_acquire);
  }
  bool latencyRefreshPending() const noexcept override {
    return hostContext.restartRequested.load(std::memory_order_acquire) ||
           hostContext.latencyChanged.load(std::memory_order_acquire);
  }
  bool refreshPluginLatency() override {
    const bool restart =
        hostContext.restartRequested.exchange(false, std::memory_order_acq_rel);
    const bool latencyChanged =
        hostContext.latencyChanged.exchange(false, std::memory_order_acq_rel);
    if (!restart && !latencyChanged)
      return false;
    if (latencyChanged && !latencyExtension) {
      processingError.store(true, std::memory_order_release);
      throw std::runtime_error(
          "CLAP plugin notified a latency change without clap.latency: " +
          pluginId);
    }
    if (processing) {
      plugin->stop_processing(plugin);
      processing = false;
    }
    if (active) {
      plugin->deactivate(plugin);
      active = false;
    }
    if (!plugin->activate(plugin, sampleRate, 1, maxFrameCount)) {
      processingError.store(true, std::memory_order_release);
      throw std::runtime_error("CLAP plugin failed to reactivate for latency "
                               "change: " +
                               pluginId);
    }
    active = true;
    clap_audio_port_info_t updatedInput{}, updatedOutput{};
    if (ports->count(plugin, true) != 1 || ports->count(plugin, false) != 1 ||
        !ports->get(plugin, 0, true, &updatedInput) ||
        !ports->get(plugin, 0, false, &updatedOutput) ||
        updatedInput.channel_count != inputInfo.channel_count ||
        updatedOutput.channel_count != outputInfo.channel_count ||
        !(updatedInput.flags & CLAP_AUDIO_PORT_IS_MAIN) ||
        !(updatedOutput.flags & CLAP_AUDIO_PORT_IS_MAIN)) {
      processingError.store(true, std::memory_order_release);
      throw std::runtime_error(
          "CLAP restart changed the unsupported audio bus layout: " + pluginId);
    }
    const uint32_t updatedLatency =
        latencyExtension ? latencyExtension->get(plugin) : 0;
    const uint32_t previousLatency =
        latency.exchange(updatedLatency, std::memory_order_acq_rel);
    // The snapshot above covers changed() notifications emitted during
    // activate, so avoid a duplicate lifecycle cycle on the next timer tick.
    hostContext.latencyChanged.store(false, std::memory_order_release);
    if (!plugin->start_processing(plugin)) {
      processingError.store(true, std::memory_order_release);
      throw std::runtime_error("CLAP plugin failed to resume after latency "
                               "change: " +
                               pluginId);
    }
    processing = true;
    return updatedLatency != previousLatency;
  }
  std::vector<std::wstring>
  initialize(float, unsigned,
             const std::vector<std::wstring> &channels) override {
    return channels;
  }
  void process(float **output, float **input, unsigned frames) noexcept override {
    if (!plugin || frames > maxFrameCount ||
        processingError.load(std::memory_order_acquire)) {
      for (size_t c = 0; c < outputChannels.size(); ++c)
        std::fill_n(output[c], frames, 0.0f);
      return;
    }
    for (size_t c = 0; c < inputChannels.size(); ++c) {
      inputChannels[c] = input[c];
      outputChannels[c] = output[c];
    }
    buildParameterEvents();
    clapProcess.frames_count = frames;
    currentClapAudioHost = &host;
    const auto result = plugin->process(plugin, &clapProcess);
    currentClapAudioHost = nullptr;
    if (result == CLAP_PROCESS_ERROR) {
      processingError.store(true, std::memory_order_release);
      for (size_t c = 0; c < outputChannels.size(); ++c)
        std::fill_n(output[c], frames, 0.0f);
    }
  }

  bool processingFailed() const noexcept override {
    return processingError.load(std::memory_order_acquire);
  }

  const std::string &pluginIdentifier() const noexcept override {
    return pluginId;
  }

  void setParameterValue(const std::string &symbol, float value) override {
    if (!std::isfinite(value))
      throw std::runtime_error("CLAP parameter value must be finite");
    size_t match = clapParameters.size();
    for (size_t i = 0; i < clapParameters.size(); ++i) {
      const auto &info = clapParameters[i];
      if (symbol == std::to_string(info.id) || symbol == info.name) {
        if (match != clapParameters.size())
          throw std::runtime_error("ambiguous CLAP parameter '" + symbol +
                                   "' (use its numeric id)");
        match = i;
      }
    }
    if (match == clapParameters.size())
      throw std::runtime_error("unknown CLAP parameter '" + symbol + "'");
    const auto &info = clapParameters[match];
    if (info.flags & CLAP_PARAM_IS_READONLY)
      throw std::runtime_error("CLAP parameter '" + symbol + "' is read-only");
    if (value < info.min_value || value > info.max_value)
      throw std::runtime_error("CLAP parameter '" + symbol +
                               "' value is outside its declared range");
    // Single-writer control-thread mailbox: publish the value before its
    // revision. The audio thread only loads lock-free atomics and consumes the
    // newest value when preparing its already allocated CLAP event array.
    liveParameters[match].value.store(value, std::memory_order_relaxed);
    liveParameters[match].revision.fetch_add(1, std::memory_order_release);
  }

  bool savePersistentPluginState() override {
    if (!stateExtension || persistentIdentity.empty())
      return false;
    if (processingError.load(std::memory_order_acquire))
      throw std::runtime_error("cannot persist failed CLAP plugin state: " +
                               pluginId);

    if (processing) {
      currentClapAudioHost = &host;
      plugin->stop_processing(plugin);
      currentClapAudioHost = nullptr;
      processing = false;
    }
    if (parameterExtension && parameterExtension->flush) {
      buildParameterEvents();
      currentClapAudioHost = &host;
      parameterExtension->flush(plugin, &emptyInputEvents, &outputEvents);
      currentClapAudioHost = nullptr;
    }
    MemoryStream stream;
    clap_ostream_t output{&stream, stateWrite};
    bool saved = false;
    try {
      saved = stateExtension->save(plugin, &output);
      if (!saved || stream.failed)
        throw std::runtime_error("CLAP plugin failed to save state: " +
                                 pluginId);
      atomicWriteState(statePath, persistentIdentity, stream.output);
    } catch (...) {
      restartProcessing();
      throw;
    }
    restartProcessing();
    return saved;
  }

private:
  struct LiveParameter {
    std::atomic<float> value{0.0f};
    std::atomic<uint32_t> revision{0};
  };

  void buildParameterEvents() noexcept {
    size_t count = 0;
    for (size_t i = 0; i < parameterEvents.size(); ++i) {
      auto event = parameterEvents[i];
      const auto paramIndex = staticOverrideIndices[i];
      if (paramIndex < clapParameters.size()) {
        const auto revision = liveParameters[paramIndex].revision.load(
            std::memory_order_acquire);
        if (revision != 0)
          event.value = liveParameters[paramIndex].value.load(
              std::memory_order_relaxed);
      }
      eventBatch[count++] = event;
    }
    for (size_t i = 0; i < clapParameters.size(); ++i) {
      if (hasStaticOverride[i])
        continue;
      const auto revision =
          liveParameters[i].revision.load(std::memory_order_acquire);
      if (!revision || revision == consumedRevisions[i])
        continue;
      clap_event_param_value_t event{};
      event.header.size = sizeof(event);
      event.header.time = 0;
      event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      event.header.type = CLAP_EVENT_PARAM_VALUE;
      event.header.flags = CLAP_EVENT_DONT_RECORD;
      event.param_id = clapParameters[i].id;
      event.cookie = clapParameters[i].cookie;
      event.note_id = -1;
      event.port_index = -1;
      event.channel = -1;
      event.key = -1;
      event.value = liveParameters[i].value.load(std::memory_order_relaxed);
      eventBatch[count++] = event;
      consumedRevisions[i] = revision;
    }
    eventBatchCount = static_cast<uint32_t>(count);
  }

  static uint32_t CLAP_ABI parameterEventCount(const clap_input_events_t *list) {
    const auto *self = static_cast<const CLAPInstance *>(list->ctx);
    return self->eventBatchCount;
  }

  void restartProcessing() {
    currentClapAudioHost = &host;
    const bool started = plugin->start_processing(plugin);
    currentClapAudioHost = nullptr;
    if (!started) {
      processingError.store(true, std::memory_order_release);
      throw std::runtime_error("CLAP plugin could not resume after state save: " +
                               pluginId);
    }
    processing = true;
  }
  static const clap_event_header_t *CLAP_ABI
  parameterEventAt(const clap_input_events_t *list, uint32_t index) {
    const auto *self = static_cast<const CLAPInstance *>(list->ctx);
    return index < self->eventBatchCount ? &self->eventBatch[index].header
                                         : nullptr;
  }

  std::shared_ptr<ClapLibrary> library;
  std::string pluginId;
  double sampleRate;
  unsigned maxFrameCount;
  ClapHostContext hostContext{};
  clap_host_t host{};
  const clap_plugin_t *plugin{};
  const clap_plugin_audio_ports_t *ports{};
  const clap_plugin_params_t *parameterExtension{};
  const clap_plugin_latency_t *latencyExtension{};
  const clap_plugin_state_t *stateExtension{};
  std::string persistentIdentity;
  fs::path statePath;
  std::vector<clap_param_info_t> clapParameters;
  std::vector<clap_event_param_value_t> parameterEvents;
  std::vector<clap_event_param_value_t> eventBatch;
  std::unique_ptr<LiveParameter[]> liveParameters;
  std::unique_ptr<uint32_t[]> consumedRevisions;
  std::vector<bool> hasStaticOverride;
  std::vector<size_t> staticOverrideIndices;
  uint32_t eventBatchCount{};
  clap_audio_port_info_t inputInfo{}, outputInfo{};
  bool active = false, processing = false;
  std::atomic<bool> processingError{false};
  std::atomic<uint32_t> latency{0};
  std::vector<float *> inputChannels, outputChannels;
  clap_audio_buffer_t inputBuffer{}, outputBuffer{};
  clap_input_events_t emptyInputEvents{};
  clap_output_events_t outputEvents{};
  clap_process_t clapProcess{};
  std::vector<PluginParameterInfo> parameterInfo;
};

class CLAPPluginFilter final : public IFilter,
                               public AtomicPluginBypass,
                               public IPluginParameterControl,
                               public IPluginLatencyRefresh,
                               public IPluginFailureState,
                               public IPluginLatencyState,
                               public IPluginStatePersistence {
public:
  CLAPPluginFilter(CLAPPluginHost &host, std::string id,
                   std::vector<PluginParameterValue> overrides,
                   std::filesystem::path source, unsigned line)
      : host(host), pluginId(std::move(id)),
        parameterOverrides(std::move(overrides)), source(std::move(source)),
        line(line) {}
  bool getInPlace() override {
    return false;
  }
  std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrames,
             std::vector<std::wstring> channels) override {
    channelCount = static_cast<unsigned>(channels.size());
    instance = host.createForConfig(pluginId, sampleRate, maxFrames, channels,
                                    parameterOverrides, source, line);
    auto outputChannels = instance->initialize(sampleRate, maxFrames, channels);
    prepareBypassDelay(instance->latencySamples(), channelCount);
    return outputChannels;
  }
  void process(float **output, float **input, unsigned frames) override {
    if (copyInputWhenBypassed(output, input, frames, channelCount))
      return;
    instance->process(output, input, frames);
  }
  bool processingFailed() const noexcept override {
    return instance && instance->processingFailed();
  }
  const std::string &failureIdentifier() const noexcept override {
    return pluginId;
  }
  uint32_t latencySamples() const noexcept override {
    return instance ? instance->latencySamples() : 0;
  }
  bool latencyRefreshPending() const noexcept override {
    const auto *refresh =
        dynamic_cast<const IPluginLatencyRefresh *>(instance.get());
    return refresh && refresh->latencyRefreshPending();
  }
  bool refreshPluginLatency() override {
    auto *refresh = dynamic_cast<IPluginLatencyRefresh *>(instance.get());
    if (!refresh || !refresh->refreshPluginLatency())
      return false;
    prepareBypassDelay(instance->latencySamples(), channelCount);
    return true;
  }
  const std::string &pluginIdentifier() const noexcept override {
    return pluginId;
  }
  void setParameterValue(const std::string &symbol, float value) override {
    if (!instance)
      throw std::runtime_error("CLAP plugin is not active: " + pluginId);
    auto *control = dynamic_cast<IPluginParameterControl *>(instance.get());
    if (!control)
      throw std::runtime_error(
          "CLAP plugin does not support live parameters: " + pluginId);
    control->setParameterValue(symbol, value);
  }
  bool savePersistentPluginState() override {
    if (!instance)
      return false;
    auto *state = dynamic_cast<IPluginStatePersistence *>(instance.get());
    return state && state->savePersistentPluginState();
  }

private:
  CLAPPluginHost &host;
  std::string pluginId;
  std::vector<PluginParameterValue> parameterOverrides;
  std::filesystem::path source;
  unsigned line{};
  std::unique_ptr<IPluginInstance> instance;
  unsigned channelCount{};
};

IFilter *allocateFilter(CLAPPluginHost &host, std::string id,
                        std::vector<PluginParameterValue> overrides,
                        const std::filesystem::path &source, unsigned line) {
  void *memory = MemoryHelper::alloc(sizeof(CLAPPluginFilter));
  try {
    return new (memory) CLAPPluginFilter(host, std::move(id),
                                         std::move(overrides), source, line);
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}

class CLAPPluginFilterFactory final : public IFilterFactory,
                                      public IPluginSourceContext {
public:
  void setPluginSourceLocation(const std::filesystem::path &path,
                               unsigned sourceLine) override {
    source = path;
    line = sourceLine;
  }

  std::vector<IFilter *> createFilter(const std::wstring &, std::wstring &command,
                                      std::wstring &parameters) override {
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, wideId, extra;
    input >> format >> wideId;
    if (format != L"CLAP")
      return {};
    if (wideId.empty())
      throw std::runtime_error(
          "expected Plugin: CLAP <plugin-id> [parameter=value ...]");
    std::vector<PluginParameterValue> overrides;
    std::wstring token;
    while (input >> token) {
      const auto separator = token.find(L'=');
      if (separator == std::wstring::npos || separator == 0 ||
          separator + 1 == token.size())
        throw std::runtime_error(
            "expected CLAP parameter as numeric-id-or-name=value");
      const auto symbol =
          StringHelper::toString(token.substr(0, separator), 65001);
      const auto valueText =
          StringHelper::toString(token.substr(separator + 1), 65001);
      size_t consumed = 0;
      float value = 0.0f;
      try {
        value = std::stof(valueText, &consumed);
      } catch (const std::exception &) {
        throw std::runtime_error("invalid CLAP parameter value for '" + symbol +
                                 "'");
      }
      if (consumed != valueText.size() || !std::isfinite(value))
        throw std::runtime_error("invalid CLAP parameter value for '" + symbol +
                                 "'");
      if (std::any_of(
              overrides.begin(), overrides.end(), [&](const auto &entry) {
                return entry.symbol == symbol;
              }))
        throw std::runtime_error("duplicate CLAP parameter override '" +
                                 symbol + "'");
      overrides.push_back({symbol, value});
    }
    return {allocateFilter(host, StringHelper::toString(wideId, 65001),
                           std::move(overrides), source, line)};
  }

private:
  CLAPPluginHost host;
  std::filesystem::path source;
  unsigned line{};
};
} // namespace

struct CLAPPluginHost::CatalogItem {
  std::shared_ptr<ClapLibrary> library;
  std::string id;
  std::string name;
};

CLAPPluginHost::CLAPPluginHost() = default;
CLAPPluginHost::~CLAPPluginHost() = default;

void CLAPPluginHost::scan() const {
  if (scanned)
    return;
  scanned = true;
  for (const auto &path : pluginFiles()) {
    auto library = openLibrary(path);
    if (!library)
      continue;
    const uint32_t count = library->factory->get_plugin_count(library->factory);
    for (uint32_t i = 0; i < count; ++i) {
      const auto *descriptor =
          library->factory->get_plugin_descriptor(library->factory, i);
      if (descriptor && descriptor->id && descriptor->name)
        catalog.push_back({library, descriptor->id, descriptor->name});
    }
  }
}

std::vector<std::pair<std::string, std::string>> CLAPPluginHost::list() const {
  scan();
  std::vector<std::pair<std::string, std::string>> result;
  result.reserve(catalog.size());
  for (const auto &item : catalog)
    result.emplace_back(item.id, item.name);
  return result;
}

PluginDescription CLAPPluginHost::describe(const std::string &id) const {
  scan();
  const auto found = std::find_if(catalog.begin(), catalog.end(),
                                  [&](const auto &item) { return item.id == id; });
  if (found == catalog.end())
    throw std::runtime_error("CLAP plugin not found: " + id);
  PluginDescription result{id, found->name, {}};
  ClapHostContext hostContext{};
  hostContext.mainThread = pthread_self();
  clap_host_t host{};
  host.clap_version = CLAP_VERSION;
  host.host_data = &hostContext;
  host.name = "SkyAPO";
  host.vendor = "SkyAPO project";
  host.url = "https://github.com/skyapo/skyapo";
  host.version = SKYAPO_VERSION;
  host.get_extension = hostExtension;
  host.request_restart = hostRequestRestart;
  host.request_process = hostRequest;
  host.request_callback = hostRequest;
  const clap_plugin_t *plugin = found->library->factory->create_plugin(
      found->library->factory, &host, id.c_str());
  if (!plugin)
    throw std::runtime_error("CLAP plugin create failed: " + id);
  if (!plugin->init(plugin)) {
    plugin->destroy(plugin);
    throw std::runtime_error("CLAP plugin init failed: " + id);
  }
  auto destroy = [](const clap_plugin_t *instance) { instance->destroy(instance); };
  std::unique_ptr<const clap_plugin_t, decltype(destroy)> cleanup(plugin,
                                                                  destroy);
  const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  if (!params)
    return result;
  const uint32_t count = params->count(plugin);
  if (count > 4096)
    throw std::runtime_error("CLAP plugin declares too many parameters: " + id);
  result.inputParameters.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    clap_param_info_t info{};
    if (!params->get_info(plugin, i, &info))
      throw std::runtime_error("cannot read CLAP parameter metadata: " + id);
    double value = info.default_value;
    (void)params->get_value(plugin, info.id, &value);
    result.inputParameters.push_back(
        {std::to_string(info.id), info.name,
         static_cast<float>(info.default_value), static_cast<float>(info.min_value),
         static_cast<float>(info.max_value), static_cast<float>(value)});
  }
  return result;
}

std::unique_ptr<IPluginInstance>
CLAPPluginHost::create(const std::string &id, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) {
  return createForConfig(id, sampleRate, maxFrames, channels, parameters, {}, 0);
}

std::unique_ptr<IPluginInstance> CLAPPluginHost::createForConfig(
    const std::string &id, float sampleRate, unsigned maxFrames,
    const std::vector<std::wstring> &channels,
    const std::vector<PluginParameterValue> &parameters,
    const std::filesystem::path &source, unsigned line) {
  scan();
  const auto found = std::find_if(catalog.begin(), catalog.end(),
                                  [&](const auto &item) { return item.id == id; });
  if (found == catalog.end())
    throw std::runtime_error("CLAP plugin not found: " + id +
                             " (check CLAP_PATH and standard plugin paths)");
  const auto count = found->library->factory->get_plugin_count(
      found->library->factory);
  for (uint32_t i = 0; i < count; ++i) {
    const auto *descriptor = found->library->factory->get_plugin_descriptor(
        found->library->factory, i);
    if (descriptor && descriptor->id && id == descriptor->id)
      return std::make_unique<CLAPInstance>(found->library, descriptor, id,
                                            sampleRate, maxFrames, channels,
                                            parameters,
                                            source.empty()
                                                ? std::string{}
                                                : stateIdentity(source, line, id));
  }
  throw std::runtime_error("CLAP plugin descriptor disappeared: " + id);
}

std::unique_ptr<IFilterFactory> makeCLAPPluginFilterFactory() {
  return std::make_unique<CLAPPluginFilterFactory>();
}
