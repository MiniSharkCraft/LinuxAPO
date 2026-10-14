#include "MainWindow.h"
#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
#include "ResponseAnalysis.h"
#endif
#include "ChannelCopyEditor.h"
#include "ConvolutionEditor.h"
#include "DeviceFilterEditor.h"
#include "IncludeEditor.h"
#include "IIRFilterEditor.h"

#include "Editor/FilterTableRow.h"
#include "Editor/FilterTemplate.h"
#include "Editor/IFilterGUI.h"
#include "Editor/guis/BiQuadFilterGUIFactory.h"
#include "Editor/guis/DelayFilterGUIFactory.h"
#include "Editor/guis/PreampFilterGUIFactory.h"
#include "Editor/guis/StageFilterGUIFactory.h"
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
#include "Editor/guis/GraphicEQFilterGUIFactory.h"
#endif
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QAction>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHash>
#include <QLocale>
#include <QSaveFile>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QTemporaryFile>
#include <QStandardPaths>
#include <QSpinBox>
#include <QSettings>
#include <QTimer>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <QVBoxLayout>
#include <memory>
#include <limits>
#include <QStringList>
#include <system_error>
#include <utility>

namespace {
const QString PluginPickerLine =
    QStringLiteral("__SKYAPO_PICK_INSTALLED_PLUGIN__");

struct PluginParameterModel {
  QString symbol;
  QString name;
  double defaultValue{};
  double minimum{};
  double maximum{};
};

QString withoutInlineComment(const QString &line);

bool parseMetadataNumber(const QString &text, double &value,
                         bool allowInfinity = false) {
  const auto normalized = text.trimmed().toLower();
  if (allowInfinity && (normalized == QStringLiteral("-inf") ||
                        normalized == QStringLiteral("-infinity"))) {
    value = -std::numeric_limits<float>::max();
    return true;
  }
  if (allowInfinity && (normalized == QStringLiteral("inf") ||
                        normalized == QStringLiteral("+inf") ||
                        normalized == QStringLiteral("infinity") ||
                        normalized == QStringLiteral("+infinity"))) {
    value = std::numeric_limits<float>::max();
    return true;
  }
  bool ok = false;
  value = QLocale::c().toDouble(text.trimmed(), &ok);
  return ok && std::isfinite(value);
}

#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
bool parseStreamingFormat(const QByteArray &status, unsigned &sampleRate,
                          unsigned &channels) {
  bool streaming = false;
  bool hasRate = false;
  bool hasChannels = false;
  static const QRegularExpression ratePattern(
      QStringLiteral(R"(^Sample rate: ([0-9]+) Hz$)"));
  static const QRegularExpression channelPattern(
      QStringLiteral(R"(^Channels: ([0-9]+)$)"));
  for (const auto &line : QString::fromUtf8(status).split(QLatin1Char('\n'))) {
    if (line == QStringLiteral("Daemon: streaming"))
      streaming = true;
    const auto rateMatch = ratePattern.match(line);
    if (rateMatch.hasMatch()) {
      bool ok = false;
      const auto value = rateMatch.captured(1).toUInt(&ok);
      if (ok && value >= 8000 && value <= 384000) {
        sampleRate = value;
        hasRate = true;
      }
    }
    const auto channelMatch = channelPattern.match(line);
    if (channelMatch.hasMatch()) {
      bool ok = false;
      const auto value = channelMatch.captured(1).toUInt(&ok);
      if (ok && value > 0 && value <= 8) {
        channels = value;
        hasChannels = true;
      }
    }
  }
  return streaming && hasRate && hasChannels;
}
#endif

bool parsePluginMetadata(const QByteArray &output,
                         const QString &expectedFormat,
                         const QString &expectedId,
                         QVector<PluginParameterModel> &parameters,
                         QString &error) {
  const QRegularExpression rangeExpression(
      QStringLiteral(R"(range=\[([^,]+),\s*([^\]]+)\])"),
      QRegularExpression::CaseInsensitiveOption);
  QString reportedFormat;
  QString reportedId;
  for (const auto &line : QString::fromUtf8(output).split(QLatin1Char('\n'))) {
    if (line.startsWith(QStringLiteral("Format: "))) {
      reportedFormat = line.mid(8).trimmed();
      continue;
    }
    if (line.startsWith(QStringLiteral("URI: "))) {
      reportedId = line.mid(5).trimmed();
      reportedFormat = QStringLiteral("LV2");
      continue;
    }
    if (line.startsWith(QStringLiteral("ID: "))) {
      reportedId = line.mid(4).trimmed();
      continue;
    }
    if (line.startsWith(QStringLiteral("Class UID: "))) {
      reportedId = line.mid(11).trimmed();
      continue;
    }
    const auto columns = line.split(QLatin1Char('\t'));
    if (columns.size() < 4)
      continue;
    const QString symbol = columns[0].trimmed();
    const QString name = columns[1].trimmed();
    const QRegularExpressionMatch defaultMatch =
        QRegularExpression(QStringLiteral(R"(default=([^\t]+))"),
                           QRegularExpression::CaseInsensitiveOption)
            .match(line);
    const QRegularExpressionMatch rangeMatch = rangeExpression.match(line);
    if (symbol.isEmpty() || !defaultMatch.hasMatch() ||
        !rangeMatch.hasMatch()) {
      error =
          QObject::tr("Plugin returned a malformed parameter metadata row.");
      return false;
    }
    PluginParameterModel parameter;
    parameter.symbol = symbol;
    parameter.name = name.isEmpty() ? symbol : name;
    if (!parseMetadataNumber(defaultMatch.captured(1),
                             parameter.defaultValue) ||
        !parseMetadataNumber(rangeMatch.captured(1), parameter.minimum, true) ||
        !parseMetadataNumber(rangeMatch.captured(2), parameter.maximum, true) ||
        parameter.minimum > parameter.maximum ||
        parameter.defaultValue < parameter.minimum ||
        parameter.defaultValue > parameter.maximum) {
      error = QObject::tr("Plugin parameter metadata is invalid for '%1'.")
                  .arg(symbol);
      return false;
    }
    if (std::any_of(parameters.cbegin(), parameters.cend(),
                    [&parameter](const auto &existing) {
                      return existing.symbol == parameter.symbol;
                    })) {
      error = QObject::tr("Plugin reported duplicate parameter ID '%1'.")
                  .arg(symbol);
      return false;
    }
    parameters.push_back(std::move(parameter));
  }
  if (reportedFormat.compare(expectedFormat, Qt::CaseInsensitive) != 0 ||
      reportedId != expectedId) {
    error = QObject::tr("Plugin metadata did not match the selected %1 plugin.")
                .arg(expectedFormat);
    return false;
  }
  return true;
}

bool patchPluginDirective(const QString &line, const QString &format,
                          const QString &pluginId,
                          const QVector<PluginParameterModel> &parameters,
                          const QHash<QString, double> &values,
                          QString &updated, QString &error) {
  const QString active = withoutInlineComment(line);
  static const QRegularExpression directiveExpression(
      QStringLiteral(R"(^\s*Plugin\s*:\s*(LV2|CLAP|VST3)\s+(\S+)(.*)$)"),
      QRegularExpression::CaseInsensitiveOption);
  const auto directive = directiveExpression.match(active);
  if (!directive.hasMatch() ||
      directive.captured(1).compare(format, Qt::CaseInsensitive) != 0 ||
      directive.captured(2) != pluginId) {
    error = QObject::tr("This plugin directive cannot be edited safely.");
    return false;
  }

  struct Edit {
    qsizetype start{};
    qsizetype length{};
    QString replacement;
  };
  const qsizetype suffixStart = directive.capturedStart(3);
  const QString suffix = directive.captured(3);
  static const QRegularExpression tokenExpression(QStringLiteral(R"(\S+)"));
  static const QRegularExpression assignmentExpression(
      QStringLiteral(R"(^([^=]+)=(.*)$)"));
  QHash<QString, Edit> assignments;
  auto tokenIterator = tokenExpression.globalMatch(suffix);
  while (tokenIterator.hasNext()) {
    const auto token = tokenIterator.next();
    const auto assignment = assignmentExpression.match(token.captured());
    if (!assignment.hasMatch() || assignment.captured(1).isEmpty()) {
      error = QObject::tr("The directive contains an unsupported parameter "
                          "token '%1'. Edit it as raw text instead.")
                  .arg(token.captured());
      return false;
    }
    const QString key = assignment.captured(1);
    if (assignments.contains(key)) {
      error = QObject::tr("The directive contains duplicate parameter '%1'. "
                          "Resolve the duplicate before using this editor.")
                  .arg(key);
      return false;
    }
    const qsizetype valueStart =
        suffixStart + token.capturedStart() + assignment.capturedStart(2);
    assignments.insert(key, Edit{valueStart, assignment.capturedLength(2), {}});
  }

  QVector<Edit> edits;
  QStringList added;
  for (const auto &parameter : parameters) {
    if (!values.contains(parameter.symbol)) {
      error = QObject::tr("No configured value was supplied for '%1'.")
                  .arg(parameter.symbol);
      return false;
    }
    const double value = values.value(parameter.symbol);
    if (!std::isfinite(value) || value < parameter.minimum ||
        value > parameter.maximum) {
      error = QObject::tr("Value for '%1' is outside the plugin's declared "
                          "range.")
                  .arg(parameter.symbol);
      return false;
    }
    QString matchedKey;
    const QStringList acceptedKeys =
        format.compare(QStringLiteral("CLAP"), Qt::CaseInsensitive) == 0
            ? QStringList{parameter.symbol, parameter.name}
            : QStringList{parameter.symbol};
    for (const auto &key : acceptedKeys) {
      if (assignments.contains(key)) {
        if (!matchedKey.isEmpty() && matchedKey != key) {
          error = QObject::tr("Both the ID and name of '%1' are configured; "
                              "resolve this ambiguity in raw text first.")
                      .arg(parameter.symbol);
          return false;
        }
        matchedKey = key;
      }
    }
    const QString formatted = QString::number(value, 'g', 9);
    if (!matchedKey.isEmpty()) {
      auto edit = assignments.value(matchedKey);
      edit.replacement = formatted;
      if (line.mid(edit.start, edit.length) != formatted)
        edits.push_back(std::move(edit));
    } else {
      const double tolerance =
          std::max(1.0, std::abs(parameter.defaultValue)) * 1.0e-9;
      if (std::abs(value - parameter.defaultValue) > tolerance)
        added.push_back(parameter.symbol + QLatin1Char('=') + formatted);
    }
  }

  // Keep unknown assignments exactly as written; plugin-specific extensions
  // may not be represented in the metadata table returned by a host.
  qsizetype insertion = active.size();
  while (insertion > suffixStart && line[insertion - 1].isSpace())
    --insertion;
  if (!added.isEmpty())
    edits.push_back(
        {insertion, 0, QLatin1Char(' ') + added.join(QLatin1Char(' '))});

  updated = line;
  std::sort(edits.begin(), edits.end(),
            [](const Edit &left, const Edit &right) {
              return left.start > right.start;
            });
  for (const auto &edit : edits)
    updated.replace(edit.start, edit.length, edit.replacement);
  return true;
}

bool readConfiguredPluginValues(const QString &line, const QString &format,
                                const QVector<PluginParameterModel> &parameters,
                                QHash<QString, double> &values,
                                QString &error) {
  const QString active = withoutInlineComment(line);
  static const QRegularExpression directiveExpression(
      QStringLiteral(R"(^\s*Plugin\s*:\s*(LV2|CLAP|VST3)\s+(\S+)(.*)$)"),
      QRegularExpression::CaseInsensitiveOption);
  const auto directive = directiveExpression.match(active);
  if (!directive.hasMatch() ||
      directive.captured(1).compare(format, Qt::CaseInsensitive) != 0) {
    error = QObject::tr("This plugin directive cannot be read safely.");
    return false;
  }

  QHash<QString, QString> keysToSymbols;
  for (const auto &parameter : parameters) {
    keysToSymbols.insert(parameter.symbol, parameter.symbol);
    if (format.compare(QStringLiteral("CLAP"), Qt::CaseInsensitive) == 0 &&
        !parameter.name.isEmpty()) {
      if (keysToSymbols.contains(parameter.name) &&
          keysToSymbols.value(parameter.name) != parameter.symbol) {
        error = QObject::tr("CLAP parameter name '%1' is ambiguous; edit the "
                            "directive as raw text.")
                    .arg(parameter.name);
        return false;
      }
      keysToSymbols.insert(parameter.name, parameter.symbol);
    }
  }

  const QString suffix = directive.captured(3);
  static const QRegularExpression tokenExpression(QStringLiteral(R"(\S+)"));
  static const QRegularExpression assignmentExpression(
      QStringLiteral(R"(^([^=]+)=(.*)$)"));
  QSet<QString> configured;
  auto iterator = tokenExpression.globalMatch(suffix);
  while (iterator.hasNext()) {
    const auto token = iterator.next();
    const auto assignment = assignmentExpression.match(token.captured());
    if (!assignment.hasMatch()) {
      error = QObject::tr("The directive contains an unsupported parameter "
                          "token '%1'. Edit it as raw text instead.")
                  .arg(token.captured());
      return false;
    }
    const QString key = assignment.captured(1);
    if (!keysToSymbols.contains(key))
      continue; // Preserve plugin-specific assignments unknown to the host.
    const QString symbol = keysToSymbols.value(key);
    if (configured.contains(symbol)) {
      error = QObject::tr("Plugin parameter '%1' is configured more than "
                          "once; resolve this in raw text first.")
                  .arg(symbol);
      return false;
    }
    double value = 0.0;
    if (!parseMetadataNumber(assignment.captured(2), value)) {
      error = QObject::tr("Configured value for '%1' is not a finite number.")
                  .arg(symbol);
      return false;
    }
    const auto parameter = std::find_if(
        parameters.cbegin(), parameters.cend(),
        [&symbol](const auto &entry) { return entry.symbol == symbol; });
    if (parameter == parameters.cend() || value < parameter->minimum ||
        value > parameter->maximum) {
      error = QObject::tr("Configured value for '%1' is outside its declared "
                          "range.")
                  .arg(symbol);
      return false;
    }
    configured.insert(symbol);
    values.insert(symbol, value);
  }
  return true;
}

QString defaultCliProgram() {
  const QString sibling = QCoreApplication::applicationDirPath() + "/skyapo";
  return QFileInfo::exists(sibling) ? sibling : QStringLiteral("skyapo");
}

QString runtimeStatusText(int result, const QByteArray &output,
                          const QByteArray &error) {
  const QString details = QString::fromUtf8(output).trimmed();
  if (result != 0) {
    const QString reason = QString::fromUtf8(error).trimmed();
    return QObject::tr("Daemon offline or unresponsive") +
           (reason.isEmpty() ? QString{} : QStringLiteral("\n") + reason);
  }

  QString state;
  for (const auto &line : details.split(QLatin1Char('\n'))) {
    if (line.startsWith(QStringLiteral("Daemon: "))) {
      state = line.mid(8).trimmed().toLower();
      break;
    }
  }

  QString summary;
  if (state == QStringLiteral("streaming")) {
    summary = QObject::tr("Streaming — audio processing active");
  } else if (state == QStringLiteral("connecting") ||
             state == QStringLiteral("unconnected")) {
    summary = QObject::tr("Waiting for PipeWire (filter state: %1)").arg(state);
  } else if (state == QStringLiteral("paused")) {
    summary = QObject::tr("PipeWire connected but paused (not streaming)");
  } else if (state == QStringLiteral("error")) {
    summary =
        QObject::tr("PipeWire reported an error (recovery is not confirmed)");
  } else if (state == QStringLiteral("not reachable") ||
             state == QStringLiteral("unresponsive")) {
    summary = QObject::tr("Daemon offline or unresponsive");
  } else if (!state.isEmpty()) {
    summary = QObject::tr("Daemon state unknown: %1").arg(state);
  } else {
    summary = QObject::tr("Daemon state unavailable");
  }

  if (!details.isEmpty())
    summary += QStringLiteral("\n") + details;
  else
    summary += QObject::tr("\nRuntime details unavailable");
  return summary;
}

QString withoutInlineComment(const QString &line) {
  bool quoted = false;
  bool escaped = false;
  for (qsizetype i = 0; i < line.size(); ++i) {
    const QChar ch = line[i];
    if (ch == QLatin1Char('#') && !quoted)
      return line.left(i);
    if (ch == QLatin1Char('"') && !escaped)
      quoted = !quoted;
    if (quoted && ch == QLatin1Char('\\') && !escaped)
      escaped = true;
    else
      escaped = false;
  }
  return line;
}

bool rebaseRelativeIncludes(ConfigFile &config, const QString &sourcePath,
                            const QString &destinationPath, QString &error) {
  const auto canonicalConfigPath = [](const QString &path, QString &canonical) {
    const QByteArray encoded = path.toUtf8();
    std::error_code code;
    const auto normalized = std::filesystem::weakly_canonical(
        std::filesystem::u8path(encoded.constData()), code);
    if (code)
      return false;
    const auto utf8 = normalized.u8string();
    canonical =
        QString::fromUtf8(utf8.data(), static_cast<qsizetype>(utf8.size()));
    return true;
  };
  QString canonicalSource;
  QString canonicalDestination;
  if (!canonicalConfigPath(sourcePath, canonicalSource) ||
      !canonicalConfigPath(destinationPath, canonicalDestination)) {
    error = QObject::tr("Could not canonicalize the source or destination "
                        "config path to preserve Include semantics");
    return false;
  }
  const QDir sourceDirectory = QFileInfo(canonicalSource).absoluteDir();
  const QDir destinationDirectory =
      QFileInfo(canonicalDestination).absoluteDir();
  for (qsizetype index = 0; index < config.lineCount(); ++index) {
    const auto &line = config.line(index);
    const QByteArray bytes = line.changed ? line.text : line.originalText;
    const QString raw = QString::fromUtf8(bytes);
    if (raw.toUtf8() != bytes) {
      error = QObject::tr("Cannot safely preserve Include paths in a config "
                          "that is not valid UTF-8");
      return false;
    }
    const QString active = withoutInlineComment(raw);
    const qsizetype colon = active.indexOf(QLatin1Char(':'));
    if (colon < 0 || active.left(colon).trimmed() != QStringLiteral("Include"))
      continue;

    qsizetype pathBegin = colon + 1;
    while (pathBegin < active.size() && active[pathBegin].isSpace())
      ++pathBegin;
    qsizetype pathEnd = active.size();
    while (pathEnd > pathBegin && active[pathEnd - 1].isSpace())
      --pathEnd;
    if (pathBegin == pathEnd)
      continue;

    QString includePath = active.mid(pathBegin, pathEnd - pathBegin);
    const bool startsQuoted = includePath.startsWith(QLatin1Char('"'));
    const bool endsQuoted = includePath.endsWith(QLatin1Char('"'));
    if (startsQuoted != endsQuoted) {
      error = QObject::tr("Cannot safely preserve malformed quoted Include "
                          "at line %1")
                  .arg(index + 1);
      return false;
    }
    if (startsQuoted)
      includePath = includePath.mid(1, includePath.size() - 2);
    if (includePath.isEmpty() || includePath.contains(QLatin1Char('"'))) {
      error = QObject::tr("Cannot safely preserve empty or escaped-quote "
                          "Include at line %1")
                  .arg(index + 1);
      return false;
    }
    if (includePath.contains(QLatin1Char('`'))) {
      error = QObject::tr("Cannot safely rebase expression-based Include at "
                          "line %1; keep the config in its current directory")
                  .arg(index + 1);
      return false;
    }
    const QFileInfo includeInfo(includePath);
    if (includeInfo.isAbsolute())
      continue;

    const QString originalTarget =
        QDir::cleanPath(sourceDirectory.absoluteFilePath(includePath));
    QString rebased = destinationDirectory.relativeFilePath(originalTarget);
    if (rebased.isEmpty() || rebased == QStringLiteral("."))
      rebased = QFileInfo(originalTarget).fileName();
    if (rebased.contains(QLatin1Char('"'))) {
      error = QObject::tr("Cannot safely preserve Include target containing "
                          "a quote at line %1")
                  .arg(index + 1);
      return false;
    }
    if (!startsQuoted && rebased.contains(QLatin1Char('#')))
      rebased = QLatin1Char('"') + rebased + QLatin1Char('"');
    else if (startsQuoted)
      rebased = QLatin1Char('"') + rebased + QLatin1Char('"');

    const QString updated = raw.left(pathBegin) + rebased + raw.mid(pathEnd);
    if (updated != raw)
      config.replace(index, updated);
  }
  return true;
}

} // namespace

