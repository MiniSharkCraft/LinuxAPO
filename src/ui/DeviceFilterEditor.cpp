#include "DeviceFilterEditor.h"

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <utility>

DeviceFilterEditor::DeviceFilterEditor(QString parameters, QWidget *parent)
    : IFilterGUI(parent) {
  auto *layout = new QVBoxLayout(this);
  auto *help = new QLabel(
      tr("Match the selected PipeWire capture device. Separate alternative "
         "matches with ';'; words in one match are combined with AND. Use "
         "'all' to match every device."),
      this);
  help->setWordWrap(true);
  layout->addWidget(help);

  patternEdit = new QLineEdit(std::move(parameters), this);
  patternEdit->setObjectName(QStringLiteral("devicePatternEdit"));
  patternEdit->setPlaceholderText(tr("e.g. Built-in Audio; USB Microphone"));
  layout->addWidget(patternEdit);
  connect(patternEdit, &QLineEdit::editingFinished, this,
          [this] { emit updateModel(); });
}

void DeviceFilterEditor::store(QString &command, QString &parameters) {
  command = QStringLiteral("Device");
  parameters = patternEdit->text().trimmed();
}
