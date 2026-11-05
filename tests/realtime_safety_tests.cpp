#include "../src/platform/PlatformChannels.h"
#include "../src/platform/RealtimeAudit.h"
#include "../src/pipewire/PipeWireCallbackBoundary.h"
#include "../src/pipewire/CallbackRecoveryGate.h"
#include "../src/pipewire/PlaybackFaultRecovery.h"
#include "../src/pipewire/RuntimeAudioGate.h"
#include "Engine.h"
#include "../src/pipewire/TransitionMetricsGeneration.h"
#include "TestTemp.h"
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <new>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef SKYAPO_TEST_CONVOLUTION
#include <sndfile.h>
#endif
extern "C" void *__wrap_malloc(size_t);
extern "C" void __wrap_free(void *);
int main() {
  {
    using skyapo::pipewire::audioOutputMustBeSilent;
    using skyapo::pipewire::configReloadNeedsValidation;
    using skyapo::pipewire::formatRebuildMustDefer;
    using skyapo::pipewire::resumeRuntimeAudioGate;
    using skyapo::pipewire::RuntimeAudioMode;
    if (!configReloadNeedsValidation(false, false, true) ||
        !formatRebuildMustDefer(false, false, true) ||
        !formatRebuildMustDefer(true, false, false) ||
        !formatRebuildMustDefer(false, true, false) ||
        formatRebuildMustDefer(false, false, false) ||
        resumeRuntimeAudioGate(RuntimeAudioMode::Quiescing, false, false,
                               true) != RuntimeAudioMode::Quiescing ||
        resumeRuntimeAudioGate(RuntimeAudioMode::Quiescing, true, false,
                               false) != RuntimeAudioMode::Faulted ||
        resumeRuntimeAudioGate(RuntimeAudioMode::Quiescing, false, false,
                               false) != RuntimeAudioMode::Running ||
        resumeRuntimeAudioGate(RuntimeAudioMode::Stopping, false, false,
                               true) != RuntimeAudioMode::Stopping) {
      std::cerr << "queued PDC validation was mistaken for permission to "
                   "resume audio or skip config validation\n";
      return 1;
    }

    // A control-thread resume can race the callback that publishes the PDC
    // queue bit. Even if audioMode is momentarily Running, callback output
    // must remain silent until that queued validation is handled.
    if (!audioOutputMustBeSilent(RuntimeAudioMode::Running, true) ||
        audioOutputMustBeSilent(RuntimeAudioMode::Running, false) ||
        !audioOutputMustBeSilent(RuntimeAudioMode::Quiescing, false) ||
        !audioOutputMustBeSilent(RuntimeAudioMode::Faulted, false)) {
      std::cerr << "audio callback gate ignored unresolved PDC validation\n";
      return 1;
    }

    std::atomic<bool> pluginLatencyEventQueued{false};
    std::atomic<RuntimeAudioMode> audioMode{RuntimeAudioMode::Running};
    const bool candidateBuildStartedWithFault =
        configReloadNeedsValidation(false, false, false);
    std::thread callback([&] {
      pluginLatencyEventQueued.store(true, std::memory_order_release);
      audioMode.store(RuntimeAudioMode::Quiescing, std::memory_order_seq_cst);
    });
    callback.join();
    const bool commitNeedsValidation = configReloadNeedsValidation(
        false, false, pluginLatencyEventQueued.load(std::memory_order_acquire));
    const auto modeAfterResumeAttempt = resumeRuntimeAudioGate(
        audioMode.load(std::memory_order_seq_cst), false, false,
        pluginLatencyEventQueued.load(std::memory_order_acquire));
    if (candidateBuildStartedWithFault || !commitNeedsValidation ||
        modeAfterResumeAttempt != RuntimeAudioMode::Quiescing) {
      std::cerr << "a PDC event queued after candidate construction was lost "
                   "before config commit\n";
      return 1;
    }
  }
  {
    using skyapo::pipewire::detail::PlaybackFaultRecovery;
    std::atomic<bool> faulted{false};
    std::atomic<bool> pluginLatencyFaulted{false};
    std::atomic<bool> controlCallbackFailed{false};

    const auto reload = PlaybackFaultRecovery::beginConfigReload(true, false);
    std::thread realtimeCallback([&] {
      pluginLatencyFaulted.store(true, std::memory_order_release);
      faulted.store(true, std::memory_order_release);
    });
    realtimeCallback.join();

    PlaybackFaultRecovery::finishConfigReload(reload, pluginLatencyFaulted,
                                              faulted, controlCallbackFailed);
    if (!faulted.load(std::memory_order_acquire) ||
        !pluginLatencyFaulted.load(std::memory_order_acquire)) {
      std::cerr << "an unvalidated config reload cleared a PDC fault raised "
                   "while the candidate was being built\n";
      return 1;
    }

    const auto validatedReload =
        PlaybackFaultRecovery::beginConfigReload(true, true);
    PlaybackFaultRecovery::finishConfigReload(
        validatedReload, pluginLatencyFaulted, faulted, controlCallbackFailed);
    if (faulted.load(std::memory_order_acquire) ||
        pluginLatencyFaulted.load(std::memory_order_acquire)) {
      std::cerr << "a validated replacement did not clear its recoverable "
                   "PDC fault\n";
      return 1;
    }

    faulted.store(true, std::memory_order_release);
    pluginLatencyFaulted.store(true, std::memory_order_release);
    controlCallbackFailed.store(true, std::memory_order_release);
    PlaybackFaultRecovery::finishConfigReload(
        validatedReload, pluginLatencyFaulted, faulted, controlCallbackFailed);
    if (!faulted.load(std::memory_order_acquire) ||
        pluginLatencyFaulted.load(std::memory_order_acquire)) {
      std::cerr << "config reload cleared a permanent control-callback fault\n";
      return 1;
    }
  }
  {
    using skyapo::pipewire::detail::CallbackRecoveryGate;
    using skyapo::pipewire::detail::PlaybackFaultRecovery;
    CallbackRecoveryGate gate;
    std::atomic<bool> callbackEntered{false};
    std::atomic<bool> publishFault{false};
    std::atomic<bool> faulted{false};
    std::atomic<bool> pluginLatencyFaulted{false};
    std::atomic<bool> controlCallbackFailed{false};

    std::thread callback([&] {
      auto flight = gate.enterCallback();
      callbackEntered.store(true, std::memory_order_release);
      while (!publishFault.load(std::memory_order_acquire))
        std::this_thread::yield();
      if (flight.mayProcess()) {
        pluginLatencyFaulted.store(true, std::memory_order_release);
        faulted.store(true, std::memory_order_release);
      }
    });
    while (!callbackEntered.load(std::memory_order_acquire))
      std::this_thread::yield();

    gate.closeForRecovery();
    publishFault.store(true, std::memory_order_release);
    while (gate.callbacksInFlight() != 0)
      std::this_thread::yield();
    const auto reload = PlaybackFaultRecovery::beginConfigReload(
        true, faulted.load(std::memory_order_acquire));
    if (!reload.validatedForRecovery) {
      callback.join();
      std::cerr << "config commit did not observe a PDC fault from a callback "
                   "that was drained at the recovery boundary\n";
      return 1;
    }
    PlaybackFaultRecovery::finishConfigReload(reload, pluginLatencyFaulted,
                                              faulted, controlCallbackFailed);
    gate.finishRecovery();
    callback.join();
    if (faulted.load(std::memory_order_acquire) ||
        pluginLatencyFaulted.load(std::memory_order_acquire) ||
        gate.recoveryInProgress() || gate.callbacksInFlight() != 0) {
      std::cerr << "validated recovery did not clear the drained PDC fault "
                   "and reopen the callback gate\n";
      return 1;
    }
  }
  {
    using skyapo::pipewire::detail::CallbackRecoveryGate;
    CallbackRecoveryGate gate;
    realtime::allocations = 0;
    realtime::deallocations = 0;
    {
      realtime::Scope audit;
      {
        auto activeCallback = gate.enterCallback();
        if (!activeCallback.mayProcess()) {
          std::cerr << "open recovery gate rejected an audio callback\n";
          return 1;
        }
        gate.closeForRecovery();
        if (!gate.recoveryInProgress() || gate.callbacksInFlight() != 1) {
          std::cerr << "recovery gate did not close and track an active "
                       "callback\n";
          return 1;
        }
      }
      if (gate.callbacksInFlight() != 0) {
        std::cerr << "recovery callback guard did not leave cleanly\n";
        return 1;
      }

      {
        auto callbackAfterDrain = gate.enterCallback();
        if (callbackAfterDrain.mayProcess() || gate.callbacksInFlight() != 1) {
          std::cerr << "callback entering after recovery drain was admitted\n";
          return 1;
        }
        gate.finishRecovery();
        if (gate.recoveryInProgress() || callbackAfterDrain.mayProcess()) {
          std::cerr << "callback admitted while recovery was closed became "
                       "active after the gate reopened\n";
          return 1;
        }
      }
      if (gate.callbacksInFlight() != 0) {
        std::cerr << "recovery callback count underflowed after gate reopen\n";
        return 1;
      }
      auto recoveredCallback = gate.enterCallback();
      if (!recoveredCallback.mayProcess()) {
        std::cerr << "validated recovered engine remained gated\n";
        return 1;
      }
    }
    if (realtime::allocations.load() != 0 ||
        realtime::deallocations.load() != 0) {
      std::cerr << "callback recovery gate allocated in realtime scope\n";
      return 1;
    }
  }
  {
    using skyapo::pipewire::detail::CallbackRecoveryGate;
    CallbackRecoveryGate gate;
    std::atomic<bool> priorEntered{false};
    std::atomic<bool> releasePrior{false};
    std::atomic<bool> drainStarted{false};
    std::atomic<bool> swapStarted{false};
    std::atomic<bool> lateEntered{false};
    std::atomic<bool> swapping{false};
    std::atomic<bool> violation{false};

    std::thread priorCallback([&] {
      auto flight = gate.enterCallback();
      if (!flight.mayProcess())
        violation.store(true, std::memory_order_seq_cst);
      priorEntered.store(true, std::memory_order_seq_cst);
      while (!releasePrior.load(std::memory_order_seq_cst))
        std::this_thread::yield();
      if (swapping.load(std::memory_order_seq_cst))
        violation.store(true, std::memory_order_seq_cst);
    });
    while (!priorEntered.load(std::memory_order_seq_cst))
      std::this_thread::yield();

    gate.closeForRecovery();
    std::thread controlThread([&] {
      drainStarted.store(true, std::memory_order_seq_cst);
      while (gate.callbacksInFlight() != 0)
        std::this_thread::yield();
      swapping.store(true, std::memory_order_seq_cst);
      swapStarted.store(true, std::memory_order_seq_cst);
      while (!lateEntered.load(std::memory_order_seq_cst))
        std::this_thread::yield();
      swapping.store(false, std::memory_order_seq_cst);
      gate.finishRecovery();
    });
    while (!drainStarted.load(std::memory_order_seq_cst))
      std::this_thread::yield();
    const bool drainedBeforePriorExit =
        swapStarted.load(std::memory_order_seq_cst);
    releasePrior.store(true, std::memory_order_seq_cst);
    while (!swapStarted.load(std::memory_order_seq_cst))
      std::this_thread::yield();

    std::thread lateCallback([&] {
      auto flight = gate.enterCallback();
      if (flight.mayProcess() && swapping.load(std::memory_order_seq_cst))
        violation.store(true, std::memory_order_seq_cst);
      if (flight.mayProcess())
        violation.store(true, std::memory_order_seq_cst);
      lateEntered.store(true, std::memory_order_seq_cst);
    });

    priorCallback.join();
    lateCallback.join();
    controlThread.join();
    if (drainedBeforePriorExit || violation.load(std::memory_order_seq_cst) ||
        gate.recoveryInProgress() || gate.callbacksInFlight() != 0) {
      std::cerr << "concurrent callback recovery admitted Engine access across "
                   "the drain/swap boundary\n";
      return 1;
    }
  }
  {
    std::atomic<bool> callbackFailed{false};
    std::atomic<bool> faulted{false};
    bool returnedFromCallback = false;
    skyapo::pipewire::detail::invokeControlCallback(
        [&] { returnedFromCallback = true; }, callbackFailed, faulted);
    if (!returnedFromCallback || callbackFailed.load() || faulted.load()) {
      std::cerr
          << "successful PipeWire control callback changed failure state\n";
      return 1;
    }

    callbackFailed.store(false);
    faulted.store(false);
    skyapo::pipewire::detail::invokeControlCallback(
        [] { throw std::bad_alloc(); }, callbackFailed, faulted);
    if (!callbackFailed.load() || !faulted.load()) {
      std::cerr << "PipeWire control callback did not contain bad_alloc\n";
      return 1;
    }

    callbackFailed.store(false);
    faulted.store(false);
    skyapo::pipewire::detail::invokeControlCallback([] { throw 7; },
                                                    callbackFailed, faulted);
    if (!callbackFailed.load() || !faulted.load()) {
      std::cerr
          << "PipeWire control callback did not contain unknown exception\n";
      return 1;
    }
  }
  {
    skyapo::pipewire::TransitionMetricsGeneration generations;
    const auto first = generations.graphReplaced();
    generations.transitionStarted(first);
    generations.transitionCompleted();
    // Simulate a completion event queued behind a newer hard graph swap.
    generations.graphReplaced();
    if (generations.shouldResetMetrics()) {
      std::cerr << "stale graph completion reset current telemetry\n";
      return 1;
    }
    const auto next = generations.graphReplaced();
    generations.transitionStarted(next);
    generations.transitionCompleted();
    if (!generations.shouldResetMetrics() || generations.shouldResetMetrics()) {
      std::cerr << "current graph completion did not reset telemetry once\n";
      return 1;
    }
  }
  {
    realtime::Scope scope;
    auto *c = __wrap_malloc(8);
    __wrap_free(c);
    auto *cpp = ::operator new(16);
    ::operator delete(cpp);
  }
  if (realtime::allocations.load() != 2 ||
      realtime::deallocations.load() != 2) {
    std::cerr << "allocation instrumentation is not working\n";
    return 1;
  }
  realtime::allocations = 0;
  realtime::deallocations = 0;
  std::string path = skyapo::test::temporaryPattern("skyapo-rt-test-");
  int fd = mkstemp(path.data());
  if (fd < 0)
    return 1;
  close(fd);
  {
    std::ofstream f(path);
    f << "Preamp: -6 dB\nFilter: ON PK Fc 1000 Hz Gain 2 dB Q 1\nDelay: 3 "
         "Samples\nFilter: ON IIR Order 1 Coefficients 1 0 1 0\n";
  }
  for (unsigned channels : {1u, 2u}) {
    std::vector<std::wstring> names =
        channels == 1
            ? std::vector<std::wstring>{eapoChannel("MONO")}
            : std::vector<std::wstring>{eapoChannel("FL"), eapoChannel("FR")};
    Engine engine(48000, channels, 8192, names);
    engine.loadConfig(path);
    std::vector<float> audio(channels * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      unsigned frames = (iteration * 71) % 8192 + 1;
      for (unsigned i = 0; i < frames * channels; ++i)
        audio[i] = .1f * std::sin(float(i));
      {
        realtime::Scope scope;
        engine.process(audio.data(), frames);
      }
    }
  }
  const std::string transitionPath = std::string(path) + ".transition";
  {
    std::ofstream config(transitionPath);
    config << "Preamp: -6 dB\n";
  }
  {
    std::ofstream config(path);
    config << "Preamp: 0 dB\n";
  }
  Engine transitionFrom(48000, 2, 512, {eapoChannel("FL"), eapoChannel("FR")});
  Engine transitionTo(48000, 2, 512, {eapoChannel("FL"), eapoChannel("FR")});
  transitionFrom.loadConfig(path);
  transitionTo.loadConfig(transitionPath);
  realtime::allocations = 0;
  realtime::deallocations = 0;
  std::vector<float> transitionAudio(512 * 2, 0.1f);
  unsigned transitionCounter = 0;
  for (unsigned frames : {127u, 256u, 97u}) {
    {
      realtime::Scope scope;
      transitionCounter = transitionFrom.processTransitionTo(
          transitionTo, transitionAudio.data(), frames, transitionCounter, 480);
    }
  }
  if (transitionCounter != 480 || realtime::allocations.load() != 0 ||
      realtime::deallocations.load() != 0) {
    std::cerr << "upstream FilterConfiguration transition allocated/freed "
                 "in realtime scope\n";
    return 1;
  }
  {
    realtime::Scope scope;
    transitionCounter = transitionFrom.processTransitionTo(
        transitionTo, transitionAudio.data(), 32, transitionCounter, 480);
  }
  if (transitionCounter != 512 || realtime::allocations.load() != 0 ||
      realtime::deallocations.load() != 0) {
    std::cerr << "upstream FilterConfiguration transition block crossing was "
                 "not allocation-free\n";
    return 1;
  }
  unlink(transitionPath.c_str());
  {
    std::string routePath =
        skyapo::test::temporaryPattern("skyapo-route-test-");
    int routeFd = mkstemp(routePath.data());
    if (routeFd < 0)
      return 1;
    close(routeFd);
    {
      std::ofstream f(routePath);
      f << "Channel: L\nCopy: L=R R=L\nPreamp: -6 dB\n"
           "Filter: ON PK Fc 1000 Hz Gain 2 dB Q 1\nDelay: 3 Samples\n";
    }
    Engine route(48000, 2, 8192, {L"L", L"R"});
    route.loadConfig(routePath);
    std::vector<float> audio(2 * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      unsigned frames = (iteration * 43) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::cos(float(i));
      {
        realtime::Scope scope;
        route.process(audio.data(), frames);
      }
    }
    unlink(routePath.c_str());
  }
#ifdef SKYAPO_TEST_LV2
  {
    std::string pluginPath = skyapo::test::temporaryPattern("skyapo-lv2-test-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: LV2 https://skyapo.example/plugins/test-gain\n";
    }
    Engine plugin(48000, 2, 8192, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 8192);
    realtime::allocations = 0;
    realtime::deallocations = 0;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      plugin.setPluginParameter("https://skyapo.example/plugins/test-gain",
                                "gain", iteration % 2 == 0 ? 0.25f : 0.75f);
      plugin.setPluginBypass("https://skyapo.example/plugins/test-gain",
                             iteration % 2 == 0);
      const unsigned frames = (iteration * 59) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::sin(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    unlink(pluginPath.c_str());
  }
  {
    std::string pluginPath =
        skyapo::test::temporaryPattern("skyapo-lv2-latency-rt-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Copy: L2=L R2=R\n"
           "Plugin: LV2 "
           "https://skyapo.example/plugins/test-latency-designated\n"
           "Copy: L=0.5*L+0.5*L2\n";
    }
    Engine plugin(48000, 2, 8192, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 8192);
    realtime::allocations = 0;
    realtime::deallocations = 0;
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      const unsigned frames = (iteration * 61) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::cos(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
      if (realtime::allocations.load() != 0 ||
          realtime::deallocations.load() != 0) {
        std::cerr << "plugin/Copy PDC allocated in realtime processing\n";
        return 1;
      }
      if (plugin.pluginLatencyRefreshPending())
        plugin.refreshPluginLatencies();
    }
    unlink(pluginPath.c_str());
  }
#endif
#ifdef SKYAPO_TEST_CLAP
  {
    std::string pluginPath =
        skyapo::test::temporaryPattern("skyapo-clap-test-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: CLAP org.skyapo.test.gain 7=0.25\n";
    }
    Engine plugin(48000, 2, 8192, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      plugin.setPluginBypass("org.skyapo.test.gain", iteration % 2 == 0);
      const unsigned frames = (iteration * 61) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::sin(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    unlink(pluginPath.c_str());
  }
  {
    std::string pluginPath =
        skyapo::test::temporaryPattern("skyapo-clap-latency-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: CLAP org.skyapo.test.latency\n";
    }
    Engine plugin(48000, 2, 128, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 128);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      plugin.setPluginBypass("org.skyapo.test.latency", iteration % 2 == 0);
      const unsigned frames = (iteration * 47) % 128 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::sin(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    unlink(pluginPath.c_str());
  }
#endif
#ifdef SKYAPO_TEST_VST3
  {
    std::string pluginPath =
        skyapo::test::temporaryPattern("skyapo-vst3-test-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: VST3 534B5941504F00010000000000000001 7=0.25\n";
    }
    Engine plugin(48000, 2, 8192, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      // Exercise a newly published VST3 automation point on each block.
      plugin.setPluginParameter("534B5941504F00010000000000000001", "7",
                                0.2f +
                                    0.1f * static_cast<float>(iteration % 7));
      plugin.setPluginBypass("534B5941504F00010000000000000001",
                             iteration % 2 == 0);
      const unsigned frames = (iteration * 67) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::sin(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    unlink(pluginPath.c_str());
  }
  {
    std::string pluginPath =
        skyapo::test::temporaryPattern("skyapo-vst3-latency-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: VST3 534B5941504F00010000000000000003\n";
    }
    Engine plugin(48000, 2, 128, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 128);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      plugin.setPluginBypass("534B5941504F00010000000000000003",
                             iteration % 2 == 0);
      const unsigned frames = (iteration * 53) % 128 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::cos(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    unlink(pluginPath.c_str());
  }
#endif
#ifdef SKYAPO_TEST_VST2
  {
    std::string pluginPath =
        skyapo::test::temporaryPattern("skyapo-vst2-test-");
    int pluginFd = mkstemp(pluginPath.data());
    if (pluginFd < 0)
      return 1;
    close(pluginFd);
    {
      std::ofstream f(pluginPath);
      f << "Plugin: VST2 \"" SKYAPO_TEST_VST2_PATH "\" 0=0.25\n";
    }
    Engine plugin(48000, 2, 8192, {L"L", L"R"});
    plugin.loadConfig(pluginPath);
    std::vector<float> audio(2 * 8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      plugin.setPluginParameter(SKYAPO_TEST_VST2_PATH, "Gain",
                                iteration % 2 == 0 ? 0.25f : 0.75f);
      plugin.setPluginBypass(SKYAPO_TEST_VST2_PATH, iteration % 2 == 0);
      const unsigned frames = (iteration * 73) % 8192 + 1;
      for (unsigned i = 0; i < frames * 2; ++i)
        audio[i] = .1f * std::sin(float(i + iteration));
      {
        realtime::Scope scope;
        plugin.process(audio.data(), frames);
      }
    }
    std::string dynamicConfig =
        skyapo::test::temporaryPattern("skyapo-vst2-dynamic-rt-");
    int dynamicFd = mkstemp(dynamicConfig.data());
    if (dynamicFd < 0)
      return 1;
    close(dynamicFd);
    {
      std::ofstream f(dynamicConfig);
      f << "Plugin: VST2 \"" SKYAPO_TEST_VST2_DYNAMIC_LATENCY_PATH "\"\n";
    }
    Engine dynamicPlugin(48000, 2, 128, {L"L", L"R"});
    dynamicPlugin.loadConfig(dynamicConfig);
    std::vector<float> dynamicAudio(128 * 2, 0.1f);
    {
      realtime::Scope scope;
      dynamicPlugin.process(dynamicAudio.data(), 64);
    }
    if (!dynamicPlugin.pluginLatencyRefreshPending()) {
      std::cerr
          << "VST2 latency-change notification was not raised in RT test\n";
      return 1;
    }
    unlink(dynamicConfig.c_str());
    std::string monoConfig =
        skyapo::test::temporaryPattern("skyapo-vst2-mono-");
    int monoFd = mkstemp(monoConfig.data());
    if (monoFd < 0)
      return 1;
    close(monoFd);
    {
      std::ofstream f(monoConfig);
      f << "Plugin: VST2 \"" SKYAPO_TEST_VST2_MONO_PATH "\"\n";
    }
    Engine monoPlugin(48000, 1, 8192, {L"C"});
    monoPlugin.loadConfig(monoConfig);
    std::vector<float> monoAudio(8192);
    for (unsigned iteration = 0; iteration < 1000; ++iteration) {
      const unsigned frames = (iteration * 79) % 8192 + 1;
      for (unsigned i = 0; i < frames; ++i)
        monoAudio[i] = .1f * std::cos(float(i + iteration));
      {
        realtime::Scope scope;
        monoPlugin.process(monoAudio.data(), frames);
      }
    }
    unlink(monoConfig.c_str());
    unlink(pluginPath.c_str());
  }
#endif
#ifdef SKYAPO_TEST_CONVOLUTION
  std::string irTemplate = skyapo::test::temporaryPattern("skyapo-rt-ir-");
  int irFd = mkstemp(irTemplate.data());
  if (irFd < 0)
    return 1;
  close(irFd);
  SF_INFO info{};
  info.samplerate = 48000;
  info.channels = 1;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  SNDFILE *ir = sf_open(irTemplate.c_str(), SFM_WRITE, &info);
  if (!ir)
    return 1;
  const float taps[] = {1.0f, 0.25f, -0.125f, 0.0f};
  if (sf_writef_float(ir, taps, 4) != 4) {
    sf_close(ir);
    unlink(irTemplate.c_str());
    return 1;
  }
  sf_close(ir);
  std::string configPath = std::string(irTemplate) + ".txt";
  {
    std::ofstream f(configPath);
    f << "Convolution: " << irTemplate << '\n';
  }
  Engine convolution(48000, 2, 256, {eapoChannel("FL"), eapoChannel("FR")});
  convolution.loadConfig(configPath);
  std::vector<float> convolutionBlock(256 * 2);
  for (unsigned iteration = 0; iteration < 1000; ++iteration) {
    for (unsigned i = 0; i < convolutionBlock.size(); ++i)
      convolutionBlock[i] = .1f * std::sin(float(i + iteration));
    {
      realtime::Scope scope;
      convolution.process(convolutionBlock.data(), 256);
    }
  }
  unlink(configPath.c_str());
  unlink(irTemplate.c_str());
#endif
  unlink(path.c_str());
  if (realtime::allocations.load() || realtime::deallocations.load()) {
    std::cerr << "DSP process allocated/freed in realtime scope\n";
    return 1;
  }
  if (eapoChannel("FC") != L"C" || eapoChannel("LFE") != L"LFE" ||
      eapoChannel("SL") != L"SL")
    return 1;
  std::cout << "Allocation hooks verified; mono/stereo, channel-routing"
#ifdef SKYAPO_TEST_CONVOLUTION
               ", and upstream convolution"
#endif
#ifdef SKYAPO_TEST_VST3
               ", and VST3 plugin"
#endif
#ifdef SKYAPO_TEST_VST2
               ", and FST-hosted VST2-compatible plugin"
#endif
               " DSP blocks without heap allocation/freeing\n";
  return 0;
}
