// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <initializer_list> // IWYU pragma: keep -- braced range-for lists require this header.
#include <qanystringview.h>
#include <utility>
#include "saved_systems_model.h"
#include "site_groups.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1String>
#include <QMap>
#include <QMetaType>
#include <QSet>
#include <QUuid>
#include <QVariant>
#include <cmath>
#include <limits>

#include <dsd-neo/runtime/log.h>
#include "json_store.h"
#include "radio_settings.h"
#include "realtime_clock.h"
#include "replay_store.h"
#include "stored_int.h"

namespace dsd_qt {

namespace {

constexpr const char kStoreFileName[] = "saved_systems.json";

/**
 * @brief The bias-tee tri-state from a stored value, migrating legacy bools.
 *
 * Legacy stores saved the wizard's on/off switch as a bool. true was an
 * explicit choice; false was the untouched default, whose observed behavior
 * was to follow the app-wide pref — it must read as follow (-1), never as a
 * frozen off. Out-of-range ints collapse to follow as well.
 */
int
bias_tee_from_stored(const QVariant& value) {
    if (value.typeId() == QMetaType::Bool) {
        return value.toBool() ? 1 : -1;
    }
    const int mode = stored_int_or(value, -1);
    return (mode >= -1 && mode <= 1) ? mode : -1;
}

constexpr int kPlaceMaxStates = 64;
constexpr int kPlaceMaxCounties = 512;
constexpr int kPlaceMaxName = 128;

/** A RadioReference id: a whole number (never text or a bool) in 1..INT32_MAX. */
bool
place_id(const QVariant& raw, int* id) {
    const int type = raw.typeId();
    if (type == QMetaType::Double) {
        const double value = raw.toDouble();
        if (!(value >= 1.0 && value <= static_cast<double>(std::numeric_limits<int>::max()))) {
            return false; // NaN included
        }
        *id = static_cast<int>(value);
        return !(value - *id > 0.0);
    }
    const bool whole = type == QMetaType::Int || type == QMetaType::UInt || type == QMetaType::LongLong
                       || type == QMetaType::ULongLong;
    int value = 0;
    if (!whole || !stored_int(raw, &value) || value < 1) {
        return false;
    }
    *id = value;
    return true;
}

/** An entry's optional name: absent leaves @p name empty; text up to the limit; anything else fails. */
bool
place_name(const QVariantMap& entry, QString* name) {
    name->clear();
    const auto it = entry.constFind(QStringLiteral("name"));
    if (it == entry.cend()) {
        return true;
    }
    if (it->typeId() != QMetaType::QString) {
        return false;
    }
    *name = it->toString();
    return name->size() <= kPlaceMaxName;
}

/** The list @p place holds at @p key, of at most @p max entries; an absent key is an empty list. */
bool
place_list(const QVariantMap& place, const QString& key, int max, QVariantList* list) {
    list->clear();
    const auto it = place.constFind(key);
    if (it == place.cend()) {
        return true;
    }
    if (it->typeId() != QMetaType::QVariantList) {
        return false;
    }
    *list = it->toList();
    return list->size() <= max;
}

/** Append @p entry, or merge it into the entry @p id already has, which takes only the keys it lacks. */
void
merge_place_entry(QVariantList* out, QHash<int, qsizetype>* at, int id, const QVariantMap& entry) {
    const auto found = at->constFind(id);
    if (found == at->cend()) {
        at->insert(id, out->size());
        out->append(entry);
        return;
    }
    QVariantMap kept = out->at(*found).toMap();
    for (auto it = entry.cbegin(); it != entry.cend(); ++it) {
        if (!kept.contains(it.key())) {
            kept.insert(it.key(), it.value());
        }
    }
    (*out)[*found] = kept;
}

/** One state or county entry, keyed by @p idKey, with an optional state link when @p linked. */
bool
place_entry(const QVariant& value, const QString& idKey, bool linked, int* id, QVariantMap* entry) {
    if (value.typeId() != QMetaType::QVariantMap) {
        return false;
    }
    const QVariantMap raw = value.toMap();
    QString name;
    if (!place_id(raw.value(idKey), id) || !place_name(raw, &name)) {
        return false;
    }
    *entry = QVariantMap{{idKey, *id}};
    if (!name.isEmpty()) {
        entry->insert(QStringLiteral("name"), name);
    }
    const QString link = QStringLiteral("stid");
    if (linked && raw.contains(link)) {
        int stid = 0;
        if (!place_id(raw.value(link), &stid)) {
            return false;
        }
        entry->insert(link, stid);
    }
    return true;
}

/** The entries @p place holds at @p key, validated and merged by id into @p out. */
bool
place_entries(const QVariantMap& place, const QString& key, const QString& idKey, int max, QVariantList* out) {
    QVariantList raw;
    if (!place_list(place, key, max, &raw)) {
        return false;
    }
    QHash<int, qsizetype> at;
    const bool linked = idKey != QLatin1String("stid");
    for (const QVariant& value : raw) {
        int id = 0;
        QVariantMap entry;
        if (!place_entry(value, idKey, linked, &id, &entry)) {
            return false;
        }
        merge_place_entry(out, &at, id, entry);
    }
    return true;
}

/** The candidate state ids, validated, each once. */
bool
place_candidates(const QVariantMap& place, QVariantList* out) {
    QVariantList raw;
    if (!place_list(place, QStringLiteral("candidateStates"), kPlaceMaxStates, &raw)) {
        return false;
    }
    QSet<int> seen;
    for (const QVariant& value : raw) {
        int stid = 0;
        if (!place_id(value, &stid)) {
            return false;
        }
        if (!seen.contains(stid)) {
            seen.insert(stid);
            out->append(stid);
        }
    }
    return true;
}

/** Every id a place holds, in order: what decides whether its names need filling again. */
QList<int>
place_ids(const QVariantMap& place) {
    QList<int> ids;
    for (const auto& [key, idKey] : {std::make_pair("states", "stid"), std::make_pair("counties", "ctid")}) {
        for (const QVariant& entry : place.value(QLatin1String(key)).toList()) {
            ids.append(entry.toMap().value(QLatin1String(idKey)).toInt());
        }
        ids.append(0);
    }
    for (const QVariant& stid : place.value(QStringLiteral("candidateStates")).toList()) {
        ids.append(stid.toInt());
    }
    return ids;
}

/** Set @p entry's name to @p name when it is a usable name and differs; true when it changed. */
bool
set_place_name(QVariantMap& entry, const QString& name) {
    if (name.isEmpty() || name.size() > kPlaceMaxName || entry.value(QStringLiteral("name")).toString() == name) {
        return false;
    }
    entry.insert(QStringLiteral("name"), name);
    return true;
}

/** Name and link the counties @p countyNames covers in @p counties; @p linked says whether any links to @p stid. */
bool
name_place_counties(QVariantList& counties, int stid, const QHash<int, QString>& countyNames, bool* linked) {
    bool changed = false;
    const QString link = QStringLiteral("stid");
    for (QVariant& value : counties) {
        QVariantMap county = value.toMap();
        const auto name = countyNames.constFind(county.value(QStringLiteral("ctid")).toInt());
        if (name == countyNames.cend()) {
            *linked = *linked || county.value(link).toInt() == stid;
            continue;
        }
        *linked = true;
        bool countyChanged = set_place_name(county, *name);
        if (county.value(link).toInt() != stid) {
            county.insert(link, stid);
            countyChanged = true;
        }
        if (countyChanged) {
            value = county;
            changed = true;
        }
    }
    return changed;
}

/** @p stid's names applied to @p place (see SavedSystemsModel::applyPlaceNames()); true when anything changed. */
bool
apply_place_names(QVariantMap& place, int stid, const QString& stateName, const QHash<int, QString>& countyNames) {
    bool linked = false;
    QVariantList counties = place.value(QStringLiteral("counties")).toList();
    bool changed = name_place_counties(counties, stid, countyNames, &linked);
    QVariantList states = place.value(QStringLiteral("states")).toList();
    bool listed = false;
    for (QVariant& value : states) {
        QVariantMap state = value.toMap();
        if (state.value(QStringLiteral("stid")).toInt() == stid) {
            listed = true;
            if (set_place_name(state, stateName)) {
                value = state;
                changed = true;
            }
        }
    }
    if (linked && !listed && states.size() < kPlaceMaxStates) {
        QVariantMap state{{QStringLiteral("stid"), stid}};
        set_place_name(state, stateName);
        states.append(state);
        changed = true;
    }
    if (changed) {
        place.insert(QStringLiteral("states"), states);
        place.insert(QStringLiteral("counties"), counties);
    }
    return changed;
}

/** The keys an import carries of its own site; an edit that changes the tuning keeps only these, never stored ones. */
constexpr const char* kFreshProvenanceKeys[] = {"rrSid",   "rrSiteId",   "siteName", "siteLat",
                                                "siteLon", "hasSitePos", "place"};

/** Whether @p system changes a field that decides what the row's imported site is. */
bool
changes_site_tuning(const QVariantMap& previous, const QVariantMap& system) {
    for (const char* key : {"freqMhz", "decodeFlag", "chanCsvPath", "groupCsvPath", "keyCsvPath", "keyCsvHex",
                            "p25BandplanCsvPath", "srcCsvPath"}) {
        const QString name = QLatin1String(key);
        if (system.contains(name) && system.value(name) != previous.value(name)) {
            return true;
        }
    }
    return false;
}

/**
 * @brief The fields an edit that changes the tuning applies: the stored provenance cleared, then what @p system
 * carries of its own (an edit-mode re-import).
 */
QVariantMap
reimport_fields(const QVariantMap& system) {
    QVariantMap fields = rr_provenance_cleared(system);
    for (const char* key : kFreshProvenanceKeys) {
        const QString name = QLatin1String(key);
        if (system.contains(name)) {
            fields.insert(name, system.value(name));
        }
    }
    return fields;
}

/**
 * @brief A generated "base — oldSite" name follows its site: when @p system renames the site of the row's own
 * RadioReference site id (a re-import or an Update from RadioReference), with or without a tuning change.
 */
void
rename_generated_name(const QVariantMap& previous, const QVariantMap& system, QVariantMap* fields) {
    const QString siteKey = QStringLiteral("siteName");
    const QString siteIdKey = QStringLiteral("rrSiteId");
    const int storedSite = previous.value(siteIdKey).toInt();
    int site = 0;
    if (!system.contains(siteKey) || storedSite <= 0 || !stored_int(system.value(siteIdKey), &site)
        || site != storedSite) {
        return;
    }
    const QString oldSite = previous.value(siteKey).toString();
    const QString newSite = system.value(siteKey).toString();
    if (newSite == oldSite) {
        return;
    }
    const QString nameKey = QStringLiteral("name");
    const QString name = (system.contains(nameKey) ? system : previous).value(nameKey).toString();
    fields->insert(nameKey, site_generated_name(name, oldSite, newSite));
}

} // namespace

QVariantMap
SavedSystemsModel::validPlace(const QVariant& raw) {
    if (raw.typeId() != QMetaType::QVariantMap) {
        return QVariantMap();
    }
    const QVariantMap place = raw.toMap();
    QVariantList states;
    QVariantList counties;
    QVariantList candidates;
    if (!place_entries(place, QStringLiteral("states"), QStringLiteral("stid"), kPlaceMaxStates, &states)
        || !place_entries(place, QStringLiteral("counties"), QStringLiteral("ctid"), kPlaceMaxCounties, &counties)
        || !place_candidates(place, &candidates)) {
        return QVariantMap();
    }
    if (states.isEmpty() && counties.isEmpty() && candidates.isEmpty()) {
        return QVariantMap();
    }
    return {{QStringLiteral("states"), states},
            {QStringLiteral("counties"), counties},
            {QStringLiteral("candidateStates"), candidates}};
}

void
SavedSystemsModel::setPlaceNameCache(PlaceNameFill fill) {
    m_placeNames = std::move(fill);
}

void
SavedSystemsModel::fillPlaceNames(Row& row, const QVariantMap& previous) const {
    if (!m_placeNames || row.place.isEmpty() || place_ids(row.place) == place_ids(previous)) {
        return;
    }
    m_placeNames(row.place);
    row.place = validPlace(row.place);
}

SavedSystemsModel::SavedSystemsModel(QObject* parent)
    : QAbstractListModel(parent), m_realtime_now_ms(&realtimeMSecsSinceEpoch) {
    load();
}

SavedSystemsModel::~SavedSystemsModel() = default;

int
SavedSystemsModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

/** @brief Identity roles; each group reads one field without copying the whole row into a map. */
QVariant
SavedSystemsModel::identityRoleValue(const Row& row, int role) {
    switch (role) {
        case AirspyRole: return row.airspy;
        case UidRole: return row.uid;
        case NameRole: return row.name;
        case SourceTypeRole: return row.sourceType;
        case HostRole: return row.host;
        case PortRole: return row.port;
        case FreqMhzRole: return row.freqMhz;
        case DecodeFlagRole: return row.decodeFlag;
        case FilePathRole: return row.filePath;
        default: return QVariant();
    }
}

/** @brief Optional direct-key and site metadata, separate from the identity and tuner groups. */
QVariant
SavedSystemsModel::detailRoleValue(const Row& row, int role) {
    switch (role) {
        case EncKeyTypeRole: return row.encKeyType;
        case EncForceKeyRole: return row.encForceKey;
        case RrSidRole: return row.rrSid;
        case RrSiteIdRole: return row.rrSiteId;
        case SiteNameRole: return row.siteName;
        case SiteLatRole: return row.siteLat;
        case SiteLonRole: return row.siteLon;
        case HasSitePosRole: return row.hasSitePos;
        case AvoidSiteRole: return row.avoidSite;
        case PlaceRole: return row.place;
        case KeyConfiguredRole:
            return !row.encKeyType.isEmpty() || !row.keyCsvPath.isEmpty() || !row.decryptionProfileUid.isEmpty();
        default: return QVariant();
    }
}

/** @brief Tuning, imports and recency roles. */
QVariant
SavedSystemsModel::tuningRoleValue(const Row& row, int role) {
    switch (role) {
        case TrunkingRole: return row.trunking;
        case GainDbRole: return row.gainDb;
        case PpmRole: return row.ppm;
        case HangtimeRole: return row.hangtime;
        case BandwidthKhzRole: return row.bandwidthKhz;
        case BiasTeeRole: return row.biasTee;
        case ExtraArgsRole: return row.extraArgs;
        case LastHeardRole: return row.lastHeard;
        case ChanCsvPathRole: return row.chanCsvPath;
        case GroupCsvPathRole: return row.groupCsvPath;
        case KeyCsvPathRole: return row.keyCsvPath;
        case KeyCsvHexRole: return row.keyCsvHex;
        case P25BandplanCsvPathRole: return row.p25BandplanCsvPath;
        case SrcCsvPathRole: return row.srcCsvPath;
        default: return QVariant();
    }
}

QVariant
SavedSystemsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return QVariant();
    }
    const Row& row = m_rows.at(index.row());
    QVariant identity = identityRoleValue(row, role);
    if (identity.isValid()) {
        return identity;
    }
    const QVariant detail = detailRoleValue(row, role);
    return detail.isValid() ? detail : tuningRoleValue(row, role);
}

