// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "site_roaming_controller.h"

#include <QAbstractItemModel>
#include <QDebug>
#include <QLatin1String>
#include <QList>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <Qt>
#include <algorithm>
#include <cmath>
#include <dsd-neo/runtime/decode_clock.h>
#include <string>
#include <utility>
#include "app_prefs.h"
#include "decoder_host.h"
#include "location_feed.h"
#include "saved_systems_model.h"
#include "site_groups.h"

namespace dsd_qt {

namespace {

constexpr int kTickMs = 1000;
/* How long a session that has had no fix yet reads as locating (its first fix on the way) rather than paused. */
constexpr qint64 kLocatingMs = 60000;
constexpr double kKmPerMile = 1.609344;
constexpr double kSameKm = 1e-9;

/* Each Reason's sentence, in the enum's order. A %1 is the distance (OutOfRange) or the site (SwitchFailed). */
const char* const kReasonTexts[] = {
    "",
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Start listening again to use GPS roaming"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "GPS roaming starts once listening begins"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming works while listening to a site of a multi-site system"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "GPS roaming is off while extra options are set in Settings"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Paused while trunking is off"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Paused while you tune by hand"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Paused while a talkgroup is held"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Waiting for your last change to take effect"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Location permission is off"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Roaming needs precise location"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Location is off"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Waiting for a location fix"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Location is too imprecise right now"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "No site in this system can be ranked"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "No saved site within %1"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "The GPS roaming distances in Settings are not valid"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Could not switch to %1"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "Holding the site you chose"),
};
static_assert(sizeof(kReasonTexts) / sizeof(kReasonTexts[0]) == SiteRoaming::ManualHold + 1,
              "one sentence per Reason, in the enum's order");

/* What a pause is for, as the Monitor header names it after "PAUSED: ", in the Reason enum's order; empty for the
 * reasons that never pause (the header then says "PAUSED" alone). */
const char* const kPauseWords[] = {
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "TRUNKING OFF"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "TUNED BY HAND"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "TALKGROUP HELD"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "CHANGE PENDING"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "NO PERMISSION"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "APPROXIMATE ONLY"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "LOCATION OFF"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "NO FIX"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "IMPRECISE FIX"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "NO SITE TO RANK"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "OUT OF RANGE"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "CHECK SETTINGS"),
    QT_TRANSLATE_NOOP("dsd_qt::SiteRoaming", "SWITCH FAILED"),
    "",
};
static_assert(sizeof(kPauseWords) / sizeof(kPauseWords[0]) == SiteRoaming::ManualHold + 1,
              "one pause word per Reason, in the enum's order");

/* The reasons the user's own action stands behind, and the ends that need no word: cancelling for one leaves no notice,
 * since what they did follows (a failed session's card, an unrun start's own cause, a session that runs on). */
bool
silent_reason(const QString& reason) {
    static const QStringList silent = {
        QStringLiteral("userStop"),   QStringLiteral("start"),      QStringLiteral("retry"),
        QStringLiteral("restore"),    QStringLiteral("close"),      QStringLiteral("dismiss"),
        QStringLiteral("failed"),     QStringLiteral("revoked"),    QStringLiteral("unarmable"),
        QStringLiteral("superseded"), QStringLiteral("unreadable"),
    };
    return silent.contains(reason);
}

/*
 * The one precedence of every reason a roam ends for (#786 r5), strongest first: the user's stop (a generation that
 * cannot be read counts as one), the user's own choice (another start, Retry, a restore, a close, a dismissal), an
 * operator command that changes what the session follows (Hold TG, a manual tune, a tuner release, trunking on or off:
 * "intent"), roaming turned off, the foreground lost, and then everything else (the verdict revoked, a stop the host
 * gave up on, a failed or refused start, the checks before a step). Lower is stronger.
 */
int
end_rank(const QString& reason) {
    static const QStringList manual = {QStringLiteral("start"), QStringLiteral("retry"), QStringLiteral("restore"),
                                       QStringLiteral("close"), QStringLiteral("dismiss")};
    if (reason == QLatin1String("userStop") || reason == QLatin1String("unreadable")) {
        return 0;
    }
    if (manual.contains(reason)) {
        return 1;
    }
    if (reason == QLatin1String("intent")) {
        return 2;
    }
    if (reason == QLatin1String("disabled")) {
        return 3;
    }
    return reason == QLatin1String("foreground") ? 4 : 5;
}

SiteRoaming::State
state_for(SiteRoaming::Reason reason) {
    if (reason >= SiteRoaming::NoSession && reason <= SiteRoaming::GlobalExtraArgs) {
        return SiteRoaming::Idle;
    }
    return reason == SiteRoaming::ManualHold ? SiteRoaming::Holding : SiteRoaming::Paused;
}

bool
same_km(double a, double b) {
    return std::fabs(a - b) < kSameKm;
}

} // namespace

bool
SiteRoaming::View::operator==(const View& other) const {
    return sameState(other) && sameLabels(other);
}

bool
SiteRoaming::View::sameState(const View& other) const {
    return state == other.state && reason == other.reason && reasonArg == other.reasonArg
           && currentUid == other.currentUid && same_km(currentKm, other.currentKm)
           && candidateUid == other.candidateUid && same_km(candidateKm, other.candidateKm)
           && destinationUid == other.destinationUid && groupKey == other.groupKey
           && transactionActive == other.transactionActive && fix == other.fix && available == other.available
           && locating == other.locating;
}

bool
SiteRoaming::View::sameLabels(const View& other) const {
    return currentName == other.currentName && candidateName == other.candidateName
           && destinationName == other.destinationName && systemName == other.systemName
           && statusText == other.statusText && headerText == other.headerText
           && headerShortText == other.headerShortText && action == other.action;
}

SiteRoaming::SiteRoaming(AppPrefs* prefs, SavedSystemsModel* systems, QObject* parent)
    : QObject(parent), m_prefs(prefs), m_systems(systems),
      m_clock([] { return static_cast<qint64>(dsd_realtime_mono_ms()); }) {
    m_timer.setObjectName(QStringLiteral("siteRoamingTick"));
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &SiteRoaming::tick);
    m_ownFeed = new LocationFeed(this);
    attachFeed(m_ownFeed);
    if (m_prefs != nullptr) {
        connect(m_prefs, &AppPrefs::roamingEnabledChanged, this, &SiteRoaming::onEnabledChanged);
        connect(m_prefs, &AppPrefs::roamingDistancesChanged, this, &SiteRoaming::evaluate);
        connect(m_prefs, &AppPrefs::extraArgsChanged, this, &SiteRoaming::evaluate);
        // The distance in "No saved site within %1" is in the unit shown: publish() derives it again.
        connect(m_prefs, &AppPrefs::metricUnitsChanged, this, &SiteRoaming::evaluate);
    }
    if (m_systems != nullptr) {
        connect(m_systems, &QAbstractItemModel::dataChanged, this, &SiteRoaming::evaluate);
        connect(m_systems, &QAbstractItemModel::rowsInserted, this, &SiteRoaming::evaluate);
        connect(m_systems, &QAbstractItemModel::rowsRemoved, this, &SiteRoaming::evaluate);
        connect(m_systems, &QAbstractItemModel::modelReset, this, &SiteRoaming::evaluate);
        connect(m_systems, &SavedSystemsModel::sitesChanged, this, &SiteRoaming::evaluate);
    }
}

SiteRoaming::~SiteRoaming() = default;

