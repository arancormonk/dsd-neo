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
#include <QtGlobal>
#include <cmath>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>

#include <dsd-neo/app_control/frontend.h>
#include <dsd-neo/app_control/rx_tone_view.h>
#include <dsd-neo/app_control/scan_timing_view.h>
#include <dsd-neo/core/analog_tone.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/analog_tones.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <dsd-neo/runtime/scan_row_edit.h>
#include <dsd-neo/runtime/squelch.h>

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include "metrics_model.h"

namespace {

int g_failures = 0;

/* What the stubbed frontend reports for the channel width. Per-case, because
 * every other case wants the at-rest 0. */
static int g_stub_channel_bandwidth_hz = 0;
static int g_stub_channel_bandwidth_dsp_limited = 0;
/* Whether the stubbed front end says a stream runs, and its demod rate (issue #525). */
static int g_stub_stream_active = 0;
/* The demodulator kind of the published analog width (issue #524): FM unless a case publishes an AM width. */
static int g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
static int g_stub_demod_rate_hz = 0;

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
    out->channel_bandwidth_dsp_limited = g_stub_channel_bandwidth_dsp_limited;
    out->stream_active = g_stub_stream_active;
    out->channel_analog_kind = g_stub_channel_analog_kind;
    out->demod_rate_hz = g_stub_demod_rate_hz;
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
test_lead_slot_keeps_earlier_call_and_quality() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    state.synctype = DSD_SYNC_P25P2_POS;
    dsd_qt::MetricsModel model;

    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 1U, 4242U, 1202U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.observed_m = dsd_decode_now_mono_s() - 2.0;
    expect("slot 2 opens first", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    state.p25_p2_voice_err_hist_len = 50;
    state.p25_p2_voice_err_hist_count[1] = 3;
    state.p25_p2_voice_err_hist_sum[1] = 6;
    model.refresh(&opts, &state);
    expect("slot 2 alone headlines with its quality", model.leadSlot() == 2 && model.voiceErrsValid()
                                                          && model.voiceErrsSamples() == 3
                                                          && model.voiceErrsPerFrame() == 2.0);

    call.slot = 0U;
    call.ota_target_id = 1201U;
    call.observed_m += 1.0;
    expect("slot 1 opens later", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    state.p25_p2_voice_err_hist_count[0] = 4;
    state.p25_p2_voice_err_hist_sum[0] = 20;
    model.refresh(&opts, &state);
    expect("earlier slot 2 call keeps the headline",
           model.slot1CallState() == 2 && model.slot2CallState() == 2 && model.leadSlot() == 2);
    expect("quality stays with the earlier slot 2 call", model.voiceErrsValid() && model.voiceErrsSamples() == 3
                                                             && model.voiceErrsPerFrame() == 2.0
                                                             && model.slot1VoiceErrsPerFrame() == 5.0);

    expect("earlier slot 2 call ends", dsd_call_state_end(&state, 1U, dsd_decode_now_mono_s()) == 1);
    model.refresh(&opts, &state);
    expect("remaining slot 1 call takes the headline",
           model.slot1CallState() == 2 && model.slot2CallState() == 3 && model.leadSlot() == 1);
    expect("quality moves to the remaining slot 1 call",
           model.voiceErrsValid() && model.voiceErrsSamples() == 4 && model.voiceErrsPerFrame() == 5.0);
    freeState(&state);
}

static void
test_lead_slot_change_alone_notifies() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    state.synctype = DSD_SYNC_P25P2_POS;
    dsd_qt::MetricsModel model;

    /* Stamps ahead of the poll keep both displayed durations at zero, even if the
     * test is descheduled. Restarting an epoch can then change only the lead. */
    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 4242U, 1201U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.observed_m = dsd_decode_now_mono_s() + 60.0;
    expect("initial lead opens", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    dsd_call_observation other = call;
    other.slot = 1U;
    other.ota_target_id = 1202U;
    other.observed_m += 0.0004;
    expect("companion opens", dsd_call_state_observe(&state, &other, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(&opts, &state);
    expect("initial lead is slot 1", model.leadSlot() == 1);

    int slot1_changes = 0;
    int slot2_changes = 0;
    int quality_changes = 0;
    int lead_changes = 0;
    int notified_lead = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::slot1Changed, [&]() { ++slot1_changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::slot2Changed, [&]() { ++slot2_changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::qualityChanged, [&]() { ++quality_changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::leadSlotChanged, [&]() {
        ++lead_changes;
        notified_lead = model.leadSlot();
    });

    call.observed_m += 0.0008;
    expect("initial lead restarts with the same identity",
           dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    model.refresh(&opts, &state);
    expect("slot fields and quality did not change", slot1_changes == 0 && slot2_changes == 0 && quality_changes == 0);
    expect("lead-only change publishes and notifies", model.leadSlot() == 2 && lead_changes == 1 && notified_lead == 2);
    model.refresh(&opts, &state);
    expect("unchanged lead is silent", lead_changes == 1);
    model.clear();
    expect("clear notifies the no-lead sentinel", model.leadSlot() == 0 && lead_changes == 2 && notified_lead == 0);
    freeState(&state);
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
test_direct_key_presence() {
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_MEMSET(&opts, 0, sizeof(opts));
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &state);
    expect("empty snapshot has no direct keys", !model.directKeys());
    // Legacy writers can provide material without presence metadata.
    for (auto* key : {&state.K, &state.R, &state.RR, &state.K1}) {
        *key = 0x1234;
        model.refresh(&opts, &state);
        expect("legacy nonzero key remains configured", model.directKeys());
        *key = 0;
        model.refresh(&opts, &state);
        expect("cleared legacy key is absent", !model.directKeys());
    }
    // Presence must not depend on a nonzero key value or on an active call.
    state.basic_key_present = 1;
    model.refresh(&opts, &state);
    expect("zero basic key remains configured", model.directKeys());
    state.basic_key_present = 0;
    for (auto& present : state.scalar_key_present) {
        present = 1;
        model.refresh(&opts, &state);
        expect("zero scalar key remains configured", model.directKeys());
        present = 0;
    }
    state.hytera_key_segments = 1;
    model.refresh(&opts, &state);
    expect("Hytera presence is published", model.directKeys());
    state.hytera_key_segments = 0;
    state.aes_key_loaded[1] = 1;
    model.refresh(&opts, &state);
    expect("direct AES presence is published", model.directKeys());
    state.keyloader = 1;
    model.refresh(&opts, &state);
    expect("automatic slot activation is not a direct override", !model.directKeys());
    model.clear();
    expect("stop clears direct-key status", !model.directKeys());
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
test_temporary_lockout_metrics() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &state);
    expect("saved lockout default published", model.persistTgLockouts());
    opts.persist_tg_lockouts = 0;
    expect("seed metric avoid", dsd_tg_policy_session_avoid_add(&state, 100) == 0);
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });
    model.refresh(&opts, &state);
    expect("lockout state notifies", changes == 1 && !model.persistTgLockouts() && model.temporaryTgAvoidCount() == 1);
    uint64_t context = 0;
    dsd_tg_policy_table_version(&state, &context, nullptr);
    expect("current list context published losslessly", model.tgPolicyContext() == QString::number(context));
    model.refresh(&opts, &state);
    expect("stable lockouts do not notify twice", changes == 1);
    dsd_tg_policy_session_avoid_clear(&state);
    model.refresh(&opts, &state);
    expect("clear count notifies", changes == 2 && model.temporaryTgAvoidCount() == 0);
    model.clear();
    expect("stopped avoids unavailable", model.tgPolicyContext().isEmpty() && model.temporaryTgAvoidCount() == 0);
    freeState(&state);
}

static void
test_call_skip_metrics() {
    static dsd_opts opts;
    static dsd_state state;
    static dsd_state snapshot;
    initOpts(&opts);
    initState(&state);
    initState(&snapshot);
    dsd_qt::MetricsModel model;
    model.refresh(&opts, &snapshot);
    int changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++changes; });
    const double now = dsd_decode_now_mono_s();
    expect("seed metric skip", dsd_tg_policy_call_skip_arm(&state, 100, 1, 0, now) == 0);
    expect("publish skip snapshot", dsd_tg_policy_copy_snapshot(&snapshot, &state) == 0);
    model.refresh(&opts, &snapshot);
    expect("skip count notifies", changes == 1 && model.callSkipCount() == 1 && model.temporaryTgAvoidCount() == 0);
    model.refresh(&opts, &snapshot);
    expect("stable skip count does not notify twice", changes == 1);
    dsd_tg_policy_call_skip_clear(&state);
    model.refresh(&opts, &snapshot);
    expect("held snapshot keeps skip", changes == 1 && model.callSkipCount() == 1);
    expect("publish skip clear through reuse", dsd_tg_policy_copy_snapshot(&snapshot, &state) == 0);
    model.refresh(&opts, &snapshot);
    expect("skip clear notifies", changes == 2 && model.callSkipCount() == 0);
    expect("seed expired metric skip",
           dsd_tg_policy_call_skip_arm(&state, 100, 1, 0, now - DSD_TG_CALL_SKIP_QUIET_S - 1.0) == 0);
    expect("publish expired skip", dsd_tg_policy_copy_snapshot(&snapshot, &state) == 0);
    model.refresh(&opts, &snapshot);
    expect("expired skips not counted", changes == 2 && model.callSkipCount() == 0);
    model.clear();
    expect("stopped skips unavailable", model.callSkipCount() == 0);
    freeState(&snapshot);
    freeState(&state);
}

/*
 * The call lines, the call-skip count and decodeNowMs read the decode clock the decoder stamps
 * their inputs on, so under the TEST source they follow it with the real clock untouched: a call
 * begun at T reads 7 s old at T + 7.5 s, and a skip armed at T lapses once its quiet window has
 * passed in decode time.
 */
static void
test_decode_clock_readings() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    const uint64_t t0_ns = 1000000000000000000ULL; // 1e9 s: far from any platform clock reading.
    dsd_decode_clock_use_test(t0_ns);
    dsd_qt::MetricsModel model;
    int now_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::decodeNowMsChanged, [&]() { ++now_changes; });

    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_P25P2_POS, 0U, 123U, 456U);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    // No observed_m: the call store stamps the start from the decode clock.
    expect("decode-clock call begins", dsd_call_state_observe(&state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1);
    expect("decode-clock skip armed", dsd_tg_policy_call_skip_arm(&state, 100, 1, 0, dsd_decode_now_mono_s()) == 0);

    dsd_decode_clock_test_set_ns(t0_ns + 7500000000ULL);
    model.refresh(&opts, &state);
    expect("call line ages on the decode clock", model.slot1CallSeconds() == 7);
    expect("skip counted inside its decode-time quiet window", model.callSkipCount() == 1);
    expect("decodeNowMs is the decode clock's wall time", model.decodeNowMs() == 1000000007500LL);
    expect("decodeNowMs announces a new decode second", now_changes == 1);
    model.refresh(&opts, &state);
    expect("the same decode second stays quiet", now_changes == 1);

    dsd_decode_clock_test_set_ns(t0_ns + static_cast<uint64_t>((DSD_TG_CALL_SKIP_QUIET_S + 1.0) * 1e9));
    model.refresh(&opts, &state);
    expect("skip lapses on the decode clock", model.callSkipCount() == 0);
    expect("decodeNowMs announces the next decode second", now_changes == 2);

    dsd_decode_clock_use_system();
    freeState(&state);
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
 * purpose: only changes to the rendered tenth notify the timing row, and they
 * never notify the unrelated control group.
 */
static void
test_scan_timing() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    int control_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::scanTimingChanged, [&]() { ++changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++control_changes; });

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
    expect("scan timing has its own notification", changes > 0);
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
    g_stub_scan_timing.remaining_ms = 1855U;
    g_stub_scan_timing.span_ms = 3000U;
    g_stub_scan_timing.show_hold = 1U;
    g_stub_scan_timing.hold_ms = 2000U;
    model.refresh(&opts, &state);
    expect("an idle dwell counts down", model.scanTimerLive() && model.scanTimerSpanMs() == 3000);
    expect("the countdown reaches the row in tenths", model.scanTimerRemainingDs() == 18);
    expect("a conventional row shows its activity hold", model.scanHoldMs() == 2000);
    expect("the dwell the countdown already shows is not printed twice", model.scanDwellMs() == 0);

    const int quiet = changes;
    const int controls_settled = control_changes;
    g_stub_scan_timing.remaining_ms = 1801U;
    model.refresh(&opts, &state);
    expect("a countdown moving inside one tenth notifies nothing",
           changes == quiet && model.scanTimerRemainingDs() == 18);
    g_stub_scan_timing.remaining_ms = 1799U;
    model.refresh(&opts, &state);
    expect("the countdown moves on the tenth", model.scanTimerRemainingDs() == 17 && changes > quiet);
    expect("countdowns do not notify unrelated controls", control_changes == controls_settled);

    // The -Y publisher saturates large accepted -t values at UINT32_MAX milliseconds.
    // This must remain positive at the Qt property boundary, including through QVariant.
    g_stub_scan_timing.span_ms = UINT32_MAX;
    g_stub_scan_timing.show_hang = 1U;
    g_stub_scan_timing.hang_ms = UINT32_MAX;
    model.refresh(&opts, &state);
    expect("a large timer total does not become negative",
           model.property("scanTimerSpanMs").toULongLong() == static_cast<qulonglong>(UINT32_MAX));
    expect("a large effective hangtime does not become negative",
           model.property("scanHangMs").toULongLong() == static_cast<qulonglong>(UINT32_MAX));

    /* The per-visit cap (#507) rides the same row. It is not driven by the stay reason,
     * so it reads out beside whatever else holds the receiver, and its countdown is in
     * tenths for the same reason the window's is. */
    g_stub_scan_timing.show_visit = 1U;
    g_stub_scan_timing.visit_ms = 20000U;
    g_stub_scan_timing.visit_live = 1U;
    g_stub_scan_timing.visit_remaining_ms = 12349U;
    model.refresh(&opts, &state);
    expect("the visit cap reads out with its countdown",
           model.scanVisitMs() == 20000 && model.scanVisitLive() && model.scanVisitRemainingDs() == 123);
    const int capped = changes;
    g_stub_scan_timing.visit_remaining_ms = 12301U;
    model.refresh(&opts, &state);
    expect("a cap moving inside one tenth notifies nothing", changes == capped && model.scanVisitRemainingDs() == 123);

    /* Suspended by a hold: the cap still applies to the row, so its width is stated, but
     * there is no countdown -- zero tenths would read as a visit that just ran out. */
    g_stub_scan_timing.visit_live = 0U;
    g_stub_scan_timing.visit_remaining_ms = 0U;
    model.refresh(&opts, &state);
    expect("a suspended cap keeps its width and drops the countdown",
           model.scanVisitMs() == 20000 && !model.scanVisitLive() && model.scanVisitRemainingDs() == 0);
    expect("suspending the cap notifies the scan timing group", changes > capped);

    /* Withheld by the view: no cap is in force, so no number the row could print arrives. */
    g_stub_scan_timing.show_visit = 0U;
    model.refresh(&opts, &state);
    expect("no cap on air leaves nothing to print", model.scanVisitMs() == 0);

    model.clear();
    expect("a stopped session leaves no scan timing behind",
           !model.scanTimingVisible() && model.scanStayReason() == DSD_SCAN_STAY_NONE
               && model.scanStayPhrase().isEmpty() && !model.scanTimerLive() && model.scanTimerRemainingDs() == 0
               && model.scanTimerSpanMs() == 0 && model.scanDwellMs() == 0
               && model.scanDwellState() == DSD_APP_SCAN_DWELL_NONE && model.scanHoldMs() == 0
               && model.scanHangMs() == 0 && model.scanVisitMs() == 0 && !model.scanVisitLive()
               && model.scanVisitRemainingDs() == 0);

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

