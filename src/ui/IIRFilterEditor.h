#pragma once

#include "Editor/IFilterGUI.h"

class QSpinBox;
class QTableWidget;

// Visual editor for the explicit numeric subset of EAPO's custom IIR syntax.
// Expressions and other parser-compatible but non-literal values stay raw.
class IIRFilterEditor final : public IFilterGUI {
  Q_OBJECT
public:
  static IIRFilterEditor *create(const QString &command,
                                 const QString &parameters,
                                 QWidget *parent = nullptr);

  void store(QString &command, QString &parameters) override;
  QSpinBox *orderControl() const {
    return order;
  }
  QTableWidget *coefficientTable() const {
    return coefficients;
  }

private:
  explicit IIRFilterEditor(unsigned filterOrder, QStringList values,
                           QWidget *parent);
  void resizeCoefficients(int newOrder);

  QSpinBox *order{};
  QTableWidget *coefficients{};
};
