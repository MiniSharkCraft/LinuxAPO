#include "MainWindow.h"
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
#include "ResponseAnalysis.h"
#endif
#include "ChannelCopyEditor.h"
#include "ConvolutionEditor.h"
#include "IncludeEditor.h"
#include "IIRFilterEditor.h"

#include "Editor/FilterTableRow.h"
#include "Editor/FilterTemplate.h"
#include "Editor/IFilterGUI.h"
#include "Editor/guis/BiQuadFilterGUIFactory.h"
#include "Editor/guis/DelayFilterGUIFactory.h"
#include "Editor/guis/PreampFilterGUIFactory.h"
#include "Editor/guis/StageFilterGUIFactory.h"
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
#include "Editor/guis/GraphicEQFilterGUIFactory.h"
#endif
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QAction>
#include <QDir>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QSettings>
#include <QTimer>
#include <QThread>
#include <algorithm>
#include <QVBoxLayout>
#include <memory>
#include <utility>

namespace {
QString defaultCliProgram() {
  const QString sibling = QCoreApplication::applicationDirPath() + "/skyapo";
  return QFileInfo::exists(sibling) ? sibling : QStringLiteral("skyapo");
}

QString runtimeStatusText(int result, const QByteArray &output,
                          const QByteArray &error) {
  const QString details = QString::fromUtf8(output).trimmed();
  if (result != 0) {
    const QString reason = QString::fromUtf8(error).trimmed();
    return QObject::tr("Daemon offline or unresponsive") +
           (reason.isEmpty() ? QString{} : QStringLiteral("\n") + reason);
  }

  QString state;
  for (const auto &line : details.split(QLatin1Char('\n'))) {
    if (line.startsWith(QStringLiteral("Daemon: "))) {
      state = line.mid(8).trimmed().toLower();
      break;
    }
  }

  QString summary;
  if (state == QStringLiteral("streaming")) {
    summary = QObject::tr("Streaming — audio processing active");
  } else if (state == QStringLiteral("connecting") ||
             state == QStringLiteral("unconnected")) {
    summary = QObject::tr("Waiting for PipeWire (filter state: %1)")
                  .arg(state);
  } else if (state == QStringLiteral("paused")) {
    summary = QObject::tr("PipeWire connected but paused (not streaming)");
  } else if (state == QStringLiteral("error")) {
    summary = QObject::tr(
        "PipeWire reported an error (recovery is not confirmed)");
  } else if (state == QStringLiteral("not reachable") ||
             state == QStringLiteral("unresponsive")) {
    summary = QObject::tr("Daemon offline or unresponsive");
  } else if (!state.isEmpty()) {
    summary = QObject::tr("Daemon state unknown: %1").arg(state);
  } else {
    summary = QObject::tr("Daemon state unavailable");
  }

  if (!details.isEmpty())
    summary += QStringLiteral("\n") + details;
  else
    summary += QObject::tr("\nRuntime details unavailable");
  return summary;
}

} // namespace

#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
class ResponsePlot final : public QWidget {
public:
  explicit ResponsePlot(QVector<ResponseCurve> curves, QWidget *parent = nullptr)
      : QWidget(parent), curves(std::move(curves)) {
    setMinimumSize(700, 380);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    const QRectF plot(68, 18, width() - 88, height() - 64);
    constexpr double minHz = 20.0, maxHz = 24000.0;
    constexpr double minDb = -24.0, maxDb = 12.0;
    const auto xFor = [&plot](double hz) {
      return plot.left() + std::log(hz / minHz) / std::log(maxHz / minHz) * plot.width();
    };
    const auto yFor = [&plot](double db) {
      return plot.bottom() - (db - minDb) / (maxDb - minDb) * plot.height();
    };
    painter.setPen(QPen(palette().mid().color(), 1));
    for (int db = -24; db <= 12; db += 6) {
      const double y = yFor(db);
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
      painter.drawText(QRectF(4, y - 9, 56, 18), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1 dB").arg(db));
    }
    for (const double hz : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0,
                            2000.0, 5000.0, 10000.0, 20000.0}) {
      const double x = xFor(hz);
      painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
      painter.drawText(QRectF(x - 30, plot.bottom() + 5, 60, 18), Qt::AlignHCenter,
                       hz >= 1000.0 ? QStringLiteral("%1k").arg(hz / 1000.0)
                                    : QString::number(static_cast<int>(hz)));
    }
    painter.setPen(palette().text().color());
    painter.drawText(QRectF(plot.left(), height() - 22, plot.width(), 18),
                     Qt::AlignHCenter, tr("Frequency (Hz, logarithmic)"));
    const QList<QColor> colors{QColor(40, 120, 220), QColor(220, 90, 65),
                               QColor(50, 160, 100), QColor(150, 80, 180)};
    int curveIndex = 0;
    for (const auto &curve : curves) {
      QPainterPath path;
      bool started = false;
      for (const auto &point : curve.points) {
        if (point.x() < minHz || point.x() > maxHz)
          continue;
        const QPointF mapped(xFor(point.x()), yFor(std::clamp(point.y(), minDb, maxDb)));
        if (!started) {
          path.moveTo(mapped);
          started = true;
        } else {
          path.lineTo(mapped);
        }
      }
      painter.setPen(QPen(colors[curveIndex % colors.size()], 2));
      painter.drawPath(path);
      painter.drawText(QPointF(plot.left() + curveIndex * 110, plot.top() + 14),
                       curve.channel);
      ++curveIndex;
    }
  }

private:
  QVector<ResponseCurve> curves;
};
#endif

