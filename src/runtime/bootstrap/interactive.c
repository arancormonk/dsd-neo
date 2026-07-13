// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/frontend_types.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/decode_mode.h>
#include <dsd-neo/runtime/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#ifndef DSD_RUNTIME_HAS_TERMINAL_UI
#define DSD_RUNTIME_HAS_TERMINAL_UI 1
#endif

static int
path_is_regular_file(const char* path) {
    dsd_stat_t st;
    if (!path || path[0] == '\0') {
        return 0;
    }
    if (dsd_stat_path(path, &st) != 0) {
        return 0;
    }
    return dsd_stat_is_regular(&st);
}

static void
trim_newline(char* s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
}

static int
prompt_yes_no(const char* q, int def_yes) {
    char buf[32];
    DSD_FPRINTF(stderr, "%s [%c/%c]: ", q, def_yes ? 'Y' : 'y', def_yes ? 'n' : 'N');
    if (!fgets(buf, sizeof buf, stdin)) {
        return def_yes;
    }
    trim_newline(buf);
    if (buf[0] == '\0') {
        return def_yes;
    }
    if (buf[0] == 'y' || buf[0] == 'Y') {
        return 1;
    }
    if (buf[0] == 'n' || buf[0] == 'N') {
        return 0;
    }
    return def_yes;
}

static int
prompt_int(const char* q, int def_val, int min_val, int max_val) {
    char buf[64];
    DSD_FPRINTF(stderr, "%s [%d]: ", q, def_val);
    if (!fgets(buf, sizeof buf, stdin)) {
        return def_val;
    }
    trim_newline(buf);
    if (buf[0] == '\0') {
        return def_val;
    }
    char* end = NULL;
    long v = strtol(buf, &end, 10);
    if (end == buf) {
        return def_val;
    }
    if (v < min_val) {
        v = min_val;
    }
    if (v > max_val) {
        v = max_val;
    }
    return (int)v;
}

static void
prompt_string(const char* q, const char* def_val, char* out, size_t out_sz) {
    char buf[1024];
    if (!out || out_sz == 0) {
        return;
    }
    DSD_FPRINTF(stderr, "%s [%s]: ", q, (def_val && *def_val) ? def_val : "");
    if (!fgets(buf, sizeof buf, stdin)) {
        if (def_val) {
            DSD_SNPRINTF(out, out_sz, "%s", def_val);
        } else {
            out[0] = '\0';
        }
        return;
    }
    trim_newline(buf);
    if (buf[0] == '\0') {
        if (def_val) {
            DSD_SNPRINTF(out, out_sz, "%s", def_val);
        } else {
            out[0] = '\0';
        }
        return;
    }
    DSD_SNPRINTF(out, out_sz, "%s", buf);
}

static int
interactive_choose_input_source(void) {
    DSD_FPRINTF(stderr, "\nChoose input source:\n");
    DSD_FPRINTF(stderr, "  1) PulseAudio (mic/loopback) [default]\n");
    DSD_FPRINTF(stderr, "  2) RTL-SDR USB dongle\n");
    DSD_FPRINTF(stderr, "  3) rtl_tcp (network RTL-SDR)\n");
    DSD_FPRINTF(stderr, "  4) File (WAV/BIN)\n");
    DSD_FPRINTF(stderr, "  5) TCP audio (7355)\n");
    DSD_FPRINTF(stderr, "  6) UDP audio (7355)\n");
    return prompt_int("Selection", 1, 1, 6);
}

static void
interactive_configure_rtl_input(dsd_opts* opts, int* src) {
#ifdef USE_RTLSDR
    char freq[64];
    prompt_string("Center frequency in Hz (K/M/G suffix ok, e.g., 851.375M or 851375000)", "", freq, sizeof freq);
    if (freq[0] == '\0') {
        LOG_WARN("WARNING: No frequency entered; falling back to PulseAudio input.\n");
        *src = 1;
        return;
    }
    int dev = prompt_int("RTL device index", 0, 0, 255);
    int gain = prompt_int("RTL gain (dB)", 22, 0, 60);
    int ppm = prompt_int("PPM error", 0, -200, 200);
    int bw = prompt_int("DSP bandwidth (kHz: 4,6,8,12,16,24,48)", 48, 4, 48);
    int sql = prompt_int("Squelch (0=off; negative dB ok via CLI later)", 0, -1000, 100000);
    int vol = prompt_int("Monitor gain multiplier (1..3)", 1, 1, 3);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtl:%d:%s:%d:%d:%d:%d:%d", dev, freq, gain, ppm, bw,
                 sql, vol);
#else
    (void)opts;
    LOG_WARN("WARNING: RTL-SDR support not enabled in this build.\n");
    *src = 1;
#endif
}

