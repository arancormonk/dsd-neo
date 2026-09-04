// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "metrics_model.h"

#include <QChar>
#include <QDateTime>
#include <QtGlobal>
#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/enc_lockout.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

namespace dsd_qt {

namespace {

/**
 * @brief How long a lock keeps reading as locked after the last synced frame.
 *
 * Long enough to ride out the gaps between frames a 250 ms poll lands in, short
 * enough that a decoder which has genuinely stopped finding anything says so
 * while the reader is still looking at it.
 *
 * Shared with the Android notification through app-control, so the two surfaces cannot
 * drift apart on when a session stops reading as locked.
 */
constexpr double kSyncHoldSeconds = DSD_APP_SYNC_HOLD_S;

/**
 * @brief "ALG 84 · KID 0001" when the crypto header decoded, else empty.
 *
 * What the terminal UI's slot line showed, so AES and RC4 traffic read differently at a
 * glance; the ENC tag alone covers "encrypted, alg unknown".
 */
QString
slot_enc_text(quint8 algid, quint16 kid) {
    if (algid == 0U) {
        return QString();
    }
    return QStringLiteral("ALG %1 · KID %2")
        .arg(algid, 2, 16, QLatin1Char('0'))
        .arg(kid, 4, 16, QLatin1Char('0'))
        .toUpper();
}

} // namespace

/**
 * @brief Structured identity for one slot, display-ready for the hero panel.
 *
 * Thin adapter over the shared app-control view: it owns the ended-hold decay, the
 * identity-less-epoch suppression and the staged CSV group name lookup, so the
 * terminal, this panel and the Android notification cannot drift apart on any of them.
 */
MetricsModel::SlotCall
MetricsModel::slotCallView(const dsd_state* snapshot, quint8 slot, double now_m) {
    MetricsModel::SlotCall out;
    dsd_app_slot_call view;
    dsd_app_slot_call_view(snapshot, slot, now_m, &view);

    out.state = view.state;
    if (view.state != DSD_APP_CALL_LINE_ACTIVE && view.state != DSD_APP_CALL_LINE_ENDED) {
        return out;
    }

    out.tg_text = QString::fromUtf8(view.tg_text);
    out.tg_id = static_cast<qulonglong>(view.tg_id);
    out.src_text = QString::fromUtf8(view.src_text);
    out.name = QString::fromUtf8(view.name);
    out.channel = QString::fromUtf8(view.channel);
    out.enc = view.enc != 0U;
    if (out.enc) {
        out.enc_text = slot_enc_text(view.algid, view.kid);
    }
    out.seconds = static_cast<int>(view.elapsed_ms / 1000U);
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
    if (slot1Moved || slot2Moved) {
        Q_EMIT leadSlotChanged();
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
    /* The latch is per-session state, not part of the published frame: a stopped
     * session that starts again on the same frequency must not inherit the old
     * session's answer to "is there anything here". */
    m_sync_type_here = DSD_SYNC_NONE;
    m_sync_seen_m = 0.0;
    publish(View());
}

/**
 * @brief Fill in what the decoder is doing and what it is set to.
 *
 * The settings are read from the options snapshot rather than remembered from the
 * last command, because a command can be refused and, on Android, the service
 * that owns them outlives this process.
 */
/*
 * Scan hold and avoids read whichever rotation owns the tuner: the coordinator's
 * publication under --trunk-scan, the scan-list flags under -Y. Plain trunking is not
 * a rotation, so the controls have nothing to act on and the gate stays false.
 */
void
MetricsModel::fillScanControlView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot) {
    const bool trunk_scan = opts_snapshot->trunk_scan_enabled != 0;
    next.scan_rotation_active = opts_snapshot->scanner_mode != 0 || trunk_scan;
    next.scan_hold = trunk_scan ? (snapshot->trunk_scan_hold != 0) : (snapshot->lcn_scan_hold != 0);
    next.scan_avoid_count = trunk_scan ? snapshot->trunk_scan_avoided_count : snapshot->lcn_avoid_count;
    next.scan_target_avoided = trunk_scan && snapshot->trunk_scan_active_avoided != 0;
}

void
MetricsModel::fillDecoderView(View& next, const dsd_opts* opts_snapshot, const dsd_state* snapshot, double now_m) {
    next.scan_mode = QString::fromLatin1(dsd_scan_mode_name(dsd_scan_mode_active(snapshot)));
    next.decode_mode = static_cast<int>(dsd_scan_mode_configured_preset(opts_snapshot, snapshot));

    /* Held for a moment after the last live sync, rather than latched until
     * something explicitly clears it.
     *
     * A hold is needed at all because sync comes and goes between the 250 ms
     * polls, so an instantaneous reading flickers. But it has to expire on its
     * own: the first version cleared the latch on a retune or a decode-mode
     * change, and any such one-shot reset is a race against whatever the poll
     * happens to read on that same frame -- observed as the strip still naming
     * P25 minutes after being told to decode DMR, with no P25 sync in the log at
     * all. Decay answers the question the reader is actually asking ("is it
     * locked now") and cannot get stuck on an answer to a question they stopped
     * asking. lastsynctype is deliberately not consulted: it holds the previous
     * lock until the engine's no-carrier path gets round to clearing it. */
    if (snapshot->synctype != DSD_SYNC_NONE) {
        m_sync_type_here = snapshot->synctype;
        m_sync_seen_m = now_m;
    } else if (m_sync_type_here != DSD_SYNC_NONE && (now_m - m_sync_seen_m) > kSyncHoldSeconds) {
        m_sync_type_here = DSD_SYNC_NONE;
    }
    next.synced_here = m_sync_type_here != DSD_SYNC_NONE;
    if (next.synced_here) {
        next.sync_label = QString::fromUtf8(dsd_synctype_to_string(m_sync_type_here));
    }
    /* Rides the same decayed reading as synced_here rather than the raw snapshot:
     * a control offered on one frame of sync and withdrawn on the next would
     * flicker under the finger. */
    next.trunkable_sync = next.synced_here && DSD_SYNC_IS_TRUNKABLE(m_sync_type_here);
    /* Three states, not two: GFSK is what the DMR and EDACS/ProVoice presets
     * select, and folding it into C4FM made a control bound to this reading show
     * C4FM as already-selected on a session that was never on it. Through the
     * shared helper so this and ui_handle_mod_set()'s skip test cannot drift. */
    next.modulation = dsd_opts_modulation(opts_snapshot);
    /* Gated on radio_input like center_freq_hz above, and for the same reason:
     * on a WAV, UDP, TCP or symbol-file session these are options the front end
     * never applied, and publishing them would put three plausible tuner
     * readings on screen for a session that has no tuner. */
    next.tuner_gain_db = next.radio_input ? opts_snapshot->rtl_gain_value : 0;
    /* rtl_squelch_level is a mean-power threshold, not decibels — the same
     * conversion the engine's own status line uses. Publishing the raw value
     * would put "0" on screen for a squelch of -120 dB. A level that gates
     * nothing is published separately, because pwr_to_dB() renders it as -120
     * too and the panel would otherwise show a threshold that is not in force. */
    next.squelch_db = next.radio_input ? pwr_to_dB(opts_snapshot->rtl_squelch_level) : 0.0;
    next.squelch_off = next.radio_input && dsd_squelch_is_off(opts_snapshot->rtl_squelch_level);
    next.ppm = next.radio_input ? opts_snapshot->rtlsdr_ppm_error : 0;
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
    next.radio_input = dsd_opts_input_is_radio(opts_snapshot) != 0;

    next.carrier_lock = metrics.carrier_lock != 0;
    next.cfo_hz = metrics.cfo_hz;
    next.stream_active = metrics.stream_active != 0;
    /* Where the front end is pointed, and whether something other than the user
     * is pointing it. Both come from the same options snapshot as radio_input,
     * so a frame never mixes a center from one generation with a gate from
     * another. Scanner mode counts alongside trunking: it owns the tuner too,
     * stepping the channel map once the hangtime expires. */
    next.center_freq_hz = next.radio_input ? static_cast<double>(opts_snapshot->rtlsdr_center_freq) : 0.0;
    next.channel_bandwidth_hz = next.radio_input ? metrics.channel_bandwidth_hz : 0;
    next.trunking_enabled = opts_snapshot->trunk_enable != 0;
    next.scanner_mode = opts_snapshot->scanner_mode != 0;
    /* Trunk scan counts as a third owner even though it has no reading of its own:
     * it steps targets from the engine loop and a release cannot clear it, so an
     * affordance gated only on the other two offers a tune the scan then undoes. */
    next.tuner_controlled = next.trunking_enabled || next.scanner_mode || (opts_snapshot->trunk_scan_enabled != 0);

    /* Sync is held for a moment after the last synced frame rather than sampled;
     * the hold decays on its own rather than being cleared on a retune. See
     * fillDecoderView(), which documents why a one-shot reset was a race. */
    fillDecoderView(next, opts_snapshot, snapshot, dsd_time_now_monotonic_s());

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
    fillScanControlView(next, opts_snapshot, snapshot);

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
