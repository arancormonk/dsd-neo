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
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVariant>
#include <cmath>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/scan_mode.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "metrics_model.h"

namespace {

int g_failures = 0;

/* What the stubbed frontend reports for the channel width. Per-case, because
 * every other case wants the at-rest 0. */
static int g_stub_channel_bandwidth_hz = 0;

/* The scan-timing view a case feeds the model. Zeroed between cases: reason NONE
 * is the contract's "nothing to show", which is what every other case wants. */
static dsd_app_scan_timing g_stub_scan_timing;

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

/*
 * The scan-timing view, stubbed through the same link seam as the frontend metrics
 * above, so a case can hand the model a view it built by hand.
 *
 * Deliberately not the real one: the reason -> phrase / show-this-budget table is
 * app-control's and is pinned by APP_CONTROL_SCAN_TIMING_VIEW. What this suite owns
 * is the copy out of the view into the published frame, and running the real table
 * here would only pin it twice. The one rule reproduced is the contract's `active`
 * gate, because the model deliberately has no gate of its own -- "is anything
 * scanning" is decided once, in the view.
 */
extern "C" int
dsd_app_scan_timing_view(const dsd_opts* opts, const dsd_state* state, double now_m, dsd_app_scan_timing* out) {
    if (out == nullptr) {
        return -1;
    }
    *out = dsd_app_scan_timing{};
    if (opts == nullptr || state == nullptr) {
        return -1;
    }
    (void)now_m;
    const bool scanning = opts->trunk_scan_enabled == 1 || opts->scanner_mode == 1;
    if (!scanning || g_stub_scan_timing.reason == DSD_SCAN_STAY_NONE) {
        return 0;
    }
    *out = g_stub_scan_timing;
    out->active = 1U;
    return 1;
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

static void
test_quality_without_identity(int protocol, uint8_t slot) {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.audio_in_type = AUDIO_IN_PULSE;
    state.synctype = protocol;
    state.errs = 3;
    state.errs2 = 9;
    state.errsR = 4;
    state.errs2R = 11;
    dsd_qt::MetricsModel model;

    // The vocoder can establish media before a late-entry identity header arrives.
    dsd_call_observation call = {};
    call.protocol = protocol;
    call.slot = slot;
    call.kind = DSD_CALL_KIND_VOICE;
    expect("provisional voice call opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    expect("provisional voice has media", dsd_call_state_update_media(&state, slot, 1, 0) == 1);
    model.refresh(&opts, &state);
    expect("provisional voice stays out of the identity headline", model.leadSlot() == 0);
    expect("late-entry media exposes non-P25 quality without identity",
           model.qualityValid() && model.lastFrameErrsValid() && model.lastFrameErrs() == (slot == 0 ? 3 : 4)
               && model.lastFrameErrs2() == (slot == 0 ? 9 : 11));

    if (slot == 1) {
        call.slot = 0;
        call.kind = DSD_CALL_KIND_GROUP_VOICE;
        call.ota_target_id = 101;
        expect("identified companion opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
        model.refresh(&opts, &state);
        expect("an identity lead retains priority before media arrives",
               model.leadSlot() == 1 && !model.lastFrameErrsValid());
        dsd_call_state_update_media(&state, 0, 1, 0);
        model.refresh(&opts, &state);
        expect("an identity lead keeps its own error readings", model.leadSlot() == 1 && model.lastFrameErrsValid()
                                                                    && model.lastFrameErrs() == 3
                                                                    && model.lastFrameErrs2() == 9);
        dsd_call_state_end(&state, 0, 0);
    }
    dsd_call_state_end(&state, slot, 0);
    model.refresh(&opts, &state);
    expect("ended provisional media cannot supply fallback errors", !model.lastFrameErrsValid());
    dsd_state_ext_free_all(&state);
}

static void
test_quality() {
    static dsd_opts opts;
    static dsd_state state;
    opts.audio_in_type = AUDIO_IN_PULSE;
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p25_p1_voice_err_hist_len = 50;
    dsd_qt::MetricsModel model;
    int quality_signals = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::qualityChanged, [&]() { ++quality_signals; });
    model.refresh(&opts, &state);
    expect("P25 sync alone has no quality reading", !model.qualityValid() && !model.voiceErrsValid());
    state.p25_p1_fec_ok = 9;
    state.p25_p1_fec_err = 1;
    model.refresh(&opts, &state);
    expect("CC quality is available on PCM", model.qualityValid() && !model.radioInput() && model.ccFecValid());
    expect("CC counters and ok percent", model.ccFecOk() == 9 && model.ccFecErr() == 1 && model.ccFecOkPct() == 90.0);
    expect("one quality notification", quality_signals == 1);
    model.refresh(&opts, &state);
    expect("identical quality is silent", quality_signals == 1);
    state.synctype = DSD_SYNC_NONE;
    model.refresh(&opts, &state);
    expect("sync gap retains CC FEC", model.ccFecValid() && quality_signals == 1);
    state.p25_p1_fec_ok = state.p25_p1_fec_err = 0;
    model.refresh(&opts, &state);
    expect("no-carrier counter reset clears CC FEC", !model.ccFecValid() && !model.qualityValid());

    dsd_call_observation call = {};
    call.protocol = DSD_SYNC_P25P1_POS;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = 101;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    state.p25_p1_voice_err_hist_count = 2;
    state.p25_p1_voice_err_hist_sum = 9;
    state.p25_p1_voice_fec_ok = 3;
    state.p25_p1_voice_fec_err = 1;
    model.refresh(&opts, &state);
    expect("P1 uses populated count",
           model.voiceErrsValid() && model.voiceErrsSamples() == 2 && model.voiceErrsPerFrame() == 4.5);
    expect("voice FEC ok percent", model.voiceFecValid() && model.voiceFecOkPct() == 75.0);
    const int before_samples = quality_signals;
    state.p25_p1_voice_err_hist_count = 4;
    state.p25_p1_voice_err_hist_sum = 18;
    model.refresh(&opts, &state);
    expect("sample count alone notifies", quality_signals == before_samples + 1 && model.voiceErrsSamples() == 4);
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    model.refresh(&opts, &state);
    expect("new call waits for fresh samples", !model.voiceErrsValid() && model.voiceErrsSamples() == 0);

    call.protocol = DSD_SYNC_P25P2_POS;
    state.synctype = call.protocol;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    call.slot = 1;
    call.ota_target_id = 102;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    state.p25_p2_voice_err_hist_len = 50;
    state.p25_p2_voice_err_hist_count[1] = 3;
    state.p25_p2_voice_err_hist_sum[1] = 6;
    state.p25_p2_rs_facch_ok = 2;
    state.p25_p2_rs_sacch_ok = 1;
    state.p25_p2_rs_ess_err = 1;
    model.refresh(&opts, &state);
    expect("unsampled lead slot does not borrow other slot", model.leadSlot() == 1 && !model.voiceErrsValid());
    expect("P2 slot readings are independent", !model.slot1VoiceErrsValid() && model.slot2VoiceErrsValid()
                                                   && model.slot2VoiceErrsSamples() == 3
                                                   && model.slot2VoiceErrsPerFrame() == 2.0);
    expect("P2 RS sums FACCH SACCH ESS", model.rsValid() && model.rsOkPct() == 75.0);
    dsd_call_state_end(&state, 0, 0);
    model.refresh(&opts, &state);
    expect("P2 summary follows lead slot", model.leadSlot() == 2 && model.voiceErrsValid()
                                               && model.voiceErrsPerFrame() == 2.0 && model.voiceErrsSamples() == 3);
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    model.refresh(&opts, &state);
    expect("P2 new call invalidates its samples", !model.slot2VoiceErrsValid() && !model.voiceErrsValid());

    call.protocol = DSD_SYNC_DMR_BS_VOICE_POS;
    state.synctype = call.protocol;
    dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN);
    state.errsR = 2;
    state.errs2R = 7;
    dsd_call_state_update_media(&state, 1, 1, 0);
    model.refresh(&opts, &state);
    expect("non-P25 lead slot last-frame fallback", model.lastFrameErrsValid() && model.lastFrameErrs() == 2
                                                        && model.lastFrameErrs2() == 7 && !model.voiceErrsValid());
    model.clear();
    expect("clear removes all quality", !model.qualityValid() && !model.ccFecValid() && !model.voiceFecValid()
                                            && !model.rsValid() && !model.voiceErrsValid()
                                            && !model.slot2VoiceErrsValid() && !model.lastFrameErrsValid()
                                            && model.voiceErrsSamples() == 0);
    const int cleared = quality_signals;
    model.clear();
    expect("repeated clear is silent", quality_signals == cleared);
    model.refresh(&opts, &state);
    model.refresh(nullptr, &state);
    expect("missing snapshot clears quality", !model.qualityValid());
    dsd_state_ext_free_all(&state);
}

static void
test_site() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int signals = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::siteChanged, [&]() { ++signals; });
    state.synctype = DSD_SYNC_P25P1_POS;
    state.p2_cc = 0x293;
    model.refresh(&opts, &state);
    expect("NAC-only P1 identity", model.p25NacValid() && model.p25Nac() == 0x293 && !model.p25WacnValid()
                                       && !model.p25SysIdValid() && !model.p25Phase2ParamsReady()
                                       && model.siteLine() == QStringLiteral("P25 · NAC 293") && model.siteConfirmed());
    const int first = signals;
    model.refresh(&opts, &state);
    expect("identical site does not notify", signals == first);
    state.p2_wacn = 0xBEE00;
    state.p2_sysid = 0x123;
    state.p2_rfssid = 2;
    state.p2_siteid = 3;
    state.p25_site_lra_valid = 1;
    state.p25_site_lra = 0;
    state.trunk_cc_freq = 851000000;
    state.trunk_vc_freq[0] = 852000000;
    model.refresh(&opts, &state);
    expect("full P25", model.p25WacnValid() && model.p25Wacn() == 0xBEE00 && model.p25SysIdValid()
                           && model.p25SysId() == 0x123 && model.p25Rfss() == 2 && model.p25Site() == 3
                           && model.p25LraValid() && model.p25Lra() == 0 && model.p25Phase2ParamsReady()
                           && model.ccFreqHz() == 851000000 && model.vcFreqHz() == 852000000);
    const int beforeFrequency = signals;
    state.trunk_cc_freq += 12500;
    model.refresh(&opts, &state);
    expect("frequency-only site change notifies", signals == beforeFrequency + 1 && model.ccFreqHz() == 851012500);
    state.synctype = DSD_SYNC_P25P2_POS;
    model.refresh(&opts, &state);
    expect("full P2 parameters ready", model.p25Phase2ParamsReady());
    state.p2_wacn = 0xFFFFF;
    state.p2_sysid = 0xFFF;
    model.refresh(&opts, &state);
    expect("invalid system fields do not hide NAC", !model.p25WacnValid() && !model.p25SysIdValid()
                                                        && model.p25NacValid() && model.siteLine().contains("NAC 293")
                                                        && !model.p25Phase2ParamsReady());
    state.p2_wacn = 0xBEE00;
    state.p2_sysid = 0x123;
    state.p25_site_lra_valid = 0;
    model.refresh(&opts, &state);
    expect("LRA validity alone updates", !model.p25LraValid() && !model.siteLine().contains("LRA"));
    for (auto nac : {0ULL, 0xFFFULL, 0x1000ULL}) {
        state.p2_cc = nac;
        model.refresh(&opts, &state);
        expect("invalid NAC omitted independently", !model.p25NacValid() && !model.p25Phase2ParamsReady()
                                                        && model.p25WacnValid() && !model.siteLine().contains("NAC"));
    }
    const QString retained = model.siteLine();
    state.synctype = DSD_SYNC_NONE;
    state.p2_wacn = 0;
    model.refresh(&opts, &state);
    expect("loss retains copied identity but withdraws confirmation",
           model.siteLine() == retained && !model.siteConfirmed());
    state.synctype = DSD_SYNC_DMR_BS_DATA_POS;
    state.dmr_color_code = 7;
    DSD_SNPRINTF(state.dmr_site_parms, sizeof(state.dmr_site_parms), "%s", "Net 12 Site 3; ");
    state.dmr_rest_channel = 4;
    model.refresh(&opts, &state);
    expect("DMR verbatim site", model.siteProtocol() == "DMR" && model.dmrColorCode() == 7
                                    && model.dmrSiteText() == "Net 12 Site 3; " && model.dmrRestLsn() == 4
                                    && !model.p25WacnValid());
    state.synctype = DSD_SYNC_NXDN_POS;
    state.nxdn_last_ran = 64;
    model.refresh(&opts, &state);
    expect("unknown NXDN RAN hidden", model.nxdnRan() == -1 && model.siteLine().isEmpty());
    state.nxdn_last_ran = 0;
    DSD_SNPRINTF(state.nxdn_location_category, sizeof(state.nxdn_location_category), "%s", "Type-D");
    state.nxdn_location_sys_code = 12;
    state.nxdn_location_site_code = 3;
    model.refresh(&opts, &state);
    expect("IDAS area and location", model.siteProtocol() == "IDAS" && model.nxdnRan() == 0
                                         && model.nxdnLocationCategory() == "Type-D" && model.nxdnSysCode() == 12
                                         && model.nxdnSiteCode() == 3 && model.siteLine().contains("Area 0"));
    state.synctype = DSD_SYNC_EDACS_POS;
    state.edacs_site_id = 7;
    model.refresh(&opts, &state);
    expect("EDACS site", model.siteProtocol() == "EDACS" && model.edacsSiteText().contains("007"));
    model.clear();
    expect("stop clears every site group", model.siteLine().isEmpty() && model.siteProtocol().isEmpty()
                                               && !model.siteConfirmed() && model.ccFreqHz() == 0
                                               && model.vcFreqHz() == 0 && model.dmrSiteText().isEmpty()
                                               && model.edacsSiteText().isEmpty() && model.nxdnRan() == -1);
    const int cleared = signals;
    model.clear();
    expect("repeated site clear silent", signals == cleared);
    dsd_state_ext_free_all(&state);
}

