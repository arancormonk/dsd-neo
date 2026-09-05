// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI menu label helpers.
 *
 * Every label follows one grammar: `Noun [State]` for toggles (On/Off),
 * `Noun... [current]` for rows that open a prompt, `Prefix: value` for
 * read-only status rows.
 */

#include <dsd-neo/app_control/history.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/radioreference.h>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "menu_labels.h"

static dsdneoRuntimeConfig g_cfg;
static int g_cfg_valid = 1;
static int g_stat_path_rc = -1;
static int g_tcp_valid = 1;
static int g_rr_available = 1;
static char g_rr_builtin_key[64];
static int g_env_int_value;
static int g_env_int_has_value;
static double g_env_double_value;
static int g_env_double_has_value;
static dsdneoUserDecodeMode g_infer_mode = DSDCFG_MODE_AUTO;
static int g_history_mode = 1;

const dsdneoRuntimeConfig*
dsd_neo_get_config(void) {
    return g_cfg_valid ? &g_cfg : NULL;
}

int
env_get_int(const char* name, int defv) {
    (void)name;
    return g_env_int_has_value ? g_env_int_value : defv;
}

double
env_get_double(const char* name, double defv) {
    (void)name;
    return g_env_double_has_value ? g_env_double_value : defv;
}

int
tcp_input_is_valid(const tcp_input_ctx* ctx) {
    return (ctx != NULL && g_tcp_valid) ? 1 : 0;
}

int
dsd_stat_path(const char* path, dsd_stat_t* st) {
    (void)path;
    if (st) {
        DSD_MEMSET(st, 0, sizeof(*st));
    }
    return g_stat_path_rc;
}

int
dsd_rr_available(void) {
    return g_rr_available;
}

/* menu_labels.c reaches dsd_user_imports_dir() from rr_imports_available(); this
 * target links no libraries at all, so the runtime definition is stubbed here. */
static const char* g_imports_dir = "/tmp/dsd-neo-imports";

const char*
dsd_user_imports_dir(void) {
    return g_imports_dir;
}

const char*
dsd_rr_builtin_app_key(void) {
    return g_rr_builtin_key;
}

/* The decoder-mode label reads the live preset back through the runtime; both
 * halves are stubbed so the test pins the label's shape, not the inference. */
dsdneoUserDecodeMode
dsd_infer_decode_mode_preset(const dsd_opts* opts) {
    (void)opts;
    return g_infer_mode;
}

const char*
dsd_decode_mode_display_name(dsdneoUserDecodeMode mode) {
    switch (mode) {
        case DSDCFG_MODE_AUTO: return "Auto";
        case DSDCFG_MODE_DMR: return "DMR";
        default: return "Other";
    }
}

int
dsd_app_frontend_history_get_mode(void) {
    return g_history_mode;
}

static int
expect_int(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_str(const char* tag, const char* got, const char* want) {
    if (strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "%s: got '%s' want '%s'\n", tag, got, want);
        return 1;
    }
    return 0;
}

static void
reset_fixture(dsd_opts* opts, dsd_state* state, UiCtx* ctx) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));
    DSD_MEMSET(&g_cfg, 0, sizeof(g_cfg));
    g_cfg_valid = 1;
    g_stat_path_rc = -1;
    g_tcp_valid = 1;
    g_rr_available = 1;
    g_rr_builtin_key[0] = '\0';
    g_env_int_value = 0;
    g_env_int_has_value = 0;
    g_env_double_value = 0.0;
    g_env_double_has_value = 0;
    g_infer_mode = DSDCFG_MODE_AUTO;
    g_history_mode = 1;
    ctx->opts = opts;
    ctx->state = state;
}

static int
test_predicates(void) {
    int rc = 0;
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_int("rtl active null ctx", io_rtl_active(NULL), 0);
    opts.audio_in_type = AUDIO_IN_RTL;
    rc |= expect_int("rtl active input", io_rtl_active(&ctx), 1);

    rc |= expect_int("trunk predicate null ctx", trunk_enabled(NULL), 0);
    rc |= expect_int("trunk predicate off", trunk_enabled(&ctx), 0);
    opts.trunk_enable = 1;
    rc |= expect_int("trunk predicate trunking", trunk_enabled(&ctx), 1);
    opts.trunk_enable = 0;
    opts.scanner_mode = 1;
    rc |= expect_int("trunk predicate scanner", trunk_enabled(&ctx), 1);
    /* A conventional trunk-scan target parks with trunk_enable off, yet Next channel still
       has somewhere to go: the next target. */
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    rc |= expect_int("trunk predicate trunk scan", trunk_enabled(&ctx), 1);
    opts.trunk_scan_enabled = 0;

    /* Hold and avoid rows need a rotation to act on: -Y or --trunk-scan, not plain -T. */
    rc |= expect_int("scan rotation null ctx", scan_rotation_active(NULL), 0);
    rc |= expect_int("scan rotation off", scan_rotation_active(&ctx), 0);
    opts.trunk_enable = 1;
    rc |= expect_int("scan rotation plain trunking", scan_rotation_active(&ctx), 0);
    opts.trunk_enable = 0;
    opts.scanner_mode = 1;
    rc |= expect_int("scan rotation scanner", scan_rotation_active(&ctx), 1);
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    rc |= expect_int("scan rotation trunk scan", scan_rotation_active(&ctx), 1);
    opts.trunk_scan_enabled = 0;

    rc |= expect_int("provoice predicate null ctx", provoice_active(NULL), 0);
    rc |= expect_int("provoice predicate off", provoice_active(&ctx), 0);
    opts.frame_provoice = 1;
    rc |= expect_int("provoice predicate on", provoice_active(&ctx), 1);

    return rc;
}

