// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief GPS site roaming's executor (#786): when roaming may run, what it is doing, and the switch it carries out.
 *
 * The QML context object `siteRoaming`. It decides with SiteRoamingCore (site_roaming.h) and owns the one roam
 * transaction there can be at a time. Main.qml owns the start path a roam ends in, so the two meet at named points:
 *
 *  - Main binds `facts` to the decoder snapshot it already reads (metrics) and to the session it names, tells the
 *    executor about every accepted start (noteSessionStart()), the post-initialization edge
 *    (noteSessionInitialized()), a site the user picked in the chooser (noteSiteChoice()) and every accepted operator
 *    command (noteOperatorIntent(), from CommandBridge::operatorIntentSubmitted()).
 *  - When a switch is due the executor emits switchRequested(); Main records the restart (`pendingRestart.kind ==
 *    "roam"`), and the executor ends the session with DecoderHost::stopForSwitch().
 *  - Once the decoder is idle Main asks startPlan() for the start: the authoritative checks run there (the foreground
 *    epoch and the user stop generation read from the host now, the verdict derived again, the destination still a
 *    roamable site of the group). Main reports a wait for USB access (noteWaitingForUsb(), noteUsbDenied()) and what the
 *    start came to (noteStartResult()).
 *  - Main cancels the roam through cancel() for anything the user decided (another start, a restore, a failure);
 *    incidental navigation leaves it. The executor cancels it itself for a disable, a foreground loss, a user stop, a
 *    session that can no longer roam and a verdict that went away. transactionEnded() tells Main to drop what it holds
 *    for it.
 *
 * Every time is read from one monotonic clock (the frontend's, the one location fixes are stamped on); a test injects
 * its own. Only a new location measurement can add evidence for a switch: the 1 s timer, list, settings and snapshot
 * changes can only take a verdict away (or carry out one already derived).
 *
 * The QML-facing state: `state` and `reason` (enums below) with `statusText`, the plain sentence for them; the site
 * the session follows, a nearer candidate and a destination, each by uid, name and (from the latest fix) distance;
 * `action`, what the reason's button does (performAction()); `recoveryNotice` for Home after a roam that stopped
 * listening; `fix`, the roaming fix the site chooser may rank from.
 */

#ifndef DSD_NEO_SRC_UI_QT_SITE_ROAMING_CONTROLLER_H_
#define DSD_NEO_SRC_UI_QT_SITE_ROAMING_CONTROLLER_H_

#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <QVariantMap>
#include <QtGlobal>
#include <functional>
#include <type_traits> // IWYU pragma: keep -- QPointer's conversion checks.
#include <vector>
#include "site_roaming.h"

namespace dsd_qt {

class AppPrefs;
class DecoderHost;
class LocationFeed;
class SavedSystemsModel;

class SiteRoaming : public QObject {
    Q_OBJECT
    /* The roaming switch (AppPrefs::roamingEnabled); change it with setEnabledByUser(). */
    Q_PROPERTY(bool enabled READ enabled NOTIFY enabledChanged)
    /* Whether this platform can roam at all (a host with a location feed); without it the feature is hidden. */
    Q_PROPERTY(bool available READ available NOTIFY changed)
    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(Reason reason READ reason NOTIFY changed)
    /* The state as one plain sentence (the reason's while Idle, Paused or Holding); empty while Off. */
    Q_PROPERTY(QString statusText READ statusText NOTIFY changed)
    /* The state for the Monitor header, state first and the feature name last ("STAYING · GPS ROAMING", "PAUSED: NO
     * FIX · GPS ROAMING"), so an elided line keeps the state; empty while Off or Idle. Its sentence is statusText. A
     * session's wait for its first fix reads "LOCATING · GPS ROAMING" for up to 60 s; a fix lost later reads paused. */
    Q_PROPERTY(QString headerText READ headerText NOTIFY changed)
    /* The state alone ("WAITING", "LOCATING"), for a header too narrow for headerText; empty while Off or Idle. */
    Q_PROPERTY(QString headerShortText READ headerShortText NOTIFY changed)
    /* GlobalExtraArgs' sentence, for a screen that says why roaming cannot be switched on (the site chooser). */
    Q_PROPERTY(QString globalExtraArgsText READ globalExtraArgsText CONSTANT)
    /* What the reason's button does (performAction()). */
    Q_PROPERTY(Action action READ action NOTIFY changed)
    /* The site group of the session roaming follows ("<rrSid>|<receiver>", site_group_key()); empty with none. */
    Q_PROPERTY(QString groupKey READ groupKey NOTIFY changed)
    /* That group's name, as its Home card names it (site_group_title() of its first row); empty with none. */
    Q_PROPERTY(QString systemName READ systemName NOTIFY changed)
    /* The site the session runs, its name and its distance from the latest fix in km (-1 when unknown). */
    Q_PROPERTY(QString currentUid READ currentUid NOTIFY changed)
    Q_PROPERTY(QString currentName READ currentName NOTIFY changed)
    Q_PROPERTY(double currentDistanceKm READ currentDistanceKm NOTIFY changed)
    /* The nearer site being checked (Candidate) or waited for (WaitingForCall); empty otherwise. */
    Q_PROPERTY(QString candidateUid READ candidateUid NOTIFY changed)
    Q_PROPERTY(QString candidateName READ candidateName NOTIFY changed)
    Q_PROPERTY(double candidateDistanceKm READ candidateDistanceKm NOTIFY changed)
    /* The site a switch under way goes to (WaitingForCall and Switching); empty otherwise. */
    Q_PROPERTY(QString destinationUid READ destinationUid NOTIFY changed)
    Q_PROPERTY(QString destinationName READ destinationName NOTIFY changed)
    /* A roam transaction is under way, from the switch verdict until it is done, cancelled or refused. */
    Q_PROPERTY(bool transactionActive READ transactionActive NOTIFY changed)
    /* The latest usable roaming fix, {lat, lon, accuracyM}, or empty: the chooser ranks from it while it is there. */
    Q_PROPERTY(QVariantMap fix READ fix NOTIFY changed)
    /* After a roam that left listening stopped: "Roaming did not switch to %1. Listening stopped.", and the action
     * that starts the site it left ("Listen to %2", recoveryUid). Empty otherwise; the next start clears it. */
    Q_PROPERTY(QString recoveryNotice READ recoveryNotice NOTIFY recoveryNoticeChanged)
    Q_PROPERTY(QString recoveryActionText READ recoveryActionText NOTIFY recoveryNoticeChanged)
    Q_PROPERTY(QString recoveryUid READ recoveryUid NOTIFY recoveryNoticeChanged)
    /*
     * What the decoder snapshot and Main say of the running session, as Main binds it: savedUid (the saved system the
     * session runs as, empty for a scan, Explore or an operator hold), trunking, radioInput, replayInput, scanKind,
     * operatorTuned, heldTg, callBusy and optionsKnown (MetricsModel's readings of those names).
     */
    Q_PROPERTY(QVariantMap facts READ facts WRITE setFacts NOTIFY factsChanged)

  public:
    enum State {
        /** Roaming is switched off, or this platform cannot roam. */
        Off,
        /** Switched on, but the session is not a saved site roaming can follow: @c reason says why. */
        Idle,
        /** It could roam, but something holds it: @c reason says what. */
        Paused,
        /** Staying on the site the session runs: it is the nearest, or no other is clearly closer. */
        Following,
        /** Another site (candidateUid) is closer; its readings do not yet span the dwell. */
        Candidate,
        /** A switch to destinationUid waits for the call the session follows (at most 90 s). */
        WaitingForCall,
        /** Switching to destinationUid: the session stops, then the destination starts. */
        Switching,
        /** The user chose this site in the site chooser; roaming waits for Resume roaming. */
        Holding,
    };
    Q_ENUM(State)

    enum Reason {
        NoReason,
        /** Idle: nothing is listening. */
        NoSession,
        /** Idle: the session was running before the app opened (a recreated window took it up); it never arms. */
        SessionAdopted,
        /** Idle: the session is starting (before the engine's post-initialization edge). */
        SessionStarting,
        /** Idle: the session is not a saved site (a scan list, Explore, Signal search, an operator's Explore). */
        NotSavedSite,
        /** Idle: the site is in no site group, or left the one the session started in. */
        NotGrouped,
        /** Idle: the site itself cannot roam (trunking off, a receiver that cannot retune, or extra options). */
        SiteNotRoamable,
        /** Idle: the site's system has fewer than two sites roaming can use. */
        FewSites,
        /** Idle: the session's input cannot be retuned (network audio, a file, an I/Q replay). */
        NotTunable,
        /** Idle: the session scans. */
        Scanning,
        /** Idle: extra options are set in Settings. */
        GlobalExtraArgs,
        /** Paused: trunking was turned off in the session. */
        NotTrunking,
        /** Paused: the operator tuned by hand or took the tuner. */
        OperatorTune,
        /** Paused: a talkgroup is held. */
        TalkgroupHeld,
        /** Paused: an operator command was queued and the decoder has not shown its outcome yet. */
        PendingChange,
        /** Paused: location permission is off. */
        PermissionOff,
        /** Paused: only approximate location is allowed. */
        ApproximateOnly,
        /** Paused: location is off on the device. */
        LocationOff,
        /** Paused: no fix yet, or the last is too old. */
        NoFix,
        /** Paused: the fix is too imprecise. */
        ImpreciseFix,
        /** Paused: no site of the system is open, positioned and roamable. */
        NoRankableSite,
        /** Paused: no site is within the maximum distance (statusText names it). */
        OutOfRange,
        /** Paused: the distances in Settings are not usable. */
        InvalidSettings,
        /** Paused: the last switch could not start its destination (statusText names it); no retry follows. */
        SwitchFailed,
        /** Holding: the user chose this site. */
        ManualHold,
    };
    Q_ENUM(Reason)

    enum Action {
        NoAction,
        /** Open the platform's settings page for this app's permissions. */
        OpenAppSettings,
        /** Open the platform's location settings. */
        TurnOnLocation,
        /** Resume roaming after a site the user chose. */
        ResumeRoaming,
    };
    Q_ENUM(Action)

    SiteRoaming(AppPrefs* prefs, SavedSystemsModel* systems, QObject* parent = nullptr);
    ~SiteRoaming() override;
    SiteRoaming(const SiteRoaming&) = delete;
    SiteRoaming& operator=(const SiteRoaming&) = delete;

    /** @brief The host the session runs on; null (a host with no roaming) leaves roaming Off. */
    void setHost(DecoderHost* host);
    /**
     * @brief The location feed roaming holds its lease on (LocationFeed::roamingLease()), shared with the map; the
     * caller keeps its host the same as this one's. Null (the default) is a feed of roaming's own over its host.
     */
    void setLocationFeed(LocationFeed* feed);
    /** @brief The monotonic clock in ms every time here is read from: the frontend's by default, a test's instead. */
    void setClock(std::function<qint64()> clock);
    /**
     * @brief Whether a location reading is held in memory. Never once the feed is stopped or the app left the
     * foreground: only the newest reading, and only while roaming runs (PRIVACY_POLICY.md).
     */
    bool holdsLocation() const;

    bool enabled() const;
    bool available() const;
    State state() const;
    Reason reason() const;
    QString statusText() const;
    QString headerText() const;
    QString headerShortText() const;
    QString globalExtraArgsText() const;
    Action action() const;
    QString groupKey() const;
    QString systemName() const;
    QString currentUid() const;
    QString currentName() const;
    double currentDistanceKm() const;
    QString candidateUid() const;
    QString candidateName() const;
    double candidateDistanceKm() const;
    QString destinationUid() const;
    QString destinationName() const;
    bool transactionActive() const;
    QVariantMap fix() const;
    QString recoveryNotice() const;
    QString recoveryActionText() const;
    QString recoveryUid() const;
    QVariantMap facts() const;
    void setFacts(const QVariantMap& readings);

    /** @brief Switch roaming on or off for the user. On, it asks for precise location once (a new request each time). */
    Q_INVOKABLE void setEnabledByUser(bool on);
    /** @brief Do what `action` names. */
    Q_INVOKABLE void performAction();
    /** @brief Roam again after a site the user chose. */
    Q_INVOKABLE void resumeRoaming();
    /** @brief Hide the recovery notice. */
    Q_INVOKABLE void clearRecoveryNotice();

    /**
     * @brief The user picked saved site @p uid in the site chooser while a session roaming judges runs (shown neither
     * Off nor Idle; Stop and switch, Listen to nearest, a row that restarts): the session that pick starts holds
     * roaming. A pick with nothing running, or from a session roaming idles on, is a start like any other.
     */
    Q_INVOKABLE void noteSiteChoice(const QString& uid);
    /** @brief The start a site choice was noted for did not happen (refused, or its restart given up). */
    Q_INVOKABLE void forgetSiteChoice();
    /**
     * @brief Main took up a session of @p kind it did not start (the service outlived the window). It never arms:
     * its stop generation and initialization edge were never seen. A saved one idles with SessionAdopted.
     */
    Q_INVOKABLE void noteSessionAdopted(const QString& kind);
    /**
     * @brief The host accepted a start of @p kind ("saved", "scan", "explore") naming @p uid: a new session. @p roamId
     * is the roam the start belongs to, 0 for any other start (which cancels a roam under way).
     */
    Q_INVOKABLE void noteSessionStart(const QString& kind, const QString& uid, qint64 roamId);
    /** @brief The session passed the engine's post-initialization edge: roaming may arm, and a roam's start is done. */
    Q_INVOKABLE void noteSessionInitialized();
    /** @brief An operator command was queued (CommandBridge::operatorIntentSubmitted()'s @p kind). */
    Q_INVOKABLE void noteOperatorIntent(const QString& kind);
    /**
     * @brief Cancel the roam under way for something the user decided: @p reason "start" (another start), "retry",
     * "restore", "close", "dismiss" or "failed" (the session failed). Each is silent: what the user did follows.
     */
    Q_INVOKABLE void cancel(const QString& why);
    /**
     * @brief The start of roam @p roamId, now that the decoder is idle: {ok, uid, expectedStopGeneration,
     * expectedForegroundEpoch} after the authoritative checks, or {ok: false} when they cancelled it (or it is no
     * longer this roam's turn).
     */
    Q_INVOKABLE QVariantMap startPlan(qint64 roamId);
    /** @brief Roam @p roamId's start waits for USB access. */
    Q_INVOKABLE void noteWaitingForUsb(qint64 roamId);
    /** @brief USB access was refused for roam @p roamId: it ends refused, and roaming pauses. */
    Q_INVOKABLE void noteUsbDenied(qint64 roamId);
    /** @brief What SessionArgsBuilder::start() answered for roam @p roamId's start ({ok, started, superseded}). */
    Q_INVOKABLE void noteStartResult(qint64 roamId, const QVariantMap& result);

  public Q_SLOTS:
    /** @brief The 1 s timer's evaluation: it revokes a verdict, or carries out one already derived; it adds nothing. */
    void tick();

  Q_SIGNALS:
    void enabledChanged();
    void changed();
    void recoveryNoticeChanged();
    void factsChanged();
    /** @brief Roam @p roamId switches to @p uid: Main records the restart before the session stops for it. */
    void switchRequested(qint64 roamId, const QString& uid);
    /** @brief Roam @p roamId ended: "done", "cancelled" or "refused". Main drops what it holds for it. */
    void transactionEnded(qint64 roamId, const QString& outcome);

  private:
    enum class Stage { None, WaitingForCall, Stopping, WaitingForUsb, Starting };

    struct Facts {
        QString savedUid;
        bool trunking = false;
        bool radioInput = false;
        bool replayInput = false;
        bool operatorTuned = false;
        bool callBusy = false;
        bool optionsKnown = false;
        int scanKind = 0;
        qulonglong heldTg = 0;
    };

    struct Session {
        qint64 id = 0;
        QString kind;
        QString uid;
        QString groupKey;
        qint64 stopGeneration = -1;
        bool initialized = false;
        bool manualHold = false;
        /* A user stop was seen while it ran (the generation moved), or the generation could not be read. */
        bool stopObserved = false;
        /* What the decoder was started with (#786 r3): Settings' extra options were set, and the row was roamable
         * (its own extra options, receiver, trunking). Edits made while it runs reach the decoder only at its next
         * start, so they can only take roaming away, never give it. */
        bool globalExtrasAtStart = false;
        bool roamableAtStart = false;
    };

    struct Intent {
        bool pending = false;
        QString kind;
        bool baseline = false;
    };

    struct Transaction {
        qint64 roamId = 0;
        QString fromUid;
        QString toUid;
        QString groupKey;
        qint64 sessionId = 0;
        qint64 destSessionId = 0;
        qint64 stopGeneration = -1;
        qint64 epoch = 0;
        Stage stage = Stage::None;
        /* The destination's start showed on the host (Starting or later): an Idle after that is its end. */
        bool startShown = false;
        /* The session the switch leaves showed its stop (Stopping), and then went idle (startPlan() asks only then):
         * a Running between the two is a stop the host gave up on, one after them the destination's own start. */
        bool stopShown = false;
        bool stopDone = false;
    };

    struct View {
        State state = Off;
        Reason reason = NoReason;
        QString reasonArg;
        QString currentUid;
        double currentKm = -1;
        QString candidateUid;
        double candidateKm = -1;
        QString destinationUid;
        QString groupKey;
        bool transactionActive = false;
        QVariantMap fix;
        bool available = false;
        /* Waiting for the session's first fix, within kLocatingMs of its first wait: the header says LOCATING. */
        bool locating = false;
        /* Everything QML reads that derives from the above and the saved systems (#786 r7): computed in publish(), so
         * a change of a name alone (a site renamed by an RR update) or of the unit shown is a change of the view. */
        QString currentName;
        QString candidateName;
        QString destinationName;
        QString systemName;
        QString statusText;
        QString headerText;
        QString headerShortText;
        Action action = NoAction;

        bool operator==(const View& other) const;
        bool sameState(const View& other) const;
        bool sameLabels(const View& other) const;
    };

    void evaluate();
    void evaluateSoon();
    void evaluateOnce();
    void judge(qint64 nowMs);
    void onVerdict(qint64 nowMs, const SiteRoamingVerdict& verdict);
    void onSwitchVerdict(qint64 nowMs, const QString& uid);
    void attemptStop(qint64 nowMs);
    void progressTransaction();
    void progressStop(int phase);
    void feedTransaction(qint64 nowMs);
    bool userStoppedSince(qint64 generation) const;
    bool leftForegroundSince(qint64 epoch) const;
    void finishUnrunStart();
    void retireStartForForeground();
    void onHostSession();
    void onStreamUpdate(const QVariantMap& record);
    void onFeedForegroundMoved(qint64 epoch);
    void onFeedLeasesDropped();
    void attachFeed(LocationFeed* feed);
    void onEnabledChanged();
    void noteForeground();
    void onForegroundLost();
    void endSession();
    void clearIntents();
    void clearAppliedIntents();
    bool intentPending() const;
    void noteQualification();
    void revoke(qint64 nowMs);
    void ensureStream(bool need);
    void forgetMeasurement();
    void updateTimer();

    Reason armReason() const;
    Reason sessionShapeReason() const;
    Reason siteShapeReason() const;
    Reason liveReason() const;
    Reason streamReason() const;
    bool globalExtras() const;
    int roamableSiblings() const;
    std::vector<SiteRoamingCandidate> candidates(const QString& key) const;
    SiteRoamingSettings settings() const;
    const SiteRoamingMeasurement* usableMeasurement() const;
    SiteRoamingVerdict judgeAgainst(qint64 nowMs, const QString& key, const QString& fromUid);
    QString boundaryFailure(qint64 nowMs);
    QString settingsFailure() const;
    QString destinationFailure() const;

    void beginTransaction(const QString& uid);
    void cancelTransaction(const QString& why);
    void finishTransaction(const QString& outcome, bool notice);
    void endTransaction(const QString& why, bool listeningStopped, bool refusal);
    QString endCause(const QString& why) const;
    bool leftStopped() const;
    bool active() const;

    void setView(State shown, Reason why, const QString& arg = QString());
    void publish();
    bool locating(qint64 nowMs) const;
    QString siteName(const QString& uid) const;
    double distanceTo(const QVariantMap& at, const QString& uid) const;
    QString distanceText(double km) const;
    QString reasonText(const View& view) const;
    QString statusTextOf(const View& view) const;
    QString headerShortTextOf(const View& view) const;
    QString headerTextOf(const View& view) const;
    QString systemNameOf(const QString& key) const;
    static Action actionOf(Reason reason);
    void refreshNotice();
    void deriveLabels(View& view) const;
    qint64 now() const;

    AppPrefs* m_prefs = nullptr;
    SavedSystemsModel* m_systems = nullptr;
    QPointer<DecoderHost> m_host;
    std::function<qint64()> m_clock;
    QTimer m_timer{this};

    SiteRoamingCore m_core;
    CallWaitClock m_callWait;
    Facts m_facts;
    QVariantMap m_factsMap;
    Session m_session;
    bool m_sessionLive = false;
    /* A saved session Main took up without starting it (noteSessionAdopted()), until it ends or another starts. */
    bool m_adoptedLive = false;
    QString m_pendingChoiceUid;
    Intent m_holdIntent;
    Intent m_tunerIntent;
    Intent m_trunkingIntent;
    Transaction m_tx;
    qint64 m_nextRoamId = 1;
    qint64 m_nextPermissionId = 1;
    QString m_failedUid;

    /* The feed roaming takes its lease on: the shared one (setLocationFeed()), else its own over m_host. */
    QPointer<LocationFeed> m_feed;
    LocationFeed* m_ownFeed = nullptr;
    /* Roaming follows the feed in this foreground epoch (its lease taken since the last foreground loss). */
    bool m_streamOn = false;
    QString m_streamState;
    /* The newest fix stamp seen from each provider, and as it stood at the last reset (a foreground loss): a fix at
     * or before that, or within kSameFixNs of another provider's (a relay), is from before the reset, and is never fed
     * again. */
    QHash<QString, qint64> m_seenNs;
    QHash<QString, qint64> m_resetFloor;
    bool m_hasMeasurement = false;
    SiteRoamingMeasurement m_measurement;
    /* The session has had a fix fed, and when it first waited for one (-1: not yet): until its first fix, or for
     * kLocatingMs, a missing fix reads as locating rather than paused. */
    bool m_sessionHadFix = false;
    qint64 m_locatingSinceMs = -1;
    bool m_epochKnown = false;
    qint64 m_seenEpoch = 0;
    QString m_qualifyingUid;
    QString m_verdictUid;

    bool m_evaluating = false;
    /* An evaluation is queued for the next turn of the event loop (evaluateSoon()). */
    bool m_evaluateQueued = false;
    bool m_again = false;
    View m_pending;
    View m_view;

    QString m_noticeText;
    QString m_noticeAction;
    QString m_noticeUid;
    /* The site the notice says roaming did not switch to; its text is derived again when names change. */
    QString m_noticeToUid;
    /* The session the notice says stopped (the roam's own session id; 0: no notice). */
    qint64 m_noticeSessionId = 0;
};

} // namespace dsd_qt

#endif /* DSD_NEO_SRC_UI_QT_SITE_ROAMING_CONTROLLER_H_ */
