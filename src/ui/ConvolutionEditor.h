#pragma once

#include "Editor/IFilterGUI.h"

class QLabel;
class QLineEdit;

// Linux editor for the upstream Convolution directive. The upstream GUI is
// tied to Windows service permissions and sf_wchar_open, so this adapter
// preserves the directive and leaves audio-format/rate validation to the
// actual config checker/engine.
class ConvolutionEditor final : public IFilterGUI {
  Q_OBJECT
public:
  ConvolutionEditor(QString parameters, QString configFile,
                    QWidget *parent = nullptr);
  void store(QString &command, QString &parameters) override;

private:
  void updateFileStatus();
  QString configPath;
  QLineEdit *pathEdit{};
  QLabel *statusLabel{};
};
