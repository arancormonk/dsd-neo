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

void
KnownFrequencyIndex::add(const QVariantMap& s, const QString& label) {
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
        m_entries.append({hz,
                          width,
                          {{QStringLiteral("name"), label},
                           {QStringLiteral("frequencyHz"), hz},
                           {QStringLiteral("systemUid"), s.value(QStringLiteral("uid"))}}});
    }
    /* The system's channel map lists its channels: a row that names no mode or bandwidth of its own has its width. */
    addCsv(s.value(QStringLiteral("chanCsvPath")).toString(), s, label, width);
}

namespace {
struct CsvColumns {
    int frequency = 1, name = -1, mode = -1, options = -1, channels = -1;
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
        } else if (name == QStringLiteral("mode") || name == QStringLiteral("type")) {
            columns.mode = i;
        } else if (name == QStringLiteral("options") || name == QStringLiteral("relevant_cli_switches")) {
            columns.options = i;
        } else if (name == QStringLiteral("chan_csv")) {
            columns.channels = i;
        }
    }
    return columns;
}
} // namespace

/* A channel's width from its mode and options; @p inherited when it names no mode and no bandwidth. */
static double
knownWidth(const QString& mode, const QString& options, double inherited) {
    double width = mode.isEmpty() ? inherited : 12500;
    if (mode == QStringLiteral("am") || mode == QStringLiteral("am-conventional") || mode == QStringLiteral("-fM")) {
        width = 6000;
    } else if (mode.startsWith(QLatin1String("nxdn48")) || mode == QStringLiteral("-fi")) {
        width = 6250;
    }
    const auto args = QProcess::splitCommand(options);
    for (qsizetype i = 0; i + 1 < args.size(); ++i) {
        if (args[i] != QStringLiteral("--nfm-bandwidth-hz") && args[i] != QStringLiteral("--am-bandwidth-hz")) {
            continue;
        }
        bool ok = false;
        double hz = args[i + 1].toDouble(&ok);
        if (ok && std::isfinite(hz) && hz >= 5000 && hz <= 25000) {
            width = hz;
        }
    }
    return width;
}

void
KnownFrequencyIndex::addCsv(const QString& path, const QVariantMap& s, const QString& label, double width, int depth) {
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
        const QString channel = fields.value(columns.name);
        if (!channel.isEmpty()) {
            name += QStringLiteral(" / ") + channel;
        }
        const double rowWidth = knownWidth(fields.value(columns.mode), fields.value(columns.options), width);
        m_entries.append({hz,
                          rowWidth,
                          {{QStringLiteral("name"), name},
                           {QStringLiteral("frequencyHz"), hz},
                           {QStringLiteral("systemUid"), s.value(QStringLiteral("uid"))}}});
        if (depth == 0 && !fields.value(columns.channels).isEmpty()) {
            addCsv(QDir(directory).filePath(fields[columns.channels]), s, name, rowWidth, depth + 1);
        }
    }
}

void
KnownFrequencyIndex::rebuild() {
    m_entries.clear();
    if (m_systems) {
        for (int i = 0; i < m_systems->count(); ++i) {
            auto s = m_systems->get(i);
            add(s, s.value(QStringLiteral("name")).toString());
        }
    }
    if (m_lists) {
        for (int i = 0; i < m_lists->count(); ++i) {
            auto list = m_lists->get(i);
            QString name = list.value(QStringLiteral("name")).toString();
            for (const QVariant& value : list.value(QStringLiteral("entries")).toList()) {
                auto s = value.toMap();
                if (s.value(QStringLiteral("kind")).toString() != QStringLiteral("system")) {
                    add(s, name + QStringLiteral(" / ") + s.value(QStringLiteral("name")).toString());
                }
            }
            addCsv(list.value(QStringLiteral("targetsCsvPath")).toString(), list, name, 12500);
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
            QString key = entry.value.value(QStringLiteral("systemUid")).toString() + QLatin1Char('/')
                          + entry.value.value(QStringLiteral("name")).toString() + QString::number(entry.hz);
            if (!seen.contains(key)) {
                result.append(entry.value);
                seen.insert(key);
            }
        }
    }
    return result;
}
} // namespace dsd_qt
