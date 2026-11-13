// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "saved_system_cards.h"

#include <QByteArray>
#include <QDebug>
#include <QLatin1Char>
#include <QLatin1String>
#include <QMap>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <utility>

#include "app_prefs.h"
#include "frequency_query.h"
#include "imported_files_model.h"
#include "saved_systems_model.h"
#include "site_groups.h"
#include "system_search_query.h"
#include "text_order.h"

namespace dsd_qt {

namespace {

using Card = SavedSystemCards::Card;
using Member = SavedSystemCards::Member;

/* One search term: its folded text, and its reading as a frequency. */
struct Term {
    QString text;
    FrequencyQuery frequency;
};

QString
sort_name() {
    return QStringLiteral("name");
}

QString
sort_nearest() {
    return QStringLiteral("nearest");
}

/* The group's one site Play can start, or null when none or several are not avoided. */
const Member*
only_playable(const QList<const Member*>& group) {
    const Member* playable = nullptr;
    for (const Member* member : group) {
        if (member->avoidSite) {
            continue;
        }
        if (playable != nullptr) {
            return nullptr;
        }
        playable = member;
    }
    return playable;
}

/* What tells @p member's receiver apart where the card's meta does not already say it (Util.systemMeta() names a
   TCP or UDP source's address and a file's name): the dongle, an RTL-TCP server's address, an Airspy's serial. */
QString
receiver_text(const Member& member) {
    if (member.sourceType == QLatin1String("usb")) {
        return QStringLiteral("USB");
    }
    if (member.sourceType == QLatin1String("rtltcp")) {
        return QStringLiteral("RTL-TCP %1:%2").arg(member.host.trimmed(), QString::number(member.port));
    }
    if (member.sourceType == QLatin1String("airspy")) {
        const QString serial = member.airspySerial.trimmed().toUpper();
        return serial.isEmpty() ? QStringLiteral("Airspy") : QStringLiteral("Airspy %1").arg(serial);
    }
    return QString();
}

/* HomeScreen's needsSiteRefresh(): a trunked row in no site group whose channel or group file came from
   RadioReference could be grouped by refreshing it. */
bool
needs_site_refresh(const Member& member, const QHash<QString, bool>& rrFiles) {
    if (!member.trunking || site_grouped(member.rrSid, member.rrSiteId)) {
        return false;
    }
    return (!member.chanCsvPath.isEmpty() && rrFiles.value(member.chanCsvPath))
           || (!member.groupCsvPath.isEmpty() && rrFiles.value(member.groupCsvPath));
}

/* Each entry of a place list's id -> name, the first name an id is given kept. */
void
collect_named(const QVariant& list, const QString& idKey, QMap<int, QString>& into) {
    for (const QVariant& entry : list.toList()) {
        const QVariantMap map = entry.toMap();
        const int id = map.value(idKey).toInt();
        QString& name = into[id];
        if (name.isEmpty()) {
            name = map.value(QStringLiteral("name")).toString();
        }
    }
}

/* The names of @p ids that have one, each once, in name order. */
QStringList
named_sorted(const QMap<int, QString>& ids) {
    QStringList names;
    for (const QString& name : ids) {
        if (!name.isEmpty() && !names.contains(name)) {
            names.append(name);
        }
    }
    std::sort(names.begin(), names.end(),
              [](const QString& a, const QString& b) { return text_compare_names(a, b) < 0; });
    return names;
}

/* Each distinct name of @p names at its place in name order, in @p ranks; kept as it is while the names are the same
   ones (a rebuild for a lastHeard stamp folds and sorts nothing). Each name is folded once. */
void
rank_names(const QStringList& names, QHash<QString, int>& ranks) {
    const QSet<QString> distinct(names.cbegin(), names.cend());
    if (distinct.size() == ranks.size()
        && std::all_of(distinct.cbegin(), distinct.cend(),
                       [&ranks](const QString& name) { return ranks.contains(name); })) {
        return;
    }
    QList<QPair<QString, QString>> keyed; // folded, as written
    keyed.reserve(distinct.size());
    for (const QString& name : distinct) {
        keyed.append({text_fold(name), name});
    }
    std::sort(keyed.begin(), keyed.end(), [](const QPair<QString, QString>& a, const QPair<QString, QString>& b) {
        return text_compare_folded_names(a.first, a.second, b.first, b.second) < 0;
    });
    ranks.clear();
    for (int i = 0; i < keyed.size(); ++i) {
        ranks.insert(keyed.at(i).second, i);
    }
}

/* Whether two fixes name the same place: within 1e-7 degrees (about a centimetre), finer than any fix. */
bool
same_position(double lat, double lon, double otherLat, double otherLon) {
    constexpr double kDegrees = 1e-7;
    return std::fabs(lat - otherLat) <= kDegrees && std::fabs(lon - otherLon) <= kDegrees;
}

bool
by_name(const Card& a, const Card& b) {
    if (a.nameRank != b.nameRank) {
        return a.nameRank < b.nameRank;
    }
    return a.key < b.key;
}

bool
by_recent(const Card& a, const Card& b) {
    const bool aHeard = a.lastHeard > 0;
    if (aHeard != (b.lastHeard > 0)) {
        return aHeard;
    }
    if (a.lastHeard != b.lastHeard) {
        return a.lastHeard > b.lastHeard;
    }
    return by_name(a, b);
}

bool
by_added(const Card& a, const Card& b) {
    if (a.leadRow != b.leadRow) {
        return a.leadRow < b.leadRow;
    }
    return a.key < b.key;
}

/* Sections: one state by its name, then several states, then no place. Within a state's section: one county by its
   name, then several counties, then none; then the name. The other two sections are in name order. */
bool
by_place(const Card& a, const Card& b) {
    if (a.placeKind != b.placeKind) {
        return a.placeKind < b.placeKind;
    }
    if (a.placeKind != 0) {
        return by_name(a, b); // "Several states" and "No place" are in name order
    }
    if (a.stateRank != b.stateRank) {
        return a.stateRank < b.stateRank;
    }
    const auto tier = [](const Card& card) { return card.placeCounties == 1 ? 0 : (card.placeCounties > 1 ? 1 : 2); };
    if (tier(a) != tier(b)) {
        return tier(a) < tier(b);
    }
    if (a.countyRank != b.countyRank) {
        return a.countyRank < b.countyRank;
    }
    return by_name(a, b);
}

int
distance_order(const Card& card) {
    if (card.distanceState == QLatin1String("positioned")) {
        return 0;
    }
    if (card.distanceState == QLatin1String("allAvoided")) {
        return 1;
    }
    return card.distanceState == QLatin1String("noCoordinates") ? 2 : 3;
}

bool
by_distance(const Card& a, const Card& b) {
    const int aOrder = distance_order(a);
    const int bOrder = distance_order(b);
    if (aOrder != bOrder) {
        return aOrder < bOrder;
    }
    if (a.distanceKm < b.distanceKm) {
        return true;
    }
    if (b.distanceKm < a.distanceKm) {
        return false;
    }
    return by_name(a, b);
}

std::function<bool(const Card&, const Card&)>
order_for(const QString& sort) {
    if (sort == QLatin1String("name")) {
        return by_name;
    }
    if (sort == QLatin1String("recent")) {
        return by_recent;
    }
    if (sort == QLatin1String("place")) {
        return by_place;
    }
    return sort == QLatin1String("nearest") ? by_distance : by_added;
}

bool
term_matches(const Card& card, const Term& term) {
    if (term.text.isEmpty() || text_fold_contains(card.haystack, term.text)) {
        return true;
    }
    return term.frequency.valid && std::any_of(card.hz.cbegin(), card.hz.cend(), [&term](qint64 hz) {
               return frequency_query_matches(term.frequency, hz);
           });
}

/* The values of a set of distinct ints that form a longest increasing run of @p seq (kept in place by a reorder). */
QSet<int>
longest_increasing(const QList<int>& seq) {
    QList<int> tails; // index into seq of the smallest tail of each run length
    QList<int> previous(seq.size(), -1);
    for (int i = 0; i < seq.size(); ++i) {
        const auto at = std::lower_bound(tails.cbegin(), tails.cend(), seq.at(i),
                                         [&seq](int index, int value) { return seq.at(index) < value; });
        const qsizetype length = at - tails.cbegin();
        previous[i] = length > 0 ? tails.at(length - 1) : -1;
        if (length == tails.size()) {
            tails.append(i);
        } else {
            tails[length] = i;
        }
    }
    QSet<int> kept;
    for (int i = tails.isEmpty() ? -1 : tails.last(); i >= 0; i = previous.at(i)) {
        kept.insert(seq.at(i));
    }
    return kept;
}

/* Identity and display-row roles. */
QVariant
identity_role(const Card& card, int role) {
    switch (role) {
        case SavedSystemCards::KeyRole: return card.key;
        case SavedSystemCards::LeadUidRole: return card.leadUid;
        case SavedSystemCards::PlayUidRole: return card.playUid;
        case SavedSystemCards::TitleRole: return card.title;
        case SavedSystemCards::SiteCountRole: return card.siteCount;
        case SavedSystemCards::NameRole: return card.name;
        case SavedSystemCards::SourceTypeRole: return card.sourceType;
        case SavedSystemCards::HostRole: return card.host;
        case SavedSystemCards::PortRole: return card.port;
        case SavedSystemCards::FreqMhzRole: return card.freqMhz;
        case SavedSystemCards::DecodeFlagRole: return card.decodeFlag;
        case SavedSystemCards::TrunkingRole: return card.trunking;
        case SavedSystemCards::FilePathRole: return card.filePath;
        default: return QVariant();
    }
}

/* What the card says about the group, its place and its position in the list. */
QVariant
group_role(const Card& card, int role) {
    switch (role) {
        case SavedSystemCards::KeyConfiguredRole: return card.keyConfigured;
        case SavedSystemCards::LastHeardRole: return card.lastHeard;
        case SavedSystemCards::RefreshHintRole: return card.refreshHint;
        case SavedSystemCards::PlaceLabelRole: return card.placeLabel;
        case SavedSystemCards::DistanceKmRole: return card.distanceKm;
        case SavedSystemCards::DistanceStateRole: return card.distanceState;
        case SavedSystemCards::SectionRole: return card.section;
        case SavedSystemCards::SectionStartRole: return card.sectionStart;
        case SavedSystemCards::RrLinkedRole: return card.rrLinked;
        case SavedSystemCards::ReceiverLabelRole: return card.receiverLabel;
        default: return QVariant();
    }
}

QVariant
card_role(const Card& card, int role) {
    const QVariant identity = identity_role(card, role);
    return identity.isValid() ? identity : group_role(card, role);
}

QList<int>
changed_roles(const Card& before, const Card& after) {
    QList<int> roles;
    for (int role = SavedSystemCards::KeyRole; role <= SavedSystemCards::ReceiverLabelRole; ++role) {
        if (card_role(before, role) != card_role(after, role)) {
            roles.append(role);
        }
    }
    return roles;
}

/* Each section starts on its first card. A held order can show a section in two runs; only the first starts it, so a
   heading is never read twice. */
void
mark_section_starts(QList<Card>& cards) {
    QSet<QString> started;
    for (Card& card : cards) {
        card.sectionStart = !card.section.isEmpty() && !started.contains(card.section);
        started.insert(card.section);
    }
}

/* A card whose title another shown card has too (one system saved on two receivers) names its receiver. */
void
mark_shared_titles(QList<Card>& cards) {
    QHash<QString, int> titles;
    for (const Card& card : cards) {
        ++titles[card.title];
    }
    for (Card& card : cards) {
        card.receiverLabel = titles.value(card.title) > 1 ? card.receiver : QString();
    }
}

QHash<QString, int>
index_by_key(const QList<Card>& cards) {
    QHash<QString, int> index;
    index.reserve(cards.size());
    for (int i = 0; i < cards.size(); ++i) {
        index.insert(cards.at(i).key, i);
    }
    return index;
}

} // namespace

SavedSystemCards::SavedSystemCards(SavedSystemsModel* systems, AppPrefs* prefs, ImportedFilesModel* files,
                                   QObject* parent)
    : QAbstractListModel(parent), m_systems(systems), m_prefs(prefs), m_files(files) {
    const auto cardsChanged = [this]() {
        m_cardsDirty = true;
        scheduleRebuild();
    };
    if (m_systems != nullptr) {
        connect(m_systems, &QAbstractItemModel::rowsInserted, this, cardsChanged);
        connect(m_systems, &QAbstractItemModel::rowsRemoved, this, cardsChanged);
        connect(m_systems, &QAbstractItemModel::rowsMoved, this, cardsChanged);
        connect(m_systems, &QAbstractItemModel::modelReset, this, cardsChanged);
        connect(m_systems, &QAbstractItemModel::layoutChanged, this, cardsChanged);
        connect(m_systems, &QAbstractItemModel::dataChanged, this, cardsChanged);
        connect(m_systems, &SavedSystemsModel::sitesChanged, this, cardsChanged);
    }
    if (m_files != nullptr) {
        // Every library change, a file replaced in place included, may change a refresh hint.
        connect(m_files, &ImportedFilesModel::entriesChanged, this, cardsChanged);
    }
    if (m_prefs != nullptr) {
        connect(m_prefs, &AppPrefs::systemsSortChanged, this, [this]() {
            Q_EMIT sortIdChanged();
            m_orderDirty = true;
            m_userAsked = true;
            scheduleRebuild();
        });
        /* A new fix or one that expired: distances and possibly the order applied. Only the fix the user asked for
           (markLocationRequested()) is a user-asked change; an expiry or a fix another screen took is not. */
        connect(m_prefs, &AppPrefs::locationChanged, this, [this]() {
            m_cardsDirty = true;
            /* Only a change that brings a fix newer than the one at the mark answers it, whatever else is pending; an
               expiry (no fix) while the request runs leaves the mark for the answer. */
            if (m_locationRequested) {
                const qint64 at = m_prefs->lastFixAt();
                if (at > 0 && at > m_markedFixAt) {
                    m_locationRequested = false;
                    m_userAsked = true;
                }
            }
            scheduleRebuild();
        });
    }
    rebuild();
}

SavedSystemCards::~SavedSystemCards() = default;

int
SavedSystemCards::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_shown.size() + m_held.size());
}

