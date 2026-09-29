// Explicit, opt-in integration recorder. No synthetic audio is injected.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <pipewire/pipewire.h>
#include <sndfile.h>
#include <spa/param/audio/format-utils.h>
#include <vector>
#define PW_KEY_NODE_DONT_FALLBACK "node.dont-fallback"
#define PW_KEY_NODE_DONT_MOVE "node.dont-move"

namespace {
constexpr unsigned Rate = 48000, Channels = 2;
struct Probe;
struct Capture {
  Probe *owner{};
  pw_stream *stream{};
  spa_hook hook{};
  std::vector<float> samples;
  std::atomic<size_t> frames{0};
  bool formatOK = false;
};
struct Probe {
  pw_main_loop *loop{};
  spa_source *done{};
  spa_source *timer{};
  Capture raw, processed;
  bool failed = false;
};
void completion(void *data, uint64_t) {
  auto &p = *static_cast<Probe *>(data);
  if (p.raw.frames.load() >= p.raw.samples.size() / Channels &&
      p.processed.frames.load() >= p.processed.samples.size() / Channels)
    pw_main_loop_quit(p.loop);
}
void timeout(void *data, uint64_t) {
  auto &p = *static_cast<Probe *>(data);
  p.failed = true;
  pw_main_loop_quit(p.loop);
}
void state(void *data, pw_stream_state, pw_stream_state s,
           const char *message) {
  auto &c = *static_cast<Capture *>(data);
  if (s == PW_STREAM_STATE_ERROR) {
    std::cerr << "capture error: " << (message ? message : "") << '\n';
    c.owner->failed = true;
    pw_main_loop_quit(c.owner->loop);
  }
}
void format(void *data, uint32_t id, const spa_pod *param) {
  if (id != SPA_PARAM_Format || !param)
    return;
  auto &c = *static_cast<Capture *>(data);
  spa_audio_info_raw info{};
  if (spa_format_audio_raw_parse(param, &info) < 0 ||
      info.format != SPA_AUDIO_FORMAT_F32 || info.rate != Rate ||
      info.channels != Channels) {
    c.owner->failed = true;
    pw_main_loop_quit(c.owner->loop);
    return;
  }
  c.formatOK = true;
  std::cerr << "probe negotiated F32 " << info.rate << " Hz, " << info.channels
            << " channels\n";
}
void process(void *data) {
  auto &c = *static_cast<Capture *>(data);
  auto *b = pw_stream_dequeue_buffer(c.stream);
  if (!b)
    return;
  auto *buffer = b->buffer;
  if (buffer->n_datas == 1 && buffer->datas[0].data) {
    auto &d = buffer->datas[0];
    auto offset = std::min(d.chunk->offset, d.maxsize);
    auto bytes = std::min(d.chunk->size, d.maxsize - offset);
    auto available = bytes / (sizeof(float) * Channels);
    size_t cursor = c.frames.load(std::memory_order_relaxed),
           total = c.samples.size() / Channels,
           n = std::min<size_t>(available, total - cursor);
    if (n) {
      memcpy(c.samples.data() + cursor * Channels,
             static_cast<char *>(d.data) + offset,
             n * Channels * sizeof(float));
      c.frames.store(cursor + n, std::memory_order_release);
    }
    if (cursor + n == total)
      pw_loop_signal_event(pw_main_loop_get_loop(c.owner->loop), c.owner->done);
  }
  pw_stream_queue_buffer(c.stream, b);
}
void create(Probe &p, Capture &c, const char *target, unsigned seconds) {
  c.owner = &p;
  c.samples.resize(size_t(Rate) * seconds * Channels);
  static const auto events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = state;
    e.param_changed = format;
    e.process = process;
    return e;
  }();
  c.stream = pw_stream_new_simple(
      pw_main_loop_get_loop(p.loop), "SkyAPO realtime verification",
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
                        "Capture", PW_KEY_MEDIA_ROLE, "Production",
                        PW_KEY_TARGET_OBJECT, target, PW_KEY_NODE_DONT_FALLBACK,
                        "true", PW_KEY_NODE_DONT_MOVE, "true", PW_KEY_NODE_NAME,
                        target == std::string("skyapo.virtual_mic")
                            ? "skyapo.probe.processed"
                            : "skyapo.probe.raw",
                        nullptr),
      &events, &c);
  if (!c.stream)
    throw std::runtime_error("probe stream creation failed");
  uint8_t bytes[1024];
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = Rate;
  info.channels = Channels;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod *params[] = {
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
  if (pw_stream_connect(
          c.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                       PW_STREAM_FLAG_MAP_BUFFERS |
                                       PW_STREAM_FLAG_RT_PROCESS),
          params, 1) < 0)
    throw std::runtime_error("probe stream connection failed");
}
void wav(const std::string &path, const Capture &c) {
  SF_INFO info{};
  info.samplerate = Rate;
  info.channels = Channels;
  info.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;
  auto *f = sf_open(path.c_str(), SFM_WRITE, &info);
  if (!f)
    throw std::runtime_error(sf_strerror(nullptr));
  auto n = c.frames.load();
  if (sf_writef_float(f, c.samples.data(), n) != static_cast<sf_count_t>(n)) {
    sf_close(f);
    throw std::runtime_error("WAV write failed");
  }
  sf_close(f);
}
} // namespace
int main(int argc, char **argv) {
  if (argc != 3) {
    std::cerr << "usage: skyapo-realtime-probe <physical-node-name> "
                 "<output-prefix>\n";
    return 2;
  }
  pw_init(nullptr, nullptr);
  Probe p;
  p.loop = pw_main_loop_new(nullptr);
  auto *loop = pw_main_loop_get_loop(p.loop);
  p.done = pw_loop_add_event(loop, completion, &p);
  p.timer = pw_loop_add_timer(loop, timeout, &p);
  timespec delay{10, 0};
  pw_loop_update_timer(loop, p.timer, &delay, nullptr, false);
  try {
    create(p, p.raw, argv[1], 4);
    create(p, p.processed, "skyapo.virtual_mic", 4);
    pw_main_loop_run(p.loop);
    if (p.failed || !p.raw.formatOK || !p.processed.formatOK)
      throw std::runtime_error("recording failed or timed out");
    pw_stream_disconnect(p.raw.stream);
    pw_stream_disconnect(p.processed.stream);
    wav(std::string(argv[2]) + "-raw.wav", p.raw);
    wav(std::string(argv[2]) + "-processed.wav", p.processed);
    // Search a bounded lag using nonzero real microphone samples; then measure
    // regression and energy over the aligned recording, excluding startup.
    int bestLag = 0;
    double best = -2;
    size_t start = Rate / 2, window = 4096;
    for (int lag = -4096; lag <= 4096; ++lag) {
      double xy = 0, xx = 0, yy = 0;
      for (size_t i = start; i < start + window; i += 4) {
        double x = p.raw.samples[Channels * i],
               y = p.processed
                       .samples[Channels * (static_cast<ptrdiff_t>(i) + lag)];
        xy += x * y;
        xx += x * x;
        yy += y * y;
      }
      double corr = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : -2;
      if (corr > best) {
        best = corr;
        bestLag = lag;
      }
    }
    double xy = 0, xx = 0, yy = 0;
    size_t total = p.raw.frames.load();
    for (size_t i = start; i < total - 8192; ++i)
      for (unsigned c = 0; c < Channels; ++c) {
        double x = p.raw.samples[Channels * i + c],
               y = p.processed.samples[Channels * (static_cast<ptrdiff_t>(i) +
                                                   bestLag) +
                                       c];
        xy += x * y;
        xx += x * x;
        yy += y * y;
      }
    double corr = xx > 0 && yy > 0 ? xy / std::sqrt(xx * yy) : 0,
           ratio = xx > 0 ? std::sqrt(yy / xx) : 0;
    std::cout << "Recorded physical and SkyAPO clients: " << total
              << " frames each\nAlignment lag: " << bestLag
              << " frames\nCorrelation: " << corr
              << "\nRMS amplitude ratio: " << ratio
              << "\nGain: " << (ratio > 0 ? 20 * std::log10(ratio) : -INFINITY)
              << " dB\n";
    bool matched =
        corr > .99 && std::abs(ratio - std::pow(10., -6. / 20.)) < .005;
    pw_stream_destroy(p.raw.stream);
    p.raw.stream = nullptr;
    pw_stream_destroy(p.processed.stream);
    p.processed.stream = nullptr;
    pw_loop_destroy_source(loop, p.done);
    pw_loop_destroy_source(loop, p.timer);
    pw_main_loop_destroy(p.loop);
    pw_deinit();
    return matched ? 0 : 1;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    if (p.raw.stream)
      pw_stream_destroy(p.raw.stream);
    if (p.processed.stream)
      pw_stream_destroy(p.processed.stream);
    pw_loop_destroy_source(loop, p.done);
    pw_loop_destroy_source(loop, p.timer);
    pw_main_loop_destroy(p.loop);
    pw_deinit();
    return 1;
  }
}
