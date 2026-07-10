// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/csv_import.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/engine/frame_processing.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <dsd-neo/engine/trunk_tuning.h>
#include <dsd-neo/fec/block_codes.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/io/rigctl_client.h>
#include <dsd-neo/io/udp_input.h>
#include <dsd-neo/io/udp_socket_connect.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <dsd-neo/protocol/m17/m17.h>
#include <dsd-neo/protocol/nxdn/nxdn_convolution.h>
#include <dsd-neo/protocol/nxdn/nxdn_trunk_diag.h>
#include <dsd-neo/protocol/p25/p25_sm_watchdog.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/cli.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/control_pump.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_spec.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/rdio_export.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <errno.h>
#include <limits.h>
#include <mbelib.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_ext.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "dsd-neo/runtime/trunk_tuning_hooks.h"
#include "engine_hooks_install.h"

struct CODEC2;
#ifdef USE_RADIO
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#ifdef USE_RTLSDR
#include <rtl-sdr.h>
#endif

#ifdef USE_CODEC2
void codec2_destroy(struct CODEC2* codec2_state);
#endif

// Local caches to avoid redundant device I/O in hot paths
static long int s_last_rigctl_freq = -1;
static int s_last_rigctl_bw = -12345;
static uint64_t s_no_carrier_generic_recovery_request_id = 0U;
static const dsd_state* s_no_carrier_generic_recovery_state = NULL;
static long s_no_carrier_generic_recovery_cc = 0;
#ifdef USE_RADIO
static uint32_t s_last_rtl_freq = 0;
#endif

static void
reset_device_io_caches(void) {
    s_last_rigctl_freq = -1;
    s_last_rigctl_bw = -12345;
    s_no_carrier_generic_recovery_request_id = 0U;
    s_no_carrier_generic_recovery_state = NULL;
    s_no_carrier_generic_recovery_cc = 0;
    dsd_trunk_tuning_requests_reset();
#ifdef USE_RADIO
    s_last_rtl_freq = 0;
#endif
}

static int
dsd_parse_int_arg(const char* token, int* out) {
    if (!token || !out || token[0] == '\0') {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    long value = strtol(token, &end, 10);
    if (errno != 0 || end == token || value < INT_MIN || value > INT_MAX) {
        return -1;
    }
    *out = (int)value;
    return 0;
}

static int
dsd_parse_double_arg(const char* token, double* out) {
    if (!token || !out || token[0] == '\0') {
        return -1;
    }

    errno = 0;
    char* end = NULL;
    double value = strtod(token, &end);
    if (errno != 0 || end == token) {
        return -1;
    }
    *out = value;
    return 0;
}

#ifdef USE_RADIO
/*
 * The RTL FSK modem handles normal timing reacquisition locally. This fallback
 * clears queued RTL output, so keep it as a last-resort watchdog for sustained
 * no-sync periods instead of firing inside short DMR call gaps.
 */
static const double RTL_FSK_NO_SYNC_REACQUIRE_GAP_S = 10.000;
static const double RTL_FSK_NO_SYNC_REACQUIRE_COOLDOWN_S = 0.750;

static time_t
max_time_t(time_t a, time_t b) {
    return (a > b) ? a : b;
}

static double
max_double(double a, double b) {
    return (a > b) ? a : b;
}

static int
rtl_fsk_reacquire_output_is_fsk(void) {
    int output_kind = dsd_rtl_stream_metrics_hook_output_kind();
    if (output_kind == 0) {
        output_kind = rtl_stream_get_output_kind();
    }
    return output_kind == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR;
}

static void
rtl_fsk_reacquire_mark_sync(dsd_state* state, time_t now, double nowm) {
    state->rtl_fsk_reacquire_last_sync_time = now;
    state->rtl_fsk_reacquire_last_sync_m = nowm;
    state->rtl_fsk_reacquire_gap_start_m = 0.0;
}

static int
rtl_fsk_reacquire_seed_if_needed(dsd_state* state, time_t latest_sync_time, double latest_sync_m, double nowm) {
    if (state->rtl_fsk_reacquire_last_sync_time != 0 || state->rtl_fsk_reacquire_last_sync_m > 0.0) {
        return 0;
    }
    state->rtl_fsk_reacquire_last_sync_time = latest_sync_time;
    state->rtl_fsk_reacquire_last_sync_m = (latest_sync_m > 0.0) ? latest_sync_m : nowm;
    state->rtl_fsk_reacquire_gap_start_m = nowm;
    return 1;
}

static int
rtl_fsk_reacquire_update_if_new_sync(dsd_state* state, time_t latest_sync_time, double latest_sync_m) {
    if (latest_sync_time <= state->rtl_fsk_reacquire_last_sync_time
        && latest_sync_m <= state->rtl_fsk_reacquire_last_sync_m) {
        return 0;
    }
    state->rtl_fsk_reacquire_last_sync_time = latest_sync_time;
    state->rtl_fsk_reacquire_last_sync_m = latest_sync_m;
    state->rtl_fsk_reacquire_gap_start_m = 0.0;
    return 1;
}

static int
rtl_fsk_reacquire_gap_ready(const dsd_state* state, double nowm, double gap_s, double cooldown_s) {
    if (state->rtl_fsk_reacquire_gap_start_m <= 0.0) {
        return 0;
    }
    if ((nowm - state->rtl_fsk_reacquire_gap_start_m) < gap_s) {
        return 0;
    }
    if (state->rtl_fsk_reacquire_last_request_m > 0.0
        && (nowm - state->rtl_fsk_reacquire_last_request_m) < cooldown_s) {
        return 0;
    }
    return 1;
}

static void
maybe_request_rtl_fsk_reacquire_on_no_sync(const dsd_opts* opts, dsd_state* state, time_t now) {
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return;
    }
    if (!rtl_fsk_reacquire_output_is_fsk()) {
        state->rtl_fsk_reacquire_gap_start_m = 0.0;
        return;
    }

    double nowm = dsd_time_now_monotonic_s();
    time_t latest_sync_time = max_time_t(state->last_cc_sync_time, state->last_vc_sync_time);
    double latest_sync_m = max_double(state->last_cc_sync_time_m, state->last_vc_sync_time_m);

    if (state->lastsynctype != DSD_SYNC_NONE) {
        rtl_fsk_reacquire_mark_sync(state, now, nowm);
        return;
    }

    if (rtl_fsk_reacquire_seed_if_needed(state, latest_sync_time, latest_sync_m, nowm)) {
        return;
    }

    if (rtl_fsk_reacquire_update_if_new_sync(state, latest_sync_time, latest_sync_m)) {
        return;
    }

    if (state->rtl_fsk_reacquire_gap_start_m <= 0.0) {
        state->rtl_fsk_reacquire_gap_start_m = nowm;
        return;
    }
    if (!rtl_fsk_reacquire_gap_ready(state, nowm, RTL_FSK_NO_SYNC_REACQUIRE_GAP_S,
                                     RTL_FSK_NO_SYNC_REACQUIRE_COOLDOWN_S)) {
        return;
    }
    if (rtl_stream_request_fsk_reacquire() > 0) {
        state->rtl_fsk_reacquire_last_request_m = nowm;
    }
}
#endif

// Small helpers to efficiently set fixed-width strings
static inline void
set_spaces(char* buf, size_t count) {
    DSD_MEMSET(buf, ' ', count);
    buf[count] = '\0';
}

static inline void
set_underscores(char* buf, size_t count) {
    DSD_MEMSET(buf, '_', count);
    buf[count] = '\0';
}

static void
autosave_user_config(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (!state->config_autosave_enabled) {
        return;
    }

    const char* path = NULL;
    if (state->config_autosave_path[0] != '\0') {
        path = state->config_autosave_path;
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
import_global_channel_map_if_needed(dsd_opts* opts, dsd_state* state) {
    const int trunk_or_scan = (opts->trunk_enable == 1) || (opts->p25_trunk == 1) || (opts->scanner_mode == 1);
    if (opts->trunk_scan_enabled == 1 && opts->chan_in_file[0] != '\0') {
        LOG_ERROR("Trunk scan does not allow global channel maps; use per-target chan_csv values.\n");
        return -1;
    }

    if (trunk_or_scan && opts->trunk_scan_enabled != 1 && opts->chan_in_file[0] != '\0' && state->lcn_freq_count == 0) {
        if (csvChanImport(opts, state) != 0) {
            return -1;
        }
        LOG_NOTICE("Imported channel map from %s\n", opts->chan_in_file);
    }
    return 0;
}

static int
import_group_csv_if_needed(dsd_opts* opts, dsd_state* state) {
    const int trunk_enabled = (opts->trunk_enable == 1) || (opts->p25_trunk == 1) || (opts->trunk_scan_enabled == 1);
    if (trunk_enabled && opts->group_in_file[0] != '\0' && !dsd_tg_policy_has_entries(state)) {
        if (csvGroupImport(opts, state) != 0) {
            return -1;
        }
        LOG_NOTICE("Imported group list from %s\n", opts->group_in_file);
    }
    return 0;
}

static int
import_trunking_csvs_if_needed(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return 0;
    }
    if (import_global_channel_map_if_needed(opts, state) != 0) {
        return -1;
    }
    return import_group_csv_if_needed(opts, state);
}

static void
open_recording_outputs_if_needed(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    // Per-call WAV (-P) and static WAV (-w) are mutually exclusive.
    if (opts->dmr_stereo_wav == 1) {
        opts->static_wav_file = 0;
    }

    if (opts->dmr_stereo_wav == 1 && opts->wav_out_f == NULL && opts->wav_out_fR == NULL) {
        dsd_stat_t st;
        char wav_file_directory[1024];
        DSD_SNPRINTF(wav_file_directory, sizeof wav_file_directory, "%s", opts->wav_out_dir);
        wav_file_directory[sizeof wav_file_directory - 1] = '\0';
        if (dsd_stat_path(wav_file_directory, &st) == -1) {
            LOG_NOTICE("Creating directory %s to save decoded wav files\n", wav_file_directory);
            dsd_mkdir(wav_file_directory, 0700);
        }
        opts->wav_out_f = open_wav_file(opts->wav_out_dir, opts->wav_out_file, sizeof opts->wav_out_file, 8000, 0);
        opts->wav_out_fR = open_wav_file(opts->wav_out_dir, opts->wav_out_fileR, sizeof opts->wav_out_fileR, 8000, 0);
    } else if (opts->static_wav_file == 1 && opts->wav_out_f == NULL && opts->wav_out_file[0] != '\0') {
        openWavOutFileLR(opts, state);
    }

    if (opts->wav_out_file_raw[0] != '\0' && opts->wav_out_raw == NULL) {
        openWavOutFileRaw(opts, state);
    }
}

static int
analog_filter_rate_hz(const dsd_opts* opts, const dsd_state* state) {
#ifndef USE_RADIO
    UNUSED(state);
#endif
    if (!opts) {
        return 48000;
    }
#ifdef USE_RADIO
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
                return dsd_opts_effective_input_rate(opts);
            }
            break;
        default: break;
    }
    if (opts->pulse_raw_rate_out > 0) {
        return opts->pulse_raw_rate_out;
    }
    return 48000;
}

static void
dsd_engine_signal_handler(int sgnl) {
    UNUSED(sgnl);

    exitflag = 1;
}

static double
atofs(const char* s) {
    size_t len = strlen(s);
    if (len == 0) {
        return 0.0;
    }

    double value = 0.0;

    char last = s[len - 1];
    double factor = 1.0;

    switch (last) {
        case 'g':
        case 'G': factor = 1e9; break;
        case 'm':
        case 'M': factor = 1e6; break;
        case 'k':
        case 'K': factor = 1e3; break;
        default:
            if (dsd_parse_double_arg(s, &value) != 0) {
                return 0.0;
            }
            return value;
    }

    if (len == 1) {
        return 0.0;
    }

    char unitless[1024];
    if (len >= sizeof(unitless)) {
        return 0.0;
    }
    DSD_MEMCPY(unitless, s, len - 1);
    unitless[len - 1] = '\0';
    if (dsd_parse_double_arg(unitless, &value) != 0) {
        return 0.0;
    }
    return value * factor;
}