static int
test_decoder_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_str("decode mode auto", lbl_decode_mode(&ctx, b, sizeof(b)), "Mode... [Auto]");
    g_infer_mode = DSDCFG_MODE_DMR;
    rc |= expect_str("decode mode dmr", lbl_decode_mode(&ctx, b, sizeof(b)), "Mode... [DMR]");
    rc |= expect_str("decode mode null ctx is auto", lbl_decode_mode(NULL, b, sizeof(b)), "Mode... [Auto]");

    state.rf_mod = 1;
    rc |= expect_str("modulation from live state", lbl_modulation(&ctx, b, sizeof(b)), "Modulation [QPSK]");
    state.rf_mod = 2;
    rc |= expect_str("modulation gfsk", lbl_modulation(&ctx, b, sizeof(b)), "Modulation [GFSK]");
    state.rf_mod = 7;
    opts.mod_c4fm = 1;
    rc |= expect_str("modulation falls back to opts", lbl_modulation(&ctx, b, sizeof(b)), "Modulation [C4FM]");

    /* The lock reads which modulation is in force, not opts.mod_p25p2_c4fm: the toggle
       behind this row clears that flag on every press and says C4FM through rf_mod. */
    rc |= expect_str("p2 lock off", lbl_p25p2_mod_lock(&ctx, b, sizeof(b)), "P25 Phase 2 modulation lock [Off]");
    opts.mod_p25p2_profile_lock = 1;
    state.rf_mod = 1;
    rc |= expect_str("p2 lock qpsk", lbl_p25p2_mod_lock(&ctx, b, sizeof(b)), "P25 Phase 2 modulation lock [QPSK]");
    state.rf_mod = 0;
    rc |= expect_str("p2 lock c4fm after second press", lbl_p25p2_mod_lock(&ctx, b, sizeof(b)),
                     "P25 Phase 2 modulation lock [C4FM]");
    /* The CLI's -f3 spelling sets the flag without the profile lock; still a lock. */
    opts.mod_p25p2_profile_lock = 0;
    opts.mod_p25p2_c4fm = 1;
    state.rf_mod = 1;
    rc |= expect_str("p2 lock c4fm from cli flag", lbl_p25p2_mod_lock(&ctx, b, sizeof(b)),
                     "P25 Phase 2 modulation lock [C4FM]");
    opts.mod_p25p2_c4fm = 0;
    state.rf_mod = 7;

    opts.use_lpf = 1;
    opts.use_hpf = 0;
    opts.use_pbf = 1;
    opts.use_hpf_d = 0;
    rc |= expect_str("lpf on", lbl_lpf(&ctx, b, sizeof(b)), "Low-pass filter [On]");
    rc |= expect_str("hpf off", lbl_hpf(&ctx, b, sizeof(b)), "High-pass filter [Off]");
    rc |= expect_str("pbf on", lbl_pbf(&ctx, b, sizeof(b)), "Pulse-shaping band-pass [On]");
    rc |= expect_str("hpf digital off", lbl_hpf_d(&ctx, b, sizeof(b)), "Digital high-pass filter [Off]");
    /* The M17 row can disable the effective filter while the configured value
     * shown by this toggle remains enabled. */
    opts.use_cosine_filter = 0;
    ((dsd_opts*)dsd_app_get_latest_opts_snapshot())->use_cosine_filter = 1;
    rc |= expect_str("cosine on", lbl_cosine(&ctx, b, sizeof(b)), "Cosine filter [On]");

    opts.aggressive_framesync = 0;
    rc |= expect_str("crc relaxed on", lbl_crc_relax(&ctx, b, sizeof(b)), "Relaxed CRC checks [On]");
    opts.aggressive_framesync = 1;
    rc |= expect_str("crc relaxed off", lbl_crc_relax(&ctx, b, sizeof(b)), "Relaxed CRC checks [Off]");

    opts.dmr_le = 1;
    rc |= expect_str("dmr le on", lbl_dmr_le(&ctx, b, sizeof(b)), "DMR late entry [On]");
    opts.slot1_on = 1;
    opts.slot2_on = 0;
    rc |= expect_str("slot 1 on", lbl_slot1(&ctx, b, sizeof(b)), "Slot 1 audio [On]");
    rc |= expect_str("slot 2 off", lbl_slot2(&ctx, b, sizeof(b)), "Slot 2 audio [Off]");
    opts.slot_preference = 0;
    rc |= expect_str("slot pref one", lbl_slotpref(&ctx, b, sizeof(b)), "Slot preference... [1]");
    opts.slot_preference = 1;
    rc |= expect_str("slot pref two", lbl_slotpref(&ctx, b, sizeof(b)), "Slot preference... [2]");
    opts.slot_preference = 5;
    rc |= expect_str("slot pref auto", lbl_slotpref(&ctx, b, sizeof(b)), "Slot preference... [Auto]");
    state.esk_mask = 0xA0;
    state.ea_mode = 0;
    rc |= expect_str("provoice esk on", lbl_provoice_esk(&ctx, b, sizeof(b)), "ProVoice ESK mask [On]");
    rc |= expect_str("provoice ea off", lbl_provoice_mode(&ctx, b, sizeof(b)), "ProVoice EA mode [Off]");

    opts.inverted_dmr = 1;
    opts.inverted_x2tdma = 0;
    rc |= expect_str("invert all on", lbl_invert_all(&ctx, b, sizeof(b)), "Invert signal [On]");
    rc |= expect_str("invert x2 off", lbl_inv_x2(&ctx, b, sizeof(b)), "Invert X2-TDMA [Off]");
    rc |= expect_str("invert dmr on", lbl_inv_dmr(&ctx, b, sizeof(b)), "Invert DMR [On]");
    rc |= expect_str("invert dpmr off", lbl_inv_dpmr(&ctx, b, sizeof(b)), "Invert dPMR [Off]");
    rc |= expect_str("invert m17 off", lbl_inv_m17(&ctx, b, sizeof(b)), "Invert M17 [Off]");

    rc |=
        expect_str("m17 user data unset", lbl_m17_user_data(&ctx, b, sizeof(b)), "M17 encoder user data... [<unset>]");
    DSD_SNPRINTF(state.m17dat, sizeof(state.m17dat), "%s", "unit test");
    rc |= expect_str("m17 user data", lbl_m17_user_data(&ctx, b, sizeof(b)), "M17 encoder user data... [unit test]");

    return rc;
}

