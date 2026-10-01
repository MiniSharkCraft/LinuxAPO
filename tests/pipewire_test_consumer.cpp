// Native PipeWire client used to consume and numerically verify the test mic.
// --expect-silent accepts near-zero RMS for intentional plugin-failure tests.
#include <pipewire/filter.h>
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr uint32_t Invalid = SPA_ID_INVALID;
constexpr double InputAmplitude = 0.1;

struct Consumer {
  pw_main_loop* loop{};
  pw_context* context{};
  pw_core* core{};
  pw_registry* registry{};
  pw_filter* filter{};
  spa_hook coreHook{}, registryHook{}, filterHook{};
  spa_source* timeout{};
  unsigned channels{2};
  bool expectSilent{};
  bool acceptAnyAudio{};
  double expectedDb{-6.0};
  unsigned expectedRate{};
  std::array<const char*, 2> channelNames{};
  std::array<void*, 2> inputs{};
  std::array<uint32_t, 2> inputIds{Invalid, Invalid};
  std::array<uint32_t, 2> outputIds{Invalid, Invalid};
  std::array<pw_proxy*, 2> links{};
  uint32_t sourceNode{Invalid};
  uint32_t ownNode{Invalid};
  std::atomic<uint64_t> frames{0};
  std::atomic<unsigned> sampleRate{0};
  std::atomic<double> energy{0.0};
  std::atomic<double> channelDifferenceEnergy{0.0};
  bool expectIdenticalChannels{};
  std::atomic<unsigned> processCalls{0};
  std::atomic<unsigned> invalidRateCalls{0};
  std::atomic<unsigned> zeroDurationCalls{0};
  std::atomic<unsigned> nullBufferCalls{0};
  std::atomic<int> filterState{PW_FILTER_STATE_UNCONNECTED};
  std::vector<float> recordedLeft{};
  std::vector<float> recordedRight{};
  size_t recordedSamples{};
  std::atomic<bool> failed{false};

  explicit Consumer(bool mono, bool silent, bool anyAudio, double gainDb,
                    unsigned rate)
      : channels(mono ? 1 : 2), expectSilent(silent), acceptAnyAudio(anyAudio),
        expectedDb(gainDb), expectedRate(rate),
        channelNames(mono ? std::array<const char *, 2>{"MONO", ""}
                          : std::array<const char *, 2>{"FL", "FR"}) {}

  static unsigned channelIndex(const char* channel, const Consumer& c) {
    for (unsigned i = 0; i < c.channels; ++i)
      if (std::strcmp(channel, c.channelNames[i]) == 0) return i;
    return c.channels;
  }

