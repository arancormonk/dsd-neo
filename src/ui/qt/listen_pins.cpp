// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "listen_pins.h"

#include <QAbstractItemModel>
#include <QDebug>
#include <QLatin1Char>
#include <QLatin1String>
#include <QMap>
#include <QSet>
#include <QVariant>
#include <algorithm>
#include <iterator>
#include <utility>
#include "saved_systems_model.h"
#include "scan_lists_model.h"

namespace dsd_qt {

namespace {
const QLatin1String kSystem("system");
const QLatin1String kSite("site");
const QLatin1String kScan("scan");
const QLatin1String kSearch("search");
const QLatin1String kExplore("explore");

QString
pin_key(const QString& kind, const QString& uid) {
    return kind + QLatin1Char(':') + uid;
}

QVariantMap
stored_pin(const QString& kind, const QString& uid) {
    return {{QStringLiteral("kind"), kind}, {QStringLiteral("uid"), uid}};
}

/* Signal search and Explore are one of each: their pins carry no UID. */
bool
is_singleton(const QString& kind) {
    return kind == kSearch || kind == kExplore;
}

/* The UID of a site of @p row's group outside @p removed, or empty when every one is going. */
QString
remaining_sibling(const SavedSystemsModel& systems, int row, const QSet<int>& removed) {
    const QVariantList siblings = systems.siblingRows(row);
    const auto heir = std::find_if(siblings.cbegin(), siblings.cend(),
                                   [&removed](const QVariant& sibling) { return !removed.contains(sibling.toInt()); });
    return heir != siblings.cend() ? systems.get(heir->toInt()).value(QStringLiteral("uid")).toString() : QString();
}

/* The RadioReference system @p row is grouped under, as site_groups.cpp groups sites, or 0 for a row in no group. */
int
group_of(const SavedSystemsModel& systems, int row) {
    const QModelIndex at = systems.index(row);
    const int sid = systems.data(at, SavedSystemsModel::RrSidRole).toInt();
    return sid > 0 && systems.data(at, SavedSystemsModel::RrSiteIdRole).toInt() > 0 ? sid : 0;
}

/* The UID of the first site grouped under @p sid, the one its Home card stands on, or empty when none is. */
QString
group_lead(const SavedSystemsModel& systems, int sid) {
    for (int row = 0; row < systems.count(); ++row) {
        if (group_of(systems, row) == sid) {
            return systems.data(systems.index(row), SavedSystemsModel::UidRole).toString();
        }
    }
    return QString();
}

QVariantMap
named(const QString& label, const QString& accessibleName) {
    return {{QStringLiteral("label"), label},
            {QStringLiteral("accessibleName"), accessibleName},
            {QStringLiteral("found"), true}};
}
} // namespace

QString
ListenPins::text(Text id, const QString& label, const QString& detail) {
    switch (id) {
        case Text::FullRefusal: return tr("Pinned is full (%1). Unpin one first.").arg(kCap);
        case Text::NotFoundRefusal: return tr("Not found. It may have been removed.");
        case Text::SiteLabel: return tr("%1 · %2").arg(label, detail);
        case Text::SearchLabel: return tr("Signal search");
        case Text::ExploreLabel: return tr("Explore");
        case Text::StartName: return tr("Start %1").arg(label);
        case Text::ChooseSiteName: return tr("Choose a site of %1").arg(label);
        case Text::StartScanListName: return tr("Start scan list %1").arg(label);
        case Text::OpenSearchName: return tr("Open Signal search");
        case Text::StartExploreName: return tr("Start Explore");
        case Text::SetUpExploreName: return tr("Set up Explore");
    }
    return QString();
}

ListenPins::ListenPins(AppPrefs* prefs, SavedSystemsModel* systems, ScanListsModel* lists, QObject* parent)
    : QObject(parent), m_prefs(prefs), m_systems(systems), m_lists(lists) {
    if (m_prefs != nullptr) {
        connect(m_prefs, &AppPrefs::pinnedChanged, this, [this]() { refresh(false); });
        // Explore's pin says whether a tap starts it or asks for its radio first.
        connect(m_prefs, &AppPrefs::exploreChanged, this, [this]() { refresh(false); });
    }
    for (QAbstractItemModel* model :
         {static_cast<QAbstractItemModel*>(systems), static_cast<QAbstractItemModel*>(lists)}) {
        if (model == nullptr) {
            continue;
        }
        connect(model, &QAbstractItemModel::modelReset, this, [this]() { refresh(true); });
        connect(model, &QAbstractItemModel::dataChanged, this, [this]() { modelChanged(); });
        connect(model, &QAbstractItemModel::rowsInserted, this, [this]() { refresh(true); });
        connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() { rowsRemoved(); });
    }
    /* Both models signal rowsAboutToBeRemoved only for a removal they have saved, so a pin is pruned only when its
       target will not come back; a store that loads empty prunes nothing. What stays is worked out while the rows are
       still there and stored once they are gone, so nothing this object signals lands inside the removal. */
    if (m_systems != nullptr) {
        connect(m_systems, &SavedSystemsModel::sitesChanged, this, [this]() { modelChanged(); });
        connect(m_systems, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex&, int first, int last) { holdPrune(withoutSystems(first, last)); });
    }
    if (m_lists != nullptr) {
        connect(m_lists, &QAbstractItemModel::rowsAboutToBeRemoved, this,
                [this](const QModelIndex&, int first, int last) { holdPrune(withoutLists(first, last)); });
    }
    refresh(false);
}

