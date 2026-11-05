#include "ConfigFile.h"
#include "ChannelCopyEditor.h"
#include "ConvolutionEditor.h"
#include "DeviceFilterEditor.h"
#include "IncludeEditor.h"
#include "IIRFilterEditor.h"
#include "MainWindow.h"
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
#include "ResponseAnalysis.h"
#endif

#include "Editor/FilterTable.h"
#include "Editor/FilterTableRow.h"
#include "Editor/FilterTemplate.h"
#include "Editor/helpers/GUIHelper.h"
#include "Editor/guis/BiQuadFilterGUI.h"
#include "Editor/guis/BiQuadFilterGUIFactory.h"
#include "Editor/guis/DelayFilterGUI.h"
#include "Editor/guis/DelayFilterGUIFactory.h"
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
#include "Editor/guis/GraphicEQFilterGUI.h"
#endif
#include "Editor/guis/PreampFilterGUI.h"
#include "Editor/guis/PreampFilterGUIFactory.h"
#include "Editor/guis/StageFilterGUI.h"
#include "Editor/guis/StageFilterGUIFactory.h"
#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QCheckBox>
#include <QComboBox>
#include <QAction>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPalette>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QToolButton>
#include <QMessageBox>
#include <QIcon>
#include <QMenu>
#include <QSettings>
#include <QStandardPaths>
#include <cmath>
#include <limits>
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
#include <QTableWidget>
#endif
#include <iostream>

namespace {
bool writeTestImpulseResponse(const QString &path) {
  QByteArray wav;
  const auto append16 = [&wav](quint16 value) {
    wav.append(static_cast<char>(value & 0xff));
    wav.append(static_cast<char>((value >> 8) & 0xff));
  };
  const auto append32 = [&wav](quint32 value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
      wav.append(static_cast<char>((value >> shift) & 0xff));
  };
  wav.append("RIFF", 4);
  append32(36 + 16);
  wav.append("WAVEfmt ", 8);
  append32(16);
  append16(1); // PCM
  append16(1); // mono
  append32(48000);
  append32(96000);
  append16(2);
  append16(16);
  wav.append("data", 4);
  append32(16);
  for (int sample = 0; sample < 8; ++sample)
    append16(static_cast<quint16>(sample * 256));

  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size();
}

bool writeStatusCliFixture(const QString &path, const QString &state,
                           int exitCode = 0,
                           const QString &dspState = QStringLiteral("running"),
                           bool negotiatedFormat = false) {
  QByteArray script = "#!/bin/sh\n"
                      "if [ \"$1\" = \"status\" ]; then\n"
                      "  printf '%s\\n' 'Daemon: ";
  script += state.toUtf8();
  script += "' 'DSP state: ";
  script += dspState.toUtf8();
  script += "' 'Selected device: fixture.mic' 'Capture node: 52 (Test mic)' "
            "'Virtual microphone: SkyAPO Virtual Mic' 'Virtual node: 90 "
            "(skyapo.virtual_mic)' 'Format: F32 planar DSP' 'Channels: 2' ";
  script +=
      negotiatedFormat
          ? "'Sample rate: 48000 Hz' 'Quantum: 1024'\n"
          : "'Sample rate: unknown (awaiting graph)' 'Quantum: unknown'\n";
  script += "  exit ";
  script += QByteArray::number(exitCode);
  script += "\nfi\n";
  if (exitCode != 0)
    script += "echo 'daemon control socket is unresponsive' >&2\n";
  script += "exit 0\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool writeDeviceQueryFailureCliFixture(const QString &path) {
  const QByteArray script =
      "#!/bin/sh\n"
      "if [ \"$1\" = \"status\" ]; then\n"
      "  sleep 2\n"
      "  printf '%s\\n' 'Daemon: streaming'\n"
      "  exit 0\n"
      "fi\n"
      "if [ \"$1\" = \"device\" ] && [ \"$2\" = \"list\" ]; then\n"
      "  printf '%s\\n' 'enumeration fixture detail'\n"
      "  exit 7\n"
      "fi\n"
      "exit 0\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool writePolkitPreflightFixture(const QString &path) {
  const QByteArray script =
      "#!/bin/sh\n"
      "printf 'result=%s\\n' \"${SKYAPO_UI_TEST_POLKIT_RESULT:-0}\" >> "
      "\"$SKYAPO_UI_TEST_POLKIT_LOG\"\n"
      "printf 'arg=%s\\n' \"$@\" >> \"$SKYAPO_UI_TEST_POLKIT_LOG\"\n"
      "exit \"${SKYAPO_UI_TEST_POLKIT_RESULT:-0}\"\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool writeDeviceVerificationCliFixture(const QString &path) {
  const QByteArray script =
      "#!/bin/sh\n"
      "printf '%s\\n' \"$*\" >> \"$SKYAPO_UI_TEST_VERIFY_LOG\"\n"
      "case \"$1:$2:$3\" in\n"
      "  status::)\n"
      "    case \"$SKYAPO_UI_TEST_VERIFY_SCENARIO\" in\n"
      "      stopped)\n"
      "        printf '%s\\n' 'Daemon: stopped' 'DSP state: stopped' "
      "'Selected device: fixture.capture' 'Capture node: 41 (Built-in "
      "capture)' 'Virtual microphone: not active' 'Virtual node: "
      "4294967295 (unavailable)' 'Filters: 1' 'Active capture links: 0/2' "
      "'Playback node: SkyAPO Playback (100)' 'Playback target: fixture "
      "(USB DAC) (99)' 'Playback graph: unconnected / unconnected, internal "
      "links 0/2, target links 0/2 channels'\n"
      "        printf 'Config: %s\\n' \"$SKYAPO_UI_TEST_CONFIG_PATH\"\n"
      "        exit 0 ;;\n"
      "      initializing)\n"
      "        printf '%s\\n' 'Daemon: streaming' 'DSP state: initializing "
      "(awaiting negotiated format)' 'Selected device: fixture.capture' "
      "'Capture node: 41 (Built-in capture)' 'Virtual microphone: "
      "SkyAPO Virtual Mic' 'Virtual node: 4294967295 (pending)' 'Filters: 1' "
      "'Active capture links: 0/2' 'Playback node: SkyAPO Playback (100)' "
      "'Playback target: fixture (USB DAC) (99)' 'Playback graph: connecting "
      "/ connecting, internal links 0/2, target links 0/2 channels'\n"
      "        printf 'Config: %s\\n' \"$SKYAPO_UI_TEST_CONFIG_PATH\"\n"
      "        exit 0 ;;\n"
      "      disconnected)\n"
      "        printf '%s\\n' 'Daemon: streaming' 'DSP state: running' "
      "'Selected device: fixture.capture' 'Capture node: unavailable' "
      "'Virtual microphone: SkyAPO Virtual Mic' 'Virtual node: "
      "4294967295 (disconnected)' 'Filters: 1' 'Active capture links: 0/2' "
      "'Playback node: SkyAPO Playback (100)' 'Playback target: fixture "
      "(USB DAC) (99)' 'Playback graph: streaming / unconnected, internal "
      "links 1/2, target links 0/2 channels'\n"
      "        printf 'Config: %s\\n' \"$SKYAPO_UI_TEST_CONFIG_PATH\"\n"
      "        exit 0 ;;\n"
      "      unknown)\n"
      "        printf '%s\\n' 'Daemon: unknown' 'DSP state: unknown'\n"
      "        exit 0 ;;\n"
      "      malformed-node)\n"
      "        printf '%s\\n' 'Daemon: streaming' 'DSP state: running' "
      "'Selected device: fixture.capture' 'Capture node: 41 (Built-in "
      "capture)' 'Virtual microphone: SkyAPO Virtual Mic' 'Virtual node: "
      "not-a-pipewire-node' 'Filters: 1' 'Active capture links: 2/2' "
      "'Playback node: SkyAPO Playback (100)' 'Playback target: fixture "
      "(USB DAC) (99)' 'Playback graph: streaming / streaming, internal "
      "links 2/2, target links 2/2 channels'\n"
      "        printf 'Config: %s\\n' \"$SKYAPO_UI_TEST_CONFIG_PATH\"\n"
      "        exit 0 ;;\n"
      "    esac\n"
      "    if [ \"$SKYAPO_UI_TEST_VERIFY_BROKEN\" = 1 ]; then\n"
      "      printf '%s\\n' 'Daemon: streaming' 'DSP state: paused (faulted)' "
      "'Selected device: fixture.capture' 'Capture node: 41 (Built-in "
      "capture)' 'Virtual microphone: SkyAPO Virtual Mic' 'Virtual node: "
      "4294967295 "
      "(skyapo.virtual_mic)' 'Filters: 1' 'Active capture links: 1/2' "
      "'Playback node: SkyAPO Playback (100)' 'Playback target: fixture (USB "
      "DAC) "
      "(99)' 'Playback graph: streaming / streaming, internal links 2/2, "
      "target links 0/2 channels' 'Config: /fixture/config.txt'\n"
      "      exit 0\n"
      "    fi\n"
      "    printf '%s\\n' 'Daemon: streaming' 'DSP state: running' 'Selected "
      "device: fixture.capture' 'Capture node: 41 (Built-in capture)' 'Virtual "
      "microphone: SkyAPO Virtual Mic' 'Virtual node: 90 (skyapo.virtual_mic)' "
      "'Filters: 1' 'Active capture links: 2/2' 'Playback node: SkyAPO "
      "Playback (100)' 'Playback target: fixture (USB DAC) (99)' "
      "'Playback graph: "
      "streaming / streaming, internal links 2/2, target links 2/2 channels' "
      "'DSP amplitude ratio: 0.5'\n"
      "    printf 'Config: %s\\n' \"$SKYAPO_UI_TEST_CONFIG_PATH\"\n"
      "    exit 0 ;;\n"
      "  device:current:) printf '%s\\n' 'fixture.capture'; exit 0 ;;\n"
      "  device:list:)\n"
      "    printf '%b\\n' 'ID\\tNODE "
      "NAME\\tDESCRIPTION\\tSELECTED\\tCHANNELS\\tSAMPLE RATE\\tSTABLE "
      "PROPERTY\\tDEVICE BUS ID\\tALSA CARD\\tALSA PATH\\tDEFAULT' "
      "'41\\tfixture.capture\\tBuilt-in "
      "capture\\tyes\\t2\\t48000\\t\\t\\t\\t\\tno'; exit 0 ;;\n"
      "  device:output:current)\n"
      "    if [ \"$SKYAPO_UI_TEST_VERIFY_SCENARIO\" = "
      "playback-command-failure ]; then\n"
      "      echo 'fixture playback query permission denied' >&2\n"
      "      exit 7\n"
      "    fi\n"
      "    if [ \"$SKYAPO_UI_TEST_VERIFY_BROKEN\" = 1 ]; then\n"
      "      printf '%s\\n' 'fixture (USB DAC)' 'Playback target: fixture "
      "(USB DAC) "
      "(99)' 'Playback graph: streaming / streaming, internal links 2/2, "
      "target links 0/2 channels'; exit 0\n"
      "    fi\n"
      "    printf '%s\\n' 'fixture (USB DAC)' 'Playback target: fixture "
      "(USB DAC) (99)' "
      "'Playback graph: streaming / streaming, internal links 2/2, target "
      "links 2/2 channels'; exit 0 ;;\n"
      "  device:output:list)\n"
      "    printf '%b\\n' 'ID\\tNODE "
      "NAME\\tDESCRIPTION\\tSELECTED\\tCHANNELS\\tSAMPLE RATE\\tSTABLE "
      "PROPERTY\\tDEFAULT' '99\\tfixture (USB DAC)\\tBuilt-in "
      "playback\\tyes\\t2\\t48000\\t\\tno'; exit 0 ;;\n"
      "  config:check:*)\n"
      "    if [ \"$SKYAPO_UI_TEST_VERIFY_BROKEN\" = 1 ]; then echo 'Invalid "
      "config'; exit 1; fi\n"
      "    echo 'Valid config: 1 filters'; exit 0 ;;\n"
      "esac\n"
      "exit 2\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool writeDaemonControlCliFixture(const QString &path) {
  const QByteArray script =
      "#!/bin/sh\n"
      "case \"$1\" in\n"
      "  status) printf '%s\\n' 'Daemon: streaming'; exit 0 ;;\n"
      "  device) printf '%s\\n' 'ID\\tNODE "
      "NAME\\tDESCRIPTION\\tSELECTED\\tCHANNELS\\tSAMPLE RATE'; exit 0 ;;\n"
      "  start|stop)\n"
      "    printf '%s\\n' \"$1\" >> \"$SKYAPO_UI_TEST_CONTROL_LOG\"\n"
      "    while [ ! -e \"$SKYAPO_UI_TEST_CONTROL_GATE_DIR/$1\" ]; do sleep "
      "0.01; done\n"
      "    exit 0 ;;\n"
      "esac\n"
      "exit 0\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool writeConfigWorkflowCliFixture(const QString &path) {
  const QByteArray script =
      "#!/bin/sh\n"
      "case \"$1\" in\n"
      "  status)\n"
      "    if [ -e \"$SKYAPO_UI_TEST_RELOAD_MARKER\" ]; then state=reloaded; "
      "else state=streaming; fi\n"
      "    printf '%s\\n' \"Daemon: $state\"; exit 0 ;;\n"
      "  device) printf '%s\\n' 'ID\\tNODE "
      "NAME\\tDESCRIPTION\\tSELECTED\\tCHANNELS\\tSAMPLE RATE'; exit 0 ;;\n"
      "  config)\n"
      "    if [ \"$2\" = check ] && [ \"$3\" = --json ]; then\n"
      "      printf '%s\\n' "
      "'{\"valid\":true,\"filter_count\":1,\"diagnostics\":[]}'\n"
      "      exit 0\n"
      "    fi\n"
      "    printf '%s\\n' \"$2 $3\" >> \"$SKYAPO_UI_TEST_CONFIG_LOG\"\n"
      "    if [ \"$2\" = check ]; then\n"
      "      cat \"$3\" >> \"$SKYAPO_UI_TEST_CONFIG_LOG\"\n"
      "      : > \"$SKYAPO_UI_TEST_CONFIG_GATE_DIR/check-ready\"\n"
      "      while [ ! -e \"$SKYAPO_UI_TEST_CONFIG_GATE_DIR/check\" ]; do "
      "sleep 0.01; done\n"
      "      case \"$3\" in *reject*) echo 'fixture config rejected' >&2; exit "
      "1 ;; esac\n"
      "      echo 'Valid config: fixture'; exit 0\n"
      "    fi\n"
      "    if [ \"$2\" = reload ]; then\n"
      "      : > \"$SKYAPO_UI_TEST_CONFIG_GATE_DIR/reload-ready\"\n"
      "      while [ ! -e \"$SKYAPO_UI_TEST_CONFIG_GATE_DIR/reload\" ]; do "
      "sleep 0.01; done\n"
      "      if [ -e \"$SKYAPO_UI_TEST_CONFIG_GATE_DIR/reload-fail\" ]; then "
      "echo 'fixture reload rejected' >&2; exit 1; fi\n"
      "      : > \"$SKYAPO_UI_TEST_RELOAD_MARKER\"\n"
      "      echo 'Config reload succeeded'; exit 0\n"
      "    fi\n"
      "    ;;\n"
      "esac\n"
      "exit 0\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool writeLivePluginCliFixture(const QString &path) {
  const QByteArray script =
      "#!/bin/sh\n"
      "case \"$1\" in\n"
      "  status) printf '%s\\n' 'Daemon: streaming'; exit 0 ;;\n"
      "  device) printf '%s\\n' 'ID\\tNODE NAME\\tDESCRIPTION\\tSELECTED'; "
      "exit 0 ;;\n"
      "  config)\n"
      "    if [ \"$2\" = check ] && [ \"$3\" = --json ]; then\n"
      "      printf '%s\\n' '{\"valid\":true,\"filter_count\":1,"
      "\"diagnostics\":[]}'; exit 0\n"
      "    fi\n"
      "    printf '%s\\n' 'Configuration is valid'; exit 0 ;;\n"
      "  plugin)\n"
      "    case \"$2\" in\n"
      "      info) printf 'Test Gain\\nURI: test.gain.plugin\\n'"
      "'gain\\tGain\\tdefault=0.25\\trange=[0, 1]\\n'"
      "'mix\\tMix\\tdefault=1\\trange=[0, 1]\\n'; exit 0 ;;\n"
      "      set) printf 'set\\t%s\\t%s\\t%s\\n' \"$3\" \"$4\" "
      "\"$5\" >> \"$SKYAPO_UI_TEST_PLUGIN_CONTROL_LOG\"\n"
      "           if [ \"$5\" = 0.66 ]; then echo 'fixture live-set error' "
      ">&2; exit 9; fi\n"
      "           echo \"Updated $3 parameter $4 to $5\"; exit 0 ;;\n"
      "      bypass) printf 'bypass\\t%s\\t%s\\n' \"$3\" \"$4\" >> "
      "\"$SKYAPO_UI_TEST_PLUGIN_CONTROL_LOG\"\n"
      "              if [ \"$4\" = off ]; then echo 'fixture bypass error' "
      " >&2; exit 8; fi\n"
      "              echo \"Updated $3 bypass to $4\"; exit 0 ;;\n"
      "    esac ;;\n"
      "esac\n"
      "exit 0\n";
  QFile file(path);
  return file.open(QIODevice::WriteOnly) &&
         file.write(script) == script.size() &&
         file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                             QFileDevice::ExeOwner);
}

bool configIsValid(const QString &cli, const QString &path,
                   QString *diagnostic = nullptr) {
  QProcess process;
  process.start(cli, {QStringLiteral("config"), QStringLiteral("check"), path});
  if (!process.waitForStarted() || !process.waitForFinished(10000))
    return false;
  if (diagnostic)
    *diagnostic = QString::fromUtf8(process.readAllStandardError());
  return process.exitStatus() == QProcess::NormalExit &&
         process.exitCode() == 0;
}

bool waitForValidation(QLabel *label, const QString &prefix,
                       int timeoutMilliseconds = 5000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (elapsed.elapsed() < timeoutMilliseconds) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (label && label->text().startsWith(prefix))
      return true;
    QThread::msleep(5);
  }
  return label && label->text().startsWith(prefix);
}

