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
#include <Qt>
#include <algorithm>
#include <cmath>
#include <stdint.h>
#include <utility>
#include "known_frequency_index.h"
#include "saved_systems_model.h"
#include "scan_entry_name.h"
#include "scan_lists_model.h"

namespace dsd_qt {
static double knownWidth(const QString& mode, const QString& options, double inherited = 12500);

static QStringList
csvFields(const QString& line) {
    QStringList result;
    QString field;
    bool quoted = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        QChar ch = line[i];
        if (ch == QLatin1Char('"')) {
            if (quoted && i + 1 < line.size() && line[i + 1] == ch) {
                field += ch;
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (ch == QLatin1Char(',') && !quoted) {
            result.append(field.trimmed());
            field.clear();
        } else {
            field += ch;
        }
    }
    if (quoted) {
        return {};
    }
    result.append(field.trimmed());
    return result;
}

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

static CsvColumns
csvColumns(const QStringList& fields) {
    CsvColumns columns;
    for (int i = 0; i < fields.size(); ++i) {
        const QString name = fields[i].toLower();
        if (name == QStringLiteral("frequency") || name == QStringLiteral("freq") || name == QStringLiteral("freq_hz")
            || name == QStringLiteral("frequency_hz")) {
            columns.frequency = i;
        } else if (name == QStringLiteral("name") || name == QStringLiteral("id")) {
            columns.name = i;
            columns.id = name == QStringLiteral("id") ? i : columns.id;
        } else if (name == QStringLiteral("mode") || name == QStringLiteral("type")) {
            columns.mode = i;
        } else if (name == QStringLiteral("options") || name == QStringLiteral("relevant_cli_switches")) {
            columns.options = i;
        } else if (name == QStringLiteral("chan_csv")) {
            columns.channels = i;
        } else if (name == QStringLiteral("label")) {
            columns.label = i;
        }
    }
    return columns;
}

/* A row's own name: a target CSV's `label`, which names the target as its calls are named (issue #782), else its name
   or id column. */
static QString
rowName(const QStringList& fields, const CsvColumns& columns) {
    const QString labelled = fields.value(columns.label);
    return labelled.isEmpty() ? fields.value(columns.name) : labelled;
}

/* A row's identity: a target CSV's row is the target its `id` names, startable when it has one and the file's list
   can start; a channel map's row is what the file's identity says. */
QVariantMap
rowIdentity(const QStringList& fields, const CsvColumns& columns, const QVariantMap& identity) {
    if (identity.value(QStringLiteral("source")).toString() != QLatin1String("scanTarget")) {
        return identity;
    }
    QVariantMap row = identity;
    const QString id = fields.value(columns.id);
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
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    const QString directory = QFileInfo(path).absolutePath();
    if (!m_files.directories().contains(directory)) {
        m_files.addPath(directory);
    }
    if (!m_files.files().contains(path)) {
        m_files.addPath(path);
    }
    QTextStream text(&f);
    CsvColumns columns;
    bool first = true;
    int rows = 0;
    while (!text.atEnd() && ++rows <= 100000 && m_entries.size() < 200000) {
        QString line = text.readLine(1048576).trimmed();
        if (line.startsWith(QLatin1Char('#')) || line.isEmpty()) {
            continue;
        }
        const auto fields = csvFields(line);
        if (first) {
            columns = csvColumns(fields);
            first = false;
        }
        qint64 hz = parseFrequency(fields.value(columns.frequency), false);
        if (!hz) {
            continue;
        }
        QString name = label;
        const QString channel = rowName(fields, columns);
        if (!channel.isEmpty()) {
            name += QStringLiteral(" / ") + channel;
        }
        const double rowWidth = knownWidth(fields.value(columns.mode), fields.value(columns.options), width);
        m_entries.append(
            {hz, rowWidth,
             matchValue(name, hz, s.value(QStringLiteral("uid")), rowIdentity(fields, columns, identity))});
        if (depth == 0 && !fields.value(columns.channels).isEmpty()) {
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
/* The target rows of the session CSV at @p path, list @p number (@p listUid, named @p listName), appended to @p out. */
void
appendSessionTargets(const QString& path, int number, const QString& listUid, const QString& listName,
                     QVariantList* out) {
    QFile f(path);
    if (path.isEmpty() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;
    }
    QTextStream text(&f);
    CsvColumns columns;
    bool first = true;
    int rows = 0;
    while (!text.atEnd() && ++rows <= 100000) {
        const QString line = text.readLine(1048576).trimmed();
        if (line.startsWith(QLatin1Char('#')) || line.isEmpty()) {
            continue;
        }
        const auto fields = csvFields(line);
        if (first) {
            columns = csvColumns(fields);
            first = false;
            continue;
        }
        const qint64 hz = parseFrequency(fields.value(columns.frequency), false);
        const QString id = fields.value(columns.id);
        if (!hz || id.isEmpty()) {
            continue;
        }
        const QString mode = fields.value(columns.mode);
        out->append(QVariantMap{
            {QStringLiteral("listNumber"), number},
            {QStringLiteral("listUid"), listUid},
            {QStringLiteral("targetId"), id},
            {QStringLiteral("frequencyHz"), hz},
            {QStringLiteral("width"), knownWidth(mode, fields.value(columns.options), 12500)},
            {QStringLiteral("name"), listName + QStringLiteral(" / ") + rowName(fields, columns)},
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
