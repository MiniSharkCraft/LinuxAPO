#include "VST2PluginHost.h"

#include <array>
#include <cmath>
#include <dlfcn.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "usage: vst2-host-prototype-test <fixture.so>\n";
    return 2;
  }
  try {
    VST2PluginHost host;
    const std::vector<std::wstring> channels{L"L", L"R"};
    auto instance = host.create(argv[1], 48000.0f, 16, channels);
    void *fixture = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    require(fixture != nullptr, "fixture telemetry module load");
    using Query = int (*)();
    auto sampleRate = reinterpret_cast<Query>(
        dlsym(fixture, "vst2FixtureReportedSampleRate"));
    auto blockSize = reinterpret_cast<Query>(
        dlsym(fixture, "vst2FixtureReportedBlockSize"));
    require(sampleRate && blockSize, "fixture telemetry symbols");
    require(sampleRate() == 48000, "entry callback sample rate");
    require(blockSize() == 16, "entry callback block size");
    dlclose(fixture);
    require(instance->parameters().size() == 1, "parameter count");
    require(instance->parameters()[0].name == "Gain", "parameter name");
    instance->initialize(48000.0f, 16, channels);

    constexpr std::array<unsigned, 5> blocks{1, 3, 8, 2, 16};
    std::array<float, 32> left{}, right{}, outLeft{}, outRight{};
    left.fill(0.4f);
    right.fill(-0.2f);
    float *input[]{left.data(), right.data()};
    float *output[]{outLeft.data(), outRight.data()};
    unsigned checked = 0;
    for (const auto frames : blocks) {
      instance->process(output, input, frames);
      for (unsigned i = 0; i < frames; ++i) {
        require(std::abs(outLeft[i] - 0.2f) < 1.0e-6f, "left gain ratio");
        require(std::abs(outRight[i] + 0.1f) < 1.0e-6f, "right gain ratio");
      }
      checked += frames;
    }
    require(!instance->processingFailed(), "unexpected process failure");

    auto overridden = host.create(argv[1], 48000.0f, 16, channels,
                                  {{"0", 0.25f}});
    overridden->initialize(48000.0f, 16, channels);
    overridden->process(output, input, 7);
    for (unsigned i = 0; i < 7; ++i) {
      require(std::abs(outLeft[i] - 0.1f) < 1.0e-6f,
              "parameter override left ratio");
      require(std::abs(outRight[i] + 0.05f) < 1.0e-6f,
              "parameter override right ratio");
    }

    outLeft.fill(42.0f);
    outRight.fill(42.0f);
    instance->process(output, input, 17);
    require(instance->processingFailed(), "oversize block was not latched");
    for (unsigned i = 0; i < 16; ++i)
      require(outLeft[i] == 0.0f && outRight[i] == 0.0f,
              "oversize block was not silenced");
    require(outLeft[16] == 42.0f && outRight[16] == 42.0f,
            "oversize block wrote beyond the negotiated output capacity");
    std::array<float, 16> exactCapacityLeft{}, exactCapacityRight{};
    float *exactCapacityOutput[]{exactCapacityLeft.data(),
                                 exactCapacityRight.data()};
    instance->process(exactCapacityOutput, input, 17);
    instance->process(output, input, 3);
    for (unsigned i = 0; i < 3; ++i)
      require(outLeft[i] == 0.0f && outRight[i] == 0.0f,
              "failed plugin did not remain fail-closed");
    std::cout << "PASS: FST-only VST2-compatible fixture, " << checked
              << " varying samples/channel at ratio 0.5, parameter override "
                 "ratio 0.25, entry callback negotiated 48000/16 before user "
                 "assignment, oversize fail-closed\n";
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
