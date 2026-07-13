// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
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

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/symbol_levels.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <dsd-neo/runtime/shutdown.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <fcntl.h>
#include <math.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/dibit.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "pcm_input_staging.h"

#ifdef USE_RADIO
#include <dsd-neo/runtime/rtl_stream_io_hooks.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif
#include <fcntl.h> // IWYU pragma: keep

#ifdef DSD_NEO_TEST_HOOKS
#include "symbol_test_support.h"
#endif

extern dsd_socket_t Connect(char* hostname, int portno);

#ifdef DSD_NEO_TEST_HOOKS
// Test-hook entry points are intentionally externally visible to focused fixtures.
// NOLINTBEGIN(misc-use-internal-linkage)
void dsd_symbol_test_select_window(int rf_mod, int synctype, int lastsynctype, int freeze_window, int* l_edge,
                                   int* r_edge);
int dsd_symbol_test_adjust_timing_index(int samples_per_symbol, int symbol_center, int rf_mod, int jitter,
                                        int have_sync, int symbol_span, int start_i, int* jitter_after);
int dsd_symbol_test_is_m17_sync(int lastsynctype);
float dsd_symbol_test_apply_matched_filter(const dsd_opts* opts, const dsd_state* state, float sample,
                                           int rtl_symbol_rate_output, int cqpsk_symbol_rate);
unsigned int dsd_symbol_test_convert_analog_block_to_i16(const float* input, short* output, unsigned int count);
#ifdef USE_RADIO
int dsd_symbol_test_rtl_cache_and_center_contract(int out_values[10]);
int dsd_symbol_test_auto_center_step_direction(int e_ema, int deadband, int* run_dir, int* run_len, int* dir_out);
#endif
// NOLINTEND(misc-use-internal-linkage)
#endif

static inline short
float_to_int16_clip(float v) {
    if (v > 32767.0f) {
        return 32767;
    }
    if (v < -32768.0f) {
        return -32768;
    }
    return (short)lrintf(v);
}

static inline void
symbol_write_wav_short_block(SNDFILE* file, const short* samples, sf_count_t sample_count, const char* context) {
    if (file == NULL || samples == NULL || sample_count <= 0) {
        return;
    }
    sf_count_t written = sf_write_short(file, samples, sample_count);
    if (written != sample_count) {
        LOG_WARN("%s: wrote %lld/%lld samples to WAV output\n", context, (long long)written, (long long)sample_count);
    }
}

static int16_t
read_le_i16(const unsigned char* in) {
    uint16_t u = (uint16_t)in[0] | ((uint16_t)in[1] << 8);
    return (int16_t)u;
}