void
SiteRoaming::setHost(DecoderHost* host) {
    if (m_host == host) {
        return;
    }
    if (m_host != nullptr) {
        disconnect(m_host, nullptr, this, nullptr);
    }
    if (m_feed != nullptr) {
        m_feed->release(LocationFeed::roamingLease()); // the last lease stops the old host's feed
    }
    m_streamOn = false;
    forgetMeasurement();
    m_streamState.clear();
    m_tx = Transaction();
    m_session = Session();
    m_sessionLive = false;
    m_adoptedLive = false;
    clearIntents();
    m_epochKnown = false;
    m_core.reset();
    m_callWait.reset();
    m_seenNs.clear();
    m_resetFloor.clear();
    m_failedUid.clear();
    m_pendingChoiceUid.clear();
    m_sessionHadFix = false;
    m_locatingSinceMs = -1;
    m_host = host;
    if (m_feed == m_ownFeed) {
        m_ownFeed->setHost(host);
    }
    if (host != nullptr) {
        connect(host, &DecoderHost::sessionStateChanged, this, &SiteRoaming::onHostSession);
    }
    evaluate();
}

void
SiteRoaming::setLocationFeed(LocationFeed* feed) {
    LocationFeed* next = feed != nullptr ? feed : m_ownFeed;
    if (m_feed == next) {
        return;
    }
    if (m_feed != nullptr) {
        m_feed->release(LocationFeed::roamingLease());
        disconnect(m_feed, nullptr, this, nullptr);
    }
    m_streamOn = false;
    m_streamState.clear();
    forgetMeasurement();
    // Roaming's own feed follows its host only while it is the one roaming uses.
    m_ownFeed->setHost(next == m_ownFeed ? m_host.data() : nullptr);
    attachFeed(next);
    evaluate();
}

void
SiteRoaming::attachFeed(LocationFeed* feed) {
    m_feed = feed;
    connect(feed, &LocationFeed::updated, this, &SiteRoaming::onStreamUpdate);
    connect(feed, &LocationFeed::foregroundMoved, this, &SiteRoaming::onFeedForegroundMoved);
    connect(feed, &LocationFeed::leasesDropped, this, &SiteRoaming::onFeedLeasesDropped);
}

void
SiteRoaming::setClock(std::function<qint64()> clock) {
    m_clock = std::move(clock);
}

qint64
SiteRoaming::now() const {
    return m_clock();
}

bool
SiteRoaming::enabled() const {
    return m_prefs != nullptr && m_prefs->roamingEnabled();
}

bool
SiteRoaming::available() const {
    return m_host != nullptr && m_host->locationSupported();
}

bool
SiteRoaming::active() const {
    return m_tx.stage != Stage::None;
}

// ---- Evaluation ----

void
SiteRoaming::evaluate() {
    // A host or Main reacting to what this does (a stop, a signal) re-enters; the outer pass goes round again.
    if (m_evaluating) {
        m_again = true;
        return;
    }
    m_evaluating = true;
    int rounds = 0;
    do {
        m_again = false;
        evaluateOnce();
    } while (m_again && ++rounds < 4);
    m_evaluating = false;
    publish();
}

void
SiteRoaming::tick() {
    evaluate();
}

void
SiteRoaming::evaluateOnce() {
    const qint64 t = now();
    noteForeground();
    clearAppliedIntents();
    if (active() && m_tx.stage != Stage::WaitingForCall) {
        progressTransaction();
        if (active()) {
            feedTransaction(t);
            ensureStream(true); // the checks before the destination starts derive the verdict again
            setView(Switching, NoReason);
            m_pending.destinationUid = m_tx.toUid;
            updateTimer();
            return;
        }
    }
    if (!enabled() || !available()) {
        ensureStream(false);
        setView(Off, NoReason);
        updateTimer();
        return;
    }
    const Reason arm = armReason();
    if (arm != NoReason) {
        cancelTransaction(QStringLiteral("unarmable"));
        revoke(t);
        ensureStream(false);
        setView(state_for(arm), arm, arm == SwitchFailed ? siteName(m_failedUid) : QString());
        updateTimer();
        return;
    }
    ensureStream(true);
    judge(t);
    updateTimer();
}

void
SiteRoaming::judge(qint64 nowMs) {
    const Reason stream = streamReason();
    const SiteRoamingVerdict verdict =
        m_core.evaluate(nowMs, candidates(m_session.groupKey), m_session.uid.toStdString(),
                        stream == NoReason ? usableMeasurement() : nullptr, settings());
    noteQualification();
    if (m_session.stopObserved) {
        // The user stopped listening (the generation moved while it still read as running): nothing may start now.
        cancelTransaction(QStringLiteral("userStop"));
        revoke(nowMs);
        setView(Idle, NoSession);
        return;
    }
    if (stream != NoReason) {
        cancelTransaction(QStringLiteral("revoked"));
        setView(Paused, stream);
        return;
    }
    onVerdict(nowMs, verdict);
}

void
SiteRoaming::onVerdict(qint64 nowMs, const SiteRoamingVerdict& verdict) {
    using Kind = SiteRoamingVerdict::Kind;
    const QString uid = QString::fromStdString(verdict.uid);
    const QString awaited = verdict.kind == Kind::Candidate || verdict.kind == Kind::Switch ? uid : QString();
    if (awaited != m_verdictUid) {
        // A wait for a call belongs to the destination it was for.
        m_callWait.reset();
        m_verdictUid = awaited;
    }
    if (!m_failedUid.isEmpty()) {
        // A switch the old session's stop never let through (progressStop()): paused on it until a fresh fix starts a
        // new qualification, which the evidence cleared then needs.
        if (verdict.kind != Kind::Candidate && verdict.kind != Kind::Switch) {
            setView(Paused, SwitchFailed, siteName(m_failedUid));
            return;
        }
        m_failedUid.clear();
    }
    if (verdict.kind == Kind::Switch) {
        onSwitchVerdict(nowMs, uid);
        return;
    }
    cancelTransaction(QStringLiteral("revoked"));
    if (verdict.kind == Kind::Following) {
        setView(Following, NoReason);
    } else if (verdict.kind == Kind::Candidate) {
        setView(Candidate, NoReason);
        m_pending.candidateUid = uid;
    } else {
        Reason why = NoFix;
        switch (verdict.reason) {
            case SiteRoamingPause::ImpreciseFix: why = ImpreciseFix; break;
            case SiteRoamingPause::NoRankableSite: why = NoRankableSite; break;
            case SiteRoamingPause::OutOfRange: why = OutOfRange; break;
            case SiteRoamingPause::InvalidSettings: why = InvalidSettings; break;
            default: break;
        }
        setView(Paused, why, why == OutOfRange ? distanceText(settings().max_km) : QString());
    }
}

void
SiteRoaming::onSwitchVerdict(qint64 nowMs, const QString& uid) {
    if (active() && m_tx.toUid != uid) {
        cancelTransaction(QStringLiteral("revoked"));
    }
    if (!active()) {
        beginTransaction(uid);
    }
    if (m_callWait.busy(nowMs, m_facts.callBusy) == CallWait::Waiting) {
        setView(WaitingForCall, NoReason);
        m_pending.candidateUid = uid;
        m_pending.destinationUid = uid;
        return;
    }
    attemptStop(nowMs);
}