static void
dsd_engine_setup_copy_spec(char* dst, size_t dst_size, const char* src) {
    if (!dst || dst_size == 0 || !src) {
        return;
    }
    DSD_SNPRINTF(dst, dst_size, "%s", src);
    dst[dst_size - 1] = '\0';
}

static int
dsd_engine_setup_parse_int_token(const char* token, int* out) {
    int parsed = 0;
    if (!token) {
        return -1;
    }
    if (dsd_parse_int_arg(token, &parsed) != 0) {
        return -1;
    }
    *out = parsed;
    return 0;
}

static int
dsd_engine_setup_parse_bw_token_or_default(const char* token) {
    int bw = 0;
    if (token && dsd_parse_int_arg(token, &bw) != 0) {
        bw = 0;
    }
    if (bw == 4 || bw == 6 || bw == 8 || bw == 12 || bw == 16 || bw == 24 || bw == 48) {
        return bw;
    }
    return 48;
}

static double
dsd_engine_setup_parse_sql_token_or_default(const char* token, double fallback) {
    if (!token) {
        return fallback;
    }
    double sq_val = 0.0;
    (void)dsd_parse_double_arg(token, &sq_val);
    if (sq_val < 0.0) {
        return dB_to_pwr(sq_val);
    }
    return sq_val;
}

static void
dsd_engine_setup_parse_bias_token(dsd_opts* opts, const char* token) {
    if (!token) {
        return;
    }
    if (strncmp(token, "bias", 4) != 0 && strncmp(token, "b", 1) != 0) {
        return;
    }
    const char* val = strchr(token, '=');
    int on = 1;
    if (val && *(val + 1)) {
        val++;
        if (*val == '0' || *val == 'n' || *val == 'N' || *val == 'o' || *val == 'O' || *val == 'f' || *val == 'F') {
            on = 0;
        }
    }
    opts->rtl_bias_tee = on;
}

static int
dsd_engine_setup_parse_m17_udp_input(dsd_opts* opts) {
    if (!dsd_opts_audio_in_dev_is_m17udp_spec(opts->audio_in_dev)) {
        return 0;
    }
    LOG_NOTICE("M17 UDP IP Frame Input: ");
    char* saveptr = NULL;
    char inbuf[1024];
    dsd_engine_setup_copy_spec(inbuf, sizeof(inbuf), opts->audio_in_dev);
    if (dsd_strtok_r(inbuf, ":", &saveptr) != NULL) {
        const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            DSD_STRNCPY(opts->m17_hostname, curr, 1023);
        }
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            int parsed = 0;
            if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
                opts->m17_portno = parsed;
            }
        }
    }
    LOG_NOTICE("%s:", opts->m17_hostname);
    LOG_NOTICE("%d \n", opts->m17_portno);
    return 0;
}

static int
dsd_engine_setup_parse_udp_input(dsd_opts* opts) {
    if (!dsd_opts_audio_in_dev_is_udp_spec(opts->audio_in_dev)) {
        return 0;
    }
    LOG_NOTICE("UDP Direct Input: ");
    char* saveptr = NULL;
    char inbuf[1024];
    dsd_engine_setup_copy_spec(inbuf, sizeof(inbuf), opts->audio_in_dev);
    if (dsd_strtok_r(inbuf, ":", &saveptr) != NULL) {
        const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            DSD_STRNCPY(opts->udp_in_bindaddr, curr, 1023);
            opts->udp_in_bindaddr[1023] = '\0';
        }
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            int parsed = 0;
            if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
                opts->udp_in_portno = parsed;
            }
        }
    }
    if (opts->udp_in_portno == 0) {
        opts->udp_in_portno = 7355;
    }
    if (opts->udp_in_bindaddr[0] == '\0') {
        DSD_SNPRINTF(opts->udp_in_bindaddr, sizeof(opts->udp_in_bindaddr), "%s", "127.0.0.1");
    }
    LOG_NOTICE("%s:%d\n", opts->udp_in_bindaddr, opts->udp_in_portno);
    return 0;
}

static int
dsd_engine_setup_parse_m17_udp_output(dsd_opts* opts) {
    if (strncmp(opts->audio_out_dev, "m17udp", 6) != 0) {
        return 0;
    }
    LOG_NOTICE("M17 UDP IP Frame Output: ");
    char* saveptr = NULL;
    char outbuf[1024];
    dsd_engine_setup_copy_spec(outbuf, sizeof(outbuf), opts->audio_out_dev);
    if (dsd_strtok_r(outbuf, ":", &saveptr) != NULL) {
        const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            DSD_STRNCPY(opts->m17_hostname, curr, 1023);
        }
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            int parsed = 0;
            if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
                opts->m17_portno = parsed;
            }
        }
    }
    LOG_NOTICE("%s:", opts->m17_hostname);
    LOG_NOTICE("%d \n", opts->m17_portno);
    opts->m17_use_ip = 1;
    opts->audio_out_type = 9;
    return 0;
}

static int
dsd_engine_setup_parse_tcp_input(dsd_opts* opts, dsd_state* state) {
    if (!dsd_opts_audio_in_dev_is_tcp_spec(opts->audio_in_dev)) {
        return 0;
    }
    LOG_NOTICE("TCP Direct Link: ");
    char* saveptr = NULL;
    char inbuf[1024];
    dsd_engine_setup_copy_spec(inbuf, sizeof(inbuf), opts->audio_in_dev);

    if (dsd_strtok_r(inbuf, ":", &saveptr) != NULL) {
        const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            DSD_STRNCPY(opts->tcp_hostname, curr, 1023);
            DSD_MEMCPY(opts->rigctlhostname, opts->tcp_hostname, sizeof(opts->rigctlhostname));
        }
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            int parsed = 0;
            if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
                opts->tcp_portno = parsed;
            }
        }
    }

    while (1) {
        if (exitflag == 1) {
            cleanupAndExit(opts, state);
            return 1;
        }
        LOG_NOTICE("%s:", opts->tcp_hostname);
        LOG_NOTICE("%d \n", opts->tcp_portno);
        opts->tcp_sockfd = Connect(opts->tcp_hostname, opts->tcp_portno);
        if (opts->tcp_sockfd != DSD_INVALID_SOCKET) {
            opts->audio_in_type = AUDIO_IN_TCP;
            LOG_NOTICE("TCP Connection Success!\n");
            return 0;
        }
        if (opts->frame_m17 == 1) {
            dsd_sleep_ms(1000);
            continue;
        }
        DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "pulse");
        LOG_ERROR("TCP Connection Failure - Using %s Audio Input.\n", opts->audio_in_dev);
        opts->audio_in_type = AUDIO_IN_PULSE;
        return 0;
    }
}

static void
dsd_engine_setup_connect_rigctl_if_enabled(dsd_opts* opts) {
    if (opts->use_rigctl != 1) {
        return;
    }
    opts->rigctl_sockfd = Connect(opts->rigctlhostname, opts->rigctlportno);
    if (opts->rigctl_sockfd != DSD_INVALID_SOCKET) {
        opts->use_rigctl = 1;
        return;
    }
    LOG_ERROR("RIGCTL Connection Failure - RIGCTL Features Disabled\n");
    opts->use_rigctl = 0;
}

static void
dsd_engine_setup_enable_iq_replay_if_selected(dsd_opts* opts) {
    if (!dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev)) {
        return;
    }
    const char* replay_path = opts->audio_in_dev;
    const char* colon = strchr(opts->audio_in_dev, ':');
    if (colon && colon[1] != '\0') {
        replay_path = colon + 1;
    }
    LOG_NOTICE("IQ Replay Input: %s\n", replay_path);
    opts->rtltcp_enabled = 0;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->iq_replay_active = 1;
}

static int
dsd_engine_setup_parse_rtltcp_tuning_tokens(dsd_opts* opts, char** saveptr) {
    const char* curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtlsdr_center_freq = (uint32_t)atofs(curr);

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    int parsed = 0;
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtl_gain_value = parsed;
    }

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtlsdr_ppm_error = parsed;
    }

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtl_dsp_bw_khz = dsd_engine_setup_parse_bw_token_or_default(curr);

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtl_squelch_level = dsd_engine_setup_parse_sql_token_or_default(curr, opts->rtl_squelch_level);

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtl_volume_multiplier = parsed;
    }
    return 1;
}

static void
dsd_engine_setup_parse_rtltcp_input(dsd_opts* opts) {
    if (!dsd_opts_audio_in_dev_is_rtltcp_spec(opts->audio_in_dev)) {
        return;
    }
    LOG_NOTICE("RTL_TCP Input: ");
    char* saveptr = NULL;
    char inbuf[1024];
    dsd_engine_setup_copy_spec(inbuf, sizeof(inbuf), opts->audio_in_dev);

    if (dsd_strtok_r(inbuf, ":", &saveptr) != NULL) {
        const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            DSD_STRNCPY(opts->rtltcp_hostname, curr, 1023);
        }
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            int parsed = 0;
            if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
                opts->rtltcp_portno = parsed;
            }
        }
        if (dsd_engine_setup_parse_rtltcp_tuning_tokens(opts, &saveptr)) {
            while ((curr = dsd_strtok_r(NULL, ":", &saveptr)) != NULL) {
                dsd_engine_setup_parse_bias_token(opts, curr);
            }
        }
    }

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
    opts->audio_in_type = AUDIO_IN_RTL;
}

static void
dsd_engine_setup_parse_soapy_input(dsd_opts* opts) {
    if (!dsd_opts_audio_in_dev_is_soapy_spec(opts->audio_in_dev)) {
        return;
    }
    int tuning_applied = 0;
    (void)dsd_normalize_soapy_input_spec(opts, &tuning_applied);
    const char* soapy_args = "";
    if (strncmp(opts->audio_in_dev, "soapy:", 6) == 0) {
        soapy_args = opts->audio_in_dev + 6;
    }
    LOG_NOTICE("SoapySDR Input");
    if (soapy_args[0] != '\0') {
        LOG_NOTICE(": %s\n", soapy_args);
    } else {
        LOG_NOTICE(": default device args\n");
    }
    if (tuning_applied) {
        LOG_NOTICE("SoapySDR tuning: Freq=%u Gain=%d PPM=%d DSP-BW=%dkHz SQ=%.1fdB VOL=%d\n", opts->rtlsdr_center_freq,
                   opts->rtl_gain_value, opts->rtlsdr_ppm_error, opts->rtl_dsp_bw_khz,
                   pwr_to_dB(opts->rtl_squelch_level), opts->rtl_volume_multiplier);
    }
    opts->rtltcp_enabled = 0;
    opts->audio_in_type = AUDIO_IN_RTL;
}

#ifdef USE_RTLSDR
static int
dsd_engine_setup_parse_rtl_spec_tokens(dsd_opts* opts, char** saveptr) {
    const char* curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    int parsed = 0;
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtl_dev_index = parsed;
    }

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtlsdr_center_freq = (uint32_t)atofs(curr);

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtl_gain_value = parsed;
    }

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtlsdr_ppm_error = parsed;
    }

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtl_dsp_bw_khz = dsd_engine_setup_parse_bw_token_or_default(curr);

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    opts->rtl_squelch_level = dsd_engine_setup_parse_sql_token_or_default(curr, opts->rtl_squelch_level);

    curr = dsd_strtok_r(NULL, ":", saveptr);
    if (!curr) {
        return 0;
    }
    if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
        opts->rtl_volume_multiplier = parsed;
    }

    while ((curr = dsd_strtok_r(NULL, ":", saveptr)) != NULL) {
        dsd_engine_setup_parse_bias_token(opts, curr);
    }
    return 1;
}

