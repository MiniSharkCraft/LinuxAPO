// Deterministic PipeWire source for opt-in end-to-end/reconnect tests.
#include <pipewire/filter.h>
#include <pipewire/keys.h>
#include <pipewire/link.h>
#include <pipewire/pipewire.h>
#include <pipewire/proxy.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <time.h>
#include <utility>

namespace {
constexpr unsigned MaxChannels = 8;
constexpr std::array<const char *, MaxChannels> Surround51 = {
    "FL", "FR", "FC", "LFE", "RL", "RR", "", ""};
constexpr std::array<const char *, MaxChannels> Surround71 = {
    "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
struct Source {
  pw_main_loop *loop{};
  pw_filter *filter{};
  unsigned channels{2};
  std::string nodeName;
  std::string deviceSerial;
  std::string linkNodeName;
  bool playbackStream{};
  pw_core *core{};
  spa_hook coreHook{};
  pw_registry *registry{};
  spa_hook registryHook{};
  uint32_t ownNode{SPA_ID_INVALID};
  uint32_t targetNode{SPA_ID_INVALID};
  std::array<void *, MaxChannels> ports{};
  std::array<uint32_t, MaxChannels> ownPortIds{
      SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID,
      SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID};
  std::array<uint32_t, MaxChannels> targetPortIds{
      SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID,
      SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID, SPA_ID_INVALID};
  std::array<pw_proxy *, MaxChannels> links{};
  std::array<const char *, MaxChannels> channelNames{};
  double phase{};
  bool latencyProbe{};
  bool pdcTriggerStaging{};
  std::atomic<unsigned> pdcSignalStage{0};
  std::atomic<bool> signalEnabled{true};
  bool failed{};

  static void signal(void *data, int signalNumber) {
    auto &source = *static_cast<Source *>(data);
    if (source.pdcTriggerStaging && signalNumber == SIGUSR2) {
      source.pdcSignalStage.store(1, std::memory_order_relaxed);
    } else if (source.pdcTriggerStaging && signalNumber == SIGUSR1) {
      source.pdcSignalStage.store(2, std::memory_order_relaxed);
    } else if (signalNumber == SIGUSR1) {
      source.signalEnabled.store(true, std::memory_order_relaxed);
    } else {
      pw_main_loop_quit(source.loop);
    }
  }

  static void coreError(void *, uint32_t, int, int result,
                        const char *message) {
    fprintf(stderr, "test source: PipeWire error %d: %s\n", result,
            message ? message : "unknown error");
  }

  static void stateChanged(void *data, pw_filter_state old,
                           pw_filter_state state, const char *error) {
    auto &source = *static_cast<Source *>(data);
    fprintf(stderr, "test source: filter state %s -> %s%s%s\n",
            pw_filter_state_as_string(old), pw_filter_state_as_string(state),
            error ? ": " : "", error ? error : "");
    if (state == PW_FILTER_STATE_ERROR ||
        state == PW_FILTER_STATE_UNCONNECTED) {
      if (state == PW_FILTER_STATE_ERROR)
        fprintf(stderr, "test source: %s\n", error ? error : "PipeWire error");
      source.failed = state == PW_FILTER_STATE_ERROR;
      pw_main_loop_quit(source.loop);
    }
  }

  static void paramChanged(void *data, void *port, uint32_t id,
                           const spa_pod *param) {
    auto &source = *static_cast<Source *>(data);
    if (port || !param)
      return;
    if (id == SPA_PARAM_Format) {
      spa_audio_info_raw info{};
      if (spa_format_audio_raw_parse(param, &info) < 0 ||
          info.channels != source.channels ||
          (info.format != SPA_AUDIO_FORMAT_F32P &&
           info.format != SPA_AUDIO_FORMAT_F32)) {
        fprintf(stderr, "test source: unsupported format request\n");
        source.failed = true;
        pw_main_loop_quit(source.loop);
        return;
      }
    }
    pw_filter_update_params(source.filter, nullptr, &param, 1);
  }

  static void process(void *data, spa_io_position *position) {
    auto &source = *static_cast<Source *>(data);
    const uint32_t frames = position->clock.duration;
    std::array<float *, MaxChannels> buffers{};
    for (unsigned c = 0; c < source.channels; ++c) {
      buffers[c] = static_cast<float *>(
          pw_filter_get_dsp_buffer(source.ports[c], frames));
      if (!buffers[c])
        return;
    }
    constexpr double twoPi = 6.283185307179586476925286766559;
    constexpr double frequency = 440.0;
    const double rate = position->clock.rate.num
                            ? static_cast<double>(position->clock.rate.denom) /
                                  position->clock.rate.num
                            : 48000.0;
    const double step = twoPi * frequency / rate;
    const bool signalEnabled =
        source.signalEnabled.load(std::memory_order_relaxed);
    const unsigned pdcSignalStage =
        source.pdcSignalStage.load(std::memory_order_relaxed);
    uint64_t callbackUs = 0;
    if (source.latencyProbe) {
      timespec now{};
      clock_gettime(CLOCK_MONOTONIC, &now);
      callbackUs = static_cast<uint64_t>(now.tv_sec) * 1000000u +
                   static_cast<uint64_t>(now.tv_nsec) / 1000u;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      if (source.latencyProbe) {
        const uint64_t graphFrame = position->clock.position + i;
        if (source.channels == 2 && graphFrame % 8192u == 0) {
          const uint32_t timestampUs = static_cast<uint32_t>(
              callbackUs + static_cast<uint64_t>(i * 1000000.0 / rate));
          buffers[0][i] =
              0.25f +
              0.25f * static_cast<float>(timestampUs & 0xffffu) / 65536.0f;
          buffers[1][i] =
              0.25f + 0.25f * static_cast<float>(timestampUs >> 16) / 65536.0f;
        } else {
          buffers[0][i] = 0.0f;
          buffers[1][i] = 0.0f;
        }
        continue;
      }
      const float signal = static_cast<float>(0.1 * std::sin(source.phase));
      bool advancePhase = false;
      for (unsigned c = 0; c < source.channels; ++c) {
        const bool emitSignal =
            source.pdcTriggerStaging
                ? (pdcSignalStage == 0
                       ? std::strcmp(source.channelNames[c], "FR") == 0
                       : pdcSignalStage == 2)
                : signalEnabled;
        buffers[c][i] = emitSignal ? signal : 0.0f;
        advancePhase = advancePhase || emitSignal;
      }
      if (advancePhase) {
        source.phase += step;
        if (source.phase >= twoPi)
          source.phase -= twoPi;
      }
    }
  }

  explicit Source(unsigned channelCount, std::string name, std::string serial,
                  std::string destination,
                  std::array<const char *, MaxChannels> positions, bool probe,
                  bool asPlaybackStream, bool startSilent,
                  bool usePdcTriggerStaging)
      : channels(channelCount), nodeName(std::move(name)),
        deviceSerial(std::move(serial)), linkNodeName(std::move(destination)),
        playbackStream(asPlaybackStream), channelNames(positions),
        latencyProbe(probe), pdcTriggerStaging(usePdcTriggerStaging),
        signalEnabled(!startSilent) {}

  void createLink(unsigned channel) {
    if (channel >= channels || targetNode == SPA_ID_INVALID ||
        ownPortIds[channel] == SPA_ID_INVALID ||
        targetPortIds[channel] == SPA_ID_INVALID || links[channel])
      return;
    auto *properties = pw_properties_new(nullptr, nullptr);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u", ownNode);
    pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u",
                       ownPortIds[channel]);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u", targetNode);
    pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u",
                       targetPortIds[channel]);
    links[channel] = reinterpret_cast<pw_proxy *>(
        pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                              PW_VERSION_LINK, &properties->dict, 0));
    pw_properties_free(properties);
    if (!links[channel]) {
      failed = true;
      fprintf(stderr, "test source: cannot link channel %s to %s\n",
              channelNames[channel], linkNodeName.c_str());
      pw_main_loop_quit(loop);
    }
  }

  static void global(void *data, uint32_t id, uint32_t, const char *type,
                     uint32_t, const spa_dict *properties) {
    auto &source = *static_cast<Source *>(data);
    if (!properties)
      return;
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const auto *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
      if (name && source.nodeName == name)
        source.ownNode = id;
      if (name && source.linkNodeName == name) {
        source.targetNode = id;
        fprintf(stderr, "test source: target %s id=%u own=%u\n", name, id,
                source.ownNode);
      }
      for (unsigned channel = 0; channel < source.channels; ++channel)
        source.createLink(channel);
      return;
    }
    if (std::strcmp(type, PW_TYPE_INTERFACE_Port) != 0)
      return;
    const auto *nodeText = spa_dict_lookup(properties, PW_KEY_NODE_ID);
    const auto *direction = spa_dict_lookup(properties, PW_KEY_PORT_DIRECTION);
    const auto *channelName = spa_dict_lookup(properties, PW_KEY_AUDIO_CHANNEL);
    if (!nodeText || !direction || !channelName)
      return;
    char *end = nullptr;
    const auto node = std::strtoul(nodeText, &end, 10);
    if (!end || *end)
      return;
    for (unsigned channel = 0; channel < source.channels; ++channel) {
      if (std::strcmp(channelName, source.channelNames[channel]) != 0)
        continue;
      if (node == source.ownNode && std::strcmp(direction, "out") == 0)
        source.ownPortIds[channel] = id;
      else if (node == source.targetNode && std::strcmp(direction, "in") == 0)
        source.targetPortIds[channel] = id;
      else
        continue;
      fprintf(stderr,
              "test source: port node=%lu channel=%s direction=%s id=%u\n",
              node, channelName, direction, id);
      source.createLink(channel);
    }
  }

  int run() {
    pw_init(nullptr, nullptr);
    loop = pw_main_loop_new(nullptr);
    if (!loop)
      throw std::runtime_error("cannot create PipeWire loop");
    auto *context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if (!context)
      throw std::runtime_error("cannot create PipeWire context");
    core = pw_context_connect(context, nullptr, 0);
    if (!core) {
      pw_context_destroy(context);
      throw std::runtime_error("cannot connect to PipeWire");
    }
    static const pw_core_events coreEvents = [] {
      pw_core_events value{};
      value.version = PW_VERSION_CORE_EVENTS;
      value.error = coreError;
      return value;
    }();
    pw_core_add_listener(core, &coreHook, &coreEvents, this);
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, signal, this);
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, signal, this);
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGUSR1, signal, this);
    if (pdcTriggerStaging)
      pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGUSR2, signal, this);
    registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    static const pw_registry_events registryEvents = [] {
      pw_registry_events value{};
      value.version = PW_VERSION_REGISTRY_EVENTS;
      value.global = global;
      return value;
    }();
    pw_registry_add_listener(registry, &registryHook, &registryEvents, this);
    const bool mono = channels == 1;
    const char *description = nodeName == "skyapo.test.mono"
                                  ? "SkyAPO Deterministic Mono Input"
                                  : "SkyAPO Deterministic Test Input";
    std::string positions = "[";
    for (unsigned channel = 0; channel < channels; ++channel)
      positions += std::string(" ") + channelNames[channel];
    positions += " ]";
    auto *properties = pw_properties_new(
        PW_KEY_NODE_NAME, nodeName.c_str(), PW_KEY_NODE_DESCRIPTION,
        playbackStream ? "SkyAPO Test Playback Application" : description,
        PW_KEY_MEDIA_CLASS,
        playbackStream ? "Stream/Output/Audio" : "Audio/Source",
        PW_KEY_NODE_VIRTUAL, playbackStream ? "false" : "true",
        PW_KEY_NODE_WANT_DRIVER, playbackStream ? "false" : "true",
        PW_KEY_NODE_ALWAYS_PROCESS, "true", PW_KEY_NODE_PAUSE_ON_IDLE, "false",
        nullptr);
    if (!playbackStream)
      pw_properties_set(properties, "wireplumber.is-virtual", "true");
    if (playbackStream) {
      pw_properties_set(properties, "media.category", "Playback");
      pw_properties_set(properties, PW_KEY_NODE_AUTOCONNECT, "true");
      pw_properties_set(properties, "node.dont-fallback", "true");
      pw_properties_set(properties, "node.dont-move", "true");
    }
    if (!deviceSerial.empty())
      pw_properties_set(properties, "device.serial", deviceSerial.c_str());
    pw_properties_setf(properties, PW_KEY_AUDIO_CHANNELS, "%u", channels);
    pw_properties_set(properties, SPA_KEY_AUDIO_POSITION, positions.c_str());
    filter = pw_filter_new(core, "SkyAPO Test Input", properties);
    if (!filter) {
      pw_core_disconnect(core);
      pw_context_destroy(context);
      throw std::runtime_error("cannot create source filter");
    }
    static const pw_filter_events events = [] {
      pw_filter_events value{};
      value.version = PW_VERSION_FILTER_EVENTS;
      value.state_changed = stateChanged;
      value.param_changed = paramChanged;
      value.process = process;
      return value;
    }();
    spa_hook hook{};
    pw_filter_add_listener(filter, &hook, &events, this);
    for (unsigned c = 0; c < channels; ++c) {
      const auto portName = std::string("capture_") + channelNames[c];
      ports[c] = pw_filter_add_port(
          filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, portName.c_str(),
                            PW_KEY_AUDIO_CHANNEL, channelNames[c], nullptr),
          nullptr, 0);
    }
    bool missingPort = false;
    for (unsigned c = 0; c < channels; ++c)
      missingPort = missingPort || !ports[c];
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;
    info.rate = 48000;
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
    if (missingPort || !params[0] || !params[1] || !params[2] ||
        pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, params, 3) < 0) {
      pw_filter_destroy(filter);
      pw_core_disconnect(core);
      pw_context_destroy(context);
      throw std::runtime_error("cannot publish test source");
    }
    ownNode = pw_filter_get_node_id(filter);
    pw_main_loop_run(loop);
    for (auto *link : links)
      if (link)
        pw_proxy_destroy(link);
    pw_filter_destroy(filter);
    spa_hook_remove(&registryHook);
    spa_hook_remove(&coreHook);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();
    return failed ? 1 : 0;
  }
};
} // namespace

