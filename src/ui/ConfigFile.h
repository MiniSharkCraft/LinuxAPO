#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

// Line-oriented editing which preserves every untouched byte and line ending.
class ConfigFile {
public:
  struct Line {
    QByteArray originalText;
    QByteArray text;
    QByteArray ending;
    bool changed{};

    QString toQString() const;
  };

  void load(const QByteArray &contents);
  QByteArray serialize() const;
  qsizetype lineCount() const { return lines.size(); }
  const Line &line(qsizetype index) const { return lines.at(index); }
  void replace(qsizetype index, const QString &text);
  void insert(qsizetype index, const QString &text);
  void remove(qsizetype index);

private:
  QVector<Line> lines;
  QByteArray preferredEnding = "\n";
};