static void
dsd_engine_setup_update_rtl_spec_with_selected_index(dsd_opts* opts) {
    if (strncmp(opts->audio_in_dev, "rtl:", 4) != 0) {
        return;
    }
    const char* rest = strchr(opts->audio_in_dev + 4, ':');
    if (rest) {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtl:%d%s", opts->rtl_dev_index, rest);
    } else {
        DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "rtl:%d", opts->rtl_dev_index);
    }
    opts->audio_in_dev[sizeof opts->audio_in_dev - 1] = '\0';
}

static int
dsd_engine_setup_enumerate_rtl_devices(const dsd_opts* opts, char* vendor, char* product, char* serial) {
    int device_count = 0;
#if defined(_MSC_VER) && defined(_WIN32)
    __try {
        device_count = (int)rtlsdr_get_device_count();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_ERROR("RTL: libusb exception during device enumeration.\n");
        device_count = 0;
        exitflag = 1;
    }
#else
    device_count = (int)rtlsdr_get_device_count();
#endif
    // cppcheck-suppress knownConditionTrueFalse -- cppcheck does not model MSVC __try assignments.
    if (device_count == 0) {
        LOG_ERROR("No supported devices found.\n");
        exitflag = 1;
        return device_count;
    }

    LOG_NOTICE("Found %d device(s):\n", device_count);
    for (int i = 0; i < device_count; i++) {
#if defined(_MSC_VER) && defined(_WIN32)
        __try {
            (void)rtlsdr_get_device_usb_strings((uint32_t)i, vendor, product, serial);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            DSD_SNPRINTF(vendor, 256, "%s", "unknown");
            DSD_SNPRINTF(product, 256, "%s", "unknown");
            DSD_SNPRINTF(serial, 256, "%s", "unknown");
        }
#else
        (void)rtlsdr_get_device_usb_strings((uint32_t)i, vendor, product, serial);
#endif
        LOG_NOTICE("  %d:  %s, %s, SN: %s\n", i, vendor, product, serial);
        if (opts->rtl_dev_index == i) {
            LOG_NOTICE("Selected Device #%d with Serial Number: %s \n", i, serial);
        }
    }
    return device_count;
}
#endif

static int
dsd_engine_setup_configure_local_rtl(dsd_opts* opts, dsd_state* state, char* vendor, char* product, char* serial) {
    UNUSED(state);
#ifdef USE_RTLSDR
    LOG_NOTICE("RTL Input: ");
    char* saveptr = NULL;
    char inbuf[1024];
    dsd_engine_setup_copy_spec(inbuf, sizeof(inbuf), opts->audio_in_dev);
    if (dsd_strtok_r(inbuf, ":", &saveptr) != NULL) {
        (void)dsd_engine_setup_parse_rtl_spec_tokens(opts, &saveptr);
    }

    int device_count = dsd_engine_setup_enumerate_rtl_devices(opts, vendor, product, serial);
    if (device_count <= 0) {
        return 0;
    }
    if (opts->rtl_dev_index < 0 || opts->rtl_dev_index >= device_count) {
        const int requested = opts->rtl_dev_index;
        LOG_WARNING("Requested RTL device index %d out of range (found %d device(s)); using 0\n", requested,
                    device_count);
        opts->rtl_dev_index = 0;
        dsd_engine_setup_update_rtl_spec_with_selected_index(opts);
    }

    if (opts->rtl_volume_multiplier > 3 || opts->rtl_volume_multiplier < 0) {
        opts->rtl_volume_multiplier = 1;
    }
    LOG_NOTICE("RTL #%d: Freq=%d Gain=%d PPM=%d DSP-BW=%dkHz SQ=%.1fdB VOL=%d%s\n", opts->rtl_dev_index,
               opts->rtlsdr_center_freq, opts->rtl_gain_value, opts->rtlsdr_ppm_error, opts->rtl_dsp_bw_khz,
               pwr_to_dB(opts->rtl_squelch_level), opts->rtl_volume_multiplier, opts->rtl_bias_tee ? " BIAS=on" : "");
    opts->audio_in_type = AUDIO_IN_RTL;
    return 1;
#else
    UNUSED(opts);
    UNUSED(vendor);
    UNUSED(product);
    UNUSED(serial);
    return 0;
#endif
}

static void
dsd_engine_setup_parse_rtl_input(dsd_opts* opts, dsd_state* state) {
    if (!dsd_opts_audio_in_dev_is_rtl_spec(opts->audio_in_dev)) {
        return;
    }

    char vendor[256];
    char product[256];
    char serial[256];
    int rtl_ok = dsd_engine_setup_configure_local_rtl(opts, state, vendor, product, serial);
#ifdef USE_RTLSDR
    if (rtl_ok != 1) {
        LOG_ERROR("RTL Support not enabled/compiled, falling back to Pulse Audio Input.\n");
        DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "pulse");
        opts->audio_in_type = AUDIO_IN_PULSE;
    }
#else
    UNUSED(rtl_ok);
    LOG_ERROR("RTL Support not enabled/compiled, falling back to Pulse Audio Input.\n");
    DSD_SNPRINTF(opts->audio_in_dev, sizeof(opts->audio_in_dev), "%s", "pulse");
    opts->audio_in_type = AUDIO_IN_PULSE;
#endif
    UNUSED(vendor);
    UNUSED(product);
    UNUSED(serial);
}

static void
dsd_engine_setup_parse_pulse_input(dsd_opts* opts) {
    if (!dsd_opts_audio_in_dev_is_pulse_spec(opts->audio_in_dev)) {
        return;
    }
    opts->audio_in_type = AUDIO_IN_PULSE;
    parse_audio_input_string(opts, opts->audio_in_dev + 5);
}

static void
dsd_engine_setup_parse_udp_output(dsd_opts* opts, dsd_state* state) {
    if (strncmp(opts->audio_out_dev, "udp", 3) != 0) {
        return;
    }
    LOG_NOTICE("UDP Blaster Output: ");
    char* saveptr = NULL;
    char outbuf[1024];
    dsd_engine_setup_copy_spec(outbuf, sizeof(outbuf), opts->audio_out_dev);
    if (dsd_strtok_r(outbuf, ":", &saveptr) != NULL) {
        const char* curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            DSD_STRNCPY(opts->udp_hostname, curr, 1023);
        }
        curr = dsd_strtok_r(NULL, ":", &saveptr);
        if (curr != NULL) {
            int parsed = 0;
            if (dsd_engine_setup_parse_int_token(curr, &parsed) == 0) {
                opts->udp_portno = parsed;
            }
        }
    }

    LOG_NOTICE("%s:", opts->udp_hostname);
    LOG_NOTICE("%d \n", opts->udp_portno);
    int err = udp_socket_connect(opts, state);
    if (err < 0) {
        LOG_ERROR("Error Configuring UDP Socket for UDP Blaster Audio :( \n");
        DSD_SNPRINTF(opts->audio_out_dev, sizeof(opts->audio_out_dev), "%s", "pulse");
        opts->audio_out_type = 0;
    }

    opts->audio_out_type = 8;
    if (opts->monitor_input_audio != 1 && opts->frame_provoice != 1) {
        return;
    }

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
    if (opts->frame_provoice == 1 && opts->p25_trunk == 1) {
        opts->monitor_input_audio = 0;
    }
}

static void
dsd_engine_setup_parse_simple_outputs(dsd_opts* opts) {
    if (strncmp(opts->audio_out_dev, "pulse", 5) == 0) {
        opts->audio_out_type = 0;
        parse_audio_output_string(opts, opts->audio_out_dev + 5);
    }
    if (strncmp(opts->audio_out_dev, "null", 4) == 0) {
        opts->audio_out_type = 9;
        opts->audio_out = 0;
    }
    if (strncmp(opts->audio_out_dev, "-", 1) == 0) {
        opts->audio_out_fd = dsd_fileno(stdout);
        opts->audio_out_type = 1;
        LOG_NOTICE("Audio Out Device: -\n");
    }
}

static int
dsd_engine_setup_open_audio_paths(dsd_opts* opts, dsd_state* state) {
    opts->playoffset = 0;
    opts->playoffsetR = 0;
    opts->delay = 0;

    if (opts->playfiles == 1) {
        opts->split = 1;
        opts->pulse_digi_rate_out = 8000;
        opts->pulse_digi_out_channels = 1;
        if (opts->audio_out_type == 0 && openAudioOutput(opts) != 0) {
            return -1;
        }
        return 0;
    }

    opts->split = (strcmp(opts->audio_in_dev, opts->audio_out_dev) != 0) ? 1 : 0;
    if (openAudioInDevice(opts, state) != 0) {
        return -1;
    }
    return 0;
}

static int
dsd_engine_setup_io(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }

    if (dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev) && !opts->iq_replay_requested) {
        LOG_ERROR("Direct -i iqreplay:... is not supported. Use --iq-replay <path>.\n");
        return -1;
    }
    opts->iq_replay_active = dsd_opts_audio_in_dev_is_iqreplay_spec(opts->audio_in_dev) ? 1 : 0;
    (void)dsd_engine_setup_parse_m17_udp_input(opts);
    (void)dsd_engine_setup_parse_udp_input(opts);
    (void)dsd_engine_setup_parse_m17_udp_output(opts);
    if (dsd_engine_setup_parse_tcp_input(opts, state) != 0) {
        return 0;
    }
    dsd_engine_setup_connect_rigctl_if_enabled(opts);
    dsd_engine_setup_enable_iq_replay_if_selected(opts);
    dsd_engine_setup_parse_rtltcp_input(opts);
    dsd_engine_setup_parse_soapy_input(opts);
    dsd_engine_setup_parse_rtl_input(opts, state);
    dsd_engine_setup_parse_pulse_input(opts);
    dsd_engine_setup_parse_udp_output(opts, state);
    dsd_engine_setup_parse_simple_outputs(opts);
    return dsd_engine_setup_open_audio_paths(opts, state);
}

static void
m17_uppercase_inplace(char* s) {
    if (!s) {
        return;
    }
    for (int i = 0; s[i] != '\0'; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            s[i] = (char)(s[i] - 32);
        }
    }
}

static void
m17_parse_userdata_fields(dsd_state* state) {
    char* saveptr = NULL;
    const char* curr = dsd_strtok_r(state->m17dat, ":", &saveptr);
    if (curr == NULL) {
        return;
    }

    curr = dsd_strtok_r(NULL, ":", &saveptr);
    if (curr != NULL) {
        int parsed = 0;
        if (dsd_parse_int_arg(curr, &parsed) == 0) {
            state->m17_can_en = parsed;
        }
    }

    curr = dsd_strtok_r(NULL, ":", &saveptr);
    if (curr != NULL) {
        DSD_STRNCPY(state->str50c, curr, 9);
        state->str50c[9] = '\0';
    }

    curr = dsd_strtok_r(NULL, ":", &saveptr);
    if (curr != NULL) {
        DSD_STRNCPY(state->str50b, curr, 9);
        state->str50b[9] = '\0';
    }

    curr = dsd_strtok_r(NULL, ":", &saveptr);
    if (curr != NULL) {
        int parsed = 0;
        if (dsd_parse_int_arg(curr, &parsed) == 0) {
            state->m17_rate = parsed;
        }
    }

    curr = dsd_strtok_r(NULL, ":", &saveptr);
    if (curr != NULL) {
        int parsed = 0;
        if (dsd_parse_int_arg(curr, &parsed) == 0) {
            state->m17_vox = parsed;
        }
    }
}