/*
 * The received sub-audible tone or code (#522, #523), read through the real app-control view:
 * what the monitor row shows for each state, the configured policy kept apart from it, and the
 * row cleared when the session stops -- which is MetricsModel::clear(), the path UiController
 * takes on stop, not something the QML fixture can prove.
 */
static void
test_rx_tone() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int changes = 0;
    int scan_changes = 0;
    int configured_changes = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneChanged, [&]() { ++changes; });
    QObject::connect(&model, &dsd_qt::MetricsModel::scanTimingChanged, [&]() { ++scan_changes; });
    /* The configured policy has its own signal, so nothing received ever announces it. */
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneConfiguredTextChanged, [&]() { ++configured_changes; });
    expect("the configured policy reads off before the first frame",
           model.rxToneConfiguredText() == QStringLiteral("off"));

    /* Digital decoding: no detection runs, so a stale publication must not reach the row. */
    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 1000;
    model.refresh(&opts, &state);
    expect("no FM monitor, no received-tone row", !model.rxToneVisible() && model.rxToneText().isEmpty());
    expect("the configured policy reads off", model.rxToneConfiguredText() == QStringLiteral("off"));

    /* The analog FM monitor locked on 100.0 Hz. */
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    const int before = changes;
    model.refresh(&opts, &state);
    expect("a locked tone shows its value", model.rxToneVisible() && model.rxToneStatus() == DSD_APP_RX_TONE_LOCKED
                                                && model.rxToneText() == QStringLiteral("CTCSS 100.0 Hz")
                                                && model.rxToneKind() == DSD_ANALOG_TONE_KIND_CTCSS
                                                && model.rxToneTenthsHz() == 1000 && model.rxToneCarrier());
    expect("the received tone has its own notification",
           changes > before && scan_changes == 0 && configured_changes == 0);
    expect("received and configured stay apart", model.rxToneConfiguredText() == QStringLiteral("off"));
    const int settled = changes;
    model.refresh(&opts, &state);
    expect("an unchanged tone does not notify twice", changes == settled);

    /* A live stream input that went quiet: past the deadline the tap published, the decoder is
       waiting for samples and its last word no longer describes the channel. The frame's own
       clock ages it to no carrier; a deadline still ahead keeps the tone. */
    state.analog_rx.stale_after_ms = 1U;
    model.refresh(&opts, &state);
    expect("a paused stream's tone reads no carrier", model.rxToneVisible()
                                                          && model.rxToneStatus() == DSD_APP_RX_TONE_NO_CARRIER
                                                          && model.rxToneText() == QStringLiteral("\u2014")
                                                          && model.rxToneTenthsHz() == 0 && !model.rxToneCarrier());
    state.analog_rx.stale_after_ms = UINT64_MAX;
    model.refresh(&opts, &state);
    expect("a stream inside its deadline keeps the tone",
           model.rxToneStatus() == DSD_APP_RX_TONE_LOCKED && model.rxToneText() == QStringLiteral("CTCSS 100.0 Hz"));
    state.analog_rx.stale_after_ms = 0U;

    /* The policy verdict field is reserved: no value of it moves either text. */
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    model.refresh(&opts, &state);
    expect("a gate value changes neither the received nor the configured text",
           model.rxToneText() == QStringLiteral("CTCSS 100.0 Hz")
               && model.rxToneConfiguredText() == QStringLiteral("off"));
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;

    /* A retune resets the publication: the row stays, with nothing heard yet. */
    DSD_MEMSET(&state.analog_rx, 0, sizeof(state.analog_rx));
    state.analog_rx.generation = 2U;
    model.refresh(&opts, &state);
    expect("after a retune the tone is gone",
           model.rxToneVisible() && model.rxToneStatus() == DSD_APP_RX_TONE_NO_CARRIER
               && model.rxToneText() == QStringLiteral("\u2014") && model.rxToneTenthsHz() == 0);

    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    model.refresh(&opts, &state);
    expect("a carrier under evaluation reads detecting",
           model.rxToneStatus() == DSD_APP_RX_TONE_DETECTING && model.rxToneText() == QStringLiteral("detecting"));
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    model.refresh(&opts, &state);
    expect("a carrier with no tone reads none",
           model.rxToneStatus() == DSD_APP_RX_TONE_NONE && model.rxToneText() == QStringLiteral("none"));

    /* A received DCS code (#523): both standard spellings of its signal, canonical first, with
       leading zeros, apart from the configured policy, which neither it nor a gate value
       moves. */
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state.analog_rx.dcs_code = 023;
    state.analog_rx.dcs_inverted = 0;
    const int before_code = changes;
    model.refresh(&opts, &state);
    expect("a locked code shows both of its spellings",
           model.rxToneStatus() == DSD_APP_RX_TONE_LOCKED && model.rxToneText() == QStringLiteral("DCS D023N / D047I")
               && model.rxToneKind() == DSD_ANALOG_TONE_KIND_DCS && model.rxToneDcsCode() == 023
               && !model.rxToneDcsInverted() && model.rxToneDcsAliasCode() == 047 && model.rxToneDcsAliasInverted()
               && model.rxToneTenthsHz() == 0);
    expect("the received code notifies its own group", changes > before_code && configured_changes == 0);
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    model.refresh(&opts, &state);
    expect("a gate value changes neither text for a code",
           model.rxToneText() == QStringLiteral("DCS D023N / D047I")
               && model.rxToneConfiguredText() == QStringLiteral("off"));
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_OFF;
    state.analog_rx.dcs_code = 0754;
    model.refresh(&opts, &state);
    expect("another code replaces it", model.rxToneText() == QStringLiteral("DCS D754N / D116I")
                                           && model.rxToneDcsCode() == 0754 && model.rxToneDcsAliasCode() == 0116);

    /* A retune clears the code as it clears a tone. */
    DSD_MEMSET(&state.analog_rx, 0, sizeof(state.analog_rx));
    state.analog_rx.generation = 3U;
    model.refresh(&opts, &state);
    expect("after a retune the code is gone",
           model.rxToneStatus() == DSD_APP_RX_TONE_NO_CARRIER && model.rxToneKind() == 0 && model.rxToneDcsCode() == 0
               && !model.rxToneDcsInverted() && model.rxToneDcsAliasCode() == 0 && !model.rxToneDcsAliasInverted());
    state.analog_rx.carrier_open = 1;

    /* Stop with a code locked clears it too. */
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_DCS;
    state.analog_rx.dcs_code = 047;
    model.refresh(&opts, &state);
    expect("locked on a code before a stop", model.rxToneDcsCode() == 047 && model.rxToneDcsAliasCode() == 023
                                                 && model.rxToneText() == QStringLiteral("DCS D047N / D023I"));
    model.clear();
    expect("stop clears the received code", !model.rxToneVisible() && model.rxToneText().isEmpty()
                                                && model.rxToneKind() == 0 && model.rxToneDcsCode() == 0
                                                && !model.rxToneDcsInverted() && model.rxToneDcsAliasCode() == 0
                                                && !model.rxToneDcsAliasInverted());
    expect("stop keeps the configured policy with a code too", model.rxToneConfiguredText() == QStringLiteral("off"));
    DSD_MEMSET(&state.analog_rx, 0, sizeof(state.analog_rx));
    state.analog_rx.carrier_open = 1;

    /* Stop: clear() returns every rxTone reading to unknown and says so. */
    model.refresh(&opts, &state);
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 1318;
    model.refresh(&opts, &state);
    expect("locked again before the stop", model.rxToneTenthsHz() == 1318);
    const int before_stop = changes;
    model.clear();
    expect("stop clears the received tone", !model.rxToneVisible() && model.rxToneStatus() == DSD_APP_RX_TONE_HIDDEN
                                                && model.rxToneText().isEmpty() && model.rxToneKind() == 0
                                                && model.rxToneTenthsHz() == 0 && !model.rxToneCarrier());
    /* The configured policy is configuration, not session state: stop leaves it as it was. */
    expect("stop keeps the configured policy", model.rxToneConfiguredText() == QStringLiteral("off"));
    expect("stop notifies the received-tone group", changes > before_stop);
    expect("nothing received ever announced the configured policy", configured_changes == 0);

    freeState(&state);
}