int runDeviceSelectorPolkitPreflightTest(const QString &cliPath) {
  QTemporaryDir temporary;
  if (!temporary.isValid()) {
    std::cerr << "could not create isolated Polkit test directory\n";
    return 1;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     temporary.path());
  const QString configPath = temporary.filePath("config.txt");
  const QString cliFixturePath = temporary.filePath("skyapo-fixture");
  const QString fixturePath = temporary.filePath("pkcheck-fixture");
  const QString logPath = temporary.filePath("pkcheck.log");
  const QString verificationLogPath =
      temporary.filePath("verification-cli.log");
  QFile config(configPath);
  if (!config.open(QIODevice::WriteOnly) ||
      config.write("Preamp: 0 dB\n") < 0 ||
      !writePolkitPreflightFixture(fixturePath) ||
      !writeDeviceVerificationCliFixture(cliFixturePath)) {
    std::cerr << "could not create Polkit UI test fixtures\n";
    return 1;
  }
  config.close();
  qputenv("SKYAPO_UI_TEST_POLKIT_LOG", logPath.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_POLKIT_RESULT", "1");
  qputenv("SKYAPO_UI_TEST_VERIFY_LOG", verificationLogPath.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_CONFIG_PATH", configPath.toLocal8Bit());

  Q_UNUSED(cliPath);
  MainWindow window(configPath, cliFixturePath, fixturePath);
  auto *editablePreamp = window.findChild<PreampFilterGUI *>();
  auto *editableGain =
      editablePreamp
          ? editablePreamp->findChild<QDoubleSpinBox *>("doubleSpinBox")
          : nullptr;
  if (!editableGain) {
    std::cerr << "config editor Preamp control is missing from the fixture\n";
    return 1;
  }
  editableGain->setValue(-3.0);
  window.show();
  auto *openSelector = window.findChild<QPushButton *>("openDeviceSelector");
  if (!openSelector) {
    std::cerr << "Device Selector action is missing\n";
    return 1;
  }

  bool denialShown = false;
  QTimer denialProbe;
  denialProbe.setInterval(10);
  QObject::connect(&denialProbe, &QTimer::timeout, &window, [&] {
    auto *warning =
        window.findChild<QMessageBox *>("deviceSelectorAuthorizationError");
    if (!warning || !warning->isVisible())
      return;
    if (!warning->text().contains("not opened", Qt::CaseInsensitive))
      return;
    denialShown = true;
    warning->done(QMessageBox::Ok);
  });
  denialProbe.start();
  openSelector->click();
  QElapsedTimer denialWait;
  denialWait.start();
  while (!denialShown && denialWait.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  denialProbe.stop();
  const bool selectorUnexpectedlyOpened =
      window.findChild<QDialog *>("deviceSelectorDialog") != nullptr;
  QFile deniedLog(logPath);
  const bool deniedLogOpened = deniedLog.open(QIODevice::ReadOnly);
  const QByteArray denialOutput =
      deniedLogOpened ? deniedLog.readAll() : QByteArray{};
  if (!denialShown || selectorUnexpectedlyOpened || !deniedLogOpened ||
      !denialOutput.contains("result=1")) {
    std::cerr << "denied preflight state: dialog=" << denialShown
              << ", selector=" << selectorUnexpectedlyOpened
              << ", log=" << deniedLogOpened
              << ", result=" << denialOutput.toStdString() << '\n';
    return 1;
  }
  const QByteArray deniedArguments = denialOutput;
  const QByteArray processId =
      QByteArray::number(QCoreApplication::applicationPid());
  if (!deniedArguments.contains("result=1") ||
      !deniedArguments.contains("--action-id") ||
      !deniedArguments.contains("org.linuxapo.device-selector.open") ||
      !deniedArguments.contains("--process") ||
      !deniedArguments.contains(processId) ||
      !deniedArguments.contains("--allow-user-interaction")) {
    std::cerr << "Polkit preflight arguments were not narrowly scoped\n";
    return 1;
  }

  qputenv("SKYAPO_UI_TEST_POLKIT_RESULT", "0");
  bool selectorOpened = false;
  QString successFailure;
  int authorizedStep = 0;
  QElapsedTimer verificationWait;
  QTimer successProbe;
  successProbe.setInterval(10);
  QObject::connect(&successProbe, &QTimer::timeout, &window, [&] {
    auto *warning =
        window.findChild<QMessageBox *>("deviceSelectorAuthorizationError");
    if (warning && warning->isVisible()) {
      successFailure = warning->text();
      warning->done(QMessageBox::Ok);
      return;
    }
    auto *selector = window.findChild<QDialog *>("deviceSelectorDialog");
    if (!selector)
      return;
    if (authorizedStep == 0) {
      auto *status =
          selector->findChild<QLabel *>("deviceSelectorRuntimeStatus");
      auto *verify = selector->findChild<QPushButton *>("verifyAudioPath");
      if (!status || !status->text().contains("Daemon: streaming"))
        return;
      if (!verify) {
        successFailure = QStringLiteral("Device Selector does not expose the "
                                        "audio-path verification action");
        selector->reject();
        return;
      }
      verify->click();
      authorizedStep = 1;
      verificationWait.start();
      return;
    }
    auto *report = window.findChild<QDialog *>("deviceVerificationDialog");
    if (!report) {
      if (verificationWait.elapsed() > 8000) {
        successFailure = QStringLiteral(
            "verification dialog did not open after clicking its action");
        selector->reject();
      }
      return;
    }
    auto *notice = report->findChild<QLabel *>("deviceVerificationNotice");
    if (!notice ||
        !notice->text().contains("unsaved editor changes are not included",
                                 Qt::CaseInsensitive)) {
      successFailure = QStringLiteral(
          "verification did not disclose that unsaved editor changes are "
          "excluded from the saved-config check");
      report->reject();
      selector->reject();
      return;
    }
    auto *results =
        report->findChild<QTableWidget *>("deviceVerificationResults");
    auto *summary = report->findChild<QLabel *>("deviceVerificationSummary");
    if (!results || !summary || results->rowCount() != 7) {
      if (verificationWait.elapsed() > 8000) {
        successFailure = QStringLiteral(
            "verification report is missing its result table or summary");
        report->reject();
        selector->reject();
      }
      return;
    }
    if (!summary->text().contains("graph checks passed", Qt::CaseInsensitive)) {
      if (verificationWait.elapsed() > 8000) {
        QStringList observed;
        for (int row = 0; row < results->rowCount(); ++row)
          observed << QStringLiteral("%1=%2 (%3)")
                          .arg(results->item(row, 0)
                                   ? results->item(row, 0)->text()
                                   : QStringLiteral("row %1").arg(row),
                               results->item(row, 1)
                                   ? results->item(row, 1)->text()
                                   : QStringLiteral("missing"),
                               results->item(row, 2)
                                   ? results->item(row, 2)->text()
                                   : QStringLiteral("no details"));
        successFailure =
            QStringLiteral("graph verification did not pass: %1; %2")
                .arg(summary->text(), observed.join(" | "));
        report->reject();
        selector->reject();
      }
      return;
    }
    const auto rowStatus = [results](int row) {
      return results->item(row, 1) ? results->item(row, 1)->text() : QString{};
    };
    if (rowStatus(0) != QStringLiteral("PASS") ||
        rowStatus(1) != QStringLiteral("PASS") ||
        rowStatus(2) != QStringLiteral("PASS") ||
        rowStatus(3) != QStringLiteral("PASS") ||
        rowStatus(4) != QStringLiteral("PASS") ||
        rowStatus(5) != QStringLiteral("NOT TESTED") ||
        rowStatus(6) != QStringLiteral("NOT TESTED") ||
        !summary->text().contains("no test signal", Qt::CaseInsensitive)) {
      successFailure = QStringLiteral(
          "verification report misrepresented graph, DSP or hardware status");
      report->reject();
      selector->reject();
      return;
    }
    selectorOpened = true;
    report->accept();
    auto *close = selector->findChild<QPushButton *>("closeDeviceSelector");
    if (close)
      close->click();
    else
      selector->reject();
  });
  successProbe.start();
  openSelector->click();
  QElapsedTimer successWait;
  successWait.start();
  while (!selectorOpened && successFailure.isEmpty() &&
         successWait.elapsed() < 10000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  successProbe.stop();
  QFile authorizedLog(logPath);
  if (!selectorOpened || !successFailure.isEmpty() ||
      !authorizedLog.open(QIODevice::ReadOnly) ||
      !authorizedLog.readAll().contains("result=0")) {
    std::cerr << "authorized preflight did not open Device Selector";
    if (!successFailure.isEmpty())
      std::cerr << ": " << successFailure.toStdString();
    std::cerr << '\n';
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_VERIFY_BROKEN", "1");
  qunsetenv("SKYAPO_UI_TEST_VERIFY_SCENARIO");
  bool failedPathVerified = false;
  QString failedPathError;
  int failedPathStep = 0;
  QElapsedTimer failedPathWait;
  QTimer failedPathProbe;
  failedPathProbe.setInterval(10);
  QObject::connect(&failedPathProbe, &QTimer::timeout, &window, [&] {
    auto *selector = window.findChild<QDialog *>("deviceSelectorDialog");
    if (!selector)
      return;
    if (failedPathStep == 0) {
      auto *verify = selector->findChild<QPushButton *>("verifyAudioPath");
      if (!verify)
        return;
      verify->click();
      failedPathStep = 1;
      failedPathWait.start();
      return;
    }
    auto *report = window.findChild<QDialog *>("deviceVerificationDialog");
    if (report) {
      auto *results =
          report->findChild<QTableWidget *>("deviceVerificationResults");
      auto *summary = report->findChild<QLabel *>("deviceVerificationSummary");
      if (failedPathStep == 2) {
        if (results && summary && results->rowCount() == 7 &&
            results->item(4, 1) &&
            results->item(4, 1)->text() == QStringLiteral("UNKNOWN") &&
            results->item(4, 2) &&
            results->item(4, 2)->text().contains(
                "fixture playback query permission denied",
                Qt::CaseInsensitive)) {
          failedPathVerified = true;
        } else if (failedPathWait.elapsed() > 8000) {
          failedPathError = QStringLiteral(
              "playback query failure was not shown with actionable command "
              "details");
        }
        if (!failedPathVerified && failedPathError.isEmpty())
          return;
        report->reject();
        if (auto *close =
                selector->findChild<QPushButton *>("closeDeviceSelector"))
          close->click();
        else
          selector->reject();
        return;
      }
      if (results && summary &&
          summary->text().contains("graph checks failed",
                                   Qt::CaseInsensitive)) {
        const auto rowStatus = [results](int row) {
          return results->item(row, 1) ? results->item(row, 1)->text()
                                       : QString{};
        };
        if (results->rowCount() == 7 &&
            rowStatus(0) == QStringLiteral("FAIL") &&
            rowStatus(1) == QStringLiteral("FAIL") &&
            rowStatus(2) == QStringLiteral("FAIL") &&
            rowStatus(3) == QStringLiteral("FAIL") &&
            rowStatus(4) == QStringLiteral("FAIL") &&
            rowStatus(5) == QStringLiteral("NOT TESTED") &&
            rowStatus(6) == QStringLiteral("NOT TESTED") &&
            results->item(0, 2) &&
            results->item(0, 2)->text().contains(
                "does not match the config loaded by skyapod",
                Qt::CaseInsensitive)) {
          failedPathStep = 2;
          qputenv("SKYAPO_UI_TEST_VERIFY_BROKEN", "0");
          qputenv("SKYAPO_UI_TEST_VERIFY_SCENARIO", "playback-command-failure");
          report->reject();
          if (auto *verify =
                  selector->findChild<QPushButton *>("verifyAudioPath")) {
            failedPathWait.restart();
            verify->click();
            return;
          }
          failedPathError = QStringLiteral(
              "Device Selector does not expose retry verification");
        } else {
          failedPathError = QStringLiteral(
              "failed graph state was misreported in verification rows");
        }
        report->reject();
        auto *close = selector->findChild<QPushButton *>("closeDeviceSelector");
        if (close)
          close->click();
        else
          selector->reject();
        return;
      }
    }
    if (failedPathWait.isValid() && failedPathWait.elapsed() > 8000) {
      failedPathError = QStringLiteral(
          "verification did not report the injected failed graph state");
      if (report)
        report->reject();
      selector->reject();
    }
  });
  failedPathProbe.start();
  openSelector->click();
  QElapsedTimer failedPathDeadline;
  failedPathDeadline.start();
  while (!failedPathVerified && failedPathError.isEmpty() &&
         failedPathDeadline.elapsed() < 10000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  failedPathProbe.stop();
  if (!failedPathVerified && failedPathError.isEmpty())
    failedPathError = QStringLiteral(
        "timed out waiting for the failed-graph verification flow");
  if (!failedPathVerified || !failedPathError.isEmpty()) {
    std::cerr << "failed audio-path verification case failed";
    if (!failedPathError.isEmpty())
      std::cerr << ": " << failedPathError.toStdString();
    std::cerr << '\n';
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_VERIFY_BROKEN", "0");
  const auto runVerificationScenario = [&](const QByteArray &scenario,
                                           auto validate) {
    qputenv("SKYAPO_UI_TEST_VERIFY_SCENARIO", scenario);
    bool completed = false;
    QString scenarioError;
    int step = 0;
    QElapsedTimer scenarioWait;
    QTimer scenarioProbe;
    scenarioProbe.setInterval(10);
    QObject::connect(&scenarioProbe, &QTimer::timeout, &window, [&] {
      auto *selector = window.findChild<QDialog *>("deviceSelectorDialog");
      if (!selector)
        return;
      if (step == 0) {
        auto *verify = selector->findChild<QPushButton *>("verifyAudioPath");
        if (!verify) {
          scenarioError =
              QStringLiteral("Device Selector lacks the verification action");
          selector->reject();
          return;
        }
        verify->click();
        scenarioWait.start();
        step = 1;
        return;
      }
      auto *report = window.findChild<QDialog *>("deviceVerificationDialog");
      auto *results =
          report
              ? report->findChild<QTableWidget *>("deviceVerificationResults")
              : nullptr;
      auto *summary =
          report ? report->findChild<QLabel *>("deviceVerificationSummary")
                 : nullptr;
      if (!report || !results || !summary || results->rowCount() != 7) {
        if (scenarioWait.elapsed() > 8000) {
          scenarioError =
              QStringLiteral("verification report did not finish loading");
          if (report)
            report->reject();
          selector->reject();
        }
        return;
      }
      for (int row = 0; row < results->rowCount(); ++row) {
        if (!results->item(row, 1) ||
            results->item(row, 1)->text() == QStringLiteral("CHECKING"))
          return;
      }
      if (!validate(results, summary, scenarioError) && scenarioError.isEmpty())
        scenarioError = QStringLiteral("unexpected verification result state");
      completed = scenarioError.isEmpty();
      report->reject();
      selector->reject();
    });
    scenarioProbe.start();
    openSelector->click();
    QElapsedTimer deadline;
    deadline.start();
    while (!completed && scenarioError.isEmpty() &&
           deadline.elapsed() < 10000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    scenarioProbe.stop();
    if (!completed && scenarioError.isEmpty())
      scenarioError = QStringLiteral("timed out waiting for scenario results");
    if (!completed) {
      std::cerr << "verification scenario " << scenario.toStdString()
                << " failed: " << scenarioError.toStdString() << '\n';
      return false;
    }
    return true;
  };
  const auto resultAt = [](QTableWidget *results, int row, int column) {
    const auto *item = results->item(row, column);
    return item ? item->text() : QString{};
  };
  const auto failedSummary = [](QLabel *summary) {
    return summary->text().contains("graph checks failed", Qt::CaseInsensitive);
  };
  if (!runVerificationScenario(
          "stopped",
          [=](QTableWidget *results, QLabel *summary, QString &error) {
            const bool expected =
                failedSummary(summary) &&
                resultAt(results, 1, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 1, 2).contains("stopped") &&
                resultAt(results, 2, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 2, 2).contains("0/2") &&
                resultAt(results, 4, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 5, 1) == QStringLiteral("NOT TESTED") &&
                resultAt(results, 6, 1) == QStringLiteral("NOT TESTED");
            if (!expected)
              error = QStringLiteral(
                  "stopped daemon was not distinguished from active audio");
            return expected;
          }) ||
      !runVerificationScenario(
          "initializing",
          [=](QTableWidget *results, QLabel *summary, QString &error) {
            const bool expected =
                failedSummary(summary) &&
                resultAt(results, 1, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 1, 2).contains("initializing") &&
                resultAt(results, 2, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 4, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 5, 1) == QStringLiteral("NOT TESTED") &&
                resultAt(results, 6, 1) == QStringLiteral("NOT TESTED");
            if (!expected)
              error = QStringLiteral(
                  "initializing DSP was reported as an active audio path");
            return expected;
          }) ||
      !runVerificationScenario(
          "disconnected",
          [=](QTableWidget *results, QLabel *summary, QString &error) {
            const bool expected =
                failedSummary(summary) &&
                resultAt(results, 1, 1) == QStringLiteral("PASS") &&
                resultAt(results, 2, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 2, 2).contains("0/2") &&
                resultAt(results, 3, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 4, 1) == QStringLiteral("FAIL") &&
                resultAt(results, 4, 2).contains("unconnected") &&
                resultAt(results, 5, 1) == QStringLiteral("NOT TESTED") &&
                resultAt(results, 6, 1) == QStringLiteral("NOT TESTED");
            if (!expected)
              error = QStringLiteral(
                  "disconnected routes were not identified independently");
            return expected;
          }) ||
      !runVerificationScenario(
          "unknown",
          [=](QTableWidget *results, QLabel *summary, QString &error) {
            const bool expected =
                failedSummary(summary) &&
                resultAt(results, 0, 1) == QStringLiteral("UNKNOWN") &&
                resultAt(results, 1, 1) == QStringLiteral("UNKNOWN") &&
                resultAt(results, 1, 2).contains("unknown") &&
                resultAt(results, 2, 1) == QStringLiteral("UNKNOWN") &&
                resultAt(results, 2, 2).contains("unknown") &&
                resultAt(results, 3, 1) == QStringLiteral("UNKNOWN") &&
                resultAt(results, 4, 1) == QStringLiteral("UNKNOWN") &&
                resultAt(results, 4, 2).contains("unavailable") &&
                resultAt(results, 5, 1) == QStringLiteral("NOT TESTED") &&
                resultAt(results, 6, 1) == QStringLiteral("NOT TESTED");
            if (!expected)
              error = QStringLiteral(
                  "unknown runtime data was presented as verified");
            return expected;
          }) ||
      !runVerificationScenario(
          "malformed-node",
          [=](QTableWidget *results, QLabel *summary, QString &error) {
            const bool expected =
                failedSummary(summary) &&
                resultAt(results, 0, 1) == QStringLiteral("PASS") &&
                resultAt(results, 1, 1) == QStringLiteral("PASS") &&
                resultAt(results, 2, 1) == QStringLiteral("PASS") &&
                resultAt(results, 3, 1) == QStringLiteral("UNKNOWN") &&
                resultAt(results, 3, 2).contains("not-a-pipewire-node") &&
                resultAt(results, 4, 1) == QStringLiteral("PASS");
            if (!expected)
              error = QStringLiteral(
                  "malformed virtual-node identity was reported as verified "
                  "or definitely absent");
            return expected;
          }))
    return 1;
  QFile verificationLog(verificationLogPath);
  if (!verificationLog.open(QIODevice::ReadOnly)) {
    std::cerr << "audio-path verification did not invoke the CLI fixture\n";
    return 1;
  }
  const auto verificationCommands = verificationLog.readAll().split('\n');
  if (verificationCommands.size() < 6 ||
      std::any_of(verificationCommands.cbegin(), verificationCommands.cend(),
                  [](const QByteArray &command) {
                    return !command.isEmpty() && command != "status" &&
                           command != "device current" &&
                           command != "device list" &&
                           command != "device output current" &&
                           command != "device output list" &&
                           !command.startsWith("config check ");
                  })) {
    std::cerr << "audio-path verification issued a non-read-only CLI command\n";
    return 1;
  }
  qunsetenv("SKYAPO_UI_TEST_POLKIT_LOG");
  qunsetenv("SKYAPO_UI_TEST_POLKIT_RESULT");
  qunsetenv("SKYAPO_UI_TEST_VERIFY_LOG");
  qunsetenv("SKYAPO_UI_TEST_VERIFY_BROKEN");
  qunsetenv("SKYAPO_UI_TEST_VERIFY_SCENARIO");
  qunsetenv("SKYAPO_UI_TEST_CONFIG_PATH");
  return 0;
}
} // namespace

int main(int argc, char **argv) {
  QApplication application(argc, argv);
#ifdef SKYAPO_HAVE_BRANDED_APP_ICON
  const QIcon applicationIcon(QStringLiteral(":/icons/skyapo.png"));
  if (applicationIcon.isNull() || applicationIcon.availableSizes().isEmpty()) {
    std::cerr << "application icon resource is missing or unreadable\n";
    return 1;
  }
#else
  if (!QIcon(QStringLiteral(":/icons/skyapo.png")).isNull()) {
    std::cerr << "unlicensed branded application icon was embedded\n";
    return 1;
  }
#endif
  application.setQuitOnLastWindowClosed(false);
  const QPalette initialPalette = application.palette();
  QPalette forcedPalette = initialPalette;
  forcedPalette.setColor(QPalette::Window, QColor(16, 16, 16));
  application.setPalette(forcedPalette);
  if (!GUIHelper::isDarkMode()) {
    std::cerr << "upstream GUI theme adapter did not detect a dark palette\n";
    return 1;
  }
  forcedPalette.setColor(QPalette::Window, QColor(240, 240, 240));
  application.setPalette(forcedPalette);
  if (GUIHelper::isDarkMode()) {
    std::cerr << "upstream GUI theme adapter misdetected a light palette\n";
    return 1;
  }
  application.setPalette(initialPalette);
  QCoreApplication::setOrganizationName(QStringLiteral("SkyAPOTests"));
  QCoreApplication::setApplicationName(QStringLiteral("SkyAPOUITests"));
  if (argc == 3 && QString::fromLocal8Bit(argv[1]) ==
                       QStringLiteral("--device-selector-polkit"))
    return runDeviceSelectorPolkitPreflightTest(
        QString::fromLocal8Bit(argv[2]));
  if (argc == 5 && QString::fromLocal8Bit(argv[1]) ==
                       QStringLiteral("--device-selector-live")) {
    const QString configPath = QString::fromLocal8Bit(argv[2]);
    const QString cliPath = QString::fromLocal8Bit(argv[3]);
    const QString targetName = QString::fromLocal8Bit(argv[4]);
    QTemporaryDir settingsDirectory;
    if (!settingsDirectory.isValid()) {
      std::cerr << "could not create isolated live-selector settings\n";
      return 1;
    }
    const QString polkitFixturePath =
        settingsDirectory.filePath(QStringLiteral("pkcheck-fixture"));
    if (!writePolkitPreflightFixture(polkitFixturePath)) {
      std::cerr << "could not create live-selector Polkit fixture\n";
      return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory.path());
    MainWindow window(configPath, cliPath, polkitFixturePath);
    window.show();
    auto *openSelector = window.findChild<QPushButton *>("openDeviceSelector");
    if (!openSelector) {
      std::cerr << "Configuration Editor did not expose Device Selector\n";
      return 1;
    }

    QElapsedTimer elapsed;
    elapsed.start();
    QTimer probe;
    probe.setInterval(25);
    int step = 0;
    bool passed = false;
    QString failure;
    QString captureNodeBefore;
    QElapsedTimer verificationWait;
    QObject::connect(&probe, &QTimer::timeout, &window, [&] {
      auto *selector = window.findChild<QDialog *>("deviceSelectorDialog");
      if (!selector) {
        if (elapsed.elapsed() > 15000 && !passed) {
          failure = QStringLiteral("Device Selector did not open in time");
          if (auto *modal = QApplication::activeModalWidget())
            modal->close();
          probe.stop();
        }
        return;
      }
      auto *table = selector->findChild<QTableWidget *>("playbackDevicesTable");
      auto *status =
          selector->findChild<QLabel *>("deviceSelectorRuntimeStatus");
      auto *feedback = selector->findChild<QLabel *>("deviceSelectionFeedback");
      auto *apply = selector->findChild<QPushButton *>("selectPlaybackDevice");
      auto *close = selector->findChild<QPushButton *>("closeDeviceSelector");
      if (!table || !status || !feedback || !apply || !close)
        return;
      if (elapsed.elapsed() > 30000) {
        failure = QStringLiteral("live Device Selector timed out (rows=%1, "
                                 "status=%2, feedback=%3)")
                      .arg(table->rowCount())
                      .arg(status->text())
                      .arg(feedback->text());
        selector->reject();
        probe.stop();
        return;
      }
      if (step == 0) {
        if (table->rowCount() < 2 ||
            !status->text().contains(QStringLiteral("Daemon: streaming")))
          return;
        const QRegularExpression capturePattern(
            QStringLiteral("Capture node: ([0-9]+)"));
        const auto captureMatch = capturePattern.match(status->text());
        if (!captureMatch.hasMatch()) {
          failure =
              QStringLiteral(
                  "runtime status did not expose the active capture node: %1")
                  .arg(status->text());
          selector->reject();
          probe.stop();
          return;
        }
        captureNodeBefore = captureMatch.captured(1);
        int targetRow = -1;
        for (int row = 0; row < table->rowCount(); ++row) {
          if (table->item(row, 2) &&
              table->item(row, 2)->data(Qt::UserRole).toString() ==
                  targetName) {
            targetRow = row;
            break;
          }
        }
        if (targetRow < 0) {
          failure =
              QStringLiteral("PipeWire target %1 was not listed by the UI")
                  .arg(targetName);
          selector->reject();
          probe.stop();
          return;
        }
        table->selectRow(targetRow);
        if (!apply->isEnabled())
          return;
        apply->click();
        step = 1;
        return;
      }

      if (step == 1) {
        const QRegularExpression capturePattern(
            QStringLiteral("Capture node: ([0-9]+)"));
        const auto captureMatch = capturePattern.match(status->text());
        const bool targetSelected = [&] {
          for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 2) &&
                table->item(row, 2)->data(Qt::UserRole).toString() ==
                    targetName &&
                table->item(row, 0) &&
                table->item(row, 0)->text() == QStringLiteral("Selected"))
              return true;
          return false;
        }();
        if (!feedback->text().contains(
                QStringLiteral("without restarting capture")) ||
            !status->text().contains(targetName) || !targetSelected ||
            !captureMatch.hasMatch())
          return;
        if (captureMatch.captured(1) != captureNodeBefore) {
          failure =
              QStringLiteral("playback selection changed capture node %1 to %2")
                  .arg(captureNodeBefore, captureMatch.captured(1));
          selector->reject();
          probe.stop();
          return;
        }
        auto *verify = selector->findChild<QPushButton *>("verifyAudioPath");
        if (!verify) {
          failure = QStringLiteral(
              "live Device Selector does not expose audio-path verification");
          selector->reject();
          probe.stop();
          return;
        }
        verify->click();
        verificationWait.start();
        step = 2;
        return;
      }

      auto *report = window.findChild<QDialog *>("deviceVerificationDialog");
      if (!report) {
        if (verificationWait.elapsed() > 10000) {
          failure = QStringLiteral(
              "audio-path verification dialog did not open in live graph test");
          selector->reject();
          probe.stop();
        }
        return;
      }
      auto *results =
          report->findChild<QTableWidget *>("deviceVerificationResults");
      auto *summary = report->findChild<QLabel *>("deviceVerificationSummary");
      if (!results || !summary || results->rowCount() != 7) {
        if (verificationWait.elapsed() > 10000) {
          failure = QStringLiteral(
              "live audio-path verification result table is incomplete");
          report->reject();
          selector->reject();
          probe.stop();
        }
        return;
      }
      const auto rowStatus = [results](int row) {
        return results->item(row, 1) ? results->item(row, 1)->text()
                                     : QString{};
      };
      if (!summary->text().contains(QStringLiteral("graph checks passed"),
                                    Qt::CaseInsensitive)) {
        if (verificationWait.elapsed() > 10000) {
          QStringList observed;
          for (int row = 0; row < results->rowCount(); ++row)
            observed << QStringLiteral("%1=%2 (%3)")
                            .arg(results->item(row, 0)
                                     ? results->item(row, 0)->text()
                                     : QStringLiteral("row %1").arg(row),
                                 rowStatus(row),
                                 results->item(row, 2)
                                     ? results->item(row, 2)->text()
                                     : QStringLiteral("no details"));
          failure =
              QStringLiteral("live graph verification did not pass: %1; %2")
                  .arg(summary->text(), observed.join(" | "));
          report->reject();
          selector->reject();
          probe.stop();
        }
        return;
      }
      if (rowStatus(0) != QStringLiteral("PASS") ||
          rowStatus(1) != QStringLiteral("PASS") ||
          rowStatus(2) != QStringLiteral("PASS") ||
          rowStatus(3) != QStringLiteral("PASS") ||
          rowStatus(4) != QStringLiteral("PASS") ||
          rowStatus(5) != QStringLiteral("NOT TESTED") ||
          rowStatus(6) != QStringLiteral("NOT TESTED") ||
          !summary->text().contains(QStringLiteral("no test signal"),
                                    Qt::CaseInsensitive)) {
        failure = QStringLiteral(
            "live verification misreported graph, DSP, or hardware state");
        report->reject();
        selector->reject();
        probe.stop();
        return;
      }
      report->accept();
      passed = true;
      close->click();
      probe.stop();
    });
    probe.start();
    openSelector->click();
    QElapsedTimer waitForSelector;
    waitForSelector.start();
    while (!passed && failure.isEmpty() && waitForSelector.elapsed() < 32000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    probe.stop();
    if (!passed) {
      std::cerr
          << (failure.isEmpty()
                  ? "Device Selector did not apply the live PipeWire target"
                  : failure.toStdString())
          << '\n';
      return 1;
    }
    std::cout << "Live Device Selector selected " << targetName.toStdString()
              << " through skyapo, retained capture node "
              << captureNodeBefore.toStdString()
              << ", and reported an in-place "
                 "playback target switch. Audio-path verification reported "
                 "5/5 graph checks passed; numerical DSP and physical hardware "
                 "were correctly marked NOT TESTED.\n";
    return 0;
  }
  ConfigFile file;
  const QByteArray original =
      "Preamp: -6 dB\r\n; keep this comment exactly\r\nInclude: child.txt";
  file.load(original);
  if (file.lineCount() != 3 || file.serialize() != original) {
    std::cerr << "config document did not preserve source bytes\n";
    return 1;
  }
  file.replace(0, "Preamp: -3 dB");
  const QByteArray expected =
      "Preamp: -3 dB\r\n; keep this comment exactly\r\nInclude: child.txt";
  if (file.serialize() != expected) {
    std::cerr << "editing one line changed untouched source bytes\n";
    return 1;
  }
  file.insert(3, "Filter: ON PK Fc 100 Hz Gain 2 dB Q 1");
  const QByteArray appended = file.serialize();
  if (!appended.endsWith("Filter: ON PK Fc 100 Hz Gain 2 dB Q 1") ||
      appended.count("\r\n") != 3) {
    std::cerr << "appended line did not preserve the document newline style\n";
    return 1;
  }
  file.remove(1);
  if (file.lineCount() != 3 ||
      file.serialize().contains("keep this comment exactly")) {
    std::cerr << "line removal did not update the model\n";
    return 1;
  }
  ConfigFile moved;
  const QByteArray beforeMove = "one\r\ntwo\r\nthree\r\n";
  moved.load(beforeMove);
  moved.move(0, 2);
  if (moved.serialize() != "two\r\nthree\r\none\r\n") {
    std::cerr << "moving a config line did not preserve CRLF endings\n";
    return 1;
  }
  const auto movedBytes = moved.serialize();
  moved.replace(-1, "invalid");
  moved.replace(moved.lineCount(), "invalid");
  moved.remove(-1);
  moved.remove(moved.lineCount());
  moved.insert(-1, "invalid");
  if (moved.serialize() != movedBytes) {
    std::cerr << "out-of-range config edits changed the document\n";
    return 1;
  }

  PreampFilterGUIFactory preampFactory;
  QString command = "Preamp";
  QString parameters = "-6 dB";
  auto *preamp = preampFactory.createFilterGUI(command, parameters);
  if (!preamp) {
    std::cerr << "upstream Preamp editor did not recognize its config line\n";
    return 1;
  }
  auto *gain = preamp->findChild<QDoubleSpinBox *>("doubleSpinBox");
  if (!gain) {
    std::cerr << "upstream Preamp control widget is missing\n";
    delete preamp;
    return 1;
  }
  gain->setValue(-12.0);
  QString storedCommand, storedParameters;
  preamp->store(storedCommand, storedParameters);
  delete preamp;
  if (storedCommand != "Preamp" || !storedParameters.contains("-12")) {
    std::cerr << "upstream Preamp editor failed to serialize its control\n";
    return 1;
  }

  BiQuadFilterGUIFactory biquadFactory;
  command = "Filter";
  parameters = "ON PK Fc 100 Hz Gain 0 dB Q 1";
  auto *biquad = biquadFactory.createFilterGUI(command, parameters);
  if (!biquad) {
    std::cerr << "upstream BiQuad editor did not recognize its config line\n";
    return 1;
  }
  auto *frequency = biquad->findChild<QDoubleSpinBox *>("freqSpinBox");
  auto *eqGain = biquad->findChild<QDoubleSpinBox *>("gainSpinBox");
  if (!frequency || !eqGain) {
    std::cerr << "upstream BiQuad controls are missing\n";
    delete biquad;
    return 1;
  }
  frequency->setValue(250.0);
  eqGain->setValue(4.0);
  biquad->store(storedCommand, storedParameters);
  delete biquad;
  if (storedCommand != "Filter" || !storedParameters.contains("250") ||
      !storedParameters.contains("4")) {
    std::cerr << "upstream BiQuad editor failed to serialize its controls\n";
    return 1;
  }

  DelayFilterGUIFactory delayFactory;
  command = "Delay";
  parameters = "4 ms";
  auto *delay = delayFactory.createFilterGUI(command, parameters);
  if (!delay) {
    std::cerr << "upstream Delay editor did not recognize its config line\n";
    return 1;
  }
  auto *delayValue = delay->findChild<QDoubleSpinBox *>("delaySpinBox");
  auto *delayUnit = delay->findChild<QComboBox *>("unitComboBox");
  if (!delayValue || !delayUnit) {
    std::cerr << "upstream Delay controls are missing\n";
    delete delay;
    return 1;
  }
  delayValue->setValue(25.5);
  delay->store(storedCommand, storedParameters);
  if (storedCommand != "Delay" || !storedParameters.contains("25.5") ||
      !storedParameters.contains("ms")) {
    std::cerr << "upstream Delay editor failed to serialize milliseconds\n";
    delete delay;
    return 1;
  }
  delayUnit->setCurrentIndex(1);
  delayValue->setValue(128.0);
  delay->store(storedCommand, storedParameters);
  delete delay;
  if (storedCommand != "Delay" || !storedParameters.contains("128") ||
      !storedParameters.contains("samples")) {
    std::cerr << "upstream Delay editor failed to serialize sample units\n";
    return 1;
  }

  StageFilterGUIFactory stageFactory;
  command = "Stage";
  parameters = "capture";
  auto *stage = stageFactory.createFilterGUI(command, parameters);
  if (!stage) {
    std::cerr << "upstream Stage editor did not recognize its config line\n";
    return 1;
  }
  auto *preMix = stage->findChild<QCheckBox *>("preMixCheckBox");
  auto *postMix = stage->findChild<QCheckBox *>("postMixCheckBox");
  auto *capture = stage->findChild<QCheckBox *>("captureCheckBox");
  if (!preMix || !postMix || !capture || !capture->isChecked()) {
    std::cerr << "upstream Stage controls did not load capture selection\n";
    delete stage;
    return 1;
  }
  preMix->setChecked(true);
  postMix->setChecked(true);
  stage->store(storedCommand, storedParameters);
  delete stage;
  if (storedCommand != "Stage" || !storedParameters.contains("pre-mix") ||
      !storedParameters.contains("post-mix") ||
      !storedParameters.contains("capture")) {
    std::cerr << "upstream Stage editor failed to serialize selections\n";
    return 1;
  }

  auto *channelEditor = ChannelCopyEditor::create("Channel", "L SUB");
  if (!channelEditor ||
      channelEditor->kind() != ChannelCopyEditor::Kind::Channel ||
      !channelEditor->findChild<QCheckBox *>("channel_L")->isChecked() ||
      !channelEditor->findChild<QCheckBox *>("channel_LFE")->isChecked()) {
    std::cerr
        << "Channel visual editor did not load supported EAPO names/alias\n";
    delete channelEditor;
    return 1;
  }
  channelEditor->findChild<QCheckBox *>("channel_C")->setChecked(true);
  channelEditor->store(storedCommand, storedParameters);
  delete channelEditor;
  if (storedCommand != "Channel" || storedParameters != "L C LFE") {
    std::cerr << "Channel visual editor did not serialize edited selections\n";
    return 1;
  }
  auto *allChannels = ChannelCopyEditor::create("Channel", "ALL");
  if (!allChannels ||
      !allChannels->findChild<QCheckBox *>("channelSelectAll")->isChecked()) {
    std::cerr << "Channel visual editor did not load ALL selection\n";
    delete allChannels;
    return 1;
  }
  allChannels->store(storedCommand, storedParameters);
  delete allChannels;
  if (storedParameters != "ALL" ||
      ChannelCopyEditor::create("Channel", "unknown") != nullptr) {
    std::cerr << "Channel editor accepted unsupported semantics\n";
    return 1;
  }

  auto *copyEditor = ChannelCopyEditor::create("Copy", "L=0.5*R R=L");
  if (!copyEditor || copyEditor->kind() != ChannelCopyEditor::Kind::Copy ||
      copyEditor->copyTable()->rowCount() != 2) {
    std::cerr << "Copy visual editor did not load simple channel mappings\n";
    delete copyEditor;
    return 1;
  }
  auto *copyGain = copyEditor->findChild<QDoubleSpinBox *>("copyGain_0");
  auto *copySource = copyEditor->findChild<QComboBox *>("copySource_0");
  if (!copyGain || !copySource) {
    std::cerr << "Copy visual editor controls are missing\n";
    delete copyEditor;
    return 1;
  }
  copyGain->setValue(0.75);
  copyEditor->findChild<QComboBox *>("copySource_0")->setCurrentText("C");
  copyEditor->store(storedCommand, storedParameters);
  delete copyEditor;
  if (storedCommand != "Copy" || storedParameters != "L=0.75*C R=L" ||
      ChannelCopyEditor::create("Copy", "L=R+C") != nullptr ||
      ChannelCopyEditor::create("Copy", "L=unknown") != nullptr) {
    std::cerr
        << "Copy editor serialization/unsupported-expression boundary failed\n";
    return 1;
  }

  auto *iirEditor = IIRFilterEditor::create(
      "Filter 2", "ON IIR Order 2 Coefficients 0.0380602 0.0761205 "
                  "0.0380602 1.2706 -1.84776 0.729402");
  if (!iirEditor || iirEditor->orderControl()->value() != 2 ||
      iirEditor->coefficientTable()->rowCount() != 3) {
    std::cerr << "IIR visual editor did not load upstream coefficient order\n";
    delete iirEditor;
    return 1;
  }
  auto *iirNumerator = iirEditor->findChild<QLineEdit *>("iirCoefficient_b_0");
  if (!iirNumerator || iirNumerator->text() != "0.0380602") {
    std::cerr << "IIR visual editor did not expose the b0 coefficient\n";
    delete iirEditor;
    return 1;
  }
  int iirModelUpdates = 0;
  QObject::connect(iirEditor, &IFilterGUI::updateModel,
                   [&iirModelUpdates] { ++iirModelUpdates; });
  iirNumerator->setText("not-a-number");
  if (iirModelUpdates != 0) {
    std::cerr << "IIR editor propagated a non-numeric intermediate value\n";
    delete iirEditor;
    return 1;
  }
  iirNumerator->setText("0.05");
  if (iirModelUpdates != 1) {
    std::cerr << "IIR editor did not propagate a valid coefficient edit\n";
    delete iirEditor;
    return 1;
  }
  iirEditor->orderControl()->setValue(3);
  iirEditor->store(storedCommand, storedParameters);
  delete iirEditor;
  if (storedCommand != "Filter" ||
      storedParameters != "ON IIR Order 3 Coefficients 0.05 0.0761205 "
                          "0.0380602 0 1.2706 -1.84776 0.729402 0") {
    std::cerr << "IIR visual editor failed coefficient/order serialization\n";
    return 1;
  }
  if (IIRFilterEditor::create("Filter", "ON IIR Order 1 Coefficients 1 0 1") ||
      IIRFilterEditor::create("Filter",
                              "ON IIR Order 1 Coefficients 1 0 1 0 extra") ||
      IIRFilterEditor::create("Filter",
                              "ON IIR Order 1 Coefficients ${b0} 0 1 0") ||
      IIRFilterEditor::create("filter",
                              "ON IIR Order 1 Coefficients 1 0 1 0") ||
      IIRFilterEditor::create("Filter",
                              "on iir Order 1 Coefficients 1 0 1 0") ||
      IIRFilterEditor::create("Filter",
                              "ON IIR Order 257 Coefficients 1 0 1 0") ||
      IIRFilterEditor::create("Copy", "ON IIR Order 1 Coefficients 1 0 1 0")) {
    std::cerr << "IIR visual editor accepted unsupported raw syntax\n";
    return 1;
  }

  if (argc != 3) {
    std::cerr << "UI integration test needs CLI fixture and skyapo paths\n";
    return 1;
  }
  QTemporaryDir temporary;
  if (!temporary.isValid()) {
    std::cerr << "could not create temporary UI test directory\n";
    return 1;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     temporary.path() + QStringLiteral("/settings"));
  QSettings isolatedSettings;
  isolatedSettings.clear();
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  qputenv("XDG_CONFIG_HOME",
          QDir(temporary.path()).filePath("xdg-config").toLocal8Bit());
  const QString defaultConfigPath =
      QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
      QStringLiteral("/skyapo/config.txt");
  if (!QDir().mkpath(QFileInfo(defaultConfigPath).absolutePath())) {
    std::cerr << "could not create isolated default config directory\n";
    return 1;
  }
  QFile defaultConfig(defaultConfigPath);
  if (!defaultConfig.open(QIODevice::WriteOnly) ||
      defaultConfig.write("Preamp: -6 dB\n") != 14) {
    std::cerr << "could not write isolated default config fixture\n";
    return 1;
  }
  defaultConfig.close();
  const auto shortResponse =
      analyzeConfigResponse(defaultConfigPath, 44100, 1, 128);
  if (!shortResponse.error.isEmpty() || shortResponse.curves.size() != 1 ||
      shortResponse.curves.front().points.size() != 64 ||
      std::abs(shortResponse.curves.front().points.front().y() + 6.0) > 0.01 ||
      !shortResponse.peakGainDb ||
      std::abs(*shortResponse.peakGainDb + 6.0) > 0.01) {
    std::cerr << "128-sample response analysis did not process a partial "
                 "Engine block or peak gain correctly: "
              << shortResponse.error.toStdString() << '\n';
    return 1;
  }
  const auto largestResponse =
      analyzeConfigResponse(defaultConfigPath, 44100, 1, 1U << 20);
  if (!largestResponse.error.isEmpty() || largestResponse.curves.size() != 1 ||
      largestResponse.curves.front().points.size() != (1U << 19) ||
      std::abs(largestResponse.curves.front().points.front().y() + 6.0) >
          0.01) {
    std::cerr << "largest advertised response FFT size failed: "
              << largestResponse.error.toStdString() << '\n';
    return 1;
  }
  const auto oversizedResponse =
      analyzeConfigResponse(defaultConfigPath, 48000, 8, 1U << 20);
  if (oversizedResponse.error.isEmpty() ||
      !oversizedResponse.error.contains("64 MiB")) {
    std::cerr << "response analysis did not reject a workspace over budget\n";
    return 1;
  }
  const QString responseConfigPath = temporary.filePath("response-config.txt");
  QFile responseConfig(responseConfigPath);
  if (!responseConfig.open(QIODevice::WriteOnly) ||
      responseConfig.write(
          "Preamp: -6 dB\nFilter: ON PK Fc 1000 Hz Gain 6 dB Q 1.0\n") < 0) {
    std::cerr << "could not create response analysis fixture\n";
    return 1;
  }
  responseConfig.close();
  const QString delayResponsePath = temporary.filePath("delay-response.txt");
  QFile delayResponseConfig(delayResponsePath);
  if (!delayResponseConfig.open(QIODevice::WriteOnly) ||
      delayResponseConfig.write("Delay: 1 Samples\n") < 0) {
    std::cerr << "could not create phase-response delay fixture\n";
    return 1;
  }
  delayResponseConfig.close();
  const auto delayResponse =
      analyzeConfigResponse(delayResponsePath, 48000, 1, 16384);
  if (!delayResponse.error.isEmpty() || delayResponse.curves.size() != 1 ||
      delayResponse.curves.front().points.size() !=
          delayResponse.curves.front().phasePoints.size()) {
    std::cerr << "response analysis did not return aligned magnitude and "
                 "phase curves: "
              << delayResponse.error.toStdString() << '\n';
    return 1;
  }
  auto delayedPhase = delayResponse.curves.front().phasePoints.cbegin();
  for (auto point = delayResponse.curves.front().phasePoints.cbegin();
       point != delayResponse.curves.front().phasePoints.cend(); ++point)
    if (std::abs(point->x() - 1000.0) < std::abs(delayedPhase->x() - 1000.0))
      delayedPhase = point;
  const double expectedDelayPhase =
      -360.0 * delayedPhase->x() / static_cast<double>(48000);
  if (!std::isfinite(delayedPhase->y()) ||
      std::abs(delayedPhase->y() - expectedDelayPhase) > 0.02) {
    std::cerr << "one-sample EAPO Delay response phase mismatch near 1 kHz: "
              << "measured " << delayedPhase->y() << " degrees, expected "
              << expectedDelayPhase << " degrees\n";
    return 1;
  }
  const QString quietResponsePath = temporary.filePath("quiet-response.txt");
  QFile quietResponseConfig(quietResponsePath);
  if (!quietResponseConfig.open(QIODevice::WriteOnly) ||
      quietResponseConfig.write("Preamp: -300 dB\n") < 0) {
    std::cerr << "could not create low-level phase response fixture\n";
    return 1;
  }
  quietResponseConfig.close();
  const auto quietResponse =
      analyzeConfigResponse(quietResponsePath, 48000, 1, 16384);
  if (!quietResponse.error.isEmpty() || quietResponse.curves.size() != 1 ||
      !std::isnan(quietResponse.curves.front().phasePoints.front().y())) {
    std::cerr << "phase below the documented response floor must be marked "
                 "undefined\n";
    return 1;
  }
  const auto response = analyzeConfigResponse(responseConfigPath);
  if (!response.error.isEmpty() || response.curves.size() != 4 ||
      !response.peakGainDb || std::abs(*response.peakGainDb) > 0.1) {
    std::cerr << "actual Engine response curves or peak-gain result failed: "
              << response.error.toStdString() << '\n';
    return 1;
  }
  const auto monoResponse =
      analyzeConfigResponse(responseConfigPath, 44100, 1, 8192);
  if (!monoResponse.error.isEmpty() || monoResponse.curves.size() != 1 ||
      monoResponse.curves.front().points.size() != 4096) {
    std::cerr << "response analysis did not honor an explicit mono/44.1 kHz "
                 "format: "
              << monoResponse.error.toStdString() << '\n';
    return 1;
  }
  const auto gainAt = [](const ResponseCurve &curve, double frequency) {
    auto closest = curve.points.constBegin();
    for (auto point = curve.points.constBegin();
         point != curve.points.constEnd(); ++point)
      if (std::abs(point->x() - frequency) < std::abs(closest->x() - frequency))
        closest = point;
    return closest->y();
  };
  for (const auto &curve : response.curves) {
    if (curve.channel != "L → L" && curve.channel != "R → R")
      continue;
    if (curve.channel == "L → L") {
      std::cout << "Calculated Engine response L→L: " << gainAt(curve, 100.0)
                << " dB near 100 Hz, " << gainAt(curve, 1000.0)
                << " dB near 1 kHz\n";
    }
    if (std::abs(gainAt(curve, 100.0) + 6.0) > 0.25 ||
        std::abs(gainAt(curve, 1000.0)) > 0.5) {
      std::cerr << "measured Engine response did not match Preamp+BiQuad: "
                << gainAt(curve, 100.0) << " dB @ 100 Hz, "
                << gainAt(curve, 1000.0) << " dB @ 1 kHz\n";
      return 1;
    }
  }
  const QString pluginResponsePath = temporary.filePath("plugin-response.txt");
  QFile pluginResponseConfig(pluginResponsePath);
  if (!pluginResponseConfig.open(QIODevice::WriteOnly) ||
      pluginResponseConfig.write("Plugin: CLAP nonexistent\n") < 0) {
    std::cerr << "could not create plugin response guard fixture\n";
    return 1;
  }
  pluginResponseConfig.close();
  const auto pluginResponse = analyzeConfigResponse(pluginResponsePath);
  if (pluginResponse.error.isEmpty()) {
    std::cerr
        << "response analysis accepted a plugin config in the editor process\n";
    return 1;
  }
#endif
  const QString liveConfigPath = temporary.filePath("live-config.txt");
  QFile liveConfig(liveConfigPath);
  if (!liveConfig.open(QIODevice::WriteOnly) ||
      liveConfig.write("Preamp: -3 dB\n") < 0) {
    std::cerr << "could not create live validation fixture\n";
    return 1;
  }
  liveConfig.close();
  MainWindow liveWindow(liveConfigPath, QString::fromLocal8Bit(argv[1]));
  auto *liveValidation = liveWindow.findChild<QLabel *>("liveConfigValidation");
  auto *livePreamp = liveWindow.findChild<PreampFilterGUI *>();
  auto *liveGain =
      livePreamp ? livePreamp->findChild<QDoubleSpinBox *>("doubleSpinBox")
                 : nullptr;
  if (!liveValidation || !liveGain ||
      !waitForValidation(liveValidation, "Valid config")) {
    std::cerr << "GUI did not asynchronously validate the opened config: "
              << (liveValidation ? liveValidation->text().toStdString()
                                 : "validation label missing")
              << '\n';
    return 1;
  }
  liveGain->setValue(-6.0);
  if (!waitForValidation(liveValidation, "Invalid config")) {
    std::cerr << "GUI did not surface the live validation diagnostic: "
              << liveValidation->text().toStdString() << '\n';
    return 1;
  }
  if (!liveValidation->text().contains(liveConfigPath + ":1") ||
      !liveValidation->text().contains("fixture rejected unsaved Preamp")) {
    std::cerr << "live validator lost its source location or reason: "
              << liveValidation->text().toStdString() << '\n';
    return 1;
  }
  auto *liveTable = liveWindow.findChild<FilterTable *>();
  auto *invalidRow = liveTable && liveTable->itemAt(0)
                         ? liveTable->itemAt(0)->row.data()
                         : nullptr;
  auto *invalidRowNumber =
      invalidRow ? invalidRow->findChild<QLabel *>("labelNumber") : nullptr;
  if (!invalidRow || !invalidRow->property("skyapoValidationError").toBool() ||
      !invalidRowNumber ||
      !invalidRowNumber->toolTip().contains(
          "fixture rejected unsaved Preamp")) {
    std::cerr << "live diagnostic was not attached to its filter row\n";
    return 1;
  }
  QFile liveConfigAfterInvalid(liveConfigPath);
  if (!liveConfigAfterInvalid.open(QIODevice::ReadOnly) ||
      liveConfigAfterInvalid.readAll() != "Preamp: -3 dB\n") {
    std::cerr << "live validation wrote unsaved edits to the config file\n";
    return 1;
  }

  liveGain->setValue(-6.0);
  QElapsedTimer inFlightEdit;
  inFlightEdit.start();
  while (inFlightEdit.elapsed() < 420) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  liveGain->setValue(-3.0);
  bool staleErrorDisplayed = false;
  QElapsedTimer latestValidation;
  latestValidation.start();
  while (latestValidation.elapsed() < 1800 &&
         !liveValidation->text().startsWith("Valid config")) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    staleErrorDisplayed |= liveValidation->text().startsWith("Invalid config");
    QThread::msleep(5);
  }
  if (!liveValidation->text().startsWith("Valid config") ||
      staleErrorDisplayed) {
    std::cerr << "stale config diagnostics replaced the latest edit: "
              << liveValidation->text().toStdString() << '\n';
    return 1;
  }
  if (invalidRow->property("skyapoValidationError").toBool() ||
      !invalidRowNumber->toolTip().isEmpty()) {
    std::cerr << "stale filter-row diagnostic remained after a valid edit\n";
    return 1;
  }
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  auto *responseButton = liveWindow.findChild<QPushButton *>("analyzeResponse");
  auto *responseSource = liveWindow.findChild<QComboBox *>("analysisSource");
  auto *responseResolution =
      liveWindow.findChild<QComboBox *>("analysisResolution");
  auto *useLiveAnalysisFormat =
      liveWindow.findChild<QCheckBox *>("analysisUseLiveFormat");
  auto *analysisSampleRate =
      liveWindow.findChild<QSpinBox *>("analysisSampleRate");
  auto *analysisChannels = liveWindow.findChild<QSpinBox *>("analysisChannels");
  bool responseDialogOpened = false;
  bool responseDialogFailed = false;
  double responseGainAt1kHz = std::numeric_limits<double>::quiet_NaN();
  double responsePhaseAt1kHz = std::numeric_limits<double>::quiet_NaN();
  qlonglong responseBinCount = -1;
  double responsePlotMaxFrequency = std::numeric_limits<double>::quiet_NaN();
  QString responseFormatLabel;
  QString responsePeakGainText;
  QString responseFailureText;
  QTimer responseDialogCloser;
  responseDialogCloser.setInterval(10);
  QObject::connect(&responseDialogCloser, &QTimer::timeout, &application, [&] {
    for (auto *widget : QApplication::topLevelWidgets()) {
      auto *dialog = qobject_cast<QDialog *>(widget);
      if (!dialog)
        continue;
      if (dialog->windowTitle() == "Calculated filter response") {
        responseDialogOpened = true;
        if (auto *plot = dialog->findChild<QWidget *>("responsePlot")) {
          responseGainAt1kHz =
              plot->property("responseAt1000HzLtoLDb").toDouble();
          responsePhaseAt1kHz =
              plot->property("responsePhaseAt1000HzLtoLDeg").toDouble();
          responseBinCount = plot->property("analysisBinCount").toLongLong();
          responsePlotMaxFrequency =
              plot->property("plotMaxFrequencyHz").toDouble();
        }
        for (auto *label : dialog->findChildren<QLabel *>())
          if (label->text().contains("FFT samples"))
            responseFormatLabel = label->text();
        if (auto *peakGain = dialog->findChild<QLabel *>("analysisPeakGain"))
          responsePeakGainText = peakGain->text();
        dialog->accept();
        responseDialogCloser.stop();
      } else if (dialog->windowTitle() == "Response analysis") {
        responseDialogFailed = true;
        for (auto *label : dialog->findChildren<QLabel *>())
          responseFailureText += label->text();
        dialog->accept();
        responseDialogCloser.stop();
      }
    }
  });
  if (!responseButton || !responseSource || !responseResolution ||
      !useLiveAnalysisFormat || !analysisSampleRate || !analysisChannels ||
      responseResolution->count() != 14 ||
      responseResolution->currentData().toUInt() != 16384U ||
      responseResolution->itemData(0).toUInt() != 128U ||
      responseResolution->itemData(responseResolution->count() - 1).toUInt() !=
          (1U << 20) ||
      responseSource->count() != 2 ||
      responseSource->itemText(0) != "config.txt" ||
      responseSource->itemText(1) != "Current file") {
    std::cerr << "response analysis source/resolution controls are missing\n";
    return 1;
  }
  if (!useLiveAnalysisFormat->isChecked() || analysisSampleRate->isEnabled() ||
      analysisChannels->isEnabled()) {
    std::cerr
        << "response analysis must default to the live negotiated format\n";
    return 1;
  }
  useLiveAnalysisFormat->setChecked(false);
  analysisSampleRate->setValue(44100);
  analysisChannels->setValue(1);
  if (analysisSampleRate->isEnabled() == false ||
      analysisChannels->isEnabled() == false) {
    std::cerr << "manual response format controls did not enable\n";
    return 1;
  }
  const int selectedResolutionIndex = responseResolution->findData(8192U);
  if (selectedResolutionIndex < 0) {
    std::cerr << "response analysis does not offer 8192-sample resolution\n";
    return 1;
  }
  responseResolution->setCurrentIndex(selectedResolutionIndex);
  QSettings savedAnalysisSettings;
  if (savedAnalysisSettings.value(QStringLiteral("analysis/resolution"))
          .toUInt() != 8192U) {
    std::cerr << "response analysis resolution was not persisted\n";
    return 1;
  }
  const auto runResponseAndCaptureGain = [&] {
    responseDialogOpened = false;
    responseDialogFailed = false;
    responseGainAt1kHz = std::numeric_limits<double>::quiet_NaN();
    responsePhaseAt1kHz = std::numeric_limits<double>::quiet_NaN();
    responseBinCount = -1;
    responsePlotMaxFrequency = std::numeric_limits<double>::quiet_NaN();
    responseFormatLabel.clear();
    responsePeakGainText.clear();
    responseDialogCloser.start();
    responseButton->click();
    QElapsedTimer responseWait;
    responseWait.start();
    while (!responseDialogOpened && !responseDialogFailed &&
           responseWait.elapsed() < 5000) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    return responseDialogOpened && !responseDialogFailed &&
           std::isfinite(responseGainAt1kHz) &&
           std::isfinite(responsePhaseAt1kHz) &&
           std::isfinite(responsePlotMaxFrequency) && responseBinCount == 4096;
  };
  responseSource->setCurrentIndex(0);
  if (!runResponseAndCaptureGain() ||
      std::abs(responseGainAt1kHz + 6.0) > 0.25 ||
      std::abs(responsePhaseAt1kHz) > 0.02 ||
      std::abs(responsePlotMaxFrequency - 22050.0) > 0.01 ||
      !responseFormatLabel.contains("44100 Hz") ||
      !responseFormatLabel.contains("1 channel") ||
      !responsePeakGainText.contains("Peak gain (calculated): -6.00 dB")) {
    std::cerr << "analysis did not use the saved default config.txt; gain="
              << responseGainAt1kHz << " dB\n";
    return 1;
  }
  responseSource->setCurrentIndex(1);
  if (!runResponseAndCaptureGain() ||
      std::abs(responseGainAt1kHz + 3.0) > 0.25 ||
      std::abs(responsePhaseAt1kHz) > 0.02 ||
      std::abs(responsePlotMaxFrequency - 22050.0) > 0.01 ||
      !responseFormatLabel.contains("44100 Hz") ||
      !responseFormatLabel.contains("1 channel") ||
      !responsePeakGainText.contains("Peak gain (calculated): -3.00 dB")) {
    std::cerr << "analysis did not use the current editor document; gain="
              << responseGainAt1kHz << " dB\n";
    return 1;
  }
  useLiveAnalysisFormat->setChecked(true);
  responseFailureText.clear();
  responseDialogFailed = false;
  responseDialogCloser.start();
  responseButton->click();
  QElapsedTimer unavailableFormatWait;
  unavailableFormatWait.start();
  while (!responseDialogFailed && unavailableFormatWait.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!responseDialogFailed ||
      !responseFailureText.contains("not streaming with a known sample rate") ||
      !responseButton->isEnabled()) {
    std::cerr << "response analysis fabricated/failed to report an unknown "
                 "live format\n";
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_STREAMING_FORMAT", "96000:2");
  if (analysisSampleRate->isEnabled() || analysisChannels->isEnabled() ||
      !runResponseAndCaptureGain() ||
      std::abs(responseGainAt1kHz + 3.0) > 0.25 ||
      std::abs(responsePhaseAt1kHz) > 0.02 ||
      !responseFormatLabel.contains("96000 Hz") ||
      !responseFormatLabel.contains("2 channels")) {
    std::cerr << "response analysis did not use the daemon's negotiated "
                 "96 kHz stereo format; label="
              << responseFormatLabel.toStdString() << '\n';
    return 1;
  }
  qunsetenv("SKYAPO_UI_TEST_STREAMING_FORMAT");
#endif
  QFile liveConfigAfterValid(liveConfigPath);
  if (!liveConfigAfterValid.open(QIODevice::ReadOnly) ||
      liveConfigAfterValid.readAll() != "Preamp: -3 dB\n") {
    std::cerr << "live validation changed the on-disk config\n";
    return 1;
  }
  const QString includeLiveConfigPath = temporary.filePath("include-live.txt");
  QFile includeLiveConfig(includeLiveConfigPath);
  QFile includeLiveChild(temporary.filePath("include-live-child.txt"));
  if (!includeLiveChild.open(QIODevice::WriteOnly) ||
      includeLiveChild.write("Preamp: 0 dB\n") < 0) {
    std::cerr << "could not create live Include validation child\n";
    return 1;
  }
  includeLiveChild.close();
  if (!includeLiveConfig.open(QIODevice::WriteOnly) ||
      includeLiveConfig.write(
          "Preamp: -3 dB\nInclude: include-live-child.txt\n") < 0) {
    std::cerr << "could not create live Include validation root\n";
    return 1;
  }
  includeLiveConfig.close();
  MainWindow includeLiveWindow(includeLiveConfigPath,
                               QString::fromLocal8Bit(argv[2]));
  auto *includeLiveValidation =
      includeLiveWindow.findChild<QLabel *>("liveConfigValidation");
  auto *includeLiveEditor = includeLiveWindow.findChild<IncludeEditor *>();
  auto *includeLivePath =
      includeLiveEditor
          ? includeLiveEditor->findChild<QLineEdit *>("includePathEdit")
          : nullptr;
  if (!includeLiveValidation || !includeLivePath ||
      !waitForValidation(includeLiveValidation, "Valid config")) {
    std::cerr << "live validation did not resolve the relative Include: "
              << (includeLiveValidation
                      ? includeLiveValidation->text().toStdString()
                      : "validation UI missing")
              << '\n';
    return 1;
  }
  includeLivePath->setText("missing-live-child.txt");
  QMetaObject::invokeMethod(includeLivePath, "editingFinished",
                            Qt::DirectConnection);
  if (!waitForValidation(includeLiveValidation, "Invalid config") ||
      !includeLiveValidation->text().contains("cannot open config")) {
    std::cerr << "live validation did not report the missing relative Include: "
              << includeLiveValidation->text().toStdString() << '\n';
    return 1;
  }
  QFile includeLiveConfigAfterCheck(includeLiveConfigPath);
  if (!includeLiveConfigAfterCheck.open(QIODevice::ReadOnly) ||
      includeLiveConfigAfterCheck.readAll() !=
          "Preamp: -3 dB\nInclude: include-live-child.txt\n") {
    std::cerr << "live Include validation wrote unsaved edits to disk\n";
    return 1;
  }

  const QString navigationRootPath = temporary.filePath("include-root.txt");
  const QString navigationChildPath = temporary.filePath("child.txt");
  QFile navigationChild(navigationChildPath);
  if (!navigationChild.open(QIODevice::WriteOnly) ||
      navigationChild.write(
          "# included child\nFilter: ON PK Fc 30000 Hz Gain 6 dB Q 1\n") < 0) {
    std::cerr << "could not create Include navigation child fixture\n";
    return 1;
  }
  navigationChild.close();
  QFile navigationRoot(navigationRootPath);
  if (!navigationRoot.open(QIODevice::WriteOnly) ||
      navigationRoot.write("Preamp: -3 dB\nInclude: child.txt\n") < 0) {
    std::cerr << "could not create Include navigation root fixture\n";
    return 1;
  }
  navigationRoot.close();
  MainWindow navigationWindow(navigationRootPath,
                              QString::fromLocal8Bit(argv[2]));
  if (!QFile::exists(QStringLiteral(":/icons/list-add-green.ico")) ||
      !QFile::exists(QStringLiteral(":/icons/list-remove-red.ico")) ||
      QFile::exists(QStringLiteral(":/sounds/pinkNoise.flac")) ||
      QFile::exists(QStringLiteral(":/translations/qtbase_de.qm"))) {
    std::cerr << "UI embedded missing required or retained unused resources\n";
    return 1;
  }
  auto *aboutAction = navigationWindow.findChild<QAction *>("aboutSkyAPO");
  QString aboutText;
  if (!aboutAction) {
    std::cerr << "About/licensing action is missing from the UI\n";
    return 1;
  }
  QTimer::singleShot(0, &application, [&] {
    for (auto *widget : QApplication::topLevelWidgets()) {
      auto *about = qobject_cast<QMessageBox *>(widget);
      if (about && about->windowTitle() == "About SkyAPO") {
        aboutText = about->text();
        about->accept();
      }
    }
  });
  aboutAction->trigger();
  if (!aboutText.contains("Qt") || !aboutText.contains("LGPL-3.0") ||
      !aboutText.contains("GPL-2.0-or-later")) {
    std::cerr << "About dialog omitted Qt or SkyAPO license information\n";
    return 1;
  }
  auto *navigationValidation =
      navigationWindow.findChild<QLabel *>("liveConfigValidation");
  if (!navigationValidation ||
      !waitForValidation(navigationValidation, "Invalid config") ||
      !navigationValidation->text().contains(navigationChildPath + ":2")) {
    std::cerr << "included-file diagnostic lost its real source location: "
              << (navigationValidation
                      ? navigationValidation->text().toStdString()
                      : "validation label missing")
              << '\n';
    return 1;
  }
  auto *navigationTable = navigationWindow.findChild<FilterTable *>();
  auto *includeRow = navigationTable && navigationTable->itemAt(1)
                         ? navigationTable->itemAt(1)->row.data()
                         : nullptr;
  auto *openDiagnostic =
      navigationWindow.findChild<QPushButton *>("openDiagnosticSource");
  if (!includeRow || !includeRow->property("skyapoValidationError").toBool() ||
      !includeRow->toolTip().contains(navigationChildPath + ":2") ||
      !openDiagnostic || openDiagnostic->isHidden()) {
    std::cerr
        << "included-file diagnostic was not mapped to its root Include row\n";
    return 1;
  }
  openDiagnostic->click();
  MainWindow *childWindow = nullptr;
  QElapsedTimer navigationWait;
  navigationWait.start();
  while (navigationWait.elapsed() < 2000 && !childWindow) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    for (auto *widget : QApplication::topLevelWidgets()) {
      auto *candidate = qobject_cast<MainWindow *>(widget);
      auto *path =
          candidate ? candidate->findChild<QLineEdit *>("configPath") : nullptr;
      if (path && path->text() == navigationChildPath) {
        childWindow = candidate;
        break;
      }
    }
    QThread::msleep(5);
  }
  auto *childTable =
      childWindow ? childWindow->findChild<FilterTable *>() : nullptr;
  if (!childTable || !childTable->getFocusedItem() ||
      childTable->getFocusedItem()->index != 1) {
    std::cerr
        << "Open error location did not select the included source line\n";
    return 1;
  }
  childWindow->close();

  const QString configPath = temporary.filePath("config.txt");
  QFile includeFile(temporary.filePath("child.txt"));
  if (!includeFile.open(QIODevice::WriteOnly) ||
      includeFile.write("Preamp: 0 dB\n") < 0) {
    std::cerr << "could not create Include editor fixture\n";
    return 1;
  }
  includeFile.close();
  IncludeEditor includeEditor("child.txt", configPath);
  auto *includePath = includeEditor.findChild<QLineEdit *>("includePathEdit");
  auto *includeStatus = includeEditor.findChild<QLabel *>("includeStatus");
  if (!includePath || !includeStatus ||
      !includeStatus->text().contains("found")) {
    std::cerr
        << "Include editor did not resolve a path relative to its config\n";
    return 1;
  }
  QString includeCommand, includeParameters;
  includeEditor.store(includeCommand, includeParameters);
  if (includeCommand != "Include" || includeParameters != "child.txt") {
    std::cerr << "Include editor changed the directive path on serialization\n";
    return 1;
  }

  const QString convolutionConfigPath =
      temporary.filePath("convolution-ui/config.txt");
  const QString impulsePath =
      temporary.filePath("convolution-ui/ir/room response.wav");
  if (!QDir().mkpath(QFileInfo(impulsePath).absolutePath()) ||
      !writeTestImpulseResponse(impulsePath)) {
    std::cerr << "could not create convolution impulse-response fixture\n";
    return 1;
  }
  QFile convolutionConfig(convolutionConfigPath);
  if (!convolutionConfig.open(QIODevice::WriteOnly) ||
      convolutionConfig.write("Convolution: ir/room response.wav\n") < 0) {
    std::cerr << "could not create convolution config fixture\n";
    return 1;
  }
  convolutionConfig.close();
  ConvolutionEditor convolutionEditor("ir/room response.wav",
                                      convolutionConfigPath);
  auto *convolutionPath =
      convolutionEditor.findChild<QLineEdit *>("convolutionPathEdit");
  auto *convolutionStatus =
      convolutionEditor.findChild<QLabel *>("convolutionStatus");
  if (!convolutionPath || !convolutionStatus ||
      !convolutionStatus->text().contains("File found")) {
    std::cerr
        << "Convolution editor did not resolve its config-relative path\n";
    return 1;
  }
  QString convolutionCommand, convolutionParameters;
  convolutionEditor.store(convolutionCommand, convolutionParameters);
  if (convolutionCommand != "Convolution" ||
      convolutionParameters != "ir/room response.wav") {
    std::cerr
        << "Convolution editor changed the directive path on serialization\n";
    return 1;
  }
  convolutionPath->setText("missing response.wav");
  convolutionEditor.update();
  QMetaObject::invokeMethod(convolutionPath, "editingFinished",
                            Qt::DirectConnection);
  if (!convolutionStatus->text().contains("not found or unreadable")) {
    std::cerr
        << "Convolution editor did not report a missing impulse response\n";
    return 1;
  }
#ifdef SKYAPO_UI_HAVE_CONVOLUTION_VALIDATION
  MainWindow convolutionWindow(convolutionConfigPath,
                               QString::fromLocal8Bit(argv[1]));
  auto *integratedConvolution =
      convolutionWindow.findChild<ConvolutionEditor *>();
  auto *integratedConvolutionPath =
      integratedConvolution
          ? integratedConvolution->findChild<QLineEdit *>("convolutionPathEdit")
          : nullptr;
  auto *integratedConvolutionStatus =
      integratedConvolution
          ? integratedConvolution->findChild<QLabel *>("convolutionStatus")
          : nullptr;
  if (!integratedConvolutionPath || !integratedConvolutionStatus ||
      integratedConvolutionPath->text() != "ir/room response.wav") {
    std::cerr << "MainWindow did not create the Convolution directive row\n";
    return 1;
  }
  QString diagnostic;
  if (!configIsValid(QString::fromLocal8Bit(argv[2]), convolutionConfigPath,
                     &diagnostic)) {
    std::cerr << "config checker rejected the valid Convolution fixture: "
              << diagnostic.toStdString() << '\n';
    return 1;
  }
  integratedConvolutionPath->setText("missing response.wav");
  QMetaObject::invokeMethod(integratedConvolutionPath, "editingFinished",
                            Qt::DirectConnection);
  if (!integratedConvolutionStatus->text().contains(
          "not found or unreadable")) {
    std::cerr
        << "integrated Convolution row did not surface missing file state\n";
    return 1;
  }
  bool savedConvolution = false;
  for (auto *button : convolutionWindow.findChildren<QPushButton *>()) {
    if (button->text() == "Save") {
      button->click();
      savedConvolution = true;
      break;
    }
  }
  QFile savedConvolutionConfig(convolutionConfigPath);
  if (!savedConvolution || !savedConvolutionConfig.open(QIODevice::ReadOnly) ||
      savedConvolutionConfig.readAll() !=
          "Convolution: missing response.wav\n") {
    std::cerr
        << "Convolution editor did not round-trip an edited path to config\n";
    return 1;
  }
  if (configIsValid(QString::fromLocal8Bit(argv[2]), convolutionConfigPath,
                    &diagnostic) ||
      !diagnostic.contains("cannot open convolution impulse response")) {
    std::cerr << "config checker did not reject a missing Convolution file\n";
    return 1;
  }
#endif

  QFile config(configPath);
  if (!config.open(QIODevice::WriteOnly) ||
      config.write(
          "Preamp: 0 dB\n; keep order\nPreamp: -6 dB\nInclude: child.txt\n"
          "Channel: L R\nCopy: L=R R=L\n"
          "Filter: ON IIR Order 1 Coefficients 1 0 1 0\n"
          "Filter: ON IIR Order 1 Coefficients ${b0} 0 1 0\n") < 0) {
    std::cerr << "could not create temporary UI config\n";
    return 1;
  }
  config.close();
  const QString deviceLogPath = temporary.filePath("selected-device.log");
  qputenv("SKYAPO_UI_TEST_DEVICE_LOG", deviceLogPath.toLocal8Bit());

  QElapsedTimer construction;
  construction.start();
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                     temporary.path() + QStringLiteral("/settings"));
  QSettings uiSettings;
  uiSettings.clear();
  {
    MainWindow preferencesWindow(configPath, QString::fromLocal8Bit(argv[1]));
    preferencesWindow.resize(777, 555);
    preferencesWindow.close();
  }
  if (uiSettings.value(QStringLiteral("ui/geometry")).toByteArray().isEmpty() ||
      uiSettings.value(QStringLiteral("ui/state")).toByteArray().isEmpty() ||
      uiSettings.value(QStringLiteral("ui/lastDirectory")).toString() !=
          QFileInfo(configPath).absolutePath()) {
    std::cerr << "closing the editor did not persist window preferences\n";
    return 1;
  }
  const QByteArray savedWindowGeometry =
      uiSettings.value(QStringLiteral("ui/geometry")).toByteArray();
  uiSettings.clear();
  uiSettings.setValue(QStringLiteral("ui/geometry"), savedWindowGeometry);
  // Measure construction itself; the fixture preparation and nested config
  // checker processes below must not count against the responsiveness limit.
  construction.restart();
  MainWindow window(configPath, QString::fromLocal8Bit(argv[1]));

  if (window.size() != QSize(777, 555)) {
    std::cerr << "editor did not restore its saved window geometry\n";
    return 1;
  }
  const auto recentFiles =
      uiSettings.value(QStringLiteral("ui/recentFiles")).toStringList();
  auto *recentMenu =
      window.findChild<QMenu *>(QStringLiteral("recentFilesMenu"));
  if (recentFiles.isEmpty() ||
      recentFiles.first() != QFileInfo(configPath).absoluteFilePath() ||
      !recentMenu || recentMenu->actions().isEmpty() ||
      recentMenu->actions().first()->data().toString() != recentFiles.first()) {
    std::cerr << "opening a config did not persist it in Open Recent\n";
    return 1;
  }
  const QString saveAsSourcePath = temporary.filePath("save-as-source.txt");
  const QString saveAsDestinationPath =
      temporary.filePath("save-as-copy/renamed-config.txt");
  if (!QDir().mkpath(QFileInfo(saveAsDestinationPath).absolutePath())) {
    std::cerr << "could not create Save As destination directory\n";
    return 1;
  }
  QFile saveAsSource(saveAsSourcePath);
  if (!saveAsSource.open(QIODevice::WriteOnly) ||
      saveAsSource.write("Preamp: -6 dB\n") != 14) {
    std::cerr << "could not create Save As source config\n";
    return 1;
  }
  saveAsSource.close();
  MainWindow saveAsWindow(saveAsSourcePath, QString::fromLocal8Bit(argv[1]));
  auto *saveAsAction = saveAsWindow.findChild<QAction *>("saveConfigAs");
  if (!saveAsAction ||
      saveAsAction->text() != QStringLiteral("Save Configuration As…")) {
    std::cerr << "File menu does not expose Save Configuration As\n";
    return 1;
  }
  auto *saveAsPreamp = saveAsWindow.findChild<PreampFilterGUI *>();
  auto *saveAsGain =
      saveAsPreamp ? saveAsPreamp->findChild<QDoubleSpinBox *>("doubleSpinBox")
                   : nullptr;
  if (!saveAsGain) {
    std::cerr << "Save As workflow did not load an editable config row\n";
    return 1;
  }
  saveAsGain->setValue(-9.0);
  bool saveAsSucceeded = false;
  if (!QMetaObject::invokeMethod(&saveAsWindow, "saveConfigAsPath",
                                 Qt::DirectConnection,
                                 Q_RETURN_ARG(bool, saveAsSucceeded),
                                 Q_ARG(QString, saveAsDestinationPath)) ||
      !saveAsSucceeded) {
    std::cerr << "GUI Save As did not save to the selected destination\n";
    return 1;
  }
  QFile saveAsCopy(saveAsDestinationPath);
  QFile unchangedSaveAsSource(saveAsSourcePath);
  auto *saveAsPathEdit = saveAsWindow.findChild<QLineEdit *>("configPath");
  const auto saveAsRecent =
      uiSettings.value(QStringLiteral("ui/recentFiles")).toStringList();
  if (!saveAsCopy.open(QIODevice::ReadOnly) ||
      saveAsCopy.readAll() != "Preamp: -9 dB\n" ||
      !unchangedSaveAsSource.open(QIODevice::ReadOnly) ||
      unchangedSaveAsSource.readAll() != "Preamp: -6 dB\n" || !saveAsPathEdit ||
      saveAsPathEdit->text() !=
          QFileInfo(saveAsDestinationPath).absoluteFilePath() ||
      saveAsWindow.windowTitle().startsWith('*') || saveAsRecent.isEmpty() ||
      saveAsRecent.first() !=
          QFileInfo(saveAsDestinationPath).absoluteFilePath()) {
    std::cerr << "Save As did not atomically switch editor path/state or "
                 "update recents\n";
    return 1;
  }
  const QString includeSourceDirectory =
      temporary.filePath("save-include-source");
  const QString includeDestinationDirectory =
      temporary.filePath("save-include-destination");
  const QString nestedIncludeDirectory =
      includeSourceDirectory + QStringLiteral("/nested");
  if (!QDir().mkpath(nestedIncludeDirectory) ||
      !QDir().mkpath(includeDestinationDirectory)) {
    std::cerr << "could not create Save As Include fixture directories\n";
    return 1;
  }
  const QString includeRootPath =
      includeSourceDirectory + QStringLiteral("/root.txt");
  const QString includeChildPath =
      nestedIncludeDirectory + QStringLiteral("/child#one.txt");
  const QString includeLeafPath =
      includeSourceDirectory + QStringLiteral("/leaf config.txt");
  const QString includeDestinationPath =
      includeDestinationDirectory + QStringLiteral("/copied.txt");
  const QString includeAliasDirectory =
      temporary.filePath("save-include-alias");
  const QString includeAliasPath =
      includeAliasDirectory + QStringLiteral("/root-link.txt");
  const auto writeFixture = [](const QString &path,
                               const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(contents) == contents.size();
  };
  const QByteArray rootIncludeContents =
      "Preamp: -3 dB\nInclude: \"nested/child#one.txt\" # root comment\n";
  if (!writeFixture(includeRootPath, rootIncludeContents) ||
      !writeFixture(includeChildPath,
                    "Preamp: 0 dB\nInclude: \"../leaf config.txt\" "
                    "# nested comment\n") ||
      !writeFixture(includeLeafPath, "Preamp: 0 dB\n") ||
      !QDir().mkpath(includeAliasDirectory) ||
      !QFile::link(includeRootPath, includeAliasPath) ||
      !configIsValid(QString::fromLocal8Bit(argv[2]), includeAliasPath)) {
    std::cerr
        << "nested quoted/comment Include fixture is not initially valid\n";
    return 1;
  }
  MainWindow includeSaveAsWindow(includeAliasPath,
                                 QString::fromLocal8Bit(argv[1]));
  bool includeSaveAsSucceeded = false;
  if (!QMetaObject::invokeMethod(&includeSaveAsWindow, "saveConfigAsPath",
                                 Qt::DirectConnection,
                                 Q_RETURN_ARG(bool, includeSaveAsSucceeded),
                                 Q_ARG(QString, includeDestinationPath)) ||
      !includeSaveAsSucceeded ||
      !configIsValid(QString::fromLocal8Bit(argv[2]), includeDestinationPath)) {
    std::cerr << "Save As changed the target of a nested quoted Include\n";
    return 1;
  }
  QFile rebasedIncludeRoot(includeDestinationPath);
  const QString rebasedChildPath =
      QDir(includeDestinationDirectory).relativeFilePath(includeChildPath);
  const QByteArray expectedRebasedRoot = "Preamp: -3 dB\nInclude: \"" +
                                         rebasedChildPath.toUtf8() +
                                         "\" # root comment\n";
  if (!rebasedIncludeRoot.open(QIODevice::ReadOnly) ||
      rebasedIncludeRoot.readAll() != expectedRebasedRoot) {
    std::cerr << "Save As did not preserve quoted path/comment text while "
                 "rebasing the Include\n";
    return 1;
  }
  if (window.findChildren<FilterTableRow *>().size() != 8 ||
      window.findChildren<IncludeEditor *>().size() != 1 ||
      window.findChildren<ChannelCopyEditor *>().size() != 2 ||
      window.findChildren<IIRFilterEditor *>().size() != 1) {
    std::cerr << "upstream FilterTableRow was not used by the Linux editor\n";
    return 1;
  }
  auto *rowMenu = window.findChild<QMenu *>("upstreamFilterAddMenu");
  if (!rowMenu) {
    std::cerr << "Linux FilterTable row adapter was not created\n";
    return 1;
  }
  const QStringList expectedAddCategories{"Comment",
                                          "Control",
                                          "Basic filters",
                                          "Parametric filters",
                                          "Graphic equalizers",
                                          "Advanced filters",
                                          "Plugins"};
  QStringList actualAddCategories;
  for (const auto *action : rowMenu->actions())
    actualAddCategories.push_back(action->text());
  const auto submenu = [rowMenu](const char *name) {
    return rowMenu->findChild<QMenu *>(QString::fromLatin1(name));
  };
  const auto containsTemplate = [](QMenu *menu, const QString &line) {
    if (!menu)
      return false;
    for (const auto *action : menu->actions())
      if (action->data().canConvert<FilterTemplate>() &&
          action->data().value<FilterTemplate>().getLine() == line)
        return true;
    return false;
  };
  auto *controlMenu = submenu("addFilterControlMenu");
  auto *basicMenu = submenu("addFilterBasicMenu");
  auto *advancedMenu = submenu("addFilterAdvancedMenu");
  auto *pluginsMenu = submenu("addFilterPluginsMenu");
  const bool hasPluginChoices =
      pluginsMenu && pluginsMenu->findChild<QAction *>("addLinuxVstPlugin") &&
      pluginsMenu->findChild<QAction *>("addWindowsVstPlugin") &&
      pluginsMenu->findChild<QAction *>("addInstalledPluginFromMenu");
  if (actualAddCategories != expectedAddCategories ||
      !containsTemplate(basicMenu, "Preamp: 0 dB") ||
      !containsTemplate(basicMenu, "Delay: 0 ms") ||
      !containsTemplate(basicMenu, "Copy: L=L") ||
      !containsTemplate(controlMenu, "Include: child.txt") ||
      !containsTemplate(advancedMenu, "Convolution: impulse-response.wav") ||
      !containsTemplate(
          advancedMenu,
          "LoudnessCorrection: ReferenceLevel 0 ReferenceOffset 0") ||
      !hasPluginChoices) {
    std::cerr << "Add Filter menu does not match the required category order, "
                 "filter groups, or explicit platform VST entries\n";
    return 1;
  }
  if (construction.elapsed() >= 1000) {
    std::cerr << "MainWindow blocked while starting delayed CLI requests ("
              << construction.elapsed() << " ms)\n";
    return 1;
  }
  auto *table = window.findChild<FilterTable *>();
  if (!table || !table->itemAt(0) || !table->itemAt(0)->row) {
    std::cerr << "Linux FilterTable adapter did not expose ordered rows\n";
    return 1;
  }
  auto *integratedIir = window.findChild<IIRFilterEditor *>();
  auto *integratedCoefficient =
      integratedIir
          ? integratedIir->findChild<QLineEdit *>("iirCoefficient_b_0")
          : nullptr;
  if (!integratedCoefficient) {
    std::cerr << "MainWindow did not build the IIR visual row\n";
    return 1;
  }
  integratedCoefficient->setText("0.5");
  table->setSelection(table->itemAt(0));
  if (table->getFocusedItem() != table->itemAt(0) ||
      !table->getSelectedItems().contains(table->itemAt(0))) {
    std::cerr << "row selection/focus model did not update\n";
    return 1;
  }
  table->setSelection(table->itemAt(2), false, true);
  if (table->getSelectedItems().size() != 3) {
    std::cerr << "shift selection did not select an ordered row range\n";
    return 1;
  }
  table->setSelection(table->itemAt(1), true, false);
  if (table->getSelectedItems().size() != 2) {
    std::cerr << "control selection did not toggle an individual row\n";
    return 1;
  }
  QKeyEvent clearSelection(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(table->itemAt(0)->row, &clearSelection);
  if (!table->getSelectedItems().isEmpty()) {
    std::cerr << "Escape did not clear row selection\n";
    return 1;
  }
  table->setSelection(table->itemAt(0));
  QKeyEvent moveDown(QEvent::KeyPress, Qt::Key_Down, Qt::AltModifier);
  QApplication::sendEvent(table->itemAt(0)->row, &moveDown);
  for (auto *button : window.findChildren<QPushButton *>())
    if (button->text() == "Save")
      button->click();
  QFile reordered(configPath);
  if (!reordered.open(QIODevice::ReadOnly) ||
      reordered.readAll() !=
          "; keep order\nPreamp: 0 dB\nPreamp: -6 dB\nInclude: child.txt\n"
          "Channel: L R\nCopy: L=R R=L\n"
          "Filter: ON IIR Order 1 Coefficients 0.5 0 1 0\n"
          "Filter: ON IIR Order 1 Coefficients ${b0} 0 1 0\n") {
    std::cerr << "Alt+Down did not reorder and save the selected config row\n";
    return 1;
  }
  unsigned heartbeat = 0;
  QTimer pulse;
  pulse.setInterval(10);
  QObject::connect(&pulse, &QTimer::timeout, [&heartbeat] { ++heartbeat; });
  pulse.start();
  QEventLoop loop;
  QTimer::singleShot(1500, &loop, &QEventLoop::quit);
  loop.exec();

  if (heartbeat < 50) {
    std::cerr << "GUI event loop stalled while CLI fixture was running\n";
    return 1;
  }
  auto *deviceCombo = window.findChild<QComboBox *>("inputDeviceCombo");
  if (!deviceCombo || deviceCombo->count() != 2 ||
      deviceCombo->currentData().toString() != "fixture.usb-mic" ||
      !deviceCombo->currentText().contains("1 ch, 44100 Hz") ||
      !deviceCombo->toolTip().contains("stable PipeWire node name")) {
    std::cerr << "device selector did not display enumerated format details: "
              << (deviceCombo ? deviceCombo->count() : -1) << ", "
              << (deviceCombo
                      ? deviceCombo->currentData().toString().toStdString()
                      : "missing")
              << ", "
              << (deviceCombo ? deviceCombo->currentText().toStdString()
                              : "missing")
              << '\n';
    return 1;
  }
  auto *runtimeStatus = window.findChild<QLabel *>("daemonStatus");
  if (!runtimeStatus || !runtimeStatus->wordWrap() ||
      runtimeStatus->textFormat() != Qt::PlainText ||
      !runtimeStatus->accessibleName().contains("PipeWire")) {
    std::cerr << "runtime status does not expose readable accessible PipeWire "
                 "details\n";
    return 1;
  }
  struct StatusCase {
    QString filterState;
    QString dspState;
    QString expectedSummary;
    bool negotiatedFormat;
  };
  const QList<StatusCase> statusCases{
      {"streaming", "running", "Streaming — audio processing active", true},
      {"streaming", "initializing (awaiting negotiated format)",
       "PipeWire connected — waiting for negotiated audio format", false},
      {"streaming", "reconfiguring (format change; output silence)",
       "PipeWire connected — rebuilding DSP for audio format (output silence)",
       true},
      {"streaming", "quiescing (output silence)",
       "PipeWire connected — DSP transition in progress (output silence)",
       true},
      {"streaming", "paused (plugin latency refresh failed; output silence)",
       "PipeWire streaming — DSP paused (output silence)", true},
      {"connecting", "initializing",
       "Waiting for PipeWire (filter state: connecting)", false},
      {"unconnected", "initializing",
       "Waiting for PipeWire (filter state: unconnected)", false},
      {"paused", "quiescing", "PipeWire connected but paused (not streaming)",
       false},
      {"error", "initializing",
       "PipeWire reported an error (recovery is not confirmed)", false},
      {"not reachable", "initializing", "Daemon offline or unresponsive",
       false},
      {"future-state", "running", "Daemon state unknown: future-state", false},
      {"", "initializing", "Daemon offline or unresponsive", false}};
  for (qsizetype stateIndex = 0; stateIndex < statusCases.size();
       ++stateIndex) {
    const auto &testCase = statusCases[stateIndex];
    const QString scriptPath =
        temporary.filePath(QStringLiteral("status-cli-%1.sh").arg(stateIndex));
    const int exitCode = testCase.filterState.isEmpty() ? 1 : 0;
    if (!writeStatusCliFixture(scriptPath, testCase.filterState, exitCode,
                               testCase.dspState, testCase.negotiatedFormat)) {
      std::cerr << "could not create PipeWire status fixture script\n";
      return 1;
    }
    MainWindow statusWindow(configPath, scriptPath);
    auto *label = statusWindow.findChild<QLabel *>("daemonStatus");
    QElapsedTimer statusWait;
    statusWait.start();
    while (statusWait.elapsed() < 1500 &&
           (!label || !label->text().startsWith(testCase.expectedSummary))) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    if (!label || !label->text().startsWith(testCase.expectedSummary) ||
        (exitCode == 0 &&
         (label->text().contains("Sample rate: unknown (awaiting graph)") ==
              testCase.negotiatedFormat ||
          !label->text().contains("SkyAPO Virtual Mic") ||
          label->accessibleDescription() != label->text()))) {
      std::cerr << "UI did not accurately summarize PipeWire state '"
                << testCase.filterState.toStdString() << "' / DSP state '"
                << testCase.dspState.toStdString() << "': "
                << (label ? label->text().toStdString()
                          : "status label missing")
                << '\n';
      return 1;
    }
  }
  deviceCombo->setCurrentIndex(0);
  QMetaObject::invokeMethod(deviceCombo, "activated", Qt::DirectConnection,
                            Q_ARG(int, 0));
  if (deviceCombo->isEnabled()) {
    std::cerr << "device selector remained interactive during device change\n";
    return 1;
  }
  QMetaObject::invokeMethod(deviceCombo, "activated", Qt::DirectConnection,
                            Q_ARG(int, 1));
  QEventLoop selectionLoop;
  QTimer::singleShot(300, &selectionLoop, &QEventLoop::quit);
  selectionLoop.exec();
  QFile deviceLog(deviceLogPath);
  if (!deviceLog.open(QIODevice::ReadOnly) ||
      deviceLog.readAll() != "fixture.capture\n") {
    std::cerr
        << "device selector did not persist exactly one stable node name\n";
    return 1;
  }

  const QString dynamicDeviceListPath =
      temporary.filePath("dynamic-device-list.tsv");
  const auto writeDynamicDeviceList = [&](const QByteArray &contents) {
    QFile list(dynamicDeviceListPath);
    if (!list.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return false;
    return list.write(contents) == contents.size();
  };
  const QByteArray selectedDeviceList =
      "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE RATE\t"
      "STABLE PROPERTY\tDEVICE BUS ID\tALSA CARD\tALSA PATH\tDEFAULT\n"
      "41\tfixture.capture\tBuilt-in capture\t\t2\t48000\t\t\t\t\tno\n"
      "42\tfixture.usb-mic\tSelected USB microphone\tyes\t1\t44100\t"
      "\t\t\t\tyes\n";
  if (!writeDynamicDeviceList(selectedDeviceList)) {
    std::cerr << "could not create changing device-list fixture\n";
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_DEVICE_LIST_FILE",
          dynamicDeviceListPath.toLocal8Bit());
  MainWindow dynamicDeviceWindow(configPath, QString::fromLocal8Bit(argv[1]));

  auto *dynamicDeviceCombo =
      dynamicDeviceWindow.findChild<QComboBox *>("inputDeviceCombo");
  auto *deviceRefreshTimer =
      dynamicDeviceWindow.findChild<QTimer *>("deviceRefreshTimer");
  QElapsedTimer dynamicDeviceWait;
  dynamicDeviceWait.start();
  while (dynamicDeviceWait.elapsed() < 2000 &&
         (!dynamicDeviceCombo ||
          dynamicDeviceCombo->currentData().toString() != "fixture.usb-mic")) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!dynamicDeviceCombo || !deviceRefreshTimer ||
      dynamicDeviceCombo->currentData().toString() != "fixture.usb-mic" ||
      !dynamicDeviceCombo->currentText().contains("default input") ||
      !writeDynamicDeviceList(
          "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE RATE\n"
          "41\tfixture.capture\tBuilt-in capture\t\t2\t48000\n")) {
    std::cerr << "dynamic device fixture did not initialize with selection\n";
    return 1;
  }
  deviceRefreshTimer->start(1);
  dynamicDeviceWait.restart();
  while (dynamicDeviceWait.elapsed() < 2000 &&
         !dynamicDeviceCombo->currentText().contains(
             "Selected input unavailable")) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  deviceRefreshTimer->stop();
  if (!dynamicDeviceCombo->currentText().contains(
          "Selected input unavailable") ||
      dynamicDeviceCombo->itemData(0, Qt::UserRole + 1).toString() !=
          "fixture.usb-mic" ||
      dynamicDeviceCombo->itemData(0).toString() != QString{} ||
      dynamicDeviceCombo->count() != 2 || deviceRefreshTimer->isActive()) {
    std::cerr << "removed selected node was not retained as unavailable by "
                 "stable name\n";
    return 1;
  }
  dynamicDeviceCombo->setCurrentIndex(0);
  QMetaObject::invokeMethod(dynamicDeviceCombo, "activated",
                            Qt::DirectConnection, Q_ARG(int, 0));
  if (!writeDynamicDeviceList(selectedDeviceList)) {
    std::cerr << "could not restore dynamic device-list fixture\n";
    return 1;
  }
  deviceRefreshTimer->start(1);
  dynamicDeviceWait.restart();
  while (dynamicDeviceWait.elapsed() < 2000 &&
         dynamicDeviceCombo->currentData().toString() != "fixture.usb-mic") {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  deviceRefreshTimer->stop();
  if (dynamicDeviceCombo->currentData().toString() != "fixture.usb-mic" ||
      deviceRefreshTimer->isActive()) {
    std::cerr
        << "reappearing selected node was not restored by the daemon marker\n";
    return 1;
  }
  deviceRefreshTimer->stop();
  if (deviceRefreshTimer->isActive()) {
    std::cerr << "device refresh timer remained active after the fixture\n";
    return 1;
  }

  qunsetenv("SKYAPO_UI_TEST_DEVICE_LIST_FILE");
  const QString graphicConfigPath = temporary.filePath("graphic-eq.txt");
  QFile graphicConfig(graphicConfigPath);
  if (!graphicConfig.open(QIODevice::WriteOnly) ||
      graphicConfig.write("GraphicEQ: 100 0; 1000 2; 5000 -3\n") < 0) {
    std::cerr << "could not create GraphicEQ UI fixture\n";
    return 1;
  }
  graphicConfig.close();

  MainWindow graphicWindow(graphicConfigPath, QString::fromLocal8Bit(argv[1]));

#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  auto *graphicEditor = graphicWindow.findChild<GraphicEQFilterGUI *>();
  auto *graphicTable =
      graphicEditor ? graphicEditor->findChild<QTableWidget *>("tableWidget")
                    : nullptr;
  if (!graphicEditor || !graphicTable || graphicTable->rowCount() != 3) {
    std::cerr << "MainWindow did not create upstream GraphicEQ visual row\n";
    return 1;
  }
  graphicTable->item(1, 1)->setText("4.5");
  for (auto *button : graphicWindow.findChildren<QPushButton *>())
    if (button->text() == "Save")
      button->click();
  QFile savedGraphicConfig(graphicConfigPath);
  if (!savedGraphicConfig.open(QIODevice::ReadOnly) ||
      savedGraphicConfig.readAll() != "GraphicEQ: 100 0; 1000 4.5; 5000 -3\n") {
    std::cerr << "GraphicEQ visual edit did not serialize to config text\n";
    return 1;
  }
#else
  if (graphicWindow.findChild<QWidget *>("GraphicEQFilterGUI")) {
    std::cerr << "GraphicEQ visual editor was enabled without FFTW3f\n";
    return 1;
  }
  QFile rawGraphicConfig(graphicConfigPath);
  if (!rawGraphicConfig.open(QIODevice::ReadOnly) ||
      rawGraphicConfig.readAll() != "GraphicEQ: 100 0; 1000 2; 5000 -3\n") {
    std::cerr << "unsupported GraphicEQ text was changed by the editor\n";
    return 1;
  }
#endif
  const QString deviceEditorConfigPath =
      temporary.filePath("device-editor.txt");
  QFile deviceEditorConfig(deviceEditorConfigPath);
  if (!deviceEditorConfig.open(QIODevice::WriteOnly) ||
      deviceEditorConfig.write("Device: Built-in Audio; USB Microphone\n") <
          0) {
    std::cerr << "could not create Device editor UI fixture\n";
    return 1;
  }
  deviceEditorConfig.close();
  MainWindow deviceEditorWindow(deviceEditorConfigPath,
                                QString::fromLocal8Bit(argv[1]));
  auto *deviceEditor = deviceEditorWindow.findChild<DeviceFilterEditor *>();
  auto *devicePattern =
      deviceEditor ? deviceEditor->findChild<QLineEdit *>("devicePatternEdit")
                   : nullptr;
  if (!deviceEditor || !devicePattern ||
      devicePattern->text() != "Built-in Audio; USB Microphone") {
    std::cerr << "MainWindow did not create Device match editor from config\n";
    return 1;
  }
  devicePattern->setText("USB Microphone; Built-in Audio");
  QMetaObject::invokeMethod(devicePattern, "editingFinished",
                            Qt::DirectConnection);
  bool savedDeviceEditor = false;
  for (auto *button : deviceEditorWindow.findChildren<QPushButton *>()) {
    if (button->text() == "Save") {
      button->click();
      savedDeviceEditor = true;
      break;
    }
  }
  QFile savedDeviceEditorConfig(deviceEditorConfigPath);
  if (!savedDeviceEditor ||
      !savedDeviceEditorConfig.open(QIODevice::ReadOnly) ||
      savedDeviceEditorConfig.readAll() !=
          "Device: USB Microphone; Built-in Audio\n") {
    std::cerr << "Device editor did not serialize its edited pattern\n";
    return 1;
  }

  auto *pluginAction = window.findChild<QAction *>("browsePlugins");
  if (!pluginAction) {
    std::cerr << "plugin catalog action is missing from the UI\n";
    return 1;
  }

  pluginAction->trigger();

  if (pluginAction->isEnabled()) {
    std::cerr << "plugin catalog action was not disabled during discovery\n";
    return 1;
  }
  QDialog *pluginDialog = nullptr;
  QElapsedTimer pluginWait;
  pluginWait.start();
  while (pluginWait.elapsed() < 2000 && !pluginDialog) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    const auto dialogs = window.findChildren<QDialog *>();
    for (auto *candidate : dialogs)
      if (candidate->windowTitle() == "Installed plugins") {
        pluginDialog = candidate;
        break;
      }
    QThread::msleep(5);
  }
  auto *pluginCatalog =
      pluginDialog ? pluginDialog->findChild<QPlainTextEdit *>("pluginCatalog")
                   : nullptr;

  if (!pluginDialog || !pluginCatalog ||
      !pluginCatalog->toPlainText().contains(
          "https://example.test/plugins/gain") ||
      !pluginCatalog->toPlainText().contains("Test Gain") ||
      !pluginAction->isEnabled()) {
    std::cerr << "plugin catalog did not show CLI discovery results\n";
    return 1;
  }
  pluginDialog->accept();

  auto *addPluginAction = window.findChild<QAction *>("addInstalledPlugin");
  if (!addPluginAction) {
    std::cerr << "plugin insertion action is missing from the editor\n";
    return 1;
  }
  bool pickerWasValid = false;
  bool pickerWasAccepted = false;
  QTimer::singleShot(200, &window, [&] {
    for (auto *candidate : window.findChildren<QDialog *>()) {
      if (candidate->objectName() != "pluginPicker")
        continue;
      auto *choices = candidate->findChild<QListWidget *>("pluginPickerList");
      auto *add = candidate->findChild<QPushButton *>("addPluginButton");
      pickerWasValid = choices && choices->count() == 2 &&
                       choices->item(0)->text().contains("Test Gain") && add;
      if (pickerWasValid) {
        add->click();
        pickerWasAccepted = true;
      }
      break;
    }
  });

  addPluginAction->trigger();

  QElapsedTimer pickerWait;
  pickerWait.start();
  while (pickerWait.elapsed() < 2000 && !pickerWasAccepted) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!pickerWasValid || !pickerWasAccepted) {
    auto *choices = window.findChild<QListWidget *>("pluginPickerList");
    std::cerr << "plugin picker did not present discovered catalog entries; "
              << "valid=" << pickerWasValid << " accepted=" << pickerWasAccepted
              << " choices=" << (choices ? choices->count() : -1) << "\n";
    for (auto *dialog : window.findChildren<QDialog *>())
      std::cerr << "dialog: " << dialog->windowTitle().toStdString() << "\n";
    return 1;
  }
  bool insertedPluginDirective = false;
  for (int index = 0; table->itemAt(index); ++index)
    insertedPluginDirective =
        insertedPluginDirective ||
        table->itemAt(index)->text ==
            "Plugin: LV2 https://example.test/plugins/gain";
  if (!insertedPluginDirective) {
    std::cerr
        << "choosing a discovered plugin did not insert its config line\n";
    return 1;
  }

  bool linuxVstPickerValid = false;
  bool linuxVstPickerAccepted = false;
  QTimer::singleShot(200, &window, [&] {
    for (auto *candidate : window.findChildren<QDialog *>()) {
      if (candidate->objectName() != "pluginPicker" ||
          candidate->windowTitle() != "Add VST plugin (Linux)")
        continue;
      auto *choices = candidate->findChild<QListWidget *>("pluginPickerList");
      auto *add = candidate->findChild<QPushButton *>("addPluginButton");
      linuxVstPickerValid = choices && choices->count() == 1 && add &&
                            choices->item(0)->text().contains("VST3") &&
                            choices->item(0)->text().contains("test.vst3.uid");
      if (linuxVstPickerValid) {
        add->click();
        linuxVstPickerAccepted = true;
      }
      break;
    }
  });
  table->addLine("__SKYAPO_PICK_NATIVE_VST_PLUGIN__");
  QElapsedTimer linuxVstPickerWait;
  linuxVstPickerWait.start();
  while (linuxVstPickerWait.elapsed() < 2000 && !linuxVstPickerAccepted) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  bool insertedNativeVstDirective = false;
  for (int index = 0; table->itemAt(index); ++index)
    insertedNativeVstDirective =
        insertedNativeVstDirective ||
        table->itemAt(index)->text == "Plugin: VST3 test.vst3.uid";
  if (!linuxVstPickerValid || !linuxVstPickerAccepted ||
      !insertedNativeVstDirective) {
    std::cerr << "Linux VST menu flow did not restrict insertion to a "
                 "discovered native VST3 plugin\n";
    return 1;
  }

  const QString pluginParameterConfigPath =
      temporary.filePath(QStringLiteral("plugin-parameters.txt"));
  const QByteArray pluginParameterOriginal =
      "; preserve this header\r\n"
      "Plugin: LV2 https://example.test/plugins/gain   gain=0.25 # keep "
      "note\r\n";
  QFile pluginParameterConfig(pluginParameterConfigPath);
  if (!pluginParameterConfig.open(QIODevice::WriteOnly) ||
      pluginParameterConfig.write(pluginParameterOriginal) !=
          pluginParameterOriginal.size()) {
    std::cerr << "could not create plugin parameter editor fixture\n";
    return 1;
  }
  pluginParameterConfig.close();
  MainWindow pluginParameterWindow(pluginParameterConfigPath,
                                   QString::fromLocal8Bit(argv[1]));
  auto *parameterTable = pluginParameterWindow.findChild<FilterTable *>();
  auto *parameterAction = pluginParameterWindow.findChild<QAction *>(
      "configureSelectedPluginParameters");
  if (!parameterTable || !parameterTable->itemAt(1) || !parameterAction) {
    std::cerr << "plugin parameter editor action or target row is missing\n";
    return 1;
  }
  parameterTable->setSelection(parameterTable->itemAt(1));

  bool parameterDialogWasAccepted = false;
  QTimer::singleShot(250, &pluginParameterWindow, [&] {
    for (auto *candidate : pluginParameterWindow.findChildren<QDialog *>()) {
      if (candidate->objectName() != "pluginParameterEditor")
        continue;
      auto *gain =
          candidate->findChild<QDoubleSpinBox *>("pluginParameter_gain");
      auto *notice = candidate->findChild<QLabel *>();
      auto *buttons =
          candidate->findChild<QDialogButtonBox *>("pluginParameterButtons");
      if (gain && notice &&
          notice->text().contains("not live plugin", Qt::CaseInsensitive) &&
          buttons) {
        gain->setValue(0.75);
        buttons->button(QDialogButtonBox::Ok)->click();
        parameterDialogWasAccepted = true;
      }
      break;
    }
  });
  parameterAction->trigger();

  QElapsedTimer parameterWait;
  parameterWait.start();
  while (parameterWait.elapsed() < 2000 && !parameterDialogWasAccepted) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  bool savedParameterConfig = false;
  for (auto *button : pluginParameterWindow.findChildren<QPushButton *>()) {
    if (button->text() == "Save") {
      button->click();
      savedParameterConfig = true;
      break;
    }
  }
  QFile savedPluginParameterConfig(pluginParameterConfigPath);
  const QByteArray expectedPluginParameterConfig =
      "; preserve this header\r\n"
      "Plugin: LV2 https://example.test/plugins/gain   gain=0.75 # keep "
      "note\r\n";
  if (!parameterDialogWasAccepted || !savedParameterConfig ||
      !savedPluginParameterConfig.open(QIODevice::ReadOnly) ||
      savedPluginParameterConfig.readAll() != expectedPluginParameterConfig) {
    std::cerr << "plugin parameter editor failed to preserve config comments, "
                 "spacing, or CRLF while applying a value\n";
    return 1;
  }
  const auto verifyPluginParameterFormat = [&](const QString &filename,
                                               const QByteArray &source,
                                               const QString &symbol,
                                               const QByteArray &expected) {
    const QString path = temporary.filePath(filename);
    QFile input(path);
    if (!input.open(QIODevice::WriteOnly) ||
        input.write(source) != source.size())
      return false;
    input.close();
    MainWindow formatWindow(path, QString::fromLocal8Bit(argv[1]));
    auto *formatTable = formatWindow.findChild<FilterTable *>();
    auto *formatAction =
        formatWindow.findChild<QAction *>("configureSelectedPluginParameters");
    if (!formatTable || !formatTable->itemAt(0) || !formatAction)
      return false;
    formatTable->setSelection(formatTable->itemAt(0));
    bool accepted = false;
    QTimer::singleShot(250, &formatWindow, [&] {
      for (auto *candidate : formatWindow.findChildren<QDialog *>()) {
        if (candidate->objectName() != "pluginParameterEditor")
          continue;
        auto *spin =
            candidate->findChild<QDoubleSpinBox *>("pluginParameter_" + symbol);
        auto *buttons =
            candidate->findChild<QDialogButtonBox *>("pluginParameterButtons");
        if (spin && buttons) {
          spin->setValue(0.75);
          buttons->button(QDialogButtonBox::Ok)->click();
          accepted = true;
        }
        break;
      }
    });

    formatAction->trigger();

    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 2000 && !accepted) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    bool saved = false;
    for (auto *button : formatWindow.findChildren<QPushButton *>()) {
      if (button->text() == "Save") {
        button->click();
        saved = true;
        break;
      }
    }
    QFile result(path);
    return accepted && saved && result.open(QIODevice::ReadOnly) &&
           result.readAll() == expected;
  };
  if (!verifyPluginParameterFormat(QStringLiteral("plugin-clap-parameters.txt"),
                                   "Plugin: CLAP test.clap.gain Gain=0.25\n",
                                   QStringLiteral("7"),
                                   "Plugin: CLAP test.clap.gain Gain=0.75\n")) {
    std::cerr << "CLAP name-based parameter configuration was not preserved\n";
    return 1;
  }

  if (!verifyPluginParameterFormat(QStringLiteral("plugin-vst3-parameters.txt"),
                                   "Plugin: VST3 test.vst3.uid 42=0.25\n",
                                   QStringLiteral("42"),
                                   "Plugin: VST3 test.vst3.uid 42=0.75\n")) {
    std::cerr << "VST3 numeric-ID parameter configuration was not preserved\n";
    return 1;
  }

  const QString livePluginCli = temporary.filePath("live-plugin-cli.sh");
  const QString livePluginLog = temporary.filePath("live-plugin-control.log");
  const QString livePluginConfigPath =
      temporary.filePath("live-plugin-config.txt");
  QFile livePluginConfig(livePluginConfigPath);
  const QByteArray livePluginConfigBytes =
      "Plugin: LV2 test.gain.plugin gain=0.25 mix=1\n";
  if (!writeLivePluginCliFixture(livePluginCli) ||
      !livePluginConfig.open(QIODevice::WriteOnly) ||
      livePluginConfig.write(livePluginConfigBytes) !=
          livePluginConfigBytes.size()) {
    std::cerr << "could not create live plugin GUI fixtures\n";
    return 1;
  }
  livePluginConfig.close();
  qunsetenv("SKYAPO_UI_TEST_PLUGIN_CONTROL_LOG");
  qputenv("SKYAPO_UI_TEST_PLUGIN_CONTROL_LOG", livePluginLog.toLocal8Bit());
  MainWindow livePluginWindow(livePluginConfigPath, livePluginCli);

  auto *livePluginTable = livePluginWindow.findChild<FilterTable *>();
  auto *livePluginParameters = livePluginWindow.findChild<QAction *>(
      "configureSelectedPluginParameters");
  auto *liveBypassOn =
      livePluginWindow.findChild<QAction *>("setPluginBypassOn");
  auto *liveBypassOff =
      livePluginWindow.findChild<QAction *>("setPluginBypassOff");
  if (!livePluginTable || !livePluginTable->itemAt(0) ||
      !livePluginParameters || !liveBypassOn || !liveBypassOff) {
    std::cerr << "live plugin parameter/bypass controls are missing\n";
    return 1;
  }
  livePluginTable->setSelection(livePluginTable->itemAt(0));

  auto dismissMessage = [&](const QString &title, QString &message) {
    auto *poll = new QTimer(&livePluginWindow);
    poll->setInterval(10);
    QObject::connect(
        poll, &QTimer::timeout, &livePluginWindow,
        [&livePluginWindow, poll, &message, title] {
          auto *active =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
          QList<QMessageBox *> candidates;
          if (active)
            candidates.push_back(active);
          candidates.append(livePluginWindow.findChildren<QMessageBox *>());
          for (auto *candidate : candidates) {
            if (!candidate->windowTitle().contains(title))
              continue;

            message = candidate->text();
            candidate->accept();

            poll->stop();
            poll->deleteLater();
            QTimer::singleShot(0, &livePluginWindow, [&livePluginWindow] {
              for (auto *dialog : livePluginWindow.findChildren<QDialog *>())
                if (dialog->objectName() == "pluginParameterEditor") {
                  dialog->reject();
                  break;
                }
            });
            return;
          }
        });
    poll->start();
  };
  auto waitFor = [](const std::function<bool()> &condition) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < 3000 && !condition()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    return condition();
  };
  auto openLiveParameterDialog = [&] {
    bool applied = false;
    QString acknowledgement;
    dismissMessage(QStringLiteral("Apply plugin parameters live"),
                   acknowledgement);
    auto *editorPoll = new QTimer(&livePluginWindow);
    editorPoll->setInterval(10);
    QObject::connect(editorPoll, &QTimer::timeout, &livePluginWindow, [&] {
      for (auto *candidate : livePluginWindow.findChildren<QDialog *>()) {
        if (candidate->objectName() != "pluginParameterEditor")
          continue;
        auto *gain =
            candidate->findChild<QDoubleSpinBox *>("pluginParameter_gain");
        auto *mix =
            candidate->findChild<QDoubleSpinBox *>("pluginParameter_mix");
        auto *apply =
            candidate->findChild<QPushButton *>("applyPluginParametersLive");
        if (gain && mix && apply) {

          gain->setValue(0.75);
          mix->setValue(0.5);

          apply->click();

          applied = true;
        }
        editorPoll->stop();
        editorPoll->deleteLater();
        return;
      }
    });
    editorPoll->start();

    livePluginParameters->trigger();

    const bool dialogApplied = waitFor([&] { return applied; });

    const bool messageDismissed =
        waitFor([&] { return !acknowledgement.isEmpty(); });

    return dialogApplied && messageDismissed && !acknowledgement.isEmpty();
  };
  if (!openLiveParameterDialog() || livePluginWindow.isWindowModified()) {
    std::cerr << "live apply did not acknowledge updates or preserve the "
                 "unsaved configuration distinction\n";
    return 1;
  }

  QFile livePluginControlLog(livePluginLog);
  const QByteArray expectedLiveParameterCalls =
      "set\ttest.gain.plugin\tgain\t0.75\n"
      "set\ttest.gain.plugin\tmix\t0.5\n";
  if (!livePluginControlLog.open(QIODevice::ReadOnly) ||
      livePluginControlLog.readAll() != expectedLiveParameterCalls) {
    std::cerr << "live GUI apply did not issue exact plugin set CLI calls\n";
    return 1;
  }
  QFile unchangedLivePluginConfig(livePluginConfigPath);
  if (!unchangedLivePluginConfig.open(QIODevice::ReadOnly) ||
      unchangedLivePluginConfig.readAll() != livePluginConfigBytes) {
    std::cerr << "live plugin apply unexpectedly rewrote the configuration\n";
    return 1;
  }

  QString bypassAcknowledgement;

  dismissMessage(QStringLiteral("Plugin bypass"), bypassAcknowledgement);
  liveBypassOn->trigger();

  if (!waitFor([&] { return !bypassAcknowledgement.isEmpty(); }) ||
      !bypassAcknowledgement.contains("bypass to on")) {
    std::cerr << "GUI bypass-on did not show the daemon acknowledgement\n";
    return 1;
  }
  QFile bypassLog(livePluginLog);
  const QByteArray expectedBypassCalls =
      expectedLiveParameterCalls + "bypass\ttest.gain.plugin\ton\n";
  if (!bypassLog.open(QIODevice::ReadOnly) ||
      bypassLog.readAll() != expectedBypassCalls) {
    std::cerr << "GUI bypass-on did not issue the exact plugin bypass call\n";
    return 1;
  }

  QString liveApplyError;

  dismissMessage(QStringLiteral("Plugin parameter update failed"),
                 liveApplyError);
  bool failedApplyWasTriggered = false;
  auto *failedEditorPoll = new QTimer(&livePluginWindow);
  failedEditorPoll->setInterval(10);
  QObject::connect(failedEditorPoll, &QTimer::timeout, &livePluginWindow, [&] {
    for (auto *candidate : livePluginWindow.findChildren<QDialog *>()) {
      if (candidate->objectName() != "pluginParameterEditor")
        continue;
      auto *gain =
          candidate->findChild<QDoubleSpinBox *>("pluginParameter_gain");
      auto *apply =
          candidate->findChild<QPushButton *>("applyPluginParametersLive");
      if (gain && apply) {

        gain->setValue(0.66);
        apply->click();

        failedApplyWasTriggered = true;
      }
      failedEditorPoll->stop();
      failedEditorPoll->deleteLater();
      return;
    }
  });
  failedEditorPoll->start();
  livePluginParameters->trigger();

  if (!waitFor([&] { return failedApplyWasTriggered; }) || !waitFor([&] {
        return liveApplyError.contains("fixture live-set error");
      })) {
    std::cerr << "live plugin failure was not surfaced with CLI error detail\n";
    return 1;
  }

  QString bypassError;

  dismissMessage(QStringLiteral("Plugin bypass failed"), bypassError);
  liveBypassOff->trigger();

  if (!waitFor([&] { return bypassError.contains("fixture bypass error"); })) {
    std::cerr << "plugin bypass failure was not surfaced with CLI error "
                 "detail\n";
    return 1;
  }

  const QString nonPluginConfigPath =
      temporary.filePath("non-plugin-selection.txt");
  QFile nonPluginConfig(nonPluginConfigPath);
  const QByteArray nonPluginConfigBytes = "Preamp: -6 dB\n";
  if (!nonPluginConfig.open(QIODevice::WriteOnly) ||
      nonPluginConfig.write(nonPluginConfigBytes) !=
          nonPluginConfigBytes.size()) {
    std::cerr << "could not create non-plugin selection fixture\n";
    return 1;
  }
  nonPluginConfig.close();
  MainWindow nonPluginWindow(nonPluginConfigPath, livePluginCli);
  auto *nonPluginTable = nonPluginWindow.findChild<FilterTable *>();
  auto *nonPluginBypass =
      nonPluginWindow.findChild<QAction *>("setPluginBypassOn");
  if (!nonPluginTable || !nonPluginTable->itemAt(0) || !nonPluginBypass) {
    std::cerr << "non-plugin validation fixture is invalid\n";
    return 1;
  }
  nonPluginTable->setSelection(nonPluginTable->itemAt(0));
  const QString expectedNonPluginMessage =
      QStringLiteral("Select a native LV2, CLAP, or VST3 Plugin: row.");
  QString invalidSelectionMessage;
  QFile controlLogBeforeNonPlugin(livePluginLog);
  if (!controlLogBeforeNonPlugin.open(QIODevice::ReadOnly)) {
    std::cerr << "could not read plugin control log before non-plugin check\n";
    return 1;
  }
  const QByteArray controlCallsBeforeNonPlugin =
      controlLogBeforeNonPlugin.readAll();
  auto *invalidSelectionPoll = new QTimer(&nonPluginWindow);
  invalidSelectionPoll->setInterval(10);
  QObject::connect(
      invalidSelectionPoll, &QTimer::timeout, &nonPluginWindow, [&] {
        QList<QMessageBox *> candidates;
        if (auto *active =
                qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
          candidates.push_back(active);
        for (auto *widget : QApplication::topLevelWidgets()) {
          if (auto *message = qobject_cast<QMessageBox *>(widget))
            candidates.push_back(message);
          candidates.append(widget->findChildren<QMessageBox *>());
        }
        for (auto *candidate : candidates) {
          if (candidate->text().contains(expectedNonPluginMessage)) {
            invalidSelectionMessage = candidate->text();
            candidate->accept();
            invalidSelectionPoll->stop();
            break;
          }
        }
      });
  invalidSelectionPoll->start();
  QTimer::singleShot(3000, &nonPluginWindow, [] {
    if (auto *active =
            qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
      active->reject();
  });
  nonPluginBypass->trigger();
  invalidSelectionPoll->stop();
  invalidSelectionPoll->deleteLater();
  QFile controlLogAfterNonPlugin(livePluginLog);
  if (!invalidSelectionMessage.contains(expectedNonPluginMessage) ||
      !controlLogAfterNonPlugin.open(QIODevice::ReadOnly) ||
      controlLogAfterNonPlugin.readAll() != controlCallsBeforeNonPlugin) {
    std::cerr << "plugin bypass accepted a non-plugin selected row\n";
    return 1;
  }
  qunsetenv("SKYAPO_UI_TEST_PLUGIN_CONTROL_LOG");
  MainWindow stalledCliWindow(configPath, QString::fromLocal8Bit(argv[1]));
  auto cliTimeouts = stalledCliWindow.findChildren<QTimer *>("cliTimeout");
  if (cliTimeouts.isEmpty()) {
    std::cerr << "CLI requests did not receive a bounded-timeout timer\n";
    return 1;
  }
  for (auto *timeout : cliTimeouts)
    timeout->setInterval(20);
  auto *stalledStatus = stalledCliWindow.findChild<QLabel *>("daemonStatus");
  QElapsedTimer timeoutWait;
  timeoutWait.start();
  while (timeoutWait.elapsed() < 1500 &&
         (!stalledStatus ||
          !stalledStatus->text().contains("timed out", Qt::CaseInsensitive))) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!stalledStatus ||
      !stalledStatus->text().contains("timed out", Qt::CaseInsensitive)) {
    std::cerr << "hung CLI request did not surface a timeout in the UI: "
              << (stalledStatus ? stalledStatus->text().toStdString()
                                : "status label missing")
              << '\n';
    return 1;
  }
  const QString deviceFailureScript =
      temporary.filePath(QStringLiteral("device-query-failure-cli.sh"));
  if (!writeDeviceQueryFailureCliFixture(deviceFailureScript)) {
    std::cerr << "could not create device-query failure fixture script\n";
    return 1;
  }
  MainWindow deviceFailureWindow(configPath, deviceFailureScript);
  auto *deviceFailureStatus =
      deviceFailureWindow.findChild<QLabel *>("daemonStatus");
  QElapsedTimer deviceFailureWait;
  deviceFailureWait.start();
  while (deviceFailureWait.elapsed() < 1500 &&
         (!deviceFailureStatus ||
          !deviceFailureStatus->text().contains("Device query failed:"))) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  const QString expectedDeviceFailure =
      QStringLiteral("Device query failed: enumeration fixture detail");
  if (!deviceFailureStatus ||
      deviceFailureStatus->text() != expectedDeviceFailure ||
      deviceFailureStatus->accessibleDescription() != expectedDeviceFailure) {
    std::cerr << "device enumeration failure was not surfaced accessibly with "
                 "the CLI diagnostic: "
              << (deviceFailureStatus
                      ? deviceFailureStatus->text().toStdString()
                      : "status label missing")
              << '\n';
    return 1;
  }
  const QString daemonControlScript =
      temporary.filePath(QStringLiteral("daemon-control-cli.sh"));
  const QString daemonControlLog =
      temporary.filePath(QStringLiteral("daemon-control.log"));
  const QString daemonControlGateDir =
      temporary.filePath(QStringLiteral("daemon-control-gates"));
  if (!QDir().mkpath(daemonControlGateDir) ||
      !writeDaemonControlCliFixture(daemonControlScript)) {
    std::cerr << "could not create daemon control CLI fixture\n";
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_CONTROL_LOG", daemonControlLog.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_CONTROL_GATE_DIR",
          daemonControlGateDir.toLocal8Bit());
  MainWindow daemonControlWindow(configPath, daemonControlScript);
  auto *startDaemon =
      daemonControlWindow.findChild<QPushButton *>("startDaemon");
  auto *stopDaemon = daemonControlWindow.findChild<QPushButton *>("stopDaemon");
  if (!startDaemon || !stopDaemon) {
    std::cerr << "daemon control buttons are missing stable test identifiers\n";
    return 1;
  }
  startDaemon->click();
  if (startDaemon->isEnabled() || stopDaemon->isEnabled()) {
    std::cerr << "Start did not serialize daemon control actions\n";
    return 1;
  }
  stopDaemon->click();
  const auto waitForControlLog = [&](const QByteArray &expected) {
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 2000) {
      QFile log(daemonControlLog);
      if (log.open(QIODevice::ReadOnly) && log.readAll().contains(expected))
        return true;
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    return false;
  };
  const auto releaseDaemonControl = [&](const QString &command) {
    QFile gate(daemonControlGateDir + QLatin1Char('/') + command);
    return gate.open(QIODevice::WriteOnly);
  };
  if (!waitForControlLog("start\n")) {
    std::cerr << "Start request did not reach the delayed CLI fixture\n";
    return 1;
  }
  {
    QFile log(daemonControlLog);
    if (!log.open(QIODevice::ReadOnly) || log.readAll() != "start\n") {
      std::cerr << "Stop overlapped the still-pending Start request\n";
      return 1;
    }
  }
  if (!releaseDaemonControl(QStringLiteral("start"))) {
    std::cerr << "could not release delayed Start fixture\n";
    return 1;
  }
  QElapsedTimer controlCompletionWait;
  controlCompletionWait.start();
  while (controlCompletionWait.elapsed() < 2000 &&
         (!startDaemon->isEnabled() || !stopDaemon->isEnabled())) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!startDaemon->isEnabled() || !stopDaemon->isEnabled()) {
    std::cerr << "daemon controls did not unlock after Start completed\n";
    return 1;
  }
  stopDaemon->click();
  if (startDaemon->isEnabled() || stopDaemon->isEnabled() ||
      !waitForControlLog("stop\n")) {
    std::cerr << "Stop did not start after the prior control completed\n";
    return 1;
  }
  if (!releaseDaemonControl(QStringLiteral("stop"))) {
    std::cerr << "could not release delayed Stop fixture\n";
    return 1;
  }
  controlCompletionWait.restart();
  while (controlCompletionWait.elapsed() < 2000 &&
         (!startDaemon->isEnabled() || !stopDaemon->isEnabled())) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  QFile daemonControlResults(daemonControlLog);
  if (!startDaemon->isEnabled() || !stopDaemon->isEnabled() ||
      !daemonControlResults.open(QIODevice::ReadOnly) ||
      daemonControlResults.readAll() != "start\nstop\n") {
    std::cerr << "Start/Stop requests were not serialized in user order\n";
    return 1;
  }
  qunsetenv("SKYAPO_UI_TEST_CONTROL_LOG");
  qunsetenv("SKYAPO_UI_TEST_CONTROL_GATE_DIR");
  const QString configWorkflowScript =
      temporary.filePath(QStringLiteral("config-workflow-cli.sh"));
  const QString configWorkflowLog =
      temporary.filePath(QStringLiteral("config-workflow.log"));
  const QString configWorkflowGateDir =
      temporary.filePath(QStringLiteral("config-workflow-gates"));
  const QString reloadMarker =
      temporary.filePath(QStringLiteral("config-reloaded"));
  if (!QDir().mkpath(configWorkflowGateDir) ||
      !writeConfigWorkflowCliFixture(configWorkflowScript)) {
    std::cerr << "could not create config workflow CLI fixture\n";
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_CONFIG_LOG", configWorkflowLog.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_CONFIG_GATE_DIR",
          configWorkflowGateDir.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_RELOAD_MARKER", reloadMarker.toLocal8Bit());
  QString capturedDialogTitle;
  QString capturedDialogText;
  QTimer modalCapture;
  modalCapture.setInterval(5);
  QObject::connect(&modalCapture, &QTimer::timeout, [&] {
    auto *message =
        qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (!message || !capturedDialogTitle.isEmpty())
      return;
    capturedDialogTitle = message->windowTitle();
    capturedDialogText = message->text();
    if (auto *ok = message->button(QMessageBox::Ok))
      ok->click();
  });
  modalCapture.start();
  unsigned workflowHeartbeat = 0;
  QTimer workflowPulse;
  workflowPulse.setInterval(10);
  QObject::connect(&workflowPulse, &QTimer::timeout,
                   [&workflowHeartbeat] { ++workflowHeartbeat; });
  workflowPulse.start();
  const auto writeWorkflowConfig = [](const QString &path,
                                      const QByteArray &contents) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(contents) == contents.size();
  };
  const auto waitForWorkflowFile = [](const QString &path) {
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 2000 && !QFileInfo::exists(path)) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    return QFileInfo::exists(path);
  };
  const auto waitForResponsiveHeartbeat = [&](unsigned previous) {
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 60) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    return workflowHeartbeat > previous;
  };
  const auto waitForDialog = [&](const QString &title) {
    QElapsedTimer wait;
    wait.start();
    while (wait.elapsed() < 2000 && capturedDialogTitle.isEmpty()) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    return capturedDialogTitle == title;
  };
  const auto createWorkflowGate = [&](const QString &name) {
    QFile gate(configWorkflowGateDir + QLatin1Char('/') + name);
    return gate.open(QIODevice::WriteOnly);
  };

  const QString checkConfigPath = temporary.filePath("workflow-check.txt");
  if (!writeWorkflowConfig(checkConfigPath, "Preamp: -1 dB\n")) {
    std::cerr << "could not create config-check fixture\n";
    return 1;
  }
  MainWindow checkWindow(checkConfigPath, configWorkflowScript);
  auto *checkGain = checkWindow.findChild<PreampFilterGUI *>()
                        ? checkWindow.findChild<PreampFilterGUI *>()
                              ->findChild<QDoubleSpinBox *>("doubleSpinBox")
                        : nullptr;
  QPushButton *checkButton = nullptr;
  for (auto *button : checkWindow.findChildren<QPushButton *>())
    if (button->text() == "Save & Check")
      checkButton = button;
  if (!checkGain || !checkButton) {
    std::cerr << "config-check controls were not created\n";
    return 1;
  }
  checkGain->setValue(-8.0);
  checkButton->click();
  QFile savedCheckConfig(checkConfigPath);
  if (!savedCheckConfig.open(QIODevice::ReadOnly) ||
      savedCheckConfig.readAll() != "Preamp: -8 dB\n") {
    std::cerr << "Save & Check invoked the CLI before saving editor changes\n";
    return 1;
  }
  const unsigned checkHeartbeat = workflowHeartbeat;
  if (!waitForWorkflowFile(configWorkflowGateDir + "/check-ready") ||
      !waitForResponsiveHeartbeat(checkHeartbeat) ||
      !createWorkflowGate("check") || !waitForDialog("Configuration valid") ||
      !capturedDialogText.contains("Valid config: fixture")) {
    std::cerr << "successful Save & Check did not remain responsive or show "
                 "the CLI result\n";
    return 1;
  }
  std::cerr << "workflow-check-success\n";

  const QString rejectedConfigPath =
      temporary.filePath("workflow-reject-config.txt");
  if (!writeWorkflowConfig(rejectedConfigPath, "Preamp: 0 dB\n")) {
    std::cerr << "could not create rejected config-check fixture\n";
    return 1;
  }
  capturedDialogTitle.clear();
  capturedDialogText.clear();
  QFile::remove(configWorkflowGateDir + "/check-ready");
  QFile::remove(configWorkflowGateDir + "/check");
  MainWindow rejectedCheckWindow(rejectedConfigPath, configWorkflowScript);
  QPushButton *rejectedCheckButton = nullptr;
  for (auto *button : rejectedCheckWindow.findChildren<QPushButton *>())
    if (button->text() == "Save & Check")
      rejectedCheckButton = button;
  if (!rejectedCheckButton) {
    std::cerr << "Save & Check button was missing for rejected config\n";
    return 1;
  }
  rejectedCheckButton->click();
  if (!waitForWorkflowFile(configWorkflowGateDir + "/check-ready") ||
      !createWorkflowGate("check") || !waitForDialog("Configuration error") ||
      !capturedDialogText.contains("fixture config rejected")) {
    std::cerr << "failed Save & Check did not display the CLI diagnostic\n";
    return 1;
  }
  std::cerr << "workflow-check-failure\n";

  const QString reloadConfigPath = temporary.filePath("workflow-reload.txt");
  if (!writeWorkflowConfig(reloadConfigPath, "Preamp: -1 dB\n")) {
    std::cerr << "could not create config-reload fixture\n";
    return 1;
  }
  capturedDialogTitle.clear();
  capturedDialogText.clear();
  QFile::remove(configWorkflowGateDir + "/reload-ready");
  QFile::remove(configWorkflowGateDir + "/reload");
  MainWindow reloadWindow(reloadConfigPath, configWorkflowScript);
  auto *reloadPreamp = reloadWindow.findChild<PreampFilterGUI *>();
  auto *reloadGain =
      reloadPreamp ? reloadPreamp->findChild<QDoubleSpinBox *>("doubleSpinBox")
                   : nullptr;
  QPushButton *reloadButton = nullptr;
  for (auto *button : reloadWindow.findChildren<QPushButton *>())
    if (button->text() == "Save & Reload")
      reloadButton = button;
  if (!reloadGain || !reloadButton) {
    std::cerr << "config-reload controls were not created\n";
    return 1;
  }
  reloadGain->setValue(-5.0);
  reloadButton->click();
  QFile savedReloadConfig(reloadConfigPath);
  if (!savedReloadConfig.open(QIODevice::ReadOnly) ||
      savedReloadConfig.readAll() != "Preamp: -5 dB\n") {
    std::cerr << "Save & Reload invoked the CLI before saving editor changes\n";
    return 1;
  }
  const unsigned reloadHeartbeat = workflowHeartbeat;
  if (!waitForWorkflowFile(configWorkflowGateDir + "/reload-ready") ||
      !waitForResponsiveHeartbeat(reloadHeartbeat) ||
      !createWorkflowGate("reload") || !waitForDialog("Reload requested") ||
      !capturedDialogText.contains("Config reload succeeded")) {
    std::cerr << "successful Save & Reload did not remain responsive or show "
                 "the CLI result\n";
    return 1;
  }
  std::cerr << "workflow-reload-success\n";
  QElapsedTimer statusRefreshWait;
  statusRefreshWait.start();
  auto *reloadedStatus = reloadWindow.findChild<QLabel *>("daemonStatus");
  while (statusRefreshWait.elapsed() < 2000 &&
         (!reloadedStatus ||
          !reloadedStatus->text().contains("Daemon: reloaded"))) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!reloadedStatus || !reloadedStatus->text().contains("Daemon: reloaded")) {
    std::cerr << "successful config reload did not refresh daemon status\n";
    return 1;
  }

  capturedDialogTitle.clear();
  capturedDialogText.clear();
  QFile::remove(configWorkflowGateDir + "/reload-ready");
  QFile::remove(configWorkflowGateDir + "/reload");
  QFile reloadFailureGate(configWorkflowGateDir + "/reload-fail");
  if (!reloadFailureGate.open(QIODevice::WriteOnly)) {
    std::cerr << "could not create failed reload fixture state\n";
    return 1;
  }
  MainWindow failedReloadWindow(reloadConfigPath, configWorkflowScript);
  QPushButton *failedReloadButton = nullptr;
  for (auto *button : failedReloadWindow.findChildren<QPushButton *>())
    if (button->text() == "Save & Reload")
      failedReloadButton = button;
  if (!failedReloadButton) {
    std::cerr << "Save & Reload button was missing for failure fixture\n";
    return 1;
  }
  failedReloadButton->click();
  if (!waitForWorkflowFile(configWorkflowGateDir + "/reload-ready") ||
      !createWorkflowGate("reload") || !waitForDialog("Reload failed") ||
      !capturedDialogText.contains("fixture reload rejected")) {
    std::cerr << "failed Save & Reload did not display the CLI diagnostic\n";
    return 1;
  }
  std::cerr << "workflow-reload-failure\n";

  const QString stageConfigPath = temporary.filePath("stage-config.txt");
  QFile stageConfig(stageConfigPath);
  if (!stageConfig.open(QIODevice::WriteOnly) ||
      stageConfig.write("Stage: pre-mix capture\n") < 0) {
    std::cerr << "could not create Stage editor UI fixture\n";
    return 1;
  }
  stageConfig.close();
  MainWindow stageWindow(stageConfigPath, QString::fromLocal8Bit(argv[1]));
  auto *stagePreMix = stageWindow.findChild<QCheckBox *>("preMixCheckBox");
  auto *stagePostMix = stageWindow.findChild<QCheckBox *>("postMixCheckBox");
  auto *stageCapture = stageWindow.findChild<QCheckBox *>("captureCheckBox");
  if (!stagePreMix || !stagePostMix || !stageCapture ||
      stagePreMix->isEnabled() || !stagePostMix->isEnabled() ||
      !stageCapture->isEnabled() ||
      !stagePreMix->toolTip().contains("per-application pre-mix",
                                       Qt::CaseInsensitive) ||
      stagePostMix->toolTip().contains("does not process")) {
    std::cerr
        << "Stage editor did not expose supported post-mix and distinguish "
           "unsupported per-application pre-mix (pre="
        << (stagePreMix ? stagePreMix->isEnabled() : false)
        << ", post=" << (stagePostMix ? stagePostMix->isEnabled() : false)
        << ", capture=" << (stageCapture ? stageCapture->isEnabled() : false)
        << ", pre tooltip="
        << (stagePreMix ? stagePreMix->toolTip().toStdString() : "missing")
        << ", post tooltip="
        << (stagePostMix ? stagePostMix->toolTip().toStdString() : "missing")
        << ")\n";
    return 1;
  }
  stagePostMix->setChecked(true);
  QPushButton *saveStageConfig = nullptr;
  for (auto *button : stageWindow.findChildren<QPushButton *>())
    if (button->text() == "Save")
      saveStageConfig = button;
  if (!saveStageConfig) {
    std::cerr << "Stage editor did not expose Save\n";
    return 1;
  }
  saveStageConfig->click();
  QFile savedStageConfig(stageConfigPath);
  if (!savedStageConfig.open(QIODevice::ReadOnly) ||
      savedStageConfig.readAll() != "Stage: pre-mix post-mix capture\n") {
    std::cerr << "Stage post-mix checkbox did not serialize to the config\n";
    return 1;
  }

  const QString outputDeviceLogPath =
      temporary.filePath(QStringLiteral("selector-output-device.log"));
  const QString restartLogPath =
      temporary.filePath(QStringLiteral("selector-restart.log"));
  const QString polkitLogPath =
      temporary.filePath(QStringLiteral("selector-polkit.log"));
  const QString polkitFixturePath =
      temporary.filePath(QStringLiteral("pkcheck-fixture"));
  if (!writePolkitPreflightFixture(polkitFixturePath)) {
    std::cerr << "could not create the Polkit preflight fixture\n";
    return 1;
  }
  QFile::remove(outputDeviceLogPath);
  QFile::remove(restartLogPath);
  QFile::remove(deviceLogPath);
  QFile::remove(polkitLogPath);
  qputenv("SKYAPO_UI_TEST_POLKIT_LOG", polkitLogPath.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_POLKIT_RESULT", "1");
  qputenv("SKYAPO_UI_TEST_OUTPUT_DEVICE_LOG",
          outputDeviceLogPath.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_RESTART_LOG", restartLogPath.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_DAEMON_RUNNING", "1");
  qputenv("SKYAPO_UI_TEST_DEVICE_LOG", deviceLogPath.toLocal8Bit());
  qputenv("SKYAPO_UI_TEST_DEVICE_LIST_FILE",
          dynamicDeviceListPath.toLocal8Bit());
  MainWindow selectorWindow(configPath, QString::fromLocal8Bit(argv[1]),
                            polkitFixturePath);
  auto *openDeviceSelector =
      selectorWindow.findChild<QPushButton *>("openDeviceSelector");
  if (!openDeviceSelector) {
    std::cerr << "Configuration Editor did not expose Device Selector\n";
    return 1;
  }

  QElapsedTimer authorizationElapsed;
  authorizationElapsed.start();
  QTimer authorizationProbe;
  authorizationProbe.setInterval(10);
  bool authorizationDeniedShown = false;
  QString authorizationFailure;
  QObject::connect(&authorizationProbe, &QTimer::timeout, &selectorWindow, [&] {
    if (authorizationElapsed.elapsed() > 5000) {
      authorizationFailure =
          QStringLiteral("Polkit denial dialog did not appear");
      authorizationProbe.stop();
      return;
    }
    auto *warning = selectorWindow.findChild<QMessageBox *>(
        "deviceSelectorAuthorizationError");
    if (!warning || !warning->isVisible())
      return;
    if (!warning->text().contains("not opened", Qt::CaseInsensitive)) {
      authorizationFailure =
          QStringLiteral("Polkit denial message was unclear");
      warning->done(QMessageBox::Ok);
      authorizationProbe.stop();
      return;
    }
    authorizationDeniedShown = true;
    warning->done(QMessageBox::Ok);
    authorizationProbe.stop();
  });
  authorizationProbe.start();
  openDeviceSelector->click();
  QElapsedTimer authorizationWait;
  authorizationWait.start();
  while (!authorizationDeniedShown && authorizationFailure.isEmpty() &&
         authorizationWait.elapsed() < 6000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  authorizationProbe.stop();
  QFile polkitLog(polkitLogPath);
  if (!authorizationDeniedShown || !authorizationFailure.isEmpty() ||
      selectorWindow.findChild<QDialog *>("deviceSelectorDialog") ||
      !polkitLog.open(QIODevice::ReadOnly)) {
    std::cerr << (authorizationFailure.isEmpty()
                      ? "Device Selector did not fail closed on denied Polkit "
                        "authorization"
                      : authorizationFailure.toStdString())
              << '\n';
    return 1;
  }
  const QByteArray deniedPolkitArguments = polkitLog.readAll();
  if (!deniedPolkitArguments.contains("org.linuxapo.device-selector.open") ||
      !deniedPolkitArguments.contains("--process") ||
      !deniedPolkitArguments.contains(
          QByteArray::number(QCoreApplication::applicationPid()))) {
    std::cerr << "Polkit preflight did not use the fixed action and current UI "
                 "process\n";
    return 1;
  }

  qputenv("SKYAPO_UI_TEST_POLKIT_RESULT", "0");
  QElapsedTimer selectorElapsed;
  selectorElapsed.start();
  QTimer selectorProbe;
  selectorProbe.setInterval(25);
  int selectorStep = 0;
  bool selectorPassed = false;
  QString selectorFailure;
  QObject::connect(&selectorProbe, &QTimer::timeout, &selectorWindow, [&] {
    if (selectorElapsed.elapsed() > 20000 && !selectorPassed &&
        selectorFailure.isEmpty()) {
      selectorFailure =
          QStringLiteral(
              "Device Selector integration timed out before completing step %1")
              .arg(selectorStep);
      auto *activeDialog =
          qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (activeDialog)
        activeDialog->reject();
      return;
    }
    auto *selector =
        selectorWindow.findChild<QDialog *>("deviceSelectorDialog");
    if (!selector)
      return;
    auto *captureTable =
        selector->findChild<QTableWidget *>("captureDevicesTable");
    auto *playbackTable =
        selector->findChild<QTableWidget *>("playbackDevicesTable");
    auto *status = selector->findChild<QLabel *>("deviceSelectorRuntimeStatus");
    auto *troubleshooting = selector->findChild<QToolButton *>(
        "deviceSelectorTroubleshootingToggle");
    auto *selectCapture =
        selector->findChild<QPushButton *>("selectCaptureDevice");
    auto *selectPlayback =
        selector->findChild<QPushButton *>("selectPlaybackDevice");
    auto *closeSelector =
        selector->findChild<QPushButton *>("closeDeviceSelector");
    auto *refreshTimer =
        selector->findChild<QTimer *>("deviceSelectorRefreshTimer");
    if (!captureTable || !playbackTable || !status || !troubleshooting ||
        !selectCapture || !selectPlayback || !closeSelector || !refreshTimer)
      return;
    if (refreshTimer->interval() != 250)
      refreshTimer->setInterval(250);
    if (selectorElapsed.elapsed() > 20000) {
      selectorFailure =
          QStringLiteral("Device Selector integration timed out (capture "
                         "rows=%1, playback rows=%2, status=%3)")
              .arg(captureTable->rowCount())
              .arg(playbackTable->rowCount())
              .arg(status->text());
      selector->reject();
      return;
    }
    if (selectorStep == 0) {
      if (troubleshooting->isChecked() || status->isVisible()) {
        selectorFailure = "Device Selector troubleshooting details are not "
                          "collapsed by default";
        selector->reject();
        return;
      }
      troubleshooting->click();
      if (!troubleshooting->isChecked() || !status->isVisible()) {
        selectorFailure =
            "Device Selector troubleshooting details did not expand";
        selector->reject();
        return;
      }
      troubleshooting->click();
      if (captureTable->rowCount() != 2 || playbackTable->rowCount() != 2 ||
          !status->text().contains("Daemon: streaming") ||
          !status->text().contains("fixture.sink"))
        return;
      if (captureTable->currentRow() != 1 || playbackTable->currentRow() != 0 ||
          captureTable->item(0, 2)->data(Qt::UserRole).toString() !=
              "fixture.capture" ||
          playbackTable->item(1, 2)->data(Qt::UserRole).toString() !=
              "fixture.sink.alt") {
        selectorFailure =
            "Device Selector did not display capture/playback fixture state";
        selector->reject();
        return;
      }
      captureTable->selectRow(0);
      selectCapture->click();
      if (!writeDynamicDeviceList(
              "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE "
              "RATE\tSTABLE PROPERTY\tDEVICE BUS ID\tALSA CARD\tALSA "
              "PATH\tDEFAULT\n"
              "41\tfixture.capture\tBuilt-in capture\tyes\t2\t48000\t"
              "\t\t\t\tno\n"
              "42\tfixture.usb-mic\tUSB microphone\t\t1\t44100\t"
              "\t\t\t\tyes\n")) {
        selectorFailure =
            QStringLiteral("could not update the capture selection fixture");
        selector->reject();
        return;
      }
      selectorStep = 1;
      return;
    }
    if (selectorStep == 1) {
      QFile selectionLog(deviceLogPath);
      if (!selectionLog.open(QIODevice::ReadOnly) ||
          !selectionLog.readAll().contains("fixture.capture"))
        return;
      QFile restartLog(restartLogPath);
      if (!restartLog.open(QIODevice::ReadOnly) ||
          !restartLog.readAll().contains("restart"))
        return;
      playbackTable->selectRow(1);
      selectPlayback->click();
      selectorStep = 2;
      return;
    }
    if (selectorStep == 2) {
      QFile selectionLog(outputDeviceLogPath);
      QFile restartLog(restartLogPath);
      if (!selectionLog.open(QIODevice::ReadOnly) ||
          !selectionLog.readAll().contains("fixture.sink.alt") ||
          !restartLog.open(QIODevice::ReadOnly) ||
          restartLog.readAll().split('\n').size() != 2 ||
          playbackTable->currentRow() != 1 ||
          playbackTable->item(1, 0)->text() != "Selected")
        return;
      if (!writeDynamicDeviceList(
              "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE "
              "RATE\tSTABLE PROPERTY\tDEVICE BUS ID\tALSA CARD\tALSA "
              "PATH\tDEFAULT\n"
              "42\tfixture.usb-mic\tUSB microphone\t\t1\t44100\t"
              "\t\t\t\tyes\n")) {
        selectorFailure =
            QStringLiteral("could not simulate selected capture hot-unplug");
        selector->reject();
        return;
      }
      selectorStep = 3;
      return;
    }
    if (selectorStep == 3) {
      if (captureTable->rowCount() != 2 ||
          captureTable->item(0, 2)->data(Qt::UserRole).toString() !=
              "fixture.usb-mic" ||
          captureTable->currentRow() != 1 ||
          captureTable->item(1, 0)->text() != "Unavailable" ||
          selectCapture->isEnabled() ||
          !status->text().contains("fixture.capture (unavailable)"))
        return;
      if (!writeDynamicDeviceList(
              "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE "
              "RATE\tSTABLE PROPERTY\tDEVICE BUS ID\tALSA CARD\tALSA "
              "PATH\tDEFAULT\n"
              "41\tfixture.capture\tBuilt-in capture\tyes\t2\t48000\t"
              "\t\t\t\tno\n"
              "42\tfixture.usb-mic\tUSB microphone\t\t1\t44100\t"
              "\t\t\t\tyes\n")) {
        selectorFailure =
            QStringLiteral("could not simulate selected capture reappearance");
        selector->reject();
        return;
      }
      selectorStep = 4;
      return;
    }
    if (selectorStep == 4) {
      if (captureTable->rowCount() != 2 || captureTable->currentRow() != 0 ||
          captureTable->item(0, 0)->text() != "Selected")
        return;
      selectorPassed = true;
      closeSelector->click();
      selectorProbe.stop();
    }
  });
  selectorProbe.start();
  openDeviceSelector->click();
  QElapsedTimer waitForSelector;
  waitForSelector.start();
  while (!selectorPassed && selectorFailure.isEmpty() &&
         waitForSelector.elapsed() < 22000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  selectorProbe.stop();
  if (!selectorPassed) {
    std::cerr << (selectorFailure.isEmpty()
                      ? "Device Selector failed to load or apply both endpoint "
                        "selections"
                      : selectorFailure.toStdString())
              << '\n';
    return 1;
  }

  qunsetenv("SKYAPO_UI_TEST_DAEMON_RUNNING");
  qunsetenv("SKYAPO_UI_TEST_DEVICE_LIST_FILE");
  QFile::remove(outputDeviceLogPath);
  MainWindow stoppedSelectorWindow(configPath, QString::fromLocal8Bit(argv[1]),
                                   polkitFixturePath);
  auto *openStoppedSelector =
      stoppedSelectorWindow.findChild<QPushButton *>("openDeviceSelector");
  if (!openStoppedSelector) {
    std::cerr << "stopped-daemon Device Selector button is missing\n";
    return 1;
  }
  QElapsedTimer stoppedSelectorElapsed;
  stoppedSelectorElapsed.start();
  QTimer stoppedSelectorProbe;
  stoppedSelectorProbe.setInterval(25);
  bool stoppedSelectorPassed = false;
  bool stoppedSelectorCompleted = false;
  QString stoppedSelectorFailure;
  QObject::connect(
      &stoppedSelectorProbe, &QTimer::timeout, &stoppedSelectorWindow, [&] {
        auto *selector =
            stoppedSelectorWindow.findChild<QDialog *>("deviceSelectorDialog");
        if (!selector)
          return;
        auto *playbackTable =
            selector->findChild<QTableWidget *>("playbackDevicesTable");
        auto *status =
            selector->findChild<QLabel *>("deviceSelectorRuntimeStatus");
        auto *feedback =
            selector->findChild<QLabel *>("deviceSelectionFeedback");
        auto *selectPlayback =
            selector->findChild<QPushButton *>("selectPlaybackDevice");
        auto *closeSelector =
            selector->findChild<QPushButton *>("closeDeviceSelector");
        if (!playbackTable || !status || !feedback || !selectPlayback ||
            !closeSelector)
          return;
        if (stoppedSelectorElapsed.elapsed() > 5000) {
          stoppedSelectorFailure =
              QStringLiteral("stopped-daemon Device Selector timed "
                             "out (playback rows=%1, status=%2, "
                             "feedback=%3, selected=%4)")
                  .arg(playbackTable->rowCount())
                  .arg(status->text())
                  .arg(feedback->text())
                  .arg(stoppedSelectorPassed);
          selector->reject();
          return;
        }
        if (!stoppedSelectorPassed) {
          if (playbackTable->rowCount() != 2 ||
              !status->text().contains("Daemon: not reachable"))
            return;
          playbackTable->selectRow(1);
          selectPlayback->click();
          stoppedSelectorPassed = true;
          return;
        }
        QFile selectionLog(outputDeviceLogPath);
        QFile restartLog(restartLogPath);
        if (!selectionLog.open(QIODevice::ReadOnly) ||
            !selectionLog.readAll().contains("fixture.sink.alt") ||
            !feedback->text().contains("next time skyapod is started") ||
            !restartLog.open(QIODevice::ReadOnly) ||
            restartLog.readAll().split('\n').size() != 2)
          return;
        closeSelector->click();
        stoppedSelectorCompleted = true;
        stoppedSelectorProbe.stop();
      });
  stoppedSelectorProbe.start();
  openStoppedSelector->click();
  QElapsedTimer waitForStoppedSelector;
  waitForStoppedSelector.start();
  while (!stoppedSelectorCompleted && stoppedSelectorFailure.isEmpty() &&
         waitForStoppedSelector.elapsed() < 7000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  stoppedSelectorProbe.stop();
  if (!stoppedSelectorPassed || !stoppedSelectorCompleted ||
      !stoppedSelectorFailure.isEmpty()) {
    std::cerr << (stoppedSelectorFailure.isEmpty()
                      ? "stopped-daemon selection did not save without restart"
                      : stoppedSelectorFailure.toStdString())
              << '\n';
    return 1;
  }

  modalCapture.stop();
  workflowPulse.stop();
  qunsetenv("SKYAPO_UI_TEST_CONFIG_LOG");
  qunsetenv("SKYAPO_UI_TEST_CONFIG_GATE_DIR");
  qunsetenv("SKYAPO_UI_TEST_RELOAD_MARKER");
  qunsetenv("SKYAPO_UI_TEST_OUTPUT_DEVICE_LOG");
  qunsetenv("SKYAPO_UI_TEST_RESTART_LOG");
  qunsetenv("SKYAPO_UI_TEST_POLKIT_LOG");
  qunsetenv("SKYAPO_UI_TEST_POLKIT_RESULT");
  qunsetenv("SKYAPO_UI_TEST_DAEMON_RUNNING");
  std::cout << "upstream editor widgets, capture/playback device selection, "
               "config preservation, async UI, bounded CLI failure handling, "
               "and GraphicEQ serialization tests passed\n";
  return 0;
}
