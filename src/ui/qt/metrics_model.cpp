// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "metrics_model.h"

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/state.h>

#include "call_line.h"

namespace dsd_qt {

namespace {

QString
crypto_text(const dsd_call_snapshot& call) {
    switch (call.crypto) {
        case DSD_CALL_CRYPTO_CLEAR: return QStringLiteral("clear");
        case DSD_CALL_CRYPTO_ENCRYPTED_PENDING: return QStringLiteral("enc?");
        case DSD_CALL_CRYPTO_ENCRYPTED:
            return QStringLiteral("ENC alg %1 key %2").arg(call.algid, 0, 16).arg(call.kid, 0, 16);
        case DSD_CALL_CRYPTO_DECRYPTABLE: return QStringLiteral("decryptable");
        default: return QString();
    }
}

QString
call_text(const dsd_state* snapshot, quint8 slot, double now_m) {
    dsd_call_snapshot call = {};
    const int lookup = (snapshot != nullptr) ? dsd_call_state_get(snapshot, slot, &call) : -1;
    const CallLineState line = call_line_state(lookup, call, now_m);
    if (line == kCallLineNone) {
        return QStringLiteral("—");
    }
    // An ended epoch outlives the transmission by design; past the hold window the slot
    // is quiet and has to say so, or the last call of the day stays on screen as though
    // it were still up. See call_line.h.
    if (line == kCallLineIdle) {
        return QStringLiteral("idle");
    }

    QString target = (call.target_text[0] != '\0') ? QString::fromUtf8(call.target_text)
                                                   : QString::number(static_cast<qulonglong>(call.ota_target_id));
    QString source = (call.source_text[0] != '\0') ? QString::fromUtf8(call.source_text)
                                                   : QString::number(static_cast<qulonglong>(call.ota_source_id));

    QString text = QStringLiteral("TG %1 ← %2").arg(target, source);
    const QString crypto = crypto_text(call);
    if (!crypto.isEmpty()) {
        text += QStringLiteral(" [%1]").arg(crypto);
    }
    // Leads rather than trails: the label elides on the right, and an alias-bearing line
    // on a phone is long enough that a trailing marker is the first thing cut -- which
    // would render a finished call identically to a live one.
    if (line == kCallLineEnded) {
        text.prepend(QStringLiteral("ended · "));
    }
    return text;
}

} // namespace

MetricsModel::MetricsModel(QObject* parent) : QObject(parent) {}

MetricsModel::~MetricsModel() = default;

void
MetricsModel::clear() {
    m_stream_active = false;
    m_snr_db = 0.0;
    m_symbol_rate_hz = 0;
    m_output_rate_hz = 0;
    m_carrier_lock = false;
    m_cfo_hz = 0.0;
    m_tuner_gain_text.clear();
    m_slot_text[0].clear();
    m_slot_text[1].clear();
    m_message_text.clear();
    Q_EMIT changed();
}

void
MetricsModel::refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    dsd_frontend_metrics metrics;
    /* 0 is success here, negative is failure — not a count. */
    const bool have_metrics =
        dsd_app_frontend_get_metrics_for_snapshot(opts_snapshot, snapshot, &metrics, DSD_FRONTEND_SNR_FALLBACK_ALL)
        == 0;

    if (!have_metrics) {
        /* Keeping the last readings would render a failed fetch as a healthy decoder:
         * a locked carrier and a plausible SNR for something that is not reporting.
         * Same reasoning as clear(), which is what the caller sees on a stop. */
        clear();
        return;
    }

    m_stream_active = metrics.stream_active != 0;
    m_symbol_rate_hz = metrics.symbol_rate_hz;
    m_output_rate_hz = static_cast<int>(metrics.output_rate_hz);
    m_carrier_lock = metrics.carrier_lock != 0;
    m_cfo_hz = metrics.cfo_hz;
    m_snr_db = (metrics.cqpsk_enable != 0) ? metrics.snr_cqpsk_db : metrics.snr_c4fm_db;

    if (metrics.tuner_gain_is_auto != 0) {
        m_tuner_gain_text = QStringLiteral("auto");
    } else if (metrics.tuner_gain_valid != 0) {
        m_tuner_gain_text = QStringLiteral("%1 dB").arg(metrics.tuner_gain_tenth_db / 10.0, 0, 'f', 1);
    } else {
        m_tuner_gain_text = QStringLiteral("—");
    }

    const double now_m = dsd_time_now_monotonic_s();
    m_slot_text[0] = call_text(snapshot, 0, now_m);
    m_slot_text[1] = call_text(snapshot, 1, now_m);
    m_message_text = (snapshot != nullptr) ? QString::fromUtf8(snapshot->ui_msg) : QString();

    Q_EMIT changed();
}

} // namespace dsd_qt
