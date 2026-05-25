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

#include <dsd-neo/core/cleanup.h>
#include <dsd-neo/core/constants.h>
#include <dsd-neo/core/dibit.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/frame.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/dsp/dmr_sync.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/symbol.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/dsp/sync_hamming.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/runtime/colors.h>
#include <dsd-neo/runtime/comp.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/telemetry.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/timing.h"

#ifdef USE_RADIO
#include <dsd-neo/core/power.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#endif

static int
frame_sync_opts_has_4800_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_dmr == 1 || opts->frame_nxdn96 == 1 || opts->frame_ysf == 1
            || opts->frame_m17 == 1);
}

#ifdef USE_RADIO
static int
dmr_best_sync_hamming(const char* window, const char** out_name) {
    static const char* names[] = {"bs_data",     "bs_voice",    "ms_data",      "ms_voice",
                                  "dm_ts1_data", "dm_ts2_data", "dm_ts1_voice", "dm_ts2_voice"};
    static const char* patterns[] = {DMR_BS_DATA_SYNC,
                                     DMR_BS_VOICE_SYNC,
                                     DMR_MS_DATA_SYNC,
                                     DMR_MS_VOICE_SYNC,
                                     DMR_DIRECT_MODE_TS1_DATA_SYNC,
                                     DMR_DIRECT_MODE_TS2_DATA_SYNC,
                                     DMR_DIRECT_MODE_TS1_VOICE_SYNC,
                                     DMR_DIRECT_MODE_TS2_VOICE_SYNC};
    int best = 24;
    int best_idx = 0;

    if (!window) {
        if (out_name) {
            *out_name = "none";
        }
        return best;
    }

    for (int p = 0; p < 8; p++) {
        int ham = 0;
        for (int k = 0; k < 24; k++) {
            if (window[k] != patterns[p][k]) {
                ham++;
            }
        }
        if (ham < best) {
            best = ham;
            best_idx = p;
        }
    }
    if (out_name) {
        *out_name = names[best_idx];
    }
    return best;
}

static int
rtl_opts_has_any_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1 || opts->frame_dmr == 1 || opts->frame_nxdn48 == 1
            || opts->frame_nxdn96 == 1 || opts->frame_x2tdma == 1 || opts->frame_ysf == 1 || opts->frame_dpmr == 1
            || opts->frame_m17 == 1);
}

static int
rtl_opts_has_4800_wide_four_level_mode(const dsd_opts* opts) {
    if (!opts) {
        return 0;
    }
    return (opts->frame_dmr == 1 || opts->frame_nxdn96 == 1 || opts->frame_ysf == 1 || opts->frame_m17 == 1);
}

static int
rtl_p25_profile_for_state(const dsd_state* state) {
    return (state && state->rf_mod == 1) ? DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK
                                         : DSD_RTL_STREAM_CHANNEL_PROFILE_P25_C4FM;
}

static int
rtl_fallback_profile_for_symbol_rate(int sym_rate_hz) {
    if (sym_rate_hz == 2400) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    if (sym_rate_hz == 9600) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    }
    return DSD_RTL_STREAM_CHANNEL_PROFILE_WIDE;
}

static int
rtl_profile_for_explicit_symbol_rate(const dsd_opts* opts, int sym_rate_hz, int preferred_levels) {
    if (sym_rate_hz == 4800 && preferred_levels == 2 && opts->frame_dstar == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    if (sym_rate_hz == 9600 && opts->frame_provoice == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    }
    if (sym_rate_hz == 2400 && (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1)) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    if (sym_rate_hz == 6000 && opts->frame_p25p2 == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_P25_CQPSK;
    }
    if (sym_rate_hz == 6000 && opts->frame_x2tdma == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    return -1;
}

static int
rtl_profile_for_enabled_protocols(const dsd_opts* opts, const dsd_state* state) {
    if (rtl_opts_has_4800_wide_four_level_mode(opts)) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    if (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1) {
        return rtl_p25_profile_for_state(state);
    }
    if (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1 || opts->frame_dstar == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_6K25;
    }
    if (opts->frame_x2tdma == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_12K5;
    }
    if (opts->frame_provoice == 1) {
        return DSD_RTL_STREAM_CHANNEL_PROFILE_PROVOICE;
    }
    return -1;
}

static int
rtl_profile_for_symbol_rate(const dsd_opts* opts, const dsd_state* state, int sym_rate_hz, int preferred_levels) {
    if (opts) {
        int profile = rtl_profile_for_explicit_symbol_rate(opts, sym_rate_hz, preferred_levels);
        if (profile >= 0) {
            return profile;
        }
        profile = rtl_profile_for_enabled_protocols(opts, state);
        if (profile >= 0) {
            return profile;
        }
    }
    return rtl_fallback_profile_for_symbol_rate(sym_rate_hz);
}

static int
rtl_levels_for_symbol_rate(const dsd_opts* opts, int sym_rate_hz, int preferred_levels) {
    if (preferred_levels == 2 || preferred_levels == 4) {
        return preferred_levels;
    }
    if (!opts) {
        return 4;
    }
    if (sym_rate_hz == 9600 && opts->frame_provoice == 1) {
        return 2;
    }
    if (sym_rate_hz == 4800 && opts->frame_dstar == 1 && !frame_sync_opts_has_4800_four_level_mode(opts)) {
        return 2;
    }
    if ((opts->frame_dstar == 1 || opts->frame_provoice == 1) && !rtl_opts_has_any_four_level_mode(opts)) {
        return 2;
    }
    return 4;
}

static void
rtl_maybe_update_symbol_profile_with_hint(const dsd_opts* opts, const dsd_state* state, int sym_rate_hz,
                                          int preferred_levels) {
    if (!opts || !state || opts->audio_in_type != AUDIO_IN_RTL || !state->rtl_ctx || sym_rate_hz <= 0) {
        return;
    }
    (void)dsd_rtl_stream_metrics_hook_set_symbol_profile(
        sym_rate_hz, rtl_levels_for_symbol_rate(opts, sym_rate_hz, preferred_levels),
        rtl_profile_for_symbol_rate(opts, state, sym_rate_hz, preferred_levels));
}

static void
rtl_maybe_update_symbol_profile(const dsd_opts* opts, const dsd_state* state, int sym_rate_hz) {
    rtl_maybe_update_symbol_profile_with_hint(opts, state, sym_rate_hz, 0);
}
#endif

static inline void
dmr_set_symbol_timing(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }

    int demod_rate = 0;
#ifdef USE_RADIO
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        demod_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
    }
#endif

    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, 4800, demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
#ifdef USE_RADIO
    rtl_maybe_update_symbol_profile(opts, state, 4800);
#endif
}

/* Modulation auto-detect state (file scope for reset access).
 * Vote counters and Hamming distance tracking for C4FM/QPSK/GFSK switching.
 * These are atomic because trunk_tune_to_freq() resets them from the tuning
 * thread while getFrameSync() reads/writes them on the DSP thread. */
static atomic_int g_vote_qpsk = 0;
static atomic_int g_vote_c4fm = 0;
static atomic_int g_vote_gfsk = 0;
static atomic_int g_ham_c4fm_recent = 24;
static atomic_int g_ham_qpsk_recent = 24;
static atomic_int g_ham_gfsk_recent = 24;
static atomic_int g_qpsk_dwell_enter_ms = 0;
static dsd_atomic_u64 g_frame_sync_ui_last_publish_ms = {0};

enum { DSD_FRAME_SYNC_UI_PUBLISH_INTERVAL_MS = 50 };

static void
frame_sync_publish_ui_throttled(const dsd_opts* opts, const dsd_state* state) {
    if (!opts || opts->use_ncurses_terminal != 1) {
        return;
    }

    const uint64_t now_ms = dsd_time_monotonic_ms();
    const uint64_t last_ms = dsd_atomic_u64_load_relaxed(&g_frame_sync_ui_last_publish_ms);
    if (last_ms != 0 && (now_ms - last_ms) < DSD_FRAME_SYNC_UI_PUBLISH_INTERVAL_MS) {
        return;
    }

    dsd_atomic_u64_store_relaxed(&g_frame_sync_ui_last_publish_ms, now_ms);
    ui_publish_both_and_redraw(opts, state);
}

static void
p25p2_note_sync_activity(const dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    const int voice_tuned =
        (opts && opts->p25_trunk == 1 && (opts->p25_is_tuned == 1 || opts->trunk_is_tuned == 1)) ? 1 : 0;

    /*
     * Exact P25P2 sync means the channel is present, but while following a VC it
     * does not necessarily mean voice is still active. TDMA VCs can keep sending
     * LCCH/idle after a call ends; refreshing last_vc_sync_time here holds the
     * trunk release path open and delays return to the CC. Voice/MAC handlers
     * update last_vc_sync_time when the call is actually active.
     */
    if (voice_tuned) {
        return;
    }

    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();
    state->last_cc_sync_time = now;
    state->last_cc_sync_time_m = nowm;
}

void
dsd_frame_sync_reset_mod_state(void) {
    atomic_store(&g_vote_qpsk, 0);
    atomic_store(&g_vote_c4fm, 0);
    atomic_store(&g_vote_gfsk, 0);
    atomic_store(&g_ham_c4fm_recent, 24);
    atomic_store(&g_ham_qpsk_recent, 24);
    atomic_store(&g_ham_gfsk_recent, 24);
    atomic_store(&g_qpsk_dwell_enter_ms, 0);
}

/*
 * P25 CQPSK handling - matches OP25 exactly.
 *
 * OP25 does NOT use constellation permutation tables. It only uses:
 * 1. Normal sync detection (P25_FRAME_SYNC_MAGIC)
 * 2. Polarity reversal detection (reverse_p ^= 0x02)
 * 3. Tuning error detection (log only, no dibit remapping)
 *
 * The Costas loop handles legitimate 90° phase ambiguity via PT_45 rotation.
 * Tuning errors (±1200Hz, ±2400Hz) cannot be fixed by dibit remapping - they
 * require RF correction.
 */

