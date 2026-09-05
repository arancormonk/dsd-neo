// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/scan_mode.h>
#include <dsd-neo/runtime/scan_options.h>
#include <stdlib.h>
#include <string.h>

static void
test_p25_modulation_helpers(dsd_opts* opts, dsd_state* state) {
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_P25P2, DSD_DECODE_PRESET_PROFILE_CLI, opts, state) == 0);
    opts->mod_cli_lock = 1;
    opts->mod_p25p2_profile_lock = 1;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    assert(opts->mod_p25p2_profile_lock && state->samplesPerSymbol == 8);
    assert(dsd_scan_mode_effective_profile(opts, state).symbol_rate_hz == 6000);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_DMR) == 0);
    assert(!opts->mod_p25p2_profile_lock && state->samplesPerSymbol == 10);
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    assert(opts->mod_p25p2_profile_lock && state->samplesPerSymbol == 8);
    dsd_scan_mode_leave(opts, state);
    opts->mod_p25p2_profile_lock = 0;
    opts->mod_p25p2_c4fm = 1;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 0;
    state->rf_mod = 0;
    state->samplesPerSymbol = 10;
    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    assert(dsd_scan_mode_enter(opts, state, DSD_SCAN_MODE_P25) == 0);
    assert(opts->mod_p25p2_c4fm && state->samplesPerSymbol == 10 && state->rf_mod == 0);
    /* An explicit trunk-target modulation still takes precedence over the helper. */
    dsd_scan_mode_target_modulation(state, DSD_SCAN_MODULATION_AUTO);
    assert(dsd_scan_mode_suspend(opts, state));
    (void)dsd_scan_mode_resume(opts, state);
    assert(!opts->mod_p25p2_c4fm);
    dsd_scan_mode_leave(opts, state);
    assert(opts->mod_p25p2_c4fm && opts->mod_cli_lock);
}

/* Row-scoped options are policy, not acquisition. An operator edit to any of them beneath
 * a parked row must take effect without tearing the row down, and the equality that gates
 * restaging a tune must ignore them. */
static void
test_row_option_edits_are_not_acquisition_changes(void) {
    dsd_opts* o = (dsd_opts*)calloc(1, sizeof(*o));
    dsd_state* s = (dsd_state*)calloc(1, sizeof(*s));
    assert(o && s);
    o->wav_sample_rate = 48000;
    o->audio_in_type = AUDIO_IN_WAV;
    o->frame_dmr = 1;
    o->dmr_mute_encL = o->dmr_mute_encR = 1;
    o->aggressive_framesync = 1;
    o->scan_voice_hold_ms = 2000;
    DSD_SNPRINTF(o->group_in_file, sizeof(o->group_in_file), "%s", "global.csv");
    s->M = 0;
    const dsd_scan_option_values none = {0};
    assert(dsd_scan_mode_options(o, s, &none) == -1); /* no scope yet: nothing to apply */
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_options(o, s, NULL) == 0);
    dsd_scan_settings before;
    dsd_scan_settings_capture(o, s, &before);

    assert(dsd_scan_mode_suspend(o, s));
    o->dmr_mute_encL = o->dmr_mute_encR = 0;
    o->aggressive_framesync = 0;
    o->dmr_crc_relaxed_default = 1;
    o->scan_voice_only = 1;
    o->scan_voice_hold_ms = 4000;
    o->unmute_encrypted_p25 = 1;
    s->M = 1;
    DSD_SNPRINTF(o->group_in_file, sizeof(o->group_in_file), "%s", "edited.csv");
    assert(dsd_scan_mode_resume(o, s) == 0);
    assert(o->dmr_mute_encL == 0 && o->dmr_mute_encR == 0 && o->aggressive_framesync == 0
           && o->dmr_crc_relaxed_default == 1 && o->scan_voice_only == 1 && o->scan_voice_hold_ms == 4000
           && o->unmute_encrypted_p25 == 1 && s->M == 1);
    assert(strcmp(o->group_in_file, "edited.csv") == 0);
    dsd_scan_settings after;
    dsd_scan_settings_capture(o, s, &after);
    assert(dsd_scan_settings_equal(&before, &after, 1));
    assert(after.dmr_mute_encL == 0 && after.force_key == 1 && after.scan_voice_hold_ms == 4000);

    /* A row override layered over the edited baseline still yields to a decoder change. */
    dsd_scan_option_values row = {0};
    row.present = DSD_SCAN_OPT_HOLD | DSD_SCAN_OPT_MUTE_DMR | DSD_SCAN_OPT_FORCE;
    row.hold_ms = 3000;
    row.mute_dmr = 1;
    row.force = 0x21;
    assert(dsd_scan_mode_options(o, s, &row) == 0);
    assert(o->scan_voice_hold_ms == 3000 && o->dmr_mute_encL == 1 && s->M == 0x21);
    assert(dsd_scan_mode_suspend(o, s));
    assert(o->scan_voice_hold_ms == 4000 && o->dmr_mute_encL == 0 && s->M == 1);
    o->scan_voice_hold_ms = 5000;
    assert(dsd_scan_mode_resume(o, s) == 0);
    assert(o->scan_voice_hold_ms == 3000 && o->dmr_mute_encL == 1 && s->M == 0x21);
    assert(dsd_scan_mode_suspend(o, s));
    o->inverted_dmr = !o->inverted_dmr;
    assert(dsd_scan_mode_resume(o, s) == 1);
    dsd_scan_mode_leave(o, s);
    assert(o->scan_voice_hold_ms == 5000 && o->dmr_mute_encL == 0 && s->M == 1);
    dsd_state_ext_free_all(s);
    free(s);
    free(o);
}

