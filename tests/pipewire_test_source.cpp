// Deterministic PipeWire source for opt-in end-to-end/reconnect tests.
#include <cmath>
#include <csignal>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>
#include <spa/param/props.h>
#include <stdexcept>

namespace {
volatile sig_atomic_t stopping = 0;

struct Source {
  pw_main_loop *loop{};
  pw_filter *filter{};
  void *left{};
  void *right{};
  double phase{};
  bool failed{};

  static void signal(void *data, int) {
    auto &source = *static_cast<Source *>(data);
    stopping = 1;
    pw_main_loop_quit(source.loop);
  }

  static void stateChanged(void *data, pw_filter_state, pw_filter_state state,
                           const char *error) {
    auto &source = *static_cast<Source *>(data);
    if (state == PW_FILTER_STATE_ERROR || state == PW_FILTER_STATE_UNCONNECTED) {
      if (state == PW_FILTER_STATE_ERROR)
        fprintf(stderr, "test source: %s\n", error ? error : "PipeWire error");
      source.failed = state == PW_FILTER_STATE_ERROR;
      pw_main_loop_quit(source.loop);
    }
  }

  static void process(void *data, spa_io_position *position) {
    auto &source = *static_cast<Source *>(data);
    const uint32_t frames = position->clock.duration;
    auto *left = static_cast<float *>(pw_filter_get_dsp_buffer(source.left, frames));
    auto *right = static_cast<float *>(pw_filter_get_dsp_buffer(source.right, frames));
    if (!left || !right)
      return;
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
      if (source.phase >= twoPi)
        source.phase -= twoPi;
      left[i] = sample;
      right[i] = sample;
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
    auto *core = pw_context_connect(context, nullptr, 0);
    if (!core) {
      pw_context_destroy(context);
      throw std::runtime_error("cannot connect to PipeWire");
    }
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, signal, this);
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, signal, this);
    auto *properties = pw_properties_new(
        PW_KEY_NODE_NAME, "skyapo.test.input", PW_KEY_NODE_DESCRIPTION,
        "SkyAPO Deterministic Test Input", PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_WANT_DRIVER, "true",
        PW_KEY_NODE_PAUSE_ON_IDLE, "false", PW_KEY_AUDIO_CHANNELS, "2",
        SPA_KEY_AUDIO_POSITION, "[ FL FR ]", nullptr);
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
    left = pw_filter_add_port(
        filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "capture_FL", PW_KEY_AUDIO_CHANNEL,
                          "FL", nullptr),
        nullptr, 0);
    right = pw_filter_add_port(
        filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
        pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                          PW_KEY_PORT_NAME, "capture_FR", PW_KEY_AUDIO_CHANNEL,
                          "FR", nullptr),
        nullptr, 0);
    if (!left || !right ||
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
} // namespace

int main() {
  try {
    Source source;
    return source.run();
  } catch (const std::exception &error) {
    fprintf(stderr, "test source: %s\n", error.what());
    return 1;
  }
}
