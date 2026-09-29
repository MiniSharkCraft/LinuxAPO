#pragma once

#include "ConfigFile.h"
#include <QMainWindow>
#include <QString>
#include <functional>
#include <vector>

class QComboBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QString path = {});

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  void openConfig(const QString &path);
  void rebuildRows();
  void addFilter(const QString &line);
  void markModified();
  bool saveConfig();
  void checkConfig();
  void reloadConfig();
  void refreshStatus();
  void refreshDevices();
  void selectDevice(int index);
  using CliCompletion = std::function<void(int, QByteArray, QByteArray)>;
  void runCli(const QStringList &arguments, CliCompletion completion);
  QString defaultConfigPath() const;
  void updateTitle();

  QString configPath;
  ConfigFile document;
  bool modified{};
  QLineEdit *pathEdit{};
  QLabel *statusLabel{};
  QComboBox *deviceCombo{};
  QVBoxLayout *rowsLayout{};
  bool statusRequestPending{};
  bool deviceRequestPending{};
};
