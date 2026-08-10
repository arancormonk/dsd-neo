// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 *
 * Regression test: what MetricsModel makes of a decoder snapshot.
 *
 * The readings a screen gates on are derived here, and getting them wrong is
 * invisible in a screenshot: a lock light that stays lit after the decoder has
 * been told to look for something else reads as "it is working" while nothing is
 * being decoded at all. That one shipped — a one-shot latch reset raced whatever
 * the poll happened to read on the same frame — which is what these cases pin.
 */

#include <QCoreApplication>
#include <QString>
#include <cmath>
#include <stdio.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "metrics_model.h"

namespace {

int g_failures = 0;

/* What the stubbed frontend reports for the channel width. Per-case, because
 * every other case wants the at-rest 0. */
static int g_stub_channel_bandwidth_hz = 0;

void
expect(const char* what, bool ok) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_failures++;
    }
}

} // namespace

/*
 * The frontend boundary, stubbed. Linking the real one would drag in the engine
 * and the radio backends for a test about arithmetic over a snapshot.
 */
extern "C" int
dsd_app_frontend_get_metrics_for_snapshot(const dsd_opts* opts, const dsd_state* state, dsd_frontend_metrics* out,
                                          unsigned int snr_fallbacks) {
    (void)opts;
    (void)state;
    (void)snr_fallbacks;
    if (out == nullptr) {
        return -1;
    }
    *out = dsd_frontend_metrics{};
    out->channel_bandwidth_hz = g_stub_channel_bandwidth_hz;
    return 0;
}

