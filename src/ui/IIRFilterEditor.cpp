#include "IIRFilterEditor.h"

#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {
constexpr int maxVisualOrder = 256;
const QRegularExpression numberPattern(
    R"(^[-+]?(?:(?:[0-9]+(?:\.[0-9]*)?)|(?:\.[0-9]+))(?:[eE][-+]?[0-9]+)?$)");
} // namespace

IIRFilterEditor *IIRFilterEditor::create(const QString &command,
                                         const QString &parameters,
                                         QWidget *parent) {
  static const QRegularExpression filterCommand(R"(^Filter(?:\s+[0-9]+)?$)");
  if (!filterCommand.match(command.trimmed()).hasMatch())
    return nullptr;

  static const QRegularExpression syntax(
      R"(^\s*ON\s+IIR\s+Order\s+([0-9]+)\s+Coefficients\s+(.+?)\s*$)");
  const auto match = syntax.match(parameters);
  if (!match.hasMatch())
    return nullptr;

  bool orderOk = false;
  const uint parsedOrder = match.captured(1).toUInt(&orderOk);
  if (!orderOk || parsedOrder < 1 || parsedOrder > maxVisualOrder)
    return nullptr;

  const QStringList values = match.captured(2).split(
      QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  if (values.size() != static_cast<qsizetype>(2 * (parsedOrder + 1)))
    return nullptr;
  for (const auto &value : values) {
    if (!numberPattern.match(value).hasMatch())
      return nullptr;
    bool ok = false;
    const double numeric = value.toDouble(&ok);
    if (!ok || !std::isfinite(numeric))
      return nullptr;
  }

  return new IIRFilterEditor(parsedOrder, values, parent);
}

IIRFilterEditor::IIRFilterEditor(unsigned filterOrder, QStringList values,
                                 QWidget *parent)
    : IFilterGUI(parent) {
  auto *layout = new QVBoxLayout(this);
  auto *orderLabel = new QLabel(tr("IIR order (m):"), this);
  order = new QSpinBox(this);
  order->setObjectName(QStringLiteral("iirOrderSpinBox"));
  order->setRange(1, maxVisualOrder);
  order->setValue(static_cast<int>(filterOrder));
  layout->addWidget(orderLabel);
  layout->addWidget(order);

  auto *hint = new QLabel(
      tr("Coefficients are b0…bm followed by a0…am. Numeric literals only; "
         "expressions remain in raw text."),
      this);
  hint->setWordWrap(true);
  layout->addWidget(hint);

  coefficients = new QTableWidget(static_cast<int>(filterOrder + 1), 2, this);
  coefficients->setObjectName(QStringLiteral("iirCoefficientTable"));
  coefficients->setHorizontalHeaderLabels(
      {tr("Numerator b"), tr("Denominator a")});
  coefficients->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
  layout->addWidget(coefficients);
  for (unsigned index = 0; index <= filterOrder; ++index) {
    for (unsigned side = 0; side < 2; ++side) {
      const QString value =
          values.at(static_cast<qsizetype>(side * (filterOrder + 1) + index));
      auto *edit = new QLineEdit(value, coefficients);
      edit->setValidator(new QRegularExpressionValidator(numberPattern, edit));
      edit->setObjectName(
          QStringLiteral("iirCoefficient_%1_%2")
              .arg(side == 0 ? QLatin1Char('b') : QLatin1Char('a'))
              .arg(index));
      coefficients->setCellWidget(static_cast<int>(index),
                                  static_cast<int>(side), edit);
      connect(edit, &QLineEdit::textChanged, this, [this, edit] {
        bool ok = false;
        const double value = edit->text().toDouble(&ok);
        if (ok && std::isfinite(value))
          emit updateModel();
      });
    }
  }
  connect(order, qOverload<int>(&QSpinBox::valueChanged), this,
          [this](int value) { resizeCoefficients(value); });
}

void IIRFilterEditor::resizeCoefficients(int newOrder) {
  const int oldRows = coefficients->rowCount();
  coefficients->setRowCount(newOrder + 1);
  for (int row = oldRows; row <= newOrder; ++row) {
    if (row < 0)
      continue;
    for (int side = 0; side < 2; ++side) {
      auto *edit = new QLineEdit(QStringLiteral("0"), coefficients);
      edit->setValidator(new QRegularExpressionValidator(numberPattern, edit));
      edit->setObjectName(
          QStringLiteral("iirCoefficient_%1_%2")
              .arg(side == 0 ? QLatin1Char('b') : QLatin1Char('a'))
              .arg(row));
      coefficients->setCellWidget(row, side, edit);
      connect(edit, &QLineEdit::textChanged, this, [this, edit] {
        bool ok = false;
        const double value = edit->text().toDouble(&ok);
        if (ok && std::isfinite(value))
          emit updateModel();
      });
    }
  }
  emit updateModel();
}

void IIRFilterEditor::store(QString &command, QString &parameters) {
  command = QStringLiteral("Filter");
  QStringList values;
  const int count = order->value() + 1;
  for (int side = 0; side < 2; ++side) {
    for (int row = 0; row < count; ++row) {
      auto *edit =
          qobject_cast<QLineEdit *>(coefficients->cellWidget(row, side));
      values << (edit ? edit->text().trimmed() : QStringLiteral("0"));
    }
  }
  parameters = QStringLiteral("ON IIR Order %1 Coefficients %2")
                   .arg(order->value())
                   .arg(values.join(QLatin1Char(' ')));
}
