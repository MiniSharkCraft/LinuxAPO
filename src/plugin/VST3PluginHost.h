#pragma once

#include "IPluginInstance.h"
#include "IFilterFactory.h"
#include <memory>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

class VST3PluginHost final : public IPluginHost {
public:
  std::unique_ptr<IPluginInstance>
  create(const std::string &uid, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels,
         const std::vector<PluginParameterValue> &parameters = {}) override;
  std::unique_ptr<IPluginInstance>
  createForConfig(const std::string &uid, float sampleRate, unsigned maxFrames,
                  const std::vector<std::wstring> &channels,
                  const std::vector<PluginParameterValue> &parameters,
                  const std::filesystem::path &source, unsigned line);
  PluginDescription describe(const std::string &uid) const;
  std::vector<std::pair<std::string, std::string>> list() const;
};

std::unique_ptr<IFilterFactory> makeVST3PluginFilterFactory();