void
SiteRoaming::attemptStop(qint64 nowMs) {
    const QString failure = boundaryFailure(nowMs);
    if (!failure.isEmpty()) {
        if (failure == QLatin1String("userStop") || failure == QLatin1String("unreadable")) {
            m_session.stopObserved = true;
        }
        cancelTransaction(failure);
        revoke(nowMs);
        m_again = true; // judged again with the candidate gone
        return;
    }
    m_tx.stage = Stage::Stopping;
    setView(Switching, NoReason);
    m_pending.destinationUid = m_tx.toUid;
    const qint64 roamId = m_tx.roamId;
    // Main records the restart first: a host that stops at once reports Idle inside stopForSwitch().
    Q_EMIT switchRequested(roamId, m_tx.toUid);
    if (m_host != nullptr && active() && m_tx.roamId == roamId) {
        m_host->stopForSwitch();
    }
}

/* The checks every step of a switch runs before it acts, reading the host now: what failed, or empty. */
QString
SiteRoaming::boundaryFailure(qint64 nowMs) {
    if (m_host == nullptr || m_systems == nullptr) {
        return QStringLiteral("host");
    }
    const qint64 epoch = m_host->foregroundEpoch();
    if (epoch < 0 || epoch != m_tx.epoch) {
        return QStringLiteral("foreground");
    }
    const qint64 generation = m_host->userStopGeneration();
    if (generation < 0) {
        return QStringLiteral("unreadable");
    }
    if (generation != m_tx.stopGeneration) {
        return QStringLiteral("userStop");
    }
    QString held = settingsFailure();
    if (!held.isEmpty()) {
        return held;
    }
    QString destination = destinationFailure();
    if (!destination.isEmpty()) {
        return destination;
    }
    const SiteRoamingVerdict verdict = judgeAgainst(nowMs, m_tx.groupKey, m_tx.fromUid);
    if (verdict.kind != SiteRoamingVerdict::Kind::Switch || QString::fromStdString(verdict.uid) != m_tx.toUid) {
        return QStringLiteral("verdict");
    }
    return QString();
}

/* boundaryFailure()'s part that holds a switch back whatever its destination: roaming off, an operator barrier, extra
 * options in Settings. */
QString
SiteRoaming::settingsFailure() const {
    if (!enabled()) {
        return QStringLiteral("disabled");
    }
    if (intentPending()) {
        return QStringLiteral("intent");
    }
    return globalExtras() ? QStringLiteral("extras") : QString();
}

/* boundaryFailure()'s part on the destination: still saved, in the group captured at the session's start, roamable and
 * not avoided. */
QString
SiteRoaming::destinationFailure() const {
    const int row = m_systems->rowForUid(m_tx.toUid);
    if (row < 0) {
        return QStringLiteral("removed");
    }
    if (m_systems->groupKey(row) != m_tx.groupKey || !m_systems->roamable(row)
        || m_systems->get(row).value(QStringLiteral("avoidSite")).toBool()) {
        return QStringLiteral("destination");
    }
    return QString();
}

SiteRoamingVerdict
SiteRoaming::judgeAgainst(qint64 nowMs, const QString& key, const QString& fromUid) {
    return m_core.evaluate(nowMs, candidates(key), fromUid.toStdString(),
                           streamReason() == NoReason ? usableMeasurement() : nullptr, settings());
}

/* Every measurement and tick still reaches the core while a switch is past its call wait, judged from the session the
 * switch leaves until the destination's start takes over (then from the destination's session): the evidence stays
 * continuous, so a fix that revokes the verdict or a fix gone stale is seen when it happens, and the start's checks
 * (startPlan()) derive the verdict from all of it. A tick never adds evidence: the core counts only a measurement it has
 * not seen. */
void
SiteRoaming::feedTransaction(qint64 nowMs) {
    if (m_tx.stage == Stage::Starting) {
        (void)judgeAgainst(nowMs, m_session.groupKey, m_session.uid);
    } else {
        (void)judgeAgainst(nowMs, m_tx.groupKey, m_tx.fromUid);
    }
}

/* Where a switch past its stop goes next, from the session state the host shows. */
void
SiteRoaming::progressTransaction() {
    if (m_host == nullptr) {
        finishTransaction(QStringLiteral("cancelled"), false);
        return;
    }
    const int phase = m_host->sessionState();
    if (m_tx.stage != Stage::Starting) {
        progressStop(phase);
        return;
    }
    m_tx.startShown = m_tx.startShown || phase != DecoderHost::Idle;
    if (phase == DecoderHost::Failed) {
        // A user stop never fails a session, so this is the destination's own failure (its card says why).
        endTransaction(QStringLiteral("failed"), false, true);
    } else if (phase == DecoderHost::Idle && (m_host->startSuperseded() || userStoppedSince(m_tx.stopGeneration))) {
        // The destination ended unrun because the user stopped listening or the app left the foreground while it
        // started (superseded), or the user stopped it after its start committed and before its initialization edge
        // (the generation moved): nothing failed. An epoch move with no supersede was seen first, at noteForeground().
        finishUnrunStart();
    } else if (phase == DecoderHost::Idle && m_tx.startShown) {
        // The destination ended without ever listening (a bootstrap that exits) and without failing.
        endTransaction(QStringLiteral("noListen"), true, true);
    } else if (m_session.initialized && m_session.id == m_tx.destSessionId) {
        finishTransaction(QStringLiteral("done"), false);
    }
}

/*
 * Before the destination's start (Stopping, WaitingForUsb), from the phase the host shows. Failed: the failure card says
 * why. Running again after the stop showed and before it went idle: the host gave up waiting for the service to
 * acknowledge the stop (its 2 s stop grace), so the old session runs on: the switch ends refused with no notice (nothing
 * stopped), its evidence goes, and roaming pauses on it until a fresh fix starts a new qualification. Idle is Main's to
 * act on (startPlan()); the Starting or Running of the roam's own start, which follows that, is noteStartResult()'s.
 */
void
SiteRoaming::progressStop(int phase) {
    if (phase == DecoderHost::Failed) {
        endTransaction(QStringLiteral("failed"), false, false);
        return;
    }
    if (phase == DecoderHost::Stopping) {
        m_tx.stopShown = true;
    } else if (phase == DecoderHost::Idle) {
        m_tx.stopDone = true;
    } else if (phase == DecoderHost::Running && m_tx.stage == Stage::Stopping && m_tx.stopShown && !m_tx.stopDone) {
        // The session runs on, so no cause leaves listening stopped: never the notice.
        endTransaction(QStringLiteral("timeout"), false, true);
        revoke(now());
    }
}

/* Whether the user asked to stop listening since @p generation was read, or the generation cannot be read now. */
bool
SiteRoaming::userStoppedSince(qint64 generation) const {
    const qint64 current = m_host != nullptr ? m_host->userStopGeneration() : -1;
    return current < 0 || current != generation;
}

/* Whether the app left the foreground since epoch @p epoch, or the epoch cannot be read now. */
bool
SiteRoaming::leftForegroundSince(qint64 epoch) const {
    const qint64 current = m_host != nullptr ? m_host->foregroundEpoch() : -1;
    return current < 0 || current != epoch;
}

/* The destination's start ended unrun, superseded: the user's own stop is silent, and leaving the foreground leaves
 * listening stopped with the notice (as a switch the foreground loss cancelled before the start). Never a refusal. */
void
SiteRoaming::finishUnrunStart() {
    endTransaction(QStringLiteral("superseded"), true, false);
}

/* The app left the foreground while the destination's start is on its way: it is retired unless it committed. One
 * retired, or one the service fenced itself (its START carries the epoch), leaves listening stopped with the notice;
 * one that committed runs on, and progressTransaction() follows it to its initialization edge. */
