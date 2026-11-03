// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The replay rate plan (issue #701): which chain a replay runs to reach the mode's DSP rate.
 *
 * - KEEP: every committed fixture sidecar keeps the chain it recorded, with or without TETRA's 18 kHz requirement where
 *   its rate meets it; a TETRA sidecar that misses it only through a power-of-two post_downsample folds it into the
 *   half-band chain.
 * - The general rule: a capture at D x 2^k decimates with no resampling, any other is resampled to the largest D x 2^k
 *   at or below its rate (or up to D), with the examples the plan names (48254 Hz to 72 kHz, 2.048 MS/s to 1.152 MS/s,
 *   2.4 MS/s to 1.536 MS/s, a 2.304 MS/s WAV at base 32), and an explicit request is met exactly.
 * - The refusals, with their reasons; the applied config's chain is consistent from its DSP input rate.
 * - The chunk rule: equal rates read exactly dsd_iq_replay_chunk_bytes(), a resampled capture's chunk resamples to
 *   no more than one live transfer.
 */

#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/digital_channel.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/iq_types.h"

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR must name tests/fixtures/iq"
#endif

static int
expect_u32(const char* label, uint32_t got, uint32_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%u want=%u\n", label, (unsigned)got, (unsigned)want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        return 1;
    }
    return 0;
}

/* The helpers build their config in this scratch copy; each is passed straight to one expect_*() call, which copies
   it before planning. */
static dsd_iq_replay_config g_cfg;

static const dsd_iq_replay_config*
sidecar_cfg(uint32_t rate_hz, uint32_t base, uint32_t post, int bw_khz) {
    dsd_iq_replay_config* cfg = &g_cfg;
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->container = DSD_IQ_REPLAY_CONTAINER_SIDECAR;
    cfg->format = DSD_IQ_FORMAT_CU8;
    cfg->sample_rate_hz = rate_hz;
    cfg->base_decimation = base;
    cfg->post_downsample = post;
    cfg->demod_rate_hz = rate_hz / base / post;
    cfg->rtl_dsp_bw_khz = bw_khz;
    cfg->recorded_dsp_bw_khz = bw_khz;
    return cfg;
}

static const dsd_iq_replay_config*
wav_cfg(uint32_t rate_hz) {
    dsd_iq_replay_config* cfg = &g_cfg;
    DSD_MEMSET(cfg, 0, sizeof(*cfg));
    cfg->container = DSD_IQ_REPLAY_CONTAINER_WAV;
    cfg->format = DSD_IQ_FORMAT_CF32;
    cfg->sample_rate_hz = rate_hz;
    cfg->base_decimation = 1U;
    cfg->post_downsample = 1U;
    cfg->demod_rate_hz = rate_hz;
    return cfg;
}

/* What every plan holds whatever chain it runs (issue #806): a KEEP plan never resamples and runs the planner's chain
   at its complex rate; any other runs no step, as forced by the plan. */
static int
expect_plan_identity(const char* label, const dsd_iq_replay_rate_plan* plan) {
    char sub[192];
    int rc = 0;
    DSD_SNPRINTF(sub, sizeof(sub), "%s: the planner's chain planned", label);
    rc |= expect_true(sub, plan->chain.status == 0 && plan->chain.step_out_hz == plan->complex_rate_hz);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: the chain's passes are the plan's", label);
    rc |= expect_true(sub, (1U << plan->chain.passes) == plan->base_decimation);
    if (plan->origin == DSD_IQ_REPLAY_ORIGIN_KEEP) {
        DSD_SNPRINTF(sub, sizeof(sub), "%s: KEEP never resamples", label);
        rc |= expect_true(sub, !plan->resample && plan->dsp_input_rate_hz == plan->capture_rate_hz);
        DSD_SNPRINTF(sub, sizeof(sub), "%s: KEEP takes the recorded or derived forced flag", label);
        rc |= expect_true(sub, plan->forced_provenance != DSD_IQ_REPLAY_FORCED_PLAN);
        return rc;
    }
    DSD_SNPRINTF(sub, sizeof(sub), "%s: a %s plan runs no step", label, dsd_iq_replay_origin_name(plan->origin));
    rc |= expect_true(sub, !plan->chain.step.enabled && plan->chain.step.L == 0 && plan->chain.step.M == 0);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: a %s plan runs as forced", label, dsd_iq_replay_origin_name(plan->origin));
    rc |= expect_true(sub, plan->forced == 1 && plan->forced_provenance == DSD_IQ_REPLAY_FORCED_PLAN);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: a %s plan's complex rate is its demod rate", label,
                 dsd_iq_replay_origin_name(plan->origin));
    rc |= expect_true(sub, plan->complex_rate_hz == plan->demod_rate_hz && plan->post_downsample == 1U);
    return rc;
}

/* The plan @p cfg gets for a mode whose channel filter is designed up to @p max_complex_hz (0: no ceiling) into
   @p out (may be NULL), and that applying it leaves a chain that is consistent from its DSP input rate. */