QHash<int, QByteArray>
SavedSystemsModel::roleNames() const {
    QHash<int, QByteArray> roles;
    roles.insert(AirspyRole, QByteArrayLiteral("airspy"));
    roles.insert(NameRole, QByteArrayLiteral("name"));
    roles.insert(SourceTypeRole, QByteArrayLiteral("sourceType"));
    roles.insert(HostRole, QByteArrayLiteral("host"));
    roles.insert(PortRole, QByteArrayLiteral("port"));
    roles.insert(FreqMhzRole, QByteArrayLiteral("freqMhz"));
    roles.insert(DecodeFlagRole, QByteArrayLiteral("decodeFlag"));
    roles.insert(TrunkingRole, QByteArrayLiteral("trunking"));
    roles.insert(GainDbRole, QByteArrayLiteral("gainDb"));
    roles.insert(PpmRole, QByteArrayLiteral("ppm"));
    roles.insert(HangtimeRole, QByteArrayLiteral("hangtime"));
    roles.insert(BandwidthKhzRole, QByteArrayLiteral("bandwidthKhz"));
    roles.insert(BiasTeeRole, QByteArrayLiteral("biasTee"));
    roles.insert(ExtraArgsRole, QByteArrayLiteral("extraArgs"));
    roles.insert(FilePathRole, QByteArrayLiteral("filePath"));
    roles.insert(LastHeardRole, QByteArrayLiteral("lastHeard"));
    roles.insert(ChanCsvPathRole, QByteArrayLiteral("chanCsvPath"));
    roles.insert(GroupCsvPathRole, QByteArrayLiteral("groupCsvPath"));
    roles.insert(KeyCsvPathRole, QByteArrayLiteral("keyCsvPath"));
    roles.insert(KeyCsvHexRole, QByteArrayLiteral("keyCsvHex"));
    roles.insert(P25BandplanCsvPathRole, QByteArrayLiteral("p25BandplanCsvPath"));
    roles.insert(SrcCsvPathRole, QByteArrayLiteral("srcCsvPath"));
    roles.insert(UidRole, QByteArrayLiteral("uid"));
    roles.insert(EncKeyTypeRole, QByteArrayLiteral("encKeyType"));
    roles.insert(EncForceKeyRole, QByteArrayLiteral("encForceKey"));
    roles.insert(RrSidRole, QByteArrayLiteral("rrSid"));
    roles.insert(RrSiteIdRole, QByteArrayLiteral("rrSiteId"));
    roles.insert(SiteNameRole, QByteArrayLiteral("siteName"));
    roles.insert(SiteLatRole, QByteArrayLiteral("siteLat"));
    roles.insert(SiteLonRole, QByteArrayLiteral("siteLon"));
    roles.insert(HasSitePosRole, QByteArrayLiteral("hasSitePos"));
    roles.insert(AvoidSiteRole, QByteArrayLiteral("avoidSite"));
    roles.insert(PlaceRole, QByteArrayLiteral("place"));
    roles.insert(KeyConfiguredRole, QByteArrayLiteral("keyConfigured"));
    return roles;
}

