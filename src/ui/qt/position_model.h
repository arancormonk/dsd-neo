// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The map's tracks (issue #800): the verified positions the decoder published, read through the position view
 *        (<dsd-neo/app_control/position_view.h>) and kept for the app's life, one row per radio.
 *
 * Polled from UiController's tick on the Qt main thread (before its no-redraw early return, so positions arrive with
 * no redraw, and once more at Stop), and by PositionTap while the app is in the background. Never cleared at a session
 * edge (decision D4): a Stop keeps the tracks, a new run adds to them.
 */

#ifndef DSD_NEO_SRC_UI_QT_POSITION_MODEL_H_
#define DSD_NEO_SRC_UI_QT_POSITION_MODEL_H_

#include <QAbstractListModel>
#include <QHash>
#include <QList>
#include <QObject>
#include <QSortFilterProxyModel>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <Qt>
#include <QtGlobal>
#include <dsd-neo/core/position.h>
#include <memory>

#include "position_tap.h"

class QByteArray;

namespace dsd_qt {

/**
 * The tracks, newest first by when each first appeared; a row is updated in place, never moved, so a screen reader's
 * order holds. A track is the report's typed source (the protocol family, the sender scope with its identity
 * discriminator, the radio ID or callsign: decision D7). A report with no checked source (hashed or unverified, or one
 * that names none) is an anonymous row of its own: one point, no trail, no name.
 *
 * Trails: at most kPointsPerTrack points, oldest first. A point whose position may be old or invalid (DSD_POS_FLAG_
 * VALIDITY_UNKNOWN, decision D11) never extends a trail and never moves the track's current position: it is held apart
 * as the track's newest unknown fix (a hollow marker). A trail breaks (Point::breakBefore) wherever reports may be
 * missing or a run changed: no line is drawn across it. At most kTrackCapacity rows (anonymous ones included) are held,
 * and the one whose newest report came first leaves first, named or not (spec 4.5); anonymous rows are held to
 * kAnonymousCapacity of them, the oldest anonymous one leaving for a new one past that share.
 *
 * Losses: when the view lost reports, the background queue dropped some, or a run ended before its last reports were
 * read, one gap row ("positions may be missing from" timeText) stands where the loss was found, above the tracks that
 * existed then; a later loss moves it.
 *
 * Ages (seconds, -1 when not known): a fix heard on the system clock ages against the real wall clock, whatever the
 * decoder's clock does. A fix heard on the replay clock ages only while the view's clock sample names its run, says
 * REPLAY and running: sample.decode_now_s - rx_time_s (it stops while a replay is paused, and never mixes another
 * clock). A stopped replay's fixes have no age; their receive time stays (timeText). Ages are read at each poll for the
 * rows it changed, for every row when the sample's run, clock or running flag changes, and once a second while ageing
 * is set (the map sets it while visible). The age filter shows a fix whose age is known and at most ageFilterSeconds
 * (0: every fix); a fix with no age is older than any filter.
 */
class PositionModel : public QAbstractListModel {
    Q_OBJECT
    /** Tracks held (the gap row not counted). */
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    /** The most tracks held. */
    Q_PROPERTY(int capacity READ capacity CONSTANT)
    /** The most points a trail holds. */
    Q_PROPERTY(int pointsPerTrack READ pointsPerTrack CONSTANT)
    /** Seconds: fixes older (or with no age) are not shown; 0 shows every fix. */
    Q_PROPERTY(int ageFilterSeconds READ ageFilterSeconds WRITE setAgeFilterSeconds NOTIFY ageFilterSecondsChanged)
    /** The map is visible: ages are read again once a second. */
    Q_PROPERTY(bool ageing READ ageing WRITE setAgeing NOTIFY ageingChanged)

  public:
    /* kAnonymousCapacity: anonymous rows (hashed, unverified or sourceless points, one row per report) are held to a
     * quarter of the 200, so a flood of them cannot push out every named track; the rest leave by age as all rows do. */
    enum { kTrackCapacity = 200, kAnonymousCapacity = 50, kPointsPerTrack = 100, kAgeTickMs = 1000 };

