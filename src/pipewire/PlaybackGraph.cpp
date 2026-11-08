#include "PlaybackGraph.h"
#include "CallbackRecoveryGate.h"
#include "PipeWireCallbackBoundary.h"
#include "PlaybackFaultRecovery.h"

#include "../platform/PlatformChannels.h"
#include "../platform/RealtimeAudit.h"
#include "DeviceManager.h"
#include "Engine.h"
#include "helpers/StringHelper.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <pipewire/filter.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
#include <fcntl.h>
#include <unistd.h>
#endif

namespace skyapo::pipewire {
namespace {
constexpr unsigned MaxFrames = 65536;
constexpr unsigned MaxChannels = 8;
constexpr unsigned TransitionMs = 10;
static_assert(std::atomic<float>::is_always_lock_free,
              "realtime playback controls require lock-free float atomics");
constexpr std::array<const char *, 9> ChannelOrder = {
    "MONO", "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
constexpr std::array<std::pair<const char *, uint32_t>, 9> SpaChannels = {
    {{"MONO", SPA_AUDIO_CHANNEL_MONO},
     {"FL", SPA_AUDIO_CHANNEL_FL},
     {"FR", SPA_AUDIO_CHANNEL_FR},
     {"FC", SPA_AUDIO_CHANNEL_FC},
     {"LFE", SPA_AUDIO_CHANNEL_LFE},
     {"RL", SPA_AUDIO_CHANNEL_RL},
     {"RR", SPA_AUDIO_CHANNEL_RR},
     {"SL", SPA_AUDIO_CHANNEL_SL},
     {"SR", SPA_AUDIO_CHANNEL_SR}}};

unsigned transitionDurationMs() {
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
  if (const char *value = std::getenv("SKYAPO_TEST_TRANSITION_MS")) {
    char *end = nullptr;
    errno = 0;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno || end == value || *end != '\0' || parsed < 10 || parsed > 5000)
      throw std::runtime_error(
          "SKYAPO_TEST_TRANSITION_MS must be an integer from 10 to 5000");
    return static_cast<unsigned>(parsed);
  }
#endif
  return TransitionMs;
}

std::string property(const spa_dict *dict, const char *key) {
  const char *value = dict ? spa_dict_lookup(dict, key) : nullptr;
  return value ? value : "";
}

std::vector<std::wstring> channelNames(const std::vector<AudioPort> &ports) {
  std::vector<std::wstring> names;
  names.reserve(ports.size());
  for (const auto &port : ports)
    names.push_back(eapoChannel(port.channel));
  return names;
}

#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
enum class PlaybackLinkFaultForTesting { none, reject, asynchronousError };

std::atomic<bool> playbackLinkFaultUsedForTesting{false};
std::atomic<bool> playbackLinkAsyncErrorPendingForTesting{false};

void writePlaybackLinkFaultReceiptForTesting(const char *message) noexcept {
  const char *receipt = std::getenv("SKYAPO_TEST_LINK_FAULT_RECEIPT");
  if (!receipt)
    return;
  const int descriptor =
      open(receipt, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (descriptor < 0)
    return;
  (void)write(descriptor, message, std::strlen(message));
  close(descriptor);
}

PlaybackLinkFaultForTesting playbackLinkFaultForTesting() noexcept {
  const char *trigger = std::getenv("SKYAPO_TEST_LINK_FAULT_TRIGGER");
  if (!trigger || access(trigger, F_OK) != 0)
    return PlaybackLinkFaultForTesting::none;

  bool expected = false;
  if (!playbackLinkFaultUsedForTesting.compare_exchange_strong(
          expected, true, std::memory_order_relaxed))
    return PlaybackLinkFaultForTesting::none;

  const char *mode = std::getenv("SKYAPO_TEST_LINK_FAULT_MODE");
  if (mode && std::strcmp(mode, "async-error") == 0) {
    playbackLinkAsyncErrorPendingForTesting.store(true,
                                                  std::memory_order_relaxed);
    return PlaybackLinkFaultForTesting::asynchronousError;
  }
  writePlaybackLinkFaultReceiptForTesting(
      "injected playback link creation refusal\n");
  return PlaybackLinkFaultForTesting::reject;
}

void recordPlaybackLinkAsyncErrorForTesting() noexcept {
  if (playbackLinkAsyncErrorPendingForTesting.exchange(
          false, std::memory_order_relaxed))
    writePlaybackLinkFaultReceiptForTesting(
        "observed asynchronous PipeWire link error\n");
}
#endif

} // namespace

struct PlaybackGraph::Impl {
  pw_core *core{};
  pw_loop *loop{};
  pw_registry *registry{};
  spa_hook coreHook{};
  bool coreListenerAttached{};
  int initialRegistrySyncSequence{-1};
  bool initialFrontendCreated{};
  AudioDevice sink;
  std::vector<AudioPort> ports;
  std::string configPath;
  std::shared_ptr<const ConfigSourceSnapshot> acceptedConfigSources;
  std::string lastInternalLinkError;
  std::string lastSinkLinkError;
  pw_node *frontend{};
  uint32_t frontendId{SPA_ID_INVALID};
  spa_hook frontendHook{};
  spa_hook frontendProxyHook{};
  bool frontendListenerAttached{};
  bool frontendProxyListenerAttached{};
  bool frontendLinksEnabled{};
  bool sessionManagerPresent{};
  std::array<uint32_t, MaxChannels> frontendMonitorIds{};
  std::array<uint32_t, MaxChannels> dspInputIds{};
  struct FrontendLink {
    Impl *owner{};
    unsigned channel{};
    pw_proxy *proxy{};
    spa_hook proxyHook{};
    spa_hook linkHook{};
    uint32_t globalId{SPA_ID_INVALID};
    bool proxyListenerAttached{};
    bool linkListenerAttached{};
    bool active{};
  };
  std::array<FrontendLink, MaxChannels> frontendLinks{};
  std::string lastFrontendLinkError;
  std::string frontendPositions;
  std::array<float, MaxChannels> frontendRestoreVolumes{};
  float frontendScalarMetadata{1.0f};
  float frontendRestoreScalar{1.0f};
  bool frontendRestoreMuted{};
  bool frontendControlsRestored{};
  bool frontendRecreateNeeded{};
  std::atomic<bool> frontendFaulted{true};
  std::vector<AudioPort> knownOutputPorts;
  pw_filter *filter{};
  pw_filter *streamFilter{};
  spa_hook filterHook{};
  spa_hook streamFilterHook{};
  spa_hook registryHook{};
  spa_source *formatEvent{};
  spa_source *linkRetryTimer{};
  spa_source *frontendLinkStartTimer{};
  bool filterListenerAttached{};
  bool streamFilterListenerAttached{};
  bool registryListenerAttached{};
  std::array<void *, MaxChannels> inputs{};
  std::array<void *, MaxChannels> outputs{};
  std::array<void *, MaxChannels> streamInputs{};
  std::array<void *, MaxChannels> streamOutputs{};
  std::array<uint32_t, MaxChannels> ownOutputIds{};
  std::array<uint32_t, MaxChannels> streamInputIds{};
  std::array<uint32_t, MaxChannels> streamOutputIds{};
  std::array<pw_proxy *, MaxChannels> links{};
  std::array<spa_hook, MaxChannels> linkHooks{};
  std::array<uint32_t, MaxChannels> linkGlobalIds{};
  std::array<pw_proxy *, MaxChannels> sinkLinks{};
  std::array<spa_hook, MaxChannels> sinkLinkHooks{};
  std::array<spa_hook, MaxChannels> sinkProxyHooks{};
  std::array<uint32_t, MaxChannels> sinkLinkGlobalIds{};
  struct ObservedSinkLink {
    uint32_t id{SPA_ID_INVALID};
    uint32_t outputNode{SPA_ID_INVALID};
    uint32_t outputPort{SPA_ID_INVALID};
    uint32_t inputNode{SPA_ID_INVALID};
    uint32_t inputPort{SPA_ID_INVALID};
  };
  std::array<ObservedSinkLink, MaxChannels * 4> observedSinkLinks{};
  size_t observedSinkLinkCount{};
  std::array<bool, MaxChannels> linked{};
  std::array<spa_hook, MaxChannels> proxyHooks{};
  std::array<bool, MaxChannels> internalLinked{};
  std::array<bool, MaxChannels> internalLinkExists{};
  std::array<bool, MaxChannels> proxyListenerAttached{};
  std::array<bool, MaxChannels> linkListenerAttached{};
  std::array<bool, MaxChannels> sinkProxyListenerAttached{};
  std::array<bool, MaxChannels> sinkLinkListenerAttached{};
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
  std::array<pw_proxy *, MaxChannels> testPreexistingSinkLinks{};
  std::array<bool, MaxChannels> testSinkLinkAlreadyExistsInjected{};
#endif
  struct LinkContext {
    Impl *owner{};
    unsigned channel{};
    bool sinkLink{};
  };
  struct NodeBinding {
    Impl *owner{};
    uint32_t id{};
    pw_node *node{};
    spa_hook hook{};
  };
  std::array<LinkContext, MaxChannels> linkContexts{};
  std::array<LinkContext, MaxChannels> sinkLinkContexts{};
  std::vector<std::unique_ptr<NodeBinding>> sinkBindings;
  std::vector<AudioPort> knownInputPorts;
  std::vector<float> work;
  std::unique_ptr<Engine> activeOwner;
  std::unique_ptr<Engine> pendingOwner;
  std::vector<std::unique_ptr<Engine>> retiredOwners;
  std::atomic<Engine *> active{nullptr};
  std::atomic<Engine *> pending{nullptr};
  detail::CallbackRecoveryGate callbackGate;
  std::atomic<unsigned> requestedRate{0}, requestedQuantum{0};
  std::atomic<unsigned> rate{0}, quantum{0};
  std::array<std::atomic<float>, MaxChannels> sinkVolumes{};
  std::array<std::atomic<float>, MaxChannels> streamVolumes{};
  std::atomic<bool> sinkMuted{false};
  std::atomic<bool> streamMuted{false};
  std::atomic<bool> transitionComplete{false};
  std::atomic<bool> faulted{false};
  std::atomic<bool> controlCallbackFailed{false};
  std::atomic<bool> pluginLatencyFaulted{false};
  std::atomic<bool> pluginLatencyRefreshQueued{false};
  std::atomic<bool> formatEventPending{false};
  unsigned linkRetryBackoffMs{100};
  bool linkRetryScheduled{};
  unsigned channels{};
  unsigned transitionCounter{};
  unsigned transitionLength{};
  unsigned pluginLatencyRefreshes{};
  pw_filter_state state{PW_FILTER_STATE_UNCONNECTED};
  pw_filter_state streamState{PW_FILTER_STATE_UNCONNECTED};
  std::string error;
  bool sinkAvailable{true};
  bool destroying{};