static uint32_t
read_le_u32(const unsigned char* in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static int
probe_symbol_replay_format(dsd_opts* opts, dsd_state* state) {
    if (opts == NULL || state == NULL || opts->symbolfile == NULL) {
        return -1;
    }
    if (state->symbol_replay_header_checked) {
        return state->symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN ? -1 : 0;
    }

    state->symbol_replay_header_checked = 1;
    state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_LEGACY;
    state->symbol_replay_has_soft = 0;

    long pos = ftell(opts->symbolfile);
    unsigned char header[DSD_SYMBOL_CAPTURE_SOFT_HEADER_SIZE];
    size_t got = fread(header, 1, sizeof(header), opts->symbolfile);
    if (got >= 8U && memcmp(header, DSD_SYMBOL_CAPTURE_SOFT_MAGIC, 8) == 0) {
        if (got == sizeof(header) && header[8] == 2 && header[9] == DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE) {
            state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_SOFT;
            return 0;
        }
        state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
        return -1;
    }

    if (pos >= 0) {
        (void)fseek(opts->symbolfile, pos, SEEK_SET);
    } else {
        (void)fseek(opts->symbolfile, 0L, SEEK_SET);
    }
    return 0;
}

static int
read_soft_symbol_record(dsd_opts* opts, dsd_state* state, float* symbol_out) {
    unsigned char record[DSD_SYMBOL_CAPTURE_SOFT_RECORD_SIZE];
    if (opts == NULL || state == NULL || opts->symbolfile == NULL || symbol_out == NULL) {
        return 0;
    }
    if (fread(record, 1, sizeof(record), opts->symbolfile) != sizeof(record)) {
        return 0;
    }

    state->symbolc = record[0] & 3;
    state->symbol_replay_soft.reliability = record[1];
    state->symbol_replay_soft.llr[0] = read_le_i16(record + 2);
    state->symbol_replay_soft.llr[1] = read_le_i16(record + 4);
    uint32_t raw_symbol = read_le_u32(record + 6);
    DSD_MEMCPY(symbol_out, &raw_symbol, sizeof(raw_symbol));
    state->symbol_replay_soft_symbol = *symbol_out;
    state->symbol_replay_has_soft = 1;
    state->symbol_replay_soft_records++;
    return 1;
}

static int
dsd_pcm_input_take_staged_tail_sample(dsd_opts* opts, float* sample_out, int begin_tail) {
    if (!opts || !sample_out) {
        return 0;
    }
    if (begin_tail && !dsd_pcm_input_begin_resampler_tail(opts)) {
        return 0;
    }
    if (!dsd_pcm_input_stage_resampler_tail(opts) || opts->input_upsample_pos >= opts->input_upsample_len) {
        return 0;
    }

    *sample_out = opts->input_upsample_buf[opts->input_upsample_pos++];
    return 1;
}

/*
 * Centralized window selection helpers per modulation. These encapsulate
 * left/right offsets used during symbol decision and allow a single point for
 * future tuning. When freeze_window is enabled (env/config), defaults are used
 * and any per-protocol dynamic tweaks are disabled for A/B comparisons.
 */
static inline void
select_window_c4fm(const dsd_state* state, int* l_edge, int* r_edge, int freeze_window) {
    int l = 2;
    int r = 2;
    if (!freeze_window) {
        if (DSD_SYNC_IS_YSF(state->synctype) || DSD_SYNC_IS_DMR_BS(state->lastsynctype)
            || state->lastsynctype == DSD_SYNC_DMR_MS_VOICE || state->lastsynctype == DSD_SYNC_DMR_MS_DATA) {
            l = 1; // YSF, DMR, some NXDN cases
        } else {
            l = 2; // P25 and NXDN96 prefer wider left window
        }
    }
    *l_edge = l;
    *r_edge = r;
}

static inline void
select_window_qpsk(int* l_edge, int* r_edge, int freeze_window) {
    (void)freeze_window; /* no dynamic tweaks for QPSK at present */
    *l_edge = 1;         // pick i == center-1
    *r_edge = 2;         // pick i == center+2
}

static inline void
select_window_gfsk(int* l_edge, int* r_edge, int freeze_window) {
    (void)freeze_window; /* no dynamic tweaks for GFSK at present */
    *l_edge = 1;         // pick i == center-1
    *r_edge = 1;         // pick i == center+1
}

#ifdef USE_RADIO
static inline void
clamp_symbol_center_to_margin(int* center, int samples_per_symbol) {
    int min_c = 1;
    int max_c = samples_per_symbol - 2;
    if (*center < min_c) {
        *center = min_c;
    }
    if (*center > max_c) {
        *center = max_c;
    }
}

#endif

typedef struct {
    float sample;
    float sum;
    int count;
    int symbol_span;
    int l_edge_pre;
    int r_edge_pre;
    unsigned int analog_out_cap;
#ifdef USE_RADIO
    int rtl_output_kind;
    int rtl_direct_output;
    int rtl_symbol_rate_output;
    int rtl_fsk_discriminator_output;
    int rtl_profile_changed;
    int rtl_channel_profile;
    int rtl_symbol_rate_hz;
    int rtl_symbol_levels;
    uint32_t rtl_stream_generation;
    int cqpsk_symbol_rate;
#endif
} symbol_work_ctx;

static inline void
symbol_work_ctx_init(symbol_work_ctx* work, const dsd_state* state) {
    if (!work) {
        return;
    }
    *work = (symbol_work_ctx){0};
    work->symbol_span = 1;
#ifdef USE_RADIO
    work->rtl_symbol_levels = 4;
#endif
    if (!state) {
        return;
    }
    work->analog_out_cap = (unsigned int)(sizeof(state->analog_out) / sizeof(state->analog_out[0]));
}

static inline int
symbol_timing_debug_enabled(const dsd_opts* opts, const dsd_state* state, int have_sync) {
    return opts->symboltiming == 1 && have_sync == 0 && state->lastsynctype != DSD_SYNC_NONE;
}

static inline void
symbol_timing_debug_char(const dsd_opts* opts, const dsd_state* state, int have_sync, char c) {
    if (symbol_timing_debug_enabled(opts, state, have_sync)) {
        DSD_FPRINTF(stderr, "%c", c);
    }
}

static inline int
symbol_is_m17_sync(int lastsynctype) {
    return lastsynctype == DSD_SYNC_M17_STR_POS || lastsynctype == DSD_SYNC_M17_STR_NEG
           || lastsynctype == DSD_SYNC_M17_LSF_POS || lastsynctype == DSD_SYNC_M17_LSF_NEG
           || lastsynctype == DSD_SYNC_M17_PKT_POS || lastsynctype == DSD_SYNC_M17_PKT_NEG
           || lastsynctype == DSD_SYNC_M17_PRE_POS || lastsynctype == DSD_SYNC_M17_PRE_NEG
           || lastsynctype == DSD_SYNC_M17_EOT_POS || lastsynctype == DSD_SYNC_M17_EOT_NEG;
}

static inline float
symbol_apply_matched_filter(const dsd_opts* opts, const dsd_state* state, float sample, int rtl_symbol_rate_output,
                            int cqpsk_symbol_rate) {
    if (!opts->use_cosine_filter || rtl_symbol_rate_output || cqpsk_symbol_rate) {
        return sample;
    }
    if (DSD_SYNC_IS_DMR_BS(state->lastsynctype) || DSD_SYNC_IS_DMR_MS(state->lastsynctype)
        || DSD_SYNC_IS_YSF(state->lastsynctype)) {
        return dmr_filter(sample, state->samplesPerSymbol);
    }
    if (symbol_is_m17_sync(state->lastsynctype)) {
        return m17_filter(sample, state->samplesPerSymbol);
    }
    if (DSD_SYNC_IS_P25P1(state->lastsynctype)) {
        return p25_filter(sample, state->samplesPerSymbol);
    }
    if (DSD_SYNC_IS_DPMR(state->lastsynctype)) {
        if (opts->frame_dpmr == 1) {
            return dpmr_filter(sample, state->samplesPerSymbol);
        }
        return sample;
    }
    if (DSD_SYNC_IS_NXDN(state->lastsynctype)) {
        const dsd_nxdn_variant variant = dsd_frame_sync_active_nxdn_variant(opts, state);
        if (variant == DSD_NXDN_VARIANT_48) {
            return nxdn_filter(sample, state->samplesPerSymbol);
        }
        if (variant != DSD_NXDN_VARIANT_96) {
            return sample;
        }
        if (state->samplesPerSymbol == 8) {
            return sample;
        }
        return dmr_filter(sample, state->samplesPerSymbol);
    }
    return sample;
}

#ifdef DSD_NEO_TEST_HOOKS
float
dsd_symbol_test_apply_matched_filter(const dsd_opts* opts, const dsd_state* state, float sample,
                                     int rtl_symbol_rate_output, int cqpsk_symbol_rate) {
    return symbol_apply_matched_filter(opts, state, sample, rtl_symbol_rate_output, cqpsk_symbol_rate);
}
#endif

static inline float
symbol_apply_sync_clip(const dsd_state* state, int have_sync, float sample) {
    if (have_sync == 1 && state->rf_mod == 0) {
        if (sample > state->max) {
            return state->max;
        }
        if (sample < state->min) {
            return state->min;
        }
    }
    return sample;
}

static inline void
symbol_jitter_above_center(const dsd_opts* opts, dsd_state* state, int have_sync, int i, float sample) {
    if (sample > (state->maxref * 1.25f)) {
        if ((state->jitter < 0) && (state->rf_mod == 1)) {
            state->jitter = i;
        }
        symbol_timing_debug_char(opts, state, have_sync, 'O');
        return;
    }
    symbol_timing_debug_char(opts, state, have_sync, '+');
    if ((state->jitter < 0) && (state->lastsample < state->center) && (state->rf_mod != 1)) {
        state->jitter = i;
    }
}

static inline void
symbol_jitter_below_center(const dsd_opts* opts, dsd_state* state, int have_sync, int i, float sample) {
    if (sample < (state->minref * 1.25f)) {
        if ((state->jitter < 0) && (state->rf_mod == 1)) {
            state->jitter = i;
        }
        symbol_timing_debug_char(opts, state, have_sync, 'X');
        return;
    }
    symbol_timing_debug_char(opts, state, have_sync, '-');
    if ((state->jitter < 0) && (state->lastsample > state->center) && (state->rf_mod != 1)) {
        state->jitter = i;
    }
}

static inline void
symbol_update_jitter(const dsd_opts* opts, dsd_state* state, int have_sync, int i, float sample) {
    if (sample > state->center) {
        symbol_jitter_above_center(opts, state, have_sync, i, sample);
    } else {
        symbol_jitter_below_center(opts, state, have_sync, i, sample);
    }
}

static inline void
symbol_accumulate_add(symbol_work_ctx* work, float sample) {
    work->sum += sample;
    work->count++;
}

static inline int
symbol_accumulate_nxdn_window(const dsd_state* state, int i) {
    return state->samplesPerSymbol == 20 && i >= 7 && i <= 13;
}

#ifdef USE_RADIO
static inline int
symbol_accumulate_symbol_rate_override(const dsd_state* state, const symbol_work_ctx* work) {
    (void)state;
    return work->rtl_symbol_rate_output || work->cqpsk_symbol_rate;
}
#endif

static inline int
symbol_accumulate_sps5_window(const dsd_state* state, int i) {
    return state->samplesPerSymbol == 5 && i == 2;
}

static inline int
symbol_accumulate_c4fm_window(const dsd_state* state, const symbol_work_ctx* work, int i) {
    return i >= state->symbolCenter - work->l_edge_pre && i <= state->symbolCenter + work->r_edge_pre;
}

static inline int
symbol_accumulate_other_window(const dsd_state* state, const symbol_work_ctx* work, int i) {
    if (state->rf_mod == 2 && state->samplesPerSymbol <= 4) {
        return i == state->symbolCenter;
    }
    return i == state->symbolCenter - work->l_edge_pre || i == state->symbolCenter + work->r_edge_pre;
}

static inline void
symbol_accumulate_sample(const dsd_state* state, symbol_work_ctx* work, int i, float sample) {
    if (symbol_accumulate_nxdn_window(state, i)) {
        symbol_accumulate_add(work, sample);
    }
#ifdef USE_RADIO
    if (symbol_accumulate_symbol_rate_override(state, work)) {
        symbol_accumulate_add(work, sample);
        return;
    }
#endif
    if (symbol_accumulate_sps5_window(state, i)) {
        symbol_accumulate_add(work, sample);
        return;
    }
    if (state->rf_mod == 0) {
        if (symbol_accumulate_c4fm_window(state, work, i)) {
            symbol_accumulate_add(work, sample);
        }
        return;
    }
    if (symbol_accumulate_other_window(state, work, i)) {
        symbol_accumulate_add(work, sample);
    }
}

static inline void
symbol_adjust_timing_nxdn(const dsd_state* state, int* i) {
    if ((state->jitter >= 7) && (state->jitter <= 10)) {
        (*i)--;
    } else if ((state->jitter >= 11) && (state->jitter <= 14)) {
        (*i)++;
    }
}

static inline void
symbol_adjust_timing_qpsk(const dsd_state* state, int* i) {
    if ((state->jitter >= 0) && (state->jitter < state->symbolCenter)) {
        (*i)++;
    } else if ((state->jitter > state->symbolCenter) && (state->jitter < 10)) {
        (*i)--;
    }
}

static inline void
symbol_adjust_timing_gfsk(const dsd_state* state, int* i) {
    if ((state->jitter >= state->symbolCenter - 1) && (state->jitter <= state->symbolCenter)) {
        (*i)--;
    } else if ((state->jitter >= state->symbolCenter + 1) && (state->jitter <= state->symbolCenter + 2)) {
        (*i)++;
    }
}

static inline void
symbol_adjust_timing_c4fm(const dsd_state* state, int* i) {
    if ((state->jitter > 0) && (state->jitter <= state->symbolCenter)) {
        (*i)--;
    } else if ((state->jitter > state->symbolCenter) && (state->jitter < state->samplesPerSymbol)) {
        (*i)++;
    }
}

static inline void
symbol_adjust_timing_index(dsd_state* state, int have_sync, int symbol_span, int* i) {
    if (symbol_span <= 1 || *i != 0 || have_sync != 0) {
        return;
    }
    if (state->jitter < 0) {
        return;
    }
    if (state->samplesPerSymbol == 20) {
        symbol_adjust_timing_nxdn(state, i);
    } else if (state->rf_mod == 1) {
        symbol_adjust_timing_qpsk(state, i);
    } else if (state->rf_mod == 2) {
        symbol_adjust_timing_gfsk(state, i);
    } else if (state->rf_mod == 0) {
        symbol_adjust_timing_c4fm(state, i);
    }
    state->jitter = -1;
}

#ifdef DSD_NEO_TEST_HOOKS
void
dsd_symbol_test_select_window(int rf_mod, int synctype, int lastsynctype, int freeze_window, int* l_edge, int* r_edge) {
    if (!l_edge || !r_edge) {
        return;
    }
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.rf_mod = rf_mod;
    state.synctype = synctype;
    state.lastsynctype = lastsynctype;
    if (rf_mod == 0) {
        select_window_c4fm(&state, l_edge, r_edge, freeze_window);
    } else if (rf_mod == 1) {
        select_window_qpsk(l_edge, r_edge, freeze_window);
    } else {
        select_window_gfsk(l_edge, r_edge, freeze_window);
    }
}

int
dsd_symbol_test_adjust_timing_index(int samples_per_symbol, int symbol_center, int rf_mod, int jitter, int have_sync,
                                    int symbol_span, int start_i, int* jitter_after) {
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    state.samplesPerSymbol = samples_per_symbol;
    state.symbolCenter = symbol_center;
    state.rf_mod = rf_mod;
    state.jitter = jitter;
    int i = start_i;
    symbol_adjust_timing_index(&state, have_sync, symbol_span, &i);
    if (jitter_after) {
        *jitter_after = state.jitter;
    }
    return i;
}

int
dsd_symbol_test_is_m17_sync(int lastsynctype) {
    return symbol_is_m17_sync(lastsynctype);
}
#endif

#ifdef USE_RADIO
/*
 * Nudge symbolCenter by ±1 based on a smoothed TED residual when available.
 * Guards against oscillation using a small deadband and a cooldown period.
 *
 * Safety gates:
 *  - Only when using RTL input (audio_in_type == AUDIO_IN_RTL)
 *  - Only when not currently synchronized (have_sync == 0)
 *  - Only for C4FM path (rf_mod == 0) to avoid QPSK perturbations
 */
static inline int
maybe_auto_center_allowed(const dsd_opts* opts, const dsd_state* state, int have_sync) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int freeze_window = (cfg && cfg->window_freeze_is_set) ? (cfg->window_freeze != 0) : 0;
    if (freeze_window) {
        return 0; // explicit freeze requested
    }
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return 0; // only when using RTL stream/demod pipeline
    }
    if (have_sync != 0) {
        return 0;
    }
    if (state->rf_mod != 0) {
        return 0; // limit to C4FM for now; avoid QPSK
    }
    return 1;
}