static int
expect_plan_in(const char* label, const dsd_iq_replay_config* cfg_in, uint32_t target_hz, uint32_t required_hz,
               int explicit_target, uint32_t max_complex_hz, uint32_t want_dsp_in, uint32_t want_base,
               uint32_t want_post, uint32_t want_demod, int want_kept, dsd_iq_replay_rate_plan* out) {
    dsd_iq_replay_config cfg = *cfg_in;
    dsd_iq_replay_rate_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));
    char err[256] = {0};
    const int prc =
        dsd_iq_replay_plan_rate(&cfg, target_hz, required_hz, explicit_target, max_complex_hz, &plan, err, sizeof(err));
    if (prc != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: %s: refused (%d): %s\n", label, prc, err);
        return 1;
    }
    if (out) {
        *out = plan;
    }
    char sub[160];
    int rc = expect_plan_identity(label, &plan);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: dsp input rate", label);
    rc |= expect_u32(sub, plan.dsp_input_rate_hz, want_dsp_in);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: base_decimation", label);
    rc |= expect_u32(sub, plan.base_decimation, want_base);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: post_downsample", label);
    rc |= expect_u32(sub, plan.post_downsample, want_post);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: demod rate", label);
    rc |= expect_u32(sub, plan.demod_rate_hz, want_demod);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: kept the recorded chain", label);
    rc |= expect_true(sub, plan.kept_recorded == want_kept);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: resample flag", label);
    rc |= expect_true(sub, plan.resample == (want_dsp_in != cfg.sample_rate_hz));
    DSD_SNPRINTF(sub, sizeof(sub), "%s: capture rate", label);
    rc |= expect_u32(sub, plan.capture_rate_hz, cfg.sample_rate_hz);

    const uint32_t capture_hz = cfg.sample_rate_hz;
    DSD_SNPRINTF(sub, sizeof(sub), "%s: apply", label);
    rc |= expect_true(sub, dsd_iq_replay_apply_rate_plan(&cfg, &plan) == DSD_IQ_OK);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: applied capture rate", label);
    rc |= expect_u32(sub, cfg.sample_rate_hz, capture_hz);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: applied dsp input rate", label);
    rc |= expect_u32(sub, dsd_iq_replay_dsp_input_rate_hz(&cfg), want_dsp_in);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: applied chain consistent", label);
    rc |= expect_true(sub, cfg.base_decimation != 0U && cfg.post_downsample != 0U
                               && (cfg.base_decimation & (cfg.base_decimation - 1U)) == 0U
                               && cfg.base_decimation <= DSD_IQ_REPLAY_MAX_BASE_DECIMATION
                               && dsd_iq_replay_dsp_input_rate_hz(&cfg) / cfg.base_decimation / cfg.post_downsample
                                      == cfg.demod_rate_hz);
    DSD_SNPRINTF(sub, sizeof(sub), "%s: the applied config carries the plan", label);
    rc |= expect_true(sub, cfg.plan_applied && dsd_iq_replay_rate_plan_equal(&cfg.plan, &plan));
    return rc;
}

/* expect_plan_in() for a mode whose channel filter puts no ceiling on the rate. */
static int
expect_plan(const char* label, const dsd_iq_replay_config* cfg_in, uint32_t target_hz, uint32_t required_hz,
            int explicit_target, uint32_t want_dsp_in, uint32_t want_base, uint32_t want_post, uint32_t want_demod,
            int want_kept) {
    return expect_plan_in(label, cfg_in, target_hz, required_hz, explicit_target, 0U, want_dsp_in, want_base, want_post,
                          want_demod, want_kept, NULL);
}

static int
expect_refused(const char* label, const dsd_iq_replay_config* cfg_in, uint32_t target_hz, uint32_t required_hz,
               int explicit_target, const char* want_reason) {
    dsd_iq_replay_config cfg = *cfg_in;
    dsd_iq_replay_rate_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));
    char err[256] = {0};
    const int prc = dsd_iq_replay_plan_rate(&cfg, target_hz, required_hz, explicit_target, 0U, &plan, err, sizeof(err));
    if (prc == DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: %s: planned dsp input %u Hz base %u, want a refusal\n", label,
                    (unsigned)plan.dsp_input_rate_hz, (unsigned)plan.base_decimation);
        return 1;
    }
    if (!strstr(err, want_reason)) {
        DSD_FPRINTF(stderr, "FAIL: %s: reason \"%s\" lacks \"%s\"\n", label, err, want_reason);
        return 1;
    }
    return 0;
}

/* ---------------- KEEP over the committed fixtures ---------------- */

typedef struct {
    int rc;
    int sidecars;
    int tetra_resampled;
    int digital_folded;
    int forced;
} fixture_walk;

/* A committed fixture whose capture rate its half-band chain does not take to its recorded bandwidth: the Airspy-rate
   captures (issue #806), which step. */
static int
fixture_is_forced(const dsd_iq_replay_config* cfg) {
    return (uint64_t)cfg->sample_rate_hz != (uint64_t)cfg->recorded_dsp_bw_khz * 1000U * cfg->base_decimation;
}

/* A forced fixture replays its live chain: its passes, then the step to its recorded bandwidth, from the recorded step
   fields or, for one written without them, the forced flag derived from its rates. */
static void
check_forced_fixture(const char* name, const dsd_iq_replay_config* cfg, fixture_walk* walk) {
    char label[512];
    const uint32_t bw_hz = (uint32_t)cfg->recorded_dsp_bw_khz * 1000U;
    dsd_iq_replay_rate_plan plan;
    DSD_SNPRINTF(label, sizeof(label), "forced fixture %s keeps its passes", name);
    walk->rc |= expect_plan_in(label, cfg, bw_hz, 0U, 0, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, cfg->sample_rate_hz,
                               cfg->base_decimation, 1U, cfg->sample_rate_hz / cfg->base_decimation,
                               cfg->rate_chain.has_step, &plan);
    DSD_SNPRINTF(label, sizeof(label), "forced fixture %s steps to its %u Hz bandwidth", name, (unsigned)bw_hz);
    walk->rc |= expect_true(label, plan.origin == DSD_IQ_REPLAY_ORIGIN_KEEP && plan.chain.step.enabled
                                       && plan.complex_rate_hz == bw_hz && plan.forced == 1);
    DSD_SNPRINTF(label, sizeof(label), "forced fixture %s: its forced flag is %s", name,
                 cfg->rate_chain.has_forced ? "recorded" : "derived");
    walk->rc |= expect_true(
        label, plan.forced_provenance
                   == (cfg->rate_chain.has_forced ? DSD_IQ_REPLAY_FORCED_SIDECAR : DSD_IQ_REPLAY_FORCED_DERIVED));
}

