// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The session's unit activity log for the Activity screen: the rows of the decoder's ring as the app-control
 * view (<dsd-neo/app_control/unit_activity_view.h>) resolves them, newest first, and the screen's filter over them.
 *
 * Refreshed from UiController's poll tick with its held snapshot, on the Qt main thread; never from another thread or
 * a second snapshot reader.
 */

#ifndef DSD_NEO_SRC_UI_QT_UNIT_ACTIVITY_MODEL_H_
#define DSD_NEO_SRC_UI_QT_UNIT_ACTIVITY_MODEL_H_

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QSortFilterProxyModel>
#include <QString>
#include <QVariant>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/app_control/unit_activity_view.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/unit_activity.h>
#include <memory>
#include <sys/types.h>

namespace dsd_qt {

/**
 * The rows of the unit activity log, newest first (by seq, which names a row for its whole life).
 *
 * A row read again (a message merged into it, or a name that changed) is updated in place, never moved, so a screen
 * reader's order holds: rows only arrive at the top and leave at the bottom. At most kCapacity rows are held (the
 * ring's own size), the oldest leaving first. When the ring dropped rows before the app read them (the app was in the
 * background on a busy site), one gap row ("Earlier activity was not kept") stands where they were, below the rows
 * read after it; a later loss moves that one gap row there and drops the rows still below it, so the list never shows
 * two. Names follow dsd_app_unit_activity_name_key(): when it changes, the held rows are read again and renamed in
 * place.
 */
class UnitActivityModel : public QAbstractListModel {
    Q_OBJECT
    /** Rows of the log held (the gap row not counted). */
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** The most rows held: the ring's size (DSD_UNIT_ACTIVITY_CAPACITY). */
    Q_PROPERTY(int capacity READ capacity CONSTANT)

  public:
    enum Role {
        SeqRole = Qt::UserRole + 1, /**< The row's seq; 0 on the gap row. */
        GapRole,                    /**< True on the gap row. */
        TimeTextRole,               /**< "HH:mm:ss" of the first message, local time; "" when unknown. */
        HeadlineRole,               /**< dsd_app_unit_activity_headline(): the kind label, with a deny's reason. */
        KindLabelRole,
        FiltersRole, /**< DSD_UA_FILTER_* bits. */
        NameRole,    /**< The subject's name, else "". */
        SubjectRole, /**< The subject as an ID ("1234567", "TG 100"). */
        TargetRole,
        ReasonRole,
        ScopeRole,
        RepeatRole, /**< How many messages the row holds (1 at first). */
        ProtocolRole,
        WarningRole, /**< An inhibit or kill command or its acknowledgement (DSD_UA_TONE_WARNING). */
    };

    enum { kCapacity = DSD_UNIT_ACTIVITY_CAPACITY };

    explicit UnitActivityModel(QObject* parent = nullptr);
    ~UnitActivityModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return m_count;
    }

    static int
    capacity() {
        return kCapacity;
    }

    /** @brief Read what @p snapshot's log holds after the rows read before; nothing for a NULL @p snapshot. */
    void refresh(const dsd_state* snapshot);
    /** @brief Drop every row and start over at the next log (a session that stopped or is starting). */
    void clear();
    /** @brief The row whose seq is @p seq (0: the gap row), or -1 when none is held. */
    Q_INVOKABLE int rowForSeq(qulonglong seq) const;

  Q_SIGNALS:
    void countChanged();

  private:
    struct Row {
        quint64 seq = 0;      /* 0 on the gap row */
        quint64 gapAfter = 0; /* the gap row: the newest seq held when the loss was found (it sits below newer rows) */
        bool gap = false;
        qint64 wallSeconds = 0;
        int repeat = 0;
        uint filters = 0;
        bool warning = false;
        QString headline, kindLabel, name, subject, target, reason, scope, protocol;
    };

    static Row rowFrom(const dsd_app_unit_activity_row& in);
    static QVariant value(const Row& row, int role);
    static QList<int> changedRoles(const Row& held, const Row& next);
    /* Twice the seq, or twice the gap's boundary plus one: rows are held in descending order of this. */
    static quint64 orderKey(const Row& row);
    int positionFor(quint64 key) const;
    quint64 newestSeq() const;
    quint64 oldestSeq() const;
    void dropAll();
    void placeGap(quint64 after);
    void upsert(const dsd_app_unit_activity_row& in, bool insertNew);
    void trim();
    void readNew(const dsd_state* snapshot);
    void renameHeld(const dsd_state* snapshot);
    void setCount(int held);

    QVector<Row> m_rows;
    int m_count = 0;
    dsd_app_unit_activity_cursor m_cursor{};
    quint64 m_nameKey = 0;
    /* One page of view rows, kept to spare the stack: each row is several hundred bytes. */
    std::unique_ptr<dsd_app_unit_activity_row[]> m_page;
};

/** The Activity screen's filter: every row (0), or the rows of one DSD_UA_FILTER_* bit. The gap row always passes. */
class UnitActivityFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(int filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)

  public:
    explicit UnitActivityFilterModel(QObject* parent = nullptr);

    int
    filter() const {
        return m_filter;
    }

    void setFilter(int chosen);

    int
    count() const {
        return rowCount();
    }

    /** @brief The view row of the source row whose seq is @p seq (0: the gap row), or -1 when it is not shown. */
    Q_INVOKABLE int rowForSeq(qulonglong seq) const;

  Q_SIGNALS:
    void filterChanged();
    void countChanged();

  protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;

  private:
    int m_filter = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_UNIT_ACTIVITY_MODEL_H_ */