QVariant
SavedSystemCards::data(const QModelIndex& index, int role) const {
    const qsizetype row = index.isValid() ? index.row() : -1;
    if (row < 0 || row >= m_shown.size() + m_held.size()) {
        return QVariant();
    }
    const bool held = row >= m_shown.size();
    if (role == HeldRole) {
        return held;
    }
    return card_role(held ? m_held.at(row - m_shown.size()) : m_shown.at(row), role);
}

QHash<int, QByteArray>
SavedSystemCards::roleNames() const {
    return {
        {KeyRole, QByteArrayLiteral("key")},
        {LeadUidRole, QByteArrayLiteral("leadUid")},
        {PlayUidRole, QByteArrayLiteral("playUid")},
        {TitleRole, QByteArrayLiteral("title")},
        {SiteCountRole, QByteArrayLiteral("siteCount")},
        {NameRole, QByteArrayLiteral("name")},
        {SourceTypeRole, QByteArrayLiteral("sourceType")},
        {HostRole, QByteArrayLiteral("host")},
        {PortRole, QByteArrayLiteral("port")},
        {FreqMhzRole, QByteArrayLiteral("freqMhz")},
        {DecodeFlagRole, QByteArrayLiteral("decodeFlag")},
        {TrunkingRole, QByteArrayLiteral("trunking")},
        {FilePathRole, QByteArrayLiteral("filePath")},
        {KeyConfiguredRole, QByteArrayLiteral("keyConfigured")},
        {LastHeardRole, QByteArrayLiteral("lastHeard")},
        {RefreshHintRole, QByteArrayLiteral("refreshHint")},
        {PlaceLabelRole, QByteArrayLiteral("placeLabel")},
        {DistanceKmRole, QByteArrayLiteral("distanceKm")},
        {DistanceStateRole, QByteArrayLiteral("distanceState")},
        {SectionRole, QByteArrayLiteral("section")},
        {SectionStartRole, QByteArrayLiteral("sectionStart")},
        {RrLinkedRole, QByteArrayLiteral("rrLinked")},
        {ReceiverLabelRole, QByteArrayLiteral("receiverLabel")},
        {HeldRole, QByteArrayLiteral("held")},
    };
}

