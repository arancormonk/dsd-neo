// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*
 * Copyright (C) 2010 DSD Author
 * GPG Key ID: 0x3F1D7FD0 (74EF 430D F7F2 0A48 FCE6  F630 FAA2 635D 3F1D 7FD0)
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#define _MAIN

#include <dsd-neo/core/dsd.h>
#include <dsd-neo/dsp/dmr_sync.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr_const.h>
#include <dsd-neo/protocol/dstar/dstar_const.h>
#include <dsd-neo/protocol/nxdn/nxdn_const.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25p1_const.h>
#include <dsd-neo/protocol/provoice/provoice_const.h>
#include <dsd-neo/protocol/x2tdma/x2tdma_const.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/git_ver.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/ui/ui_async.h>

#include <ctype.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>

#include <dsd-neo/io/udp_input.h>
#if defined(__SSE__) || defined(__SSE2__)
#include <xmmintrin.h>
#endif

#ifdef USE_RTLSDR
#include <dsd-neo/io/rtl_stream_c.h>
#include <rtl-sdr.h>
#endif

/* exitflag is defined in src/runtime/exitflag.c and declared in dsd.h */

// Local caches to avoid redundant device I/O in hot paths
static long int s_last_rigctl_freq = -1;
static int s_last_rigctl_bw = -12345;
#ifdef USE_RTLSDR
static uint32_t s_last_rtl_freq = 0;
#endif

// Config autosave state: when enabled, snapshot opts/state to this path.
static int s_user_config_save_enabled = 0;
static char s_user_config_save_path[1024];

// --- Bootstrap helpers ----------------------------------------------------
static int
is_truthy_env(const char* v) {
    if (!v || !*v) {
        return 0;
    }

    /* no argv processing here */
    if (v[0] == '1') {
        return 1;
    }
    if (v[0] == 'y' || v[0] == 'Y') {
        return 1;
    }
    if (v[0] == 't' || v[0] == 'T') {
        return 1;
    }
    return 0;
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
    fprintf(stderr, "%s [%c/%c]: ", q, def_yes ? 'Y' : 'y', def_yes ? 'n' : 'N');
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

static void
autosave_user_config(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (!s_user_config_save_enabled) {
        return;
    }

    const char* path = NULL;
    if (s_user_config_save_path[0] != '\0') {
        path = s_user_config_save_path;
    } else {
        path = dsd_user_config_default_path();
        if (!path || !*path) {
            return;
        }
    }

    dsdneoUserConfig cfg;
    dsd_snapshot_opts_to_user_config(opts, state, &cfg);
    if (dsd_user_config_save_atomic(path, &cfg) == 0) {
        LOG_DEBUG("Autosaved configuration to %s\n", path);
    } else {
        LOG_WARNING("Failed to save configuration to %s\n", path);
    }
}

static int
analog_filter_rate_hz(const dsd_opts* opts, const dsd_state* state) {
    if (!opts) {
        return 48000;
    }
#ifdef USE_RTLSDR
    if (opts->audio_in_type == AUDIO_IN_RTL && state && state->rtl_ctx) {
        uint32_t Fs = rtl_stream_output_rate(state->rtl_ctx);
        if (Fs > 0) {
            return (int)Fs;
        }
    }
#endif
    switch (opts->audio_in_type) {
        case AUDIO_IN_PULSE:
            if (opts->pulse_digi_rate_in > 0) {
                return opts->pulse_digi_rate_in;
            }
            break;
        case AUDIO_IN_STDIN:
        case AUDIO_IN_WAV:
        case AUDIO_IN_UDP:
        case AUDIO_IN_TCP:
            if (opts->wav_sample_rate > 0) {
                return opts->wav_sample_rate;
            }
            break;
        default: break;
    }
    if (opts->pulse_raw_rate_out > 0) {
        return opts->pulse_raw_rate_out;
    }
    return 48000;
}

static int
prompt_int(const char* q, int def_val, int min_val, int max_val) {
    char buf[64];
    fprintf(stderr, "%s [%d]: ", q, def_val);
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
    fprintf(stderr, "%s [%s]: ", q, (def_val && *def_val) ? def_val : "");
    if (!fgets(buf, sizeof buf, stdin)) {
        if (def_val) {
            snprintf(out, out_sz, "%s", def_val);
        } else {
            out[0] = '\0';
        }
        return;
    }
    trim_newline(buf);
    if (buf[0] == '\0') {
        if (def_val) {
            snprintf(out, out_sz, "%s", def_val);
        } else {
            out[0] = '\0';
        }
        return;
    }
    snprintf(out, out_sz, "%s", buf);
}

/* Optional: enable Flush-To-Zero and Denormals-Are-Zero on x86 SSE to avoid
   potential denormal slowdowns in float-heavy DSP (guarded by env flag). */
// Removed legacy local bootstrap helpers; runtime/bootstrap now provides these

static void
bootstrap_interactive(dsd_opts* opts, dsd_state* state) {
    if (!dsd_isatty(DSD_STDIN_FILENO) || !dsd_isatty(DSD_STDOUT_FILENO)) {
        // Non-interactive environment: keep defaults
        return;
    }

    const char* skip_env = getenv("DSD_NEO_NO_BOOTSTRAP");
    if (is_truthy_env(skip_env)) {
        return;
    }

    LOG_NOTICE("No CLI arguments detected — starting interactive setup.\n");
    LOG_NOTICE("Press Enter to accept defaults in [brackets].\n");

    fprintf(stderr, "\nChoose input source:\n");
    fprintf(stderr, "  1) PulseAudio (mic/loopback) [default]\n");
    fprintf(stderr, "  2) RTL-SDR USB dongle\n");
    fprintf(stderr, "  3) rtl_tcp (network RTL-SDR)\n");
    fprintf(stderr, "  4) File (WAV/BIN)\n");
    fprintf(stderr, "  5) TCP audio (7355)\n");
    fprintf(stderr, "  6) UDP audio (7355)\n");
    int src = prompt_int("Selection", 1, 1, 6);

    switch (src) {
        case 2: {
#ifdef USE_RTLSDR
            // RTL-SDR path: rtl:dev:freq:gain:ppm:bw:sql:vol
            char freq[64];
            prompt_string("Center frequency in Hz (K/M/G suffix ok, e.g., 851.375M or 851375000)", "", freq,
                          sizeof freq);
            if (freq[0] == '\0') {
                LOG_WARNING("No frequency entered; falling back to PulseAudio input.\n");
                src = 1;
                break;
            }
            int dev = prompt_int("RTL device index", 0, 0, 255);
            int gain = prompt_int("RTL gain (dB)", 22, 0, 60);
            int ppm = prompt_int("PPM error", 0, -200, 200);
            int bw = prompt_int("DSP bandwidth (kHz: 4,6,8,12,16,24,48)", 48, 4, 48);
            int sql = prompt_int("Squelch (0=off; negative dB ok via CLI later)", 0, -1000, 100000);
            int vol = prompt_int("Volume multiplier (1..3)", 1, 1, 3);
            snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtl:%d:%s:%d:%d:%d:%d:%d", dev, freq, gain, ppm,
                     bw, sql, vol);
            break;
#else
            LOG_WARNING("RTL-SDR support not enabled in this build.\n");
            src = 1; // fall back
            break;
#endif
        }
        case 3: {
            // rtl_tcp: rtltcp[:host:port[:freq:gain:ppm:bw:sql:vol]]
            char host[128];
            prompt_string("rtl_tcp host", "127.0.0.1", host, sizeof host);
            int port = prompt_int("rtl_tcp port", 1234, 1, 65535);
            char freq[64];
            prompt_string("Center frequency in Hz (K/M/G suffix ok, optional — Enter to skip)", "", freq, sizeof freq);
            if (freq[0] == '\0') {
                snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d", host, port);
            } else {
                int gain = prompt_int("RTL gain (dB)", 22, 0, 60);
                int ppm = prompt_int("PPM error", 0, -200, 200);
                int bw = prompt_int("DSP bandwidth (kHz: 4,6,8,12,16,24,48)", 48, 4, 48);
                int sql = prompt_int("Squelch (0=off)", 0, -1000, 100000);
                int vol = prompt_int("Volume multiplier (1..3)", 1, 1, 3);
                snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtltcp:%s:%d:%s:%d:%d:%d:%d:%d", host, port,
                         freq, gain, ppm, bw, sql, vol);
            }
            break;
        }
        case 4: {
            // File input
            char path[1024];
            prompt_string("Path to WAV/BIN/RAW/SYM file", "", path, sizeof path);
            if (path[0] == '\0') {
                LOG_WARNING("No file provided; falling back to PulseAudio input.\n");
                src = 1;
                break;
            }
            // Optional sample rate tweak for WAV/RAW
            int sr = prompt_int("Sample rate for WAV/RAW (48000 or 96000)", 48000, 8000, 192000);
            snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", path);
            if (sr != 48000) {
                opts->wav_sample_rate = sr;
                opts->wav_interpolator = opts->wav_sample_rate / opts->wav_decimator;
                state->samplesPerSymbol = state->samplesPerSymbol * opts->wav_interpolator;
                state->symbolCenter = state->symbolCenter * opts->wav_interpolator;
            }
            break;
        }
        case 5: {
            char host[128];
            prompt_string("TCP host", "127.0.0.1", host, sizeof host);
            int port = prompt_int("TCP port", 7355, 1, 65535);
            snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "tcp:%s:%d", host, port);
            break;
        }
        case 6: {
            char addr[64];
            prompt_string("UDP bind address", "127.0.0.1", addr, sizeof addr);
            int port = prompt_int("UDP port", 7355, 1, 65535);
            snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "udp:%s:%d", addr, port);
            break;
        }
        case 1:
        default: break;
    }

    if (src == 1) {
        LOG_NOTICE("PulseAudio selected; choose devices.\n");
        dsd_bootstrap_choose_audio_input(opts);
        dsd_bootstrap_choose_audio_output(opts);
    }

    // Choose what to decode (default: Auto)
    fprintf(stderr, "\nWhat do you want to decode?\n");
    fprintf(stderr, "  1) Auto (P25, YSF, D-STAR, X2-TDMA, DMR) [default]\n");
    fprintf(stderr, "  2) P25 Phase 1 only\n");
    fprintf(stderr, "  3) P25 Phase 2 only\n");
    fprintf(stderr, "  4) DMR\n");
    fprintf(stderr, "  5) NXDN48 (6.25 kHz)\n");
    fprintf(stderr, "  6) NXDN96 (12.5 kHz)\n");
    fprintf(stderr, "  7) X2-TDMA\n");
    fprintf(stderr, "  8) YSF\n");
    fprintf(stderr, "  9) D-STAR\n");
    fprintf(stderr, " 10) EDACS/ProVoice (std/net)\n");
    fprintf(stderr, " 11) dPMR\n");
    fprintf(stderr, " 12) M17\n");
    fprintf(stderr, " 13) P25 + DMR (TDMA)\n");
    fprintf(stderr, " 14) Analog monitor (passive)\n");
    int mode = prompt_int("Selection", 1, 1, 14);

    // Apply decode mode selection
    switch (mode) {
        case 1: /* Auto: keep init defaults */
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "AUTO");
            break;
        case 2: /* P25 Phase 1 only (-f1) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 1;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->ssize = 36;
            opts->msize = 15;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "P25p1");
            break;
        case 3: /* P25 Phase 2 only (-f2) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 1;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 8;
            state->symbolCenter = 3;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 1;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "P25p2");
            break;
        case 4: /* DMR (-fs) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->inverted_p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 1;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 1;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 2;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "DMR");
            break;
        case 5: /* NXDN48 (-fi) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 1;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 20;
            state->symbolCenter = 9; /* (sps-1)/2 */
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "NXDN48");
            break;
        case 6: /* NXDN96 (-fn) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 1;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "NXDN96");
            break;
        case 7: /* X2-TDMA (-fx) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 1;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->rf_mod = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "X2-TDMA");
            break;
        case 8: /* YSF (-fy) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 1;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "YSF");
            break;
        case 9: /* D-STAR (-fd) */
            opts->frame_dstar = 1;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->rf_mod = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "DSTAR");
            break;
        case 10: /* EDACS/ProVoice (std/net) (-fh) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 1;
            state->ea_mode = 0;
            state->esk_mask = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 5;
            state->symbolCenter = 2;
            opts->mod_c4fm = 0;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 1;
            state->rf_mod = 2;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "EDACS/PV");
            break;
        case 11: /* dPMR (-fm) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_provoice = 0;
            opts->frame_dpmr = 1;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            state->samplesPerSymbol = 20; // same as NXDN48
            state->symbolCenter = 9;      // (sps-1)/2, same as NXDN48
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "dPMR");
            break;
        case 12: /* M17 (-fz) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_provoice = 0;
            opts->frame_dpmr = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 1;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->dmr_stereo = 0;
            opts->use_cosine_filter = 0; // per -fz note
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "M17");
            break;
        case 13: /* P25 + DMR (TDMA) (-ft) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 1;
            opts->frame_p25p2 = 1;
            opts->inverted_p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 1;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->mod_c4fm = 1;
            opts->mod_qpsk = 0;
            opts->mod_gfsk = 0;
            state->rf_mod = 0;
            opts->dmr_stereo = 1;
            opts->dmr_mono = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 2;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "TDMA");
            break;
        case 14: /* Analog monitor (-fA) */
            opts->frame_dstar = 0;
            opts->frame_x2tdma = 0;
            opts->frame_p25p1 = 0;
            opts->frame_p25p2 = 0;
            opts->frame_nxdn48 = 0;
            opts->frame_nxdn96 = 0;
            opts->frame_dmr = 0;
            opts->frame_dpmr = 0;
            opts->frame_provoice = 0;
            opts->frame_ysf = 0;
            opts->frame_m17 = 0;
            opts->pulse_digi_rate_out = 8000;
            opts->pulse_digi_out_channels = 1;
            opts->dmr_stereo = 0;
            state->dmr_stereo = 0;
            opts->dmr_mono = 0;
            state->rf_mod = 0;
            opts->monitor_input_audio = 1;
            opts->analog_only = 1;
            snprintf(opts->output_name, sizeof opts->output_name, "%s", "Analog Monitor");
            break;
        default: break;
    }

    // Offer trunking toggle when applicable
    int trunk_supported = 0;
    switch (mode) {
        case 1:  // Auto
        case 2:  // P25p1
        case 3:  // P25p2
        case 4:  // DMR
        case 5:  // NXDN48
        case 6:  // NXDN96
        case 10: // EDACS/ProVoice
        case 13: // P25+DMR (TDMA)
            trunk_supported = 1;
            break;
        default: trunk_supported = 0; break;
    }
    if (trunk_supported && (src == 2 || src == 3 || src == 5)) {
        int want_trunk = prompt_yes_no("Is this a trunked system?", 0);
        if (want_trunk) {
            opts->p25_trunk = 1;
            opts->trunk_enable = 1;
            // For TCP audio source, enable rigctl on default SDR++ port to allow tuning
            if (src == 5) {
                if (opts->rigctlportno == 0) {
                    opts->rigctlportno = 4532; // SDR++ default
                }
                opts->use_rigctl = 1;
            }
            LOG_NOTICE("Trunking: Enabled.\n");

            // Optional trunking CSV imports
            // Channel map CSV (channum,freq) — usually required for DMR/EDACS/NXDN Type-C; P25 often learns
            char cpath[1024];
            prompt_string("Channel map CSV path (optional)", "", cpath, sizeof cpath);
            if (cpath[0] != '\0') {
                // Verify file exists before attempting import
                struct stat st;
                if (stat(cpath, &st) == 0 && S_ISREG(st.st_mode)) {
                    strncpy(opts->chan_in_file, cpath, sizeof opts->chan_in_file - 1);
                    opts->chan_in_file[sizeof opts->chan_in_file - 1] = '\0';
                    csvChanImport(opts, state);
                    LOG_NOTICE("Imported channel map from %s\n", opts->chan_in_file);
                } else {
                    LOG_WARNING("Channel map file not found: %s — skipping import.\n", cpath);
                }
            }

            // Group list CSV (TG,Mode,Name)
            char gpath[1024];
            prompt_string("Group list CSV path (optional)", "", gpath, sizeof gpath);
            if (gpath[0] != '\0') {
                struct stat stg;
                if (stat(gpath, &stg) == 0 && S_ISREG(stg.st_mode)) {
                    strncpy(opts->group_in_file, gpath, sizeof opts->group_in_file - 1);
                    opts->group_in_file[sizeof opts->group_in_file - 1] = '\0';
                    csvGroupImport(opts, state);
                    LOG_NOTICE("Imported group list from %s\n", opts->group_in_file);
                    // Optional allow-list toggle
                    int use_allow = prompt_yes_no("Use group list as allow/white list?", 0);
                    if (use_allow) {
                        opts->trunk_use_allow_list = 1;
                        LOG_NOTICE("Allow/white list: Enabled.\n");
                    }
                } else {
                    LOG_WARNING("Group list file not found: %s — skipping import.\n", gpath);
                }
            }
        }
    }

    // Output sink quick choice when not using Pulse input helper
    if (src != 1) {
        int use_pulse_out = prompt_yes_no("Use PulseAudio for output?", 1);
        if (use_pulse_out) {
            // Only pick output sink; do not touch input previously chosen
            dsd_bootstrap_choose_audio_output(opts);
        } else {
            int mute = prompt_yes_no("Mute audio output (null sink)?", 0);
            if (mute) {
                snprintf(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "null");
            }
        }
    }

    int want_ncurses = prompt_yes_no("Enable ncurses terminal UI (-N)?", 1);
    if (want_ncurses) {
        opts->use_ncurses_terminal = 1;
    }

    LOG_NOTICE("Interactive setup complete.\n");
}