MainWindow::MainWindow(QString path, QString cliExecutable)
    : cliExecutable(std::move(cliExecutable)) {
  Q_INIT_RESOURCE(skyapo_editor);
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  Q_INIT_RESOURCE(skyapo_graphiceq);
#endif
  setWindowTitle(tr("Equalizer APO Configuration Editor — SkyAPO"));
  resize(1080, 760);

  auto *root = new QWidget(this);
  auto *outer = new QVBoxLayout(root);
  auto *toolbar = new QHBoxLayout;
  pathEdit = new QLineEdit(root);
  pathEdit->setObjectName(QStringLiteral("configPath"));
  pathEdit->setReadOnly(true);
  auto *openButton = new QPushButton(tr("Open…"), root);
  auto *saveButton = new QPushButton(tr("Save"), root);
  auto *checkButton = new QPushButton(tr("Save & Check"), root);
  auto *reloadButton = new QPushButton(tr("Save & Reload"), root);
  auto *addButton = new QPushButton(tr("Add filter"), root);
  responseButton = new QPushButton(tr("Analyze response"), root);
  responseButton->setObjectName(QStringLiteral("analyzeResponse"));
#ifndef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  responseButton->setEnabled(false);
  responseButton->setToolTip(tr("Requires FFTW3f support at build time"));
#endif
  toolbar->addWidget(pathEdit, 1);
  toolbar->addWidget(openButton);
  toolbar->addWidget(saveButton);
  toolbar->addWidget(checkButton);
  toolbar->addWidget(reloadButton);
  toolbar->addWidget(addButton);
  toolbar->addWidget(responseButton);
  outer->addLayout(toolbar);

  auto *deviceRow = new QHBoxLayout;
  deviceCombo = new QComboBox(root);
  deviceCombo->setMinimumWidth(380);
  deviceCombo->setAccessibleName(tr("Selected PipeWire capture device"));
  deviceCombo->setToolTip(
      tr("The selected device is stored by its stable PipeWire node name. "
         "Enumerated channel count and sample rate appear when available; "
         "the negotiated runtime format is shown in daemon status."));
  auto *refreshButton = new QPushButton(tr("Refresh devices"), root);
  auto *startButton = new QPushButton(tr("Start daemon"), root);
  auto *stopButton = new QPushButton(tr("Stop daemon"), root);
  deviceRow->addWidget(new QLabel(tr("Input device:"), root));
  deviceRow->addWidget(deviceCombo, 1);
  deviceRow->addWidget(refreshButton);
  deviceRow->addWidget(startButton);
  deviceRow->addWidget(stopButton);
  outer->addLayout(deviceRow);

  auto *scroll = new QScrollArea(root);
  scroll->setWidgetResizable(true);
  rowTable = new FilterTable(scroll);
  rowTable->setHandlers(
      [this](QMenu *menu) { populateAddPopupMenu(menu); },
      [this](const QString &line, FilterTable::Item *before) {
        document.insert(before ? before->index : document.lineCount(), line);
        markModified();
      },
      [this](FilterTable::Item *item) {
        if (item && item->index >= 0 && item->index < document.lineCount()) {
          document.remove(item->index);
          markModified();
        }
      },
      [this] { rebuildRows(); }, [this] { syncRowsToDocument(); },
      [this](const std::vector<FilterTable::Item *> &items) {
        for (auto *item : items)
          if (item && item->index >= 0 && item->index < document.lineCount())
            document.remove(item->index);
        if (!items.empty()) {
          markModified();
          rebuildRows();
        }
      },
      [this](FilterTable::Item *item, int offset) {
        if (!item)
          return;
        const qsizetype target = item->index + offset;
        if (target < 0 || target >= document.lineCount())
          return;
        document.move(item->index, target);
        markModified();
        rebuildRows();
      });
  rowTable->setFocusPolicy(Qt::StrongFocus);
  rowTable->createAddPopupMenu();
  scroll->setWidget(rowTable);
  outer->addWidget(scroll, 1);
  statusLabel = new QLabel(tr("Checking daemon…"), root);
  statusLabel->setObjectName(QStringLiteral("daemonStatus"));
  statusLabel->setAccessibleName(tr("PipeWire and SkyAPO runtime status"));
  statusLabel->setTextFormat(Qt::PlainText);
  statusLabel->setWordWrap(true);
  statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(statusLabel);
  validationLabel = new QLabel(tr("Configuration not checked"), root);
  validationLabel->setObjectName(QStringLiteral("liveConfigValidation"));
  validationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto *validationRow = new QHBoxLayout;
  validationRow->addWidget(validationLabel, 1);
  diagnosticSourceButton = new QPushButton(tr("Open error location"), root);
  diagnosticSourceButton->setObjectName(QStringLiteral("openDiagnosticSource"));
  diagnosticSourceButton->hide();
  validationRow->addWidget(diagnosticSourceButton);
  outer->addLayout(validationRow);
  connect(diagnosticSourceButton, &QPushButton::clicked, this,
          &MainWindow::openDiagnosticSource);
  auto *fileMenu = menuBar()->addMenu(tr("File"));
  recentFilesMenu = fileMenu->addMenu(tr("Open Recent"));
  recentFilesMenu->setObjectName(QStringLiteral("recentFilesMenu"));
  refreshRecentFilesMenu();
  auto *toolsMenu = menuBar()->addMenu(tr("Tools"));
  auto *pluginCatalogAction =
      toolsMenu->addAction(tr("Browse installed plugins…"));
  pluginCatalogAction->setObjectName(QStringLiteral("browsePlugins"));
  connect(pluginCatalogAction, &QAction::triggered, this,
          [this, pluginCatalogAction] {
            if (pluginRequestPending)
              return;
            pluginRequestPending = true;
            pluginCatalogAction->setEnabled(false);
            runCli({"plugin", "list"},
                   [this, pluginCatalogAction](int result, QByteArray output,
                                               QByteArray error) {
                     pluginRequestPending = false;
                     pluginCatalogAction->setEnabled(true);
                     if (result != 0) {
                       QMessageBox::warning(
                           this, tr("Plugin catalog"),
                           QString::fromUtf8(error.isEmpty() ? output : error));
                       return;
                     }
                     auto *dialog = new QDialog(this);
                     dialog->setAttribute(Qt::WA_DeleteOnClose);
                     dialog->setWindowTitle(tr("Installed plugins"));
                     auto *layout = new QVBoxLayout(dialog);
                     auto *catalog = new QPlainTextEdit(dialog);
                     catalog->setObjectName(QStringLiteral("pluginCatalog"));
                     catalog->setReadOnly(true);
                     catalog->setLineWrapMode(QPlainTextEdit::NoWrap);
                     catalog->setPlainText(QString::fromUtf8(output));
                     layout->addWidget(catalog);
                     auto *close = new QPushButton(tr("Close"), dialog);
                     connect(close, &QPushButton::clicked, dialog,
                             &QDialog::accept);
                     layout->addWidget(close, 0, Qt::AlignRight);
                     dialog->resize(760, 480);
                     dialog->show();
                   });
          });
  auto *helpMenu = menuBar()->addMenu(tr("Help"));
  auto *aboutAction = helpMenu->addAction(tr("About SkyAPO"));
  aboutAction->setObjectName(QStringLiteral("aboutSkyAPO"));
  connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
  setCentralWidget(root);
  QSettings settings;
  const QByteArray savedGeometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
  if (!savedGeometry.isEmpty())
    restoreGeometry(savedGeometry);
  const QByteArray savedState = settings.value(QStringLiteral("ui/state")).toByteArray();
  if (!savedState.isEmpty())
    restoreState(savedState);

  validationTimer = new QTimer(this);
  validationTimer->setSingleShot(true);
  validationTimer->setInterval(350);
  connect(validationTimer, &QTimer::timeout, this,
          &MainWindow::validateLiveConfig);

  connect(openButton, &QPushButton::clicked, this, [this] {
    if (modified &&
        QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("Discard unsaved edits and open another configuration?")) !=
            QMessageBox::Yes)
      return;
    QSettings settings;
    const QString initialDirectory = configPath.isEmpty()
        ? settings.value(QStringLiteral("ui/lastDirectory")).toString()
        : QFileInfo(configPath).absolutePath();
    const auto selected = QFileDialog::getOpenFileName(
        this, tr("Open configuration"), initialDirectory,
        tr("Configuration files (config.txt *.txt);;All files (*)"));
    if (!selected.isEmpty())
      openConfig(selected);
  });
  connect(saveButton, &QPushButton::clicked, this, &MainWindow::saveConfig);
  connect(checkButton, &QPushButton::clicked, this, &MainWindow::checkConfig);
  connect(reloadButton, &QPushButton::clicked, this, &MainWindow::reloadConfig);
  connect(addButton, &QPushButton::clicked, this, [this, addButton] {
    QMenu menu(addButton);
    populateAddPopupMenu(&menu);
    if (auto *action =
            menu.exec(addButton->mapToGlobal(QPoint(0, addButton->height()))))
      addFilter(action->data().value<FilterTemplate>().getLine());
  });
  connect(responseButton, &QPushButton::clicked, this,
          &MainWindow::analyzeResponse);
  connect(refreshButton, &QPushButton::clicked, this,
          &MainWindow::refreshDevices);
  connect(deviceCombo, qOverload<int>(&QComboBox::activated), this,
          &MainWindow::selectDevice);
  connect(startButton, &QPushButton::clicked, this, [this] {
    runCli({"start"}, [this](int result, QByteArray output, QByteArray error) {
      if (result != 0)
        QMessageBox::warning(
            this, tr("SkyAPO"),
            QString::fromUtf8(error.isEmpty() ? output : error));
      refreshStatus();
    });
  });
  connect(stopButton, &QPushButton::clicked, this, [this] {
    runCli({"stop"}, [this](int result, QByteArray output, QByteArray error) {
      if (result != 0)
        QMessageBox::warning(
            this, tr("SkyAPO"),
            QString::fromUtf8(error.isEmpty() ? output : error));
      refreshStatus();
    });
  });

  if (path.isEmpty()) {
    path = defaultConfigPath();
    if (!QFileInfo::exists(path)) {
      QDir().mkpath(QFileInfo(path).absolutePath());
      QSaveFile file(path);
      if (file.open(QIODevice::WriteOnly)) {
        file.write("Preamp: 0 dB\n");
        file.commit();
      }
    }
  }
  openConfig(path);
  refreshDevices();
  refreshStatus();
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &MainWindow::refreshStatus);
  timer->start(2000);
  auto *deviceTimer = new QTimer(this);
  deviceTimer->setObjectName(QStringLiteral("deviceRefreshTimer"));
  deviceTimer->setInterval(5000);
  connect(deviceTimer, &QTimer::timeout, this, &MainWindow::refreshDevices);
  deviceTimer->start();
}

