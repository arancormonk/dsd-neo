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
 *
 * This model is the store alone: every view that shows it binds through its own
 * CallHistoryFilterModel, so one screen's search or pills never filter another.
 */

#ifndef DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_
#define DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QTimer>

#include <dsd-neo/core/state_fwd.h>

namespace dsd_qt {

class CallHistoryModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString sessionLabel READ sessionLabel WRITE setSessionLabel NOTIFY sessionLabelChanged)
    Q_PROPERTY(QStringList systemLabels READ systemLabels NOTIFY countChanged)

  public:
    enum Roles {
        NameRole = Qt::UserRole + 1,
        TgRole,
        SrcRole,
        EncRole,
        WhenRole,         // call start, seconds since epoch
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
        return static_cast<int>(m_rows.size());
    }

    /**
     * @brief Which saved system the current session is on; stamped onto new rows.
     *
     * Persisted: the Android service outlives the Activity, and a relaunched UI
     * ingests the running session's backlog before any start button is pressed —
     * those rows must carry the system that produced them, not an empty string.
     */
    QString
    sessionLabel() const {
        return m_sessionLabel;
    }

    void setSessionLabel(const QString& label);

    /** @brief Distinct system names present in the log, for the filter pill. */
    QStringList systemLabels() const;

    /**
     * @brief Ingest newly committed voice rows from the snapshot ring.
     *
     * Call from the UI poll tick only. Duplicate protection is by content key, not
     * ring position: the ring shifts on every push, so positions mean nothing.
     * Updates are granular (insert/change/remove), never a model reset — a reset
     * would destroy every delegate and the reader's scroll position per ingest.
     */
    void refresh(const dsd_state* snapshot);

    Q_INVOKABLE void clearAll();

  Q_SIGNALS:
    void countChanged();
    void sessionLabelChanged();

  public:
    /** @brief One logged call. Public only so file-local helpers can build one. */
    struct Row {
        qint64 when = 0;
        QString name;
        qulonglong tg = 0;
        qulonglong src = 0;
        bool enc = false;
        int durationSecs = -1;
        QString systemName;
    };

  private:
    static QString keyFor(const Row& row);

    /** @brief Scan the ring for committed voice rows not seen before. */
    QList<Row> collectFresh(const dsd_state* snapshot);

    /**
     * @brief Absorb @p row into a recent same-target row when the two overlap
     *        within the merge window; the merged row spans both fragments.
     * @return Index of the row it merged into, or -1 when it is a new call.
     */
    int tryMerge(const Row& row);

    /**
     * @brief Merge @p row into the log or insert it at its sorted position.
     * @return true when a new row was inserted (the count changed).
     */
    bool ingestRow(const Row& row);

    void load();
    void scheduleSave();
    void saveNow() const;

    QList<Row> m_rows; // newest first
    QSet<QString> m_seen;
    QString m_sessionLabel;
    /* Rows starting at or before this stamp were cleared by the user; persisted so
     * the still-populated ring cannot resurrect them after an Activity restart. */
    qint64 m_clearedThrough = 0;
    quint64 m_revision[2] = {0U, 0U};
    bool m_seeded = false;
    QTimer m_saveTimer;
    QSettings m_settings;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_CALL_HISTORY_MODEL_H_ */