#ifdef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
class ResponsePlot final : public QWidget {
public:
  explicit ResponsePlot(QVector<ResponseCurve> curves,
                        QWidget *parent = nullptr)
      : QWidget(parent), curves(std::move(curves)) {
    setObjectName(QStringLiteral("responsePlot"));
    if (!this->curves.isEmpty())
      setProperty("analysisBinCount", this->curves.front().points.size());
    for (const auto &curve : this->curves) {
      if (curve.channel != QStringLiteral("L → L"))
        continue;
      if (curve.points.isEmpty())
        continue;
      auto closest = curve.points.cbegin();
      for (auto point = curve.points.cbegin(); point != curve.points.cend();
           ++point) {
        if (std::abs(point->x() - 1000.0) < std::abs(closest->x() - 1000.0))
          closest = point;
      }
      if (closest != curve.points.cend())
        setProperty("responseAt1000HzLtoLDb", closest->y());
      break;
    }
    setMinimumSize(700, 380);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette().base());
    const QRectF plot(68, 18, width() - 88, height() - 64);
    constexpr double minHz = 20.0, maxHz = 24000.0;
    constexpr double minDb = -24.0, maxDb = 12.0;
    const auto xFor = [&plot](double hz) {
      return plot.left() +
             std::log(hz / minHz) / std::log(maxHz / minHz) * plot.width();
    };
    const auto yFor = [&plot](double db) {
      return plot.bottom() - (db - minDb) / (maxDb - minDb) * plot.height();
    };
    painter.setPen(QPen(palette().mid().color(), 1));
    for (int db = -24; db <= 12; db += 6) {
      const double y = yFor(db);
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
      painter.drawText(QRectF(4, y - 9, 56, 18),
                       Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1 dB").arg(db));
    }
    for (const double hz : {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0,
                            5000.0, 10000.0, 20000.0}) {
      const double x = xFor(hz);
      painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
      painter.drawText(QRectF(x - 30, plot.bottom() + 5, 60, 18),
                       Qt::AlignHCenter,
                       hz >= 1000.0 ? QStringLiteral("%1k").arg(hz / 1000.0)
                                    : QString::number(static_cast<int>(hz)));
    }
    painter.setPen(palette().text().color());
    painter.drawText(QRectF(plot.left(), height() - 22, plot.width(), 18),
                     Qt::AlignHCenter, tr("Frequency (Hz, logarithmic)"));
    const QList<QColor> colors{QColor(40, 120, 220), QColor(220, 90, 65),
                               QColor(50, 160, 100), QColor(150, 80, 180)};
    int curveIndex = 0;
    for (const auto &curve : curves) {
      QPainterPath path;
      bool started = false;
      for (const auto &point : curve.points) {
        if (point.x() < minHz || point.x() > maxHz)
          continue;
        const QPointF mapped(xFor(point.x()),
                             yFor(std::clamp(point.y(), minDb, maxDb)));
        if (!started) {
          path.moveTo(mapped);
          started = true;
        } else {
          path.lineTo(mapped);
        }
      }
      painter.setPen(QPen(colors[curveIndex % colors.size()], 2));
      painter.drawPath(path);
      painter.drawText(QPointF(plot.left() + curveIndex * 110, plot.top() + 14),
                       curve.channel);
      ++curveIndex;
    }
  }

private:
  QVector<ResponseCurve> curves;
};
#endif

