#pragma once

#include "IFilterFactory.h"
#include "IPluginInstance.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

class LV2PluginHost final : public IPluginHost {
public:
  LV2PluginHost();
  ~LV2PluginHost() override;
  LV2PluginHost(const LV2PluginHost &) = delete;
  LV2PluginHost &operator=(const LV2PluginHost &) = delete;

  std::unique_ptr<IPluginInstance>
  create(const std::string &uri, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels,
         const std::vector<PluginParameterValue> &parameters = {}) override;
  PluginDescription describe(const std::string &uri) const;
  std::vector<std::pair<std::string, std::string>> list() const;
};

std::unique_ptr<IFilterFactory> makeLV2PluginFilterFactory();
