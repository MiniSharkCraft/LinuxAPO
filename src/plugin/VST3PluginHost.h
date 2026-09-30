#pragma once

#include "IPluginInstance.h"
#include "IFilterFactory.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

class VST3PluginHost final : public IPluginHost {
public:
  std::unique_ptr<IPluginInstance>
  create(const std::string &uid, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels,
         const std::vector<PluginParameterValue> &parameters = {}) override;
  PluginDescription describe(const std::string &uid) const;
  std::vector<std::pair<std::string, std::string>> list() const;
};

std::unique_ptr<IFilterFactory> makeVST3PluginFilterFactory();
