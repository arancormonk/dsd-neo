// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_QT_TALKGROUP_FILTER_MODEL_H_
#define DSD_NEO_SRC_UI_QT_TALKGROUP_FILTER_MODEL_H_

#include <QObject>
#include <QSortFilterProxyModel>
#include <QString>
#include <QtGlobal>

namespace dsd_qt {

class TalkgroupFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(QString filterTag READ filterTag WRITE setFilterTag NOTIFY filterChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    explicit TalkgroupFilterModel(QObject* parent = nullptr);

    int
    count() const {
        return rowCount();
    }

    QString
    filterText() const {
        return m_filterText;
    }

    QString
    filterTag() const {
        return m_filterTag;
    }

    void setFilterText(const QString& text);
    void setFilterTag(const QString& tag);

  Q_SIGNALS:
    void countChanged();
    void filterChanged();

  protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    template <typename Mutate>
    void
    applyFilterChange(Mutate&& mutate) {
/* Qt 6.10's granular change protocol, retaining support for Qt 6.9. */
#if QT_VERSION >= 0x060a00
        beginFilterChange();
        mutate();
        endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
        mutate();
        invalidateRowsFilter();
#endif
    }

    QString m_filterText;
    QString m_filterTag;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_TALKGROUP_FILTER_MODEL_H_ */