static int
test_trunking_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    opts.trunk_enable = 1;
    rc |= expect_str("trunk on", lbl_trunk(&ctx, b, sizeof(b)), "Trunking [On]");
    opts.scanner_mode = 0;
    rc |= expect_str("scanner off", lbl_scan(&ctx, b, sizeof(b)), "Conventional scanning [Off]");

    /* The hold and avoid labels read whichever scanner is running. */
    rc |= expect_str("scan hold null ctx", lbl_scan_hold(NULL, b, sizeof(b)), "Scan hold [Off]");
    rc |= expect_str("scan hold off", lbl_scan_hold(&ctx, b, sizeof(b)), "Scan hold [Off]");
    rc |= expect_str("avoid clear none", lbl_scan_avoid_clear(&ctx, b, sizeof(b)), "Clear avoids [0]");
    opts.scanner_mode = 1;
    state.lcn_scan_hold = 1;
    state.lcn_avoid_count = 3;
    state.trunk_scan_hold = 0;
    state.trunk_scan_avoided_count = 7;
    rc |= expect_str("scan hold -Y", lbl_scan_hold(&ctx, b, sizeof(b)), "Scan hold [On]");
    rc |= expect_str("avoid clear -Y", lbl_scan_avoid_clear(&ctx, b, sizeof(b)), "Clear avoids [3]");
    opts.scanner_mode = 0;
    opts.trunk_scan_enabled = 1;
    rc |= expect_str("scan hold trunk scan", lbl_scan_hold(&ctx, b, sizeof(b)), "Scan hold [Off]");
    rc |= expect_str("avoid clear trunk scan", lbl_scan_avoid_clear(&ctx, b, sizeof(b)), "Clear avoids [7]");
    state.trunk_scan_hold = 1;
    rc |= expect_str("scan hold trunk scan on", lbl_scan_hold(&ctx, b, sizeof(b)), "Scan hold [On]");
    opts.trunk_scan_enabled = 0;
    state.lcn_scan_hold = 0;
    state.lcn_avoid_count = 0;
    state.trunk_scan_hold = 0;
    state.trunk_scan_avoided_count = 0;
    opts.p25_lcw_retune = 1;
    rc |= expect_str("lcw on", lbl_lcw(&ctx, b, sizeof(b)), "LCW explicit retune [On]");
    opts.trunk_use_allow_list = 1;
    rc |= expect_str("allow on", lbl_allow(&ctx, b, sizeof(b)), "Allow-list mode [On]");
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_private_calls = 0;
    opts.trunk_tune_data_calls = 1;
    rc |= expect_str("group calls on", lbl_tune_group(&ctx, b, sizeof(b)), "Group calls [On]");
    rc |= expect_str("private calls off", lbl_tune_priv(&ctx, b, sizeof(b)), "Private calls [Off]");
    rc |= expect_str("data calls on", lbl_tune_data(&ctx, b, sizeof(b)), "Data calls [On]");
    opts.reverse_mute = 1;
    opts.p25_prefer_candidates = 1;
    rc |= expect_str("reverse mute on", lbl_rev_mute(&ctx, b, sizeof(b)), "Reverse mute [On]");
    rc |= expect_str("prefer cc on", lbl_pref_cc(&ctx, b, sizeof(b)), "Prefer CC candidates [On]");

    state.tg_hold = 0;
    rc |= expect_str("tg hold none", lbl_tg_hold(&ctx, b, sizeof(b)), "Talkgroup hold... [none]");
    state.tg_hold = 4242;
    rc |= expect_str("tg hold set", lbl_tg_hold(&ctx, b, sizeof(b)), "Talkgroup hold... [4242]");
    opts.trunk_hangtime = 2.75f;
    rc |= expect_str("hangtime", lbl_hangtime(&ctx, b, sizeof(b)), "Hangtime... [2.8 s]");
    rc |= expect_str("voice-only null ctx", lbl_scan_voice_only(NULL, b, sizeof(b)), "Voice-only scan [Off]");
    opts.scan_voice_only = 0;
    rc |= expect_str("voice-only off", lbl_scan_voice_only(&ctx, b, sizeof(b)), "Voice-only scan [Off]");
    opts.scan_voice_only = 1;
    rc |= expect_str("voice-only on", lbl_scan_voice_only(&ctx, b, sizeof(b)), "Voice-only scan [On]");
    opts.scan_voice_only = 0;
    rc |= expect_str("voice qualify null ctx", lbl_scan_voice_qualify(NULL, b, sizeof(b)), "Voice qualify... [0 ms]");
    opts.scan_voice_qualify_ms = 1500;
    rc |= expect_str("voice qualify", lbl_scan_voice_qualify(&ctx, b, sizeof(b)), "Voice qualify... [1500 ms]");
    rc |= expect_str("voice hold null ctx", lbl_scan_voice_hold(NULL, b, sizeof(b)), "Voice hold... [0 ms]");
    opts.scan_voice_hold_ms = 2500;
    rc |= expect_str("voice hold", lbl_scan_voice_hold(&ctx, b, sizeof(b)), "Voice hold... [2500 ms]");

    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 4;
    DSD_SNPRINTF(opts.rigctlhostname, sizeof(opts.rigctlhostname), "%s", "rig.local");
    opts.rigctlportno = 4532;
    rc |= expect_str("rigctl on", lbl_rigctl(&ctx, b, sizeof(b)), "Rigctl: rig.local:4532 [On]");
    opts.rigctl_sockfd = DSD_INVALID_SOCKET;
    rc |= expect_str("rigctl off", lbl_rigctl(&ctx, b, sizeof(b)), "Rigctl: rig.local:4532 [Off]");
    opts.rigctlhostname[0] = '\0';
    rc |= expect_str("rigctl unconfigured", lbl_rigctl(&ctx, b, sizeof(b)), "Rigctl... [Off]");

    g_env_double_has_value = 1;
    g_env_double_value = 1.5;
    rc |= expect_str("p25 vc grace", lbl_p25_vc_grace(NULL, b, sizeof(b)), "VC grace... [1.500 s]");
    rc |= expect_str("p25 min follow", lbl_p25_min_follow(NULL, b, sizeof(b)), "Minimum follow dwell... [1.500 s]");
    rc |= expect_str("p25 grant voice", lbl_p25_grant_voice(NULL, b, sizeof(b)), "Grant-to-voice timeout... [1.500 s]");
    rc |= expect_str("p25 cc grace", lbl_p25_cc_grace(NULL, b, sizeof(b)), "CC hunt grace... [1.500 s]");
    rc |= expect_str("p25 force extra", lbl_p25_force_extra(NULL, b, sizeof(b)), "Safety-net extra... [1.500 s]");
    rc |= expect_str("p25 force margin", lbl_p25_force_margin(NULL, b, sizeof(b)), "Safety-net margin... [1.500 s]");
    rc |= expect_str("p25p1 err pct", lbl_p25_p1_err_pct(NULL, b, sizeof(b)), "Phase 1 error hold... [1.5%]");
    rc |= expect_str("p25p1 err sec", lbl_p25_p1_err_sec(NULL, b, sizeof(b)), "Phase 1 error hold time... [1.500 s]");

    return rc;
}

