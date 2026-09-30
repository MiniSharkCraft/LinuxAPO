#pragma once

#include <spa/utils/dict.h>
#include <string>

namespace skyapo::pipewire {

struct DeviceIdentityMetadata {
  std::string serial;
  std::string busId;
  std::string alsaCard;
  std::string alsaPath;

  // Prefer actual hardware identity properties. ALSA card/path are only a
  // useful fallback hint: card indexes and profile paths may change.
  std::string stableProperty() const {
    if (!serial.empty())
      return "device.serial=" + serial;
    if (!busId.empty())
      return "device.bus-id=" + busId;
    return {};
  }

  std::string fallbackHint() const {
    if (!alsaCard.empty() || !alsaPath.empty())
      return "api.alsa.card=" + alsaCard + ";api.alsa.path=" + alsaPath;
    return {};
  }
};

inline DeviceIdentityMetadata deviceIdentityFromProps(const spa_dict *props) {
  DeviceIdentityMetadata identity;
  if (!props)
    return identity;
  const auto copy = [props](const char *key) {
    const auto *value = spa_dict_lookup(props, key);
    return value ? std::string(value) : std::string{};
  };
  identity.serial = copy("device.serial");
  identity.busId = copy("device.bus-id");
  identity.alsaCard = copy("api.alsa.card");
  identity.alsaPath = copy("api.alsa.path");
  return identity;
}

} // namespace skyapo::pipewire
