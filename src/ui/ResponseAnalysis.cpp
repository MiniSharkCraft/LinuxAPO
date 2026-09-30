#include "ResponseAnalysis.h"

#include "Engine.h"
#include <fftw3.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <exception>
#include <vector>

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
  if (sampleRate == 0 || channels == 0 || fftFrames < 1024 ||
      fftFrames % 1024 != 0) {
    result.error = QStringLiteral("Analysis requires a nonzero format and an "
                                  "FFT size divisible by 1024.");
    return result;
  }

  try {
    for (unsigned inputChannel = 0; inputChannel < channels; ++inputChannel) {
      // Never load third-party module code in the editor process. Plugin
      // directives are reported as unsupported for this diagnostic analysis.
      Engine engine(sampleRate, channels, 1024, {}, false, false);
      engine.loadConfig(configPath.toStdString());
      std::vector<float> block(1024U * channels, 0.0F);
      std::vector<float> impulseResponse(fftFrames * channels, 0.0F);
      for (unsigned blockStart = 0; blockStart < fftFrames;
           blockStart += 1024) {
        std::fill(block.begin(), block.end(), 0.0F);
        if (blockStart == 0)
          block[inputChannel] = 1.0F;
        engine.process(block.data(), 1024);
        for (unsigned frame = 0; frame < 1024; ++frame)
          for (unsigned outputChannel = 0; outputChannel < channels;
               ++outputChannel)
            impulseResponse[(blockStart + frame) * channels + outputChannel] =
                block[frame * channels + outputChannel];
      }

      for (unsigned outputChannel = 0; outputChannel < channels;
           ++outputChannel) {
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
    }
  } catch (const std::exception &error) {
    result.error = QString::fromLocal8Bit(error.what());
  }
  return result;
}