ListenPins::~ListenPins() = default;

QVariantList
ListenPins::entries() const {
    return m_entries;
}

int
ListenPins::count() const {
    return static_cast<int>(m_entries.size());
}

bool
ListenPins::full() const {
    return count() >= kCap;
}

void
ListenPins::refresh(bool topology) {
    QVariantList next;
    if (m_prefs != nullptr) {
        for (const QVariant& stored : m_prefs->pinned()) {
            next.append(resolve(stored.toMap()));
        }
    }
    const int oldCount = count();
    const bool oldFull = full();
    const bool changed = next != m_entries;
    if (changed) {
        m_entries = next;
        Q_EMIT entriesChanged();
    }
    if (count() != oldCount) {
        Q_EMIT countChanged();
    }
    if (full() != oldFull) {
        Q_EMIT fullChanged();
    }
    if (changed || topology) {
        ++m_revision;
        Q_EMIT revisionChanged();
    }
    rememberGroups();
}

/* Each saved row's group as this refresh saw it, so the next change can tell a row that left its group. */
void
ListenPins::rememberGroups() {
    m_groupOf.clear();
    for (int row = 0; m_systems != nullptr && row < m_systems->count(); ++row) {
        m_groupOf.insert(m_systems->data(m_systems->index(row), SavedSystemsModel::UidRole).toString(),
                         group_of(*m_systems, row));
    }
}

void
ListenPins::modelChanged() {
    followLeavers();
    refresh(true);
}

/* A system pin is the system, not the row it names. When that row leaves its group (an edit of its frequency, decode
   flag or a CSV field, or the removal of a library file it names) and the group still has sites, the pin moves to the
   group's lead, as a removal moves it; with none left it stays with the row. */
void
ListenPins::followLeavers() {
    if (m_prefs == nullptr || m_systems == nullptr) {
        return;
    }
    QVariantList pins = m_prefs->pinned();
    bool moved = false;
    for (QVariant& value : pins) {
        QVariantMap stored = value.toMap();
        const QString uid = stored.value(QStringLiteral("uid")).toString();
        const int was = m_groupOf.value(uid, 0);
        const int row = m_systems->rowForUid(uid);
        if (stored.value(QStringLiteral("kind")).toString() != kSystem || was <= 0 || row < 0
            || group_of(*m_systems, row) == was) {
            continue;
        }
        const QString lead = group_lead(*m_systems, was);
        if (!lead.isEmpty()) {
            stored.insert(QStringLiteral("uid"), lead);
            value = stored;
            moved = true;
        }
    }
    if (moved) {
        store(pins);
    }
}

QVariantMap
ListenPins::resolve(const QVariantMap& stored) {
    const QString kind = stored.value(QStringLiteral("kind")).toString();
    const QString uid = stored.value(QStringLiteral("uid")).toString();
    const QString key = pin_key(kind, uid);
    QVariantMap entry = kind == kScan      ? resolveScan(uid)
                        : kind == kSearch  ? named(text(Text::SearchLabel), text(Text::OpenSearchName))
                        : kind == kExplore ? resolveExplore()
                                           : resolveSaved(kind, uid);
    if (entry.isEmpty()) {
        entry = {{QStringLiteral("label"), m_lastLabels.value(key)},
                 {QStringLiteral("accessibleName"), QString()},
                 {QStringLiteral("found"), false}};
    } else {
        m_lastLabels.insert(key, entry.value(QStringLiteral("label")).toString());
    }
    entry.insert(QStringLiteral("key"), key);
    entry.insert(QStringLiteral("kind"), kind);
    entry.insert(QStringLiteral("uid"), uid);
    return entry;
}

