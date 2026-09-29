#include "ConfigFile.h"
#include "MainWindow.h"

#include "Editor/FilterTable.h"
#include "Editor/FilterTableRow.h"
#include "Editor/FilterTemplate.h"
#include "Editor/guis/BiQuadFilterGUI.h"
#include "Editor/guis/BiQuadFilterGUIFactory.h"
#include "Editor/guis/DelayFilterGUI.h"
#include "Editor/guis/DelayFilterGUIFactory.h"
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
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
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
  QFile config(configPath);
  if (!config.open(QIODevice::WriteOnly) ||
      config.write("Preamp: 0 dB\n; keep order\nPreamp: -6 dB\n") < 0) {
    std::cerr << "could not create temporary UI config\n";
    return 1;
  }
  config.close();

  QElapsedTimer construction;
  construction.start();
  MainWindow window(configPath, QString::fromLocal8Bit(argv[1]));
  if (window.findChildren<FilterTableRow *>().size() != 3) {
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
      reordered.readAll() != "; keep order\nPreamp: 0 dB\nPreamp: -6 dB\n") {
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
  std::cout << "upstream editor widgets, selection/reordering, config "
               "preservation, and async UI "
               "tests passed\n";
  return 0;
}
