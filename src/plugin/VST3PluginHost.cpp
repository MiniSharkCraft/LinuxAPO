#include "VST3PluginHost.h"

#include "IFilter.h"
#include "IFilterFactory.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/vstspeaker.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
using VST3::Hosting::Module;
struct CatalogItem {
  Module::Ptr module;
  VST3::Hosting::ClassInfo info;
  std::string uid;
  std::string name;
};
using Catalog = std::vector<CatalogItem>;
Steinberg::Vst::HostApplication &hostApplication() {
  static Steinberg::Vst::HostApplication host;
  return host;
}

std::vector<std::string> modulePaths() {
  auto paths = Module::getModulePaths();
  if (const char *env = std::getenv("VST3_PATH")) {
    std::string list(env);
    size_t begin = 0;
    while (begin <= list.size()) {
      const size_t end = list.find(':', begin);
      const auto value = list.substr(begin, end == std::string::npos
                                                ? std::string::npos
                                                : end - begin);
      if (!value.empty()) {
        std::error_code ec;
        const std::filesystem::path root(value);
        if (root.extension() == ".vst3") {
          paths.push_back(root.string());
        } else if (std::filesystem::is_directory(root, ec)) {
          std::filesystem::recursive_directory_iterator it(
              root, std::filesystem::directory_options::skip_permission_denied,
              ec), finish;
          for (; it != finish; it.increment(ec)) {
            if (ec) {
              ec.clear();
              continue;
            }
            if (it->is_directory(ec) && it->path().extension() == ".vst3") {
              paths.push_back(it->path().string());
              it.disable_recursion_pending();
            }
          }
        }
      }
      if (end == std::string::npos)
        break;
      begin = end + 1;
    }
  }
  std::sort(paths.begin(), paths.end());
  paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
  return paths;
}

const Catalog &catalog() {
  static const Catalog cache = [] {
    Catalog result;
    for (const auto &path : modulePaths()) {
      std::error_code ec;
      if (!std::filesystem::exists(path, ec))
        continue;
      std::string error;
      auto module = Module::create(path, error);
      if (!module)
        continue;
      for (const auto &info : module->getFactory().classInfos()) {
        if (info.category() == "Audio Module Class")
          result.push_back({module, info, info.ID().toString(false), info.name()});
      }
    }
    return result;
  }();
  return cache;
}

class VST3Instance final : public IPluginInstance {
public:
  VST3Instance(Module::Ptr pluginModule,
               const VST3::Hosting::ClassInfo &classInfo, std::string uid,
               float sampleRate, unsigned maxFrames,
               const std::vector<std::wstring> &channels)
      : module(std::move(pluginModule)), pluginUid(std::move(uid)),
        channelCount(channels.size()), maxFrameCount(maxFrames) {
    using namespace Steinberg;
    using namespace Steinberg::Vst;
    if (!std::isfinite(sampleRate) || sampleRate < 8000 || !maxFrames ||
        (channelCount != 1 && channelCount != 2))
      throw std::runtime_error("VST3 host currently supports mono/stereo only");
    PluginContextFactory::instance().setPluginContext(&hostApplication());
    provider = std::make_unique<PlugProvider>(module->getFactory(), classInfo);
    if (!provider->initialize())
      throw std::runtime_error("VST3 plugin initialization failed: " + pluginUid);
    component = provider->getComponentPtr();
    processor = FUnknownPtr<IAudioProcessor>(component);
    if (!component || !processor)
      throw std::runtime_error("VST3 class lacks IComponent/IAudioProcessor: " +
                               pluginUid);
    if (component->getBusCount(kAudio, kInput) != 1 ||
        component->getBusCount(kAudio, kOutput) != 1)
      throw std::runtime_error("VST3 plugin must have one audio input/output bus: " +
                               pluginUid);
    BusInfo inputInfo{}, outputInfo{};
    if (component->getBusInfo(kAudio, kInput, 0, inputInfo) != kResultOk ||
        component->getBusInfo(kAudio, kOutput, 0, outputInfo) != kResultOk)
      throw std::runtime_error("VST3 plugin audio bus layout does not match host: " +
                               pluginUid);
    if (inputInfo.channelCount != static_cast<int32>(channelCount) ||
        outputInfo.channelCount != static_cast<int32>(channelCount))
      throw std::runtime_error("VST3 plugin bus channels are " +
                               std::to_string(inputInfo.channelCount) + "/" +
                               std::to_string(outputInfo.channelCount) +
                               ", host requires " +
                               std::to_string(channelCount) + "/" +
                               std::to_string(channelCount) + ": " + pluginUid);
    SpeakerArrangement arrangement = channelCount == 1 ? SpeakerArr::kMono
                                                        : SpeakerArr::kStereo;
    if (processor->setBusArrangements(&arrangement, 1, &arrangement, 1) !=
        kResultOk)
      throw std::runtime_error("VST3 plugin rejected mono/stereo layout: " +
                               pluginUid);
    if (component->activateBus(kAudio, kInput, 0, true) != kResultOk ||
        component->activateBus(kAudio, kOutput, 0, true) != kResultOk)
      throw std::runtime_error("VST3 plugin bus activation failed: " + pluginUid);
    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(maxFrames);
    setup.sampleRate = sampleRate;
    if (processor->setupProcessing(setup) != kResultOk ||
        component->setActive(true) != kResultOk)
      throw std::runtime_error("VST3 plugin setup failed: " + pluginUid);
    active = true;
    // Steinberg's own AudioClient does not gate activation on this return
    // value: several compliant plug-ins return kResultFalse while still
    // processing normally. We still check the process() result per block.
    (void)processor->setProcessing(true);
    processing = true;
    inputs = std::make_unique<float *[]>(channelCount);
    outputs = std::make_unique<float *[]>(channelCount);
    inputBus.numChannels = outputBus.numChannels = static_cast<int32>(channelCount);
    inputBus.channelBuffers32 = inputs.get();
    outputBus.channelBuffers32 = outputs.get();
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numInputs = data.numOutputs = 1;
    data.inputs = &inputBus;
    data.outputs = &outputBus;
  }