/* A system is named by its group's lead, the row its Home card stands on, and starts as that card does: the one site
   it can play, else the site chooser. A site is its system's name and its own. Empty when the row is not saved. */
QVariantMap
ListenPins::resolveSaved(const QString& kind, const QString& uid) const {
    const int row = m_systems != nullptr ? m_systems->rowForUid(uid) : -1;
    if (row < 0) {
        return {};
    }
    if (kind == kSite) {
        const QVariantMap site = m_systems->get(row);
        const QString name = site.value(QStringLiteral("name")).toString();
        const QString siteName = site.value(QStringLiteral("siteName")).toString();
        const QString label = siteName.isEmpty() ? name : text(Text::SiteLabel, name, siteName);
        return named(label, text(Text::StartName, label));
    }
    const QVariantList siblings = m_systems->siblingRows(row);
    const int lead = siblings.isEmpty() ? row : siblings.first().toInt();
    const QString label = m_systems->get(lead).value(QStringLiteral("name")).toString();
    const auto playable = std::count_if(siblings.cbegin(), siblings.cend(), [this](const QVariant& sibling) {
        return !m_systems->get(sibling.toInt()).value(QStringLiteral("avoidSite")).toBool();
    });
    const bool chooses = !siblings.isEmpty() && playable != 1;
    return named(label, text(chooses ? Text::ChooseSiteName : Text::StartName, label));
}

QVariantMap
ListenPins::resolveScan(const QString& uid) const {
    const int row = m_lists != nullptr ? m_lists->rowForUid(uid) : -1;
    if (row < 0) {
        return {};
    }
    const QString label = m_lists->get(row).value(QStringLiteral("name")).toString();
    return named(label, text(Text::StartScanListName, label));
}

QVariantMap
ListenPins::resolveExplore() const {
    // As Home's Explore card reads it: a radio chosen means a tap starts, none means it opens the setup.
    const bool configured = m_prefs != nullptr && !m_prefs->exploreSourceType().isEmpty();
    return named(text(Text::ExploreLabel), text(configured ? Text::StartExploreName : Text::SetUpExploreName));
}

bool
ListenPins::exists(const QString& kind, const QString& uid) const {
    if (is_singleton(kind)) {
        return true;
    }
    if (kind == kScan) {
        return m_lists != nullptr && m_lists->rowForUid(uid) >= 0;
    }
    return (kind == kSystem || kind == kSite) && m_systems != nullptr && m_systems->rowForUid(uid) >= 0;
}

/* The UIDs a system pin can name for the card of @p uid: the row's own and each of its group's. *standalone says the
   row is in no group, where its card is the site itself. */
QStringList
ListenPins::groupUids(const QString& uid, bool* standalone) const {
    const int row = m_systems != nullptr ? m_systems->rowForUid(uid) : -1;
    const QVariantList siblings = row >= 0 ? m_systems->siblingRows(row) : QVariantList();
    *standalone = siblings.isEmpty();
    QStringList uids{uid};
    for (const QVariant& sibling : siblings) {
        uids.append(m_systems->get(sibling.toInt()).value(QStringLiteral("uid")).toString());
    }
    return uids;
}

/* The positions in @p pins that isPinned() matches for @p kind and @p uid. */
QList<int>
ListenPins::matching(const QVariantList& pins, const QString& kind, const QString& uid) const {
    bool standalone = false;
    const QStringList members = kind == kSystem ? groupUids(uid, &standalone) : QStringList();
    QList<int> hits;
    for (int i = 0; i < pins.size(); ++i) {
        const QVariantMap stored = pins.at(i).toMap();
        const QString pinKind = stored.value(QStringLiteral("kind")).toString();
        const QString pinUid = stored.value(QStringLiteral("uid")).toString();
        const bool hit = kind == kSystem ? (pinKind == kSystem && members.contains(pinUid))
                                               || (standalone && pinKind == kSite && pinUid == uid)
                                         : pinKind == kind && (is_singleton(kind) || pinUid == uid);
        if (hit) {
            hits.append(i);
        }
    }
    return hits;
}

