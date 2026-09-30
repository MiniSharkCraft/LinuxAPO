#include "ChannelCopyEditor.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QSignalBlocker>

#include <QRegularExpression>

namespace {
const QStringList channelNames{"L", "R", "C", "LFE", "RL", "RR", "SL", "SR"};

QString canonicalChannel(QString name) {
  name = name.toUpper();
  if (name == "SUB")
    return "LFE";
  return channelNames.contains(name) ? name : QString{};
}
} // namespace

ChannelCopyEditor::ChannelCopyEditor(Kind kind, QWidget *parent)
    : IFilterGUI(parent), editorKind(kind) {
  auto *layout = new QVBoxLayout(this);
  if (kind == Kind::Channel) {
    channelLayout = new QGridLayout;
    auto *all = new QCheckBox(tr("ALL"), this);
    all->setObjectName("channelSelectAll");
    channelLayout->addWidget(all, 0, 0);
    for (int i = 0; i < channelNames.size(); ++i)
      addChannelCheckbox(channelNames[i], 1 + i / 4, i % 4);
    layout->addLayout(channelLayout);
    connect(all, &QCheckBox::toggled, this, [this](bool enabled) {
      for (auto *box : findChildren<QCheckBox *>())
        if (box->objectName() != "channelSelectAll")
          box->setEnabled(!enabled);
      emit updateModel();
    });
  } else {
    assignments = new QTableWidget(0, 4, this);
    assignments->setObjectName("copyAssignments");
    assignments->setHorizontalHeaderLabels(
        {tr("Target"), tr("Source"), tr("Linear gain"), tr("Remove")});
    assignments->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    layout->addWidget(assignments);
    auto *add = new QPushButton(tr("Add channel assignment"), this);
    add->setObjectName("addCopyAssignment");
    layout->addWidget(add);
    connect(add, &QPushButton::clicked, this,
            [this] { addCopyAssignment("L", "L", 1.0); emit updateModel(); });
    connect(assignments, &QTableWidget::cellChanged, this,
            [this] { emit updateModel(); });
  }
}

void ChannelCopyEditor::addChannelCheckbox(const QString &name, int row,
                                           int column) {
  auto *box = new QCheckBox(name, this);
  box->setObjectName("channel_" + name);
  channelLayout->addWidget(box, row, column);
  connect(box, &QCheckBox::toggled, this, [this] { emit updateModel(); });
}

ChannelCopyEditor *ChannelCopyEditor::create(const QString &command,
                                             const QString &parameters,
                                             QWidget *parent) {
  if (command.compare("Channel", Qt::CaseInsensitive) == 0) {
    const auto tokens = parameters.split(QRegularExpression("\\s+"),
                                         Qt::SkipEmptyParts);
    if (tokens.isEmpty())
      return nullptr;
    QStringList selected;
    bool all = false;
    for (const auto &token : tokens) {
      if (token.compare("ALL", Qt::CaseInsensitive) == 0) {
        all = true;
        continue;
      }
      const auto canonical = canonicalChannel(token);
      if (canonical.isEmpty() || selected.contains(canonical) || all)
        return nullptr;
      selected.push_back(canonical);
    }
    if (all && tokens.size() != 1)
      return nullptr;
    auto *editor = new ChannelCopyEditor(Kind::Channel, parent);
    for (const auto &name : selected)
      if (auto *box = editor->findChild<QCheckBox *>("channel_" + name))
        box->setChecked(true);
    if (all)
      editor->findChild<QCheckBox *>("channelSelectAll")->setChecked(true);
    return editor;
  }
  if (command.compare("Copy", Qt::CaseInsensitive) != 0)
    return nullptr;

  const auto tokens = parameters.split(QRegularExpression("\\s+"),
                                       Qt::SkipEmptyParts);
  if (tokens.isEmpty())
    return nullptr;
  static const QRegularExpression assignment(
      R"(^([A-Za-z]+)=(([-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?)\*)?([A-Za-z]+)$)");
  struct Parsed { QString target; QString source; double gain; };
  QList<Parsed> parsed;
  for (const auto &token : tokens) {
    const auto match = assignment.match(token);
    if (!match.hasMatch())
      return nullptr;
    const auto target = canonicalChannel(match.captured(1));
    const auto source = canonicalChannel(match.captured(4));
    bool ok = true;
    const double gain = match.captured(3).isEmpty()
                            ? 1.0
                            : match.captured(3).toDouble(&ok);
    if (target.isEmpty() || source.isEmpty() || !ok || !qIsFinite(gain) ||
        qAbs(gain) > 1000000.0)
      return nullptr;
    parsed.push_back({target, source, gain});
  }
  auto *editor = new ChannelCopyEditor(Kind::Copy, parent);
  for (const auto &entry : parsed)
    editor->addCopyAssignment(entry.target, entry.source, entry.gain);
  return editor;
}