/*
 * The Tone filter row (#527) through the real app-control view: shown while a policy is in force where detection runs,
 * the policy text on its configuration signal and the verdict on its own, neither moving the received tone, and the
 * verdict gone on stop while the policy stays.
 */
static void
test_tone_filter() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int received = 0;
    int configured = 0;
    int verdicts = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneChanged, [&]() { ++received; });
    QObject::connect(&model, &dsd_qt::MetricsModel::rxToneConfiguredTextChanged, [&]() { ++configured; });
    QObject::connect(&model, &dsd_qt::MetricsModel::toneFilterChanged, [&]() { ++verdicts; });
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    opts.analog_tone_filter = DSD_TONE_FILTER_ALLOW;
    expect("the list parses", dsd_tone_set_parse("100.0/D023N", &opts.analog_tone_set, nullptr, 0) == 0);
    state.analog_rx.carrier_open = 1;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_ACQUIRING;
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_PENDING;
    model.refresh(&opts, &state);
    expect("the policy row shows the policy and its check",
           model.toneFilterVisible() && model.rxToneConfiguredText() == QStringLiteral("allow 100.0 Hz/D023N")
               && model.toneFilterGate() == DSD_ANALOG_TONE_GATE_PENDING
               && model.toneFilterStatusText() == QStringLiteral("muted: checking tone"));
    expect("the policy and its verdict each notify", configured == 1 && verdicts == 1);

    const int received_before = received;
    const int configured_before = configured;
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_LOCKED;
    state.analog_rx.tone_kind = DSD_ANALOG_TONE_KIND_CTCSS;
    state.analog_rx.ctcss_tenths_hz = 670;
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_REJECTED;
    model.refresh(&opts, &state);
    expect("a rejected tone", model.toneFilterStatusText() == QStringLiteral("muted: not allowed")
                                  && model.rxToneText() == QStringLiteral("CTCSS 67.0 Hz"));
    expect("the verdict never announces the policy", configured == configured_before && received > received_before);
    state.analog_rx.tone_state = DSD_ANALOG_TONE_STATE_NONE;
    state.analog_rx.tone_kind = 0;
    state.analog_rx.ctcss_tenths_hz = 0;
    state.analog_rx.gate_no_tone = 1;
    model.refresh(&opts, &state);
    expect("rejected for want of a tone", model.toneFilterStatusText() == QStringLiteral("muted: no tone"));
    state.analog_rx.gate = DSD_ANALOG_TONE_GATE_ALLOWED;
    state.analog_rx.gate_no_tone = 0;
    model.refresh(&opts, &state);
    expect("passing", model.toneFilterStatusText() == QStringLiteral("passing"));

    /* Stop: the verdict goes, the policy is configuration and stays. */
    model.clear();
    expect("stop clears the verdict",
           !model.toneFilterVisible() && model.toneFilterStatusText().isEmpty() && model.toneFilterGate() == 0);
    expect("stop keeps the policy", model.rxToneConfiguredText() == QStringLiteral("allow 100.0 Hz/D023N"));
    /* A digital mode: no detection, no row, whatever is configured. */
    opts.analog_only = 0;
    model.refresh(&opts, &state);
    expect("no detection, no policy row", !model.toneFilterVisible() && model.toneFilterStatusText().isEmpty());
    freeState(&state);
}