void
SiteRoaming::retireStartForForeground() {
    const bool evaluating = m_evaluating;
    m_evaluating = true;
    const bool retired = m_host == nullptr || m_host->retireRoamAttempt();
    m_evaluating = evaluating;
    m_again = true;
    if (retired) {
        endTransaction(QStringLiteral("foreground"), true, false);
    }
}

void
SiteRoaming::onHostSession() {
    const int phase = m_host != nullptr ? m_host->sessionState() : DecoderHost::Idle;
    if (phase == DecoderHost::Idle || phase == DecoderHost::Failed) {
        m_adoptedLive = false;
        if (m_sessionLive) {
            endSession();
        }
    } else if (phase == DecoderHost::Running && m_sessionLive && m_noticeSessionId != 0
               && m_noticeSessionId == m_session.id) {
        // #786: the session this roam's notice says stopped runs again (a stop the host gave up on after the roam was
        // cancelled): the notice is no longer true.
        clearRecoveryNotice();
    }
    evaluate();
}

void
SiteRoaming::endSession() {
    m_sessionLive = false;
    // The operator's commands belong to the session: one that never applied dies with it.
    clearIntents();
}

void
SiteRoaming::onEnabledChanged() {
    if (!enabled()) {
        // Roaming work only: the operator's pending commands stay.
        cancelTransaction(QStringLiteral("disabled"));
    }
    Q_EMIT enabledChanged();
    evaluate();
}

// ---- Foreground and location ----

void
SiteRoaming::noteForeground() {
    if (m_host == nullptr) {
        return;
    }
    const qint64 epoch = m_host->foregroundEpoch();
    if (epoch < 0) {
        return; // unreadable: the checks before each step refuse it; nothing to learn here
    }
    if (!m_epochKnown) {
        m_epochKnown = true;
        m_seenEpoch = epoch;
        return;
    }
    if (epoch != m_seenEpoch) {
        m_seenEpoch = epoch;
        onForegroundLost();
    }
}

void
SiteRoaming::onForegroundLost() {
    // A switch goes unless its destination already committed: its start would follow a moment the app was not on
    // screen for.
    if (active() && m_tx.stage != Stage::Starting) {
        cancelTransaction(QStringLiteral("foreground"));
    } else if (active()) {
        retireStartForForeground();
    }
    // A partly qualified candidate cannot finish on return: its evidence and every fix seen are forgotten, and a fix
    // from before (the same measurement polled again) is never fed once more.
    m_core.reset();
    m_callWait.reset();
    m_verdictUid.clear();
    m_qualifyingUid.clear();
    m_resetFloor = m_seenNs;
    forgetMeasurement();
    // The platform stopped the feed when the app left the foreground. The pass this runs in takes the lease again when
    // roaming needs it (the feed starts the feed once more, even while the app is still hidden: the platform keeps
    // that start until the app is visible), or gives the lease up.
    m_streamOn = false;
    m_streamState.clear();
}

void
SiteRoaming::onStreamUpdate(const QVariantMap& record) {
    if (!m_streamOn) {
        return;
    }
    m_streamState = record.value(QStringLiteral("state")).toString();
    forgetMeasurement();
    if (record.value(QStringLiteral("hasFix")).toBool()) {
        const QString provider = record.value(QStringLiteral("provider")).toString();
        const qint64 ns = record.value(QStringLiteral("measurementNs")).toLongLong();
        m_seenNs[provider] = std::max(m_seenNs.value(provider, 0), ns);
        // From before the reset: at or below its provider's floor, or another provider's relay of a fix seen then.
        bool before = m_resetFloor.contains(provider) && ns <= m_resetFloor.value(provider);
        for (auto it = m_resetFloor.cbegin(); !before && it != m_resetFloor.cend(); ++it) {
            before = it.key() != provider && site_roaming_same_fix(it.value(), ns);
        }
        if (!before) {
            m_measurement.id.provider = provider.toStdString();
            m_measurement.id.elapsed_realtime_ns = ns;
            m_measurement.lat = record.value(QStringLiteral("lat")).toDouble();
            m_measurement.lon = record.value(QStringLiteral("lon")).toDouble();
            m_measurement.has_accuracy = record.contains(QStringLiteral("accuracyM"));
            m_measurement.accuracy_m = record.value(QStringLiteral("accuracyM")).toDouble();
            m_measurement.fix_at_ms = record.value(QStringLiteral("fixAtMs")).toLongLong();
            m_hasMeasurement = true;
            m_sessionHadFix = true;
        }
    }
    // #786 r5: judged once the event loop turns, not here. The host polls the feed inside the UI tick before that tick
    // refreshes the decoder snapshot (UiController::tick()), so a fix judged here would act on the facts of the tick
    // before (a call that began since read as none). The tick's model refresh judges it at once if any fact changed.
    evaluateSoon();
}

/* The feed saw the foreground epoch move before any fix of the new epoch: a move roaming has not seen yet is handled
 * now, by the pass that notes the foreground, before the feed starts again. One roaming saw first is done already. */
void
SiteRoaming::onFeedForegroundMoved(qint64 epoch) {
    if (m_epochKnown && epoch == m_seenEpoch) {
        return;
    }
    evaluate();
}

/* The shared feed changed hosts: roaming's lease is gone, and it is taken again on the next pass that needs it. */
void
SiteRoaming::onFeedLeasesDropped() {
    m_streamOn = false;
    m_streamState.clear();
    forgetMeasurement();
    evaluateSoon();
}

void
SiteRoaming::evaluateSoon() {
    if (m_evaluateQueued) {
        return;
    }
    m_evaluateQueued = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            m_evaluateQueued = false;
            evaluate();
        },
        Qt::QueuedConnection);
}

/* Roaming's lease on the feed: taken when roaming needs location (the feed starts it, starts it again after a
 * foreground loss, or roaming joins the map's and reads where it stands), given up when it does not. The lease is
 * idempotent: one held since before a foreground loss is given up too, though roaming stopped following it then. */
void
SiteRoaming::ensureStream(bool need) {
    if (m_host == nullptr || m_feed == nullptr) {
        m_streamOn = false;
        return;
    }
    if (need && !m_streamOn) {
        if (m_locatingSinceMs < 0) {
            m_locatingSinceMs = now(); // the session's first wait for a fix
        }
        m_streamOn = true;
        m_streamState.clear();
        forgetMeasurement();
        if (!m_feed->acquire(LocationFeed::roamingLease())) {
            m_streamOn = false; // a feed with no host to lease from: nothing follows, and the next pass asks again
            return;
        }
        const QVariantMap standing = m_feed->record();
        if (!standing.isEmpty()) {
            onStreamUpdate(standing);
        }
    } else if (!need) {
        if (m_streamOn) {
            m_streamOn = false;
            m_streamState.clear();
            forgetMeasurement();
        }
        m_feed->release(LocationFeed::roamingLease());
    }
}

SiteRoaming::Reason
SiteRoaming::streamReason() const {
    if (m_streamState == QLatin1String("noPermission")) {
        return PermissionOff;
    }
    if (m_streamState == QLatin1String("approximateOnly")) {
        return ApproximateOnly;
    }
    return m_streamState == QLatin1String("locationOff") ? LocationOff : NoReason;
}

/* The reading goes with its flag: the privacy policy promises only the newest readings, and only while roaming runs. */
void
SiteRoaming::forgetMeasurement() {
    m_hasMeasurement = false;
    m_measurement = SiteRoamingMeasurement();
}

