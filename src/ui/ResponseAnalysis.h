#pragma once

#include <QString>
#include <QPointF>
#include <QVector>
#include <optional>

struct ResponseCurve {
  QString channel;
  QVector<QPointF> points;
};

struct ResponseAnalysisResult {
  QVector<ResponseCurve> curves;
  std::optional<unsigned> impulseOnsetSamples{std::nullopt};
  std::optional<double> engineInitializationMs{std::nullopt};
  std::optional<double> engineProcessingPercent{std::nullopt};
  QString error;
};

// Runs the actual SkyAPO/EAPO Engine over per-channel impulses and measures
// impulse onset, Engine initialization, and Engine::process cost. Intended for
// an analysis worker, never for the realtime PipeWire callback.
ResponseAnalysisResult analyzeConfigResponse(const QString &configPath,
                                             unsigned sampleRate = 48000,
                                             unsigned channels = 2,
                                             unsigned fftFrames = 16384);
