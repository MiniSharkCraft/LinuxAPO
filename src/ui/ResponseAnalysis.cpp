#include "ResponseAnalysis.h"

#include "Engine.h"
#include "../platform/linux/ConfigSource.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <fftw3.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <vector>

namespace {
constexpr unsigned MinAnalysisFrames = 128;
constexpr unsigned MaxAnalysisFrames = 1U << 20;
constexpr unsigned MaxAnalysisChannels = 8;
constexpr size_t MaxAnalysisWorkspaceBytes = 64U * 1024U * 1024U;
constexpr float ImpulseOnsetThreshold = 1.0e-5F;
constexpr qint64 ConfigHashChunkBytes = 64 * 1024;

std::optional<QByteArray>
configFileSha256(const std::string &path,
                 const std::function<bool()> &cancellationRequested = {}) {
  const QString filePath =
      QString::fromLocal8Bit(path.data(), static_cast<qsizetype>(path.size()));
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly))
    return std::nullopt;

  QCryptographicHash hash(QCryptographicHash::Sha256);
  while (!file.atEnd()) {
    if (cancellationRequested && cancellationRequested())
      return std::nullopt;
    const QByteArray chunk = file.read(ConfigHashChunkBytes);
    if (chunk.isEmpty() && file.error() != QFileDevice::NoError)
      return std::nullopt;
    hash.addData(chunk);
  }
  if (file.error() != QFileDevice::NoError)
    return std::nullopt;
  return hash.result();
}

QVector<ResponseConfigDependency> captureConfigDependencies(
    const std::vector<std::filesystem::path> &paths, QString &error,
    const std::function<bool()> &cancellationRequested = {}) {
  QVector<ResponseConfigDependency> dependencies;
  dependencies.reserve(static_cast<qsizetype>(paths.size()));
  for (const auto &path : paths) {
    if (cancellationRequested && cancellationRequested())
      return {};
    const std::string nativePath = path.string();
    const auto digest = configFileSha256(nativePath, cancellationRequested);
    if (!digest) {
      if (cancellationRequested && cancellationRequested())
        return {};
      const QString displayPath = QString::fromLocal8Bit(
          nativePath.data(), static_cast<qsizetype>(nativePath.size()));
      error = QStringLiteral("Could not read configuration dependency '%1'.")
                  .arg(displayPath);
      return {};
    }
    dependencies.append({nativePath, *digest});
  }
  return dependencies;
}

bool sameConfigDependencies(const QVector<ResponseConfigDependency> &first,
                            const QVector<ResponseConfigDependency> &second) {
  if (first.size() != second.size())
    return false;
  for (qsizetype index = 0; index < first.size(); ++index)
    if (first[index].path != second[index].path ||
        first[index].sha256 != second[index].sha256)
      return false;
  return true;
}

QString changedConfigError() {
  return QStringLiteral(
      "A configuration or Include file changed during response analysis. "
      "Run the analysis again for the latest configuration.");
}

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

bool responseConfigDependenciesUnchanged(
    const QVector<ResponseConfigDependency> &dependencies,
    std::function<bool()> cancellationRequested) {
  if (dependencies.isEmpty())
    return false;
  QVector<ResponseConfigDependency> current;
  current.reserve(dependencies.size());
  for (const auto &dependency : dependencies) {
    if (cancellationRequested && cancellationRequested())
      return false;
    const auto digest =
        configFileSha256(dependency.path, cancellationRequested);
    if (!digest)
      return false;
    current.append({dependency.path, *digest});
  }
  return sameConfigDependencies(dependencies, current);
}