namespace {

/**
 * @brief Overwrite @p target only when @p key is present; absent keys keep the base value.
 *
 * @p key is a QString so callers can pass QStringLiteral: building one from a
 * `const char*` here would heap-allocate on every field of every stored row.
 */
void
map_take_string(const QVariantMap& map, const QString& key, QString* target) {
    if (map.contains(key)) {
        *target = map.value(key).toString();
    }
}

/** @brief As map_take_string(), for an int: a value that is not one (a restored 64-bit one included) keeps the base. */
void
map_take_int(const QVariantMap& map, const QString& key, int* target) {
    if (map.contains(key)) {
        (void)stored_int(map.value(key), target);
    }
}

void
map_take_bool(const QVariantMap& map, const QString& key, bool* target) {
    if (map.contains(key)) {
        *target = map.value(key).toBool();
    }
}

} // namespace

void
SavedSystemsModel::fillRowKey(const QVariantMap& map, Row& row) {
    if (map.contains(QStringLiteral("encKeyValue"))) {
        row.encKeyValue = map.value(QStringLiteral("encKeyValue")).toString();
    }
    if (map.contains(QStringLiteral("encKeyType"))) {
        const QVariant stored = map.value(QStringLiteral("encKeyType"));
        row.encKeyType = stored.toString();
        // WP0 persisted a wire-enum integer, with 0 also its untouched default.
        // Preserve that default as absent while migrating actual typed keys.
        if (stored.typeId() != QMetaType::QString) {
            int type = 0;
            const bool ok = stored_int(stored, &type);
            const QStringList types{QStringLiteral("basic"), QStringLiteral("hex"), QStringLiteral("rc4"),
                                    QStringLiteral("scrambler")};
            if (ok && type >= 0 && type < types.size()) {
                row.encKeyType = type == 0 && row.encKeyValue.isEmpty() ? QString() : types[type];
            }
        }
    }
    map_take_int(map, QStringLiteral("encForceKey"), &row.encForceKey);
}

