// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Structured, persistent call log behind the history screen and the
 *        monitor's recent-calls panel.
 *
 * Rows are ingested from the published snapshot's event-history ring — committed
 * voice rows only, keyed so a ring that shifts under us never double-counts — and
 * persisted as JSON so the log survives sessions and process death, which the
 * in-memory event ring deliberately does not. Refreshed from the single UI poll
 * tick only.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QString>
#include <QTimer>

#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

class CallHistoryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY sessionLabelChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(QString filterSystem READ filterSystem WRITE setFilterSystem NOTIFY filterChanged)
    Q_PROPERTY(int filterKind READ filterKind WRITE setFilterKind NOTIFY filterChanged)
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)

  public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        TgRole,
        SrcRole,
        EncRole,
        WhenRole,         // seconds since epoch
        DurationSecsRole, // -1 when unknown
        SystemNameRole,
        DayLabelRole, // "TODAY" / "YESTERDAY" / "MON 3 AUG" — drives list sections
        TimeTextRole  // "12:04"
    };

    explicit CallHistoryModel(QObject* parent = nullptr);
    ~CallHistoryModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return static_cast<int>(m_visible.size());
    }

    /** @brief Which saved system the current session is on; stamped onto new rows. */
    QString
    sessionLabel() const {
        return m_sessionLabel;
    }

    void setSessionLabel(const QString& label);

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

    /** @brief Call-type filter: 0 = all calls, 1 = clear only, 2 = encrypted only. */
    int
    filterKind() const {
        return m_filterKind;
    }

    void setFilterKind(int kind);

    /** @brief Distinct system names present in the log, for the filter pill. */
    QStringList systemLabels() const;

    /**
     * @brief Ingest newly committed voice rows from the snapshot ring.
     *
     * Call from the UI poll tick only. Duplicate protection is by content key, not
     * ring position: the ring shifts on every push, so positions mean nothing.
     */
    void refresh(const dsd_state* snapshot);

    Q_INVOKABLE void clearAll();

  Q_SIGNALS:
    void countChanged();
    void sessionLabelChanged();
    void filterChanged();

  private:
    struct Row {
        qint64 when = 0;
        QString name;
        qulonglong tg = 0;
        qulonglong src = 0;
        bool enc = false;
        int durationSecs = -1;
        QString systemName;
    };

    static QString keyFor(const Row& row);

    void load();
    void scheduleSave();
    void saveNow() const;
    QString storePath() const;
    void rebuildVisible();
    bool rowVisible(const Row& row) const;

    QList<Row> m_rows; // newest first
    QList<int> m_visible;
    QSet<QString> m_seen;
    QString m_sessionLabel;
    QString m_filterText;
    QString m_filterSystem;
    int m_filterKind = 0;
    quint64 m_revision[2] = {0U, 0U};
    bool m_seeded = false;
    QTimer m_saveTimer;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_ */
