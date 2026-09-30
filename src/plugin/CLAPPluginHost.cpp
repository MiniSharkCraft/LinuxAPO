#include "CLAPPluginHost.h"

#include "IFilter.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include <clap/clap.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <atomic>
#include <sstream>
#include <stdexcept>
#include <string>
#include <dlfcn.h>
#include <pthread.h>

#ifndef SKYAPO_VERSION
#define SKYAPO_VERSION "0.1.0"
#endif

namespace fs = std::filesystem;
static_assert(std::atomic<bool>::is_always_lock_free,
              "CLAP failure latch must be lock-free on the audio thread");

namespace {
struct ClapLibrary {
  void *handle{};
  const clap_plugin_entry_t *entry{};
  const clap_plugin_factory_t *factory{};
  fs::path path;
  bool initialized = false;
  ~ClapLibrary() {
    if (initialized)
      entry->deinit();
    if (handle)
      dlclose(handle);
  }
};

std::vector<fs::path> searchPaths() {
  std::vector<fs::path> paths;
  if (const char *env = std::getenv("CLAP_PATH")) {
    std::string list(env);
    size_t begin = 0;
    while (begin <= list.size()) {
      const size_t end = list.find(':', begin);
      if (end != begin)
        paths.emplace_back(list.substr(begin, end == std::string::npos
                                                 ? std::string::npos
                                                 : end - begin));
      if (end == std::string::npos)
        break;
      begin = end + 1;
    }
  }
  if (const char *home = std::getenv("HOME"))
    paths.emplace_back(fs::path(home) / ".clap");
  paths.emplace_back("/usr/lib/clap");
  paths.emplace_back("/usr/local/lib/clap");
  paths.emplace_back("/usr/lib64/clap");
  return paths;
}

std::vector<fs::path> pluginFiles() {
  std::vector<fs::path> result;
  for (const auto &root : searchPaths()) {
    std::error_code ec;
    if (!fs::exists(root, ec))
      continue;
    if (fs::is_regular_file(root, ec) && root.extension() == ".clap") {
      result.push_back(root);
      continue;
    }
    fs::recursive_directory_iterator it(
        root, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      if (it->is_regular_file(ec) && it->path().extension() == ".clap")
        result.push_back(it->path());
    }
  }
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());
  return result;
}

std::shared_ptr<ClapLibrary> openLibrary(const fs::path &path) {
  auto library = std::make_shared<ClapLibrary>();
  library->path = path;
  library->handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library->handle)
    return {};
  library->entry = static_cast<const clap_plugin_entry_t *>(
      dlsym(library->handle, "clap_entry"));
  if (!library->entry ||
      !clap_version_is_compatible(library->entry->clap_version))
    return {};
  if (!library->entry->init(path.c_str()))
    return {};
  library->initialized = true;
  library->factory = static_cast<const clap_plugin_factory_t *>(
      library->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
  if (!library->factory)
    return {};
  return library;
}

thread_local const clap_host_t *currentClapAudioHost = nullptr;
bool CLAP_ABI hostIsMainThread(const clap_host_t *host) {
  return host && host->host_data &&
         pthread_equal(*static_cast<const pthread_t *>(host->host_data),
                       pthread_self());
}
bool CLAP_ABI hostIsAudioThread(const clap_host_t *host) {
  return host && currentClapAudioHost == host;
}
const clap_host_thread_check_t threadCheck{hostIsMainThread, hostIsAudioThread};
const void *CLAP_ABI hostExtension(const clap_host_t *, const char *id) {
  return id && std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0 ? &threadCheck
                                                           : nullptr;
}
void CLAP_ABI hostRequest(const clap_host_t *) {}
bool CLAP_ABI rejectEvent(const clap_output_events_t *,
                          const clap_event_header_t *) {
  return false;
}