void
SavedSystemsModel::fillRowDetails(const QVariantMap& map, Row& row) {
    fillRowKey(map, row);
    map_take_int(map, QStringLiteral("rrSid"), &row.rrSid);
    map_take_int(map, QStringLiteral("rrSiteId"), &row.rrSiteId);
    if (map.contains(QStringLiteral("siteName"))) {
        row.siteName = map.value(QStringLiteral("siteName")).toString();
    }
    if (map.contains(QStringLiteral("siteLat"))) {
        row.siteLat = map.value(QStringLiteral("siteLat")).toDouble();
    }
    if (map.contains(QStringLiteral("siteLon"))) {
        row.siteLon = map.value(QStringLiteral("siteLon")).toDouble();
    }
    if (map.contains(QStringLiteral("hasSitePos"))) {
        row.hasSitePos = map.value(QStringLiteral("hasSitePos")).toBool();
    }
    bool latOk = true;
    bool lonOk = true;
    if (map.contains(QStringLiteral("siteLat"))) {
        map.value(QStringLiteral("siteLat")).toDouble(&latOk);
    }
    if (map.contains(QStringLiteral("siteLon"))) {
        map.value(QStringLiteral("siteLon")).toDouble(&lonOk);
    }
    row.hasSitePos = row.hasSitePos && latOk && lonOk && site_position_valid(row.siteLat, row.siteLon);
    if (map.contains(QStringLiteral("avoidSite"))) {
        row.avoidSite = map.value(QStringLiteral("avoidSite")).toBool();
    }
    if (map.contains(QStringLiteral("place"))) {
        row.place = validPlace(map.value(QStringLiteral("place")));
    }
}

SavedSystemsModel::Row
SavedSystemsModel::rowFromMap(const QVariantMap& map, const Row& base) {
    Row row = base;
    // An edit cannot change identity. Loading can adopt a valid persisted UUID;
    // add() replaces it afterwards so duplicating a row never duplicates identity.
    if (row.uid.isEmpty()) {
        const QString stored = canonicalUid(map.value(QStringLiteral("uid")).toString());
        row.uid = stored.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : stored;
    }
    fillRowDetails(map, row);
    if (base.uid.isEmpty() && (!map.contains(QStringLiteral("siteLat")) || !map.contains(QStringLiteral("siteLon")))) {
        row.hasSitePos = false;
    }

    map_take_string(map, QStringLiteral("name"), &row.name);
    map_take_string(map, QStringLiteral("sourceType"), &row.sourceType);
    map_take_string(map, QStringLiteral("host"), &row.host);
    map_take_int(map, QStringLiteral("port"), &row.port);
    map_take_string(map, QStringLiteral("freqMhz"), &row.freqMhz);
    map_take_string(map, QStringLiteral("decodeFlag"), &row.decodeFlag);
    if (map.contains(QStringLiteral("radioSettings"))) {
        row.radioSettings = map.value(QStringLiteral("radioSettings")).toMap();
    } else if (row.decodeFlag != base.decodeFlag) {
        // A new wizard preset supersedes the previous live preset/modulation.
        row.radioSettings.remove(QStringLiteral("decodeFlag"));
        row.radioSettings.remove(QStringLiteral("modulation"));
    }
    map_take_bool(map, QStringLiteral("trunking"), &row.trunking);
    if (map.contains(QStringLiteral("airspy"))) {
        row.airspy = map.value(QStringLiteral("airspy")).toMap();
    }
    map_take_int(map, QStringLiteral("gainDb"), &row.gainDb);
    map_take_string(map, QStringLiteral("ppm"), &row.ppm);
    map_take_string(map, QStringLiteral("hangtime"), &row.hangtime);
    map_take_int(map, QStringLiteral("bandwidthKhz"), &row.bandwidthKhz);
    if (map.contains(QStringLiteral("biasTee"))) {
        row.biasTee = bias_tee_from_stored(map.value(QStringLiteral("biasTee")));
    }
    map_take_string(map, QStringLiteral("extraArgs"), &row.extraArgs);
    map_take_string(map, QStringLiteral("filePath"), &row.filePath);
    map_take_string(map, QStringLiteral("decryptionProfileUid"), &row.decryptionProfileUid);
    if (map.contains(QStringLiteral("lastHeard"))) {
        row.lastHeard = map.value(QStringLiteral("lastHeard")).toLongLong();
    }
    map_take_string(map, QStringLiteral("chanCsvPath"), &row.chanCsvPath);
    map_take_string(map, QStringLiteral("groupCsvPath"), &row.groupCsvPath);
    map_take_string(map, QStringLiteral("keyCsvPath"), &row.keyCsvPath);
    map_take_bool(map, QStringLiteral("keyCsvHex"), &row.keyCsvHex);
    map_take_string(map, QStringLiteral("p25BandplanCsvPath"), &row.p25BandplanCsvPath);
    map_take_string(map, QStringLiteral("srcCsvPath"), &row.srcCsvPath);
    return row;
}

