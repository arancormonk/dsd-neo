// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Per-view filter over the shared call-history store.
 *
 * The store (CallHistoryModel) is one process-wide log; each screen that shows it
 * instantiates its own filter, so the history tab's search and pills cannot
 * silently filter the monitor's recent-calls pane — the bug this class exists to
 * prevent. Filter changes re-evaluate with granular row signals, never a reset.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_FILTER_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_FILTER_H_

#include <QObject>
#include <QSortFilterProxyModel>
#include <QString>
#include <QtGlobal>

namespace dsd_qt {

class CallHistoryFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(QString filterSystem READ filterSystem WRITE setFilterSystem NOTIFY filterChanged)
    Q_PROPERTY(int filterKind READ filterKind WRITE setFilterKind NOTIFY filterChanged)
    Q_PROPERTY(qlonglong historySession READ historySession WRITE setHistorySession NOTIFY filterChanged)

  public:
    explicit CallHistoryFilterModel(QObject* parent = nullptr);
    ~CallHistoryFilterModel() override;

    int
    count() const {
        return rowCount();
    }

    QString
    filterText() const {
        return m_filterText;
    }

    void setFilterText(const QString& text);

    QString
    filterSystem() const {
        return m_filterSystem;
    }

    void setFilterSystem(const QString& system);

    /** @brief Row-type filter: 0 = everything, 1 = clear calls, 2 = encrypted calls, 3 = messages/notices. */
    int
    filterKind() const {
        return m_filterKind;
    }

    void setFilterKind(int kind);

    /**
     * @brief The decode session whose rows the view shows (CallHistoryModel::session()); 0 = every row.
     *
     * The monitor's recent-calls pane sets this to the running session, so a fresh
     * session does not open onto days of persisted history as though it were live.
     * It selects by the session that logged a row, not by the row's stamps. A replay's
     * calls are stamped with the capture's time, and a start-time bound would hide them.
     */
    qlonglong
    historySession() const {
        return m_historySession;
    }

    void setHistorySession(qlonglong session);

  Q_SIGNALS:
    void countChanged();
    void filterChanged();

  protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    /**
     * @brief Mutate a filter member inside the proxy's change protocol.
     *
     * Qt 6.10 replaced invalidateRowsFilter() with the begin/endFilterChange
     * pair; the tree supports Qt 6.9, so both spellings live behind this helper.
     */
    template <typename Mutate>
    void
    applyFilterChange(Mutate&& mutate) {
/* 0x060a00 = Qt 6.10.0, spelled numerically because cppcheck cannot expand the
 * function-like QT_VERSION_CHECK macro in a preprocessor condition. */
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
    QString m_filterSystem;
    int m_filterKind = 0;
    qlonglong m_historySession = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_FILTER_H_ */