class CLAPInstance final : public IPluginInstance {
public:
  CLAPInstance(std::shared_ptr<ClapLibrary> lib,
               const clap_plugin_descriptor_t *descriptor, std::string id,
               float sampleRate, unsigned maxFrames,
               const std::vector<std::wstring> &channels,
               const std::vector<PluginParameterValue> &overrides)
      : library(std::move(lib)), pluginId(std::move(id)), maxFrameCount(maxFrames),
        inputChannels(channels.size()), outputChannels(channels.size()) {
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || !maxFrames ||
        channels.empty())
      throw std::runtime_error("invalid CLAP audio configuration");
    host.clap_version = CLAP_VERSION;
    mainThread = pthread_self();
    host.host_data = &mainThread;
    host.name = "SkyAPO";
    host.vendor = "SkyAPO project";
    host.url = "https://github.com/skyapo/skyapo";
    host.version = SKYAPO_VERSION;
    host.get_extension = hostExtension;
    host.request_restart = hostRequest;
    host.request_process = hostRequest;
    host.request_callback = hostRequest;

    plugin = library->factory->create_plugin(library->factory, &host,
                                              descriptor->id);
    if (!plugin)
      throw std::runtime_error("CLAP plugin create failed: " + pluginId);
    if (!plugin->init(plugin)) {
      plugin->destroy(plugin);
      plugin = nullptr;
      throw std::runtime_error("CLAP plugin init failed: " + pluginId);
    }
    auto cleanupPlugin = [this](const clap_plugin_t *instance) {
      if (processing) {
        currentClapAudioHost = &host;
        instance->stop_processing(instance);
        currentClapAudioHost = nullptr;
      }
      if (active)
        instance->deactivate(instance);
      instance->destroy(instance);
      plugin = nullptr;
      processing = false;
      active = false;
    };
    std::unique_ptr<const clap_plugin_t, decltype(cleanupPlugin)> cleanup(
        plugin, cleanupPlugin);
    ports = static_cast<const clap_plugin_audio_ports_t *>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    if (!ports || ports->count(plugin, true) != 1 ||
        ports->count(plugin, false) != 1 ||
        !ports->get(plugin, 0, true, &inputInfo) ||
        !ports->get(plugin, 0, false, &outputInfo) ||
        !(inputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) ||
        !(outputInfo.flags & CLAP_AUDIO_PORT_IS_MAIN) ||
        inputInfo.channel_count != channels.size() ||
        outputInfo.channel_count != channels.size()) {
      throw std::runtime_error("CLAP plugin must expose one main input and "
                               "output matching the SkyAPO channel count: " +
                               pluginId);
    }