bool
SiteRoaming::holdsLocation() const {
    return m_hasMeasurement || !m_measurement.id.provider.empty();
}

const SiteRoamingMeasurement*
SiteRoaming::usableMeasurement() const {
    return m_hasMeasurement ? &m_measurement : nullptr;
}

/* The 1 s pass is for a session roaming judges (armed or paused: a fix ages, the epoch may move) and for a switch
 * under way; idle or off, every change that matters evaluates anyway. */
void
SiteRoaming::updateTimer() {
    const State shown = m_pending.state;
    const bool judging =
        m_sessionLive && (shown == Paused || shown == Following || shown == Candidate || shown == WaitingForCall);
    const bool run = m_host != nullptr && enabled() && (active() || judging);
    if (run && !m_timer.isActive()) {
        m_timer.start();
    } else if (!run && m_timer.isActive()) {
        m_timer.stop();
    }
}

// ---- Arming ----

SiteRoaming::Reason
SiteRoaming::armReason() const {
    const Reason shape = sessionShapeReason();
    return shape != NoReason ? shape : liveReason();
}

SiteRoaming::Reason
SiteRoaming::sessionShapeReason() const {
    if (!m_sessionLive) {
        if (!m_failedUid.isEmpty()) {
            return SwitchFailed;
        }
        return m_adoptedLive ? SessionAdopted : NoSession;
    }
    const int phase = m_host->sessionState();
    if (phase != DecoderHost::Running) {
        return phase == DecoderHost::Starting ? SessionStarting : NoSession;
    }
    if (m_session.stopObserved) {
        return NoSession;
    }
    if (!m_session.initialized) {
        return SessionStarting;
    }
    // The operator's hold makes the session Explore's for its length: it pauses roaming rather than idling it.
    if (m_session.kind == QLatin1String("saved") && m_facts.optionsKnown && m_facts.operatorTuned) {
        return OperatorTune;
    }
    if (m_session.kind != QLatin1String("saved") || m_facts.savedUid != m_session.uid) {
        return NotSavedSite;
    }
    return siteShapeReason();
}

/* What makes the running saved site one roaming can follow: no extra options anywhere, its row still in the group it
 * started in, roamable, beside at least one other roamable site. Extra options and the row's roamability count as the
 * session started (the decoder runs what it was started with) and as they are now (an edit applies at the next start,
 * and roaming must not restart into it): either one rules roaming out. */
SiteRoaming::Reason
SiteRoaming::siteShapeReason() const {
    if (globalExtras() || m_session.globalExtrasAtStart) {
        return GlobalExtraArgs;
    }
    const int row = m_systems->rowForUid(m_session.uid);
    if (row < 0 || m_session.groupKey.isEmpty() || m_systems->groupKey(row) != m_session.groupKey) {
        return NotGrouped;
    }
    if (!m_session.roamableAtStart || !m_systems->roamable(row)) {
        return SiteNotRoamable;
    }
    return roamableSiblings() < 2 ? FewSites : NoReason;
}

SiteRoaming::Reason
SiteRoaming::liveReason() const {
    if (!m_facts.optionsKnown) {
        return SessionStarting;
    }
    if (!m_facts.radioInput || m_facts.replayInput) {
        return NotTunable;
    }
    if (m_facts.scanKind != 0) {
        return Scanning;
    }
    if (intentPending()) {
        return PendingChange;
    }
    if (!m_facts.trunking) {
        return NotTrunking;
    }
    if (m_facts.operatorTuned) {
        return OperatorTune;
    }
    if (m_facts.heldTg != 0) {
        return TalkgroupHeld;
    }
    return m_session.manualHold ? ManualHold : NoReason;
}

bool
SiteRoaming::globalExtras() const {
    return m_prefs == nullptr || !m_prefs->extraArgs().trimmed().isEmpty();
}

int
SiteRoaming::roamableSiblings() const {
    const QVariantList rows = m_systems->groupRows(m_session.groupKey);
    return static_cast<int>(std::count_if(rows.cbegin(), rows.cend(),
                                          [this](const QVariant& row) { return m_systems->roamable(row.toInt()); }));
}

std::vector<SiteRoamingCandidate>
SiteRoaming::candidates(const QString& key) const {
    std::vector<SiteRoamingCandidate> out;
    if (m_systems == nullptr) {
        return out;
    }
    for (const QVariant& entry : m_systems->groupRows(key)) {
        const int row = entry.toInt();
        const QVariantMap site = m_systems->get(row);
        SiteRoamingCandidate candidate;
        candidate.uid = site.value(QStringLiteral("uid")).toString().toStdString();
        candidate.avoid = site.value(QStringLiteral("avoidSite")).toBool();
        candidate.has_position = site.value(QStringLiteral("hasSitePos")).toBool();
        candidate.lat = site.value(QStringLiteral("siteLat")).toDouble();
        candidate.lon = site.value(QStringLiteral("siteLon")).toDouble();
        candidate.roamable = m_systems->roamable(row);
        out.push_back(candidate);
    }
    return out;
}

SiteRoamingSettings
SiteRoaming::settings() const {
    SiteRoamingSettings out;
    if (m_prefs != nullptr) {
        out.max_km = m_prefs->roamingMaxDistanceKm();
        out.margin_km = m_prefs->roamingMarginKm();
    }
    return out;
}

/* A candidate's qualification begins (its first qualifying measurement): a user stop seen now ends the session's
 * roaming, so a Stop Qt has not shown yet is never absorbed into a switch. */
void
SiteRoaming::noteQualification() {
    const SiteRoamingEvidence& evidence = m_core.evidence();
    const QString uid = evidence.count > 0 ? QString::fromStdString(evidence.uid) : QString();
    if (uid.isEmpty() || uid == m_qualifyingUid) {
        m_qualifyingUid = uid;
        return;
    }
    m_qualifyingUid = uid;
    const qint64 generation = m_host->userStopGeneration();
    if (generation < 0 || generation != m_session.stopGeneration) {
        m_session.stopObserved = true;
    }
}

void
SiteRoaming::revoke(qint64 nowMs) {
    // Judged with no fix: the evidence goes, the fixes already seen stay seen.
    m_core.evaluate(nowMs, candidates(m_session.groupKey), m_session.uid.toStdString(), nullptr, settings());
    m_qualifyingUid.clear();
    m_verdictUid.clear();
    m_callWait.reset();
}

// ---- Operator intents ----

void
SiteRoaming::noteOperatorIntent(const QString& kind) {
    if (!m_sessionLive) {
        return;
    }
    if (kind == QLatin1String("hold") || kind == QLatin1String("release")) {
        m_holdIntent = {true, kind, false};
    } else if (kind == QLatin1String("manualTune") || kind == QLatin1String("releaseTuner")) {
        const bool baseline = m_tunerIntent.pending ? m_tunerIntent.baseline : m_facts.operatorTuned;
        m_tunerIntent = {true, kind, baseline};
    } else if (kind == QLatin1String("trunkingOn") || kind == QLatin1String("trunkingOff")) {
        m_trunkingIntent = {true, kind, false};
    } else {
        return;
    }
    // #786 r6: a command accepted before the destination's start ends the switch, in its call wait or in its stop (a
    // drain can keep the old session taking commands for minutes); the destination's own start is not the old
    // session's to fence, and its edge then waits for the command as any session does. This is the only barrier past
    // the call wait: once the stop is issued the decoder snapshot is the old session's last publication (engine.c's
    // ENGINE_OUT publish, then a teardown that publishes nothing), so a snapshot value alone never ends the switch.
    if (active() && m_tx.stage != Stage::Starting) {
        cancelTransaction(QStringLiteral("intent"));
    }
    revoke(now());
    evaluate();
}

