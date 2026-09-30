#pragma once

#include "IPluginInstance.h"
#include <memory>
#include <string>
#include <vector>

// Experimental in-process VST2-compatible host backed only by the FST
// reverse-engineered API header. This is deliberately not wired into Engine
// or the config parser yet.
class VST2PluginHost {
public:
  std::unique_ptr<IPluginInstance>
  create(const std::string &modulePath, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels,
         const std::vector<PluginParameterValue> &parameters = {}) const;
};