static void
m17_finalize_userdata_log(dsd_state* state) {
    if (state->m17_can_en > 15) {
        state->m17_can_en = 15;
    }
    if (state->m17_vox > 1) {
        state->m17_vox = 1;
    }
    LOG_NOTICE(" M17:%d:%s:%s:%d;", state->m17_can_en, state->str50c, state->str50b, state->m17_rate);
    if (state->m17_vox == 1) {
        LOG_NOTICE("VOX;");
    }
    LOG_NOTICE("\n");
}

static void
dsd_engine_parse_m17_userdata(dsd_opts* opts, dsd_state* state) {
    UNUSED(opts);

    if (strncmp(state->m17dat, "M17", 3) != 0) {
        return;
    }

    LOG_NOTICE("M17 User Data: ");
    m17_uppercase_inplace(state->m17dat);
    m17_parse_userdata_fields(state);
    m17_finalize_userdata_log(state);
}

static void
no_carrier_rotate_symbol_output_if_needed(dsd_opts* opts, dsd_state* state) {
    if (opts->symbol_out_f && opts->symbol_out_file_is_auto == 1) {
        rotate_symbol_out_file(opts, state);
    }
}

static void
no_carrier_reset_floating_gain_if_needed(const dsd_opts* opts, dsd_state* state) {
    if (opts->floating_point == 1) {
        state->aout_gain = opts->audio_gain;
        state->aout_gainR = opts->audio_gain;
    }
}

static void
no_carrier_reset_nxdn_scan_markers(dsd_state* state) {
    state->nxdn_last_ran = -1;
    state->nxdn_last_rid = 0;
    state->nxdn_last_tg = 0;
}

static void
no_carrier_tune_rigctl_if_needed(const dsd_opts* opts, long int freq) {
    if (opts->use_rigctl != 1) {
        return;
    }
    if (opts->setmod_bw != 0 && opts->setmod_bw != s_last_rigctl_bw) {
        SetModulation(opts->rigctl_sockfd, opts->setmod_bw);
        s_last_rigctl_bw = opts->setmod_bw;
    }
    if (freq != s_last_rigctl_freq) {
        SetFreq(opts->rigctl_sockfd, freq);
        s_last_rigctl_freq = freq;
    }
}

#ifdef USE_RADIO
static int
no_carrier_tune_rtl_if_needed(const dsd_opts* opts, dsd_state* state, uint32_t rf) {
    if (opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx) {
        return 1;
    }
    if (rf != s_last_rtl_freq) {
        int tune_result = rtl_stream_tune(state->rtl_ctx, rf);
        if (tune_result == RTL_STREAM_TUNE_DEFERRED) {
            return 0;
        }
        s_last_rtl_freq = rf;
    }
    return 1;
}
#endif

static void
no_carrier_step_scanner_mode_if_needed(const dsd_opts* opts, dsd_state* state, time_t now) {
    if (opts->scanner_mode != 1 || (now - state->last_cc_sync_time) <= opts->trunk_hangtime) {
        return;
    }

    no_carrier_reset_nxdn_scan_markers(state);
    if (state->lcn_freq_roll >= state->lcn_freq_count) {
        state->lcn_freq_roll = 0;
    }

    long int freq = state->trunk_lcn_freq[state->lcn_freq_roll];
    if (freq != 0) {
        no_carrier_tune_rigctl_if_needed(opts, freq);
        if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RADIO
            if (!no_carrier_tune_rtl_if_needed(opts, state, (uint32_t)freq)) {
                return;
            }
#endif
        }
    }
    state->lcn_freq_roll++;
    state->last_cc_sync_time = now;
    state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
}

static int
no_carrier_is_cc_return_due(const dsd_opts* opts, const dsd_state* state, time_t now) {
    if ((opts->trunk_enable != 1 && opts->p25_trunk != 1) || (opts->trunk_is_tuned != 1 && opts->p25_is_tuned != 1)) {
        return 0;
    }

    double dt = (state->last_vc_sync_time == 0) ? 1e9 : (double)(now - state->last_vc_sync_time);
    return dt > opts->trunk_hangtime;
}

static int
no_carrier_has_mapped_dmr_rest_channel(const dsd_state* state) {
    if (state->dmr_rest_channel < 0 || state->dmr_rest_channel >= DSD_TRUNK_CHAN_MAP_SIZE) {
        return 0;
    }
    return (state->trunk_chan_map[state->dmr_rest_channel] != 0) ? 1 : 0;
}

static long
no_carrier_select_control_channel(dsd_state* state) {
    long cc = (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
    if (cc == 0) {
        return 0;
    }
    if (no_carrier_has_mapped_dmr_rest_channel(state)) {
        cc = state->trunk_chan_map[state->dmr_rest_channel];
        state->p25_cc_freq = cc;
        state->trunk_cc_freq = cc;
    }
    return cc;
}

static int
no_carrier_p25_frames_enabled(const dsd_opts* opts) {
    return (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1) ? 1 : 0;
}

static int
no_carrier_generic_trunk_synctype(int synctype) {
    if (DSD_SYNC_IS_DMR(synctype) || DSD_SYNC_IS_NXDN(synctype) || DSD_SYNC_IS_EDACS(synctype)) {
        return 1;
    }
    return DSD_SYNC_IS_X2TDMA(synctype) ? 1 : 0;
}

static int
no_carrier_has_active_p25_voice_state(const dsd_opts* opts, const dsd_state* state) {
    if (opts->p25_is_tuned != 1) {
        return 0;
    }
    /*
     * Configured P25 follows can tune a voice channel before CC identity hints
     * are decoded; p25_is_tuned plus a tracked P25 VC is still a P25 return hint.
     */
    return (state->p25_vc_freq[0] != 0 || state->p25_vc_freq[1] != 0) ? 1 : 0;
}

static int
no_carrier_has_selectable_control_channel(const dsd_state* state) {
    return (state->trunk_cc_freq != 0 || state->p25_cc_freq != 0) ? 1 : 0;
}

static void
no_carrier_clear_stale_p25_return_hints_after_generic_activity(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    if (opts->p25_trunk != 1 && opts->trunk_enable != 1) {
        return;
    }
    if (!no_carrier_generic_trunk_synctype(state->lastsynctype)
        && !no_carrier_generic_trunk_synctype(state->synctype)) {
        return;
    }

    state->p2_cc = 0;
    state->p2_wacn = 0;
    state->p2_sysid = 0;
    state->p2_rfssid = 0;
    state->p2_siteid = 0;
    state->p25_sys_is_tdma = 0;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;
    state->p25_p2_active_slot = -1;
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->p25_p2_enc_lockout_muted[0] = 0;
    state->p25_p2_enc_lockout_muted[1] = 0;
    state->p25_call_is_packet[0] = 0;
    state->p25_call_is_packet[1] = 0;
    opts->p25_is_tuned = 0;
}

static int
no_carrier_is_p25_trunk_return(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || !state || opts->p25_trunk != 1 || !no_carrier_p25_frames_enabled(opts)) {
        return 0;
    }
    if (no_carrier_has_mapped_dmr_rest_channel(state)) {
        return 0;
    }
    if (DSD_SYNC_IS_P25(state->lastsynctype) || DSD_SYNC_IS_P25(state->synctype)) {
        return 1;
    }
    if (no_carrier_generic_trunk_synctype(state->lastsynctype) || no_carrier_generic_trunk_synctype(state->synctype)) {
        return 0;
    }
    return no_carrier_has_active_p25_voice_state(opts, state);
}

static int
no_carrier_should_clear_generic_p25_alias(const dsd_state* state, long cc, int is_p25_return) {
    if (is_p25_return) {
        return 0;
    }
    if (no_carrier_has_mapped_dmr_rest_channel(state)) {
        return 1;
    }
    return (state->p25_cc_freq != 0 && state->trunk_cc_freq != 0 && state->p25_cc_freq != state->trunk_cc_freq
            && state->p25_cc_freq != cc)
               ? 1
               : 0;
}

static void
no_carrier_sync_selected_control_channel(dsd_state* state, long cc, int is_p25_return, int clear_generic_p25_alias) {
    if (cc == 0) {
        return;
    }

    const int selected_existing_p25_alias = (state->p25_cc_freq == cc) ? 1 : 0;
    state->trunk_cc_freq = cc;
    if (is_p25_return) {
        state->p25_cc_freq = cc;
    } else if (clear_generic_p25_alias || !selected_existing_p25_alias) {
        state->p25_cc_freq = 0;
    }
}

static void
no_carrier_sync_helper_tune_cache(const dsd_opts* opts, const dsd_state* state, long cc) {
    if (cc == 0) {
        return;
    }
    if (opts->use_rigctl == 1) {
        s_last_rigctl_freq = cc;
        if (opts->setmod_bw != 0) {
            s_last_rigctl_bw = opts->setmod_bw;
        }
    }
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        s_last_rtl_freq = (uint32_t)cc;
    }
#else
    UNUSED(state);
#endif
}

static void
no_carrier_clear_voice_tune_state(dsd_opts* opts, dsd_state* state) {
    opts->p25_is_tuned = 0;
    opts->trunk_is_tuned = 0;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;
    state->trunk_vc_freq[0] = 0;
    state->trunk_vc_freq[1] = 0;
    state->p25_p2_active_slot = -1;
    state->p25_p2_audio_allowed[0] = 0;
    state->p25_p2_audio_allowed[1] = 0;
    state->p25_p2_enc_lockout_muted[0] = 0;
    state->p25_p2_enc_lockout_muted[1] = 0;
    state->p25_call_is_packet[0] = 0;
    state->p25_call_is_packet[1] = 0;
}

static dsd_trunk_tune_result
no_carrier_return_to_cc_correlated(dsd_opts* opts, dsd_state* state, uint64_t* out_request_id) {
    const uint64_t request_id = dsd_trunk_tuning_request_begin();
    if (out_request_id) {
        *out_request_id = request_id;
    }
    if (request_id == 0U) {
        return DSD_TRUNK_TUNE_RESULT_FAILED;
    }

    dsd_trunk_tune_result result = dsd_engine_return_to_cc_request(opts, state, request_id);
    if (result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        dsd_trunk_tuning_request_mark_ready(request_id);
        dsd_trunk_tune_result status = dsd_trunk_tuning_request_status(request_id, NULL);
        return status == DSD_TRUNK_TUNE_RESULT_OK ? status : result;
    }
    dsd_trunk_tuning_request_complete(request_id, result);
    return dsd_trunk_tuning_request_status(request_id, NULL);
}

static int
no_carrier_try_helper_return_to_cc(dsd_opts* opts, dsd_state* state, long cc, int p25_return,
                                   int clear_generic_p25_alias, int* helper_attempted,
                                   dsd_trunk_tune_result* helper_result) {
    if (helper_result) {
        *helper_result = DSD_TRUNK_TUNE_RESULT_OK;
    }
    if (!p25_return) {
        return 0;
    }

    *helper_attempted = 1;
    if (!p25_sm_tick_guard_try_enter()) {
        if (helper_result) {
            *helper_result = DSD_TRUNK_TUNE_RESULT_DEFERRED;
        }
        return 0;
    }
    /* The watchdog owns this same context. Keep the selected CC, tagged retune,
     * and request handoff atomic even when the tuner wait blocks. */
    const long old_p25_cc_freq = state->p25_cc_freq;
    const long old_trunk_cc_freq = state->trunk_cc_freq;
    no_carrier_sync_selected_control_channel(state, cc, p25_return, clear_generic_p25_alias);

    uint64_t tune_request_id = 0U;
    dsd_trunk_tune_result tune_result = no_carrier_return_to_cc_correlated(opts, state, &tune_request_id);
    if (tune_result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        (void)p25_sm_await_pending_cc_tune(p25_sm_get_ctx(), opts, state, tune_request_id, "no-carrier");
    } else if (tune_result == DSD_TRUNK_TUNE_RESULT_OK) {
        double completed_m = 0.0;
        (void)dsd_trunk_tuning_request_status(tune_request_id, &completed_m);
        if (completed_m <= 0.0) {
            completed_m = dsd_time_now_monotonic_s();
        }
        (void)p25_sm_restart_pending_cc_acquisition(p25_sm_get_ctx(), opts, state, completed_m, "no-carrier");
    }
    if (helper_result) {
        *helper_result = tune_result;
    }
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        if (tune_result != DSD_TRUNK_TUNE_RESULT_DEFERRED) {
            state->p25_cc_freq = old_p25_cc_freq;
            state->trunk_cc_freq = old_trunk_cc_freq;
        }
        p25_sm_tick_guard_leave();
        return 0;
    }

    no_carrier_sync_helper_tune_cache(opts, state, cc);
    state->edacs_tuned_lcn = -1;
    state->dmr_rest_channel = -1;
    p25_sm_tick_guard_leave();
    return 1;
}

