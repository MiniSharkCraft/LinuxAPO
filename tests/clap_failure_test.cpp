#include "CLAPPluginHost.h"
#include "Engine.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>

static bool engineReportsFailure() {
  char path[] = "/tmp/skyapo-clap-failure-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0)
    return false;
  const std::string config = "Plugin: CLAP org.skyapo.test.error-once\n";
  const auto written = write(fd, config.data(), config.size());
  close(fd);
  if (written != static_cast<ssize_t>(config.size())) {
    unlink(path);
    return false;
  }

  Engine engine(48000, 2, 64, {L"L", L"R"});
  engine.loadConfig(path);
  unlink(path);
  std::array<float, 128> block{};
  block.fill(0.25f);
  engine.process(block.data(), 64);
  const auto failures = engine.failedPluginDescriptions();
  return failures.size() == 1 &&
         failures.front().find("org.skyapo.test.error-once") !=
             std::string::npos &&
         failures.front().find(":1") != std::string::npos;
}

int main() {
  CLAPPluginHost host;
  auto instance = host.create("org.skyapo.test.error-once", 48000, 64,
                              {L"L", L"R"});
  std::array<float, 8> inputLeft{1, 2, 3, 4, 5, 6, 7, 8};
  std::array<float, 8> inputRight{1, 2, 3, 4, 5, 6, 7, 8};
  std::array<float, 8> outputLeft{};
  std::array<float, 8> outputRight{};
  float *input[]{inputLeft.data(), inputRight.data()};
  float *output[]{outputLeft.data(), outputRight.data()};

  outputLeft.fill(9.0f);
  outputRight.fill(9.0f);
  instance->process(output, input, 8);
  if (!instance->processingFailed()) {
    std::cerr << "CLAP process error was not latched\n";
    return 1;
  }
  for (const auto *channel : {&outputLeft, &outputRight})
    for (float sample : *channel)
      if (sample != 0.0f) {
        std::cerr << "CLAP process error did not silence current block\n";
        return 1;
      }

  outputLeft.fill(9.0f);
  outputRight.fill(9.0f);
  instance->process(output, input, 8);
  for (const auto *channel : {&outputLeft, &outputRight})
    for (float sample : *channel)
      if (sample != 0.0f) {
        std::cerr << "latched CLAP error did not silence subsequent block\n";
        return 1;
      }
  if (!engineReportsFailure()) {
    std::cerr << "Engine did not report the failed CLAP plugin/config line\n";
    return 1;
  }
  std::cout << "CLAP error latched; current and subsequent blocks are silent\n";
  return 0;
}
