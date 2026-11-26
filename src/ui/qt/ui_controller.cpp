// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <QDebug>
#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>
#include "auto_start_attachment.h"
#include "auto_start_policy.h"
#include "command_bridge.h"
#include "diagnostics_log.h"
#include "ui_controller.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QVariantList>
#include <Qt>
// WP0's C export payload uses a flexible array; only its fixed header is read in C++.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include <dsd-neo/app_control/commands.h>
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/runtime/decode_clock.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "call_history_model.h"
#include "decoder_host.h"
#include "dmr_channels_model.h"
#include "heard_talkgroup_store.h"
#include "metrics_model.h"
#include "p25_network_model.h" // WP-F2
#include "position_model.h"
#include "recording_status.h"
#include "talkgroup_list_model.h"
#include "unit_activity_model.h"

namespace dsd_qt {

namespace {

/* Deliberately battery-friendly: the terminal UI polls at 15 ms with a 66 ms draw
 * cap, but a phone has no reason to redraw a metrics panel faster than the eye
 * notices. Raise this when the spectrum view lands. */
constexpr int kDefaultPollIntervalMs = 250;

} // namespace

UiController::UiController(DecoderHost* host, MetricsModel* metrics, CallHistoryModel* history,
                           TalkgroupListModel* talkgroups, QObject* parent)
    : QObject(parent), m_host(host), m_metrics(metrics), m_history(history), m_talkgroups(talkgroups) {
    m_timer.setInterval(kDefaultPollIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &UiController::tick);
    if (m_host != nullptr) {
        m_session = m_host->sessionState();
        connect(m_host, &DecoderHost::sessionStateChanged, this, &UiController::onSessionStateChanged);
    }
    if (auto* app = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
        connect(app, &QGuiApplication::applicationStateChanged, this, &UiController::onApplicationStateChanged);
        // A controller made while the app is already off screen withholds keys from its first poll on: no transition
        // will come to say so.
        onApplicationStateChanged(app->applicationState());
    }
}

UiController::~UiController() {
    // The GUI thread reads no more snapshots through this controller.
    wipeSnapshotKeys();
    withholdSnapshotKeys(false);
}

void
UiController::withholdSnapshotKeys(bool withhold) {
    dsd_app_snapshot_consumer_withhold_keys(withhold ? 1 : 0);
    dsd_app_opts_snapshot_consumer_withhold_keys(withhold ? 1 : 0);
}

void
UiController::wipeSnapshotKeys() {
    dsd_app_snapshot_consumer_wipe_keys();
    dsd_app_opts_snapshot_consumer_wipe_keys();
}

void
UiController::onApplicationStateChanged(Qt::ApplicationState state) {
    // Off screen the timer may still tick: its reads copy no key back, until the app is on screen again.
    withholdSnapshotKeys(state == Qt::ApplicationSuspended || state == Qt::ApplicationHidden);
}

// WP-S2: attachment consumption and target lookup happen outside this pure decision.
void
UiController::requestAutoStart(bool enabled, bool onboardingDone, const QString& kind, const QString& uid,
                               const QVariantMap& target, const QString& attachedKind, const QStringList& companions) {
    const bool exists = !uid.isEmpty() && target.value(QStringLiteral("uid")).toString() == uid
                        && (kind == QStringLiteral("saved") || kind == QStringLiteral("scan"));
    // #647 D3a: the receiver that just attached, not whatever else is on the bus, decides.
    const bool kindMatches = autoStartKindMatches(target.value(QStringLiteral("sourceType")).toString(), attachedKind);
    if (m_host
        && autoStartAllowed(enabled, onboardingDone, m_host->sessionState(), m_autoStartBlocked, exists, kindMatches)) {
        Q_EMIT autoStartRequested(kind, uid, kind == QStringLiteral("scan") ? companions : QStringList());
    }
}

int
UiController::pollIntervalMs() const {
    return m_timer.interval();
}

void
UiController::setPollIntervalMs(int interval_ms) {
    const int clamped = (interval_ms < 50) ? 50 : interval_ms;
    if (clamped == m_timer.interval()) {
        return;
    }
    m_timer.setInterval(clamped);
    Q_EMIT pollIntervalChanged();
}

void
UiController::start() {
    m_timer.start();
}

void
UiController::stop() {
    m_timer.stop();
}

void
UiController::flushHistory() {
    if (m_history == nullptr) {
        return;
    }
    const dsd_opts* opts_snapshot = dsd_app_get_latest_opts_snapshot();
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();
    m_history->refresh(snapshot, opts_snapshot);
}

void
UiController::flushHistoryBeforeStart() {
    if (m_history == nullptr) {
        return;
    }
    m_history->refresh(dsd_app_get_latest_snapshot(), dsd_app_get_latest_opts_snapshot(), false);
}

void
UiController::onSessionStateChanged() {
    const DecoderHost::SessionState previous = m_session;
    m_session = m_host->sessionState();
    if (m_session == previous) {
        return;
    }

    // The host can deliver a USB attachment synchronously just after publishing
    // Idle. Associate its last export before that attachment builds a new session.
    if (m_session == DecoderHost::Idle || m_session == DecoderHost::Failed) {
        pollCommandResults();
        // Not saved here: the session's last rows are ingested later in the tick.
        m_historySaveOwed = true;
    }

    if (m_session == DecoderHost::Starting && m_diagnostics != nullptr) {
        m_diagnostics->markSessionStarting();
    }

    /* Entering a session: the incoming run owns the screen, so the readings clear
     * before the monitoring view appears. Leaving one: the metrics describe a
     * decoder that no longer exists. See MetricsModel::clear(). Diagnostics are
     * exempt: process history must retain what preceded a stopped or failed session. */
    if (m_session == DecoderHost::Starting || m_session == DecoderHost::Idle || m_session == DecoderHost::Failed) {
        m_active_ordinal = 0;
        clearLiveModels();
    }
}

void
UiController::clearLiveModels() {
    if (m_commands) {
        m_commands->clearSessionInputs();
    }
    if (m_network != nullptr) {
        m_network->clear();
    }
    if (m_dmrChannels != nullptr) {
        m_dmrChannels->clear();
    }
    if (m_metrics != nullptr) {
        m_metrics->clear();
    }
    if (m_talkgroups != nullptr) {
        m_talkgroups->clear();
    }
    // One log spans a session, scan hops included; a new session starts its own.
    if (m_unitActivity != nullptr) {
        m_unitActivity->clear();
    }
    // Later live models join this list. History is durable across sessions and
    // targets; clearing it here would discard calls the operator asked to keep.
    // Diagnostics are also exempt so a stopped/failed session retains its context.
}

QString
UiController::talkgroupExportSequenceNow() const {
    dsd_app_tg_export_result newest = {};
    const quint64 held = dsd_app_tg_export_result_get(&newest) != 0 ? newest.sequence : 0U;
    return QString::number(std::max<quint64>(held, m_talkgroupExportSequence));
}

void
UiController::pollTalkgroupExport() {
    // WP-D1: retained export completion is independent of redraw and host lifecycle. The newest one first, so an
    // idle tick reads one result and allocates nothing.
    dsd_app_tg_export_result newest = {};
    if (dsd_app_tg_export_result_get(&newest) == 0 || newest.sequence == m_talkgroupExportSequence) {
        return;
    }
    // Issue #903: every completion since the last poll, so a second export finishing before the poll is not lost.
    std::vector<dsd_app_tg_export_result> read(DSD_APP_RESULT_HISTORY);
    const int count = dsd_app_tg_export_results_since(m_talkgroupExportSequence, read.data(), DSD_APP_RESULT_HISTORY);
    QVariantList results;
    quint64 missedThrough = 0;
    // One published after the read above is in this batch too: the next poll starts after it.
    quint64 last = newest.sequence;
    for (int i = 0; i < count; ++i) {
        const dsd_app_tg_export_result& result = read[static_cast<size_t>(i)];
        if (i == 0 && result.sequence > m_talkgroupExportSequence + 1U) {
            // Older than the history keeps: those completions are gone, and the flow must say so.
            missedThrough = result.sequence - 1U;
        }
        results.append(QVariantMap{{QStringLiteral("sequence"), QString::number(result.sequence)},
                                   {QStringLiteral("policyContext"), QString::number(result.policy_context)},
                                   {QStringLiteral("policyGeneration"), result.policy_generation},
                                   {QStringLiteral("path"), QString::fromUtf8(result.path)},
                                   {QStringLiteral("success"), result.success == 1},
                                   {QStringLiteral("tuningMoved"), result.tuning_moved != 0}});
        last = std::max<quint64>(last, result.sequence);
    }
    m_talkgroupExportSequence = last;
    m_talkgroupExportResults = {{QStringLiteral("results"), results},
                                {QStringLiteral("missedThrough"), QString::number(missedThrough)}};
    Q_EMIT talkgroupExportResultsChanged();
}

void
UiController::pollDecryptionResult() {
    dsd_app_decryption_result result = {};
    if (!dsd_app_decryption_result_get(&result) || result.sequence == m_decryptionSequence) {
        return;
    }
    m_decryptionSequence = result.sequence;
    m_decryptionResult = {{"sequence", QString::number(result.sequence)},
                          {"requestId", QString::number(result.request_id)},
                          {"session", QString::number(result.session_generation)},
                          {"status", result.status},
                          {"scope", result.scope}};
    if (m_commands) {
        m_commands->acknowledgeDecryptionResult(result.request_id);
    }
    Q_EMIT decryptionResultChanged();
}

void
UiController::pollCommandResults() {
    pollTalkgroupExport();
    pollDecryptionResult();
    if (m_commands) {
        m_commands->pollDmrChannelResults();
    }
}

void
UiController::setDmrChannels(DmrChannelsModel* model) {
    if (m_dmrChannels != nullptr) {
        disconnect(m_dmrChannels, nullptr, this, nullptr);
    }
    m_dmrChannels = model;
    if (m_dmrChannels != nullptr) {
        connect(m_dmrChannels, &DmrChannelsModel::activeChanged, this, &UiController::onDmrChannelsActiveChanged);
    }
}

void
UiController::onDmrChannelsActiveChanged() {
    if (m_dmrChannels == nullptr || !m_dmrChannels->active() || !sessionIsLive()) {
        return;
    }
    // The GUI thread's one reader (this file), between ticks: the same held buffers a tick reads.
    m_dmrChannels->refresh(dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot(),
                           dsd_app_command_session_generation());
}

void
UiController::setCommandBridge(CommandBridge* commands) {
    m_commands = commands;
}

bool
UiController::invalidateForTarget(const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    if (!snapshot || snapshot->trunk_scan_active_ordinal == m_active_ordinal) {
        return false;
    }
    m_active_ordinal = snapshot->trunk_scan_active_ordinal;
    // The new target can be quiet; waiting for a new call would let the old
    // target's held sync/identity continue to caption this frequency. The metrics
    // are not cleared here: refreshModels() drops their latches inside the same
    // publication, so no reading passes through its default on a hop (#782).
    // Nor does the network: an open sheet gets the new target's rows in one
    // replacement (an empty list first would rebuild every row it shows).
    if (m_network) {
        m_network->replaceForTarget(sessionIsLive() ? snapshot : nullptr);
    }
    if (m_dmrChannels) {
        m_dmrChannels->replaceForTarget(opts_snapshot, sessionIsLive() ? snapshot : nullptr,
                                        dsd_app_command_session_generation());
    }
    if (m_talkgroups) {
        m_talkgroups->invalidateForTarget();
    }
    return true;
}

bool
UiController::sessionIsLive() const {
    return !m_host || m_session == DecoderHost::Running || m_session == DecoderHost::Stopping;
}

void
UiController::tick() {
    pollCommandResults();

    if (m_diagnostics) {
        m_diagnostics->refresh();
    }
    if (m_recordingStatus != nullptr) {
        m_recordingStatus->refresh();
    }
    /* Host state is not published through the redraw flag: a stopped engine raises
     * nothing, and "stopped" is exactly what the UI must notice. */
    if (m_host != nullptr) {
        m_host->refresh();
        // Terminal lifecycle publication can race the first result poll. Consume
        // its final retained export before queued QML restarts build their argv.
        pollCommandResults();
    }

    const bool sessionEnded = std::exchange(m_historySaveOwed, false);
    refreshModels();
    if (sessionEnded && m_history != nullptr) {
        /* A stop is often the last thing before the app goes away, and a full log's
         * save waits up to a minute. Ingest the final snapshot even on a tick without a
         * redraw (a row the refresh above read is not read twice), then write it now. */
        flushHistory();
        m_history->saveNow();
    }
    if (sessionEnded && m_positions != nullptr) {
        // The run's final publish carries its last reports; read them now, as History reads its last rows.
        m_positions->poll();
    }
    /* Back from the background: the first ingest takes over from History's background tap, whether or
     * not this tick had a redraw to ingest (a session that ended while the app was away publishes none). */
    if (m_history != nullptr && m_history->backgroundCaptureHandoffOwed()) {
        flushHistory();
    }
    flushHeardTalkgroups(sessionEnded);
}

void
UiController::flushHeardTalkgroups(bool sessionEnded) {
    if (m_heardTalkgroups == nullptr) {
        return;
    }
    // After History's ingest above, which is what feeds the store. Real time: the interval paces writes.
    if (sessionEnded) {
        m_heardTalkgroups->flushNow();
    } else {
        m_heardTalkgroups->flushIfDue(static_cast<qint64>(dsd_realtime_mono_ms()));
    }
}

void
UiController::refreshModels() {
    /* Positions are read on every tick, redraw or not: a run publishes them with any snapshot, and a stopped one
     * leaves its last reports published for this read. */
    if (m_positions != nullptr) {
        m_positions->poll();
    }
    const bool redraw = dsd_app_frontend_redraw_consume() != 0;
    const bool live = sessionIsLive();
    // Once this session has supplied its first redraw, age its held deadlines on
    // every UI tick. Input can stall without another redraw; scan countdowns and
    // the short sync hold must still expire. The countdowns age on the decode clock
    // the engine stamped them with, so in a replay they freeze while samples stop;
    // the sync hold is real time and expires either way. Keep the startup admission gate.
    if (!redraw && !(live && m_metrics && m_metrics->optionsKnown())) {
        return;
    }

    /* Consumed once, here, and handed to the models. Each accessor deep-copies
     * whenever the publisher has moved on, so fetching per model would let a publish
     * land mid-frame and leave the status card describing one generation and the
     * event list another — and would repeat the copy for every fetch. */
    const dsd_opts* opts_snapshot = dsd_app_get_latest_opts_snapshot();
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();

    const bool new_target = invalidateForTarget(opts_snapshot, snapshot);
    if (m_metrics != nullptr) {
        if (live) {
            m_metrics->refresh(opts_snapshot, snapshot, new_target);
        } else if (new_target) {
            m_metrics->clear();
        }
    }
    if (redraw) {
        refreshRedrawModels(opts_snapshot, snapshot, live, new_target);
    }
}

void
UiController::refreshRedrawModels(const dsd_opts* opts_snapshot, const dsd_state* snapshot, bool live,
                                  bool new_target) {
    if (live && m_network != nullptr && !new_target) { // A hop has published this snapshot's network already.
        m_network->refresh(snapshot);
    }
    if (live && m_dmrChannels != nullptr && !new_target) { // As the network: a hop published these rows already.
        m_dmrChannels->refresh(opts_snapshot, snapshot, dsd_app_command_session_generation());
    }
    /* The call history is persistent by design, so unlike the metrics it is never
     * cleared on session boundaries — only fed. */
    if (m_history != nullptr) {
        m_history->refresh(snapshot, opts_snapshot);
    }
    if (live && m_talkgroups != nullptr) {
        m_talkgroups->refresh(opts_snapshot, snapshot);
    }
    if (live && m_unitActivity != nullptr) {
        m_unitActivity->refresh(snapshot);
    }
}

} // namespace dsd_qt
