#pragma once

#include "Editor/IFilterGUI.h"

class QLineEdit;

class DeviceFilterEditor final : public IFilterGUI {
  Q_OBJECT
public:
  explicit DeviceFilterEditor(QString parameters, QWidget *parent = nullptr);
  void store(QString &command, QString &parameters) override;

private:
  QLineEdit *patternEdit{};
};