static int
no_carrier_accept_generic_gate_recovery(const dsd_opts* opts, dsd_state* state, long cc) {
    s_no_carrier_generic_recovery_request_id = 0U;
    s_no_carrier_generic_recovery_state = NULL;
    s_no_carrier_generic_recovery_cc = 0;
    no_carrier_sync_helper_tune_cache(opts, state, cc);
    state->edacs_tuned_lcn = -1;
    state->dmr_rest_channel = -1;
    return 1;
}

static int
no_carrier_try_generic_gate_recovery(dsd_opts* opts, dsd_state* state, long cc, int p25_return,
                                     int clear_generic_p25_alias, int* helper_attempted) {
    if (p25_return) {
        return 0;
    }

    if (s_no_carrier_generic_recovery_request_id != 0U) {
        const dsd_trunk_tune_result status =
            dsd_trunk_tuning_request_status(s_no_carrier_generic_recovery_request_id, NULL);
        *helper_attempted = 1;
        if (status == DSD_TRUNK_TUNE_RESULT_PENDING) {
            return 0;
        }
        const uint64_t unresolved_request_id = dsd_trunk_tuning_pending_request();
        if (unresolved_request_id == 0U) {
            if (s_no_carrier_generic_recovery_state == state && s_no_carrier_generic_recovery_cc == cc) {
                return no_carrier_accept_generic_gate_recovery(opts, state, cc);
            }
            /* The old target completed, but decoder ownership moved while it
             * was in flight. Establish a fresh boundary for the current CC. */
        }
        if (unresolved_request_id != 0U
            && dsd_trunk_tuning_request_status(unresolved_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING) {
            return 0;
        }
    } else {
        const uint64_t unresolved_request_id = dsd_trunk_tuning_pending_request();
        if (unresolved_request_id == 0U) {
            return 0;
        }
        *helper_attempted = 1;
        if (dsd_trunk_tuning_request_status(unresolved_request_id, NULL) == DSD_TRUNK_TUNE_RESULT_PENDING) {
            /* Do not replace correlated work with an untagged legacy return.
             * Its completion will either open the gate or make this path retry
             * with a newer correlated request. */
            return 0;
        }
    }

    *helper_attempted = 1;
    const int old_p25_is_tuned = opts->p25_is_tuned;
    const int old_trunk_is_tuned = opts->trunk_is_tuned;
    const long old_p25_cc_freq = state->p25_cc_freq;
    const long old_trunk_cc_freq = state->trunk_cc_freq;
    no_carrier_sync_selected_control_channel(state, cc, 0, clear_generic_p25_alias);

    uint64_t tune_request_id = 0U;
    const dsd_trunk_tune_result tune_result = no_carrier_return_to_cc_correlated(opts, state, &tune_request_id);
    s_no_carrier_generic_recovery_request_id = tune_request_id;
    s_no_carrier_generic_recovery_state = state;
    s_no_carrier_generic_recovery_cc = cc;
    if (tune_result == DSD_TRUNK_TUNE_RESULT_PENDING) {
        opts->p25_is_tuned = old_p25_is_tuned;
        opts->trunk_is_tuned = old_trunk_is_tuned;
        no_carrier_sync_helper_tune_cache(opts, state, cc);
        return 0;
    }
    if (!dsd_trunk_tune_result_is_ok(tune_result)) {
        if (tune_result != DSD_TRUNK_TUNE_RESULT_DEFERRED) {
            state->p25_cc_freq = old_p25_cc_freq;
            state->trunk_cc_freq = old_trunk_cc_freq;
        }
        return 0;
    }

    return no_carrier_accept_generic_gate_recovery(opts, state, cc);
}

static int
no_carrier_helper_result_is_deferred(int helper_attempted, dsd_trunk_tune_result helper_result) {
    return (helper_attempted && helper_result == DSD_TRUNK_TUNE_RESULT_DEFERRED) ? 1 : 0;
}

static int
no_carrier_apply_legacy_cc_return(const dsd_opts* opts, dsd_state* state, long cc, int helper_attempted) {
    if (opts->use_rigctl == 1) {
        no_carrier_tune_rigctl_if_needed(opts, cc);
        state->dmr_rest_channel = -1;
        return 1;
    }
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (!helper_attempted) {
#ifdef USE_RADIO
            if (!no_carrier_tune_rtl_if_needed(opts, state, (uint32_t)cc)) {
                return 0;
            }
            state->dmr_rest_channel = -1;
#endif
            return 1;
        }
#ifdef USE_RADIO
        state->dmr_rest_channel = -1;
#endif
        return state->rtl_ctx ? 0 : 1;
    }
    state->dmr_rest_channel = -1;
    return 1;
}

static void
no_carrier_enable_p25_cc_slots(dsd_opts* opts) {
    opts->slot1_on = 1;
    opts->slot2_on = 1;
}

static void
no_carrier_enable_p25_cc_slots_if_known(dsd_opts* opts, const dsd_state* state) {
    if (state->p25_cc_is_tdma == 0 || state->p25_cc_is_tdma == 1) {
        no_carrier_enable_p25_cc_slots(opts);
    }
}

static int
no_carrier_current_demod_rate(const dsd_opts* opts, const dsd_state* state) {
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#ifdef USE_RADIO
    if (opts && opts->audio_in_type == AUDIO_IN_RTL && state && state->rtl_ctx) {
        uint32_t rtl_rate = rtl_stream_output_rate(state->rtl_ctx);
        if (rtl_rate > 0) {
            demod_rate = (int)rtl_rate;
        }
    }
#else
    (void)state;
#endif
    return demod_rate;
}

static void
no_carrier_apply_p25_cc_symbolrate(dsd_opts* opts, dsd_state* state) {
    int sym_rate = 0;
    if (state->p25_cc_is_tdma == 0) {
        sym_rate = 4800;
    } else if (state->p25_cc_is_tdma == 1) {
        sym_rate = 6000;
    } else {
        return;
    }
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, no_carrier_current_demod_rate(opts, state));
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
    no_carrier_enable_p25_cc_slots(opts);
}

static void
no_carrier_return_to_control_channel_if_needed(dsd_opts* opts, dsd_state* state, time_t now) {
    if (!no_carrier_is_cc_return_due(opts, state, now)) {
        return;
    }

    long cc = no_carrier_select_control_channel(state);
    const int p25_return = no_carrier_is_p25_trunk_return(opts, state);
    const int clear_generic_p25_alias = no_carrier_should_clear_generic_p25_alias(state, cc, p25_return);
    int accepted_cc_return = 0;
    int clear_failed_helper_state = 0;
    int clear_unreturnable_voice_state = 0;
    if (cc != 0) {
        int p25_helper_attempted = 0;
        dsd_trunk_tune_result p25_helper_result = DSD_TRUNK_TUNE_RESULT_OK;
        int generic_helper_attempted = 0;
        if (no_carrier_try_helper_return_to_cc(opts, state, cc, p25_return, clear_generic_p25_alias,
                                               &p25_helper_attempted, &p25_helper_result)) {
            no_carrier_enable_p25_cc_slots_if_known(opts, state);
            accepted_cc_return = 1;
        } else if (no_carrier_helper_result_is_deferred(p25_helper_attempted, p25_helper_result)) {
            /* Another P25 transition owns the guard; leave the staged
             * voice state intact so the main loop can retry safely. */
        } else if (no_carrier_try_generic_gate_recovery(opts, state, cc, p25_return, clear_generic_p25_alias,
                                                        &generic_helper_attempted)) {
            accepted_cc_return = 1;
        } else if (!generic_helper_attempted
                   && no_carrier_apply_legacy_cc_return(opts, state, cc, p25_helper_attempted)) {
            no_carrier_sync_selected_control_channel(state, cc, p25_return, clear_generic_p25_alias);
            accepted_cc_return = 1;
            state->edacs_tuned_lcn = -1;
            state->last_cc_sync_time = now;
            state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
            if (p25_return) {
                no_carrier_apply_p25_cc_symbolrate(opts, state);
            }
        } else if (p25_helper_attempted
                   && !no_carrier_helper_result_is_deferred(p25_helper_attempted, p25_helper_result)) {
            clear_failed_helper_state = 1;
        }
    } else {
        clear_unreturnable_voice_state = 1;
    }

    if (accepted_cc_return || clear_failed_helper_state || clear_unreturnable_voice_state) {
        no_carrier_clear_voice_tune_state(opts, state);
        DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
        state->is_con_plus = 0;
    }
}

static void
no_carrier_reset_dibit_and_dmr_buffers(dsd_state* state) {
    state->dibit_buf_p = state->dibit_buf + 200;
    DSD_MEMSET(state->dibit_buf, 0, sizeof(int) * 200);
    state->dmr_payload_p = state->dmr_payload_buf + 200;
    DSD_MEMSET(state->dmr_payload_buf, 0, sizeof(int) * 200);
    DSD_MEMSET(state->dmr_stereo_payload, 1, sizeof(int) * 144);
    if (state->dmr_reliab_buf) {
        state->dmr_reliab_p = state->dmr_reliab_buf + 200;
        DSD_MEMSET(state->dmr_reliab_buf, 0, 200 * sizeof(uint8_t));
    }
    if (state->dmr_soft_buf) {
        state->dmr_soft_p = state->dmr_soft_buf + 200;
        DSD_MEMSET(state->dmr_soft_buf, 0, 200 * sizeof(dsd_dibit_soft_t));
    }
}

static void
no_carrier_close_mbe_outputs_if_needed(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }
}

static void
no_carrier_reset_decode_state(dsd_state* state, int preserve_dmr_confidence) {
    state->jitter = -1;
    state->lastsynctype = DSD_SYNC_NONE;
    state->carrier = 0;
    state->max = 15000;
    state->min = -15000;
    state->center = 0;
    state->rtl_fsk_sps_num = 0;
    state->rtl_fsk_sps_den = 0;
    state->rtl_fsk_sps_accum = 0;
    state->m17_polarity = 0;
    state->err_str[0] = '\0';
    state->err_strR[0] = '\0';
    set_spaces(state->fsubtype, 14);
    set_spaces(state->ftype, 13);
    state->errs = 0;
    state->errs2 = 0;
    if (!preserve_dmr_confidence) {
        dmr_confidence_reset(state);
    }
}

static void
no_carrier_reset_non_trunk_fields_if_needed(const dsd_opts* opts, dsd_state* state) {
    if (opts->p25_trunk != 0 || opts->trunk_enable != 0) {
        return;
    }
    state->lasttg = 0;
    state->lastsrc = 0;
    state->lasttgR = 0;
    state->lastsrcR = 0;
    state->gi[0] = -1;
    state->gi[1] = -1;
    state->p25_vc_freq[0] = 0;
    state->p25_vc_freq[1] = 0;
    state->dmr_rest_channel = -1;
    state->nxdn_location_site_code = 0;
    state->nxdn_location_sys_code = 0;
    set_spaces(state->nxdn_location_category, 1);
    state->nxdn_rcn = 0;
    state->nxdn_base_freq = 0;
    state->nxdn_step = 0;
    state->nxdn_bw = 0;
    state->dmr_branding_sub[0] = '\0';
    state->dmr_branding[0] = '\0';
    state->dmr_site_parms[0] = '\0';
}