static void
test_decryption_metadata() {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    state.enc_lockout_key_epoch = 10;
    DSD_SNPRINTF(state.key_profile_ref, sizeof(state.key_profile_ref), "%s", "opaque-profile");
    dsd_qt::MetricsModel model;
    for (uint8_t slot = 0; slot < 2; ++slot) {
        dsd_call_observation observation = {};
        observation.protocol = DSD_SYNC_P25P2_POS;
        observation.slot = slot;
        observation.kind = DSD_CALL_KIND_GROUP_VOICE;
        observation.ota_target_id = 123;
        observation.observed_m = 4.0;
        dsd_call_state_observe(&state, &observation, DSD_CALL_BOUNDARY_BEGIN);
        dsd_call_crypto_update crypto = {};
        crypto.classification = DSD_CALL_CRYPTO_DECRYPTABLE;
        crypto.algid = 0x84;
        crypto.kid = 2 + slot;
        crypto.observed_m = 4.0;
        dsd_call_state_update_crypto(&state, slot, &crypto);
        dsd_call_snapshot before = {}, after = {};
        dsd_call_state_get(&state, slot, &before);
        expect("resolver note accepted",
               dsd_call_state_note_key_selection(&state, slot, before.epoch, DSD_CALL_KEY_SIGNALED, crypto.kid,
                                                 crypto.kid, 1, 0)
                   == 1);
        dsd_call_state_get(&state, slot, &after);
        expect("selection does not extend activity", before.updated_m == after.updated_m);
        expect("wrong call epoch rejected",
               dsd_call_state_note_key_selection(&state, slot, before.epoch + 100, DSD_CALL_KEY_SIGNALED, 99, 99, 1, 0)
                   == 0);
    }
    model.refresh(&opts, &state);
    const auto slots = model.decryptionSlots();
    expect("two independent key selections",
           slots.size() == 2 && slots[0].toMap().value("keyId") == "2" && slots[1].toMap().value("keyId") == "3");
    expect("only opaque profile association published",
           slots[0].toMap().value("profileRef") == "opaque-profile" && !slots[0].toMap().contains("material"));
    state.enc_lockout_key_epoch++;
    model.refresh(&opts, &state);
    expect("old key result becomes pending",
           model.decryptionSlots()[0].toMap().value("availability") == "Waiting for key reevaluation");
    model.clear();
    expect("stopped session has no live key metadata", model.decryptionSlots().isEmpty());
    dsd_state_ext_free_all(&state);
}

