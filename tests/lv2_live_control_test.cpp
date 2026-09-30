#include "IPluginParameterControl.h"
#include "LV2PluginHost.h"

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
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

    for (const auto &invalid :
         std::vector<std::pair<std::string, float>>{
             {"missing", 0.5f}, {"gain", 1.01f},
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
    std::cout << "LV2 live parameter mailbox passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "LV2 live parameter test failed: " << error.what() << '\n';
    return 1;
  }
}