static int
has_suffix(const char* s, const char* suffix) {
    const size_t n = strlen(s);
    const size_t m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static int
check_fixture(const char* name, void* user) {
    fixture_walk* walk = (fixture_walk*)user;
    if (!has_suffix(name, ".json")) {
        return 0;
    }
    char path[4096];
    DSD_SNPRINTF(path, sizeof(path), "%s/%s", DSD_NEO_TEST_IQ_FIXTURE_DIR, name);
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    char err[256] = {0};
    if (dsd_iq_replay_read_metadata(path, &cfg, err, sizeof(err)) != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: fixture %s: %s\n", name, err);
        walk->rc = 1;
        return 0;
    }
    walk->sidecars++;
    const uint32_t base = cfg.base_decimation;
    const uint32_t post = cfg.post_downsample;
    const uint32_t demod = cfg.demod_rate_hz;
    const uint32_t rate = cfg.sample_rate_hz;
    const uint32_t bw_hz = (uint32_t)cfg.recorded_dsp_bw_khz * 1000U;
    char label[512];
    if (fixture_is_forced(&cfg)) {
        walk->forced++;
        check_forced_fixture(name, &cfg, walk);
        dsd_iq_replay_config_clear(&cfg);
        return 0;
    }
    /* No requirement: the recorded chain, whatever the target, unchanged (issue #806): no step, and not forced, so a
       default replay runs every committed capture as it did. */
    DSD_SNPRINTF(label, sizeof(label), "fixture %s keeps its chain", name);
    dsd_iq_replay_rate_plan plan;
    walk->rc |= expect_plan_in(label, &cfg, bw_hz, 0U, 0, 0U, rate, base, post, demod, 1, &plan);
    DSD_SNPRINTF(label, sizeof(label), "fixture %s: kept, unstepped, derived not forced at %u Hz", name,
                 (unsigned)(rate / base));
    walk->rc |= expect_true(label, plan.origin == DSD_IQ_REPLAY_ORIGIN_KEEP && !plan.chain.step.enabled
                                       && plan.forced == 0 && plan.forced_provenance == DSD_IQ_REPLAY_FORCED_DERIVED
                                       && plan.complex_rate_hz == rate / base);
    /* A digital mode filters only up to DSD_DIGITAL_CHANNEL_MAX_RATE_HZ: a capture above it folds one more half-band
       pass (the 144 kHz real-air TETRA captures run at 72 kHz under P25 or DMR); every other keeps its chain. */
    if (rate / base > DSD_DIGITAL_CHANNEL_MAX_RATE_HZ) {
        DSD_SNPRINTF(label, sizeof(label), "fixture %s folds a pass in a digital mode", name);
        walk->rc |= expect_plan_in(label, &cfg, bw_hz, 0U, 0, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, rate, base * 2U, post,
                                   rate / base / 2U / post, 0, &plan);
        walk->rc |= expect_true(label, plan.origin == DSD_IQ_REPLAY_ORIGIN_KEEP && !plan.chain.step.enabled);
        walk->digital_folded++;
    } else {
        DSD_SNPRINTF(label, sizeof(label), "fixture %s keeps its chain in a digital mode", name);
        walk->rc |= expect_plan_in(label, &cfg, bw_hz, 0U, 0, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, rate, base, post, demod,
                                   1, NULL);
    }
    DSD_SNPRINTF(label, sizeof(label), "fixture %s keeps its chain under a 72 kHz default", name);
    walk->rc |= expect_plan(label, &cfg, 72000U, 0U, 0, rate, base, post, demod, 1);
    /* TETRA's 18 kHz: kept where the CQPSK rate meets it. */
    if (strncmp(name, "tetra_", 6) == 0) {
        DSD_SNPRINTF(label, sizeof(label), "fixture %s under TETRA", name);
        if (post == 1U && ((rate / base) % 18000U) == 0U) {
            walk->rc |= expect_plan(label, &cfg, 72000U, 18000U, 0, rate, base, post, demod, 1);
        } else {
            /* The 50 kHz source capture: only resampling reaches a TETRA rate. */
            walk->rc |= expect_plan(label, &cfg, 72000U, 18000U, 0, 72000U, 1U, 1U, 72000U, 0);
            walk->tetra_resampled++;
        }
    }
    dsd_iq_replay_config_clear(&cfg);
    return 0;
}

static int
test_fixtures_keep(void) {
    fixture_walk walk = {0, 0, 0, 0, 0};
    if (dsd_dir_list(DSD_NEO_TEST_IQ_FIXTURE_DIR, check_fixture, &walk) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not list %s\n", DSD_NEO_TEST_IQ_FIXTURE_DIR);
        return 1;
    }
    int rc = walk.rc;
    rc |= expect_true("the fixture walk found the committed sidecars", walk.sidecars >= 50);
    rc |= expect_true("exactly one TETRA fixture (the 50 kHz source) needs resampling", walk.tetra_resampled == 1);
    rc |=
        expect_true("exactly the two 144 kHz real-air TETRA fixtures fold in a digital mode", walk.digital_folded == 2);
    rc |= expect_true("the forced-rate fixtures were found", walk.forced >= 4);
    return rc;
}

/* ---------------- Re-planned sidecars ---------------- */

static int
test_tetra_post_fold(void) {
    int rc = 0;
    /* 144 kHz, decimated by 2 after the demodulator: CQPSK runs at 144 kHz, a multiple of 18 kHz, but skips the post
       stage; folded into the half-band chain it runs at 72 kHz. */
    rc |= expect_plan("TETRA sidecar 144k base 1 post 2 folds post", sidecar_cfg(144000U, 1U, 2U, 72), 72000U, 18000U,
                      0, 144000U, 2U, 1U, 72000U, 0);
    /* The fold keeps the recorded demod rate: a 36 kHz target would have the general rule decimate by 4. */
    rc |= expect_plan("TETRA sidecar 144k post 2 folds whatever the target", sidecar_cfg(144000U, 1U, 2U, 72), 36000U,
                      18000U, 0, 144000U, 2U, 1U, 72000U, 0);
    /* TETRA runs 36..144 kHz only: a recorded chain at 18 kHz or at 162 kHz is not kept, direct or folded, and
       re-plans to 72 kHz. */
    rc |= expect_plan("TETRA sidecar 18k direct is not kept", sidecar_cfg(18000U, 1U, 1U, 18), 72000U, 18000U, 0,
                      72000U, 1U, 1U, 72000U, 0);
    rc |= expect_plan("TETRA sidecar 162k direct is not kept", sidecar_cfg(162000U, 1U, 1U, 162), 72000U, 18000U, 0,
                      144000U, 2U, 1U, 72000U, 0);
    rc |= expect_plan("TETRA sidecar 36k post 2 folded to 18k is not kept", sidecar_cfg(36000U, 1U, 2U, 36), 72000U,
                      18000U, 0, 72000U, 1U, 1U, 72000U, 0);
    rc |= expect_plan("TETRA sidecar 324k post 2 folded to 162k is not kept", sidecar_cfg(324000U, 1U, 2U, 162), 72000U,
                      18000U, 0, 288000U, 4U, 1U, 72000U, 0);
    /* Without TETRA the post-demod decimation is kept. */
    rc |= expect_plan("144k base 1 post 2 kept without TETRA", sidecar_cfg(144000U, 1U, 2U, 72), 72000U, 0U, 0, 144000U,
                      1U, 2U, 72000U, 1);
    /* 216 kHz with post 3 cannot fold (3 is no power of two): the general rule resamples it to the largest 72 kHz x
       2^k at or below 216 kHz, 144 kHz, and decimates by 2. */
    rc |= expect_plan("TETRA sidecar 216k post 3 takes the general rule", sidecar_cfg(216000U, 1U, 3U, 72), 72000U,
                      18000U, 0, 144000U, 2U, 1U, 72000U, 0);
    /* A 48 kHz sidecar under TETRA: up to 72 kHz. */
    rc |= expect_plan("48k cu8 sidecar under TETRA", sidecar_cfg(48000U, 1U, 1U, 48), 72000U, 18000U, 0, 72000U, 1U, 1U,
                      72000U, 0);
    /* An explicit request is met exactly, and skips KEEP. */
    rc |= expect_plan("48k sidecar asked for 24 kHz", sidecar_cfg(48000U, 1U, 1U, 48), 24000U, 0U, 1, 48000U, 2U, 1U,
                      24000U, 0);
    rc |= expect_plan("48k sidecar asked for its own 48 kHz", sidecar_cfg(48000U, 1U, 1U, 48), 48000U, 0U, 1, 48000U,
                      1U, 1U, 48000U, 0);
    return rc;
}

/* ---------------- The general rule ---------------- */

static int
test_general_rule_examples(void) {
    int rc = 0;
    rc |= expect_plan("48254 Hz WAV under TETRA", wav_cfg(48254U), 72000U, 18000U, 0, 72000U, 1U, 1U, 72000U, 0);
    rc |= expect_plan("48254 Hz WAV at 48 kHz", wav_cfg(48254U), 48000U, 0U, 0, 48000U, 1U, 1U, 48000U, 0);
    rc |= expect_plan("2.304 MS/s WAV under TETRA: no resampling, base 32", wav_cfg(2304000U), 72000U, 18000U, 0,
                      2304000U, 32U, 1U, 72000U, 0);
    rc |= expect_plan("2.048 MS/s under TETRA: 1.152 MS/s / 16", wav_cfg(2048000U), 72000U, 18000U, 0, 1152000U, 16U,
                      1U, 72000U, 0);
    rc |= expect_plan("2.4 MS/s at 48 kHz: 1.536 MS/s / 32", wav_cfg(2400000U), 48000U, 0U, 0, 1536000U, 32U, 1U,
                      48000U, 0);
    rc |= expect_plan("48 kHz WAV at 48 kHz", wav_cfg(48000U), 48000U, 0U, 0, 48000U, 1U, 1U, 48000U, 0);
    rc |= expect_plan("a WAV never keeps: 96 kHz at 48 kHz", wav_cfg(96000U), 48000U, 0U, 0, 96000U, 2U, 1U, 48000U, 0);
    /* The limits' edges. */
    rc |= expect_plan("8 kHz up to 72 kHz (9x)", wav_cfg(8000U), 72000U, 18000U, 0, 72000U, 1U, 1U, 72000U, 0);
    rc |= expect_plan("2^10 x 8 kHz decimates by 1024", wav_cfg(8192000U), 8000U, 0U, 0, 8192000U, 1024U, 1U, 8000U, 0);
    rc |= expect_plan("just under 2 x 2^10 x D", wav_cfg(16383999U), 8000U, 0U, 0, 8192000U, 1024U, 1U, 8000U, 0);
    return rc;
}

static int
test_refusals(void) {
    int rc = 0;
    rc |= expect_refused("a capture below 8 kHz", wav_cfg(7999U), 8000U, 0U, 0, "below");
    rc |= expect_refused("an upsample past 9x", wav_cfg(8000U), 80000U, 0U, 0, "upsample");
    rc |= expect_refused("2 x 2^10 x D decimates by 2 in the resampler", wav_cfg(16384000U), 8000U, 0U, 0, "too fast");
    rc |= expect_refused("a target of 0", wav_cfg(48000U), 0U, 0U, 1, "target");

    dsd_iq_replay_rate_plan plan;
    char err[64];
    rc |= expect_true("NULL config refused", dsd_iq_replay_plan_rate(NULL, 48000U, 0U, 0, 0U, &plan, err, sizeof(err))
                                                 == DSD_IQ_ERR_INVALID_ARG);
    dsd_iq_replay_config cfg = *wav_cfg(48000U);
    rc |= expect_true("NULL plan refused", dsd_iq_replay_plan_rate(&cfg, 48000U, 0U, 0, 0U, NULL, err, sizeof(err))
                                               == DSD_IQ_ERR_INVALID_ARG);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    rc |= expect_true("apply refuses an empty plan", dsd_iq_replay_apply_rate_plan(&cfg, &plan) != DSD_IQ_OK);
    rc |= expect_true("plan 48 kHz", dsd_iq_replay_plan_rate(&cfg, 48000U, 0U, 0, 0U, &plan, err, sizeof(err)) == 0);
    dsd_iq_replay_config other = *wav_cfg(96000U);
    rc |= expect_true("apply refuses a plan made for another capture rate",
                      dsd_iq_replay_apply_rate_plan(&other, &plan) == DSD_IQ_ERR_INVALID_ARG);
    return rc;
}

static int
test_dsp_input_rate(void) {
    int rc = 0;
    dsd_iq_replay_config cfg = *wav_cfg(48254U);
    rc |= expect_u32("dsp input rate 0 reads as the capture rate", dsd_iq_replay_dsp_input_rate_hz(&cfg), 48254U);
    cfg.dsp_input_rate_hz = 72000U;
    rc |= expect_u32("dsp input rate set", dsd_iq_replay_dsp_input_rate_hz(&cfg), 72000U);
    rc |= expect_u32("dsp input rate of NULL", dsd_iq_replay_dsp_input_rate_hz(NULL), 0U);
    return rc;
}

/* ---------------- Chunks ---------------- */

static int
test_chunk_bytes_for(void) {
    int rc = 0;
    const dsd_iq_sample_format formats[3] = {DSD_IQ_FORMAT_CU8, DSD_IQ_FORMAT_CS16, DSD_IQ_FORMAT_CF32};
    const uint32_t rates[7] = {8000U, 48000U, 48254U, 72000U, 1536000U, 2048000U, 2400000U};
    for (size_t f = 0; f < 3; f++) {
        for (size_t r = 0; r < 7; r++) {
            char label[96];
            DSD_SNPRINTF(label, sizeof(label), "equal rates read dsd_iq_replay_chunk_bytes (format %d, %u Hz)",
                         (int)formats[f], (unsigned)rates[r]);
            rc |= expect_true(label, dsd_iq_replay_chunk_bytes_for(formats[f], rates[r], rates[r])
                                         == dsd_iq_replay_chunk_bytes(formats[f], rates[r]));
        }
    }
    /* 48254 Hz to 72 kHz: the span rule, 257 samples (5.33 ms), resampling to at most 384. */
    rc |= expect_u32("48254 to 72000 cf32", (uint32_t)dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CF32, 48254U, 72000U),
                     257U * 8U);
    /* 2.048 MS/s to 1.152 MS/s: one live transfer, 8192 samples. */
    rc |= expect_u32("2.048M to 1.152M cu8",
                     (uint32_t)dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CU8, 2048000U, 1152000U), 8192U * 2U);
    /* Upsampling a fast capture: what it resamples to still fits one transfer, floor(8191 x Fs / R). */
    rc |= expect_u32("1.536M to 2M cs16",
                     (uint32_t)dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CS16, 1536000U, 2000000U), 6290U * 4U);
    rc |= expect_u32("a slow capture reads at least one sample",
                     (uint32_t)dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CU8, 100U, 72000U), 2U);
    rc |= expect_u32("no rate", (uint32_t)dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CU8, 48000U, 0U), 0U);
    rc |= expect_u32("no capture rate", (uint32_t)dsd_iq_replay_chunk_bytes_for(DSD_IQ_FORMAT_CU8, 0U, 48000U), 0U);
    return rc;
}