  Impl(pw_core *ownerCore, pw_loop *ownerLoop, pw_registry *ownerRegistry,
       AudioDevice selectedSink, std::vector<AudioPort> sinkPorts,
       std::string config, bool hasSessionManager)
      : core(ownerCore), loop(ownerLoop), registry(ownerRegistry),
        sink(std::move(selectedSink)), ports(std::move(sinkPorts)),
        configPath(std::move(config)),
        sessionManagerPresent(hasSessionManager) {
    if (!core || !loop || !registry)
      throw std::invalid_argument("playback graph requires a PipeWire core");
    std::sort(
        ports.begin(), ports.end(), [](const auto &left, const auto &right) {
          const auto leftPosition =
              std::find(ChannelOrder.begin(), ChannelOrder.end(), left.channel);
          const auto rightPosition = std::find(
              ChannelOrder.begin(), ChannelOrder.end(), right.channel);
          return leftPosition < rightPosition;
        });
    if (ports.empty() || ports.size() > MaxChannels)
      throw std::runtime_error("selected playback sink has no supported ports "
                               "or more than 8 channels");
    for (const auto &port : ports) {
      if (port.channel.empty() ||
          std::find_if(SpaChannels.begin(), SpaChannels.end(),
                       [&](const auto &item) {
                         return port.channel == item.first;
                       }) == SpaChannels.end())
        throw std::runtime_error("unsupported playback channel position: " +
                                 port.channel);
    }
    channels = static_cast<unsigned>(ports.size());
    work.resize(static_cast<size_t>(channels) * MaxFrames);
    for (unsigned channel = 0; channel < MaxChannels; ++channel) {
      sinkVolumes[channel].store(1.0f, std::memory_order_relaxed);
      streamVolumes[channel].store(1.0f, std::memory_order_relaxed);
    }
    frontendMonitorIds.fill(SPA_ID_INVALID);
    dspInputIds.fill(SPA_ID_INVALID);
    for (unsigned channel = 0; channel < channels; ++channel) {
      frontendLinks[channel].owner = this;
      frontendLinks[channel].channel = channel;
    }
    ownOutputIds.fill(SPA_ID_INVALID);
    streamInputIds.fill(SPA_ID_INVALID);
    streamOutputIds.fill(SPA_ID_INVALID);
    linkGlobalIds.fill(SPA_ID_INVALID);
    sinkLinkGlobalIds.fill(SPA_ID_INVALID);
    for (unsigned channel = 0; channel < channels; ++channel) {
      linkContexts[channel] = {this, channel};
      sinkLinkContexts[channel] = {this, channel, true};
    }
    frontendPositions = "[";
    for (const auto &port : ports)
      frontendPositions += " " + port.channel;
    frontendPositions += " ]";
    formatEvent = pw_loop_add_event(loop, formatReady, this);
    if (!formatEvent)
      throw std::runtime_error("cannot create playback graph format event");
    linkRetryTimer = pw_loop_add_timer(loop, retryLinks, this);
    if (!linkRetryTimer) {
      pw_loop_destroy_source(loop, formatEvent);
      formatEvent = nullptr;
      throw std::runtime_error("cannot create playback link retry timer");
    }
    frontendLinkStartTimer = pw_loop_add_timer(loop, startFrontendLinks, this);
    if (!frontendLinkStartTimer) {
      pw_loop_destroy_source(loop, linkRetryTimer);
      pw_loop_destroy_source(loop, formatEvent);
      linkRetryTimer = nullptr;
      formatEvent = nullptr;
      throw std::runtime_error("cannot create playback adapter start timer");
    }
    try {
      static const auto coreEvents = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = initialRegistrySyncDone;
        return events;
      }();
      if (pw_core_add_listener(core, &coreHook, &coreEvents, this) < 0)
        throw std::runtime_error("cannot listen for PipeWire core events");
      coreListenerAttached = true;
      static const auto registryEvents = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = global;
        events.global_remove = globalRemove;
        return events;
      }();
      if (pw_registry_add_listener(registry, &registryHook, &registryEvents,
                                   this) < 0)
        throw std::runtime_error("cannot listen for playback graph ports");
      registryListenerAttached = true;
      createFilters();
      // Wait until the registry has delivered its initial globals before
      // creating the public Audio/Sink adapter. In particular, the
      // WirePlumber client must be known before frontendBound decides whether
      // to defer our monitor links for policy PortConfig.
      initialRegistrySyncSequence = pw_core_sync(core, PW_ID_CORE, 0);
      if (initialRegistrySyncSequence < 0)
        throw std::runtime_error(
            "cannot synchronize the initial PipeWire registry");
    } catch (...) {
      destroyResources();
      throw;
    }
    // Own output ports arrive through registry globals after the listener has
    // been attached. Schedule a bounded retry as some PipeWire versions emit
    // the initial globals before this object subscribes.
    for (unsigned channel = 0; channel < channels; ++channel)
      createLink(channel);
  }

  ~Impl() {
    destroyResources();
  }

  void destroyLink(unsigned channel) noexcept {
    if (channel >= MaxChannels)
      return;
    if (proxyListenerAttached[channel]) {
      spa_hook_remove(&proxyHooks[channel]);
      proxyListenerAttached[channel] = false;
    }
    if (linkListenerAttached[channel]) {
      spa_hook_remove(&linkHooks[channel]);
      linkListenerAttached[channel] = false;
    }
    if (links[channel]) {
      pw_proxy_destroy(links[channel]);
      links[channel] = nullptr;
    }
    linkGlobalIds[channel] = SPA_ID_INVALID;
    internalLinkExists[channel] = false;
    internalLinked[channel] = false;
  }

  void destroySinkLink(unsigned channel) noexcept {
    if (channel >= MaxChannels)
      return;
    if (sinkProxyListenerAttached[channel]) {
      spa_hook_remove(&sinkProxyHooks[channel]);
      sinkProxyListenerAttached[channel] = false;
    }
    if (sinkLinkListenerAttached[channel]) {
      spa_hook_remove(&sinkLinkHooks[channel]);
      sinkLinkListenerAttached[channel] = false;
    }
    if (sinkLinks[channel]) {
      pw_proxy_destroy(sinkLinks[channel]);
      sinkLinks[channel] = nullptr;
    }
    sinkLinkGlobalIds[channel] = SPA_ID_INVALID;
    linked[channel] = false;
  }

  void selectSink(AudioDevice selectedSink,
                  std::vector<AudioPort> selectedPorts) {
    std::sort(selectedPorts.begin(), selectedPorts.end(),
              [](const auto &left, const auto &right) {
                const auto leftPosition = std::find(
                    ChannelOrder.begin(), ChannelOrder.end(), left.channel);
                const auto rightPosition = std::find(
                    ChannelOrder.begin(), ChannelOrder.end(), right.channel);
                return leftPosition < rightPosition;
              });
    if (selectedPorts.size() != channels)
      throw std::runtime_error(
          "selected playback sink channel count differs from the active graph; "
          "restart skyapod to change playback channel layout");
    for (unsigned channel = 0; channel < channels; ++channel) {
      if (selectedPorts[channel].direction != "in" ||
          selectedPorts[channel].channel != ports[channel].channel)
        throw std::runtime_error(
            "selected playback sink channel layout differs from the active "
            "graph; restart skyapod to change playback channel layout");
    }

    auto previousSink = sink;
    auto previousPorts = ports;
    const bool previousSinkAvailable = sinkAvailable;
    for (unsigned channel = 0; channel < channels; ++channel)
      destroySinkLink(channel);
    sink = std::move(selectedSink);
    ports = std::move(selectedPorts);
    sinkAvailable = true;
    error.clear();
    sinkLinkGlobalIds.fill(SPA_ID_INVALID);
    linked.fill(false);
    try {
      recreateStreamFilter();
    } catch (...) {
      const auto switchFailure = std::current_exception();
      sink = std::move(previousSink);
      ports = std::move(previousPorts);
      sinkAvailable = previousSinkAvailable;
      sinkLinkGlobalIds.fill(SPA_ID_INVALID);
      linked.fill(false);
      try {
        // Restore the previous stream and its explicit target links.
        recreateStreamFilter();
        for (unsigned channel = 0; channel < channels; ++channel)
          ports[channel].node = sink.id;
        error.clear();
      } catch (const std::exception &rollbackFailure) {
        error = std::string("playback target switch and rollback failed: ") +
                rollbackFailure.what();
        throw std::runtime_error(error);
      } catch (...) {
        error = "playback target switch and rollback failed with an unknown "
                "error";
        throw std::runtime_error(error);
      }
      std::rethrow_exception(switchFailure);
    }
    for (unsigned channel = 0; channel < channels; ++channel)
      ports[channel].node = sink.id;
  }

