#include "ConfigFile.h"

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
  std::cout << "upstream editor widget and config preservation tests passed\n";
  return 0;
}