/* ---------------- Forced chains (issue #806) ---------------- */

/* A sidecar as the capture writer records a live chain: @p sidecar_cfg()'s, plus the nominal rate, the forced flag and
   the DSP rate after the step with its ratio (0/0: no step). */
static const dsd_iq_replay_config*
recorded_cfg(uint32_t rate_hz, uint32_t base, int bw_khz, uint32_t nominal_hz, int forced, uint32_t dsp_hz, uint32_t L,
             uint32_t M, double approx_ppm) {
    dsd_iq_replay_config* cfg = (dsd_iq_replay_config*)sidecar_cfg(rate_hz, base, 1U, bw_khz);
    cfg->rate_chain.has_nominal = 1;
    cfg->rate_chain.nominal_rate_hz = nominal_hz;
    cfg->rate_chain.has_forced = 1;
    cfg->rate_chain.device_forced = forced;
    cfg->rate_chain.has_step = 1;
    cfg->rate_chain.dsp_rate_hz = dsp_hz;
    cfg->rate_chain.step_l = L;
    cfg->rate_chain.step_m = M;
    cfg->rate_chain.step_approx_ppm = approx_ppm;
    return cfg;
}

/* @p plan steps hb_out = @p hb_num / 2^@p hb_shift Hz by @p L / @p M to @p out_hz, with @p cfg's passes. */
static int
expect_step(const char* label, const dsd_iq_replay_rate_plan* plan, int64_t hb_num, unsigned hb_shift, int L, int M,
            uint32_t out_hz) {
    const dsd_rate_plan* c = &plan->chain;
    if (plan->origin != DSD_IQ_REPLAY_ORIGIN_KEEP || !c->step.enabled || c->step.L != L || c->step.M != M
        || c->hb_out_num != hb_num || c->hb_out_shift != hb_shift || plan->complex_rate_hz != out_hz
        || c->backend != DSD_RATE_PLAN_BACKEND_REPLAY_KEEP || !c->device_forced) {
        DSD_FPRINTF(stderr,
                    "FAIL: %s: origin %d step %d %d/%d from %lld/2^%u Hz to %u Hz (backend %d forced %d), want %d/%d "
                    "from %lld/2^%u Hz to %u Hz\n",
                    label, (int)plan->origin, c->step.enabled, c->step.L, c->step.M, (long long)c->hb_out_num,
                    c->hb_out_shift, (unsigned)plan->complex_rate_hz, (int)c->backend, c->device_forced, L, M,
                    (long long)hb_num, hb_shift, (unsigned)out_hz);
        return 1;
    }
    return 0;
}

