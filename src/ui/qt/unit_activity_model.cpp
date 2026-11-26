// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "unit_activity_model.h"

#include <QByteArray>
#include <QDateTime>
#include <QDebug>
#include <QHash>
#include <algorithm>
#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/state_fwd.h>
#include <iterator>
#include <utility>

namespace dsd_qt {

namespace {

/* View rows read per call: a page of a few tens of KiB, read until the view has no more. */
constexpr int kPage = 64;

QString
time_text(qint64 wall_seconds) {
    return wall_seconds > 0 ? QDateTime::fromSecsSinceEpoch(wall_seconds).toString(QStringLiteral("HH:mm:ss"))
                            : QString();
}

} // namespace

UnitActivityModel::UnitActivityModel(QObject* parent)
    : QAbstractListModel(parent), m_page(new dsd_app_unit_activity_row[kPage]) {}

UnitActivityModel::~UnitActivityModel() = default;

int
UnitActivityModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(m_rows.size());
}

QVariant
UnitActivityModel::value(const Row& row, int role) {
    switch (role) {
        case SeqRole: return row.seq;
        case GapRole: return row.gap;
        case TimeTextRole: return time_text(row.wallSeconds);
        case HeadlineRole: return row.headline;
        case KindLabelRole: return row.kindLabel;
        case FiltersRole: return row.filters;
        case NameRole: return row.name;
        case SubjectRole: return row.subject;
        case TargetRole: return row.target;
        case ReasonRole: return row.reason;
        case ScopeRole: return row.scope;
        case RepeatRole: return row.repeat;
        case ProtocolRole: return row.protocol;
        case WarningRole: return row.warning;
        default: return {};
    }
}

QVariant
UnitActivityModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    return value(m_rows.at(index.row()), role);
}

QHash<int, QByteArray>
UnitActivityModel::roleNames() const {
    return {{SeqRole, QByteArrayLiteral("seq")},
            {GapRole, QByteArrayLiteral("gap")},
            {TimeTextRole, QByteArrayLiteral("timeText")},
            {HeadlineRole, QByteArrayLiteral("headline")},
            {KindLabelRole, QByteArrayLiteral("kindLabel")},
            {FiltersRole, QByteArrayLiteral("filters")},
            {NameRole, QByteArrayLiteral("name")},
            {SubjectRole, QByteArrayLiteral("subject")},
            {TargetRole, QByteArrayLiteral("target")},
            {ReasonRole, QByteArrayLiteral("reason")},
            {ScopeRole, QByteArrayLiteral("scope")},
            {RepeatRole, QByteArrayLiteral("repeat")},
            {ProtocolRole, QByteArrayLiteral("protocol")},
            {WarningRole, QByteArrayLiteral("warning")}};
}

UnitActivityModel::Row
UnitActivityModel::rowFrom(const dsd_app_unit_activity_row& in) {
    Row row;
    row.seq = in.seq;
    row.wallSeconds = in.wall_s;
    row.repeat = in.repeat;
    row.filters = in.filters;
    row.warning = in.tone == DSD_UA_TONE_WARNING;
    char headline[DSD_APP_UNIT_ACTIVITY_HEADLINE_SIZE];
    dsd_app_unit_activity_headline(&in, headline, sizeof headline);
    row.headline = QString::fromUtf8(headline);
    row.kindLabel = QString::fromUtf8(in.kind_label);
    row.name = QString::fromUtf8(in.subject_name);
    row.subject = QString::fromUtf8(in.subject_text);
    row.target = QString::fromUtf8(in.target_text);
    row.reason = QString::fromUtf8(in.reason_text);
    row.scope = QString::fromUtf8(in.scope_text);
    row.protocol = QString::fromUtf8(in.protocol_text);
    return row;
}

quint64
UnitActivityModel::orderKey(const Row& row) {
    return row.gap ? (row.gapAfter * 2U) + 1U : row.seq * 2U;
}

/* The first row whose order key is at most @p key: where a row of that key is, or goes. */
int
UnitActivityModel::positionFor(quint64 key) const {
    const auto it = std::lower_bound(m_rows.cbegin(), m_rows.cend(), key,
                                     [](const Row& row, quint64 wanted) { return orderKey(row) > wanted; });
    return static_cast<int>(it - m_rows.cbegin());
}