static void
interactive_configure_rtltcp_input(dsd_opts* opts) {
    char host[128];
    prompt_string("rtl_tcp host", "127.0.0.1", host, sizeof host);
    int port = prompt_int("rtl_tcp port", 1234, 1, 65535);
    char freq[64];
    prompt_string("Center frequency in Hz (K/M/G suffix ok, optional — Enter to skip)", "", freq, sizeof freq);
    if (freq[0] == '\0') {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d", host, port);
        return;
    }
    int gain = prompt_int("RTL gain (dB)", 22, 0, 60);
    int ppm = prompt_int("PPM error", 0, -200, 200);
    int bw = prompt_int("DSP bandwidth (kHz: 4,6,8,12,16,24,48)", 48, 4, 48);
    int sql = prompt_int("Squelch (0=off)", 0, -1000, 100000);
    int vol = prompt_int("Monitor gain multiplier (1..3)", 1, 1, 3);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d:%s:%d:%d:%d:%d:%d", host, port, freq,
                 gain, ppm, bw, sql, vol);
}

static void
interactive_configure_file_input(dsd_opts* opts, dsd_state* state, int* src) {
    char path[1024];
    prompt_string("Path to WAV/BIN/RAW/SYM file", "", path, sizeof path);
    if (path[0] == '\0') {
        LOG_WARN("WARNING: No file provided; falling back to PulseAudio input.\n");
        *src = 1;
        return;
    }
    int sr = prompt_int("Sample rate for WAV/RAW (48000 or 96000)", 48000, 8000, 192000);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", path);
    {
        int old_effective_rate = dsd_opts_effective_input_rate(opts);
        dsd_opts_apply_input_sample_rate(opts, sr);
        dsd_state_rescale_symbol_timing(state, old_effective_rate, dsd_opts_effective_input_rate(opts));
    }
}

static void
interactive_configure_tcp_input(dsd_opts* opts) {
    char host[128];
    prompt_string("TCP host", "127.0.0.1", host, sizeof host);
    int port = prompt_int("TCP port", 7355, 1, 65535);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "tcp:%s:%d", host, port);
}

static void
interactive_configure_udp_input(dsd_opts* opts) {
    char addr[64];
    prompt_string("UDP bind address", "127.0.0.1", addr, sizeof addr);
    int port = prompt_int("UDP port", 7355, 1, 65535);
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "udp:%s:%d", addr, port);
}

static void
interactive_configure_input_source(dsd_opts* opts, dsd_state* state, int* src) {
    switch (*src) {
        case 2: interactive_configure_rtl_input(opts, src); break;
        case 3: interactive_configure_rtltcp_input(opts); break;
        case 4: interactive_configure_file_input(opts, state, src); break;
        case 5: interactive_configure_tcp_input(opts); break;
        case 6: interactive_configure_udp_input(opts); break;
        case 1:
        default: break;
    }
}

static int
interactive_choose_decode_mode(void) {
    DSD_FPRINTF(stderr, "\nWhat do you want to decode?\n");
    DSD_FPRINTF(stderr, "  1) Auto (P25, YSF, D-STAR, X2-TDMA, DMR) [default]\n");
    DSD_FPRINTF(stderr, "  2) P25 Phase 1 only\n");
    DSD_FPRINTF(stderr, "  3) P25 Phase 2 only\n");
    DSD_FPRINTF(stderr, "  4) DMR\n");
    DSD_FPRINTF(stderr, "  5) NXDN48 (6.25 kHz)\n");
    DSD_FPRINTF(stderr, "  6) NXDN96 (12.5 kHz)\n");
    DSD_FPRINTF(stderr, "  7) X2-TDMA\n");
    DSD_FPRINTF(stderr, "  8) YSF\n");
    DSD_FPRINTF(stderr, "  9) D-STAR\n");
    DSD_FPRINTF(stderr, " 10) EDACS/ProVoice (std/net)\n");
    DSD_FPRINTF(stderr, " 11) dPMR\n");
    DSD_FPRINTF(stderr, " 12) M17\n");
    DSD_FPRINTF(stderr, " 13) P25 + DMR (TDMA)\n");
    DSD_FPRINTF(stderr, " 14) Analog monitor (passive)\n");
    return prompt_int("Selection", 1, 1, 14);
}

static dsdneoUserDecodeMode
interactive_mode_to_decode_mode(int mode) {
    switch (mode) {
        case 1: return DSDCFG_MODE_AUTO;
        case 2: return DSDCFG_MODE_P25P1;
        case 3: return DSDCFG_MODE_P25P2;
        case 4: return DSDCFG_MODE_DMR;
        case 5: return DSDCFG_MODE_NXDN48;
        case 6: return DSDCFG_MODE_NXDN96;
        case 7: return DSDCFG_MODE_X2TDMA;
        case 8: return DSDCFG_MODE_YSF;
        case 9: return DSDCFG_MODE_DSTAR;
        case 10: return DSDCFG_MODE_EDACS_PV;
        case 11: return DSDCFG_MODE_DPMR;
        case 12: return DSDCFG_MODE_M17;
        case 13: return DSDCFG_MODE_TDMA;
        case 14: return DSDCFG_MODE_ANALOG;
        default: return DSDCFG_MODE_UNSET;
    }
}

static int
interactive_mode_supports_trunk(int mode) {
    switch (mode) {
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 10:
        case 13: return 1;
        default: return 0;
    }
}