MainWindow::MainWindow(QString path, QString cliExecutable)
    : cliExecutable(std::move(cliExecutable)) {
  Q_INIT_RESOURCE(skyapo_editor);
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  Q_INIT_RESOURCE(skyapo_graphiceq);
#endif
  setWindowTitle(tr("Equalizer APO Configuration Editor — SkyAPO"));
  resize(1080, 760);

  auto *root = new QWidget(this);
  auto *outer = new QVBoxLayout(root);
  auto *toolbar = new QHBoxLayout;
  pathEdit = new QLineEdit(root);
  pathEdit->setObjectName(QStringLiteral("configPath"));
  pathEdit->setReadOnly(true);
  auto *openButton = new QPushButton(tr("Open…"), root);
  auto *saveButton = new QPushButton(tr("Save"), root);
  auto *checkButton = new QPushButton(tr("Save & Check"), root);
  auto *reloadButton = new QPushButton(tr("Save & Reload"), root);
  auto *addButton = new QPushButton(tr("Add filter"), root);
  responseButton = new QPushButton(tr("Analyze response"), root);
  responseButton->setObjectName(QStringLiteral("analyzeResponse"));
  responseSourceCombo = new QComboBox(root);
  responseSourceCombo->setObjectName(QStringLiteral("analysisSource"));
  responseSourceCombo->addItem(tr("config.txt"), false);
  responseSourceCombo->addItem(tr("Current file"), true);
  responseSourceCombo->setToolTip(
      tr("Choose whether response analysis starts from the default config.txt "
         "or the configuration currently open in the editor."));
  responseResolutionCombo = new QComboBox(root);
  responseResolutionCombo->setObjectName(QStringLiteral("analysisResolution"));
  for (unsigned frames = 128; frames <= (1U << 20); frames *= 2)
    responseResolutionCombo->addItem(tr("%1 samples").arg(frames),
                                     QVariant::fromValue(frames));
  responseResolutionCombo->setToolTip(
      tr("FFT resolution for theoretical response analysis. Larger values "
         "increase frequency detail and analysis time. Estimated sample, FFT "
         "and curve buffers are capped at 64 MiB; Engine and FFT library "
         "overhead is additional."));
  {
    QSettings settings;
    const int savedIndex = responseResolutionCombo->findData(
        settings.value(QStringLiteral("analysis/resolution"), 16384).toUInt());
    responseResolutionCombo->setCurrentIndex(
        savedIndex >= 0 ? savedIndex
                        : responseResolutionCombo->findData(16384U));
  }
  connect(responseResolutionCombo,
          qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
            QSettings settings;
            settings.setValue(QStringLiteral("analysis/resolution"),
                              responseResolutionCombo->currentData());
          });
#ifndef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  responseButton->setEnabled(false);
  responseButton->setToolTip(tr("Requires FFTW3f support at build time"));
