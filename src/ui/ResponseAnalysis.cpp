#include "ResponseAnalysis.h"

#include "Engine.h"
#include <fftw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <exception>
#include <limits>
#include <vector>

namespace {
constexpr unsigned MinAnalysisFrames = 128;
constexpr unsigned MaxAnalysisFrames = 1U << 20;
constexpr unsigned MaxAnalysisChannels = 8;
constexpr size_t MaxAnalysisWorkspaceBytes = 64U * 1024U * 1024U;
constexpr float ImpulseOnsetThreshold = 1.0e-5F;

bool exceedsWorkspaceBudget(unsigned frames, unsigned channels) {
  const auto multiply = [](size_t a, size_t b, size_t &out) {
    if (b && a > std::numeric_limits<size_t>::max() / b)
      return false;
    out = a * b;
    return true;
  };
  const auto add = [](size_t a, size_t b, size_t &out) {
    if (b > std::numeric_limits<size_t>::max() - a)
      return false;
    out = a + b;
    return true;
  };
  size_t impulseSamples{}, impulseBytes{}, channelPairs{}, curvePoints{},
      curveBytes{}, fftFloats{}, fftBytes{}, total{};
  if (!multiply(frames, channels, impulseSamples) ||
      !multiply(impulseSamples, sizeof(float), impulseBytes) ||
      !multiply(channels, channels, channelPairs) ||
      !multiply(channelPairs, frames / 2, curvePoints) ||
      !multiply(curvePoints, sizeof(QPointF), curveBytes) ||
      !multiply(frames + 2 * (frames / 2 + 1), 1, fftFloats) ||
      !multiply(fftFloats, sizeof(float), fftBytes) ||
      !add(impulseBytes, curveBytes, total) || !add(total, fftBytes, total))
    return true;
  return total > MaxAnalysisWorkspaceBytes;
}
} // namespace

namespace {
QString channelName(unsigned channel) {
  static const char *names[] = {"L", "R", "C", "LFE", "RL", "RR", "SL", "SR"};
  return channel < sizeof(names) / sizeof(names[0])
             ? QString::fromLatin1(names[channel])
             : QStringLiteral("%1").arg(channel + 1);
}
} // namespace

