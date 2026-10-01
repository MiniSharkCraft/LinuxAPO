#include "VST2PluginFilter.h"

#include "IPluginBypassControl.h"
#include "IPluginFailureState.h"
#include "IPluginParameterControl.h"
#include "IPluginLatencyState.h"
#include "IPluginSourceContext.h"
#include "IPluginStatePersistence.h"
#include "VST2PluginHost.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <new>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
class VST2PluginFilter final : public IFilter,
                               public AtomicPluginBypass,
                               public IPluginParameterControl,
                               public IPluginFailureState,
                               public IPluginLatencyState,
                               public IPluginStatePersistence {
public:
  VST2PluginFilter(VST2PluginHost &host, std::string modulePath,
                   std::vector<PluginParameterValue> overrides,
                   std::filesystem::path source, unsigned sourceLine)
      : host(host), modulePath(std::move(modulePath)),
        parameterOverrides(std::move(overrides)), source(std::move(source)),
        sourceLine(sourceLine) {}

  bool getInPlace() override {
    return false;
  }
  std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrames,
             std::vector<std::wstring> channels) override {
    channelCount = static_cast<unsigned>(channels.size());
    instance = host.createForConfig(modulePath, sampleRate, maxFrames, channels,
                                    parameterOverrides, source, sourceLine);
    auto outputChannels = instance->initialize(sampleRate, maxFrames, channels);
    prepareBypassDelay(instance->latencySamples(), channelCount);
    return outputChannels;
  }
  void process(float **output, float **input, unsigned frames) override {
    if (copyInputWhenBypassed(output, input, frames, channelCount))
      return;
    instance->process(output, input, frames);
  }
  bool processingFailed() const noexcept override {
    return instance && instance->processingFailed();
  }
  const std::string &failureIdentifier() const noexcept override {
    return modulePath;
  }
  uint32_t latencySamples() const noexcept override {
    return instance ? instance->latencySamples() : 0;
  }
  const std::string &pluginIdentifier() const noexcept override {
    return modulePath;
  }
  void setParameterValue(const std::string &symbol, float value) override {
    auto *control = dynamic_cast<IPluginParameterControl *>(instance.get());
    if (!control)
      throw std::runtime_error("VST2: live parameter control is unavailable");
    control->setParameterValue(symbol, value);
  }
  bool savePersistentPluginState() override {
    auto *state = dynamic_cast<IPluginStatePersistence *>(instance.get());
    return state && state->savePersistentPluginState();
  }

private:
  VST2PluginHost &host;
  std::string modulePath;
  std::vector<PluginParameterValue> parameterOverrides;
  std::filesystem::path source;
  unsigned sourceLine{};
  std::unique_ptr<IPluginInstance> instance;
  unsigned channelCount{};
};

IFilter *allocateVST2Filter(VST2PluginHost &host, std::string modulePath,
                            std::vector<PluginParameterValue> overrides,
                            const std::filesystem::path &source,
                            unsigned sourceLine) {
  void *memory = MemoryHelper::alloc(sizeof(VST2PluginFilter));
  try {
    return new (memory)
        VST2PluginFilter(host, std::move(modulePath), std::move(overrides),
                         source, sourceLine);
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}

class VST2FilterFactory final : public IFilterFactory,
                                public IPluginSourceContext {
public:
  void setPluginSourceLocation(const std::filesystem::path &path,
                               unsigned line) override {
    source = path;
    sourceLine = line;
  }

  std::vector<IFilter *> createFilter(const std::wstring &configPath,
                                      std::wstring &command,
                                      std::wstring &parameters) override {
    if (command == L"VSTPlugin") {
      std::wistringstream input(parameters);
      std::wstring key, value;
      std::optional<std::wstring> library;
      std::vector<PluginParameterValue> overrides;
      while (input >> std::quoted(key)) {
        if (!(input >> std::quoted(value)))
          throw std::runtime_error("VSTPlugin: expected key/value pairs such "
                                   "as Library \"plugin.so\"");
        if (key == L"Library") {
          if (library || value.empty())
            throw std::runtime_error(
                "VSTPlugin: Library must be specified exactly once");
          library = value;
          continue;
        }
        if (key == L"ChunkData")
          throw std::runtime_error("VSTPlugin: ChunkData state is not "
                                   "supported by SkyAPO's Linux VST2 host");

        const auto symbol = StringHelper::toString(key, 65001);
        const auto valueText = StringHelper::toString(value, 65001);
        size_t consumed = 0;
        float parameterValue = 0.0f;
        try {
          parameterValue = std::stof(valueText, &consumed);
        } catch (const std::exception &) {
          throw std::runtime_error("VSTPlugin: invalid value for parameter '" +
                                   symbol + "'");
        }
        if (consumed != valueText.size() || !std::isfinite(parameterValue) ||
            std::any_of(
                overrides.begin(), overrides.end(),
                [&](const auto &entry) { return entry.symbol == symbol; }))
          throw std::runtime_error(
              "VSTPlugin: invalid or duplicate parameter '" + symbol + "'");
        overrides.push_back({symbol, parameterValue});
      }
      if (!library)
        throw std::runtime_error("VSTPlugin: missing Library value");
      std::filesystem::path modulePath(StringHelper::toString(*library, 65001));
      if (StringHelper::toLowerCase(modulePath.extension().wstring()) ==
          L".dll")
        throw std::runtime_error(
            "VSTPlugin: Windows DLL loading is not supported; configure a "
            "Linux-loadable module");
      if (modulePath.is_relative()) {
        const std::filesystem::path sourcePath(
            StringHelper::toString(configPath, 65001));
        modulePath = (sourcePath.parent_path() / modulePath).lexically_normal();
      }
      return {
          allocateVST2Filter(host, modulePath.string(), std::move(overrides),
                             source, sourceLine)};
    }
    if (command != L"Plugin")
      return {};
    std::wistringstream input(parameters);
    std::wstring format, widePath;
    input >> format;
    if (format != L"VST2")
      return {};
    input >> std::quoted(widePath);
    if (widePath.empty())
      throw std::runtime_error(
          "expected Plugin: VST2 <module.so> [parameter-index=value ...]");

    std::vector<PluginParameterValue> overrides;
    std::wstring token;
    while (input >> token) {
      const auto separator = token.find(L'=');
      if (separator == std::wstring::npos || separator == 0 ||
          separator + 1 == token.size())
        throw std::runtime_error(
            "expected VST2 parameter as decimal-index=value");
      const auto symbol =
          StringHelper::toString(token.substr(0, separator), 65001);
      const auto valueText =
          StringHelper::toString(token.substr(separator + 1), 65001);
      size_t consumed = 0;
      float value = 0.0f;
      try {
        value = std::stof(valueText, &consumed);
      } catch (const std::exception &) {
        throw std::runtime_error("invalid VST2 parameter override '" + symbol +
                                 "'");
      }
      if (consumed != valueText.size() || !std::isfinite(value) ||
          std::any_of(
              overrides.begin(), overrides.end(),
              [&](const auto &entry) { return entry.symbol == symbol; }))
        throw std::runtime_error(
            "invalid or duplicate VST2 parameter override '" + symbol + "'");
      overrides.push_back({symbol, value});
    }
    return {allocateVST2Filter(host, StringHelper::toString(widePath, 65001),
                               std::move(overrides), source, sourceLine)};
  }

private:
  VST2PluginHost host;
  std::filesystem::path source;
  unsigned sourceLine{};
};
} // namespace

std::unique_ptr<IFilterFactory> makeVST2PluginFilterFactory() {
  return std::make_unique<VST2FilterFactory>();
}