#endif
  toolbar->addWidget(pathEdit, 1);
  toolbar->addWidget(openButton);
  toolbar->addWidget(saveButton);
  toolbar->addWidget(checkButton);
  toolbar->addWidget(reloadButton);
  toolbar->addWidget(addButton);
  toolbar->addWidget(new QLabel(tr("Analyze from:"), root));
  toolbar->addWidget(responseSourceCombo);
  toolbar->addWidget(new QLabel(tr("FFT:"), root));
  toolbar->addWidget(responseResolutionCombo);
  toolbar->addWidget(responseButton);
  outer->addLayout(toolbar);

  auto *analysisFormatRow = new QHBoxLayout;
  analysisUseRuntimeFormat =
      new QCheckBox(tr("Use live PipeWire format"), root);
  analysisUseRuntimeFormat->setObjectName(
      QStringLiteral("analysisUseLiveFormat"));
  analysisUseRuntimeFormat->setChecked(true);
  analysisUseRuntimeFormat->setToolTip(
      tr("Use the currently streaming daemon's negotiated sample rate and "
         "channel count. Uncheck to choose a theoretical analysis format "
         "manually when the daemon is not running."));
  analysisSampleRate = new QSpinBox(root);
  analysisSampleRate->setObjectName(QStringLiteral("analysisSampleRate"));
  analysisSampleRate->setRange(8000, 384000);
  analysisSampleRate->setSingleStep(1000);
  analysisSampleRate->setSuffix(tr(" Hz"));
  analysisChannels = new QSpinBox(root);
  analysisChannels->setObjectName(QStringLiteral("analysisChannels"));
  analysisChannels->setRange(1, 8);
  analysisChannels->setSuffix(tr(" channels"));
  {
    QSettings settings;
    analysisSampleRate->setValue(
        settings.value(QStringLiteral("analysis/sample-rate"), 48000).toInt());
    analysisChannels->setValue(
        settings.value(QStringLiteral("analysis/channels"), 2).toInt());
  }
  const auto updateManualFormatEnabled = [this] {
    const bool manual = !analysisUseRuntimeFormat->isChecked();
    analysisSampleRate->setEnabled(manual);
    analysisChannels->setEnabled(manual);
  };
  connect(analysisUseRuntimeFormat, &QCheckBox::toggled, this,
          [updateManualFormatEnabled](bool) { updateManualFormatEnabled(); });
  connect(analysisSampleRate, qOverload<int>(&QSpinBox::valueChanged), this,
          [](int value) {
            QSettings().setValue(QStringLiteral("analysis/sample-rate"), value);
          });
  connect(analysisChannels, qOverload<int>(&QSpinBox::valueChanged), this,
          [](int value) {
            QSettings().setValue(QStringLiteral("analysis/channels"), value);
          });
  updateManualFormatEnabled();
  analysisFormatRow->addWidget(analysisUseRuntimeFormat);
  analysisFormatRow->addWidget(new QLabel(tr("Manual rate:"), root));
  analysisFormatRow->addWidget(analysisSampleRate);
  analysisFormatRow->addWidget(new QLabel(tr("Channels:"), root));
  analysisFormatRow->addWidget(analysisChannels);
  analysisFormatRow->addStretch(1);
  outer->addLayout(analysisFormatRow);

  auto *deviceRow = new QHBoxLayout;
  deviceCombo = new QComboBox(root);
  deviceCombo->setObjectName(QStringLiteral("inputDeviceCombo"));
  deviceCombo->setMinimumWidth(380);
  deviceCombo->setAccessibleName(tr("Selected PipeWire capture device"));
  deviceCombo->setToolTip(
      tr("The selected device is stored by its stable PipeWire node name. "
         "Enumerated channel count and sample rate appear when available; "
         "the negotiated runtime format is shown in daemon status."));
  auto *refreshButton = new QPushButton(tr("Refresh devices"), root);
  auto *startButton = new QPushButton(tr("Start daemon"), root);
  auto *stopButton = new QPushButton(tr("Stop daemon"), root);
  startButton->setObjectName(QStringLiteral("startDaemon"));
  stopButton->setObjectName(QStringLiteral("stopDaemon"));
  deviceRow->addWidget(new QLabel(tr("Input device:"), root));
  deviceRow->addWidget(deviceCombo, 1);
  deviceRow->addWidget(refreshButton);
  deviceRow->addWidget(startButton);
  deviceRow->addWidget(stopButton);
  outer->addLayout(deviceRow);

  auto *scroll = new QScrollArea(root);
  scroll->setWidgetResizable(true);
  rowTable = new FilterTable(scroll);
  rowTable->setHandlers(
      [this](QMenu *menu) { populateAddPopupMenu(menu); },
      [this](const QString &line, FilterTable::Item *before) {
        if (line == PluginPickerLine) {
          addPluginToConfiguration(
              before ? std::optional<qsizetype>(before->index) : std::nullopt);
          return;
        }
        document.insert(before ? before->index : document.lineCount(), line);
        markModified();
      },
      [this](FilterTable::Item *item) {
        if (item && item->index >= 0 && item->index < document.lineCount()) {
          document.remove(item->index);
          markModified();
        }
      },
      [this] { rebuildRows(); }, [this] { syncRowsToDocument(); },
      [this](const std::vector<FilterTable::Item *> &items) {
        for (auto *item : items)
          if (item && item->index >= 0 && item->index < document.lineCount())
            document.remove(item->index);
        if (!items.empty()) {
          markModified();
          rebuildRows();
        }
      },
      [this](FilterTable::Item *item, int offset) {
        if (!item)
          return;
        const qsizetype target = item->index + offset;
        if (target < 0 || target >= document.lineCount())
          return;
        document.move(item->index, target);
        markModified();
        rebuildRows();
      });
  rowTable->setFocusPolicy(Qt::StrongFocus);
  rowTable->createAddPopupMenu();
  scroll->setWidget(rowTable);
  outer->addWidget(scroll, 1);
  statusLabel = new QLabel(tr("Checking daemon…"), root);
  statusLabel->setObjectName(QStringLiteral("daemonStatus"));
  statusLabel->setAccessibleName(tr("PipeWire and SkyAPO runtime status"));
  statusLabel->setTextFormat(Qt::PlainText);
  statusLabel->setWordWrap(true);
  statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  outer->addWidget(statusLabel);
  validationLabel = new QLabel(tr("Configuration not checked"), root);
  validationLabel->setObjectName(QStringLiteral("liveConfigValidation"));
  validationLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto *validationRow = new QHBoxLayout;
  validationRow->addWidget(validationLabel, 1);
  diagnosticSourceButton = new QPushButton(tr("Open error location"), root);
  diagnosticSourceButton->setObjectName(QStringLiteral("openDiagnosticSource"));
  diagnosticSourceButton->hide();
  validationRow->addWidget(diagnosticSourceButton);
  outer->addLayout(validationRow);
  connect(diagnosticSourceButton, &QPushButton::clicked, this,
          &MainWindow::openDiagnosticSource);
  auto *fileMenu = menuBar()->addMenu(tr("File"));
  auto *saveAsAction = fileMenu->addAction(tr("Save Configuration As…"));
  saveAsAction->setObjectName(QStringLiteral("saveConfigAs"));
  connect(saveAsAction, &QAction::triggered, this, &MainWindow::saveConfigAs);
  recentFilesMenu = fileMenu->addMenu(tr("Open Recent"));
  recentFilesMenu->setObjectName(QStringLiteral("recentFilesMenu"));
  refreshRecentFilesMenu();
  auto *toolsMenu = menuBar()->addMenu(tr("Tools"));
  auto *addInstalledPluginAction =
      toolsMenu->addAction(tr("Add installed plugin…"));
  addInstalledPluginAction->setObjectName(QStringLiteral("addInstalledPlugin"));
  connect(addInstalledPluginAction, &QAction::triggered, this,
          [this] { addPluginToConfiguration(); });
  toolsMenu->addSeparator();
  auto *pluginCatalogAction =
      toolsMenu->addAction(tr("Browse installed plugins…"));
  pluginCatalogAction->setObjectName(QStringLiteral("browsePlugins"));
  connect(pluginCatalogAction, &QAction::triggered, this,
          [this, pluginCatalogAction] {
            if (pluginRequestPending)
              return;
            pluginRequestPending = true;
            pluginCatalogAction->setEnabled(false);
            runCli({"plugin", "list"}, [this, pluginCatalogAction](
                                           int result, QByteArray output,
                                           QByteArray error) {
              pluginRequestPending = false;
              pluginCatalogAction->setEnabled(true);
              if (result != 0) {
                QMessageBox::warning(
                    this, tr("Plugin catalog"),
                    QString::fromUtf8(error.isEmpty() ? output : error));
                return;
              }
              auto *dialog = new QDialog(this);
              dialog->setAttribute(Qt::WA_DeleteOnClose);
              dialog->setWindowTitle(tr("Installed plugins"));
              auto *layout = new QVBoxLayout(dialog);
              auto *catalog = new QPlainTextEdit(dialog);
              catalog->setObjectName(QStringLiteral("pluginCatalog"));
              catalog->setReadOnly(true);
              catalog->setLineWrapMode(QPlainTextEdit::NoWrap);
              catalog->setPlainText(QString::fromUtf8(output));
              layout->addWidget(catalog);
              auto *close = new QPushButton(tr("Close"), dialog);
              connect(close, &QPushButton::clicked, dialog, &QDialog::accept);
              layout->addWidget(close, 0, Qt::AlignRight);
              dialog->resize(760, 480);
              dialog->show();
            });
          });
  toolsMenu->addSeparator();
  auto *pluginParametersAction =
      toolsMenu->addAction(tr("Configure selected plugin parameters…"));
  pluginParametersAction->setObjectName(
      QStringLiteral("configureSelectedPluginParameters"));
  connect(pluginParametersAction, &QAction::triggered, this,
          &MainWindow::configureSelectedPluginParameters);
  auto *helpMenu = menuBar()->addMenu(tr("Help"));
  auto *aboutAction = helpMenu->addAction(tr("About SkyAPO"));
  aboutAction->setObjectName(QStringLiteral("aboutSkyAPO"));
  connect(aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
  setCentralWidget(root);
  QSettings settings;
  const QByteArray savedGeometry =
      settings.value(QStringLiteral("ui/geometry")).toByteArray();
  if (!savedGeometry.isEmpty())
    restoreGeometry(savedGeometry);
  const QByteArray savedState =
      settings.value(QStringLiteral("ui/state")).toByteArray();
  if (!savedState.isEmpty())
    restoreState(savedState);

  validationTimer = new QTimer(this);
  validationTimer->setSingleShot(true);
  validationTimer->setInterval(350);
  connect(validationTimer, &QTimer::timeout, this,
          &MainWindow::validateLiveConfig);

  connect(openButton, &QPushButton::clicked, this, [this] {
    if (modified &&
        QMessageBox::question(
            this, tr("Unsaved changes"),
            tr("Discard unsaved edits and open another configuration?")) !=
            QMessageBox::Yes)
      return;
    QSettings settings;
    const QString initialDirectory =
        configPath.isEmpty()
            ? settings.value(QStringLiteral("ui/lastDirectory")).toString()
            : QFileInfo(configPath).absolutePath();
    const auto selected = QFileDialog::getOpenFileName(
        this, tr("Open configuration"), initialDirectory,
        tr("Configuration files (config.txt *.txt);;All files (*)"));
    if (!selected.isEmpty())
      openConfig(selected);
  });
  connect(saveButton, &QPushButton::clicked, this, &MainWindow::saveConfig);
  connect(checkButton, &QPushButton::clicked, this, &MainWindow::checkConfig);
  connect(reloadButton, &QPushButton::clicked, this, &MainWindow::reloadConfig);
  connect(addButton, &QPushButton::clicked, this, [this, addButton] {
    QMenu menu(addButton);
    populateAddPopupMenu(&menu);
    if (auto *action =
            menu.exec(addButton->mapToGlobal(QPoint(0, addButton->height())))) {
      const auto line = action->data().value<FilterTemplate>().getLine();
      if (line == PluginPickerLine)
        addPluginToConfiguration();
      else
        addFilter(line);
    }
  });
  connect(responseButton, &QPushButton::clicked, this,
          &MainWindow::analyzeResponse);
  connect(refreshButton, &QPushButton::clicked, this,
          &MainWindow::refreshDevices);
  connect(deviceCombo, qOverload<int>(&QComboBox::activated), this,
          &MainWindow::selectDevice);
  const auto runDaemonControl = [this, startButton,
                                 stopButton](const QString &command) {
    if (daemonControlPending)
      return;
    daemonControlPending = true;
    startButton->setEnabled(false);
    stopButton->setEnabled(false);
    runCli({command}, [this, startButton, stopButton](
                          int result, QByteArray output, QByteArray error) {
      daemonControlPending = false;
      startButton->setEnabled(true);
      stopButton->setEnabled(true);
      if (result != 0)
        QMessageBox::warning(
            this, tr("SkyAPO"),
            QString::fromUtf8(error.isEmpty() ? output : error));
      refreshStatus();
    });
  };
  connect(startButton, &QPushButton::clicked, this,
          [runDaemonControl] { runDaemonControl(QStringLiteral("start")); });
  connect(stopButton, &QPushButton::clicked, this,
          [runDaemonControl] { runDaemonControl(QStringLiteral("stop")); });

  if (path.isEmpty()) {
    path = defaultConfigPath();
    if (!QFileInfo::exists(path)) {
      QDir().mkpath(QFileInfo(path).absolutePath());
      QSaveFile file(path);
      if (file.open(QIODevice::WriteOnly)) {
        file.write("Preamp: 0 dB\n");
        file.commit();
      }
    }
  }
  openConfig(path);
  refreshDevices();
  refreshStatus();
  auto *timer = new QTimer(this);
  connect(timer, &QTimer::timeout, this, &MainWindow::refreshStatus);
  timer->start(2000);
  auto *deviceTimer = new QTimer(this);
  deviceTimer->setObjectName(QStringLiteral("deviceRefreshTimer"));
  deviceTimer->setInterval(5000);
  connect(deviceTimer, &QTimer::timeout, this, &MainWindow::refreshDevices);
  deviceTimer->start();
}