void
SavedSystemCards::setQuery(const QString& text) {
    if (text == m_query) {
        return;
    }
    m_query = text;
    Q_EMIT queryChanged();
    m_userAsked = true;
    scheduleRebuild();
}

QString
SavedSystemCards::sortId() const {
    return m_prefs != nullptr ? m_prefs->systemsSort() : QStringLiteral("added");
}

void
SavedSystemCards::setScreenReaderActive(bool active) {
    if (active == m_screenReaderActive) {
        return;
    }
    m_screenReaderActive = active;
    Q_EMIT screenReaderActiveChanged();
    if (!active && (m_reorderHeld || !m_held.isEmpty())) {
        scheduleRebuild();
    }
}

void
SavedSystemCards::setHoldReorders(bool hold) {
    if (hold == m_holdReorders) {
        return;
    }
    m_holdReorders = hold;
    Q_EMIT holdReordersChanged();
    if (!hold && m_reorderHeld) {
        scheduleRebuild();
    }
}

void
SavedSystemCards::setProtocolLabels(const QVariantMap& labels) {
    if (labels == m_protocolLabels) {
        return;
    }
    m_protocolLabels = labels;
    Q_EMIT protocolLabelsChanged();
    m_cardsDirty = true;
    scheduleRebuild();
}

void
SavedSystemCards::setLocationSupported(bool supported) {
    if (supported == m_locationSupported) {
        return;
    }
    m_locationSupported = supported;
    m_cardsDirty = true;
    scheduleRebuild();
}