MainWindow::~MainWindow() {
  if (responseThread && responseThread->isRunning())
    responseThread->wait();
  const auto processes = findChildren<QProcess *>();
  for (auto *process : processes) {
    QObject::disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning)
      process->kill();
  }
}

void MainWindow::analyzeResponse() {
#ifndef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  return;
#else
  if (configPath.isEmpty())
    return;
  const QString directory = QFileInfo(configPath).absolutePath();
  auto snapshot = std::make_shared<QTemporaryFile>(
      directory + QStringLiteral("/.skyapo-response-XXXXXX"));
  const QByteArray contents = document.serialize();
  if (!snapshot->open() || snapshot->write(contents) != contents.size() ||
      !snapshot->flush()) {
    QMessageBox::warning(this, tr("Response analysis"),
                         tr("Could not create a config snapshot: %1")
                             .arg(snapshot->errorString()));
    return;
  }
  const QString snapshotPath = QFileInfo(snapshot->fileName()).absoluteFilePath();
  snapshot->close();
  responseButton->setEnabled(false);
  responseButton->setText(tr("Analyzing…"));
  QPointer<MainWindow> self(this);
  auto *worker = QThread::create([self, snapshot, snapshotPath] {
    auto result = analyzeConfigResponse(snapshotPath);
    if (!self)
      return;
    QMetaObject::invokeMethod(
        self, [self, snapshot, result = std::move(result)]() mutable {
          Q_UNUSED(snapshot);
          if (!self)
            return;
          self->responseButton->setEnabled(true);
          self->responseButton->setText(self->tr("Analyze response"));
          if (!result.error.isEmpty()) {
            QMessageBox::warning(self, self->tr("Response analysis"),
                                 result.error);
            return;
          }
          QDialog dialog(self);
          dialog.setWindowTitle(self->tr("Measured filter response"));
          auto *layout = new QVBoxLayout(&dialog);
          layout->addWidget(new QLabel(
              self->tr("Actual Engine impulse response · 48 kHz stereo · 16,384 samples · all input/output channel paths"),
              &dialog));
          layout->addWidget(new ResponsePlot(std::move(result.curves), &dialog));
          auto *close = new QPushButton(self->tr("Close"), &dialog);
          QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
          layout->addWidget(close, 0, Qt::AlignRight);
          dialog.resize(820, 500);
          dialog.exec();
        }, Qt::QueuedConnection);
  });
  responseThread = worker;
  worker->setParent(this);
  connect(worker, &QThread::finished, this, [this, worker] {
    if (responseThread == worker)
      responseThread = nullptr;
    worker->deleteLater();
  });
  worker->start();
#endif
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (!modified) {
    saveWindowPreferences();
    event->accept();
    return;
  }
  const auto choice = QMessageBox::warning(
      this, tr("Unsaved configuration"),
      tr("Save changes to %1 before closing?").arg(configPath),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (choice == QMessageBox::Cancel) {
    event->ignore();
  } else if (choice == QMessageBox::Save && !saveConfig()) {
    event->ignore();
  } else {
    saveWindowPreferences();
    event->accept();
  }
}

void MainWindow::saveWindowPreferences() {
  QSettings settings;
  settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("ui/state"), saveState());
  settings.setValue(QStringLiteral("ui/lastDirectory"),
                    configPath.isEmpty() ? QString() : QFileInfo(configPath).absolutePath());
}

