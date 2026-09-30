#include "../src/pipewire/DeviceIdentity.h"

#include <iostream>

int main() {
  using skyapo::pipewire::DeviceIdentityMetadata;
  const spa_dict_item items[] = {
      {"device.serial", "USB-serial-42"},
      {"device.bus-id", "usb-0000:00:14.0-2"},
      {"api.alsa.card", "2"},
      {"api.alsa.path", "front:0"}};
  const spa_dict props{0, 4, items};
  const auto identity = skyapo::pipewire::deviceIdentityFromProps(&props);
  if (identity.stableProperty() != "device.serial=USB-serial-42" ||
      identity.fallbackHint() != "api.alsa.card=2;api.alsa.path=front:0") {
    std::cerr << "serial identity priority or ALSA fallback hint is incorrect\n";
    return 1;
  }

  const spa_dict_item busItems[] = {{"device.bus-id", "usb-0000:00:14.0-2"}};
  const spa_dict busProps{0, 1, busItems};
  const auto busIdentity =
      skyapo::pipewire::deviceIdentityFromProps(&busProps);
  if (busIdentity.stableProperty() != "device.bus-id=usb-0000:00:14.0-2") {
    std::cerr << "device.bus-id fallback was not selected\n";
    return 1;
  }

  const spa_dict_item alsaItems[] = {{"api.alsa.card", "2"},
                                     {"api.alsa.path", "front:0"}};
  const spa_dict alsaProps{0, 2, alsaItems};
  const auto alsaIdentity =
      skyapo::pipewire::deviceIdentityFromProps(&alsaProps);
  if (!alsaIdentity.stableProperty().empty() ||
      alsaIdentity.fallbackHint() != "api.alsa.card=2;api.alsa.path=front:0") {
    std::cerr << "unstable ALSA hints were incorrectly called stable identity\n";
    return 1;
  }

  const spa_dict noProps{0, 0, nullptr};
  const auto emptyIdentity =
      skyapo::pipewire::deviceIdentityFromProps(&noProps);
  if (!emptyIdentity.stableProperty().empty() ||
      !emptyIdentity.fallbackHint().empty() ||
      !skyapo::pipewire::deviceIdentityFromProps(nullptr)
           .stableProperty()
           .empty()) {
    std::cerr << "empty PipeWire identity metadata should stay unavailable\n";
    return 1;
  }

  std::cout << "device identity metadata priority and non-stable ALSA hint tests passed\n";
}
