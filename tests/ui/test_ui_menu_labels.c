// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Deterministic contracts for terminal UI menu label helpers.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/tcp_input.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/config.h>
#include <sndfile.h>
#include <stdio.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "menu_env.h"
#include "menu_internal.h"
#include "menu_labels.h"

static dsdneoRuntimeConfig g_cfg;
static int g_cfg_valid = 1;
static int g_stat_path_rc = -1;
static int g_tcp_valid = 1;
static int g_env_int_value;
static int g_env_int_has_value;
static double g_env_double_value;
static int g_env_double_has_value;

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
    g_env_int_value = 0;
    g_env_int_has_value = 0;
    g_env_double_value = 0.0;
    g_env_double_has_value = 0;
    ctx->opts = opts;
    ctx->state = state;
}

static int
test_visibility_and_state_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_int("always-on predicate", io_always_on(NULL), 1);
    rc |= expect_int("rtl active null ctx", io_rtl_active(NULL), 0);
    opts.audio_in_type = AUDIO_IN_RTL;
    rc |= expect_int("rtl active input", io_rtl_active(&ctx), 1);

    opts.inverted_dmr = 1;
    rc |= expect_str("invert all active", lbl_invert_all(&ctx, b, sizeof(b)), "Toggle Signal Inversion [Active]");
    opts.payload = 0;
    rc |= expect_str("payload inactive", lbl_toggle_payload(&ctx, b, sizeof(b)), "Toggle Payload Logging [Inactive]");
    opts.p25_trunk = 1;
    rc |= expect_str("trunk active", lbl_trunk(&ctx, b, sizeof(b)), "Toggle Trunking [Active]");
    opts.scanner_mode = 0;
    rc |= expect_str("scanner inactive", lbl_scan(&ctx, b, sizeof(b)), "Toggle Scanning Mode [Inactive]");
    opts.p25_lcw_retune = 1;
    rc |= expect_str("lcw active", lbl_lcw(&ctx, b, sizeof(b)), "Toggle P25 LCW Retune [Active]");
    opts.trunk_tune_enc_calls = 0;
    rc |= expect_str("p25 enc lockout on", lbl_p25_enc_lockout(&ctx, b, sizeof(b)), "P25 Encrypted Call Lockout [On]");
    opts.aggressive_framesync = 0;
    rc |= expect_str("crc relaxed active", lbl_crc_relax(&ctx, b, sizeof(b)), "Toggle Relaxed CRC checks [Active]");
    opts.trunk_use_allow_list = 1;
    rc |= expect_str("allow active", lbl_allow(&ctx, b, sizeof(b)), "Toggle Allow/White List [Active]");
    opts.trunk_tune_group_calls = 1;
    opts.trunk_tune_private_calls = 0;
    opts.trunk_tune_data_calls = 1;
    rc |= expect_str("tune group active", lbl_tune_group(&ctx, b, sizeof(b)), "Toggle Tune Group Calls [Active]");
    rc |=
        expect_str("tune private inactive", lbl_tune_priv(&ctx, b, sizeof(b)), "Toggle Tune Private Calls [Inactive]");
    rc |= expect_str("tune data active", lbl_tune_data(&ctx, b, sizeof(b)), "Toggle Tune Data Calls [Active]");
    opts.reverse_mute = 1;
    opts.dmr_le = 1;
    opts.p25_prefer_candidates = 1;
    rc |= expect_str("reverse mute active", lbl_rev_mute(&ctx, b, sizeof(b)), "Toggle Reverse Mute [Active]");
    rc |= expect_str("dmr le active", lbl_dmr_le(&ctx, b, sizeof(b)), "Toggle DMR Late Entry [Active]");
    rc |= expect_str("prefer cc active", lbl_pref_cc(&ctx, b, sizeof(b)), "Prefer P25 CC Candidates [Active]");

    return rc;
}