static inline int
maybe_auto_center_step_direction(int e_ema, int deadband, int* run_dir, int* run_len, int* dir_out) {
    int dir = 0;
    if (e_ema > deadband) {
        dir = +1; /* sample was early; center → right */
    } else if (e_ema < -deadband) {
        dir = -1; /* sample was late; center → left */
    } else {
        *run_dir = 0;
        *run_len = 0;
        return 0;
    }
    if (dir == *run_dir) {
        (*run_len)++;
    } else {
        *run_dir = dir;
        *run_len = 1;
    }
    *dir_out = dir;
    return 1;
}

static inline void
maybe_auto_center(const dsd_opts* opts, dsd_state* state, int have_sync) {
    if (!maybe_auto_center_allowed(opts, state, have_sync)) {
        return;
    }
    /* Cooldown to avoid rapid flips */
    static int cooldown = 0;
    if (cooldown > 0) {
        cooldown--;
        return;
    }
    /* Read smoothed CQPSK timing residual in Q14 units (0 when unavailable). */
    int e_ema = dsd_rtl_stream_metrics_hook_cqpsk_timing_bias();
    if (e_ema == 0) {
        return;
    }
    /* Small deadband and persistence guard.
     * Q14 scaling: 1024 ~= 0.0625 normalized residual. */
    const int deadband = 1024;
    static int run_dir = 0; /* -1, 0, +1 */
    static int run_len = 0;
    int dir = 0;
    if (!maybe_auto_center_step_direction(e_ema, deadband, &run_dir, &run_len, &dir)) {
        return;
    }
    /* Require brief persistence before nudging center. */
    if (run_len >= 6) {
        int c = state->symbolCenter + dir;
        /* Keep a reasonable margin within [0..samplesPerSymbol-1] */
        clamp_symbol_center_to_margin(&c, state->samplesPerSymbol);
        state->symbolCenter = c;
        cooldown = 12; /* short cooldown after each nudge */
        run_len = 0;
    }
}
#endif

#ifdef USE_RADIO
/*
 * When using the RTL pipeline without resampling to 48 kHz, adjust
 * samplesPerSymbol and symbolCenter proportional to the current output rate
 * to preserve decoder timing windows. Runs cheaply with a rate-change guard.
 */
static inline void
maybe_adjust_sps_for_output_rate(const dsd_opts* opts, dsd_state* state) {
    if (opts->audio_in_type != AUDIO_IN_RTL) {
        return; /* only for RTL input */
    }
    static unsigned int last_rate = 0;
    unsigned int Fs = 0;
    if (state->rtl_ctx) {
        Fs = dsd_rtl_stream_metrics_hook_output_rate_hz();
    }
    if (Fs == 0 || Fs == last_rate) {
        return;
    }
    dsd_audio_rescale_symbol_timing(state, last_rate != 0 ? (int)last_rate : 48000, (int)Fs);
    last_rate = Fs;
}
#endif

#ifdef USE_RADIO
enum {
    RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR_LOCAL = 1,
    RTL_STREAM_OUTPUT_SYMBOL_CQPSK_LOCAL = 2,
};

enum {
    RTL_SYMBOL_CACHE_EMPTY = 0,
    RTL_SYMBOL_CACHE_READY = 1,
    RTL_SYMBOL_CACHE_RETRY = 2,
};

static inline int
rtl_direct_output_active(int output_kind) {
    return output_kind == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR_LOCAL
           || output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK_LOCAL;
}

static inline int
rtl_symbol_rate_output_active(int output_kind) {
    return output_kind == RTL_STREAM_OUTPUT_SYMBOL_CQPSK_LOCAL;
}

static inline int
rtl_fsk_discriminator_output_active(int output_kind) {
    return output_kind == RTL_STREAM_OUTPUT_FSK_DISCRIMINATOR_LOCAL;
}

static inline int
rtl_symbol_current_profile(int* output_kind, int* channel_profile, int* symbol_rate_hz, int* levels,
                           uint32_t* generation) {
    int current_output_kind = dsd_rtl_stream_metrics_hook_output_kind();
    if (output_kind) {
        *output_kind = current_output_kind;
    }
    if (generation) {
        *generation = dsd_rtl_stream_metrics_hook_stream_generation();
    }
    if (!rtl_direct_output_active(current_output_kind)) {
        if (channel_profile) {
            *channel_profile = 0;
        }
        if (symbol_rate_hz) {
            *symbol_rate_hz = 0;
        }
        if (levels) {
            *levels = 4;
        }
        return 0;
    }

    int current_symbol_rate_hz = 0;
    int current_levels = 4;
    int current_channel_profile = 0;
    (void)dsd_rtl_stream_metrics_hook_symbol_profile(&current_symbol_rate_hz, &current_levels,
                                                     &current_channel_profile);
    if (current_levels != 2) {
        current_levels = 4;
    }
    if (channel_profile) {
        *channel_profile = current_channel_profile;
    }
    if (symbol_rate_hz) {
        *symbol_rate_hz = current_symbol_rate_hz;
    }
    if (levels) {
        *levels = current_levels;
    }
    return 1;
}

static inline void
apply_rtl_symbol_thresholds(dsd_state* state, int levels) {
    if (!state) {
        return;
    }
    state->center = 0.0f;
    if (levels == 2) {
        state->min = -1.0f;
        state->max = 1.0f;
        state->lmid = -0.5f;
        state->umid = 0.5f;
        state->minref = -0.8f;
        state->maxref = 0.8f;
    } else {
        state->min = -3.0f;
        state->max = 3.0f;
        state->lmid = -2.0f;
        state->umid = 2.0f;
        state->minref = -2.4f;
        state->maxref = 2.4f;
    }
}

