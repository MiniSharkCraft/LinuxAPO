#include "MainWindow.h"

#include "Editor/FilterTemplate.h"
#include "Editor/IFilterGUI.h"
#include "Editor/guis/BiQuadFilterGUIFactory.h"
#include "Editor/guis/DelayFilterGUIFactory.h"
#include "Editor/guis/PreampFilterGUIFactory.h"
#include "Editor/guis/StageFilterGUIFactory.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <memory>
#include <utility>

namespace {
QString defaultCliProgram() {
  const QString sibling = QCoreApplication::applicationDirPath() + "/skyapo";
  return QFileInfo::exists(sibling) ? sibling : QStringLiteral("skyapo");
}

} // namespace

MainWindow::MainWindow(QString path, QString cliExecutable)
    : cliExecutable(std::move(cliExecutable)) {
  setWindowTitle(tr("Equalizer APO Configuration Editor — SkyAPO"));
  resize(1080, 760);

  auto *root = new QWidget(this);
  auto *outer = new QVBoxLayout(root);
  auto *toolbar = new QHBoxLayout;
  pathEdit = new QLineEdit(root);
  pathEdit->setReadOnly(true);
  auto *openButton = new QPushButton(tr("Open…"), root);
  auto *saveButton = new QPushButton(tr("Save"), root);
  auto *checkButton = new QPushButton(tr("Save & Check"), root);
  auto *reloadButton = new QPushButton(tr("Save & Reload"), root);
  auto *addButton = new QPushButton(tr("Add filter"), root);
  toolbar->addWidget(pathEdit, 1);
  toolbar->addWidget(openButton);
  toolbar->addWidget(saveButton);
  toolbar->addWidget(checkButton);
  toolbar->addWidget(reloadButton);
  toolbar->addWidget(addButton);
  outer->addLayout(toolbar);

  auto *deviceRow = new QHBoxLayout;
  deviceCombo = new QComboBox(root);
  deviceCombo->setMinimumWidth(380);
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
  auto *rows = new QWidget(scroll);
  rowsLayout = new QVBoxLayout(rows);
  rowsLayout->setContentsMargins(8, 8, 8, 8);
  rowsLayout->setSpacing(5);
  rowsLayout->addStretch(1);
  scroll->setWidget(rows);
  outer->addWidget(scroll, 1);
  statusLabel = new QLabel(tr("Checking daemon…"), root);
  statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(statusLabel);
  setCentralWidget(root);

  connect(openButton, &QPushButton::clicked, this, [this] {
    if (modified &&
        QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("Discard unsaved edits and open another configuration?")) !=
            QMessageBox::Yes)
      return;
    const auto selected = QFileDialog::getOpenFileName(
        this, tr("Open configuration"), configPath,
        tr("Configuration files (config.txt *.txt);;All files (*)"));
    if (!selected.isEmpty())
      openConfig(selected);
  });
  connect(saveButton, &QPushButton::clicked, this, &MainWindow::saveConfig);
  connect(checkButton, &QPushButton::clicked, this, &MainWindow::checkConfig);
  connect(reloadButton, &QPushButton::clicked, this, &MainWindow::reloadConfig);
  connect(addButton, &QPushButton::clicked, this, [this, addButton] {
    QMenu menu(addButton);
    PreampFilterGUIFactory preamp;
    BiQuadFilterGUIFactory biquad;
    DelayFilterGUIFactory delay;
    StageFilterGUIFactory stage;
    for (const auto &filter : preamp.createFilterTemplates()) {
      auto *action = menu.addAction(filter.getName());
      action->setData(filter.getLine());
    }
    auto *section = menu.addSection(tr("Parametric filters"));
    Q_UNUSED(section);
    for (const auto &filter : biquad.createFilterTemplates()) {
      auto *action = menu.addAction(filter.getName());
      action->setData(filter.getLine());
    }
    menu.addSeparator();
    for (const auto &filter : delay.createFilterTemplates()) {
      auto *action = menu.addAction(filter.getName());
      action->setData(filter.getLine());
    }
    for (const auto &filter : stage.createFilterTemplates()) {
      auto *action = menu.addAction(filter.getName());
      action->setData(filter.getLine());
    }
    if (auto *action =
            menu.exec(addButton->mapToGlobal(QPoint(0, addButton->height()))))
      addFilter(action->data().toString());
  });
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
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (!modified) {
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
    event->accept();
  }
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
  const auto finish = [process, state](int result, QByteArray output,
                                       QByteArray error) {
    if (state->delivered)
      return;
    state->delivered = true;
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
  process->start(this->cliExecutable.isEmpty() ? defaultCliProgram()
                                               : this->cliExecutable,
                 arguments);
}