bool
SiteRoaming::intentPending() const {
    return m_holdIntent.pending || m_tunerIntent.pending || m_trunkingIntent.pending;
}

void
SiteRoaming::clearIntents() {
    m_holdIntent = Intent();
    m_tunerIntent = Intent();
    m_trunkingIntent = Intent();
}

/* An intent is done when the decoder's snapshot shows its outcome; a delay alone never ends it. */
void
SiteRoaming::clearAppliedIntents() {
    if (!m_facts.optionsKnown) {
        return;
    }
    if (m_holdIntent.pending && (m_holdIntent.kind == QLatin1String("hold")) == (m_facts.heldTg != 0)) {
        m_holdIntent = Intent();
    }
    // A manual tune or a tuner release shows as the operator holding the tuner, already held or not.
    if (m_tunerIntent.pending && (m_facts.operatorTuned || m_facts.operatorTuned != m_tunerIntent.baseline)) {
        m_tunerIntent = Intent();
    }
    if (m_trunkingIntent.pending && m_facts.trunking == (m_trunkingIntent.kind == QLatin1String("trunkingOn"))) {
        m_trunkingIntent = Intent();
    }
}

// ---- Transaction ----

void
SiteRoaming::beginTransaction(const QString& uid) {
    m_tx = Transaction();
    m_tx.roamId = m_nextRoamId++;
    m_tx.fromUid = m_session.uid;
    m_tx.toUid = uid;
    m_tx.groupKey = m_session.groupKey;
    m_tx.sessionId = m_session.id;
    m_tx.stopGeneration = m_session.stopGeneration;
    m_tx.epoch = m_seenEpoch;
    m_tx.stage = Stage::WaitingForCall;
}

void
SiteRoaming::cancelTransaction(const QString& why) {
    if (!active()) {
        return;
    }
    const bool notice = !silent_reason(endCause(why));
    switch (m_tx.stage) {
        case Stage::WaitingForCall: endTransaction(why, false, false); return;
        case Stage::Starting: {
            // Handed to the host: its attempt is retired, unless it already went past that point and runs. The Idle a
            // winning retirement shows is this cancel's, not the destination ending unstarted: it is judged after.
            const bool evaluating = m_evaluating;
            m_evaluating = true;
            const bool retired = m_host == nullptr || m_host->retireRoamAttempt();
            m_evaluating = evaluating;
            m_again = true;
            finishTransaction(retired ? QStringLiteral("cancelled") : QStringLiteral("done"), retired && notice);
            return;
        }
        // Stopping or waiting for USB access: the session it left stays stopped, unless the host shows it running
        // again (a stop it gave up on) or failed (its card says why).
        default: endTransaction(why, leftStopped(), false); return;
    }
}

/* Whether the session a roam left shows stopped (stopping or idle), the premise of Home's notice. */
bool
SiteRoaming::leftStopped() const {
    const int phase = m_host != nullptr ? m_host->sessionState() : DecoderHost::Idle;
    return phase == DecoderHost::Idle || phase == DecoderHost::Stopping;
}

/* What endCause() makes of @p why now: a stronger cause present (a user stop, roaming off, the foreground lost) wins. */
QString
SiteRoaming::endCause(const QString& why) const {
    QString cause = why;
    const auto stronger = [&cause](const QString& other) {
        if (end_rank(other) < end_rank(cause)) {
            cause = other;
        }
    };
    if (active() && userStoppedSince(m_tx.stopGeneration)) {
        stronger(QStringLiteral("userStop"));
    }
    if (!enabled()) {
        stronger(QStringLiteral("disabled"));
    }
    if (active() && leftForegroundSince(m_tx.epoch)) {
        stronger(QStringLiteral("foreground"));
    }
    return cause;
}

/*
 * Every end of a roam but "done" comes here (#786 r5). @p why is what ended it at this point; endCause() lets a
 * stronger cause present now win. @p listeningStopped is whether the session the roam left stays stopped because of
 * it, the premise of Home's notice, which then shows unless the cause is one the user's action or a card already
 * explains. @p refusal makes it a refusal (roaming pauses on the site: "Could not switch to %1") when its own cause
 * stands; a stronger one makes it a cancel.
 */
void
SiteRoaming::endTransaction(const QString& why, bool listeningStopped, bool refusal) {
    const QString cause = endCause(why);
    if (refusal && cause == why) {
        m_failedUid = m_tx.toUid;
        finishTransaction(QStringLiteral("refused"), listeningStopped);
        return;
    }
    finishTransaction(QStringLiteral("cancelled"), listeningStopped && !silent_reason(cause));
}

void
SiteRoaming::finishTransaction(const QString& outcome, bool notice) {
    if (!active()) {
        return;
    }
    const Transaction tx = m_tx;
    m_tx = Transaction();
    m_callWait.reset();
    m_verdictUid.clear();
    if (notice) {
        m_noticeText = tr("Roaming did not switch to %1. Listening stopped.").arg(siteName(tx.toUid));
        m_noticeAction = tr("Listen to %1").arg(siteName(tx.fromUid));
        m_noticeUid = tx.fromUid;
        m_noticeToUid = tx.toUid;
        m_noticeSessionId = tx.sessionId;
        Q_EMIT recoveryNoticeChanged();
    }
    if (m_evaluating) {
        m_again = true;
    }
    Q_EMIT transactionEnded(tx.roamId, outcome);
}

void
SiteRoaming::cancel(const QString& why) {
    if (!active()) {
        return;
    }
    // What the host already shows decides first: a destination that failed is refused, not cancelled.
    if (m_tx.stage != Stage::WaitingForCall) {
        progressTransaction();
    }
    cancelTransaction(why.isEmpty() ? QStringLiteral("start") : why);
    evaluate();
}

QVariantMap
SiteRoaming::startPlan(qint64 roamId) {
    QVariantMap refused{{QStringLiteral("ok"), false}};
    const auto turn = [this, roamId] {
        return active() && m_tx.roamId == roamId
               && (m_tx.stage == Stage::Stopping || m_tx.stage == Stage::WaitingForUsb);
    };
    if (!turn()) {
        return refused;
    }
    // Main asks only once the session it leaves is idle. Asked at any other moment, no one will ask again: the roam
    // ends rather than stay stranded in its stop.
    if (m_host == nullptr || m_host->sessionState() != DecoderHost::Idle) {
        cancelTransaction(QStringLiteral("turn"));
        evaluate();
        return refused;
    }
    m_tx.stopDone = true; // the start that follows may show Running before noteStartResult() moves the stage on
    noteForeground();
    const QString failure = turn() ? boundaryFailure(now()) : QStringLiteral("foreground");
    if (!failure.isEmpty()) {
        cancelTransaction(failure);
        evaluate();
        return refused;
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("uid"), m_tx.toUid},
            {QStringLiteral("expectedStopGeneration"), m_tx.stopGeneration},
            {QStringLiteral("expectedForegroundEpoch"), m_tx.epoch}};
}

void
SiteRoaming::noteWaitingForUsb(qint64 roamId) {
    if (active() && m_tx.roamId == roamId && (m_tx.stage == Stage::Stopping || m_tx.stage == Stage::WaitingForUsb)) {
        m_tx.stage = Stage::WaitingForUsb;
        evaluate();
    }
}

