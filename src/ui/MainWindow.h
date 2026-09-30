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
class QPushButton;
class QScrollArea;
class QTimer;
class QThread;

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QString path = {}, QString cliExecutable = {});
  ~MainWindow() override;

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
  void scheduleLiveValidation();
  void validateLiveConfig();
  void clearRowDiagnostics();
  void setRowDiagnostic(int index, const QString &diagnostic);
  void openDiagnosticSource();
  void analyzeResponse();
  void showAbout();
  void refreshRecentFilesMenu();
  void openRecentFile();
  void rememberConfig(const QString &path);
  void saveWindowPreferences();
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
  QLabel *validationLabel{};
  QPushButton *diagnosticSourceButton{};
  QString diagnosticSourcePath;
  int diagnosticSourceLine{};
  QPushButton *responseButton{};
  QThread *responseThread{};
  QComboBox *deviceCombo{};
  QMenu *recentFilesMenu{};
  FilterTable *rowTable{};
  QTimer *validationTimer{};
  std::vector<std::shared_ptr<FilterTable::Item>> rowItems;
  bool statusRequestPending{};
  bool deviceRequestPending{};
  bool deviceSetPending{};
  bool pluginRequestPending{};
  quint64 configRevision{};
};
