#include "ConvolutionFilterFactory.h"
#include "ConvolutionFilter.h"
#include "ConvolutionMemoryEstimator.h"
#include "helpers/MemoryHelper.h"
#include "helpers/StringHelper.h"

#include <fftw3.h>
#include <sndfile.h>

#include <cmath>
#include <filesystem>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace {
uint64_t checkedAdd(uint64_t lhs, uint64_t rhs) {
  if (rhs > std::numeric_limits<uint64_t>::max() - lhs)
    throw std::runtime_error("convolution memory estimate overflow");
  return lhs + rhs;
}

uint64_t checkedMultiply(uint64_t lhs, uint64_t rhs) {
  if (rhs && lhs > std::numeric_limits<uint64_t>::max() / rhs)
    throw std::runtime_error("convolution memory estimate overflow");
  return lhs * rhs;
}

class LinuxConvolutionFilter final : public ConvolutionFilter,
                                     public ConvolutionMemoryEstimator {
public:
  explicit LinuxConvolutionFilter(std::wstring path)
      : ConvolutionFilter(path), filename_(std::move(path)) {}

  uint64_t estimatedMemoryBytes(float activeSampleRate, unsigned frameCount,
                                unsigned outputChannels) const override {
    const auto path = StringHelper::toString(filename_, 65001);
    SF_INFO info{};
    SNDFILE *file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file)
      throw std::runtime_error("cannot open convolution impulse response '" +
                               path + "': " + sf_strerror(nullptr));
    struct FileGuard {
      SNDFILE *file;
      ~FileGuard() {
        if (file)
          sf_close(file);
      }
    } guard{file};
    if (info.samplerate <= 0 || info.channels <= 0 || info.frames <= 0)
      throw std::runtime_error("convolution impulse response is empty or "
                               "invalid: " +
                               path);
    if (std::abs(activeSampleRate - info.samplerate) > 1.0f)
      throw std::runtime_error("convolution impulse response sample rate does "
                               "not match the active audio rate: " +
                               path);
    if (!frameCount || !outputChannels ||
        info.frames > std::numeric_limits<int>::max() ||
        info.channels > std::numeric_limits<int>::max() ||
        frameCount > static_cast<unsigned>(std::numeric_limits<int>::max()))
      throw std::runtime_error("convolution impulse response/block is too "
                               "large: " +
                               path);

    const uint64_t irFrames = static_cast<uint64_t>(info.frames);
    const uint64_t irChannels = static_cast<uint64_t>(info.channels);
    const uint64_t block = frameCount;
    const uint64_t segments = 1 + (irFrames - 1) / block;
    const uint64_t frequencyBins = checkedAdd(block, 1);
    const uint64_t filterAndMixBuffers =
        checkedAdd(checkedMultiply(2, segments),
                   checkedMultiply(2, checkedAdd(segments, 1)));

    // Account for the concrete arrays allocated by hcInitSingle() in the
    // pinned upstream libHybridConv implementation, plus its two FFT plans.
    uint64_t perOutputChannel = sizeof(HConvSingle);
    perOutputChannel = checkedAdd(
        perOutputChannel,
        checkedMultiply(checkedMultiply(2, block), sizeof(float))); // dft_time
    perOutputChannel = checkedAdd(
        perOutputChannel,
        checkedMultiply(frequencyBins, sizeof(fftwf_complex))); // dft_freq
    perOutputChannel = checkedAdd(
        perOutputChannel,
        checkedMultiply(checkedMultiply(2, frequencyBins), sizeof(float)));
    perOutputChannel = checkedAdd(
        perOutputChannel,
        checkedMultiply(checkedMultiply(filterAndMixBuffers, frequencyBins),
                        sizeof(float)));
    perOutputChannel =
        checkedAdd(perOutputChannel,
                   checkedMultiply(filterAndMixBuffers, sizeof(float *)));
    perOutputChannel =
        checkedAdd(perOutputChannel,
                   checkedMultiply(block, sizeof(float))); // history_time
    perOutputChannel = checkedAdd(perOutputChannel, 2 * sizeof(int));
    perOutputChannel = checkedAdd(perOutputChannel, 32 * 1024); // FFT plans

    uint64_t estimate = checkedMultiply(perOutputChannel, outputChannels);
    estimate = checkedAdd(
        estimate, checkedMultiply(checkedMultiply(irFrames, irChannels),
                                  sizeof(float))); // decoded interleaved IR
    estimate = checkedAdd(estimate,
                          checkedMultiply(irFrames, sizeof(float))); // work IR
    // Include 25% headroom for FFTW planner and allocator-private workspace.
    estimate = checkedAdd(estimate, estimate / 4);
    return checkedAdd(estimate, 256 * 1024);
  }