/* A forced chain a sidecar written before issue #806 recorded keeps its passes and runs the live step to its recorded
   bandwidth, its forced flag derived from the rates; the reader never resamples it. */
static int
test_forced_keep_steps(void) {
    static const struct {
        const char* name;
        int64_t hb_num;
        uint32_t rate_hz;
        uint32_t base;
        int bw_khz;
        unsigned hb_shift;
        int L;
        int M;
    } k_cases[] = {
        {"Airspy R2 2.5 MS/s base 32", 78125, 2500000U, 32U, 48, 0U, 384, 625},
        {"Airspy Mini 3 MS/s base 32", 93750, 3000000U, 32U, 48, 0U, 64, 125},
        {"312.5 kS/s base 4", 78125, 312500U, 4U, 48, 0U, 384, 625},
        {"2.5 MS/s base 64 at 24 kHz (39062.5 Hz)", 78125, 2500000U, 64U, 24, 1U, 384, 625},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        const uint32_t bw_hz = (uint32_t)k_cases[i].bw_khz * 1000U;
        const uint32_t legacy_demod = k_cases[i].rate_hz / k_cases[i].base;
        for (int digital = 0; digital <= 1; digital++) {
            char label[160];
            DSD_SNPRINTF(label, sizeof(label), "forced KEEP %s%s", k_cases[i].name, digital ? " (digital mode)" : "");
            dsd_iq_replay_rate_plan plan;
            const uint32_t max_hz = digital ? DSD_DIGITAL_CHANNEL_MAX_RATE_HZ : 0U;
            rc |= expect_plan_in(label, sidecar_cfg(k_cases[i].rate_hz, k_cases[i].base, 1U, k_cases[i].bw_khz), bw_hz,
                                 0U, 0, max_hz, k_cases[i].rate_hz, k_cases[i].base, 1U, legacy_demod, 0, &plan);
            rc |= expect_step(label, &plan, k_cases[i].hb_num, k_cases[i].hb_shift, k_cases[i].L, k_cases[i].M, bw_hz);
            rc |= expect_true(label, plan.forced == 1 && plan.forced_provenance == DSD_IQ_REPLAY_FORCED_DERIVED
                                         && plan.target_dsp_hz == bw_hz);
        }
    }
    return rc;
}

