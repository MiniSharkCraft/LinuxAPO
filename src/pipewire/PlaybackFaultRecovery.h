#pragma once

#include <atomic>

namespace skyapo::pipewire::detail {

class PlaybackFaultRecovery {
public:
  struct ConfigReload {
    bool validatedForRecovery{};
  };

  static ConfigReload beginConfigReload(bool forced, bool faulted) noexcept {
    return {forced && faulted};
  }

  static void
  finishConfigReload(ConfigReload reload,
                     std::atomic<bool> &pluginLatencyFaulted,
                     std::atomic<bool> &faulted,
                     const std::atomic<bool> &controlCallbackFailed) noexcept {
    if (!reload.validatedForRecovery)
      return;

    pluginLatencyFaulted.store(false, std::memory_order_release);
    if (!controlCallbackFailed.load(std::memory_order_acquire))
      faulted.store(false, std::memory_order_release);
  }
};

} // namespace skyapo::pipewire::detail