void
SiteRoaming::noteUsbDenied(qint64 roamId) {
    if (active() && m_tx.roamId == roamId && m_tx.stage == Stage::WaitingForUsb) {
        endTransaction(QStringLiteral("usbDenied"), false, true);
        evaluate();
    }
}

void
SiteRoaming::noteStartResult(qint64 roamId, const QVariantMap& result) {
    if (!active() || m_tx.roamId != roamId || (m_tx.stage != Stage::Stopping && m_tx.stage != Stage::WaitingForUsb)) {
        return;
    }
    if (result.value(QStringLiteral("superseded")).toBool()) {
        // The user stopped listening, or the app left the foreground, after the last read: no card.
        finishUnrunStart();
    } else if (!result.value(QStringLiteral("ok")).toBool() || !result.value(QStringLiteral("started")).toBool()) {
        // Main shows the failure card, as for any refused start.
        endTransaction(QStringLiteral("startRefused"), false, true);
    } else {
        m_tx.stage = Stage::Starting;
    }
    evaluate();
}

// ---- Sessions ----

void
SiteRoaming::noteSessionStart(const QString& kind, const QString& uid, qint64 roamId) {
    const bool own = active() && roamId != 0 && roamId == m_tx.roamId && m_tx.stage == Stage::Starting;
    if (!own) {
        cancelTransaction(QStringLiteral("start"));
    }
    Session next;
    next.id = m_session.id + 1;
    next.kind = kind;
    next.uid = uid;
    const int row = m_systems != nullptr ? m_systems->rowForUid(uid) : -1;
    next.groupKey = row >= 0 ? m_systems->groupKey(row) : QString();
    // A roam's destination keeps the generation its switch carried, so a user stop since then is never absorbed.
    next.stopGeneration = own ? m_tx.stopGeneration : (m_host != nullptr ? m_host->userStopGeneration() : -1);
    next.stopObserved = next.stopGeneration < 0;
    next.globalExtrasAtStart = globalExtras();
    next.roamableAtStart = row >= 0 && m_systems->roamable(row);
    next.manualHold = !own && enabled() && !uid.isEmpty() && uid == m_pendingChoiceUid;
    m_session = next;
    m_sessionLive = true;
    m_adoptedLive = false;
    m_pendingChoiceUid.clear();
    clearIntents();
    m_failedUid.clear();
    m_qualifyingUid.clear();
    m_verdictUid.clear();
    m_callWait.reset();
    m_sessionHadFix = false;
    m_locatingSinceMs = -1;
    if (own) {
        m_tx.destSessionId = next.id;
    }
    clearRecoveryNotice();
    evaluate();
}

void
SiteRoaming::noteSessionInitialized() {
    if (!m_sessionLive) {
        return;
    }
    m_session.initialized = true;
    if (active() && m_tx.stage == Stage::Starting && m_tx.destSessionId == m_session.id) {
        finishTransaction(QStringLiteral("done"), false);
    }
    // #786 r5: roaming arms at this edge, which the host signals inside its refresh, before the tick's decoder snapshot:
    // nothing judged before it counts (a roam's destination was judged while it started), so no switch is due on
    // facts from before the edge. The fixes already seen stay seen.
    revoke(now());
    evaluate();
}

void
SiteRoaming::noteSiteChoice(const QString& uid) {
    // A session roaming judges is one it shows as neither Off nor Idle: following, checking, switching, holding or
    // paused (a hand-tuned saved site among them). One it idles on (a scan, an input it cannot retune) holds nothing.
    const State shown = m_view.state;
    const bool judged = enabled() && available() && m_sessionLive && shown != Off && shown != Idle;
    m_pendingChoiceUid = judged ? uid : QString();
}

void
SiteRoaming::forgetSiteChoice() {
    m_pendingChoiceUid.clear();
}

void
SiteRoaming::noteSessionAdopted(const QString& kind) {
    if (m_sessionLive || m_host == nullptr || m_host->sessionState() != DecoderHost::Running) {
        return;
    }
    m_adoptedLive = kind == QLatin1String("saved");
    evaluate();
}

void
SiteRoaming::resumeRoaming() {
    if (m_session.manualHold) {
        m_session.manualHold = false;
        evaluate();
    }
}

void
SiteRoaming::setEnabledByUser(bool on) {
    if (m_prefs == nullptr) {
        return;
    }
    m_prefs->setRoamingEnabled(on);
    if (on && available()) {
        m_host->requestRoamingPermission(m_nextPermissionId++);
    }
}

void
SiteRoaming::performAction() {
    switch (action()) {
        case OpenAppSettings:
            if (m_host != nullptr) {
                m_host->openAppLocationSettings();
            }
            break;
        case TurnOnLocation:
            if (m_host != nullptr) {
                m_host->openLocationSourceSettings();
            }
            break;
        case ResumeRoaming: resumeRoaming(); break;
        default: break;
    }
}

void
SiteRoaming::clearRecoveryNotice() {
    if (m_noticeText.isEmpty()) {
        return;
    }
    m_noticeText.clear();
    m_noticeAction.clear();
    m_noticeUid.clear();
    m_noticeToUid.clear();
    m_noticeSessionId = 0;
    Q_EMIT recoveryNoticeChanged();
}

// ---- Facts ----

QVariantMap
SiteRoaming::facts() const {
    return m_factsMap;
}

void
SiteRoaming::setFacts(const QVariantMap& readings) {
    if (readings == m_factsMap) {
        return;
    }
    m_factsMap = readings;
    Facts next;
    next.savedUid = readings.value(QStringLiteral("savedUid")).toString();
    next.trunking = readings.value(QStringLiteral("trunking")).toBool();
    next.radioInput = readings.value(QStringLiteral("radioInput")).toBool();
    next.replayInput = readings.value(QStringLiteral("replayInput")).toBool();
    next.operatorTuned = readings.value(QStringLiteral("operatorTuned")).toBool();
    next.callBusy = readings.value(QStringLiteral("callBusy")).toBool();
    next.optionsKnown = readings.value(QStringLiteral("optionsKnown")).toBool();
    next.scanKind = readings.value(QStringLiteral("scanKind")).toInt();
    next.heldTg = readings.value(QStringLiteral("heldTg")).toULongLong();
    m_facts = std::move(next);
    Q_EMIT factsChanged();
    evaluate();
}

// ---- View ----

void
SiteRoaming::setView(State shown, Reason why, const QString& arg) {
    m_pending.state = shown;
    m_pending.reason = why;
    m_pending.reasonArg = arg;
    m_pending.candidateUid.clear();
    m_pending.destinationUid.clear();
}

void
SiteRoaming::publish() {
    View next = m_pending;
    next.available = available();
    next.transactionActive = active();
    next.currentUid = m_sessionLive && m_session.kind == QLatin1String("saved") ? m_session.uid : QString();
    next.groupKey = next.state != Off && m_sessionLive ? m_session.groupKey : QString();
    next.fix.clear();
    const qint64 t = now();
    if (m_streamOn && m_hasMeasurement && streamReason() == NoReason && m_measurement.fix_at_ms <= t
        && t - m_measurement.fix_at_ms <= kMaxFixAgeMs && m_measurement.has_accuracy
        && m_measurement.accuracy_m <= kMaxAccuracyM) {
        next.fix = {{QStringLiteral("lat"), m_measurement.lat},
                    {QStringLiteral("lon"), m_measurement.lon},
                    {QStringLiteral("accuracyM"), m_measurement.accuracy_m}};
    }
    next.locating = next.state == Paused && next.reason == NoFix && locating(t);
    next.currentKm = distanceTo(next.fix, next.currentUid);
    next.candidateKm = distanceTo(next.fix, next.candidateUid);
    deriveLabels(next);
    if (!(next == m_view)) {
        m_view = std::move(next);
        Q_EMIT changed();
    }
    refreshNotice();
}