void
SavedSystemCards::markLocationRequested() {
    m_locationRequested = true;
    m_markedFixAt = m_prefs != nullptr ? m_prefs->lastFixAt() : 0;
}

void
SavedSystemCards::clearLocationRequest() {
    m_locationRequested = false;
}

int
SavedSystemCards::rowForKey(const QString& key) const {
    for (int i = 0; i < m_shown.size(); ++i) {
        if (m_shown.at(i).key == key) {
            return i;
        }
    }
    return -1;
}

void
SavedSystemCards::scheduleRebuild() {
    if (m_rebuildPending) {
        return;
    }
    m_rebuildPending = true;
    QTimer::singleShot(0, this, [this]() { rebuild(); });
}

void
SavedSystemCards::rebuild() {
    m_rebuildPending = false;
    ++m_rebuildCount;
    const bool userAsked = std::exchange(m_userAsked, false);
    const int shownBefore = shownCount();
    const int totalBefore = m_totalCount;
    const Fix fix = readFix();
    /* Not only locationChanged: AppPrefs drops a fix 24 wall-clock hours on at the first read, before its expiry timer
     * (monotonic, stopped while a phone sleeps) says so. When this read is that one, its locationChanged follows,
     * queued, after this rebuild. */
    if (fix.valid != m_fix.valid || fix.at != m_fix.at || !same_position(fix.lat, fix.lon, m_fix.lat, m_fix.lon)) {
        m_cardsDirty = true; // the distances were measured from another fix, or none
    }
    m_fix = fix;
    if (m_cardsDirty) {
        collectCards(fix);
        m_orderDirty = true;
    }
    if (m_orderDirty) {
        sortCards(fix);
    }
    m_cardsDirty = false;
    m_orderDirty = false;
    m_totalCount = static_cast<int>(m_all.size());
    publish(filteredCards(), userAsked);
    if (m_totalCount != totalBefore) {
        Q_EMIT totalCountChanged();
    }
    if (shownCount() != shownBefore) {
        Q_EMIT shownCountChanged();
    }
    Q_EMIT rebuilt();
}