void MainWindow::openConfig(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::critical(this, tr("Open failed"), file.errorString());
    return;
  }
  configPath = QFileInfo(path).absoluteFilePath();
  document.load(file.readAll());
  modified = false;
  pathEdit->setText(configPath);
  rebuildRows();
  updateTitle();
}

void MainWindow::rebuildRows() {
  while (rowsLayout->count() > 1) {
    auto *item = rowsLayout->takeAt(0);
    if (item->widget())
      item->widget()->deleteLater();
    delete item;
  }

  static PreampFilterGUIFactory preampFactory;
  static BiQuadFilterGUIFactory biquadFactory;
  static DelayFilterGUIFactory delayFactory;
  static StageFilterGUIFactory stageFactory;
  for (qsizetype index = 0; index < document.lineCount(); ++index) {
    const QString raw = document.line(index).toQString();
    auto *frame = new QFrame(this);
    frame->setFrameShape(QFrame::StyledPanel);
    auto *layout = new QHBoxLayout(frame);
    layout->setContentsMargins(6, 4, 6, 4);
    auto *number = new QLabel(QString::number(index + 1), frame);
    number->setMinimumWidth(32);
    layout->addWidget(number);

    IFilterGUI *editor = nullptr;
    const qsizetype colon = raw.indexOf(':');
    if (colon >= 0) {
      QString command = raw.left(colon).trimmed();
      QString parameters = raw.mid(colon + 1).trimmed();
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
    }

    if (editor) {
      layout->addWidget(editor, 1);
      connect(
          editor, &IFilterGUI::updateModel, this, [this, editor, index, raw] {
            QString command, parameters;
            editor->store(command, parameters);
            const qsizetype colon = raw.indexOf(':');
            const QString key =
                colon >= 0 ? raw.left(colon + 1) : command + ':';
            const QString afterColon =
                colon >= 0 ? raw.mid(colon + 1) : QString{};
            qsizetype spacing = 0;
            while (spacing < afterColon.size() && afterColon[spacing].isSpace())
              ++spacing;
            document.replace(index,
                             key + afterColon.left(spacing) + parameters);
            markModified();
          });
    } else {
      auto *rawEditor = new QLineEdit(raw, frame);
      layout->addWidget(rawEditor, 1);
      connect(rawEditor, &QLineEdit::textEdited, this,
              [this, index](const QString &text) {
                document.replace(index, text);
                markModified();
              });
    }

    auto *remove = new QToolButton(frame);
    remove->setText(QStringLiteral("×"));
    remove->setToolTip(tr("Remove this line"));
    layout->addWidget(remove);
    connect(remove, &QToolButton::clicked, this, [this, index] {
      document.remove(index);
      markModified();
      rebuildRows();
    });
    rowsLayout->insertWidget(rowsLayout->count() - 1, frame);
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
  updateTitle();
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
    statusLabel->setText(
        result == 0
            ? QString::fromUtf8(output).trimmed()
            : tr("Daemon query failed: %1").arg(QString::fromUtf8(error)));
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
             statusLabel->setText(
                 tr("Device query failed: %1").arg(QString::fromUtf8(error)));
             return;
           }
           const QSignalBlocker blocker(deviceCombo);
           deviceCombo->clear();
           const auto lines =
               QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts);
           for (qsizetype i = 1; i < lines.size(); ++i) {
             const auto columns = lines[i].split('\t');
             if (columns.size() < 4)
               continue;
             const QString nodeName = columns[1];
             const QString description = columns[2];
             deviceCombo->addItem(description + "  —  " + nodeName, nodeName);
             if (columns[3] == "yes")
               deviceCombo->setCurrentIndex(deviceCombo->count() - 1);
           }
           if (deviceCombo->count() == 0)
             deviceCombo->addItem(tr("No PipeWire input devices"));
         });
}

void MainWindow::selectDevice(int index) {
  const auto nodeName = deviceCombo->itemData(index).toString();
  if (nodeName.isEmpty())
    return;
  runCli({"device", "set", nodeName},
         [this](int result, QByteArray, QByteArray error) {
           if (result != 0)
             QMessageBox::warning(this, tr("Device selection failed"),
                                  QString::fromUtf8(error));
           refreshStatus();
         });
}