    parameterExtension = static_cast<const clap_plugin_params_t *>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    if (!parameterExtension && !overrides.empty()) {
      throw std::runtime_error("CLAP plugin does not expose parameters: " +
                               pluginId);
    }
    if (parameterExtension) {
      const uint32_t count = parameterExtension->count(plugin);
      if (count > 4096) {
        throw std::runtime_error("CLAP plugin declares too many parameters: " +
                                 pluginId);
      }
      clapParameters.reserve(count);
      parameterInfo.reserve(count);
      for (uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!parameterExtension->get_info(plugin, i, &info) ||
            !std::isfinite(info.min_value) || !std::isfinite(info.max_value) ||
            !std::isfinite(info.default_value) ||
            !std::isfinite(static_cast<float>(info.min_value)) ||
            !std::isfinite(static_cast<float>(info.max_value)) ||
            !std::isfinite(static_cast<float>(info.default_value)) ||
            info.min_value > info.max_value ||
            info.default_value < info.min_value ||
            info.default_value > info.max_value) {
          throw std::runtime_error("CLAP plugin supplied invalid parameter "
                                   "metadata: " + pluginId);
        }
        double value = info.default_value;
        (void)parameterExtension->get_value(plugin, info.id, &value);
        if (!std::isfinite(value) || value < info.min_value ||
            value > info.max_value)
          value = info.default_value;
        clapParameters.push_back(info);
        parameterInfo.push_back({std::to_string(info.id), info.name,
                                 static_cast<float>(info.default_value),
                                 static_cast<float>(info.min_value),
                                 static_cast<float>(info.max_value),
                                 static_cast<float>(value)});
      }
    }
    for (const auto &override : overrides) {
      if (!std::isfinite(override.value))
        throw std::runtime_error("CLAP parameter value must be finite");
      size_t match = clapParameters.size();
      for (size_t i = 0; i < clapParameters.size(); ++i) {
        const auto &info = clapParameters[i];
        if (override.symbol == std::to_string(info.id) ||
            override.symbol == info.name) {
          if (match != clapParameters.size())
            throw std::runtime_error("ambiguous CLAP parameter '" +
                                     override.symbol + "' (use its numeric id)");
          match = i;
        }
      }
      if (match == clapParameters.size())
        throw std::runtime_error("unknown CLAP parameter '" + override.symbol +
                                 "'");
      const auto &info = clapParameters[match];
      if (info.flags & CLAP_PARAM_IS_READONLY)
        throw std::runtime_error("CLAP parameter '" + override.symbol +
                                 "' is read-only");
      if (override.value < info.min_value || override.value > info.max_value)
        throw std::runtime_error("CLAP parameter '" + override.symbol +
                                 "' value is outside its declared range");
      clap_event_param_value_t event{};
      event.header.size = sizeof(event);
      event.header.time = 0;
      event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
      event.header.type = CLAP_EVENT_PARAM_VALUE;
      event.header.flags = CLAP_EVENT_DONT_RECORD;
      event.param_id = info.id;
      event.cookie = info.cookie;
      event.note_id = -1;
      event.port_index = -1;
      event.channel = -1;
      event.key = -1;
      event.value = override.value;
      parameterEvents.push_back(event);
      parameterInfo[match].value = override.value;
    }
    if (!plugin->activate(plugin, sampleRate, 1, maxFrames)) {
      throw std::runtime_error("CLAP plugin activation failed: " + pluginId);
    }
    active = true;
    if (!plugin->start_processing(plugin)) {
      throw std::runtime_error("CLAP plugin start_processing failed: " +
                               pluginId);
    }
    processing = true;
    inputBuffer.data32 = inputChannels.data();
    inputBuffer.channel_count = static_cast<uint32_t>(inputChannels.size());
    outputBuffer.data32 = outputChannels.data();
    outputBuffer.channel_count = static_cast<uint32_t>(outputChannels.size());
    emptyInputEvents.ctx = this;
    emptyInputEvents.size = parameterEventCount;
    emptyInputEvents.get = parameterEventAt;
    outputEvents.try_push = rejectEvent;
    clapProcess.in_events = &emptyInputEvents;
    clapProcess.out_events = &outputEvents;
    clapProcess.audio_inputs = &inputBuffer;
    clapProcess.audio_outputs = &outputBuffer;
    clapProcess.audio_inputs_count = 1;
    clapProcess.audio_outputs_count = 1;
    clapProcess.transport = nullptr;
    clapProcess.steady_time = -1;
    cleanup.release();
  }

  ~CLAPInstance() override {
    if (plugin) {
      if (processing) {
        currentClapAudioHost = &host;
        plugin->stop_processing(plugin);
        currentClapAudioHost = nullptr;
      }
      if (active)
        plugin->deactivate(plugin);
      plugin->destroy(plugin);
    }
  }

  const std::string &uri() const noexcept override { return pluginId; }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return parameterInfo;
  }
  std::vector<std::wstring>
  initialize(float, unsigned,
             const std::vector<std::wstring> &channels) override {
    return channels;
  }
  void process(float **output, float **input, unsigned frames) noexcept override {
    if (!plugin || frames > maxFrameCount ||
        processingError.load(std::memory_order_acquire)) {
      for (size_t c = 0; c < outputChannels.size(); ++c)
        std::fill_n(output[c], frames, 0.0f);
      return;
    }
    for (size_t c = 0; c < inputChannels.size(); ++c) {
      inputChannels[c] = input[c];
      outputChannels[c] = output[c];
    }
    clapProcess.frames_count = frames;
    currentClapAudioHost = &host;
    const auto result = plugin->process(plugin, &clapProcess);
    currentClapAudioHost = nullptr;
    if (result == CLAP_PROCESS_ERROR) {
      processingError.store(true, std::memory_order_release);
      for (size_t c = 0; c < outputChannels.size(); ++c)
        std::fill_n(output[c], frames, 0.0f);
    }
  }

  bool processingFailed() const noexcept override {
    return processingError.load(std::memory_order_acquire);
  }