static void
no_carrier_reset_last_call_display(dsd_state* state) {
    state->lasttg = 0;
    state->lastsrc = 0;
    state->lasttgR = 0;
    state->lastsrcR = 0;
    state->gi[0] = -1;
    state->gi[1] = -1;
    state->nxdn_last_rid = 0;
    state->nxdn_last_tg = 0;
}

static void
no_carrier_reset_voice_and_audio_metrics(dsd_state* state) {
    state->lastp25type = 0;
    state->repeat = 0;
    state->nac = 0;
    state->numtdulc = 0;
    state->slot1light[0] = '\0';
    state->slot2light[0] = '\0';
    state->firstframe = 0;
    DSD_MEMSET(state->aout_max_buf, 0, sizeof(float) * 200);
    state->aout_max_buf_p = state->aout_max_buf;
    state->aout_max_buf_idx = 0;
    DSD_MEMSET(state->aout_max_bufR, 0, sizeof(float) * 200);
    state->aout_max_buf_pR = state->aout_max_bufR;
    state->aout_max_buf_idxR = 0;
    set_underscores(state->algid, 8);
    set_underscores(state->keyid, 16);
    mbe_initMbeParms(state->cur_mp, state->prev_mp, state->prev_mp_enhanced);
    mbe_initMbeParms(state->cur_mp2, state->prev_mp2, state->prev_mp_enhanced2);
}

static void
no_carrier_reset_payload_and_keystream_state(dsd_state* state) {
    state->dmr_ms_mode = 0;
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
    DSD_MEMSET(state->ks_octetL, 0, sizeof(state->ks_octetL));
    DSD_MEMSET(state->ks_octetR, 0, sizeof(state->ks_octetR));
    DSD_MEMSET(state->ks_bitstreamL, 0, sizeof(state->ks_bitstreamL));
    DSD_MEMSET(state->ks_bitstreamR, 0, sizeof(state->ks_bitstreamR));
    state->octet_counter = 0;
    state->bit_counterL = 0;
    state->bit_counterR = 0;
    state->xl_is_hdu = 0;
    state->nxdn_new_iv = 0;
}

static void
no_carrier_reset_dmr_data_blocks(dsd_state* state) {
    state->dmr_lrrp_source[0] = 0;
    state->dmr_lrrp_source[1] = 0;
    state->dmr_lrrp_target[0] = 0;
    state->dmr_lrrp_target[1] = 0;
    state->data_header_blocks[0] = 1;
    state->data_header_blocks[1] = 1;
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
    for (short i = 0; i < 4; i++) {
        state->ess_b[0][i] = 0;
        state->ess_b[1][i] = 0;
    }
    state->fourv_counter[0] = 0;
    state->fourv_counter[1] = 0;
    state->voice_counter[0] = 0;
    state->voice_counter[1] = 0;
}

static void
no_carrier_reset_nxdn_alias_state(dsd_state* state) {
    state->nxdn_part_of_frame = 0;
    state->nxdn_ran = 0;
    state->nxdn_sf = 0;
    DSD_MEMSET(state->nxdn_sacch_frame_segcrc, 1, sizeof(state->nxdn_sacch_frame_segcrc));
    state->nxdn_sacch_non_superframe = 1;
    DSD_MEMSET(state->nxdn_sacch_frame_segment, 1, sizeof(state->nxdn_sacch_frame_segment));
    state->nxdn_alias_block_number = 0;
    DSD_MEMSET(state->nxdn_alias_block_segment, 0, sizeof(state->nxdn_alias_block_segment));
    state->nxdn_alias_arib_total_segments = 0;
    state->nxdn_alias_arib_seen_mask = 0;
    DSD_MEMSET(state->nxdn_alias_arib_segments, 0, sizeof(state->nxdn_alias_arib_segments));
    state->nxdn_call_type[0] = '\0';
}

static void
no_carrier_unload_keys_if_needed(dsd_state* state) {
    if (state->keyloader != 1) {
        return;
    }
    state->R = 0;
    state->RR = 0;
    state->K = 0;
    state->K1 = 0;
    state->K2 = 0;
    state->K3 = 0;
    state->K4 = 0;
    state->hytera_key_segments = 0U;
    DSD_MEMSET(state->A1, 0, sizeof(state->A1));
    DSD_MEMSET(state->A2, 0, sizeof(state->A2));
    DSD_MEMSET(state->A3, 0, sizeof(state->A3));
    DSD_MEMSET(state->A4, 0, sizeof(state->A4));
    DSD_MEMSET(state->aes_key_loaded, 0, sizeof(state->aes_key_loaded));
    DSD_MEMSET(state->aes_key_segments, 0, sizeof(state->aes_key_segments));
    state->H = 0;
}

static void
no_carrier_reset_dmr_misc_state(dsd_state* state) {
    state->nxdn_cipher_type = 0;
    DSD_MEMSET(state->dmr_cach_fragment, 1, sizeof(state->dmr_cach_fragment));
    state->dmr_cach_counter = 0;
    DSD_MEMSET(state->dmr_pdu_sf, 0, sizeof(state->dmr_pdu_sf));
    DSD_MEMSET(state->p25_mac_frag, 0, sizeof(state->p25_mac_frag));
    DSD_MEMSET(state->p25_apx_alias_rx, 0, sizeof(state->p25_apx_alias_rx));
    DSD_MEMSET(state->p25_l3h_alias_phase1, 0, sizeof(state->p25_l3h_alias_phase1));
    DSD_MEMSET(state->data_header_valid, 0, sizeof(state->data_header_valid));
    DSD_MEMSET(state->cap_plus_csbk_bits, 0, sizeof(state->cap_plus_csbk_bits));
    DSD_MEMSET(state->cap_plus_block_num, 0, sizeof(state->cap_plus_block_num));
    DSD_MEMSET(state->data_block_crc_valid, 0, sizeof(state->data_block_crc_valid));
    DSD_MEMSET(state->dmr_embedded_signalling, 0, sizeof(state->dmr_embedded_signalling));
    DSD_MEMSET(state->late_entry_mi_fragment, 0, sizeof(state->late_entry_mi_fragment));
    DSD_MEMSET(state->dmr_alias_format, 0, sizeof(state->dmr_alias_format));
    DSD_MEMSET(state->dmr_alias_block_len, 0, sizeof(state->dmr_alias_block_len));
    DSD_MEMSET(state->dmr_alias_char_size, 0, sizeof(state->dmr_alias_char_size));
    DSD_MEMSET(state->dmr_alias_block_segment, 0, sizeof(state->dmr_alias_block_segment));
    DSD_MEMSET(state->dmr_embedded_gps, 0, sizeof(state->dmr_embedded_gps));
    DSD_MEMSET(state->dmr_lrrp_gps, 0, sizeof(state->dmr_lrrp_gps));
    DSD_MEMSET(state->generic_talker_alias, 0, sizeof(state->generic_talker_alias));
    state->generic_talker_alias_src[0] = 0;
    state->generic_talker_alias_src[1] = 0;
}

static void
no_carrier_reset_p25_metrics(dsd_state* state) {
    state->p25_p1_fec_ok = 0;
    state->p25_p1_fec_err = 0;
    state->p25_p1_voice_fec_ok = 0;
    state->p25_p1_voice_fec_err = 0;
    state->p25_p2_rs_facch_ok = 0;
    state->p25_p2_rs_facch_err = 0;
    state->p25_p2_rs_facch_corr = 0;
    state->p25_p2_rs_sacch_ok = 0;
    state->p25_p2_rs_sacch_err = 0;
    state->p25_p2_rs_sacch_corr = 0;
    state->p25_p2_rs_ess_ok = 0;
    state->p25_p2_rs_ess_err = 0;
    state->p25_p2_rs_ess_corr = 0;
}

static void
no_carrier_reset_p25_cc_cache(dsd_state* state) {
    dsd_trunk_cc_candidates* cc_candidates = dsd_trunk_cc_candidates_get(state);
    if (cc_candidates) {
        cc_candidates->count = 0;
        cc_candidates->idx = 0;
    }
    state->p25_cc_cache_loaded = 0;
}

static void
no_carrier_reset_p25_metrics_and_cache(dsd_state* state) {
    no_carrier_reset_p25_metrics(state);
    no_carrier_reset_p25_cc_cache(state);
}

static void
no_carrier_clear_stale_follow_state_if_needed(dsd_opts* opts, dsd_state* state, time_t now) {
    const int trunking_enabled = (opts->p25_trunk == 1 || opts->trunk_enable == 1) ? 1 : 0;
    const int voice_tuned = (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1) ? 1 : 0;
    const int vc_recent = (state->last_vc_sync_time != 0 && (now - state->last_vc_sync_time) <= 10) ? 1 : 0;
    const int retryable_cc_return = no_carrier_has_selectable_control_channel(state);
    const int preserve_voice_state = (trunking_enabled && voice_tuned && (vc_recent || retryable_cc_return)) ? 1 : 0;

    if (now - state->last_cc_sync_time > 10 && !preserve_voice_state) {
        state->dmr_rest_channel = -1;
        state->p25_vc_freq[0] = 0;
        state->p25_vc_freq[1] = 0;
        state->trunk_vc_freq[0] = 0;
        state->trunk_vc_freq[1] = 0;
        state->dmr_mfid = -1;
        state->dmr_branding_sub[0] = '\0';
        state->dmr_branding[0] = '\0';
        state->dmr_site_parms[0] = '\0';
        opts->p25_is_tuned = 0;
        opts->trunk_is_tuned = 0;
        DSD_MEMSET(state->active_channel, 0, sizeof(state->active_channel));
    }
}

static void
no_carrier_reset_call_strings_and_dpmr(dsd_opts* opts, dsd_state* state) {
    set_spaces(state->call_string[0], 21);
    set_spaces(state->call_string[1], 21);
    opts->dPMR_next_part_of_superframe = 0;
    state->dPMRVoiceFS2Frame.CalledIDOk = 0;
    state->dPMRVoiceFS2Frame.CallingIDOk = 0;
    DSD_MEMSET(state->dPMRVoiceFS2Frame.CalledID, 0, 8);
    DSD_MEMSET(state->dPMRVoiceFS2Frame.CallingID, 0, 8);
    DSD_MEMSET(state->dPMRVoiceFS2Frame.Version, 0, 8);
    set_spaces(state->dpmr_caller_id, 6);
    set_spaces(state->dpmr_target_id, 6);
}

static void
no_carrier_reset_ysf_and_dstar_strings(dsd_state* state) {
    set_spaces(state->ysf_tgt, 10);
    set_spaces(state->ysf_src, 10);
    set_spaces(state->ysf_upl, 10);
    set_spaces(state->ysf_dnl, 10);
    set_spaces(state->ysf_rm1, 5);
    set_spaces(state->ysf_rm2, 5);
    set_spaces(state->ysf_rm3, 5);
    set_spaces(state->ysf_rm4, 5);
    DSD_MEMSET(state->ysf_txt, 0, sizeof(state->ysf_txt));
    state->ysf_dt = 9;
    state->ysf_fi = 9;
    state->ysf_cm = 9;

    set_spaces(state->dstar_rpt1, 8);
    set_spaces(state->dstar_rpt2, 8);
    set_spaces(state->dstar_dst, 8);
    set_spaces(state->dstar_src, 8);
    set_spaces(state->dstar_txt, 8);
    set_spaces(state->dstar_gps, 8);
}

