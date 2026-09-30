#include "ConfigFile.h"
#include "ChannelCopyEditor.h"
#include "IncludeEditor.h"
#include "IIRFilterEditor.h"
#include "MainWindow.h"

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
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
#include <QTableWidget>
#endif
#include <iostream>

int main(int argc, char **argv) {
  QApplication application(argc, argv);
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

  if (argc != 2) {
    std::cerr << "UI integration test needs the delayed CLI fixture path\n";
    return 1;
  }
  QTemporaryDir temporary;
  if (!temporary.isValid()) {
    std::cerr << "could not create temporary UI test directory\n";
    return 1;
  }
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
  MainWindow window(configPath, QString::fromLocal8Bit(argv[1]));
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
  if (addActions.isEmpty() ||
      !addActions.first()->data().canConvert<FilterTemplate>() ||
      addActions.first()->data().value<FilterTemplate>().getLine() !=
          "Preamp: 0 dB") {
    std::cerr << "upstream row add-menu did not preserve filter templates\n";
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
      !deviceCombo->currentText().contains("1 ch, 44100 Hz")) {
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
  std::cout << "upstream editor widgets, selection/reordering, config "
               "preservation, async UI, stable device selection, and "
               "GraphicEQ serialization "
               "tests passed\n";
  return 0;
}
