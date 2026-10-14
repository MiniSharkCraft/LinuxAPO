#include "../src/pipewire/DeviceIdentity.h"
#include "../src/pipewire/DeviceSelection.h"

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

  Devices devices;
  AudioDevice renamed{52,      "skyapo.test.renamed", "Test input", 2, 48000,
                      identity};
  devices.sources.push_back(renamed);
  const std::string oldName = "skyapo.test.input";
  const std::string stable = "device.serial=USB-serial-42";
  if (findSelectedDevice(devices, oldName, stable) != &devices.sources[0] ||
      !isSelectedDevice(devices.sources[0], oldName, stable)) {
    std::cerr << "saved stable identity did not recover a renamed node\n";
    return 1;
  }

  AudioDevice sameNameWithoutIdentity{53, oldName, "Different device",
                                      2,  48000,   {}};
  Devices missingStableIdentity;
  missingStableIdentity.sources.push_back(sameNameWithoutIdentity);
  if (findSelectedDevice(missingStableIdentity, oldName, stable) != nullptr ||
      isSelectedDevice(missingStableIdentity.sources[0], oldName, stable)) {
    std::cerr << "missing stable identity incorrectly fell back to node name\n";
    return 1;
  }

  if (findSelectedDevice(missingStableIdentity, oldName, {}) !=
      &missingStableIdentity.sources[0]) {
    std::cerr << "legacy name-only selection did not retain its fallback\n";
    return 1;
  }

  Devices ambiguous;
  ambiguous.sources.push_back(renamed);
  ambiguous.sources.push_back(AudioDevice{
      54, "another.node", "Duplicate identity", 2, 48000, identity});
  bool rejectedAmbiguousIdentity = false;
  try {
    (void)findSelectedDevice(ambiguous, oldName, stable);
  } catch (const std::runtime_error &) {
    rejectedAmbiguousIdentity = true;
  }
  if (!rejectedAmbiguousIdentity) {
    std::cerr << "ambiguous stable identity was not rejected\n";
    return 1;
  }

  Devices defaultSources;
  defaultSources.sources.push_back(
      AudioDevice{55, "source.default", "Default source", 2, 48000, {}});
  defaultSources.sources.push_back(
      AudioDevice{56, "source.other", "Other source", 1, 44100, {}});
  markDefaultCaptureSource(defaultSources, std::string("source.default"));
  if (defaultSources.sources[0].isDefault != std::optional<bool>(true) ||
      defaultSources.sources[1].isDefault != std::optional<bool>(false)) {
    std::cerr
        << "PipeWire default source name was not mapped to capture nodes\n";
    return 1;
  }
  markDefaultCaptureSource(defaultSources, std::nullopt);
  if (defaultSources.sources[0].isDefault.has_value() ||
      defaultSources.sources[1].isDefault.has_value()) {
    std::cerr << "missing default-source metadata was not kept unknown\n";
    return 1;
  }

  std::cout
      << "device identity/default metadata, renamed-node recovery, legacy "
         "fallback, unknown default state, and ambiguity tests passed\n";
}