static int
test_call_alert_slot_and_muting_labels(void) {
    int rc = 0;
    char b[160];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    opts.slot_preference = 0;
    rc |= expect_str("slot pref one", lbl_slotpref(&ctx, b, sizeof(b)), "Set TDMA Slot Preference... [now 1]");
    opts.slot_preference = 1;
    rc |= expect_str("slot pref two", lbl_slotpref(&ctx, b, sizeof(b)), "Set TDMA Slot Preference... [now 2]");
    opts.slot_preference = 5;
    rc |= expect_str("slot pref auto", lbl_slotpref(&ctx, b, sizeof(b)), "Set TDMA Slot Preference... [now Auto]");

    opts.slot1_on = 1;
    opts.slot2_on = 1;
    rc |= expect_str("slots both", lbl_slots_on(&ctx, b, sizeof(b)), "Set TDMA Synth Slots... [now both]");
    opts.slot2_on = 0;
    rc |= expect_str("slot one", lbl_slots_on(&ctx, b, sizeof(b)), "Set TDMA Synth Slots... [now 1]");
    opts.slot1_on = 0;
    opts.slot2_on = 1;
    rc |= expect_str("slot two", lbl_slots_on(&ctx, b, sizeof(b)), "Set TDMA Synth Slots... [now 2]");
    opts.slot2_on = 0;
    rc |= expect_str("slots off", lbl_slots_on(&ctx, b, sizeof(b)), "Set TDMA Synth Slots... [now off]");

    opts.dmr_mute_encL = 1;
    opts.dmr_mute_encR = 1;
    opts.unmute_encrypted_p25 = 0;
    rc |= expect_str("muting active", lbl_muting(&ctx, b, sizeof(b)), "Toggle Encrypted Audio Muting [Active]");
    opts.unmute_encrypted_p25 = 1;
    rc |= expect_str("muting inactive", lbl_muting(&ctx, b, sizeof(b)), "Toggle Encrypted Audio Muting [Inactive]");

    opts.call_alert = 0;
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_ALL;
    rc |= expect_str("call alert inactive", lbl_call_alert(&ctx, b, sizeof(b)), "Toggle Call Alert Beep [Inactive]");
    rc |= expect_str("call alert events off when disabled", lbl_call_alert_events(&ctx, b, sizeof(b)),
                     "Call Alert Events [Off]");
    opts.call_alert = 1;
    opts.call_alert_events = 0;
    rc |= expect_str("call alert all when enabled with default", lbl_call_alert_events(&ctx, b, sizeof(b)),
                     "Call Alert Events [All]");
    opts.call_alert_events = DSD_CALL_ALERT_EVENT_VOICE_START | DSD_CALL_ALERT_EVENT_DATA;
    rc |= expect_str("call alert start data", lbl_call_alert_events(&ctx, b, sizeof(b)),
                     "Call Alert Events [Start+Data]");

    return rc;
}