static inline void
rtl_symbol_cache_publish_pending(dsd_state* state) {
    if (!state) {
        return;
    }
    int pending = 0;
    if (state->rtl_symbol_cache_pos < state->rtl_symbol_cache_len) {
        pending = state->rtl_symbol_cache_len - state->rtl_symbol_cache_pos;
    }
    int delta = pending - state->rtl_symbol_cache_published_pending;
    if (delta != 0) {
        dsd_rtl_stream_metrics_hook_symbol_cache_pending_delta(delta);
        state->rtl_symbol_cache_published_pending = pending;
    }
}

static inline void
rtl_symbol_cache_reset_pending(dsd_state* state) {
    if (!state) {
        return;
    }
    state->rtl_symbol_cache_pos = 0;
    state->rtl_symbol_cache_len = 0;
    rtl_symbol_cache_publish_pending(state);
}

static inline int
rtl_symbol_cache_pop(dsd_state* state, uint32_t generation, float* sample_out) {
    if (!state || !sample_out || state->rtl_symbol_cache_pos >= state->rtl_symbol_cache_len) {
        return RTL_SYMBOL_CACHE_EMPTY;
    }
    if (dsd_rtl_stream_metrics_hook_stream_generation() != generation) {
        rtl_symbol_cache_reset_pending(state);
        return RTL_SYMBOL_CACHE_RETRY;
    }
    float sample = state->rtl_symbol_cache[state->rtl_symbol_cache_pos++];
    rtl_symbol_cache_publish_pending(state);
    if (dsd_rtl_stream_metrics_hook_stream_generation() != generation) {
        rtl_symbol_cache_reset_pending(state);
        return RTL_SYMBOL_CACHE_RETRY;
    }
    *sample_out = sample;
    return RTL_SYMBOL_CACHE_READY;
}

static inline int
rtl_symbol_cache_profile(dsd_state* state, int output_kind, int channel_profile, int symbol_rate_hz, int levels,
                         uint32_t generation) {
    if (!state) {
        return 0;
    }
    if (state->rtl_symbol_cache_output_kind != output_kind || state->rtl_symbol_cache_channel_profile != channel_profile
        || state->rtl_symbol_cache_symbol_rate_hz != symbol_rate_hz || state->rtl_symbol_cache_levels != levels
        || state->rtl_symbol_cache_generation != generation) {
        rtl_symbol_cache_reset_pending(state);
        state->rtl_symbol_cache_output_kind = output_kind;
        state->rtl_symbol_cache_channel_profile = channel_profile;
        state->rtl_symbol_cache_symbol_rate_hz = symbol_rate_hz;
        state->rtl_symbol_cache_levels = levels;
        state->rtl_symbol_cache_generation = generation;
        return 1;
    }
    return 0;
}

static inline void
rtl_symbol_cache_clear(dsd_state* state) {
    if (!state) {
        return;
    }
    rtl_symbol_cache_reset_pending(state);
    state->rtl_symbol_cache_output_kind = 0;
    state->rtl_symbol_cache_channel_profile = 0;
    state->rtl_symbol_cache_symbol_rate_hz = 0;
    state->rtl_symbol_cache_levels = 0;
    state->rtl_symbol_cache_generation = 0;
}

#ifdef DSD_NEO_TEST_HOOKS
int
dsd_symbol_test_auto_center_step_direction(int e_ema, int deadband, int* run_dir, int* run_len, int* dir_out) {
    if (!run_dir || !run_len || !dir_out) {
        return 0;
    }
    return maybe_auto_center_step_direction(e_ema, deadband, run_dir, run_len, dir_out);
}

int
dsd_symbol_test_rtl_cache_and_center_contract(int out_values[10]) {
    if (!out_values) {
        return 0;
    }

    int center = -5;
    clamp_symbol_center_to_margin(&center, 10);
    out_values[0] = center;
    center = 99;
    clamp_symbol_center_to_margin(&center, 10);
    out_values[1] = center;

    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    out_values[2] = rtl_symbol_cache_profile(&state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK_LOCAL, 3, 4800, 4, 0U);
    out_values[3] = rtl_symbol_cache_profile(&state, RTL_STREAM_OUTPUT_SYMBOL_CQPSK_LOCAL, 3, 4800, 4, 0U);
    out_values[4] = state.rtl_symbol_cache_output_kind;
    out_values[5] = state.rtl_symbol_cache_symbol_rate_hz;

    state.rtl_symbol_cache[0] = 1.25f;
    state.rtl_symbol_cache[1] = -2.5f;
    state.rtl_symbol_cache_len = 2;
    state.rtl_symbol_cache_pos = 0;
    state.rtl_symbol_cache_generation = 0U;
    float sample = 0.0f;
    out_values[6] = rtl_symbol_cache_pop(&state, 0U, &sample);
    out_values[7] = (int)lrintf(sample * 100.0f);
    out_values[8] = state.rtl_symbol_cache_pos;
    rtl_symbol_cache_clear(&state);
    out_values[9] = state.rtl_symbol_cache_len + state.rtl_symbol_cache_output_kind + state.rtl_symbol_cache_levels;
    return 10;
}
#endif

static int
rtl_symbol_cache_refill(dsd_state* state, int output_kind, int channel_profile, int symbol_rate_hz, int levels,
                        uint32_t* generation, uint32_t expected_generation, float* sample_out) {
    int got = 0;
    uint32_t read_generation = generation ? *generation : expected_generation;
    if (dsd_rtl_stream_io_hook_read(state, state->rtl_symbol_cache, DSD_RTL_SYMBOL_CACHE_CAP, &got) < 0 || got <= 0) {
        rtl_symbol_cache_reset_pending(state);
        return RTL_SYMBOL_CACHE_EMPTY;
    }
    if (got > DSD_RTL_SYMBOL_CACHE_CAP) {
        got = DSD_RTL_SYMBOL_CACHE_CAP;
    }

    uint32_t fill_generation = dsd_rtl_stream_metrics_hook_stream_generation();
    if (fill_generation != read_generation) {
        if (generation) {
            *generation = fill_generation;
        }
        rtl_symbol_cache_reset_pending(state);
        return RTL_SYMBOL_CACHE_RETRY;
    }

    state->rtl_symbol_cache_len = got;
    state->rtl_symbol_cache_pos = 0;
    state->rtl_symbol_cache_output_kind = output_kind;
    state->rtl_symbol_cache_channel_profile = channel_profile;
    state->rtl_symbol_cache_symbol_rate_hz = symbol_rate_hz;
    state->rtl_symbol_cache_levels = levels;
    state->rtl_symbol_cache_generation = generation ? *generation : fill_generation;
    rtl_symbol_cache_publish_pending(state);
    return rtl_symbol_cache_pop(state, read_generation, sample_out);
}

static int
rtl_symbol_cache_take(dsd_state* state, int output_kind, int channel_profile, int symbol_rate_hz, int levels,
                      uint32_t* generation, float* sample_out) {
    if (!state || !sample_out) {
        return RTL_SYMBOL_CACHE_EMPTY;
    }

    uint32_t current_generation = dsd_rtl_stream_metrics_hook_stream_generation();
    if (generation && current_generation != *generation) {
        *generation = current_generation;
        rtl_symbol_cache_reset_pending(state);
        return RTL_SYMBOL_CACHE_RETRY;
    }
    uint32_t expected_generation = generation ? *generation : current_generation;
    int pop_status = rtl_symbol_cache_pop(state, expected_generation, sample_out);
    if (pop_status != RTL_SYMBOL_CACHE_EMPTY) {
        return pop_status;
    }

    return rtl_symbol_cache_refill(state, output_kind, channel_profile, symbol_rate_hz, levels, generation,
                                   expected_generation, sample_out);
}
#endif

static inline short
symbol_scale_pcm_i16(short s, int input_volume_multiplier) {
    if (input_volume_multiplier > 1) {
        int v = (int)s * input_volume_multiplier;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        return (short)v;
    }
    return s;
}

static inline unsigned int
symbol_analog_block_size(const dsd_opts* opts, const dsd_state* state, unsigned int analog_out_cap) {
    unsigned int analog_block = analog_out_cap;
#ifndef USE_RADIO
    (void)state;
#endif
    if (opts->audio_in_type == AUDIO_IN_RTL) {
#ifdef USE_RADIO
        unsigned int Fs = 0;
        if (state->rtl_ctx) {
            Fs = dsd_rtl_stream_metrics_hook_output_rate_hz();
        }
        if (Fs > 0) {
            analog_block = (unsigned int)(((uint64_t)Fs * 20 + 999) / 1000);
            if (analog_block < 320) {
                analog_block = 320;
            } else if (analog_block > 4000) {
                analog_block = 4000;
            }
        }
#endif
    }
#ifdef USE_RADIO
    if (analog_block > analog_out_cap) {
        analog_block = analog_out_cap;
    }
#endif
    return analog_block;
}