/* TETRA's requirement is met through the step: a forced chain whose half-band output reaches 72 kHz steps there, as a
   live Airspy at a 72 kHz bandwidth does, instead of the reader resampling the capture rate. One whose passes fall
   short (39062.5 Hz) is not kept and resamples, step off. */
static int
test_tetra_via_step(void) {
    int rc = 0;
    dsd_iq_replay_rate_plan plan;
    rc |= expect_plan_in("TETRA on a 2.5 MS/s base 32 bw 48 sidecar", sidecar_cfg(2500000U, 32U, 1U, 48), 72000U,
                         18000U, 0, 0U, 2500000U, 32U, 1U, 78125U, 0, &plan);
    rc |= expect_step("TETRA 2.5 MS/s base 32", &plan, 78125, 0U, 576, 625, 72000U);
    rc |= expect_plan_in("TETRA on a 3 MS/s base 32 sidecar", sidecar_cfg(3000000U, 32U, 1U, 48), 72000U, 18000U, 0, 0U,
                         3000000U, 32U, 1U, 93750U, 0, &plan);
    rc |= expect_step("TETRA 3 MS/s base 32", &plan, 93750, 0U, 96, 125, 72000U);
    rc |= expect_plan_in("TETRA on a 312.5 kS/s base 4 sidecar", sidecar_cfg(312500U, 4U, 1U, 72), 72000U, 18000U, 0,
                         0U, 312500U, 4U, 1U, 78125U, 0, &plan);
    rc |= expect_step("TETRA 312.5 kS/s base 4", &plan, 78125, 0U, 576, 625, 72000U);
    /* A recorded 48 kHz step does not run TETRA; the forced chain steps to 72 kHz instead. */
    rc |= expect_plan_in("TETRA on a recorded 48 kHz step",
                         recorded_cfg(2500000U, 32U, 48, 2500000U, 1, 48000U, 384U, 625U, 0.0), 72000U, 18000U, 0, 0U,
                         2500000U, 32U, 1U, 78125U, 0, &plan);
    rc |= expect_step("TETRA on a recorded 48 kHz step", &plan, 78125, 0U, 576, 625, 72000U);
    rc |= expect_true("TETRA on a recorded step: the recorded forced flag",
                      plan.forced_provenance == DSD_IQ_REPLAY_FORCED_SIDECAR);
    /* 39062.5 Hz after 6 passes is below 72 kHz, and KEEP keeps the recorded passes: the reader resamples 2.5 MS/s to
       2.304 MS/s, decimated by 32. */
    rc |= expect_plan_in("TETRA on a 2.5 MS/s base 64 sidecar", sidecar_cfg(2500000U, 64U, 1U, 24), 72000U, 18000U, 0,
                         0U, 2304000U, 32U, 1U, 72000U, 0, &plan);
    rc |= expect_true("TETRA 2.5 MS/s base 64: resampled", plan.origin == DSD_IQ_REPLAY_ORIGIN_RESAMPLED);
    return rc;
}

