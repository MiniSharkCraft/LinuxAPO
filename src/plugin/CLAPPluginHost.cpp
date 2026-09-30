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
#include <sstream>
#include <stdexcept>
#include <string>
#include <dlfcn.h>
#include <pthread.h>

namespace fs = std::filesystem;

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
uint32_t CLAP_ABI noEvents(const clap_input_events_t *) { return 0; }
const clap_event_header_t *CLAP_ABI noEventGet(const clap_input_events_t *,
                                               uint32_t) {
  return nullptr;
}
bool CLAP_ABI rejectEvent(const clap_output_events_t *,
                          const clap_event_header_t *) {
  return false;
}

class CLAPInstance final : public IPluginInstance {
public:
  CLAPInstance(std::shared_ptr<ClapLibrary> lib,
               const clap_plugin_descriptor_t *descriptor, std::string id,
               float sampleRate, unsigned maxFrames,
               const std::vector<std::wstring> &channels)
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
    host.version = "0.1.0";
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
      plugin->destroy(plugin);
      plugin = nullptr;
      throw std::runtime_error("CLAP plugin must expose one main input and "
                               "output matching the SkyAPO channel count: " +
                               pluginId);
    }
    if (!plugin->activate(plugin, sampleRate, 1, maxFrames)) {
      plugin->destroy(plugin);
      plugin = nullptr;
      throw std::runtime_error("CLAP plugin activation failed: " + pluginId);
    }
    active = true;
    if (!plugin->start_processing(plugin)) {
      plugin->deactivate(plugin);
      active = false;
      plugin->destroy(plugin);
      plugin = nullptr;
      throw std::runtime_error("CLAP plugin start_processing failed: " +
                               pluginId);
    }
    processing = true;
    inputBuffer.data32 = inputChannels.data();
    inputBuffer.channel_count = static_cast<uint32_t>(inputChannels.size());
    outputBuffer.data32 = outputChannels.data();
    outputBuffer.channel_count = static_cast<uint32_t>(outputChannels.size());
    emptyInputEvents.size = noEvents;
    emptyInputEvents.get = noEventGet;
    outputEvents.try_push = rejectEvent;
    clapProcess.in_events = &emptyInputEvents;
    clapProcess.out_events = &outputEvents;
    clapProcess.audio_inputs = &inputBuffer;
    clapProcess.audio_outputs = &outputBuffer;
    clapProcess.audio_inputs_count = 1;
    clapProcess.audio_outputs_count = 1;
    clapProcess.transport = nullptr;
    clapProcess.steady_time = -1;
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
    if (!plugin || frames > maxFrameCount) {
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
    if (result == CLAP_PROCESS_ERROR)
      for (size_t c = 0; c < outputChannels.size(); ++c)
        std::fill_n(output[c], frames, 0.0f);
  }

private:
  std::shared_ptr<ClapLibrary> library;
  std::string pluginId;
  unsigned maxFrameCount;
  clap_host_t host{};
  pthread_t mainThread{};
  const clap_plugin_t *plugin{};
  const clap_plugin_audio_ports_t *ports{};
  clap_audio_port_info_t inputInfo{}, outputInfo{};
  bool active = false, processing = false;
  std::vector<float *> inputChannels, outputChannels;
  clap_audio_buffer_t inputBuffer{}, outputBuffer{};
  clap_input_events_t emptyInputEvents{};
  clap_output_events_t outputEvents{};
  clap_process_t clapProcess{};
  std::vector<PluginParameterInfo> parameterInfo;
};

class CLAPPluginFilter final : public IFilter {
public:
  CLAPPluginFilter(CLAPPluginHost &host, std::string id)
      : host(host), pluginId(std::move(id)) {}
  bool getInPlace() override { return false; }
  std::vector<std::wstring> initialize(float sampleRate, unsigned maxFrames,
                                      std::vector<std::wstring> channels) override {
    instance = host.create(pluginId, sampleRate, maxFrames, channels);
    return instance->initialize(sampleRate, maxFrames, channels);
  }
  void process(float **output, float **input, unsigned frames) override {
    instance->process(output, input, frames);
  }

private:
  CLAPPluginHost &host;
  std::string pluginId;
  std::unique_ptr<IPluginInstance> instance;
};

IFilter *allocateFilter(CLAPPluginHost &host, std::string id) {
  void *memory = MemoryHelper::alloc(sizeof(CLAPPluginFilter));
  try {
    return new (memory) CLAPPluginFilter(host, std::move(id));
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
    if (wideId.empty() || (input >> extra))
      throw std::runtime_error("expected Plugin: CLAP <plugin-id>");
    return {allocateFilter(host, StringHelper::toString(wideId, 65001))};
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
  return {id, found->name, {}};
}

std::unique_ptr<IPluginInstance>
CLAPPluginHost::create(const std::string &id, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) {
  if (!parameters.empty())
    throw std::runtime_error("CLAP config parameter overrides are not yet "
                             "implemented; no values were applied");
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
                                            sampleRate, maxFrames, channels);
  }
  throw std::runtime_error("CLAP plugin descriptor disappeared: " + id);
}

std::unique_ptr<IFilterFactory> makeCLAPPluginFilterFactory() {
  return std::make_unique<CLAPPluginFilterFactory>();
}