MainWindow::~MainWindow() {
  if (responseThread && responseThread->isRunning())
    responseThread->wait();
  const auto processes = findChildren<QProcess *>();
  for (auto *process : processes) {
    QObject::disconnect(process, nullptr, this, nullptr);
    if (process->state() != QProcess::NotRunning)
      process->kill();
  }
}

void MainWindow::analyzeResponse() {
#ifndef SKYAPO_UI_HAVE_RESPONSE_ANALYSIS
  return;
#else
  if (configPath.isEmpty())
    return;
  const bool useCurrentFile = responseSourceCombo->currentData().toBool();
  const QString sourcePath = useCurrentFile ? configPath : defaultConfigPath();
  QByteArray contents;
  if (useCurrentFile) {
    contents = document.serialize();
  } else {
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
      QMessageBox::warning(
          this, tr("Response analysis"),
          tr("Could not read the default configuration '%1': %2")
              .arg(sourcePath, source.errorString()));
      return;
    }
    contents = source.readAll();
  }
  const QString directory = QFileInfo(sourcePath).absolutePath();
  auto snapshot = std::make_shared<QTemporaryFile>(
      directory + QStringLiteral("/.skyapo-response-XXXXXX"));
  if (!snapshot->open() || snapshot->write(contents) != contents.size() ||
      !snapshot->flush()) {
    QMessageBox::warning(this, tr("Response analysis"),
                         tr("Could not create a config snapshot: %1")
                             .arg(snapshot->errorString()));
    return;
  }
  const QString snapshotPath =
      QFileInfo(snapshot->fileName()).absoluteFilePath();
  const unsigned fftFrames = responseResolutionCombo->currentData().toUInt();
  const bool useRuntimeFormat = analysisUseRuntimeFormat->isChecked();
  const unsigned selectedRate =
      static_cast<unsigned>(analysisSampleRate->value());
  const unsigned selectedChannels =
      static_cast<unsigned>(analysisChannels->value());
  snapshot->close();
  responseButton->setEnabled(false);
  responseButton->setText(useRuntimeFormat ? tr("Reading format…")
                                           : tr("Analyzing…"));
  QPointer<MainWindow> self(this);
  const auto startAnalysis = [self, snapshot, snapshotPath, fftFrames](
                                 unsigned sampleRate, unsigned channels) {
    if (!self)
      return;
    self->responseButton->setText(self->tr("Analyzing…"));
    auto *worker = QThread::create([self, snapshot, snapshotPath, fftFrames,
                                    sampleRate, channels] {
      auto result =
          analyzeConfigResponse(snapshotPath, sampleRate, channels, fftFrames);
      if (!self)
        return;
      QMetaObject::invokeMethod(
          self,
          [self, snapshot, fftFrames, sampleRate, channels,
           result = std::move(result)]() mutable {
            Q_UNUSED(snapshot);
            if (!self)
              return;
            self->responseButton->setEnabled(true);
            self->responseButton->setText(self->tr("Analyze response"));
            if (!result.error.isEmpty()) {
              QMessageBox::warning(self, self->tr("Response analysis"),
                                   result.error);
              return;
            }
            QDialog dialog(self);
            dialog.setWindowTitle(self->tr("Calculated filter response"));
            auto *layout = new QVBoxLayout(&dialog);
            layout->addWidget(new QLabel(
                self->tr("EAPO Engine impulse response · %1 Hz · %2 %3 · "
                         "%4 FFT samples · all input/output channel paths")
                    .arg(sampleRate)
                    .arg(channels)
                    .arg(channels == 1 ? self->tr("channel")
                                       : self->tr("channels"))
                    .arg(fftFrames),
                &dialog));
            layout->addWidget(
                new ResponsePlot(std::move(result.curves), &dialog));
            auto *close = new QPushButton(self->tr("Close"), &dialog);
            QObject::connect(close, &QPushButton::clicked, &dialog,
                             &QDialog::accept);
            layout->addWidget(close, 0, Qt::AlignRight);
            dialog.resize(820, 500);
            dialog.exec();
          },
          Qt::QueuedConnection);
    });
    self->responseThread = worker;
    worker->setParent(self);
    QObject::connect(worker, &QThread::finished, self, [self, worker] {
      if (self && self->responseThread == worker)
        self->responseThread = nullptr;
      worker->deleteLater();
    });
    worker->start();
  };
  if (!useRuntimeFormat) {
    startAnalysis(selectedRate, selectedChannels);
    return;
  }
  runCli({QStringLiteral("status")}, [self, startAnalysis](int result,
                                                           QByteArray output,
                                                           QByteArray error) {
    if (!self)
      return;
    unsigned sampleRate = 0;
    unsigned channels = 0;
    if (result != 0 || !parseStreamingFormat(output, sampleRate, channels)) {
      self->responseButton->setEnabled(true);
      self->responseButton->setText(self->tr("Analyze response"));
      const QString detail =
          QString::fromUtf8(error.isEmpty() ? output : error).trimmed();
      QMessageBox::warning(
          self, self->tr("Response analysis"),
          self->tr("The daemon is not streaming with a known sample "
                   "rate and channel count. Start SkyAPO, or disable "
                   "'Use live PipeWire format' and choose a manual "
                   "analysis format.%1")
              .arg(detail.isEmpty() ? QString{}
                                    : QStringLiteral("\n") + detail));
      return;
    }
    startAnalysis(sampleRate, channels);
  });
#endif
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (!modified) {
    saveWindowPreferences();
    event->accept();
    return;
  }
  const auto choice = QMessageBox::warning(
      this, tr("Unsaved configuration"),
      tr("Save changes to %1 before closing?").arg(configPath),
      QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
      QMessageBox::Save);
  if (choice == QMessageBox::Cancel) {
    event->ignore();
  } else if (choice == QMessageBox::Save && !saveConfig()) {
    event->ignore();
  } else {
    saveWindowPreferences();
    event->accept();
  }
}

void MainWindow::saveWindowPreferences() {
  QSettings settings;
  settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
  settings.setValue(QStringLiteral("ui/state"), saveState());
  settings.setValue(
      QStringLiteral("ui/lastDirectory"),
      configPath.isEmpty() ? QString() : QFileInfo(configPath).absolutePath());
}

void MainWindow::rememberConfig(const QString &path) {
  if (path.isEmpty())
    return;
  QSettings settings;
  QStringList files =
      settings.value(QStringLiteral("ui/recentFiles")).toStringList();
  const QString absolutePath = QFileInfo(path).absoluteFilePath();
  files.removeAll(absolutePath);
  files.prepend(absolutePath);
  while (files.size() > 10)
    files.removeLast();
  settings.setValue(QStringLiteral("ui/recentFiles"), files);
  refreshRecentFilesMenu();
}

void MainWindow::refreshRecentFilesMenu() {
  if (!recentFilesMenu)
    return;
  recentFilesMenu->clear();
  QSettings settings;
  const QStringList files =
      settings.value(QStringLiteral("ui/recentFiles")).toStringList();
  int count = 0;
  for (const auto &path : files) {
    if (!QFileInfo::exists(path))
      continue;
    auto *action = recentFilesMenu->addAction(QFileInfo(path).fileName());
    action->setToolTip(path);
    action->setData(path);
    connect(action, &QAction::triggered, this, &MainWindow::openRecentFile);
    if (++count == 10)
      break;
  }
  if (count == 0) {
    auto *empty = recentFilesMenu->addAction(tr("No recent configurations"));
    empty->setEnabled(false);
  }
}

void MainWindow::openRecentFile() {
  auto *action = qobject_cast<QAction *>(sender());
  if (!action)
    return;
  if (modified &&
      QMessageBox::question(
          this, tr("Unsaved changes"),
          tr("Discard unsaved edits and open another configuration?")) !=
          QMessageBox::Yes)
    return;
  openConfig(action->data().toString());
}

QString MainWindow::defaultConfigPath() const {
  return QStandardPaths::writableLocation(
             QStandardPaths::GenericConfigLocation) +
         "/skyapo/config.txt";
}

