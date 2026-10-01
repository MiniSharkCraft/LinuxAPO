#pragma once

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <vector>

#include "PluginLatencyLimits.h"

// Control-thread interface for transparent host bypass. Implementations
// publish the value atomically; the audio callback reads it without locking.
class IPluginBypassControl {
public:
  virtual ~IPluginBypassControl() = default;
  virtual void setPluginBypassed(bool bypassed) noexcept = 0;
  virtual bool pluginBypassed() const noexcept = 0;
};

class AtomicPluginBypass : public IPluginBypassControl {
public:
  void setPluginBypassed(bool bypassed) noexcept override {
    bypassed_.store(bypassed, std::memory_order_release);
  }
  bool pluginBypassed() const noexcept override {
    return bypassed_.load(std::memory_order_acquire);
  }

  // Called while a candidate graph is being built, never from process().
  // The dry path uses the plugin's known initial latency so toggling host
  // bypass does not move the signal earlier in time.
  void prepareBypassDelay(uint32_t latencySamples, unsigned channels) {
    if (latencySamples > skyapo::plugin::MaxRealtimeLatencySamples)
      throw std::runtime_error(
          "plugin latency exceeds SkyAPO's realtime compensation safety limit");
    if (!latencySamples) {
      bypassDelay_.clear();
      bypassLatency_ = 0;
      bypassChannels_ = channels;
      bypassCursor_ = 0;
      return;
    }
    if (!channels || latencySamples > std::numeric_limits<size_t>::max() /
                                          static_cast<size_t>(channels))
      throw std::runtime_error("plugin bypass delay buffer size overflow");
    bypassLatency_ = latencySamples;
    bypassChannels_ = channels;
    bypassCursor_ = 0;
    bypassDelay_.assign(static_cast<size_t>(latencySamples) * channels, 0.0f);
  }

protected:
  bool copyInputWhenBypassed(float **output, float **input, unsigned frames,
                             unsigned channels) noexcept {
    const bool bypassed = pluginBypassed();
    if (!bypassLatency_) {
      if (!bypassed)
        return false;
      for (unsigned channel = 0; channel < channels; ++channel)
        std::copy_n(input[channel], frames, output[channel]);
      return true;
    }
    if (channels != bypassChannels_)
      return false;
    for (unsigned frame = 0; frame < frames; ++frame) {
      const size_t base = static_cast<size_t>(bypassCursor_) * channels;
      for (unsigned channel = 0; channel < channels; ++channel) {
        const size_t index = base + channel;
        const float delayed = bypassDelay_[index];
        bypassDelay_[index] = input[channel][frame];
        if (bypassed)
          output[channel][frame] = delayed;
      }
      if (++bypassCursor_ == bypassLatency_)
        bypassCursor_ = 0;
    }
    return bypassed;
  }

private:
  std::atomic<bool> bypassed_{false};
  std::vector<float> bypassDelay_;
  uint32_t bypassLatency_{};
  unsigned bypassChannels_{};
  uint32_t bypassCursor_{};
};

static_assert(std::atomic<bool>::is_always_lock_free,
              "plugin bypass state must be lock-free on the audio thread");