protected:
  void initializeFilters(unsigned frameCount) override {
    const auto path = StringHelper::toString(filename_, 65001);
    SF_INFO info{};
    SNDFILE *file = sf_open(path.c_str(), SFM_READ, &info);
    if (!file)
      throw std::runtime_error("cannot open convolution impulse response '" +
                               path + "': " + sf_strerror(nullptr));
    struct FileGuard {
      SNDFILE *file;
      ~FileGuard() { if (file) sf_close(file); }
    } guard{file};
    if (info.samplerate <= 0 || info.channels <= 0 || info.frames <= 0)
      throw std::runtime_error("convolution impulse response is empty or invalid: " + path);
    if (std::abs(sampleRate - info.samplerate) > 1.0f)
      throw std::runtime_error("convolution impulse response sample rate does "
                               "not match the active audio rate: " + path);
    if (info.frames > std::numeric_limits<int>::max() ||
        info.channels > std::numeric_limits<int>::max() ||
        frameCount > static_cast<unsigned>(std::numeric_limits<int>::max()))
      throw std::runtime_error("convolution impulse response/block is too large: " + path);
    const auto total = static_cast<size_t>(info.frames) * info.channels;
    if (total / static_cast<size_t>(info.channels) !=
        static_cast<size_t>(info.frames))
      throw std::runtime_error("convolution impulse response size overflow: " + path);
    std::vector<float> interleaved(total);
    const auto read = sf_readf_float(file, interleaved.data(), info.frames);
    if (read != info.frames)
      throw std::runtime_error("short read from convolution impulse response: " + path);
    sf_close(file);
    guard.file = nullptr;

    std::vector<float> impulse(static_cast<size_t>(info.frames));
    auto *newFilters = static_cast<HConvSingle *>(
        MemoryHelper::alloc(sizeof(HConvSingle) * channelCount));
    filters = newFilters;
    for (unsigned channel = 0; channel < channelCount; ++channel) {
      const unsigned sourceChannel = channel % static_cast<unsigned>(info.channels);
      for (sf_count_t frame = 0; frame < info.frames; ++frame)
        impulse[static_cast<size_t>(frame)] =
            interleaved[static_cast<size_t>(frame) * info.channels + sourceChannel];
      hcInitSingle(&filters[channel], impulse.data(),
                   static_cast<int>(info.frames), static_cast<int>(frameCount), 1);
    }
  }

private:
  // The upstream base keeps the filename private. Retain the Linux path here.
  std::wstring filename_;
public:
  void setPath(const std::wstring &path) { filename_ = path; }
private:
  std::string filename() const { return StringHelper::toString(filename_, 65001); }
};
} // namespace

std::vector<IFilter *> ConvolutionFilterFactory::createFilter(
    const std::wstring &configPath, std::wstring &command,
    std::wstring &parameters) {
  if (command != L"Convolution")
    return {};
  auto value = StringHelper::trim(parameters);
  if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
    value = value.substr(1, value.size() - 2);
  if (value.empty())
    return {};
  std::filesystem::path ir(StringHelper::toString(value, 65001));
  if (ir.is_relative()) {
    const auto config = std::filesystem::path(StringHelper::toString(configPath, 65001));
    ir = config.parent_path() / ir;
  }
  auto normalized = StringHelper::toWString(std::filesystem::absolute(ir).lexically_normal().string(), 65001);
  void *memory = MemoryHelper::alloc(sizeof(LinuxConvolutionFilter));
  LinuxConvolutionFilter *filter = nullptr;
  try {
    filter = new (memory) LinuxConvolutionFilter(normalized);
  } catch (...) {
    MemoryHelper::free(memory);
    throw;
  }
  return {filter};
}