  void destroyResources() noexcept {
    destroying = true;
    destroyFrontend();
    if (streamFilter) {
      pw_filter_disconnect(streamFilter);
    }
    if (filter) {
      pw_filter_disconnect(filter);
    }
    while (callbackGate.callbacksInFlight() != 0)
      std::this_thread::yield();
    // These hooks are stored on this Impl, but their list links point into the
    // filter proxies. Unlink them while those proxies are still alive.
    if (streamFilterListenerAttached) {
      spa_hook_remove(&streamFilterHook);
      streamFilterListenerAttached = false;
    }
    if (filterListenerAttached) {
      spa_hook_remove(&filterHook);
      filterListenerAttached = false;
    }
    if (streamFilter) {
      pw_filter_destroy(streamFilter);
      streamFilter = nullptr;
    }
    if (filter) {
      pw_filter_destroy(filter);
      filter = nullptr;
    }
    for (auto &binding : sinkBindings) {
      spa_hook_remove(&binding->hook);
      if (binding->node)
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(binding->node));
    }
    for (unsigned channel = 0; channel < channels; ++channel) {
      destroyLink(channel);
      destroySinkLink(channel);
    }
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    for (auto *&link : testPreexistingSinkLinks) {
      if (link)
        pw_proxy_destroy(link);
      link = nullptr;
    }
#endif
    if (registryListenerAttached) {
      spa_hook_remove(&registryHook);
      registryListenerAttached = false;
    }
    if (coreListenerAttached) {
      spa_hook_remove(&coreHook);
      coreListenerAttached = false;
    }
    if (formatEvent)
      pw_loop_destroy_source(loop, formatEvent);
    if (linkRetryTimer)
      pw_loop_destroy_source(loop, linkRetryTimer);
    if (frontendLinkStartTimer)
      pw_loop_destroy_source(loop, frontendLinkStartTimer);
  }

  void scheduleLinkRetry() {
    if (destroying || !linkRetryTimer || linkRetryScheduled)
      return;
    const unsigned delayMs = linkRetryBackoffMs;
    linkRetryBackoffMs = std::min(linkRetryBackoffMs * 2, 5000u);
    timespec delay{static_cast<time_t>(delayMs / 1000),
                   static_cast<long>(delayMs % 1000) * 1000000L};
    if (pw_loop_update_timer(loop, linkRetryTimer, &delay, nullptr, false) <
        0) {
      error = "cannot schedule playback link retry";
      return;
    }
    linkRetryScheduled = true;
  }

  void setInternalLinkError(std::string message) {
    error = std::move(message);
    lastInternalLinkError = error;
  }

  void setSinkLinkError(std::string message) {
    error = std::move(message);
    lastSinkLinkError = error;
  }

  void resetLinkRetryBackoffIfRecovered() {
    const bool internalReady =
        std::all_of(internalLinked.begin(), internalLinked.begin() + channels,
                    [](bool value) { return value; });
    const bool sinkReady =
        std::all_of(linked.begin(), linked.begin() + channels,
                    [](bool value) { return value; });
    if (internalReady && sinkReady &&
        !frontendFaulted.load(std::memory_order_acquire))
      linkRetryBackoffMs = 100;
  }

  void clearSinkLinkErrorIfRecovered() {
    if (!std::all_of(linked.begin(), linked.begin() + channels,
                     [](bool value) { return value; }))
      return;
    if (!lastSinkLinkError.empty() && error == lastSinkLinkError)
      error.clear();
    lastSinkLinkError.clear();
    resetLinkRetryBackoffIfRecovered();
  }

  void clearInternalLinkErrorIfRecovered() {
    if (!std::all_of(internalLinked.begin(), internalLinked.begin() + channels,
                     [](bool value) { return value; }))
      return;
    if (!lastInternalLinkError.empty() && error == lastInternalLinkError)
      error.clear();
    lastInternalLinkError.clear();
    resetLinkRetryBackoffIfRecovered();
  }

  static void retryLinks(void *data, uint64_t) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          self.linkRetryScheduled = false;
          if (self.frontendRecreateNeeded) {
            self.destroyFrontend();
            try {
              self.createFrontend(self.frontendPositions);
              self.frontendRecreateNeeded = false;
            } catch (const std::exception &exception) {
              self.lastFrontendLinkError = self.error = exception.what();
              self.scheduleLinkRetry();
            }
          }
          for (unsigned channel = 0; channel < self.channels; ++channel)
            self.createFrontendLink(channel);
          for (unsigned channel = 0; channel < self.channels; ++channel)
            if (!self.links[channel] &&
                self.ownOutputIds[channel] != SPA_ID_INVALID &&
                self.streamInputIds[channel] != SPA_ID_INVALID)
              self.createLink(channel);
          for (unsigned channel = 0; channel < self.channels; ++channel)
            if (!self.sinkLinks[channel] && self.sinkAvailable &&
                self.sink.id != SPA_ID_INVALID &&
                self.streamOutputIds[channel] != SPA_ID_INVALID &&
                self.ports[channel].id != SPA_ID_INVALID)
              self.createSinkLink(channel);
          self.resetLinkRetryBackoffIfRecovered();
        },
        self.controlCallbackFailed, self.faulted);
  }

  void createFilters() {
    const auto &positions = frontendPositions;
    // Only the native adapter is exposed to session policy. Fixed DSP
    // filters use an internal class so WirePlumber never sends them PortConfig.
    auto *props = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.playback.dsp", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO Playback DSP", PW_KEY_MEDIA_CLASS, "Filter/Audio",
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_NODE_AUTOCONNECT, "false",
        PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_NODE_PAUSE_ON_IDLE, "false",
        PW_KEY_AUDIO_CHANNELS, std::to_string(channels).c_str(),
        PW_KEY_AUDIO_RATE,
        std::to_string(sink.sampleRate ? sink.sampleRate : 48000).c_str(),
        PW_KEY_AUDIO_FORMAT, "F32", SPA_KEY_AUDIO_POSITION, positions.c_str(),
        nullptr);
    if (!props)
      throw std::runtime_error("cannot allocate playback node properties");
    filter = pw_filter_new(core, "SkyAPO Playback", props);
    if (!filter)
      throw std::runtime_error("cannot create SkyAPO playback filter");
    static const auto events = [] {
      pw_filter_events value{};
      value.version = PW_VERSION_FILTER_EVENTS;
      value.state_changed = stateChanged;
      value.param_changed = paramChanged;
      value.process = process;
      return value;
    }();
    pw_filter_add_listener(filter, &filterHook, &events, this);
    filterListenerAttached = true;
    auto *streamProps = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.playback.stream", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO Playback Stream", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_MEDIA_CLASS, "Filter/Audio", "node.virtual", "true",
        PW_KEY_NODE_AUTOCONNECT, "false", "node.dont-fallback", "true",
        "node.linger", "true", nullptr);
    if (!streamProps)
      throw std::runtime_error("cannot allocate playback stream properties");
    streamFilter = pw_filter_new(core, "SkyAPO Playback Stream", streamProps);
    if (!streamFilter)
      throw std::runtime_error("cannot create SkyAPO playback stream");
    static const auto streamEvents = [] {
      pw_filter_events value{};
      value.version = PW_VERSION_FILTER_EVENTS;
      value.state_changed = streamStateChanged;
      value.param_changed = streamParamChanged;
      value.process = streamProcess;
      return value;
    }();
    pw_filter_add_listener(streamFilter, &streamFilterHook, &streamEvents,
                           this);
    streamFilterListenerAttached = true;
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto inputName = "playback_" + ports[channel].channel;
      const auto outputName = "to_device_" + ports[channel].channel;
      inputs[channel] = pw_filter_add_port(
          filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, inputName.c_str(),
                            PW_KEY_AUDIO_CHANNEL,
                            ports[channel].channel.c_str(), nullptr),
          nullptr, 0);
      outputs[channel] = pw_filter_add_port(
          filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, outputName.c_str(),
                            PW_KEY_AUDIO_CHANNEL,
                            ports[channel].channel.c_str(), nullptr),
          nullptr, 0);
      if (!inputs[channel] || !outputs[channel])
        throw std::runtime_error("cannot create SkyAPO playback DSP ports");
      const auto streamInputName = "from_dsp_" + ports[channel].channel;
      const auto streamOutputName = "playback_" + ports[channel].channel;
      streamInputs[channel] = pw_filter_add_port(
          streamFilter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, streamInputName.c_str(),
                            PW_KEY_AUDIO_CHANNEL,
                            ports[channel].channel.c_str(), nullptr),
          nullptr, 0);
      streamOutputs[channel] = pw_filter_add_port(
          streamFilter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, streamOutputName.c_str(),
                            PW_KEY_AUDIO_CHANNEL,
                            ports[channel].channel.c_str(), nullptr),
          nullptr, 0);
      if (!streamInputs[channel] || !streamOutputs[channel])
        throw std::runtime_error("cannot create playback stream DSP ports");
    }
    // Unlike the processing ports, the device node itself must advertise raw
    // audio formats for session managers (notably WirePlumber's AudioAdapter)
    // to treat an Audio/Sink node as a usable device. Per-port format.dsp only
    // describes the internal pw_filter graph and is not node EnumFormat.
    spa_audio_info_raw nodeFormat{};
    nodeFormat.format = SPA_AUDIO_FORMAT_F32P;
    nodeFormat.rate = sink.sampleRate ? sink.sampleRate : 48000;
    nodeFormat.channels = channels;
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto position = std::find_if(
          SpaChannels.begin(), SpaChannels.end(), [&](const auto &candidate) {
            return ports[channel].channel == candidate.first;
          });
      if (position == SpaChannels.end())
        throw std::runtime_error("unsupported playback channel position " +
                                 ports[channel].channel);
      nodeFormat.position[channel] = position->second;
    }
    uint8_t formatBytes[1024];
    spa_pod_builder formatBuilder =
        SPA_POD_BUILDER_INIT(formatBytes, sizeof(formatBytes));
    std::array<float, MaxChannels> unityVolumes{};
    unityVolumes.fill(1.0f);
    const spa_pod *nodeParams[] = {
        spa_format_audio_raw_build(&formatBuilder, SPA_PARAM_EnumFormat,
                                   &nodeFormat),
        spa_format_audio_raw_build(&formatBuilder, SPA_PARAM_Format,
                                   &nodeFormat),
        static_cast<spa_pod *>(spa_pod_builder_add_object(
            &formatBuilder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_volume, SPA_POD_Float(1.0f), SPA_PROP_mute,
            SPA_POD_Bool(false), SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                          unityVolumes.data())))};
    if (!nodeParams[0] || !nodeParams[1] || !nodeParams[2] ||
        pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nodeParams, 3) <
            0 ||
        pw_filter_connect(streamFilter, PW_FILTER_FLAG_RT_PROCESS, nodeParams,
                          3) < 0)
      throw std::runtime_error("cannot connect SkyAPO playback filter");
  }

  static void initialRegistrySyncDone(void *data, uint32_t id,
                                      int sequence) noexcept {
    auto &self = *static_cast<Impl *>(data);
    if (id != PW_ID_CORE || sequence != self.initialRegistrySyncSequence ||
        self.initialFrontendCreated || self.destroying)
      return;
    detail::invokeControlCallback(
        [&] {
          self.initialRegistrySyncSequence = -1;
          if (self.sessionManagerPresent) {
            self.createFrontend(self.frontendPositions);
            self.initialFrontendCreated = true;
            std::cerr << "skyapod: initial PipeWire registry synchronized; "
                      << "session manager detected before playback adapter "
                         "creation\n";
          } else {
            // WirePlumber may start slightly after PipeWire and register only
            // after the initial registry barrier. Give its client global a
            // bounded chance to arrive before exposing the Audio/Sink; its
            // PortConfig must settle before we create monitor links.
            timespec delay{0, 500000000};
            if (pw_loop_update_timer(self.loop, self.frontendLinkStartTimer,
                                     &delay, nullptr, false) < 0)
              throw std::runtime_error(
                  "cannot schedule initial session-manager discovery grace "
                  "period");
          }
        },
        self.controlCallbackFailed, self.faulted);
  }

  void destroyFrontend() noexcept {
    frontendLinksEnabled = false;
    if (frontendListenerAttached) {
      spa_hook_remove(&frontendHook);
      frontendListenerAttached = false;
    }
    if (frontendProxyListenerAttached) {
      spa_hook_remove(&frontendProxyHook);
      frontendProxyListenerAttached = false;
    }
    for (unsigned channel = 0; channel < channels; ++channel)
      destroyFrontendLink(channel);
    if (frontend) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(frontend));
      frontend = nullptr;
    }
    frontendId = SPA_ID_INVALID;
    frontendMonitorIds.fill(SPA_ID_INVALID);
    frontendFaulted.store(true, std::memory_order_release);
  }

  void createFrontend(const std::string &positions) {
    frontendPositions = positions;
    frontendLinksEnabled = false;
    frontendControlsRestored = false;
    frontendRestoreScalar = frontendScalarMetadata;
    frontendRestoreMuted = sinkMuted.load(std::memory_order_acquire);
    for (unsigned channel = 0; channel < channels; ++channel)
      frontendRestoreVolumes[channel] =
          sinkVolumes[channel].load(std::memory_order_acquire);
    auto *properties = pw_properties_new(
        "factory.name", "support.null-audio-sink", PW_KEY_NODE_NAME,
        "skyapo.playback", PW_KEY_NODE_DESCRIPTION, "SkyAPO Playback",
        PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_VIRTUAL, "true",
        PW_KEY_AUDIO_CHANNELS, std::to_string(channels).c_str(),
        SPA_KEY_AUDIO_POSITION, positions.c_str(), "monitor.channel-volumes",
        "false", "adapter.auto-port-config",
        "{ mode = dsp monitor = true position = preserve }", nullptr);
    if (!properties)
      throw std::runtime_error("cannot allocate playback adapter properties");
    frontend = static_cast<pw_node *>(
        pw_core_create_object(core, "adapter", PW_TYPE_INTERFACE_Node,
                              PW_VERSION_NODE, &properties->dict, 0));
    pw_properties_free(properties);
    if (!frontend)
      throw std::runtime_error("cannot create playback adapter");
    static const auto events = [] {
      pw_node_events value{};
      value.version = PW_VERSION_NODE_EVENTS;
      value.param = frontendParam;
      return value;
    }();
    if (pw_node_add_listener(frontend, &frontendHook, &events, this) < 0)
      throw std::runtime_error("cannot listen for playback adapter controls");
    frontendListenerAttached = true;
    static const auto proxyEvents = [] {
      pw_proxy_events value{};
      value.version = PW_VERSION_PROXY_EVENTS;
      value.error = frontendError;
      value.bound = frontendBound;
      value.removed = frontendRemoved;
      return value;
    }();
    pw_proxy_add_listener(reinterpret_cast<pw_proxy *>(frontend),
                          &frontendProxyHook, &proxyEvents, this);
    frontendProxyListenerAttached = true;
    uint32_t params[] = {SPA_PARAM_Props};
    if (pw_node_subscribe_params(frontend, params, 1) < 0)
      throw std::runtime_error("cannot subscribe to playback adapter controls");
    // Restore the public controls when recreating our own adapter. DSP gain
    // stays after the Engine; the monitor itself deliberately remains unity.
    uint8_t bytes[512];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    const auto volumes = frontendRestoreVolumes;
    const auto *controls = static_cast<spa_pod *>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
        SPA_POD_Float(frontendRestoreScalar), SPA_PROP_channelVolumes,
        SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels, volumes.data()),
        SPA_PROP_mute,
        SPA_POD_Bool(sinkMuted.load(std::memory_order_acquire))));
    if (!controls ||
        pw_node_set_param(frontend, SPA_PARAM_Props, 0, controls) < 0)
      throw std::runtime_error("cannot restore playback adapter controls");
  }

  static void frontendBound(void *data, uint32_t id) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          self.frontendId = id;
          for (const auto &port : self.knownOutputPorts)
            if (port.node == id)
              for (unsigned channel = 0; channel < self.channels; ++channel)
                if (port.channel == self.ports[channel].channel)
                  self.frontendMonitorIds[channel] = port.id;
          if (self.sessionManagerPresent) {
            // WirePlumber 0.4 configures Audio/Sink adapters immediately after
            // discovery. Starting this adapter through monitor links before
            // that transaction completes makes Format/PortConfig fail with
            // EIO. Let policy configure it before starting our graph.
            timespec delay{0, 500000000};
            pw_loop_update_timer(self.loop, self.frontendLinkStartTimer, &delay,
                                 nullptr, false);
          } else {
            self.enableFrontendLinks();
          }
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void frontendRemoved(void *data) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          self.frontendFaulted.store(true, std::memory_order_release);
          self.frontendRecreateNeeded = true;
          self.lastFrontendLinkError = self.error =
              "playback adapter disappeared";
          self.scheduleLinkRetry();
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void frontendError(void *data, int, int,
                            const char *message) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          self.lastFrontendLinkError = self.error =
              message ? message : "playback adapter failed";
          self.frontendFaulted.store(true, std::memory_order_release);
          self.frontendRecreateNeeded = true;
          self.scheduleLinkRetry();
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void frontendParam(void *data, int, uint32_t id, uint32_t, uint32_t,
                            const spa_pod *param) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          if (id == SPA_PARAM_Props && param)
            self.applyFrontendProps(param);
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void startFrontendLinks(void *data, uint64_t) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          if (!self.initialFrontendCreated) {
            self.createFrontend(self.frontendPositions);
            self.initialFrontendCreated = true;
            std::cerr << "skyapod: created playback adapter after the "
                      << "initial session-manager discovery grace period; "
                      << "session manager "
                      << (self.sessionManagerPresent ? "detected"
                                                     : "not detected")
                      << '\n';
            return;
          }
          self.frontendLinksEnabled = true;
          for (unsigned channel = 0; channel < self.channels; ++channel)
            self.createFrontendLink(channel);
        },
        self.controlCallbackFailed, self.faulted);
  }

  void destroyFrontendLink(unsigned channel) noexcept {
    auto &link = frontendLinks[channel];
    if (link.linkListenerAttached)
      spa_hook_remove(&link.linkHook);
    if (link.proxyListenerAttached)
      spa_hook_remove(&link.proxyHook);
    link.linkListenerAttached = link.proxyListenerAttached = false;
    if (link.proxy)
      pw_proxy_destroy(link.proxy);
    link.proxy = nullptr;
    link.globalId = SPA_ID_INVALID;
    link.active = false;
    frontendFaulted.store(true, std::memory_order_release);
  }

  void createFrontendLink(unsigned channel) {
    auto &link = frontendLinks[channel];
    if (destroying || !frontendLinksEnabled || link.proxy ||
        frontendId == SPA_ID_INVALID ||
        frontendMonitorIds[channel] == SPA_ID_INVALID ||
        dspInputIds[channel] == SPA_ID_INVALID)
      return;
    auto *properties = pw_properties_new(nullptr, nullptr);
    if (!properties) {
      lastFrontendLinkError = error =
          "cannot allocate playback adapter link properties";
      scheduleLinkRetry();
      return;
    }
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", frontendId);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       frontendMonitorIds[channel]);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u",
                       pw_filter_get_node_id(filter));
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       dspInputIds[channel]);
    link.proxy = static_cast<pw_proxy *>(
        pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                              PW_VERSION_LINK, &properties->dict, 0));
    pw_properties_free(properties);
    if (!link.proxy) {
      lastFrontendLinkError = error = "cannot link playback adapter to DSP";
      scheduleLinkRetry();
      return;
    }
    static const auto proxyEvents = [] {
      pw_proxy_events value{};
      value.version = PW_VERSION_PROXY_EVENTS;
      value.error = frontendLinkError;
      return value;
    }();
    pw_proxy_add_listener(link.proxy, &link.proxyHook, &proxyEvents, &link);
    link.proxyListenerAttached = true;
    static const auto events = [] {
      pw_link_events value{};
      value.version = PW_VERSION_LINK_EVENTS;
      value.info = frontendLinkInfo;
      return value;
    }();
    if (pw_link_add_listener(reinterpret_cast<pw_link *>(link.proxy),
                             &link.linkHook, &events, &link) < 0) {
      destroyFrontendLink(channel);
      lastFrontendLinkError = error =
          "cannot listen for playback adapter link state";
      scheduleLinkRetry();
      return;
    }
    link.linkListenerAttached = true;
  }

  static void frontendLinkError(void *data, int, int,
                                const char *message) noexcept {
    auto &link = *static_cast<FrontendLink *>(data);
    auto &self = *link.owner;
    detail::invokeControlCallback(
        [&] {
          self.lastFrontendLinkError = self.error =
              message ? message : "playback adapter link failed";
          self.destroyFrontendLink(link.channel);
          self.scheduleLinkRetry();
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void frontendLinkInfo(void *data, const pw_link_info *info) noexcept {
    auto &link = *static_cast<FrontendLink *>(data);
    auto &self = *link.owner;
    detail::invokeControlCallback(
        [&] {
          if (!info)
            return;
          link.globalId = info->id;
          link.active = info->state == PW_LINK_STATE_ACTIVE;
          if (info->state == PW_LINK_STATE_ERROR) {
            self.lastFrontendLinkError = self.error =
                info->error ? info->error : "playback adapter link failed";
            self.destroyFrontendLink(link.channel);
            self.scheduleLinkRetry();
          } else {
            self.updateFrontendRecoveryGate();
          }
        },
        self.controlCallbackFailed, self.faulted);
  }

  void updateFrontendRecoveryGate() {
    const bool ready =
        frontendControlsRestored &&
        std::all_of(frontendLinks.begin(), frontendLinks.begin() + channels,
                    [](const auto &link) { return link.active; });
    frontendFaulted.store(!ready, std::memory_order_release);
    if (ready) {
      if (error == lastFrontendLinkError)
        error.clear();
      lastFrontendLinkError.clear();
    }
  }

  void recreateStreamFilter() {
    const auto oldStreamNode =
        streamFilter ? pw_filter_get_node_id(streamFilter) : SPA_ID_INVALID;
    if (streamFilter)
      pw_filter_disconnect(streamFilter);
    // Disconnect tears down the node, while listener removal is the explicit
    // synchronization point with the data loop. Do this before changing any
    // port pointers read by its realtime callback.
    if (streamFilterListenerAttached) {
      spa_hook_remove(&streamFilterHook);
      streamFilterListenerAttached = false;
    }
    while (callbackGate.callbacksInFlight() != 0)
      std::this_thread::yield();
    for (unsigned channel = 0; channel < channels; ++channel) {
      destroyLink(channel);
      destroySinkLink(channel);
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
      if (testPreexistingSinkLinks[channel]) {
        pw_proxy_destroy(testPreexistingSinkLinks[channel]);
        testPreexistingSinkLinks[channel] = nullptr;
      }
      testSinkLinkAlreadyExistsInjected[channel] = false;
#endif
      linked[channel] = false;
      streamInputs[channel] = nullptr;
      streamOutputs[channel] = nullptr;
      streamInputIds[channel] = SPA_ID_INVALID;
      streamOutputIds[channel] = SPA_ID_INVALID;
    }
    if (streamFilter) {
      pw_filter_destroy(streamFilter);
      streamFilter = nullptr;
    }
    clearObservedSinkLinks(oldStreamNode);
    streamState = PW_FILTER_STATE_UNCONNECTED;

    auto *streamProps = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.playback.stream", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO Playback Stream", PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_MEDIA_CLASS, "Filter/Audio", "node.virtual", "true",
        PW_KEY_NODE_AUTOCONNECT, "false", "node.dont-fallback", "true",
        "node.linger", "true", nullptr);
    if (!streamProps)
      throw std::runtime_error("cannot allocate playback stream properties");
    streamFilter = pw_filter_new(core, "SkyAPO Playback Stream", streamProps);
    if (!streamFilter)
      throw std::runtime_error("cannot create SkyAPO playback stream");
    static const auto streamEvents = [] {
      pw_filter_events value{};
      value.version = PW_VERSION_FILTER_EVENTS;
      value.state_changed = streamStateChanged;
      value.param_changed = streamParamChanged;
      value.process = streamProcess;
      return value;
    }();
    pw_filter_add_listener(streamFilter, &streamFilterHook, &streamEvents,
                           this);
    streamFilterListenerAttached = true;
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto inputName = "from_dsp_" + ports[channel].channel;
      const auto outputName = "playback_" + ports[channel].channel;
      streamInputs[channel] = pw_filter_add_port(
          streamFilter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, inputName.c_str(),
                            PW_KEY_AUDIO_CHANNEL,
                            ports[channel].channel.c_str(), nullptr),
          nullptr, 0);
      streamOutputs[channel] = pw_filter_add_port(
          streamFilter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, outputName.c_str(),
                            PW_KEY_AUDIO_CHANNEL,
                            ports[channel].channel.c_str(), nullptr),
          nullptr, 0);
      if (!streamInputs[channel] || !streamOutputs[channel])
        throw std::runtime_error("cannot create playback stream DSP ports");
    }

    spa_audio_info_raw nodeFormat{};
    nodeFormat.format = SPA_AUDIO_FORMAT_F32P;
    nodeFormat.rate = sink.sampleRate ? sink.sampleRate : 48000;
    nodeFormat.channels = channels;
    for (unsigned channel = 0; channel < channels; ++channel) {
      const auto position = std::find_if(
          SpaChannels.begin(), SpaChannels.end(), [&](const auto &candidate) {
            return ports[channel].channel == candidate.first;
          });
      if (position == SpaChannels.end())
        throw std::runtime_error("unsupported playback channel position " +
                                 ports[channel].channel);
      nodeFormat.position[channel] = position->second;
    }
    uint8_t formatBytes[1024];
    spa_pod_builder formatBuilder =
        SPA_POD_BUILDER_INIT(formatBytes, sizeof(formatBytes));
    std::array<float, MaxChannels> unityVolumes{};
    unityVolumes.fill(1.0f);
    const spa_pod *params[] = {
        spa_format_audio_raw_build(&formatBuilder, SPA_PARAM_EnumFormat,
                                   &nodeFormat),
        spa_format_audio_raw_build(&formatBuilder, SPA_PARAM_Format,
                                   &nodeFormat),
        static_cast<spa_pod *>(spa_pod_builder_add_object(
            &formatBuilder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_volume, SPA_POD_Float(1.0f), SPA_PROP_mute,
            SPA_POD_Bool(false), SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                          unityVolumes.data())))};
    if (!params[0] || !params[1] || !params[2] ||
        pw_filter_connect(streamFilter, PW_FILTER_FLAG_RT_PROCESS, params, 3) <
            0)
      throw std::runtime_error("cannot reconnect SkyAPO playback stream");
  }

  detail::PlaybackFaultRecovery::ConfigReload
  buildEngine(unsigned sampleRate, unsigned maxFrames, bool force = false,
              std::shared_ptr<const ConfigSourceSnapshot> sourceSnapshot = {}) {
    if (sampleRate < 8000 || sampleRate > 384000 || !maxFrames ||
        maxFrames > MaxFrames)
      throw std::runtime_error("unsupported playback graph format");
    auto *current = active.load(std::memory_order_acquire);
    if (!force && current && current->sampleRate() == sampleRate &&
        current->supportsFrameCount(maxFrames))
      return detail::PlaybackFaultRecovery::beginConfigReload(false, false);
    const auto createEngine = [&](const auto &sources) {
      auto engine = std::make_unique<Engine>(
          sampleRate, channels, maxFrames, channelNames(ports), true, true,
          deviceMatchText(sink),
          StringHelper::toWString(sink.description, 65001),
          StringHelper::toWString("skyapo.playback", 65001), L"post-mix");
      if (sources)
        engine->loadConfig(configPath, sources);
      else
        engine->loadConfig(configPath, true);
      return engine;
    };
    auto candidate = createEngine(sourceSnapshot);
    auto acceptedSources = candidate->configSourceSnapshot();
    for (const auto &notice : candidate->configNotices())
      std::cerr << "skyapod: playback config warning " << notice.file.string()
                << ':' << notice.line << ": " << notice.message << '\n';
    if (pending.load(std::memory_order_seq_cst))
      throw std::runtime_error("playback config transition is already active");

    // Serialize an explicit config commit against the realtime callback. A
    // callback may discover an unsafe dynamic plugin latency while the
    // replacement Engine is being built. Close and drain only at this commit
    // boundary, then inspect the fault latch and validate the candidate before
    // publishing it. This keeps a late fault from being mistaken for a
    // previously healthy crossfade.
    if (force) {
      callbackGate.closeForRecovery();
      if (!waitForCallbacksToDrain())
        throw std::runtime_error(
            "timed out draining playback callbacks before config commit");
      reclaim();
      current = active.load(std::memory_order_acquire);
      if (pending.load(std::memory_order_seq_cst))
        throw std::runtime_error(
            "playback config transition completed while preparing reload");
    }
    const auto recovery = detail::PlaybackFaultRecovery::beginConfigReload(
        force, faulted.load(std::memory_order_acquire));
    const bool replacingFaultedEngine = recovery.validatedForRecovery;
    if (replacingFaultedEngine)
      candidate->validateForActivation();
    if (current && !replacingFaultedEngine &&
        current->sampleRate() == sampleRate &&
        current->channels() == channels &&
        current->supportsFrameCount(maxFrames)) {
      retiredOwners.reserve(retiredOwners.size() + 1);
      transitionCounter = 0;
      transitionLength =
          std::max(1u, sampleRate * transitionDurationMs() / 1000);
      pendingOwner = std::move(candidate);
      pending.store(pendingOwner.get(), std::memory_order_seq_cst);
    } else {
      if (activeOwner) {
        retiredOwners.reserve(retiredOwners.size() + 1);
        retiredOwners.push_back(std::move(activeOwner));
      }
      activeOwner = std::move(candidate);
      active.store(activeOwner.get(), std::memory_order_seq_cst);
      transitionCounter = 0;
      transitionLength = 0;
    }
    requestedRate.store(sampleRate, std::memory_order_release);
    requestedQuantum.store(maxFrames, std::memory_order_release);
    rate.store(sampleRate, std::memory_order_relaxed);
    quantum.store(maxFrames, std::memory_order_relaxed);
    reclaim();
    acceptedConfigSources = std::move(acceptedSources);
    std::cerr << "skyapod: playback DSP ready at " << sampleRate << " Hz, "
              << channels << " channels, " << candidateFilterCount()
              << " filters\n";
    return recovery;
  }

  unsigned candidateFilterCount() const noexcept {
    auto *engine = pending.load(std::memory_order_acquire);
    if (!engine)
      engine = active.load(std::memory_order_acquire);
    return engine ? engine->filterCount() : 0;
  }

  void reclaim() {
    if (transitionComplete.exchange(false, std::memory_order_acq_rel) &&
        pendingOwner) {
      if (activeOwner)
        retiredOwners.push_back(std::move(activeOwner));
      activeOwner = std::move(pendingOwner);
      transitionCounter = 0;
      transitionLength = 0;
    }
    if (callbackGate.callbacksInFlight() == 0)
      retiredOwners.clear();
  }

  bool waitForCallbacksToDrain() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (callbackGate.callbacksInFlight() != 0) {
      if (std::chrono::steady_clock::now() >= deadline)
        return false;
      std::this_thread::yield();
    }
    return true;
  }

  void refreshPluginLatency() {
    if (!pluginLatencyFaulted.load(std::memory_order_acquire))
      return;
    if (pending.load(std::memory_order_seq_cst)) {
      if (!waitForCallbacksToDrain()) {
        error = "timed out draining playback callbacks after plugin latency "
                "changed during graph transition";
        return;
      }
      reclaim();
      if (pending.load(std::memory_order_seq_cst)) {
        // The candidate was never proven safe with its newly reported
        // latency. Drop it off the audio thread; remain muted until an
        // explicit configuration reload builds and validates a replacement.
        pending.store(nullptr, std::memory_order_seq_cst);
        pendingOwner.reset();
        transitionCounter = 0;
        transitionLength = 0;
        transitionComplete.store(false, std::memory_order_release);
        error = "playback plugin latency changed during graph transition; "
                "candidate discarded and output remains silent until config "
                "reload";
        return;
      }
    }
    if (!waitForCallbacksToDrain()) {
      error =
          "timed out draining playback callbacks for plugin latency refresh";
      return;
    }
    reclaim();
    auto *engine = active.load(std::memory_order_seq_cst);
    if (!engine || !engine->pluginLatencyRefreshPending()) {
      error = "playback Engine changed before plugin latency refresh";
      return;
    }
    try {
      const unsigned refreshed = engine->refreshPluginLatencies();
      if (!refreshed || engine->pluginLatencyRefreshPending())
        throw std::runtime_error(
            "playback plugin latency refresh did not validate a new PDC plan");
      pluginLatencyRefreshes += refreshed;
      pluginLatencyFaulted.store(false, std::memory_order_release);
      if (!controlCallbackFailed.load(std::memory_order_acquire))
        faulted.store(false, std::memory_order_release);
      error.clear();
      std::cerr << "skyapod: refreshed playback latency for " << refreshed
                << " plugin instance(s)\n";
    } catch (const std::exception &exception) {
      error = std::string("playback plugin latency refresh failed: ") +
              exception.what();
      faulted.store(true, std::memory_order_release);
      std::cerr << "skyapod: " << error << '\n';
    } catch (...) {
      error = "playback plugin latency refresh failed: unknown exception";
      faulted.store(true, std::memory_order_release);
      std::cerr << "skyapod: " << error << '\n';
    }
  }

  void createLink(unsigned channel) {
    if (channel >= channels || links[channel] ||
        ownOutputIds[channel] == SPA_ID_INVALID ||
        streamInputIds[channel] == SPA_ID_INVALID ||
        internalLinkExists[channel])
      return;
    auto *properties = pw_properties_new(nullptr, nullptr);
    if (!properties) {
      setInternalLinkError("cannot allocate playback-link properties");
      scheduleLinkRetry();
      return;
    }
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u",
                       pw_filter_get_node_id(filter));
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       ownOutputIds[channel]);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u",
                       pw_filter_get_node_id(streamFilter));
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       streamInputIds[channel]);
    void *createdLink = nullptr;
    bool rejectCreationForTesting = false;
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    const auto injectedFault = playbackLinkFaultForTesting();
    rejectCreationForTesting =
        injectedFault == PlaybackLinkFaultForTesting::reject;
    if (injectedFault == PlaybackLinkFaultForTesting::asynchronousError)
      pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                         SPA_ID_INVALID);