static void
interactive_maybe_import_channel_map(dsd_opts* opts, dsd_state* state) {
    char cpath[1024];
    prompt_string("Channel map CSV path (optional)", "", cpath, sizeof cpath);
    if (cpath[0] == '\0') {
        return;
    }
    if (!path_is_regular_file(cpath)) {
        LOG_WARN("WARNING: Channel map file not found: %s — skipping import.\n", cpath);
        return;
    }
    DSD_STRNCPY(opts->chan_in_file, cpath, sizeof opts->chan_in_file - 1);
    opts->chan_in_file[sizeof opts->chan_in_file - 1] = '\0';
    if (csvChanImport(opts, state) == 0) {
        LOG_INFO("NOTICE: Imported channel map from %s\n", opts->chan_in_file);
    } else {
        LOG_WARN("WARNING: Failed to import channel map from %s\n", opts->chan_in_file);
    }
}

static void
interactive_maybe_import_group_list(dsd_opts* opts, dsd_state* state) {
    char gpath[1024];
    prompt_string("Group list CSV path (optional)", "", gpath, sizeof gpath);
    if (gpath[0] == '\0') {
        return;
    }
    if (!path_is_regular_file(gpath)) {
        LOG_WARN("WARNING: Group list file not found: %s — skipping import.\n", gpath);
        return;
    }
    DSD_STRNCPY(opts->group_in_file, gpath, sizeof opts->group_in_file - 1);
    opts->group_in_file[sizeof opts->group_in_file - 1] = '\0';
    if (csvGroupImport(opts, state) == 0) {
        LOG_INFO("NOTICE: Imported group list from %s\n", opts->group_in_file);
    } else {
        LOG_WARN("WARNING: Failed to import group list from %s\n", opts->group_in_file);
    }
    if (prompt_yes_no("Use group list as allow/white list?", 0)) {
        opts->trunk_use_allow_list = 1;
        LOG_INFO("NOTICE: Allow/white list: Enabled.\n");
    }
}

static void
interactive_maybe_configure_trunking(int mode, int src, dsd_opts* opts, dsd_state* state) {
    if (!interactive_mode_supports_trunk(mode) || (src != 2 && src != 3 && src != 5)) {
        return;
    }
    if (!prompt_yes_no("Is this a trunked system?", 0)) {
        return;
    }
    opts->p25_trunk = 1;
    opts->trunk_enable = 1;
    if (src == 5) {
        if (opts->rigctlportno == 0) {
            opts->rigctlportno = 4532;
        }
        opts->use_rigctl = 1;
    }
    LOG_INFO("NOTICE: Trunking: Enabled.\n");
    interactive_maybe_import_channel_map(opts, state);
    interactive_maybe_import_group_list(opts, state);
}

static void
interactive_maybe_configure_output(dsd_opts* opts, int src) {
    if (src == 1) {
        return;
    }
    if (prompt_yes_no("Use PulseAudio for output?", 1)) {
        dsd_bootstrap_choose_audio_output(opts);
        return;
    }
    if (prompt_yes_no("Mute audio output (null sink)?", 0)) {
        DSD_SNPRINTF(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
    }
}

static void
interactive_maybe_enable_terminal_frontend(dsd_opts* opts) {
#if DSD_RUNTIME_HAS_TERMINAL_UI
    int want_terminal = prompt_yes_no("Enable terminal frontend (--frontend terminal)?", 1);
    if (want_terminal) {
        opts->frontend_kind = DSD_FRONTEND_TERMINAL;
    }
#else
    if (opts) {
        opts->frontend_kind = DSD_FRONTEND_NONE;
    }
#endif
}

void
dsd_bootstrap_interactive(dsd_opts* opts, dsd_state* state) {
    if (!dsd_isatty(DSD_STDIN_FILENO) || !dsd_isatty(DSD_STDOUT_FILENO)) {
        // Non-interactive environment: keep defaults
        return;
    }

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    if (!cfg) {
        dsd_neo_config_init(opts);
        cfg = dsd_neo_get_config();
    }
    if (cfg && cfg->no_bootstrap_enable) {
        return;
    }

    LOG_INFO("NOTICE: No CLI arguments detected — starting interactive setup.\n");
    LOG_INFO("NOTICE: Press Enter to accept defaults in [brackets].\n");

    int src = interactive_choose_input_source();
    interactive_configure_input_source(opts, state, &src);

    if (src == 1) {
        LOG_INFO("NOTICE: PulseAudio selected; choose devices.\n");
        dsd_bootstrap_choose_audio_input(opts);
        dsd_bootstrap_choose_audio_output(opts);
    }

    int mode = interactive_choose_decode_mode();
    dsdneoUserDecodeMode decode_mode = interactive_mode_to_decode_mode(mode);
    if (decode_mode != DSDCFG_MODE_UNSET) {
        (void)dsd_apply_decode_mode_preset(decode_mode, DSD_DECODE_PRESET_PROFILE_INTERACTIVE, opts, state);
    }

    interactive_maybe_configure_trunking(mode, src, opts, state);
    interactive_maybe_configure_output(opts, src);
    interactive_maybe_enable_terminal_frontend(opts);

    LOG_INFO("NOTICE: Interactive setup complete.\n");
}
