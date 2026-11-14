// SPDX-License-Identifier: GPL-3.0-or-later
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUuid>
#include <QVariantList>
#include <Qt>
#include <QtGlobal>
#include <algorithm>
#include <dsd-neo/runtime/log.h>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <iterator>
#include <utility>
#include "app_prefs.h"
#include "json_store.h"
#include "realtime_clock.h"
#include "scan_lists_model.h"

namespace dsd_qt {
namespace {
QString
uuid() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/* @p input's entries, each with a uid of its own: a uid that is missing, repeats an earlier entry's, or is in @p taken
 * (another list's) is replaced. A target id names one target in a session, so two lists of the app's never share one;
 * only a person's own target CSVs can (issue #697). */
QVariantList
normalizeEntries(const QVariantList& input, const QSet<QString>& taken) {
    QVariantList entries;
    QSet<QString> ids;
    for (const auto& value : input) {
        const auto old = value.toMap();
        QVariantMap entry{{"kind", "freq"}, {"enabled", true},  {"dwellMs", 0},
                          {"holdMs", 0},    {"modulation", ""}, {"gainDb", -1}};
        for (const auto& field : {"uid", "kind", "systemUid", "name", "protocol", "freqMhz", "enabled", "dwellMs",
                                  "holdMs", "modulation", "gainDb", "decryptionMode", "decryptionProfileUid"}) {
            if (old.contains(field)) {
                entry[field] = old.value(field);
            }
        }
        QString id = entry.value("uid").toString();
        if (id.isEmpty() || ids.contains(id) || taken.contains(id)) {
            id = uuid();
        }
        ids.insert(id);
        entry["uid"] = id;
        if (!entry.contains("enabled")) {
            entry["enabled"] = true;
        }
        entries << entry;
    }
    return entries;
}

/* @p input over @p base (the defaults when empty), its entries' uids apart from @p taken: the uids of every other list's
 * entries. */
QVariantMap
normalize(const QVariantMap& input, const QVariantMap& base = {}, const QSet<QString>& taken = {}) {
    QVariantMap result =
        base.isEmpty()
            ? QVariantMap{{"name", ""},         {"sourceType", "usb"}, {"host", ""},          {"port", 1234},
                          {"gainDb", -1},       {"ppm", ""},           {"bandwidthKhz", -1},  {"biasTee", -1},
                          {"voiceOnly", false}, {"skipSilent", false}, {"defaultDwellMs", 0}, {"defaultHoldMs", 0},
                          {"groupCsvPath", ""}, {"srcCsvPath", ""},    {"lastHeard", 0}}
            : base;
    const QStringList fields{
        "airspy",       "name",      "sourceType", "host",       "port",           "gainDb",        "ppm",
        "bandwidthKhz", "biasTee",   "voiceOnly",  "skipSilent", "defaultDwellMs", "defaultHoldMs", "groupCsvPath",
        "srcCsvPath",   "lastHeard", "entries",    "isDraft",    "targetSource",   "targetsCsvPath"};
    for (const auto& field : fields) {
        if (input.contains(field)) {
            result[field] = input.value(field);
        }
    }
    if (!result.contains("uid")) {
        result["uid"] = input.value("uid", uuid());
    }
    if (!result.contains("isDraft")) {
        result["isDraft"] = false;
    }
    if (!result.contains("lastHeard")) {
        result["lastHeard"] = 0;
    }
    if (!result.contains("targetSource")) {
        result["targetSource"] = "entries";
    }
    if (result.value("targetSource") == "csv") {
        result["entries"] = QVariantList();
    } else {
        result["targetsCsvPath"] = "";
    }
    result["entries"] = normalizeEntries(result.value("entries").toList(), taken);
    return result;
}

/* The uids of the entries of every list in @p rows but row @p skip. */
QSet<QString>
entryUids(const QList<QVariantMap>& rows, qsizetype skip = -1) {
    QSet<QString> uids;
    for (qsizetype i = 0; i < rows.size(); ++i) {
        if (i != skip) {
            for (const auto& entry : rows[i].value("entries").toList()) {
                uids.insert(entry.toMap().value("uid").toString());
            }
        }
    }
    return uids;
}

/* Whether the load gave an entry of @p row a uid other than the one @p stored holds for it: none, a repeat, or one an
 * earlier list holds. */
bool
entryUidsMinted(const QJsonObject& stored, const QVariantMap& row) {
    const QJsonArray before = stored.value(QStringLiteral("entries")).toArray();
    const QVariantList after = row.value("entries").toList();
    for (qsizetype i = 0; i < after.size(); ++i) {
        if (i >= before.size()
            || after[i].toMap().value("uid").toString()
                   != before.at(i).toObject().value(QStringLiteral("uid")).toString()) {
            return true;
        }
    }
    return false;
}
} // namespace

ScanListsModel::ScanListsModel(QObject* parent) : QAbstractListModel(parent) {
    QSet<QString> ids;
    QSet<QString> entryIds;
    bool entryUidsRepaired = false;
    for (const auto& value : json_store_load_array(QStringLiteral("scan_lists.json"))) {
        if (!value.isObject()) {
            continue;
        }
        const QJsonObject object = value.toObject();
        const QString stored = object.value(QStringLiteral("uid")).toString();
        // Issue #697: an entry uid an earlier list holds (a hand-edited or older store) is given another, the first
        // list keeping it, so no two of the app's lists share a target id.
        auto row = normalize(object.toVariantMap(), {}, entryIds);
        entryUidsRepaired = entryUidsRepaired || entryUidsMinted(object, row);
        entryIds.unite(entryUids({row}));
        auto id = row.value("uid").toString();
        if (id.isEmpty() || ids.contains(id)) {
            id = uuid();
        }
        // Stored as text, so a UID that was not (a number) is saved as the one it reads as, and loads unrepaired.
        row["uid"] = id;
        // Issue #697: a UID minted here is not the one anything stored before this load named.
        m_uidsRepaired = m_uidsRepaired || id != stored;
        ids.insert(id);
        m_rows << row;
    }
    // Issue #697: a remembered scan session names its other lists by uid; after a re-mint those may be other lists
    // or none, so they are forgotten, as a restore forgets them.
    if (m_uidsRepaired) {
        (void)AppPrefs::forgetScanCompanions();
    }
    // Saved now (issue #801), or the next launch mints others and whatever named these (a pinned shortcut, the last
    // started list, a history row's entry) loses them. Unsaved, they still hold for this run. An entry's new uid names
    // no list, so it leaves the remembered companions alone.
    if ((m_uidsRepaired || entryUidsRepaired) && !save(m_rows)) {
        LOG_WARN("Scan-list identity repair could not be persisted; identities may change on restart.\n");
    }
}

int
ScanListsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QHash<int, QByteArray>
ScanListsModel::roleNames() const {
    return {{Qt::UserRole + 1, "name"},          {Qt::UserRole + 2, "uid"},     {Qt::UserRole + 3, "lastHeard"},
            {Qt::UserRole + 4, "entries"},       {Qt::UserRole + 5, "isDraft"}, {Qt::UserRole + 6, "targetSource"},
            {Qt::UserRole + 7, "targetsCsvPath"}};
}

QVariant
ScanListsModel::data(const QModelIndex& index, int role) const {
    return get(index.row()).value(QString::fromUtf8(roleNames().value(role)));
}

QVariantMap
ScanListsModel::get(int row) const {
    return row >= 0 && row < count() ? m_rows[row] : QVariantMap();
}

int
ScanListsModel::rowForUid(const QString& uid) const {
    for (int i = 0; i < count(); ++i) {
        if (m_rows[i].value("uid") == uid) {
            return i;
        }
    }
    return -1;
}

QVariantMap
ScanListsModel::newDraft() const {
    return normalize({{"isDraft", true}});
}

QString
ScanListsModel::newEntryId() const {
    const QSet<QString> existing = entryUids(m_rows);
    QString id;
    do {
        id = uuid();
    } while (existing.contains(id));
    return id;
}

bool
ScanListsModel::add(const QVariantMap& list) {
    auto row = normalize(list, {}, entryUids(m_rows));
    if (rowForUid(row.value("uid").toString()) >= 0) {
        row["uid"] = uuid();
    }
    row["lastHeard"] = 0;
    auto next = m_rows;
    next << row;
    if (!save(next)) {
        return false;
    }
    beginInsertRows({}, count(), count());
    m_rows = std::move(next);
    endInsertRows();
    Q_EMIT countChanged();
    return true;
}

bool
ScanListsModel::update(int row, const QVariantMap& list) {
    if (row < 0 || row >= count()) {
        return false;
    }
    auto next = m_rows;
    // The list's own entries keep their uids; one another list holds is given a new one, as at load and in add().
    next[row] = normalize(list, next[row], entryUids(next, row));
    if (!save(next)) {
        return false;
    }
    m_rows = std::move(next);
    Q_EMIT dataChanged(index(row), index(row));
    return true;
}

bool
ScanListsModel::remove(int row) {
    if (row < 0 || row >= count()) {
        return false;
    }
    const QString uid = m_rows[row].value("uid").toString();
    auto next = m_rows;
    next.removeAt(row);
    if (!save(next)) {
        return false;
    }
    beginRemoveRows({}, row, row);
    m_rows = std::move(next);
    endRemoveRows();
    if (QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]{1,63}$")).match(uid).hasMatch()) {
        // The generated targets: the list's folder (issue #697), hidden files included (removeRecursively() takes
        // every entry), and the file older versions wrote beside it.
        QDir(json_store_path(QStringLiteral("scan_lists/") + uid)).removeRecursively();
        QFile::remove(json_store_path(QStringLiteral("scan_lists/") + uid + QStringLiteral(".csv")));
    }
    Q_EMIT countChanged();
    return true;
}

