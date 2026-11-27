// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Tap and plan source for tools/squelch_model.py: prints the channel-filter plans the demodulator really runs, and the
 * auto squelch plans full_demod() installs on them, as JSON lines.
 *
 * Not part of the build. tools/squelch_model.py compiles it against the static DSP, runtime, IQ and platform libraries
 * of a configured build tree (the link line of dsd-neo_test_dsp_channel_filters) and reads its output.
 *
 * The first line holds the half-band decimator taps and the analog channel constants. Then each stdin line
 * "<rate_hz> <width_hz> <kind>" (kind 0 = FM, 1 = AM) gets one JSON line with the plan full_demod() designs for an
 * analog-family monitor at that complex rate: the width-driven analog design for width_hz > 0, the legacy WIDE profile
 * plan (288-tap cap, 63-tap fallback) for width_hz 0. It also reports the validator's view of the width
 * (dsd_analog_width_realizable(), the tap count for the rate, the widest width the rate fits and the legacy WIDE
 * width) and whether dsd_channel_lpf_design_analog() called directly gives the same taps as the plan.
 *
 * Three more fields, "<path> <profile> <passes>", run that request on a whole chain without a step (issue #899): the
 * path (0 the analog monitor, 1 the M17 encoder's monitor, 2 the 9600 bit/s two-level FSK path EDACS analog voice
 * reads), the channel filter (-1 the analog family's design above, -2 the channel LPF off, or a dsd_ch_lpf_profile off
 * the family: PROVOICE, WIDE) and the half-band passes ahead of it at 2^passes times the rate. full_demod() runs a block
 * of that chain under AUTO, and the line also carries the auto squelch plan it installed (the decoder's own: the whole
 * cascade's for two passes or more) and whether the tracker ran. A seventh field, "<reader_source_hz>", reads that
 * source through the replay reader's resampler into the cascade (dsd_iq_reader_plan_for(), its squelch model built as
 * a rate plan's commit builds it), so the plan is the one behind the reader.
 *
 * "squelch_model_taps noise" takes the same chain requests and adds the NFM noise squelch's plan the decoder designs
 * for that chain (tools/noise_squelch_model.py): squelch_noise_design()'s calls on the channel taps and the half-band
 * stages, through the reader's model on a reader chain. The line's "noise_plan" holds its band and every band-pass's
 * calibration reference power (p_ref, the K sub-bands then the staggered set), so the tool scores its windows against
 * the references the decoder holds.
 *
 * "squelch_model_taps track" runs the auto squelch's tracker itself instead (tools/squelch_paths_model.py's timing
 * checks): a first stdin line "<margin_db> <rate_hz> <lag> <rho_lag> <beta> <neff> <sqrt_neff> <noise_gain>" sets the
 * plan (one this harness printed, or the tools' model of a chain the decoder's plan does not cover), then interleaved
 * float32 I/Q follows to EOF; stdout gets one flag byte per sample (DSD_SQUELCH_FLAG_CLOSED when the gate was closed),
 * from a tracker that starts LEARNING.
 */

#include <dsd-neo/core/parse.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/squelch_reader_model.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"

static void
print_taps(const float* taps, int len) {
    DSD_FPRINTF(stdout, "[");
    for (int i = 0; i < len; i++) {
        DSD_FPRINTF(stdout, "%s%.9g", i ? "," : "", (double)taps[i]);
    }
    DSD_FPRINTF(stdout, "]");
}

static void
release_demod(demod_state* s) {
    dsd_squelch_reader_model_release(&s->squelch_reader_model);
    dsd_neo_aligned_free(s->post_polydecim_taps);
    dsd_neo_aligned_free(s->post_polydecim_hist);
    s->post_polydecim_taps = NULL;
    s->post_polydecim_hist = NULL;
}

/* One block through full_demod() so the channel LPF designs its plan, as DSP_CHANNEL_FILTERS does: the analog
   family's. */
static int
design_plan(demod_state* s, int rate_hz, int kind, int width_hz) {
    release_demod(s);
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    /* The channel filter designs at the complex rate (issue #806); left 0, every plan would be the fallback. */
    s->complex_rate = rate_hz;
    s->rate_out2 = 0;
    s->mode_demod = &raw_demod;
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 1024;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->analog_family = 1;
    s->analog_demod = kind;
    s->channel_lpf_width_hz = width_hz;
    full_demod(s);
    return s->channel_lpf_plan_taps_len;
}

/* Blocks of silence through full_demod() on @p path with @p passes half-band passes and no step, under AUTO, until a
   block leaves channel samples: the channel filter designs its plan and the tracker installs its own. With
   @p reader_source_hz above 0 the chain reads that source through the replay reader's resampler into the cascade's
   2^passes x rate input, its model built as a rate plan's commit builds it. Returns the channel taps' length (0 with
   the channel LPF off), or -1 when the reader or its model cannot be built. */
static int
run_chain_plan(demod_state* s, int rate_hz, int kind, int width_hz, int path, int profile, int passes,
               long reader_source_hz) {
    release_demod(s);
    DSD_MEMSET(s, 0, sizeof(*s));
    s->rate_in = rate_hz;
    s->rate_out = rate_hz;
    s->complex_rate = rate_hz;
    s->post_downsample = 1;
    s->downsample_passes = passes;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->mode_demod = &dsd_fm_demod;
    s->channel_lpf_enable = profile == -2 ? 0 : 1;
    s->channel_lpf_profile = profile >= 0 ? profile : DSD_CH_LPF_PROFILE_WIDE;
    if (path == 0) {
        s->analog_family = 1;
        s->analog_demod = kind;
        s->mode_demod = kind == DSD_ANALOG_DEMOD_AM ? &dsd_am_demod : &dsd_fm_demod;
        s->channel_lpf_width_hz = profile == -1 ? width_hz : 0;
    } else if (path == 1) {
        s->m17_encoder_monitor = 1;
    } else {
        s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
        s->symbol_rate_hz = 9600;
        s->symbol_levels = 2;
    }
    s->squelch_mode = DSD_SQUELCH_MODE_AUTO;
    s->squelch_margin_db = 10;
    if (reader_source_hz > 0) {
        /* The cascade's input rate, 2^passes x the rate, in 64 bits: past the reader's INT_MAX limit it is refused
           here, never narrowed to a wrapped rate the reader would take as another chain. */
        const uint64_t dsp_in = (uint64_t)rate_hz << passes;
        if (dsp_in > (uint64_t)INT_MAX
            || dsd_iq_reader_plan_for(&s->iq_reader, (uint32_t)reader_source_hz, (uint32_t)dsp_in, (uint64_t)rate_hz)
                   != 0
            || dsd_demod_squelch_reader_model_sync(s) != 0) {
            return -1;
        }
    }
    const int pairs = passes >= 6 ? 16384 : (256 << passes);
    for (int i = 0; i < 8; i++) {
        DSD_MEMSET(s->input_cb_buf, 0, (size_t)pairs * 2U * sizeof(float));
        s->lowpassed = s->input_cb_buf;
        s->lp_len = pairs * 2;
        full_demod(s);
        if (!s->front_end_empty && s->lp_len > 0) {
            break;
        }
    }
    return s->channel_lpf_enable ? s->channel_lpf_plan_taps_len : 0;
}

static void
print_floor_plan(const dsd_squelch_floor_plan* fp, int installed) {
    DSD_FPRINTF(stdout,
                ",\"installed\":%d,\"floor_plan\":{\"valid\":%d,\"rate_hz\":%d,\"lag\":%d,\"rho_lag\":%.17g,"
                "\"beta\":%.17g,\"neff\":%.17g,\"sqrt_neff\":%.17g,\"noise_gain\":%.17g}",
                installed, fp->valid, fp->rate_hz, fp->lag, fp->rho_lag, fp->beta, fp->neff, fp->sqrt_neff,
                fp->noise_gain);
}

/* The noise squelch's plan the decoder designs for the chain run_chain_plan() just set up (squelch_noise_design() in
   src/dsp/demod_pipeline.cpp, no step): the channel taps (none with the LPF off), @p passes half-band stages (hb31
   first, hb15 after) and, on a reader chain, the reader's model; printed as the line's "noise_plan". */
static void
print_noise_plan(demod_state* s, int taps_len, int passes, int rate_hz) {
    dsd_noise_squelch_stage stages[DSD_NOISE_SQUELCH_MAX_STAGES];
    const int count = passes < DSD_NOISE_SQUELCH_MAX_STAGES ? passes : DSD_NOISE_SQUELCH_MAX_STAGES;
    for (int i = 0; i < count; i++) {
        stages[i].taps = i == 0 ? hb31_q15_taps : hb_q15_taps;
        stages[i].len = i == 0 ? 31 : HB_TAPS;
    }
    const float* taps = taps_len > 0 ? s->channel_lpf_plan_taps : NULL;
    dsd_noise_squelch_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));
    if (s->iq_reader.enabled) {
        (void)dsd_squelch_reader_model_noise_plan(&s->squelch_reader_model, &plan, taps, taps_len, stages, count,
                                                  rate_hz);
    } else {
        (void)dsd_noise_squelch_plan_design(&plan, taps, taps_len, stages, count, rate_hz);
    }
    DSD_FPRINTF(stdout,
                ",\"noise_plan\":{\"valid\":%d,\"rate_hz\":%d,\"edge_hz\":%.17g,\"lo_hz\":%.17g,\"hi_hz\":%.17g,"
                "\"sub_bands\":%d,\"bands\":%d,\"p_ref_sum\":%.17g,\"p_ref\":[",
                plan.valid, plan.rate_hz, plan.edge_hz, plan.lo_hz, plan.hi_hz, plan.sub_bands, plan.bands,
                plan.p_ref_sum);
    for (int k = 0; plan.valid && k < plan.bands; k++) {
        DSD_FPRINTF(stdout, "%s%.17g", k ? "," : "", plan.p_ref[k]);
    }
    DSD_FPRINTF(stdout, "]}");
}

