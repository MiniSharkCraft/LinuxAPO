// Deterministic PipeWire source for opt-in end-to-end/reconnect tests.
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>

#include <array>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
struct Source {
  pw_main_loop* loop{};
  pw_filter* filter{};
  unsigned channels{2};
  std::string nodeName;
  std::array<void*, 2> ports{};
  double phase{};
  bool failed{};

  static void signal(void* data, int) {
    auto& source = *static_cast<Source*>(data);
    pw_main_loop_quit(source.loop);
  }

  static void stateChanged(void* data, pw_filter_state, pw_filter_state state,
                           const char* error) {
    auto& source = *static_cast<Source*>(data);
    if (state == PW_FILTER_STATE_ERROR ||
        state == PW_FILTER_STATE_UNCONNECTED) {
      if (state == PW_FILTER_STATE_ERROR)
        fprintf(stderr, "test source: %s\n", error ? error : "PipeWire error");
      source.failed = state == PW_FILTER_STATE_ERROR;
      pw_main_loop_quit(source.loop);
    }
  }

  static void process(void* data, spa_io_position* position) {
    auto& source = *static_cast<Source*>(data);
    const uint32_t frames = position->clock.duration;
    std::array<float*, 2> buffers{};
    for (unsigned c = 0; c < source.channels; ++c) {
      buffers[c] = static_cast<float*>(
          pw_filter_get_dsp_buffer(source.ports[c], frames));
      if (!buffers[c]) return;
    }
    constexpr double twoPi = 6.283185307179586476925286766559;
    constexpr double frequency = 440.0;
    const double rate = position->clock.rate.num
                            ? static_cast<double>(position->clock.rate.denom) /
                                  position->clock.rate.num
                            : 48000.0;
    const double step = twoPi * frequency / rate;
    for (uint32_t i = 0; i < frames; ++i) {
      const float sample = static_cast<float>(0.1 * std::sin(source.phase));
      source.phase += step;
      if (source.phase >= twoPi) source.phase -= twoPi;
      for (unsigned c = 0; c < source.channels; ++c) buffers[c][i] = sample;
    }
  }

  explicit Source(unsigned channelCount, std::string name)
      : channels(channelCount), nodeName(std::move(name)) {}

  int run() {
    pw_init(nullptr, nullptr);
    loop = pw_main_loop_new(nullptr);
    if (!loop) throw std::runtime_error("cannot create PipeWire loop");
    auto* context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
    if (!context) throw std::runtime_error("cannot create PipeWire context");
    auto* core = pw_context_connect(context, nullptr, 0);
    if (!core) {
      pw_context_destroy(context);
      throw std::runtime_error("cannot connect to PipeWire");
    }
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, signal, this);
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, signal, this);
    const bool mono = channels == 1;
    const char* description = nodeName == "skyapo.test.mono"
                                  ? "SkyAPO Deterministic Mono Input"
                                  : "SkyAPO Deterministic Test Input";
    const char* positions = mono ? "[ MONO ]" : "[ FL FR ]";
    auto* properties =
        pw_properties_new(PW_KEY_NODE_NAME, nodeName.c_str(),
                          PW_KEY_NODE_DESCRIPTION, description,
                          PW_KEY_MEDIA_CLASS, "Audio/Source", PW_KEY_NODE_VIRTUAL,
                          "true", PW_KEY_NODE_WANT_DRIVER, "true",
                          PW_KEY_NODE_PAUSE_ON_IDLE, "false", nullptr);
    pw_properties_setf(properties, PW_KEY_AUDIO_CHANNELS, "%u", channels);
    pw_properties_set(properties, SPA_KEY_AUDIO_POSITION, positions);
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
      value.process = process;
      return value;
    }();
    spa_hook hook{};
    pw_filter_add_listener(filter, &hook, &events, this);
    const std::array<const char*, 2> channelNames = {mono ? "MONO" : "FL",
                                                     "FR"};
    for (unsigned c = 0; c < channels; ++c) {
      const auto portName = std::string("capture_") + channelNames[c];
      ports[c] = pw_filter_add_port(
          filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
          pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                            PW_KEY_PORT_NAME, portName.c_str(),
                            PW_KEY_AUDIO_CHANNEL, channelNames[c], nullptr),
          nullptr, 0);
    }
    if (!ports[0] || (channels == 2 && !ports[1]) ||
        pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0) {
      pw_filter_destroy(filter);
      pw_core_disconnect(core);
      pw_context_destroy(context);
      throw std::runtime_error("cannot publish test source");
    }
    pw_main_loop_run(loop);
    pw_filter_destroy(filter);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);
    pw_deinit();
    return failed ? 1 : 0;
  }
};
}  // namespace

int main(int argc, char** argv) {
  try {
    unsigned channels = 2;
    std::string name = "skyapo.test.input";
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--mono") {
        channels = 1;
        name = "skyapo.test.mono";
      } else if (arg == "--name" && i + 1 < argc) {
        name = argv[++i];
      } else {
        fprintf(stderr, "usage: skyapo-pipewire-test-source [--mono] "
                        "[--name NODE_NAME]\n");
        return 2;
      }
    }
    Source source(channels, std::move(name));
    return source.run();
  } catch (const std::exception& error) {
    fprintf(stderr, "test source: %s\n", error.what());
    return 1;
  }
}