quint64
UnitActivityModel::newestSeq() const {
    const auto it = std::find_if(m_rows.cbegin(), m_rows.cend(), [](const Row& row) { return !row.gap; });
    return it == m_rows.cend() ? 0U : it->seq;
}

quint64
UnitActivityModel::oldestSeq() const {
    const auto it = std::find_if(m_rows.crbegin(), m_rows.crend(), [](const Row& row) { return !row.gap; });
    return it == m_rows.crend() ? 0U : it->seq;
}

int
UnitActivityModel::rowForSeq(qulonglong seq) const {
    if (seq == 0U) {
        const auto gap = std::find_if(m_rows.cbegin(), m_rows.cend(), [](const Row& row) { return row.gap; });
        return gap == m_rows.cend() ? -1 : static_cast<int>(gap - m_rows.cbegin());
    }
    const int at = positionFor(seq * 2U);
    return at < m_rows.size() && !m_rows.at(at).gap && m_rows.at(at).seq == seq ? at : -1;
}

void
UnitActivityModel::setCount(int held) {
    if (held != m_count) {
        m_count = held;
        Q_EMIT countChanged();
    }
}

void
UnitActivityModel::dropAll() {
    if (m_rows.isEmpty()) {
        return;
    }
    beginResetModel();
    m_rows.clear();
    endResetModel();
    setCount(0);
}

void
UnitActivityModel::clear() {
    dropAll();
    m_cursor = dsd_app_unit_activity_cursor{};
    m_nameKey = 0;
}

/* The one gap row, below the rows newer than @p after. A later loss moves it there, and the rows still below the old
 * gap go with it: rows were lost between them and the rows above the new gap too, and one gap could not say so. */
void
UnitActivityModel::placeGap(quint64 after) {
    const int held = rowForSeq(0);
    if (held >= 0 && m_rows.at(held).gapAfter == after) {
        return;
    }
    if (held >= 0) {
        const int last = static_cast<int>(m_rows.size()) - 1;
        beginRemoveRows(QModelIndex(), held, last);
        m_rows.remove(held, last - held + 1);
        endRemoveRows();
        setCount(m_count - (last - held));
    }
    Row gap;
    gap.gap = true;
    gap.gapAfter = after;
    const int at = positionFor(orderKey(gap));
    beginInsertRows(QModelIndex(), at, at);
    m_rows.insert(at, gap);
    endInsertRows();
}

/* The roles whose values differ between @p held and @p next, two reads of one row (its seq and gap never change).
 * Compared field by field: no text is formatted for a row that did not change. */
QList<int>
UnitActivityModel::changedRoles(const Row& held, const Row& next) {
    QList<int> changed;
    const auto note = [&changed](bool differs, int role) {
        if (differs) {
            changed.append(role);
        }
    };
    note(held.wallSeconds != next.wallSeconds, TimeTextRole);
    note(held.headline != next.headline, HeadlineRole);
    note(held.kindLabel != next.kindLabel, KindLabelRole);
    note(held.filters != next.filters, FiltersRole);
    note(held.name != next.name, NameRole);
    note(held.subject != next.subject, SubjectRole);
    note(held.target != next.target, TargetRole);
    note(held.reason != next.reason, ReasonRole);
    note(held.scope != next.scope, ScopeRole);
    note(held.repeat != next.repeat, RepeatRole);
    note(held.protocol != next.protocol, ProtocolRole);
    note(held.warning != next.warning, WarningRole);
    return changed;
}

/* Update the held row of @p in's seq in place; else, with @p insertNew, add it where its seq goes. A row older than
 * every held one is not added to a full list: the trim would drop it again at once. */
void
UnitActivityModel::upsert(const dsd_app_unit_activity_row& in, bool insertNew) {
    Row row = rowFrom(in);
    const int at = positionFor(orderKey(row));
    if (at < m_rows.size() && !m_rows.at(at).gap && m_rows.at(at).seq == row.seq) {
        const QList<int> changed = changedRoles(m_rows.at(at), row);
        if (!changed.isEmpty()) {
            m_rows[at] = std::move(row);
            const QModelIndex idx = index(at, 0);
            Q_EMIT dataChanged(idx, idx, changed);
        }
        return;
    }
    if (!insertNew || (m_count >= kCapacity && row.seq < oldestSeq())) {
        return;
    }
    beginInsertRows(QModelIndex(), at, at);
    m_rows.insert(at, std::move(row));
    endInsertRows();
    setCount(m_count + 1);
}

