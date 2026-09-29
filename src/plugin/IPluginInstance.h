#pragma once

#include <memory>
#include <string>
#include <vector>

// Format-neutral DSP boundary. Format hosts create and configure instances on
// the control thread; process() is called by the audio graph.
class IPluginInstance {
public:
  virtual ~IPluginInstance() = default;
  virtual const std::string &uri() const noexcept = 0;
  virtual std::vector<std::wstring>
  initialize(float sampleRate, unsigned maxFrames,
             const std::vector<std::wstring> &channels) = 0;
  virtual void process(float **output, float **input,
                       unsigned frames) noexcept = 0;
};

class IPluginHost {
public:
  virtual ~IPluginHost() = default;
  virtual std::unique_ptr<IPluginInstance>
  create(const std::string &uri, float sampleRate, unsigned maxFrames,
         const std::vector<std::wstring> &channels) = 0;
};
