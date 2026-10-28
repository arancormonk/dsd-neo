// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QList>
#include <QMap>
#include <QVariantMap>
#include <utility>
#include "talkgroup_filter_model.h"

#include <QAbstractListModel>
#include <QByteArray>
#include <QVariant>
#include <Qt>

#include <dsd-neo/core/id_pattern.h>
#include <dsd-neo/core/parse.h>
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
    applyFilterChange([&]() {
        m_filterText = text;
        // A typed decimal ID also finds the pattern rows that match it.
        const QByteArray digits = text.trimmed().toLatin1();
        uint32_t id = 0;
        m_filterIdValid = !digits.isEmpty() && dsd_parse_uint32_strict(digits.constData(), 10, UINT32_MAX, &id) == 0
                          && digits.at(0) >= '0' && digits.at(0) <= '9';
        m_filterId = id;
    });
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
    if (m_filterText.isEmpty()
        || source->data(idx, TalkgroupListModel::NameRole).toString().contains(m_filterText, Qt::CaseInsensitive)
        || source->data(idx, TalkgroupListModel::IdTextRole).toString().contains(m_filterText)) {
        return true;
    }
    if (!m_filterIdValid) {
        return false;
    }
    const QByteArray pattern = source->data(idx, TalkgroupListModel::PatternRole).toString().toLatin1();
    dsd_id_pattern parsed;
    return !pattern.isEmpty() && dsd_id_pattern_parse(pattern.constData(), &parsed) == 0
           && dsd_id_pattern_match(&parsed, m_filterId) != 0;
}

QVariantList
TalkgroupFilterModel::snapshotSelection() const {
    QVariantList rows;
    for (int i = 0; i < rowCount(); ++i) {
        const auto at = index(i, 0);
        rows.append(QVariantMap{{"index", data(at, TalkgroupListModel::TableIndexRole)},
                                {"first", data(at, TalkgroupListModel::IdStartRole)},
                                {"last", data(at, TalkgroupListModel::IdEndRole)},
                                {"pattern", data(at, TalkgroupListModel::PatternRole)}});
    }
    return rows;
}

} // namespace dsd_qt