    enum Role {
        TrackIdRole = Qt::UserRole + 1, /**< The row's identity for its whole life (never reused); 0 on the gap row. */
        GapRole,                        /**< True on the gap row. */
        AnonymousRole,                  /**< No checked source: one point, no trail, no name. */
        SourceHashedRole,               /**< The source was a hash (DMR USBD). */
        SourceUnverifiedRole,           /**< No checked provenance named the source. */
        SourceIdRole,                   /**< Radio ID; undefined when none. */
        SourceTextRole,                 /**< Callsign (D-STAR, M17); "" when none. */
        LabelRole,                      /**< The newest name the decoder gave the source; "" when none. */
        TargetIdRole,                   /**< Undefined when none. */
        TargetGroupRole,                /**< The target is a talkgroup. */
        ProtocolRole,                   /**< DSD_SYNC_* of the carrier. */
        KindRole,                       /**< dsd_position_kind of the newest report. */
        KindTextRole,                   /**< The report format, in words ("LRRP", "TETRA LIP", ...). */
        HasPositionRole,                /**< The track has a current position (a fix not marked validity unknown). */
        LatitudeRole,                   /**< Current position, degrees (display only); undefined when none. */
        LongitudeRole,
        HasUnknownFixRole,   /**< The track holds a fix whose position may be old or invalid (hollow marker). */
        UnknownLatitudeRole, /**< That fix, degrees; undefined when none. */
        UnknownLongitudeRole,
        ValidityUnknownRole, /**< The newest report's position may be old or invalid (D11). */
        ProvisionalRole,     /**< The newest report's format is not confirmed by a published specification (D12). */
        WeakCheckRole,       /**< The newest report passed only a weak check (DMR embedded GPS). */
        EmergencyRole,       /**< The newest report carried an emergency indication. */
        AltitudeDmRole,      /**< Decimetres; undefined when unknown. */
        ErrorMRole,          /**< Position error, metres; undefined when unknown. */
        ErrorLowerBoundRole, /**< errorM is a lower bound. */
        SpeedDkmhRole,       /**< 0.1 km/h; undefined when unknown. */
        SpeedLowerBoundRole, /**< speedDkmh is a lower bound. */
        HeadingDdegRole,     /**< 0.1 degree; undefined when unknown. */
        HeadingAmbiguousRole,
        SatsRole,              /**< Satellites; undefined when not sent. */
        FreqHzRole,            /**< Frequency heard on, Hz; undefined when unknown. */
        FixTimeRole,           /**< The report's own UTC time, Unix seconds; undefined when none. */
        ReceivedTimeRole,      /**< When the newest report was received, seconds on its clock (receivedClock). */
        ReceivedClockRole,     /**< dsd_decode_clock_source of receivedTime. */
        TimeTextRole,          /**< "HH:mm:ss" local of receivedTime (the gap row: of the loss); "" when unknown. */
        AgeSecondsRole,        /**< Age of the current position; -1 when none or unknown. */
        UnknownAgeSecondsRole, /**< Age of the unknown fix; -1 when none or unknown. */
        ReportAgeSecondsRole,  /**< Age of the newest report's own time (fixTime); -1 when none or unknown. */
        ShownRole,             /**< The age filter shows one of the track's fixes (the gap row: always). */
        PointCountRole,        /**< Points in the trail. */
        /** The current position's own report, the one its marker is drawn from (spec 4.6: the sheet and the List
         *  describe the fix shown): {latitude, longitude, kindText, targetId and targetGroup (who this report was sent
         *  to; a track's reports can each name another), weakCheck, provisional, sourceHashed,
         *  sourceUnverified, errorM, errorLowerBound, speedDkmh, speedLowerBound, headingDdeg, headingAmbiguous,
         *  altitudeDm, sats, freqHz, fixTime, receivedTime, timeText, newest}; unknown values left out; {} when none. */
        CurrentFixRole,
        /** The newest validity-unknown report (the open ring), in the same keys; {} when none. */
        UnknownFixRole,
        /** Whether the canvas draws the current marker / the open ring now (the age filter can hide either): the
         *  sheet and the List describe what is drawn. Age roles: they change with the filter and the clock. */
        CurrentShownRole,
        UnknownShownRole,
    };

    /** @brief One trail point. */
    struct Point {
        qint32 latE7 = 0;
        qint32 lonE7 = 0;
        quint32 errM = 0;  /**< DSD_POSITION_ERR_UNKNOWN (0) when unknown */
        quint16 flags = 0; /**< dsd_position_flag: a segment ending at a FORMAT_PROVISIONAL point is drawn dashed */
        bool breakBefore = false; /**< no line joins this point to the one before it */
    };

    /** @brief One row: a track, an anonymous point, or the gap row. */
    struct Row {
        quint64 id = 0;          /**< 0 on the gap row */
        quint64 order = 0;       /**< when the row was placed: rows are held in descending order of it */
        quint64 lastArrival = 0; /**< when the newest report arrived: eviction order */
        quint64 breakEpoch = 0;  /**< the model's break epoch at the last trail point */
        qint64 ageS = -1;        /**< current position's age */
        qint64 unknownAgeS = -1;
        qint64 reportAgeS = -1;
        qint64 gapTimeS = 0;        /**< the gap row: the receive time of the last report read before the loss */
        QVector<Point> trail;       /**< oldest first; the last is the current position */
        QString label;              /**< the newest non-empty label */
        dsd_position_fix current{}; /**< the newest fix on the trail */
        dsd_position_fix unknown{}; /**< the newest validity-unknown fix */
        bool gap = false;
        bool anonymous = false;
        bool hasCurrent = false;
        bool hasUnknown = false;
        bool latestIsUnknown = false; /**< the newest report is @c unknown (else @c current) */
        bool currentShown = false;    /**< the age filter shows the current position */
        bool unknownShown = false;    /**< the age filter shows the unknown fix */
        bool gapTimeKnown = false;    /**< the gap row: gapTimeS and gapClock are known */
        quint8 gapClock = 0;          /**< the gap row: the clock of gapTimeS */