static void
test_options_readiness() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });
    expect("fresh metrics do not establish single-system options",
           !model.optionsKnown() && !model.scanRotationActive());
    model.refresh(nullptr, &state);
    expect("state without options is not ready", !model.optionsKnown());
    model.refresh(&opts, nullptr);
    expect("options without a state snapshot are not ready", !model.optionsKnown());
    model.refresh(&opts, &state);
    expect("valid quiet snapshot establishes options", model.optionsKnown() && !model.scanRotationActive());
    expect("readiness notifies controls", changes > 0);
    const int unchanged = changes;
    model.refresh(&opts, &state);
    expect("unchanged options do not notify twice", changes == unchanged);
    for (int mode = 0; mode < 2; ++mode) {
        model.clear();
        expect("clear forgets the previous session's options", !model.optionsKnown());
        opts.scanner_mode = mode == 0;
        opts.trunk_scan_enabled = mode == 1;
        model.refresh(&opts, &state);
        expect("both effective scan modes arrive with readiness", model.optionsKnown() && model.scanRotationActive());
        model.refresh(nullptr, &state);
        expect("losing a snapshot resets readiness", !model.optionsKnown());
    }
    freeState(&state);
}

/*
 * Why the rotation is staying on the row on air, and how long is left (#508).
 *
 * The countdown is published in tenths of a second rather than milliseconds on
 * purpose: the reading decides whether controlChanged fires, so at millisecond
 * resolution every 250 ms poll would move it and re-evaluate every binding on the
 * control group for a row that renders one decimal place.
 */
