#include "Runtime.h"
#include "../platform/PlatformChannels.h"
#include "../platform/RealtimeAudit.h"
#include "../platform/Settings.h"
#include "DeviceManager.h"
#include "Engine.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/port-config.h>
#include <spa/param/props.h>
#include <sstream>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <time.h>

namespace {
constexpr unsigned MaxFrames = 8192, MaxChannels = 8;
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
  spa_source *rateEvent{}, *statusEvent{}, *sigint{}, *sigterm{},
      *selectionTimer{}, *configEvent{}, *reloadTimer{};
  int configWatch = -1;
  int server = -1;
  bool ownsSocket = false;
  bool cleaning = false;
  bool initialized = false;
  std::array<void *, MaxChannels> inputs{}, outputs{};
  std::array<uint32_t, MaxChannels> ownInputs{};
  std::array<pw_proxy *, MaxChannels> links{};
  struct LinkContext {
    Runtime *owner;
    unsigned channel;
  };
  std::array<LinkContext, MaxChannels> linkContexts{};
  std::array<spa_hook, MaxChannels> linkHooks{};
  std::array<bool, MaxChannels> linked{};
  std::vector<float> work;
  std::unique_ptr<Engine> activeEngine;
  std::vector<std::unique_ptr<Engine>> retiredEngines;
  std::atomic<Engine *> active{nullptr};
  std::atomic<unsigned> callbacksInFlight{0};
  std::atomic<bool> resetMetrics{false};
  std::atomic<unsigned> rate{0}, quantum{0}, requestedRate{0};
  std::atomic<uint64_t> blocks{0}, samples{0}, totalNs{0}, maxNs{0},
      overruns{0};
  std::atomic<double> rawEnergy{0}, processedEnergy{0};
  double rawSum = 0, processedSum = 0;
  unsigned channels = 0;
  std::string configError;
  pw_filter_state state = PW_FILTER_STATE_UNCONNECTED;
  explicit Runtime(volatile sig_atomic_t &stop) : stopping(stop) {
    ownInputs.fill(SPA_ID_INVALID);
  }
  ~Runtime() {
    cleaning = true;
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
      for (auto *s : {rateEvent, statusEvent, sigint, sigterm, selectionTimer,
                      configEvent, reloadTimer})
        if (s)
          pw_loop_destroy_source(l, s);
    }
    if (configWatch >= 0)
      close(configWatch);
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
  void buildEngine(unsigned hz) {
    if (hz < 8000 || hz > 384000)
      throw std::runtime_error("unsupported graph rate " + std::to_string(hz));
    auto *current = active.load(std::memory_order_acquire);
    if (current && current->sampleRate() == hz)
      return;
    std::vector<std::wstring> names;
    for (auto &p : ports)
      names.push_back(eapoChannel(p.channel));
    auto engine = std::make_unique<Engine>(hz, channels, MaxFrames, names);
    engine->loadConfig(config);
    installEngine(std::move(engine));
    auto *pointer = active.load(std::memory_order_acquire);
    std::cerr << "skyapod: DSP ready at " << hz << " Hz, " << channels
              << " channels, " << pointer->filterCount() << " filters\n";
  }
  void installEngine(std::unique_ptr<Engine> replacement) {
    // Keep the old graph owned until every callback that could have loaded it
    // has left. All graph destruction remains on this control thread.
    if (activeEngine)
      retiredEngines.push_back(std::move(activeEngine));
    auto *pointer = replacement.get();
    active.store(pointer, std::memory_order_seq_cst);
    activeEngine = std::move(replacement);
    reclaimRetired();
  }
  void reclaimRetired() {
    if (callbacksInFlight.load(std::memory_order_seq_cst) == 0)
      retiredEngines.clear();
  }
  void reloadConfig() {
    auto *current = active.load(std::memory_order_acquire);
    const unsigned hz = current ? current->sampleRate() : 48000;
    std::vector<std::wstring> names;
    for (auto &p : ports)
      names.push_back(eapoChannel(p.channel));
    auto replacement = std::make_unique<Engine>(hz, channels, MaxFrames, names);
    replacement->loadConfig(config);
    const unsigned count = replacement->filterCount();
    installEngine(std::move(replacement));
    resetMetrics.store(true, std::memory_order_release);
    configError.clear();
    std::cerr << "skyapod: config reload succeeded (" << count << " filters)\n";
  }
  static void configReady(void *data, int, uint32_t) {
    auto &r = *static_cast<Runtime *>(data);
    alignas(inotify_event) char buffer[4096];
    bool relevant = false;
    for (;;) {
      const ssize_t length = read(r.configWatch, buffer, sizeof(buffer));
      if (length <= 0)
        break;
      for (size_t offset = 0; offset < static_cast<size_t>(length);) {
        const auto *event =
            reinterpret_cast<const inotify_event *>(buffer + offset);
        if (!event->len ||
            std::filesystem::path(r.config).filename() == event->name)
          relevant = true;
        offset += sizeof(inotify_event) + event->len;
      }
    }
    if (relevant) {
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
      r.buildEngine(r.requestedRate.load());
    } catch (const std::exception &e) {
      r.fail(e.what());
    }
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
    if (!frames || frames > 65536)
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
    Engine *engine = r.active.load(std::memory_order_acquire);
    if (!engine || engine->sampleRate() != hz) {
      for (unsigned c = 0; c < r.channels; ++c)
        if (out[c])
          std::fill_n(out[c], frames, 0.0f);
      if (r.requestedRate.exchange(hz) != hz)
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
      engine->process(r.work.data(), n);
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
    c.owner->linked[c.channel] = info->state == PW_LINK_STATE_ACTIVE;
    if (info->state == PW_LINK_STATE_ERROR)
      c.owner->fail(info->error ? info->error : "capture link failed");
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
        auto *p = pw_properties_new(nullptr, nullptr);
        pw_properties_setf(p, PW_KEY_LINK_OUTPUT_NODE, "%u", r.device.id);
        pw_properties_setf(p, PW_KEY_LINK_OUTPUT_PORT, "%u", r.ports[c].id);
        pw_properties_setf(p, PW_KEY_LINK_INPUT_NODE, "%u",
                           pw_filter_get_node_id(r.filter));
        pw_properties_setf(p, PW_KEY_LINK_INPUT_PORT, "%u", id);
        r.links[c] = reinterpret_cast<pw_proxy *>(pw_core_create_object(
            r.core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
            &p->dict, 0));
        pw_properties_free(p);
        if (!r.links[c])
          r.fail("cannot create capture link");
        else {
          static const auto events = [] {
            pw_link_events e{};
            e.version = PW_VERSION_LINK_EVENTS;
            e.info = linkInfo;
            return e;
          }();
          r.linkContexts[c] = {&r, c};
          pw_link_add_listener(reinterpret_cast<pw_link *>(r.links[c]),
                               &r.linkHooks[c], &events, &r.linkContexts[c]);
          std::cerr << "skyapod: linked physical port " << r.ports[c].id
                    << " -> " << id << " (" << channel << ")\n";
        }
      }
  }
  static void removed(void *data, uint32_t id) {
    auto &r = *static_cast<Runtime *>(data);
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
    s << "\nFilters: " << (e ? e->filterCount() : 0) << "\nConfig: " << config
      << "\nProcessed blocks: " << b << "\nOverruns: " << overruns.load();
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
    return s.str();
  }
  static void statusReady(void *data, int, uint32_t) {
    auto &r = *static_cast<Runtime *>(data);
    for (;;) {
      int client =
          accept4(r.server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
      if (client < 0)
        break;
      auto text = r.status();
      send(client, text.data(), text.size(), MSG_NOSIGNAL);
      close(client);
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
    buildEngine(48000);
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
    sigint = pw_loop_add_signal(loop, SIGINT, signal, this);
    sigterm = pw_loop_add_signal(loop, SIGTERM, signal, this);
    selectionTimer = pw_loop_add_timer(loop, selectionChanged, this);
    reloadTimer = pw_loop_add_timer(loop, reloadReady, this);
    configWatch = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (configWatch < 0)
      throw std::runtime_error("cannot create config file watcher");
    const auto parent = std::filesystem::path(config).parent_path();
    if (inotify_add_watch(configWatch, parent.c_str(),
                          IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_ATTRIB |
                              IN_DELETE | IN_Q_OVERFLOW) < 0)
      throw std::runtime_error("cannot watch config directory: " +
                               parent.string());
    configEvent =
        pw_loop_add_io(loop, configWatch, SPA_IO_IN, false, configReady, this);
    if (!rateEvent || !sigint || !sigterm || !selectionTimer || !reloadTimer ||
        !configEvent)
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
    uint8_t bytes[2048];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(bytes, sizeof(bytes));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32P;
    info.rate = 48000;
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
    const spa_pod *params[] = {
        spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info),
        spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info),
        static_cast<spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_volume,
            SPA_POD_Float(1.0f), SPA_PROP_mute, SPA_POD_Bool(false),
            SPA_PROP_channelVolumes,
            SPA_POD_Array(sizeof(float), SPA_TYPE_Float, channels,
                          unityVolumes.data())))};
    if (pw_filter_connect(filter, PW_FILTER_FLAG_RT_PROCESS, params, 3) < 0)
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
  if (!r.error.empty())
    throw std::runtime_error(r.error);
}
