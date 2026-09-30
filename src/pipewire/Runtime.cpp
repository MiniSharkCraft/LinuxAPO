#include "Runtime.h"
#include "../platform/PlatformChannels.h"
#include "../platform/RealtimeAudit.h"
#include "../platform/Settings.h"
#include "DeviceManager.h"
#include "DefaultSinkVolumeMonitor.h"
#include "Engine.h"
#include "LoudnessVolumeProvider.h"
#include "../platform/ConfigWatcher.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/port-config.h>
#include <spa/param/props.h>
#include <sstream>
#include <sys/stat.h>
#include <time.h>
#include <thread>

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
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  for (;;) {
    pollfd ready{fd, POLLIN, 0};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
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
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(1000);
  while (sent < frame.size()) {
    const auto count = send(fd, frame.data() + sent, frame.size() - sent,
                            MSG_NOSIGNAL);
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
struct Runtime {
  volatile sig_atomic_t &stopping;
  AudioDevice device;
  std::string config, error, path;
  std::vector<AudioPort> ports;
  pw_main_loop *main{};
  pw_context *context{};
  pw_core *core{};
  pw_registry *registry{};
  pw_filter *filter{};
  spa_hook coreHook{}, registryHook{}, filterHook{};
  spa_source *rateEvent{}, *transitionEvent{}, *statusEvent{}, *sigint{},
      *sigterm{}, *selectionTimer{}, *configEvent{}, *reloadTimer{},
      *linkRetryTimer{};
  std::unique_ptr<skyapo::platform::ConfigWatcher> configWatcher;
  int server = -1;
  bool ownsSocket = false;
  bool cleaning = false;
  bool initialized = false;
  enum class AudioMode : unsigned { Running, Quiescing, Stopping };
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
  std::vector<float> work;
  std::unique_ptr<Engine> activeEngine;
  std::unique_ptr<Engine> pendingEngine;
  std::vector<std::unique_ptr<Engine>> retiredEngines;
  std::unique_ptr<skyapo::pipewire::DefaultSinkVolumeMonitor>
      renderVolumeMonitor;
  skyapo::pipewire::DefaultSinkVolumeSnapshot renderVolume;
  std::atomic<Engine *> active{nullptr};
  std::atomic<Engine *> pending{nullptr};
  std::atomic<unsigned> callbacksInFlight{0};
  std::atomic<AudioMode> audioMode{AudioMode::Running};
  std::atomic<bool> transitionComplete{false};
  unsigned transitionCounter{};
  unsigned transitionLength{};
  unsigned transitionDurationMs{10};
  uint64_t formatRebuildsDuringTransition{};
  std::atomic<bool> resetMetrics{false};
  std::atomic<unsigned> rate{0}, quantum{0}, requestedRate{0},
      requestedQuantum{0};
  std::atomic<uint64_t> blocks{0}, samples{0}, totalNs{0}, maxNs{0},
      overruns{0};
  std::atomic<double> rawEnergy{0}, processedEnergy{0};
  double rawSum = 0, processedSum = 0;
  unsigned channels = 0;
  std::string configError;
  pw_filter_state state = PW_FILTER_STATE_UNCONNECTED;
  explicit Runtime(volatile sig_atomic_t &stop) : stopping(stop) {
    ownInputs.fill(SPA_ID_INVALID);
    linkGlobalIds.fill(SPA_ID_INVALID);
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    if (const char *value = std::getenv("SKYAPO_TEST_TRANSITION_MS")) {
      char *end = nullptr;
      errno = 0;
      const unsigned long parsed = std::strtoul(value, &end, 10);
      if (errno || end == value || *end != '\0' || parsed < 10 ||
          parsed > 5000)
        throw std::runtime_error(
            "SKYAPO_TEST_TRANSITION_MS must be an integer from 10 to 5000");
      transitionDurationMs = static_cast<unsigned>(parsed);
    }
#endif
  }
  ~Runtime() {
    cleaning = true;
    skyapo::platform::LoudnessVolumeProvider::publish(false, 0.0f);
    // Release registry-bound proxies/listeners while their core is still live.
    renderVolumeMonitor.reset();
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
      for (auto *s : {rateEvent, transitionEvent, statusEvent, sigint, sigterm,
                      selectionTimer, configEvent, reloadTimer, linkRetryTimer})
        if (s)
          pw_loop_destroy_source(l, s);
    }
    if (server >= 0)
      close(server);
    if (ownsSocket)
      unlink(path.c_str());
    if (context)
      pw_context_destroy(context);
    if (main)
      pw_main_loop_destroy(main);
    if (initialized)
      pw_deinit();
  }
  void fail(const std::string &e) {
    error = e;
    pw_main_loop_quit(main);
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
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
    while (pending.load(std::memory_order_seq_cst)) {
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
    auto mode = audioMode.load(std::memory_order_seq_cst);
    if (mode == AudioMode::Stopping)
      throw std::runtime_error("audio runtime is shutting down");
    audioMode.store(AudioMode::Quiescing, std::memory_order_seq_cst);
    if (waitForCallbacksToDrain())
      return true;
    audioMode.store(AudioMode::Running, std::memory_order_seq_cst);
    return false;
  }
  void resumeAudio() noexcept {
    if (audioMode.load(std::memory_order_seq_cst) != AudioMode::Stopping)
      audioMode.store(AudioMode::Running, std::memory_order_seq_cst);
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
        current->maxFrames() == blockFrames)
      return;
    std::vector<std::wstring> names;
    for (auto &p : ports)
      names.push_back(eapoChannel(p.channel));
    const auto createEngine = [&] {
      auto candidate =
          std::make_unique<Engine>(hz, channels, blockFrames, names, true);
      candidate->loadConfig(config);
      return candidate;
    };
    // Validate the existing sidecars before the active graph is allowed to
    // overwrite them with a fresh snapshot. If corrupt state/plugin load is
    // rejected, this candidate fails and the active graph and bytes survive.
    auto engine = createEngine();
    const auto saved = saveCurrentPluginStates();
    if (saved)
      std::cerr << "skyapod: saved state for " << saved
                << " CLAP plugin instance(s) before graph rebuild\n";
    if (saved)
      engine = createEngine();
    configWatcher->update(engine->configFiles());
    installEngine(std::move(engine));
    auto *pointer = active.load(std::memory_order_acquire);
    std::cerr << "skyapod: DSP ready at " << hz << " Hz, " << channels
              << " channels, " << pointer->filterCount() << " filters\n";
  }
  void installEngine(std::unique_ptr<Engine> replacement) {
    promoteCompletedTransition();
    if (pendingEngine || pending.load(std::memory_order_seq_cst))
      throw std::runtime_error("another DSP graph transition is still active");
    auto *current = active.load(std::memory_order_seq_cst);
    if (current && sameFormat(*current, *replacement)) {
      // Keep both owners on the control thread. The callback only reads the
      // published raw pointer and never destroys or mutates ownership.
      retiredEngines.reserve(retiredEngines.size() + 1);
      transitionCounter = 0;
      transitionLength = std::max(
          1u, replacement->sampleRate() * transitionDurationMs / 1000);
      auto *next = replacement.get();
      pendingEngine = std::move(replacement);
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
  bool waitForCallbacksToDrain() {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(500);
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
          "timed out waiting for realtime callbacks before CLAP state save");
    }
    try {
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
    // callbacks before making any main-thread CLAP state calls. Never save in
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
                  << " CLAP plugin instance(s)\n";
    } catch (const std::exception &error) {
      std::cerr << "skyapod: CLAP state save failed during shutdown: "
                << error.what() << '\n';
    }
  }
  void reloadConfig() {
    // Only one pair of configurations may be in flight. A very short wait on
    // the control thread lets realtime finish and retire the accepted graph
    // before another candidate is prepared.
    waitForTransition();
    auto *current = active.load(std::memory_order_acquire);
    const unsigned hz = current ? current->sampleRate() : requestedRate.load();
    const unsigned blockFrames = current ? current->maxFrames()
                                         : requestedQuantum.load();
    std::vector<std::wstring> names;
    for (auto &p : ports)
      names.push_back(eapoChannel(p.channel));
    if (!hz || !blockFrames)
      throw std::runtime_error("cannot reload before PipeWire format negotiation");
    const auto createEngine = [&] {
      auto candidate =
          std::make_unique<Engine>(hz, channels, blockFrames, names, true);
      candidate->loadConfig(config);
      return candidate;
    };
    // Read/validate persisted state first. A malformed sidecar or plugin load
    // rejection must not be silently overwritten by saving the live graph.
    auto replacement = createEngine();
    const auto saved = saveCurrentPluginStates();
    if (saved)
      std::cerr << "skyapod: saved state for " << saved
                << " CLAP plugin instance(s) before config reload\n";
    if (saved)
      replacement = createEngine();
    configWatcher->update(replacement->configFiles());
    const unsigned count = replacement->filterCount();
    installEngine(std::move(replacement));
    resetMetrics.store(true, std::memory_order_release);
    configError.clear();
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
      std::cerr << "skyapod: config reload failed; retaining last valid graph: "
                << e.what() << '\n';
    }
  }
  static void rateChanged(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    try {
      r.reclaimRetired();
      r.buildEngine(r.requestedRate.load(), r.requestedQuantum.load());
    } catch (const std::exception &e) {
      r.fail(e.what());
    }
  }
  static void transitionReady(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    r.reclaimRetired();
    // Drop the one crossfade quantum from the steady-state signal statistics.
    r.resetMetrics.store(true, std::memory_order_release);
  }
  static void process(void *data, spa_io_position *position) {
    realtime::Scope audit;
    auto &r = *static_cast<Runtime *>(data);
    struct FlightGuard {
      std::atomic<unsigned> &count;
      explicit FlightGuard(std::atomic<unsigned> &value) : count(value) {
        count.fetch_add(1, std::memory_order_seq_cst);
      }
      ~FlightGuard() { count.fetch_sub(1, std::memory_order_seq_cst); }
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
    if (r.audioMode.load(std::memory_order_seq_cst) != AudioMode::Running) {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      return;
    }
    Engine *engine = r.active.load(std::memory_order_seq_cst);
    Engine *nextEngine = r.pending.load(std::memory_order_seq_cst);
    if (!engine || engine->sampleRate() != hz ||
        engine->maxFrames() != frames) {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      const bool rateChanged = r.requestedRate.exchange(hz) != hz;
      const bool quantumChanged = r.requestedQuantum.exchange(frames) != frames;
      if (!engine || engine->sampleRate() != hz ||
          engine->maxFrames() != frames || rateChanged || quantumChanged)
        pw_loop_signal_event(pw_main_loop_get_loop(r.main), r.rateEvent);
      return;
    }
    double raw = 0, processed = 0;
    for (uint64_t offset = 0; offset < frames; offset += MaxFrames) {
      const unsigned n = std::min<uint64_t>(MaxFrames, frames - offset);
      for (unsigned f = 0; f < n; ++f)
        for (unsigned c = 0; c < r.channels; ++c) {
          float s = in[c] ? in[c][offset + f] : 0.0f;
          r.work[f * r.channels + c] = s;
          raw += double(s) * s;
        }
      if (nextEngine) {
        r.transitionCounter = engine->processTransitionTo(
            *nextEngine, r.work.data(), n, r.transitionCounter,
            r.transitionLength);
        if (r.transitionCounter >= r.transitionLength) {
          engine = nextEngine;
          r.active.store(engine, std::memory_order_seq_cst);
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
      for (unsigned f = 0; f < n; ++f)
        for (unsigned c = 0; c < r.channels; ++c) {
          float s = r.work[f * r.channels + c];
          if (out[c])
            out[c][offset + f] = s;
          processed += double(s) * s;
        }
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
  }
  static void stateChanged(void *data, pw_filter_state old,
                           pw_filter_state state, const char *error) {
    auto &r = *static_cast<Runtime *>(data);
    r.state = state;
    std::cerr << "skyapod: virtual mic " << pw_filter_state_as_string(state)
              << '\n';
    if (state == PW_FILTER_STATE_ERROR)
      r.fail(error ? error : "filter error");
    if (state == PW_FILTER_STATE_UNCONNECTED &&
        old != PW_FILTER_STATE_UNCONNECTED && !r.cleaning && !r.stopping)
      r.fail("virtual source disconnected");
  }
  static void selectionChanged(void *data, uint64_t) {
    auto &r = *static_cast<Runtime *>(data);
    r.reclaimRetired();
    try {
      if (settings::device() != r.device.name)
        r.fail("device selection changed");
    } catch (const std::exception &e) {
      r.fail(e.what());
    }
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
    }
  }
  static void coreError(void *data, uint32_t id, int, int res,
                        const char *message) {
    auto &r = *static_cast<Runtime *>(data);
    std::cerr << "skyapod: PipeWire error " << res << ": "
              << (message ? message : "") << '\n';
    if (id == PW_ID_CORE)
      r.fail(message ? message : "PipeWire disconnected");
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
    links[channel] = reinterpret_cast<pw_proxy *>(pw_core_create_object(
        core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
        &properties->dict, 0));
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
              << " -> " << ownInputs[channel] << " ("
              << ports[channel].channel << ")\n";
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
  static void global(void *data, uint32_t id, uint32_t, const char *type,
                     uint32_t, const spa_dict *props) {
    auto &r = *static_cast<Runtime *>(data);
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
      std::cerr << "skyapod: capture link removed for "
                << r.ports[c].channel << "; scheduling recovery\n";
      r.scheduleLinkRetry();
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
      void *data,
      const skyapo::pipewire::DefaultSinkVolumeSnapshot &snapshot) {
    auto &r = *static_cast<Runtime *>(data);
    r.renderVolume = snapshot;
    skyapo::platform::LoudnessVolumeProvider::publish(
        snapshot.uniformChannelGainAvailable, snapshot.uniformChannelGainDb);
  }
  std::string status() {
    std::ostringstream s;
    const auto n = samples.load(), b = blocks.load();
    auto *e = active.load();
    s << "Daemon: " << pw_filter_state_as_string(state)
      << "\nSelected device: " << device.name << "\nCapture node: " << device.id
      << " (" << device.description
      << ")\nVirtual microphone: SkyAPO Virtual Mic\nVirtual node: "
      << pw_filter_get_node_id(filter)
      << " (skyapo.virtual_mic)\nFormat: F32 planar DSP\nChannels: " << channels
      << "\nGraph transition: "
      << (pending.load(std::memory_order_seq_cst) ? "crossfading" : "stable")
      << "\nFormat rebuilds during transition: "
      << formatRebuildsDuringTransition
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
    s << "\nDefault render endpoint: ";
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
    s << "\nFilters: " << (e ? e->filterCount() : 0) << "\nFilter chain:";
    if (!e || e->filterDescriptions().empty())
      s << "\n  (none)";
    else
      for (const auto &filter : e->filterDescriptions())
        s << "\n  " << filter;
    s << "\nPlugin-reported latency sum: ";
    const auto pluginLatency =
        e ? e->pluginLatencySamples() : std::optional<uint64_t>{};
    if (pluginLatency)
      s << *pluginLatency << " samples (no delay compensation)";
    else
      s << "unknown";
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
    s << "Plugin failures:";
    const auto failures = e ? e->failedPluginDescriptions()
                            : std::vector<std::string>{};
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
          r.reloadConfig();
          text = "Config reload succeeded\n";
        } catch (const std::exception &e) {
          r.configError = e.what();
          text = std::string("Config reload failed; keeping last valid graph: ") +
                 e.what() + "\n";
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
      } else if (request.command ==
                 settings::ipc::Command::SetPluginBypass) {
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
  void init(const std::string &selected, const std::string &cfg) {
    config = cfg;
    auto all = enumerateDevices();
    auto it = std::find_if(all.sources.begin(), all.sources.end(),
                           [&](auto &d) { return d.name == selected; });
    if (it == all.sources.end())
      throw std::runtime_error("selected device unavailable: " + selected);
    device = *it;
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
      e.error = coreError;
      return e;
    }();
    pw_core_add_listener(core, &coreHook, &ce, this);
    rateEvent = pw_loop_add_event(loop, rateChanged, this);
    transitionEvent = pw_loop_add_event(loop, transitionReady, this);
    sigint = pw_loop_add_signal(loop, SIGINT, signal, this);
    sigterm = pw_loop_add_signal(loop, SIGTERM, signal, this);
    selectionTimer = pw_loop_add_timer(loop, selectionChanged, this);
    reloadTimer = pw_loop_add_timer(loop, reloadReady, this);
    linkRetryTimer = pw_loop_add_timer(loop, retryLinks, this);
    configWatcher = std::make_unique<skyapo::platform::ConfigWatcher>(config);
    configEvent = pw_loop_add_io(loop, configWatcher->fileDescriptor(),
                                 SPA_IO_IN, false, configReady, this);
    if (!rateEvent || !transitionEvent || !sigint || !sigterm ||
        !selectionTimer || !reloadTimer || !linkRetryTimer || !configEvent)
      throw std::runtime_error("cannot create PipeWire loop events");
    timespec interval{1, 0};
    pw_loop_update_timer(loop, selectionTimer, &interval, &interval, false);
    auto *props = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.virtual_mic", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO Virtual Mic", PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true",
        PW_KEY_NODE_PAUSE_ON_IDLE, "false", nullptr);
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
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType,
                        SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype,
                        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_AUDIO_format, SPA_POD_Id(info.format),
                        SPA_FORMAT_AUDIO_rate,
                        SPA_POD_CHOICE_ENUM_Int(3, 48000, 44100, 96000),
                        SPA_FORMAT_AUDIO_channels, SPA_POD_Int(channels), 0);
    if (!SPA_FLAG_IS_SET(info.flags, SPA_AUDIO_FLAG_UNPOSITIONED))
      spa_pod_builder_add(&b, SPA_FORMAT_AUDIO_position,
                          SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id,
                                        channels, info.position), 0);
    const spa_pod *enumFormat =
        static_cast<spa_pod *>(spa_pod_builder_pop(&b, &formatFrame));
    const spa_pod *params[] = {
        enumFormat,
        static_cast<spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
            SPA_POD_Float(1.0f), SPA_PROP_mute, SPA_POD_Bool(false),
            SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                          unityVolumes.data())))};
    if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, params, 2) < 0)
      throw std::runtime_error("filter connection failed");
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
void runPipeWire(const std::string &device, const std::string &config,
                 volatile sig_atomic_t &stopping) {
  Runtime r(stopping);
  r.init(device, config);
  pw_main_loop_run(r.main);
  r.shutdownAudio();
  if (!r.error.empty())
    throw std::runtime_error(r.error);
}