void MainWindow::rememberConfig(const QString &path) {
  if (path.isEmpty())
    return;
  QSettings settings;
  QStringList files = settings.value(QStringLiteral("ui/recentFiles")).toStringList();
  const QString absolutePath = QFileInfo(path).absoluteFilePath();
  files.removeAll(absolutePath);
  files.prepend(absolutePath);
  while (files.size() > 10)
    files.removeLast();
  settings.setValue(QStringLiteral("ui/recentFiles"), files);
  refreshRecentFilesMenu();
}

void MainWindow::refreshRecentFilesMenu() {
  if (!recentFilesMenu)
    return;
  recentFilesMenu->clear();
  QSettings settings;
  const QStringList files = settings.value(QStringLiteral("ui/recentFiles")).toStringList();
  int count = 0;
  for (const auto &path : files) {
    if (!QFileInfo::exists(path))
      continue;
    auto *action = recentFilesMenu->addAction(QFileInfo(path).fileName());
    action->setToolTip(path);
    action->setData(path);
    connect(action, &QAction::triggered, this, &MainWindow::openRecentFile);
    if (++count == 10)
      break;
  }
  if (count == 0) {
    auto *empty = recentFilesMenu->addAction(tr("No recent configurations"));
    empty->setEnabled(false);
  }
}