private:
  static uint32_t CLAP_ABI parameterEventCount(const clap_input_events_t *list) {
    const auto *self = static_cast<const CLAPInstance *>(list->ctx);
    return static_cast<uint32_t>(self->parameterEvents.size());
  }
  static const clap_event_header_t *CLAP_ABI
  parameterEventAt(const clap_input_events_t *list, uint32_t index) {
    const auto *self = static_cast<const CLAPInstance *>(list->ctx);
    return index < self->parameterEvents.size()
               ? &self->parameterEvents[index].header
               : nullptr;
  }

  std::shared_ptr<ClapLibrary> library;
  std::string pluginId;
  unsigned maxFrameCount;
  clap_host_t host{};
  pthread_t mainThread{};
  const clap_plugin_t *plugin{};
  const clap_plugin_audio_ports_t *ports{};
  const clap_plugin_params_t *parameterExtension{};
  std::vector<clap_param_info_t> clapParameters;
  std::vector<clap_event_param_value_t> parameterEvents;
  clap_audio_port_info_t inputInfo{}, outputInfo{};
  bool active = false, processing = false;
  std::atomic<bool> processingError{false};
  std::vector<float *> inputChannels, outputChannels;
  clap_audio_buffer_t inputBuffer{}, outputBuffer{};
  clap_input_events_t emptyInputEvents{};
  clap_output_events_t outputEvents{};
  clap_process_t clapProcess{};
  std::vector<PluginParameterInfo> parameterInfo;
};

class CLAPPluginFilter final : public IFilter {
public:
  CLAPPluginFilter(CLAPPluginHost &host, std::string id,
                   std::vector<PluginParameterValue> overrides)
      : host(host), pluginId(std::move(id)),
        parameterOverrides(std::move(overrides)) {}
  bool getInPlace() override { return false; }
  std::vector<std::wstring> initialize(float sampleRate, unsigned maxFrames,
                                      std::vector<std::wstring> channels) override {
    instance = host.create(pluginId, sampleRate, maxFrames, channels,
                           parameterOverrides);
    return instance->initialize(sampleRate, maxFrames, channels);
  }
  void process(float **output, float **input, unsigned frames) override {
    instance->process(output, input, frames);
  }

private:
  CLAPPluginHost &host;
  std::string pluginId;
  std::vector<PluginParameterValue> parameterOverrides;
  std::unique_ptr<IPluginInstance> instance;
};

IFilter *allocateFilter(CLAPPluginHost &host, std::string id,
                        std::vector<PluginParameterValue> overrides) {
  void *memory = MemoryHelper::alloc(sizeof(CLAPPluginFilter));
  try {
    return new (memory) CLAPPluginFilter(host, std::move(id),
                                         std::move(overrides));
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}

class CLAPPluginFilterFactory final : public IFilterFactory {
public:
  std::vector<IFilter *> createFilter(const std::wstring &, std::wstring &command,
                                      std::wstring &parameters) override {
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, wideId, extra;
    input >> format >> wideId;
    if (format != L"CLAP")
      return {};
    if (wideId.empty())
      throw std::runtime_error(
          "expected Plugin: CLAP <plugin-id> [parameter=value ...]");
    std::vector<PluginParameterValue> overrides;
    std::wstring token;
    while (input >> token) {
      const auto separator = token.find(L'=');
      if (separator == std::wstring::npos || separator == 0 ||
          separator + 1 == token.size())
        throw std::runtime_error(
            "expected CLAP parameter as numeric-id-or-name=value");
      const auto symbol =
          StringHelper::toString(token.substr(0, separator), 65001);
      const auto valueText =
          StringHelper::toString(token.substr(separator + 1), 65001);
      size_t consumed = 0;
      float value = 0.0f;
      try {
        value = std::stof(valueText, &consumed);
      } catch (const std::exception &) {
        throw std::runtime_error("invalid CLAP parameter value for '" + symbol +
                                 "'");
      }
      if (consumed != valueText.size() || !std::isfinite(value))
        throw std::runtime_error("invalid CLAP parameter value for '" + symbol +
                                 "'");
      if (std::any_of(
              overrides.begin(), overrides.end(), [&](const auto &entry) {
                return entry.symbol == symbol;
              }))
        throw std::runtime_error("duplicate CLAP parameter override '" +
                                 symbol + "'");
      overrides.push_back({symbol, value});
    }
    return {allocateFilter(host, StringHelper::toString(wideId, 65001),
                           std::move(overrides))};
  }

private:
  CLAPPluginHost host;
};
} // namespace

struct CLAPPluginHost::CatalogItem {
  std::shared_ptr<ClapLibrary> library;
  std::string id;
  std::string name;
};

