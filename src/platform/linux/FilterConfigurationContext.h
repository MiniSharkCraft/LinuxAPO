#pragma once

#include "IFilterFactoryContext.h"

// Construction-time values needed by the portable upstream
// FilterConfiguration buffer owner. This is deliberately not named
// FilterEngine: the upstream runtime engine has a much larger Win32-bound API.
class FilterConfigurationContext final : public IFilterFactoryContext {
public:
  FilterConfigurationContext(unsigned realChannels, unsigned outputChannels,
                             unsigned maxFrames) noexcept
      : realChannelCount_(realChannels), outputChannelCount_(outputChannels),
        maxFrameCount_(maxFrames) {}

  unsigned getRealChannelCount() const noexcept override {
    return realChannelCount_;
  }
  unsigned getOutputChannelCount() const noexcept override {
    return outputChannelCount_;
  }
  unsigned getMaxFrameCount() const noexcept override {
    return maxFrameCount_;
  }

private:
  unsigned realChannelCount_;
  unsigned outputChannelCount_;
  unsigned maxFrameCount_;
};