static int
test_io_and_capture_labels(void) {
    int rc = 0;
    char b[192];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    rc |= expect_str("current input null", lbl_current_input(NULL, b, sizeof(b)), "Current Input: ?");
    opts.audio_out_type = 0;
    rc |= expect_str("current output pulse default", lbl_current_output(&ctx, b, sizeof(b)),
                     "Current Output: Pulse [default]");
    DSD_SNPRINTF(opts.pa_output_idx, sizeof(opts.pa_output_idx), "%s", "alsa_output.monitor");
    rc |= expect_str("current output pulse index", lbl_current_output(&ctx, b, sizeof(b)),
                     "Current Output: Pulse [alsa_output.monitor]");
    opts.audio_out_type = 8;
    DSD_SNPRINTF(opts.udp_hostname, sizeof(opts.udp_hostname), "%s", "239.1.2.3");
    opts.udp_portno = 23456;
    rc |=
        expect_str("current output udp", lbl_current_output(&ctx, b, sizeof(b)), "Current Output: UDP 239.1.2.3:23456");

    opts.audio_in_type = AUDIO_IN_TCP;
    DSD_SNPRINTF(opts.tcp_hostname, sizeof(opts.tcp_hostname), "%s", "tcp.example");
    opts.tcp_portno = 7355;
    rc |= expect_str("current input tcp", lbl_current_input(&ctx, b, sizeof(b)), "Current Input: TCP tcp.example:7355");
    opts.audio_in_type = AUDIO_IN_UDP;
    opts.udp_in_portno = 7356;
    rc |= expect_str("current input udp default addr", lbl_current_input(&ctx, b, sizeof(b)),
                     "Current Input: UDP 127.0.0.1:7356");
    opts.audio_in_type = AUDIO_IN_WAV;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "capture.wav");
    rc |= expect_str("current input file", lbl_current_input(&ctx, b, sizeof(b)), "Current Input: capture.wav");
    opts.audio_in_type = AUDIO_IN_RTL;
    opts.rtl_dev_index = 2;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "rtl");
    rc |= expect_str("current input rtl", lbl_current_input(&ctx, b, sizeof(b)), "Current Input: RTL-SDR dev 2");
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "soapy:driver=test");
    rc |= expect_str("current input soapy", lbl_current_input(&ctx, b, sizeof(b)),
                     "Current Input: SoapySDR [driver=test]");
    opts.audio_in_type = AUDIO_IN_PULSE;
    rc |= expect_str("current input pulse", lbl_current_input(&ctx, b, sizeof(b)), "Current Input: Pulse");
    opts.audio_in_type = AUDIO_IN_STDIN;
    rc |= expect_str("current input stdin", lbl_current_input(&ctx, b, sizeof(b)), "Current Input: STDIN");

    opts.audio_out = 0;
    rc |= expect_str("output muted", lbl_out_mute(&ctx, b, sizeof(b)), "Mute Output [On]");
    opts.monitor_input_audio = 1;
    opts.use_cosine_filter = 1;
    opts.input_volume_multiplier = 0;
    rc |= expect_str("monitor active", lbl_monitor(&ctx, b, sizeof(b)), "Toggle Source Audio Monitor [Active]");
    rc |= expect_str("cosine active", lbl_cosine(&ctx, b, sizeof(b)), "Toggle Cosine Filter [Active]");
    rc |= expect_str("input volume clamps display minimum", lbl_input_volume(&ctx, b, sizeof(b)), "Input Volume: 1X");

    opts.audio_in_type = AUDIO_IN_TCP;
    opts.tcp_in_ctx = (tcp_input_ctx*)0x1;
    g_tcp_valid = 1;
    rc |= expect_str("tcp active", lbl_tcp(&ctx, b, sizeof(b)), "TCP Direct Audio: tcp.example:7355 [Active]");
    g_tcp_valid = 0;
    rc |= expect_str("tcp inactive", lbl_tcp(&ctx, b, sizeof(b)), "TCP Direct Audio: tcp.example:7355 [Inactive]");

    opts.use_rigctl = 1;
    opts.rigctl_sockfd = 4;
    DSD_SNPRINTF(opts.rigctlhostname, sizeof(opts.rigctlhostname), "%s", "rig.local");
    opts.rigctlportno = 4532;
    rc |= expect_str("rigctl active", lbl_rigctl(&ctx, b, sizeof(b)), "Rigctl: rig.local:4532 [Active]");
    opts.rigctl_sockfd = 0;
    rc |= expect_str("rigctl inactive", lbl_rigctl(&ctx, b, sizeof(b)), "Rigctl: rig.local:4532 [Inactive]");

    opts.symbol_out_f = (FILE*)0x1;
    DSD_SNPRINTF(opts.symbol_out_file, sizeof(opts.symbol_out_file), "%s", "symbols.bin");
    rc |= expect_str("symbol save active", lbl_sym_save(&ctx, b, sizeof(b)),
                     "Save Symbols to File [Active: symbols.bin]");
    rc |= expect_str("symbol capture active", lbl_stop_symbol_capture(&ctx, b, sizeof(b)),
                     "Stop Symbol Capture [Active: symbols.bin]");
    opts.dmr_stereo_wav = 1;
    opts.wav_out_f = (SNDFILE*)0x1;
    rc |= expect_str("per call wav active", lbl_per_call_wav(&ctx, b, sizeof(b)), "Save Per-Call WAV [Active]");
    opts.symbolfile = (FILE*)0x2;
    opts.audio_in_type = AUDIO_IN_SYMBOL_BIN;
    DSD_SNPRINTF(opts.audio_in_dev, sizeof(opts.audio_in_dev), "%s", "last.bin");
    rc |= expect_str("stop playback active", lbl_stop_symbol_playback(&ctx, b, sizeof(b)),
                     "Stop Symbol Playback [Active: last.bin]");
    g_stat_path_rc = 0;
    rc |= expect_str("replay last available", lbl_replay_last(&ctx, b, sizeof(b)),
                     "Replay Last Symbol Capture [last.bin]");
    g_stat_path_rc = -1;
    rc |= expect_str("replay last inactive", lbl_replay_last(&ctx, b, sizeof(b)),
                     "Replay Last Symbol Capture [Inactive]");

    return rc;
}