/* A sidecar that recorded its chain (issue #806) replays it: the step it recorded, and its own forced flag. */
static int
test_recorded_chain(void) {
    int rc = 0;
    dsd_iq_replay_rate_plan plan;
    rc |= expect_plan_in("recorded Airspy step", recorded_cfg(2500000U, 32U, 48, 2500000U, 1, 48000U, 384U, 625U, 0.0),
                         48000U, 0U, 0, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 2500000U, 32U, 1U, 78125U, 1, &plan);
    rc |= expect_step("recorded Airspy step", &plan, 78125, 0U, 384, 625, 48000U);
    rc |= expect_true("recorded Airspy step: recorded forced, kept",
                      plan.forced_provenance == DSD_IQ_REPLAY_FORCED_SIDECAR && plan.kept_recorded == 1
                          && plan.target_dsp_hz == 0U);
    /* The tolerance boundary: an RTL-SDR read back 1535999 Hz for 1536000. The nominal rate plans it: 5 passes to
       48000 Hz exactly, no step, not forced. The legacy demod rate still truncates the sample rate. */
    rc |= expect_plan_in("recorded RTL read-back 1535999",
                         recorded_cfg(1535999U, 32U, 48, 1536000U, 0, 48000U, 0U, 0U, 0.0), 48000U, 0U, 0,
                         DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 1535999U, 32U, 1U, 47999U, 1, &plan);
    rc |= expect_true("recorded RTL read-back 1535999: 48000 Hz, no step, not forced",
                      plan.complex_rate_hz == 48000U && !plan.chain.step.enabled && plan.forced == 0
                          && plan.chain.nominal_hz == 1536000U && plan.chain.passes == 5);
    /* Without the nominal rate (a sidecar from before it) the nominal rate is the sample rate, whose half-band output
       is not whole: a chain not forced is not kept with a fractional rate, so the general rule resamples it to the
       largest 48 kHz x 2^k at or below it, 768 kHz, decimated by 16. */
    rc |= expect_plan_in("legacy RTL read-back 1535999", sidecar_cfg(1535999U, 32U, 1U, 48), 48000U, 0U, 0, 0U, 768000U,
                         16U, 1U, 48000U, 0, &plan);
    rc |= expect_true("legacy RTL read-back 1535999: resampled", plan.origin == DSD_IQ_REPLAY_ORIGIN_RESAMPLED);
    /* A device that picked a rate the half-band chain takes exactly: forced, but nothing to step. */
    rc |= expect_plan_in("recorded forced, no step", recorded_cfg(3072000U, 64U, 48, 3072000U, 1, 48000U, 0U, 0U, 0.0),
                         48000U, 0U, 0, 0U, 3072000U, 64U, 1U, 48000U, 1, &plan);
    rc |= expect_true("recorded forced, no step", plan.forced == 1 && !plan.chain.step.enabled
                                                      && plan.forced_provenance == DSD_IQ_REPLAY_FORCED_SIDECAR);
    /* The approximation case: a SoapySDR device at 2000001 Hz steps 96/125, 0.5 ppm from 48 kHz. */
    rc |= expect_plan_in("recorded approximated step",
                         recorded_cfg(2000001U, 32U, 48, 2000001U, 1, 48000U, 96U, 125U, 0.5), 48000U, 0U, 0, 0U,
                         2000001U, 32U, 1U, 62500U, 1, &plan);
    rc |= expect_step("recorded approximated step", &plan, 2000001, 5U, 96, 125, 48000U);
    rc |= expect_true("recorded approximated step: 0.5 ppm", fabs(plan.chain.step.approx_ppm - 0.5) <= 1e-6);
    return rc;
}

/* Never twice: only a KEEP plan steps. A bandwidth asked for, a capture at D x 2^k and a WAV run the reader's plan with
   the step off, whether or not the reader resamples. */
static int
test_never_twice(void) {
    int rc = 0;
    dsd_iq_replay_rate_plan plan;
    rc |= expect_plan_in("explicit 48 kHz on a forced 2.5 MS/s sidecar", sidecar_cfg(2500000U, 32U, 1U, 48), 48000U, 0U,
                         1, 0U, 1536000U, 32U, 1U, 48000U, 0, &plan);
    rc |= expect_true("explicit on a forced sidecar: resampled", plan.origin == DSD_IQ_REPLAY_ORIGIN_RESAMPLED);
    rc |= expect_plan_in("explicit 24 kHz on a recorded step",
                         recorded_cfg(2500000U, 32U, 48, 2500000U, 1, 48000U, 384U, 625U, 0.0), 24000U, 0U, 1, 0U,
                         1536000U, 64U, 1U, 24000U, 0, &plan);
    rc |= expect_true("explicit on a recorded step: resampled", plan.origin == DSD_IQ_REPLAY_ORIGIN_RESAMPLED);
    rc |= expect_plan_in("explicit 48 kHz on a 1.536 MS/s sidecar", sidecar_cfg(1536000U, 32U, 1U, 24), 48000U, 0U, 1,
                         0U, 1536000U, 32U, 1U, 48000U, 0, &plan);
    rc |= expect_true("explicit at D x 2^k: decimated", plan.origin == DSD_IQ_REPLAY_ORIGIN_EXACT);
    rc |= expect_plan_in("a 2.5 MS/s WAV", wav_cfg(2500000U), 48000U, 0U, 0, 0U, 1536000U, 32U, 1U, 48000U, 0, &plan);
    rc |= expect_true("a WAV: WAV origin", plan.origin == DSD_IQ_REPLAY_ORIGIN_WAV);
    rc |= expect_plan_in("a 1.536 MS/s WAV", wav_cfg(1536000U), 48000U, 0U, 0, 0U, 1536000U, 32U, 1U, 48000U, 0, &plan);
    rc |= expect_true("a WAV at D x 2^k: WAV origin", plan.origin == DSD_IQ_REPLAY_ORIGIN_WAV);
    return rc;
}

/* A-B4: a sidecar that recorded no forced flag is forced only when its sample rate misses the RTL-shaped request of its
   whole chain. An RTL-SDR at 24 kHz is not forced (FSK then runs digital_resample=auto as live did); post decimation
   alone never makes a chain forced. */
static int
test_derived_forced(void) {
    int rc = 0;
    dsd_iq_replay_rate_plan plan;
    rc |= expect_plan_in("RTL 24 kHz sidecar", sidecar_cfg(1536000U, 64U, 1U, 24), 24000U, 0U, 0, 0U, 1536000U, 64U, 1U,
                         24000U, 1, &plan);
    rc |= expect_true("RTL 24 kHz sidecar: not forced, derived",
                      plan.forced == 0 && plan.forced_provenance == DSD_IQ_REPLAY_FORCED_DERIVED);
    rc |= expect_plan_in("144k base 1 post 2 bw 72", sidecar_cfg(144000U, 1U, 2U, 72), 72000U, 0U, 0, 0U, 144000U, 1U,
                         2U, 72000U, 1, &plan);
    rc |= expect_true("144k post 2: post decimation is not forcing", plan.forced == 0 && !plan.chain.step.enabled);
    rc |= expect_true("derived: a 1 Hz read-back is in tolerance",
                      dsd_iq_rate_chain_derived_forced(1535999U, 48, 32U, 1U) == 0);
    rc |= expect_true("derived: 2.5 MS/s at 48 kHz is forced", dsd_iq_rate_chain_derived_forced(2500000U, 48, 32U, 1U));
    rc |= expect_true("derived: 101 ppm off is forced", dsd_iq_rate_chain_derived_forced(1536156U, 48, 32U, 1U));
    rc |= expect_true("derived: no bandwidth recorded", dsd_iq_rate_chain_derived_forced(2500000U, 0, 32U, 1U) == 0);
    return rc;
}