void
printFrameSync(const dsd_opts* opts, const dsd_state* state, const char* frametype, int offset,
               const char* modulation) {
    UNUSED3(state, offset, modulation);

    char timestr[9];
    getTimeC_buf(timestr);
    if (opts->verbose > 0) {
        DSD_FPRINTF(stderr, "%s ", timestr);
        DSD_FPRINTF(stderr, "Sync: %s ", frametype);
    }

    //oops, that made a nested if-if-if-if statement,
    //causing a memory leak

    // if (opts->verbose > 2)
    //DSD_FPRINTF(stderr,"o: %4i ", offset);
    // if (opts->verbose > 1)
    //DSD_FPRINTF(stderr,"mod: %s ", modulation);
    // if (opts->verbose > 2)
    //DSD_FPRINTF(stderr,"g: %f ", state->aout_gain);

    /* stack buffer; no free */
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    time_t now;
    double nowm;
    int synctest_pos;
    float lmax;
    float lmin;
    char* modulation;
    char* synctest_p;
    char* synctest;
    char* synctest8;
    char* synctest10;
    char* synctest12;
    char* synctest16;
    char* synctest20;
    char* synctest32;
    char* synctest48;
} frame_sync_match_ctx;

static inline void
frame_sync_set_basic_lock(frame_sync_match_ctx* ctx) {
    dsd_state* state = ctx->state;
    state->carrier = 1;
    state->offset = ctx->synctest_pos;
    state->max = ((state->max) + ctx->lmax) / 2;
    state->min = ((state->min) + ctx->lmin) / 2;
}

static inline void
frame_sync_note_cc_sync(frame_sync_match_ctx* ctx) {
    ctx->state->last_cc_sync_time = ctx->now;
    ctx->state->last_cc_sync_time_m = ctx->nowm;
}

static inline void
frame_sync_maybe_force_dmr_gfsk(const dsd_opts* opts, dsd_state* state) {
    if (!opts->mod_cli_lock || opts->mod_gfsk) {
        state->rf_mod = 2;
    }
}

static int
frame_sync_try_p25p1(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_p25p1 != 1) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest, P25P1_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        state->dmrburstR = 17;
        state->payload_algidR = 0;
        state->dmr_stereo = 1;
        DSD_SPRINTF(state->ftype, "P25 Phase 1");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+P25p1", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_P25P1_POS;
        frame_sync_note_cc_sync(ctx);
        if (state->rf_mod == 0) {
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        } else if (state->rf_mod == 1) {
            dsd_sync_warm_start_center_outer_only(opts, state, 24);
        }
        return DSD_SYNC_P25P1_POS;
    }

    if (strcmp(ctx->synctest, INV_P25P1_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        state->dmrburstR = 17;
        state->payload_algidR = 0;
        state->dmr_stereo = 1;
        DSD_SPRINTF(state->ftype, "P25 Phase 1");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-P25p1 ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_P25P1_NEG;
        frame_sync_note_cc_sync(ctx);
        if (state->rf_mod == 0) {
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        } else if (state->rf_mod == 1) {
            dsd_sync_warm_start_center_outer_only(opts, state, 24);
        }
        return DSD_SYNC_P25P1_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_x2tdma(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_x2tdma != 1) {
        return DSD_SYNC_NONE;
    }

    if ((strcmp(ctx->synctest, X2TDMA_BS_DATA_SYNC) == 0) || (strcmp(ctx->synctest, X2TDMA_MS_DATA_SYNC) == 0)) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "X2-TDMA");
        if (opts->inverted_x2tdma == 0) {
            if (opts->errorbars == 1) {
                printFrameSync(opts, state, "+X2-TDMA ", ctx->synctest_pos + 1, ctx->modulation);
            }
            state->lastsynctype = DSD_SYNC_X2TDMA_DATA_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
            return DSD_SYNC_X2TDMA_DATA_POS;
        }
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-X2-TDMA ", ctx->synctest_pos + 1, ctx->modulation);
        }
        if (state->lastsynctype != DSD_SYNC_X2TDMA_VOICE_NEG) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_X2TDMA_VOICE_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_X2TDMA_VOICE_NEG;
    }

    if ((strcmp(ctx->synctest, X2TDMA_BS_VOICE_SYNC) == 0) || (strcmp(ctx->synctest, X2TDMA_MS_VOICE_SYNC) == 0)) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "X2-TDMA");
        if (opts->inverted_x2tdma == 0) {
            if (opts->errorbars == 1) {
                printFrameSync(opts, state, "+X2-TDMA ", ctx->synctest_pos + 1, ctx->modulation);
            }
            if (state->lastsynctype != DSD_SYNC_X2TDMA_VOICE_POS) {
                state->firstframe = 1;
            }
            state->lastsynctype = DSD_SYNC_X2TDMA_VOICE_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
            return DSD_SYNC_X2TDMA_VOICE_POS;
        }
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-X2-TDMA ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_X2TDMA_DATA_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_X2TDMA_DATA_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_ysf(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    dsd_strncpy_s(ctx->synctest20, 21, (ctx->synctest_p - 19), 20);
    if (opts->frame_ysf != 1 || dsd_frame_sync_suppress_p25_alt_sync(opts, state)) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest20, FUSION_SYNC) == 0) {
        printFrameSync(opts, state, "+YSF ", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        opts->inverted_ysf = 0;
        state->lastsynctype = DSD_SYNC_YSF_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 20);
        return DSD_SYNC_YSF_POS;
    }

    if (strcmp(ctx->synctest20, INV_FUSION_SYNC) == 0) {
        printFrameSync(opts, state, "-YSF ", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        opts->inverted_ysf = 1;
        state->lastsynctype = DSD_SYNC_YSF_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 20);
        return DSD_SYNC_YSF_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_p25p2(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    dsd_strncpy_s(ctx->synctest20, 21, (ctx->synctest_p - 19), 20);
    if (opts->frame_p25p2 != 1) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest20, P25P2_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        opts->inverted_p2 = 0;
        state->lastsynctype = DSD_SYNC_P25P2_POS;
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+P25p2", ctx->synctest_pos + 1, ctx->modulation);
        }
        if (state->p2_wacn != 0 && state->p2_cc != 0 && state->p2_sysid != 0) {
            printFrameInfo(opts, state);
        } else {
            DSD_FPRINTF(stderr, "%s", KRED);
            DSD_FPRINTF(stderr, " P2 Missing Parameters            ");
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
        p25p2_note_sync_activity(opts, state);
        if (state->rf_mod == 1) {
            dsd_sync_warm_start_center_outer_only(opts, state, 20);
        }
        return DSD_SYNC_P25P2_POS;
    }

    if (strcmp(ctx->synctest20, INV_P25P2_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        opts->inverted_p2 = 1;
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-P25p2", ctx->synctest_pos + 1, ctx->modulation);
        }
        if (state->p2_wacn != 0 && state->p2_cc != 0 && state->p2_sysid != 0) {
            printFrameInfo(opts, state);
        } else {
            DSD_FPRINTF(stderr, "%s", KRED);
            DSD_FPRINTF(stderr, " P2 Missing Parameters            ");
            DSD_FPRINTF(stderr, "%s", KNRM);
        }
        state->lastsynctype = DSD_SYNC_P25P2_NEG;
        p25p2_note_sync_activity(opts, state);
        if (state->rf_mod == 1) {
            dsd_sync_warm_start_center_outer_only(opts, state, 20);
        }
        return DSD_SYNC_P25P2_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_dpmr(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    dsd_strncpy_s(ctx->synctest, 25, (ctx->synctest_p - 23), 24);
    dsd_strncpy_s(ctx->synctest12, 13, (ctx->synctest_p - 11), 12);
    if (opts->frame_dpmr != 1) {
        return DSD_SYNC_NONE;
    }

    if (opts->inverted_dpmr == 0 && strcmp(ctx->synctest12, DPMR_FRAME_SYNC_2) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "dPMR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+dPMR ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DPMR_FS2_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 12);
        return DSD_SYNC_DPMR_FS2_POS;
    }

    if (opts->inverted_dpmr == 1 && strcmp(ctx->synctest12, INV_DPMR_FRAME_SYNC_2) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "dPMR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-dPMR ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DPMR_FS2_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 12);
        return DSD_SYNC_DPMR_FS2_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17_preamble(frame_sync_match_ctx* ctx, int ham_pre, int ham_piv) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;

    if (ham_pre <= 1) {
        state->m17_polarity = 1;
        printFrameSync(opts, state, "+M17 PREAMBLE", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        state->lastsynctype = DSD_SYNC_M17_PRE_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        DSD_FPRINTF(stderr, "\n");
        return DSD_SYNC_M17_PRE_POS;
    }

    if (ham_piv <= 1) {
        state->m17_polarity = 2;
        printFrameSync(opts, state, "-M17 PREAMBLE", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        state->lastsynctype = DSD_SYNC_M17_PRE_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        DSD_FPRINTF(stderr, "\n");
        return DSD_SYNC_M17_PRE_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17_pkt(frame_sync_match_ctx* ctx, int ham_pkt, int ham_brt, int is_inverted) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;

    if (ham_pkt <= 1 && !is_inverted) {
        printFrameSync(opts, state, "+M17 PKT", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        if (state->lastsynctype == DSD_SYNC_M17_PKT_POS || state->lastsynctype == DSD_SYNC_M17_STR_POS) {
            state->lastsynctype = DSD_SYNC_M17_PKT_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
            return DSD_SYNC_M17_PKT_POS;
        }
        state->lastsynctype = DSD_SYNC_M17_PKT_POS;
        DSD_FPRINTF(stderr, "\n");
        return DSD_SYNC_NONE;
    }

    if (ham_brt <= 1 && is_inverted) {
        printFrameSync(opts, state, "-M17 PKT", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        if (state->lastsynctype == DSD_SYNC_M17_PKT_NEG || state->lastsynctype == DSD_SYNC_M17_STR_NEG) {
            state->lastsynctype = DSD_SYNC_M17_PKT_NEG;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
            return DSD_SYNC_M17_PKT_NEG;
        }
        state->lastsynctype = DSD_SYNC_M17_PKT_NEG;
        DSD_FPRINTF(stderr, "\n");
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17_str(frame_sync_match_ctx* ctx, int ham_str, int is_inverted) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (ham_str > 1) {
        return DSD_SYNC_NONE;
    }

    if (!is_inverted) {
        printFrameSync(opts, state, "+M17 STR", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        if (state->lastsynctype == DSD_SYNC_M17_LSF_POS || state->lastsynctype == DSD_SYNC_M17_STR_POS) {
            state->lastsynctype = DSD_SYNC_M17_STR_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
            return DSD_SYNC_M17_STR_POS;
        }
        state->lastsynctype = DSD_SYNC_M17_STR_POS;
        DSD_FPRINTF(stderr, "\n");
        return DSD_SYNC_NONE;
    }

    printFrameSync(opts, state, "-M17 LSF", ctx->synctest_pos + 1, ctx->modulation);
    frame_sync_set_basic_lock(ctx);
    if (state->lastsynctype == DSD_SYNC_M17_PRE_NEG) {
        state->lastsynctype = DSD_SYNC_M17_LSF_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        return DSD_SYNC_M17_LSF_NEG;
    }
    state->lastsynctype = DSD_SYNC_M17_LSF_NEG;
    DSD_FPRINTF(stderr, "\n");
    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17_lsf(frame_sync_match_ctx* ctx, int ham_lsf, int is_inverted) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (ham_lsf > 1) {
        return DSD_SYNC_NONE;
    }

    if (!is_inverted) {
        printFrameSync(opts, state, "+M17 LSF", ctx->synctest_pos + 1, ctx->modulation);
        frame_sync_set_basic_lock(ctx);
        if (state->lastsynctype == DSD_SYNC_M17_PRE_POS) {
            state->lastsynctype = DSD_SYNC_M17_LSF_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
            return DSD_SYNC_M17_LSF_POS;
        }
        state->lastsynctype = DSD_SYNC_M17_LSF_POS;
        DSD_FPRINTF(stderr, "\n");
        return DSD_SYNC_NONE;
    }

    printFrameSync(opts, state, "-M17 STR", ctx->synctest_pos + 1, ctx->modulation);
    frame_sync_set_basic_lock(ctx);
    if (state->lastsynctype == DSD_SYNC_M17_LSF_NEG || state->lastsynctype == DSD_SYNC_M17_STR_NEG) {
        state->lastsynctype = DSD_SYNC_M17_STR_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 8);
        return DSD_SYNC_M17_STR_NEG;
    }
    state->lastsynctype = DSD_SYNC_M17_STR_NEG;
    DSD_FPRINTF(stderr, "\n");
    return DSD_SYNC_NONE;
}

static int
frame_sync_try_m17(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    const dsd_state* state = ctx->state;
    if (opts->frame_m17 != 1) {
        return DSD_SYNC_NONE;
    }

    dsd_strncpy_s(ctx->synctest16, 17, (ctx->synctest_p - 15), 16);
    dsd_strncpy_s(ctx->synctest8, 9, (ctx->synctest_p - 7), 8);
    int ham_pre = dsd_sync_hamming_distance(ctx->synctest8, M17_PRE, 8);
    int ham_piv = dsd_sync_hamming_distance(ctx->synctest8, M17_PIV, 8);
    int ham_lsf = dsd_sync_hamming_distance(ctx->synctest8, M17_LSF, 8);
    int ham_str = dsd_sync_hamming_distance(ctx->synctest8, M17_STR, 8);
    int ham_pkt = dsd_sync_hamming_distance(ctx->synctest8, M17_PKT, 8);
    int ham_brt = dsd_sync_hamming_distance(ctx->synctest8, M17_BRT, 8);
    int is_inverted = opts->inverted_m17;
    if (!opts->inverted_m17 && state->m17_polarity == 2) {
        is_inverted = 1;
    }

    int sync_type = frame_sync_try_m17_preamble(ctx, ham_pre, ham_piv);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_m17_pkt(ctx, ham_pkt, ham_brt, is_inverted);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_m17_str(ctx, ham_str, is_inverted);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    return frame_sync_try_m17_lsf(ctx, ham_lsf, is_inverted);
}

static inline void
frame_sync_prepare_dmr_sync(frame_sync_match_ctx* ctx) {
    frame_sync_set_basic_lock(ctx);
    dmr_set_symbol_timing(ctx->opts, ctx->state);
    frame_sync_maybe_force_dmr_gfsk(ctx->opts, ctx->state);
}

static int
frame_sync_try_dmr_ms_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_MS_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    DSD_SPRINTF(state->ftype, "DMR MS");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_DATA;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_VOICE;
}

static int
frame_sync_try_dmr_ms_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_MS_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    DSD_SPRINTF(state->ftype, "DMR MS");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_VOICE;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_DATA;
}

static int
frame_sync_try_dmr_bs_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_BS_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 0;
    DSD_SPRINTF(state->ftype, "DMR ");
    if (opts->inverted_dmr == 0) {
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+DMR ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DMR_BS_DATA_POS;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_BS_DATA_POS;
    }

    if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_NEG) {
        state->firstframe = 1;
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_NEG;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_BS_VOICE_NEG;
}

static int
frame_sync_try_dmr_dm_ts1_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS1_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SPRINTF(state->ftype, "DMR ");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_DATA;
    }

    if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_NEG) {
        state->firstframe = 1;
    }
    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_VOICE;
}

static int
frame_sync_try_dmr_dm_ts2_data(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS2_DATA_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SPRINTF(state->ftype, "DMR ");
    if (opts->inverted_dmr == 0) {
        state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_DATA;
    }

    if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_NEG) {
        state->firstframe = 1;
    }
    state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_VOICE;
}

static int
frame_sync_try_dmr_bs_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_BS_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 0;
    DSD_SPRINTF(state->ftype, "DMR ");
    if (opts->inverted_dmr == 0) {
        if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_POS) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_DMR_BS_VOICE_POS;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_BS_VOICE_POS;
    }

    if (opts->errorbars == 1) {
        printFrameSync(opts, state, "-DMR ", ctx->synctest_pos + 1, ctx->modulation);
    }
    state->lastsynctype = DSD_SYNC_DMR_BS_DATA_NEG;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_BS_DATA_NEG;
}

