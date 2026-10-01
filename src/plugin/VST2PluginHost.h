#pragma once

#include "IPluginInstance.h"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Experimental in-process VST2-compatible host backed only by the FST
// reverse-engineered API header. Engine registration is opt-in at build time.
class VST2PluginHost : public IPluginHost {
public:
  std::unique_ptr<IPluginInstance>
  create(const std::string &modulePath, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels,
         const std::vector<PluginParameterValue> &parameters = {}) override;
  std::unique_ptr<IPluginInstance>
  createForConfig(const std::string &modulePath, float sampleRate,
                  unsigned maxFrames,
                  const std::vector<std::wstring> &channels,
                  const std::vector<PluginParameterValue> &parameters,
                  const std::filesystem::path &source, unsigned sourceLine);
};