void ChannelCopyEditor::addCopyAssignment(const QString &target,
                                          const QString &source,
                                          double factor) {
  const int row = assignments->rowCount();
  assignments->insertRow(row);
  for (int column = 0; column < 2; ++column) {
    auto *combo = new QComboBox(assignments);
    combo->addItems(channelNames);
    combo->setCurrentText(column == 0 ? target : source);
    combo->setObjectName(column == 0 ? QString("copyTarget_%1").arg(row)
                                    : QString("copySource_%1").arg(row));
    assignments->setCellWidget(row, column, combo);
    connect(combo, &QComboBox::currentTextChanged, this,
            [this] { emit updateModel(); });
  }
  auto *gain = new QDoubleSpinBox(assignments);
  gain->setRange(-1000000.0, 1000000.0);
  gain->setDecimals(6);
  gain->setValue(factor);
  gain->setObjectName(QString("copyGain_%1").arg(row));
  assignments->setCellWidget(row, 2, gain);
  connect(gain, qOverload<double>(&QDoubleSpinBox::valueChanged), this,
          [this] { emit updateModel(); });
  auto *remove = new QPushButton(tr("Remove"), assignments);
  remove->setObjectName(QString("removeCopyAssignment_%1").arg(row));
  assignments->setCellWidget(row, 3, remove);
  connect(remove, &QPushButton::clicked, this, [this, row] {
    assignments->removeRow(row);
    // Qt does not reindex cell-widget object names when rows are removed.
    // Rebuild the small table to keep stable test/accessibility identifiers.
    QList<QList<QString>> values;
    for (int current = 0; current < assignments->rowCount(); ++current) {
      auto *target = qobject_cast<QComboBox *>(assignments->cellWidget(current, 0));
      auto *source = qobject_cast<QComboBox *>(assignments->cellWidget(current, 1));
      auto *factor = qobject_cast<QDoubleSpinBox *>(assignments->cellWidget(current, 2));
      if (target && source && factor)
        values.push_back({target->currentText(), source->currentText(),
                          QString::number(factor->value(), 'g', 9)});
    }
    {
      const QSignalBlocker blocker(assignments);
      assignments->setRowCount(0);
      for (const auto &value : values)
        addCopyAssignment(value[0], value[1], value[2].toDouble());
    }
    emit updateModel();
  });
}

void ChannelCopyEditor::store(QString &command, QString &parameters) {
  if (editorKind == Kind::Channel) {
    command = "Channel";
    if (findChild<QCheckBox *>("channelSelectAll")->isChecked()) {
      parameters = "ALL";
      return;
    }
    QStringList selected;
    for (const auto &name : channelNames)
      if (findChild<QCheckBox *>("channel_" + name)->isChecked())
        selected << name;
    parameters = selected.join(' ');
    return;
  }
  command = "Copy";
  QStringList serialized;
  for (int row = 0; row < assignments->rowCount(); ++row) {
    const auto *target = qobject_cast<QComboBox *>(assignments->cellWidget(row, 0));
    const auto *source = qobject_cast<QComboBox *>(assignments->cellWidget(row, 1));
    const auto *gain = qobject_cast<QDoubleSpinBox *>(assignments->cellWidget(row, 2));
    if (!target || !source || !gain)
      continue;
    QString expression = source->currentText();
    if (gain->value() != 1.0)
      expression = QString::number(gain->value(), 'g', 9) + "*" + expression;
    serialized << target->currentText() + "=" + expression;
  }
  parameters = serialized.join(' ');
}