static int
frame_sync_try_dmr_dm_ts1_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS1_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SPRINTF(state->ftype, "DMR ");
    if (opts->inverted_dmr == 0) {
        if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_POS) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_VOICE;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_DATA;
}

static int
frame_sync_try_dmr_dm_ts2_voice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (strcmp(ctx->synctest, DMR_DIRECT_MODE_TS2_VOICE_SYNC) != 0) {
        return DSD_SYNC_NONE;
    }

    frame_sync_prepare_dmr_sync(ctx);
    state->directmode = 1;
    DSD_SPRINTF(state->ftype, "DMR ");
    if (opts->inverted_dmr == 0) {
        if (state->lastsynctype != DSD_SYNC_DMR_BS_VOICE_POS) {
            state->firstframe = 1;
        }
        state->lastsynctype = DSD_SYNC_DMR_MS_VOICE;
        dsd_mark_cc_sync(state);
        dmr_resample_on_sync(opts, state);
        return DSD_SYNC_DMR_MS_VOICE;
    }

    state->lastsynctype = DSD_SYNC_DMR_MS_DATA;
    dsd_mark_cc_sync(state);
    dmr_resample_on_sync(opts, state);
    return DSD_SYNC_DMR_MS_DATA;
}

static int
frame_sync_try_dmr(frame_sync_match_ctx* ctx) {
    if (ctx->opts->frame_dmr != 1) {
        return DSD_SYNC_NONE;
    }

    int sync_type = frame_sync_try_dmr_ms_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_ms_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_bs_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts1_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts2_data(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_bs_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    sync_type = frame_sync_try_dmr_dm_ts1_voice(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }
    return frame_sync_try_dmr_dm_ts2_voice(ctx);
}

static int
frame_sync_try_provoice(frame_sync_match_ctx* ctx) {
    dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_provoice != 1) {
        return DSD_SYNC_NONE;
    }

    dsd_strncpy_s(ctx->synctest32, 33, (ctx->synctest_p - 31), 32);
    dsd_strncpy_s(ctx->synctest48, 49, (ctx->synctest_p - 47), 48);
    if ((strcmp(ctx->synctest32, PROVOICE_SYNC) == 0) || (strcmp(ctx->synctest32, PROVOICE_EA_SYNC) == 0)) {
        frame_sync_note_cc_sync(ctx);
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "ProVoice ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+PV   ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_PROVOICE_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 32);
        return DSD_SYNC_PROVOICE_POS;
    }

    if ((strcmp(ctx->synctest32, INV_PROVOICE_SYNC) == 0) || (strcmp(ctx->synctest32, INV_PROVOICE_EA_SYNC) == 0)) {
        frame_sync_note_cc_sync(ctx);
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "ProVoice ");
        printFrameSync(opts, state, "-PV   ", ctx->synctest_pos + 1, ctx->modulation);
        state->lastsynctype = DSD_SYNC_PROVOICE_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 32);
        return DSD_SYNC_PROVOICE_NEG;
    }

    if (strcmp(ctx->synctest48, EDACS_SYNC) == 0) {
        dsd_mark_cc_sync(state);
        frame_sync_set_basic_lock(ctx);
        printFrameSync(opts, state, "-EDACS", ctx->synctest_pos + 1, ctx->modulation);
        state->lastsynctype = DSD_SYNC_EDACS_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 48);
        return DSD_SYNC_EDACS_NEG;
    }

    if (strcmp(ctx->synctest48, INV_EDACS_SYNC) == 0) {
        dsd_mark_cc_sync(state);
        frame_sync_set_basic_lock(ctx);
        printFrameSync(opts, state, "+EDACS", ctx->synctest_pos + 1, ctx->modulation);
        state->lastsynctype = DSD_SYNC_EDACS_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 48);
        return DSD_SYNC_EDACS_POS;
    }

    if ((strcmp(ctx->synctest48, DOTTING_SEQUENCE_A) == 0) || (strcmp(ctx->synctest48, DOTTING_SEQUENCE_B) == 0)) {
        if (opts->p25_trunk == 1 && opts->p25_is_tuned == 1) {
            printFrameSync(opts, state, " EDACS  DOTTING SEQUENCE: ", ctx->synctest_pos + 1, ctx->modulation);
            dsd_frame_sync_hook_eot_cc(opts, state);
        }
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_dstar(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_dstar != 1 || dsd_frame_sync_suppress_p25_alt_sync(opts, state)) {
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest, DSTAR_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "DSTAR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+DSTAR VOICE ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_VOICE_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_VOICE_POS;
    }

    if (strcmp(ctx->synctest, INV_DSTAR_SYNC) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "DSTAR ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-DSTAR VOICE ", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_VOICE_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_VOICE_NEG;
    }

    if (strcmp(ctx->synctest, DSTAR_HD) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, "DSTAR_HD ");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "+DSTAR HEADER", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_HD_POS;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_HD_POS;
    }

    if (strcmp(ctx->synctest, INV_DSTAR_HD) == 0) {
        frame_sync_set_basic_lock(ctx);
        DSD_SPRINTF(state->ftype, " DSTAR_HD");
        if (opts->errorbars == 1) {
            printFrameSync(opts, state, "-DSTAR HEADER", ctx->synctest_pos + 1, ctx->modulation);
        }
        state->lastsynctype = DSD_SYNC_DSTAR_HD_NEG;
        dsd_sync_warm_start_thresholds_outer_only(opts, state, 24);
        return DSD_SYNC_DSTAR_HD_NEG;
    }

    return DSD_SYNC_NONE;
}

