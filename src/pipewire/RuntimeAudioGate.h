#pragma once

namespace skyapo::pipewire {

enum class RuntimeAudioMode : unsigned {
  Running,
  Quiescing,
  Faulted,
  Stopping
};

constexpr bool
configReloadNeedsValidation(bool pluginLatencyFaulted,
                            bool callbackProcessingFaulted,
                            bool pluginLatencyEventQueued) noexcept {
  return pluginLatencyFaulted || callbackProcessingFaulted ||
         pluginLatencyEventQueued;
}

constexpr bool formatRebuildMustDefer(bool pluginLatencyFaulted,
                                      bool callbackProcessingFaulted,
                                      bool pluginLatencyEventQueued) noexcept {
  return configReloadNeedsValidation(pluginLatencyFaulted,
                                     callbackProcessingFaulted,
                                     pluginLatencyEventQueued);
}

constexpr RuntimeAudioMode
resumeRuntimeAudioGate(RuntimeAudioMode current, bool pluginLatencyFaulted,
                       bool callbackProcessingFaulted,
                       bool pluginLatencyEventQueued) noexcept {
  if (current == RuntimeAudioMode::Stopping)
    return RuntimeAudioMode::Stopping;
  if (pluginLatencyFaulted || callbackProcessingFaulted)
    return RuntimeAudioMode::Faulted;
  if (pluginLatencyEventQueued)
    return RuntimeAudioMode::Quiescing;
  return RuntimeAudioMode::Running;
}

constexpr bool audioOutputMustBeSilent(RuntimeAudioMode current,
                                       bool pluginLatencyEventQueued) noexcept {
  return current != RuntimeAudioMode::Running || pluginLatencyEventQueued;
}

} // namespace skyapo::pipewire