/*
 * What the live tone-filter editor opens on, through the real app-control view: the row is reachable wherever detection
 * runs, a policy or none (toneFilterEditable); the configured mode and list, spelled as written, on their own signal
 * and kept across a stop as configuration; and whether a scan row's own policy shadows an edit. Never the row's policy.
 */
static void
test_tone_filter_editor_setting() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int settings = 0;
    int verdicts = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::toneFilterSettingChanged, [&]() { ++settings; });
    QObject::connect(&model, &dsd_qt::MetricsModel::toneFilterChanged, [&]() { ++verdicts; });
    expect("the editor opens on off before the first frame",
           model.toneFilterConfiguredMode() == DSD_TONE_FILTER_OFF && model.toneFilterConfiguredList().isEmpty()
               && !model.toneFilterEditable() && !model.toneFilterRowOverride());
    opts.analog_only = 1;
    opts.monitor_input_audio = 1;
    opts.audio_in_type = AUDIO_IN_WAV;
    opts.wav_sample_rate = 48000;
    model.refresh(&opts, &state);
    expect("no policy, but the editor is reachable where detection runs",
           model.toneFilterEditable() && !model.toneFilterVisible()
               && model.toneFilterConfiguredMode() == DSD_TONE_FILTER_OFF);
    expect("reachability is session state", verdicts == 1 && settings == 0);

    opts.analog_tone_filter = DSD_TONE_FILTER_BLOCK;
    expect("the list parses", dsd_tone_set_parse("D023I/100", &opts.analog_tone_set, nullptr, 0) == 0);
    model.refresh(&opts, &state);
    expect("the configured policy as written",
           model.toneFilterConfiguredMode() == DSD_TONE_FILTER_BLOCK
               && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I"));
    expect("the setting announces itself once", settings == 1);
    model.refresh(&opts, &state);
    expect("an unchanged setting stays quiet", settings == 1);

    /* An nfm row's own policy on air: the editor still opens on the configured one, and knows the row shadows it. */
    expect("scope",
           dsd_scan_mode_begin(&opts, &state) == 0 && dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
    dsd_scan_option_values row = {};
    row.present = DSD_SCAN_OPT_TONE;
    row.tone_filter = DSD_TONE_FILTER_ALLOW;
    expect("row list", dsd_tone_set_parse("67.0", &row.tone_set, nullptr, 0) == 0);
    expect("row installed", dsd_scan_mode_options(&opts, &state, &row) == 0);
    model.refresh(&opts, &state);
    expect("under a row: the configured policy, shadowed",
           model.toneFilterConfiguredMode() == DSD_TONE_FILTER_BLOCK
               && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I") && model.toneFilterRowOverride());
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("the row gone, nothing shadows", !model.toneFilterRowOverride());

    /* Stop: the configured policy stays for the next session's editor; reachability goes. */
    const int settings_before = settings;
    model.clear();
    expect("stop keeps the configured policy", model.toneFilterConfiguredMode() == DSD_TONE_FILTER_BLOCK
                                                   && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I")
                                                   && settings == settings_before);
    expect("stop takes the editor away", !model.toneFilterEditable() && !model.toneFilterRowOverride());
    /* A digital mode: nothing to edit on screen, the setting still read. */
    opts.analog_only = 0;
    model.refresh(&opts, &state);
    expect("no detection, no editor",
           !model.toneFilterEditable() && model.toneFilterConfiguredList() == QStringLiteral("100.0/D023I"));
    freeState(&state);
}

/* Issue #518: the scan row on air for the "this channel" editors, from the app-control scan row view: the name and the
 * fields it takes, sets in its list and runs an edit of, while the scanner that published it runs. */
static void
test_scan_row() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controls; });
    model.refresh(&opts, &state);
    expect("no scan row, nothing to edit", !model.scanRowActive() && model.scanRowEditable() == 0);
    opts.trunk_scan_enabled = 1;
    state.scan_row_scanner = (uint8_t)DSD_SCAN_ROW_SCANNER_TRUNK_SCAN;
    state.scan_row_session = 5U;
    state.scan_row_index = 0;
    state.scan_row_editable = (uint8_t)(DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN);
    state.scan_row_listed = (uint8_t)DSD_SCAN_ROW_FIELD_SQUELCH;
    state.scan_row_edited = (uint8_t)DSD_SCAN_ROW_FIELD_GAIN;
    DSD_SNPRINTF(state.trunk_scan_active_id, sizeof state.trunk_scan_active_id, "%s", "county-p25");
    const int before = controls;
    model.refresh(&opts, &state);
    expect("a trunk target on air",
           model.scanRowActive() && model.scanRowLabel() == QStringLiteral("county-p25")
               && model.scanRowEditable() == (DSD_SCAN_ROW_FIELD_SQUELCH | DSD_SCAN_ROW_FIELD_GAIN)
               && model.scanRowListed() == DSD_SCAN_ROW_FIELD_SQUELCH
               && model.scanRowEdited() == DSD_SCAN_ROW_FIELD_GAIN);
    expect("the row announces itself", controls == before + 1);
    expect("the readings are the row's", model.scanRowSynced());
    /* The width setting a "this channel" step starts from is the options snapshot's, the row's own while on air. */
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 12500;
    model.refresh(&opts, &state);
    expect("the width setting in force", model.analogBandwidthSettingHz() == 12500 && controls == before + 2);
    opts.analog_nfm_bandwidth_hz = 0;
    model.refresh(&opts, &state);
    expect("the default's setting", model.analogBandwidthSettingHz() == 0 && controls == before + 3);
    /* Options from another row's scope (read between the decoder's two publishes): the row stays on air, and the
       readings beside it are not its own until the next pair. */
    state.scan_row_scope_seq = opts.scan_row_scope_seq + 1U;
    model.refresh(&opts, &state);
    expect("snapshots of two scopes", model.scanRowActive() && !model.scanRowSynced() && controls == before + 4);
    state.scan_row_scope_seq = opts.scan_row_scope_seq;
    model.refresh(&opts, &state);
    expect("one scope again", model.scanRowSynced() && controls == before + 5);
    state.scan_row_edited = 0U;
    model.refresh(&opts, &state);
    expect("an edit dropped is announced", model.scanRowEdited() == 0 && controls == before + 6);
    opts.trunk_scan_enabled = 0;
    model.refresh(&opts, &state);
    expect("the scanner gone, no row", !model.scanRowActive() && model.scanRowLabel().isEmpty());
    freeState(&state);
}

/* Issue #621: on audio input with a rigctl peer (-U) that demodulates it, the analog width is the passband the peer is
 * asked for. The monitor's Passband row offers it, no DSP rate bounds it, and an unset NFM width stands for -B (or the
 * peer's own passband without one), which the sheet shows and steps from. Without the peer PCM input filters nothing,
 * and a radio input has a channel filter instead. */