bool
ListenPins::isPinned(const QString& kind, const QString& uid) const {
    return !matching(m_entries, kind, uid).isEmpty();
}

QString
ListenPins::pin(const QString& kind, const QString& uid) {
    if (isPinned(kind, uid)) {
        return QString();
    }
    if (m_prefs == nullptr || !exists(kind, uid)) {
        return text(Text::NotFoundRefusal);
    }
    // Pins whose targets are missing count: they are never dropped behind the user's back.
    QVariantList pins = m_prefs->pinned();
    if (pins.size() >= kCap) {
        return text(Text::FullRefusal);
    }
    pins.append(stored_pin(kind, is_singleton(kind) ? QString() : uid));
    store(pins);
    // A UID the stored form cannot hold (a scan list's past its bound) is left out by the preference.
    return isPinned(kind, uid) ? QString() : text(Text::NotFoundRefusal);
}

void
ListenPins::unpin(const QString& kind, const QString& uid) {
    if (m_prefs == nullptr) {
        return;
    }
    QVariantList pins = m_prefs->pinned();
    const QList<int> hits = matching(pins, kind, uid);
    for (auto it = hits.crbegin(); it != hits.crend(); ++it) {
        pins.removeAt(*it);
    }
    store(pins);
}

void
ListenPins::unpinKey(const QString& key) {
    if (m_prefs == nullptr) {
        return;
    }
    QVariantList kept;
    for (const QVariant& value : m_prefs->pinned()) {
        const QVariantMap stored = value.toMap();
        if (pin_key(stored.value(QStringLiteral("kind")).toString(), stored.value(QStringLiteral("uid")).toString())
            != key) {
            kept.append(stored);
        }
    }
    store(kept);
}

/* The pins once rows @p first to @p last of the saved systems are gone. A site's pin goes with its row; a system's
   moves to a site of its group that stays, so the card stays pinned, else it goes. */
QVariantList
ListenPins::withoutSystems(int first, int last) const {
    if (m_prefs == nullptr || m_systems == nullptr) {
        return {};
    }
    QSet<int> removed;
    for (int row = first; row <= last; ++row) {
        removed.insert(row);
    }
    QVariantList kept;
    for (const QVariant& value : m_prefs->pinned()) {
        QVariantMap stored = value.toMap();
        const QString kind = stored.value(QStringLiteral("kind")).toString();
        const int row = kind == kSystem || kind == kSite
                            ? m_systems->rowForUid(stored.value(QStringLiteral("uid")).toString())
                            : -1;
        if (!removed.contains(row)) {
            kept.append(stored);
            continue;
        }
        const QString heir = kind == kSystem ? remaining_sibling(*m_systems, row, removed) : QString();
        if (!heir.isEmpty()) {
            stored.insert(QStringLiteral("uid"), heir);
            kept.append(stored);
        }
    }
    // A pin moved onto a site its group already had pinned is a repeat, which the preference keeps once.
    return kept;
}

/* The pins once rows @p first to @p last of the scan lists are gone. */
QVariantList
ListenPins::withoutLists(int first, int last) const {
    if (m_prefs == nullptr || m_lists == nullptr) {
        return {};
    }
    QSet<QString> removed;
    for (int row = first; row <= last; ++row) {
        removed.insert(m_lists->get(row).value(QStringLiteral("uid")).toString());
    }
    QVariantList kept;
    for (const QVariant& value : m_prefs->pinned()) {
        const QVariantMap stored = value.toMap();
        if (stored.value(QStringLiteral("kind")).toString() != kScan
            || !removed.contains(stored.value(QStringLiteral("uid")).toString())) {
            kept.append(stored);
        }
    }
    return kept;
}

void
ListenPins::holdPrune(const QVariantList& kept) {
    m_pendingPins = kept;
    m_prunePending = m_prefs != nullptr;
}

void
ListenPins::rowsRemoved() {
    if (m_prunePending) {
        m_prunePending = false;
        store(m_pendingPins);
    }
    modelChanged();
}

/* Through the live preference, so its pinnedChanged reaches refresh() and every view; unchanged pins write nothing. */
void
ListenPins::store(const QVariantList& pins) {
    m_prefs->setPinned(pins);
}

} // namespace dsd_qt
