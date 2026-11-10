#pragma once

#include "ConfigFile.h"
#include "Editor/FilterTable.h"
#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QComboBox;
class QCheckBox;
class QCloseEvent;
class QDockWidget;
class QFileSystemWatcher;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QTimer;
class QThread;
class QVBoxLayout;
class QWidget;

class MainWindow final : public QMainWindow {
  Q_OBJECT
public:
  explicit MainWindow(QString path = {}, QString cliExecutable = {});
  ~MainWindow() override;

protected:
  void closeEvent(QCloseEvent *event) override;

private:
  enum class ResponseAnalysisChange { CurrentDocument, InputDevice };

  void openConfig(const QString &path);
  void rebuildRows();
  void addFilter(const QString &line);
  void addPluginToConfiguration(std::optional<qsizetype> beforeIndex = {});
  void configureSelectedPluginParameters();
  void markModified();
  bool saveConfig();
  Q_INVOKABLE bool saveConfigAsPath(const QString &path);
  void saveConfigAs();
  void checkConfig();
  void reloadConfig();
  void refreshStatus();
  void refreshDevices();
  void selectDevice(int index);
  void finishResponseAnalysisAfterDeviceChange();
  void scheduleLiveValidation();
  void validateLiveConfig();
  void clearRowDiagnostics();
  void setRowDiagnostic(int index, const QString &diagnostic);
  void openDiagnosticSource();
  void analyzeResponse();
  void updateAnalysisInputChannels(unsigned channels);
  void clearResponsePlot(const QString &message);
  bool watchResponseConfigFiles(const QStringList &paths);
  void markResponseAnalysisStale(ResponseAnalysisChange change);
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
  QComboBox *responseSourceCombo{};
  QComboBox *responseResolutionCombo{};
  QComboBox *analysisChannelCombo{};
  QCheckBox *analysisUseRuntimeFormat{};
  QSpinBox *analysisSampleRate{};
  QSpinBox *analysisChannels{};
  QDockWidget *responseDock{};
  QFileSystemWatcher *responseConfigWatcher{};
  QWidget *responsePlotContainer{};
  QVBoxLayout *responsePlotLayout{};
  QLabel *analysisStatusLabel{};
  QLabel *analysisSummaryLabel{};
  QLabel *analysisPeakGainLabel{};
  QLabel *analysisLatencyLabel{};
  QLabel *analysisInitializationLabel{};
  QLabel *analysisCpuLabel{};
  QThread *responseThread{};
  QThread *responseDependencyThread{};
  std::shared_ptr<std::atomic_bool> responseAnalysisCancellation;
  QString responseAnalysisDeviceConnectionName;
  std::vector<std::pair<QString, QString>> responseSymlinkTargets;
  QComboBox *deviceCombo{};
  QMenu *recentFilesMenu{};
  FilterTable *rowTable{};
  QTimer *validationTimer{};
  std::vector<std::shared_ptr<FilterTable::Item>> rowItems;
  bool statusRequestPending{};
  bool deviceRequestPending{};
  bool deviceSetPending{};
  bool daemonControlPending{};
  bool pluginRequestPending{};
  quint64 configRevision{};
  bool responseAnalysisTracksEditor{};
  bool responseAnalysisHasResult{};
  bool responseAnalysisPending{};
  bool responseAnalysisDeviceChangedWhileRunning{};
  bool responseDependencyCheckPending{};
  bool responseDependencyChangedWhileChecking{};
  QString saveAsError;
};