/* Down to kCapacity rows, the oldest going first; the gap row stays, at the bottom once the rows below it go. */
void
UnitActivityModel::trim() {
    while (m_count > kCapacity) {
        int end = static_cast<int>(m_rows.size()) - 1;
        if (m_rows.at(end).gap) {
            --end;
        }
        int start = end;
        while (start > 0 && end - start + 1 < m_count - kCapacity && !m_rows.at(start - 1).gap) {
            --start;
        }
        beginRemoveRows(QModelIndex(), start, end);
        m_rows.remove(start, end - start + 1);
        endRemoveRows();
        setCount(m_count - (end - start + 1));
    }
}

/* The rows revised after the cursor: a new log replaces the held rows, a loss places the gap above them. */
void
UnitActivityModel::readNew(const dsd_state* snapshot) {
    for (;;) {
        dsd_app_unit_activity_meta meta{};
        const int n = dsd_app_unit_activity_rows(snapshot, &m_cursor, m_page.get(), kPage, &meta);
        if (n < 0 || !meta.held) {
            break;
        }
        if (meta.reset) {
            dropAll();
        }
        if (meta.lost) {
            placeGap(newestSeq());
        }
        for (int i = 0; i < n; ++i) {
            upsert(m_page[i], true);
        }
        if (!meta.more || n == 0) {
            break;
        }
    }
    trim();
}

/* Every row the log holds, read again from its start to rename the held ones in place. The cursor stays: readNew()
 * has read up to the same snapshot, so this finds nothing new, and the loss it reports again is not a new one. */
void
UnitActivityModel::renameHeld(const dsd_state* snapshot) {
    dsd_app_unit_activity_cursor cursor{m_cursor.instance, 0};
    for (;;) {
        dsd_app_unit_activity_meta meta{};
        const int n = dsd_app_unit_activity_rows(snapshot, &cursor, m_page.get(), kPage, &meta);
        if (n <= 0 || !meta.held || meta.reset) {
            break;
        }
        for (int i = 0; i < n; ++i) {
            upsert(m_page[i], false);
        }
        if (!meta.more) {
            break;
        }
    }
}

void
UnitActivityModel::refresh(const dsd_state* snapshot) {
    if (snapshot == nullptr) {
        return;
    }
    const quint64 heldKey = m_nameKey;
    readNew(snapshot);
    m_nameKey = dsd_app_unit_activity_name_key(snapshot);
    if (heldKey != 0U && m_nameKey != heldKey && m_count > 0) {
        renameHeld(snapshot);
    }
}

UnitActivityFilterModel::UnitActivityFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    connect(this, &QAbstractItemModel::rowsInserted, this, &UnitActivityFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &UnitActivityFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &UnitActivityFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &UnitActivityFilterModel::countChanged);
}

void
UnitActivityFilterModel::setFilter(int chosen) {
    if (chosen == m_filter) {
        return;
    }
/* Qt 6.10's granular change protocol, retaining support for Qt 6.9. */
#if QT_VERSION >= 0x060a00
    beginFilterChange();
    m_filter = chosen;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    m_filter = chosen;
    invalidateRowsFilter();
#endif
    Q_EMIT filterChanged();
}

bool
UnitActivityFilterModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    const QAbstractItemModel* source = sourceModel();
    if (source == nullptr) {
        return false;
    }
    const QModelIndex idx = source->index(source_row, 0, source_parent);
    if (m_filter == 0 || source->data(idx, UnitActivityModel::GapRole).toBool()) {
        return true;
    }
    return (source->data(idx, UnitActivityModel::FiltersRole).toUInt() & static_cast<uint>(m_filter)) != 0U;
}

int
UnitActivityFilterModel::rowForSeq(qulonglong seq) const {
    const auto* source = qobject_cast<const UnitActivityModel*>(sourceModel());
    const int row = source != nullptr ? source->rowForSeq(seq) : -1;
    return row < 0 ? -1 : mapFromSource(source->index(row, 0)).row();
}

} // namespace dsd_qt
