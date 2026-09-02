// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "call_history_filter.h"

#include <QAbstractListModel>
#include <QDebug>
#include <QList>
#include <QVariant>
#include <Qt>

#include "call_history_model.h"

namespace dsd_qt {

namespace {

/** @brief Whether a row survives the row-type pill (all / clear / encrypted / messages). */
bool
kind_accepts(int filterKind, bool voice, bool enc) {
    switch (filterKind) {
        case 1: return voice && !enc;
        case 2: return voice && enc;
        case 3: return !voice;
        default: return true;
    }
}

/** @brief Case-insensitive search over identity, payload text and the scan channel heard on. */
bool
text_matches(const QAbstractItemModel* source, const QModelIndex& idx, const QString& text) {
    return source->data(idx, CallHistoryModel::NameRole).toString().contains(text, Qt::CaseInsensitive)
           || source->data(idx, CallHistoryModel::TgRole).toString().contains(text)
           || source->data(idx, CallHistoryModel::SrcRole).toString().contains(text)
           || source->data(idx, CallHistoryModel::DetailRole).toString().contains(text, Qt::CaseInsensitive)
           || source->data(idx, CallHistoryModel::ChannelRole).toString().contains(text, Qt::CaseInsensitive);
}

} // namespace

CallHistoryFilterModel::CallHistoryFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    // count is a convenience for QML empty states; keep it live across every way
    // the mapped row set can move.
    connect(this, &QAbstractItemModel::rowsInserted, this, &CallHistoryFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &CallHistoryFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &CallHistoryFilterModel::countChanged);
    connect(this, &QAbstractItemModel::layoutChanged, this, &CallHistoryFilterModel::countChanged);
}

CallHistoryFilterModel::~CallHistoryFilterModel() = default;

void
CallHistoryFilterModel::setFilterText(const QString& text) {
    if (text == m_filterText) {
        return;
    }
    applyFilterChange([&]() { m_filterText = text; });
    Q_EMIT filterChanged();
}

void
CallHistoryFilterModel::setFilterSystem(const QString& system) {
    if (system == m_filterSystem) {
        return;
    }
    applyFilterChange([&]() { m_filterSystem = system; });
    Q_EMIT filterChanged();
}

void
CallHistoryFilterModel::setFilterKind(int kind) {
    if (kind == m_filterKind) {
        return;
    }
    applyFilterChange([&]() { m_filterKind = kind; });
    Q_EMIT filterChanged();
}

void
CallHistoryFilterModel::setMinWhen(qlonglong when) {
    if (when == m_minWhen) {
        return;
    }
    applyFilterChange([&]() { m_minWhen = when; });
    Q_EMIT filterChanged();
}

bool
CallHistoryFilterModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    const QAbstractItemModel* source = sourceModel();
    if (source == nullptr) {
        return false;
    }
    const QModelIndex idx = source->index(source_row, 0, source_parent);
    if (m_minWhen > 0 && source->data(idx, CallHistoryModel::WhenRole).toLongLong() < m_minWhen) {
        return false;
    }
    if (!m_filterSystem.isEmpty() && source->data(idx, CallHistoryModel::SystemNameRole).toString() != m_filterSystem) {
        return false;
    }
    const bool enc = source->data(idx, CallHistoryModel::EncRole).toBool();
    const bool voice = source->data(idx, CallHistoryModel::KindRole).toInt() == CallHistoryModel::KindVoice;
    if (!kind_accepts(m_filterKind, voice, enc)) {
        return false;
    }
    return m_filterText.isEmpty() || text_matches(source, idx, m_filterText);
}

} // namespace dsd_qt
