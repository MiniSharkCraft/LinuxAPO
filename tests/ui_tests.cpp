#include "ConfigFile.h"
#include "ChannelCopyEditor.h"
#include "ConvolutionEditor.h"
#include "IncludeEditor.h"
#include "IIRFilterEditor.h"
#include "MainWindow.h"
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
#include "ResponseAnalysis.h"
#endif

#include "Editor/FilterTable.h"
#include "Editor/FilterTableRow.h"
#include "Editor/FilterTemplate.h"
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
#include <QCheckBox>
#include <QComboBox>
#include <QAction>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QPlainTextEdit>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QThread>
#include <QMessageBox>
#include <QMenu>
#include <QSettings>
#include <cmath>
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
                           int exitCode = 0) {
  QByteArray script =
      "#!/bin/sh\n"
      "if [ \"$1\" = \"status\" ]; then\n"
      "  printf '%s\\n' 'Daemon: ";
  script += state.toUtf8();
  script += "' 'Selected device: fixture.mic' 'Capture node: 52 (Test mic)' "
            "'Virtual microphone: SkyAPO Virtual Mic' 'Virtual node: 90 "
            "(skyapo.virtual_mic)' 'Format: F32 planar DSP' 'Channels: 2' "
            "'Sample rate: unknown (awaiting graph)' 'Quantum: unknown'\n"
            "  exit ";
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
  return file.open(QIODevice::WriteOnly) && file.write(script) == script.size() &&
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
} // namespace

