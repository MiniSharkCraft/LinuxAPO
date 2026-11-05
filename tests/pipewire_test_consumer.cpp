// Native PipeWire client used to consume and numerically verify the test mic.
// --expect-silent accepts near-zero RMS for intentional plugin-failure tests.
#include <pipewire/filter.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <csignal>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <time.h>

namespace {
volatile sig_atomic_t measurementArmRequested = 0;
constexpr uint32_t Invalid = SPA_ID_INVALID;
constexpr double InputAmplitude = 0.1;
constexpr unsigned MaxChannels = 8;
constexpr std::array<const char *, MaxChannels> Surround51 = {
    "FL", "FR", "FC", "LFE", "RL", "RR", "", ""};
constexpr std::array<const char *, MaxChannels> Surround71 = {
    "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};

struct Consumer {
  struct LinkWatch {
    Consumer *consumer{};
    unsigned channel{};
  };

  pw_main_loop *loop{};
  pw_context *context{};
  pw_core *core{};
  pw_registry *registry{};
  pw_filter *filter{};
  spa_hook coreHook{}, registryHook{}, filterHook{};
  std::array<spa_hook, MaxChannels> linkHooks{};
  spa_source *timeout{}, *readyTimer{}, *armTimer{};
  unsigned channels{2};
  bool expectSilent{};
  bool acceptAnyAudio{};
  bool ignoreSilentBlocks{};
  bool observeOnly{};
  bool signalReady{};
  bool signalConnected{};
  bool armMeasurementOnSignal{};
  bool readinessReported{};
  double expectedDb{-6.0};
  unsigned expectedRate{};
  unsigned durationMs{3000};
  unsigned maxConsecutiveSilenceMs{};
  std::string captureNodeName{"skyapo.virtual_mic"};
  std::string consumerNodeName{"skyapo.test.consumer"};
  std::array<const char *, MaxChannels> channelNames{};
  std::array<void *, MaxChannels> inputs{};
  std::array<uint32_t, MaxChannels> inputIds{
      Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid};
  std::array<uint32_t, MaxChannels> outputIds{
      Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid, Invalid};
  std::array<pw_proxy *, MaxChannels> links{};
  std::array<LinkWatch, MaxChannels> linkWatches{};
  std::array<pw_link_state, MaxChannels> linkStates{};
  uint32_t sourceNode{Invalid};
  uint32_t ownNode{Invalid};
  std::atomic<uint64_t> frames{0};
  std::atomic<unsigned> sampleRate{0};
  std::atomic<double> energy{0.0};
  std::atomic<bool> measurementEnabled{true};
  std::atomic<double> channelDifferenceEnergy{0.0};
  bool expectIdenticalChannels{};
  bool measureSourceLag{};
  std::array<uint32_t, 64> sourceConsumerLatencyUs{};
  size_t sourceConsumerLatencyCount{};
  std::atomic<unsigned> processCalls{0};
  std::atomic<unsigned> invalidRateCalls{0};
  std::atomic<unsigned> zeroDurationCalls{0};
  std::atomic<unsigned> nullBufferCalls{0};
  std::atomic<int> filterState{PW_FILTER_STATE_UNCONNECTED};
  uint64_t consecutiveSilentFrames{};
  uint64_t maximumSilentFrames{};
  bool silenceMeasurementArmed{};
  std::vector<float> recordedLeft{};
  std::vector<float> recordedRight{};
  size_t recordedSamples{};
  std::atomic<bool> failed{false};

  explicit Consumer(bool mono, bool surround51, bool surround71, bool silent,
                    bool anyAudio, bool ignoreSilent, double gainDb,
                    unsigned rate, unsigned duration, std::string captureNode,
                    unsigned silenceLimitMs, std::string consumerNode,
                    bool onlyObserve, bool announceReady,
                    bool announceConnected, bool armOnSignal)
      : channels(mono ? 1 : (surround51 ? 6 : (surround71 ? 8 : 2))),
        expectSilent(silent), acceptAnyAudio(anyAudio),
        ignoreSilentBlocks(ignoreSilent), observeOnly(onlyObserve),
        signalReady(announceReady), signalConnected(announceConnected),
        armMeasurementOnSignal(armOnSignal), expectedDb(gainDb),
        expectedRate(rate), durationMs(duration),
        maxConsecutiveSilenceMs(silenceLimitMs),
        captureNodeName(std::move(captureNode)),
        consumerNodeName(std::move(consumerNode)),
        channelNames(
            mono ? std::array<const char *, MaxChannels>{"MONO", "", "", "", "",
                                                         "", "", ""}
                 : (surround51
                        ? Surround51
                        : (surround71
                               ? Surround71
                               : std::array<const char *, MaxChannels>{
                                     "FL", "FR", "", "", "", "", "", ""}))) {}

  static unsigned channelIndex(const char *channel, const Consumer &c) {
    for (unsigned i = 0; i < c.channels; ++i)
      if (std::strcmp(channel, c.channelNames[i]) == 0)
        return i;
    return c.channels;
  }

  static void linkInfo(void *data, const pw_link_info *info) {
    auto &watch = *static_cast<LinkWatch *>(data);
    if (!info)
      return;
    auto &state = watch.consumer->linkStates[watch.channel];
    if (state != info->state)
      std::fprintf(stderr, "test consumer: link %s state %s\n",
                   watch.consumer->channelNames[watch.channel],
                   pw_link_state_as_string(info->state));
    std::fflush(stderr);
    state = info->state;
  }

  void linkChannel(unsigned channel) {
    if (channel >= channels || sourceNode == Invalid || ownNode == Invalid ||
        outputIds[channel] == Invalid || inputIds[channel] == Invalid ||
        links[channel])
      return;
    auto *properties = pw_properties_new(nullptr, nullptr);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", sourceNode);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       outputIds[channel]);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", ownNode);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       inputIds[channel]);
    links[channel] = reinterpret_cast<pw_proxy *>(
        pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                              PW_VERSION_LINK, &properties->dict, 0));
    pw_properties_free(properties);
    if (links[channel]) {
      static const pw_link_events events = [] {
        pw_link_events value{};
        value.version = PW_VERSION_LINK_EVENTS;
        value.info = linkInfo;
        return value;
      }();
      linkWatches[channel] = {this, channel};
      pw_proxy_add_object_listener(links[channel], &linkHooks[channel], &events,
                                   &linkWatches[channel]);
    }
    if (!links[channel]) {
      failed.store(true, std::memory_order_relaxed);
      std::fprintf(stderr, "test consumer: cannot link channel %s\n",
                   channelNames[channel]);
      pw_main_loop_quit(loop);
    }
  }

  static void global(void *data, uint32_t id, uint32_t, const char *type,
                     uint32_t, const spa_dict *props) {
    auto &c = *static_cast<Consumer *>(data);
    if (!props)
      return;
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (name && c.captureNodeName == name)
        c.sourceNode = id;
      else if (name && c.consumerNodeName == name)
        c.ownNode = id;
      else
        return;
      if (!c.observeOnly)
        for (unsigned ch = 0; ch < c.channels; ++ch)
          c.linkChannel(ch);
      return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
      return;
    const char *nodeValue = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    const char *channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
    if (!nodeValue || !direction || !channel)
      return;
    char *end = nullptr;
    const auto nodeId = std::strtoul(nodeValue, &end, 10);
    if (!end || *end)
      return;
    const unsigned index = channelIndex(channel, c);
    if (index >= c.channels)
      return;
    if (nodeId == c.ownNode && std::strcmp(direction, "in") == 0)
      c.inputIds[index] = id;
    else if (nodeId == c.sourceNode && std::strcmp(direction, "out") == 0)
      c.outputIds[index] = id;
    else
      return;
    if (!c.observeOnly)
      c.linkChannel(index);
  }

  static void globalRemove(void *, uint32_t) {}

  static void coreError(void *data, uint32_t, int, int result,
                        const char *message) {
    auto &c = *static_cast<Consumer *>(data);
    c.failed.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "test consumer: PipeWire error %d: %s\n", result,
                 message ? message : "unknown error");
    pw_main_loop_quit(c.loop);
  }

  static void stateChanged(void *data, pw_filter_state, pw_filter_state state,
                           const char *error) {
    auto &c = *static_cast<Consumer *>(data);
    c.filterState.store(static_cast<int>(state), std::memory_order_relaxed);
    if (state != PW_FILTER_STATE_ERROR)
      return;
    c.failed.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "test consumer: %s\n",
                 error ? error : "PipeWire filter error");
    pw_main_loop_quit(c.loop);
  }

  static void paramChanged(void *data, void *port, uint32_t id,
                           const spa_pod *param) {
    auto &c = *static_cast<Consumer *>(data);
    if (port || !param)
      return;
    if (id == SPA_PARAM_Format) {
      spa_audio_info_raw info{};
      if (spa_format_audio_raw_parse(param, &info) < 0 ||
          info.channels != c.channels ||
          (info.format != SPA_AUDIO_FORMAT_F32P &&
           info.format != SPA_AUDIO_FORMAT_F32)) {
        c.failed.store(true, std::memory_order_relaxed);
        std::fprintf(stderr, "test consumer: unsupported format request\n");
        pw_main_loop_quit(c.loop);
        return;
      }
    }
    pw_filter_update_params(c.filter, nullptr, &param, 1);
  }

  static void process(void *data, spa_io_position *position) {
    auto &c = *static_cast<Consumer *>(data);
    c.processCalls.fetch_add(1, std::memory_order_relaxed);
    const auto rate = position->clock.rate.num ? position->clock.rate.denom /
                                                     position->clock.rate.num
                                               : 0;
    const auto previousRate = c.sampleRate.load(std::memory_order_relaxed);
    if (!rate || (c.expectedRate && rate != c.expectedRate) ||
        (previousRate && previousRate != rate)) {
      c.invalidRateCalls.fetch_add(1, std::memory_order_relaxed);
      c.failed.store(true, std::memory_order_relaxed);
      return;
    }
    c.sampleRate.store(rate, std::memory_order_relaxed);
    const uint32_t count = position->clock.duration;
    if (count == 0)
      c.zeroDurationCalls.fetch_add(1, std::memory_order_relaxed);
    double blockEnergy = 0.0;
    double blockChannelDifferenceEnergy = 0.0;
    uint64_t callbackUs = 0;
    if (c.measureSourceLag) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      callbackUs = static_cast<uint64_t>(now.tv_sec) * 1000000u +
                   static_cast<uint64_t>(now.tv_nsec) / 1000u;
    }
    std::array<float *, MaxChannels> channelData{};
    for (unsigned ch = 0; ch < c.channels; ++ch) {
      channelData[ch] =
          static_cast<float *>(pw_filter_get_dsp_buffer(c.inputs[ch], count));
      if (!channelData[ch]) {
        c.nullBufferCalls.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      for (uint32_t i = 0; i < count; ++i)
        blockEnergy +=
            static_cast<double>(channelData[ch][i]) * channelData[ch][i];
    }
    if (c.maxConsecutiveSilenceMs) {
      constexpr float SilenceThreshold = 1.0e-7f;
      for (uint32_t i = 0; i < count; ++i) {
        bool silentFrame = true;
        for (unsigned ch = 0; ch < c.channels; ++ch)
          silentFrame =
              silentFrame && std::abs(channelData[ch][i]) <= SilenceThreshold;
        if (!c.silenceMeasurementArmed) {
          c.silenceMeasurementArmed = !silentFrame;
          continue;
        }
        if (silentFrame) {
          ++c.consecutiveSilentFrames;
        } else {
          c.maximumSilentFrames =
              std::max(c.maximumSilentFrames, c.consecutiveSilentFrames);
          c.consecutiveSilentFrames = 0;
        }
      }
    }
    if (c.measureSourceLag && c.channels == 2) {
      constexpr double preampGain = 0.5011872336272722;
      for (uint32_t i = 0; i < count && c.sourceConsumerLatencyCount <
                                            c.sourceConsumerLatencyUs.size();
           ++i) {
        if (channelData[0][i] < 0.1f || channelData[1][i] < 0.1f)
          continue;
        const auto low = static_cast<uint32_t>(std::clamp(
            std::lround((channelData[0][i] / preampGain - 0.25) * 262144.0), 0l,
            65535l));
        const auto high = static_cast<uint32_t>(std::clamp(
            std::lround((channelData[1][i] / preampGain - 0.25) * 262144.0), 0l,
            65535l));
        const uint32_t sourceUs = (high << 16) | low;
        const uint32_t consumerUs = static_cast<uint32_t>(
            callbackUs + static_cast<uint64_t>(i * 1000000.0 / rate));
        const uint32_t elapsedUs = consumerUs - sourceUs;
        if (elapsedUs < 1000000u)
          c.sourceConsumerLatencyUs[c.sourceConsumerLatencyCount++] = elapsedUs;
      }
    }
    if (c.expectIdenticalChannels) {
      for (uint32_t i = 0; i < count; ++i) {
        for (unsigned ch = 1; ch < c.channels; ++ch) {
          const double difference =
              static_cast<double>(channelData[0][i]) - channelData[ch][i];
          blockChannelDifferenceEnergy += difference * difference;
        }
      }
      const auto remaining = c.recordedLeft.size() - c.recordedSamples;
      const auto copyCount = std::min<size_t>(count, remaining);
      std::copy_n(channelData[0], copyCount,
                  c.recordedLeft.data() + c.recordedSamples);
      std::copy_n(channelData[1], copyCount,
                  c.recordedRight.data() + c.recordedSamples);
      c.recordedSamples += copyCount;
    }
    if (c.ignoreSilentBlocks && blockEnergy < 1.0e-12)
      return;
    if (!c.measurementEnabled.load(std::memory_order_acquire))
      return;
    double total = c.energy.load(std::memory_order_relaxed);
    while (!c.energy.compare_exchange_weak(total, total + blockEnergy,
                                           std::memory_order_relaxed)) {
    }
    if (c.expectIdenticalChannels) {
      double differenceTotal =
          c.channelDifferenceEnergy.load(std::memory_order_relaxed);
      while (!c.channelDifferenceEnergy.compare_exchange_weak(
          differenceTotal, differenceTotal + blockChannelDifferenceEnergy,
          std::memory_order_relaxed)) {
      }
    }
    c.frames.fetch_add(count, std::memory_order_relaxed);
  }

  static void finish(void *data, uint64_t) {
    pw_main_loop_quit(static_cast<Consumer *>(data)->loop);
  }

  static void requestMeasurementArm(int) {
    measurementArmRequested = 1;
  }

  static void pollMeasurementArm(void *data, uint64_t) {
    auto &consumer = *static_cast<Consumer *>(data);
    if (measurementArmRequested) {
      measurementArmRequested = 0;
      consumer.energy.store(0.0, std::memory_order_release);
      consumer.frames.store(0, std::memory_order_release);
      consumer.measurementEnabled.store(true, std::memory_order_release);
      std::puts("SKYAPO_CONSUMER_MEASUREMENT_ARMED");
      std::fflush(stdout);
      timespec measurement{0, 700000000};
      pw_loop_update_timer(pw_main_loop_get_loop(consumer.loop),
                           consumer.timeout, &measurement, nullptr, false);
    } else {
      timespec retry{0, 10000000};
      pw_loop_update_timer(pw_main_loop_get_loop(consumer.loop),
                           consumer.armTimer, &retry, nullptr, false);
    }
  }

  void startDurationTimer() {
    timespec duration{static_cast<time_t>(durationMs / 1000),
                      static_cast<long>((durationMs % 1000) * 1000000)};
    pw_loop_update_timer(pw_main_loop_get_loop(loop), timeout, &duration,
                         nullptr, false);
  }

  static void reportReady(void *data, uint64_t) {
    auto &consumer = *static_cast<Consumer *>(data);
    if (consumer.readinessReported)
      return;
    if (consumer.signalReady &&
        consumer.frames.load(std::memory_order_acquire) != 0) {
      consumer.readinessReported = true;
      consumer.startDurationTimer();
      std::puts("SKYAPO_CONSUMER_READY");
      std::fflush(stdout);
      return;
    }
    if (consumer.signalConnected &&
        consumer.filterState.load(std::memory_order_acquire) ==
            PW_FILTER_STATE_STREAMING &&
        (consumer.observeOnly ||
         (consumer.linkStates[0] == PW_LINK_STATE_ACTIVE &&
          consumer.linkStates[1] == PW_LINK_STATE_ACTIVE))) {
      consumer.readinessReported = true;
      consumer.startDurationTimer();
      std::puts("SKYAPO_CONSUMER_CONNECTED");
      std::fflush(stdout);
      return;
    }
    timespec retry{0, 10000000};
    pw_loop_update_timer(pw_main_loop_get_loop(consumer.loop),
                         consumer.readyTimer, &retry, nullptr, false);
  }

  int run() {
    if (measureSourceLag && channels != 2)
      throw std::runtime_error(
          "source-to-consumer timestamp measurement requires stereo");
    if (expectIdenticalChannels) {
      constexpr size_t sampleCapacity = 48000 * 5;
      recordedLeft.resize(sampleCapacity);
      recordedRight.resize(sampleCapacity);
    }
    pw_init(nullptr, nullptr);
    loop = pw_main_loop_new(nullptr);
    if (!loop)
      throw std::runtime_error("cannot create PipeWire loop");
    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if (!context)
      throw std::runtime_error("cannot create PipeWire context");
    core = pw_context_connect(context, nullptr, 0);
    if (!core)
      throw std::runtime_error("cannot connect to PipeWire");

    static const pw_core_events coreEvents = [] {
      pw_core_events value{};
      value.version = PW_VERSION_CORE_EVENTS;
      value.error = coreError;
      return value;
    }();
    pw_core_add_listener(core, &coreHook, &coreEvents, this);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events registryEvents = [] {
      pw_registry_events value{};
      value.version = PW_VERSION_REGISTRY_EVENTS;
      value.global = global;
      value.global_remove = globalRemove;
      return value;
    }();
    pw_registry_add_listener(registry, &registryHook, &registryEvents, this);

    const bool mono = channels == 1;
    auto *properties = pw_properties_new(
        PW_KEY_NODE_NAME, consumerNodeName.c_str(), PW_KEY_NODE_DESCRIPTION,
        "SkyAPO PipeWire Test Consumer", PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_NODE_WANT_DRIVER, "true",
        PW_KEY_NODE_ALWAYS_PROCESS, "true", PW_KEY_NODE_PAUSE_ON_IDLE, "false",
        "wireplumber.is-virtual", "true", nullptr);
    pw_properties_setf(properties, PW_KEY_AUDIO_CHANNELS, "%u", channels);
    std::string positions = "[";
    for (unsigned channel = 0; channel < channels; ++channel)
      positions += std::string(" ") + channelNames[channel];
    positions += " ]";
    pw_properties_set(properties, SPA_KEY_AUDIO_POSITION, positions.c_str());
    filter = pw_filter_new(core, "SkyAPO Test Consumer", properties);
    if (!filter)
      throw std::runtime_error("cannot create consumer filter");
    static const pw_filter_events filterEvents = [] {
      pw_filter_events value{};
      value.version = PW_VERSION_FILTER_EVENTS;
      value.state_changed = stateChanged;
      value.param_changed = paramChanged;
      value.process = process;
      return value;
    }();
    pw_filter_add_listener(filter, &filterHook, &filterEvents, this);
    for (unsigned ch = 0; ch < channels; ++ch) {
      const auto portName = std::string("record_") + channelNames[ch];
      inputs[ch] = pw_filter_add_port(
          filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, portName.c_str(),
                            PW_KEY_AUDIO_CHANNEL, channelNames[ch], nullptr),
          nullptr, 0);
      if (!inputs[ch])
        throw std::runtime_error("cannot create consumer port");
    }
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;
    info.rate = expectedRate ? expectedRate : 48000;
    info.channels = channels;
    for (unsigned channel = 0; channel < channels; ++channel) {
      const char *name = channelNames[channel];
      if (std::strcmp(name, "MONO") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_MONO;
      else if (std::strcmp(name, "FL") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_FL;
      else if (std::strcmp(name, "FR") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_FR;
      else if (std::strcmp(name, "FC") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_FC;
      else if (std::strcmp(name, "LFE") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_LFE;
      else if (std::strcmp(name, "RL") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_RL;
      else if (std::strcmp(name, "RR") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_RR;
      else if (std::strcmp(name, "SL") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_SL;
      else if (std::strcmp(name, "SR") == 0)
        info.position[channel] = SPA_AUDIO_CHANNEL_SR;
      else
        throw std::runtime_error("unsupported test channel position");
    }
    uint8_t formatBytes[1024];
    spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(formatBytes, sizeof(formatBytes));
    std::array<float, MaxChannels> unityVolumes{};
    unityVolumes.fill(1.0f);
    const spa_pod *params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info),
        spa_format_audio_raw_build(&builder, SPA_PARAM_Format, &info),
        static_cast<spa_pod *>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
            SPA_POD_Float(1.0f), SPA_PROP_mute, SPA_POD_Bool(false),
            SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                          unityVolumes.data())))};
    if (!params[0] || !params[1] || !params[2] ||
        pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, params, 3) < 0)
      throw std::runtime_error("cannot connect consumer filter");
    ownNode = pw_filter_get_node_id(filter);
    timeout = pw_loop_add_timer(pw_main_loop_get_loop(loop), finish, this);
    if (!timeout)
      throw std::runtime_error("cannot create test timer");
    if (!signalReady && !signalConnected)
      startDurationTimer();
    if (signalReady || signalConnected) {
      readyTimer =
          pw_loop_add_timer(pw_main_loop_get_loop(loop), reportReady, this);
      if (!readyTimer)
        throw std::runtime_error("cannot create consumer readiness timer");
      timespec retry{0, 10000000};
      pw_loop_update_timer(pw_main_loop_get_loop(loop), readyTimer, &retry,
                           nullptr, false);
    }
    if (armMeasurementOnSignal) {
      measurementEnabled.store(false, std::memory_order_release);
      measurementArmRequested = 0;
      std::signal(SIGUSR1, requestMeasurementArm);
      armTimer = pw_loop_add_timer(pw_main_loop_get_loop(loop),
                                   pollMeasurementArm, this);
      if (!armTimer)
        throw std::runtime_error("cannot create measurement signal handler");
      timespec retry{0, 10000000};
      pw_loop_update_timer(pw_main_loop_get_loop(loop), armTimer, &retry,
                           nullptr, false);
    }
    pw_main_loop_run(loop);

    const auto actualFilterState = pw_filter_get_state(filter, nullptr);
    if (readyTimer)
      pw_loop_destroy_source(pw_main_loop_get_loop(loop), readyTimer);
    if (armTimer)
      pw_loop_destroy_source(pw_main_loop_get_loop(loop), armTimer);
    if (timeout)
      pw_loop_destroy_source(pw_main_loop_get_loop(loop), timeout);
    for (unsigned channel = 0; channel < channels; ++channel)
      if (links[channel]) {
        spa_hook_remove(&linkHooks[channel]);
        pw_proxy_destroy(links[channel]);
      }
    pw_filter_destroy(filter);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();

    const auto capturedFrames = frames.load();
    if (capturedFrames == 0) {
      std::fprintf(stderr,
                   "test consumer diagnostics: sourceNode=%u ownNode=%u "
                   "outputPorts=%u,%u inputPorts=%u,%u links=%p,%p "
                   "linkStates=%d,%d "
                   "failed=%d processCalls=%u invalidRate=%u zeroDuration=%u "
                   "nullBuffer=%u filterState=%d actualFilterState=%d\n",
                   sourceNode, ownNode, outputIds[0], outputIds[1], inputIds[0],
                   inputIds[1], static_cast<void *>(links[0]),
                   static_cast<void *>(links[1]),
                   static_cast<int>(linkStates[0]),
                   static_cast<int>(linkStates[1]), failed.load(),
                   processCalls.load(), invalidRateCalls.load(),
                   zeroDurationCalls.load(), nullBufferCalls.load(),
                   filterState.load(), static_cast<int>(actualFilterState));
    }
    const double rms =
        capturedFrames ? std::sqrt(energy.load() / (capturedFrames * channels))
                       : 0.0;
    const double expected =
        InputAmplitude * std::pow(10.0, expectedDb / 20.0) / std::sqrt(2.0);
    const double ratio = expected > 0 ? rms / expected : 0.0;
    auto sortedLatency = sourceConsumerLatencyUs;
    std::sort(sortedLatency.begin(),
              sortedLatency.begin() + sourceConsumerLatencyCount);
    const auto medianLatency =
        sourceConsumerLatencyCount
            ? sortedLatency[sourceConsumerLatencyCount / 2]
            : 0u;
    const auto p95Latency =
        sourceConsumerLatencyCount
            ? sortedLatency[(sourceConsumerLatencyCount - 1) * 95 / 100]
            : 0u;
    const double channelDifferenceRatio =
        expectIdenticalChannels && energy.load() > 0.0
            ? std::sqrt(channelDifferenceEnergy.load() / energy.load())
            : 0.0;
    maximumSilentFrames =
        std::max(maximumSilentFrames, consecutiveSilentFrames);
    const uint64_t maximumAllowedSilentFrames =
        static_cast<uint64_t>(sampleRate.load()) * maxConsecutiveSilenceMs /
        1000;
    const bool silenceRunMatches =
        !maxConsecutiveSilenceMs ||
        (silenceMeasurementArmed &&
         maximumSilentFrames <= maximumAllowedSilentFrames);
    int bestChannelLag = 0;
    double bestLagDifferenceRatio = channelDifferenceRatio;
    double recordedLeftRms = 0.0;
    double recordedRightRms = 0.0;
    if (expectIdenticalChannels && recordedSamples > 256) {
      // This is a test-only alignment diagnostic. Its exhaustive ±128-sample
      // lag search is quadratic in the recording length; cap its working
      // window while retaining the full capture for RMS/amplitude checks.
      // 8192 samples still spans many periods of the deterministic test tone
      // and is far longer than the plugin PDC offsets covered by this suite.
      const size_t correlationSamples = std::min<size_t>(recordedSamples, 8192);
      double bestCorrelation = -std::numeric_limits<double>::infinity();
      const auto count = correlationSamples;
      double leftEnergy = 0.0;
      double rightEnergy = 0.0;
      for (size_t i = 0; i < count; ++i) {
        leftEnergy += static_cast<double>(recordedLeft[i]) * recordedLeft[i];
        rightEnergy += static_cast<double>(recordedRight[i]) * recordedRight[i];
      }
      recordedLeftRms = std::sqrt(leftEnergy / count);
      recordedRightRms = std::sqrt(rightEnergy / count);
      for (int lag = -128; lag <= 128; ++lag) {
        const size_t begin = lag < 0 ? static_cast<size_t>(-lag) : 0;
        const size_t end = lag > 0 ? count - static_cast<size_t>(lag) : count;
        double crossEnergy = 0.0;
        double leftEnergy = 0.0;
        double rightEnergy = 0.0;
        for (size_t i = begin; i < end; ++i) {
          const auto j = static_cast<size_t>(static_cast<int64_t>(i) + lag);
          crossEnergy +=
              static_cast<double>(recordedLeft[i]) * recordedRight[j];
          leftEnergy += static_cast<double>(recordedLeft[i]) * recordedLeft[i];
          rightEnergy +=
              static_cast<double>(recordedRight[j]) * recordedRight[j];
        }
        const double correlation =
            leftEnergy > 0.0 && rightEnergy > 0.0
                ? crossEnergy / std::sqrt(leftEnergy * rightEnergy)
                : -std::numeric_limits<double>::infinity();
        if (correlation > bestCorrelation) {
          bestCorrelation = correlation;
          bestChannelLag = lag;
          bestLagDifferenceRatio =
              std::sqrt(std::max(0.0, 2.0 - 2.0 * bestCorrelation));
        }
      }
    }
    if (measureSourceLag) {
      std::printf("Captured frames: %llu\nChannels: %u\nSample rate: %u Hz\n"
                  "Timestamp-probe RMS: %.8f\n",
                  static_cast<unsigned long long>(capturedFrames), channels,
                  sampleRate.load(), rms);
    } else {
      std::printf(
          "Captured frames: %llu\nChannels: %u\nSample rate: %u Hz\nRMS: %.8f\n"
          "Expected RMS: %.8f\nRMS ratio to expected: %.6f\n",
          static_cast<unsigned long long>(capturedFrames), channels,
          sampleRate.load(), rms, expected, ratio);
    }
    if (expectIdenticalChannels)
      std::printf("Inter-channel difference ratio: %.8f\n",
                  channelDifferenceRatio);
    if (maxConsecutiveSilenceMs)
      std::printf("Longest consecutive near-silence: %llu frames (%.3f ms)\n",
                  static_cast<unsigned long long>(maximumSilentFrames),
                  sampleRate.load()
                      ? 1000.0 * maximumSilentFrames / sampleRate.load()
                      : 0.0);
    if (expectIdenticalChannels)
      std::printf("Best relative channel lag: %d samples\n"
                  "Best-lag difference ratio: %.8f\n"
                  "Left RMS: %.8f\nRight RMS: %.8f\n",
                  bestChannelLag, bestLagDifferenceRatio, recordedLeftRms,
                  recordedRightRms);
    if (measureSourceLag)
      std::printf("Source callback to virtual-consumer callback latency: "
                  "median %u us, p95 %u us, n=%zu at %u Hz\n",
                  medianLatency, p95Latency, sourceConsumerLatencyCount,
                  sampleRate.load());
    const bool audioMatches =
        acceptAnyAudio ||
        (expectSilent ? rms < 1.0e-6 : std::abs(ratio - 1.0) < 0.03);
    const bool channelsMatch =
        !expectIdenticalChannels || channelDifferenceRatio < 0.01;
    const bool latencyMatches =
        !measureSourceLag || sourceConsumerLatencyCount >= 4;
    return !failed.load() && capturedFrames > 48000 && audioMatches &&
                   channelsMatch && latencyMatches && silenceRunMatches
               ? 0
               : 1;
  }
};
} // namespace