void
ScanListsModel::touch(int row) {
    update(row, {{"lastHeard", realtimeSecsSinceEpoch()}});
}

int
ScanListsModel::entryCount(const QVariantMap& list) const {
    if (list.value("targetSource") != "csv") {
        return static_cast<int>(list.value("entries").toList().size());
    }
    const int accepted = m_acceptedRows ? m_acceptedRows(list.value("targetsCsvPath").toString()) : -1;
    return accepted > 0 ? accepted : 0;
}

namespace {
/* A list's own fallback files: talkgroups and source (radio ID) names. Empty is "none", which a start accepts. */
const char* const kFallbackCsvFields[] = {"groupCsvPath", "srcCsvPath"};

/* Whether @p row runs from target CSV @p path. */
bool
targetsPath(const QVariantMap& row, const QString& path) {
    return row.value("targetSource") == "csv" && row.value("targetsCsvPath") == path;
}

/* Whether @p row names @p path as one of its fallback files. */
bool
fallsBackOn(const QVariantMap& row, const QString& path) {
    return std::any_of(std::begin(kFallbackCsvFields), std::end(kFallbackCsvFields),
                       [&row, &path](const char* field) { return row.value(field) == path; });
}

/* The names of the lists @p matches picks for @p path; none for an empty path. */
template <typename Match>
QStringList
listNames(const QList<QVariantMap>& rows, const QString& path, Match matches) {
    QStringList names;
    if (!path.isEmpty()) {
        for (const auto& row : rows) {
            if (matches(row, path)) {
                names << row.value("name").toString();
            }
        }
    }
    return names;
}
} // namespace

