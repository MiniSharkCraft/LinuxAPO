#pragma once

#include <cstdint>

// Linux-only preflight contract for upstream libHybridConv allocations.
// Engine uses this before calling IFilter::initialize(), which may allocate
// large FFT state without reporting allocation failures.
class ConvolutionMemoryEstimator {
public:
  virtual ~ConvolutionMemoryEstimator() = default;
  virtual uint64_t estimatedMemoryBytes(float sampleRate, unsigned frameCount,
                                        unsigned outputChannels) const = 0;
};