/* The next number of a header, or 0 when there is none. */
static int
next_double(char** cur, double* out) {
    char* end = NULL;
    *out = strtod(*cur, &end);
    if (end == *cur) {
        return 0;
    }
    *cur = end;
    return 1;
}

/* The tracker over stdin's I/Q, one flag byte per sample to stdout. */
static int
run_tracker(void) {
    char header[1024];
    if (!fgets(header, sizeof header, stdin)) {
        return 1;
    }
    double v[8];
    char* cur = header;
    for (int i = 0; i < 8; i++) {
        if (!next_double(&cur, &v[i])) {
            DSD_FPRINTF(stderr, "bad track header\n");
            return 1;
        }
    }
    dsd_squelch_floor_plan plan;
    DSD_MEMSET(&plan, 0, sizeof plan);
    plan.rate_hz = (int)v[1];
    plan.lag = (int)v[2];
    plan.rho_lag = v[3];
    plan.beta = v[4];
    plan.neff = v[5];
    plan.sqrt_neff = v[6];
    plan.noise_gain = v[7];
    if (plan.rate_hz < 100 || plan.lag < 1 || plan.lag > DSD_SQUELCH_FLOOR_MAX_LAG || !(plan.sqrt_neff > 0.0)
        || !(plan.noise_gain > 0.0)) {
        DSD_FPRINTF(stderr, "plan not valid\n");
        return 1;
    }
    plan.valid = 1;
    dsd_squelch_floor* t = static_cast<dsd_squelch_floor*>(calloc(1, sizeof(dsd_squelch_floor)));
    if (!t) {
        return 1;
    }
    dsd_squelch_floor_set_plan(t, &plan);
    dsd_squelch_floor_set_margin(t, (int)v[0]);
    dsd_squelch_floor_reset(t);
    static float iq[2 * 8192];
    static uint8_t flags[8192];
    size_t got = 0;
    while ((got = fread(iq, 2 * sizeof(float), 8192, stdin)) > 0) {
        dsd_squelch_floor_process(t, iq, (int)got, flags);
        if (fwrite(flags, 1, got, stdout) != got) {
            free(t);
            return 1;
        }
    }
    free(t);
    return 0;
}

