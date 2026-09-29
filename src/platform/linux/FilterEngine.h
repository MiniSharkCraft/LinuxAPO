/*
 * Minimal Linux construction context for the portable upstream
 * FilterConfiguration implementation. The Windows FilterEngine is not
 * compiled; SkyAPO supplies only the immutable metadata its configuration
 * needs to size planar work buffers.
 */
#pragma once

class FilterEngine {
public:
  FilterEngine(unsigned realChannels, unsigned outputChannels,
               unsigned maxFrames)
      : realChannelCount(realChannels), outputChannelCount(outputChannels),
        maxFrameCount(maxFrames) {}

  unsigned getRealChannelCount() const { return realChannelCount; }
  unsigned getOutputChannelCount() const { return outputChannelCount; }
  unsigned getMaxFrameCount() const { return maxFrameCount; }

private:
  unsigned realChannelCount;
  unsigned outputChannelCount;
  unsigned maxFrameCount;
};