ResponseAnalysisResult
analyzeConfigResponse(const QString &configPath, unsigned sampleRate,
                      unsigned channels, unsigned fftFrames,
                      const QString &sourceConfigPath,
                      const QByteArray &sourceConfigSha256,
                      std::function<bool()> cancellationRequested,
                      ResponseAnalysisDeviceContext deviceContext) {
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
  const auto cancelIfRequested = [&] {
    if (!cancellationRequested || !cancellationRequested())
      return false;
    result.cancelled = true;
    return true;
  };
  if (cancelIfRequested())
    return result;

  try {
    QString dependencyError;
    QVector<ResponseConfigDependency> engineDependencies;
    {
      // Capture the loaded config/Include set before response engines parse it.
      Engine probe(sampleRate, channels, 1024, {}, false, false,
                   deviceContext.matchText, deviceContext.deviceName,
                   deviceContext.connectionName);
      probe.loadConfig(configPath.toStdString());
      if (!probe.configNotices().empty()) {
        QStringList notices;
        for (const auto &notice : probe.configNotices()) {
          const std::string nativePath = notice.file.string();
          const QString path = QString::fromLocal8Bit(
              nativePath.data(), static_cast<qsizetype>(nativePath.size()));
          notices.append(
              QStringLiteral("%1:%2: %3")
                  .arg(path)
                  .arg(notice.line)
                  .arg(QString::fromLocal8Bit(
                      notice.message.data(),
                      static_cast<qsizetype>(notice.message.size()))));
        }
        result.error =
            QStringLiteral("Response is incomplete because the Engine "
                           "cannot apply these configuration stages:\n%1")
                .arg(notices.join(QLatin1Char('\n')));
        return result;
      }
      engineDependencies = captureConfigDependencies(
          probe.configFiles(), dependencyError, cancellationRequested);
    }
    if (!dependencyError.isEmpty()) {
      if (cancelIfRequested())
        return result;
      result.error = dependencyError;
      return result;
    }
    if (cancelIfRequested())
      return result;
    if (engineDependencies.isEmpty()) {
      result.error =
          QStringLiteral("No configuration dependencies were loaded.");
      return result;
    }
    result.configDependencies = engineDependencies;

    if (!sourceConfigPath.isEmpty()) {
      if (sourceConfigSha256.size() !=
          QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        result.error =
            QStringLiteral("The source configuration fingerprint is invalid.");
        return result;
      }
      const std::string snapshotPath =
          skyapo::platform::ConfigSource::canonicalize(
              std::filesystem::path(configPath.toStdString()))
              .string();
      const auto snapshotRoot = std::find_if(
          engineDependencies.cbegin(), engineDependencies.cend(),
          [&snapshotPath](const ResponseConfigDependency &dependency) {
            return dependency.path == snapshotPath;
          });
      if (snapshotRoot == engineDependencies.cend() ||
          snapshotRoot->sha256 != sourceConfigSha256) {
        result.error = changedConfigError();
        return result;
      }
      result.configDependencies.append(
          {sourceConfigPath.toLocal8Bit().toStdString(), sourceConfigSha256});
    }

    double totalInitializationMs = 0.0;
    double totalEngineProcessingMs = 0.0;
    unsigned initializedEngines = 0;
    for (unsigned inputChannel = 0; inputChannel < channels; ++inputChannel) {
      if (cancelIfRequested())
        return result;
      // Never load third-party module code in the editor process. Plugin
      // directives are reported as unsupported for this diagnostic analysis.
      const auto initializationStarted = std::chrono::steady_clock::now();
      Engine engine(sampleRate, channels, 1024, {}, false, false,
                    deviceContext.matchText, deviceContext.deviceName,
                    deviceContext.connectionName);
      engine.loadConfig(configPath.toStdString());
      totalInitializationMs +=
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - initializationStarted)
              .count();
      ++initializedEngines;
      QString currentDependencyError;
      const auto currentDependencies = captureConfigDependencies(
          engine.configFiles(), currentDependencyError, cancellationRequested);
      if (!currentDependencyError.isEmpty()) {
        if (cancelIfRequested())
          return result;
        result.error = currentDependencyError;
        return result;
      }
      if (!sameConfigDependencies(engineDependencies, currentDependencies)) {
        result.error = changedConfigError();
        return result;
      }
      std::vector<float> block(1024U * channels, 0.0F);
      std::vector<float> impulseResponse(fftFrames * channels, 0.0F);
      double engineProcessingMs = 0.0;
      for (unsigned blockStart = 0; blockStart < fftFrames;) {
        if (cancelIfRequested())
          return result;
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
        if (cancelIfRequested())
          return result;
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
    if (cancelIfRequested())
      return result;
    if (!responseConfigDependenciesUnchanged(result.configDependencies,
                                             cancellationRequested)) {
      if (cancelIfRequested())
        return result;
      result.error = changedConfigError();
      return result;
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