ResponseAnalysisResult analyzeConfigResponse(const QString &configPath,
                                             unsigned sampleRate,
                                             unsigned channels,
                                             unsigned fftFrames) {
  ResponseAnalysisResult result;
  if (sampleRate == 0 || channels == 0 || channels > MaxAnalysisChannels ||
      fftFrames < MinAnalysisFrames || fftFrames > MaxAnalysisFrames ||
      (fftFrames & (fftFrames - 1)) != 0) {
    result.error = QStringLiteral(
        "Analysis requires 1–8 channels and a power-of-two FFT size from "
        "128 to 1048576 samples.");
    return result;
  }
  if (exceedsWorkspaceBudget(fftFrames, channels)) {
    result.error = QStringLiteral(
        "Estimated sample, FFT, and curve buffers exceed the 64 MiB "
        "analysis budget.");
    return result;
  }

  try {
    double totalInitializationMs = 0.0;
    double totalEngineProcessingMs = 0.0;
    unsigned initializedEngines = 0;
    for (unsigned inputChannel = 0; inputChannel < channels; ++inputChannel) {
      // Never load third-party module code in the editor process. Plugin
      // directives are reported as unsupported for this diagnostic analysis.
      const auto initializationStarted = std::chrono::steady_clock::now();
      Engine engine(sampleRate, channels, 1024, {}, false, false);
      engine.loadConfig(configPath.toStdString());
      totalInitializationMs +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - initializationStarted)
              .count();
      ++initializedEngines;
      std::vector<float> block(1024U * channels, 0.0F);
      std::vector<float> impulseResponse(fftFrames * channels, 0.0F);
      double engineProcessingMs = 0.0;
      for (unsigned blockStart = 0; blockStart < fftFrames;) {
        const unsigned frameCount = std::min(1024U, fftFrames - blockStart);
        std::fill(block.begin(), block.end(), 0.0F);
        if (blockStart == 0)
          block[inputChannel] = 1.0F;
        const auto processStarted = std::chrono::steady_clock::now();
        engine.process(block.data(), frameCount);
        engineProcessingMs +=
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - processStarted)
                .count();
        for (unsigned frame = 0; frame < frameCount; ++frame)
          for (unsigned outputChannel = 0; outputChannel < channels;
               ++outputChannel)
            impulseResponse[(blockStart + frame) * channels + outputChannel] =
                block[frame * channels + outputChannel];
        blockStart += frameCount;
      }

      for (unsigned outputChannel = 0; outputChannel < channels;
           ++outputChannel) {
        float pathPeak = 0.0F;
        for (unsigned frame = 0; frame < fftFrames; ++frame) {
          const float sample =
              impulseResponse[frame * channels + outputChannel];
          if (std::isfinite(sample))
            pathPeak = std::max(pathPeak, std::abs(sample));
        }
        if (pathPeak > 0.0F) {
          for (unsigned frame = 0; frame < fftFrames; ++frame) {
            const float sample =
                impulseResponse[frame * channels + outputChannel];
            if (std::isfinite(sample) &&
                std::abs(sample) >= ImpulseOnsetThreshold) {
              if (!result.impulseOnsetSamples ||
                  frame > *result.impulseOnsetSamples)
                result.impulseOnsetSamples = frame;
              break;
            }
          }
        }
        auto *input =
            static_cast<float *>(fftwf_malloc(sizeof(float) * fftFrames));
        auto *output = static_cast<fftwf_complex *>(
            fftwf_malloc(sizeof(fftwf_complex) * (fftFrames / 2 + 1)));
        if (!input || !output) {
          fftwf_free(input);
          fftwf_free(output);
          result.error = QStringLiteral("Could not allocate FFT workspace.");
          return result;
        }
        for (unsigned frame = 0; frame < fftFrames; ++frame)
          input[frame] = impulseResponse[frame * channels + outputChannel];
        auto plan = fftwf_plan_dft_r2c_1d(static_cast<int>(fftFrames), input,
                                          output, FFTW_ESTIMATE);
        if (!plan) {
          fftwf_free(input);
          fftwf_free(output);
          result.error = QStringLiteral("Could not create FFT plan.");
          return result;
        }
        fftwf_execute(plan);
        fftwf_destroy_plan(plan);

        ResponseCurve curve;
        curve.channel = QStringLiteral("%1 → %2").arg(
            channelName(inputChannel), channelName(outputChannel));
        curve.points.reserve(static_cast<qsizetype>(fftFrames / 2));
        for (unsigned bin = 1; bin <= fftFrames / 2; ++bin) {
          const double frequency =
              static_cast<double>(bin) * sampleRate / fftFrames;
          const double magnitude = std::hypot(output[bin][0], output[bin][1]);
          const double db = 20.0 * std::log10(std::max(magnitude, 1.0e-12));
          curve.points.append(QPointF(frequency, db));
        }
        fftwf_free(input);
        fftwf_free(output);
        result.curves.append(std::move(curve));
      }
      totalEngineProcessingMs += engineProcessingMs;
    }
    if (initializedEngines > 0) {
      result.engineInitializationMs =
          totalInitializationMs / initializedEngines;
      const double realtimeDurationMs =
          static_cast<double>(fftFrames) * 1000.0 / sampleRate;
      if (realtimeDurationMs > 0.0)
        result.engineProcessingPercent =
            (totalEngineProcessingMs / initializedEngines) * 100.0 /
            realtimeDurationMs;
    }
  } catch (const std::exception &error) {
    result.error = QString::fromLocal8Bit(error.what());
  }
  return result;
}