static void
test_rigctl_audio_passband() {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    dsd_qt::MetricsModel model;
    int tuner = 0;
    int controls = 0;
    QObject::connect(&model, &dsd_qt::MetricsModel::tunerChanged, [&]() { ++tuner; });
    QObject::connect(&model, &dsd_qt::MetricsModel::controlChanged, [&]() { ++controls; });

    opts.audio_in_type = AUDIO_IN_TCP;
    opts.analog_only = 0;
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    opts.analog_nfm_bandwidth_hz = 0;
    opts.analog_am_bandwidth_hz = 0;
    opts.setmod_bw = 12500;
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;

    /* A digital session with no width set: the peer changes no reading and offers nothing, so the flag alone moves the
     * tuner group, and the spelling of an unset width (-B 16 kHz, the same width as the default) the control group. */
    opts.setmod_bw = 16000;
    model.refresh(&opts, &state);
    int tuner_before = tuner;
    int controls_before = controls;
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = (dsd_socket_t)5;
    model.refresh(&opts, &state);
    expect("digital peer: the passband flag", model.peerPassband() && model.analogBandwidthReading().isEmpty()
                                                  && !model.nfmBandwidthOffered() && !model.amBandwidthOffered());
    expect("digital peer: the flag alone moves the tuner group", tuner == tuner_before + 1);
    expect("digital peer: no analog monitor runs, so no passband is in force", !model.passbandInForce());
    expect("digital peer: the spelling alone moves the control group",
           controls == controls_before + 1 && model.nfmBandwidthUnsetText() == QStringLiteral("-B 16 kHz")
               && model.nfmBandwidthUnsetHz() == 16000);
    /* An explicit NFM default on the digital session offers the NFM width, and the reading is what the peer is asked
     * for now, FM at -B, under a typed digital row on air as well: never the configured width, which nothing asks
     * for (the Passband row's summary and the terminal's field read it). */
    opts.analog_nfm_bandwidth_hz = 20000;
    opts.setmod_bw = 12500;
    expect("digital peer: typed digital row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("digital peer: typed digital row options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
    model.refresh(&opts, &state);
    expect("digital peer, explicit width: NFM offered", model.nfmBandwidthOffered() && !model.passbandInForce());
    expect("digital peer, explicit width: the reading is -B",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)") && !model.analogBandwidthAm());
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("digital peer, explicit width, no row: the reading is -B",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)"));
    opts.analog_nfm_bandwidth_hz = 0;
    opts.setmod_bw = 16000;
    model.refresh(&opts, &state);
    expect("digital peer, no width: no reading", model.analogBandwidthReading().isEmpty());
    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    opts.setmod_bw = 12500;

    /* The analog FM monitor (-fA) without the peer. */
    opts.analog_only = 1;
    model.refresh(&opts, &state);
    expect("no peer: no passband", !model.peerPassband());
    expect("no peer: no width offered", !model.nfmBandwidthOffered() && !model.amBandwidthOffered());
    expect("no peer: PCM input filters nothing",
           model.analogBandwidthReading() == QStringLiteral("not used on PCM input"));
    expect("no peer: an unset width reads as the default",
           model.nfmBandwidthUnsetText() == QStringLiteral("default")
               && model.amBandwidthUnsetText() == QStringLiteral("default"));
    expect("no peer: an unset width stands for the kind's default",
           model.nfmBandwidthUnsetHz() == 16000 && model.amBandwidthUnsetHz() == 6000);

    /* The peer connects: the FM monitor asks it for -B, standing in for the unset NFM width. */
    opts.use_rigctl = 1;
    opts.rigctl_sockfd = (dsd_socket_t)5;
    tuner_before = tuner;
    controls_before = controls;
    model.refresh(&opts, &state);
    expect("peer: the passband", model.peerPassband());
    expect("peer: the FM monitor runs, so its passband is in force", model.passbandInForce());
    expect("peer: the NFM width offered, AM's not", model.nfmBandwidthOffered() && !model.amBandwidthOffered());
    expect("peer: no DSP rate bounds the passband", model.analogBandwidthMaxHz() == 0);
    expect("peer: -B stands in for the unset width", model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)"));
    expect("peer: an unset NFM width reads as -B, AM's as the default",
           model.nfmBandwidthUnsetText() == QStringLiteral("-B 12.5 kHz")
               && model.amBandwidthUnsetText() == QStringLiteral("default"));
    expect("peer: an unset NFM width steps from -B, AM's from its default",
           model.nfmBandwidthUnsetHz() == 12500 && model.amBandwidthUnsetHz() == 6000);
    expect("peer: the flag moves the tuner group", tuner == tuner_before + 1);
    expect("peer: the stand-ins move the control group", controls == controls_before + 1);

    /* With the NFM width unset a typed digital row on air asks for the same -B, so the reading holds, and the flag
     * alone moves the tuner group. */
    expect("peer: typed digital row over -B", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("peer: typed digital row over -B options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
    tuner_before = tuner;
    model.refresh(&opts, &state);
    expect("peer: under a typed digital row -B is no setting's passband",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)") && !model.passbandInForce());
    expect("peer: the in-force flag alone moves the tuner group", tuner == tuner_before + 1);
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("peer: the row's leave puts -B back in force", model.passbandInForce());

    /* An explicit NFM width is the passband asked for; -B still names what an unset one would be. */
    opts.analog_nfm_bandwidth_hz = 20000;
    model.refresh(&opts, &state);
    expect("peer: an explicit width is the passband", model.analogBandwidthReading() == QStringLiteral("20 kHz"));
    expect("peer: the explicit width is configured", model.nfmBandwidthConfiguredHz() == 20000);
    expect("peer: -B still stands in for an unset width",
           model.nfmBandwidthUnsetText() == QStringLiteral("-B 12.5 kHz"));

    /* A typed digital row on air keeps the peer following at -B: the reading is not the configured width's, and the
     * flag alone says so, the configured width staying what a step moves. */
    expect("peer: typed digital row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    expect("peer: typed digital row options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
    model.refresh(&opts, &state);
    expect("peer: under a typed digital row the peer follows at -B",
           model.analogBandwidthReading() == QStringLiteral("12.5 kHz (-B)") && !model.passbandInForce());
    expect("peer: under a typed digital row the configured width stays", model.nfmBandwidthConfiguredHz() == 20000);
    dsd_scan_mode_leave(&opts, &state);
    model.refresh(&opts, &state);
    expect("peer: the row's leave brings the configured passband back",
           model.analogBandwidthReading() == QStringLiteral("20 kHz") && model.passbandInForce());

    /* -B moves under the explicit width: the reading holds, and only what an unset width stands for changes. */
    opts.setmod_bw = 16000;
    tuner_before = tuner;
    controls_before = controls;
    model.refresh(&opts, &state);
    expect("peer: -B under an explicit width leaves the reading",
           model.analogBandwidthReading() == QStringLiteral("20 kHz") && tuner == tuner_before);
    expect("peer: -B under an explicit width moves the stand-in",
           model.nfmBandwidthUnsetText() == QStringLiteral("-B 16 kHz") && model.nfmBandwidthUnsetHz() == 16000
               && controls == controls_before + 1);

    /* Neither a width nor -B: the peer keeps its own passband. */
    opts.analog_nfm_bandwidth_hz = 0;
    opts.setmod_bw = 0;
    model.refresh(&opts, &state);
    expect("peer: without -B the peer's own passband", model.analogBandwidthReading() == QStringLiteral("peer's own"));
    expect("peer: an unset width stands for the peer's own",
           model.nfmBandwidthUnsetText() == QStringLiteral("peer's own") && model.nfmBandwidthUnsetHz() == 0);

    /* A radio input has a channel filter, whatever rigctl follows. */
    opts.setmod_bw = 12500;
    opts.audio_in_type = AUDIO_IN_RTL;
    model.refresh(&opts, &state);
    expect("radio input: no peer passband", !model.peerPassband() && model.radioInput());
    expect("radio input: an unset width is the default",
           model.nfmBandwidthUnsetText() == QStringLiteral("default") && model.nfmBandwidthUnsetHz() == 16000);

    opts.use_rigctl = 0;
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    freeState(&state);
}

int
main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    test_scan_row();
    test_rigctl_audio_passband();
    test_options_readiness();
    test_decode_clock_readings();
    test_temporary_lockout_metrics();
    test_call_skip_metrics();
    test_scan_timing();
    test_rx_tone();
    test_tone_filter();
    test_tone_filter_editor_setting();
    test_direct_key_presence();
    test_decryption_metadata();
    test_site();
    test_quality();
    test_lead_slot_keeps_earlier_call_and_quality();
    test_lead_slot_change_alone_notifies();
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
    emergency.observed_m = dsd_decode_now_mono_s();
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
    expect("without a trunk scan the buttons edit the gain in force", model.configuredTunerGainDb() == 30);
    expect("without a trunk scan no target overrides the gain", !model.tunerGainRowOverride());
    expect("squelch is reported in dB", std::fabs(model.squelchDb() - (-120.0)) < 0.5);

    /* Issue #518 follow-up: under --trunk-scan the buttons edit the configured gain the coordinator publishes, and a
     * parked target with its own rtl_gain is flagged, from the same app_control view the terminal uses. */
    opts.trunk_scan_enabled = 1;
    state.trunk_scan_target_count = 2U;
    state.trunk_scan_configured_gain = 20;
    state.trunk_scan_gain_override = 1U;
    model.refresh(&opts, &state);
    expect("the target's own gain is the reading", model.tunerGainDb() == 30);
    expect("the configured gain is what the buttons edit", model.configuredTunerGainDb() == 20);
    expect("the target's override is flagged", model.tunerGainRowOverride());
    opts.trunk_scan_enabled = 0;
    state.trunk_scan_target_count = 0U;
    state.trunk_scan_configured_gain = 0;
    state.trunk_scan_gain_override = 0U;
    model.refresh(&opts, &state);

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

    /* Issue #521: a row overriding the squelch. The panel pairs what is in force with the
     * configured default its buttons edit, and says a row is responsible. 0 dB means off. */
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    model.refresh(&opts, &state);
    expect("no scope: effective and configured agree", std::fabs(model.effectiveSquelchDb() - (-80.0)) < 1e-6
                                                           && std::fabs(model.configuredSquelchDb() - (-80.0)) < 1e-6);
    expect("no scope: no row badge", !model.squelchRowOverride());
    expect("no scope: the readout is the terminal's", model.squelchReadout() == QStringLiteral("-80.0 dB"));
    expect("row squelch scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    dsd_scan_option_values squelch_row{};
    squelch_row.present = DSD_SCAN_OPT_SQUELCH;
    squelch_row.squelch_db = -60;
    expect("row squelch installed", dsd_scan_mode_options(&opts, &state, &squelch_row) == 0);
    model.refresh(&opts, &state);
    expect("row squelch is in force", std::fabs(model.effectiveSquelchDb() - (-60.0)) < 1e-6);
    expect("the default stays configured", std::fabs(model.configuredSquelchDb() - (-80.0)) < 1e-6);
    expect("a row override is flagged", model.squelchRowOverride());
    expect("the effective squelch still drives squelchDb", std::fabs(model.squelchDb() - (-60.0)) < 1e-6);
    expect("the override readout is the terminal's",
           model.squelchReadout() == QStringLiteral("-60.0 dB (row; default -80.0 dB)"));
    expect("neither level is off", !model.effectiveSquelchOff() && !model.configuredSquelchOff());
    squelch_row.squelch_db = 0;
    expect("row squelch off installed", dsd_scan_mode_options(&opts, &state, &squelch_row) == 0);
    model.refresh(&opts, &state);
    expect("a row that switches the squelch off reads 0", std::fabs(model.effectiveSquelchDb()) < 1e-9);
    expect("an off row is still an override", model.squelchRowOverride() && model.squelchOff());
    expect("the view decides the row is off", model.effectiveSquelchOff() && !model.configuredSquelchOff());
    expect("the off readout is the terminal's",
           model.squelchReadout() == QStringLiteral("off (row; default -80.0 dB)"));
    /* A legacy linear default at full scale reads 0 dB like off, but gates everything: the
     * panel takes the view's decision, not the sign of the number. */
    dsd_scan_mode_leave(&opts, &state);
    opts.rtl_squelch_level = 1.0;
    expect("full-scale default scope", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
    squelch_row.squelch_db = -60;
    expect("row over a full-scale default", dsd_scan_mode_options(&opts, &state, &squelch_row) == 0);
    model.refresh(&opts, &state);
    expect("a full-scale default reads 0 dB", std::fabs(model.configuredSquelchDb()) < 1e-9);
    expect("a full-scale default is not off", !model.configuredSquelchOff());
    expect("a full-scale default readout", model.squelchReadout() == QStringLiteral("-60.0 dB (row; default 0.0 dB)"));
    /* Audio input takes a squelch too (issue #628): the same readout, flagged for the monitor's Squelch row. */
    expect("a radio input is not audio input", !model.squelchAudioInput());
    opts.audio_in_type = AUDIO_IN_WAV;
    model.refresh(&opts, &state);
    expect("audio input is flagged", model.squelchAudioInput());
    expect("audio input publishes the squelch override", model.squelchRowOverride());
    expect("audio input publishes the readout",
           model.squelchReadout() == QStringLiteral("-60.0 dB (row; default 0.0 dB)"));
    /* A symbol file has no squelch to feed. */
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    model.refresh(&opts, &state);
    expect("a symbol file is not audio input", !model.squelchAudioInput());
    expect("a symbol file publishes no squelch override", !model.squelchRowOverride());
    expect("a symbol file publishes no squelch readout", model.squelchReadout().isEmpty());
    opts.audio_in_type = AUDIO_IN_RTL;
    dsd_scan_mode_leave(&opts, &state);
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    model.refresh(&opts, &state);
    expect("leaving the row clears the badge", !model.squelchRowOverride());
    expect("leaving the row restores the default", std::fabs(model.effectiveSquelchDb() - (-80.0)) < 1e-6);
    opts.rtl_squelch_margin_db = 7;
    model.refresh(&opts, &state);
    expect("a level is not auto",
           !model.configuredSquelchAuto() && !model.effectiveSquelchAuto() && model.squelchAutoStatus().isEmpty());
    expect("a level keeps the margin Auto starts from again", model.configuredSquelchMarginDb() == 7);
    expect("the level beneath is a threshold", !model.configuredSquelchLevelOff());

    /* The auto squelch (issue #518 follow-up): whether each setting is AUTO, its margin and what the one in force
     * shows come from the same view, and the level beneath it stays what dB goes back to. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.rtl_squelch_margin_db = 12;
    const int saved_analog_only = opts.analog_only;
    opts.analog_only = 0;
    model.refresh(&opts, &state);
    expect("an auto default is auto on both sides", model.configuredSquelchAuto() && model.effectiveSquelchAuto());
    expect("its margin on both sides",
           model.configuredSquelchMarginDb() == 12 && model.effectiveSquelchMarginDb() == 12);
    expect("the level beneath it stays readable", std::fabs(model.configuredSquelchDb() - (-80.0)) < 1e-6);
    expect("the level beneath it is not off", !model.configuredSquelchLevelOff());
    opts.rtl_squelch_level = 0.0;
    model.refresh(&opts, &state);
    expect("an off level beneath auto reads off there, not in the setting",
           model.configuredSquelchLevelOff() && !model.configuredSquelchOff());
    opts.rtl_squelch_level = dsd_squelch_level_from_sql(-80.0);
    expect("auto is off on a digital session", model.squelchAutoStatus() == QStringLiteral("off on digital"));
    expect("the readout is the terminal's", model.squelchReadout() == QStringLiteral("auto +12 dB (off on digital)"));
    opts.analog_only = 1;
    state.squelch_auto_active = 1;
    state.squelch_auto_plan_valid = 1;
    state.squelch_auto_state = 1;
    state.squelch_auto_floor_cdb = -7830;
    model.refresh(&opts, &state);
    expect("an analog session shows the floor", model.squelchAutoStatus() == QStringLiteral("floor -78.3 dB"));
    state.squelch_auto_floor_cdb = -7900;
    model.refresh(&opts, &state);
    expect("a moving floor reaches the panel", model.squelchAutoStatus() == QStringLiteral("floor -79.0 dB"));
    /* The noise squelch: its flags, its quieting while it runs, and no Noise offered on the AM monitor on its own. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    state.squelch_noise_active = 1;
    state.squelch_noise_measured = 1;
    state.squelch_noise_quieting_cdb = 2310;
    model.refresh(&opts, &state);
    expect("a noise default is noise on both sides", model.configuredSquelchNoise() && model.effectiveSquelchNoise());
    expect("and not auto", !model.configuredSquelchAuto() && !model.effectiveSquelchAuto());
    expect("the noise squelch shows its quieting", model.squelchAutoStatus() == QStringLiteral("quieting 23 dB"));
    expect("the readout is the terminal's", model.squelchReadout() == QStringLiteral("noise +12 dB (quieting 23 dB)"));
    expect("the FM monitor offers Noise", model.squelchNoiseOffered());
    opts.analog_demod = DSD_ANALOG_DEMOD_AM;
    state.squelch_noise_active = 0;
    state.squelch_noise_measured = 0;
    model.refresh(&opts, &state);
    expect("the AM monitor offers no Noise", !model.squelchNoiseOffered());
    expect("a noise setting on AM runs as auto",
           model.squelchAutoStatus() == QStringLiteral("as auto: floor -79.0 dB"));
    opts.analog_demod = DSD_ANALOG_DEMOD_FM;
    state.squelch_noise_quieting_cdb = 0;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_AUTO;
    opts.audio_in_type = AUDIO_IN_WAV;
    state.squelch_auto_active = 0;
    model.refresh(&opts, &state);
    expect("auto on audio input reads off there",
           model.configuredSquelchAuto() && model.squelchAutoStatus() == QStringLiteral("off: no radio input"));
    /* The PCM noise squelch on audio input (issue #628): learning, then its quieting, or off with no band. */
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_NOISE;
    const int saved_monitor = opts.monitor_input_audio;
    opts.monitor_input_audio = 1;
    state.squelch_noise_active = 1;
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_LEARNING;
    model.refresh(&opts, &state);
    expect("the PCM noise squelch learns", model.squelchAutoStatus() == QStringLiteral("learning"));
    expect("the readout is the terminal's", model.squelchReadout() == QStringLiteral("noise +12 dB (learning)"));
    expect("the PCM FM monitor offers Noise", model.squelchNoiseOffered());
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_KNOWN;
    state.squelch_noise_measured = 1;
    state.squelch_noise_quieting_cdb = 2310;
    model.refresh(&opts, &state);
    expect("then shows its quieting", model.squelchAutoStatus() == QStringLiteral("quieting 23 dB"));
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NO_BAND;
    model.refresh(&opts, &state);
    expect("or why it is off", model.squelchAutoStatus() == QStringLiteral("off: no band above voice"));
    state.squelch_noise_active = 0;
    state.squelch_noise_measured = 0;
    state.squelch_noise_quieting_cdb = 0;
    state.squelch_noise_state = DSD_SQUELCH_NOISE_STATE_NONE;
    opts.monitor_input_audio = saved_monitor;
    opts.audio_in_type = AUDIO_IN_RTL;
    state.squelch_auto_active = 0;
    state.squelch_auto_plan_valid = 0;
    state.squelch_auto_state = 0;
    state.squelch_auto_floor_cdb = 0;
    opts.analog_only = saved_analog_only;
    opts.rtl_squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    opts.rtl_squelch_margin_db = DSD_SQUELCH_MARGIN_DEFAULT_DB;
    model.refresh(&opts, &state);

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

        /* Slot 2 live as well: the earlier open epoch keeps the headline. */
        dsd_call_observation other = named;
        other.slot = 1U;
        other.ota_target_id = 1202U;
        other.observed_m = 2.5;
        (void)dsd_call_state_observe(&state, &other, DSD_CALL_BOUNDARY_BEGIN);
        model.refresh(&opts, &state);
        expect("slot 2 is live too", model.slot2CallState() == 2);
        expect("between two live calls the earlier slot 1 call headlines", model.leadSlot() == 1);

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

    /* Issue #525: the analog channel width the Radio sheet shows and steps from. While a running stream runs the
     * analog monitor it is the width the front end reports (with its DSP-limited flag); otherwise the configured
     * width, the default when none is set. The configured value (0 = default) is published on its own for the stepper,
     * and with a stream running the widest width its DSP rate filters. The front end's width is kept apart from the
     * configured one throughout, so each case shows which of the two the model took. */
    {
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_stream_active = 1;
        g_stub_demod_rate_hz = 24000;
        model.refresh(&opts, &state);
        /* Issue #524: the rate still bounds the widths the controls offer outside the preset: a width set for the next
         * switch is held to it there. */
        expect("digital: no analog width", model.analogBandwidthHz() == 0 && !model.analogBandwidthDspLimited()
                                               && model.analogBandwidthMaxHz() == 20400);
        expect("digital: neither width offered by default",
               !model.nfmBandwidthOffered() && !model.amBandwidthOffered());
        opts.analog_only = 1;
        opts.analog_demod = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("analog before a published width reads the default", model.analogBandwidthHz() == 16000);
        expect("the default is configured as 0", model.analogBandwidthConfiguredHz() == 0);
        expect("the widest width the 24 kHz rate filters", model.analogBandwidthMaxHz() == 20400);
        expect("analog: the NFM width offered, the AM default fits the rate",
               model.nfmBandwidthOffered() && !model.amBandwidthOffered());
        expect("the reading says it is the default",
               model.analogBandwidthReading() == QStringLiteral("16 kHz (default)"));

        opts.analog_nfm_bandwidth_hz = 12500;
        g_stub_channel_bandwidth_hz = 10800;
        model.refresh(&opts, &state);
        expect("on the monitor, the front end's width", model.analogBandwidthHz() == 10800);
        expect("not DSP-limited", !model.analogBandwidthDspLimited());
        expect("the configured width", model.analogBandwidthConfiguredHz() == 12500);
        expect("the reading is the front end's width", model.analogBandwidthReading() == QStringLiteral("10.8 kHz"));

        /* The flag must move on its own: everything else holds still here. */
        g_stub_channel_bandwidth_dsp_limited = 1;
        model.refresh(&opts, &state);
        expect("DSP-limited alone moves the tuner group", model.analogBandwidthDspLimited());
        expect("the reading says DSP-limited",
               model.analogBandwidthReading() == QStringLiteral("10.8 kHz (DSP-limited)"));
        g_stub_channel_bandwidth_dsp_limited = 0;

        /* The front end's mirror outlives its stream: once the stream stops, what it still says is the last
         * session's width, not this one's. */
        g_stub_stream_active = 0;
        g_stub_channel_bandwidth_dsp_limited = 1;
        model.refresh(&opts, &state);
        expect("no stream: the configured width", model.analogBandwidthHz() == 12500);
        expect("no stream: not DSP-limited", !model.analogBandwidthDspLimited());
        /* The input is an RTL one ("pulse" on an RTL input opens as an RTL-SDR): the next start runs at its 48 kHz DSP
         * bandwidth, which bounds the steps. */
        expect("no stream: the DSP bandwidth bounds the steps", model.analogBandwidthMaxHz() == 42000);
        /* At a 12 kHz DSP bandwidth the default runs no channel filter: the sheet reads what the next start publishes
         * and offers only the widths that rate filters. */
        opts.analog_nfm_bandwidth_hz = 0;
        opts.rtl_dsp_bw_khz = 12;
        model.refresh(&opts, &state);
        expect("no stream at 12 kHz: the default reads as the rate",
               model.analogBandwidthHz() == 12000 && model.analogBandwidthDspLimited());
        expect("no stream at 12 kHz: the steps it filters", model.analogBandwidthMaxHz() == 9600);
        expect("no stream at 12 kHz: the reading says DSP-limited",
               model.analogBandwidthReading() == QStringLiteral("12 kHz (DSP-limited)"));
        opts.rtl_dsp_bw_khz = 48;
        opts.analog_nfm_bandwidth_hz = 12500;
        g_stub_stream_active = 1;
        g_stub_channel_bandwidth_dsp_limited = 0;

        /* A typed digital row on the analog session filters with its own profile: the front end's width is the row's
         * channel, so the sheet shows the configured analog width the row's leave returns to. */
        expect("typed row on analog", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_DMR) == 0);
        expect("typed row options", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
        model.refresh(&opts, &state);
        expect("under a typed row the configured width", model.analogBandwidthHz() == 12500);
        expect("under a typed row the configured preset still shows", model.analogBandwidthConfiguredHz() == 12500);
        dsd_scan_mode_leave(&opts, &state);

        /* Issue #526: an nfm row with its own width runs over the configured one. The reading is the row's width with
         * the configured width it returns to, and the stepper still edits the configured width, not the row's. */
        dsd_scan_option_values width_row{};
        width_row.present = DSD_SCAN_OPT_BANDWIDTH;
        width_row.channel_bw_hz = 16000;
        expect("nfm width row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
        expect("nfm width row options", dsd_scan_mode_options(&opts, &state, &width_row) == 0);
        expect("nfm width row in force", opts.analog_nfm_bandwidth_hz == 16000);
        g_stub_channel_bandwidth_hz = 16000;
        model.refresh(&opts, &state);
        expect("under a width row the row's width", model.analogBandwidthHz() == 16000);
        expect("under a width row the stepper edits the configured width",
               model.analogBandwidthConfiguredHz() == 12500);
        expect("under a width row the reading names both",
               model.analogBandwidthReading() == QStringLiteral("16 kHz (row; default 12.5 kHz)"));
        expect("under a width row the row is on air", model.analogBandwidthRowActive());
        expect("under a width row the override is flagged", model.analogBandwidthRowOverride());
        dsd_scan_mode_leave(&opts, &state);
        expect("the width row's leave restores the configured width", opts.analog_nfm_bandwidth_hz == 12500);

        /* ...and on a digital session, where the nfm row is the only analog receiver: the width it runs is in force and
         * read as the terminal reads it, flagged for the sheet as a row on air, and badged while the row sets its own
         * width; nothing once it leaves. */
        opts.analog_only = 0;
        opts.frame_dmr = 1;
        model.refresh(&opts, &state);
        expect("digital session: no row, no width", !model.analogBandwidthRowActive() && model.analogBandwidthHz() == 0
                                                        && model.analogBandwidthReading().isEmpty());
        expect("digital nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
        expect("digital nfm row without a width", dsd_scan_mode_options(&opts, &state, nullptr) == 0);
        g_stub_channel_bandwidth_hz = 12500;
        model.refresh(&opts, &state);
        expect("digital nfm row: on air, no override",
               model.analogBandwidthRowActive() && !model.analogBandwidthRowOverride());
        expect("digital nfm row: the configured width in force", model.analogBandwidthHz() == 12500);
        expect("digital nfm row: read as the terminal reads it",
               model.analogBandwidthReading() == QStringLiteral("12.5 kHz"));
        expect("digital nfm row: the stepper edits the configured width", model.analogBandwidthConfiguredHz() == 12500);
        expect("digital nfm row with a width", dsd_scan_mode_options(&opts, &state, &width_row) == 0);
        g_stub_channel_bandwidth_hz = 16000;
        model.refresh(&opts, &state);
        expect("digital width row: the override is flagged", model.analogBandwidthRowOverride());
        expect("digital width row: the row's width", model.analogBandwidthHz() == 16000);
        expect("digital width row: the reading names both",
               model.analogBandwidthReading() == QStringLiteral("16 kHz (row; default 12.5 kHz)"));
        expect("digital width row: the stepper edits the configured width",
               model.analogBandwidthConfiguredHz() == 12500);
        dsd_scan_mode_leave(&opts, &state);
        model.refresh(&opts, &state);
        expect("digital session after the row: no row, no width",
               !model.analogBandwidthRowActive() && !model.analogBandwidthRowOverride()
                   && model.analogBandwidthHz() == 0 && model.analogBandwidthReading().isEmpty());
        opts.frame_dmr = 0;
        opts.analog_only = 1;
        g_stub_channel_bandwidth_hz = 10800;

        /* PCM input: no channel filter runs, so no width is in force (the terminal shows no Analog field there
         * either); the configured width is still published for the control. */
        opts.audio_in_type = AUDIO_IN_PULSE;
        opts.analog_nfm_bandwidth_hz = 20000;
        model.refresh(&opts, &state);
        expect("PCM input has no width in force", model.analogBandwidthHz() == 0 && !model.analogBandwidthDspLimited()
                                                      && model.analogBandwidthMaxHz() == 0);
        expect("PCM input says the width does not apply",
               model.analogBandwidthReading() == QStringLiteral("not used on PCM input"));
        expect("the configured width moves the control group", model.analogBandwidthConfiguredHz() == 20000);

        opts.analog_only = 0;
        opts.analog_nfm_bandwidth_hz = 0;
        g_stub_channel_bandwidth_hz = 0;
        g_stub_stream_active = 0;
        g_stub_demod_rate_hz = 0;
        opts.audio_in_type = AUDIO_IN_RTL;
    }

    /* Issue #524: under the AM preset the view reads the AM width: the configured one, 6 kHz by default and never
     * DSP-limited (a rate that cannot filter it is refused), and the running monitor's own width once the front end
     * publishes an AM one. An FM width still published (a switch to AM not landed yet) is another channel's. The
     * configured AM width is what the stepper edits, and stays published off a radio. */
    {
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_stream_active = 1;
        g_stub_demod_rate_hz = 16000;
        opts.analog_only = 1;
        opts.monitor_input_audio = 1;
        opts.analog_demod = DSD_ANALOG_DEMOD_AM;
        model.refresh(&opts, &state);
        expect("am: the default width", model.analogBandwidthHz() == 6000 && !model.analogBandwidthDspLimited());
        expect("am: the default is configured as 0", model.analogBandwidthConfiguredHz() == 0);
        expect("am: the 16 kHz rate bounds the steps at 13.2 kHz", model.analogBandwidthMaxHz() == 13200);
        expect("am: the reading says it is the default",
               model.analogBandwidthReading() == QStringLiteral("6 kHz (default)"));

        opts.analog_am_bandwidth_hz = 10000;
        g_stub_channel_bandwidth_hz = 16000;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("am: an FM width is not the AM channel", model.analogBandwidthHz() == 10000);
        expect("am: the configured AM width", model.analogBandwidthConfiguredHz() == 10000);
        g_stub_channel_bandwidth_hz = 9000;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_AM;
        model.refresh(&opts, &state);
        expect("am: the AM monitor's width", model.analogBandwidthHz() == 9000);
        expect("am: the reading is the AM monitor's width", model.analogBandwidthReading() == QStringLiteral("9 kHz"));
        expect("am: the width is AM's", model.analogBandwidthAm());

        /* Issue #526: an nfm row with its own width over the AM session runs FM. The readings describe the row's NFM
         * width, the configured widths are the configured view's rather than the row's in dsd_opts, and the AM
         * preset's own width stays offered; the leave brings AM back. */
        opts.analog_nfm_bandwidth_hz = 11250;
        dsd_scan_option_values am_session_row{};
        am_session_row.present = DSD_SCAN_OPT_BANDWIDTH;
        am_session_row.channel_bw_hz = 12500;
        expect("am: nfm row", dsd_scan_mode_enter(&opts, &state, DSD_SCAN_MODE_NFM) == 0);
        expect("am: nfm row options", dsd_scan_mode_options(&opts, &state, &am_session_row) == 0);
        g_stub_channel_bandwidth_hz = 12500;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("am nfm row: the row's NFM width", !model.analogBandwidthAm() && model.analogBandwidthHz() == 12500);
        expect("am nfm row: the configured NFM width",
               model.nfmBandwidthConfiguredHz() == 11250 && model.analogBandwidthConfiguredHz() == 11250);
        expect("am nfm row: the configured AM width", model.amBandwidthConfiguredHz() == 10000);
        expect("am nfm row: both widths offered", model.amBandwidthOffered() && model.nfmBandwidthOffered());
        dsd_scan_mode_leave(&opts, &state);
        opts.analog_nfm_bandwidth_hz = 0;
        g_stub_channel_bandwidth_hz = 9000;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_AM;
        model.refresh(&opts, &state);
        expect("am after the nfm row: AM again", model.analogBandwidthAm() && model.analogBandwidthHz() == 9000);

        opts.audio_in_type = AUDIO_IN_PULSE;
        model.refresh(&opts, &state);
        expect("am: no width in force off a radio",
               model.analogBandwidthHz() == 0 && model.analogBandwidthMaxHz() == 0);
        expect("am: the configured AM width stays off a radio", model.analogBandwidthConfiguredHz() == 10000);

        /* Each kind's configured width is published whichever preset runs, for the control of the kind the preset
         * does not run: an explicit NFM width under AM, an explicit AM width under a digital mode. */
        opts.audio_in_type = AUDIO_IN_RTL;
        opts.analog_nfm_bandwidth_hz = 25000;
        model.refresh(&opts, &state);
        expect("am: the configured NFM width under AM", model.nfmBandwidthConfiguredHz() == 25000);
        expect("am: the configured AM width under AM", model.amBandwidthConfiguredHz() == 10000);
        opts.analog_only = 0;
        opts.monitor_input_audio = 0;
        opts.analog_demod = DSD_ANALOG_DEMOD_FM;
        model.refresh(&opts, &state);
        expect("digital: the configured AM width", model.amBandwidthConfiguredHz() == 10000);
        expect("digital: the configured NFM width", model.nfmBandwidthConfiguredHz() == 25000);
        expect("digital: the section's width is the NFM one", model.analogBandwidthConfiguredHz() == 25000);
        expect("digital: both explicit widths offered", model.nfmBandwidthOffered() && model.amBandwidthOffered());
        opts.analog_nfm_bandwidth_hz = 0;

        /* The unset AM default is offered where the running stream's rate cannot filter it but filters a narrower AM
         * width (a replay or device at 7.5 kHz filters up to 5.55 kHz): a switch to AM is refused there with word to
         * narrow the width, which has to be possible before it. */
        opts.analog_am_bandwidth_hz = 0;
        g_stub_demod_rate_hz = 7500;
        model.refresh(&opts, &state);
        expect("digital at 7.5 kHz: the AM default is offered", model.amBandwidthOffered());
        expect("digital at 7.5 kHz: the rate bounds the steps", model.analogBandwidthMaxHz() == 5550);
        expect("digital at 7.5 kHz: the NFM default is not", !model.nfmBandwidthOffered());
        opts.audio_in_type = AUDIO_IN_PULSE;
        model.refresh(&opts, &state);
        expect("pcm: nothing offered", !model.amBandwidthOffered() && model.analogBandwidthMaxHz() == 0);
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_demod_rate_hz = 16000;

        opts.analog_only = 0;
        opts.monitor_input_audio = 0;
        opts.analog_demod = DSD_ANALOG_DEMOD_FM;
        opts.analog_am_bandwidth_hz = 0;
        opts.audio_in_type = AUDIO_IN_RTL;
        g_stub_channel_bandwidth_hz = 0;
        g_stub_channel_analog_kind = DSD_ANALOG_DEMOD_FM;
        g_stub_stream_active = 0;
        g_stub_demod_rate_hz = 0;
        model.refresh(&opts, &state);
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