void MainWindow::runCli(const QStringList &arguments,
                        CliCompletion completion) {
  auto *process = new QProcess(this);
  process->setProcessChannelMode(QProcess::SeparateChannels);
  struct CompletionState {
    bool delivered{};
    CliCompletion callback;
  };
  auto state = std::make_shared<CompletionState>();
  state->callback = std::move(completion);
  auto *timeout = new QTimer(process);
  timeout->setObjectName(QStringLiteral("cliTimeout"));
  timeout->setSingleShot(true);
  constexpr int cliTimeoutMilliseconds = 30000;
  const auto finish = [process, timeout, state](int result, QByteArray output,
                                                QByteArray error) {
    if (state->delivered)
      return;
    state->delivered = true;
    timeout->stop();
    process->deleteLater();
    if (state->callback)
      state->callback(result, std::move(output), std::move(error));
  };
  connect(
      process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
      [process, finish](int exitCode, QProcess::ExitStatus exitStatus) {
        const int result = exitStatus == QProcess::NormalExit ? exitCode : -1;
        finish(result, process->readAllStandardOutput(),
               process->readAllStandardError());
      });
  connect(process, &QProcess::errorOccurred, this,
          [process, finish](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
              finish(-1, process->readAllStandardOutput(),
                     process->errorString().toUtf8());
          });
  connect(timeout, &QTimer::timeout, this, [process, finish, arguments] {
    const QString command = arguments.join(QLatin1Char(' '));
    const QByteArray detail =
        tr("SkyAPO command timed out after %1 seconds: %2")
            .arg(cliTimeoutMilliseconds / 1000)
            .arg(command)
            .toUtf8();
    process->kill();
    finish(-1, process->readAllStandardOutput(), detail);
  });
  process->start(this->cliExecutable.isEmpty() ? defaultCliProgram()
                                               : this->cliExecutable,
                 arguments);
  timeout->start(cliTimeoutMilliseconds);
}

void MainWindow::openConfig(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::critical(this, tr("Open failed"), file.errorString());
    return;
  }
  configPath = QFileInfo(path).absoluteFilePath();
  rememberConfig(configPath);
  document.load(file.readAll());
  modified = false;
  ++configRevision;
  pathEdit->setText(configPath);
  rebuildRows();
  updateTitle();
  validationLabel->setText(tr("Checking configuration…"));
  scheduleLiveValidation();
}

void MainWindow::rebuildRows() {
  clearRowDiagnostics();
  rowTable->clearRows();
  rowItems.clear();

  static PreampFilterGUIFactory preampFactory;
  static BiQuadFilterGUIFactory biquadFactory;
  static DelayFilterGUIFactory delayFactory;
  static StageFilterGUIFactory stageFactory;
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  static GraphicEQFilterGUIFactory graphicEQFactory;
  graphicEQFactory.initialize(rowTable);
  graphicEQFactory.startOfFile(configPath);
#endif
  for (qsizetype index = 0; index < document.lineCount(); ++index) {
    const QString raw = document.line(index).toQString();
    auto item = std::make_shared<FilterTable::Item>();
    item->text = raw;
    item->index = static_cast<int>(index);

    IFilterGUI *editor = nullptr;
    const qsizetype colon = raw.indexOf(':');
    if (colon >= 0) {
      QString command = raw.left(colon).trimmed();
      QString parameters = raw.mid(colon + 1).trimmed();
      if (command == "Device")
        editor = new DeviceFilterEditor(parameters, rowTable);
      if (command == "Convolution")
        editor = new ConvolutionEditor(parameters, configPath, rowTable);
      if (command == "Include")
        editor = new IncludeEditor(parameters, configPath, rowTable);
      if (!editor)
        editor = IIRFilterEditor::create(command, parameters, rowTable);
      if (!editor)
        editor = ChannelCopyEditor::create(command, parameters, rowTable);
      if (!editor)
        editor = preampFactory.createFilterGUI(command, parameters);
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = biquadFactory.createFilterGUI(command, parameters);
      }
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = delayFactory.createFilterGUI(command, parameters);
      }
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = stageFactory.createFilterGUI(command, parameters);
        if (editor) {
          const QString unsupportedStageTooltip =
              tr("Windows playback stage; SkyAPO's PipeWire capture path does "
                 "not process this stage.");
          for (const auto *name : {"preMixCheckBox", "postMixCheckBox"}) {
            if (auto *stage = editor->findChild<QCheckBox *>(name)) {
              stage->setEnabled(false);
              stage->setToolTip(unsupportedStageTooltip);
            }
          }
        }
      }
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
      if (!editor) {
        command = raw.left(colon).trimmed();
        parameters = raw.mid(colon + 1).trimmed();
        editor = graphicEQFactory.createFilterGUI(command, parameters);
      }
#endif
    }

    item->gui = editor;
    auto *row = new FilterTableRow(rowTable, static_cast<int>(index + 1),
                                   item.get(), editor);
    rowTable->addRow(row, item.get());
    if (editor) {
      connect(
          editor, &IFilterGUI::updateModel, this, [this, editor, item, index] {
            QString command, parameters;
            editor->store(command, parameters);
            const QString original = document.line(index).toQString();
            const qsizetype colon = original.indexOf(':');
            const QString key =
                colon >= 0 ? original.left(colon + 1) : command + ':';
            const QString afterColon =
                colon >= 0 ? original.mid(colon + 1) : QString{};
            qsizetype spacing = 0;
            while (spacing < afterColon.size() && afterColon[spacing].isSpace())
              ++spacing;
            const QString updated = key + afterColon.left(spacing) + parameters;
            document.replace(index, updated);
            item->text = updated;
            markModified();
          });
    }

    connect(row, &QObject::destroyed, this, [item] { Q_UNUSED(item); });
    rowItems.push_back(std::move(item));
  }
}

void MainWindow::populateAddPopupMenu(QMenu *menu) {
  PreampFilterGUIFactory preamp;
  BiQuadFilterGUIFactory biquad;
  DelayFilterGUIFactory delay;
  StageFilterGUIFactory stage;
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  GraphicEQFilterGUIFactory graphicEQ;
#endif
  const auto append = [menu](const QList<FilterTemplate> &templates) {
    for (const auto &filter : templates) {
      auto *action = menu->addAction(filter.getName());
      action->setData(QVariant::fromValue(filter));
    }
  };
  append(preamp.createFilterTemplates());
  menu->addAction(tr("Include configuration…"))
      ->setData(QVariant::fromValue(FilterTemplate(tr("Include configuration…"),
                                                   "Include: child.txt",
                                                   {tr("Configuration")})));
  menu->addAction(tr("Device match"))
      ->setData(QVariant::fromValue(FilterTemplate(
          tr("Device match"), "Device: all", {tr("Configuration")})));
  menu->addAction(tr("Channel selection"))
      ->setData(QVariant::fromValue(FilterTemplate(
          tr("Channel selection"), "Channel: L R", {tr("Basic filters")})));
  menu->addAction(tr("Copy between channels"))
      ->setData(QVariant::fromValue(FilterTemplate(
          tr("Copy between channels"), "Copy: L=L", {tr("Basic filters")})));
  menu->addSection(tr("Parametric filters"));
  append(biquad.createFilterTemplates());
  menu->addAction(tr("Custom IIR filter"))
      ->setData(QVariant::fromValue(
          FilterTemplate(tr("Custom IIR filter"),
                         "Filter: ON IIR Order 2 Coefficients 1 0 0 1 0 0",
                         {tr("Parametric filters")})));
  menu->addSeparator();
  append(delay.createFilterTemplates());
  append(stage.createFilterTemplates());
  menu->addSection(tr("Plugins"));
  menu->addAction(tr("Installed plugin…"))
      ->setData(QVariant::fromValue(FilterTemplate(
          tr("Installed plugin…"), PluginPickerLine, {tr("Plugins")})));
  menu->addAction(tr("Convolution (impulse response)"))
      ->setData(QVariant::fromValue(FilterTemplate(
          tr("Convolution (impulse response)"),
          "Convolution: impulse-response.wav", {tr("Advanced filters")})));
#ifdef SKYAPO_HAVE_GRAPHIC_EQ
  menu->addSeparator();
  append(graphicEQ.createFilterTemplates());
#endif
}

void MainWindow::syncRowsToDocument() {
  const qsizetype common =
      std::min<qsizetype>(rowItems.size(), document.lineCount());
  for (qsizetype index = 0; index < common; ++index) {
    const auto &item = rowItems[static_cast<size_t>(index)];
    if (document.line(index).toQString() != item->text) {
      document.replace(index, item->text);
      markModified();
    }
  }
}

void MainWindow::addFilter(const QString &line) {
  const bool hadLines = document.lineCount() > 0;
  const qsizetype last = document.lineCount() - 1;
  if (hadLines && document.line(last).ending.isEmpty())
    document.insert(last + 1, line);
  else
    document.insert(document.lineCount(), line);
  markModified();
  rebuildRows();
}

