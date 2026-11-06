// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QAbstractListModel>
#include <QChar>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QLatin1String>
#include <QProcess>
#include <QSet>
#include <QStringList>
#include <QTextStream>
#include <QVector>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <dsd-neo/core/csv_validate.h>
#include <dsd-neo/runtime/scan_options.h>
#include <new>
#include <stdint.h>
#include <utility>
#include "known_frequency_index.h"
#include "saved_systems_model.h"
#include "scan_entry_name.h"
#include "scan_lists_model.h"

namespace dsd_qt {
static double knownWidth(const QString& mode, const QString& options, double inherited = 12500);

static qint64
parseFrequency(QString value, bool mhz) {
    value = value.trimmed();
    if (value.endsWith(QLatin1Char('M'), Qt::CaseInsensitive)) {
        value.chop(1);
        mhz = true;
    }
    bool ok = false;
    double f = value.toDouble(&ok);
    if (mhz) {
        f *= 1e6;
    }
    return ok && std::isfinite(f) && f > 0 && f <= UINT32_MAX ? (qint64)std::llround(f) : 0;
}

KnownFrequencyIndex::KnownFrequencyIndex(SavedSystemsModel* systems, ScanListsModel* lists, QObject* parent)
    : QObject(parent), m_systems(systems), m_lists(lists) {
    connect(&m_files, &QFileSystemWatcher::fileChanged, this, &KnownFrequencyIndex::rebuild);
    connect(&m_files, &QFileSystemWatcher::directoryChanged, this, &KnownFrequencyIndex::rebuild);
    for (QAbstractItemModel* model :
         {static_cast<QAbstractItemModel*>(systems), static_cast<QAbstractItemModel*>(lists)}) {
        if (!model) {
            continue;
        }
        connect(model, &QAbstractItemModel::modelReset, this, &KnownFrequencyIndex::rebuild);
        connect(model, &QAbstractItemModel::dataChanged, this, &KnownFrequencyIndex::rebuild);
        connect(model, &QAbstractItemModel::rowsInserted, this, &KnownFrequencyIndex::rebuild);
        connect(model, &QAbstractItemModel::rowsRemoved, this, &KnownFrequencyIndex::rebuild);
    }
    rebuild();
}

namespace {
/* What a match is (issue #644): the fields KnownFrequencyIndex's class comment lists beside its name. */
QVariantMap
matchIdentity(const QString& source, const QString& listUid, const QString& targetId, bool selectable) {
    return {{QStringLiteral("source"), source},
            {QStringLiteral("listUid"), listUid},
            {QStringLiteral("targetId"), targetId},
            {QStringLiteral("selectable"), selectable}};
}

QVariantMap
matchValue(const QString& name, qint64 hz, const QVariant& systemUid, const QVariantMap& identity) {
    QVariantMap value = identity;
    value.insert(QStringLiteral("name"), name);
    value.insert(QStringLiteral("frequencyHz"), hz);
    value.insert(QStringLiteral("systemUid"), systemUid);
    return value;
}

/* @p identity with the label of @p protocol (a scan-list protocol id), which tells two matches of one name apart;
   none for no protocol (a saved system that auto-detects). */
QVariantMap
withProtocol(QVariantMap identity, const QString& protocol) {
    identity.insert(QStringLiteral("protocolLabel"),
                    protocol.isEmpty() ? QString() : scan_entry_protocol_label(protocol));
    return identity;
}

/* A channel map's row under @p identity's list, if any: it names the channel and starts nothing. */
QVariantMap
channelMapIdentity(const QVariantMap& identity) {
    return matchIdentity(QStringLiteral("channelMap"), identity.value(QStringLiteral("listUid")).toString(), QString(),
                         false);
}
} // namespace

void
KnownFrequencyIndex::add(const QVariantMap& s, const QString& label, const QVariantMap& identity) {
    /* A saved system's mode is its decode flag, as the session builder and the rest of the app read it
       (decode_flag_names_am() in session_args.cpp); a decoder chosen only through its extra arguments is not read. A
       scan-list entry names its protocol. */
    double width =
        s.value(QStringLiteral("channelBandwidthHz"),
                knownWidth(s.value(QStringLiteral("decodeFlag"), s.value(QStringLiteral("protocol"))).toString(),
                           s.value(QStringLiteral("extraArgs")).toString()))
            .toDouble();
    if (!std::isfinite(width) || width <= 0 || width > 200000) {
        width = 12500;
    }
    double step = s.value(QStringLiteral("stepHz"), width).toDouble();
    if (std::isfinite(step) && step > 0) {
        width = std::min(step, width);
    }
    qint64 hz = parseFrequency(s.value(QStringLiteral("freqMhz")).toString(), true);
    if (hz) {
        m_entries.append({hz, width, matchValue(label, hz, s.value(QStringLiteral("uid")), identity)});
    }
    /* The system's channel map lists its channels: a row that names no mode or bandwidth of its own has its width. */
    addCsv(s.value(QStringLiteral("chanCsvPath")).toString(), s, label, width, channelMapIdentity(identity));
}

namespace {
struct CsvColumns {
    int frequency = 1, name = -1, mode = -1, options = -1, channels = -1, label = -1, id = -1;
};

/* A row's own name: a target CSV's `label`, which names the target as its calls are named (issue #782), else its name
   or id column, shown without the blanks around it (an id keeps those inside its quotes). */
static QString
rowName(const QStringList& fields, const CsvColumns& columns) {
    const QString labelled = fields.value(columns.label);
    return (labelled.isEmpty() ? fields.value(columns.name) : labelled).trimmed();
}

/* Cell @p cell as the engine's scan_unquote() reads it: trimmed of blanks (C isspace()), then one pair of surrounding
   quotes taken off, so the blanks inside them stay. */
QString
engineUnquote(QString cell) {
    const auto blank = [](QChar ch) {
        return ch == QLatin1Char(' ') || (ch >= QLatin1Char('\t') && ch <= QLatin1Char('\r'));
    };
    while (!cell.isEmpty() && blank(cell.front())) {
        cell.remove(0, 1);
    }
    while (!cell.isEmpty() && blank(cell.back())) {
        cell.chop(1);
    }
    if (cell.size() >= 2 && cell.front() == QLatin1Char('"') && cell.back() == QLatin1Char('"')) {
        cell = cell.mid(1, cell.size() - 2);
    }
    return cell;
}

/* Cell @p column of @p line as the engine reads a target's id (trunk_scan.c: the line split at commas outside quotes,
   then engineUnquote()): the id the session names the target by. Empty for no such column or cell. */
QString
engineTargetId(const QString& line, int column) {
    int at = 0;
    qsizetype start = 0;
    bool quoted = false;
    for (qsizetype i = 0; column >= 0 && i <= line.size(); ++i) {
        const bool end = i == line.size();
        if (!end && line[i] == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (end || (line[i] == QLatin1Char(',') && !quoted)) {
            if (at++ == column) {
                return engineUnquote(line.mid(start, i - start));
            }
            start = i + 1;
        }
    }
    return {};
}

/* Which of the engine's loaders a CSV is read as (issue #644): a trunk-scan target list (dsd_trunk_scan_load_targets_csv(),
   trunk_scan.c) or a channel map (chan_import_stats(), dsd_import.c). */
enum class EngineCsvGrammar { Targets, ChannelMap };

/* The target loader's row buffer (line[4096]: a longer row refuses the file), and the channel map reader's (1 MiB, the
   newline and terminator included). Neither loader caps its rows; the index keeps at most kStoredRowMax of one file's,
   reading the rest only for what refuses the file. */
constexpr qsizetype kEngineTargetRowMax = 4095;
constexpr qsizetype kEngineChannelRowMax = 1048574;
constexpr qsizetype kStoredRowMax = 100000;

struct EngineCsvRow {
    QString line;
    QStringList fields;
    qint64 hz = 0;
};

struct EngineCsv {
    CsvColumns columns;
    QVector<EngineCsvRow> rows;
};

/* Every cell of @p line as the engine's target loader splits it (dsd_trunk_scan_split_csv_fields(): at commas outside
   quotes), each as it stands. */
QStringList
engineRawCells(const QString& line) {
    QStringList cells;
    qsizetype start = 0;
    bool quoted = false;
    for (qsizetype i = 0; i <= line.size(); ++i) {
        const bool end = i == line.size();
        if (!end && line[i] == QLatin1Char('"')) {
            quoted = !quoted;
        } else if (end || (line[i] == QLatin1Char(',') && !quoted)) {
            cells.append(line.mid(start, i - start));
            start = i + 1;
        }
    }
    return cells;
}

/* Every cell of @p line as the engine's target loader splits and unquotes it (engineTargetId()). */
QStringList
engineCells(const QString& line) {
    QStringList cells = engineRawCells(line);
    std::transform(cells.begin(), cells.end(), cells.begin(), engineUnquote);
    return cells;
}

/* C isspace() over @p text's ends, as scan_trim() and trim_ws() trim. */
QString
engineTrim(QString text) {
    const auto blank = [](QChar ch) {
        return ch == QLatin1Char(' ') || (ch >= QLatin1Char('\t') && ch <= QLatin1Char('\r'));
    };
    while (!text.isEmpty() && blank(text.front())) {
        text.remove(0, 1);
    }
    while (!text.isEmpty() && blank(text.back())) {
        text.chop(1);
    }
    return text;
}

/* @p cell as a decimal frequency in [@p min_hz, @p max_hz], digits only once trimmed (scan_parse_u32_decimal(),
   parse_dec_long_strict()); 0 for anything else. */
qint64
engineHz(const QString& cell, qint64 min_hz, qint64 max_hz) {
    const QString digits = engineTrim(cell);
    if (digits.isEmpty()) {
        return 0;
    }
    if (std::any_of(digits.cbegin(), digits.cend(),
                    [](QChar ch) { return ch < QLatin1Char('0') || ch > QLatin1Char('9'); })) {
        return 0;
    }
    bool ok = false;
    const qint64 hz = digits.toLongLong(&ok, 10);
    return ok && hz >= min_hz && hz <= max_hz ? hz : 0;
}

/* Whether @p cell, trimmed, is wrapped in double quotes (scan_field_wrapped(), chan_import_options()). */
bool
engineWrapped(const QString& cell) {
    const QString trimmed = engineTrim(cell);
    return trimmed.size() >= 2 && trimmed.front() == QLatin1Char('"') && trimmed.back() == QLatin1Char('"');
}

/* An options cell as its loader hands it to dsd_scan_options_parse_cell() (issue #637): @p legacy as the loader has
   always read it (a target's without one pair of outer quotes, a channel map's as split); when the cell was wrapped
   in double quotes, @p content its inside. A cell that reads as a spreadsheet's quoting reads as its content, each
   doubled quote as one; any other as @p legacy. */
QString
engineOptionsText(const QString& legacy, const QString& content, bool wrapped) {
    if (!wrapped) {
        return legacy;
    }
    const QByteArray legacyBytes = legacy.toUtf8();
    const QByteArray contentBytes = content.toUtf8();
    if (dsd_scan_options_cell_csv_quoted(legacyBytes.constData(), contentBytes.constData(),
                                         static_cast<size_t>(contentBytes.size()))
        == 0) {
        return legacy;
    }
    QString text = content;
    return text.replace(QStringLiteral("\"\""), QStringLiteral("\""));
}

/* A target's label cell, unquoted (@p cell), as scan_csv_unescape_doubled_quotes() reads one that was @p wrapped:
   each doubled quote as one, when every quote in it is doubled; otherwise as it is. */
QString
engineLabelText(const QString& cell, bool wrapped) {
    if (!wrapped) {
        return cell;
    }
    for (qsizetype i = 0; i < cell.size(); ++i) {
        if (cell[i] == QLatin1Char('"')) {
            if (i + 1 >= cell.size() || cell[i + 1] != QLatin1Char('"')) {
                return cell;
            }
            ++i;
        }
    }
    QString text = cell;
    return text.replace(QStringLiteral("\"\""), QStringLiteral("\""));
}

/* The target loader's header, row 1 (scan_read_target_csv_header()): its first seven cells, unquoted, are exactly
   id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes, and later cells are named columns, compared without
   ASCII case. False when the engine refuses it. */
bool
engineTargetHeader(const QString& line, CsvColumns* columns) {
    static const char* const required[] = {"id",   "type", "frequency_hz", "chan_csv", "dwell_ms", "activity_hold_ms",
                                           "notes"};
    const QStringList names = engineCells(line);
    if (names.size() < 7) {
        return false;
    }
    for (int i = 0; i < 7; ++i) {
        if (names[i] != QLatin1String(required[i])) {
            return false;
        }
    }
    *columns = CsvColumns{};
    columns->id = columns->name = 0;
    columns->mode = 1;
    columns->frequency = 2;
    columns->channels = 3;
    for (int i = 7; i < names.size(); ++i) {
        const QString name = names[i].toLower();
        if (name == QLatin1String("options")) {
            columns->options = i;
        } else if (name == QLatin1String("label")) {
            columns->label = i;
        }
    }
    return true;
}

/* The channel map reader's header, row 1 (chan_parse_header()): column 0 is the channel key and column 1 the frequency
   whatever they are called; from the third on, name, mode and options (relevant_CLI_switches) are found by name,
   without ASCII case. */
CsvColumns
engineChannelHeader(const QString& line) {
    CsvColumns columns;
    columns.frequency = 1;
    const QStringList cells = line.split(QLatin1Char(','));
    for (int i = 2; i < cells.size(); ++i) {
        const QString name = engineTrim(cells[i]).toLower();
        if (name == QLatin1String("name") && columns.name < 0) {
            columns.name = i;
        } else if (name == QLatin1String("mode")) {
            columns.mode = i;
        } else if (name == QLatin1String("options") || name == QLatin1String("relevant_cli_switches")) {
            columns.options = i;
        }
    }
    return columns;
}

/* A target row's cells as the target loader reads them: split outside quotes and unquoted (scan_unquote()), the options
   cell as dsd_scan_options_parse_cell() reads it and the label as scan_parse_target_label() does. */
QStringList
engineTargetFields(const QString& line, const CsvColumns& columns) {
    const QStringList raw = engineRawCells(line);
    QStringList fields = raw;
    std::transform(fields.begin(), fields.end(), fields.begin(), engineUnquote);
    if (columns.options >= 0 && columns.options < fields.size()) {
        const QString& text = fields[columns.options];
        fields[columns.options] = engineOptionsText(text, text, engineWrapped(raw[columns.options]));
    }
    if (columns.label >= 0 && columns.label < fields.size()) {
        fields[columns.label] = engineLabelText(fields[columns.label], engineWrapped(raw[columns.label]));
    }
    return fields;
}

/* A channel map row's cells as chan_import_row() reads them: split at every comma, trimmed, the options cell as
   chan_import_options() hands it to dsd_scan_options_parse_cell(). */
QStringList
engineChannelFields(const QString& line, const CsvColumns& columns) {
    const QStringList raw = line.split(QLatin1Char(','));
    QStringList fields = raw;
    std::transform(fields.begin(), fields.end(), fields.begin(), engineTrim);
    if (columns.options >= 0 && columns.options < fields.size()) {
        const QString trimmed = fields[columns.options];
        const bool wrapped = engineWrapped(trimmed);
        fields[columns.options] =
            engineOptionsText(raw[columns.options], wrapped ? trimmed.mid(1, trimmed.size() - 2) : QString(), wrapped);
    }
    return fields;
}

/* One data row of @p grammar into @p out, or false when the engine refuses the whole file for it. A row the engine
   loads no frequency for is left out, and so is every row once kStoredRowMax are kept: such a row is only checked for
   what refuses the file. */
bool
engineCsvRow(EngineCsvGrammar grammar, const QString& raw, EngineCsv* out) {
    const bool store = out->rows.size() < kStoredRowMax;
    if (grammar == EngineCsvGrammar::Targets) {
        if (raw.size() > kEngineTargetRowMax) {
            return false;
        }
        const QString line = engineTrim(raw);
        if (!store || line.isEmpty()) {
            return true;
        }
        const qint64 hz = engineHz(engineTargetId(line, out->columns.frequency), 1, UINT32_MAX);
        if (hz) {
            out->rows.append({line, engineTargetFields(line, out->columns), hz});
        }
        return true;
    }
    if (raw.toUtf8().size() > kEngineChannelRowMax) {
        return false;
    }
    if (!store || engineTrim(raw).isEmpty()) {
        return true;
    }
    const QStringList fields = engineChannelFields(raw, out->columns);
    const qint64 hz = engineHz(fields.value(1), 100000, 2147483647LL);
    if (hz && dsd_csv_channel_has_slot(fields.value(0).toUtf8().constData())) {
        out->rows.append({QString(), fields, hz});
    }
    return true;
}

/* readEngineCsv()'s reading of @p text into @p out; false when the engine refuses the file. */
bool
readEngineCsvRows(QTextStream& text, EngineCsvGrammar grammar, EngineCsv* out) {
    if (text.atEnd()) {
        return false;
    }
    const QString header = text.readLine(kEngineChannelRowMax + 1);
    if (grammar == EngineCsvGrammar::Targets) {
        if (header.size() > kEngineTargetRowMax || !engineTargetHeader(header, &out->columns)) {
            return false;
        }
    } else {
        out->columns = engineChannelHeader(header);
    }
    while (!text.atEnd()) {
        if (!engineCsvRow(grammar, text.readLine(kEngineChannelRowMax + 1), out)) {
            return false;
        }
    }
    return true;
}

/* The rows of the CSV at @p path as the engine's @p grammar loader reads them (issue #644): its header is row 1 and
   nothing is a comment (a `#` row is data), a blank row is skipped, and the caps and refusals are the engine's: a file
   it refuses (a bad target header, a row past its buffer) gives no rows. At most kStoredRowMax rows are kept, and a
   file too large to hold gives none. False when the file cannot be read or is refused. */
bool
readEngineCsv(const QString& path, EngineCsvGrammar grammar, EngineCsv* out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream text(&f);
    bool read = false;
    try {
        read = readEngineCsvRows(text, grammar, out);
    } catch (const std::bad_alloc&) {
        read = false;
    }
    if (!read) {
        out->rows.clear();
    }
    return read;
}

/* A row's identity: a target CSV's row is the target its `id` names (@p line's id cell as the engine reads it),
   startable when it has one and the file's list can start; a channel map's row is what the file's identity says. */
QVariantMap
rowIdentity(const QString& line, const QStringList& fields, const CsvColumns& columns, const QVariantMap& identity) {
    if (identity.value(QStringLiteral("source")).toString() != QLatin1String("scanTarget")) {
        return identity;
    }
    QVariantMap row = identity;
    const QString id = engineTargetId(line, columns.id);
    row.insert(QStringLiteral("targetId"), id);
    row.insert(QStringLiteral("selectable"), identity.value(QStringLiteral("selectable")).toBool() && !id.isEmpty());
    // A target's type is its protocol and its kind: "nxdn48-conventional".
    row.insert(QStringLiteral("protocolLabel"),
               scan_entry_protocol_label(fields.value(columns.mode).section(QLatin1Char('-'), 0, 0).toLower()));
    return row;
}
} // namespace

/* The channel width @p mode names: a decode flag, compared exactly since its case is significant (-fm is dPMR, -fM is
   AM), or a channel-map mode, scan target type or protocol name, compared without case. NXDN48, dPMR and D-STAR run the
   receiver's 6.25 kHz channel; any other mode a 12.5 kHz one. */
static bool
amMode(const QString& mode) {
    static const QSet<QString> amNames{QStringLiteral("am"), QStringLiteral("am-conventional")};
    return mode == QStringLiteral("-fM") || amNames.contains(mode.toLower());
}

static double
modeWidth(const QString& mode) {
    static const QSet<QString> narrowFlags{QStringLiteral("-fi"), QStringLiteral("-fm"), QStringLiteral("-fd")};
    static const QSet<QString> narrowNames{QStringLiteral("dpmr"), QStringLiteral("dstar"), QStringLiteral("d-star")};
    const QString name = mode.toLower();
    if (amMode(mode)) {
        return 6000;
    }
    if (narrowFlags.contains(mode) || narrowNames.contains(name) || name.startsWith(QLatin1String("nxdn48"))) {
        return 6250;
    }
    return 12500;
}

/* The width option of @p mode's analog kind: an AM channel's --am-bandwidth-hz, an NFM channel's --nfm-bandwidth-hz;
   empty for a digital mode, whose channel neither sets. */
static QString
widthOption(const QString& mode) {
    static const QSet<QString> nfmNames{QStringLiteral("nfm"), QStringLiteral("nfm-conventional")};
    if (mode == QStringLiteral("-fA") || nfmNames.contains(mode.toLower())) {
        return QStringLiteral("--nfm-bandwidth-hz");
    }
    return amMode(mode) ? QStringLiteral("--am-bandwidth-hz") : QString();
}

/* The width @p option sets in @p options, as "--option value" or "--option=value" (the last one counts), else
   @p width. */
static double
optionWidth(const QString& options, const QString& option, double width) {
    const auto args = QProcess::splitCommand(options);
    const QString prefix = option + QLatin1Char('=');
    for (qsizetype i = 0; i < args.size(); ++i) {
        QString value;
        if (args[i] == option && i + 1 < args.size()) {
            value = args[i + 1];
        } else if (args[i].startsWith(prefix)) {
            value = args[i].mid(prefix.size());
        } else {
            continue;
        }
        bool ok = false;
        const double hz = value.toDouble(&ok);
        if (ok && std::isfinite(hz) && hz >= 5000 && hz <= 25000) {
            width = hz;
        }
    }
    return width;
}

/* A channel's width from its mode and options; @p inherited when it names no mode. Only the width option of the
   channel's own analog kind applies: an NFM system that also carries an AM width keeps its NFM channels' width. */
static double
knownWidth(const QString& mode, const QString& options, double inherited) {
    if (mode.isEmpty()) {
        return inherited;
    }
    const QString option = widthOption(mode);
    return option.isEmpty() ? modeWidth(mode) : optionWidth(options, option, modeWidth(mode));
}

void
KnownFrequencyIndex::addCsv(const QString& path, const QVariantMap& s, const QString& label, double width,
                            const QVariantMap& identity, int depth) {
    if (path.isEmpty()) {
        return;
    }
    const bool targets = identity.value(QStringLiteral("source")).toString() == QLatin1String("scanTarget");
    EngineCsv csv;
    if (!readEngineCsv(path, targets ? EngineCsvGrammar::Targets : EngineCsvGrammar::ChannelMap, &csv)) {
        return;
    }
    const QString directory = QFileInfo(path).absolutePath();
    if (!m_files.directories().contains(directory)) {
        m_files.addPath(directory);
    }
    if (!m_files.files().contains(path)) {
        m_files.addPath(path);
    }
    const CsvColumns& columns = csv.columns;
    for (const EngineCsvRow& row : csv.rows) {
        if (m_entries.size() >= 200000) {
            break;
        }
        const QStringList& fields = row.fields;
        QString name = label;
        const QString channel = rowName(fields, columns);
        if (!channel.isEmpty()) {
            name += QStringLiteral(" / ") + channel;
        }
        const double rowWidth = knownWidth(fields.value(columns.mode), fields.value(columns.options), width);
        m_entries.append({row.hz, rowWidth,
                          matchValue(name, row.hz, s.value(QStringLiteral("uid")),
                                     rowIdentity(row.line, fields, columns, identity))});
        if (depth == 0 && targets && !fields.value(columns.channels).isEmpty()) {
            addCsv(QDir(directory).filePath(fields[columns.channels]), s, name, rowWidth, channelMapIdentity(identity),
                   depth + 1);
        }
    }
}

void
KnownFrequencyIndex::addListEntry(const QVariantMap& entry, const QString& listName, const QString& listUid,
                                  bool startable) {
    // A disabled entry is no target of the list's session.
    const QString uid = entry.value(QStringLiteral("uid")).toString();
    const bool selectable = startable && entry.value(QStringLiteral("enabled"), true).toBool() && !uid.isEmpty();
    QVariantMap s = entry;
    QString protocol = entry.value(QStringLiteral("protocol")).toString();
    if (entry.value(QStringLiteral("kind")).toString() == QStringLiteral("system")) {
        /* An entry for a saved system is a target of the list too, the entry's uid its id: the system's frequency,
           width and protocol (its decode flag). The system's channel map is indexed once, under the system. */
        s = m_systems ? m_systems->getByUid(entry.value(QStringLiteral("systemUid")).toString()) : QVariantMap();
        if (s.isEmpty()) {
            return;
        }
        s.remove(QStringLiteral("chanCsvPath"));
        s.insert(QStringLiteral("uid"), uid);
        protocol = scan_entry_protocol_for_flag(s.value(QStringLiteral("decodeFlag")).toString());
    }
    add(s,
        listName + QStringLiteral(" / ")
            + scan_entry_name(s.value(QStringLiteral("name")).toString(), protocol,
                              s.value(QStringLiteral("freqMhz")).toString()),
        withProtocol(matchIdentity(QStringLiteral("scanEntry"), listUid, uid, selectable), protocol));
}

namespace {
/* The target rows of the session CSV at @p path, list @p number (@p listUid, named @p listName), appended to @p out: as
   the engine loaded them (readEngineCsv()), so every target the session runs, and none it refused. */
void
appendSessionTargets(const QString& path, int number, const QString& listUid, const QString& listName,
                     QVariantList* out) {
    EngineCsv csv;
    if (path.isEmpty() || !readEngineCsv(path, EngineCsvGrammar::Targets, &csv)) {
        return;
    }
    const CsvColumns& columns = csv.columns;
    for (const EngineCsvRow& row : csv.rows) {
        const QString id = engineTargetId(row.line, columns.id);
        if (id.isEmpty()) {
            continue;
        }
        const QString mode = row.fields.value(columns.mode);
        out->append(QVariantMap{
            {QStringLiteral("listNumber"), number},
            {QStringLiteral("listUid"), listUid},
            {QStringLiteral("targetId"), id},
            {QStringLiteral("frequencyHz"), row.hz},
            {QStringLiteral("width"), knownWidth(mode, row.fields.value(columns.options), 12500)},
            {QStringLiteral("name"), listName + QStringLiteral(" / ") + rowName(row.fields, columns)},
            {QStringLiteral("protocolLabel"),
             scan_entry_protocol_label(mode.section(QLatin1Char('-'), 0, 0).toLower())},
        });
    }
}
} // namespace

QVariantList
KnownFrequencyIndex::sessionTargets(const QVariantList& lists) const {
    QVariantList out;
    for (qsizetype i = 0; i < lists.size(); ++i) {
        const QVariantMap list = lists.at(i).toMap();
        appendSessionTargets(list.value(QStringLiteral("path")).toString(), static_cast<int>(i) + 1,
                             list.value(QStringLiteral("uid")).toString(),
                             list.value(QStringLiteral("name")).toString(), &out);
    }
    return out;
}

void
KnownFrequencyIndex::rebuild() {
    m_entries.clear();
    if (m_systems) {
        for (int i = 0; i < m_systems->count(); ++i) {
            auto s = m_systems->get(i);
            // Issue #782: an unnamed system by its protocol and frequency, as the scan list and its targets name it.
            add(s,
                scan_entry_name(s.value(QStringLiteral("name")).toString(),
                                scan_entry_protocol_for_flag(s.value(QStringLiteral("decodeFlag")).toString()),
                                s.value(QStringLiteral("freqMhz")).toString()),
                withProtocol(matchIdentity(QStringLiteral("system"), QString(), QString(), true),
                             scan_entry_protocol_for_flag(s.value(QStringLiteral("decodeFlag")).toString())));
        }
    }
    if (m_lists) {
        for (int i = 0; i < m_lists->count(); ++i) {
            auto list = m_lists->get(i);
            QString name = list.value(QStringLiteral("name")).toString();
            // A draft list cannot start, so none of its rows can be started with its settings.
            const bool startable = !list.value(QStringLiteral("isDraft")).toBool();
            const QString listUid = list.value(QStringLiteral("uid")).toString();
            for (const QVariant& value : list.value(QStringLiteral("entries")).toList()) {
                addListEntry(value.toMap(), name, listUid, startable);
            }
            addCsv(list.value(QStringLiteral("targetsCsvPath")).toString(), list, name, 12500,
                   matchIdentity(QStringLiteral("scanTarget"), listUid, QString(), startable));
        }
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.hz < b.hz; });
    ++m_revision;
    Q_EMIT changed();
}

QVariantList
KnownFrequencyIndex::lookup(double frequency, double width) const {
    QVariantList result;
    if (!std::isfinite(frequency) || !std::isfinite(width) || width <= 0) {
        return result;
    }
    double nearest = 1e100;
    QSet<QString> seen;
    double radius = std::min(width, 200000.0) / 2;
    auto begin = std::lower_bound(m_entries.cbegin(), m_entries.cend(), frequency - radius,
                                  [](const Entry& e, double hz) { return e.hz < hz; });
    for (auto it = begin; it != m_entries.cend() && it->hz <= frequency + radius; ++it) {
        const auto& entry = *it;
        double distance = std::abs(frequency - entry.hz);
        if (distance > std::min(width, entry.width) / 2) {
            continue;
        }
        if (distance + 0.5 < nearest) {
            result.clear();
            seen.clear();
            nearest = distance;
        }
        if (std::abs(distance - nearest) < 0.5) {
            // Two rows of one list on one frequency under one name are two targets, each its own id.
            QString key = entry.value.value(QStringLiteral("systemUid")).toString() + QLatin1Char('/')
                          + entry.value.value(QStringLiteral("name")).toString() + QString::number(entry.hz)
                          + QLatin1Char('/') + entry.value.value(QStringLiteral("source")).toString() + QLatin1Char('/')
                          + entry.value.value(QStringLiteral("targetId")).toString();
            if (!seen.contains(key)) {
                result.append(entry.value);
                seen.insert(key);
            }
        }
    }
    return result;
}

QVariantList
KnownFrequencyIndex::tunedMatches(double frequency, double channelWidth) const {
    return lookup(frequency, std::isfinite(channelWidth) && channelWidth > 0 ? channelWidth : 12500);
}

QStringList
KnownFrequencyIndex::matchNames(const QVariantList& matches) {
    QStringList names;
    for (const QVariant& match : matches) {
        const QString name = match.toMap().value(QStringLiteral("name")).toString();
        if (!names.contains(name)) {
            names.append(name);
        }
    }
    return names;
}

QStringList
KnownFrequencyIndex::tunedNames(double frequency, double channelWidth) const {
    return matchNames(tunedMatches(frequency, channelWidth));
}
} // namespace dsd_qt