static void
no_carrier_reset_m17_and_sample_buffers(dsd_state* state) {
    DSD_MEMSET(state->m17_lsf, 0, sizeof(state->m17_lsf));
    DSD_MEMSET(state->m17_pkt, 0, sizeof(state->m17_pkt));
    state->m17_pbc_ct = 0;
    state->m17_str_dt = 9;
    state->m17_bert_locked = 0;
    state->m17_bert_lfsr = 1;
    state->m17_bert_lock_count = 0;
    state->m17_bert_window_bits = 0;
    state->m17_bert_window_errors = 0;
    state->m17_bert_bits = 0;
    state->m17_bert_errors = 0;
    state->m17_bert_resyncs = 0;
    state->m17_dst = 0;
    state->m17_src = 0;
    state->m17_can = 0;
    DSD_MEMSET(state->m17_dst_csd, 0, sizeof(state->m17_dst_csd));
    DSD_MEMSET(state->m17_src_csd, 0, sizeof(state->m17_src_csd));
    state->m17_dst_str[0] = '\0';
    state->m17_src_str[0] = '\0';
    state->m17_enc = 0;
    state->m17_enc_st = 0;
    state->m17_payload_decrypted = 0;
    state->m17_signature_advertised = 0;
    DSD_MEMSET(state->m17_signature_digest, 0, sizeof(state->m17_signature_digest));
    DSD_MEMSET(state->m17_signature, 0, sizeof(state->m17_signature));
    state->m17_signature_received_mask = 0;
    state->m17_signature_complete = 0;
    state->m17_signature_bad_sequence = 0;
    DSD_MEMSET(state->m17_meta, 0, sizeof(state->m17_meta));

    DSD_MEMSET(state->audio_out_temp_buf, 0.0f, sizeof(state->audio_out_temp_buf));
    DSD_MEMSET(state->audio_out_temp_bufR, 0.0f, sizeof(state->audio_out_temp_bufR));
    DSD_MEMSET(state->f_l, 0.0f, sizeof(state->f_l));
    DSD_MEMSET(state->f_r, 0.0f, sizeof(state->f_r));
    DSD_MEMSET(state->f_l4, 0.0f, sizeof(state->f_l4));
    DSD_MEMSET(state->f_r4, 0.0f, sizeof(state->f_r4));
    DSD_MEMSET(state->s_l, 0, sizeof(state->s_l));
    DSD_MEMSET(state->s_r, 0, sizeof(state->s_r));
    DSD_MEMSET(state->s_l4, 0, sizeof(state->s_l4));
    DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
    DSD_MEMSET(state->s_lu, 0, sizeof(state->s_lu));
    DSD_MEMSET(state->s_ru, 0, sizeof(state->s_ru));
    DSD_MEMSET(state->s_l4u, 0, sizeof(state->s_l4u));
    DSD_MEMSET(state->s_r4u, 0, sizeof(state->s_r4u));
    DSD_MEMSET(state->static_ks_counter, 0, sizeof(state->static_ks_counter));
}

void
noCarrier(dsd_opts* opts, dsd_state* state) {
    const time_t now = time(NULL);

#ifdef USE_RADIO
    maybe_request_rtl_fsk_reacquire_on_no_sync(opts, state, now);
#endif

    no_carrier_rotate_symbol_output_if_needed(opts, state);
    no_carrier_reset_floating_gain_if_needed(opts, state);

//only do it here on the tweaks
#ifdef LIMAZULUTWEAKS
    no_carrier_reset_nxdn_scan_markers(state);
#endif

    no_carrier_step_scanner_mode_if_needed(opts, state, now);
    no_carrier_return_to_control_channel_if_needed(opts, state, now);
    no_carrier_clear_stale_p25_return_hints_after_generic_activity(opts, state);
    no_carrier_reset_dibit_and_dmr_buffers(state);
    no_carrier_close_mbe_outputs_if_needed(opts, state);
    const int preserve_scan_state = opts && opts->trunk_scan_enabled == 1;
    no_carrier_reset_decode_state(state, preserve_scan_state);
    no_carrier_reset_non_trunk_fields_if_needed(opts, state);
    no_carrier_reset_last_call_display(state);
    no_carrier_reset_voice_and_audio_metrics(state);
    no_carrier_reset_payload_and_keystream_state(state);
    no_carrier_reset_dmr_data_blocks(state);
    no_carrier_reset_nxdn_alias_state(state);
    no_carrier_unload_keys_if_needed(state);
    no_carrier_reset_dmr_misc_state(state);
    if (preserve_scan_state) {
        no_carrier_reset_p25_metrics(state);
    } else {
        no_carrier_reset_p25_metrics_and_cache(state);
    }
    no_carrier_reset_call_strings_and_dpmr(opts, state);
    no_carrier_clear_stale_follow_state_if_needed(opts, state, now);
    no_carrier_reset_ysf_and_dstar_strings(state);
    no_carrier_reset_m17_and_sample_buffers(state);
} //nocarrier

static int
live_scanner_apply_audio_gain(dsd_opts* opts, dsd_state* state) {
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
    return 0;
}

#ifdef USE_RADIO
static int
live_scanner_start_rtl_if_needed(dsd_opts* opts, dsd_state* state) {
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (state->rtl_ctx == NULL) {
            if (rtl_stream_create_mirrored(opts, &state->rtl_ctx) < 0) {
                LOG_ERROR("Failed to create radio stream.\n");
                opts->rtl_started = 0;
                opts->rtl_needs_restart = 0;
                return -1;
            }
        }
        if (state->rtl_ctx && rtl_stream_start(state->rtl_ctx) < 0) {
            LOG_ERROR("Failed to open radio stream.\n");
            rtl_stream_destroy(state->rtl_ctx);
            state->rtl_ctx = NULL;
            opts->rtl_started = 0;
            opts->rtl_needs_restart = 0;
            return -1;
        }
        opts->rtl_started = 1;
        opts->rtl_needs_restart = 0;
    }
    return 0;
}
#endif

static int
live_scanner_open_audio_if_needed(dsd_opts* opts) {
    if (opts->audio_in_type == AUDIO_IN_PULSE) {
        if (openAudioInput(opts) != 0) {
            return -1;
        }
    }
    if (opts->audio_out_type == 0) {
        if (openAudioOutput(opts) != 0) {
            return -1;
        }
    }
    return 0;
}

static int
live_scanner_start_trunk_scan_if_needed(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->trunk_scan_enabled != 1) {
        return 0;
    }
    char scan_err[256] = {0};
    if (dsd_engine_trunk_scan_init(opts, state, scan_err, sizeof scan_err) != 0) {
        LOG_ERROR("Trunk scan: %s\n", scan_err[0] ? scan_err : "initialization failed");
        return -1;
    }
    return 0;
}

static void
live_scanner_emit_start_history(dsd_state* state) {
    if (state == NULL || state->event_history_s == NULL) {
        return;
    }

    watchdog_event_status(state, "Any decoded voice calls or data calls display here;", 0);
    push_event_history(&state->event_history_s[0]);
    init_event_history(&state->event_history_s[0], 0, 1);
    watchdog_event_status(state, "DSD-neo Started and Event History Initialized;", 0);
    push_event_history(&state->event_history_s[0]);
    init_event_history(&state->event_history_s[0], 0, 1);
}

static void
live_scanner_emit_start_log_if_enabled(const dsd_opts* opts, dsd_state* state) {
    if (opts->event_out_file[0] == 0) {
        return;
    }
    time_t now = time(NULL);
    char timestr[9];
    char datestr[11];
    char event_string[2000];
    getTimeN_buf(now, timestr);
    getDateN_buf(now, datestr);
    DSD_MEMSET(event_string, 0, sizeof(event_string));
    DSD_SNPRINTF(event_string, sizeof event_string, "%s %s DSD-neo Started and Event History Initialized;", datestr,
                 timestr);
    write_event_to_log_file(opts, state, 0, 0, event_string);
    DSD_MEMSET(event_string, 0, sizeof(event_string));
    DSD_SNPRINTF(event_string, sizeof event_string, "%s %s Any decoded voice calls or data calls display here;",
                 datestr, timestr);
    write_event_to_log_file(opts, state, 0, 0, event_string);
}

static void
live_scanner_update_thresholds(dsd_state* state, int* last_max, int* last_min) {
    int current_max = (int)state->max;
    int current_min = (int)state->min;
    if (current_max == *last_max && current_min == *last_min) {
        return;
    }
    state->center = ((state->max) + (state->min)) / 2;
    state->umid = (((state->max) - state->center) * 5 / 8) + state->center;
    state->lmid = (((state->min) - state->center) * 5 / 8) + state->center;
    *last_max = current_max;
    *last_min = current_min;
}

static void
live_scanner_process_synced_frames(dsd_opts* opts, dsd_state* state, int* last_max, int* last_min,
                                   uint64_t* frame_tune_generation) {
    while (state->synctype != DSD_SYNC_NONE) {
        p25_sm_tick_guard_enter();
        if (!frame_tune_generation || dsd_trunk_tuning_frame_is_current(*frame_tune_generation)) {
            processFrame(opts, state);
        }
        p25_sm_tick_guard_leave();
        dsd_trunk_scan_hook_tick(opts, state);

#ifdef TRACE_DSD
        state->debug_prefix = '\0';
#endif

        dsd_runtime_pump_controls(opts, state);
        if (frame_tune_generation) {
            *frame_tune_generation = dsd_trunk_tuning_generation();
        }
        state->synctype = getFrameSync(opts, state);
        live_scanner_update_thresholds(state, last_max, last_min);
    }
}

static void
live_scanner_main_loop(dsd_opts* opts, dsd_state* state) {
    int last_max = INT_MIN;
    int last_min = INT_MAX;
    uint64_t frame_tune_generation;

    while (!exitflag) {
        dsd_runtime_pump_controls(opts, state);
        p25_sm_try_tick(opts, state);
        dsd_trunk_scan_hook_tick(opts, state);
        dsd_runtime_pump_controls(opts, state);

        noCarrier(opts, state);
        frame_tune_generation = dsd_trunk_tuning_generation();
        state->synctype = getFrameSync(opts, state);
        live_scanner_update_thresholds(state, &last_max, &last_min);
        live_scanner_process_synced_frames(opts, state, &last_max, &last_min, &frame_tune_generation);
    }
}

static int
dsd_engine_lifecycle_start_if_needed(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                                     int* lifecycle_started) {
    if (!hooks || !hooks->start || !lifecycle_started || *lifecycle_started) {
        return 0;
    }
    if (hooks->start(opts, state, hooks->context) != 0) {
        return -1;
    }
    *lifecycle_started = 1;
    return 0;
}

static int
liveScanner(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks, int* lifecycle_started) {
    if (!opts || !state) {
        return -1;
    }
    (void)live_scanner_apply_audio_gain(opts, state);
#ifdef USE_RADIO
    if (live_scanner_start_rtl_if_needed(opts, state) != 0) {
        return -1;
    }
#endif
    if (live_scanner_open_audio_if_needed(opts) != 0) {
        return -1;
    }
    if (live_scanner_start_trunk_scan_if_needed(opts, state) != 0) {
        return -1;
    }

    live_scanner_emit_start_history(state);
    live_scanner_emit_start_log_if_enabled(opts, state);
    if (dsd_frame_log_enabled(opts)) {
        dsd_frame_logf(opts, "DSD-neo frame logging initialized");
    }

    if (dsd_engine_lifecycle_start_if_needed(opts, state, hooks, lifecycle_started) != 0) {
        return -1;
    }
    p25_sm_watchdog_start(opts, state);
    live_scanner_main_loop(opts, state);
    p25_sm_watchdog_stop();
    return 0;
}