        const dsd_position_fix&
        latest() const {
            return latestIsUnknown ? unknown : current;
        }
    };

    /** @brief Real wall-clock seconds; injectable for tests. */
    using WallClock = qint64 (*)();

    explicit PositionModel(QObject* parent = nullptr);
    ~PositionModel() override;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    int
    count() const {
        return m_count;
    }

    static int
    capacity() {
        return kTrackCapacity;
    }

    static int
    pointsPerTrack() {
        return kPointsPerTrack;
    }

    int
    ageFilterSeconds() const {
        return m_ageFilterS;
    }

    void setAgeFilterSeconds(int seconds);

    bool
    ageing() const {
        return m_ageTimer.isActive();
    }

    void setAgeing(bool on);

    /** @brief The rows, in view order (for the map item, on the GUI thread). */
    const QVector<Row>&
    rows() const {
        return m_rows;
    }

    /** @brief Read what the view published since the last read (handing over from the background tap first). */
    void poll();

    /** @brief Drop every track and the gap row. The read position (stream, seq) stays, so the reports published up to
     * now are not read again; a running background tap drops what it holds and goes on from where it read. */
    Q_INVOKABLE void clear();

    /** @brief The row of track @p trackId (0: the gap row), or -1 when none is held. */
    Q_INVOKABLE int rowForTrack(qulonglong trackId) const;

    /** @brief Every role of track @p trackId by its role name; empty when none is held. */
    Q_INVOKABLE QVariantMap trackFor(qulonglong trackId) const;

    /** @brief Read ages again now (the map's timer does this once a second while ageing). */
    void refreshAges();

    /** @brief Read the view through @p source instead (tests; nullptr reads nothing). Stops a running tap. */
    void setSourceForTest(PositionSource source);
    /** @brief Read the wall clock through @p clock instead (tests). */
    void setWallClockForTest(WallClock clock);
    /** @brief The background tap's poll interval (tests); takes effect at its next start. */
    void setBackgroundPollIntervalForTest(int pollMs);
    bool backgroundCaptureRunningForTest() const;
    void waitForBackgroundPollForTest();

  public Q_SLOTS:
    /** @brief Start the background tap on hide or suspend; on any other state the next poll takes over from it. */
    void onApplicationStateChanged(Qt::ApplicationState state);

  Q_SIGNALS:
    void countChanged();
    void ageFilterSecondsChanged();
    void ageingChanged();
    /** Rows, trails or positions changed (the map item rebuilds its geometry). */
    void tracksChanged();
    /** Ages or what the age filter shows changed (the map item repaints marker strength). */
    void agesChanged();

  private:
    struct Ingest;

    static QVariant value(const Row& row, int role);
    static QVariant gapValue(const Row& row, int role);
    int positionFor(quint64 order) const;
    int gapRow() const;
    void setCount(int held);
    void readView();
    void handOff();
    void noteReceived(const dsd_position_fix& fix);
    void applyMarks(quint8 gap, Ingest& batch);
    void ingestPage(const dsd_position_fix* fixes, int n, quint8 gap, Ingest& batch);
    void ingest(const dsd_position_fix& fix, quint8 gap, Ingest& batch);
    void placeGap();
    int insertTrack(const dsd_position_fix& fix);
    void evictOldest(bool anonymousOnly);
    int anonymousCount() const;
    static void addFix(Row& row, const dsd_position_fix& fix, quint64 arrival, quint64 breakEpoch);
    void finishBatch(const Ingest& batch);
    bool updateAges(Row& row, qint64 wallNow) const;

    QVector<Row> m_rows;
    int m_count = 0;
    QHash<QByteArray, quint64> m_trackIds; /* typed source -> track id */
    quint64 m_nextOrder = 1;
    quint64 m_arrivals = 0;
    quint64 m_breakEpoch = 0;
    PositionReadState m_read;
    /* The receive time of the last report read, for a gap found after it. */
    bool m_lastRxKnown = false;
    qint64 m_lastRxS = 0;
    quint8 m_lastRxClock = 0;
    int m_ageFilterS = 0;
    QTimer m_ageTimer;
    PositionSource m_source;
    WallClock m_wallClock;
    std::unique_ptr<PositionTap> m_tap;
    int m_tapPollMs = 0;
    bool m_handoffOwed = false;
    std::unique_ptr<dsd_position_fix[]> m_page;
};

/** The List's rows: the tracks the age filter shows, and the gap row. */
class PositionFilterModel : public QSortFilterProxyModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY countChanged)

  public:
    explicit PositionFilterModel(QObject* parent = nullptr);

    int
    count() const {
        return rowCount();
    }

    /** @brief The tracks the filter shows, the gap row left out (a gap alone is no radio to show). */
    int trackCount() const;

    /** @brief The view row of track @p trackId (0: the gap row), or -1 when it is not shown. */
    Q_INVOKABLE int rowForTrack(qulonglong trackId) const;

  Q_SIGNALS:
    void countChanged();

  protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const override;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_POSITION_MODEL_H_ */