void MainWindow::openRecentFile() {
  auto *action = qobject_cast<QAction *>(sender());
  if (!action)
    return;
  if (modified &&
      QMessageBox::question(this, tr("Unsaved changes"),
                            tr("Discard unsaved edits and open another configuration?")) != QMessageBox::Yes)
    return;
  openConfig(action->data().toString());
}

QString MainWindow::defaultConfigPath() const {
  return QStandardPaths::writableLocation(
             QStandardPaths::GenericConfigLocation) +
         "/skyapo/config.txt";
}

void MainWindow::runCli(const QStringList &arguments,
                        CliCompletion completion) {
  auto *process = new QProcess(this);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  struct CompletionState {
    bool delivered{};
    CliCompletion callback;
  };
  auto state = std::make_shared<CompletionState>();
  state->callback = std::move(completion);
  auto *timeout = new QTimer(process);
  timeout->setObjectName(QStringLiteral("cliTimeout"));
  timeout->setSingleShot(true);
  constexpr int cliTimeoutMilliseconds = 30000;
  const auto finish = [process, timeout, state](int result, QByteArray output,
                                       QByteArray error) {
    if (state->delivered)
      return;
    state->delivered = true;
    timeout->stop();
    process->deleteLater();
    if (state->callback)
      state->callback(result, std::move(output), std::move(error));
  };
  connect(
      process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [process, finish](int exitCode, QProcess::ExitStatus exitStatus) {
        const int result = exitStatus == QProcess::NormalExit ? exitCode : -1;
        finish(result, process->readAllStandardOutput(),
               process->readAllStandardError());
      });
  connect(process, &QProcess::errorOccurred, this,
          [process, finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
              finish(-1, process->readAllStandardOutput(),
                     process->errorString().toUtf8());
          });
  connect(timeout, &QTimer::timeout, this, [process, finish, arguments] {
    const QString command = arguments.join(QLatin1Char(' '));
    const QByteArray detail =
        tr("SkyAPO command timed out after %1 seconds: %2")
            .arg(cliTimeoutMilliseconds / 1000)
            .arg(command)
            .toUtf8();
    process->kill();
    finish(-1, process->readAllStandardOutput(), detail);
  });
  process->start(this->cliExecutable.isEmpty() ? defaultCliProgram()
                                               : this->cliExecutable,
                 arguments);
  timeout->start(cliTimeoutMilliseconds);
}

void MainWindow::openConfig(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::critical(this, tr("Open failed"), file.errorString());
    return;
  }
  configPath = QFileInfo(path).absoluteFilePath();
  rememberConfig(configPath);
  document.load(file.readAll());
  modified = false;
  ++configRevision;
  pathEdit->setText(configPath);
  rebuildRows();
  updateTitle();
  validationLabel->setText(tr("Checking configuration…"));
  scheduleLiveValidation();
}

void MainWindow::rebuildRows() {
  clearRowDiagnostics();
  rowTable->clearRows();
  rowItems.clear();

  static PreampFilterGUIFactory preampFactory;
  static BiQuadFilterGUIFactory biquadFactory;
  static DelayFilterGUIFactory delayFactory;
  static StageFilterGUIFactory stageFactory;
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  static GraphicEQFilterGUIFactory graphicEQFactory;
  graphicEQFactory.initialize(rowTable);
  graphicEQFactory.startOfFile(configPath);
#endif
  for (qsizetype index = 0; index < document.lineCount(); ++index) {
    const QString raw = document.line(index).toQString();
    auto item = std::make_shared<FilterTable::Item>();
    item->text = raw;
    item->index = static_cast<int>(index);

    IFilterGUI *editor = nullptr;
    const qsizetype colon = raw.indexOf(':');
    if (colon >= 0) {
      QString command = raw.left(colon).trimmed();
      QString parameters = raw.mid(colon + 1).trimmed();
      if (command == "Convolution")
        editor = new ConvolutionEditor(parameters, configPath, rowTable);
      if (command == "Include")
        editor = new IncludeEditor(parameters, configPath, rowTable);
      if (!editor)
        editor = IIRFilterEditor::create(command, parameters, rowTable);
      if (!editor)
        editor = ChannelCopyEditor::create(command, parameters, rowTable);
      if (!editor)
        editor = preampFactory.createFilterGUI(command, parameters);
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = biquadFactory.createFilterGUI(command, parameters);
      }
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = delayFactory.createFilterGUI(command, parameters);
      }
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = stageFactory.createFilterGUI(command, parameters);
      }
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = graphicEQFactory.createFilterGUI(command, parameters);
      }