  ~VST3Instance() override {
    if (processing) processor->setProcessing(false);
    if (active) component->setActive(false);
  }

  const std::string &uri() const noexcept override { return pluginUid; }
  const std::vector<PluginParameterInfo> &parameters() const noexcept override {
    return emptyParameters;
  }
  std::vector<std::wstring> initialize(float, unsigned,
      const std::vector<std::wstring> &channels) override { return channels; }
  void process(float **output, float **input, unsigned frames) noexcept override {
    if (frames > maxFrameCount) {
      for (unsigned c = 0; c < channelCount; ++c)
        std::fill_n(output[c], frames, 0.0f);
      return;
    }
    for (size_t c = 0; c < channelCount; ++c) {
      inputs[c] = input[c];
      outputs[c] = output[c];
    }
    data.numSamples = static_cast<int32_t>(frames);
    if (processor->process(data) != Steinberg::kResultOk)
      for (size_t c = 0; c < channelCount; ++c)
        std::fill_n(output[c], frames, 0.0f);
  }

private:
  Module::Ptr module;
  std::string pluginUid;
  size_t channelCount{};
  unsigned maxFrameCount{};
  std::unique_ptr<Steinberg::Vst::PlugProvider> provider;
  Steinberg::IPtr<Steinberg::Vst::IComponent> component;
  Steinberg::FUnknownPtr<Steinberg::Vst::IAudioProcessor> processor;
  std::unique_ptr<float *[]> inputs, outputs;
  Steinberg::Vst::AudioBusBuffers inputBus{}, outputBus{};
  Steinberg::Vst::ProcessData data{};
  std::vector<PluginParameterInfo> emptyParameters;
  bool active = false, processing = false;
};

class VST3PluginFilter final : public IFilter {
public:
  VST3PluginFilter(VST3PluginHost &owner, std::string uid)
      : host(owner), pluginUid(std::move(uid)) {}
  bool getInPlace() override { return false; }
  std::vector<std::wstring> initialize(float rate, unsigned maxFrames,
                                       std::vector<std::wstring> channels) override {
    instance = host.create(pluginUid, rate, maxFrames, channels);
    return instance->initialize(rate, maxFrames, channels);
  }
  void process(float **output, float **input, unsigned frames) override {
    instance->process(output, input, frames);
  }
private:
  VST3PluginHost &host;
  std::string pluginUid;
  std::unique_ptr<IPluginInstance> instance;
};

IFilter *allocateFilter(VST3PluginHost &host, std::string uid) {
  void *memory = MemoryHelper::alloc(sizeof(VST3PluginFilter));
  try { return new (memory) VST3PluginFilter(host, std::move(uid)); }
  catch (...) { MemoryHelper::free(memory); throw; }
}

class VST3FilterFactory final : public IFilterFactory {
public:
  VST3FilterFactory() : host(std::make_unique<VST3PluginHost>()) {}
  std::vector<IFilter *> createFilter(const std::wstring &, std::wstring &command,
                                      std::wstring &parameters) override {
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, uid;
    input >> format >> uid;
    if (format != L"VST3")
      return {};
    if (uid.empty())
      throw std::runtime_error("expected Plugin: VST3 <class-uid>");
    std::wstring token;
    if (input >> token)
      throw std::runtime_error("VST3 config parameter overrides are not supported yet");
    return {allocateFilter(*host, StringHelper::toString(uid, 65001))};
  }
private:
  std::unique_ptr<VST3PluginHost> host;
};
} // namespace

std::unique_ptr<IPluginInstance>
VST3PluginHost::create(const std::string &uid, float sampleRate,
                       unsigned maxFrames,
                       const std::vector<std::wstring> &channels,
                       const std::vector<PluginParameterValue> &parameters) {
  if (!parameters.empty())
    throw std::runtime_error("VST3 parameter overrides are not supported yet");
  for (const auto &item : catalog())
    if (item.uid == uid)
      return std::make_unique<VST3Instance>(item.module, item.info, uid,
                                            sampleRate, maxFrames, channels);
  throw std::runtime_error("VST3 class UID not found: " + uid);
}

PluginDescription VST3PluginHost::describe(const std::string &uid) const {
  for (const auto &item : catalog())
    if (item.uid == uid)
      return {item.uid, item.name, {}};
  throw std::runtime_error("VST3 class UID not found: " + uid);
}

std::vector<std::pair<std::string, std::string>> VST3PluginHost::list() const {
  std::vector<std::pair<std::string, std::string>> result;
  for (const auto &item : catalog())
    result.emplace_back(item.uid, item.name);
  return result;
}

std::unique_ptr<IFilterFactory> makeVST3PluginFilterFactory() {
  return std::make_unique<VST3FilterFactory>();
}
