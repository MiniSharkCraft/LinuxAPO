#include "VST2PluginFilter.h"

#include "IPluginBypassControl.h"
#include "IPluginFailureState.h"
#include "IPluginParameterControl.h"
#include "IPluginLatencyState.h"
#include "VST2PluginHost.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <new>
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
                               public IPluginLatencyState {
public:
  VST2PluginFilter(VST2PluginHost &host, std::string modulePath,
                   std::vector<PluginParameterValue> overrides)
      : host(host), modulePath(std::move(modulePath)),
        parameterOverrides(std::move(overrides)) {}

  bool getInPlace() override {
    return false;
  }
  std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrames,
             std::vector<std::wstring> channels) override {
    channelCount = static_cast<unsigned>(channels.size());
    instance = host.create(modulePath, sampleRate, maxFrames, channels,
                           parameterOverrides);
    return instance->initialize(sampleRate, maxFrames, channels);
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

private:
  VST2PluginHost &host;
  std::string modulePath;
  std::vector<PluginParameterValue> parameterOverrides;
  std::unique_ptr<IPluginInstance> instance;
  unsigned channelCount{};
};

IFilter *allocateVST2Filter(VST2PluginHost &host, std::string modulePath,
                            std::vector<PluginParameterValue> overrides) {
  void *memory = MemoryHelper::alloc(sizeof(VST2PluginFilter));
  try {
    return new (memory)
        VST2PluginFilter(host, std::move(modulePath), std::move(overrides));
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
}

class VST2FilterFactory final : public IFilterFactory {
public:
  std::vector<IFilter *> createFilter(const std::wstring &,
                                      std::wstring &command,
                                      std::wstring &parameters) override {
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
                               std::move(overrides))};
  }

private:
  VST2PluginHost host;
};
} // namespace

std::unique_ptr<IFilterFactory> makeVST2PluginFilterFactory() {
  return std::make_unique<VST2FilterFactory>();
}
