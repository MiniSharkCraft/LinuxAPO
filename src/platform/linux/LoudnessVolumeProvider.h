#pragma once

#include <atomic>
#include <cmath>

namespace skyapo::platform {

/**
 * Process-wide snapshot consumed by the upstream EAPO LoudnessCorrection
 * update thread. Writers run on the PipeWire control loop; readers run on that
 * filter's non-realtime polling thread. No PipeWire API crosses this boundary.
 */
class LoudnessVolumeProvider {
public:
  static void publish(bool available, float endpointLevelDb) noexcept {
    if (available && std::isfinite(endpointLevelDb))
      levelDb_.store(endpointLevelDb, std::memory_order_relaxed);
    else
      available = false;
    available_.store(available, std::memory_order_release);
  }

  static bool read(float &endpointLevelDb) noexcept {
    if (!available_.load(std::memory_order_acquire))
      return false;
    endpointLevelDb = levelDb_.load(std::memory_order_relaxed);
    return std::isfinite(endpointLevelDb);
  }

  static bool available() noexcept {
    return available_.load(std::memory_order_acquire);
  }

private:
  inline static std::atomic<bool> available_{false};
  inline static std::atomic<float> levelDb_{0.0f};
};

} // namespace skyapo::platform