static inline void
symbol_convert_analog_block_to_i16(dsd_state* state, unsigned int analog_block) {
    for (unsigned int i = 0; i < analog_block; i++) {
        state->analog_out[i] = float_to_int16_clip(state->analog_out_f[i]);
    }
}

#ifdef DSD_NEO_TEST_HOOKS
unsigned int
dsd_symbol_test_convert_analog_block_to_i16(const float* input, short* output, unsigned int count) {
    if (!input || !output) {
        return 0U;
    }
    static dsd_state state;
    DSD_MEMSET(&state, 0, sizeof(state));
    unsigned int cap = (unsigned int)(sizeof(state.analog_out_f) / sizeof(state.analog_out_f[0]));
    unsigned int n = count < cap ? count : cap;
    for (unsigned int i = 0; i < n; i++) {
        state.analog_out_f[i] = input[i];
    }
    symbol_convert_analog_block_to_i16(&state, n);
    for (unsigned int i = 0; i < n; i++) {
        output[i] = state.analog_out[i];
    }
    return n;
}
#endif

static inline void
symbol_update_unsynced_input_power(dsd_opts* opts, dsd_state* state, unsigned int analog_block) {
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return;
    }
    dsd_input_level_snapshot snapshot;
    if (dsd_input_level_metrics_from_pcm_f32_i16_scale(state->analog_out_f, analog_block, 1U,
                                                       DSD_INPUT_LEVEL_SOURCE_PCM, &snapshot)
        == 0) {
        dsd_input_level_publish(opts, state, &snapshot, DSD_INPUT_LEVEL_NOTIFY_ALL);
    }
}

static inline void
symbol_write_unsynced_raw_wav(dsd_opts* opts, dsd_state* state, unsigned int analog_block) {
    if (opts->wav_out_raw != NULL && opts->frame_nxdn48 == 0 && opts->frame_nxdn96 == 0 && opts->frame_dpmr == 0
        && opts->frame_m17 == 0) {
        symbol_convert_analog_block_to_i16(state, analog_block);
        symbol_write_wav_short_block(opts->wav_out_raw, state->analog_out, analog_block, "symbol raw WAV");
        sf_write_sync(opts->wav_out_raw);
    }
}

static inline void
symbol_apply_unsynced_filters(dsd_opts* opts, dsd_state* state, unsigned int analog_block) {
    if (opts->use_lpf == 1) {
        lpf_f(state, state->analog_out_f, (int)analog_block);
    }
    if (opts->use_hpf == 1) {
        hpf_f(state, state->analog_out_f, (int)analog_block);
    }
    if (opts->use_pbf == 1) {
        pbf_f(state, state->analog_out_f, (int)analog_block);
    }
    if (opts->audio_gainA > 0.0f) {
        analog_gain_f(opts, state, state->analog_out_f, (int)analog_block);
    } else {
        agsm_f(opts, state, state->analog_out_f, (int)analog_block);
    }
}

static inline void
symbol_output_unsynced_analog(dsd_opts* opts, dsd_state* state, unsigned int analog_block) {
    if ((opts->rtl_pwr > opts->rtl_squelch_level) && opts->monitor_input_audio == 1 && state->carrier == 0
        && opts->audio_out == 1) {
        symbol_convert_analog_block_to_i16(state, analog_block);
        size_t bytes = (size_t)analog_block * sizeof(short);
        if (opts->audio_out_type == 0 && opts->audio_raw_out) {
            dsd_audio_write(opts->audio_raw_out, state->analog_out, analog_block);
        }
        if (opts->audio_out_type == 8) {
            dsd_udp_audio_hook_blast_analog(opts, state, bytes, state->analog_out);
        }
        if (opts->trunk_enable != 1) {
            state->last_cc_sync_time = time(NULL);
            state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
        }
        if (!(opts->trunk_enable == 1 && opts->trunk_is_tuned == 1)) {
            state->last_vc_sync_time = time(NULL);
            state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
        }
    }
}

static inline void
symbol_reset_analog_buffers(dsd_state* state) {
    DSD_MEMSET(state->analog_out_f, 0, sizeof(state->analog_out_f));
    DSD_MEMSET(state->analog_out, 0, sizeof(state->analog_out));
    state->analog_sample_counter = 0;
}

static inline void
symbol_finalize_unsynced_analog_block(dsd_opts* opts, dsd_state* state, unsigned int analog_block) {
    symbol_update_unsynced_input_power(opts, state, analog_block);
    symbol_write_unsynced_raw_wav(opts, state, analog_block);
    symbol_apply_unsynced_filters(opts, state, analog_block);
    symbol_output_unsynced_analog(opts, state, analog_block);
    symbol_reset_analog_buffers(state);
}

static inline void
symbol_process_unsynced_analog(dsd_opts* opts, dsd_state* state, unsigned int analog_out_cap, float sample) {
    unsigned int analog_block = symbol_analog_block_size(opts, state, analog_out_cap);
    if (analog_block == 0) {
        return;
    }
    if ((unsigned int)state->analog_sample_counter >= analog_block) {
        state->analog_sample_counter = (int)analog_block - 1;
    }
    state->analog_out_f[state->analog_sample_counter++] = sample;
    if ((unsigned int)state->analog_sample_counter == analog_block) {
        symbol_finalize_unsynced_analog_block(opts, state, analog_block);
    }
}

static inline void
symbol_process_synced_analog(dsd_opts* opts, dsd_state* state, unsigned int analog_out_cap, float sample) {
    int analog_max_index = (int)analog_out_cap - 1;
    if (state->analog_sample_counter > analog_max_index) {
        state->analog_sample_counter = analog_max_index;
    }
    state->analog_out_f[state->analog_sample_counter++] = sample;
    if ((unsigned int)state->analog_sample_counter == analog_out_cap) {
        if (opts->wav_out_raw != NULL) {
            symbol_convert_analog_block_to_i16(state, analog_out_cap);
            symbol_write_wav_short_block(opts->wav_out_raw, state->analog_out, analog_out_cap, "symbol raw WAV");
            sf_write_sync(opts->wav_out_raw);
        }
        symbol_reset_analog_buffers(state);
    }
}

static inline void
symbol_process_analog_capture(dsd_opts* opts, dsd_state* state, const symbol_work_ctx* work, int have_sync) {
#ifdef USE_RADIO
    if (work->rtl_symbol_rate_output) {
        return;
    }
#endif
    if (have_sync == 0) {
        symbol_process_unsynced_analog(opts, state, work->analog_out_cap, work->sample);
    } else if (have_sync == 1) {
        symbol_process_synced_analog(opts, state, work->analog_out_cap, work->sample);
    }
}

static inline int
symbol_read_sample_pulse(dsd_opts* opts, float* sample_out) {
    short s = 0;
    if (opts->audio_in_stream) {
        dsd_audio_read(opts->audio_in_stream, &s, 1);
    }
    s = symbol_scale_pcm_i16(s, opts->input_volume_multiplier);
    *sample_out = (float)s;
    return 1;
}

static inline void
symbol_close_audio_in_file(dsd_opts* opts) {
    if (opts == NULL || opts->audio_in_file == NULL) {
        return;
    }
    sf_close(opts->audio_in_file);
    opts->audio_in_file = NULL;
}

static inline int
symbol_stop_after_shutdown(float* sample_out) {
    if (exitflag != 1) {
        return 0;
    }
    if (sample_out != NULL) {
        *sample_out = 0.0f;
    }
    return 1;
}

static inline int
symbol_open_pulse_input_and_reconfigure_output(dsd_opts* opts, dsd_state* state) {
    opts->audio_in_type = AUDIO_IN_PULSE;
    if (openAudioInput(opts) != 0) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    if (dsd_audio_reconfigure_output_for_input_policy(opts) != 0) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    return 1;
}

static inline int
symbol_read_sample_stdin(dsd_opts* opts, dsd_state* state, float* sample_out) {
    if (symbol_stop_after_shutdown(sample_out)) {
        return 0;
    }
    if (opts->audio_in_file == NULL) {
        dsd_request_shutdown(opts, state);
        return 0;
    }

    short s = 0;
    sf_count_t result = sf_read_short(opts->audio_in_file, &s, 1);
    s = symbol_scale_pcm_i16(s, opts->input_volume_multiplier);
    *sample_out = (float)s;
    if (result == 0) {
        if (dsd_pcm_input_take_staged_tail_sample(opts, sample_out, 1)) {
            return 1;
        }
        symbol_close_audio_in_file(opts);
        dsd_request_shutdown(opts, state);
        return 0;
    }
    if (dsd_pcm_input_uses_staged_resampler(opts)) {
        dsd_pcm_input_stage_resample(opts, *sample_out);
        *sample_out = opts->input_upsample_buf[opts->input_upsample_pos++];
    }
    return 1;
}