QList<Member>
SavedSystemCards::readMembers() {
    QList<Member> members;
    const int rows = m_systems != nullptr ? m_systems->rowCount() : 0;
    members.reserve(rows);
    for (int row = 0; row < rows; ++row) {
        const QModelIndex at = m_systems->index(row);
        const auto text = [&at](int role) { return at.data(role).toString(); };
        Member member;
        member.row = row;
        member.uid = text(SavedSystemsModel::UidRole);
        member.groupKey = m_systems->groupKey(row);
        member.name = text(SavedSystemsModel::NameRole);
        member.siteName = text(SavedSystemsModel::SiteNameRole);
        member.sourceType = text(SavedSystemsModel::SourceTypeRole);
        member.host = text(SavedSystemsModel::HostRole);
        member.freqMhz = text(SavedSystemsModel::FreqMhzRole);
        member.decodeFlag = text(SavedSystemsModel::DecodeFlagRole);
        member.filePath = text(SavedSystemsModel::FilePathRole);
        member.airspySerial = at.data(SavedSystemsModel::AirspyRole).toMap().value(QStringLiteral("serial")).toString();
        member.chanCsvPath = text(SavedSystemsModel::ChanCsvPathRole);
        member.groupCsvPath = text(SavedSystemsModel::GroupCsvPathRole);
        member.place = at.data(SavedSystemsModel::PlaceRole).toMap();
        member.lastHeard = at.data(SavedSystemsModel::LastHeardRole).toLongLong();
        member.siteLat = at.data(SavedSystemsModel::SiteLatRole).toDouble();
        member.siteLon = at.data(SavedSystemsModel::SiteLonRole).toDouble();
        member.port = at.data(SavedSystemsModel::PortRole).toInt();
        member.rrSid = at.data(SavedSystemsModel::RrSidRole).toInt();
        member.rrSiteId = at.data(SavedSystemsModel::RrSiteIdRole).toInt();
        member.trunking = at.data(SavedSystemsModel::TrunkingRole).toBool();
        member.avoidSite = at.data(SavedSystemsModel::AvoidSiteRole).toBool();
        member.hasSitePos = at.data(SavedSystemsModel::HasSitePosRole).toBool();
        member.keyConfigured = at.data(SavedSystemsModel::KeyConfiguredRole).toBool();
        members.append(member);
    }
    return members;
}

QHash<QString, bool>
SavedSystemCards::radioReferenceFiles() const {
    QHash<QString, bool> files;
    const int rows = m_files != nullptr ? m_files->rowCount() : 0;
    for (int row = 0; row < rows; ++row) {
        const QModelIndex at = m_files->index(row);
        if (at.data(ImportedFilesModel::OriginRole).toString() == QLatin1String("radioreference")) {
            files.insert(at.data(ImportedFilesModel::PathRole).toString(), true);
        }
    }
    return files;
}

SavedSystemCards::Fix
SavedSystemCards::readFix() const {
    Fix fix;
    fix.at = m_locationSupported && m_prefs != nullptr ? m_prefs->lastFixAt() : 0;
    fix.valid = fix.at > 0;
    if (fix.valid) {
        fix.lat = m_prefs->lastLat();
        fix.lon = m_prefs->lastLon();
    }
    return fix;
}