int main(int argc, char **argv) {
  try {
    unsigned channels = 2;
    std::string name = "skyapo.test.input";
    std::string serial;
    std::string linkNode;
    bool latencyProbe = false;
    bool playbackStream = false;
    bool startSilent = false;
    bool pdcTriggerStaging = false;
    auto channelNames = std::array<const char *, MaxChannels>{
        "FL", "FR", "", "", "", "", "", ""};
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--mono") {
        channels = 1;
        name = "skyapo.test.mono";
        channelNames = {"MONO", "", "", "", "", "", "", ""};
      } else if (arg == "--surround51") {
        channels = 6;
        name = "skyapo.test.surround51";
        channelNames = Surround51;
      } else if (arg == "--surround71") {
        channels = 8;
        name = "skyapo.test.surround71";
        channelNames = Surround71;
      } else if (arg == "--name" && i + 1 < argc) {
        name = argv[++i];
      } else if (arg == "--serial" && i + 1 < argc) {
        serial = argv[++i];
      } else if (arg == "--link-node" && i + 1 < argc) {
        linkNode = argv[++i];
      } else if (arg == "--latency-probe") {
        latencyProbe = true;
      } else if (arg == "--playback-stream") {
        playbackStream = true;
      } else if (arg == "--start-silent") {
        startSilent = true;
      } else if (arg == "--pdc-trigger-staging") {
        pdcTriggerStaging = true;
      } else {
        fprintf(stderr, "usage: skyapo-pipewire-test-source "
                        "[--mono|--surround51|--surround71] "
                        "[--name NODE_NAME] [--serial DEVICE_SERIAL] "
                        "[--link-node NODE_NAME] "
                        "[--playback-stream] [--latency-probe] "
                        "[--start-silent] [--pdc-trigger-staging]\n");
        return 2;
      }
    }
    Source source(channels, std::move(name), std::move(serial),
                  std::move(linkNode), channelNames, latencyProbe,
                  playbackStream, startSilent, pdcTriggerStaging);
    return source.run();
  } catch (const std::exception &error) {
    fprintf(stderr, "test source: %s\n", error.what());
    return 1;
  }
}