static inline int
symbol_read_sample_wav(dsd_opts* opts, dsd_state* state, float* sample_out) {
    if (symbol_stop_after_shutdown(sample_out)) {
        return 0;
    }
    if (opts->audio_in_file == NULL) {
        dsd_request_shutdown(opts, state);
        return 0;
    }

    short s = 0;
    sf_count_t result = sf_read_short(opts->audio_in_file, &s, 1);
    s = symbol_scale_pcm_i16(s, opts->input_volume_multiplier);
    *sample_out = (float)s;
    if (result == 0) {
        if (dsd_pcm_input_take_staged_tail_sample(opts, sample_out, 1)) {
            return 1;
        }
        symbol_close_audio_in_file(opts);
        DSD_FPRINTF(stderr, "\nEnd of %s\n", opts->audio_in_dev);
        if (opts->audio_out_type == 0 && dsd_opts_frontend_active(opts)) {
            return symbol_open_pulse_input_and_reconfigure_output(opts, state);
        }
        dsd_request_shutdown(opts, state);
        return 0;
    }
    if (dsd_pcm_input_uses_staged_resampler(opts)) {
        dsd_pcm_input_stage_resample(opts, *sample_out);
        *sample_out = opts->input_upsample_buf[opts->input_upsample_pos++];
    }
    return 1;
}

#ifdef USE_RADIO
static inline void
symbol_maybe_publish_rtl_input_level(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL) {
        return;
    }
    if ((state->symbolcnt & 0x3FU) != 0) {
        return;
    }
    dsd_input_level_snapshot snapshot;
    if (dsd_rtl_stream_metrics_hook_input_level(&snapshot) == 0 && snapshot.sample_count > 0U) {
        dsd_input_level_publish(opts, state, &snapshot, DSD_INPUT_LEVEL_NOTIFY_RF);
    }
}

static inline int
symbol_refresh_rtl_profile(dsd_state* state, symbol_work_ctx* work) {
    if (!work) {
        return 0;
    }
    work->rtl_direct_output =
        rtl_symbol_current_profile(&work->rtl_output_kind, &work->rtl_channel_profile, &work->rtl_symbol_rate_hz,
                                   &work->rtl_symbol_levels, &work->rtl_stream_generation);
    work->rtl_symbol_rate_output =
        (work->rtl_direct_output && rtl_symbol_rate_output_active(work->rtl_output_kind)) ? 1 : 0;
    work->rtl_fsk_discriminator_output =
        (work->rtl_direct_output && rtl_fsk_discriminator_output_active(work->rtl_output_kind)) ? 1 : 0;
    if (work->rtl_direct_output) {
        work->rtl_profile_changed |=
            rtl_symbol_cache_profile(state, work->rtl_output_kind, work->rtl_channel_profile, work->rtl_symbol_rate_hz,
                                     work->rtl_symbol_levels, work->rtl_stream_generation);
    } else {
        rtl_symbol_cache_clear(state);
    }
    return work->rtl_direct_output;
}

static inline int
symbol_rtl_fsk_output_rate_hz(const dsd_opts* opts) {
    unsigned int output_rate = dsd_rtl_stream_metrics_hook_output_rate_hz();
    int output_rate_hz = output_rate > 0U ? (int)output_rate : dsd_opts_current_input_timing_rate(opts);
    return output_rate_hz > 0 ? output_rate_hz : 48000;
}

static inline int
symbol_rtl_fsk_symbol_rate_hz(const symbol_work_ctx* work) {
    return work->rtl_symbol_rate_hz > 0 ? work->rtl_symbol_rate_hz : 4800;
}

static inline void
symbol_reset_rtl_fsk_discriminator_slicer(dsd_state* state) {
    if (!state) {
        return;
    }

    state->center = 0.0f;
    state->min = -30000.0f;
    state->max = 30000.0f;
    state->lmid = -20000.0f;
    state->umid = 20000.0f;
    state->minref = -24000.0f;
    state->maxref = 24000.0f;
    int minmax_cap = (int)(sizeof(state->minbuf) / sizeof(state->minbuf[0]));
    for (int i = 0; i < minmax_cap; i++) {
        state->minbuf[i] = state->min;
        state->maxbuf[i] = state->max;
    }
    state->midx = 0;
    dsd_state_invalidate_minmax_sums(state);
}

static inline int
symbol_reset_rtl_fsk_timing_if_needed(dsd_state* state, int output_rate_hz, int symbol_rate_hz,
                                      const symbol_work_ctx* work) {
    if (state->rtl_fsk_sps_num == output_rate_hz && state->rtl_fsk_sps_den == symbol_rate_hz
        && !work->rtl_profile_changed) {
        return 0;
    }
    state->rtl_fsk_sps_num = output_rate_hz;
    state->rtl_fsk_sps_den = symbol_rate_hz;
    state->rtl_fsk_sps_accum = 0;
    state->jitter = -1;
    symbol_reset_rtl_fsk_discriminator_slicer(state);
    return 1;
}

static inline int
symbol_rtl_fsk_whole_sps(int output_rate_hz, int symbol_rate_hz, int* rem_out) {
    int whole = output_rate_hz / symbol_rate_hz;
    int rem = output_rate_hz % symbol_rate_hz;
    if (whole < 2) {
        whole = 2;
        rem = 0;
    }
    if (whole > 64) {
        whole = 64;
        rem = 0;
    }
    *rem_out = rem;
    return whole;
}

static inline int
symbol_rtl_fsk_next_sps(dsd_state* state, int output_rate_hz, int symbol_rate_hz) {
    int rem = 0;
    int sps = symbol_rtl_fsk_whole_sps(output_rate_hz, symbol_rate_hz, &rem);
    if (rem <= 0 || state->rtl_fsk_sps_den <= 0) {
        return sps;
    }

    int accum = state->rtl_fsk_sps_accum + rem;
    if (accum >= state->rtl_fsk_sps_den) {
        sps++;
        accum -= state->rtl_fsk_sps_den;
    }
    state->rtl_fsk_sps_accum = accum;
    return sps > 64 ? 64 : sps;
}

static inline void
symbol_apply_rtl_fsk_discriminator_timing(const dsd_opts* opts, dsd_state* state, const symbol_work_ctx* work) {
    if (!opts || !state || !work) {
        return;
    }
    int output_rate_hz = symbol_rtl_fsk_output_rate_hz(opts);
    int symbol_rate_hz = symbol_rtl_fsk_symbol_rate_hz(work);
    (void)symbol_reset_rtl_fsk_timing_if_needed(state, output_rate_hz, symbol_rate_hz, work);

    state->samplesPerSymbol = symbol_rtl_fsk_next_sps(state, output_rate_hz, symbol_rate_hz);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
}

static inline int
symbol_read_cached_rtl_sample(dsd_opts* opts, dsd_state* state, float* sample_out, symbol_work_ctx* work) {
    for (;;) {
        int cache_status =
            rtl_symbol_cache_take(state, work->rtl_output_kind, work->rtl_channel_profile, work->rtl_symbol_rate_hz,
                                  work->rtl_symbol_levels, &work->rtl_stream_generation, sample_out);
        if (cache_status == RTL_SYMBOL_CACHE_READY) {
            return 1;
        }
        if (cache_status != RTL_SYMBOL_CACHE_RETRY) {
            dsd_request_shutdown(opts, state);
            return 0;
        }
        symbol_refresh_rtl_profile(state, work);
        if (!work->rtl_fsk_discriminator_output) {
            return 0;
        }
        if (work->rtl_profile_changed || state->samplesPerSymbol <= 1) {
            symbol_apply_rtl_fsk_discriminator_timing(opts, state, work);
        }
    }
}

static inline int
symbol_read_sample_rtl(dsd_opts* opts, dsd_state* state, float* sample_out, symbol_work_ctx* work) {
    if (!state->rtl_ctx) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    if (work->rtl_fsk_discriminator_output) {
        if (!symbol_read_cached_rtl_sample(opts, state, sample_out, work)) {
            return 0;
        }
        opts->rtl_pwr = dsd_rtl_stream_io_hook_return_pwr(state);
        return 1;
    }
    int got = 0;
    if (dsd_rtl_stream_io_hook_read(state, sample_out, 1, &got) < 0 || got != 1) {
        dsd_request_shutdown(opts, state);
        return 0;
    }
    opts->rtl_pwr = dsd_rtl_stream_io_hook_return_pwr(state);
    if (!work->rtl_symbol_rate_output && !work->cqpsk_symbol_rate && !work->rtl_fsk_discriminator_output) {
        *sample_out *= opts->rtl_volume_multiplier;
    }
    return 1;
}
#endif