int
main(void) {
    test_row_option_edits_are_not_acquisition_changes();
    dsd_opts* o = (dsd_opts*)calloc(1, sizeof(*o));
    dsd_state* s = (dsd_state*)calloc(1, sizeof(*s));
    dsd_state* copy = (dsd_state*)calloc(1, sizeof(*copy));
    assert(o && s && copy);
    o->pulse_digi_out_channels = 1;
    o->pulse_digi_rate_out = 44100;
    o->wav_sample_rate = 48000;
    o->audio_in_type = AUDIO_IN_WAV;
    o->frame_dstar = 2;
    o->frame_p25p1 = 1;
    o->mod_gfsk = 1;
    o->mod_cli_lock = 1;
    o->inverted_p2 = 3;
    o->use_cosine_filter = 1;
    o->ssize = 47;
    o->msize = 12;
    s->rf_mod = 2;
    s->samplesPerSymbol = 13;
    s->symbolCenter = 5;
    dsd_scan_settings baseline;
    dsd_scan_settings restored;
    dsd_scan_settings_capture(o, s, &baseline);
    const int rates[] = {4800, 4800, 4800, 4800, 2400, 2400, 4800, 4800, 4800};
    for (int m = DSD_SCAN_MODE_P25; m <= DSD_SCAN_MODE_M17; m++) {
        dsd_scan_mode parsed;
        assert(dsd_scan_mode_parse(dsd_scan_mode_name((dsd_scan_mode)m), &parsed) == 0 && parsed == (dsd_scan_mode)m);
        dsd_scan_settings prepared;
        assert(dsd_scan_mode_prepare(o, s, parsed, &prepared) == 0);
        assert(dsd_scan_mode_active(s) == DSD_SCAN_MODE_INHERIT);
        assert(dsd_scan_mode_enter(o, s, parsed) == 0);
        assert(dsd_scan_mode_active(s) == parsed);
        assert(dsd_scan_mode_effective_profile(o, s).symbol_rate_hz == rates[m]);
        assert(dsd_scan_mode_effective_profile(o, s).levels == (m == DSD_SCAN_MODE_DSTAR ? 2 : 4));
        assert(s->samplesPerSymbol == 48000 / rates[m]);
        assert(s->rf_mod == 2 && o->mod_gfsk == 1 && o->mod_cli_lock == 1);
        assert(o->pulse_digi_out_channels == 1 && o->pulse_digi_rate_out == 44100);
        assert(o->frame_x2tdma == 0 && o->frame_provoice == 0);
        assert(o->frame_p25p1 == (m == DSD_SCAN_MODE_P25));
        assert(o->frame_p25p2 == (m == DSD_SCAN_MODE_P25));
        assert(o->frame_dmr == (m == DSD_SCAN_MODE_DMR));
        assert(o->frame_nxdn48 == (m == DSD_SCAN_MODE_NXDN48));
        assert(o->frame_nxdn96 == (m == DSD_SCAN_MODE_NXDN96));
        assert(o->frame_dpmr == (m == DSD_SCAN_MODE_DPMR));
        assert(o->frame_dstar == (m == DSD_SCAN_MODE_DSTAR));
        assert(o->frame_ysf == (m == DSD_SCAN_MODE_YSF));
        assert(o->frame_m17 == (m == DSD_SCAN_MODE_M17));
        if (m == DSD_SCAN_MODE_DMR) {
            assert(o->dmr_stereo == 1 && o->dmr_mono == 0);
        }
        if (m == DSD_SCAN_MODE_M17) {
            assert(o->use_cosine_filter == 0);
        }
        dsd_scan_mode_configured(o, s, &restored);
        assert(memcmp(&baseline, &restored, sizeof(baseline)) == 0);
        dsd_scan_mode_copy_snapshot(copy, s);
        assert(copy->state_ext[6] != s->state_ext[6]);
        dsd_scan_mode_leave(o, s);
        dsd_scan_settings_capture(o, s, &restored);
        assert(memcmp(&baseline, &restored, sizeof(baseline)) == 0);
        assert(dsd_scan_mode_active(copy) == parsed);
    }
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_M17) == 0);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_NXDN96) == 0);
    assert(o->use_cosine_filter == 1 && s->samplesPerSymbol == 10);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_P25) == 0);
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_6000_4;
    assert(dsd_scan_mode_effective_profile(o, s).symbol_rate_hz == 6000);
    assert(dsd_scan_mode_suspend(o, s));
    assert(o->frame_dstar == 2 && o->ssize == 47);
    assert(dsd_scan_mode_resume(o, s) == 0);
    assert(s->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4);
    assert(dsd_scan_mode_suspend(o, s));
    o->mod_gfsk = 0;
    o->mod_qpsk = 1;
    assert(dsd_scan_mode_resume(o, s) == 1);
    assert(s->sps_hunt_idx == DSD_FRAME_SYNC_SPS_PROFILE_6000_4 && s->samplesPerSymbol == 8);
    assert(s->rf_mod == 1);
    assert(dsd_scan_mode_suspend(o, s));
    o->frame_dstar = 0;
    o->frame_ysf = 1;
    (void)dsd_scan_mode_resume(o, s);
    assert(o->frame_p25p1 && !o->frame_ysf);
    dsd_scan_mode_leave(o, s);
    assert(o->frame_ysf && !o->frame_dstar);
    /* Presets overwrite only the visible label. A different suffix after its NUL
     * is not an effective setting change and must not interrupt a parked row. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_NXDN96, DSD_DECODE_PRESET_PROFILE_CLI, o, s) == 0);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_P25) == 0);
    assert(dsd_scan_mode_suspend(o, s));
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_DSTAR, DSD_DECODE_PRESET_PROFILE_CLI, o, s) == 0);
    assert(dsd_scan_mode_resume(o, s) == 0);
    dsd_scan_mode_leave(o, s);
    assert(o->frame_dstar && !o->frame_p25p1);
    dsd_scan_mode parsed;
    assert(dsd_scan_mode_parse("  NxDn48 ", &parsed) == 0 && parsed == DSD_SCAN_MODE_NXDN48);
    assert(dsd_scan_mode_parse("   ", &parsed) == 0 && parsed == DSD_SCAN_MODE_INHERIT);
    assert(dsd_scan_mode_parse("NXDN", &parsed) == -1);
    assert(dsd_scan_mode_parse("analog", &parsed) == -1);
    assert(dsd_scan_mode_parse("p25p1", &parsed) == -1);
    /* AUTO may be captured on any hunt profile. Leaving a typed row must
     * publish the restored clock and slicer levels, not AUTO's starting preset. */
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, o, s) == 0);
    const int profile_rates[] = {4800, 2400, 9600, 6000, 4800};
    const int profile_levels[] = {4, 4, 2, 4, 2};
    for (int i = 0; i < DSD_FRAME_SYNC_SPS_PROFILE_COUNT; i++) {
        s->sps_hunt_idx = i;
        s->samplesPerSymbol = 48000 / profile_rates[i];
        s->symbolCenter = dsd_opts_symbol_center(s->samplesPerSymbol);
        assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_DMR) == 0);
        dsd_scan_mode_leave(o, s);
        const dsd_decode_mode_profile profile = dsd_scan_mode_effective_profile(o, s);
        assert(profile.symbol_rate_hz == profile_rates[i]);
        assert(profile.levels == profile_levels[i]);
        assert((int)profile.sps_profile_index == i);
        assert(s->samplesPerSymbol == 48000 / profile.symbol_rate_hz);
    }
    test_p25_modulation_helpers(o, s);
    assert(dsd_apply_decode_mode_preset(DSDCFG_MODE_AUTO, DSD_DECODE_PRESET_PROFILE_CLI, o, s) == 0);
    dsd_scan_settings_capture(o, s, &baseline);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_DMR) == 0);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_INHERIT) == 0);
    /* AUTO acquisition on a blank row may move the live clock and modulation. */
    s->samplesPerSymbol = 20;
    s->symbolCenter = 9;
    s->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_2400_4;
    s->rf_mod = 2;
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_P25) == 0);
    dsd_scan_mode_leave(o, s);
    dsd_scan_settings_capture(o, s, &restored);
    assert(dsd_scan_settings_equal(&baseline, &restored, 1));
    /* Persistence must retain custom decoder combinations and the configured
     * modulation/slot policy without allocating a whole options copy. */
    o->frame_x2tdma = 0;
    o->mod_cli_lock = 1;
    o->mod_c4fm = 0;
    o->mod_qpsk = 1;
    o->mod_gfsk = 0;
    o->dmr_mono = 1;
    assert(dsd_infer_decode_mode_preset_exact(o) == DSDCFG_MODE_UNSET);
    assert(dsd_scan_mode_enter(o, s, DSD_SCAN_MODE_DMR) == 0);
    dsdneoUserConfig config;
    dsd_snapshot_opts_to_user_config(o, s, &config);
    assert(config.decode_mode == DSDCFG_MODE_UNSET && config.dmr_mono == 1);
    assert(config.has_demod && config.demod_path == DSDCFG_DEMOD_QPSK);
    dsd_scan_mode_leave(o, s);
    dsd_state_ext_free_all(copy);
    dsd_state_ext_free_all(s);
    free(copy);
    free(s);
    free(o);
    return 0;
}