#endif
    if (!rejectCreationForTesting)
      createdLink =
          pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                                PW_VERSION_LINK, &properties->dict, 0);
    links[channel] = reinterpret_cast<pw_proxy *>(createdLink);
    pw_properties_free(properties);
    if (!links[channel]) {
      setInternalLinkError("cannot connect SkyAPO DSP to its policy stream");
      scheduleLinkRetry();
      return;
    }
    static const auto proxyEvents = [] {
      pw_proxy_events value{};
      value.version = PW_VERSION_PROXY_EVENTS;
      value.error = proxyError;
      return value;
    }();
    pw_proxy_add_listener(links[channel], &proxyHooks[channel], &proxyEvents,
                          &linkContexts[channel]);
    proxyListenerAttached[channel] = true;
    static const auto events = [] {
      pw_link_events value{};
      value.version = PW_VERSION_LINK_EVENTS;
      value.info = linkInfo;
      return value;
    }();
    if (pw_link_add_listener(reinterpret_cast<pw_link *>(links[channel]),
                             &linkHooks[channel], &events,
                             &linkContexts[channel]) < 0) {
      setInternalLinkError("cannot monitor SkyAPO internal playback link");
      destroyLink(channel);
      scheduleLinkRetry();
      return;
    }
    linkListenerAttached[channel] = true;
    internalLinkExists[channel] = true;
    std::cerr << "skyapod: linking playback DSP port " << ownOutputIds[channel]
              << " -> stream port " << streamInputIds[channel] << " ("
              << ports[channel].channel << ", link proxy "
              << pw_proxy_get_id(links[channel]) << ")\n";
  }

  void createSinkLink(unsigned channel) {
    if (channel >= channels || !sinkAvailable || sink.id == SPA_ID_INVALID ||
        streamOutputIds[channel] == SPA_ID_INVALID ||
        ports[channel].id == SPA_ID_INVALID)
      return;
    if (sinkLinks[channel])
      return;
    if (sinkLinkGlobalIds[channel] != SPA_ID_INVALID) {
      bindSinkLink(channel, sinkLinkGlobalIds[channel]);
      return;
    }
    const auto observedLink = findObservedSinkLink(channel);
    if (observedLink != SPA_ID_INVALID) {
      sinkLinkGlobalIds[channel] = observedLink;
      bindSinkLink(channel, observedLink);
      return;
    }
    auto *properties = pw_properties_new(nullptr, nullptr);
    if (!properties) {
      setSinkLinkError("cannot allocate playback sink-link properties");
      scheduleLinkRetry();
      return;
    }
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u",
                       pw_filter_get_node_id(streamFilter));
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       streamOutputIds[channel]);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", sink.id);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       ports[channel].id);
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
    const char *injectExistingLink =
        std::getenv("SKYAPO_TEST_PLAYBACK_LINK_ALREADY_EXISTS");
    if (injectExistingLink && std::strcmp(injectExistingLink, "1") == 0 &&
        !testSinkLinkAlreadyExistsInjected[channel]) {
      testSinkLinkAlreadyExistsInjected[channel] = true;
      testPreexistingSinkLinks[channel] = reinterpret_cast<pw_proxy *>(
          pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                                PW_VERSION_LINK, &properties->dict, 0));
      if (!testPreexistingSinkLinks[channel]) {
        pw_properties_free(properties);
        setSinkLinkError("test could not create the preexisting playback link");
        scheduleLinkRetry();
        return;
      }
    }