void
SavedSystemCards::collectCards(const Fix& fix) {
    const QList<Member> members = readMembers();
    QHash<QString, int> cardOf;
    QList<QList<const Member*>> groups;
    for (const Member& member : members) {
        const QString key = member.groupKey.isEmpty() ? member.uid : member.groupKey;
        const auto found = cardOf.constFind(key);
        if (found == cardOf.cend()) {
            cardOf.insert(key, static_cast<int>(groups.size()));
            groups.append({&member});
        } else {
            groups[*found].append(&member);
        }
    }
    const QHash<QString, bool> rrFiles = radioReferenceFiles();
    QHash<QString, RowText> texts;
    QList<Card> cards;
    cards.reserve(groups.size());
    QStringList titles;
    QStringList places;
    for (const QList<const Member*>& group : groups) {
        Card card = buildCard(group, rrFiles, fix);
        QStringList lines;
        QSet<QString> flags;
        for (const Member* member : group) {
            const RowText text = memberText(*member, texts);
            lines.append(text.folded);
            card.hz.append(text.hz);
            flags.insert(member->decodeFlag);
        }
        for (const QString& flag : flags) {
            const QString label = m_protocolLabels.value(flag, m_protocolLabels.value(QString())).toString();
            if (!label.isEmpty()) {
                lines.append(system_search_text(label));
            }
        }
        card.haystack = lines.join(QLatin1Char('\n'));
        titles.append(card.title);
        places.append(card.placeState);
        places.append(card.placeCounty);
        cards.append(card);
    }
    m_rowTexts = std::move(texts);
    rank_names(titles, m_nameRanks);
    rank_names(places, m_placeRanks);
    for (Card& card : cards) {
        card.nameRank = m_nameRanks.value(card.title);
        card.stateRank = m_placeRanks.value(card.placeState);
        card.countyRank = m_placeRanks.value(card.placeCounty);
    }
    m_all = std::move(cards);
}

SavedSystemCards::RowText
SavedSystemCards::memberText(const Member& member, QHash<QString, RowText>& next) const {
    const auto cached = m_rowTexts.constFind(member.uid);
    if (cached != m_rowTexts.cend() && cached->name == member.name && cached->siteName == member.siteName
        && cached->freqMhz == member.freqMhz && cached->place == member.place) {
        next.insert(member.uid, *cached);
        return *cached;
    }
    QStringList parts{system_search_text(member.name), system_search_text(member.siteName)};
    for (const QString& list : {QStringLiteral("states"), QStringLiteral("counties")}) {
        for (const QVariant& entry : member.place.value(list).toList()) {
            parts.append(system_search_text(entry.toMap().value(QStringLiteral("name")).toString()));
        }
    }
    RowText text;
    text.name = member.name;
    text.siteName = member.siteName;
    text.freqMhz = member.freqMhz;
    text.place = member.place;
    text.folded = parts.join(QLatin1Char('\n'));
    text.hz = system_search_hz(member.freqMhz);
    next.insert(member.uid, text);
    return text;
}

SavedSystemCards::Card
SavedSystemCards::buildCard(const QList<const Member*>& group, const QHash<QString, bool>& rrFiles,
                            const Fix& fix) const {
    const Member& lead = *group.first();
    const bool grouped = !lead.groupKey.isEmpty();
    // Play starts the one site not avoided, or the row itself in no group; the card describes that row.
    const Member* play = grouped ? only_playable(group) : &lead;
    const Member& shown = play != nullptr ? *play : lead;
    Card card;
    card.key = grouped ? lead.groupKey : lead.uid;
    card.leadUid = lead.uid;
    card.leadRow = lead.row;
    card.playUid = play != nullptr ? play->uid : QString();
    card.title = site_group_title({{QStringLiteral("name"), lead.name}, {QStringLiteral("siteName"), lead.siteName}});
    card.siteCount = grouped ? static_cast<int>(group.size()) : 0;
    card.name = shown.name;
    card.sourceType = shown.sourceType;
    card.host = shown.host;
    card.port = shown.port;
    card.freqMhz = shown.freqMhz;
    card.decodeFlag = shown.decodeFlag;
    card.trunking = shown.trunking;
    card.filePath = shown.filePath;
    card.receiver = receiver_text(shown);
    card.keyConfigured = shown.keyConfigured;
    card.refreshHint = needs_site_refresh(shown, rrFiles);
    for (const Member* member : group) {
        card.lastHeard = std::max(card.lastHeard, member->lastHeard);
        card.rrLinked = card.rrLinked || site_grouped(member->rrSid, member->rrSiteId) || !member->place.isEmpty();
    }
    placeCard(card, group);
    locateCard(card, group, fix);
    return card;
}

void
SavedSystemCards::placeCard(Card& card, const QList<const Member*>& group) const {
    QMap<int, QString> states;
    QMap<int, QString> counties;
    for (const Member* member : group) {
        collect_named(member->place.value(QStringLiteral("states")), QStringLiteral("stid"), states);
        collect_named(member->place.value(QStringLiteral("counties")), QStringLiteral("ctid"), counties);
    }
    const QStringList stateNames = named_sorted(states);
    const QStringList countyNames = named_sorted(counties);
    card.placeCounties = static_cast<int>(countyNames.size());
    if (stateNames.isEmpty()) {
        card.placeKind = 2;
        return;
    }
    if (stateNames.size() > 1) {
        card.placeKind = 1;
        card.placeLabel = stateNames.join(QStringLiteral(", "));
        return;
    }
    card.placeKind = 0;
    card.placeState = stateNames.first();
    if (countyNames.size() == 1) {
        card.placeCounty = countyNames.first();
        card.placeLabel = tr("%1, %2").arg(card.placeCounty, card.placeState);
    } else if (countyNames.isEmpty()) {
        card.placeLabel = card.placeState;
    } else {
        card.placeLabel = tr("%1 · %2 counties").arg(card.placeState, QString::number(countyNames.size()));
    }
}