#endif
    }

    item->gui = editor;
    auto *row = new FilterTableRow(rowTable, static_cast<int>(index + 1),
                                   item.get(), editor);
    rowTable->addRow(row, item.get());
    if (editor) {
      connect(
          editor, &IFilterGUI::updateModel, this, [this, editor, item, index] {
            QString command, parameters;
            editor->store(command, parameters);
            const QString original = document.line(index).toQString();
            const qsizetype colon = original.indexOf(':');
            const QString key =
                colon >= 0 ? original.left(colon + 1) : command + ':';
            const QString afterColon =
                colon >= 0 ? original.mid(colon + 1) : QString{};
            qsizetype spacing = 0;
            while (spacing < afterColon.size() && afterColon[spacing].isSpace())
              ++spacing;
            const QString updated = key + afterColon.left(spacing) + parameters;
            document.replace(index, updated);
            item->text = updated;
            markModified();
          });
    }

    connect(row, &QObject::destroyed, this, [item] { Q_UNUSED(item); });
    rowItems.push_back(std::move(item));
  }
}

void MainWindow::populateAddPopupMenu(QMenu *menu) {
  PreampFilterGUIFactory preamp;
  BiQuadFilterGUIFactory biquad;
  DelayFilterGUIFactory delay;
  StageFilterGUIFactory stage;
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  GraphicEQFilterGUIFactory graphicEQ;
#endif
  const auto append = [menu](const QList<FilterTemplate> &templates) {
    for (const auto &filter : templates) {
      auto *action = menu->addAction(filter.getName());
      action->setData(QVariant::fromValue(filter));
    }
  };
  append(preamp.createFilterTemplates());
  menu->addAction(tr("Channel selection"))->setData(
      QVariant::fromValue(FilterTemplate(tr("Channel selection"),
                                         "Channel: L R", {tr("Basic filters")})));
  menu->addAction(tr("Copy between channels"))->setData(
      QVariant::fromValue(FilterTemplate(tr("Copy between channels"),
                                         "Copy: L=L", {tr("Basic filters")})));
  menu->addSection(tr("Parametric filters"));
  append(biquad.createFilterTemplates());
  menu->addAction(tr("Custom IIR filter"))
      ->setData(QVariant::fromValue(
          FilterTemplate(tr("Custom IIR filter"),
                         "Filter: ON IIR Order 2 Coefficients 1 0 0 1 0 0",
                         {tr("Parametric filters")})));
  menu->addSeparator();
  append(delay.createFilterTemplates());
  append(stage.createFilterTemplates());
  menu->addAction(tr("Convolution (impulse response)"))
      ->setData(QVariant::fromValue(FilterTemplate(
          tr("Convolution (impulse response)"),
          "Convolution: impulse-response.wav", {tr("Advanced filters")})));
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  menu->addSeparator();
  append(graphicEQ.createFilterTemplates());
#endif
}

void MainWindow::syncRowsToDocument() {
  const qsizetype common =
      std::min<qsizetype>(rowItems.size(), document.lineCount());
  for (qsizetype index = 0; index < common; ++index) {
    const auto &item = rowItems[static_cast<size_t>(index)];
    if (document.line(index).toQString() != item->text) {
      document.replace(index, item->text);
      markModified();
    }
  }
}

void MainWindow::addFilter(const QString &line) {
  const bool hadLines = document.lineCount() > 0;
  const qsizetype last = document.lineCount() - 1;
  if (hadLines && document.line(last).ending.isEmpty())
    document.insert(last + 1, line);
  else
    document.insert(document.lineCount(), line);
  markModified();
  rebuildRows();
}

void MainWindow::markModified() {
  modified = true;
  clearRowDiagnostics();
  ++configRevision;
  updateTitle();
  validationLabel->setText(tr("Checking unsaved configuration edits…"));
  scheduleLiveValidation();
}

void MainWindow::scheduleLiveValidation() {
  if (validationTimer)
    validationTimer->start();
}

void MainWindow::clearRowDiagnostics() {
  diagnosticSourcePath.clear();
  diagnosticSourceLine = 0;
  if (diagnosticSourceButton)
    diagnosticSourceButton->hide();
  for (const auto &item : rowItems) {
    if (!item || !item->row)
      continue;
    item->row->setProperty("skyapoValidationError", false);
    item->row->setToolTip({});
    item->row->setAccessibleDescription({});
    if (auto *number = item->row->findChild<QLabel *>("labelNumber")) {
      number->setStyleSheet({});
      number->setToolTip({});
    }
  }
}

void MainWindow::openDiagnosticSource() {
  if (diagnosticSourcePath.isEmpty())
    return;
  auto *window = new MainWindow(diagnosticSourcePath, cliExecutable);
  window->setAttribute(Qt::WA_DeleteOnClose);
  window->show();
  const int line = diagnosticSourceLine;
  QTimer::singleShot(0, window, [window, line] {
    if (line <= 0 || !window->rowTable)
      return;
    auto *item = window->rowTable->itemAt(line - 1);
    if (!item)
      return;
    window->rowTable->setSelection(item);
    if (auto *scroll = window->findChild<QScrollArea *>())
      scroll->ensureWidgetVisible(item->row);
  });
}

