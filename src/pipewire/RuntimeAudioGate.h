#pragma once

namespace skyapo::pipewire {

enum class AudioGateMode : unsigned { Running, Quiescing, Faulted, Stopping };

constexpr bool pdcFormatRebuildMustRemainFaulted(bool faulted,
                                                 bool refreshQueued) noexcept {
  return faulted || refreshQueued;
}

// A queued latency refresh is an unresolved PDC validation, not permission to
// resume. Keep output quiesced until the control thread validates the live
// engine(s), or retain the stronger fault/stop state.
constexpr AudioGateMode resumeAudioGate(AudioGateMode current, bool faulted,
                                        bool refreshQueued) noexcept {
  if (current == AudioGateMode::Stopping)
    return AudioGateMode::Stopping;
  if (faulted)
    return AudioGateMode::Faulted;
  if (refreshQueued)
    return AudioGateMode::Quiescing;
  return AudioGateMode::Running;
}

constexpr bool audioOutputMustBeSilent(AudioGateMode current,
                                       bool refreshQueued) noexcept {
  return current != AudioGateMode::Running || refreshQueued;
}

} // namespace skyapo::pipewire