void
SavedSystemCards::locateCard(Card& card, const QList<const Member*>& group, const Fix& fix) {
    if (!fix.valid) {
        return;
    }
    bool playable = false;
    bool positioned = false;
    double nearest = 0;
    for (const Member* member : group) {
        if (member->avoidSite) {
            continue;
        }
        playable = true;
        const double distance =
            member->hasSitePos ? site_distance_km(fix.lat, fix.lon, member->siteLat, member->siteLon) : -1;
        if (distance >= 0 && (!positioned || distance < nearest)) {
            nearest = distance;
            positioned = true;
        }
    }
    if (positioned) {
        card.distanceKm = nearest;
        card.distanceState = QStringLiteral("positioned");
    } else {
        card.distanceState = playable ? QStringLiteral("noCoordinates") : QStringLiteral("allAvoided");
    }
}

QString
SavedSystemCards::applicableSort(const Fix& fix) const {
    QString stored = sortId();
    if (stored == sort_nearest() && !fix.valid) {
        return sort_name();
    }
    return stored;
}

void
SavedSystemCards::sortCards(const Fix& fix) {
    const QString sort = applicableSort(fix);
    for (Card& card : m_all) {
        if (sort == QLatin1String("place")) {
            card.section =
                card.placeKind == 0 ? card.placeState : (card.placeKind == 1 ? tr("Several states") : tr("No place"));
        } else if (sort == sort_nearest()) {
            card.section = card.distanceState == QLatin1String("allAvoided")      ? tr("All sites avoided")
                           : card.distanceState == QLatin1String("noCoordinates") ? tr("No coordinates")
                                                                                  : QString();
        } else {
            card.section.clear();
        }
    }
    m_order.resize(m_all.size());
    std::iota(m_order.begin(), m_order.end(), 0);
    const auto before = order_for(sort);
    std::sort(m_order.begin(), m_order.end(),
              [this, &before](int a, int b) { return before(m_all.at(a), m_all.at(b)); });
    if (sort != m_effectiveSort) {
        m_effectiveSort = sort;
        Q_EMIT effectiveSortChanged();
    }
}

QList<Card>
SavedSystemCards::filteredCards() const {
    QList<Term> terms;
    for (const QString& term : system_search_terms(m_query)) {
        terms.append(Term{system_search_text(term), system_search_frequency(term)});
    }
    QList<Card> shown;
    shown.reserve(m_order.size());
    for (const int index : m_order) {
        const Card& card = m_all.at(index);
        if (std::all_of(terms.cbegin(), terms.cend(), [&card](const Term& term) { return term_matches(card, term); })) {
            shown.append(card);
        }
    }
    return shown;
}

void
SavedSystemCards::publish(QList<Card>&& next, bool userAsked) {
    /* With a screen reader on screen, a reorder nobody asked for waits: the cards shown keep their places. */
    const bool hold = m_screenReaderActive && m_holdReorders && !userAsked && reordersShown(next);
    if (hold) {
        next = heldOrder(std::move(next));
    }
    m_reorderHeld = hold;
    mark_section_starts(next);
    mark_shared_titles(next);
    if (m_screenReaderActive) {
        applyPositional(std::move(next));
        return;
    }
    releaseHeldRows();
    applyDiff(std::move(next));
}

/* Whether @p next puts two cards shown now in the other order. */
bool
SavedSystemCards::reordersShown(const QList<Card>& next) const {
    const QHash<QString, int> nextIndex = index_by_key(next);
    int last = -1;
    for (const Card& card : m_shown) {
        const int at = nextIndex.value(card.key, -1);
        if (at >= 0) {
            if (at < last) {
                return true;
            }
            last = at;
        }
    }
    return false;
}

/* The cards still shown keep their places; a new one goes after the card it follows in @p next. */
QList<Card>
SavedSystemCards::heldOrder(QList<Card>&& next) const {
    const QHash<QString, int> nextIndex = index_by_key(next);
    QList<Card> held;
    held.reserve(next.size());
    QSet<QString> heldKeys;
    for (const Card& card : m_shown) {
        const int at = nextIndex.value(card.key, -1);
        if (at >= 0) {
            held.append(next.at(at));
            heldKeys.insert(card.key);
        }
    }
    for (int i = 0; i < next.size(); ++i) {
        const Card& card = next.at(i);
        if (heldKeys.contains(card.key)) {
            continue;
        }
        int after = -1;
        for (int j = 0; i > 0 && j < held.size(); ++j) {
            if (held.at(j).key == next.at(i - 1).key) {
                after = j;
                break;
            }
        }
        held.insert(after + 1, card);
    }
    return held;
}

