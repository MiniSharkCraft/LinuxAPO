// Deterministic ordinary PipeWire playback client for policy integration tests.
#include <pipewire/keys.h>
#include <pipewire/pipewire.h>
#include <pipewire/stream.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/format-utils.h>

#include <cmath>
#include <array>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
struct PlaybackStream {
  static constexpr unsigned MaxChannels = 8;
  pw_main_loop *loop{};
  pw_stream *stream{};
  spa_hook streamHook{};
  double phase{};
  float amplitude{0.1f};
  unsigned channels{2};
  std::array<spa_audio_channel, MaxChannels> positions{
      SPA_AUDIO_CHANNEL_FL,  SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
      SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR,
      SPA_AUDIO_CHANNEL_SL,  SPA_AUDIO_CHANNEL_SR};
  std::string targetObject;
  std::string nodeName{"skyapo.test.playback-app"};
  bool failed{};

  static void finish(void *data, int) {
    auto &self = *static_cast<PlaybackStream *>(data);
    pw_main_loop_quit(self.loop);
  }

  static void stateChanged(void *data, pw_stream_state oldState,
                           pw_stream_state state, const char *error) {
    auto &self = *static_cast<PlaybackStream *>(data);
    std::fprintf(stderr, "test playback stream: state %s -> %s",
                 pw_stream_state_as_string(oldState),
                 pw_stream_state_as_string(state));
    if (error)
      std::fprintf(stderr, ": %s", error);
    std::fputc('\n', stderr);
    if (state == PW_STREAM_STATE_ERROR ||
        state == PW_STREAM_STATE_UNCONNECTED) {
      self.failed = state == PW_STREAM_STATE_ERROR;
      pw_main_loop_quit(self.loop);
    }
  }

  static void process(void *data) {
    auto &self = *static_cast<PlaybackStream *>(data);
    auto *buffer = pw_stream_dequeue_buffer(self.stream);
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0)
      return;
    auto &audio = buffer->buffer->datas[0];
    if (!audio.data || audio.maxsize < self.channels * sizeof(float)) {
      pw_stream_queue_buffer(self.stream, buffer);
      return;
    }
    auto *samples = static_cast<float *>(audio.data);
    const auto frames = audio.maxsize / (self.channels * sizeof(float));
    constexpr double twoPi = 6.283185307179586476925286766559;
    constexpr double frequency = 440.0;
    constexpr double sampleRate = 48000.0;
    constexpr double step = twoPi * frequency / sampleRate;
    for (uint32_t frame = 0; frame < frames; ++frame) {
      const float sample =
          static_cast<float>(self.amplitude * std::sin(self.phase));
      self.phase += step;
      if (self.phase >= twoPi)
        self.phase -= twoPi;
      for (unsigned channel = 0; channel < self.channels; ++channel)
        samples[self.channels * frame + channel] = sample;
    }
    audio.chunk->offset = 0;
    audio.chunk->stride = self.channels * sizeof(float);
    audio.chunk->size = frames * self.channels * sizeof(float);
    pw_stream_queue_buffer(self.stream, buffer);
  }

  int run() {
    pw_init(nullptr, nullptr);
    loop = pw_main_loop_new(nullptr);
    if (!loop)
      throw std::runtime_error("cannot create PipeWire main loop");
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, finish, this);
    pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, finish, this);

    std::string positionList = "[";
    static constexpr std::array<const char *, MaxChannels> PositionNames{
        "FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
    for (unsigned channel = 0; channel < channels; ++channel)
      positionList += " " + std::string(PositionNames[channel]);
    positionList += " ]";
    auto *properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music", PW_KEY_NODE_NAME, nodeName.c_str(),
        PW_KEY_NODE_DESCRIPTION, "SkyAPO Test Playback Application",
        PW_KEY_NODE_AUTOCONNECT, "true", PW_KEY_AUDIO_CHANNELS,
        std::to_string(channels).c_str(), SPA_KEY_AUDIO_POSITION,
        positionList.c_str(), nullptr);
    if (!targetObject.empty())
      pw_properties_set(properties, PW_KEY_TARGET_OBJECT, targetObject.c_str());
    static const pw_stream_events events = [] {
      pw_stream_events value{};
      value.version = PW_VERSION_STREAM_EVENTS;
      value.state_changed = stateChanged;
      value.process = process;
      return value;
    }();
    stream = pw_stream_new_simple(pw_main_loop_get_loop(loop),
                                  "SkyAPO Test Playback Application",
                                  properties, &events, this);
    if (!stream)
      throw std::runtime_error("cannot create PipeWire playback stream");
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48000;
    info.channels = channels;
    for (unsigned channel = 0; channel < channels; ++channel)
      info.position[channel] = positions[channel];
    uint8_t formatBytes[1024];
    spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(formatBytes, sizeof(formatBytes));
    const spa_pod *params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
    if (!params[0] || pw_stream_connect(stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                        static_cast<pw_stream_flags>(
                                            PW_STREAM_FLAG_AUTOCONNECT |
                                            PW_STREAM_FLAG_MAP_BUFFERS |
                                            PW_STREAM_FLAG_RT_PROCESS),
                                        params, 1) < 0)
      throw std::runtime_error("cannot connect PipeWire playback stream");
    pw_main_loop_run(loop);
    pw_stream_destroy(stream);
    pw_main_loop_destroy(loop);
    pw_deinit();
    return failed ? 1 : 0;
  }
};
} // namespace

int main(int argc, char **argv) {
  try {
    PlaybackStream stream;
    for (int arg = 1; arg < argc; ++arg) {
      const std::string option(argv[arg]);
      if (option == "--target" && arg + 1 < argc) {
        stream.targetObject = argv[++arg];
      } else if (option == "--name" && arg + 1 < argc) {
        stream.nodeName = argv[++arg];
        if (stream.nodeName.empty())
          throw std::runtime_error("--name must not be empty");
      } else if (option == "--amplitude" && arg + 1 < argc) {
        char *end = nullptr;
        const float amplitude = std::strtof(argv[++arg], &end);
        if (end == argv[arg] || *end || !std::isfinite(amplitude) ||
            amplitude < 0.0f || amplitude > 0.1f)
          throw std::runtime_error(
              "--amplitude must be finite and in the range [0, 0.1]");
        stream.amplitude = amplitude;
      } else {
        if (option == "--surround51" || option == "--surround71") {
          stream.channels = option == "--surround51" ? 6 : 8;
          continue;
        }
        std::fprintf(stderr, "usage: skyapo-pipewire-test-playback-stream "
                             "[--target NODE-NAME] [--name NODE-NAME] "
                             "[--amplitude 0..0.1] "
                             "[--surround51|--surround71]\n");
        return 2;
      }
    }
    return stream.run();
  } catch (const std::exception &error) {
    std::fprintf(stderr, "test playback stream: %s\n", error.what());
    return 1;
  }
}
