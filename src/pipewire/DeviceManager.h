#pragma once
#include "DeviceIdentity.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
struct AudioDevice {
  uint32_t id;
  std::string name, description;
  unsigned channels{}, sampleRate{};
  skyapo::pipewire::DeviceIdentityMetadata identity;
  // Unknown when the PipeWire default metadata key is unavailable or invalid.
  std::optional<bool> isDefault;
};
struct AudioPort {
  uint32_t id, node;
  std::string direction, channel, name;
};
struct Devices {
  std::vector<AudioDevice> sources;
  std::vector<AudioPort> ports;
};
inline void
markDefaultCaptureSource(Devices &devices,
                         const std::optional<std::string> &defaultSourceName) {
  const bool known = defaultSourceName && !defaultSourceName->empty();
  for (auto &device : devices.sources)
    device.isDefault =
        known ? std::optional<bool>(device.name == *defaultSourceName)
              : std::nullopt;
}
Devices enumerateDevices();
std::wstring deviceMatchText(const AudioDevice &device);