QString
SavedSystemsModel::canonicalUid(const QString& candidate) {
    const QUuid parsed(candidate);
    return parsed.isNull() ? QString() : parsed.toString(QUuid::WithoutBraces);
}

QVariantMap
SavedSystemsModel::mapFromRow(const Row& row) const {
    QVariantMap map;
    map.insert(QStringLiteral("uid"), row.uid);
    map.insert(QStringLiteral("decryptionProfileUid"), row.decryptionProfileUid);
    map.insert(QStringLiteral("encKeyType"), row.encKeyType);
    map.insert(QStringLiteral("encKeyConfigured"), !row.encKeyType.isEmpty());
    map.insert(QStringLiteral("encForceKey"), row.encForceKey);
    map.insert(QStringLiteral("rrSid"), row.rrSid);
    map.insert(QStringLiteral("rrSiteId"), row.rrSiteId);
    map.insert(QStringLiteral("siteName"), row.siteName);
    map.insert(QStringLiteral("siteLat"), row.siteLat);
    map.insert(QStringLiteral("siteLon"), row.siteLon);
    map.insert(QStringLiteral("hasSitePos"), row.hasSitePos);
    map.insert(QStringLiteral("avoidSite"), row.avoidSite);
    map.insert(QStringLiteral("place"), row.place);

    map.insert(QStringLiteral("name"), row.name);
    map.insert(QStringLiteral("sourceType"), row.sourceType);
    map.insert(QStringLiteral("host"), row.host);
    map.insert(QStringLiteral("port"), row.port);
    map.insert(QStringLiteral("freqMhz"), row.freqMhz);
    map.insert(QStringLiteral("decodeFlag"), row.decodeFlag);
    map.insert(QStringLiteral("radioSettings"), row.radioSettings);
    map.insert(QStringLiteral("trunking"), row.trunking);
    map.insert(QStringLiteral("airspy"), row.airspy);
    map.insert(QStringLiteral("gainDb"), row.gainDb);
    map.insert(QStringLiteral("ppm"), row.ppm);
    map.insert(QStringLiteral("hangtime"), row.hangtime);
    map.insert(QStringLiteral("bandwidthKhz"), row.bandwidthKhz);
    map.insert(QStringLiteral("biasTee"), row.biasTee);
    map.insert(QStringLiteral("extraArgs"), row.extraArgs);
    map.insert(QStringLiteral("filePath"), row.filePath);
    map.insert(QStringLiteral("lastHeard"), row.lastHeard);
    map.insert(QStringLiteral("chanCsvPath"), row.chanCsvPath);
    map.insert(QStringLiteral("groupCsvPath"), row.groupCsvPath);
    map.insert(QStringLiteral("keyCsvPath"), row.keyCsvPath);
    map.insert(QStringLiteral("keyCsvHex"), row.keyCsvHex);
    map.insert(QStringLiteral("p25BandplanCsvPath"), row.p25BandplanCsvPath);
    map.insert(QStringLiteral("srcCsvPath"), row.srcCsvPath);
    return map;
}

bool
SavedSystemsModel::add(const QVariantMap& system) {
    return addAll({system}, QString()) >= 0;
}

bool
SavedSystemsModel::addWithKeyFrom(const QString& sourceUid, const QVariantMap& system) {
    return rowForUid(sourceUid) >= 0 && addAll({system}, sourceUid) >= 0;
}

int
SavedSystemsModel::addAll(const QVariantList& systems, const QString& keySourceUid) {
    const int keySource = keySourceUid.isEmpty() ? -1 : rowForUid(keySourceUid);
    if (systems.isEmpty() || (!keySourceUid.isEmpty() && keySource < 0)) {
        return -1;
    }
    QList<Row> added;
    added.reserve(systems.size());
    for (const QVariant& system : systems) {
        Row row;
        if (!newRow(system, keySource, &row)) {
            return -1;
        }
        added.append(row);
    }
    return appendRows(added);
}

bool
SavedSystemsModel::takeRetainedKey(int source, const QVariantMap& system, QVariantMap* fields) const {
    *fields = system;
    if (source < 0) {
        return true;
    }
    const Row& stored = m_rows.at(source);
    if (system.contains(QStringLiteral("encKeyValue"))) {
        return false;
    }
    if (!stored.decryptionProfileUid.isEmpty()
        && system.value(QStringLiteral("decryptionProfileUid")).toString() == stored.decryptionProfileUid) {
        return true;
    }
    if (stored.encKeyType.isEmpty() || stored.encKeyValue.isEmpty()
        || system.value(QStringLiteral("encKeyType")).toString() != stored.encKeyType) {
        return false;
    }
    // This map stays in C++; the row persists the private value under its own fresh UID.
    fields->insert(QStringLiteral("encKeyValue"), stored.encKeyValue);
    return true;
}

