#pragma once

#include "DeviceManager.h"

#include <stdexcept>

inline const AudioDevice *
findSelectedDevice(const Devices &devices, const std::string &nodeName,
                   const std::string &stableProperty) {
  if (!stableProperty.empty()) {
    const AudioDevice *match = nullptr;
    for (const auto &device : devices.sources) {
      if (device.identity.stableProperty() != stableProperty)
        continue;
      if (match)
        throw std::runtime_error(
            "selected stable device identity is ambiguous: " + stableProperty);
      match = &device;
    }
    return match;
  }

  const AudioDevice *match = nullptr;
  for (const auto &device : devices.sources) {
    if (device.name != nodeName)
      continue;
    if (match)
      throw std::runtime_error("selected PipeWire node name is ambiguous: " +
                               nodeName);
    match = &device;
  }
  return match;
}

inline bool isSelectedDevice(const AudioDevice &device,
                             const std::string &nodeName,
                             const std::string &stableProperty) {
  return stableProperty.empty()
             ? device.name == nodeName
             : device.identity.stableProperty() == stableProperty;
}