static int
frame_sync_try_nxdn(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if ((opts->frame_nxdn96 != 1) && (opts->frame_nxdn48 != 1)) {
        return DSD_SYNC_NONE;
    }

    dsd_strncpy_s(ctx->synctest10, 11, (ctx->synctest_p - 9), 10);
    if ((strcmp(ctx->synctest10, "3131331131") == 0) || (strcmp(ctx->synctest10, "3331331131") == 0)
        || (strcmp(ctx->synctest10, "3131331111") == 0) || (strcmp(ctx->synctest10, "3331331111") == 0)
        || (strcmp(ctx->synctest10, "3131311131") == 0)) {
        state->offset = ctx->synctest_pos;
        state->max = ((state->max) + ctx->lmax) / 2;
        state->min = ((state->min) + ctx->lmin) / 2;
        if (state->lastsynctype == DSD_SYNC_NXDN_POS) {
            frame_sync_note_cc_sync(ctx);
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 10);
            return DSD_SYNC_NXDN_POS;
        }
        state->lastsynctype = DSD_SYNC_NXDN_POS;
        return DSD_SYNC_NONE;
    }

    if ((strcmp(ctx->synctest10, "1313113313") == 0) || (strcmp(ctx->synctest10, "1113113313") == 0)
        || (strcmp(ctx->synctest10, "1313113333") == 0) || (strcmp(ctx->synctest10, "1113113333") == 0)
        || (strcmp(ctx->synctest10, "1313133313") == 0)) {
        state->offset = ctx->synctest_pos;
        state->max = ((state->max) + ctx->lmax) / 2;
        state->min = ((state->min) + ctx->lmin) / 2;
        if (state->lastsynctype == DSD_SYNC_NXDN_NEG) {
            frame_sync_note_cc_sync(ctx);
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 10);
            return DSD_SYNC_NXDN_NEG;
        }
        state->lastsynctype = DSD_SYNC_NXDN_NEG;
    }

    return DSD_SYNC_NONE;
}

#ifdef PVCONVENTIONAL
static void
frame_sync_pvconv_decode_addrs(const frame_sync_match_ctx* ctx, char one_symbol, uint8_t* tx_addr, uint8_t* rx_addr) {
    *tx_addr = 0;
    *rx_addr = 0;
    for (int bit = 0; bit < 8; bit++) {
        *tx_addr = (uint8_t)(*tx_addr << 1);
        *rx_addr = (uint8_t)(*rx_addr << 1);
        if (*(ctx->synctest_p - 15 + bit) == one_symbol) {
            *tx_addr = (uint8_t)(*tx_addr + 1);
        }
        if (*(ctx->synctest_p - 7 + bit) == one_symbol) {
            *rx_addr = (uint8_t)(*rx_addr + 1);
        }
    }
}

static int
frame_sync_try_provoice_conventional(frame_sync_match_ctx* ctx) {
    const dsd_opts* opts = ctx->opts;
    dsd_state* state = ctx->state;
    if (opts->frame_provoice != 1) {
        return DSD_SYNC_NONE;
    }

    DSD_MEMSET(ctx->synctest32, 0, 33);
    dsd_strncpy_s(ctx->synctest32, 33, (ctx->synctest_p - 31), 16);
    if (strcmp(ctx->synctest32, INV_PROVOICE_CONV_SHORT) == 0) {
        if (state->lastsynctype == DSD_SYNC_PROVOICE_NEG) {
            frame_sync_set_basic_lock(ctx);
            DSD_SPRINTF(state->ftype, "ProVoice ");
            uint8_t pvc_txa = 0;
            uint8_t pvc_rxa = 0;
            frame_sync_pvconv_decode_addrs(ctx, '1', &pvc_txa, &pvc_rxa);
            printFrameSync(opts, state, "-PV_C ", ctx->synctest_pos + 1, ctx->modulation);
            DSD_FPRINTF(stderr, "TX: %d ", pvc_txa);
            DSD_FPRINTF(stderr, "RX: %d ", pvc_rxa);
            if (pvc_txa == 172) {
                DSD_FPRINTF(stderr, "ALL CALL ");
            }
            state->lastsynctype = DSD_SYNC_PROVOICE_NEG;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 16);
            return DSD_SYNC_PROVOICE_NEG;
        }
        state->lastsynctype = DSD_SYNC_PROVOICE_NEG;
        return DSD_SYNC_NONE;
    }

    if (strcmp(ctx->synctest32, PROVOICE_CONV_SHORT) == 0) {
        if (state->lastsynctype == DSD_SYNC_PROVOICE_POS) {
            frame_sync_set_basic_lock(ctx);
            DSD_SPRINTF(state->ftype, "ProVoice ");
            uint8_t pvc_txa = 0;
            uint8_t pvc_rxa = 0;
            frame_sync_pvconv_decode_addrs(ctx, '3', &pvc_txa, &pvc_rxa);
            printFrameSync(opts, state, "+PV_C ", ctx->synctest_pos + 1, ctx->modulation);
            DSD_FPRINTF(stderr, "TX: %d ", pvc_txa);
            DSD_FPRINTF(stderr, "RX: %d ", pvc_rxa);
            if (pvc_txa == 172) {
                DSD_FPRINTF(stderr, "ALL CALL ");
            }
            state->lastsynctype = DSD_SYNC_PROVOICE_POS;
            dsd_sync_warm_start_thresholds_outer_only(opts, state, 16);
            return DSD_SYNC_PROVOICE_POS;
        }
        state->lastsynctype = DSD_SYNC_PROVOICE_POS;
    }

    return DSD_SYNC_NONE;
}
#else
static int
frame_sync_try_provoice_conventional(frame_sync_match_ctx* ctx) {
    UNUSED(ctx);
    return DSD_SYNC_NONE;
}
#endif