static int
test_encryption_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    opts.unmute_encrypted_p25 = 0;
    rc |= expect_str("muting on", lbl_muting(&ctx, b, sizeof(b)), "Mute encrypted audio [On]");
    opts.unmute_encrypted_p25 = 1;
    rc |= expect_str("muting off", lbl_muting(&ctx, b, sizeof(b)), "Mute encrypted audio [Off]");

    opts.trunk_tune_enc_calls = 0;
    rc |= expect_str("enc lockout on", lbl_p25_enc_lockout(&ctx, b, sizeof(b)), "Lock out encrypted calls [On]");
    opts.trunk_tune_enc_calls = 1;
    rc |= expect_str("enc lockout off", lbl_p25_enc_lockout(&ctx, b, sizeof(b)), "Lock out encrypted calls [Off]");
    rc |= expect_str("enc lockout clear count", lbl_enc_lockout_clear(&ctx, b, sizeof(b)), "Clear lockouts [0]");

    state.M = 1;
    rc |= expect_str("force bp on", lbl_key_force_bp(&ctx, b, sizeof(b)), "Force basic/scrambler key [On]");
    rc |= expect_str("force rc4 off", lbl_key_force_rc4(&ctx, b, sizeof(b)), "Force RC4 key [Off]");
    state.M = 0x21;
    rc |= expect_str("force bp off", lbl_key_force_bp(&ctx, b, sizeof(b)), "Force basic/scrambler key [Off]");
    rc |= expect_str("force rc4 on", lbl_key_force_rc4(&ctx, b, sizeof(b)), "Force RC4 key [On]");

    rc |= expect_str("hytera unset", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera privacy key (hex)...");
    state.H = 0x1234U;
    state.K1 = 0x11U;
    state.hytera_key_segments = 1;
    rc |= expect_str("hytera 40 bit", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera privacy key (hex)... [40-bit]");
    state.hytera_key_segments = 2;
    state.K2 = 0x22U;
    rc |= expect_str("hytera 128 bit", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera privacy key (hex)... [128-bit]");
    state.hytera_key_segments = 4;
    state.K3 = 0x33U;
    state.K4 = 0x44U;
    rc |= expect_str("hytera 256 bit", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera privacy key (hex)... [256-bit]");

    return rc;
}

static int
test_input_and_audio_labels(void) {
    int rc = 0;
    char b[192];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_str("source null", lbl_current_input(NULL, b, sizeof(b)), "Source: ?");
    opts.audio_out_type = 0;
    rc |= expect_str("output pulse default", lbl_current_output(&ctx, b, sizeof(b)), "Output: Pulse [default]");
    DSD_SNPRINTF(opts.pa_output_idx, sizeof(opts.pa_output_idx), "%s", "alsa_output.monitor");
    rc |=
        expect_str("output pulse index", lbl_current_output(&ctx, b, sizeof(b)), "Output: Pulse [alsa_output.monitor]");
    opts.audio_out_type = 8;
    DSD_SNPRINTF(opts.udp_hostname, sizeof(opts.udp_hostname), "%s", "239.1.2.3");
    opts.udp_portno = 23456;
    rc |= expect_str("output udp", lbl_current_output(&ctx, b, sizeof(b)), "Output: UDP 239.1.2.3:23456");

    opts.audio_in_type = AUDIO_IN_TCP;
    DSD_SNPRINTF(opts.tcp_hostname, sizeof(opts.tcp_hostname), "%s", "tcp.example");
    opts.tcp_portno = 7355;
    rc |= expect_str("source tcp", lbl_current_input(&ctx, b, sizeof(b)), "Source: TCP tcp.example:7355");
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.udp_in_portno = 7356;
    rc |= expect_str("source udp default addr", lbl_current_input(&ctx, b, sizeof(b)), "Source: UDP 127.0.0.1:7356");
    opts.audio_in_type = AUDIO_IN_WAV;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "capture.wav");
    rc |= expect_str("source file", lbl_current_input(&ctx, b, sizeof(b)), "Source: capture.wav");
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dev_index = 2;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "rtl");
    rc |= expect_str("source rtl", lbl_current_input(&ctx, b, sizeof(b)), "Source: RTL-SDR dev 2");
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "soapy:driver=test");
    rc |= expect_str("source soapy", lbl_current_input(&ctx, b, sizeof(b)), "Source: SoapySDR [driver=test]");
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "soapy");
    rc |= expect_str("source soapy bare", lbl_current_input(&ctx, b, sizeof(b)), "Source: SoapySDR");
    opts.audio_in_type = AUDIO_IN_PULSE;
    rc |= expect_str("source pulse", lbl_current_input(&ctx, b, sizeof(b)), "Source: Pulse");
    opts.audio_in_type = AUDIO_IN_STDIN;
    rc |= expect_str("source stdin", lbl_current_input(&ctx, b, sizeof(b)), "Source: STDIN");

    opts.audio_out = 0;
    rc |= expect_str("output muted", lbl_out_mute(&ctx, b, sizeof(b)), "Mute [On]");
    opts.audio_out = 1;
    rc |= expect_str("output unmuted", lbl_out_mute(&ctx, b, sizeof(b)), "Mute [Off]");
    opts.monitor_input_audio = 0;
    ((dsd_opts*)dsd_app_get_latest_opts_snapshot())->monitor_input_audio = 1;
    rc |= expect_str("monitor on", lbl_monitor(&ctx, b, sizeof(b)), "Source audio monitor [On]");
    opts.input_volume_multiplier = 0;
    rc |=
        expect_str("input volume clamps display minimum", lbl_input_volume(&ctx, b, sizeof(b)), "Input volume... [1X]");
    opts.input_volume_multiplier = 4;
    rc |= expect_str("input volume", lbl_input_volume(&ctx, b, sizeof(b)), "Input volume... [4X]");
    opts.input_warn_db = -33.5;
    rc |= expect_str("input warn from opts", lbl_input_warn(&ctx, b, sizeof(b)), "Low-input warning... [-33.5 dBFS]");
    g_env_double_has_value = 1;
    g_env_double_value = -22.25;
    rc |= expect_str("input warn from env default path", lbl_input_warn(NULL, b, sizeof(b)),
                     "Low-input warning... [-22.2 dBFS]");

    opts.audio_gain = 0.0f;
    opts.audio_gainA = 50.0f;
    rc |= expect_str("digital gain auto", lbl_gain_dig(&ctx, b, sizeof(b)), "Digital gain... [auto]");
    opts.audio_gain = 12.0f;
    rc |= expect_str("digital gain set", lbl_gain_dig(&ctx, b, sizeof(b)), "Digital gain... [12]");
    rc |= expect_str("analog gain", lbl_gain_ana(&ctx, b, sizeof(b)), "Analog gain... [50]");

    opts.audio_in_type = AUDIO_IN_TCP;
    opts.tcp_in_ctx = (tcp_input_ctx*)0x1;
    g_tcp_valid = 1;
    rc |= expect_str("tcp on", lbl_tcp(&ctx, b, sizeof(b)), "TCP audio: tcp.example:7355 [On]");
    g_tcp_valid = 0;
    rc |= expect_str("tcp off", lbl_tcp(&ctx, b, sizeof(b)), "TCP audio: tcp.example:7355 [Off]");
    opts.tcp_hostname[0] = '\0';
    rc |= expect_str("tcp unconfigured", lbl_tcp(&ctx, b, sizeof(b)), "TCP audio... [Off]");

    g_cfg.deemph_mode = DSD_NEO_DEEMPH_NFM;
    rc |= expect_str("deemphasis nfm", lbl_deemph(NULL, b, sizeof(b)), "Deemphasis [NFM]");
    rc |= expect_str("audio lpf off", lbl_audio_lpf(NULL, b, sizeof(b)), "Audio low-pass... [Off]");
    g_cfg.audio_lpf_is_set = 1;
    g_cfg.audio_lpf_cutoff_hz = 4200;
    rc |= expect_str("audio lpf set", lbl_audio_lpf(NULL, b, sizeof(b)), "Audio low-pass... [4200 Hz]");

    opts.call_alert = 0;
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_ALL;
    rc |= expect_str("call alert off", lbl_call_alert(&ctx, b, sizeof(b)), "Call alert beep [Off]");
    rc |= expect_str("call alert events off when disabled", lbl_call_alert_events(&ctx, b, sizeof(b)),
                     "Alert on... [Off]");
    opts.call_alert = 1;
    opts.call_alert_events = 0;
    rc |= expect_str("call alert zero mask defaults to all", lbl_call_alert_events(&ctx, b, sizeof(b)),
                     "Alert on... [All]");
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA;
    rc |= expect_str("call alert start data", lbl_call_alert_events(&ctx, b, sizeof(b)), "Alert on... [Start+Data]");

    return rc;
}

