// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "ui_controller.h"

#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>

#include "call_history_model.h"
#include "decoder_host.h"
#include "metrics_model.h"

namespace dsd_qt {

namespace {

/* Deliberately battery-friendly: the terminal UI polls at 15 ms with a 66 ms draw
 * cap, but a phone has no reason to redraw a metrics panel faster than the eye
 * notices. Raise this when the spectrum view lands. */
constexpr int kDefaultPollIntervalMs = 250;

} // namespace

UiController::UiController(DecoderHost* host, MetricsModel* metrics, CallHistoryModel* history, QObject* parent)
    : QObject(parent), m_host(host), m_metrics(metrics), m_history(history) {
    m_timer.setInterval(kDefaultPollIntervalMs);
    m_timer.setTimerType(Qt::CoarseTimer);
    connect(&m_timer, &QTimer::timeout, this, &UiController::tick);
    if (m_host != nullptr) {
        m_session = m_host->sessionState();
        connect(m_host, &DecoderHost::sessionStateChanged, this, &UiController::onSessionStateChanged);
    }
}

UiController::~UiController() = default;

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
UiController::onSessionStateChanged() {
    const DecoderHost::SessionState previous = m_session;
    m_session = m_host->sessionState();
    if (m_session == previous) {
        return;
    }

    /* Entering a session: the incoming run owns the screen, so the readings clear
     * before the monitoring view appears. Leaving one: the metrics describe a
     * decoder that no longer exists. See MetricsModel::clear(). */
    if (m_session == DecoderHost::Starting || m_session == DecoderHost::Idle || m_session == DecoderHost::Failed) {
        if (m_metrics != nullptr) {
            m_metrics->clear();
        }
    }
}

void
UiController::tick() {
    /* Host state is not published through the redraw flag: a stopped engine raises
     * nothing, and "stopped" is exactly what the UI must notice. */
    if (m_host != nullptr) {
        m_host->refresh();
    }

    if (dsd_app_frontend_redraw_consume() == 0) {
        return;
    }

    /* Consumed once, here, and handed to both models. Each accessor deep-copies
     * whenever the publisher has moved on, so fetching per model would let a publish
     * land mid-frame and leave the status card describing one generation and the
     * event list another — and would repeat the copy for every fetch. */
    const dsd_opts* opts_snapshot = dsd_app_get_latest_opts_snapshot();
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();

    if (m_metrics != nullptr) {
        m_metrics->refresh(opts_snapshot, snapshot);
    }
    /* The call history is persistent by design, so unlike the metrics it is never
     * cleared on session boundaries — only fed. */
    if (m_history != nullptr) {
        m_history->refresh(snapshot);
    }
}

} // namespace dsd_qt