static int
symbol_read_sample_tcp(dsd_opts* opts, dsd_state* state, float* sample_out) {
    short s = 0;
    int tcp_result = dsd_net_audio_input_hook_tcp_read_sample(opts->tcp_in_ctx, (int16_t*)&s);
    s = symbol_scale_pcm_i16(s, opts->input_volume_multiplier);
    *sample_out = (float)s;
    if (tcp_result == 0) {
        int reconnected = 0;
    TCP_RETRY:
        if (exitflag == 1) {
            dsd_request_shutdown(opts, state);
            return 0;
        }
        int backoff_ms = 300;
        const dsdneoRuntimeConfig* cfg_retry = dsd_neo_get_config();
        if (!cfg_retry) {
            dsd_neo_config_init();
            cfg_retry = dsd_neo_get_config();
        }
        if (cfg_retry && cfg_retry->tcpin_backoff_ms_is_set) {
            backoff_ms = cfg_retry->tcpin_backoff_ms;
        }
        DSD_FPRINTF(stderr, "\nConnection to TCP Server Interrupted. Trying again in %d ms.\n", backoff_ms);
        dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
        opts->tcp_in_ctx = NULL;
        dsd_socket_close(opts->tcp_sockfd);
        dsd_sleep_ms((unsigned int)backoff_ms);

        opts->tcp_sockfd = 0;
        opts->tcp_sockfd = Connect(opts->tcp_hostname, opts->tcp_portno);
        if (opts->tcp_sockfd != 0) {
            opts->tcp_in_ctx = dsd_net_audio_input_hook_tcp_open(opts->tcp_sockfd, opts->wav_sample_rate);
            if (opts->tcp_in_ctx == NULL) {
                DSD_FPRINTF(stderr, "Error, couldn't Reconnect to TCP audio input\n");
            } else {
                reconnected = 1;
                LOG_INFO("TCP Socket Reconnected Successfully.\n");
            }
        } else {
            LOG_ERROR("TCP Socket Connection Error.\n");
            if (opts->frame_m17 == 1) {
                goto TCP_RETRY;
            }
        }

        if (dsd_pcm_input_take_staged_tail_sample(opts, sample_out, 1)) {
            return 1;
        }
        if (reconnected) {
            dsd_opts_reset_pcm_input_state(opts);
        }

        short s_retry = 0;
        tcp_result = dsd_net_audio_input_hook_tcp_read_sample(opts->tcp_in_ctx, (int16_t*)&s_retry);
        *sample_out = (float)s_retry;
        if (tcp_result == 0) {
            dsd_net_audio_input_hook_tcp_close(opts->tcp_in_ctx);
            opts->tcp_in_ctx = NULL;
            dsd_socket_close(opts->tcp_sockfd);
            opts->tcp_sockfd = 0;
            if (!symbol_open_pulse_input_and_reconfigure_output(opts, state)) {
                return 0;
            }
            *sample_out = 0;
            DSD_FPRINTF(stderr, "Connection to TCP Server Disconnected.\n");
        }
    }
    if (opts->audio_in_type == AUDIO_IN_TCP && dsd_pcm_input_uses_staged_resampler(opts)) {
        dsd_pcm_input_stage_resample(opts, *sample_out);
        *sample_out = opts->input_upsample_buf[opts->input_upsample_pos++];
    }
    return 1;
}

static inline int
symbol_read_sample_udp(dsd_opts* opts, dsd_state* state, float* sample_out) {
    short s = 0;
    if (!dsd_net_audio_input_hook_udp_read_sample(opts, (int16_t*)&s)) {
        if (dsd_pcm_input_take_staged_tail_sample(opts, sample_out, 1)) {
            return 1;
        }
        dsd_request_shutdown(opts, state);
        return 0;
    }
    *sample_out = (float)s;
    if (opts->input_volume_multiplier > 1) {
        int v = (int)s * opts->input_volume_multiplier;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        *sample_out = (float)v;
    }
    if (dsd_pcm_input_uses_staged_resampler(opts)) {
        dsd_pcm_input_stage_resample(opts, *sample_out);
        *sample_out = opts->input_upsample_buf[opts->input_upsample_pos++];
    }
    return 1;
}

static int
symbol_take_sample(dsd_opts* opts, dsd_state* state, symbol_work_ctx* work) {
    if (symbol_stop_after_shutdown(&work->sample)) {
        return 0;
    }
    if (dsd_pcm_input_uses_staged_resampler(opts) && opts->input_upsample_pos < opts->input_upsample_len) {
        work->sample = opts->input_upsample_buf[opts->input_upsample_pos++];
        return 1;
    }
    if (dsd_pcm_input_take_staged_tail_sample(opts, &work->sample, 0)) {
        return 1;
    }
    if (opts->audio_in_type == AUDIO_IN_PULSE) {
        return symbol_read_sample_pulse(opts, &work->sample);
    }
    if (opts->audio_in_type == AUDIO_IN_STDIN) {
        return symbol_read_sample_stdin(opts, state, &work->sample);
    }
    if (opts->audio_in_type == AUDIO_IN_WAV) {
        return symbol_read_sample_wav(opts, state, &work->sample);
    }
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        return symbol_read_sample_rtl(opts, state, &work->sample, work);
    }
#endif
    if (opts->audio_in_type == AUDIO_IN_TCP) {
        return symbol_read_sample_tcp(opts, state, &work->sample);
    }
    if (opts->audio_in_type == AUDIO_IN_UDP) {
        return symbol_read_sample_udp(opts, state, &work->sample);
    }
    return 1;
}

#ifdef USE_RADIO
static inline void
symbol_init_rtl_profile(const dsd_opts* opts, dsd_state* state, symbol_work_ctx* work) {
    (void)opts;
    if (opts->audio_in_type == AUDIO_IN_RTL) {
        (void)symbol_refresh_rtl_profile(state, work);
    }
}

static int
symbol_try_rtl_symbol_rate_fast_path(dsd_opts* opts, dsd_state* state, symbol_work_ctx* work) {
    if (!work->rtl_symbol_rate_output || opts->symboltiming == 1) {
        return 0;
    }
    if (!state->rtl_ctx) {
        dsd_request_shutdown(opts, state);
        return -1;
    }

    state->samplesPerSymbol = 1;
    state->symbolCenter = 0;
    state->jitter = -1;

    for (;;) {
        apply_rtl_symbol_thresholds(state, work->rtl_symbol_levels);
        int cache_status =
            rtl_symbol_cache_take(state, work->rtl_output_kind, work->rtl_channel_profile, work->rtl_symbol_rate_hz,
                                  work->rtl_symbol_levels, &work->rtl_stream_generation, &work->sample);
        if (cache_status == RTL_SYMBOL_CACHE_READY) {
            break;
        }
        if (cache_status != RTL_SYMBOL_CACHE_RETRY) {
            dsd_request_shutdown(opts, state);
            return -1;
        }

        (void)symbol_refresh_rtl_profile(state, work);
        if (!work->rtl_symbol_rate_output) {
            break;
        }
    }

    if (!work->rtl_symbol_rate_output) {
        return 0;
    }
    opts->rtl_pwr = dsd_rtl_stream_io_hook_return_pwr(state);
    state->lastsample = work->sample;
    dsd_symbol_history_push(state, work->sample);
    state->symbolcnt++;
    symbol_maybe_publish_rtl_input_level(opts, state);
    return 1;
}

