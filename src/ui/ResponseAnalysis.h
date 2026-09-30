#pragma once

#include <QString>
#include <QPointF>
#include <QVector>

struct ResponseCurve {
  QString channel;
  QVector<QPointF> points;
};

struct ResponseAnalysisResult {
  QVector<ResponseCurve> curves;
  QString error;
};

// Runs the actual SkyAPO/EAPO Engine over per-channel impulses. Intended for
// an analysis worker, never for the realtime PipeWire callback.
ResponseAnalysisResult analyzeConfigResponse(const QString &configPath,
                                             unsigned sampleRate = 48000,
                                             unsigned channels = 2,
                                             unsigned fftFrames = 16384);