/* Row i shows next[i]: one dataChanged per row whose card or held state changed. The rows past the end of @p next keep
   the cards they showed, held; rows are added only past the end of every row there is. */
void
SavedSystemCards::applyPositional(QList<Card>&& next) {
    const qsizetype live = m_shown.size();
    QList<Card> before = std::move(m_shown);
    before.append(std::move(m_held));
    const qsizetype rows = before.size();
    const qsizetype count = next.size();
    const qsizetype kept = std::min(count, rows);
    QList<Card> added = next.mid(kept);
    next.resize(kept);
    m_shown = std::move(next);
    m_held = before.mid(kept);
    for (qsizetype row = 0; row < kept; ++row) {
        QList<int> roles = changed_roles(before.at(row), m_shown.at(row));
        if (row >= live) {
            roles.append(HeldRole);
        }
        if (!roles.isEmpty()) {
            Q_EMIT dataChanged(index(static_cast<int>(row)), index(static_cast<int>(row)), roles);
        }
    }
    if (count < live) {
        Q_EMIT dataChanged(index(static_cast<int>(count)), index(static_cast<int>(live - 1)), {HeldRole});
    }
    if (!added.isEmpty()) {
        beginInsertRows(QModelIndex(), static_cast<int>(rows), static_cast<int>(count - 1));
        m_shown.append(std::move(added));
        endInsertRows();
    }
}

/* Without a screen reader no row is held: the held ones go. */
void
SavedSystemCards::releaseHeldRows() {
    if (m_held.isEmpty()) {
        return;
    }
    const int first = static_cast<int>(m_shown.size());
    beginRemoveRows(QModelIndex(), first, first + static_cast<int>(m_held.size()) - 1);
    m_held.clear();
    endRemoveRows();
}

void
SavedSystemCards::applyDiff(QList<Card>&& next) {
    const QHash<QString, int> nextIndex = index_by_key(next);
    removeGone(nextIndex);
    moveSurvivors(nextIndex);
    insertNew(next);
    updateContent(next);
}

void
SavedSystemCards::removeGone(const QHash<QString, int>& nextIndex) {
    for (int last = static_cast<int>(m_shown.size()) - 1; last >= 0;) {
        if (nextIndex.contains(m_shown.at(last).key)) {
            --last;
            continue;
        }
        int first = last;
        while (first > 0 && !nextIndex.contains(m_shown.at(first - 1).key)) {
            --first;
        }
        beginRemoveRows(QModelIndex(), first, last);
        m_shown.remove(first, last - first + 1);
        endRemoveRows();
        last = first - 1;
    }
}

/* Every shown card is in @p nextIndex now. The longest run already in order stays; each other card moves to just
   after the card it follows in the new order, which leaves every card in place. */
void
SavedSystemCards::moveSurvivors(const QHash<QString, int>& nextIndex) {
    QList<int> seq;
    seq.reserve(m_shown.size());
    for (const Card& card : m_shown) {
        seq.append(nextIndex.value(card.key));
    }
    const QSet<int> stays = longest_increasing(seq);
    QList<int> target = seq;
    std::sort(target.begin(), target.end());
    for (int k = 0; k < target.size(); ++k) {
        if (stays.contains(target.at(k))) {
            continue;
        }
        const int from = static_cast<int>(seq.indexOf(target.at(k)));
        const int to = k == 0 ? 0 : static_cast<int>(seq.indexOf(target.at(k - 1))) + 1;
        if (from == to) {
            continue;
        }
        beginMoveRows(QModelIndex(), from, from, QModelIndex(), to);
        const int landing = from < to ? to - 1 : to;
        // Taken and inserted, not QList::move(): that rotates element by element, these move a Card as bytes.
        m_shown.insert(landing, m_shown.takeAt(from));
        seq.move(from, landing);
        endMoveRows();
    }
}

void
SavedSystemCards::insertNew(const QList<Card>& next) {
    QSet<QString> present;
    for (const Card& card : m_shown) {
        present.insert(card.key);
    }
    for (int first = 0; first < next.size();) {
        if (present.contains(next.at(first).key)) {
            ++first;
            continue;
        }
        int end = first;
        while (end < next.size() && !present.contains(next.at(end).key)) {
            ++end;
        }
        beginInsertRows(QModelIndex(), first, end - 1);
        for (int i = first; i < end; ++i) {
            m_shown.insert(i, next.at(i));
        }
        endInsertRows();
        first = end;
    }
}

void
SavedSystemCards::updateContent(const QList<Card>& next) {
    for (int i = 0; i < next.size(); ++i) {
        const QList<int> roles = changed_roles(m_shown.at(i), next.at(i));
        m_shown[i] = next.at(i);
        if (!roles.isEmpty()) {
            Q_EMIT dataChanged(index(i), index(i), roles);
        }
    }
}

} // namespace dsd_qt