CLAPPluginHost::CLAPPluginHost() = default;
CLAPPluginHost::~CLAPPluginHost() = default;

void CLAPPluginHost::scan() const {
  if (scanned)
    return;
  scanned = true;
  for (const auto &path : pluginFiles()) {
    auto library = openLibrary(path);
    if (!library)
      continue;
    const uint32_t count = library->factory->get_plugin_count(library->factory);
    for (uint32_t i = 0; i < count; ++i) {
      const auto *descriptor =
          library->factory->get_plugin_descriptor(library->factory, i);
      if (descriptor && descriptor->id && descriptor->name)
        catalog.push_back({library, descriptor->id, descriptor->name});
    }
  }
}

std::vector<std::pair<std::string, std::string>> CLAPPluginHost::list() const {
  scan();
  std::vector<std::pair<std::string, std::string>> result;
  result.reserve(catalog.size());
  for (const auto &item : catalog)
    result.emplace_back(item.id, item.name);
  return result;
}

PluginDescription CLAPPluginHost::describe(const std::string &id) const {
  scan();
  const auto found = std::find_if(catalog.begin(), catalog.end(),
                                  [&](const auto &item) { return item.id == id; });
  if (found == catalog.end())
    throw std::runtime_error("CLAP plugin not found: " + id);
  PluginDescription result{id, found->name, {}};
  pthread_t mainThread = pthread_self();
  clap_host_t host{};
  host.clap_version = CLAP_VERSION;
  host.host_data = &mainThread;
  host.name = "SkyAPO";
  host.vendor = "SkyAPO project";
  host.url = "https://github.com/skyapo/skyapo";
  host.version = SKYAPO_VERSION;
  host.get_extension = hostExtension;
  host.request_restart = hostRequest;
  host.request_process = hostRequest;
  host.request_callback = hostRequest;
  const clap_plugin_t *plugin = found->library->factory->create_plugin(
      found->library->factory, &host, id.c_str());
  if (!plugin)
    throw std::runtime_error("CLAP plugin create failed: " + id);
  if (!plugin->init(plugin)) {
    plugin->destroy(plugin);
    throw std::runtime_error("CLAP plugin init failed: " + id);
  }
  auto destroy = [](const clap_plugin_t *instance) { instance->destroy(instance); };
  std::unique_ptr<const clap_plugin_t, decltype(destroy)> cleanup(plugin,
                                                                  destroy);
  const auto *params = static_cast<const clap_plugin_params_t *>(
      plugin->get_extension(plugin, CLAP_EXT_PARAMS));
  if (!params)
    return result;
  const uint32_t count = params->count(plugin);
  if (count > 4096)
    throw std::runtime_error("CLAP plugin declares too many parameters: " + id);
  result.inputParameters.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    clap_param_info_t info{};
    if (!params->get_info(plugin, i, &info))
      throw std::runtime_error("cannot read CLAP parameter metadata: " + id);
    double value = info.default_value;
    (void)params->get_value(plugin, info.id, &value);
    result.inputParameters.push_back(
        {std::to_string(info.id), info.name,
         static_cast<float>(info.default_value), static_cast<float>(info.min_value),
         static_cast<float>(info.max_value), static_cast<float>(value)});
  }
  return result;
}

std::unique_ptr<IPluginInstance>
CLAPPluginHost::create(const std::string &id, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) {
  scan();
  const auto found = std::find_if(catalog.begin(), catalog.end(),
                                  [&](const auto &item) { return item.id == id; });
  if (found == catalog.end())
    throw std::runtime_error("CLAP plugin not found: " + id +
                             " (check CLAP_PATH and standard plugin paths)");
  const auto count = found->library->factory->get_plugin_count(
      found->library->factory);
  for (uint32_t i = 0; i < count; ++i) {
    const auto *descriptor = found->library->factory->get_plugin_descriptor(
        found->library->factory, i);
    if (descriptor && descriptor->id && id == descriptor->id)
      return std::make_unique<CLAPInstance>(found->library, descriptor, id,
                                            sampleRate, maxFrames, channels,
                                            parameters);
  }
  throw std::runtime_error("CLAP plugin descriptor disappeared: " + id);
}

std::unique_ptr<IFilterFactory> makeCLAPPluginFilterFactory() {
  return std::make_unique<CLAPPluginFilterFactory>();
}
