#pragma once

#include "Editor/IFilterGUI.h"

class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QTableWidget;

// Linux-facing visual editors for the portable EAPO Channel/Copy syntax.
// Unsupported/ambiguous directives are deliberately not instantiated.
class ChannelCopyEditor final : public IFilterGUI {
  Q_OBJECT
public:
  enum class Kind { Channel, Copy };

  static ChannelCopyEditor *create(const QString &command,
                                   const QString &parameters,
                                   QWidget *parent = nullptr);

  void store(QString &command, QString &parameters) override;
  Kind kind() const { return editorKind; }
  QTableWidget *copyTable() const { return assignments; }

private:
  explicit ChannelCopyEditor(Kind kind, QWidget *parent);
  void addCopyAssignment(const QString &target, const QString &source,
                         double factor);
  void addChannelCheckbox(const QString &name, int row, int column);

  Kind editorKind;
  QGridLayout *channelLayout{};
  QTableWidget *assignments{};
};