/* The filter domain (§13.8): a digital mode keeps a 144 kHz capture by folding a pass to 72 kHz; TETRA's own filter
   runs at 144 kHz, and the mode round trip comes back to the same chain. A TETRA chain its filter would not run on (an
   incompatible TED override, the channel filter off) takes the digital ceiling and folds too. */
static int
test_filter_domain(void) {
    int rc = 0;
    dsd_iq_replay_rate_plan tetra;
    dsd_iq_replay_rate_plan p25;
    dsd_iq_replay_rate_plan back;
    const dsd_iq_replay_config* cc144 = sidecar_cfg(144000U, 1U, 1U, 144);
    rc |= expect_plan_in("144 kHz TETRA sidecar under TETRA", cc144, 144000U, 18000U, 0, 0U, 144000U, 1U, 1U, 144000U,
                         1, &tetra);
    rc |= expect_plan_in("144 kHz TETRA sidecar under P25", sidecar_cfg(144000U, 1U, 1U, 144), 144000U, 0U, 0,
                         DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 144000U, 2U, 1U, 72000U, 0, &p25);
    rc |= expect_plan_in("144 kHz TETRA sidecar under an incompatible TED override", sidecar_cfg(144000U, 1U, 1U, 144),
                         144000U, 18000U, 0, DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 144000U, 2U, 1U, 72000U, 0, NULL);
    rc |= expect_plan_in("back to TETRA", sidecar_cfg(144000U, 1U, 1U, 144), 144000U, 18000U, 0, 0U, 144000U, 1U, 1U,
                         144000U, 1, &back);
    rc |= expect_true("the round trip comes back to the TETRA chain", dsd_iq_replay_rate_plan_equal(&tetra, &back));
    rc |= expect_true("P25 runs another chain", !dsd_iq_replay_rate_plan_equal(&tetra, &p25));
    rc |= expect_plan_in("144 kHz base 2 under DMR", sidecar_cfg(144000U, 2U, 1U, 72), 72000U, 0U, 0,
                         DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 144000U, 2U, 1U, 72000U, 1, NULL);
    /* The analog post-decimated sidecar in a digital mode: a pass is added, post kept (FSK and CQPSK skip it). */
    rc |= expect_plan_in("144k base 1 post 2 under DMR", sidecar_cfg(144000U, 1U, 2U, 72), 72000U, 0U, 0,
                         DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 144000U, 2U, 2U, 36000U, 0, &p25);
    rc |= expect_true("144k post 2 under DMR: 72 kHz complex", p25.complex_rate_hz == 72000U);
    /* 288 kHz folds two passes. */
    rc |= expect_plan_in("288 kHz under P25", sidecar_cfg(288000U, 1U, 1U, 288), 288000U, 0U, 0,
                         DSD_DIGITAL_CHANNEL_MAX_RATE_HZ, 288000U, 4U, 1U, 72000U, 0, NULL);
    return rc;
}

/* Plan identity covers origin, step and forced flag: plans that differ only there are different chains. */
static int
test_plan_identity(void) {
    int rc = 0;
    dsd_iq_replay_rate_plan a;
    dsd_iq_replay_rate_plan b;
    rc |= expect_plan_in("identity: derived", sidecar_cfg(2500000U, 32U, 1U, 48), 48000U, 0U, 0, 0U, 2500000U, 32U, 1U,
                         78125U, 0, &a);
    rc |= expect_plan_in("identity: recorded", recorded_cfg(2500000U, 32U, 48, 2500000U, 1, 48000U, 384U, 625U, 0.0),
                         48000U, 0U, 0, 0U, 2500000U, 32U, 1U, 78125U, 1, &b);
    rc |= expect_true("the same step, recorded or derived, is another plan", !dsd_iq_replay_rate_plan_equal(&a, &b));
    b = a;
    rc |= expect_true("a plan equals itself", dsd_iq_replay_rate_plan_equal(&a, &b));
    b.chain.step.L = 0;
    rc |= expect_true("a step L differs", !dsd_iq_replay_rate_plan_equal(&a, &b));
    b = a;
    b.forced = 0;
    rc |= expect_true("a forced flag differs", !dsd_iq_replay_rate_plan_equal(&a, &b));
    b = a;
    b.origin = DSD_IQ_REPLAY_ORIGIN_EXACT;
    rc |= expect_true("an origin differs", !dsd_iq_replay_rate_plan_equal(&a, &b));
    b = a;
    DSD_SNPRINTF(b.chain.message, sizeof(b.chain.message), "%s", "a message is no part of it");
    rc |= expect_true("a message is no part of the identity", dsd_iq_replay_rate_plan_equal(&a, &b));
    rc |= expect_true("NULL is no plan", !dsd_iq_replay_rate_plan_equal(&a, NULL));
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_fixtures_keep();
    rc |= test_tetra_post_fold();
    rc |= test_general_rule_examples();
    rc |= test_refusals();
    rc |= test_dsp_input_rate();
    rc |= test_chunk_bytes_for();
    rc |= test_forced_keep_steps();
    rc |= test_tetra_via_step();
    rc |= test_recorded_chain();
    rc |= test_never_twice();
    rc |= test_derived_forced();
    rc |= test_filter_domain();
    rc |= test_plan_identity();
    if (rc == 0) {
        printf("IO_IQ_REPLAY_RATE_PLAN: OK\n");
    }
    return rc;
}