/* The request's optional reader field, the rest of the line after the passes: 0 (no reader) when it holds nothing but
   white space, else one whole source rate from 0 (no reader, as the paths model writes it) to 100,000,000 Hz with
   nothing after it (dsd_parse_long_strict()). Returns 0, or -1 for a field that does not read so ("oops", "8000oops",
   two fields). */
static int
parse_reader_field(const char* text, long* out) {
    *out = 0;
    while (*text == ' ' || *text == '\t') {
        text++;
    }
    char field[32];
    size_t n = 0U;
    while (text[n] != '\0' && text[n] != ' ' && text[n] != '\t' && text[n] != '\n' && text[n] != '\r') {
        if (n + 1U >= sizeof field) {
            return -1;
        }
        field[n] = text[n];
        n++;
    }
    field[n] = '\0';
    for (const char* rest = text + n; *rest != '\0'; rest++) {
        if (*rest != ' ' && *rest != '\t' && *rest != '\n' && *rest != '\r') {
            return -1;
        }
    }
    if (n == 0U) {
        return 0;
    }
    return dsd_parse_long_strict(field, 10, 0, 100000000L, out);
}

int
main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "track") == 0) {
        return run_tracker();
    }
    const int noise_mode = argc > 1 && strcmp(argv[1], "noise") == 0;
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (!s) {
        return 1;
    }
    DSD_MEMSET(s, 0, sizeof(*s));
    DSD_FPRINTF(stdout, "{\"header\":1,\"transition_hz\":%d,\"guard_hz\":%d,\"max_taps\":%d,\"hb15\":",
                DSD_ANALOG_CHANNEL_TRANSITION_HZ, DSD_ANALOG_CHANNEL_GUARD_HZ, DSD_ANALOG_CHANNEL_MAX_TAPS);
    print_taps(hb_q15_taps, HB_TAPS);
    DSD_FPRINTF(stdout, ",\"hb31\":");
    print_taps(hb31_q15_taps, 31);
    DSD_FPRINTF(stdout, "}\n");

    static float direct[DSD_ANALOG_CHANNEL_MAX_TAPS];
    char line[128];
    int rc = 0;
    while (fgets(line, sizeof line, stdin)) {
        char* end = NULL;
        const long rate = strtol(line, &end, 10);
        char* end2 = NULL;
        const long width = strtol(end, &end2, 10);
        char* end3 = NULL;
        const long kind = strtol(end2, &end3, 10);
        if (end == line || end2 == end || end3 == end2 || rate <= 0 || rate > 10000000 || width < 0 || width > 1000000
            || (kind != DSD_ANALOG_DEMOD_FM && kind != DSD_ANALOG_DEMOD_AM)) {
            DSD_FPRINTF(stderr, "bad request: %s", line);
            rc = 1;
            continue;
        }
        /* The optional chain fields (tools/squelch_paths_model.py, tools/squelch_model.py). */
        char* end4 = NULL;
        const long path = strtol(end3, &end4, 10);
        const int with_chain = end4 != end3 ? 1 : 0;
        long profile = -1;
        long passes = 0;
        long reader_source = 0;
        if (with_chain) {
            char* end5 = NULL;
            profile = strtol(end4, &end5, 10);
            char* end6 = NULL;
            passes = strtol(end5, &end6, 10);
            /* The optional reader field: the source rate the replay reader resamples into the cascade. Nothing but
               white space after the passes is no reader; anything else must be a whole rate. */
            const int reader_ok = parse_reader_field(end6, &reader_source) == 0;
            const int profile_ok = path == 0 ? (profile == -1 || profile == -2)
                                             : (profile == -2 || profile == DSD_CH_LPF_PROFILE_WIDE
                                                || profile == DSD_CH_LPF_PROFILE_PROVOICE);
            if (end5 == end4 || end6 == end5 || path < 0 || path > 2 || !profile_ok || passes < 0 || passes > 10
                || !reader_ok) {
                DSD_FPRINTF(stderr, "bad request: %s", line);
                rc = 1;
                continue;
            }
        }
        const int taps_len = with_chain ? run_chain_plan(s, (int)rate, (int)kind, (int)width, (int)path, (int)profile,
                                                         (int)passes, reader_source)
                                        : design_plan(s, (int)rate, (int)kind, (int)width);
        if (taps_len < 0) {
            DSD_FPRINTF(stderr, "no reader for request: %s", line);
            rc = 1;
            continue;
        }
        int direct_len = -1;
        int direct_same = 0;
        if (width > 0 && profile == -1) {
            direct_len = dsd_channel_lpf_design_analog((int)rate, (int)width, direct, DSD_ANALOG_CHANNEL_MAX_TAPS);
            direct_same = direct_len == taps_len ? 1 : 0;
            for (int i = 0; direct_same && i < taps_len; i++) {
                direct_same = direct[i] == s->channel_lpf_plan_taps[i] ? 1 : 0;
            }
        }
        DSD_FPRINTF(stdout,
                    "{\"rate_out\":%ld,\"width_hz\":%ld,\"kind\":%ld,\"realizable\":%d,\"taps_for_rate\":%d,"
                    "\"max_width_for_rate\":%d,\"legacy_wide_width_hz\":%d,\"direct_len\":%d,\"direct_same\":%d,"
                    "\"taps\":",
                    rate, width, kind, width > 0 ? dsd_analog_width_realizable((int)width, (int)rate) : 0,
                    dsd_analog_channel_taps_for_rate((int)rate), dsd_analog_width_max_for_rate((int)rate),
                    dsd_channel_lpf_legacy_wide_width_hz((int)rate), direct_len, direct_same);
        print_taps(s->channel_lpf_plan_taps, taps_len > 0 ? taps_len : 0);
        if (with_chain) {
            DSD_FPRINTF(stdout,
                        ",\"path\":%ld,\"profile\":%ld,\"passes\":%ld,\"reader_source_hz\":%ld,"
                        "\"protected_edge_hz\":%.1f",
                        path, profile, passes, reader_source,
                        profile >= 0 ? dsd_channel_lpf_protected_edge_hz((int)profile) : 0.0);
            print_floor_plan(&s->squelch_floor.plan, s->squelch_auto_ran);
            if (noise_mode) {
                print_noise_plan(s, taps_len, (int)passes, (int)rate);
            }
        }
        DSD_FPRINTF(stdout, "}\n");
    }
    release_demod(s);
    dsd_neo_aligned_free(s);
    return rc;
}