bool
SavedSystemsModel::newRow(const QVariant& system, int keySource, Row* row) const {
    QVariantMap fields;
    if (system.typeId() != QMetaType::QVariantMap
        || !radio_settings_valid(system.toMap().value(QStringLiteral("radioSettings")))
        || !takeRetainedKey(keySource, system.toMap(), &fields)) {
        return false;
    }
    *row = rowFromMap(fields, Row());
    row->uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    row->filePath = migratedReplay(QString(), row->filePath);
    fillPlaceNames(*row, QVariantMap());
    return true;
}

int
SavedSystemsModel::appendRows(const QList<Row>& added) {
    // Adopts in order and stops at the first refusal.
    if (!std::all_of(added.cbegin(), added.cend(), [this](const Row& row) { return adoptReplay(row.filePath); })) {
        return -1;
    }
    auto next = m_rows;
    next.append(added);
    if (!saveRows(next)) {
        return -1;
    }
    const int first = count();
    beginInsertRows(QModelIndex(), first, first + static_cast<int>(added.size()) - 1);
    m_rows = std::move(next);
    endInsertRows();
    Q_EMIT sitesChanged();
    Q_EMIT countChanged();
    return first;
}

bool
SavedSystemsModel::update(int row, const QVariantMap& system) {
    if (row < 0 || row >= m_rows.size() || !radio_settings_valid(system.value(QStringLiteral("radioSettings")))) {
        return false;
    }
    // WP-D4, B11: changed tuning/CSV content no longer describes the imported site; only what this edit carries of
    // its own (a re-import's fresh provenance) describes it now.
    const auto previous = get(row);
    QVariantMap fields = changes_site_tuning(previous, system) ? reimport_fields(system) : system;
    rename_generated_name(previous, system, &fields);
    // Missing encKeyValue means Keep; a present empty value explicitly clears it.
    auto next = m_rows;
    next[row] = rowFromMap(fields, m_rows.at(row));
    fillPlaceNames(next[row], m_rows.at(row).place);
    // A draft opened before the replay migration finished still names the cached file: it saves as the copy.
    next[row].filePath = migratedReplay(next.at(row).uid, next.at(row).filePath);
    const QString previousFile = m_rows.at(row).filePath;
    const QString file = next.at(row).filePath;
    if ((file != previousFile && !adoptReplay(file)) || !saveRows(next)) {
        return false;
    }
    m_rows = std::move(next);
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx);
    Q_EMIT sitesChanged();
    if (file != previousFile) {
        releaseReplayIfUnused(previousFile);
    }
    return true;
}

bool
SavedSystemsModel::updateRadioSettings(const QString& uid, const QVariantMap& changes) {
    const int row = rowForUid(uid);
    if (row < 0 || changes.isEmpty() || !radio_settings_valid(changes)) {
        return false;
    }
    auto settings = m_rows.at(row).radioSettings;
    if (changes.contains(QStringLiteral("decodeFlag"))) {
        settings.remove(QStringLiteral("modulation"));
    }
    for (auto it = changes.cbegin(); it != changes.cend(); ++it) {
        settings.insert(it.key(), it.value());
    }
    QVariantMap fields{{QStringLiteral("radioSettings"), settings}};
    if (changes.contains(QStringLiteral("decodeFlag"))) {
        fields.insert(QStringLiteral("decodeFlag"), changes.value(QStringLiteral("decodeFlag")));
    }
    return update(row, fields);
}

bool
SavedSystemsModel::remove(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return false;
    }
    // Saved first: a removal that did not reach the disk comes back at the next launch, so the card (and anything
    // that follows rows out of the model) must keep it.
    auto next = m_rows;
    next.removeAt(row);
    if (!saveRows(next)) {
        return false;
    }
    const QString file = m_rows.at(row).filePath;
    beginRemoveRows(QModelIndex(), row, row);
    m_rows = std::move(next);
    endRemoveRows();
    Q_EMIT sitesChanged();
    Q_EMIT countChanged();
    releaseReplayIfUnused(file);
    return true;
}

QVariantMap
SavedSystemsModel::get(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QVariantMap();
    }
    return mapFromRow(m_rows.at(row));
}

QString
SavedSystemsModel::groupKey(int row) const {
    if (row < 0 || row >= m_rows.size()) {
        return QString();
    }
    const Row& at = m_rows.at(row);
    if (!site_grouped(at.rrSid, at.rrSiteId)) {
        return QString();
    }
    return site_group_key(at.rrSid,
                          receiver_identity(at.sourceType, at.host, at.port,
                                            at.airspy.value(QStringLiteral("serial")).toString(), at.filePath));
}

QVariantList
SavedSystemsModel::siblingRows(int row) const {
    QVariantList rows;
    const QString key = groupKey(row);
    for (int i = 0; !key.isEmpty() && i < m_rows.size(); ++i) {
        if (groupKey(i) == key) {
            rows.append(i);
        }
    }
    return rows;
}

int
SavedSystemsModel::nearestRow(int row, double lat, double lon) const {
    return site_nearest_among(siblingRows(row), [this](int index) { return get(index); }, lat, lon);
}

int
SavedSystemsModel::siteCount(int row) const {
    return siblingRows(row).size();
}

double
SavedSystemsModel::distanceKm(int row, double lat, double lon) const {
    const auto site = get(row);
    return site.value("hasSitePos").toBool()
               ? site_distance_km(lat, lon, site.value("siteLat").toDouble(), site.value("siteLon").toDouble())
               : -1;
}

bool
SavedSystemsModel::setAvoidSite(int row, bool avoid) {
    return update(row, {{"avoidSite", avoid}});
}

int
SavedSystemsModel::rowForUid(const QString& uid) const {
    if (!uid.isEmpty()) {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).uid == uid) {
                return i;
            }
        }
    }
    return -1;
}

