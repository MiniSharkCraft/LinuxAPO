#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {
struct Options {
  std::filesystem::path config;
  unsigned rate = 48000;
  unsigned channels = 2;
  unsigned blockFrames = 256;
  unsigned warmupBlocks = 500;
  unsigned measuredBlocks = 5000;
};

unsigned parseUnsigned(const char *name, const std::string &value) {
  size_t parsed = 0;
  unsigned long number = 0;
  try {
    number = std::stoul(value, &parsed);
  } catch (...) {
    throw std::runtime_error(std::string(name) +
                             " must be an unsigned integer");
  }
  if (parsed != value.size() || number > UINT32_MAX)
    throw std::runtime_error(std::string(name) +
                             " is outside the supported range");
  return static_cast<unsigned>(number);
}

void printUsage(std::ostream &out) {
  out << "Usage: skyapo-bench --config FILE [options]\n"
      << "  --rate HZ             sample rate (default 48000)\n"
      << "  --channels COUNT      channel count (default 2)\n"
      << "  --block FRAMES        block size (default 256)\n"
      << "  --warmup BLOCKS       unmeasured blocks (default 500)\n"
      << "  --iterations BLOCKS   measured blocks (default 5000)\n";
}

Options parseOptions(int argc, char **argv) {
  Options options;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      printUsage(std::cout);
      std::exit(0);
    }
    if (i + 1 >= argc)
      throw std::runtime_error("missing value for " + argument);
    const std::string value = argv[++i];
    if (argument == "--config")
      options.config = value;
    else if (argument == "--rate")
      options.rate = parseUnsigned("--rate", value);
    else if (argument == "--channels")
      options.channels = parseUnsigned("--channels", value);
    else if (argument == "--block")
      options.blockFrames = parseUnsigned("--block", value);
    else if (argument == "--warmup")
      options.warmupBlocks = parseUnsigned("--warmup", value);
    else if (argument == "--iterations")
      options.measuredBlocks = parseUnsigned("--iterations", value);
    else
      throw std::runtime_error("unknown option: " + argument);
  }

  if (options.config.empty())
    throw std::runtime_error("--config is required");
  if (options.rate < 8000 || options.rate > 384000)
    throw std::runtime_error("--rate must be between 8000 and 384000 Hz");
  if (!options.channels || options.channels > 8)
    throw std::runtime_error("--channels must be between 1 and 8");
  if (!options.blockFrames || options.blockFrames > 8192)
    throw std::runtime_error("--block must be between 1 and 8192 frames");
  if (options.warmupBlocks > 1000000 || !options.measuredBlocks ||
      options.measuredBlocks > 1000000)
    throw std::runtime_error(
        "warmup must be 0..1000000; iterations must be 1..1000000");
  if (options.blockFrames >
      std::numeric_limits<size_t>::max() / options.channels)
    throw std::runtime_error("audio block size overflows addressable memory");
  return options;
}

double percentile(const std::vector<double> &sorted, double quantile) {
  const auto index = static_cast<size_t>(
      std::ceil(quantile * static_cast<double>(sorted.size())) - 1.0);
  return sorted[std::min(index, sorted.size() - 1)];
}
} // namespace

int main(int argc, char **argv) {
  try {
    const Options options = parseOptions(argc, argv);
    Engine engine(options.rate, options.channels, options.blockFrames);
    engine.loadConfig(options.config.string());

    const size_t sampleCount =
        static_cast<size_t>(options.blockFrames) * options.channels;
    std::vector<float> input(sampleCount);
    std::vector<float> output(sampleCount);
    for (unsigned frame = 0; frame < options.blockFrames; ++frame) {
      const float sample =
          static_cast<float>(0.2 * std::sin(2.0 * 3.14159265358979323846 *
                                            997.0 * frame / options.rate));
      for (unsigned channel = 0; channel < options.channels; ++channel)
        input[static_cast<size_t>(frame) * options.channels + channel] =
            sample * (1.0f - 0.05f * channel);
    }

    for (unsigned block = 0; block < options.warmupBlocks; ++block) {
      std::copy(input.begin(), input.end(), output.begin());
      engine.process(output.data(), options.blockFrames);
    }

    using Clock = std::chrono::steady_clock;
    std::vector<double> durationsUs;
    durationsUs.reserve(options.measuredBlocks);
    long double totalUs = 0.0;
    for (unsigned block = 0; block < options.measuredBlocks; ++block) {
      std::copy(input.begin(), input.end(), output.begin());
      const auto start = Clock::now();
      engine.process(output.data(), options.blockFrames);
      const auto end = Clock::now();
      const double elapsed =
          std::chrono::duration<double, std::micro>(end - start).count();
      durationsUs.push_back(elapsed);
      totalUs += elapsed;
    }
    std::sort(durationsUs.begin(), durationsUs.end());

    const double meanUs = static_cast<double>(totalUs / durationsUs.size());
    const double medianUs = percentile(durationsUs, 0.50);
    const double p95Us = percentile(durationsUs, 0.95);
    const double maxUs = durationsUs.back();
    const double blockBudgetUs =
        1.0e6 * options.blockFrames / static_cast<double>(options.rate);
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
      throw std::runtime_error("cannot read process peak memory usage");

    std::cout << std::fixed << std::setprecision(3)
              << "{\"schema\":\"skyapo-bench-v1\""
              << ",\"sample_rate_hz\":" << options.rate
              << ",\"channels\":" << options.channels
              << ",\"block_frames\":" << options.blockFrames
              << ",\"filter_count\":" << engine.filterCount()
              << ",\"warmup_blocks\":" << options.warmupBlocks
              << ",\"measured_blocks\":" << options.measuredBlocks
              << ",\"mean_us\":" << meanUs << ",\"median_us\":" << medianUs
              << ",\"p95_us\":" << p95Us << ",\"max_us\":" << maxUs
              << ",\"block_budget_us\":" << blockBudgetUs
              << ",\"max_rss_kib\":" << usage.ru_maxrss
              << ",\"p95_budget_percent\":" << 100.0 * p95Us / blockBudgetUs
              << "}\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "skyapo-bench: " << error.what() << '\n';
    printUsage(std::cerr);
    return 2;
  }
}
