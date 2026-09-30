#include "IPluginParameterControl.h"
#include "LV2PluginHost.h"
#include "../core/Engine.h"

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#include <unistd.h>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

int main() {
  try {
    LV2PluginHost host;
    auto instance = host.create("https://skyapo.example/plugins/test-gain",
                                48000.0f, 64, {L"L", L"R"}, {{"gain", 0.25f}});
    auto *control = dynamic_cast<IPluginParameterControl *>(instance.get());
    if (!control || control->pluginIdentifier() !=
                        "https://skyapo.example/plugins/test-gain") {
      std::cerr << "LV2 live control interface missing\n";
      return 1;
    }

    std::array<float, 8> leftIn{1, 1, 1, 1, 1, 1, 1, 1};
    std::array<float, 8> rightIn{1, 1, 1, 1, 1, 1, 1, 1};
    std::array<float, 8> leftOut{};
    std::array<float, 8> rightOut{};
    float *input[]{leftIn.data(), rightIn.data()};
    float *output[]{leftOut.data(), rightOut.data()};
    instance->process(output, input, leftIn.size());
    if (std::abs(leftOut.front() - 0.25f) > 1e-6f) {
      std::cerr << "LV2 config override was not applied\n";
      return 1;
    }

    control->setParameterValue("Gain", 0.75f);
    leftOut.fill(0.0f);
    rightOut.fill(0.0f);
    instance->process(output, input, leftIn.size());
    if (std::abs(leftOut.front() - 0.75f) > 1e-6f ||
        std::abs(rightOut.back() - 0.75f) > 1e-6f) {
      std::cerr << "LV2 live control was not applied at process boundary\n";
      return 1;
    }

    for (const auto &invalid : std::vector<std::pair<std::string, float>>{
             {"missing", 0.5f},
             {"gain", 1.01f},
             {"gain", std::numeric_limits<float>::infinity()}}) {
      bool rejected = false;
      try {
        control->setParameterValue(invalid.first, invalid.second);
      } catch (const std::exception &) {
        rejected = true;
      }
      if (!rejected) {
        std::cerr << "invalid LV2 live control accepted: " << invalid.first
                  << '\n';
        return 1;
      }
    }

    const auto stateRoot = std::filesystem::temp_directory_path() /
                           ("skyapo-lv2-state-" + std::to_string(getpid()));
    std::filesystem::remove_all(stateRoot);
    std::filesystem::create_directories(stateRoot);
    if (setenv("XDG_STATE_HOME", stateRoot.c_str(), 1) != 0)
      throw std::runtime_error("cannot set isolated LV2 state directory");
    const auto config = stateRoot / "state-test.txt";
    {
      std::ofstream out(config);
      out << "Plugin: LV2 https://skyapo.example/plugins/test-gain gain=0.25\n";
    }
    {
      Engine engine(48000, 2, 64);
      engine.loadConfig(config.string());
      engine.setPluginParameter("https://skyapo.example/plugins/test-gain",
                                "gain", 0.8f);
      if (engine.savePersistentPluginStates() != 1)
        throw std::runtime_error("Engine did not save LV2 plugin state");
    }
    {
      Engine restored(48000, 2, 64);
      restored.loadConfig(config.string());
      std::array<float, 16> block{};
      for (size_t i = 0; i < block.size(); ++i)
        block[i] = 1.0f;
      restored.process(block.data(), 8);
      if (std::abs(block.front() - 0.8f) > 1e-6f ||
          std::abs(block.back() - 0.8f) > 1e-6f)
        throw std::runtime_error(
            "LV2 saved state was not numerically restored");
    }
    std::filesystem::remove_all(stateRoot);
    std::cout << "LV2 live parameter mailbox passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "LV2 live parameter test failed: " << error.what() << '\n';
    return 1;
  }
}