static int
test_recording_labels(void) {
    int rc = 0;
    char b[192];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_str("record symbols off", lbl_sym_save(&ctx, b, sizeof(b)), "Record symbols... [Off]");
    rc |= expect_str("stop recording off", lbl_stop_symbol_capture(&ctx, b, sizeof(b)), "Stop recording [Off]");
    opts.symbol_out_f = (FILE*)0x1;
    DSD_SNPRINTF(opts.symbol_out_file, sizeof(opts.symbol_out_file), "%s", "symbols.bin");
    rc |= expect_str("record symbols on", lbl_sym_save(&ctx, b, sizeof(b)), "Record symbols... [symbols.bin]");
    rc |= expect_str("stop recording on", lbl_stop_symbol_capture(&ctx, b, sizeof(b)), "Stop recording [symbols.bin]");

    rc |= expect_str("per call wav off", lbl_per_call_wav(&ctx, b, sizeof(b)), "Per-call WAV [Off]");
    opts.dmr_stereo_wav = 1;
    opts.wav_out_f = (SNDFILE*)0x1;
    rc |= expect_str("per call wav on", lbl_per_call_wav(&ctx, b, sizeof(b)), "Per-call WAV [On]");

    rc |= expect_str("stop replay off", lbl_stop_symbol_playback(&ctx, b, sizeof(b)), "Stop replay [Off]");
    opts.symbolfile = (FILE*)0x2;
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "last.bin");
    rc |= expect_str("stop replay on", lbl_stop_symbol_playback(&ctx, b, sizeof(b)), "Stop replay [last.bin]");
    g_stat_path_rc = 0;
    rc |= expect_str("replay last available", lbl_replay_last(&ctx, b, sizeof(b)), "Replay last capture [last.bin]");
    g_stat_path_rc = -1;
    rc |= expect_str("replay last none", lbl_replay_last(&ctx, b, sizeof(b)), "Replay last capture [none]");

    rc |= expect_str("event log off", lbl_event_log(&ctx, b, sizeof(b)), "Event log file... [off]");
    DSD_SNPRINTF(opts.event_out_file, sizeof(opts.event_out_file), "%s", "events.log");
    rc |= expect_str("event log set", lbl_event_log(&ctx, b, sizeof(b)), "Event log file... [events.log]");
    opts.payload = 0;
    rc |= expect_str("payload off", lbl_toggle_payload(&ctx, b, sizeof(b)), "Payload logging to console [Off]");

    rc |= expect_str("lrrp off", lbl_lrrp_current(&ctx, b, sizeof(b)), "LRRP output: off");
    opts.lrrp_file_output = 1;
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof(opts.lrrp_out_file), "%s", "lrrp.log");
    rc |= expect_str("lrrp on", lbl_lrrp_current(&ctx, b, sizeof(b)), "LRRP output: lrrp.log");

    return rc;
}