static int
test_env_config_display_and_key_labels(void) {
    int rc = 0;
    char b[192];
    static dsd_opts opts;
    static dsd_state state;
    UiCtx ctx;
    reset_fixture(&opts, &state, &ctx);

    opts.input_warn_db = -33.5;
    rc |= expect_str("input warn from opts", lbl_input_warn(&ctx, b, sizeof(b)), "Low Input Warning: -33.5 dBFS");
    g_env_double_has_value = 1;
    g_env_double_value = -22.25;
    rc |= expect_str("input warn from env default path", lbl_input_warn(NULL, b, sizeof(b)),
                     "Low Input Warning: -22.2 dBFS");
    rc |= expect_str("auto ppm snr env", lbl_auto_ppm_snr(NULL, b, sizeof(b)), "Auto-PPM SNR threshold: -22.2 dB");
    rc |= expect_str("auto ppm pwr env", lbl_auto_ppm_pwr(NULL, b, sizeof(b)), "Auto-PPM Min power: -22.2 dB");
    g_env_double_value = 0.75;
    rc |= expect_str("auto ppm zeroppm env", lbl_auto_ppm_zeroppm(NULL, b, sizeof(b)), "Auto-PPM Zero-lock PPM: 0.75");
    g_env_int_has_value = 1;
    g_env_int_value = 75;
    rc |= expect_str("auto ppm zerohz env", lbl_auto_ppm_zerohz(NULL, b, sizeof(b)), "Auto-PPM Zero-lock Hz: 75");
    rc |= expect_str("tcp prebuffer env", lbl_tcp_prebuf(NULL, b, sizeof(b)), "RTL-TCP Prebuffer: 75 ms");
    rc |= expect_str("tcp rcvbuf env", lbl_tcp_rcvbuf(NULL, b, sizeof(b)), "RTL-TCP SO_RCVBUF: 75 bytes");
    rc |= expect_str("tcp rcvtimeo env", lbl_tcp_rcvtimeo(NULL, b, sizeof(b)), "RTL-TCP SO_RCVTIMEO: 75 ms");

    g_cfg.deemph_mode = DSD_NEO_DEEMPH_NFM;
    rc |= expect_str("deemphasis nfm", lbl_deemph(NULL, b, sizeof(b)), "Deemphasis: NFM");
    g_cfg.audio_lpf_is_set = 1;
    g_cfg.audio_lpf_cutoff_hz = 4200;
    rc |= expect_str("audio lpf active", lbl_audio_lpf(NULL, b, sizeof(b)), "Audio LPF: 4200 Hz");
    g_cfg.window_freeze_is_set = 1;
    g_cfg.window_freeze = 1;
    rc |= expect_str("window freeze on", lbl_window_freeze(NULL, b, sizeof(b)), "Freeze Symbol Window: On");
    g_cfg.auto_ppm_freeze_enable = 1;
    rc |= expect_str("auto ppm freeze on", lbl_auto_ppm_freeze(NULL, b, sizeof(b)), "Auto-PPM Freeze: On");
    g_cfg.tcp_waitall_enable = 1;
    rc |= expect_str("tcp waitall on", lbl_tcp_waitall(NULL, b, sizeof(b)), "RTL-TCP MSG_WAITALL: On");
    g_cfg.rt_sched_enable = 1;
    rc |= expect_str("rt sched on", lbl_rt_sched(NULL, b, sizeof(b)), "Realtime Scheduling: On");
    g_cfg.mt_is_set = 1;
    g_cfg.mt_enable = 1;
    rc |= expect_str("mt on", lbl_mt(NULL, b, sizeof(b)), "Intra-block MT: On");

    opts.show_p25_metrics = 1;
    opts.show_p25_group_affiliations = 1;
    opts.show_channels = 1;
    rc |= expect_str("show p25 metrics", lbl_ui_p25_metrics(&ctx, b, sizeof(b)), "Show P25 Metrics [On]");
    rc |= expect_str("show p25 affiliations off", lbl_ui_p25_affil(&ctx, b, sizeof(b)), "Show P25 Affiliations [Off]");
    rc |=
        expect_str("show p25 group affiliation", lbl_ui_p25_ga(&ctx, b, sizeof(b)), "Show P25 Group Affiliation [On]");
    rc |= expect_str("show p25 neighbors off", lbl_ui_p25_neighbors(&ctx, b, sizeof(b)), "Show P25 Neighbors [Off]");
    rc |= expect_str("show p25 iden off", lbl_ui_p25_iden(&ctx, b, sizeof(b)), "Show P25 IDEN Plan [Off]");
    rc |= expect_str("show p25 cc candidates off", lbl_ui_p25_ccc(&ctx, b, sizeof(b)), "Show P25 CC Candidates [Off]");
    rc |= expect_str("show channels", lbl_ui_channels(&ctx, b, sizeof(b)), "Show Channels [On]");
    rc |=
        expect_str("show p25 callsign off", lbl_ui_p25_callsign(&ctx, b, sizeof(b)), "Show P25 Callsign Decode [Off]");

    opts.lrrp_file_output = 1;
    DSD_SNPRINTF(opts.lrrp_out_file, sizeof(opts.lrrp_out_file), "%s", "lrrp.log");
    rc |= expect_str("lrrp active", lbl_lrrp_current(&ctx, b, sizeof(b)), "LRRP Output [Active: lrrp.log]");
    state.M = 1;
    rc |= expect_str("force bp active", lbl_key_force_bp(&ctx, b, sizeof(b)), "Force BP/Scr Priority [Active]");
    state.H = 0x1234U;
    state.K1 = 0x11U;
    state.hytera_key_segments = 1;
    rc |= expect_str("hytera 40 bit", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera Privacy (HEX) [40-bit]");
    state.hytera_key_segments = 2;
    state.K2 = 0x22U;
    rc |= expect_str("hytera 128 bit", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera Privacy (HEX) [128-bit]");
    state.hytera_key_segments = 4;
    state.K3 = 0x33U;
    state.K4 = 0x44U;
    rc |= expect_str("hytera 256 bit", lbl_key_hytera(&ctx, b, sizeof(b)), "Hytera Privacy (HEX) [256-bit]");
    DSD_SNPRINTF(state.m17dat, sizeof(state.m17dat), "%s", "unit test");
    rc |= expect_str("m17 user data", lbl_m17_user_data(&ctx, b, sizeof(b)), "M17 Encoder User Data: unit test");

    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_visibility_and_state_labels();
    rc |= test_call_alert_slot_and_muting_labels();
    rc |= test_io_and_capture_labels();
    rc |= test_env_config_display_and_key_labels();
    return rc ? 1 : 0;
}
