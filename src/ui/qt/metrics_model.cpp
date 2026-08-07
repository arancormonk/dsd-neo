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

/**
 * @brief Structured identity for one slot, display-ready for the hero panel.
 *
 * The friendly name prefers the CSV-imported group name staged on the slot's active
 * history row, but only when that row is about the same talkgroup — the staged row
 * outlives a call by design and must not caption the next one.
 */
MetricsModel::SlotCall
MetricsModel::slotCallView(const dsd_state* snapshot, quint8 slot, double now_m) {
    MetricsModel::SlotCall out;
    dsd_call_snapshot call = {};
    const CallLineState line = call_line_state(dsd_call_state_get(snapshot, slot, &call), call, now_m);
    out.state = static_cast<int>(line);
    if (line != kCallLineActive && line != kCallLineEnded) {
        return out;
    }

    const QString target = (call.target_text[0] != '\0') ? QString::fromUtf8(call.target_text)
                                                         : QString::number(static_cast<qulonglong>(call.ota_target_id));
    out.tg_text = target;
    // Same preference order the event layer uses: the OTA id when one decoded,
    // the policy-resolved id otherwise. Text-only targets stay 0.
    out.tg_id = (call.ota_target_id != 0U) ? static_cast<qulonglong>(call.ota_target_id)
                                           : static_cast<qulonglong>(call.policy_target_id);
    out.src_text = (call.source_text[0] != '\0') ? QString::fromUtf8(call.source_text)
                                                 : QString::number(static_cast<qulonglong>(call.ota_source_id));

    out.name = target;
    if (snapshot->event_history_s != nullptr) {
        const Event_History* staged = &snapshot->event_history_s[slot].Event_History_Items[0];
        if (staged->t_name[0] != '\0' && staged->target_id == static_cast<uint32_t>(call.ota_target_id)) {
            out.name = QString::fromUtf8(staged->t_name);
        }
    }

    out.enc = call.crypto == DSD_CALL_CRYPTO_ENCRYPTED || call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING;
    const double ref_m = (line == kCallLineEnded) ? call.ended_m : now_m;
    const double elapsed = ref_m - call.started_m;
    out.seconds = (elapsed > 0.0) ? static_cast<int>(elapsed) : 0;
    return out;
}

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
    next.slot_call[0] = slotCallView(snapshot, 0, now_m);
    next.slot_call[1] = slotCallView(snapshot, 1, now_m);

    publish(next);
}

} // namespace dsd_qt
