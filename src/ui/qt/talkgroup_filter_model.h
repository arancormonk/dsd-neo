// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_UI_QT_TALKGROUP_FILTER_MODEL_H_
#define DSD_NEO_SRC_UI_QT_TALKGROUP_FILTER_MODEL_H_

#include <QObject>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariantList>
#include <QtGlobal>
#include <stdint.h>

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
    Q_INVOKABLE QVariantList snapshotSelection() const;
    /**
     * @brief The view row that lists the range @p idStart to @p idEnd, or -1 when none does.
     *
     * For the Talkgroups screen, which lets its model go while hidden (issue #782) and puts the reader back on the
     * card they had at the top. Both bounds, since an exact row and a range can share a start. One pass over the
     * filtered rows, once per show.
     */
    Q_INVOKABLE int rowForTalkgroup(qulonglong idStart, qulonglong idEnd) const;
    /**
     * @brief The view row of the card the Talkgroups screen kept its place on, or -1 when none lists it.
     *
     * A card is its bounds, its pattern and its table index, the token its edits carry: pattern rows all have the
     * bounds 0-0, and a table can list the same bounds twice. A row whose bounds and pattern match but whose index
     * moved (a row added or removed above it) is the next best, so the reader is put back near where they were.
     */
    Q_INVOKABLE int rowForCard(qulonglong idStart, qulonglong idEnd, const QString& pattern, int tableIndex) const;

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
    bool m_filterIdValid = false; /* The filter text is a decimal ID (m_filterId) that pattern rows are matched with. */
    uint32_t m_filterId = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_TALKGROUP_FILTER_MODEL_H_ */