#endif
    auto *createdLink =
        pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                              PW_VERSION_LINK, &properties->dict, 0);
    sinkLinks[channel] = reinterpret_cast<pw_proxy *>(createdLink);
    pw_properties_free(properties);
    if (!sinkLinks[channel]) {
      setSinkLinkError(
          "cannot connect SkyAPO playback stream to selected sink");
      scheduleLinkRetry();
      return;
    }
    if (!attachSinkLinkListeners(channel))
      return;
    std::cerr << "skyapod: linking playback stream port "
              << streamOutputIds[channel] << " -> selected sink port "
              << ports[channel].id << " (" << ports[channel].channel
              << ", link proxy " << pw_proxy_get_id(sinkLinks[channel])
              << ")\n";
  }

  static bool parseGlobalId(const std::string &text, uint32_t &id) {
    if (text.empty())
      return false;
    char *end = nullptr;
    errno = 0;
    const auto parsed = std::strtoul(text.c_str(), &end, 10);
    if (errno || end == text.c_str() || !end || *end || parsed > UINT32_MAX)
      return false;
    id = static_cast<uint32_t>(parsed);
    return true;
  }

  void rememberSinkLink(const ObservedSinkLink &link) {
    const auto streamNode =
        streamFilter ? pw_filter_get_node_id(streamFilter) : SPA_ID_INVALID;
    if (link.outputNode != streamNode)
      return;
    for (size_t index = 0; index < observedSinkLinkCount; ++index) {
      if (observedSinkLinks[index].id == link.id) {
        observedSinkLinks[index] = link;
        return;
      }
    }
    if (observedSinkLinkCount == observedSinkLinks.size()) {
      std::cerr
          << "skyapod: ignoring excess links from SkyAPO playback stream\n";
      return;
    }
    observedSinkLinks[observedSinkLinkCount++] = link;
  }

  void removeObservedSinkLink(uint32_t id) {
    for (size_t index = 0; index < observedSinkLinkCount;) {
      if (observedSinkLinks[index].id != id) {
        ++index;
        continue;
      }
      observedSinkLinks[index] = observedSinkLinks[--observedSinkLinkCount];
      observedSinkLinks[observedSinkLinkCount] = {};
    }
  }

  void clearObservedSinkLinks(uint32_t outputNode) {
    if (outputNode == SPA_ID_INVALID)
      return;
    for (size_t index = 0; index < observedSinkLinkCount;) {
      if (observedSinkLinks[index].outputNode != outputNode) {
        ++index;
        continue;
      }
      observedSinkLinks[index] = observedSinkLinks[--observedSinkLinkCount];
      observedSinkLinks[observedSinkLinkCount] = {};
    }
  }

  uint32_t findObservedSinkLink(unsigned channel) const {
    if (channel >= channels || !streamFilter ||
        streamOutputIds[channel] == SPA_ID_INVALID ||
        sink.id == SPA_ID_INVALID || ports[channel].id == SPA_ID_INVALID)
      return SPA_ID_INVALID;
    const auto streamNode = pw_filter_get_node_id(streamFilter);
    for (size_t index = 0; index < observedSinkLinkCount; ++index) {
      const auto &link = observedSinkLinks[index];
      if (link.outputNode == streamNode &&
          link.outputPort == streamOutputIds[channel] &&
          link.inputNode == sink.id && link.inputPort == ports[channel].id)
        return link.id;
    }
    return SPA_ID_INVALID;
  }

  bool attachSinkLinkListeners(unsigned channel) {
    static const auto proxyEvents = [] {
      pw_proxy_events value{};
      value.version = PW_VERSION_PROXY_EVENTS;
      value.error = proxyError;
      return value;
    }();
    pw_proxy_add_listener(sinkLinks[channel], &sinkProxyHooks[channel],
                          &proxyEvents, &sinkLinkContexts[channel]);
    sinkProxyListenerAttached[channel] = true;
    static const auto events = [] {
      pw_link_events value{};
      value.version = PW_VERSION_LINK_EVENTS;
      value.info = linkInfo;
      return value;
    }();
    if (pw_link_add_listener(reinterpret_cast<pw_link *>(sinkLinks[channel]),
                             &sinkLinkHooks[channel], &events,
                             &sinkLinkContexts[channel]) < 0) {
      setSinkLinkError("cannot monitor SkyAPO playback sink link");
      destroySinkLink(channel);
      scheduleLinkRetry();
      return false;
    }
    sinkLinkListenerAttached[channel] = true;
    return true;
  }

  void bindSinkLink(unsigned channel, uint32_t globalId) {
    if (channel >= channels || globalId == SPA_ID_INVALID)
      return;
    if (sinkLinks[channel]) {
      const auto boundId = pw_proxy_get_bound_id(sinkLinks[channel]);
      if (boundId == globalId || boundId == SPA_ID_INVALID) {
        sinkLinkGlobalIds[channel] = globalId;
        return;
      }
      destroySinkLink(channel);
    }
    auto *boundLink = static_cast<pw_proxy *>(pw_registry_bind(
        registry, globalId, PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, 0));
    if (!boundLink) {
      sinkLinkGlobalIds[channel] = globalId;
      setSinkLinkError("cannot bind existing PipeWire playback sink link");
      scheduleLinkRetry();
      return;
    }
    sinkLinkGlobalIds[channel] = globalId;
    sinkLinks[channel] = boundLink;
    if (!attachSinkLinkListeners(channel))
      return;
    std::cerr << "skyapod: monitoring existing playback sink link " << globalId
              << " (" << ports[channel].channel << ")\n";
  }

  static void global(void *data, uint32_t id, uint32_t, const char *type,
                     uint32_t, const spa_dict *props) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback([&] { handleGlobal(self, id, type, props); },
                                  self.controlCallbackFailed, self.faulted);
  }

  static void handleGlobal(Impl &self, uint32_t id, const char *type,
                           const spa_dict *props) {
    if (props && type && std::strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
      const auto application = property(props, "application.name");
      const auto binary = property(props, "application.process.binary");
      if (application == "WirePlumber" || application == "wireplumber" ||
          binary == "wireplumber")
        self.sessionManagerPresent = true;
      return;
    }
    if (props && type && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0 &&
        id == self.frontendId)
      return;
    if (props && type && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0 &&
        property(props, PW_KEY_MEDIA_CLASS) == "Audio/Sink") {
      self.considerSink(id, props);
      self.watchSinkNode(id);
    }
    if (props && type && std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
      ObservedSinkLink link{};
      link.id = id;
      if (!parseGlobalId(property(props, PW_KEY_LINK_OUTPUT_NODE),
                         link.outputNode) ||
          !parseGlobalId(property(props, PW_KEY_LINK_OUTPUT_PORT),
                         link.outputPort) ||
          !parseGlobalId(property(props, PW_KEY_LINK_INPUT_NODE),
                         link.inputNode) ||
          !parseGlobalId(property(props, PW_KEY_LINK_INPUT_PORT),
                         link.inputPort))
        return;
      self.rememberSinkLink(link);
      for (unsigned channel = 0; channel < self.channels; ++channel) {
        if (link.outputNode == pw_filter_get_node_id(self.filter) &&
            link.inputNode == pw_filter_get_node_id(self.streamFilter) &&
            self.ownOutputIds[channel] == link.outputPort &&
            self.streamInputIds[channel] == link.inputPort) {
          self.internalLinkExists[channel] = true;
          self.internalLinked[channel] = true;
          self.linkGlobalIds[channel] = id;
          return;
        }
        if (link.outputNode == pw_filter_get_node_id(self.streamFilter) &&
            link.inputNode == self.sink.id &&
            self.streamOutputIds[channel] == link.outputPort &&
            self.ports[channel].id == link.inputPort) {
          self.sinkLinkGlobalIds[channel] = id;
          if (!self.sinkLinks[channel])
            self.bindSinkLink(channel, id);
          return;
        }
      }
      return;
    }
    if (!props || !type || std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
      return;
    const auto nodeId = property(props, PW_KEY_NODE_ID);
    const auto direction = property(props, PW_KEY_PORT_DIRECTION);
    const auto channel = property(props, PW_KEY_AUDIO_CHANNEL);
    if (direction == "in" &&
        std::find_if(SpaChannels.begin(), SpaChannels.end(),
                     [&](const auto &item) { return channel == item.first; }) !=
            SpaChannels.end()) {
      char *end = nullptr;
      const auto parsedNode = std::strtoul(nodeId.c_str(), &end, 10);
      if (!nodeId.empty() && end && *end == '\0' && parsedNode <= UINT32_MAX &&
          std::none_of(self.knownInputPorts.begin(), self.knownInputPorts.end(),
                       [&](const auto &known) { return known.id == id; }))
        self.knownInputPorts.push_back({id, static_cast<uint32_t>(parsedNode),
                                        "in", channel,
                                        property(props, PW_KEY_PORT_NAME)});
    }
    if (direction == "out") {
      uint32_t node{};
      if (parseGlobalId(nodeId, node) &&
          std::none_of(self.knownOutputPorts.begin(),
                       self.knownOutputPorts.end(),
                       [&](const auto &port) { return port.id == id; }))
        self.knownOutputPorts.push_back(
            {id, node, "out", channel, property(props, PW_KEY_PORT_NAME)});
    }
    for (unsigned index = 0; index < self.channels; ++index) {
      if (channel != self.ports[index].channel)
        continue;
      if (nodeId == std::to_string(self.frontendId) && direction == "out") {
        self.frontendMonitorIds[index] = id;
        self.createFrontendLink(index);
      } else if (nodeId == std::to_string(pw_filter_get_node_id(self.filter)) &&
                 direction == "in") {
        self.dspInputIds[index] = id;
        self.createFrontendLink(index);
      } else if (nodeId == std::to_string(pw_filter_get_node_id(self.filter)) &&
                 direction == "out" &&
                 self.ownOutputIds[index] == SPA_ID_INVALID) {
        self.ownOutputIds[index] = id;
        self.createLink(index);
      } else if (nodeId ==
                     std::to_string(pw_filter_get_node_id(self.streamFilter)) &&
                 direction == "in" &&
                 self.streamInputIds[index] == SPA_ID_INVALID) {
        self.streamInputIds[index] = id;
        self.createLink(index);
      } else if (nodeId ==
                     std::to_string(pw_filter_get_node_id(self.streamFilter)) &&
                 direction == "out" &&
                 self.streamOutputIds[index] == SPA_ID_INVALID) {
        self.streamOutputIds[index] = id;
        self.createSinkLink(index);
      } else if (self.sinkAvailable && nodeId == std::to_string(self.sink.id) &&
                 direction == "in" && self.ports[index].id == SPA_ID_INVALID) {
        self.ports[index].id = id;
        self.ports[index].node = self.sink.id;
        self.createSinkLink(index);
      }
      return;
    }
  }

  void enableFrontendLinks() {
    frontendLinksEnabled = true;
    for (unsigned channel = 0; channel < channels; ++channel)
      createFrontendLink(channel);
  }

  void considerSink(uint32_t id, const spa_dict *props) {
    const auto nodeName = property(props, PW_KEY_NODE_NAME);
    const auto identity = deviceIdentityFromProps(props);
    const auto wantedIdentity = sink.identity.stableProperty();
    const auto foundIdentity = identity.stableProperty();
    const bool matches = wantedIdentity.empty()
                             ? nodeName == sink.name
                             : foundIdentity == wantedIdentity;
    if (!matches || (sinkAvailable && id == sink.id))
      return;

    sink.id = id;
    sink.name = nodeName;
    sink.description = property(props, PW_KEY_NODE_DESCRIPTION);
    sink.identity = identity;
    sinkAvailable = true;
    for (unsigned channel = 0; channel < channels; ++channel) {
      ports[channel].id = SPA_ID_INVALID;
      ports[channel].node = id;
      linked[channel] = false;
      const auto port =
          std::find_if(knownInputPorts.begin(), knownInputPorts.end(),
                       [&](const AudioPort &candidate) {
                         return candidate.node == id &&
                                candidate.channel == ports[channel].channel;
                       });
      if (port != knownInputPorts.end())
        ports[channel] = *port;
    }
    recreateStreamFilter();
  }

  void watchSinkNode(uint32_t id) {
    if (std::any_of(sinkBindings.begin(), sinkBindings.end(),
                    [&](const auto &binding) { return binding->id == id; }))
      return;
    auto binding = std::make_unique<NodeBinding>();
    binding->owner = this;
    binding->id = id;
    binding->node = static_cast<pw_node *>(pw_registry_bind(
        registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!binding->node)
      return;
    static const auto events = [] {
      pw_node_events value{};
      value.version = PW_VERSION_NODE_EVENTS;
      value.info = sinkNodeInfo;
      return value;
    }();
    if (pw_node_add_listener(binding->node, &binding->hook, &events,
                             binding.get()) < 0) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(binding->node));
      return;
    }
    sinkBindings.push_back(std::move(binding));
  }

  static void sinkNodeInfo(void *data, const pw_node_info *info) noexcept {
    auto &binding = *static_cast<NodeBinding *>(data);
    auto &self = *binding.owner;
    detail::invokeControlCallback(
        [&] {
          if (info && info->props)
            binding.owner->considerSink(binding.id, info->props);
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void globalRemove(void *data, uint32_t id) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback([&] { handleGlobalRemove(self, id); },
                                  self.controlCallbackFailed, self.faulted);
  }

  static void handleGlobalRemove(Impl &self, uint32_t id) {
    self.removeObservedSinkLink(id);
    if (id == self.frontendId) {
      self.frontendId = SPA_ID_INVALID;
      self.error = "playback adapter disappeared";
      self.frontendFaulted.store(true, std::memory_order_release);
      self.frontendRecreateNeeded = true;
      self.scheduleLinkRetry();
    }
    self.knownOutputPorts.erase(
        std::remove_if(self.knownOutputPorts.begin(),
                       self.knownOutputPorts.end(),
                       [&](const auto &port) { return port.id == id; }),
        self.knownOutputPorts.end());
    for (unsigned channel = 0; channel < self.channels; ++channel) {
      const bool monitorRemoved = self.frontendMonitorIds[channel] == id;
      const bool inputRemoved = self.dspInputIds[channel] == id;
      if (monitorRemoved || inputRemoved ||
          self.frontendLinks[channel].globalId == id) {
        self.destroyFrontendLink(channel);
        if (monitorRemoved)
          self.frontendMonitorIds[channel] = SPA_ID_INVALID;
        if (inputRemoved)
          self.dspInputIds[channel] = SPA_ID_INVALID;
        self.scheduleLinkRetry();
      }
    }
    self.knownInputPorts.erase(
        std::remove_if(self.knownInputPorts.begin(), self.knownInputPorts.end(),
                       [&](const AudioPort &port) { return port.id == id; }),
        self.knownInputPorts.end());
    for (auto it = self.sinkBindings.begin(); it != self.sinkBindings.end();) {
      if ((*it)->id != id) {
        ++it;
        continue;
      }
      spa_hook_remove(&(*it)->hook);
      if ((*it)->node)
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>((*it)->node));
      it = self.sinkBindings.erase(it);
    }
    if (id == self.sink.id) {
      self.error = "selected playback sink disappeared";
      self.sinkAvailable = false;
      self.sink.id = SPA_ID_INVALID;
      for (unsigned channel = 0; channel < self.channels; ++channel)
        self.destroySinkLink(channel);
    }
    for (unsigned channel = 0; channel < self.channels; ++channel) {
      if (self.ports[channel].id == id) {
        self.destroySinkLink(channel);
        self.ports[channel].id = SPA_ID_INVALID;
      }
      if (self.streamOutputIds[channel] == id) {
        self.destroySinkLink(channel);
        self.streamOutputIds[channel] = SPA_ID_INVALID;
      }
      if (self.sinkLinkGlobalIds[channel] == id) {
        self.destroySinkLink(channel);
        self.scheduleLinkRetry();
      }
      if (self.linkGlobalIds[channel] != id)
        continue;
      self.destroyLink(channel);
      self.createLink(channel);
    }
  }

  static void formatReady(void *data, uint64_t) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback([&] { handleFormatReady(self); },
                                  self.controlCallbackFailed, self.faulted);
  }

  static void handleFormatReady(Impl &self) {
    self.formatEventPending.store(false, std::memory_order_release);
    self.reclaim();
    const unsigned sampleRate =
        self.requestedRate.load(std::memory_order_acquire);
    const unsigned quantum =
        self.requestedQuantum.load(std::memory_order_acquire);
    if (!sampleRate || !quantum)
      return;
    auto *current = self.active.load(std::memory_order_acquire);
    const bool formatChanged = !current ||
                               current->sampleRate() != sampleRate ||
                               !current->supportsFrameCount(quantum);
    try {
      if (formatChanged) {
        // A format-only rebuild creates a fresh Engine, but must not clear a
        // PDC fault raised by the previous instance. Only an explicit,
        // validated config reload is allowed to recover that fail-closed state.
        self.pluginLatencyRefreshQueued.store(false, std::memory_order_release);
        self.buildEngine(sampleRate, quantum, false,
                         self.acceptedConfigSources);
        if (!self.pluginLatencyFaulted.load(std::memory_order_acquire))
          self.error.clear();
      } else if (self.pluginLatencyRefreshQueued.exchange(
                     false, std::memory_order_acq_rel)) {
        self.refreshPluginLatency();
      } else if (!self.faulted.load(std::memory_order_acquire)) {
        self.error.clear();
      }
    } catch (const std::exception &exception) {
      self.error = exception.what();
      std::cerr << "skyapod: playback DSP setup failed; output silence: "
                << self.error << '\n';
    }
  }

  static void stateChanged(void *data, pw_filter_state, pw_filter_state next,
                           const char *message) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          self.state = next;
          if (next == PW_FILTER_STATE_ERROR)
            self.error = message ? message : "PipeWire playback filter failed";
          else if (next == PW_FILTER_STATE_UNCONNECTED && !self.destroying)
            self.error = "PipeWire playback filter disconnected";
          else if (next == PW_FILTER_STATE_STREAMING &&
                   self.error == "PipeWire playback filter disconnected")
            self.error.clear();
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void streamStateChanged(void *data, pw_filter_state,
                                 pw_filter_state next,
                                 const char *message) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          self.streamState = next;
          if (next == PW_FILTER_STATE_ERROR)
            self.error = message ? message : "PipeWire playback stream failed";
          else if (next == PW_FILTER_STATE_UNCONNECTED && !self.destroying)
            self.error = "PipeWire playback stream disconnected";
          else if (next == PW_FILTER_STATE_STREAMING &&
                   self.error == "PipeWire playback stream disconnected")
            self.error.clear();
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void paramChanged(void *data, void *port, uint32_t id,
                           const spa_pod *param) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          if (port || !param)
            return;
          if (id == SPA_PARAM_Props) {
            self.applyVolumeProps(param, self.sinkVolumes, self.sinkMuted,
                                  "playback sink");
            return;
          }
          if (id != SPA_PARAM_Format && id != SPA_PARAM_PortConfig)
            return;
          if (id == SPA_PARAM_Format) {
            spa_audio_info_raw info{};
            if (spa_format_audio_raw_parse(param, &info) < 0 ||
                info.channels != self.channels ||
                (info.format != SPA_AUDIO_FORMAT_F32P &&
                 info.format != SPA_AUDIO_FORMAT_F32)) {
              self.error = "unsupported SkyAPO playback format request";
              return;
            }
          }
          // WirePlumber configures Audio/Sink nodes through the SPA
          // AudioAdapter session-item contract. The fixed playback ports
          // already exist, so acknowledge the format/port configuration and
          // expose the standard controls expected by the adapter.
          pw_filter_update_params(self.filter, nullptr, &param, 1);
          if (id == SPA_PARAM_PortConfig) {
            uint8_t bytes[512];
            std::array<float, MaxChannels> volumes{};
            volumes.fill(1.0f);
            spa_pod_builder builder =
                SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
            const spa_pod *props =
                static_cast<spa_pod *>(spa_pod_builder_add_object(
                    &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                    SPA_PROP_volume, SPA_POD_Float(1.0f), SPA_PROP_mute,
                    SPA_POD_Bool(false), SPA_PROP_channelVolumes,
                    SPA_POD_Array(sizeof(float), SPA_TYPE_Float, self.channels,
                                  volumes.data())));
            if (props)
              pw_filter_update_params(self.filter, nullptr, &props, 1);
            else
              self.error =
                  "cannot build playback Props for WirePlumber adapter";
          }
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void streamParamChanged(void *data, void *port, uint32_t id,
                                 const spa_pod *param) noexcept {
    auto &self = *static_cast<Impl *>(data);
    detail::invokeControlCallback(
        [&] {
          if (port || !param)
            return;
          if (id == SPA_PARAM_Props) {
            self.applyVolumeProps(param, self.streamVolumes, self.streamMuted,
                                  "playback stream");
            return;
          }
          if (id != SPA_PARAM_Format && id != SPA_PARAM_PortConfig)
            return;
          if (id == SPA_PARAM_Format) {
            spa_audio_info_raw info{};
            if (spa_format_audio_raw_parse(param, &info) < 0 ||
                info.channels != self.channels ||
                (info.format != SPA_AUDIO_FORMAT_F32P &&
                 info.format != SPA_AUDIO_FORMAT_F32)) {
              self.error = "unsupported SkyAPO playback stream format request";
              return;
            }
          }
          pw_filter_update_params(self.streamFilter, nullptr, &param, 1);
          if (id == SPA_PARAM_PortConfig) {
            uint8_t bytes[512];
            std::array<float, MaxChannels> volumes{};
            volumes.fill(1.0f);
            spa_pod_builder builder =
                SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
            const spa_pod *props =
                static_cast<spa_pod *>(spa_pod_builder_add_object(
                    &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
                    SPA_PROP_volume, SPA_POD_Float(1.0f), SPA_PROP_mute,
                    SPA_POD_Bool(false), SPA_PROP_channelVolumes,
                    SPA_POD_Array(sizeof(float), SPA_TYPE_Float, self.channels,
                                  volumes.data())));
            if (props)
              pw_filter_update_params(self.streamFilter, nullptr, &props, 1);
            else
              self.error = "cannot build stream Props for WirePlumber adapter";
          }
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void proxyError(void *data, int, int result,
                         const char *message) noexcept {
    auto &context = *static_cast<LinkContext *>(data);
    auto &self = *context.owner;
    detail::invokeControlCallback(
        [&] {
          if (context.sinkLink) {
            if (!self.sinkLinks[context.channel])
              return;
            if (result == -EEXIST) {
              auto existingId = self.sinkLinkGlobalIds[context.channel];
              if (existingId == SPA_ID_INVALID)
                existingId = self.findObservedSinkLink(context.channel);
              if (existingId != SPA_ID_INVALID) {
                self.destroySinkLink(context.channel);
                self.sinkLinkGlobalIds[context.channel] = existingId;
                self.bindSinkLink(context.channel, existingId);
                return;
              }
            }
            self.setSinkLinkError(
                message ? std::string("PipeWire playback sink link failed: ") +
                              message
                        : "PipeWire playback sink link proxy failed");
            self.destroySinkLink(context.channel);
            self.scheduleLinkRetry();
            return;
          }
          if (!self.links[context.channel])
            return;
#ifdef SKYAPO_PIPEWIRE_E2E_TESTING
          recordPlaybackLinkAsyncErrorForTesting();
#endif
          self.setInternalLinkError(
              message ? std::string("PipeWire playback link failed: ") + message
                      : "PipeWire playback link proxy failed");
          self.destroyLink(context.channel);
          self.scheduleLinkRetry();
        },
        self.controlCallbackFailed, self.faulted);
  }

  static void linkInfo(void *data, const pw_link_info *info) noexcept {
    auto &context = *static_cast<LinkContext *>(data);
    auto &self = *context.owner;
    detail::invokeControlCallback(
        [&] {
          if (!info)
            return;
          if (context.sinkLink) {
            if (info->output_node_id !=
                    pw_filter_get_node_id(self.streamFilter) ||
                info->output_port_id != self.streamOutputIds[context.channel] ||
                info->input_node_id != self.sink.id ||
                info->input_port_id != self.ports[context.channel].id) {
              self.setSinkLinkError(
                  "PipeWire playback sink link resolved to unexpected endpoint "
                  "IDs (output node/port " +
                  std::to_string(info->output_node_id) + "/" +
                  std::to_string(info->output_port_id) + ", input node/port " +
                  std::to_string(info->input_node_id) + "/" +
                  std::to_string(info->input_port_id) + ")");
              self.destroySinkLink(context.channel);
              self.scheduleLinkRetry();
              return;
            }
            self.sinkLinkGlobalIds[context.channel] = info->id;
            self.linked[context.channel] = info->state == PW_LINK_STATE_ACTIVE;
            if (info->state == PW_LINK_STATE_ERROR) {
              self.setSinkLinkError(info->error ? info->error
                                                : "playback sink link failed");
              self.destroySinkLink(context.channel);
              self.scheduleLinkRetry();
            } else if (info->state == PW_LINK_STATE_ACTIVE) {
              if (self.error == "selected playback sink disappeared" &&
                  std::all_of(self.linked.begin(),
                              self.linked.begin() + self.channels,
                              [](bool value) { return value; }))
                self.error.clear();
              self.clearSinkLinkErrorIfRecovered();
            }
            return;
          }
          self.linkGlobalIds[context.channel] = info->id;
          self.internalLinkExists[context.channel] =
              info->state != PW_LINK_STATE_ERROR;
          self.internalLinked[context.channel] =
              info->state == PW_LINK_STATE_ACTIVE;
          if (info->state == PW_LINK_STATE_ERROR) {
            self.setInternalLinkError(
                info->error ? info->error : "internal playback link failed");
            self.destroyLink(context.channel);
            self.scheduleLinkRetry();
          } else if (info->state == PW_LINK_STATE_ACTIVE) {
            self.clearInternalLinkErrorIfRecovered();
          }
        },
        self.controlCallbackFailed, self.faulted);
  }

  void applyFrontendProps(const spa_pod *pod) {
    if (!spa_pod_is_object(pod)) {
      error = "invalid playback adapter Props";
      return;
    }
    float scalar = 1.0f;
    const auto *volume = spa_pod_find_prop(pod, nullptr, SPA_PROP_volume);
    if (volume && (spa_pod_get_float(&volume->value, &scalar) < 0 ||
                   !std::isfinite(scalar) || scalar < 0.0f)) {
      error = "invalid playback adapter scalar volume";
      return;
    }
    std::array<float, MaxChannels> next{};
    const auto *channelVolume =
        spa_pod_find_prop(pod, nullptr, SPA_PROP_channelVolumes);
    if (channelVolume) {
      const auto *value = &channelVolume->value;
      if (SPA_POD_TYPE(value) != SPA_TYPE_Array ||
          SPA_POD_ARRAY_N_VALUES(value) != channels ||
          SPA_POD_ARRAY_VALUE_TYPE(value) != SPA_TYPE_Float ||
          SPA_POD_ARRAY_VALUE_SIZE(value) != sizeof(float)) {
        error = "invalid playback adapter channel volumes";
        return;
      }
      std::memcpy(next.data(), SPA_POD_ARRAY_VALUES(value),
                  channels * sizeof(float));
      for (unsigned channel = 0; channel < channels; ++channel)
        if (!std::isfinite(next[channel]) || next[channel] < 0.0f) {
          error = "invalid playback adapter channel volume";
          return;
        }
    }
    const auto *mute = spa_pod_find_prop(pod, nullptr, SPA_PROP_mute);
    bool muted = sinkMuted.load(std::memory_order_acquire);
    if (mute && spa_pod_get_bool(&mute->value, &muted) < 0) {
      error = "invalid playback adapter mute";
      return;
    }
    // Subscription may report the new adapter's defaults before our restore
    // request is acknowledged. Keep the old gain/mute and silence the DSP
    // until matching controls and every monitor link are both ready.
    if (!frontendControlsRestored) {
      if (!channelVolume || !mute || !volume ||
          next != frontendRestoreVolumes || muted != frontendRestoreMuted ||
          scalar != frontendRestoreScalar)
        return;
      frontendControlsRestored = true;
    }
    frontendScalarMetadata = scalar;
    // Match the native adapter's monitor contract: channelVolumes and mute
    // control the monitor signal. Scalar volume is valid/readable metadata,
    // but does not independently scale it. Keep this gain after the Engine.
    if (channelVolume)
      for (unsigned channel = 0; channel < channels; ++channel)
        sinkVolumes[channel].store(next[channel], std::memory_order_release);
    if (mute)
      sinkMuted.store(muted, std::memory_order_release);
    updateFrontendRecoveryGate();
  }

  void applyVolumeProps(const spa_pod *pod,
                        std::array<std::atomic<float>, MaxChannels> &volumes,
                        std::atomic<bool> &muted, const char *nodeDescription) {
    if (!spa_pod_is_object(pod)) {
      error = std::string("invalid PipeWire Props for ") + nodeDescription;
      return;
    }
    const auto *muteProp = spa_pod_find_prop(pod, nullptr, SPA_PROP_mute);
    const auto *volumeProp = spa_pod_find_prop(pod, nullptr, SPA_PROP_volume);
    const auto *channelVolumesProp =
        spa_pod_find_prop(pod, nullptr, SPA_PROP_channelVolumes);
    std::array<float, MaxChannels> next{};
    bool hasVolume = false;
    if (channelVolumesProp) {
      const auto *value = &channelVolumesProp->value;
      if (SPA_POD_TYPE(value) != SPA_TYPE_Array ||
          SPA_POD_ARRAY_N_VALUES(value) != channels ||
          SPA_POD_ARRAY_VALUE_TYPE(value) != SPA_TYPE_Float ||
          SPA_POD_ARRAY_VALUE_SIZE(value) != sizeof(float)) {
        error = std::string("invalid channelVolumes in PipeWire Props for ") +
                nodeDescription;
        return;
      }
      const auto *values =
          static_cast<const uint8_t *>(SPA_POD_ARRAY_VALUES(value));
      if (!values) {
        error = std::string("empty channelVolumes in PipeWire Props for ") +
                nodeDescription;
        return;
      }
      for (unsigned channel = 0; channel < channels; ++channel) {
        std::memcpy(&next[channel], values + channel * sizeof(float),
                    sizeof(float));
        if (!std::isfinite(next[channel]) || next[channel] < 0.0f) {
          error = std::string("non-finite or negative PipeWire volume for ") +
                  nodeDescription;
          return;
        }
      }
      hasVolume = true;
    } else if (volumeProp) {
      float scalar = 0.0f;
      if (spa_pod_get_float(&volumeProp->value, &scalar) < 0 ||
          !std::isfinite(scalar) || scalar < 0.0f) {
        error = std::string("invalid scalar PipeWire volume for ") +
                nodeDescription;
        return;
      }
      next.fill(scalar);
      hasVolume = true;
    }
    bool nextMuted = false;
    const bool hasMute = muteProp != nullptr;
    if (hasMute && spa_pod_get_bool(&muteProp->value, &nextMuted) < 0) {
      error = std::string("invalid mute value in PipeWire Props for ") +
              nodeDescription;
      return;
    }
    if (!hasVolume && !hasMute)
      return;
    if (hasVolume)
      for (unsigned channel = 0; channel < channels; ++channel)
        volumes[channel].store(next[channel], std::memory_order_release);
    if (hasMute)
      muted.store(nextMuted, std::memory_order_release);
  }

  static void process(void *data, spa_io_position *position) {
    realtime::Scope audit;
    auto &self = *static_cast<Impl *>(data);
    auto flight = self.callbackGate.enterCallback();
    const unsigned frames = position->clock.duration;
    const unsigned sampleRate =
        position->clock.rate.num
            ? position->clock.rate.denom / position->clock.rate.num
            : 0;
    if (!frames)
      return;
    self.quantum.store(frames, std::memory_order_relaxed);
    std::array<float *, MaxChannels> input{}, output{};
    for (unsigned channel = 0; channel < self.channels; ++channel) {
      input[channel] = static_cast<float *>(
          pw_filter_get_dsp_buffer(self.inputs[channel], frames));
      output[channel] = static_cast<float *>(
          pw_filter_get_dsp_buffer(self.outputs[channel], frames));
    }
    auto silence = [&] {
      for (unsigned channel = 0; channel < self.channels; ++channel)
        if (output[channel])
          std::fill_n(output[channel], frames, 0.0f);
    };
    if (!flight.mayProcess()) {
      silence();
      return;
    }
    auto *engine = self.active.load(std::memory_order_seq_cst);
    if (!engine || engine->sampleRate() != sampleRate ||
        !engine->supportsFrameCount(frames)) {
      silence();
      const bool rateChanged =
          self.requestedRate.exchange(sampleRate) != sampleRate;
      const bool quantumChanged =
          self.requestedQuantum.exchange(frames) != frames;
      if ((rateChanged || quantumChanged) &&
          !self.formatEventPending.exchange(true))
        pw_loop_signal_event(self.loop, self.formatEvent);
      return;
    }
    if (self.faulted.load(std::memory_order_acquire) ||
        self.frontendFaulted.load(std::memory_order_acquire)) {
      silence();
      return;
    }
    auto *next = self.pending.load(std::memory_order_seq_cst);
    bool pluginLatencyFaultDetected = false;
    const auto failClosedForPluginLatency = [&] {
      silence();
      self.pluginLatencyFaulted.store(true, std::memory_order_release);
      self.faulted.store(true, std::memory_order_release);
      self.pluginLatencyRefreshQueued.store(true, std::memory_order_release);
      if (!self.formatEventPending.exchange(true, std::memory_order_acq_rel))
        pw_loop_signal_event(self.loop, self.formatEvent);
    };
    try {
      for (unsigned offset = 0; offset < frames; offset += MaxFrames) {
        const unsigned count = std::min(MaxFrames, frames - offset);
        for (unsigned frame = 0; frame < count; ++frame)
          for (unsigned channel = 0; channel < self.channels; ++channel)
            self.work[static_cast<size_t>(frame) * self.channels + channel] =
                input[channel] ? input[channel][offset + frame] : 0.0f;
        if (next) {
          self.transitionCounter = engine->processTransitionTo(
              *next, self.work.data(), count, self.transitionCounter,
              self.transitionLength);
          if (self.transitionCounter >= self.transitionLength) {
            engine = next;
            self.active.store(engine, std::memory_order_seq_cst);
            self.pending.store(nullptr, std::memory_order_seq_cst);
            self.transitionComplete.store(true, std::memory_order_release);
            if (!self.formatEventPending.exchange(true,
                                                  std::memory_order_acq_rel))
              pw_loop_signal_event(self.loop, self.formatEvent);
            next = nullptr;
          }
        } else {
          engine->process(self.work.data(), count);
        }
        if (engine->pluginLatencyRefreshPending() ||
            (next && next->pluginLatencyRefreshPending())) {
          // Never publish even a partial quantum after a plugin changes its
          // latency. The control thread validates/rebuilds while output stays
          // fail-closed.
          pluginLatencyFaultDetected = true;
          break;
        }
        const bool muted = self.sinkMuted.load(std::memory_order_acquire);
        std::array<float, MaxChannels> gains{};
        for (unsigned channel = 0; channel < self.channels; ++channel)
          gains[channel] =
              muted ? 0.0f
                    : self.sinkVolumes[channel].load(std::memory_order_acquire);
        for (unsigned frame = 0; frame < count; ++frame)
          for (unsigned channel = 0; channel < self.channels; ++channel)
            if (output[channel])
              output[channel][offset + frame] =
                  self.work[static_cast<size_t>(frame) * self.channels +
                            channel] *
                  gains[channel];
      }
    } catch (...) {
      silence();
      self.faulted.store(true, std::memory_order_release);
    }
    if (!pluginLatencyFaultDetected) {
      auto *processed = self.active.load(std::memory_order_seq_cst);
      auto *candidate = self.pending.load(std::memory_order_seq_cst);
      pluginLatencyFaultDetected =
          (processed && processed->pluginLatencyRefreshPending()) ||
          (candidate && candidate->pluginLatencyRefreshPending());
    }
    if (pluginLatencyFaultDetected)
      failClosedForPluginLatency();
  }

  static void streamProcess(void *data, spa_io_position *position) {
    realtime::Scope audit;
    auto &self = *static_cast<Impl *>(data);
    auto flight = self.callbackGate.enterCallback();
    const unsigned frames = position->clock.duration;
    std::array<float *, MaxChannels> input{}, output{};
    for (unsigned channel = 0; channel < self.channels; ++channel) {
      input[channel] = static_cast<float *>(
          pw_filter_get_dsp_buffer(self.streamInputs[channel], frames));
      output[channel] = static_cast<float *>(
          pw_filter_get_dsp_buffer(self.streamOutputs[channel], frames));
    }
    const bool muted = self.streamMuted.load(std::memory_order_acquire);
    std::array<float, MaxChannels> gains{};
    for (unsigned channel = 0; channel < self.channels; ++channel)
      gains[channel] =
          muted ? 0.0f
                : self.streamVolumes[channel].load(std::memory_order_acquire);
    for (unsigned channel = 0; channel < self.channels; ++channel) {
      if (!output[channel])
        continue;
      if (input[channel])
        for (unsigned frame = 0; frame < frames; ++frame)
          output[channel][frame] = input[channel][frame] * gains[channel];
      else
        std::fill_n(output[channel], frames, 0.0f);
    }
  }
};

PlaybackGraph::PlaybackGraph(pw_core *core, pw_loop *loop,
                             pw_registry *registry, AudioDevice device,
                             std::vector<AudioPort> ports,
                             std::string configPath, bool sessionManagerPresent)
    : impl_(std::make_unique<Impl>(core, loop, registry, std::move(device),
                                   std::move(ports), std::move(configPath),
                                   sessionManagerPresent)) {}

PlaybackGraph::~PlaybackGraph() = default;

void PlaybackGraph::reloadConfig(const std::string &configPath) {
  reloadConfig(configPath, {});
}

void PlaybackGraph::reloadConfig(
    const std::string &configPath,
    std::shared_ptr<const ConfigSourceSnapshot> sourceSnapshot) {
  const auto previousPath = impl_->configPath;
  impl_->configPath = configPath;
  const unsigned sampleRate =
      impl_->requestedRate.load(std::memory_order_acquire);
  const unsigned quantum =
      impl_->requestedQuantum.load(std::memory_order_acquire);
  if (sampleRate && quantum) {
    try {
      impl_->reclaim();
      const auto recovery = impl_->buildEngine(sampleRate, quantum, true,
                                               std::move(sourceSnapshot));
      if (recovery.validatedForRecovery)
        impl_->pluginLatencyRefreshQueued.store(false,
                                                std::memory_order_release);
      detail::PlaybackFaultRecovery::finishConfigReload(
          recovery, impl_->pluginLatencyFaulted, impl_->faulted,
          impl_->controlCallbackFailed);
      impl_->callbackGate.finishRecovery();
      if (!impl_->faulted.load(std::memory_order_acquire))
        impl_->error.clear();
    } catch (...) {
      impl_->callbackGate.finishRecovery();
      impl_->configPath = previousPath;
      throw;
    }
  }
}

void PlaybackGraph::selectSink(AudioDevice device,
                               std::vector<AudioPort> ports) {
  impl_->selectSink(std::move(device), std::move(ports));
}

void PlaybackGraph::sessionManagerDetected() noexcept {
  impl_->sessionManagerPresent = true;
}

std::string PlaybackGraph::status() const {
  const auto &self = *impl_;
  std::ostringstream out;
  out << "Playback node: SkyAPO Playback (skyapo.playback, " << self.frontendId
      << ")\nPlayback stream node: skyapo.playback.stream ("
      << (self.streamFilter ? pw_filter_get_node_id(self.streamFilter)
                            : SPA_ID_INVALID)
      << ")\nPlayback target: " << self.sink.name << " ("
      << (self.sinkAvailable ? std::to_string(self.sink.id) : "unavailable")
      << ")\nPlayback graph: " << pw_filter_state_as_string(self.state) << " / "
      << pw_filter_state_as_string(self.streamState) << ", internal links "
      << std::count(self.internalLinked.begin(),
                    self.internalLinked.begin() + self.channels, true)
      << '/' << self.channels << ", target links "
      << std::count(self.linked.begin(), self.linked.begin() + self.channels,
                    true)
      << '/' << self.channels << " channels\nPlayback format: ";
  if (self.rate.load())
    out << self.rate.load() << " Hz, " << self.channels << " channels, quantum "
        << self.quantum.load();
  else
    out << "unknown (awaiting PipeWire negotiation)";
  if (auto *engine = self.active.load(std::memory_order_acquire))
    out << "\nPlayback filters: " << engine->filterCount();
  else
    out << "\nPlayback filters: unknown (DSP not initialized)";
  out << "\nPlayback plugin latency refreshes: " << self.pluginLatencyRefreshes;
  if (!self.error.empty())
    out << "\nPlayback error: " << self.error;
  if (self.faulted.load(std::memory_order_acquire)) {
    if (self.controlCallbackFailed.load(std::memory_order_acquire))
      out << "\nPlayback DSP: faulted (PipeWire control callback threw; output "
             "silence until daemon restart)";
    else if (self.pluginLatencyFaulted.load(std::memory_order_acquire))
      out << "\nPlayback DSP: faulted (plugin latency refresh failed; output "
             "silence until config reload)";
    else
      out << "\nPlayback DSP: faulted (output silence until config reload)";
  }
  return out.str();
}

const AudioDevice &PlaybackGraph::device() const noexcept {
  return impl_->sink;
}

bool PlaybackGraph::ready() const noexcept {
  const auto &self = *impl_;
  return self.state == PW_FILTER_STATE_STREAMING &&
         self.streamState == PW_FILTER_STATE_STREAMING &&
         self.active.load(std::memory_order_acquire) != nullptr &&
         self.sinkAvailable && self.frontendId != SPA_ID_INVALID &&
         !self.faulted.load(std::memory_order_acquire) &&
         !self.frontendFaulted.load(std::memory_order_acquire) &&
         std::all_of(self.frontendLinks.begin(),
                     self.frontendLinks.begin() + self.channels,
                     [](const auto &link) { return link.active; }) &&
         std::all_of(self.internalLinked.begin(),
                     self.internalLinked.begin() + self.channels,
                     [](bool value) { return value; }) &&
         std::all_of(self.linked.begin(), self.linked.begin() + self.channels,
                     [](bool value) { return value; });
}

} // namespace skyapo::pipewire
