// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "metrics_model.h"

#include <QChar>
#include <QDateTime>
#include <QtMinMax>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <stdint.h>

#include "call_line.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

namespace dsd_qt {

namespace {

/**
 * @brief Whether the call reads as encrypted over the air, including DECRYPTABLE.
 *
 * Same definition the event ring stamps on history rows (and the terminal UI
 * renders): the live view and the call log must agree on which transmissions
 * were encrypted, or a decrypted call plays clear here and then hides under
 * the log's ENC filter.
 */
bool
call_reads_encrypted(const dsd_call_snapshot& call) {
    return call.crypto == DSD_CALL_CRYPTO_ENCRYPTED || call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING
           || call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE;
}

/**
 * @brief "ALG 84 · KID 0001" when the crypto header decoded, else empty.
 *
 * What the terminal UI's slot line showed, so AES and RC4 traffic read
 * differently at a glance; the ENC tag alone covers "encrypted, alg unknown".
 */
QString
slot_enc_text(const dsd_call_snapshot& call) {
    if (call.algid == 0U) {
        return QString();
    }
    return QStringLiteral("ALG %1 · KID %2")
        .arg(call.algid, 2, 16, QLatin1Char('0'))
        .arg(call.kid, 4, 16, QLatin1Char('0'))
        .toUpper();
}

} // namespace

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
        // The staged row's target_id is stamped OTA-then-policy by the event layer,
        // so it must be compared against the same preference (out.tg_id), not the
        // OTA id alone — a policy-resolved talkgroup would never match otherwise.
        // Nonzero required: a text-only target's 0 would "match" a stale staged row.
        if (staged->t_name[0] != '\0' && out.tg_id != 0 && staged->target_id == static_cast<uint32_t>(out.tg_id)) {
            out.name = QString::fromUtf8(staged->t_name);
        }
    }

    out.enc = call_reads_encrypted(call);
    if (out.enc) {
        out.enc_text = slot_enc_text(call);
    }
    const double ref_m = (line == kCallLineEnded) ? call.ended_m : now_m;
    const double elapsed = ref_m - call.started_m;
    out.seconds = (elapsed > 0.0) ? static_cast<int>(elapsed) : 0;
    return out;
}

MetricsModel::MetricsModel(QObject* parent) : QObject(parent) {
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this]() {
        View next = m_view;
        next.ui_message.clear();
        publish(next);
    });
}

MetricsModel::~MetricsModel() = default;

void
MetricsModel::publish(const View& next) {
    const bool tunerMoved = !next.tunerEquals(m_view);
    const bool slot1Moved = !(next.slot_call[0] == m_view.slot_call[0]);
    const bool slot2Moved = !(next.slot_call[1] == m_view.slot_call[1]);
    const bool controlMoved = !next.controlEquals(m_view);
    const bool messageMoved = next.ui_message != m_view.ui_message;
    if (!tunerMoved && !slot1Moved && !slot2Moved && !controlMoved && !messageMoved) {
        return;
    }
    m_view = next;
    if (tunerMoved) {
        Q_EMIT tunerChanged();
    }
    if (slot1Moved) {
        Q_EMIT slot1Changed();
    }
    if (slot2Moved) {
        Q_EMIT slot2Changed();
    }
    if (controlMoved) {
        Q_EMIT controlChanged();
    }
    if (messageMoved) {
        Q_EMIT uiMessageChanged();
    }
}

void
MetricsModel::clear() {
    m_messageTimer.stop();
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
    next.stream_active = metrics.stream_active != 0;

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

    /* Engine truth for the monitor's toggle buttons. The engine owns both states
     * — commands only enqueue a request — and on Android the service outlives the
     * Activity, so a relaunched UI must read where they actually stand rather than
     * assume a fresh session's defaults. */
    next.audio_muted = opts_snapshot->audio_out == 0;
    next.held_tg = static_cast<qulonglong>(snapshot->tg_hold);
    next.enc_lockout_count = dsd_enc_lockout_active_count(snapshot);

    /* The engine's command acknowledgement, shown until its own expiry stamp. The
     * timer takes an expired message down without waiting for another publish —
     * an idle engine may not raise the redraw flag again for minutes. */
    const qint64 message_remaining_s =
        static_cast<qint64>(snapshot->ui_msg_expire) - QDateTime::currentSecsSinceEpoch();
    if (snapshot->ui_msg[0] != '\0' && message_remaining_s > 0) {
        next.ui_message = QString::fromUtf8(snapshot->ui_msg);
        m_messageTimer.start(static_cast<int>(qMin<qint64>(message_remaining_s, 30) * 1000) + 100);
    }

    publish(next);
}

} // namespace dsd_qt