/* What QML reads that derives from @p view's identities and the saved systems: the names saved now, the unit shown. */
void
SiteRoaming::deriveLabels(View& view) const {
    // The reason's argument names a site or a distance.
    if (view.reason == SwitchFailed) {
        view.reasonArg = siteName(m_failedUid);
    } else if (view.reason == OutOfRange) {
        view.reasonArg = distanceText(settings().max_km);
    }
    view.currentName = siteName(view.currentUid);
    view.candidateName = siteName(view.candidateUid);
    view.destinationName = siteName(view.destinationUid);
    view.systemName = systemNameOf(view.groupKey);
    view.statusText = statusTextOf(view);
    view.headerShortText = headerShortTextOf(view);
    view.headerText = headerTextOf(view);
    view.action = actionOf(view.reason);
}

/* The recovery notice names two sites: derived again from the names saved now, announced only when it changed. */
void
SiteRoaming::refreshNotice() {
    if (m_noticeUid.isEmpty()) {
        return;
    }
    const QString text = tr("Roaming did not switch to %1. Listening stopped.").arg(siteName(m_noticeToUid));
    const QString listen = tr("Listen to %1").arg(siteName(m_noticeUid));
    if (text != m_noticeText || listen != m_noticeAction) {
        m_noticeText = text;
        m_noticeAction = listen;
        Q_EMIT recoveryNoticeChanged();
    }
}

/* The session has had no fix yet and first waited for one less than kLocatingMs before @p nowMs. */
bool
SiteRoaming::locating(qint64 nowMs) const {
    return !m_sessionHadFix && m_locatingSinceMs >= 0 && nowMs - m_locatingSinceMs < kLocatingMs;
}

double
SiteRoaming::distanceTo(const QVariantMap& at, const QString& uid) const {
    if (uid.isEmpty() || at.isEmpty() || m_systems == nullptr) {
        return -1;
    }
    const QVariantMap site = m_systems->getByUid(uid);
    if (!site.value(QStringLiteral("hasSitePos")).toBool()) {
        return -1;
    }
    return site_distance_km(at.value(QStringLiteral("lat")).toDouble(), at.value(QStringLiteral("lon")).toDouble(),
                            site.value(QStringLiteral("siteLat")).toDouble(),
                            site.value(QStringLiteral("siteLon")).toDouble());
}

QString
SiteRoaming::siteName(const QString& uid) const {
    if (m_systems == nullptr || uid.isEmpty()) {
        return QString();
    }
    const QVariantMap site = m_systems->getByUid(uid);
    const QString name = site.value(QStringLiteral("siteName")).toString();
    return name.isEmpty() ? site.value(QStringLiteral("name")).toString() : name;
}

QString
SiteRoaming::distanceText(double km) const {
    const bool metric = m_prefs != nullptr && m_prefs->metricUnits();
    const double shown = std::round((metric ? km : km / kKmPerMile) * 10) / 10;
    return metric ? tr("%1 km").arg(shown) : tr("%1 mi").arg(shown);
}

QString
SiteRoaming::reasonText(const View& view) const {
    const QString text = tr(kReasonTexts[view.reason]);
    return text.contains(QLatin1String("%1")) ? text.arg(view.reasonArg) : text;
}

SiteRoaming::State
SiteRoaming::state() const {
    return m_view.state;
}

SiteRoaming::Reason
SiteRoaming::reason() const {
    return m_view.reason;
}

QString
SiteRoaming::statusText() const {
    return m_view.statusText;
}

QString
SiteRoaming::statusTextOf(const View& view) const {
    switch (view.state) {
        case Off: return QString();
        case Following: return tr("Staying on this site until another is clearly closer");
        case Candidate: return tr("%1 is closer. Checking before switching").arg(view.candidateName);
        case WaitingForCall:
            return tr("Waiting for this call to end before switching to %1 (up to 90 seconds)")
                .arg(view.destinationName);
        case Switching: return tr("Switching to %1…").arg(view.destinationName);
        default: return reasonText(view);
    }
}

QString
SiteRoaming::headerShortText() const {
    return m_view.headerShortText;
}

QString
SiteRoaming::headerShortTextOf(const View& view) const {
    switch (view.state) {
        case Following: return tr("STAYING");
        case Candidate: return tr("CHECKING");
        case WaitingForCall: return tr("WAITING");
        case Switching: return tr("SWITCHING");
        case Holding: return tr("HOLDING");
        case Paused: return view.locating ? tr("LOCATING") : tr("PAUSED");
        default: return QString();
    }
}

QString
SiteRoaming::headerText() const {
    return m_view.headerText;
}

QString
SiteRoaming::headerTextOf(const View& view) const {
    QString words = headerShortTextOf(view);
    if (view.state == WaitingForCall) {
        words = tr("WAITING FOR CALL END");
    } else if (view.state == Paused && !view.locating && kPauseWords[view.reason][0] != '\0') {
        words = tr("PAUSED: %1").arg(tr(kPauseWords[view.reason]));
    }
    return words.isEmpty() ? QString() : tr("%1 · GPS ROAMING").arg(words);
}

QString
SiteRoaming::systemName() const {
    return m_view.systemName;
}

QString
SiteRoaming::systemNameOf(const QString& key) const {
    if (m_systems == nullptr || key.isEmpty()) {
        return QString();
    }
    const QVariantList rows = m_systems->groupRows(key);
    return rows.isEmpty() ? QString() : site_group_title(m_systems->get(rows.first().toInt()));
}

QString
SiteRoaming::globalExtraArgsText() const {
    return tr(kReasonTexts[GlobalExtraArgs]);
}

SiteRoaming::Action
SiteRoaming::action() const {
    return m_view.action;
}

SiteRoaming::Action
SiteRoaming::actionOf(Reason reason) {
    switch (reason) {
        case PermissionOff:
        case ApproximateOnly: return OpenAppSettings;
        case LocationOff: return TurnOnLocation;
        case ManualHold: return ResumeRoaming;
        default: return NoAction;
    }
}

QString
SiteRoaming::groupKey() const {
    return m_view.groupKey;
}

QString
SiteRoaming::currentUid() const {
    return m_view.currentUid;
}

QString
SiteRoaming::currentName() const {
    return m_view.currentName;
}

double
SiteRoaming::currentDistanceKm() const {
    return m_view.currentKm;
}

QString
SiteRoaming::candidateUid() const {
    return m_view.candidateUid;
}

QString
SiteRoaming::candidateName() const {
    return m_view.candidateName;
}

double
SiteRoaming::candidateDistanceKm() const {
    return m_view.candidateKm;
}

QString
SiteRoaming::destinationUid() const {
    return m_view.destinationUid;
}

QString
SiteRoaming::destinationName() const {
    return m_view.destinationName;
}

bool
SiteRoaming::transactionActive() const {
    return m_view.transactionActive;
}

QVariantMap
SiteRoaming::fix() const {
    return m_view.fix;
}

QString
SiteRoaming::recoveryNotice() const {
    return m_noticeText;
}

QString
SiteRoaming::recoveryActionText() const {
    return m_noticeAction;
}

QString
SiteRoaming::recoveryUid() const {
    return m_noticeUid;
}

} // namespace dsd_qt
