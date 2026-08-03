// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Scrolling event log backed by the published state snapshot.
 *
 * Rows come from the snapshot's event-history ring, never the live decoder state,
 * and are rebuilt only when a slot's revision moves. Refreshed from the single UI
 * poll tick.
 */

#ifndef DSD_NEO_SRC_UI_QT_EVENT_LOG_MODEL_H_
#define DSD_NEO_SRC_UI_QT_EVENT_LOG_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QString>

#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

class EventLogModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    enum Roles { TextRole = Qt::UserRole + 1, SlotRole, SeverityRole };

    explicit EventLogModel(QObject* parent = nullptr);
    ~EventLogModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** @brief Row count for QML; rowCount() cannot be a property, it takes an index. */
    int
    count() const {
        return static_cast<int>(m_rows.size());
    }

    /**
     * @brief Re-read the snapshot ring. Call from the UI poll tick only.
     *
     * Takes the snapshot rather than fetching it, so that the whole frame is built
     * from the generation the caller consumed. See MetricsModel::refresh().
     *
     * @param snapshot State snapshot, or nullptr before the first publish.
     */
    void refresh(const dsd_state* snapshot);

    /**
     * @brief Drop every row, keeping the revision guard.
     *
     * The revisions deliberately survive: dsd_app_get_latest_snapshot() keeps handing
     * back the finished run's buffer after teardown, so a reset guard would let the
     * rows we just dropped repopulate on the very next tick. A genuinely new run
     * allocates its own history and moves the revisions, which refills the list.
     */
    void clear();

  Q_SIGNALS:
    void countChanged();

  private:
    struct Row {
        QString text;
        int slot = 0;
        int severity = 0;
    };

    QList<Row> m_rows;
    quint64 m_revision[2] = {0U, 0U};
    bool m_seeded = false;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_EVENT_LOG_MODEL_H_ */
