#include "../src/pipewire/DefaultSinkVolumeMonitor.h"

#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "CHECK failed at " << __FILE__ << ':' << __LINE__ << ": " \
                << #condition << '\n';                                       \
      return 1;                                                                \
    }                                                                          \
  } while (false)

namespace {
const spa_pod *makeProps(uint8_t *storage, uint32_t capacity,
                         const float *volumes, uint32_t count, bool mute) {
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, capacity);
  return static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(mute), SPA_PROP_volume, SPA_POD_Float(0.0f),
      SPA_PROP_channelVolumes,
      SPA_POD_Array(sizeof(float), SPA_TYPE_Float, count, volumes)));
}
} // namespace

int main() {
  using Monitor = skyapo::pipewire::DefaultSinkVolumeMonitor;
  using Snapshot = skyapo::pipewire::DefaultSinkVolumeSnapshot;

  std::string name;
  CHECK(Monitor::parseDefaultSinkMetadata(
      R"({"name":"alsa_output.pci-0000_00_1f.3.analog-stereo","description":"Built-in"})",
      name));
  CHECK(name == "alsa_output.pci-0000_00_1f.3.analog-stereo");
  CHECK(!Monitor::parseDefaultSinkMetadata("not-json", name));
  CHECK(!Monitor::parseDefaultSinkMetadata(R"({"other":"sink"})", name));
  CHECK(!Monitor::parseDefaultSinkMetadata(R"({"name":""})", name));

  uint8_t storage[512];
  const float stereo[] = {0.5f, 1.0f};
  auto *pod = makeProps(storage, sizeof(storage), stereo, 2, false);
  Snapshot snapshot;
  CHECK(Monitor::parseProps(pod, snapshot));
  const float expected = std::sqrt((0.25f + 1.0f) / 2.0f);
  CHECK(std::abs(snapshot.effectiveGain - expected) < 1e-6f);
  CHECK(std::abs(snapshot.effectiveDb - 20.0f * std::log10(expected)) <
        1e-5f);
  CHECK(!snapshot.muted);

  pod = makeProps(storage, sizeof(storage), stereo, 2, true);
  CHECK(Monitor::parseProps(pod, snapshot));
  CHECK(snapshot.muted);

  const float silent[] = {0.0f, 0.0f};
  pod = makeProps(storage, sizeof(storage), silent, 2, false);
  CHECK(Monitor::parseProps(pod, snapshot));
  CHECK(snapshot.muted && snapshot.effectiveGain == 0.0f);
  CHECK(std::isinf(snapshot.effectiveDb) && snapshot.effectiveDb < 0);

  const float invalid[] = {1.0f, std::numeric_limits<float>::quiet_NaN()};
  pod = makeProps(storage, sizeof(storage), invalid, 2, false);
  CHECK(!Monitor::parseProps(pod, snapshot));
  const float negative[] = {1.0f, -0.1f};
  pod = makeProps(storage, sizeof(storage), negative, 2, false);
  CHECK(!Monitor::parseProps(pod, snapshot));

  // Non-array property and incomplete Props object fail closed.
  spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
  pod = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute,
      SPA_POD_Bool(false), SPA_PROP_channelVolumes, SPA_POD_Float(1.0f)));
  CHECK(!Monitor::parseProps(pod, snapshot));

  std::cout << "default sink metadata and Props parsing passed\n";
}