static int
test_display_and_advanced_labels(void) {
    int rc = 0;
    char b[192];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    opts.frontend_display.show_p25_metrics = 1;
    opts.frontend_display.show_p25_group_affiliations = 1;
    opts.frontend_display.show_channels = 1;
    rc |= expect_str("p25 metrics on", lbl_ui_p25_metrics(&ctx, b, sizeof(b)), "P25 metrics [On]");
    rc |= expect_str("p25 affiliations off", lbl_ui_p25_affil(&ctx, b, sizeof(b)), "P25 affiliations [Off]");
    rc |= expect_str("p25 group affiliation on", lbl_ui_p25_ga(&ctx, b, sizeof(b)), "P25 group affiliation [On]");
    rc |= expect_str("p25 neighbors off", lbl_ui_p25_neighbors(&ctx, b, sizeof(b)), "P25 neighbors [Off]");
    rc |= expect_str("p25 iden off", lbl_ui_p25_iden(&ctx, b, sizeof(b)), "P25 IDEN plan [Off]");
    rc |= expect_str("p25 cc candidates off", lbl_ui_p25_ccc(&ctx, b, sizeof(b)), "P25 CC candidates [Off]");
    rc |= expect_str("p25 callsign off", lbl_ui_p25_callsign(&ctx, b, sizeof(b)), "P25 callsign decode [Off]");
    rc |= expect_str("channels on", lbl_ui_channels(&ctx, b, sizeof(b)), "Channels [On]");
    rc |= expect_str("compact view off", lbl_ui_compact(&ctx, b, sizeof(b)), "Compact view [Off]");
    opts.frontend_terminal_display.terminal_compact = 1;
    rc |= expect_str("compact view on", lbl_ui_compact(&ctx, b, sizeof(b)), "Compact view [On]");

    opts.frontend_display.constellation = 1;
    opts.frontend_display.const_norm_mode = 0;
    opts.frontend_display.eye_view = 1;
    opts.frontend_terminal_display.eye_unicode = 1;
    opts.frontend_terminal_display.eye_color = 0;
    opts.frontend_display.fsk_hist_view = 0;
    opts.frontend_display.spectrum_view = 1;
    rc |= expect_str("constellation on", lbl_vis_const(&ctx, b, sizeof(b)), "Constellation [On]");
    /* Two named modes, not on/off: 0 is radial p99 and is still normalizing. */
    rc |= expect_str("constellation norm radial", lbl_vis_const_norm(&ctx, b, sizeof(b)),
                     "Constellation normalization [Radial]");
    opts.frontend_display.const_norm_mode = 1;
    rc |= expect_str("constellation norm unit", lbl_vis_const_norm(&ctx, b, sizeof(b)),
                     "Constellation normalization [Unit circle]");
    opts.frontend_display.const_norm_mode = 0;
    rc |= expect_str("eye on", lbl_vis_eye(&ctx, b, sizeof(b)), "Eye diagram [On]");
    rc |= expect_str("eye unicode on", lbl_vis_eye_unicode(&ctx, b, sizeof(b)), "Eye diagram Unicode [On]");
    rc |= expect_str("eye color off", lbl_vis_eye_color(&ctx, b, sizeof(b)), "Eye diagram color [Off]");
    rc |= expect_str("fsk hist off", lbl_vis_fsk(&ctx, b, sizeof(b)), "FSK histogram [Off]");
    rc |= expect_str("spectrum on", lbl_vis_spectrum(&ctx, b, sizeof(b)), "Spectrum analyzer [On]");

    g_history_mode = 0;
    rc |= expect_str("history mode off", lbl_history_mode(&ctx, b, sizeof(b)), "Mode [Off]");
    g_history_mode = 1;
    rc |= expect_str("history mode short", lbl_history_mode(&ctx, b, sizeof(b)), "Mode [Short]");
    g_history_mode = 2;
    rc |= expect_str("history mode long", lbl_history_mode(&ctx, b, sizeof(b)), "Mode [Long]");
    state.eh_slot = 0;
    rc |= expect_str("history slot 1", lbl_history_slot(&ctx, b, sizeof(b)), "Slot [1]");
    state.eh_slot = 1;
    rc |= expect_str("history slot 2", lbl_history_slot(&ctx, b, sizeof(b)), "Slot [2]");
    /* The third state is the startup default, and used to read exactly like slot 1. */
    state.eh_slot = 2;
    rc |= expect_str("history slot both", lbl_history_slot(&ctx, b, sizeof(b)), "Slot [1+2]");

    g_env_double_has_value = 1;
    g_env_double_value = -22.25;
    rc |= expect_str("auto ppm snr env", lbl_auto_ppm_snr(NULL, b, sizeof(b)), "Auto-PPM SNR threshold... [-22.2 dB]");
    rc |= expect_str("auto ppm pwr env", lbl_auto_ppm_pwr(NULL, b, sizeof(b)), "Auto-PPM minimum power... [-22.2 dB]");
    g_env_double_value = 0.75;
    rc |= expect_str("auto ppm zeroppm env", lbl_auto_ppm_zeroppm(NULL, b, sizeof(b)),
                     "Auto-PPM zero-lock PPM... [0.75]");
    g_env_int_has_value = 1;
    g_env_int_value = 75;
    rc |= expect_str("auto ppm zerohz env", lbl_auto_ppm_zerohz(NULL, b, sizeof(b)), "Auto-PPM zero-lock Hz... [75]");
    rc |= expect_str("tcp prebuffer env", lbl_tcp_prebuf(NULL, b, sizeof(b)), "rtl_tcp prebuffer... [75 ms]");
    rc |= expect_str("tcp rcvbuf env", lbl_tcp_rcvbuf(NULL, b, sizeof(b)), "rtl_tcp SO_RCVBUF... [75 bytes]");
    rc |= expect_str("tcp rcvtimeo env", lbl_tcp_rcvtimeo(NULL, b, sizeof(b)), "rtl_tcp SO_RCVTIMEO... [75 ms]");
    g_env_int_value = 0;
    rc |= expect_str("tcp rcvbuf default", lbl_tcp_rcvbuf(NULL, b, sizeof(b)), "rtl_tcp SO_RCVBUF... [system default]");
    rc |= expect_str("tcp rcvtimeo off", lbl_tcp_rcvtimeo(NULL, b, sizeof(b)), "rtl_tcp SO_RCVTIMEO... [Off]");

    g_cfg.window_freeze_is_set = 1;
    g_cfg.window_freeze = 1;
    rc |= expect_str("window freeze on", lbl_window_freeze(NULL, b, sizeof(b)), "Freeze symbol window [On]");
    g_cfg.auto_ppm_freeze_enable = 1;
    rc |= expect_str("auto ppm freeze on", lbl_auto_ppm_freeze(NULL, b, sizeof(b)), "Auto-PPM freeze [On]");
    g_cfg.tcp_waitall_enable = 1;
    rc |= expect_str("tcp waitall on", lbl_tcp_waitall(NULL, b, sizeof(b)), "rtl_tcp MSG_WAITALL [On]");
    g_cfg.rt_sched_enable = 1;
    rc |= expect_str("rt sched on", lbl_rt_sched(NULL, b, sizeof(b)), "Realtime scheduling [On]");
    g_cfg.mt_is_set = 1;
    g_cfg.mt_enable = 1;
    rc |= expect_str("mt on", lbl_mt(NULL, b, sizeof(b)), "Intra-block multithreading [On]");

    return rc;
}