QVariantMap
SavedSystemsModel::getByUid(const QString& uid) const {
    return get(rowForUid(uid));
}

qint64
SavedSystemsModel::realtimeNowMs() const {
    return m_realtime_now_ms();
}

void
SavedSystemsModel::touch(int row) {
    if (row < 0 || row >= m_rows.size()) {
        return;
    }
    // Recency is advisory, so a failed save is not reported; it stays out of memory, though, so the recency shown now
    // is the one the next launch shows.
    auto next = m_rows;
    next[row].lastHeard = m_realtime_now_ms() / 1000;
    if (!saveRows(next)) {
        return;
    }
    m_rows = std::move(next);
    const QModelIndex idx = index(row);
    Q_EMIT dataChanged(idx, idx, {LastHeardRole});
}

QStringList
SavedSystemsModel::systemsReferencingPath(const QString& path) const {
    QStringList names;
    if (path.isEmpty()) {
        return names;
    }
    for (const Row& row : m_rows) {
        if (row.chanCsvPath == path || row.groupCsvPath == path || row.keyCsvPath == path
            || row.p25BandplanCsvPath == path || row.srcCsvPath == path) {
            names.append(row.name);
        }
    }
    return names;
}

QStringList
SavedSystemsModel::csvPaths() const {
    QStringList paths;
    for (const Row& row : m_rows) {
        for (const QString* path :
             {&row.chanCsvPath, &row.groupCsvPath, &row.keyCsvPath, &row.p25BandplanCsvPath, &row.srcCsvPath}) {
            if (!path->isEmpty() && !paths.contains(*path)) {
                paths.append(*path);
            }
        }
    }
    return paths;
}

bool
SavedSystemsModel::clearCsvPath(const QString& path) {
    return clearPathProvenance(path, true);
}

bool
SavedSystemsModel::invalidatePathProvenance(const QString& path) {
    return clearPathProvenance(path, false);
}

bool
SavedSystemsModel::clearPathProvenance(const QString& path, bool clearFields) {
    if (path.isEmpty()) {
        return true;
    }
    const QVariantMap cleared = rr_provenance_cleared(QVariantMap());
    auto next = m_rows;
    int first = -1;
    int last = -1;
    for (int i = 0; i < next.size(); i++) {
        Row& row = next[i];
        bool names = false;
        for (QString Row::* field :
             {&Row::chanCsvPath, &Row::groupCsvPath, &Row::keyCsvPath, &Row::p25BandplanCsvPath, &Row::srcCsvPath}) {
            if (row.*field == path) {
                names = true;
                if (clearFields) {
                    (row.*field).clear();
                }
            }
        }
        if (!names) {
            continue;
        }
        // The file no longer describes the site, and the site leaves its group with the only chooser that could lift
        // its avoid. A row with nothing to clear (and no field cleared) is left out: no save, no signal for it.
        const Row clearedRow = rowFromMap(cleared, row);
        if (clearFields || mapFromRow(clearedRow) != mapFromRow(row)) {
            row = clearedRow;
            first = first < 0 ? i : first;
            last = i;
        }
    }
    if (first < 0) {
        return true;
    }
    // Saved first, so a failure leaves every reference in place for a retry to clear.
    if (!saveRows(next)) {
        return false;
    }
    commitChangedRange(std::move(next), first, last, {});
    Q_EMIT sitesChanged();
    return true;
}

void
SavedSystemsModel::commitChangedRange(QList<Row>&& next, int first, int last, const QList<int>& roles) {
    m_rows = std::move(next);
    // One signal for the whole span: each dataChanged re-runs whatever follows the model over every row.
    Q_EMIT dataChanged(index(first), index(last), roles);
}

bool
SavedSystemsModel::applyPlaceNames(int stid, const QString& stateName, const QHash<int, QString>& countyNames) {
    if (stid <= 0) {
        return true;
    }
    auto next = m_rows;
    int first = -1;
    int last = -1;
    for (int i = 0; i < next.size(); i++) {
        if (apply_place_names(next[i].place, stid, stateName, countyNames)) {
            first = first < 0 ? i : first;
            last = i;
        }
    }
    if (first < 0) {
        return true;
    }
    // A failure leaves every row as it was; the next resolve of this state tries again.
    if (!saveRows(next)) {
        return false;
    }
    commitChangedRange(std::move(next), first, last, {PlaceRole});
    return true;
}

bool
SavedSystemsModel::namePlace(QVariantMap& place, int stid, const QString& stateName,
                             const QHash<int, QString>& countyNames) {
    return stid > 0 && apply_place_names(place, stid, stateName, countyNames);
}

int
SavedSystemsModel::refreshSites(const QList<SiteRefresh>& refreshes) {
    auto next = m_rows;
    int applied = 0;
    int first = -1;
    int last = -1;
    for (const SiteRefresh& refresh : refreshes) {
        const int row = rowForUid(refresh.uid);
        if (row < 0 || refresh.rrSid <= 0 || m_rows.at(row).rrSid != refresh.rrSid
            || m_rows.at(row).rrSiteId != refresh.rrSiteId) {
            continue;
        }
        applied++;
        // update()'s metadata path: the ids stay, so rename_generated_name() sees the row's own site.
        QVariantMap fields = refresh.fields;
        fields.insert(QStringLiteral("rrSid"), refresh.rrSid);
        fields.insert(QStringLiteral("rrSiteId"), refresh.rrSiteId);
        rename_generated_name(get(row), fields, &fields);
        Row updated = rowFromMap(fields, m_rows.at(row));
        fillPlaceNames(updated, m_rows.at(row).place);
        if (mapFromRow(updated) != mapFromRow(m_rows.at(row))) {
            next[row] = updated;
            first = first < 0 ? row : qMin(first, row);
            last = qMax(last, row);
        }
    }
    if (first < 0) {
        return applied;
    }
    if (!saveRows(next)) {
        return -1;
    }
    commitChangedRange(std::move(next), first, last, {});
    Q_EMIT sitesChanged();
    return applied;
}