void MainWindow::showAbout() {
  QMessageBox::about(
      this, tr("About SkyAPO"),
      tr("SkyAPO — Linux audio processing built around Equalizer APO.\n\n"
         "SkyAPO and the reused Equalizer APO components are licensed under "
         "GNU GPL-2.0-or-later.\n"
         "This application uses Qt %1 Widgets through dynamic system-library "
         "linking under LGPL-3.0. Qt license text and third-party notices are "
         "included with the installed documentation.")
          .arg(QString::fromLatin1(qVersion())));
}

void MainWindow::setRowDiagnostic(int index, const QString &diagnostic) {
  if (index < 0 || static_cast<size_t>(index) >= rowItems.size())
    return;
  const auto &item = rowItems[static_cast<size_t>(index)];
  if (!item || !item->row)
    return;
  item->row->setProperty("skyapoValidationError", true);
  item->row->setToolTip(diagnostic);
  item->row->setAccessibleDescription(diagnostic);
  if (auto *number = item->row->findChild<QLabel *>("labelNumber")) {
    number->setStyleSheet(QStringLiteral("color: #c62828; font-weight: bold"));
    number->setToolTip(diagnostic);
  }
}

void MainWindow::validateLiveConfig() {
  if (configPath.isEmpty())
    return;

  const QString directory = QFileInfo(configPath).absolutePath();
  auto snapshot = std::make_shared<QTemporaryFile>(
      directory + QStringLiteral("/.skyapo-validation-XXXXXX"));
  snapshot->setAutoRemove(true);
  const QByteArray contents = document.serialize();
  if (!snapshot->open() || snapshot->write(contents) != contents.size() ||
      !snapshot->flush()) {
    validationLabel->setText(
        tr("Could not create a temporary config snapshot: %1")
            .arg(snapshot->errorString()));
    return;
  }
  const QString snapshotPath =
      QFileInfo(snapshot->fileName()).absoluteFilePath();
  snapshot->close();
  const quint64 revision = configRevision;
  runCli(
      {"config", "check", "--json", snapshotPath},
      [this, revision, snapshotPath, snapshot](int result, QByteArray output,
                                               QByteArray error) {
        Q_UNUSED(snapshot);
        if (revision != configRevision)
          return;

        QJsonParseError parseError;
        const auto json = QJsonDocument::fromJson(output, &parseError);
        if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
          const QString detail =
              QString::fromUtf8(error.isEmpty() ? output : error).trimmed();
          validationLabel->setText(
              tr("Config validation failed: %1")
                  .arg(detail.isEmpty() ? tr("invalid response") : detail));
          return;
        }

        clearRowDiagnostics();
        const QJsonObject report = json.object();
        if (result == 0 && report.value(QStringLiteral("valid")).toBool()) {
          validationLabel->setText(
              tr("Valid config — %1 filters%2")
                  .arg(report.value(QStringLiteral("filter_count")).toInt())
                  .arg(modified ? tr(" (unsaved edits)") : QString{}));
          return;
        }

        const QJsonArray diagnostics =
            report.value(QStringLiteral("diagnostics")).toArray();
        if (diagnostics.isEmpty()) {
          validationLabel->setText(tr("Configuration is invalid"));
          return;
        }
        const QJsonObject diagnostic = diagnostics.first().toObject();
        QString source = diagnostic.value(QStringLiteral("file")).toString();
        const QString actualSource = source;
        const int line = diagnostic.value(QStringLiteral("line")).toInt();
        const QJsonArray includeChain = diagnostic
                                            .value(QStringLiteral("include_chain"))
                                            .toArray();
        QString rootIncludeFile;
        int rootIncludeLine = 0;
        if (!includeChain.isEmpty()) {
          const QJsonObject rootSite = includeChain.first().toObject();
          rootIncludeFile = rootSite.value(QStringLiteral("file")).toString();
          rootIncludeLine = rootSite.value(QStringLiteral("line")).toInt();
        }
        if (source.isEmpty() ||
            QFileInfo(source).absoluteFilePath() == snapshotPath)
          source = configPath;
        const QString location =
            line > 0 ? QStringLiteral("%1:%2").arg(source).arg(line) : source;
        const QString reason =
            diagnostic.value(QStringLiteral("reason")).toString();
        const bool includeFromSnapshot =
            !rootIncludeFile.isEmpty() && rootIncludeLine > 0 &&
            QFileInfo(rootIncludeFile).absoluteFilePath() ==
                QFileInfo(snapshotPath).absoluteFilePath();
        if (includeFromSnapshot) {
          const QString detail = tr("Included file error — %1: %2")
                                     .arg(location,
                                          reason.isEmpty()
                                              ? tr("unknown error")
                                              : reason);
          setRowDiagnostic(rootIncludeLine - 1, detail);
          diagnosticSourcePath = actualSource;
          diagnosticSourceLine = line;
          diagnosticSourceButton->show();
        } else if (QFileInfo(source).absoluteFilePath() ==
                   QFileInfo(configPath).absoluteFilePath()) {
          setRowDiagnostic(line - 1,
                           reason.isEmpty() ? tr("Invalid configuration") : reason);
        }
        validationLabel->setText(tr("Invalid config — %1: %2")
                                     .arg(location, reason.isEmpty()
                                                        ? tr("unknown error")
                                                        : reason));
      });
}