static int
test_radioreference_labels_and_predicates(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    g_rr_available = 1;
    rc |= expect_int("rr feature available", rr_feature_available(&ctx), 1);
    g_rr_available = 0;
    rc |= expect_int("rr feature unavailable", rr_feature_available(&ctx), 0);
    g_rr_available = 1;

    g_rr_builtin_key[0] = '\0';
    rc |= expect_int("rr key prompt offered without baked key", rr_key_prompt_offered(&ctx), 1);
    rc |= expect_int("rr key prompt offered and feature", rr_key_prompt_offered_and_feature(&ctx), 1);
    DSD_SNPRINTF(g_rr_builtin_key, sizeof g_rr_builtin_key, "%s", "BAKED");
    rc |= expect_int("rr key prompt hidden with baked key", rr_key_prompt_offered(&ctx), 0);
    rc |= expect_int("rr key prompt hidden and feature", rr_key_prompt_offered_and_feature(&ctx), 0);
    g_rr_builtin_key[0] = '\0';

    /* The predicate reports whether an imports directory resolves, not whether
       any import exists there. */
    g_imports_dir = "/tmp/dsd-neo-imports";
    rc |= expect_int("rr refresh enabled when the imports dir resolves", rr_imports_available(&ctx), 1);
    rc |= expect_int("rr imports and feature", rr_imports_available_and_feature(&ctx), 1);
    g_imports_dir = NULL;
    rc |= expect_int("rr refresh disabled without a config dir", rr_imports_available(&ctx), 0);
    g_imports_dir = "";
    rc |= expect_int("rr refresh disabled on an empty imports dir", rr_imports_available(&ctx), 0);
    g_imports_dir = "/tmp/dsd-neo-imports";

    /* Inside the Channels & groups submenu each RadioReference row gates itself. */
    g_rr_available = 0;
    rc |= expect_int("rr imports hidden without the feature", rr_imports_available_and_feature(&ctx), 0);
    rc |= expect_int("rr key hidden without the feature", rr_key_prompt_offered_and_feature(&ctx), 0);
    g_rr_available = 1;

    rc |= expect_str("rr account unset", lbl_rr_account(&ctx, b, sizeof(b)), "RadioReference username... [(not set)]");
    DSD_SNPRINTF(opts.rr_username, sizeof opts.rr_username, "%s", "kb1abc");
    rc |= expect_str("rr account set", lbl_rr_account(&ctx, b, sizeof(b)), "RadioReference username... [kb1abc]");
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_predicates();
    rc |= test_decoder_labels();
    rc |= test_trunking_labels();
    rc |= test_encryption_labels();
    rc |= test_input_and_audio_labels();
    rc |= test_recording_labels();
    rc |= test_display_and_advanced_labels();
    rc |= test_radioreference_labels_and_predicates();
    return rc ? 1 : 0;
}