static void
dsd_engine_cleanup_codec2(dsd_state* state) {
#ifdef USE_CODEC2
    if (state->codec2_1600) {
        codec2_destroy(state->codec2_1600);
        state->codec2_1600 = NULL;
    }
    if (state->codec2_3200) {
        codec2_destroy(state->codec2_3200);
        state->codec2_3200 = NULL;
    }
#else
    UNUSED(state);
#endif
}

static void
dsd_engine_cleanup_watchdog_snapshots(dsd_opts* opts, dsd_state* state) {
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);
}

static void
dsd_engine_cleanup_close_wavs(dsd_opts* opts, dsd_state* state) {
    if (opts->static_wav_file == 0) {
        if (opts->wav_out_f != NULL) {
            opts->wav_out_f = close_and_rename_wav_file(opts->wav_out_f, opts, opts->wav_out_file, opts->wav_out_dir,
                                                        &state->event_history_s[0]);
        }
        if (opts->wav_out_fR != NULL) {
            opts->wav_out_fR = close_and_rename_wav_file(opts->wav_out_fR, opts, opts->wav_out_fileR, opts->wav_out_dir,
                                                         &state->event_history_s[1]);
        }
        return;
    }

    if (opts->wav_out_f != NULL) {
        opts->wav_out_f = close_wav_file(opts->wav_out_f);
    }
    if (opts->wav_out_fR != NULL) {
        opts->wav_out_fR = close_wav_file(opts->wav_out_fR);
    }
}

static void
dsd_engine_cleanup_close_radio(const dsd_opts* opts, dsd_state* state) {
#ifdef USE_RADIO
    if (opts->rtl_started == 1 && state->rtl_ctx) {
        rtl_stream_stop(state->rtl_ctx);
        rtl_stream_destroy(state->rtl_ctx);
        state->rtl_ctx = NULL;
    }
#else
    UNUSED(opts);
    UNUSED(state);
#endif
}

static void
dsd_engine_cleanup_close_net(dsd_opts* opts) {
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
}

static void
dsd_engine_cleanup_close_mbe(dsd_opts* opts, dsd_state* state) {
    if (opts->mbe_out_f != NULL) {
        closeMbeOutFile(opts, state);
    }
    if (opts->mbe_out_fR != NULL) {
        closeMbeOutFileR(opts, state);
    }
}

static void
dsd_engine_cleanup_print_stats(dsd_state* state) {
    LOG_NOTICE("\n");
    if (state->debug_mode == 1) {
        const uint64_t* start_ms = DSD_STATE_EXT_GET_AS(uint64_t, state, DSD_STATE_EXT_ENGINE_START_MS);
        if (start_ms) {
            uint64_t elapsed_ms = dsd_time_monotonic_ms() - *start_ms;
            LOG_NOTICE("Runtime: %llu ms\n", (unsigned long long)elapsed_ms);
        }
    }
    LOG_NOTICE("Total audio errors: %i\n", state->debug_audio_errors);
    LOG_NOTICE("Total header errors: %i\n", state->debug_header_errors);
    LOG_NOTICE("Total irrecoverable header errors: %i\n", state->debug_header_critical_errors);
    LOG_NOTICE("Exiting.\n");
}

void
dsd_engine_cleanup(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    exitflag = 1;

    nxdn_trunk_diag_log_summary(opts, state);
    dsd_engine_cleanup_codec2(state);
    dsd_engine_cleanup_watchdog_snapshots(opts, state);
    noCarrier(opts, state);
    dsd_engine_cleanup_watchdog_snapshots(opts, state);
    dsd_engine_cleanup_close_wavs(opts, state);
    dsd_rdio_upload_shutdown();

    if (opts->wav_out_raw != NULL) {
        opts->wav_out_raw = close_wav_file(opts->wav_out_raw);
    }

    closeSymbolOutFile(opts, state);
    dsd_frame_log_close(opts);
    dsd_p25_sm_log_close(opts);
    dsd_engine_cleanup_close_radio(opts, state);
    dsd_engine_cleanup_close_net(opts);
    dsd_engine_cleanup_close_mbe(opts, state);
    dsd_engine_trunk_scan_shutdown(opts, state);
    autosave_user_config(opts, state);
    dsd_engine_cleanup_print_stats(state);

    closeAudioOutput(opts);
    closeAudioInput(opts);
    dsd_audio_cleanup();

    dsd_state_ext_free_all(state);
    dsd_socket_cleanup();
}

static void
dsd_engine_run_record_start_time_if_debug(dsd_state* state) {
    if (state->debug_mode != 1) {
        return;
    }
    uint64_t* start_ms = (uint64_t*)malloc(sizeof(*start_ms));
    if (start_ms) {
        *start_ms = dsd_time_monotonic_ms();
        (void)dsd_state_ext_set(state, DSD_STATE_EXT_ENGINE_START_MS, start_ms, free);
    }
}

static void
dsd_engine_run_install_hooks(void) {
    dsd_engine_frame_sync_hooks_install();
    dsd_engine_trunk_tuning_hooks_install();
    dsd_engine_rtl_stream_io_hooks_install();
    dsd_engine_rtl_stream_metrics_hooks_install();
    dsd_engine_rigctl_query_hooks_install();
    dsd_engine_net_audio_input_hooks_install();
    dsd_engine_udp_audio_hooks_install();
    dsd_engine_m17_udp_hooks_install();
    dsd_engine_p25_optional_hooks_install();
}

static int
dsd_engine_run_common_setup(dsd_opts* opts, dsd_state* state, int* early_exit) {
    if (!opts || !state || !early_exit) {
        return -1;
    }
    if (opts->trunk_scan_enabled == 1 && opts->scanner_mode == 1) {
        LOG_ERROR("Trunk scan cannot be combined with legacy scanner mode.\n");
        return -1;
    }
    if (import_trunking_csvs_if_needed(opts, state) != 0) {
        return -1;
    }
    open_recording_outputs_if_needed(opts, state);

    {
        int filter_rate = analog_filter_rate_hz(opts, state);
        init_audio_filters(state, filter_rate);
    }

    p25_sm_init(opts, state);
    dmr_sm_init(opts, state);

    if (opts->resume > 0) {
        openSerial(opts, state);
        if (opts->serial_fd < 0) {
            return -1;
        }
    }

    if (dsd_engine_setup_io(opts, state) != 0) {
        return -1;
    }
    if (exitflag) {
        *early_exit = 1;
        return 0;
    }

    signal(SIGINT, dsd_engine_signal_handler);
    signal(SIGTERM, dsd_engine_signal_handler);
    dsd_engine_parse_m17_userdata(opts, state);
    return 0;
}

static int
dsd_engine_run_open_audio_output_if_needed(dsd_opts* opts) {
    if (opts->audio_out_type == 0 && openAudioOutput(opts) != 0) {
        return -1;
    }
    return 0;
}

static void
dsd_engine_run_start_rtl_encoder_input_if_needed(dsd_opts* opts, dsd_state* state) {
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        if (state->rtl_ctx == NULL && rtl_stream_create_mirrored(opts, &state->rtl_ctx) < 0) {
            LOG_ERROR("Failed to create radio stream.\n");
        }
        if (state->rtl_ctx && rtl_stream_start(state->rtl_ctx) < 0) {
            LOG_ERROR("Failed to open radio stream.\n");
        }
        opts->rtl_started = 1;
    }
#else
    UNUSED(opts);
    UNUSED(state);
#endif
}

static int
dsd_engine_run_mode_m17_str(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                            int* lifecycle_started) {
    opts->use_cosine_filter = 1;
    opts->pulse_digi_rate_out = 48000;

    if (opts->audio_in_type == AUDIO_IN_PULSE && openAudioInput(opts) != 0) {
        return -1;
    }
    dsd_engine_run_start_rtl_encoder_input_if_needed(opts, state);
    if (dsd_engine_run_open_audio_output_if_needed(opts) != 0) {
        return -1;
    }
    if (dsd_engine_lifecycle_start_if_needed(opts, state, hooks, lifecycle_started) != 0) {
        return -1;
    }
    encodeM17STR(opts, state);
    return 0;
}

static int
dsd_engine_run_mode_m17_brt(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                            int* lifecycle_started) {
    opts->use_cosine_filter = 1;
    opts->pulse_digi_rate_out = 48000;
    if (dsd_engine_run_open_audio_output_if_needed(opts) != 0) {
        return -1;
    }
    if (dsd_engine_lifecycle_start_if_needed(opts, state, hooks, lifecycle_started) != 0) {
        return -1;
    }
    encodeM17BRT(opts, state);
    return 0;
}

static int
dsd_engine_run_mode_m17_pkt(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                            int* lifecycle_started) {
    opts->use_cosine_filter = 1;
    opts->pulse_digi_rate_out = 48000;
    if (dsd_engine_run_open_audio_output_if_needed(opts) != 0) {
        return -1;
    }
    if (dsd_engine_lifecycle_start_if_needed(opts, state, hooks, lifecycle_started) != 0) {
        return -1;
    }
    encodeM17PKT(opts, state);
    return 0;
}

static int
dsd_engine_run_mode_m17_decoder_ip(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                                   int* lifecycle_started) {
    opts->pulse_digi_rate_out = 8000;
    if (dsd_engine_run_open_audio_output_if_needed(opts) != 0) {
        return -1;
    }
    if (dsd_engine_lifecycle_start_if_needed(opts, state, hooks, lifecycle_started) != 0) {
        return -1;
    }
    processM17IPF(opts, state);
    return 0;
}

static int
dsd_engine_run_dispatch_mode(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks,
                             int* lifecycle_started) {
    if (opts->playfiles == 1) {
        if (dsd_engine_lifecycle_start_if_needed(opts, state, hooks, lifecycle_started) != 0) {
            return -1;
        }
        playMbeFiles(opts, state, state->cli_argc_effective, state->cli_argv);
        return 0;
    }
    if (opts->m17encoder == 1) {
        return dsd_engine_run_mode_m17_str(opts, state, hooks, lifecycle_started);
    }
    if (opts->m17encoderbrt == 1) {
        return dsd_engine_run_mode_m17_brt(opts, state, hooks, lifecycle_started);
    }
    if (opts->m17encoderpkt == 1) {
        return dsd_engine_run_mode_m17_pkt(opts, state, hooks, lifecycle_started);
    }
    if (opts->m17decoderip == 1) {
        return dsd_engine_run_mode_m17_decoder_ip(opts, state, hooks, lifecycle_started);
    }
    return liveScanner(opts, state, hooks, lifecycle_started);
}

int
dsd_engine_run_with_lifecycle(dsd_opts* opts, dsd_state* state, const dsd_engine_lifecycle_hooks* hooks) {
    if (!opts || !state) {
        return -1;
    }

    reset_device_io_caches();
    dsd_bootstrap_enable_ftz_daz_if_enabled();
    init_rrc_filter_memory();
    InitAllFecFunction();
    CNXDNConvolution_init();

    if (dsd_socket_init() != 0) {
        DSD_FPRINTF(stderr, "Failed to initialize socket subsystem\n");
        return 1;
    }

    int rc = 0;
    int early_exit = 0;
    int lifecycle_started = 0;
    exitflag = 0;

    dsd_engine_run_record_start_time_if_debug(state);
    dsd_engine_run_install_hooks();

    if (dsd_engine_run_common_setup(opts, state, &early_exit) != 0) {
        rc = 1;
        goto ENGINE_OUT;
    }
    if (!early_exit) {
        if (dsd_engine_run_dispatch_mode(opts, state, hooks, &lifecycle_started) != 0) {
            rc = 1;
        }
    }

ENGINE_OUT:
    if (lifecycle_started && hooks && hooks->stop) {
        hooks->stop(opts, state, hooks->context);
    }
    dsd_engine_cleanup(opts, state);
    return rc;
}

int
dsd_engine_run(dsd_opts* opts, dsd_state* state) {
    return dsd_engine_run_with_lifecycle(opts, state, NULL);
}
