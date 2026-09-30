#include "LV2PluginHost.h"

#include <array>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

int main() {
  LV2PluginHost host;
  auto plugin = host.create("https://skyapo.example/plugins/test-gain", 48000,
                            4, {L"L", L"R"});
  std::array<float, 5> leftInput{1, 2, 3, 4, 5};
  std::array<float, 5> rightInput{1, 2, 3, 4, 5};
  std::array<float, 5> leftOutput{7, 7, 7, 7, 7};
  std::array<float, 5> rightOutput{7, 7, 7, 7, 7};
  float *inputs[]{leftInput.data(), rightInput.data()};
  float *outputs[]{leftOutput.data(), rightOutput.data()};
  plugin->process(outputs, inputs, 5);
  for (float sample : leftOutput)
    if (!std::isfinite(sample) || sample != 0.0f) {
      std::cerr << "LV2 oversized block leaked stale output\n";
      return 1;
    }
  for (float sample : rightOutput)
    if (!std::isfinite(sample) || sample != 0.0f) {
      std::cerr << "LV2 oversized block leaked stale output\n";
      return 1;
    }
  std::cout << "LV2 oversized block is zero-filled\n";
  return 0;
}
