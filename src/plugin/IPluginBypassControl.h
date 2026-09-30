#pragma once

#include <algorithm>
#include <atomic>

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

protected:
  bool copyInputWhenBypassed(float **output, float **input, unsigned frames,
                             unsigned channels) const noexcept {
    if (!pluginBypassed())
      return false;
    for (unsigned channel = 0; channel < channels; ++channel)
      std::copy_n(input[channel], frames, output[channel]);
    return true;
  }

private:
  std::atomic<bool> bypassed_{false};
};

static_assert(std::atomic<bool>::is_always_lock_free,
              "plugin bypass state must be lock-free on the audio thread");