extern "C" dsd_frontend_snr_readout
dsd_app_frontend_snr_for_mod(const dsd_frontend_metrics* metrics, int rf_mod) {
    (void)metrics;
    (void)rf_mod;
    dsd_frontend_snr_readout out{};
    out.valid = 0;
    out.snr_db = 0.0;
    return out;
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtlsdr_center_freq = 769768750;
    opts.rtl_gain_value = 30;
    opts.rtl_squelch_level = dB_to_pwr(-120.0);
    opts.mod_c4fm = 1;
    opts.mod_qpsk = 0;

    dsd_qt::MetricsModel model;

    /* Nothing has synced: the strip must say so rather than default to a lock. */
    state.synctype = DSD_SYNC_NONE;
    state.lastsynctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("an unsynced decoder does not read as locked", !model.syncedHere());
    expect("an unsynced decoder names nothing", model.syncLabel().isEmpty());

    /* A live sync latches, and names what it locked to. */
    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("a live sync reads as locked", model.syncedHere());
    expect("a live sync names the protocol", model.syncLabel().contains(QStringLiteral("P25")));

    /* Sync comes and goes between polls, so a single unsynced frame must not
     * drop the lock — that is what the hold is for. */
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("one unsynced poll does not drop the lock", model.syncedHere());

    /* But the hold expires. Nothing clears it explicitly: a decoder told to look
     * for another protocol simply stops finding this one, and the reading has to
     * follow that on its own. This is the case the shipped bug failed. */
    model.expireSyncForTest();
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("a decoder that stopped finding anything stops reading as locked", !model.syncedHere());
    expect("an expired lock names nothing", model.syncLabel().isEmpty());

    /* Whether trunking has anything to follow here. A control offering to hand
     * the tuner over needs the protocol, not merely a lock: on M17 there is no
     * trunking to hand it to, and on nothing at all there is nothing to follow. */
    expect("no lock is not something to follow", !model.trunkableSync());

    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("P25p1 is something trunking can follow", model.trunkableSync());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    model.refresh(&opts, &state);
    expect("DMR is something trunking can follow", model.trunkableSync());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_M17_STR_POS;
    model.refresh(&opts, &state);
    expect("M17 locks without giving trunking anything to follow", !model.trunkableSync());
    expect("M17 still reads as locked", model.syncedHere());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_DSTAR_VOICE_POS;
    model.refresh(&opts, &state);
    expect("D-STAR locks without giving trunking anything to follow", !model.trunkableSync());

    /* Rides the decayed lock, not the raw frame: a control that vanished on the
     * one unsynced poll between two synced ones would flicker under the finger. */
    model.expireSyncForTest();
    state.synctype = DSD_SYNC_P25P2_POS;
    model.refresh(&opts, &state);
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("one unsynced poll does not withdraw the offer", model.trunkableSync());

    model.expireSyncForTest();
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("an expired lock withdraws the offer", !model.trunkableSync());

    /* The panel's own readings come from the options, not from what anything
     * asked for. Squelch is stored as mean power and must reach QML as decibels,
     * or a -120 dB threshold renders as 0. */
    expect("gain is reported", model.tunerGainDb() == 30);
    expect("squelch is reported in dB", std::fabs(model.squelchDb() - (-120.0)) < 0.5);
    expect("c4fm reads as modulation 0", model.modulation() == 0);

    opts.mod_qpsk = 1;
    opts.mod_c4fm = 0;
    model.refresh(&opts, &state);
    expect("qpsk reads as modulation 1", model.modulation() == 1);

    /* GFSK is a third state, not an absence of QPSK. Reported as C4FM, it made a
     * control bound to this reading show C4FM as already-selected on a DMR or
     * EDACS/ProVoice session that had never been on it. */
    opts.mod_qpsk = 0;
    opts.mod_c4fm = 0;
    opts.mod_gfsk = 1;
    model.refresh(&opts, &state);
    expect("gfsk reads as modulation 2", model.modulation() == 2);

    opts.mod_gfsk = 0;
    opts.mod_c4fm = 1;
    model.refresh(&opts, &state);
    expect("c4fm reads as modulation 0 again", model.modulation() == 0);

    /* Tuner ownership: the gate is the OR, and the two named owners exist only to
     * word a message. All three have to agree. */
    opts.trunk_enable = 1;
    model.refresh(&opts, &state);
    expect("trunking owns the tuner", model.tunerControlled() && model.trunkingEnabled());
    expect("trunking is not the scanner", !model.scannerMode());
    opts.trunk_enable = 0;
    opts.scanner_mode = 1;
    model.refresh(&opts, &state);
    expect("the scanner owns the tuner too", model.tunerControlled() && model.scannerMode());
    expect("the scanner is not trunking", !model.trunkingEnabled());
    opts.scanner_mode = 0;

    /* An epoch the decoder opened on a frame that synced and went no further has
     * nothing in it. Rendered, it becomes a call from talkgroup 0 by nobody —
     * and, with a stale crypto header on the slot, an encrypted one. A session
     * that has not locked onto anything must not appear to be hearing traffic. */
    {
        dsd_call_observation empty = {};
        empty.protocol = DSD_SYNC_P25P1_POS;
        empty.slot = 0U;
        empty.kind = DSD_CALL_KIND_GROUP_VOICE;
        empty.observed_m = 1.0;
        expect("an identity-less epoch opens", dsd_call_state_observe(&state, &empty, DSD_CALL_BOUNDARY_BEGIN) == 1);
        state.payload_algid = 0x84; /* the stale header that made it read as ENC */
        model.refresh(&opts, &state);
        expect("an identity-less call is not presented as active", model.slot1CallState() != 2);
        expect("an identity-less call names no talkgroup", model.slot1TgText().isEmpty());
        expect("an identity-less call is not reported encrypted", !model.slot1CallEnc());

        /* The same epoch with a talkgroup on it is a real transmission. */
        dsd_call_observation named = empty;
        named.ota_target_id = 1201U;
        named.ota_source_id = 4242U;
        named.observed_m = 2.0;
        (void)dsd_call_state_observe(&state, &named, DSD_CALL_BOUNDARY_BEGIN);
        model.refresh(&opts, &state);
        expect("a call with a talkgroup is presented", model.slot1CallState() == 2);
        expect("a call with a talkgroup names it", model.slot1TgText() == QStringLiteral("1201"));
        (void)dsd_call_state_end(&state, 0U, 3.0);
    }

    /* A stopped session must not leave its answers behind for the next one. */
    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("locked again before the stop", model.syncedHere());
    model.clear();
    expect("a cleared model reports no lock", !model.syncedHere());
    expect("a cleared model reports no tuner", !model.radioInput());

    /* The channel width rides the tuner group: it moves when the decoder changes
     * profile, not when the user changes a setting. It is also gated on a radio
     * input, like every other tuner reading. */
    {
        g_stub_channel_bandwidth_hz = 12500;
        opts.audio_in_type = AUDIO_IN_RTL;
        model.refresh(&opts, &state);
        expect("channel bandwidth reaches the model", model.channelBandwidthHz() == 12500);

        opts.audio_in_type = AUDIO_IN_PULSE;
        model.refresh(&opts, &state);
        expect("channel bandwidth is zero off a radio", model.channelBandwidthHz() == 0);

        g_stub_channel_bandwidth_hz = 0;
        opts.audio_in_type = AUDIO_IN_RTL;
    }

    /* The width must move on its own. Every other tuner reading is holding still
     * here, so if channel_bandwidth_hz were missing from tunerEquals() the frame
     * would compare equal, publish() would skip the whole View, and this second
     * reading would still be the first one. */
    {
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_channel_bandwidth_hz = 12500;
        model.refresh(&opts, &state);
        expect("width reads the first frame", model.channelBandwidthHz() == 12500);

        g_stub_channel_bandwidth_hz = 6250;
        model.refresh(&opts, &state);
        expect("width alone moves the tuner group", model.channelBandwidthHz() == 6250);

        g_stub_channel_bandwidth_hz = 0;
    }

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
