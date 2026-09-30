#include "UpstreamFilterEngineProcess.h"

#include "FilterConfiguration.h"
#include "FilterConfigurationContext.h"

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

int main() {
  constexpr unsigned channels = 2;
  constexpr unsigned blockFrames = 4;
  FilterConfigurationContext context(channels, channels, blockFrames);
  const std::vector<FilterInfo *> noFilters;
  FilterConfiguration current(&context, noFilters, channels);
  FilterConfiguration candidate(&context, noFilters, channels);

  UpstreamFilterEngineProcess engine(channels, channels, blockFrames);
  engine.setConfigurations(&current, &candidate);
  std::array<float, blockFrames * channels> input{
      0.25f, -0.25f, 0.5f, -0.5f, 0.75f, -0.75f, 1.0f, -1.0f};
  std::array<float, blockFrames * channels> output{};

  engine.process(output.data(), input.data(), blockFrames);
  for (std::size_t i = 0; i < input.size(); ++i) {
    if (std::abs(output[i] - input[i]) > 1.0e-6f) {
      std::cerr << "upstream FilterEngine process changed pass-through sample "
                << i << '\n';
      return 1;
    }
  }
  if (!engine.takeTransitionComplete()) {
    std::cerr << "upstream transition did not publish completion handoff\n";
    return 1;
  }
  if (engine.takePreviousConfiguration() != &current) {
    std::cerr << "previous configuration ownership handoff failed\n";
    return 1;
  }
  if (engine.takePreviousConfiguration() != nullptr) {
    std::cerr << "previous configuration was handed off more than once\n";
    return 1;
  }
  std::cout << "upstream FilterEngine process slice: pass-through and transition handoff passed\n";
  return 0;
}
