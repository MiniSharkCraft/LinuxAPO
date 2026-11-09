#pragma once

#include <QByteArray>
#include <QString>
#include <QPointF>
#include <QVector>
#include <optional>
#include <string>

struct ResponseCurve {
  QString channel;
  QVector<QPointF> points;
};

struct ResponseConfigDependency {
  std::string path;
  QByteArray sha256;
};

struct ResponseAnalysisResult {
  QVector<ResponseCurve> curves;
  QVector<ResponseConfigDependency> configDependencies;
  std::optional<unsigned> impulseOnsetSamples{std::nullopt};
  std::optional<double> engineInitializationMs{std::nullopt};
  std::optional<double> engineProcessingPercent{std::nullopt};
  QString error;
};

// Runs the actual SkyAPO/EAPO Engine over per-channel impulses and measures
// impulse onset, Engine initialization, and Engine::process cost. Intended for
// an analysis worker, never for the realtime PipeWire callback.
// This helper reads the tracked files and must also stay off the audio thread.
bool responseConfigDependenciesUnchanged(
    const QVector<ResponseConfigDependency> &dependencies);
ResponseAnalysisResult analyzeConfigResponse(const QString &configPath,
                                             unsigned sampleRate = 48000,
                                             unsigned channels = 2,
                                             unsigned fftFrames = 16384);