int main(int argc, char **argv) {
  bool mono = false;
  bool surround51 = false;
  bool surround71 = false;
  bool expectSilent = false;
  bool acceptAnyAudio = false;
  bool ignoreSilentBlocks = false;
  bool expectIdenticalChannels = false;
  bool measureSourceLag = false;
  bool observeOnly = false;
  bool signalReady = false;
  bool signalConnected = false;
  bool armMeasurementOnSignal = false;
  double expectedDb = -6.0;
  unsigned expectedRate = 0;
  unsigned durationMs = 3000;
  unsigned maxConsecutiveSilenceMs = 0;
  std::string captureNode = "skyapo.virtual_mic";
  std::string consumerNode = "skyapo.test.consumer";
  for (int arg = 1; arg < argc; ++arg) {
    const std::string option(argv[arg]);
    if (option == "--mono")
      mono = true;
    else if (option == "--surround51")
      surround51 = true;
    else if (option == "--surround71")
      surround71 = true;
    else if (option == "--expect-silent")
      expectSilent = true;
    else if (option == "--accept-any-audio")
      acceptAnyAudio = true;
    else if (option == "--ignore-silent-blocks")
      ignoreSilentBlocks = true;
    else if (option == "--expect-identical-channels")
      expectIdenticalChannels = true;
    else if (option == "--measure-source-lag")
      measureSourceLag = true;
    else if (option == "--expected-db" && arg + 1 < argc) {
      char *end = nullptr;
      expectedDb = std::strtod(argv[++arg], &end);
      if (end == argv[arg] || *end || !std::isfinite(expectedDb)) {
        std::fprintf(stderr, "test consumer: invalid --expected-db value\n");
        return 2;
      }
    } else if (option == "--expected-rate" && arg + 1 < argc) {
      char *end = nullptr;
      const auto parsed = std::strtoul(argv[++arg], &end, 10);
      if (end == argv[arg] || *end || parsed == 0 || parsed > UINT32_MAX) {
        std::fprintf(stderr, "test consumer: invalid --expected-rate value\n");
        return 2;
      }
      expectedRate = static_cast<unsigned>(parsed);
    } else if (option == "--duration-ms" && arg + 1 < argc) {
      char *end = nullptr;
      const auto parsed = std::strtoul(argv[++arg], &end, 10);
      if (end == argv[arg] || *end || parsed < 100 || parsed > 60000) {
        std::fprintf(stderr, "test consumer: invalid --duration-ms value\n");
        return 2;
      }
      durationMs = static_cast<unsigned>(parsed);
    } else if (option == "--max-consecutive-silence-ms" && arg + 1 < argc) {
      char *end = nullptr;
      const auto parsed = std::strtoul(argv[++arg], &end, 10);
      if (end == argv[arg] || *end || parsed == 0 || parsed > 60000) {
        std::fprintf(stderr, "test consumer: invalid "
                             "--max-consecutive-silence-ms value\n");
        return 2;
      }
      maxConsecutiveSilenceMs = static_cast<unsigned>(parsed);
    } else if (option == "--capture-node" && arg + 1 < argc) {
      captureNode = argv[++arg];
      if (captureNode.empty()) {
        std::fprintf(stderr, "test consumer: empty --capture-node value\n");
        return 2;
      }
    } else if (option == "--name" && arg + 1 < argc) {
      consumerNode = argv[++arg];
      if (consumerNode.empty()) {
        std::fprintf(stderr, "test consumer: empty --name value\n");
        return 2;
      }
    } else if (option == "--observe-only") {
      observeOnly = true;
    } else if (option == "--signal-ready") {
      signalReady = true;
    } else if (option == "--signal-connected") {
      signalConnected = true;
    } else if (option == "--arm-on-signal") {
      armMeasurementOnSignal = true;
    } else {
      std::fprintf(stderr, "usage: skyapo-pipewire-test-consumer "
                           "[--mono|--surround51|--surround71] "
                           "[--expect-silent] [--accept-any-audio] "
                           "[--ignore-silent-blocks] "
                           "[--expect-identical-channels] "
                           "[--measure-source-lag] "
                           "[--expected-db DB] [--expected-rate HZ] "
                           "[--duration-ms MS] "
                           "[--max-consecutive-silence-ms MS] "
                           "[--capture-node NAME] "
                           "[--name NODE-NAME] "
                           "[--observe-only] [--signal-ready] "
                           "[--signal-connected] [--arm-on-signal]\n");
      return 2;
    }
  }
  try {
    if (static_cast<unsigned>(mono) + static_cast<unsigned>(surround51) +
            static_cast<unsigned>(surround71) >
        1) {
      std::fprintf(stderr, "test consumer: choose only one channel layout\n");
      return 2;
    }
    Consumer consumer(mono, surround51, surround71, expectSilent,
                      acceptAnyAudio, ignoreSilentBlocks, expectedDb,
                      expectedRate, durationMs, std::move(captureNode),
                      maxConsecutiveSilenceMs, std::move(consumerNode),
                      observeOnly, signalReady, signalConnected,
                      armMeasurementOnSignal);
    if (expectIdenticalChannels && mono) {
      std::fprintf(
          stderr,
          "test consumer: --expect-identical-channels requires stereo\n");
      return 2;
    }
    consumer.expectIdenticalChannels = expectIdenticalChannels;
    consumer.measureSourceLag = measureSourceLag;
    return consumer.run();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "test consumer: %s\n", error.what());
    return 1;
  }
}