static inline void
symbol_prepare_span(const dsd_opts* opts, dsd_state* state, symbol_work_ctx* work, int have_sync) {
    if (work->rtl_fsk_discriminator_output) {
        symbol_apply_rtl_fsk_discriminator_timing(opts, state, work);
    } else if (!work->rtl_symbol_rate_output) {
        maybe_auto_center(opts, state, have_sync);
        maybe_adjust_sps_for_output_rate(opts, state);
    } else {
        state->samplesPerSymbol = 1;
        state->symbolCenter = 0;
        state->jitter = -1;
        apply_rtl_symbol_thresholds(state, work->rtl_symbol_levels);
    }

    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int freeze_window = (cfg && cfg->window_freeze_is_set) ? (cfg->window_freeze != 0) : 0;
    if (state->rf_mod == 0) {
        select_window_c4fm(state, &work->l_edge_pre, &work->r_edge_pre, freeze_window);
    } else if (state->rf_mod == 1) {
        select_window_qpsk(&work->l_edge_pre, &work->r_edge_pre, freeze_window);
    } else {
        select_window_gfsk(&work->l_edge_pre, &work->r_edge_pre, freeze_window);
    }

    work->symbol_span = state->samplesPerSymbol;
    if (work->symbol_span < 1) {
        work->symbol_span = 1;
    }
    if (work->rtl_symbol_rate_output) {
        work->symbol_span = 1;
    }
    if (!work->rtl_direct_output && opts->audio_in_type == AUDIO_IN_RTL && state->rf_mod == 1) {
        int dsp_cqpsk = 0;
        int dsp_timing = 0;
        dsd_rtl_stream_metrics_hook_cqpsk_status(&dsp_cqpsk, &dsp_timing);
        if (dsp_cqpsk && dsp_timing) {
            work->cqpsk_symbol_rate = 1;
            work->symbol_span = 1;
        }
    }
    if (work->symbol_span <= 1) {
        state->jitter = -1;
    }
}
#endif

static inline void
symbol_print_timing_line(const dsd_opts* opts, dsd_state* state, int have_sync) {
    if (!symbol_timing_debug_enabled(opts, state, have_sync)) {
        return;
    }
    if (state->jitter >= 0) {
        DSD_FPRINTF(stderr, " %i\n", state->jitter);
    } else {
        DSD_FPRINTF(stderr, "\n");
    }
}

static inline int
symbol_process_symbol_bin_input(dsd_opts* opts, dsd_state* state, float* symbol_out) {
    if (opts->symbolfile == NULL) {
        DSD_FPRINTF(stderr, "Error Opening File %s\n", opts->audio_in_dev);
        *symbol_out = -1.0f;
        return 1;
    }

    int replay_retry_count = 0;
    for (;;) {
        if (probe_symbol_replay_format(opts, state) != 0) {
            DSD_FPRINTF(stderr, "Unsupported symbol capture header in %s\n", opts->audio_in_dev);
            fclose(opts->symbolfile);
            opts->symbolfile = NULL;
            dsd_request_shutdown(opts, state);
            *symbol_out = 0.0f;
            return 1;
        }

        int read_ok = 0;
        if (state->symbol_replay_format == DSD_SYMBOL_REPLAY_FORMAT_SOFT) {
            read_ok = read_soft_symbol_record(opts, state, symbol_out);
        } else {
            int c = fgetc(opts->symbolfile);
            if (c != EOF) {
                state->symbolc = c & 3;
                state->symbol_replay_has_soft = 0;
                *symbol_out = dsd_symbol_level_from_dibit((uint8_t)state->symbolc);
                read_ok = 1;
            }
        }
        if (read_ok) {
            return 1;
        }

        fclose(opts->symbolfile);
        opts->symbolfile = NULL;
        DSD_FPRINTF(stderr, "\nEnd of %s\n", opts->audio_in_dev);
        if (state->debug_mode == 1) {
            opts->symbolfile = dsd_fopen_existing_regular_file(opts->audio_in_dev, "rb");
            opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
            state->symbol_replay_format = DSD_SYMBOL_REPLAY_FORMAT_UNKNOWN;
            state->symbol_replay_header_checked = 0;
            state->symbol_replay_has_soft = 0;
            if (opts->symbolfile == NULL) {
                DSD_FPRINTF(stderr, "Error Opening File %s\n", opts->audio_in_dev);
                *symbol_out = -1.0f;
                return 1;
            }
            if (replay_retry_count++ == 0) {
                continue;
            }
            *symbol_out = 0.0f;
            return 1;
        }
        if (opts->audio_out_type == 0 && dsd_opts_frontend_active(opts)) {
            (void)symbol_open_pulse_input_and_reconfigure_output(opts, state);
            *symbol_out = 0.0f;
            return 1;
        }
        dsd_request_shutdown(opts, state);
        *symbol_out = 0.0f;
        return 1;
    }
}

static inline int
symbol_process_symbol_flt_input(dsd_opts* opts, float* symbol_out) {
    float float_symbol = 0.0f;
    size_t read_count = fread(&float_symbol, sizeof(float), 1, opts->symbolfile);
    if (read_count != 1) {
        exitflag = 1;
        *symbol_out = 0.0f;
        return 1;
    }
    if (feof(opts->symbolfile)) {
        exitflag = 1;
    }
    *symbol_out = float_symbol * 10000.0f;
    return 1;
}

static int
symbol_process_live_samples(dsd_opts* opts, dsd_state* state, int have_sync, symbol_work_ctx* work) {
    for (int i = 0; i < work->symbol_span; i++) {
        symbol_adjust_timing_index(state, have_sync, work->symbol_span, &i);
        if (!symbol_take_sample(opts, state, work)) {
            return 0;
        }

        symbol_process_analog_capture(opts, state, work, have_sync);
#ifdef USE_RADIO
        int rtl_symbol_rate_output = work->rtl_symbol_rate_output;
        int cqpsk_symbol_rate = work->cqpsk_symbol_rate;
#else
        int rtl_symbol_rate_output = 0;
        int cqpsk_symbol_rate = 0;
#endif
        work->sample =
            symbol_apply_matched_filter(opts, state, work->sample, rtl_symbol_rate_output, cqpsk_symbol_rate);
        work->sample = symbol_apply_sync_clip(state, have_sync, work->sample);
        symbol_update_jitter(opts, state, have_sync, i, work->sample);
        symbol_accumulate_sample(state, work, i, work->sample);
        state->lastsample = work->sample;
    }
    return 1;
}

static inline float
symbol_finalize_live_symbol(const dsd_opts* opts, dsd_state* state, int have_sync, const symbol_work_ctx* work) {
    float symbol = (work->count > 0) ? (work->sum / (float)work->count) : 0.0f;
#ifdef USE_RADIO
    if (work->rtl_symbol_rate_output) {
        apply_rtl_symbol_thresholds(state, work->rtl_symbol_levels);
    }
#endif
    symbol_print_timing_line(opts, state, have_sync);
    return symbol;
}

#ifndef USE_RADIO
static inline void
symbol_prepare_span_no_radio(dsd_state* state, symbol_work_ctx* work) {
    const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
    int freeze_window = (cfg && cfg->window_freeze_is_set) ? (cfg->window_freeze != 0) : 0;
    if (state->rf_mod == 0) {
        select_window_c4fm(state, &work->l_edge_pre, &work->r_edge_pre, freeze_window);
    } else if (state->rf_mod == 1) {
        select_window_qpsk(&work->l_edge_pre, &work->r_edge_pre, freeze_window);
    } else {
        select_window_gfsk(&work->l_edge_pre, &work->r_edge_pre, freeze_window);
    }
    work->symbol_span = state->samplesPerSymbol;
    if (work->symbol_span < 1) {
        work->symbol_span = 1;
    }
    if (work->symbol_span <= 1) {
        state->jitter = -1;
    }
}
#endif

static inline void
symbol_apply_replay_overrides(dsd_opts* opts, dsd_state* state, float* symbol) {
    if (opts->audio_in_type == AUDIO_IN_SYMBOL_BIN) {
        (void)symbol_process_symbol_bin_input(opts, state, symbol);
    }
    if (opts->audio_in_type == AUDIO_IN_SYMBOL_FLT) {
        (void)symbol_process_symbol_flt_input(opts, symbol);
    }
}

static inline float
symbol_commit_symbol(dsd_opts* opts, dsd_state* state, int have_sync, const symbol_work_ctx* work, float symbol) {
    (void)have_sync;
    (void)work;
#ifndef USE_RADIO
    (void)opts;
#endif
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
#ifdef USE_RADIO
    symbol_maybe_publish_rtl_input_level(opts, state);
#endif
    return symbol;
}

float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) {
    symbol_work_ctx work;
    symbol_work_ctx_init(&work, state);

#ifdef USE_RADIO
    symbol_init_rtl_profile(opts, state, &work);
    int fast_status = symbol_try_rtl_symbol_rate_fast_path(opts, state, &work);
    if (fast_status < 0) {
        return 0.0f;
    }
    if (fast_status > 0) {
        return work.sample;
    }
    symbol_prepare_span(opts, state, &work, have_sync);
#else
    symbol_prepare_span_no_radio(state, &work);
#endif

    if (!symbol_process_live_samples(opts, state, have_sync, &work)) {
        return 0.0f;
    }

    float symbol = symbol_finalize_live_symbol(opts, state, have_sync, &work);

    symbol_apply_replay_overrides(opts, state, &symbol);
    return symbol_commit_symbol(opts, state, have_sync, &work, symbol);
}