static int
frame_sync_try_protocol_matches(frame_sync_match_ctx* ctx) {
    int sync_type = frame_sync_try_p25p1(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_x2tdma(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_ysf(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_m17(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_p25p2(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_dpmr(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    sync_type = frame_sync_try_dmr(ctx);
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    if (ctx->opts->frame_provoice == 1) {
        sync_type = frame_sync_try_provoice(ctx);
    } else if (ctx->opts->frame_dstar == 1) {
        sync_type = frame_sync_try_dstar(ctx);
    } else if (ctx->opts->frame_nxdn96 == 1 || ctx->opts->frame_nxdn48 == 1) {
        sync_type = frame_sync_try_nxdn(ctx);
    }
    if (sync_type != DSD_SYNC_NONE) {
        return sync_type;
    }

    return frame_sync_try_provoice_conventional(ctx);
}

static void
frame_sync_maybe_tick_p25_trunk_sm(dsd_opts* opts, dsd_state* state, time_t now) {
    static time_t last_tick = 0;
    static time_t last_p25_seen = 0;
    if (now == last_tick) {
        return;
    }

    int p25_by_sync = DSD_SYNC_IS_P25(state->lastsynctype) ? 1 : 0;
    if (p25_by_sync) {
        last_p25_seen = now;
    }
    int p25_recent = (last_p25_seen != 0 && (now - last_p25_seen) <= 3) ? 1 : 0;
    int p25_active = p25_by_sync || p25_recent || (state->p25_p2_active_slot != -1);
    if (opts->p25_trunk == 1 && p25_active) {
        dsd_frame_sync_hook_p25_sm_try_tick(opts, state);
    }
    last_tick = now;
}

static inline void
frame_sync_apply_cli_mod_lock(const dsd_opts* opts, dsd_state* state) {
    if (opts->mod_cli_lock) {
        int forced = opts->mod_qpsk ? 1 : (opts->mod_gfsk ? 2 : 0);
        state->rf_mod = forced;
    }
}

static int
frame_sync_select_t_max(const dsd_opts* opts, const dsd_state* state) {
    if (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1) {
        return 10;
    }
    if (opts->frame_dpmr == 1) {
        return 12;
    }
    if (opts->frame_m17 == 1) {
        return 8;
    }
    if (DSD_SYNC_IS_YSF(state->lastsynctype)) {
        return 20;
    }
    if (DSD_SYNC_IS_P25P2(state->lastsynctype) || (state->p25_p2_active_slot >= 0 && opts->frame_p25p2 == 1)) {
        return 19;
    }
    return 24;
}

static inline void
frame_sync_update_symbol_ring(const dsd_opts* opts, dsd_state* state, float symbol, float* lbuf, int* lidx, int t_max) {
    lbuf[*lidx] = symbol;
    state->sbuf[state->sidx] = symbol;
    if (*lidx == (t_max - 1)) {
        *lidx = 0;
    } else {
        (*lidx)++;
    }
    if (state->sidx == (opts->ssize - 1)) {
        state->sidx = 0;
    } else {
        state->sidx++;
    }
}

static int
frame_sync_bias_want_mod_with_snr(const dsd_state* state, int want_mod) {
#ifdef USE_RADIO
    double snr_c = dsd_rtl_stream_metrics_hook_snr_c4fm_db();
    double snr_q = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
    if (snr_c <= -50.0) {
        snr_c = dsd_rtl_stream_metrics_hook_snr_c4fm_eye_db();
    }
    if (snr_q <= -50.0) {
        snr_q = dsd_rtl_stream_metrics_hook_snr_qpsk_const_db();
    }
    if (snr_c <= -50.0) {
        if (snr_q <= -50.0) {
            return want_mod;
        }
        if (state->rf_mod == 1) {
            return 1;
        }
        return want_mod;
    }
    if (snr_q <= -50.0) {
        return want_mod;
    }

    const double kQpskOffsetDb = 6.0;
    double normalized_delta = (snr_q - kQpskOffsetDb) - snr_c;
    uint32_t now_ms = (uint32_t)dsd_time_monotonic_ms();
    uint32_t dwell_enter_ms = (uint32_t)atomic_load(&g_qpsk_dwell_enter_ms);
    int in_qpsk_dwell = (state->rf_mod == 1 && dwell_enter_ms != 0 && (uint32_t)(now_ms - dwell_enter_ms) < 2000U);
    if (normalized_delta >= 2.0) {
        return 1;
    }
    if (normalized_delta <= -3.0 && !in_qpsk_dwell) {
        return 0;
    }
    if (state->rf_mod == 1) {
        return 1;
    }
#else
    UNUSED(state);
#endif
    return want_mod;
}

static int
frame_sync_decay_recent_ham(atomic_int* ham_counter) {
    int ham = atomic_load(ham_counter);
    if (ham < 24) {
        ham++;
        atomic_store(ham_counter, ham);
    }
    return ham;
}

static int
frame_sync_ham_for_mod(int mod, int ham_c4fm, int ham_qpsk, int ham_gfsk) {
    if (mod == 1) {
        return ham_qpsk;
    }
    if (mod == 2) {
        return ham_gfsk;
    }
    return ham_c4fm;
}

static int
frame_sync_override_want_mod_with_hamming(const dsd_opts* opts, int want_mod) {
    int ham_c4fm = frame_sync_decay_recent_ham(&g_ham_c4fm_recent);
    int ham_qpsk = frame_sync_decay_recent_ham(&g_ham_qpsk_recent);
    int ham_gfsk = frame_sync_decay_recent_ham(&g_ham_gfsk_recent);

    int best_mod = want_mod;
    int best_ham = frame_sync_ham_for_mod(want_mod, ham_c4fm, ham_qpsk, ham_gfsk);
    if (ham_c4fm < best_ham) {
        best_ham = ham_c4fm;
        best_mod = 0;
    }
    int qpsk_enabled = (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1);
    if (qpsk_enabled && ham_qpsk < best_ham) {
        best_ham = ham_qpsk;
        best_mod = 1;
    }
    if (ham_gfsk < best_ham) {
        best_ham = ham_gfsk;
        best_mod = 2;
    }

    if (best_ham <= 3) {
        return best_mod;
    }
    if (best_ham >= 24) {
        return want_mod;
    }
    int current_ham = frame_sync_ham_for_mod(want_mod, ham_c4fm, ham_qpsk, ham_gfsk);
    if (current_ham >= 24 || best_ham + 4 <= current_ham) {
        return best_mod;
    }
    return want_mod;
}

static void
frame_sync_update_mod_votes(int want_mod) {
    if (want_mod == 1) {
        atomic_fetch_add(&g_vote_qpsk, 1);
        atomic_store(&g_vote_c4fm, 0);
        atomic_store(&g_vote_gfsk, 0);
    } else if (want_mod == 2) {
        atomic_fetch_add(&g_vote_gfsk, 1);
        atomic_store(&g_vote_qpsk, 0);
        atomic_store(&g_vote_c4fm, 0);
    } else {
        atomic_fetch_add(&g_vote_c4fm, 1);
        atomic_store(&g_vote_qpsk, 0);
        atomic_store(&g_vote_gfsk, 0);
    }
}

static int
frame_sync_decide_mod_switch(const dsd_state* state, int want_mod) {
    uint32_t now_ms = (uint32_t)dsd_time_monotonic_ms();
    uint32_t dwell_enter_ms = (uint32_t)atomic_load(&g_qpsk_dwell_enter_ms);
    int in_qpsk_dwell2 = (state->rf_mod == 1 && dwell_enter_ms != 0 && (uint32_t)(now_ms - dwell_enter_ms) < 2000U);
    int req_c4_votes = (state->rf_mod == 1) ? (in_qpsk_dwell2 ? 5 : 3) : 2;
    int vote_qpsk = atomic_load(&g_vote_qpsk);
    int vote_gfsk = atomic_load(&g_vote_gfsk);
    int vote_c4fm = atomic_load(&g_vote_c4fm);
    if (want_mod == 1 && vote_qpsk >= 2 && state->rf_mod != 1) {
        return 1;
    }
    if (want_mod == 2 && vote_gfsk >= 1 && state->rf_mod != 2) {
        return 2;
    }
    if (want_mod == 0 && vote_c4fm >= req_c4_votes && state->rf_mod != 0) {
        return 0;
    }
    return -1;
}

static void
frame_sync_apply_mod_switch(dsd_state* state, int do_switch) {
    if (do_switch < 0) {
        return;
    }
    if (do_switch == 1) {
        atomic_store(&g_qpsk_dwell_enter_ms, (int)(uint32_t)dsd_time_monotonic_ms());
    } else if (state->rf_mod == 1) {
        atomic_store(&g_qpsk_dwell_enter_ms, 0);
    }
    state->rf_mod = do_switch;
    atomic_store(&g_ham_c4fm_recent, 24);
    atomic_store(&g_ham_qpsk_recent, 24);
    atomic_store(&g_ham_gfsk_recent, 24);
}

static void
frame_sync_maybe_auto_switch_modulation(const dsd_opts* opts, dsd_state* state, int t_max, int* lastt) {
    if (*lastt != t_max) {
        (*lastt)++;
        return;
    }

    *lastt = 0;
    if (state->carrier == 1) {
        state->sps_hunt_counter = 0;
    }
    if (opts->mod_cli_lock) {
        return;
    }

    int want_mod = state->rf_mod;
    want_mod = frame_sync_bias_want_mod_with_snr(state, want_mod);
    want_mod = frame_sync_override_want_mod_with_hamming(opts, want_mod);
    frame_sync_update_mod_votes(want_mod);
    frame_sync_apply_mod_switch(state, frame_sync_decide_mod_switch(state, want_mod));
}

static void
frame_sync_debug_symbol_stats(const dsd_opts* opts, float symbol) {
#ifdef USE_RADIO
    const dsdneoRuntimeConfig* cfg_dbg = dsd_neo_get_config();
    if (!cfg_dbg) {
        dsd_neo_config_init(opts);
        cfg_dbg = dsd_neo_get_config();
    }
    if (cfg_dbg && cfg_dbg->debug_sync_enable) {
        static int sym_count = 0;
        static int pos_count = 0, neg_count = 0;
        static float sym_min = 1e9f, sym_max = -1e9f, sym_sum = 0.0f;
        if (symbol < sym_min) {
            sym_min = symbol;
        }
        if (symbol > sym_max) {
            sym_max = symbol;
        }
        sym_sum += symbol;
        if (symbol > 0) {
            pos_count++;
        } else {
            neg_count++;
        }
        if (++sym_count >= 4800) {
            float dc = sym_sum / (float)sym_count;
            DSD_FPRINTF(stderr, "[SYNC] range:[%.1f,%.1f] dc:%.2f ratio(1:3)=%d:%d\n", sym_min, sym_max, dc, pos_count,
                        neg_count);
            sym_min = 1e9f;
            sym_max = -1e9f;
            sym_sum = 0.0f;
            pos_count = neg_count = 0;
            sym_count = 0;
        }
    }
#else
    UNUSED2(opts, symbol);
#endif
}

static int
frame_sync_cqpsk_4level_enabled(const dsd_opts* opts, const dsd_state* state) {
#ifdef USE_RADIO
    if (state->rf_mod == 1 && opts->audio_in_type == AUDIO_IN_RTL
        && (opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1)) {
        int dsp_cqpsk = 0, dsp_fll = 0, dsp_ted = 0;
        dsd_rtl_stream_metrics_hook_dsp_get(&dsp_cqpsk, &dsp_fll, &dsp_ted);
        if (dsp_cqpsk && dsp_ted) {
            return 1;
        }
    }
#else
    UNUSED2(opts, state);
#endif
    return 0;
}

static int
frame_sync_slice_cqpsk_dibit(const dsd_opts* opts, const dsd_state* state, float symbol) {
    float sym = symbol - state->center;
    int d = 0;
    if (sym >= 2.0f) {
        d = 1;
    } else if (sym >= 0.0f) {
        d = 0;
    } else if (sym >= -2.0f) {
        d = 2;
    } else {
        d = 3;
    }

#ifdef USE_RADIO
    const dsdneoRuntimeConfig* cfg_dbg = dsd_neo_get_config();
    if (!cfg_dbg) {
        dsd_neo_config_init(opts);
        cfg_dbg = dsd_neo_get_config();
    }
    if (cfg_dbg && cfg_dbg->debug_cqpsk_enable) {
        static int sample_count = 0;
        static int hist[4] = {0, 0, 0, 0};
        static float sym_sum = 0.0f;
        static float sym_min = 1000.0f, sym_max = -1000.0f;
        hist[d]++;
        sym_sum += sym;
        if (sym < sym_min) {
            sym_min = sym;
        }
        if (sym > sym_max) {
            sym_max = sym;
        }
        if (++sample_count >= 4800) {
            float sym_avg = sym_sum / sample_count;
            DSD_FPRINTF(stderr, "[SLICER] d0:%.1f%% d1:%.1f%% d2:%.1f%% d3:%.1f%% avg:%.2f range:[%.2f,%.2f] (n=%d)\n",
                        100.0f * hist[0] / sample_count, 100.0f * hist[1] / sample_count,
                        100.0f * hist[2] / sample_count, 100.0f * hist[3] / sample_count, sym_avg, sym_min, sym_max,
                        sample_count);
            hist[0] = hist[1] = hist[2] = hist[3] = 0;
            sample_count = 0;
            sym_sum = 0.0f;
            sym_min = 1000.0f;
            sym_max = -1000.0f;
        }
    }
#else
    UNUSED(opts);
#endif
    return d;
}

static int
frame_sync_symbol_to_dibit(const dsd_opts* opts, dsd_state* state, float symbol, int cqpsk_4level) {
    if (cqpsk_4level) {
        int d = frame_sync_slice_cqpsk_dibit(opts, state, symbol);
        *state->dibit_buf_p = d;
        state->dibit_buf_p++;
        return '0' + d;
    }

    if (symbol > 0) {
        *state->dibit_buf_p = 1;
        state->dibit_buf_p++;
        return '1';
    }
    *state->dibit_buf_p = 3;
    state->dibit_buf_p++;
    return '3';
}

static void
frame_sync_capture_symbol(dsd_opts* opts, dsd_state* state, int dibit, float symbol, int cqpsk_4level) {
    if (!opts->symbol_out_f || dibit == 0) {
        return;
    }
#ifndef USE_RADIO
    UNUSED(cqpsk_4level);
#endif
    int csymbol = 0;
#ifdef USE_RADIO
    if (cqpsk_4level) {
        csymbol = dibit - '0';
    } else
#endif
        if (dibit == '1') {
        csymbol = 1;
    } else if (dibit == '3') {
        csymbol = 3;
    }
    write_symbol_capture_record(opts, state, csymbol, symbol);
}

static void
frame_sync_reset_dmr_payload_ptrs(dsd_state* state) {
    if (state->dmr_payload_p > state->dmr_payload_buf + 900000) {
        state->dmr_payload_p = state->dmr_payload_buf + 200;
    }
    if (state->dmr_reliab_p && state->dmr_reliab_p > state->dmr_reliab_buf + 900000) {
        state->dmr_reliab_p = state->dmr_reliab_buf + 200;
    }
    if (state->dmr_soft_p && state->dmr_soft_p > state->dmr_soft_buf + 900000) {
        state->dmr_soft_p = state->dmr_soft_buf + 200;
    }
}

static void
frame_sync_store_dmr_payload_symbol(dsd_state* state, float symbol, int cqpsk_4level) {
    int d = 0;
    if (cqpsk_4level) {
        float sym = symbol - state->center;
        if (sym >= 2.0f) {
            d = 1;
        } else if (sym >= 0.0f) {
            d = 0;
        } else if (sym >= -2.0f) {
            d = 2;
        } else {
            d = 3;
        }
    } else if (symbol > state->center) {
        d = (symbol > state->umid) ? 1 : 0;
    } else {
        d = (symbol < state->lmid) ? 3 : 2;
    }

    *state->dmr_payload_p = d;
    uint8_t rel = dmr_compute_reliability(state, symbol);
    if (state->dmr_reliab_p) {
        *state->dmr_reliab_p = rel;
        state->dmr_reliab_p++;
    }
    if (state->dmr_soft_p) {
        state->dmr_soft_p->reliability = rel;
        state->dmr_soft_p->llr[0] = (int16_t)(((d >> 1) & 1) ? rel : -(int)rel);
        state->dmr_soft_p->llr[1] = (int16_t)((d & 1) ? rel : -(int)rel);
        state->dmr_soft_p++;
    }
    state->dmr_payload_p++;
}

static int
frame_sync_process_dibit_and_payload(dsd_opts* opts, dsd_state* state, float symbol) {
    if (state->dibit_buf_p > state->dibit_buf + 900000) {
        state->dibit_buf_p = state->dibit_buf + 200;
    }
    frame_sync_debug_symbol_stats(opts, symbol);
    int cqpsk_4level = frame_sync_cqpsk_4level_enabled(opts, state);
    int dibit = frame_sync_symbol_to_dibit(opts, state, symbol, cqpsk_4level);
    frame_sync_capture_symbol(opts, state, dibit, symbol, cqpsk_4level);
    frame_sync_reset_dmr_payload_ptrs(state);
    frame_sync_store_dmr_payload_symbol(state, symbol, cqpsk_4level);
    return dibit;
}

typedef struct {
    int t;
    int dibit;
    int synctest_pos;
    int lastt;
    int lidx;
    int t_max;
    float symbol;
    float lmin;
    float lmax;
    char synctest[25];
    char synctest12[13];
    char synctest10[11];
    char synctest32[33];
    char synctest20[21];
    char synctest48[49];
    char synctest8[9];
    char synctest16[17];
    char modulation[8];
    char* synctest_p;
    char synctest_buf[10240];
    float lbuf[48];
    float lbuf2[48];
} frame_sync_runtime_ctx;

static void
frame_sync_runtime_init(frame_sync_runtime_ctx* rt, const dsd_opts* opts, const dsd_state* state) {
    rt->t = 0;
    rt->synctest_pos = 0;
    rt->lastt = 0;
    rt->lidx = 0;
    rt->t_max = frame_sync_select_t_max(opts, state);
    DSD_MEMSET(rt->lbuf, 0, sizeof(rt->lbuf));
    DSD_MEMSET(rt->lbuf2, 0, sizeof(rt->lbuf2));
    rt->synctest[24] = 0;
    rt->synctest12[12] = 0;
    rt->synctest10[10] = 0;
    rt->synctest32[32] = 0;
    rt->synctest20[20] = 0;
    rt->synctest48[48] = 0;
    rt->synctest8[8] = 0;
    rt->synctest16[16] = 0;
    rt->modulation[7] = 0;
    rt->synctest_p = rt->synctest_buf + 10;
}

static void
frame_sync_window_levels(const dsd_opts* opts, dsd_state* state, frame_sync_runtime_ctx* rt) {
    int i;
    for (i = 0; i < rt->t_max; i++) {
        rt->lbuf2[i] = rt->lbuf[i];
    }
    qsort(rt->lbuf2, rt->t_max, sizeof(float), comp);
    rt->lmin = (rt->lbuf2[1] + rt->lbuf2[2] + rt->lbuf2[3]) / 3.0f;
    rt->lmax = (rt->lbuf2[rt->t_max - 3] + rt->lbuf2[rt->t_max - 2] + rt->lbuf2[rt->t_max - 1]) / 3.0f;

    if (state->rf_mod == 1) {
        dsd_state_push_minmax_window(state, opts->msize, rt->lmin, rt->lmax);
        state->center = ((state->max) + (state->min)) / 2.0f;
        state->maxref = (state->max) * 0.80F;
        state->minref = (state->min) * 0.80F;
    } else {
        state->maxref = state->max;
        state->minref = state->min;
    }
}

static int
frame_sync_should_skip_snr_or_power_gate(const dsd_opts* opts) {
    int is_gfsk_mode =
        (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1 || opts->frame_dpmr == 1 || opts->frame_m17 == 1);
#ifdef USE_RADIO
    {
        const dsdneoRuntimeConfig* cfg = dsd_neo_get_config();
        if (cfg && cfg->snr_sql_is_set) {
            double snr_db = -200.0;
            if (opts->frame_p25p1 == 1) {
                snr_db = dsd_rtl_stream_metrics_hook_snr_c4fm_db();
            } else if (opts->frame_p25p2 == 1) {
                snr_db = dsd_rtl_stream_metrics_hook_snr_cqpsk_db();
            } else if (is_gfsk_mode) {
                snr_db = dsd_rtl_stream_metrics_hook_snr_gfsk_db();
            }
            if (snr_db > -150.0 && snr_db < (double)cfg->snr_sql_db) {
                return 1;
            }
        }
    }
#endif
    if (opts->audio_in_type == AUDIO_IN_RTL && opts->rtl_pwr < opts->rtl_squelch_level && is_gfsk_mode) {
        return 1;
    }
    return 0;
}

static int
frame_sync_hamming_distance_pattern(const char* symbols, const char* pattern, int len) {
    int ham = 0;
    for (int k = 0; k < len; k++) {
        int d = (unsigned char)symbols[k] - '0';
        int expect = pattern[k] - '0';
        if (d != expect) {
            ham++;
        }
    }
    return ham;
}

static void
frame_sync_update_c4fm_hamming(const dsd_opts* opts, const char* synctest) {
    if (!(opts->frame_p25p1 == 1 && !opts->mod_cli_lock)) {
        return;
    }
    int ham_norm = 0;
    int ham_inv = 0;
    for (int k = 0; k < 24; k++) {
        int d = (unsigned char)synctest[k] - '0';
        int expect_n = P25P1_SYNC[k] - '0';
        int expect_i = INV_P25P1_SYNC[k] - '0';
        if (d != expect_n) {
            ham_norm++;
        }
        if (d != expect_i) {
            ham_inv++;
        }
    }
    int c4fm_ham = (ham_norm < ham_inv) ? ham_norm : ham_inv;
    int ham_c4fm_cur = atomic_load(&g_ham_c4fm_recent);
    if (c4fm_ham < ham_c4fm_cur) {
        atomic_store(&g_ham_c4fm_recent, c4fm_ham);
    }
}

static void
frame_sync_update_qpsk_hamming(const dsd_opts* opts, const char* synctest, const char* synctest20) {
    if (!((opts->frame_p25p1 == 1 || opts->frame_p25p2 == 1) && !opts->mod_cli_lock)) {
        return;
    }

    int best_qpsk_ham = 24;
    if (opts->frame_p25p1 == 1) {
        best_qpsk_ham = dsd_qpsk_sync_hamming_with_remaps(synctest, P25P1_SYNC, INV_P25P1_SYNC, 24);
    }
    if (opts->frame_p25p2 == 1) {
        int ham_p2 = dsd_qpsk_sync_hamming_with_remaps(synctest20, P25P2_SYNC, INV_P25P2_SYNC, 20);
        int ham_p2_scaled = (ham_p2 * 24 + 19) / 20;
        if (ham_p2_scaled < best_qpsk_ham || opts->frame_p25p1 == 0) {
            best_qpsk_ham = ham_p2_scaled;
        }
    }
    int ham_qpsk_cur = atomic_load(&g_ham_qpsk_recent);
    if (best_qpsk_ham < ham_qpsk_cur) {
        atomic_store(&g_ham_qpsk_recent, best_qpsk_ham);
    }
}

static int
frame_sync_best_ham_for_patterns(const char* symbols, const char* patterns[], int pattern_count, int pattern_len,
                                 int best_start) {
    int best = best_start;
    for (int p = 0; p < pattern_count; p++) {
        int ham = frame_sync_hamming_distance_pattern(symbols, patterns[p], pattern_len);
        if (ham < best) {
            best = ham;
        }
    }
    return best;
}

static int
frame_sync_best_nxdn_scaled_ham(frame_sync_runtime_ctx* rt, int best_start) {
    DSD_STRNCPY(rt->synctest10, (rt->synctest_p - 9), 10);
    const char* nxdn_patterns[] = {"3131331131", "1313113313"};
    int best = best_start;
    for (int p = 0; p < 2; p++) {
        int ham = frame_sync_hamming_distance_pattern(rt->synctest10, nxdn_patterns[p], 10);
        int scaled_ham = (ham * 24 + 9) / 10;
        if (scaled_ham < best) {
            best = scaled_ham;
        }
    }
    return best;
}

static void
frame_sync_update_gfsk_hamming(const dsd_opts* opts, frame_sync_runtime_ctx* rt) {
    if (!((opts->frame_dmr == 1 || opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1 || opts->frame_dpmr == 1)
          && !opts->mod_cli_lock)) {
        return;
    }

    int best_gfsk_ham = 24;
    if (opts->frame_dmr == 1) {
        const char* dmr_patterns[] = {DMR_BS_DATA_SYNC, DMR_BS_VOICE_SYNC, DMR_MS_DATA_SYNC, DMR_MS_VOICE_SYNC};
        best_gfsk_ham = frame_sync_best_ham_for_patterns(rt->synctest, dmr_patterns, 4, 24, best_gfsk_ham);
    }
    if (opts->frame_dpmr == 1) {
        const char* dpmr_patterns[] = {DPMR_FRAME_SYNC_1, DPMR_FRAME_SYNC_4, INV_DPMR_FRAME_SYNC_1,
                                       INV_DPMR_FRAME_SYNC_4};
        best_gfsk_ham = frame_sync_best_ham_for_patterns(rt->synctest, dpmr_patterns, 4, 24, best_gfsk_ham);
    }
    if (opts->frame_nxdn48 == 1 || opts->frame_nxdn96 == 1) {
        best_gfsk_ham = frame_sync_best_nxdn_scaled_ham(rt, best_gfsk_ham);
    }
    int ham_gfsk_cur = atomic_load(&g_ham_gfsk_recent);
    if (best_gfsk_ham < ham_gfsk_cur) {
        atomic_store(&g_ham_gfsk_recent, best_gfsk_ham);
    }
}

#ifdef USE_RADIO
static void
frame_sync_debug_sync_dmr(dsd_opts* opts, dsd_state* state, const frame_sync_runtime_ctx* rt) {
    DSD_FPRINTF(stderr, "[SYNC] pattern=%s expect=%s\n", rt->synctest, P25P1_SYNC);
    if (opts->frame_dmr != 1) {
        return;
    }

    const char* best_name = NULL;
    int best_ham = dmr_best_sync_hamming(rt->synctest, &best_name);
    int rtl_sym_rate = 0;
    int rtl_levels = 0;
    (void)dsd_rtl_stream_metrics_hook_symbol_profile(&rtl_sym_rate, &rtl_levels, NULL);
    double snr_gfsk = dsd_rtl_stream_metrics_hook_snr_gfsk_db();
    DSD_FPRINTF(stderr,
                "[DMRDBG] best=%s ham=%d rf_mod=%d lock=%d mods(c/q/g)=%d/%d/%d "
                "rtl_profile=%d/%d pwr=%.1fdB sql=%.1fdB snr_gfsk=%.1fdB win=%.*s\n",
                best_name ? best_name : "none", best_ham, state->rf_mod, opts->mod_cli_lock, opts->mod_c4fm,
                opts->mod_qpsk, opts->mod_gfsk, rtl_sym_rate, rtl_levels, pwr_to_dB(opts->rtl_pwr),
                pwr_to_dB(opts->rtl_squelch_level), snr_gfsk, 24, rt->synctest);
}

static void
frame_sync_debug_sync_cqpsk(const frame_sync_runtime_ctx* rt) {
    static const int d_rot_map[4] = {1, 3, 0, 2};
    int ham_norm = 0, ham_inv = 0, ham_ident = 0, ham_invert = 0, ham_swap = 0, ham_xor3 = 0, ham_rot = 0;
    for (int k = 0; k < 24; k++) {
        int d = (unsigned char)rt->synctest[k];
        if (d >= '0' && d <= '3') {
            d -= '0';
        }
        int expect_n = P25P1_SYNC[k] - '0';
        int expect_i = INV_P25P1_SYNC[k] - '0';
        ham_norm += (d != expect_n);
        ham_ident += (d != expect_n);
        ham_inv += (d != expect_i);
        int d_inv = (d == 0) ? 2 : (d == 1) ? 3 : (d == 2) ? 0 : 1;
        int d_swap = ((d & 1) << 1) | ((d & 2) >> 1);
        int d_xor3 = d ^ 0x3;
        int d_rot = d_rot_map[d & 0x3];
        ham_invert += (d_inv != expect_n);
        ham_swap += (d_swap != expect_n);
        ham_xor3 += (d_xor3 != expect_n);
        ham_rot += (d_rot != expect_n);
    }
    static int dbg_win = 0;
    if ((++dbg_win % 1200) == 0) {
        DSD_FPRINTF(stderr, "[SYNCDBG] ham(norm=%d inv=%d ident=%d inv2=%d swap=%d xor3=%d rot=%d) win=%.*s\n",
                    ham_norm, ham_inv, ham_ident, ham_invert, ham_swap, ham_xor3, ham_rot, 24, rt->synctest);
    }
}
#endif

static void
frame_sync_debug_sync_window(dsd_opts* opts, dsd_state* state, const frame_sync_runtime_ctx* rt) {
#ifdef USE_RADIO
    static int debug_count = 0;
    const dsdneoRuntimeConfig* cfg_dbg = dsd_neo_get_config();
    if (!cfg_dbg) {
        dsd_neo_config_init(opts);
        cfg_dbg = dsd_neo_get_config();
    }
    int debug_sync = (cfg_dbg && cfg_dbg->debug_sync_enable) ? 1 : 0;
    int debug_cqpsk = (cfg_dbg && cfg_dbg->debug_cqpsk_enable) ? 1 : 0;

    if (debug_sync && (++debug_count % 4800) == 0) {
        frame_sync_debug_sync_dmr(opts, state, rt);
    }

    if (debug_cqpsk && state->rf_mod == 1) {
        frame_sync_debug_sync_cqpsk(rt);
    }
#else
    UNUSED2(opts, state);
    UNUSED(rt);
#endif
}

static int
frame_sync_eval_window(dsd_opts* opts, dsd_state* state, frame_sync_runtime_ctx* rt, time_t now, double nowm) {
    frame_sync_window_levels(opts, state, rt);
    if (frame_sync_should_skip_snr_or_power_gate(opts)) {
        return DSD_SYNC_NONE;
    }

    DSD_STRNCPY(rt->synctest, (rt->synctest_p - 23), 24);
    DSD_STRNCPY(rt->synctest20, (rt->synctest_p - 19), 20);

    frame_sync_debug_sync_window(opts, state, rt);
    frame_sync_update_c4fm_hamming(opts, rt->synctest);
    frame_sync_update_qpsk_hamming(opts, rt->synctest, rt->synctest20);
    frame_sync_update_gfsk_hamming(opts, rt);

    frame_sync_match_ctx match_ctx = {
        .opts = opts,
        .state = state,
        .now = now,
        .nowm = nowm,
        .synctest_pos = rt->synctest_pos,
        .lmax = rt->lmax,
        .lmin = rt->lmin,
        .modulation = rt->modulation,
        .synctest_p = rt->synctest_p,
        .synctest = rt->synctest,
        .synctest8 = rt->synctest8,
        .synctest10 = rt->synctest10,
        .synctest12 = rt->synctest12,
        .synctest16 = rt->synctest16,
        .synctest20 = rt->synctest20,
        .synctest32 = rt->synctest32,
        .synctest48 = rt->synctest48,
    };
    return frame_sync_try_protocol_matches(&match_ctx);
}

static void
frame_sync_advance_sync_window(dsd_opts* opts, dsd_state* state, frame_sync_runtime_ctx* rt) {
    if (rt->synctest_pos < 10200) {
        rt->synctest_pos++;
        rt->synctest_p++;
        return;
    }
    rt->synctest_pos = 0;
    rt->synctest_p = rt->synctest_buf;
    dsd_frame_sync_hook_no_carrier(opts, state);
}

static int
frame_sync_sps_hunt_next_index(const dsd_opts* opts, const dsd_state* state, const int* sym_rate_cycle,
                               const int* levels_cycle, int cycle_count) {
    int has_4800_four_level = frame_sync_opts_has_4800_four_level_mode(opts);
    int has_4800_binary = (opts->frame_dstar == 1);
    int has_2400 = (opts->frame_nxdn48 == 1 || opts->frame_dpmr == 1);
    int has_9600 = (opts->frame_provoice == 1);
    int has_6000 = (opts->frame_p25p2 == 1 || opts->frame_x2tdma == 1);

    int next_idx = (state->sps_hunt_idx + 1) % cycle_count;
    for (int tries = 0; tries < cycle_count; tries++) {
        int sym_rate = sym_rate_cycle[next_idx];
        int skip =
            (sym_rate == 2400 && !has_2400) || (sym_rate == 9600 && !has_9600) || (sym_rate == 6000 && !has_6000);
        if (sym_rate == 4800) {
            int levels = levels_cycle[next_idx];
            skip = skip || ((levels == 2) ? !has_4800_binary : !has_4800_four_level);
        }
        if (!skip) {
            break;
        }
        next_idx = (next_idx + 1) % cycle_count;
    }
    return next_idx;
}

static void
frame_sync_apply_sps_hunt_profile(const dsd_opts* opts, dsd_state* state, int next_idx, const int* sym_rate_cycle,
                                  const int* levels_cycle) {
    if (next_idx == state->sps_hunt_idx) {
        return;
    }
    state->sps_hunt_idx = next_idx;

#ifdef USE_RADIO
    int demod_rate = 0;
    if (opts->audio_in_type == AUDIO_IN_RTL && state->rtl_ctx) {
        demod_rate = (int)dsd_rtl_stream_metrics_hook_output_rate_hz();
    }
    if (demod_rate <= 0) {
        demod_rate = dsd_opts_current_input_timing_rate(opts);
    }
#else
    UNUSED(levels_cycle);
    int demod_rate = dsd_opts_current_input_timing_rate(opts);
#endif

    int sym_rate = sym_rate_cycle[next_idx];
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, sym_rate, demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
#ifdef USE_RADIO
    rtl_maybe_update_symbol_profile_with_hint(opts, state, sym_rate, levels_cycle[next_idx]);
#endif
    if (opts->verbose > 1) {
        DSD_FPRINTF(stderr, "SPS hunt: trying %d sps (sym=%d, demod=%d)\n", state->samplesPerSymbol, sym_rate,
                    demod_rate);
    }
}

static void
frame_sync_no_sync_sps_hunt(const dsd_opts* opts, dsd_state* state) {
    if (!(state->carrier == 0 && !opts->mod_cli_lock)) {
        return;
    }
    state->sps_hunt_counter++;
    if (state->sps_hunt_counter < dsd_frame_sync_sps_hunt_dwell_passes(opts, state)) {
        return;
    }
    state->sps_hunt_counter = 0;

    static const int sym_rate_cycle[] = {4800, 2400, 9600, 6000, 4800};
    static const int levels_cycle[] = {4, 4, 2, 4, 2};
    const int cycle_count = (int)(sizeof(sym_rate_cycle) / sizeof(sym_rate_cycle[0]));
    int next_idx = frame_sync_sps_hunt_next_index(opts, state, sym_rate_cycle, levels_cycle, cycle_count);
    frame_sync_apply_sps_hunt_profile(opts, state, next_idx, sym_rate_cycle, levels_cycle);
}

static double
frame_sync_elapsed_seconds(double nowm, time_t now, double mono_stamp, time_t wall_stamp) {
    if (mono_stamp > 0.0) {
        return nowm - mono_stamp;
    }
    if (wall_stamp != 0) {
        return (double)(now - wall_stamp);
    }
    return 1e9;
}

static void
frame_sync_p25_slot_activity(const dsd_opts* opts, dsd_state* state, time_t now, double nowm, double mac_hold,
                             double ring_hold, double dt, int* left_active, int* right_active) {
    double l_dmac =
        frame_sync_elapsed_seconds(nowm, now, state->p25_p2_last_mac_active_m[0], state->p25_p2_last_mac_active[0]);
    double r_dmac =
        frame_sync_elapsed_seconds(nowm, now, state->p25_p2_last_mac_active_m[1], state->p25_p2_last_mac_active[1]);
    int l_ring = (state->p25_p2_audio_ring_count[0] > 0) && (l_dmac <= ring_hold);
    int r_ring = (state->p25_p2_audio_ring_count[1] > 0) && (r_dmac <= ring_hold);
    int left_has_audio = state->p25_p2_audio_allowed[0] || l_ring;
    int right_has_audio = state->p25_p2_audio_allowed[1] || r_ring;
    if (dt >= opts->trunk_hangtime) {
        left_has_audio = l_ring;
        right_has_audio = r_ring;
    }
    *left_active = left_has_audio || (l_dmac <= mac_hold);
    *right_active = right_has_audio || (r_dmac <= mac_hold);
}

static void
frame_sync_no_sync_try_p25_release(dsd_opts* opts, dsd_state* state, time_t now) {
    if (!(opts->p25_trunk == 1 && opts->p25_is_tuned == 1)) {
        return;
    }
    double fallback_nowm = dsd_time_now_monotonic_s();
    double dt = frame_sync_elapsed_seconds(fallback_nowm, now, state->last_vc_sync_time_m, state->last_vc_sync_time);
    double dt_since_tune =
        frame_sync_elapsed_seconds(fallback_nowm, now, state->p25_last_vc_tune_time_m, state->p25_last_vc_tune_time);

    const dsdneoRuntimeConfig* cfg_hold = dsd_neo_get_config();
    if (!cfg_hold) {
        dsd_neo_config_init(opts);
        cfg_hold = dsd_neo_get_config();
    }
    double vc_grace = cfg_hold ? cfg_hold->p25_vc_grace_s : 0.75;
    int is_p2_vc = (state->p25_p2_active_slot != -1);
    double ring_hold = cfg_hold ? cfg_hold->p25_ring_hold_s : 0.75;
    double mac_hold = cfg_hold ? cfg_hold->p25_mac_hold_s : 0.75;
    int left_active = 0;
    int right_active = 0;
    frame_sync_p25_slot_activity(opts, state, now, fallback_nowm, mac_hold, ring_hold, dt, &left_active, &right_active);
    int both_slots_idle = (!is_p2_vc) ? 1 : !(left_active || right_active);
    if (dt >= opts->trunk_hangtime && both_slots_idle && dt_since_tune >= vc_grace) {
        state->p25_sm_force_release = 1;
        dsd_frame_sync_hook_p25_sm_on_release(opts, state);
    }
}

static int
frame_sync_handle_no_sync_timeout(dsd_opts* opts, dsd_state* state, const frame_sync_runtime_ctx* rt, time_t now) {
    if (state->lastsynctype == DSD_SYNC_P25P1_NEG || rt->synctest_pos < 1800) {
        return 0;
    }

    if ((opts->errorbars == 1) && (opts->verbose > 1) && (state->carrier == 1)) {
        DSD_FPRINTF(stderr, "Sync: no sync\n");
    }

    frame_sync_no_sync_sps_hunt(opts, state);
    frame_sync_no_sync_try_p25_release(opts, state, now);
    dsd_frame_sync_hook_no_carrier(opts, state);
    return 1;
}

int
getFrameSync(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return -1;
    }

    const time_t now = time(NULL);
    const double nowm = dsd_time_now_monotonic_s();
    frame_sync_maybe_tick_p25_trunk_sm(opts, state, now);
    frame_sync_apply_cli_mod_lock(opts, state);

    frame_sync_runtime_ctx rt;
    frame_sync_runtime_init(&rt, opts, state);

    frame_sync_publish_ui_throttled(opts, state);
    watchdog_event_history(opts, state, 0);
    watchdog_event_current(opts, state, 0);
    watchdog_event_history(opts, state, 1);
    watchdog_event_current(opts, state, 1);

    for (;;) {
        rt.t++;
        if ((rt.t % 300) == 0) {
            frame_sync_publish_ui_throttled(opts, state);
        }

        rt.symbol = getSymbol(opts, state, 0);
        frame_sync_update_symbol_ring(opts, state, rt.symbol, rt.lbuf, &rt.lidx, rt.t_max);
        frame_sync_maybe_auto_switch_modulation(opts, state, rt.t_max, &rt.lastt);
        rt.dibit = frame_sync_process_dibit_and_payload(opts, state, rt.symbol);
        *rt.synctest_p = (char)('0' + (rt.dibit & 0x3));

        if (rt.t >= rt.t_max) {
            int sync_type = frame_sync_eval_window(opts, state, &rt, now, nowm);
            if (sync_type != DSD_SYNC_NONE) {
                return sync_type;
            }
        }

        if (exitflag == 1) {
            cleanupAndExit(opts, state);
            return DSD_SYNC_NONE;
        }

        frame_sync_advance_sync_window(opts, state, &rt);
        if (frame_sync_handle_no_sync_timeout(opts, state, &rt, now)) {
            return -1;
        }
    }
}
