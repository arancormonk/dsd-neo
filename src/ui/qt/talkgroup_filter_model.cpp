// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "talkgroup_filter_model.h"

#include <QAbstractListModel>
#include <QVariant>
#include <Qt>

#include "talkgroup_list_model.h"

namespace dsd_qt {

TalkgroupFilterModel::TalkgroupFilterModel(QObject* parent) : QSortFilterProxyModel(parent) {
    connect(this, &QAbstractItemModel::rowsInserted, this, &TalkgroupFilterModel::countChanged);
    connect(this, &QAbstractItemModel::rowsRemoved, this, &TalkgroupFilterModel::countChanged);
    connect(this, &QAbstractItemModel::modelReset, this, &TalkgroupFilterModel::countChanged);
}

void
TalkgroupFilterModel::setFilterText(const QString& text) {
    if (text == m_filterText) {
        return;
    }
    applyFilterChange([&]() { m_filterText = text; });
    Q_EMIT filterChanged();
}

void
TalkgroupFilterModel::setFilterTag(const QString& tag) {
    if (tag == m_filterTag) {
        return;
    }
    applyFilterChange([&]() { m_filterTag = tag; });
    Q_EMIT filterChanged();
}

bool
TalkgroupFilterModel::filterAcceptsRow(int source_row, const QModelIndex& source_parent) const {
    const QAbstractItemModel* source = sourceModel();
    if (source == nullptr) {
        return false;
    }
    const QModelIndex idx = source->index(source_row, 0, source_parent);
    if (!m_filterTag.isEmpty() && source->data(idx, TalkgroupListModel::TagsRole).toString() != m_filterTag) {
        return false;
    }
    return m_filterText.isEmpty()
           || source->data(idx, TalkgroupListModel::NameRole).toString().contains(m_filterText, Qt::CaseInsensitive)
           || source->data(idx, TalkgroupListModel::IdTextRole).toString().contains(m_filterText);
}

} // namespace dsd_qt