void MainWindow::addPluginToConfiguration(
    std::optional<qsizetype> beforeIndex) {
  if (pluginRequestPending)
    return;
  pluginRequestPending = true;
  const quint64 requestedRevision = configRevision;
  const auto requestedIndex = beforeIndex;
  auto *insertAction = findChild<QAction *>("addInstalledPlugin");
  if (insertAction)
    insertAction->setEnabled(false);

  runCli({"plugin", "list"}, [this, insertAction, requestedRevision,
                              requestedIndex](int result, QByteArray output,
                                              QByteArray error) {
    pluginRequestPending = false;
    if (insertAction)
      insertAction->setEnabled(true);
    if (result != 0) {
      QMessageBox::warning(this, tr("Add plugin"),
                           QString::fromUtf8(error.isEmpty() ? output : error));
      return;
    }

    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("pluginPicker"));
    dialog.setWindowTitle(tr("Add installed plugin"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(
        tr("Choose a discovered LV2, CLAP, or VST3 plugin. The editor "
           "inserts its config directive; edit parameters in the "
           "configuration row."),
        &dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *plugins = new QListWidget(&dialog);
    plugins->setObjectName(QStringLiteral("pluginPickerList"));
    for (const auto &line :
         QString::fromUtf8(output).split(QLatin1Char('\n'))) {
      const auto fields = line.split(QLatin1Char('\t'));
      if (fields.size() != 3 || (fields[0] != QStringLiteral("LV2") &&
                                 fields[0] != QStringLiteral("CLAP") &&
                                 fields[0] != QStringLiteral("VST3")))
        continue;
      const QString id = fields[1].trimmed();
      if (id.isEmpty() || std::any_of(id.cbegin(), id.cend(),
                                      [](QChar ch) { return ch.isSpace(); }))
        continue;
      auto *item =
          new QListWidgetItem(QStringLiteral("%1 — %2 (%3)")
                                  .arg(fields[2].trimmed(), fields[0], id),
                              plugins);
      item->setData(Qt::UserRole,
                    QStringLiteral("Plugin: %1 %2").arg(fields[0], id));
    }
    if (plugins->count() == 0) {
      QMessageBox::information(
          this, tr("Add plugin"),
          tr("No supported plugins were discovered. Check that a "
             "native LV2, CLAP, or VST3 host was built and the plugin "
             "is installed."));
      return;
    }
    plugins->setCurrentRow(0);
    layout->addWidget(plugins);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->setObjectName(QStringLiteral("pluginPickerButtons"));
    auto *addButton = buttons->button(QDialogButtonBox::Ok);
    addButton->setObjectName(QStringLiteral("addPluginButton"));
    addButton->setText(tr("Add"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted)
      return;

    if (configRevision != requestedRevision) {
      QMessageBox::warning(
          this, tr("Add plugin"),
          tr("The configuration changed while the plugin list was "
             "loading. Open the picker again to avoid inserting into "
             "a stale location."));
      return;
    }
    const auto selectedLine =
        plugins->currentItem()->data(Qt::UserRole).toString();
    const qsizetype index =
        requestedIndex
            ? std::clamp<qsizetype>(*requestedIndex, 0, document.lineCount())
            : document.lineCount();
    document.insert(index, selectedLine);
    markModified();
    rebuildRows();
  });
}

void MainWindow::configureSelectedPluginParameters() {
  if (pluginRequestPending)
    return;
  auto *item = rowTable ? rowTable->getFocusedItem() : nullptr;
  if (!item || item->index < 0 || item->index >= document.lineCount()) {
    QMessageBox::information(this, tr("Plugin parameters"),
                             tr("Select a Plugin: row first."));
    return;
  }
  const qsizetype index = item->index;
  const QString originalLine = document.line(index).toQString();
  const QString active = withoutInlineComment(originalLine);
  static const QRegularExpression directiveExpression(
      QStringLiteral(R"(^\s*Plugin\s*:\s*(LV2|CLAP|VST3)\s+(\S+))"),
      QRegularExpression::CaseInsensitiveOption);
  const auto directive = directiveExpression.match(active);
  if (!directive.hasMatch()) {
    QMessageBox::information(
        this, tr("Plugin parameters"),
        tr("Select a native LV2, CLAP, or VST3 Plugin: row. Other plugin "
           "directives remain editable as raw text."));
    return;
  }

  const QString format = directive.captured(1).toUpper();
  const QString pluginId = directive.captured(2);
  const quint64 requestedRevision = configRevision;
  auto *action =
      findChild<QAction *>(QStringLiteral("configureSelectedPluginParameters"));
  pluginRequestPending = true;
  if (action)
    action->setEnabled(false);

  runCli({QStringLiteral("plugin"), QStringLiteral("info"), pluginId},
         [this, action, requestedRevision, index, originalLine, format,
          pluginId](int result, QByteArray output, QByteArray error) {
           pluginRequestPending = false;
           if (action)
             action->setEnabled(true);
           if (result != 0) {
             QMessageBox::warning(
                 this, tr("Plugin parameters"),
                 QString::fromUtf8(error.isEmpty() ? output : error));
             return;
           }
           if (configRevision != requestedRevision ||
               index >= document.lineCount() ||
               document.line(index).toQString() != originalLine) {
             QMessageBox::warning(
                 this, tr("Plugin parameters"),
                 tr("The selected configuration row changed while plugin "
                    "metadata was loading. Select it again and retry."));
             return;
           }

           QVector<PluginParameterModel> parameters;
           QString errorText;
           if (!parsePluginMetadata(output, format, pluginId, parameters,
                                    errorText)) {
             QMessageBox::warning(this, tr("Plugin parameters"), errorText);
             return;
           }
           if (parameters.isEmpty()) {
             QMessageBox::information(
                 this, tr("Plugin parameters"),
                 tr("This plugin exposes no writable input parameters."));
             return;
           }

           QHash<QString, double> values;
           for (const auto &parameter : parameters)
             values.insert(parameter.symbol, parameter.defaultValue);
           if (!readConfiguredPluginValues(originalLine, format, parameters,
                                           values, errorText)) {
             QMessageBox::warning(
                 this, tr("Plugin parameters"),
                 errorText + QLatin1Char('\n') +
                     tr("Edit the row as raw text to resolve it."));
             return;
           }

           QDialog dialog(this);
           dialog.setObjectName(QStringLiteral("pluginParameterEditor"));
           dialog.setWindowTitle(tr("Plugin parameters — %1").arg(pluginId));
           auto *layout = new QVBoxLayout(&dialog);
           auto *notice = new QLabel(
               tr("These are saved configuration values, not live plugin "
                  "readbacks. Save and Reload to apply changes."),
               &dialog);
           notice->setWordWrap(true);
           layout->addWidget(notice);
           auto *form = new QFormLayout;
           QHash<QString, QDoubleSpinBox *> editors;
           for (const auto &parameter : parameters) {
             auto *editor = new QDoubleSpinBox(&dialog);
             editor->setObjectName(
                 QStringLiteral("pluginParameter_%1").arg(parameter.symbol));
             editor->setDecimals(9);
             editor->setRange(parameter.minimum, parameter.maximum);
             editor->setValue(values.value(parameter.symbol));
             editor->setKeyboardTracking(false);
             form->addRow(parameter.name, editor);
             editors.insert(parameter.symbol, editor);
           }
           auto *formContainer = new QWidget(&dialog);
           formContainer->setLayout(form);
           auto *scroll = new QScrollArea(&dialog);
           scroll->setWidgetResizable(true);
           scroll->setWidget(formContainer);
           layout->addWidget(scroll, 1);
           auto *buttons = new QDialogButtonBox(
               QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
           buttons->setObjectName(QStringLiteral("pluginParameterButtons"));
           connect(buttons, &QDialogButtonBox::accepted, &dialog,
                   &QDialog::accept);
           connect(buttons, &QDialogButtonBox::rejected, &dialog,
                   &QDialog::reject);
           layout->addWidget(buttons);
           dialog.resize(560, static_cast<int>(std::min<qsizetype>(
                                  640, 180 + parameters.size() * 38)));
           if (dialog.exec() != QDialog::Accepted)
             return;

           for (auto iterator = editors.cbegin(); iterator != editors.cend();
                ++iterator)
             values.insert(iterator.key(), iterator.value()->value());
           QString updated;
           if (!patchPluginDirective(originalLine, format, pluginId, parameters,
                                     values, updated, errorText)) {
             QMessageBox::warning(this, tr("Plugin parameters"), errorText);
             return;
           }
           if (updated != originalLine) {
             document.replace(index, updated);
             markModified();
             rebuildRows();
           }
         });
}

void MainWindow::markModified() {
  modified = true;
  clearRowDiagnostics();
  ++configRevision;
  updateTitle();
  validationLabel->setText(tr("Checking unsaved configuration edits…"));
  scheduleLiveValidation();
}

void MainWindow::scheduleLiveValidation() {
  if (validationTimer)
    validationTimer->start();
}

void MainWindow::clearRowDiagnostics() {
  diagnosticSourcePath.clear();
  diagnosticSourceLine = 0;
  if (diagnosticSourceButton)
    diagnosticSourceButton->hide();
  for (const auto &item : rowItems) {
    if (!item || !item->row)
      continue;
    item->row->setProperty("skyapoValidationError", false);
    item->row->setToolTip({});
    item->row->setAccessibleDescription({});
    if (auto *number = item->row->findChild<QLabel *>("labelNumber")) {
      number->setStyleSheet({});
      number->setToolTip({});
    }
  }
}

void MainWindow::openDiagnosticSource() {
  if (diagnosticSourcePath.isEmpty())
    return;
  auto *window = new MainWindow(diagnosticSourcePath, cliExecutable);
  window->setAttribute(Qt::WA_DeleteOnClose);
  window->show();
  const int line = diagnosticSourceLine;
  QTimer::singleShot(0, window, [window, line] {
    if (line <= 0 || !window->rowTable)
      return;
    auto *item = window->rowTable->itemAt(line - 1);
    if (!item)
      return;
    window->rowTable->setSelection(item);
    if (auto *scroll = window->findChild<QScrollArea *>())
      scroll->ensureWidgetVisible(item->row);
  });
}

void MainWindow::showAbout() {
  QMessageBox::about(
      this, tr("About SkyAPO"),
      tr("SkyAPO — Linux audio processing built around Equalizer APO.\n\n"
         "SkyAPO and the reused Equalizer APO components are licensed under "
         "GNU GPL-2.0-or-later.\n"
         "This application uses Qt %1 Widgets through dynamic system-library "
         "linking under LGPL-3.0. Qt license text and third-party notices are "
         "included with the installed documentation.")
          .arg(QString::fromLatin1(qVersion())));
}

void MainWindow::setRowDiagnostic(int index, const QString &diagnostic) {
  if (index < 0 || static_cast<size_t>(index) >= rowItems.size())
    return;
  const auto &item = rowItems[static_cast<size_t>(index)];
  if (!item || !item->row)
    return;
  item->row->setProperty("skyapoValidationError", true);
  item->row->setToolTip(diagnostic);
  item->row->setAccessibleDescription(diagnostic);
  if (auto *number = item->row->findChild<QLabel *>("labelNumber")) {
    number->setStyleSheet(QStringLiteral("color: #c62828; font-weight: bold"));
    number->setToolTip(diagnostic);
  }
}

void MainWindow::validateLiveConfig() {
  if (configPath.isEmpty())
    return;

  const QString directory = QFileInfo(configPath).absolutePath();
  auto snapshot = std::make_shared<QTemporaryFile>(
      directory + QStringLiteral("/.skyapo-validation-XXXXXX"));
  snapshot->setAutoRemove(true);
  const QByteArray contents = document.serialize();
  if (!snapshot->open() || snapshot->write(contents) != contents.size() ||
      !snapshot->flush()) {
    validationLabel->setText(
        tr("Could not create a temporary config snapshot: %1")
            .arg(snapshot->errorString()));
    return;
  }
  const QString snapshotPath =
      QFileInfo(snapshot->fileName()).absoluteFilePath();
  snapshot->close();
  const quint64 revision = configRevision;
  runCli({"config", "check", "--json", snapshotPath}, [this, revision,
                                                       snapshotPath, snapshot](
                                                          int result,
                                                          QByteArray output,
                                                          QByteArray error) {
    Q_UNUSED(snapshot);
    if (revision != configRevision)
      return;

    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(output, &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
      const QString detail =
          QString::fromUtf8(error.isEmpty() ? output : error).trimmed();
      validationLabel->setText(
          tr("Config validation failed: %1")
              .arg(detail.isEmpty() ? tr("invalid response") : detail));
      return;
    }

    clearRowDiagnostics();
    const QJsonObject report = json.object();
    if (result == 0 && report.value(QStringLiteral("valid")).toBool()) {
      validationLabel->setText(
          tr("Valid config — %1 filters%2")
              .arg(report.value(QStringLiteral("filter_count")).toInt())
              .arg(modified ? tr(" (unsaved edits)") : QString{}));
      return;
    }

    const QJsonArray diagnostics =
        report.value(QStringLiteral("diagnostics")).toArray();
    if (diagnostics.isEmpty()) {
      validationLabel->setText(tr("Configuration is invalid"));
      return;
    }
    const QJsonObject diagnostic = diagnostics.first().toObject();
    QString source = diagnostic.value(QStringLiteral("file")).toString();
    const QString actualSource = source;
    const int line = diagnostic.value(QStringLiteral("line")).toInt();
    const QJsonArray includeChain =
        diagnostic.value(QStringLiteral("include_chain")).toArray();
    QString rootIncludeFile;
    int rootIncludeLine = 0;
    if (!includeChain.isEmpty()) {
      const QJsonObject rootSite = includeChain.first().toObject();
      rootIncludeFile = rootSite.value(QStringLiteral("file")).toString();
      rootIncludeLine = rootSite.value(QStringLiteral("line")).toInt();
    }
    if (source.isEmpty() ||
        QFileInfo(source).absoluteFilePath() == snapshotPath)
      source = configPath;
    const QString location =
        line > 0 ? QStringLiteral("%1:%2").arg(source).arg(line) : source;
    const QString reason =
        diagnostic.value(QStringLiteral("reason")).toString();
    const bool includeFromSnapshot =
        !rootIncludeFile.isEmpty() && rootIncludeLine > 0 &&
        QFileInfo(rootIncludeFile).absoluteFilePath() ==
            QFileInfo(snapshotPath).absoluteFilePath();
    if (includeFromSnapshot) {
      const QString detail =
          tr("Included file error — %1: %2")
              .arg(location, reason.isEmpty() ? tr("unknown error") : reason);
      setRowDiagnostic(rootIncludeLine - 1, detail);
      diagnosticSourcePath = actualSource;
      diagnosticSourceLine = line;
      diagnosticSourceButton->show();
    } else if (QFileInfo(source).absoluteFilePath() ==
               QFileInfo(configPath).absoluteFilePath()) {
      setRowDiagnostic(line - 1,
                       reason.isEmpty() ? tr("Invalid configuration") : reason);
    }
    validationLabel->setText(
        tr("Invalid config — %1: %2")
            .arg(location, reason.isEmpty() ? tr("unknown error") : reason));
  });
}

void MainWindow::updateTitle() {
  setWindowTitle(
      QStringLiteral("%1%2 — Equalizer APO Configuration Editor — SkyAPO")
          .arg(modified ? QStringLiteral("* ") : QString{}, configPath));
}

bool MainWindow::saveConfig() {
  if (configPath.isEmpty())
    return false;
  if (!modified)
    return true;
  QSaveFile file(configPath);
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(document.serialize()) < 0 || !file.commit()) {
    QMessageBox::critical(this, tr("Save failed"), file.errorString());
    return false;
  }
  modified = false;
  updateTitle();
  return true;
}

void MainWindow::saveConfigAs() {
  if (configPath.isEmpty())
    return;
  const QString destination = QFileDialog::getSaveFileName(
      this, tr("Save configuration as"), configPath,
      tr("Configuration files (*.txt);;All files (*)"));
  if (destination.isEmpty())
    return;
  if (!saveConfigAsPath(destination))
    QMessageBox::critical(this, tr("Save As failed"),
                          tr("Could not save configuration to %1:\n%2")
                              .arg(destination, saveAsError));
}

bool MainWindow::saveConfigAsPath(const QString &path) {
  saveAsError.clear();
  if (path.trimmed().isEmpty())
    return false;
  const QString destination = QFileInfo(path).absoluteFilePath();
  ConfigFile savedDocument = document;
  if (!rebaseRelativeIncludes(savedDocument, configPath, destination,
                              saveAsError))
    return false;
  QSaveFile file(destination);
  const QByteArray contents = savedDocument.serialize();
  if (!file.open(QIODevice::WriteOnly) ||
      file.write(contents) != contents.size() || !file.commit()) {
    saveAsError = file.errorString();
    return false;
  }

  document = std::move(savedDocument);
  configPath = destination;
  modified = false;
  ++configRevision;
  pathEdit->setText(configPath);
  rebuildRows();
  rememberConfig(configPath);
  updateTitle();
  validationLabel->setText(tr("Checking configuration…"));
  scheduleLiveValidation();
  return true;
}

void MainWindow::checkConfig() {
  if (!saveConfig())
    return;
  runCli({"config", "check", configPath}, [this](int result, QByteArray output,
                                                 QByteArray error) {
    QMessageBox::information(this,
                             result == 0 ? tr("Configuration valid")
                                         : tr("Configuration error"),
                             QString::fromUtf8(result == 0 ? output : error));
  });
}

void MainWindow::reloadConfig() {
  if (!saveConfig())
    return;
  runCli({"config", "reload"},
         [this](int result, QByteArray output, QByteArray error) {
           QMessageBox::information(
               this, result == 0 ? tr("Reload requested") : tr("Reload failed"),
               QString::fromUtf8(result == 0 ? output : error));
           refreshStatus();
         });
}

void MainWindow::refreshStatus() {
  if (statusRequestPending)
    return;
  statusRequestPending = true;
  runCli({"status"}, [this](int result, QByteArray output, QByteArray error) {
    statusRequestPending = false;
    const QString status = runtimeStatusText(result, output, error);
    statusLabel->setText(status);
    statusLabel->setAccessibleDescription(status);
  });
}

void MainWindow::refreshDevices() {
  if (deviceRequestPending)
    return;
  deviceRequestPending = true;
  runCli({"device", "list"}, [this](int result, QByteArray output,
                                    QByteArray error) {
    deviceRequestPending = false;
    if (result != 0) {
      QString reason = QString::fromUtf8(error).trimmed();
      if (reason.isEmpty())
        reason = QString::fromUtf8(output).trimmed();
      if (reason.isEmpty())
        reason = tr("skyapo device list exited with status %1").arg(result);
      const QString message = tr("Device query failed: %1").arg(reason);
      statusLabel->setText(message);
      statusLabel->setAccessibleDescription(message);
      return;
    }
    const QSignalBlocker blocker(deviceCombo);
    QString previousNode = deviceCombo->currentData().toString();
    if (previousNode.isEmpty() && deviceCombo->currentIndex() >= 0)
      previousNode =
          deviceCombo->itemData(deviceCombo->currentIndex(), Qt::UserRole + 1)
              .toString();
    deviceCombo->clear();
    bool selectedByDaemon = false;
    const auto lines =
        QString::fromUtf8(output).split('\n', Qt::SkipEmptyParts);
    for (qsizetype i = 1; i < lines.size(); ++i) {
      const auto columns = lines[i].split('\t');
      if (columns.size() < 4)
        continue;
      const QString nodeName = columns[1];
      const QString description = columns[2];
      QString label = description + "  —  " + nodeName;
      if (columns.size() >= 11 && columns[10] == "yes")
        label += tr("  (default input)");
      if (columns.size() >= 6) {
        const QString channelCount = columns[4];
        const QString sampleRate = columns[5];
        if (channelCount != "unknown" || sampleRate != "unknown")
          label += tr("  (%1 ch, %2 Hz)").arg(channelCount, sampleRate);
      }
      deviceCombo->addItem(label, nodeName);
      if (columns[3] == "yes") {
        deviceCombo->setCurrentIndex(deviceCombo->count() - 1);
        selectedByDaemon = true;
      }
    }
    if (!selectedByDaemon && !previousNode.isEmpty()) {
      int previousIndex = -1;
      for (int index = 0; index < deviceCombo->count(); ++index)
        if (deviceCombo->itemData(index).toString() == previousNode) {
          previousIndex = index;
          break;
        }
      if (previousIndex >= 0) {
        deviceCombo->setCurrentIndex(previousIndex);
      } else {
        deviceCombo->insertItem(
            0, tr("Selected input unavailable — %1").arg(previousNode),
            QString{});
        deviceCombo->setItemData(0, previousNode, Qt::UserRole + 1);
        deviceCombo->setCurrentIndex(0);
      }
    }
    if (deviceCombo->count() == 0)
      deviceCombo->addItem(tr("No PipeWire input devices"));
    bool hasSelectableDevice = false;
    for (int index = 0; index < deviceCombo->count(); ++index)
      hasSelectableDevice = hasSelectableDevice ||
                            !deviceCombo->itemData(index).toString().isEmpty();
    deviceCombo->setEnabled(!deviceSetPending && hasSelectableDevice);
  });
}

void MainWindow::selectDevice(int index) {
  if (deviceSetPending || index < 0)
    return;
  const auto nodeName = deviceCombo->itemData(index).toString();
  if (nodeName.isEmpty())
    return;
  deviceSetPending = true;
  deviceCombo->setEnabled(false);
  runCli({"device", "set", nodeName},
         [this](int result, QByteArray, QByteArray error) {
           deviceSetPending = false;
           if (result != 0)
             QMessageBox::warning(this, tr("Device selection failed"),
                                  QString::fromUtf8(error));
           refreshDevices();
           refreshStatus();
         });
}