void MainWindow::updateTitle() {
  setWindowTitle(
      QStringLiteral("%1%2 — Equalizer APO Configuration Editor — SkyAPO")
          .arg(modified ? QStringLiteral("* ") : QString{}, configPath));
}

bool MainWindow::saveConfig() {
  if (configPath.isEmpty())
    return false;
  if (!modified)
    return true;
  QSaveFile file(configPath);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(document.serialize()) < 0 || !file.commit()) {
    QMessageBox::critical(this, tr("Save failed"), file.errorString());
    return false;
  }
  modified = false;
  updateTitle();
  return true;
}

void MainWindow::checkConfig() {
  if (!saveConfig())
    return;
  runCli({"config", "check", configPath}, [this](int result, QByteArray output,
                                                 QByteArray error) {
    QMessageBox::information(this,
                             result == 0 ? tr("Configuration valid")
                                         : tr("Configuration error"),
                             QString::fromUtf8(result == 0 ? output : error));
  });
}

void MainWindow::reloadConfig() {
  if (!saveConfig())
    return;
  runCli({"config", "reload"},
         [this](int result, QByteArray output, QByteArray error) {
           QMessageBox::information(
               this, result == 0 ? tr("Reload requested") : tr("Reload failed"),
               QString::fromUtf8(result == 0 ? output : error));
           refreshStatus();
         });
}

void MainWindow::refreshStatus() {
  if (statusRequestPending)
    return;
  statusRequestPending = true;
  runCli({"status"}, [this](int result, QByteArray output, QByteArray error) {
    statusRequestPending = false;
    const QString status = runtimeStatusText(result, output, error);
    statusLabel->setText(status);
    statusLabel->setAccessibleDescription(status);
  });
}

void MainWindow::refreshDevices() {
  if (deviceRequestPending)
    return;
  deviceRequestPending = true;
  runCli({"device", "list"},
         [this](int result, QByteArray output, QByteArray error) {
           deviceRequestPending = false;
           if (result != 0) {
             QString reason = QString::fromUtf8(error).trimmed();
             if (reason.isEmpty())
               reason = QString::fromUtf8(output).trimmed();
             if (reason.isEmpty())
               reason =
                   tr("skyapo device list exited with status %1").arg(result);
             const QString message = tr("Device query failed: %1").arg(reason);
             statusLabel->setText(message);
             statusLabel->setAccessibleDescription(message);
             return;
           }
           const QSignalBlocker blocker(deviceCombo);
           QString previousNode = deviceCombo->currentData().toString();
           if (previousNode.isEmpty() && deviceCombo->currentIndex() >= 0)
             previousNode =
                 deviceCombo
                     ->itemData(deviceCombo->currentIndex(), Qt::UserRole + 1)
                     .toString();
           deviceCombo->clear();
           bool selectedByDaemon = false;
           const auto lines =
               QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts);
           for (qsizetype i = 1; i < lines.size(); ++i) {
             const auto columns = lines[i].split('\t');
             if (columns.size() < 4)
               continue;
             const QString nodeName = columns[1];
             const QString description = columns[2];
             QString label = description + "  —  " + nodeName;
             if (columns.size() >= 6) {
               const QString channelCount = columns[4];
               const QString sampleRate = columns[5];
               if (channelCount != "unknown" || sampleRate != "unknown")
                 label += tr("  (%1 ch, %2 Hz)").arg(channelCount, sampleRate);
             }
             deviceCombo->addItem(label, nodeName);
             if (columns[3] == "yes") {
               deviceCombo->setCurrentIndex(deviceCombo->count() - 1);
               selectedByDaemon = true;
             }
           }
           if (!selectedByDaemon && !previousNode.isEmpty()) {
             int previousIndex = -1;
             for (int index = 0; index < deviceCombo->count(); ++index)
               if (deviceCombo->itemData(index).toString() == previousNode) {
                 previousIndex = index;
                 break;
               }
             if (previousIndex >= 0) {
               deviceCombo->setCurrentIndex(previousIndex);
             } else {
               deviceCombo->insertItem(
                   0, tr("Selected input unavailable — %1").arg(previousNode),
                   QString{});
               deviceCombo->setItemData(0, previousNode, Qt::UserRole + 1);
               deviceCombo->setCurrentIndex(0);
             }
           }
           if (deviceCombo->count() == 0)
             deviceCombo->addItem(tr("No PipeWire input devices"));
           bool hasSelectableDevice = false;
           for (int index = 0; index < deviceCombo->count(); ++index)
             hasSelectableDevice =
                 hasSelectableDevice ||
                 !deviceCombo->itemData(index).toString().isEmpty();
           deviceCombo->setEnabled(!deviceSetPending && hasSelectableDevice);
         });
}

void MainWindow::selectDevice(int index) {
  if (deviceSetPending || index < 0)
    return;
  const auto nodeName = deviceCombo->itemData(index).toString();
  if (nodeName.isEmpty())
    return;
  deviceSetPending = true;
  deviceCombo->setEnabled(false);
  runCli({"device", "set", nodeName},
         [this](int result, QByteArray, QByteArray error) {
           deviceSetPending = false;
           if (result != 0)
             QMessageBox::warning(this, tr("Device selection failed"),
                                  QString::fromUtf8(error));
           refreshDevices();
           refreshStatus();
         });
}
