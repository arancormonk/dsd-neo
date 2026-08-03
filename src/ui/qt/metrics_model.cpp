// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "metrics_model.h"

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
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

/** @brief One slot's call line. @p snapshot must not be null; refresh() guarantees it. */
QString
call_text(const dsd_state* snapshot, quint8 slot, double now_m) {
    dsd_call_snapshot call = {};
    const CallLineState line = call_line_state(dsd_call_state_get(snapshot, slot, &call), call, now_m);
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
MetricsModel::publish(const View& next) {
    if (next == m_view) {
        return;
    }
    m_view = next;
    Q_EMIT changed();
}

void
MetricsModel::clear() {
    publish(View());
}

void
MetricsModel::refresh(const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    dsd_frontend_metrics metrics;
    /* A missing snapshot is the real "nothing to show" case, and it has to be tested
     * for here: the fetch fills defaults for a NULL snapshot rather than failing, so
     * its result alone never distinguishes no data from a healthy decoder reporting
     * zeros. Keeping the last readings would render either as a live one — a locked
     * carrier and a plausible SNR for something that is not reporting. 0 is success
     * below, negative is failure; it is not a count. */
    if (opts_snapshot == nullptr || snapshot == nullptr
        || dsd_app_frontend_get_metrics_for_snapshot(opts_snapshot, snapshot, &metrics, DSD_FRONTEND_SNR_FALLBACK_ALL)
               != 0) {
        clear();
        return;
    }

    /* Built whole, then published in one step, so a frame that reads identically to
     * the last one costs no binding re-evaluation. See MetricsModel::View. */
    View next;

    /* Drives whether the tuner-facing rows are shown at all. Taken from the options
     * the running session was configured with, which is the same authority the
     * metrics fetch above uses to decide whether any of them mean anything. */
    next.radio_input = opts_snapshot->audio_in_type == AUDIO_IN_RTL;

    next.stream_active = metrics.stream_active != 0;
    next.symbol_rate_hz = metrics.symbol_rate_hz;
    next.output_rate_hz = static_cast<int>(metrics.output_rate_hz);
    next.carrier_lock = metrics.carrier_lock != 0;
    next.cfo_hz = metrics.cfo_hz;

    /* Selected by modulation, not by cqpsk_enable: the C4FM estimator reads nothing on
     * a GFSK stream. Nothing reads at all on an input with no demodulator behind it
     * (UDP, TCP, a file) -- rtl_tcp does have one, since it is an RTL input like any
     * other. Publishing the estimator's no-reading sentinel would put "-100.0 dB" on
     * screen as though it were a measurement. */
    const dsd_frontend_snr_readout snr = dsd_app_frontend_snr_for_mod(&metrics, snapshot->rf_mod);
    next.snr_valid = snr.valid != 0;
    next.snr_db = next.snr_valid ? snr.snr_db : 0.0;

    if (metrics.tuner_gain_is_auto != 0) {
        next.tuner_gain_text = QStringLiteral("auto");
    } else if (metrics.tuner_gain_valid != 0) {
        next.tuner_gain_text = QStringLiteral("%1 dB").arg(metrics.tuner_gain_tenth_db / 10.0, 0, 'f', 1);
    } else {
        next.tuner_gain_text = QStringLiteral("—");
    }

    const double now_m = dsd_time_now_monotonic_s();
    next.slot_text[0] = call_text(snapshot, 0, now_m);
    next.slot_text[1] = call_text(snapshot, 1, now_m);
    next.message_text = QString::fromUtf8(snapshot->ui_msg);

    publish(next);
}

} // namespace dsd_qt
