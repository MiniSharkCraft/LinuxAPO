#include "ConfigFile.h"

QString ConfigFile::Line::toQString() const {
  return QString::fromUtf8(changed ? text : originalText);
}

void ConfigFile::load(const QByteArray &contents) {
  lines.clear();
  preferredEnding = "\n";
  qsizetype start = 0;
  while (start < contents.size()) {
    qsizetype end = start;
    while (end < contents.size() && contents[end] != '\n' &&
           contents[end] != '\r')
      ++end;
    Line line;
    line.originalText = contents.mid(start, end - start);
    line.text = line.originalText;
    if (end < contents.size()) {
      if (contents[end] == '\r' && end + 1 < contents.size() &&
          contents[end + 1] == '\n') {
        line.ending = "\r\n";
        end += 2;
      } else {
        line.ending = contents.mid(end, 1);
        ++end;
      }
      if (lines.isEmpty())
        preferredEnding = line.ending;
    }
    lines.push_back(std::move(line));
    start = end;
  }
}

QByteArray ConfigFile::serialize() const {
  QByteArray result;
  for (const auto &line : lines) {
    result += line.changed ? line.text : line.originalText;
    result += line.ending;
  }
  return result;
}

void ConfigFile::replace(qsizetype index, const QString &text) {
  auto &line = lines[index];
  line.text = text.toUtf8();
  line.changed = true;
}

void ConfigFile::insert(qsizetype index, const QString &text) {
  Line line;
  line.text = text.toUtf8();
  line.changed = true;
  line.ending = preferredEnding;
  if (index >= lines.size()) {
    if (!lines.isEmpty() && lines.last().ending.isEmpty())
      lines.last().ending = preferredEnding;
    line.ending.clear();
    lines.push_back(std::move(line));
  } else {
    lines.insert(index, std::move(line));
  }
}

void ConfigFile::remove(qsizetype index) { lines.removeAt(index); }

void ConfigFile::move(qsizetype from, qsizetype to) {
  if (from < 0 || from >= lines.size() || to < 0 || to >= lines.size() ||
      from == to)
    return;
  const bool terminated = !lines.isEmpty() && !lines.last().ending.isEmpty();
  Line line = std::move(lines[from]);
  lines.removeAt(from);
  lines.insert(to, std::move(line));
  for (qsizetype i = 0; i + 1 < lines.size(); ++i) {
    if (lines[i].ending.isEmpty())
      lines[i].ending = preferredEnding;
  }
  if (!lines.isEmpty()) {
    if (terminated && lines.last().ending.isEmpty())
      lines.last().ending = preferredEnding;
    else if (!terminated)
      lines.last().ending.clear();
  }
}