  void linkChannel(unsigned channel) {
    if (channel >= channels || sourceNode == Invalid || ownNode == Invalid ||
        outputIds[channel] == Invalid || inputIds[channel] == Invalid ||
        links[channel])
      return;
    auto* properties = pw_properties_new(nullptr, nullptr);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", sourceNode);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       outputIds[channel]);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", ownNode);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       inputIds[channel]);
    links[channel] = reinterpret_cast<pw_proxy*>(
        pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                              PW_VERSION_LINK, &properties->dict, 0));
    pw_properties_free(properties);
    if (!links[channel]) {
      failed.store(true, std::memory_order_relaxed);
      std::fprintf(stderr, "test consumer: cannot link channel %s\n",
                   channelNames[channel]);
      pw_main_loop_quit(loop);
    }
  }

  static void global(void* data, uint32_t id, uint32_t, const char* type,
                     uint32_t, const spa_dict* props) {
    auto& c = *static_cast<Consumer*>(data);
    if (!props) return;
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (name && std::strcmp(name, "skyapo.virtual_mic") == 0)
        c.sourceNode = id;
      else if (name && std::strcmp(name, "skyapo.test.consumer") == 0)
        c.ownNode = id;
      else
        return;
      for (unsigned ch = 0; ch < c.channels; ++ch) c.linkChannel(ch);
      return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0) return;
    const char* nodeValue = spa_dict_lookup(props, PW_KEY_NODE_ID);
    const char* direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
    const char* channel = spa_dict_lookup(props, PW_KEY_AUDIO_CHANNEL);
    if (!nodeValue || !direction || !channel) return;
    char* end = nullptr;
    const auto nodeId = std::strtoul(nodeValue, &end, 10);
    if (!end || *end) return;
    const unsigned index = channelIndex(channel, c);
    if (index >= c.channels) return;
    if (nodeId == c.ownNode && std::strcmp(direction, "in") == 0)
      c.inputIds[index] = id;
    else if (nodeId == c.sourceNode && std::strcmp(direction, "out") == 0)
      c.outputIds[index] = id;
    else
      return;
    c.linkChannel(index);
  }

  static void globalRemove(void*, uint32_t) {}

  static void coreError(void* data, uint32_t, int, int result,
                        const char* message) {
    auto& c = *static_cast<Consumer*>(data);
    c.failed.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "test consumer: PipeWire error %d: %s\n", result,
                 message ? message : "unknown error");
    pw_main_loop_quit(c.loop);
  }

  static void stateChanged(void *data, pw_filter_state, pw_filter_state state,
                           const char *error) {
    auto& c = *static_cast<Consumer*>(data);
    c.filterState.store(static_cast<int>(state), std::memory_order_relaxed);
    if (state != PW_FILTER_STATE_ERROR)
      return;
    c.failed.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "test consumer: %s\n",
                 error ? error : "PipeWire filter error");
    pw_main_loop_quit(c.loop);
  }

  static void process(void* data, spa_io_position* position) {
    auto& c = *static_cast<Consumer*>(data);
    c.processCalls.fetch_add(1, std::memory_order_relaxed);
    const auto rate = position->clock.rate.num
                          ? position->clock.rate.denom / position->clock.rate.num
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
    std::array<float *, 2> channelData{};
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
    if (c.expectIdenticalChannels) {
      for (uint32_t i = 0; i < count; ++i) {
        const double difference =
            static_cast<double>(channelData[0][i]) - channelData[1][i];
        blockChannelDifferenceEnergy += difference * difference;
      }
      const auto remaining = c.recordedLeft.size() - c.recordedSamples;
      const auto copyCount = std::min<size_t>(count, remaining);
      std::copy_n(channelData[0], copyCount,
                  c.recordedLeft.data() + c.recordedSamples);
      std::copy_n(channelData[1], copyCount,
                  c.recordedRight.data() + c.recordedSamples);
      c.recordedSamples += copyCount;
    }
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

  static void finish(void* data, uint64_t) {
    pw_main_loop_quit(static_cast<Consumer*>(data)->loop);
  }

  int run() {
    if (expectIdenticalChannels) {
      constexpr size_t sampleCapacity = 48000 * 5;
      recordedLeft.resize(sampleCapacity);
      recordedRight.resize(sampleCapacity);
    }
    pw_init(nullptr, nullptr);
    loop = pw_main_loop_new(nullptr);
    if (!loop) throw std::runtime_error("cannot create PipeWire loop");
    context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if (!context) throw std::runtime_error("cannot create PipeWire context");
    core = pw_context_connect(context, nullptr, 0);
    if (!core) throw std::runtime_error("cannot connect to PipeWire");

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
    auto* properties = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.test.consumer", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO PipeWire Test Consumer", PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true",
        PW_KEY_NODE_PAUSE_ON_IDLE, "false", nullptr);
    pw_properties_setf(properties, PW_KEY_AUDIO_CHANNELS, "%u", channels);
    pw_properties_set(properties, SPA_KEY_AUDIO_POSITION,
                      mono ? "[ MONO ]" : "[ FL FR ]");
    filter = pw_filter_new(core, "SkyAPO Test Consumer", properties);
    if (!filter) throw std::runtime_error("cannot create consumer filter");
    static const pw_filter_events filterEvents = [] {
      pw_filter_events value{};
      value.version = PW_VERSION_FILTER_EVENTS;
      value.state_changed = stateChanged;
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
      if (!inputs[ch]) throw std::runtime_error("cannot create consumer port");
    }
    if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
      throw std::runtime_error("cannot connect consumer filter");
    ownNode = pw_filter_get_node_id(filter);
    timeout = pw_loop_add_timer(pw_main_loop_get_loop(loop), finish, this);
    if (!timeout) throw std::runtime_error("cannot create test timer");
    timespec duration{3, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(loop), timeout, &duration,
                         nullptr, false);
    pw_main_loop_run(loop);

    const auto actualFilterState = pw_filter_get_state(filter, nullptr);
    for (auto* link : links)
      if (link) pw_proxy_destroy(link);
    pw_filter_destroy(filter);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();

    const auto capturedFrames = frames.load();
    if (capturedFrames == 0) {
      std::fprintf(stderr,
                   "test consumer diagnostics: sourceNode=%u ownNode=%u "
                   "outputPorts=%u,%u inputPorts=%u,%u links=%p,%p "
                   "failed=%d processCalls=%u invalidRate=%u zeroDuration=%u "
                   "nullBuffer=%u filterState=%d actualFilterState=%d\n",
                   sourceNode, ownNode, outputIds[0], outputIds[1], inputIds[0],
                   inputIds[1], static_cast<void *>(links[0]),
                   static_cast<void *>(links[1]), failed.load(),
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
    const double channelDifferenceRatio =
        expectIdenticalChannels && energy.load() > 0.0
            ? std::sqrt(channelDifferenceEnergy.load() / energy.load())
            : 0.0;
    int bestChannelLag = 0;
    double bestLagDifferenceRatio = channelDifferenceRatio;
    double recordedLeftRms = 0.0;
    double recordedRightRms = 0.0;
    if (expectIdenticalChannels && recordedSamples > 256) {
      double bestCorrelation = -std::numeric_limits<double>::infinity();
      const auto count = recordedSamples;
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
    std::printf(
        "Captured frames: %llu\nChannels: %u\nSample rate: %u Hz\nRMS: %.8f\n"
        "Expected RMS: %.8f\nRMS ratio to expected: %.6f\n",
        static_cast<unsigned long long>(capturedFrames), channels,
        sampleRate.load(), rms,
        expected, ratio);
    if (expectIdenticalChannels)
      std::printf("Inter-channel difference ratio: %.8f\n",
                  channelDifferenceRatio);
    if (expectIdenticalChannels)
      std::printf("Best relative channel lag: %d samples\n"
                  "Best-lag difference ratio: %.8f\n"
                  "Left RMS: %.8f\nRight RMS: %.8f\n",
                  bestChannelLag, bestLagDifferenceRatio, recordedLeftRms,
                  recordedRightRms);
    const bool audioMatches =
        acceptAnyAudio ||
        (expectSilent ? rms < 1.0e-6 : std::abs(ratio - 1.0) < 0.03);
    const bool channelsMatch =
        !expectIdenticalChannels || channelDifferenceRatio < 0.01;
    return !failed.load() && capturedFrames > 48000 && audioMatches &&
                   channelsMatch
               ? 0
               : 1;
  }
};
}  // namespace

int main(int argc, char** argv) {
  bool mono = false;
  bool expectSilent = false;
  bool acceptAnyAudio = false;
  bool expectIdenticalChannels = false;
  double expectedDb = -6.0;
  unsigned expectedRate = 0;
  for (int arg = 1; arg < argc; ++arg) {
    const std::string option(argv[arg]);
    if (option == "--mono")
      mono = true;
    else if (option == "--expect-silent")
      expectSilent = true;
    else if (option == "--accept-any-audio")
      acceptAnyAudio = true;
    else if (option == "--expect-identical-channels")
      expectIdenticalChannels = true;
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
    } else {
      std::fprintf(stderr, "usage: skyapo-pipewire-test-consumer [--mono] "
                           "[--expect-silent] [--accept-any-audio] "
                           "[--expect-identical-channels] "
                           "[--expected-db DB] [--expected-rate HZ]\n");
      return 2;
    }
  }
  try {
    Consumer consumer(mono, expectSilent, acceptAnyAudio, expectedDb,
                      expectedRate);
    if (expectIdenticalChannels && mono) {
      std::fprintf(
          stderr,
          "test consumer: --expect-identical-channels requires stereo\n");
      return 2;
    }
    consumer.expectIdenticalChannels = expectIdenticalChannels;
    return consumer.run();
  } catch (const std::exception& error) {
    std::fprintf(stderr, "test consumer: %s\n", error.what());
    return 1;
  }
}