void
handler(int sgnl) {
    UNUSED(sgnl);

    exitflag = 1;
}

int
pretty_colors() {
    fprintf(stderr, "%sred\n", KRED);
    fprintf(stderr, "%sgreen\n", KGRN);
    fprintf(stderr, "%syellow\n", KYEL);
    fprintf(stderr, "%sblue\n", KBLU);
    fprintf(stderr, "%smagenta\n", KMAG);
    fprintf(stderr, "%scyan\n", KCYN);
    fprintf(stderr, "%swhite\n", KWHT);
    fprintf(stderr, "%snormal\n", KNRM);

    return 0;
}

#include <dsd-neo/io/pa_devs.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25p1_heuristics.h>

// Small helpers to efficiently set fixed-width strings
static inline void
set_spaces(char* buf, size_t count) {
    memset(buf, ' ', count);
    buf[count] = '\0';
}

static inline void
set_underscores(char* buf, size_t count) {
    memset(buf, '_', count);
    buf[count] = '\0';
}

//struct for checking existence of directory to write to
// Removed legacy globals tied to old CLI parsing (-Q/-P etc.)

void
noCarrier(dsd_opts* opts, dsd_state* state) {
    const time_t now = time(NULL);

    //when no carrier sync, rotate the symbol out file every hour, if enabled
    if (opts->symbol_out_f && opts->symbol_out_file_is_auto == 1) {
        rotate_symbol_out_file(opts, state);
    }

    if (opts->floating_point == 1) {
        state->aout_gain = opts->audio_gain;
        state->aout_gainR = opts->audio_gain;
    }

    //clear heuristics from last carrier signal
    if (opts->frame_p25p1 == 1 && opts->use_heuristics == 1) {
        initialize_p25_heuristics(&state->p25_heuristics);
        initialize_p25_heuristics(&state->inv_p25_heuristics);
    }

//only do it here on the tweaks
#ifdef LIMAZULUTWEAKS
    state->nxdn_last_ran = -1;
    state->nxdn_last_rid = 0;
    state->nxdn_last_tg = 0;
#endif

    //experimental conventional frequency scanner mode
    if (opts->scanner_mode == 1 && ((now - state->last_cc_sync_time) > opts->trunk_hangtime)) {

        //always do these -- makes sense during scanning
        state->nxdn_last_ran = -1; //
        state->nxdn_last_rid = 0;
        state->nxdn_last_tg = 0;

        if (state->lcn_freq_roll >= state->lcn_freq_count) {
            state->lcn_freq_roll = 0; //reset to zero
        }
        //check that we have a non zero value first, then tune next frequency
        if (state->trunk_lcn_freq[state->lcn_freq_roll] != 0) {
            //rigctl
            if (opts->use_rigctl == 1) {
                if (opts->setmod_bw != 0 && opts->setmod_bw != s_last_rigctl_bw) {
                    SetModulation(opts->rigctl_sockfd, opts->setmod_bw);
                    s_last_rigctl_bw = opts->setmod_bw;
                }
                long int f = state->trunk_lcn_freq[state->lcn_freq_roll];
                if (f != s_last_rigctl_freq) {
                    SetFreq(opts->rigctl_sockfd, f);
                    s_last_rigctl_freq = f;
                }
            }
            //rtl
            if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RTLSDR
                if (state->rtl_ctx) {
                    uint32_t rf = (uint32_t)state->trunk_lcn_freq[state->lcn_freq_roll];
                    if (rf != s_last_rtl_freq) {
                        rtl_stream_tune(state->rtl_ctx, rf);
                        s_last_rtl_freq = rf;
                    }
                }
#endif
            }
        }
        state->lcn_freq_roll++;
        state->last_cc_sync_time = now;
    }
    //end experimental conventional frequency scanner mode

    // Tune back to last known CC when using trunking after hangtime expires.
    // Use VC activity when currently tuned to a VC; otherwise use CC timer.
    if (opts->p25_trunk == 1 && (opts->trunk_is_tuned == 1 || opts->p25_is_tuned == 1)) {
        double dt;
        if (opts->p25_is_tuned == 1) {
            // On a voice channel: gate return by recent voice activity
            if (state->last_vc_sync_time == 0) {
                dt = 1e9; // no activity recorded; treat as expired
            } else {
                dt = (double)(now - state->last_vc_sync_time);
            }
        } else {
            // On control or idle: use CC timer
            if (state->last_cc_sync_time == 0) {
                dt = 1e9;
            } else {
                dt = (double)(now - state->last_cc_sync_time);
            }
        }

        if (dt > opts->trunk_hangtime) {
            long cc = (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
            if (cc != 0) {

                //cap+ rest channel - redundant?
                if (state->dmr_rest_channel != -1) {
                    if (state->trunk_chan_map[state->dmr_rest_channel] != 0) {
                        cc = state->trunk_chan_map[state->dmr_rest_channel];
                        state->p25_cc_freq = cc;
                        state->trunk_cc_freq = cc;
                    }
                }

                if (opts->use_rigctl == 1) //rigctl tuning
                {
                    if (opts->setmod_bw != 0 && opts->setmod_bw != s_last_rigctl_bw) {
                        SetModulation(opts->rigctl_sockfd, opts->setmod_bw);
                        s_last_rigctl_bw = opts->setmod_bw;
                    }
                    if (cc != s_last_rigctl_freq) {
                        SetFreq(opts->rigctl_sockfd, cc);
                        s_last_rigctl_freq = cc;
                    }
                    state->dmr_rest_channel = -1; //maybe?
                }
                //rtl
                else if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RTLSDR
                    if (state->rtl_ctx) {
                        uint32_t rf = (uint32_t)cc;
                        if (rf != s_last_rtl_freq) {
                            rtl_stream_tune(state->rtl_ctx, rf);
                            s_last_rtl_freq = rf;
                        }
                    }
                    state->dmr_rest_channel = -1;
#endif
                }

                opts->p25_is_tuned = 0;
                state->edacs_tuned_lcn = -1;

                state->last_cc_sync_time = now;
                //test to switch back to 10/4 P1 QPSK for P25 FDMA CC

                //if P25p2 VCH and going back to P25p1 CC, flip symbolrate
                if (state->p25_cc_is_tdma == 0) //is set on signal from P25 TSBK or MAC_SIGNAL
                {
                    state->samplesPerSymbol = 10;
                    state->symbolCenter = 4;
                    //re-enable both slots
                    opts->slot1_on = 1;
                    opts->slot2_on = 1;
                }
                //if P25p1 SNDCP channel (or revert) and going to a P25 TDMA CC
                else if (state->p25_cc_is_tdma == 1) {
                    state->samplesPerSymbol = 8;
                    state->symbolCenter = 3;
                    //re-enable both slots (in case of late entry voice, MAC_SIGNAL can turn them back off)
                    opts->slot1_on = 1;
                    opts->slot2_on = 1;
                }
            }
            //zero out vc frequencies?
            state->p25_vc_freq[0] = 0;
            state->p25_vc_freq[1] = 0;

            memset(state->active_channel, 0, sizeof(state->active_channel));

            state->is_con_plus = 0; //flag off
        }
    }

    state->dibit_buf_p = state->dibit_buf + 200;
    memset(state->dibit_buf, 0, sizeof(int) * 200);
    //dmr buffer
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    memset(state->dmr_payload_buf, 0, sizeof(int) * 200);
    memset(state->dmr_stereo_payload, 1, sizeof(int) * 144);
    if (state->dmr_reliab_buf) {
        state->dmr_reliab_p = state->dmr_reliab_buf + 200;
        memset(state->dmr_reliab_buf, 0, 200 * sizeof(uint8_t));
    }
    //dmr buffer end

    //close MBE out files
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }

    state->jitter = -1;
    state->lastsynctype = -1;
    state->carrier = 0;
    state->max = 15000;
    state->min = -15000;
    state->center = 0;
    state->m17_polarity = 0; /* Reset M17 polarity so next transmission can auto-detect fresh */
    state->err_str[0] = '\0';
    state->err_strR[0] = '\0';
    set_spaces(state->fsubtype, 14);
    set_spaces(state->ftype, 13);
    state->errs = 0;
    state->errs2 = 0;

    //zero out right away if not trunking
    if (opts->p25_trunk == 0) {
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lasttgR = 0;
        state->lastsrcR = 0;
        state->gi[0] = -1;
        state->gi[1] = -1;

        //zero out vc frequencies?
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;

        //only reset cap+ rest channel if not trunking
        state->dmr_rest_channel = -1;

        //DMR Color Code
        //  state->dmr_color_code = 16; //disabled

        //zero out nxdn site/srv/cch info if not trunking
        state->nxdn_location_site_code = 0;
        state->nxdn_location_sys_code = 0;
        set_spaces(state->nxdn_location_category, 1);

        //channel access information
        state->nxdn_rcn = 0;
        state->nxdn_base_freq = 0;
        state->nxdn_step = 0;
        state->nxdn_bw = 0;

        //dmr mfid branding and site parms
        state->dmr_branding_sub[0] = '\0';
        state->dmr_branding[0] = '\0';
        state->dmr_site_parms[0] = '\0';
    }

    //The new event history should not require this, but revert if other random issues suddenly come up
    //this was mainly for preventling numbers blipping out on signal fade, but also leaves stale values
    //on occassion when carrier drops and return to control channel, doesn't close wav files in that instance
    //  if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1 && time(NULL) - state->last_cc_sync_time > opts->trunk_hangtime)
    {
        state->lasttg = 0;
        state->lastsrc = 0;
        state->lasttgR = 0;
        state->lastsrcR = 0;
        state->gi[0] = -1;
        state->gi[1] = -1;
        //  state->nxdn_last_ran = -1; //
        state->nxdn_last_rid = 0;
        state->nxdn_last_tg = 0;
    }

    state->lastp25type = 0;
    state->repeat = 0;
    state->nac = 0;
    state->numtdulc = 0;
    state->slot1light[0] = '\0';
    state->slot2light[0] = '\0';
    state->firstframe = 0;
    memset(state->aout_max_buf, 0, sizeof(float) * 200);
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;

    memset(state->aout_max_bufR, 0, sizeof(float) * 200);
    state->aout_max_buf_pR = state->aout_max_bufR;
    state->aout_max_buf_idxR = 0;

    set_underscores(state->algid, 8);
    set_underscores(state->keyid, 16);
    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    mbe_initMbeParms(state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2);

    state->dmr_ms_mode = 0;

    //not sure if desirable here or not just yet, may need to disable a few of these
    state->payload_mi = 0;
    state->payload_miR = 0;
    state->payload_mfid = 0;
    state->payload_mfidR = 0;
    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;

    state->HYTL = 0;
    state->HYTR = 0;
    state->DMRvcL = 0;
    state->DMRvcR = 0;
    state->dropL = 256;
    state->dropR = 256;

    state->payload_miN = 0;
    state->p25vc = 0;
    state->payload_miP = 0;

    //ks array storage and counters
    memset(state->ks_octetL, 0, sizeof(state->ks_octetL));
    memset(state->ks_octetR, 0, sizeof(state->ks_octetR));
    memset(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    memset(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    state->octet_counter = 0;
    state->bit_counterL = 0;
    state->bit_counterR = 0;

    //xl specific, we need to know if the ESS is from HDU, or from LDU2
    state->xl_is_hdu = 0;

    //NXDN, when a new IV has arrived
    state->nxdn_new_iv = 0;

    //initialize dmr data header source
    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_source[1] = 0;
    state->dmr_lrrp_target[0] = 0;
    state->dmr_lrrp_target[1] = 0;

    //initialize data header bits
    state->data_header_blocks[0] = 1; //initialize with 1, otherwise we may end up segfaulting when no/bad data header
    state->data_header_blocks[1] = 1; //when trying to fill the superframe and 0-1 blocks give us an overflow
    state->data_header_padding[0] = 0;
    state->data_header_padding[1] = 0;
    state->data_header_format[0] = 7;
    state->data_header_format[1] = 7;
    state->data_header_sap[0] = 0;
    state->data_header_sap[1] = 0;
    state->data_block_counter[0] = 1;
    state->data_block_counter[1] = 1;
    state->data_p_head[0] = 0;
    state->data_p_head[1] = 0;
    state->data_block_poc[0] = 0;
    state->data_block_poc[1] = 0;
    state->data_byte_ctr[0] = 0;
    state->data_byte_ctr[1] = 0;
    state->data_ks_start[0] = 0;
    state->data_ks_start[1] = 0;

    state->dmr_encL = 0;
    state->dmr_encR = 0;

    state->dmrburstL = 17;
    state->dmrburstR = 17;

    //reset P2 ESS_B fragments and 4V counter
    for (short i = 0; i < 4; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b[1][i] = 0;
    }
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;

    //values displayed in ncurses terminal
    // state->p25_vc_freq[0] = 0;
    // state->p25_vc_freq[1] = 0;

    //new nxdn stuff
    state->nxdn_part_of_frame = 0;
    state->nxdn_ran = 0;
    state->nxdn_sf = 0;
    memset(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc)); //init on 1, bad CRC all
    state->nxdn_sacch_non_superframe = TRUE;
    memset(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    state->nxdn_alias_block_number = 0;
    memset(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));
    state->nxdn_call_type[0] = '\0';

    //unload keys when using keylaoder
    if (state->keyloader == 1) {
        state->R = 0;  //NXDN, or RC4 (slot 1)
        state->RR = 0; //RC4 (slot 2)
        state->K = 0;  //BP
        state->K1 = 0; //tera 10/32/64 char BP
        state->K2 = 0;
        state->K3 = 0;
        state->K4 = 0;
        memset(state->A1, 0, sizeof(state->A1));
        memset(state->A2, 0, sizeof(state->A2));
        memset(state->A3, 0, sizeof(state->A3));
        memset(state->A4, 0, sizeof(state->A4));
        memset(state->aes_key_loaded, 0, sizeof(state->aes_key_loaded));
        state->H = 0; //shim for above
    }

    //forcing key application will re-enable this at the time of voice tx
    state->nxdn_cipher_type = 0;

    //dmr overaching manufacturer in use for a particular system or radio
    // state->dmr_mfid = -1;

    //dmr slco stuff
    memset(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
    state->dmr_cach_counter = 0;

    //initialize unified dmr pdu 'superframe'
    memset(state->dmr_pdu_sf, 0, sizeof(state->dmr_pdu_sf));
    memset(state->data_header_valid, 0, sizeof(state->data_header_valid));

    //initialize cap+ bits and block num storage
    memset(state->cap_plus_csbk_bits, 0, sizeof(state->cap_plus_csbk_bits));
    memset(state->cap_plus_block_num, 0, sizeof(state->cap_plus_block_num));

    //init confirmed data individual block crc as invalid
    memset(state->data_block_crc_valid, 0, sizeof(state->data_block_crc_valid));

    //embedded signalling
    memset(state->dmr_embedded_signalling, 0, sizeof(state->dmr_embedded_signalling));

    //late entry mi fragments
    memset(state->late_entry_mi_fragment, 0, sizeof(state->late_entry_mi_fragment));

    //dmr talker alias new/fixed stuff
    memset(state->dmr_alias_format, 0, sizeof(state->dmr_alias_format));
    memset(state->dmr_alias_block_len, 0, sizeof(state->dmr_alias_block_len));
    memset(state->dmr_alias_char_size, 0, sizeof(state->dmr_alias_char_size));
    memset(state->dmr_alias_block_segment, 0, sizeof(state->dmr_alias_block_segment));
    memset(state->dmr_embedded_gps, 0, sizeof(state->dmr_embedded_gps));
    memset(state->dmr_lrrp_gps, 0, sizeof(state->dmr_lrrp_gps));

    //Generic Talker Alias String
    memset(state->generic_talker_alias, 0, sizeof(state->generic_talker_alias));
    state->generic_talker_alias_src[0] = 0;
    state->generic_talker_alias_src[1] = 0;

    /* Initialize P25 metrics counters used by ncurses BER display */
    state->p25_p1_fec_ok = 0;
    state->p25_p1_fec_err = 0;
    state->p25_p2_rs_facch_ok = 0;
    state->p25_p2_rs_facch_err = 0;
    state->p25_p2_rs_facch_corr = 0;
    state->p25_p2_rs_sacch_ok = 0;
    state->p25_p2_rs_sacch_err = 0;
    state->p25_p2_rs_sacch_corr = 0;
    state->p25_p2_rs_ess_ok = 0;
    state->p25_p2_rs_ess_err = 0;
    state->p25_p2_rs_ess_corr = 0;

    // Initialize P25 SM candidate cache bookkeeping
    state->p25_cc_cand_count = 0;
    state->p25_cc_cand_idx = 0;
    state->p25_cc_cache_loaded = 0;

    // memset(state->active_channel, 0, sizeof(state->active_channel));

    //REMUS! multi-purpose call_string
    set_spaces(state->call_string[0], 21);
    set_spaces(state->call_string[1], 21);

    if (now - state->last_cc_sync_time > 10) //ten seconds of no carrier
    {
        state->dmr_rest_channel = -1;
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
        state->dmr_mfid = -1;
        state->dmr_branding_sub[0] = '\0';
        state->dmr_branding[0] = '\0';
        state->dmr_site_parms[0] = '\0';
        opts->p25_is_tuned = 0;
        memset(state->active_channel, 0, sizeof(state->active_channel));
    }

    opts->dPMR_next_part_of_superframe = 0;

    state->dPMRVoiceFS2Frame.CalledIDOk = 0;
    state->dPMRVoiceFS2Frame.CallingIDOk = 0;
    memset(state->dPMRVoiceFS2Frame.CalledID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.CallingID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.Version, 0, 8);

    set_spaces(state->dpmr_caller_id, 6);
    set_spaces(state->dpmr_target_id, 6);

    //YSF Fusion Call Strings
    set_spaces(state->ysf_tgt, 10);
    set_spaces(state->ysf_src, 10);
    set_spaces(state->ysf_upl, 10);
    set_spaces(state->ysf_dnl, 10);
    set_spaces(state->ysf_rm1, 5);
    set_spaces(state->ysf_rm2, 5);
    set_spaces(state->ysf_rm3, 5);
    set_spaces(state->ysf_rm4, 5);
    memset(state->ysf_txt, 0, sizeof(state->ysf_txt));
    state->ysf_dt = 9;
    state->ysf_fi = 9;
    state->ysf_cm = 9;

    //DSTAR Call Strings
    set_spaces(state->dstar_rpt1, 8);
    set_spaces(state->dstar_rpt2, 8);
    set_spaces(state->dstar_dst, 8);
    set_spaces(state->dstar_src, 8);
    set_spaces(state->dstar_txt, 8);
    set_spaces(state->dstar_gps, 8);

    //M17 Storage
    memset(state->m17_lsf, 0, sizeof(state->m17_lsf));
    memset(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0;
    state->m17_str_dt = 9;

    state->m17_dst = 0;
    state->m17_src = 0;
    state->m17_can = 0;
    memset(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    memset(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    state->m17_dst_str[0] = '\0';
    state->m17_src_str[0] = '\0';

    state->m17_enc = 0;
    state->m17_enc_st = 0;
    memset(state->m17_meta, 0, sizeof(state->m17_meta));

    //misc str storage
    //  sprintf (state->str50a, "%s", "");
    // memset (state->str50b, 0, 50*sizeof(char));
    // memset (state->str50c, 0, 50*sizeof(char));
    // memset (state->m17sms, 0, 800*sizeof(char));
    // sprintf (state->m17dat, "%s", "");

    //set float temp buffer to baseline
    memset(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    memset(state->audio_out_temp_bufR, 0.0f, sizeof(state->audio_out_temp_bufR));

    //set float temp buffer to baseline
    memset(state->f_l, 0.0f, sizeof(state->f_l));
    memset(state->f_r, 0.0f, sizeof(state->f_r));

    //set float temp buffer to baseline
    memset(state->f_l4, 0.0f, sizeof(state->f_l4));
    memset(state->f_r4, 0.0f, sizeof(state->f_r4));

    //zero out the short sample storage buffers
    memset(state->s_l, 0, sizeof(state->s_l));
    memset(state->s_r, 0, sizeof(state->s_r));
    memset(state->s_l4, 0, sizeof(state->s_l4));
    memset(state->s_r4, 0, sizeof(state->s_r4));

    memset(state->s_lu, 0, sizeof(state->s_lu));
    memset(state->s_ru, 0, sizeof(state->s_ru));
    memset(state->s_l4u, 0, sizeof(state->s_l4u));
    memset(state->s_r4u, 0, sizeof(state->s_r4u));

    //we do reset the counter, but not the static_ks_bits
    memset(state->static_ks_counter, 0, sizeof(state->static_ks_counter));

} //nocarrier

void
initOpts(dsd_opts* opts) {
    opts->floating_point = 0; //use floating point audio output
    opts->onesymbol = 10;
    opts->mbe_in_file[0] = 0;
    opts->mbe_in_f = NULL;
    opts->errorbars = 1;
    opts->datascope = 0;
    opts->constellation = 0;
    opts->const_gate_qpsk = 0.25f;
    opts->const_gate_other = 0.05f;
    opts->const_norm_mode = 0; // default: radial percentile normalization
    opts->eye_view = 0;
    opts->fsk_hist_view = 0;
    opts->eye_unicode = 1;              //default On for clearer rendering
    opts->eye_color = 1;                //default On when terminal supports color
    opts->show_dsp_panel = 0;           // hide compact DSP panel by default
    opts->show_p25_metrics = 0;         // hide P25 metrics by default
    opts->show_p25_neighbors = 0;       // hide P25 Neighbors by default
    opts->show_p25_iden_plan = 0;       // hide P25 IDEN Plan by default
    opts->show_p25_cc_candidates = 0;   // hide P25 CC Candidates by default
    opts->show_p25_callsign_decode = 0; // hide P25 callsign decode by default (many false positives)
    opts->show_channels = 0;            // hide Channels section by default
    opts->symboltiming = 0;
    opts->verbose = 2;
    opts->p25enc = 0;
    opts->p25lc = 0;
    opts->p25status = 0;
    opts->p25tg = 0;
    opts->scoperate = 15;
    snprintf(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", "pulse");
    snprintf(opts->audio_out_dev, sizeof opts->audio_out_dev, "%s", "pulse");
    opts->audio_in_fd = -1;
    opts->audio_out_fd = -1;

    opts->split = 0;
    opts->playoffset = 0;
    opts->playoffsetR = 0;
    snprintf(opts->wav_out_dir, sizeof opts->wav_out_dir, "%s", "./WAV");
    opts->mbe_out_dir[0] = 0;
    opts->mbe_out_file[0] = 0;
    opts->mbe_out_fileR[0] = 0; //second slot on a TDMA system
    opts->mbe_out_path[0] = 0;
    opts->mbe_out_f = NULL;
    opts->mbe_out_fR = NULL; //second slot on a TDMA system
    opts->audio_gain = 0;
    opts->audio_gainR = 0;
    opts->audio_gainA = 50.0f; //scale of 1 - 100
    opts->audio_out = 1;
    opts->wav_out_file[0] = 0;
    opts->wav_out_fileR[0] = 0;
    opts->wav_out_file_raw[0] = 0;
    opts->symbol_out_file[0] = 0;
    opts->lrrp_out_file[0] = 0;
    opts->event_out_file[0] = 0;
    //csv import filenames
    opts->group_in_file[0] = 0;
    opts->lcn_in_file[0] = 0;
    opts->chan_in_file[0] = 0;
    opts->key_in_file[0] = 0;
    //end import filenames
    opts->szNumbers[0] = 0;
    opts->symbol_out_f = NULL;
    opts->symbol_out_file_creation_time = time(NULL);
    opts->symbol_out_file_is_auto = 0;
    opts->mbe_out = 0;
    opts->mbe_outR = 0; //second slot on a TDMA system
    opts->wav_out_f = NULL;
    opts->wav_out_fR = NULL;
    opts->wav_out_raw = NULL;

    opts->dmr_stereo_wav = 0;  //flag for per call dmr stereo wav recordings
    opts->static_wav_file = 0; //single static wav file for decoding duration
    //opts->wav_out_fd = -1;
    opts->serial_baud = 115200;
    snprintf(opts->serial_dev, sizeof opts->serial_dev, "%s", "/dev/ttyUSB0");
    opts->resume = 0;
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 1;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 0;
    opts->frame_nxdn96 = 0;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 0;
    opts->frame_provoice = 0;
    opts->frame_ysf = 1;
    opts->frame_m17 = 0;
    opts->mod_c4fm = 1;
    opts->mod_qpsk = 0;
    opts->mod_gfsk = 0;
    opts->mod_cli_lock = 0; // by default, allow auto modulation selection
    opts->uvquality = 3;
    opts->inverted_x2tdma = 1; // most transmitter + scanner + sound card combinations show inverted signals for this
    opts->inverted_dmr = 0; // most transmitter + scanner + sound card combinations show non-inverted signals for this
    opts->inverted_m17 = 0; //samples from M17_Education seem to all be positive polarity (same from m17-tools programs)
    opts->ssize = 128;      //36 default, max is 128, much cleaner data decodes on Phase 2 cqpsk at max
    opts->msize = 1024;     //15 default, max is 1024, much cleaner data decodes on Phase 2 cqpsk at max
    opts->playfiles = 0;
    opts->m17encoder = 0;
    opts->m17encoderbrt = 0;
    opts->m17encoderpkt = 0;
    opts->m17decoderip = 0;
    opts->delay = 0;
    opts->use_cosine_filter = 1;
    opts->unmute_encrypted_p25 = 0;
    //all RTL user options -- enabled AGC by default due to weak signal related issues
    opts->rtl_dev_index = 0;  //choose which device we want by index number
    opts->rtl_gain_value = 0; //mid value, 0 - AGC - 0 to 49 acceptable values
    opts->rtl_squelch_level = dB_to_pwr(-110);
    opts->rtl_volume_multiplier =
        2; //sample multiplier; This multiplies the sample value to produce a higher 'inlvl' for the demodulator
    // Generic input volume for non-RTL inputs (Pulse/WAV/TCP/UDP)
    opts->input_volume_multiplier = 1;
    opts->rtl_udp_port =
        0; //set UDP port for RTL remote -- 0 by default, will be making this optional for some external/legacy use cases (edacs-fm, etc)
    opts->rtl_dsp_bw_khz = 48;  // DSP baseband kHz (4,6,8,12,16,24,48). Not tuner IF BW.
    opts->rtlsdr_ppm_error = 0; //initialize ppm with 0 value;
    opts->rtlsdr_center_freq =
        850000000; //set to an initial value (if user is using a channel map, then they won't need to specify anything other than -i rtl if desired)
    opts->rtl_started = 0;
    opts->rtl_needs_restart = 0;
    opts->rtl_pwr = 0;                // mean power approximation level on rtl input signal
    opts->rtl_bias_tee = 0;           // bias tee disabled by default
    opts->rtl_auto_ppm = 0;           // spectrum-based auto PPM disabled by default
    opts->rtl_auto_ppm_snr_db = 0.0f; // use default SNR threshold unless overridden
    //end RTL user options
    opts->pulse_raw_rate_in = 48000;
    opts->pulse_raw_rate_out = 48000; //
    opts->pulse_digi_rate_in = 48000;
    opts->pulse_digi_rate_out = 8000; //
    opts->pulse_raw_in_channels = 1;
    opts->pulse_raw_out_channels = 1;
    opts->pulse_digi_in_channels = 1;  //2
    opts->pulse_digi_out_channels = 2; //new default for AUTO
    memset(opts->pa_input_idx, 0, 100 * sizeof(char));
    memset(opts->pa_output_idx, 0, 100 * sizeof(char));

    opts->wav_sample_rate = 48000; //default value (DSDPlus uses 96000 on raw signal wav files)
    opts->wav_interpolator = 1;    //default factor of 1 on 48000; 2 on 96000; sample rate / decimator
    opts->wav_decimator = 48000;   //maybe for future use?

    snprintf(opts->output_name, sizeof opts->output_name, "%s", "AUTO");
    opts->pulse_flush = 1; //set 0 to flush, 1 for flushed
    opts->use_ncurses_terminal = 0;
    opts->ncurses_compact = 0;
    opts->ncurses_history = 1;
#ifdef LIMAZULUTWEAKS
    opts->ncurses_compact = 1;
#endif
    opts->payload = 0;
    opts->inverted_dpmr = 0;
    opts->dmr_mono = 0;
    opts->dmr_stereo = 1;
    opts->aggressive_framesync = 1;
    /* DMR: relax CRC gating by default (others remain strict unless -F). */
    opts->dmr_crc_relaxed_default = 1;

    opts->audio_in_type = AUDIO_IN_PULSE;
    opts->audio_out_type = 0;

    opts->lrrp_file_output = 0;

    opts->dmr_mute_encL = 1;
    opts->dmr_mute_encR = 1;

    opts->monitor_input_audio = 0; //enable with -8
    opts->analog_only = 0;         //only turned on with -fA

    opts->inverted_p2 = 0;
    opts->p2counter = 0;

    opts->call_alert = 0; //call alert beeper for ncurses

    //rigctl options
    opts->use_rigctl = 0;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->rigctlportno = 4532; //TCP Port Number; GQRX - 7356; SDR++ - 4532
    snprintf(opts->rigctlhostname, sizeof opts->rigctlhostname, "%s", "localhost");

    //UDP Socket Blaster Audio
    opts->udp_sockfd = DSD_INVALID_SOCKET;
    opts->udp_sockfdA = DSD_INVALID_SOCKET;
    opts->udp_portno = 23456; //default port, same os OP25's sockaudio.py
    snprintf(opts->udp_hostname, sizeof opts->udp_hostname, "%s", "127.0.0.1");

    //M17 UDP Port and hostname
    opts->m17_use_ip = 0;                    //if enabled, open UDP and broadcast IP frame
    opts->m17_portno = 17000;                //default is 17000
    opts->m17_udp_sock = DSD_INVALID_SOCKET; //actual UDP socket for M17 to send to
    snprintf(opts->m17_hostname, sizeof opts->m17_hostname, "%s", "127.0.0.1");

    //tcp input options
    opts->tcp_sockfd = DSD_INVALID_SOCKET;
    opts->tcp_portno = 7355; //default favored by SDR++
    snprintf(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", "localhost");

    // rtl_tcp defaults
    opts->rtltcp_enabled = 0;
    opts->rtltcp_portno = 1234;
    snprintf(opts->rtltcp_hostname, sizeof opts->rtltcp_hostname, "%s", "127.0.0.1");
    opts->rtltcp_autotune = 0; // default off; enable via CLI --rtltcp-autotune or env

    // UDP direct input defaults
    opts->udp_in_sockfd = DSD_INVALID_SOCKET;
    opts->udp_in_portno = 7355;
    opts->udp_in_bindaddr[0] = '\0';
    opts->udp_in_ctx = NULL;
    opts->udp_in_packets = 0ULL;
    opts->udp_in_bytes = 0ULL;
    opts->udp_in_drops = 0ULL;

    opts->p25_trunk = 0;                  //0 disabled, 1 is enabled
    opts->trunk_enable = opts->p25_trunk; // keep alias in sync
    opts->p25_is_tuned = 0;               //set to 1 if currently on VC, set back to 0 on carrier drop
    // Default hangtime aligned with OP25 (2s) while still releasing promptly after calls.
    opts->trunk_hangtime = 2.0f;

    opts->scanner_mode = 0; //0 disabled, 1 is enabled
    opts->trunk_cli_seen = 0;

    //reverse mute
    opts->reverse_mute = 0;

    //setmod bandwidth
    opts->setmod_bw = 0; //default to 0 - off

    //DMR Location Area - DMRLA B***S***
    opts->dmr_dmrla_is_set = 0;
    opts->dmr_dmrla_n = 0;

    //DMR Late Entry
    opts->dmr_le = 1; //re-enabled again

    //Trunking - Use Group List as Allow List
    opts->trunk_use_allow_list = 0; //disabled by default

    //Trunking - Tune Group Calls
    opts->trunk_tune_group_calls = 1; //enabled by default

    //Trunking - Tune Private Calls
    opts->trunk_tune_private_calls = 1; //enabled by default

    //Trunking - Tune Data Calls
    opts->trunk_tune_data_calls = 0; //disabled by default

    //Trunking - Tune Encrypted Calls (P25 only on applicable grants with svc opts)
    opts->trunk_tune_enc_calls = 1; //enabled by default

    //P25 LCW explicit retune (format 0x44)
    opts->p25_lcw_retune = 0; //disabled by default

    opts->dPMR_next_part_of_superframe = 0;

    opts->slot_preference = 2;
    //hardset slots to synthesize
    opts->slot1_on = 1;
    opts->slot2_on = 1;

    //enable filter options
    opts->use_lpf = 0;
    opts->use_hpf = 1;
    opts->use_pbf = 1;
    opts->use_hpf_d = 1;

    //dsp structured file
    opts->dsp_out_file[0] = 0;
    opts->use_dsp_output = 0;

    //Use P25p1 heuristics
    opts->use_heuristics = 0;

    //DMR TIII heuristic LCN fill (opt-in)
    opts->dmr_t3_heuristic_fill = 0;
    {
        const char* env = getenv("DSD_NEO_DMR_T3_HEUR");
        if (env && (env[0] == '1' || env[0] == 't' || env[0] == 'T' || env[0] == 'y' || env[0] == 'Y')) {
            opts->dmr_t3_heuristic_fill = 1;
            LOG_NOTICE("DMR TIII: Heuristic LCN fill enabled via DSD_NEO_DMR_T3_HEUR.\n");
        }
    }

    // P25P2 soft-decision RS erasure marking (enabled by default)
    opts->p25_p2_soft_erasure = 1;

    // P25P1 soft-decision FEC for voice (enabled by default)
    opts->p25_p1_soft_voice = 1;

    // Low input level warning defaults
    opts->input_warn_db = -40.0;        // warn if below -40 dBFS
    opts->input_warn_cooldown_sec = 10; // rate-limit warnings
    opts->last_input_warn_time = 0;

    // P25 SM unified follower config (CLI-mirrored; values <=0 mean unset)
    opts->p25_vc_grace_s = 0.0;
    opts->p25_min_follow_dwell_s = 0.0;
    opts->p25_grant_voice_to_s = 0.0;
    opts->p25_retune_backoff_s = 0.0;
    opts->p25_force_release_extra_s = 0.0;
    opts->p25_force_release_margin_s = 0.0;
    opts->p25_p1_err_hold_pct = 0.0;
    opts->p25_p1_err_hold_s = 0.0;

} //initopts

static void*
aligned_alloc_64(size_t size) {
    void* p = dsd_aligned_alloc(64, size);
    if (!p) {
        return malloc(size);
    }
    return p;
}

void
initState(dsd_state* state) {

    int i, j;
    // state->testcounter = 0;
    state->last_dibit = 0;
    state->dibit_buf = aligned_alloc_64(sizeof(int) * 1000000);
    state->dibit_buf_p = state->dibit_buf + 200;
    memset(state->dibit_buf, 0, sizeof(int) * 200);
    //dmr buffer -- double check this set up
    state->dmr_payload_buf = aligned_alloc_64(sizeof(int) * 1000000);
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    memset(state->dmr_payload_buf, 0, sizeof(int) * 200);
    memset(state->dmr_stereo_payload, 1, sizeof(int) * 144);
    //dmr buffer end

    // Symbol history buffer for resample-on-sync (SDRTrunk-style)
    // Note: Buffer stores symbols (one per dibit decision), not raw audio samples
    state->dmr_sample_history_size = DMR_SAMPLE_HISTORY_SIZE; // ~427ms at 4800 sym/s
    state->dmr_sample_history = aligned_alloc_64(sizeof(float) * state->dmr_sample_history_size);
    if (state->dmr_sample_history) {
        memset(state->dmr_sample_history, 0, sizeof(float) * state->dmr_sample_history_size);
    }
    state->dmr_sample_history_head = 0;
    state->dmr_sample_history_count = 0;

    // DMR equalizer initialization
    state->dmr_eq.balance = 0.0f;
    state->dmr_eq.gain = 1.0f;
    state->dmr_eq.initialized = 0;
    state->repeat = 0;

    // RTL-SDR stream context (initialized to NULL; lifecycle managed by caller)
    state->rtl_ctx = NULL;

    //Bitmap Filtering Options
    state->audio_smoothing = 0;

    memset(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    memset(state->audio_out_temp_bufR, 0.0f, sizeof(state->audio_out_temp_bufR));

    //set float temp buffer to baseline
    memset(state->f_l, 0.0f, sizeof(state->f_l));
    memset(state->f_r, 0.0f, sizeof(state->f_r));

    //set float temp buffer to baseline
    memset(state->f_l4, 0.0f, sizeof(state->f_l4));
    memset(state->f_r4, 0.0f, sizeof(state->f_r4));

    //zero out the short sample storage buffers
    memset(state->s_l, 0, sizeof(state->s_l));
    memset(state->s_r, 0, sizeof(state->s_r));
    memset(state->s_l4, 0, sizeof(state->s_l4));
    memset(state->s_r4, 0, sizeof(state->s_r4));

    memset(state->s_lu, 0, sizeof(state->s_lu));
    memset(state->s_ru, 0, sizeof(state->s_ru));
    memset(state->s_l4u, 0, sizeof(state->s_l4u));
    memset(state->s_r4u, 0, sizeof(state->s_r4u));

    state->audio_out_buf = aligned_alloc_64(sizeof(short) * 1000000);
    state->audio_out_bufR = aligned_alloc_64(sizeof(short) * 1000000);
    memset(state->audio_out_buf, 0, 100 * sizeof(short));
    memset(state->audio_out_bufR, 0, 100 * sizeof(short));
    //analog/raw signal audio buffers
    state->analog_sample_counter = 0; //when it reaches 960, then dump the raw/analog audio signal and reset
    memset(state->analog_out_f, 0, sizeof(state->analog_out_f));
    memset(state->analog_out, 0, sizeof(state->analog_out));
    //
    state->audio_out_buf_p = state->audio_out_buf + 100;
    state->audio_out_buf_pR = state->audio_out_bufR + 100;
    state->audio_out_float_buf = aligned_alloc_64(sizeof(float) * 1000000);
    state->audio_out_float_bufR = aligned_alloc_64(sizeof(float) * 1000000);
    memset(state->audio_out_float_buf, 0, 100 * sizeof(float));
    memset(state->audio_out_float_bufR, 0, 100 * sizeof(float));
    state->audio_out_float_buf_p = state->audio_out_float_buf + 100;
    state->audio_out_float_buf_pR = state->audio_out_float_bufR + 100;
    state->audio_out_idx = 0;
    state->audio_out_idx2 = 0;
    state->audio_out_idxR = 0;
    state->audio_out_idx2R = 0;
    state->audio_out_temp_buf_p = state->audio_out_temp_buf;
    state->audio_out_temp_buf_pR = state->audio_out_temp_bufR;
    //state->wav_out_bytes = 0;
    state->center = 0;
    state->jitter = -1;
    state->synctype = -1;
    state->min = -15000;
    state->max = 15000;
    state->lmid = 0;
    state->umid = 0;
    state->minref = -12000;
    state->maxref = 12000;
    state->lastsample = 0;
    for (i = 0; i < 128; i++) {
        state->sbuf[i] = 0;
    }
    state->sidx = 0;
    for (i = 0; i < 1024; i++) {
        state->maxbuf[i] = 15000;
    }
    for (i = 0; i < 1024; i++) {
        state->minbuf[i] = -15000;
    }
    state->midx = 0;
    state->err_str[0] = '\0';
    state->err_strR[0] = '\0';
    set_spaces(state->fsubtype, 14);
    set_spaces(state->ftype, 13);
    state->symbolcnt = 0;
    state->symbolc = 0; //
    state->rf_mod = 0;
    state->lastsynctype = -1;
    state->lastp25type = 0;
    state->offset = 0;
    state->carrier = 0;
    for (i = 0; i < 25; i++) {
        for (j = 0; j < 16; j++) {
            state->tg[i][j] = 48;
        }
    }
    state->tgcount = 0;
    state->lasttg = 0;
    state->lastsrc = 0;
    state->lasttgR = 0;
    state->lastsrcR = 0;
    state->gi[0] = -1;
    state->gi[1] = -1;
    state->eh_index = 0;
    state->eh_slot = 0;
    state->nac = 0;
    state->errs = 0;
    state->errs2 = 0;
    state->mbe_file_type = -1;
    state->optind = 0;
    state->numtdulc = 0;
    state->firstframe = 0;
    state->slot1light[0] = '\0';
    state->slot2light[0] = '\0';
    state->aout_gain = 25.0f;
    state->aout_gainR = 25.0f;
    state->aout_gainA = 0.0f; //use purely as a display or internal value, no user setting
    memset(state->aout_max_buf, 0, sizeof(float) * 200);
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;

    memset(state->aout_max_bufR, 0, sizeof(float) * 200);
    state->aout_max_buf_pR = state->aout_max_bufR;
    state->aout_max_buf_idxR = 0;

    state->samplesPerSymbol = 10;
    state->symbolCenter = 4;
    set_underscores(state->algid, 8);
    set_underscores(state->keyid, 16);
    state->currentslot = 0;
    state->cur_mp = malloc(sizeof(mbe_parms));
    state->prev_mp = malloc(sizeof(mbe_parms));
    state->prev_mp_enhanced = malloc(sizeof(mbe_parms));

    state->cur_mp2 = malloc(sizeof(mbe_parms));
    state->prev_mp2 = malloc(sizeof(mbe_parms));
    state->prev_mp_enhanced2 = malloc(sizeof(mbe_parms));

    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    mbe_initMbeParms(state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2);
    state->p25kid = 0;

    // Initialize P25 neighbor/candidate UI helpers
    state->p25_nb_count = 0;
    for (int i2 = 0; i2 < 32; i2++) {
        state->p25_nb_freq[i2] = 0;
        state->p25_nb_last_seen[i2] = 0;
    }
    // Clear P25 call flags
    state->p25_call_emergency[0] = state->p25_call_emergency[1] = 0;
    state->p25_call_priority[0] = state->p25_call_priority[1] = 0;

    state->debug_audio_errors = 0;
    state->debug_audio_errorsR = 0;
    state->debug_header_errors = 0;
    state->debug_header_critical_errors = 0;
    state->debug_mode = 0;

    state->nxdn_last_ran = -1;
    state->nxdn_last_rid = 0;
    state->nxdn_last_tg = 0;
    state->nxdn_cipher_type = 0;
    state->nxdn_key = 0;
    state->nxdn_call_type[0] = '\0';
    state->payload_miN = 0;

    state->dpmr_color_code = -1;

    state->payload_mi = 0;
    state->payload_miR = 0;
    state->payload_mfid = 0;
    state->payload_mfidR = 0;
    state->payload_algid = 0;
    state->payload_algidR = 0;
    state->payload_keyid = 0;
    state->payload_keyidR = 0;

    //init P2 ESS_B fragments and 4V counter
    for (short i = 0; i < 4; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b[1][i] = 0;
    }
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;

    state->K = 0;
    state->R = 0;
    state->RR = 0;
    state->H = 0;
    state->K1 = 0;
    state->K2 = 0;
    state->K3 = 0;
    state->K4 = 0;
    state->M = 0; //force key priority over settings from fid/so

    state->dmr_stereo = 0; //1, or 0?
    state->dmrburstL = 17; //initialize at higher value than possible
    state->dmrburstR = 17; //17 in char array is set for ERR
    state->dmr_so = 0;
    state->dmr_soR = 0;
    state->dmr_fid = 0;
    state->dmr_fidR = 0;
    state->dmr_flco = 0;
    state->dmr_flcoR = 0;
    state->dmr_ms_mode = 0;

    state->HYTL = 0;
    state->HYTR = 0;
    state->DMRvcL = 0;
    state->DMRvcR = 0;
    state->dropL = 256;
    state->dropR = 256;

    state->tyt_ap = 0;
    state->tyt_bp = 0;
    state->tyt_ep = 0;
    state->retevis_ap = 0;

    state->ken_sc = 0;
    state->any_bp = 0;
    state->straight_ks = 0;
    state->straight_mod = 0;

    //ks array storage and counters
    memset(state->ks_octetL, 0, sizeof(state->ks_octetL));
    memset(state->ks_octetR, 0, sizeof(state->ks_octetR));
    memset(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    memset(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    state->octet_counter = 0;
    state->bit_counterL = 0;
    state->bit_counterR = 0;

    memset(state->static_ks_bits, 0, sizeof(state->static_ks_bits));
    memset(state->static_ks_counter, 0, sizeof(state->static_ks_counter));

    //AES Specific Variables
    memset(state->aes_key, 0, sizeof(state->aes_key));
    memset(state->aes_iv, 0, sizeof(state->aes_iv));
    memset(state->aes_ivR, 0, sizeof(state->aes_ivR));
    memset(state->A1, 0, sizeof(state->A1));
    memset(state->A2, 0, sizeof(state->A2));
    memset(state->A3, 0, sizeof(state->A3));
    memset(state->A4, 0, sizeof(state->A4));
    memset(state->aes_key_loaded, 0, sizeof(state->aes_key_loaded));

    //xl specific, we need to know if the ESS is from HDU, or from LDU2
    state->xl_is_hdu = 0;

    //NXDN, when a new IV has arrived
    state->nxdn_new_iv = 0;

    state->p25vc = 0;
    state->payload_miP = 0;
    state->payload_miN = 0;

    //initialize dmr data header source
    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_source[1] = 0;
    state->dmr_lrrp_target[0] = 0;
    state->dmr_lrrp_target[1] = 0;

    //initialize data header bits
    state->data_header_blocks[0] = 1; //initialize with 1, otherwise we may end up segfaulting when no/bad data header
    state->data_header_blocks[1] = 1; //when trying to fill the superframe and 0-1 blocks give us an overflow
    state->data_header_padding[0] = 0;
    state->data_header_padding[1] = 0;
    state->data_header_format[0] = 7;
    state->data_header_format[1] = 7;
    state->data_header_sap[0] = 0;
    state->data_header_sap[1] = 0;
    state->data_block_counter[0] = 1;
    state->data_block_counter[1] = 1;
    state->data_p_head[0] = 0;
    state->data_p_head[1] = 0;
    state->data_block_poc[0] = 0;
    state->data_block_poc[1] = 0;
    state->data_byte_ctr[0] = 0;
    state->data_byte_ctr[1] = 0;
    state->data_ks_start[0] = 0;
    state->data_ks_start[1] = 0;

    /* menu overlay is now fully async and nonblocking; no demod gating needed */

    state->dmr_encL = 0;
    state->dmr_encR = 0;

    //P2 variables
    state->p2_wacn = 0;
    state->p2_sysid = 0;
    state->p2_cc = 0;
    state->p2_siteid = 0;
    state->p2_rfssid = 0;
    state->p2_hardset = 0;
    state->p2_is_lcch = 0;
    // P25p2 RS metrics
    state->p25_p2_rs_facch_ok = 0;
    state->p25_p2_rs_facch_err = 0;
    state->p25_p2_rs_facch_corr = 0;
    state->p25_p2_rs_sacch_ok = 0;
    state->p25_p2_rs_sacch_err = 0;
    state->p25_p2_rs_sacch_corr = 0;
    state->p25_p2_rs_ess_ok = 0;
    state->p25_p2_rs_ess_err = 0;
    state->p25_p2_rs_ess_corr = 0;
    state->p25_p2_enc_lo_early = 0;
    state->p25_p2_enc_pending[0] = 0;
    state->p25_p2_enc_pending[1] = 0;
    state->p25_p2_enc_pending_ttg[0] = 0;
    state->p25_p2_enc_pending_ttg[1] = 0;
    state->p25_cc_is_tdma =
        2; //init on 2, TSBK NET_STS will set 0, TDMA NET_STS will set 1. //used to determine if we need to change symbol rate when cc hunting

    //experimental symbol file capture read throttle
    state->symbol_throttle = 100; //throttle speed
    state->use_throttle = 0;      //only use throttle if set to 1

    state->p2_scramble_offset = 0;
    state->p2_vch_chan_num = 0;

    //p25 iden_up values
    state->p25_chan_iden = 0;
    for (int i = 0; i < 16; i++) {
        state->p25_chan_type[i] = 0;
        state->p25_trans_off[i] = 0;
        state->p25_chan_spac[i] = 0;
        state->p25_base_freq[i] = 0;
    }

    //values displayed in ncurses terminal
    state->p25_cc_freq = 0;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;

    // Initialize P25 regroup/patch tracking
    state->p25_patch_count = 0;
    for (int p = 0; p < 8; p++) {
        state->p25_patch_sgid[p] = 0;
        state->p25_patch_is_patch[p] = 0;
        state->p25_patch_active[p] = 0;
        state->p25_patch_last_update[p] = 0;
        state->p25_patch_wgid_count[p] = 0;
        state->p25_patch_wuid_count[p] = 0;
        for (int q = 0; q < 8; q++) {
            state->p25_patch_wgid[p][q] = 0;
            state->p25_patch_wuid[p][q] = 0;
        }
        state->p25_patch_key[p] = 0;
        state->p25_patch_alg[p] = 0;
        state->p25_patch_ssn[p] = 0;
    }

    //edacs - may need to make these user configurable instead for stability on non-ea systems
    state->ea_mode = -1; //init on -1, 0 is standard, 1 is ea
    state->edacs_vc_call_type = 0;
    state->esk_mask = 0x0; //esk mask value
    state->edacs_site_id = 0;
    state->edacs_sys_id = 0;
    state->edacs_area_code = 0;
    state->edacs_lcn_count = 0;
    state->edacs_cc_lcn = 0;
    state->edacs_vc_lcn = 0;
    state->edacs_tuned_lcn = -1;
    state->edacs_a_bits = 4;   //  Agency Significant Bits
    state->edacs_f_bits = 4;   //   Fleet Significant Bits
    state->edacs_s_bits = 3;   //Subfleet Significant Bits
    state->edacs_a_shift = 7;  //Calculated Shift for A Bits
    state->edacs_f_shift = 3;  //Calculated Shift for F Bits
    state->edacs_a_mask = 0xF; //Calculated Mask for A Bits
    state->edacs_f_mask = 0xF; //Calculated Mask for F Bits
    state->edacs_s_mask = 0x7; //Calculated Mask for S Bits

    //trunking
    memset(state->trunk_lcn_freq, 0, sizeof(state->trunk_lcn_freq));
    memset(state->trunk_chan_map, 0, sizeof(state->trunk_chan_map));
    state->group_tally = 0;
    state->lcn_freq_count = 0; //number of frequncies imported as an enumerated lcn list
    state->lcn_freq_roll = 0;  //needs reset if sync is found?
    state->last_cc_sync_time = time(NULL);
    state->last_vc_sync_time = time(NULL);
    state->last_active_time = time(NULL);
    state->last_t3_tune_time = time(NULL);
    state->is_con_plus = 0;

    //dmr trunking/ncurses stuff
    state->dmr_rest_channel = -1; //init on -1
    state->dmr_mfid = -1;         //
    state->dmr_cc_lpcn = 0;
    state->tg_hold = 0;

    //new nxdn stuff
    state->nxdn_part_of_frame = 0;
    state->nxdn_ran = 0;
    state->nxdn_sf = 0;
    memset(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc)); //init on 1, bad CRC all
    state->nxdn_sacch_non_superframe = TRUE;
    memset(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    state->nxdn_alias_block_number = 0;
    memset(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));

    //site/srv/cch info
    state->nxdn_location_site_code = 0;
    state->nxdn_location_sys_code = 0;
    set_spaces(state->nxdn_location_category, 1);

    //channel access information
    state->nxdn_rcn = 0;
    state->nxdn_base_freq = 0;
    state->nxdn_step = 0;
    state->nxdn_bw = 0;

    //multi-key array
    memset(state->rkey_array, 0, sizeof(state->rkey_array));
    state->keyloader = 0; //keyloader off

    //Remus DMR End Call Alert Beep
    state->dmr_end_alert[0] = 0;
    state->dmr_end_alert[1] = 0;

    state->dmr_branding[0] = '\0';
    state->dmr_branding_sub[0] = '\0';
    state->dmr_site_parms[0] = '\0';

    //initialize unified dmr pdu 'superframe'
    memset(state->dmr_pdu_sf, 0, sizeof(state->dmr_pdu_sf));
    memset(state->data_header_valid, 0, sizeof(state->data_header_valid));

    //initialize cap+ bits and block num storage
    memset(state->cap_plus_csbk_bits, 0, sizeof(state->cap_plus_csbk_bits));
    memset(state->cap_plus_block_num, 0, sizeof(state->cap_plus_block_num));

    //init confirmed data individual block crc as invalid
    memset(state->data_block_crc_valid, 0, sizeof(state->data_block_crc_valid));

    //dmr slco stuff
    memset(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
    state->dmr_cach_counter = 0;

    //embedded signalling
    memset(state->dmr_embedded_signalling, 0, sizeof(state->dmr_embedded_signalling));

    //dmr talker alias new/fixed stuff
    memset(state->dmr_alias_format, 0, sizeof(state->dmr_alias_format));
    memset(state->dmr_alias_block_len, 0, sizeof(state->dmr_alias_block_len));
    memset(state->dmr_alias_char_size, 0, sizeof(state->dmr_alias_char_size));
    memset(state->dmr_alias_block_segment, 0, sizeof(state->dmr_alias_block_segment));
    memset(state->dmr_embedded_gps, 0, sizeof(state->dmr_embedded_gps));
    memset(state->dmr_lrrp_gps, 0, sizeof(state->dmr_lrrp_gps));
    memset(state->active_channel, 0, sizeof(state->active_channel));

    //Generic Talker Alias String
    memset(state->generic_talker_alias, 0, sizeof(state->generic_talker_alias));
    state->generic_talker_alias_src[0] = 0;
    state->generic_talker_alias_src[1] = 0;

    //REMUS! multi-purpose call_string
    set_spaces(state->call_string[0], 21);
    set_spaces(state->call_string[1], 21);

    //late entry mi fragments
    memset(state->late_entry_mi_fragment, 0, sizeof(state->late_entry_mi_fragment));

    initialize_p25_heuristics(&state->p25_heuristics);
    initialize_p25_heuristics(&state->inv_p25_heuristics);

    state->dPMRVoiceFS2Frame.CalledIDOk = 0;
    state->dPMRVoiceFS2Frame.CallingIDOk = 0;
    memset(state->dPMRVoiceFS2Frame.CalledID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.CallingID, 0, 8);
    memset(state->dPMRVoiceFS2Frame.Version, 0, 8);

    set_spaces(state->dpmr_caller_id, 6);
    set_spaces(state->dpmr_target_id, 6);

    //YSF Fusion Call Strings
    set_spaces(state->ysf_tgt, 10); //10 spaces
    set_spaces(state->ysf_src, 10); //10 spaces
    set_spaces(state->ysf_upl, 10); //10 spaces
    set_spaces(state->ysf_dnl, 10); //10 spaces
    set_spaces(state->ysf_rm1, 5);  //5 spaces
    set_spaces(state->ysf_rm2, 5);  //5 spaces
    set_spaces(state->ysf_rm3, 5);  //5 spaces
    set_spaces(state->ysf_rm4, 5);  //5 spaces
    memset(state->ysf_txt, 0, sizeof(state->ysf_txt));
    state->ysf_dt = 9;
    state->ysf_fi = 9;
    state->ysf_cm = 9;

    //DSTAR Call Strings
    set_spaces(state->dstar_rpt1, 8); //8 spaces
    set_spaces(state->dstar_rpt2, 8); //8 spaces
    set_spaces(state->dstar_dst, 8);  //8 spaces
    set_spaces(state->dstar_src, 8);  //8 spaces
    set_spaces(state->dstar_txt, 8);  //8 spaces
    set_spaces(state->dstar_gps, 8);  //8 spaces

    //M17 Storage
    memset(state->m17_lsf, 0, sizeof(state->m17_lsf));
    memset(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0;
    state->m17_str_dt = 9;

    //misc str storage
    //  sprintf (state->str50a, "%s", "");
    memset(state->str50b, 0, 50 * sizeof(char));
    memset(state->str50c, 0, 50 * sizeof(char));
    memset(state->m17sms, 0, 800 * sizeof(char));
    state->m17dat[0] = '\0';

    state->m17_dst = 0;
    state->m17_src = 0;
    state->m17_can = 0;      //can value that was decoded from signal
    state->m17_can_en = -1;  //can value supplied to the encoding side
    state->m17_rate = 48000; //sampling rate for audio input
    state->m17_vox = 0;      //vox mode enabled on M17 encoder
    memset(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    memset(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    state->m17_dst_str[0] = '\0';
    state->m17_src_str[0] = '\0';

    state->m17_enc = 0;
    state->m17_enc_st = 0;
    state->m17encoder_tx = 0;
    state->m17encoder_eot = 0;
    memset(state->m17_meta, 0, sizeof(state->m17_meta));

#ifdef USE_CODEC2
    state->codec2_3200 = codec2_create(CODEC2_MODE_3200);
    state->codec2_1600 = codec2_create(CODEC2_MODE_1600);
#endif

    state->dmr_color_code = 16;
    state->dmr_t3_syscode = 0;

    // Allocate per-slot event history (2 slots)
    state->event_history_s = calloc(2, sizeof(Event_History_I));

    //debug
    //  fprintf (stderr, "allocated size of event history struct: %ld bytes; \n", 600 * sizeof(Event_History));

    if (state->event_history_s == NULL) {
        LOG_ERROR("memory allocation failure! \n");
    }

    //initialize event history items (0 to 255)
    for (uint8_t i = 0; i < 2; i++) {
        init_event_history(&state->event_history_s[i], 0, 255);
    }

    // Initialize transient UI toast message state
    state->ui_msg[0] = '\0';
    state->ui_msg_expire = 0;

} //init_state

void
liveScanner(dsd_opts* opts, dsd_state* state) {
    // Cache previous thresholds to avoid redundant recalculation
    static int last_max = INT_MIN;
    static int last_min = INT_MAX;

    if (opts->floating_point == 1) {

        if (opts->audio_gain > 50.0f) {
            opts->audio_gain = 50.0f;
        }
        if (opts->audio_gain < 0.0f) {
            opts->audio_gain = 0.0f;
        }
    } else if (opts->audio_gain == 0) {
        state->aout_gain = 15.0f;
        state->aout_gainR = 15.0f;
    }

#ifdef USE_RTLSDR
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (state->rtl_ctx == NULL) {
            if (rtl_stream_create(opts, &state->rtl_ctx) < 0) {
                LOG_ERROR("Failed to create RTL stream.\n");
            }
        }
        if (state->rtl_ctx && rtl_stream_start(state->rtl_ctx) < 0) {
            LOG_ERROR("Failed to open RTL-SDR stream.\n");
        }
        opts->rtl_started = 1;
        opts->rtl_needs_restart = 0;
    }
#endif

    if (opts->audio_in_type == AUDIO_IN_PULSE) {
        openPulseInput(opts);
    }

    if (opts->audio_out_type == 0) {
        openPulseOutput(opts);
    }

    //push a DSD-neo started event so users can see what this section does, and also gives users an idea of when context started
    state->event_history_s[0].Event_History_Items[0].color_pair = 4;
    watchdog_event_datacall(opts, state, 0, 0, "Any decoded voice calls or data calls display here;", 0);
    push_event_history(&state->event_history_s[0]);
    init_event_history(&state->event_history_s[0], 0, 1);
    state->event_history_s[0].Event_History_Items[0].color_pair = 4;
    watchdog_event_datacall(opts, state, 0, 0, "DSD-neo Started and Event History Initialized;", 0);
    push_event_history(&state->event_history_s[0]);
    init_event_history(&state->event_history_s[0], 0, 1);

    if (opts->event_out_file[0] != 0) {
        char* timestr = getTimeN(time(NULL));
        char* datestr = getDateN(time(NULL));
        char event_string[2000];
        memset(event_string, 0, sizeof(event_string));
        snprintf(event_string, sizeof event_string, "%s %s DSD-neo Started and Event History Initialized;", datestr,
                 timestr);
        write_event_to_log_file(opts, state, 0, 0, event_string);
        memset(event_string, 0, sizeof(event_string));
        snprintf(event_string, sizeof event_string, "%s %s Any decoded voice calls or data calls display here;",
                 datestr, timestr);
        write_event_to_log_file(opts, state, 0, 0, event_string);

        if (timestr != NULL) {
            free(timestr);
            timestr = NULL;
        }
        if (datestr != NULL) {
            free(datestr);
            datestr = NULL;
        }
    }

    //test P25 moto alias by loading in test vectors captured from a system and dumped on forum (see dsd_gps.c)
    // apx_embedded_alias_test_phase1(opts, state); //enable this to run test

    /* Start P25 SM watchdog thread to ensure ticks during I/O stalls */
    p25_sm_watchdog_start(opts, state);

    while (!exitflag) {
        // Drain any pending UI→Demod commands before heavy work
        ui_drain_cmds(opts, state);

        // Cooperative tick: runs only if another tick isn't in progress
        p25_sm_try_tick(opts, state);

        // Drain again to reduce latency for common key actions
        ui_drain_cmds(opts, state);

        noCarrier(opts, state);
        state->synctype = getFrameSync(opts, state);
        // Recompute thresholds only when extrema change
        if (state->max != last_max || state->min != last_min) {
            state->center = ((state->max) + (state->min)) / 2;
            state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
            state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
            last_max = state->max;
            last_min = state->min;
        }

        while (state->synctype != -1) {
            // Drain UI commands during active decoding so hotkeys work in-call
            ui_drain_cmds(opts, state);

            processFrame(opts, state);

#ifdef TRACE_DSD
            state->debug_prefix = 'S';
#endif

            // state->synctype = getFrameSync (opts, state);

#ifdef TRACE_DSD
            state->debug_prefix = '\0';
#endif

            // // recalibrate center/umid/lmid
            // state->center = ((state->max) + (state->min)) / 2;
            // state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
            // state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
            // Drain again between frames to reduce latency
            ui_drain_cmds(opts, state);
            state->synctype = getFrameSync(opts, state);
            // Recompute thresholds only when extrema change
            if (state->max != last_max || state->min != last_min) {
                state->center = ((state->max) + (state->min)) / 2;
                state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
                state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
                last_max = state->max;
                last_min = state->min;
            }
        }
    }

    p25_sm_watchdog_stop();
}

void
cleanupAndExit(dsd_opts* opts, dsd_state* state) {
    // Signal that everything should shutdown.
    exitflag = 1;

    // Stop UI thread if the ncurses UI was in use.
    if (opts->use_ncurses_terminal == 1) {
        ui_stop();
    }

#ifdef USE_CODEC2
    codec2_destroy(state->codec2_1600);
    codec2_destroy(state->codec2_3200);
#endif

    //watchdog event at this point
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);

    noCarrier(opts, state);

    //watchdog event at this point
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);

    if (opts->static_wav_file == 0) {
        if (opts->wav_out_f != NULL) {
            opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts->wav_out_file, opts->wav_out_dir,
                                                        &state->event_history_s[0]);
        }

        if (opts->wav_out_fR != NULL) {
            opts->wav_out_fR = close_and_rename_wav_file(opts->wav_out_fR, opts->wav_out_fileR, opts->wav_out_dir,
                                                         &state->event_history_s[1]);
        }
    }

    else if (opts->static_wav_file == 1) {

        if (opts->wav_out_f != NULL) {
            opts->wav_out_f = close_wav_file(opts->wav_out_f);
        }

        //this one needed?
        if (opts->wav_out_fR != NULL) {
            opts->wav_out_fR = close_wav_file(opts->wav_out_fR);
        }
    }

    if (opts->wav_out_raw != NULL) {
        opts->wav_out_raw = close_wav_file(opts->wav_out_raw);
    }

    //no if statement first?
    closeSymbolOutFile(opts, state);

#ifdef USE_RTLSDR
    if (opts->rtl_started == 1) {
        if (state->rtl_ctx) {
            rtl_stream_stop(state->rtl_ctx);
            rtl_stream_destroy(state->rtl_ctx);
            state->rtl_ctx = NULL;
        }
    }
#endif

    if (opts->udp_sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(opts->udp_sockfd);
    }

    if (opts->udp_sockfdA != DSD_INVALID_SOCKET) {
        dsd_socket_close(opts->udp_sockfdA);
    }

    if (opts->m17_udp_sock != DSD_INVALID_SOCKET) {
        dsd_socket_close(opts->m17_udp_sock);
    }

    if (opts->udp_in_ctx) {
        udp_input_stop(opts);
    }

    //close MBE out files
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }

    // Persist the final effective configuration for the next run, if enabled.
    autosave_user_config(opts, state);

    LOG_NOTICE("\n");
    LOG_NOTICE("Total audio errors: %i\n", state->debug_audio_errors);
    LOG_NOTICE("Total header errors: %i\n", state->debug_header_errors);
    LOG_NOTICE("Total irrecoverable header errors: %i\n", state->debug_header_critical_errors);
    LOG_NOTICE("Exiting.\n");

    // Cleanup socket subsystem (required for Windows, no-op on POSIX)
    dsd_socket_cleanup();

    exit(0);
}

double
atofs(char* s) {
    size_t len = strlen(s);
    if (len == 0) {
        return 0.0;
    }

    char last = s[len - 1];
    double factor = 1.0;

    switch (last) {
        case 'g':
        case 'G': factor = 1e9; break;
        case 'm':
        case 'M': factor = 1e6; break;
        case 'k':
        case 'K': factor = 1e3; break;
        default: return atof(s);
    }

    s[len - 1] = '\0';
    double val = atof(s);
    s[len - 1] = last;
    return val * factor;
}

int
main(int argc, char** argv) {
    extern char* optarg;
    extern int optind;
    dsd_opts* opts = calloc(1, sizeof(dsd_opts));
    dsd_state* state = calloc(1, sizeof(dsd_state));
    if (!opts || !state) {
        fprintf(stderr, "Failed to allocate memory for opts/state\n");
        free(opts);
        free(state);
        return 1;
    }
    int argc_effective = argc; // effective argc after runtime compaction
    const char* versionstr = mbe_versionString();

    initOpts(opts);
    initState(state);
    dsd_bootstrap_enable_ftz_daz_if_enabled();
    init_rrc_filter_memory(); //initialize input filtering
    InitAllFecFunction();
    CNXDNConvolution_init();

    // Initialize socket subsystem (required for Windows, no-op on POSIX)
    if (dsd_socket_init() != 0) {
        fprintf(stderr, "Failed to initialize socket subsystem\n");
        return 1;
    }

    exitflag = 0;

    // Optional: user configuration file (INI) -----------------------------
    int enable_config_cli = 0;
    int force_bootstrap_cli = 0;
    int print_config_cli = 0;
    int dump_template_cli = 0;
    int validate_config_cli = 0;
    int strict_config_cli = 0;
    int list_profiles_cli = 0;
    const char* config_path_cli = NULL;
    const char* profile_cli = NULL;
    const char* validate_path_cli = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0) {
            enable_config_cli = 1;
            // Optional path argument (if next arg doesn't start with '-')
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                config_path_cli = argv[++i];
            }
        } else if (strcmp(argv[i], "--interactive-setup") == 0) {
            force_bootstrap_cli = 1;
        } else if (strcmp(argv[i], "--print-config") == 0) {
            print_config_cli = 1;
        } else if (strcmp(argv[i], "--dump-config-template") == 0) {
            dump_template_cli = 1;
        } else if (strcmp(argv[i], "--validate-config") == 0) {
            validate_config_cli = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                validate_path_cli = argv[++i];
            }
        } else if (strcmp(argv[i], "--strict-config") == 0) {
            strict_config_cli = 1;
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile_cli = argv[++i];
        } else if (strcmp(argv[i], "--list-profiles") == 0) {
            list_profiles_cli = 1;
        }
    }

    const char* config_env = getenv("DSD_NEO_CONFIG");

    int user_cfg_loaded = 0;
    dsdneoUserConfig user_cfg;
    user_cfg.version = 0;

    // Default to no autosave unless a config is actually in play for this run.
    s_user_config_save_enabled = 0;
    s_user_config_save_path[0] = '\0';

    /* Config loading is opt-in: only load if --config is passed (with or
     * without a path) or if DSD_NEO_CONFIG env var is set. CLI takes
     * precedence: --config without a path uses the default, ignoring env. */
    if (enable_config_cli || (config_env && *config_env)) {
        const char* cfg_path = NULL;
        if (config_path_cli && *config_path_cli) {
            cfg_path = config_path_cli;
        } else if (enable_config_cli) {
            cfg_path = dsd_user_config_default_path();
        } else if (config_env && *config_env) {
            cfg_path = config_env;
        }

        if (cfg_path && *cfg_path) {
            // Remember the path so we can autosave the effective config later.
            s_user_config_save_enabled = 1;
            snprintf(s_user_config_save_path, sizeof s_user_config_save_path, "%s", cfg_path);
            s_user_config_save_path[sizeof s_user_config_save_path - 1] = '\0';

            int load_rc;
            if (profile_cli && *profile_cli) {
                load_rc = dsd_user_config_load_profile(cfg_path, profile_cli, &user_cfg);
            } else {
                load_rc = dsd_user_config_load(cfg_path, &user_cfg);
            }

            if (load_rc == 0) {
                dsd_apply_user_config_to_opts(&user_cfg, opts, state);
                user_cfg_loaded = 1;
                if (profile_cli && *profile_cli) {
                    LOG_NOTICE("Loaded user config from %s (profile: %s)\n", cfg_path, profile_cli);
                } else {
                    LOG_NOTICE("Loaded user config from %s\n", cfg_path);
                }
            } else if (profile_cli && *profile_cli) {
                // Missing profile is fatal when --profile is specified
                LOG_ERROR("Profile '%s' not found in config file %s\n", profile_cli, cfg_path);
                return 1;
            } else if (config_path_cli || config_env || enable_config_cli) {
                LOG_WARNING("Failed to load config file from %s; proceeding without config.\n", cfg_path);
            }
        }
    } else {
        // Config loading was not requested; do not autosave either.
        s_user_config_save_enabled = 0;
        s_user_config_save_path[0] = '\0';
    }

    // Phase 1: long-option and env parsing moved into runtime CLI helper
    {
        int oneshot_rc = 0;
        int early_rc = dsd_parse_args(argc, argv, opts, state, &argc_effective, &oneshot_rc);
        if (early_rc == DSD_PARSE_ONE_SHOT) {
            return oneshot_rc;
        } else if (early_rc != DSD_PARSE_CONTINUE) {
            return early_rc;
        }
        // Keep original argc for UI bootstrap heuristics; use argc_effective
        // only when iterating argv for file playback (-r).
    }

    // If a user config enabled trunking but this process was started with
    // any CLI arguments and none of them explicitly enabled/disabled trunk
    // (via -T / -Y), fall back to the built-in default of trunking disabled
    // for this run. This keeps CLI-driven sessions from inheriting trunk
    // enable solely from the config file.
    if (argc > 1 && user_cfg_loaded && !opts->trunk_cli_seen) {
        opts->p25_trunk = 0;
        opts->trunk_enable = 0;
    }

    // If a user config specified a non-48kHz file/RAW input and the CLI did
    // not override its sample rate, apply the corresponding symbol timing
    // scaling after all CLI/env parsing so that mode presets are adjusted
    // correctly. This mirrors legacy "-s" behavior without requiring users
    // to manage option ordering manually when using the config file.
    if (user_cfg_loaded && user_cfg.has_input && user_cfg.input_source == DSDCFG_INPUT_FILE
        && user_cfg.file_sample_rate > 0 && user_cfg.file_sample_rate != 48000 && opts->wav_decimator != 0
        && user_cfg.file_path[0] != '\0' && strcmp(opts->audio_in_dev, user_cfg.file_path) == 0
        && opts->wav_sample_rate == user_cfg.file_sample_rate) {
        opts->wav_interpolator = opts->wav_sample_rate / opts->wav_decimator;
        state->samplesPerSymbol = state->samplesPerSymbol * opts->wav_interpolator;
        state->symbolCenter = state->symbolCenter * opts->wav_interpolator;
    }

    if (print_config_cli) {
        dsdneoUserConfig eff;
        dsd_snapshot_opts_to_user_config(opts, state, &eff);
        dsd_user_config_render_ini(&eff, stdout);
        return 0;
    }

    // --dump-config-template: print commented template and exit
    if (dump_template_cli) {
        dsd_user_config_render_template(stdout);
        return 0;
    }

    // --validate-config: validate config file and exit
    if (validate_config_cli) {
        const char* vpath = validate_path_cli;
        if (!vpath || !*vpath) {
            // Use default or explicit config path
            if (config_path_cli && *config_path_cli) {
                vpath = config_path_cli;
            } else if (config_env && *config_env) {
                vpath = config_env;
            } else {
                vpath = dsd_user_config_default_path();
            }
        }
        if (!vpath || !*vpath) {
            fprintf(stderr, "No config file path specified or found.\n");
            return 1;
        }

        dsdcfg_diagnostics_t diags;
        int rc = dsd_user_config_validate(vpath, &diags);

        if (diags.count > 0) {
            dsdcfg_diags_print(&diags, stderr, vpath);
        } else {
            fprintf(stderr, "%s: OK\n", vpath);
        }

        int exit_code = 0;
        if (rc != 0 || diags.error_count > 0) {
            exit_code = 1;
        } else if (strict_config_cli && diags.warning_count > 0) {
            exit_code = 2;
        }

        dsd_user_config_diags_free(&diags);
        return exit_code;
    }

    // --list-profiles: list available profiles and exit
    if (list_profiles_cli) {
        const char* lpath = config_path_cli;
        if (!lpath || !*lpath) {
            if (config_env && *config_env) {
                lpath = config_env;
            } else {
                lpath = dsd_user_config_default_path();
            }
        }
        if (!lpath || !*lpath) {
            fprintf(stderr, "No config file path specified or found.\n");
            return 1;
        }

        const char* names[32];
        char names_buf[1024];
        int count = dsd_user_config_list_profiles(lpath, names, names_buf, sizeof names_buf, 32);

        if (count < 0) {
            fprintf(stderr, "Failed to read config file: %s\n", lpath);
            return 1;
        }

        if (count == 0) {
            printf("No profiles found in %s\n", lpath);
        } else {
            printf("Profiles in %s:\n", lpath);
            for (int i = 0; i < count; i++) {
                printf("  %s\n", names[i]);
            }
        }
        return 0;
    }

    // Print banner only if not a one-shot action
    LOG_NOTICE("------------------------------------------------------------------------------\n");
    LOG_NOTICE("| Digital Speech Decoder: DSD-neo %s (%s) \n", GIT_TAG, GIT_HASH);
    LOG_NOTICE("------------------------------------------------------------------------------\n");

    LOG_NOTICE("MBElib-neo Version: %s\n", versionstr);

#ifdef USE_CODEC2
    LOG_NOTICE("CODEC2 Support Enabled\n");
#endif

    // All long-option parsing, environment mapping, and the DMR TIII LCN
    // calculator one-shot flow are now handled inside dsd_parse_args().

    // If user requested it explicitly, or if there are no CLI args and no
    // user config, offer interactive bootstrap. The CLI flag overrides
    // any env-based skip (DSD_NEO_NO_BOOTSTRAP).
    if (force_bootstrap_cli || (argc <= 1 && !user_cfg_loaded)) {
        if (force_bootstrap_cli) {
            dsd_unsetenv("DSD_NEO_NO_BOOTSTRAP");
        }
        bootstrap_interactive(opts, state);
    }

    /* Rebuild audio filters after CLI/config/bootstrap may have changed the output rate.
       Base coefficients on the analog monitor sample rate so cutoffs stay correct. */
    {
        int filter_rate = analog_filter_rate_hz(opts, state);
        init_audio_filters(state, filter_rate);
    }

    /* Initialize trunking state machines with user configuration.
       Must be done after all opts parsing so hangtime/timeouts are honored. */
    p25_sm_init(opts, state);
    dmr_sm_init(opts, state);

    /* long-option normalization handled inside dsd_parse_args */

    if (opts->resume > 0) {
        openSerial(opts, state);
    }

    if ((strncmp(opts->audio_in_dev, "m17udp", 6) == 0)) //M17 UDP Socket Input
    {
        LOG_NOTICE("M17 UDP IP Frame Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts->audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = dsd_strtok_r(inbuf, ":", &saveptr); //should be 'm17'
        if (curr == NULL) {
            goto M17ENDIN; //end early with preset values
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //host address
        if (curr != NULL) {
            strncpy(opts->m17_hostname, curr, 1023);
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //host port
        if (curr != NULL) {
            opts->m17_portno = atoi(curr);
        }

    M17ENDIN:
        LOG_NOTICE("%s:", opts->m17_hostname);
        LOG_NOTICE("%d \n", opts->m17_portno);
    }

    if ((strncmp(opts->audio_in_dev, "udp", 3) == 0)) // UDP Direct Audio Input
    {
        LOG_NOTICE("UDP Direct Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts->audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = dsd_strtok_r(inbuf, ":", &saveptr); // 'udp'
        if (curr == NULL) {
            goto UDPINEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // bind address
        if (curr != NULL) {
            strncpy(opts->udp_in_bindaddr, curr, 1023);
            opts->udp_in_bindaddr[1023] = '\0';
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // bind port
        if (curr != NULL) {
            opts->udp_in_portno = atoi(curr);
        }

    UDPINEND:
        if (opts->udp_in_portno == 0) {
            opts->udp_in_portno = 7355;
        }
        if (opts->udp_in_bindaddr[0] == '\0') {
            snprintf(opts->udp_in_bindaddr, sizeof(opts->udp_in_bindaddr), "%s", "127.0.0.1");
        }
        LOG_NOTICE("%s:%d\n", opts->udp_in_bindaddr, opts->udp_in_portno);
    }

    if ((strncmp(opts->audio_out_dev, "m17udp", 6) == 0)) //M17 UDP Socket Output
    {
        LOG_NOTICE("M17 UDP IP Frame Output: ");
        char* curr;
        char* saveptr = NULL;
        char outbuf[1024];
        strncpy(outbuf, opts->audio_out_dev, sizeof(outbuf) - 1);
        outbuf[sizeof(outbuf) - 1] = '\0';

        curr = dsd_strtok_r(outbuf, ":", &saveptr); //should be 'm17'
        if (curr == NULL) {
            goto M17ENDOUT; //end early with preset values
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //host address
        if (curr != NULL) {
            strncpy(opts->m17_hostname, curr, 1023);
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //host port
        if (curr != NULL) {
            opts->m17_portno = atoi(curr);
        }

    M17ENDOUT:
        LOG_NOTICE("%s:", opts->m17_hostname);
        LOG_NOTICE("%d \n", opts->m17_portno);
        opts->m17_use_ip = 1;     //tell the encoder to open the socket
        opts->audio_out_type = 9; //set to null device
    }

    if ((strncmp(opts->audio_in_dev, "tcp", 3) == 0)) //tcp socket input from SDR++ and others
    {
        LOG_NOTICE("TCP Direct Link: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts->audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = dsd_strtok_r(inbuf, ":", &saveptr); //should be 'tcp'
        if (curr == NULL) {
            goto TCPEND; //end early with preset values
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //host address
        if (curr != NULL) {
            strncpy(opts->tcp_hostname, curr, 1023);
            //shim to tie the hostname of the tcp input to the rigctl hostname (probably covers a vast majority of use cases)
            //in the future, I will rework part of this so that users can enter a hostname and port similar to how tcp and rtl strings work
            memcpy(opts->rigctlhostname, opts->tcp_hostname, sizeof(opts->rigctlhostname));
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //host port
        if (curr != NULL) {
            opts->tcp_portno = atoi(curr);
        }

    TCPEND:
        if (exitflag == 1) {
            cleanupAndExit(opts, state); //needed to break the loop on ctrl+c
        }
        LOG_NOTICE("%s:", opts->tcp_hostname);
        LOG_NOTICE("%d \n", opts->tcp_portno);
        opts->tcp_sockfd = Connect(opts->tcp_hostname, opts->tcp_portno);
        if (opts->tcp_sockfd != DSD_INVALID_SOCKET) {
            opts->audio_in_type = AUDIO_IN_TCP;

            LOG_NOTICE("TCP Connection Success!\n");
            // openAudioInDevice(opts); //do this to see if it makes it work correctly
        } else {
            if (opts->frame_m17 == 1) {
                dsd_sleep_ms(1000);
                goto TCPEND; //try again if using M17 encoder / decoder over TCP
            }
            sprintf(opts->audio_in_dev, "%s", "pulse");
            LOG_ERROR("TCP Connection Failure - Using %s Audio Input.\n", opts->audio_in_dev);
            opts->audio_in_type = AUDIO_IN_PULSE;
        }
    }

    if (opts->use_rigctl == 1) {
        opts->rigctl_sockfd = Connect(opts->rigctlhostname, opts->rigctlportno);
        if (opts->rigctl_sockfd != DSD_INVALID_SOCKET) {
            opts->use_rigctl = 1;
        } else {
            LOG_ERROR("RIGCTL Connection Failure - RIGCTL Features Disabled\n");
            opts->use_rigctl = 0;
        }
    }

    if ((strncmp(opts->audio_in_dev, "rtltcp", 6) == 0)) // rtl_tcp networked RTL-SDR
    {
        LOG_NOTICE("RTL_TCP Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts->audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = dsd_strtok_r(inbuf, ":", &saveptr); // 'rtltcp'
        if (curr == NULL) {
            goto RTLTCPEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // host
        if (curr != NULL) {
            strncpy(opts->rtltcp_hostname, curr, 1023);
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // port
        if (curr != NULL) {
            opts->rtltcp_portno = atoi(curr);
        }

        // Optional: freq:gain:ppm:bw:sql:vol (mirrors rtl: string semantics)
        curr = dsd_strtok_r(NULL, ":", &saveptr); // freq
        if (curr != NULL) {
            opts->rtlsdr_center_freq = (uint32_t)atofs(curr);
        } else {
            goto RTLTCPEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // gain
        if (curr != NULL) {
            opts->rtl_gain_value = atoi(curr);
        } else {
            goto RTLTCPEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // ppm
        if (curr != NULL) {
            opts->rtlsdr_ppm_error = atoi(curr);
        } else {
            goto RTLTCPEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // bw (kHz)
        if (curr != NULL) {
            int bw = atoi(curr);
            if (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48) {
                opts->rtl_dsp_bw_khz = bw;
            } else {
                opts->rtl_dsp_bw_khz = 48;
            }
        } else {
            goto RTLTCPEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // sql (dB if negative; else linear)
        if (curr != NULL) {
            double sq_val = atof(curr);
            if (sq_val < 0.0) {
                opts->rtl_squelch_level = dB_to_pwr(sq_val);
            } else {
                opts->rtl_squelch_level = sq_val;
            }
        } else {
            goto RTLTCPEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); // vol (1..3)
        if (curr != NULL) {
            opts->rtl_volume_multiplier = atoi(curr);
        } else {
            goto RTLTCPEND;
        }

        // Optional trailing tokens: bias tee toggle
        while ((curr = dsd_strtok_r(NULL, ":", &saveptr)) != NULL) {
            if (strncmp(curr, "bias", 4) == 0 || strncmp(curr, "b", 1) == 0) {
                const char* val = strchr(curr, '=');
                int on = 1; // default enable if no explicit value
                if (val && *(val + 1)) {
                    val++; // move past '='
                    if (*val == '0' || *val == 'n' || *val == 'N' || *val == 'o' || *val == 'O' || *val == 'f'
                        || *val == 'F') {
                        on = 0;
                    }
                }
                opts->rtl_bias_tee = on;
            }
        }

    RTLTCPEND:
        if (opts->rtltcp_portno == 0) {
            opts->rtltcp_portno = 1234;
        }
        LOG_NOTICE("%s:%d", opts->rtltcp_hostname, opts->rtltcp_portno);
        if (opts->rtl_bias_tee) {
            LOG_NOTICE(" (bias=on)\n");
        } else {
            LOG_NOTICE("\n");
        }
        opts->rtltcp_enabled = 1;
        opts->audio_in_type = AUDIO_IN_RTL; // use RTL pipeline
    }

    // NOTE: Guard against matching "rtltcp" here; it shares the "rtl" prefix
    // and opts->audio_in_dev has been tokenized by strtok above. Without this
    // guard, selecting rtltcp would also fall through to the local RTL path
    // and erroneously require a USB device, causing an early exit.
    if ((strncmp(opts->audio_in_dev, "rtl", 3) == 0)
        && (strncmp(opts->audio_in_dev, "rtltcp", 6) != 0)) //rtl dongle input
    {
        uint8_t rtl_ok = 0;
        //use to list out all detected RTL dongles
        char vendor[256], product[256], serial[256];
        int device_count = 0;

#ifdef USE_RTLSDR
        LOG_NOTICE("RTL Input: ");
        char* curr;
        char* saveptr = NULL;
        char inbuf[1024];
        strncpy(inbuf, opts->audio_in_dev, sizeof(inbuf) - 1);
        inbuf[sizeof(inbuf) - 1] = '\0';

        curr = dsd_strtok_r(inbuf, ":", &saveptr); //should be 'rtl'
        if (curr == NULL) {
            goto RTLEND; //end early with preset values
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl device number "-D"
        if (curr != NULL) {
            opts->rtl_dev_index = atoi(curr);
        } else {
            goto RTLEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl freq "-c"
        if (curr != NULL) {
            opts->rtlsdr_center_freq = (uint32_t)atofs(curr);
        } else {
            goto RTLEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl gain value "-G"
        if (curr != NULL) {
            opts->rtl_gain_value = atoi(curr);
        } else {
            goto RTLEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl ppm err "-P"
        if (curr != NULL) {
            opts->rtlsdr_ppm_error = atoi(curr);
        } else {
            goto RTLEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl bandwidth "-Y"
        if (curr != NULL) {
            int bw = 0;
            bw = atoi(curr);
            //check for proper values (4,6,8,12,16,24,48)
            if (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24
                || bw == 48) // testing 4 and 16 as well for weak and/or nxdn48 systems
            {
                opts->rtl_dsp_bw_khz = bw;
            } else {
                opts->rtl_dsp_bw_khz = 48; // default baseband when input omits/invalid
            }
        } else {
            goto RTLEND;
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl squelch threshold (dB if negative; else linear)
        if (curr != NULL) {
            double sq_val = atof(curr);
            if (sq_val < 0.0) {
                opts->rtl_squelch_level = dB_to_pwr(sq_val);
            } else {
                opts->rtl_squelch_level = sq_val;
            }
        } else {
            goto RTLEND;
        }

        // curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl udp port "-U"
        // if (curr != NULL) opts->rtl_udp_port = atoi (curr);
        // else goto RTLEND;

        curr = dsd_strtok_r(NULL, ":", &saveptr); //rtl sample / volume multiplier
        if (curr != NULL) {
            opts->rtl_volume_multiplier = atoi(curr);
        } else {
            goto RTLEND;
        }

        // Optional trailing tokens: bias tee toggle
        while ((curr = dsd_strtok_r(NULL, ":", &saveptr)) != NULL) {
            if (strncmp(curr, "bias", 4) == 0 || strncmp(curr, "b", 1) == 0) {
                const char* val = strchr(curr, '=');
                int on = 1; // default enable if no explicit value
                if (val && *(val + 1)) {
                    val++; // move past '='
                    if (*val == '0' || *val == 'n' || *val == 'N' || *val == 'o' || *val == 'O' || *val == 'f'
                        || *val == 'F') {
                        on = 0;
                    }
                }
                opts->rtl_bias_tee = on;
            }
        }

    RTLEND:

        device_count = rtlsdr_get_device_count();
        if (!device_count) {
            LOG_ERROR("No supported devices found.\n");
            exitflag = 1;
        } else {
            LOG_NOTICE("Found %d device(s):\n", device_count);
        }
        for (int i = 0; i < device_count; i++) {
            rtlsdr_get_device_usb_strings(i, vendor, product, serial);
            LOG_NOTICE("  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
            if (opts->rtl_dev_index == i) {
                LOG_NOTICE("Selected Device #%d with Serial Number: %s \n", i, serial);
            }
        }

        // Guard against out-of-range index
        if (opts->rtl_dev_index < 0 || opts->rtl_dev_index >= device_count) {
            LOG_WARNING("Requested RTL device index %d out of range; using 0\n", opts->rtl_dev_index);
            opts->rtl_dev_index = 0;
        }

        if (opts->rtl_volume_multiplier > 3 || opts->rtl_volume_multiplier < 0) {
            opts->rtl_volume_multiplier = 1; //I wonder if you could flip polarity by using -1
        }

        LOG_NOTICE("RTL #%d: Freq=%d Gain=%d PPM=%d DSP-BW=%dkHz SQ=%.1fdB VOL=%d%s\n", opts->rtl_dev_index,
                   opts->rtlsdr_center_freq, opts->rtl_gain_value, opts->rtlsdr_ppm_error, opts->rtl_dsp_bw_khz,
                   pwr_to_dB(opts->rtl_squelch_level), opts->rtl_volume_multiplier,
                   opts->rtl_bias_tee ? " BIAS=on" : "");
        opts->audio_in_type = AUDIO_IN_RTL;

        rtl_ok = 1;
#endif

        if (rtl_ok == 0) //not set, means rtl support isn't compiled/available
        {
            LOG_ERROR("RTL Support not enabled/compiled, falling back to Pulse Audio Input.\n");
            sprintf(opts->audio_in_dev, "%s", "pulse");
            opts->audio_in_type = AUDIO_IN_PULSE;
        }
        UNUSED(vendor);
        UNUSED(product);
        UNUSED(serial);
        UNUSED(device_count);
    }

    if ((strncmp(opts->audio_in_dev, "pulse", 5) == 0)) {
        opts->audio_in_type = AUDIO_IN_PULSE;

        //string yeet
        parse_pulse_input_string(opts, opts->audio_in_dev + 5);
    }

    //UDP Socket Blaster Audio Output Setup
    if ((strncmp(opts->audio_out_dev, "udp", 3) == 0)) {

        //read in values
        LOG_NOTICE("UDP Blaster Output: ");
        char* curr;
        char* saveptr = NULL;
        char outbuf[1024];
        strncpy(outbuf, opts->audio_out_dev, sizeof(outbuf) - 1);
        outbuf[sizeof(outbuf) - 1] = '\0';

        curr = dsd_strtok_r(outbuf, ":", &saveptr); //should be 'udp'
        if (curr == NULL) {
            goto UDPEND; //end early with preset values
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //udp blaster hostname
        if (curr != NULL) {
            strncpy(opts->udp_hostname, curr, 1023); //set address to blast to
        }

        curr = dsd_strtok_r(NULL, ":", &saveptr); //udp blaster port
        if (curr != NULL) {
            opts->udp_portno = atoi(curr);
        }

    UDPEND:
        LOG_NOTICE("%s:", opts->udp_hostname);
        LOG_NOTICE("%d \n", opts->udp_portno);

        int err = udp_socket_connect(opts, state);
        if (err < 0) {
            LOG_ERROR("Error Configuring UDP Socket for UDP Blaster Audio :( \n");
            sprintf(opts->audio_out_dev, "%s", "pulse");
            opts->audio_out_type = 0;
        }

        opts->audio_out_type = 8;

        if (opts->monitor_input_audio == 1 || opts->frame_provoice == 1) {
            err = udp_socket_connectA(opts, state);
            if (err < 0) {
                LOG_ERROR("Error Configuring UDP Socket for UDP Blaster Audio Analog :( \n");
                opts->udp_sockfdA = DSD_INVALID_SOCKET;
                opts->monitor_input_audio = 0;
            } else {
                LOG_NOTICE("UDP Blaster Output (Analog): ");
                LOG_NOTICE("%s:", opts->udp_hostname);
                LOG_NOTICE("%d \n", opts->udp_portno + 2);
            }

            //this functionality is disabled when trunking EDACS, but we still use the behavior for analog channel monitoring
            if (opts->frame_provoice == 1 && opts->p25_trunk == 1) {
                opts->monitor_input_audio = 0;
            }
        }
    }

    if ((strncmp(opts->audio_out_dev, "pulse", 5) == 0)) {
        opts->audio_out_type = 0;

        //string yeet
        parse_pulse_output_string(opts, opts->audio_out_dev + 5);
    }

    if ((strncmp(opts->audio_out_dev, "null", 4) == 0)) {
        opts->audio_out_type = 9; //9 for NULL, or mute output
        opts->audio_out = 0;      //turn off so we won't playSynthesized
    }

    if ((strncmp(opts->audio_out_dev, "-", 1) == 0)) {
        opts->audio_out_fd = dsd_fileno(stdout); //DSD_STDOUT_FILENO;
        opts->audio_out_type = 1;                //using 1 for stdout to match input stdin as 1
        LOG_NOTICE("Audio Out Device: -\n");
    }

    if (opts->playfiles == 1) {
        opts->split = 1;
        opts->playoffset = 0;
        opts->playoffsetR = 0;
        opts->delay = 0;
        opts->pulse_digi_rate_out = 8000;
        opts->pulse_digi_out_channels = 1;
        if (opts->audio_out_type == 0) {
            openPulseOutput(opts);
        }
    }

    //this particular if-elseif-else could be rewritten to be a lot neater and simpler
    else if (strcmp(opts->audio_in_dev, opts->audio_out_dev) != 0) {
        opts->split = 1;
        opts->playoffset = 0;
        opts->playoffsetR = 0;
        opts->delay = 0;

        //open wav file should be handled directly by the -w switch now
        // if (strlen(opts->wav_out_file) > 0 && opts->dmr_stereo_wav == 0)
        //   openWavOutFile (opts, state);

        // else

        openAudioInDevice(opts);

        // fprintf (stderr,"Press CTRL + C to close.\n");
    }

    else {
        opts->split = 0;
        opts->playoffset = 0;
        opts->playoffsetR = 0;
        opts->delay = 0;
        openAudioInDevice(opts);
    }

    signal(SIGINT, handler);
    signal(SIGTERM, handler);

    //read in any user supplied M17 CAN and/or CSD data
    if ((strncmp(state->m17dat, "M17", 3) == 0)) {
        //read in values
        //string in format of M17:can:src_csd:dst_csd:input_rate

        //check and capatalize any letters in the CSD
        for (int i = 0; state->m17dat[i] != '\0'; i++) {
            if (state->m17dat[i] >= 'a' && state->m17dat[i] <= 'z') {
                state->m17dat[i] = state->m17dat[i] - 32;
            }
        }

        LOG_NOTICE("M17 User Data: ");
        char* curr;

        // if((strncmp(state->m17dat, "M17", 3) == 0))
        // goto M17END;

        curr = strtok(state->m17dat, ":"); //should be 'M17'
        if (curr != NULL)
            ; //continue
        else {
            goto M17END; //end early with preset values
        }

        curr = strtok(NULL, ":"); //m17 channel access number
        if (curr != NULL) {
            state->m17_can_en = atoi(curr);
        }

        curr = strtok(NULL, ":"); //m17 src address
        if (curr != NULL) {
            strncpy(state->str50c, curr, 9); //only read first 9
            state->str50c[9] = '\0';
        }

        curr = strtok(NULL, ":"); //m17 dst address
        if (curr != NULL) {
            strncpy(state->str50b, curr, 9); //only read first 9
            state->str50b[9] = '\0';
        }

        curr = strtok(NULL, ":"); //m17 input audio rate
        if (curr != NULL) {
            state->m17_rate = atoi(curr);
        }

        curr = strtok(NULL, ":"); //m17 vox enable
        if (curr != NULL) {
            state->m17_vox = atoi(curr);
        }

        // curr = strtok(NULL, ":"); //moved to in and out methods
        // if (curr != NULL)
        //   opts->m17_use_ip = atoi(curr);

    M17END:; //do nothing

        //check to make sure can value is no greater than 15 (4 bit value)
        if (state->m17_can_en > 15) {
            state->m17_can_en = 15;
        }

        //if vox is greater than 1, assume user meant 'yes' and set to one
        if (state->m17_vox > 1) {
            state->m17_vox = 1;
        }

        //debug print m17dat string
        // fprintf (stderr, " %s;", state->m17dat);

        LOG_NOTICE(" M17:%d:%s:%s:%d;", state->m17_can_en, state->str50c, state->str50b, state->m17_rate);
        if (state->m17_vox == 1) {
            LOG_NOTICE("VOX;");
        }
        LOG_NOTICE("\n");
    }

    if (opts->playfiles == 1) {

        // Use the effective argc (post long-option compaction) so the file
        // list aligns with state->optind from getopt.
        playMbeFiles(opts, state, argc_effective, argv);
    }

    else if (opts->m17encoder == 1) {
        //disable RRC filter for now
        opts->use_cosine_filter = 0;

        opts->pulse_digi_rate_out = 8000;

        //open any inputs, if not already opened
        if (opts->audio_in_type == AUDIO_IN_PULSE) {
            openPulseInput(opts);
        }

#ifdef USE_RTLSDR
        else if (opts->audio_in_type == AUDIO_IN_RTL) {
            if (state->rtl_ctx == NULL) {
                if (rtl_stream_create(opts, &state->rtl_ctx) < 0) {
                    LOG_ERROR("Failed to create RTL stream.\n");
                }
            }
            if (state->rtl_ctx && rtl_stream_start(state->rtl_ctx) < 0) {
                LOG_ERROR("Failed to open RTL-SDR stream.\n");
            }
            opts->rtl_started = 1;
        }
#endif

        //open any outputs, if not already opened
        if (opts->audio_out_type == 0) {
            openPulseOutput(opts);
        }
        // Start UI thread when ncurses UI is enabled so ncursesPrinter updates are rendered
        if (opts->use_ncurses_terminal == 1) {
            (void)ui_start(opts, state);
        }
        //All input and output now opened and handled correctly, so let's not break things by tweaking
        encodeM17STR(opts, state);
    }

    else if (opts->m17encoderbrt == 1) {
        opts->pulse_digi_rate_out = 8000;
        //open any outputs, if not already opened
        if (opts->audio_out_type == 0) {
            openPulseOutput(opts);
        }
        // Start UI thread when ncurses UI is enabled so ncursesPrinter updates are rendered
        if (opts->use_ncurses_terminal == 1) {
            (void)ui_start(opts, state);
        }
        encodeM17BRT(opts, state);
    }

    else if (opts->m17encoderpkt == 1) {
        //disable RRC filter for now
        opts->use_cosine_filter = 0;

        opts->pulse_digi_rate_out = 8000;
        //open any outputs, if not already opened
        if (opts->audio_out_type == 0) {
            openPulseOutput(opts);
        }
        // Start UI thread when ncurses UI is enabled so ncursesPrinter updates are rendered
        if (opts->use_ncurses_terminal == 1) {
            (void)ui_start(opts, state);
        }
        encodeM17PKT(opts, state);
    }

    else if (opts->m17decoderip == 1) {
        opts->pulse_digi_rate_out = 8000;
        //open any outputs, if not already opened
        if (opts->audio_out_type == 0) {
            openPulseOutput(opts);
        }
        // Start UI thread when ncurses UI is enabled so ncursesPrinter updates are rendered
        if (opts->use_ncurses_terminal == 1) {
            (void)ui_start(opts, state);
        }
        processM17IPF(opts, state);
    }

    else {
        // Start UI thread before entering main decode loop when enabled
        if (opts->use_ncurses_terminal == 1) {
            (void)ui_start(opts, state);
        }
        liveScanner(opts, state);
    }

    cleanupAndExit(opts, state);

    return (0);
}