QString
SavedSystemsModel::keyValueForUid(const QString& uid) const {
    const int row = rowForUid(uid);
    return row >= 0 ? m_rows.at(row).encKeyValue : QString();
}

void
SavedSystemsModel::setReplayStore(ReplayStore* replays) {
    m_replays = replays;
}

bool
SavedSystemsModel::adoptReplay(const QString& path) {
    if (path.isEmpty() || m_replays.isNull() || ReplayStore::adopt(path)) {
        return true;
    }
    // Saved naming a copy the manifest still calls pending, a later start could delete what this system uses.
    LOG_WARN("A replay copy could not be marked as used; the system was not saved.\n");
    return false;
}

QString
SavedSystemsModel::migratedReplay(const QString& uid, const QString& path) const {
    return m_replays.isNull() || path.isEmpty() ? path : m_replays->migratedPath(uid, path);
}

void
SavedSystemsModel::releaseReplayIfUnused(const QString& path) {
    if (path.isEmpty() || m_replays.isNull() || ReplayStore::names(filePaths(), path)) {
        return;
    }
    // A release that fails keeps the copy: a leak, never a loss.
    if (!ReplayStore::release(path)) {
        LOG_WARN("A replay copy no system uses could not be marked unused; it is kept.\n");
    }
}

QStringList
SavedSystemsModel::filePaths() const {
    QStringList paths;
    for (const Row& row : m_rows) {
        if (!row.filePath.isEmpty() && !paths.contains(row.filePath)) {
            paths.append(row.filePath);
        }
    }
    return paths;
}

bool
SavedSystemsModel::moveFilePaths(const QList<FilePathMove>& moves, QStringList* applied) {
    applied->clear();
    auto next = m_rows;
    QList<int> changed;
    QStringList used;
    for (const FilePathMove& move : moves) {
        const int row = rowForUid(move.uid);
        if (row < 0 || next.at(row).filePath != move.from) {
            continue; // edited, removed or reseeded since: the row stays as the user made it
        }
        next[row].filePath = move.to;
        changed.append(row);
        if (!used.contains(move.to)) {
            used.append(move.to);
        }
    }
    if (changed.isEmpty()) {
        return true;
    }
    if (!saveRows(next)) {
        return false;
    }
    m_rows = std::move(next);
    for (const int row : changed) {
        const QModelIndex idx = index(row);
        Q_EMIT dataChanged(idx, idx, {FilePathRole});
    }
    *applied = used;
    return true;
}

void
SavedSystemsModel::load() {
    QList<Row> rows;
    QSet<QString> seen;
    bool migrated = false;
    JsonStoreLoad status = JsonStoreLoad::Unreadable;
    const QJsonArray array = json_store_load_array(QLatin1String(kStoreFileName), &status);
    m_storeLoaded = status != JsonStoreLoad::Unreadable;
    // auto, not QJsonValue: iterating a QJsonArray yields a QJsonValueConstRef
    // proxy, and binding that to a QJsonValue reference converts — copying every
    // element into a temporary the loop then reads through.
    for (const auto& value : array) {
        if (!value.isObject()) {
            continue;
        }
        const QVariantMap stored = value.toObject().toVariantMap();
        Row row = rowFromMap(stored, Row());
        if (seen.contains(row.uid)) {
            row.uid = QUuid::createUuid().toString(QUuid::WithoutBraces);
        }
        seen.insert(row.uid);
        migrated |= stored.value(QStringLiteral("uid")).toString() != row.uid;
        migrated |= stored.contains(QStringLiteral("encKeyType"))
                    && stored.value(QStringLiteral("encKeyType")).typeId() != QMetaType::QString;
        // The P25 Simulcast chip used to carry "-f1 -mq", which pinned Phase 1 only;
        // -mq alone keeps the engine's default decode set, Auto, which includes both
        // Phase 1 and Phase 2.
        if (row.decodeFlag == QLatin1String("-f1 -mq")) {
            row.decodeFlag = QStringLiteral("-mq");
        }
        migrated |= normalizeLoaded(row);
        // A place validPlace() dropped is saved dropped, as the other repairs are.
        const QVariant storedPlace = stored.value(QStringLiteral("place"));
        migrated |= storedPlace.isValid() && row.place.isEmpty() && storedPlace != QVariant(QVariantMap());
        rows.append(row);
    }
    beginResetModel();
    m_rows = rows;
    endResetModel();
    if (migrated) {
        if (!save()) {
            LOG_WARN("Saved-system migration could not be persisted; identities may change on restart.\n");
        }
    }
    Q_EMIT sitesChanged();
    Q_EMIT countChanged();
}

bool
SavedSystemsModel::normalizeLoaded(Row& row) {
    if (row.rrSid > 0 && row.rrSiteId > 0) {
        return false;
    }
    // Outside a group, a site's avoid could never be lifted (the chooser that lifts it lists only a group's sites; a
    // lone RadioReference site keeps it, its "1 site" chooser opens), and its name and position are what older edits
    // left behind when they cleared only the ids. A place stays: a conventional multi-repeater row has one, no ids.
    constexpr double kCoordinateEpsilon = 1e-9;
    const bool orphan = row.avoidSite || !row.siteName.isEmpty() || row.hasSitePos
                        || std::abs(row.siteLat) > kCoordinateEpsilon || std::abs(row.siteLon) > kCoordinateEpsilon;
    row.avoidSite = false;
    row.siteName.clear();
    row.siteLat = 0;
    row.siteLon = 0;
    row.hasSitePos = false;
    return orphan;
}

bool
SavedSystemsModel::save() const {
    return saveRows(m_rows);
}

bool
SavedSystemsModel::saveRows(const QList<Row>& rows) const {
    QJsonArray array;
    for (const Row& row : rows) {
        auto stored = mapFromRow(row);
        stored.insert(QStringLiteral("encKeyValue"), row.encKeyValue);
        array.append(QJsonObject::fromVariantMap(stored));
    }
    return json_store_save_array(QLatin1String(kStoreFileName), array);
}

} // namespace dsd_qt