static void
test_scan_timing() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });

    /* A trunked target following a call: no countdown, the dwell disarmed while the
     * call holds the row, and the -t hangtime that will release it. */
    opts.trunk_scan_enabled = 1;
    g_stub_scan_timing = dsd_app_scan_timing{};
    g_stub_scan_timing.reason = DSD_SCAN_STAY_CALL_FOLLOW;
    g_stub_scan_timing.phrase = "Following call";
    g_stub_scan_timing.show_dwell = 1U;
    g_stub_scan_timing.dwell_ms = 3000U;
    g_stub_scan_timing.dwell_state = DSD_APP_SCAN_DWELL_SUSPENDED;
    /* Carried by the publication but withheld by the view: a trunked row has no
     * conventional activity hold, and printing one would invent a budget. */
    g_stub_scan_timing.hold_ms = 2000U;
    g_stub_scan_timing.show_hang = 1U;
    g_stub_scan_timing.hang_ms = 2000U;
    model.refresh(&opts, &state);
    expect("a followed call names why the scanner stays",
           model.scanTimingVisible() && model.scanStayReason() == DSD_SCAN_STAY_CALL_FOLLOW
               && model.scanStayPhrase() == QStringLiteral("Following call"));
    expect("a followed call runs no countdown",
           !model.scanTimerLive() && model.scanTimerRemainingDs() == 0 && model.scanTimerSpanMs() == 0);
    expect("the suspended dwell still reads out",
           model.scanDwellMs() == 3000 && model.scanDwellState() == DSD_APP_SCAN_DWELL_SUSPENDED);
    expect("a trunked row never shows a conventional hold", model.scanHoldMs() == 0);
    expect("a followed call shows the hangtime that will release it", model.scanHangMs() == 2000);
    expect("scan timing rides the control group", changes > 0);
    const int settled = changes;
    model.refresh(&opts, &state);
    expect("an unchanged scan timing does not notify twice", changes == settled);

    /* A conventional row counting its idle dwell down, with the activity hold that
     * would extend the stay if something showed up. */
    opts.trunk_scan_enabled = 0;
    opts.scanner_mode = 1;
    g_stub_scan_timing = dsd_app_scan_timing{};
    g_stub_scan_timing.reason = DSD_SCAN_STAY_IDLE_DWELL;
    g_stub_scan_timing.phrase = "Idle dwell";
    g_stub_scan_timing.timer_live = 1U;
    g_stub_scan_timing.remaining_ms = 1849U;
    g_stub_scan_timing.span_ms = 3000U;
    g_stub_scan_timing.show_hold = 1U;
    g_stub_scan_timing.hold_ms = 2000U;
    model.refresh(&opts, &state);
    expect("an idle dwell counts down", model.scanTimerLive() && model.scanTimerSpanMs() == 3000);
    expect("the countdown reaches the row in tenths", model.scanTimerRemainingDs() == 18);
    expect("a conventional row shows its activity hold", model.scanHoldMs() == 2000);
    expect("the dwell the countdown already shows is not printed twice", model.scanDwellMs() == 0);

    const int quiet = changes;
    g_stub_scan_timing.remaining_ms = 1801U;
    model.refresh(&opts, &state);
    expect("a countdown moving inside one tenth notifies nothing",
           changes == quiet && model.scanTimerRemainingDs() == 18);
    g_stub_scan_timing.remaining_ms = 1799U;
    model.refresh(&opts, &state);
    expect("the countdown moves on the tenth", model.scanTimerRemainingDs() == 17 && changes > quiet);

    model.clear();
    expect("a stopped session leaves no scan timing behind",
           !model.scanTimingVisible() && model.scanStayReason() == DSD_SCAN_STAY_NONE
               && model.scanStayPhrase().isEmpty() && !model.scanTimerLive() && model.scanTimerRemainingDs() == 0
               && model.scanTimerSpanMs() == 0 && model.scanDwellMs() == 0
               && model.scanDwellState() == DSD_APP_SCAN_DWELL_NONE && model.scanHoldMs() == 0
               && model.scanHangMs() == 0);

    /* Plain trunking and a plain single system are not rotations: there is no row to
     * stay on, so the view says nothing and the panel shows nothing. */
    opts.scanner_mode = 0;
    g_stub_scan_timing.reason = DSD_SCAN_STAY_IDLE_DWELL;
    opts.trunk_enable = 1;
    model.refresh(&opts, &state);
    expect("nothing is rotating: the row stays down",
           !model.scanTimingVisible() && model.scanStayPhrase().isEmpty() && model.scanDwellMs() == 0);
    opts.trunk_enable = 0;

    g_stub_scan_timing = dsd_app_scan_timing{};
    freeState(&state);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_options_readiness();
    test_scan_timing();
    test_decryption_metadata();
    test_site();
    test_quality();
    test_quality_without_identity(DSD_SYNC_DMR_BS_VOICE_POS, 0);
    test_quality_without_identity(DSD_SYNC_DMR_BS_VOICE_POS, 1);
    test_quality_without_identity(DSD_SYNC_NXDN_POS, 0);

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

    dsd_call_observation emergency = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 123U, 456U);
    emergency.kind = DSD_CALL_KIND_GROUP_VOICE;
    emergency.has_service_metadata = 1;
    emergency.emergency = 1;
    emergency.priority = 3;
    emergency.observed_m = dsd_time_now_monotonic_s();
    dsd_call_state_observe(&state, &emergency, DSD_CALL_BOUNDARY_BEGIN);
    model.refresh(&opts, &state);
    expect("emergency and priority reach metrics", model.slot1CallEmergency() && model.slot1CallPriority() == 3);
    model.clear();
    expect("clear resets emergency and priority", !model.slot1CallEmergency() && model.slot1CallPriority() == 0);
    dsd_state_ext_free_all(&state);

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

    /* A threshold at the display floor is still a threshold. Only a level that
     * gates nothing is off, and the panel has to be able to tell them apart --
     * both rendered as "-120 dB" there was no way to see which one was in force. */
    expect("a -120 dB threshold is not off", !model.squelchOff());

    opts.rtl_squelch_level = 0.0;
    model.refresh(&opts, &state);
    expect("a squelch that gates nothing reads as off", model.squelchOff());

    opts.rtl_squelch_level = dB_to_pwr(-120.0);
    model.refresh(&opts, &state);
    expect("a restored threshold stops reading as off", !model.squelchOff());
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
    expect("enter DMR row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("row selects GFSK", opts.mod_gfsk == 1);
    model.refresh(&opts, &state);
    expect("modulation control keeps configured C4FM", model.modulation() == 0);
    dsd_scan_mode_leave(&opts, &state);

    /* Every flag combination must mean the same thing inside a scope. */
    for (int flags = 0; flags < 8; flags++) {
        opts.mod_c4fm = (flags & 1) != 0;
        opts.mod_qpsk = (flags & 2) != 0;
        opts.mod_gfsk = (flags & 4) != 0;
        const int expected = dsd_opts_modulation(&opts);
        expect("enter modulation combination", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
        model.refresh(&opts, &state);
        expect("scoped modulation uses one-flag rule", model.modulation() == expected);
        dsd_scan_mode_leave(&opts, &state);
    }

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

    /* Scan hold and avoids (#380) read whichever rotation is running. Plain trunking
     * follows one system and is not a rotation, so the controls have nothing to act on. */
    model.refresh(&opts, &state);
    expect("no rotation at rest", !model.scanRotationActive());
    expect("no hold at rest", !model.scanHold());
    expect("no avoids at rest", model.scanAvoidCount() == 0);
    opts.trunk_enable = 1;
    model.refresh(&opts, &state);
    expect("plain trunking is not a rotation", !model.scanRotationActive());
    opts.trunk_enable = 0;
    state.lcn_scan_hold = 1;
    state.lcn_avoid_count = 3;
    state.trunk_scan_hold = 0;
    state.trunk_scan_avoided_count = 7;
    state.trunk_scan_active_avoided = 1;
    opts.scanner_mode = 1;
    model.refresh(&opts, &state);
    expect("-Y is a rotation", model.scanRotationActive());
    expect("-Y hold reads the scan-list flag", model.scanHold());
    expect("-Y avoids read the scan-list count", model.scanAvoidCount() == 3);
    expect("-Y never parks on an avoided row", !model.scanTargetAvoided());
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    model.refresh(&opts, &state);
    expect("trunk scan is a rotation", model.scanRotationActive());
    expect("trunk scan hold reads the coordinator's flag", !model.scanHold());
    expect("trunk scan avoids read the coordinator's count", model.scanAvoidCount() == 7);
    expect("trunk scan reports the avoided fallback", model.scanTargetAvoided());
    // WP-S1: the held snapshot supplies target identity and lifecycle clearing.
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "dispatch");
    state.trunk_scan_active_ordinal = 2;
    state.trunk_scan_target_count = 3;
    state.trunk_scan_hold = 1;
    model.refresh(&opts, &state);
    expect("trunk scan hold on", model.scanHold());
    expect("scan target identity", model.scanTargetId() == QStringLiteral("dispatch") && model.scanTargetOrdinal() == 2
                                       && model.scanTargetCount() == 3);
    /* Both flags set: --trunk-scan owns the tuner and the routing prefers it, so the
     * view reads the coordinator's fields, not the scan list's. */
    opts.scanner_mode = 1;
    state.trunk_scan_hold = 0;
    model.refresh(&opts, &state);
    expect("co-active rotations still count as one", model.scanRotationActive());
    expect("co-active hold reads the coordinator's flag", !model.scanHold());
    expect("co-active avoids read the coordinator's count", model.scanAvoidCount() == 7);
    expect("co-active avoided fallback comes from trunk scan", model.scanTargetAvoided());
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 0;
    state.lcn_scan_hold = 0;
    state.lcn_avoid_count = 0;
    state.trunk_scan_hold = 0;
    state.trunk_scan_avoided_count = 0;
    state.trunk_scan_active_avoided = 0;

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

        /* leadSlot is what MonitorScreen.qml binds heroSlot to, and it is one-based so 0
         * falls out as "neither" — the rebasing of dsd_app_lead_slot()'s -1 happens here
         * and nowhere else. An off-by-one hides the hero panel or headlines the wrong
         * slot, which no C test of dsd_app_lead_slot() and no QML case can see. */
        expect("the only live call headlines", model.leadSlot() == 1);

        /* Slot 2 live as well: an open epoch on the lower slot still outranks it. */
        dsd_call_observation other = named;
        other.slot = 1U;
        other.ota_target_id = 1202U;
        other.observed_m = 2.5;
        (void)dsd_call_state_observe(&state, &other, DSD_CALL_BOUNDARY_BEGIN);
        model.refresh(&opts, &state);
        expect("slot 2 is live too", model.slot2CallState() == 2);
        expect("between two live calls the lower slot headlines", model.leadSlot() == 1);

        /* With slot 1 ended and slot 2 still open, the open epoch wins outright — the
         * rule that made two surfaces name different units before it was shared. */
        (void)dsd_call_state_end(&state, 0U, 3.0);
        model.refresh(&opts, &state);
        expect("an open call outranks an ended one on a lower slot", model.leadSlot() == 2);

        (void)dsd_call_state_end(&state, 1U, 3.5);
    }

    /* Nothing on the air: one-based leadSlot answers 0 rather than naming slot 1. */
    model.clear();
    expect("a cleared model headlines no slot", model.leadSlot() == 0);

    /* A stopped session must not leave its answers behind for the next one. */
    state.synctype = DSD_SYNC_P25P1_POS;
    model.refresh(&opts, &state);
    expect("locked again before the stop", model.syncedHere());
    model.clear();
    expect("a cleared model reports no lock", !model.syncedHere());
    expect("a cleared model reports no tuner", !model.radioInput());
    expect("clear removes scan identity",
           model.scanTargetId().isEmpty() && model.scanTargetOrdinal() == 0 && model.scanTargetCount() == 0);

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

    /* A call with no name of its own on a named scan channel: the hero must show
     * the channel, the way the call history already does, and expose it on its own
     * so the panel can also show it beside a call that has a name. */
    {
        dsd_call_observation unnamed = {};
        unnamed.protocol = DSD_SYNC_NXDN_POS;
        unnamed.slot = 0U;
        unnamed.kind = DSD_CALL_KIND_GROUP_VOICE;
        unnamed.ota_target_id = 0U;
        unnamed.ota_source_id = 1U;
        unnamed.observed_m = 50.0;
        expect("a tg-0 call opens", dsd_call_state_observe(&state, &unnamed, DSD_CALL_BOUNDARY_BEGIN) == 1);
        expect("the state carries an event ring", state.event_history_s != nullptr);
        if (state.event_history_s != nullptr) {
            Event_History* staged = &state.event_history_s[0].Event_History_Items[0];
            DSD_SNPRINTF(staged->channel_label, sizeof(staged->channel_label), "%s", "Fire Dispatch");
            model.refresh(&opts, &state);
            expect("the slot exposes its scan channel", model.slot1Channel() == QStringLiteral("Fire Dispatch"));
            expect("a nameless call is named by its channel", model.slot1CallName() == QStringLiteral("Fire Dispatch"));
            expect("the talkgroup text stays the number", model.slot1TgText() == QStringLiteral("0"));
        }
    }

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "OK\n");
    return 0;
}
