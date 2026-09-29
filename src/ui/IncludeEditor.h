#pragma once

#include "Editor/IFilterGUI.h"

class QLabel;
class QLineEdit;

class IncludeEditor final : public IFilterGUI {
  Q_OBJECT
public:
  IncludeEditor(QString parameters, QString configFile,
                QWidget *parent = nullptr);
  void store(QString &command, QString &parameters) override;

private:
  void updateFileStatus();
  QString configPath;
  QLineEdit *pathEdit{};
  QLabel *statusLabel{};
};
