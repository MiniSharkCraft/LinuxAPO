#include "IncludeEditor.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

IncludeEditor::IncludeEditor(QString parameters, QString configFile,
                             QWidget *parent)
    : IFilterGUI(parent), configPath(std::move(configFile)) {
  auto *outer = new QVBoxLayout(this);
  auto *row = new QHBoxLayout;
  pathEdit = new QLineEdit(std::move(parameters), this);
  pathEdit->setObjectName(QStringLiteral("includePathEdit"));
  pathEdit->setPlaceholderText(tr("Relative path or absolute path"));
  auto *browse = new QPushButton(tr("Browse…"), this);
  row->addWidget(pathEdit, 1);
  row->addWidget(browse);
  outer->addLayout(row);
  statusLabel = new QLabel(this);
  statusLabel->setObjectName(QStringLiteral("includeStatus"));
  outer->addWidget(statusLabel);
  connect(pathEdit, &QLineEdit::editingFinished, this, [this] {
    updateFileStatus();
    emit updateModel();
  });
  connect(browse, &QPushButton::clicked, this, [this] {
    const QFileInfo configInfo(this->configPath);
    const QDir base = configInfo.absoluteDir();
    const QString current = pathEdit->text().trimmed();
    QFileInfo selected(current);
    if (selected.isRelative())
      selected.setFile(base, current);
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("Select included configuration"),
        current.isEmpty() ? base.absolutePath() : selected.absolutePath(),
        tr("Configuration files (*.txt);;All files (*)"));
    if (filename.isEmpty())
      return;
    const QString relative = base.relativeFilePath(filename);
    pathEdit->setText(relative.startsWith("../") ? filename : relative);
    updateFileStatus();
    emit updateModel();
  });
  updateFileStatus();
}

void IncludeEditor::store(QString &command, QString &parameters) {
  command = QStringLiteral("Include");
  parameters = pathEdit->text().trimmed();
}

void IncludeEditor::updateFileStatus() {
  const QString path = pathEdit->text().trimmed();
  if (path.isEmpty()) {
    statusLabel->setText(tr("No include path selected"));
    return;
  }
  QFileInfo info(path);
  if (info.isRelative())
    info.setFile(QFileInfo(configPath).absoluteDir(), path);
  statusLabel->setText(info.exists() && info.isFile()
                           ? tr("Include file found: %1").arg(info.fileName())
                           : tr("Include file not found: %1").arg(path));
}
