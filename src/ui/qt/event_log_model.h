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

namespace dsd_qt {

class EventLogModel : public QAbstractListModel {
    Q_OBJECT

  public:
    enum Roles { TextRole = Qt::UserRole + 1, SlotRole, SeverityRole };

    explicit EventLogModel(QObject* parent = nullptr);
    ~EventLogModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    /** @brief Re-read the snapshot ring. Call from the UI poll tick only. */
    void refresh();

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
