#pragma once

#include <pipewire/core.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <spa/pod/pod.h>

#include <cstdint>
#include <limits>
#include <string>

struct pw_metadata;

namespace skyapo::pipewire {

/**
 * Plain immutable-by-convention description of the current default sink.
 * effectiveGain is the RMS of SPA_PROP_channelVolumes (sqrt(mean(v*v))),
 * which is a stable summary when channels have different gains. dB is
 * 20*log10(effectiveGain), or negative infinity when gain is zero. muted is
 * true for explicit mute or zero effective gain. A missing/malformed default
 * is represented as available=false and effectiveGain=0.
 */
struct DefaultSinkVolumeSnapshot {
  std::string stableName;
  bool available = false;
  bool muted = false;
  float effectiveGain = 0.0f;
  float effectiveDb = -std::numeric_limits<float>::infinity();
  std::uint64_t generation = 0;
};

/**
 * Tracks metadata namespace `default`, key `default.audio.sink`, resolves its
 * `name` (stable node.name) to an Audio/Sink node and enumerates/subscribes to
 * SPA_PARAM_Props. Construct/destroy and all callbacks on the owning
 * PipeWire main-loop thread. Callback is invoked only when published state
 * changes; it receives a const snapshot valid for the duration of the call.
 * No work is done on an audio realtime callback.
 */
class DefaultSinkVolumeMonitor {
public:
  using Callback = void (*)(void *userData,
                            const DefaultSinkVolumeSnapshot &snapshot);

  DefaultSinkVolumeMonitor(pw_core *core, pw_registry *registry,
                           Callback callback, void *userData);
  ~DefaultSinkVolumeMonitor();

  DefaultSinkVolumeMonitor(const DefaultSinkVolumeMonitor &) = delete;
  DefaultSinkVolumeMonitor &operator=(const DefaultSinkVolumeMonitor &) =
      delete;

  const DefaultSinkVolumeSnapshot &snapshot() const noexcept;

  /** Parse an SPA_PARAM_Props pod. Public for isolated native-pod tests. */
  static bool parseProps(const spa_pod *pod,
                        DefaultSinkVolumeSnapshot &snapshot) noexcept;

  /** Extract stable node.name from standard PipeWire default metadata JSON. */
  static bool parseDefaultSinkMetadata(const char *value,
                                       std::string &stableName);

private:
  struct Impl;
  Impl *impl_;
};

} // namespace skyapo::pipewire