int main(int argc, char **argv) {
  QApplication application(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("SkyAPOTests"));
  QCoreApplication::setApplicationName(QStringLiteral("SkyAPOUITests"));
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
  if (!channelEditor || channelEditor->kind() != ChannelCopyEditor::Kind::Channel ||
      !channelEditor->findChild<QCheckBox *>("channel_L")->isChecked() ||
      !channelEditor->findChild<QCheckBox *>("channel_LFE")->isChecked()) {
    std::cerr << "Channel visual editor did not load supported EAPO names/alias\n";
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
    std::cerr << "Copy editor serialization/unsupported-expression boundary failed\n";
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
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  const QString responseConfigPath = temporary.filePath("response-config.txt");
  QFile responseConfig(responseConfigPath);
  if (!responseConfig.open(QIODevice::WriteOnly) ||
      responseConfig.write("Preamp: -6 dB\nFilter: ON PK Fc 1000 Hz Gain 6 dB Q 1.0\n") < 0) {
    std::cerr << "could not create response analysis fixture\n";
    return 1;
  }
  responseConfig.close();
  const auto response = analyzeConfigResponse(responseConfigPath);
  if (!response.error.isEmpty() || response.curves.size() != 4) {
    std::cerr << "actual Engine response analysis failed: "
              << response.error.toStdString() << '\n';
    return 1;
  }
  const auto gainAt = [](const ResponseCurve &curve, double frequency) {
    auto closest = curve.points.constBegin();
    for (auto point = curve.points.constBegin(); point != curve.points.constEnd(); ++point)
      if (std::abs(point->x() - frequency) < std::abs(closest->x() - frequency))
        closest = point;
    return closest->y();
  };
  for (const auto &curve : response.curves) {
    if (curve.channel != "L → L" && curve.channel != "R → R")
      continue;
    if (curve.channel == "L → L") {
      std::cout << "Measured Engine response L→L: " << gainAt(curve, 100.0)
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
    std::cerr << "response analysis accepted a plugin config in the editor process\n";
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
  if (!invalidRow ||
      !invalidRow->property("skyapoValidationError").toBool() ||
      !invalidRowNumber || !invalidRowNumber->toolTip().contains(
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
  bool responseDialogOpened = false;
  bool responseDialogFailed = false;
  QTimer responseDialogCloser;
  responseDialogCloser.setInterval(10);
  QObject::connect(&responseDialogCloser, &QTimer::timeout, &application, [&] {
    for (auto *widget : QApplication::topLevelWidgets()) {
      auto *dialog = qobject_cast<QDialog *>(widget);
      if (!dialog)
        continue;
      if (dialog->windowTitle() == "Measured filter response") {
        responseDialogOpened = true;
        dialog->accept();
        responseDialogCloser.stop();
      } else if (dialog->windowTitle() == "Response analysis") {
        responseDialogFailed = true;
        for (auto *label : dialog->findChildren<QLabel *>())
          std::cerr << "response analysis dialog: "
                    << label->text().toStdString() << '\n';
        dialog->accept();
        responseDialogCloser.stop();
      }
    }
  });
  if (!responseButton) {
    std::cerr << "response analysis action is missing from the window\n";
    return 1;
  }
  responseDialogCloser.start();
  responseButton->click();
  QElapsedTimer responseWait;
  responseWait.start();
  while (!responseDialogOpened && !responseDialogFailed &&
         responseWait.elapsed() < 5000) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  if (!responseDialogOpened || responseDialogFailed) {
    std::cerr << "asynchronous GUI response analysis did not open its measured plot\n";
    return 1;
  }
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
          "# included child\nFilter: ON PK Fc 30000 Hz Gain 6 dB Q 1\n") <
          0) {
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
  auto *openDiagnostic = navigationWindow.findChild<QPushButton *>(
      "openDiagnosticSource");
  if (!includeRow ||
      !includeRow->property("skyapoValidationError").toBool() ||
      !includeRow->toolTip().contains(navigationChildPath + ":2") ||
      !openDiagnostic || openDiagnostic->isHidden()) {
    std::cerr << "included-file diagnostic was not mapped to its root Include row\n";
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
      auto *path = candidate
                       ? candidate->findChild<QLineEdit *>("configPath")
                       : nullptr;
      if (path && path->text() == navigationChildPath) {
        childWindow = candidate;
        break;
      }
    }
    QThread::msleep(5);
  }
  auto *childTable = childWindow ? childWindow->findChild<FilterTable *>() : nullptr;
  if (!childTable || !childTable->getFocusedItem() ||
      childTable->getFocusedItem()->index != 1) {
    std::cerr << "Open error location did not select the included source line\n";
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
  MainWindow window(configPath, QString::fromLocal8Bit(argv[1]));
  if (window.size() != QSize(777, 555)) {
    std::cerr << "editor did not restore its saved window geometry\n";
    return 1;
  }
  const auto recentFiles = uiSettings.value(QStringLiteral("ui/recentFiles")).toStringList();
  auto *recentMenu = window.findChild<QMenu *>(QStringLiteral("recentFilesMenu"));
  if (recentFiles.isEmpty() || recentFiles.first() != QFileInfo(configPath).absoluteFilePath() ||
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
  const auto addActions = rowMenu->actions();
  bool hasConvolutionTemplate = false;
  for (const auto *action : addActions) {
    if (action->data().canConvert<FilterTemplate>() &&
        action->data().value<FilterTemplate>().getLine() ==
            "Convolution: impulse-response.wav") {
      hasConvolutionTemplate = true;
      break;
    }
  }
  if (addActions.isEmpty() ||
      !addActions.first()->data().canConvert<FilterTemplate>() ||
      addActions.first()->data().value<FilterTemplate>().getLine() !=
          "Preamp: 0 dB" ||
      !hasConvolutionTemplate) {
    std::cerr
        << "row add-menu did not preserve upstream and Convolution templates\n";
    return 1;
  }
  if (construction.elapsed() >= 1000) {
    std::cerr << "MainWindow blocked while starting delayed CLI requests\n";
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
  auto *deviceCombo = window.findChild<QComboBox *>();
  if (!deviceCombo || deviceCombo->count() != 2 ||
      deviceCombo->currentData().toString() != "fixture.usb-mic" ||
      !deviceCombo->currentText().contains("1 ch, 44100 Hz") ||
      !deviceCombo->toolTip().contains("stable PipeWire node name")) {
    std::cerr << "device selector did not display enumerated format details: "
              << (deviceCombo ? deviceCombo->count() : -1) << ", "
              << (deviceCombo ? deviceCombo->currentData().toString().toStdString()
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
    std::cerr << "runtime status does not expose readable accessible PipeWire details\n";
    return 1;
  }
  const QList<QPair<QString, QString>> statusCases{
      {"streaming", "Streaming — audio processing active"},
      {"connecting", "Waiting for PipeWire (filter state: connecting)"},
      {"unconnected", "Waiting for PipeWire (filter state: unconnected)"},
      {"paused", "PipeWire connected but paused (not streaming)"},
      {"error", "PipeWire reported an error (recovery is not confirmed)"},
      {"not reachable", "Daemon offline or unresponsive"},
      {"future-state", "Daemon state unknown: future-state"},
      {"", "Daemon offline or unresponsive"}};
  for (qsizetype stateIndex = 0; stateIndex < statusCases.size(); ++stateIndex) {
    const auto &[state, expectedSummary] = statusCases[stateIndex];
    const QString scriptPath = temporary.filePath(
        QStringLiteral("status-cli-%1.sh").arg(stateIndex));
    const int exitCode = state.isEmpty() ? 1 : 0;
    if (!writeStatusCliFixture(scriptPath, state, exitCode)) {
      std::cerr << "could not create PipeWire status fixture script\n";
      return 1;
    }
    MainWindow statusWindow(configPath, scriptPath);
    auto *label = statusWindow.findChild<QLabel *>("daemonStatus");
    QElapsedTimer statusWait;
    statusWait.start();
    while (statusWait.elapsed() < 1500 &&
           (!label || !label->text().startsWith(expectedSummary))) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      QThread::msleep(5);
    }
    if (!label || !label->text().startsWith(expectedSummary) ||
        (exitCode == 0 &&
         (!label->text().contains("Sample rate: unknown (awaiting graph)") ||
          !label->text().contains("SkyAPO Virtual Mic") ||
          label->accessibleDescription() != label->text()))) {
      std::cerr << "UI did not accurately summarize PipeWire state '"
                << state.toStdString() << "': "
                << (label ? label->text().toStdString() : "status label missing")
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
    std::cerr << "device selector did not persist exactly one stable node name\n";
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
      "ID\tNODE NAME\tDESCRIPTION\tSELECTED\tCHANNELS\tSAMPLE RATE\n"
      "41\tfixture.capture\tBuilt-in capture\t\t2\t48000\n"
      "42\tfixture.usb-mic\tSelected USB microphone\tyes\t1\t44100\n";
  if (!writeDynamicDeviceList(selectedDeviceList)) {
    std::cerr << "could not create changing device-list fixture\n";
    return 1;
  }
  qputenv("SKYAPO_UI_TEST_DEVICE_LIST_FILE",
          dynamicDeviceListPath.toLocal8Bit());
  MainWindow dynamicDeviceWindow(configPath, QString::fromLocal8Bit(argv[1]));
  auto *dynamicDeviceCombo = dynamicDeviceWindow.findChild<QComboBox *>();
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
  if (!dynamicDeviceCombo->currentText().contains(
          "Selected input unavailable") ||
      dynamicDeviceCombo->itemData(0, Qt::UserRole + 1).toString() !=
          "fixture.usb-mic" ||
      dynamicDeviceCombo->itemData(0).toString() != QString{} ||
      dynamicDeviceCombo->count() != 2) {
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
  if (dynamicDeviceCombo->currentData().toString() != "fixture.usb-mic") {
    std::cerr
        << "reappearing selected node was not restored by the daemon marker\n";
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
  MainWindow graphicWindow(graphicConfigPath,
                           QString::fromLocal8Bit(argv[1]));
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  auto *graphicEditor = graphicWindow.findChild<GraphicEQFilterGUI *>();
  auto *graphicTable = graphicEditor
                           ? graphicEditor->findChild<QTableWidget *>(
                                 "tableWidget")
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
      savedGraphicConfig.readAll() !=
          "GraphicEQ: 100 0; 1000 4.5; 5000 -3\n") {
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
      rawGraphicConfig.readAll() !=
          "GraphicEQ: 100 0; 1000 2; 5000 -3\n") {
    std::cerr << "unsupported GraphicEQ text was changed by the editor\n";
    return 1;
  }
#endif
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
  auto *pluginCatalog = pluginDialog
                            ? pluginDialog->findChild<QPlainTextEdit *>(
                                  "pluginCatalog")
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

  MainWindow stalledCliWindow(configPath, QString::fromLocal8Bit(argv[1]));
  auto cliTimeouts = stalledCliWindow.findChildren<QTimer *>("cliTimeout");
  if (cliTimeouts.isEmpty()) {
    std::cerr << "CLI requests did not receive a bounded-timeout timer\n";
    return 1;
  }
  for (auto *timeout : cliTimeouts)
    timeout->setInterval(20);
  auto *stalledStatus =
      stalledCliWindow.findChild<QLabel *>("daemonStatus");
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
  modalCapture.stop();
  workflowPulse.stop();
  qunsetenv("SKYAPO_UI_TEST_CONFIG_LOG");
  qunsetenv("SKYAPO_UI_TEST_CONFIG_GATE_DIR");
  qunsetenv("SKYAPO_UI_TEST_RELOAD_MARKER");
  std::cout << "upstream editor widgets, selection/reordering, config "
               "preservation, async UI, stable device selection, and "
               "bounded CLI failure handling, and GraphicEQ serialization "
               "tests passed\n";
  return 0;
}
