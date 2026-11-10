#include "../src/platform/Settings.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "skyapo-output-settings-XXXXXX")
          .string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  char *directory = mkdtemp(writable.data());
  if (!directory) {
    std::cerr << "could not create isolated settings test directory\n";
    return 1;
  }
  const std::filesystem::path root(directory);
  if (setenv("XDG_CONFIG_HOME", root.c_str(), 1) != 0) {
    std::filesystem::remove_all(root);
    std::cerr << "could not set isolated XDG_CONFIG_HOME\n";
    return 1;
  }
  try {
    settings::selectOutput("alsa_output.usb-test", "device.serial=usb-42");
    const auto saved = settings::outputDeviceSelection();
    if (saved.name != "alsa_output.usb-test" ||
        saved.stableProperty != "device.serial=usb-42")
      throw std::runtime_error("stable output selection did not round-trip");

    settings::restoreOutputSelection({"alsa_output.old", "device.serial=old"});
    const auto restored = settings::outputDeviceSelection();
    if (restored.name != "alsa_output.old" ||
        restored.stableProperty != "device.serial=old")
      throw std::runtime_error(
          "previous stable output selection was not restored");
    settings::restoreOutputSelection({});
    const auto cleared = settings::outputDeviceSelection();
    if (!cleared.name.empty() || !cleared.stableProperty.empty())
      throw std::runtime_error(
          "empty previous output selection was not restored");

    bool rejectedNewline = false;
    try {
      settings::selectOutput("bad\nnode", "");
    } catch (const std::runtime_error &) {
      rejectedNewline = true;
    }
    if (!rejectedNewline)
      throw std::runtime_error("newline in output node name was accepted");

    bool rejectedOversizedIdentity = false;
    try {
      settings::selectOutput("output", std::string(4097, 'x'));
    } catch (const std::runtime_error &) {
      rejectedOversizedIdentity = true;
    }
    if (!rejectedOversizedIdentity)
      throw std::runtime_error("oversized output identity was accepted");
  } catch (const std::exception &error) {
    std::filesystem::remove_all(root);
    std::cerr << error.what() << '\n';
    return 1;
  }
  std::filesystem::remove_all(root);
  std::cout
      << "stable output selection persistence and input validation passed\n";
}