QStringList
ScanListsModel::listsReferencingPath(const QString& path) const {
    return listNames(m_rows, path, [](const QVariantMap& row, const QString& p) {
        return targetsPath(row, p) || fallsBackOn(row, p);
    });
}

QStringList
ScanListsModel::listsTargetingPath(const QString& path) const {
    return listNames(m_rows, path, targetsPath);
}

QStringList
ScanListsModel::listsFallingBackOnPath(const QString& path) const {
    return listNames(m_rows, path, fallsBackOn);
}

QStringList
ScanListsModel::csvPaths() const {
    QStringList paths;
    const auto note = [&paths](const QString& path) {
        if (!path.isEmpty() && !paths.contains(path)) {
            paths.append(path);
        }
    };
    for (const auto& row : m_rows) {
        if (row.value("targetSource") == "csv") {
            note(row.value("targetsCsvPath").toString());
        }
        for (const char* field : kFallbackCsvFields) {
            note(row.value(field).toString());
        }
    }
    return paths;
}

bool
ScanListsModel::clearCsvPath(const QString& path) {
    if (listsReferencingPath(path).isEmpty()) {
        return true;
    }
    auto next = m_rows;
    for (auto& row : next) {
        // A list without its targets cannot run until another is chosen; one without a fallback file runs without it.
        if (targetsPath(row, path)) {
            row["targetsCsvPath"] = "";
            row["isDraft"] = true;
        }
        for (const char* field : kFallbackCsvFields) {
            if (row.value(field) == path) {
                row[field] = "";
            }
        }
    }
    if (!save(next)) {
        return false;
    }
    m_rows = std::move(next);
    Q_EMIT dataChanged(index(0), index(count() - 1));
    return true;
}

bool
ScanListsModel::save(const QList<QVariantMap>& rows) const {
    QJsonArray array;
    for (const auto& row : rows) {
        array.append(QJsonObject::fromVariantMap(row));
    }
    return json_store_save_array(QStringLiteral("scan_lists.json"), array);
}
} // namespace dsd_qt
