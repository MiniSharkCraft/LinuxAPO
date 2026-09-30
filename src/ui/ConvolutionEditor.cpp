#include "ConvolutionEditor.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

ConvolutionEditor::ConvolutionEditor(QString parameters, QString configFile,
                                     QWidget *parent)
    : IFilterGUI(parent), configPath(std::move(configFile)) {
  auto *outer = new QVBoxLayout(this);
  auto *row = new QHBoxLayout;
  row->addWidget(new QLabel(tr("Convolution impulse response:"), this));
  pathEdit = new QLineEdit(std::move(parameters), this);
  pathEdit->setObjectName(QStringLiteral("convolutionPathEdit"));
  pathEdit->setPlaceholderText(tr("Relative or absolute audio file path"));
  auto *browse = new QPushButton(tr("Browse…"), this);
  browse->setObjectName(QStringLiteral("convolutionBrowseButton"));
  row->addWidget(pathEdit, 1);
  row->addWidget(browse);
  outer->addLayout(row);
  statusLabel = new QLabel(this);
  statusLabel->setObjectName(QStringLiteral("convolutionStatus"));
  statusLabel->setWordWrap(true);
  outer->addWidget(statusLabel);

  connect(pathEdit, &QLineEdit::editingFinished, this, [this] {
    updateFileStatus();
    emit updateModel();
  });
  connect(browse, &QPushButton::clicked, this, [this] {
    const QDir base = QFileInfo(configPath).absoluteDir();
    const QString current = pathEdit->text().trimmed();
    QFileInfo selected(current);
    if (selected.isRelative() && !current.isEmpty())
      selected.setFile(base, current);
    const QString filename = QFileDialog::getOpenFileName(
        this, tr("Select convolution impulse response"),
        current.isEmpty() ? base.absolutePath() : selected.absolutePath(),
        tr("Impulse responses (*.wav *.flac *.ogg);;All files (*)"));
    if (filename.isEmpty())
      return;
    const QString relative = base.relativeFilePath(filename);
    pathEdit->setText(relative.startsWith(QStringLiteral("../")) ? filename
                                                                 : relative);
    updateFileStatus();
    emit updateModel();
  });
  updateFileStatus();
}

void ConvolutionEditor::store(QString &command, QString &parameters) {
  command = QStringLiteral("Convolution");
  parameters = pathEdit->text().trimmed();
}

void ConvolutionEditor::updateFileStatus() {
  const QString path = pathEdit->text().trimmed();
  if (path.isEmpty()) {
    statusLabel->setText(tr("No impulse response file selected"));
    return;
  }

  QFileInfo info(path);
  if (info.isRelative())
    info.setFile(QFileInfo(configPath).absoluteDir(), path);
  if (!info.exists() || !info.isFile() || !info.isReadable()) {
    statusLabel->setText(
        tr("Impulse response file not found or unreadable: %1").arg(path));
    return;
  }

  statusLabel->setText(tr("File found. Use Save & Check to validate its audio "
                          "format and sample rate."));
}
