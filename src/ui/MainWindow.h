#pragma once

#include "ConfigFile.h"
#include "Editor/FilterTable.h"
#include <QMainWindow>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

class QComboBox;
class QCloseEvent;
class QLabel;
class QLineEdit;
class QMenu;
class QScrollArea;

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QString path = {}, QString cliExecutable = {});

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
  void populateAddPopupMenu(QMenu *menu);
  void syncRowsToDocument();
  using CliCompletion = std::function<void(int, QByteArray, QByteArray)>;
  void runCli(const QStringList &arguments, CliCompletion completion);
  QString defaultConfigPath() const;
  void updateTitle();

  QString configPath;
  QString cliExecutable;
  ConfigFile document;
  bool modified{};
  QLineEdit *pathEdit{};
  QLabel *statusLabel{};
  QComboBox *deviceCombo{};
  FilterTable *rowTable{};
  std::vector<std::shared_ptr<FilterTable::Item>> rowItems;
  bool statusRequestPending{};
  bool deviceRequestPending{};
  bool deviceSetPending{};
};
