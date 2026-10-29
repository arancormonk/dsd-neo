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
#include <dsd-neo/platform/file_compat.h>
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

/* The plan @p cfg gets, and that applying it leaves a chain that is consistent from its DSP input rate. */
static int
expect_plan(const char* label, const dsd_iq_replay_config* cfg_in, uint32_t target_hz, uint32_t required_hz,
            int explicit_target, uint32_t want_dsp_in, uint32_t want_base, uint32_t want_post, uint32_t want_demod,
            int want_kept) {
    dsd_iq_replay_config cfg = *cfg_in;
    dsd_iq_replay_rate_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));
    char err[256] = {0};
    const int prc = dsd_iq_replay_plan_rate(&cfg, target_hz, required_hz, explicit_target, &plan, err, sizeof(err));
    if (prc != DSD_IQ_OK) {
        DSD_FPRINTF(stderr, "FAIL: %s: refused (%d): %s\n", label, prc, err);
        return 1;
    }
    char sub[160];
    int rc = 0;
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
    return rc;
}

static int
expect_refused(const char* label, const dsd_iq_replay_config* cfg_in, uint32_t target_hz, uint32_t required_hz,
               int explicit_target, const char* want_reason) {
    dsd_iq_replay_config cfg = *cfg_in;
    dsd_iq_replay_rate_plan plan;
    DSD_MEMSET(&plan, 0, sizeof(plan));
    char err[256] = {0};
    const int prc = dsd_iq_replay_plan_rate(&cfg, target_hz, required_hz, explicit_target, &plan, err, sizeof(err));
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
} fixture_walk;

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
    /* No requirement: the recorded chain, whatever the target. */
    DSD_SNPRINTF(label, sizeof(label), "fixture %s keeps its chain", name);
    walk->rc |= expect_plan(label, &cfg, bw_hz, 0U, 0, rate, base, post, demod, 1);
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
    fixture_walk walk = {0, 0, 0};
    if (dsd_dir_list(DSD_NEO_TEST_IQ_FIXTURE_DIR, check_fixture, &walk) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not list %s\n", DSD_NEO_TEST_IQ_FIXTURE_DIR);
        return 1;
    }
    int rc = walk.rc;
    rc |= expect_true("the fixture walk found the committed sidecars", walk.sidecars >= 50);
    rc |= expect_true("exactly one TETRA fixture (the 50 kHz source) needs resampling", walk.tetra_resampled == 1);
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
    rc |= expect_true("NULL config refused",
                      dsd_iq_replay_plan_rate(NULL, 48000U, 0U, 0, &plan, err, sizeof(err)) == DSD_IQ_ERR_INVALID_ARG);
    dsd_iq_replay_config cfg = *wav_cfg(48000U);
    rc |= expect_true("NULL plan refused",
                      dsd_iq_replay_plan_rate(&cfg, 48000U, 0U, 0, NULL, err, sizeof(err)) == DSD_IQ_ERR_INVALID_ARG);
    DSD_MEMSET(&plan, 0, sizeof(plan));
    rc |= expect_true("apply refuses an empty plan", dsd_iq_replay_apply_rate_plan(&cfg, &plan) != DSD_IQ_OK);
    rc |= expect_true("plan 48 kHz", dsd_iq_replay_plan_rate(&cfg, 48000U, 0U, 0, &plan, err, sizeof(err)) == 0);
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

int
main(void) {
    int rc = 0;
    rc |= test_fixtures_keep();
    rc |= test_tetra_post_fold();
    rc |= test_general_rule_examples();
    rc |= test_refusals();
    rc |= test_dsp_input_rate();
    rc |= test_chunk_bytes_for();
    if (rc == 0) {
        printf("IO_IQ_REPLAY_RATE_PLAN: OK\n");
    }
    return rc;
}
