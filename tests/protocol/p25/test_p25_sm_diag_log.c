// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Focused P25 state-machine diagnostic log coverage.
 *
 * Drives grant, release, and CC hunt decisions through the real SM with tuning
 * hooks so the log records can be checked without radio hardware.
 */

#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/trunk_cc_candidates.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

// NOLINTNEXTLINE(misc-use-internal-linkage)
void LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state);

// NOLINTNEXTLINE(misc-use-internal-linkage)
void
LFSRN(const char* buffer_in, char* buffer_out, dsd_state* state) {
    (void)buffer_in;
    (void)buffer_out;
    (void)state;
}

static dsd_trunk_tune_result
diag_tune_to_freq(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)ted_sps;
    if (opts) {
        opts->trunk_is_tuned = 1;
    }
    if (state) {
        state->p25_vc_freq[0] = freq;
        state->trunk_vc_freq[0] = freq;
        state->last_vc_sync_time = time(NULL);
        state->last_vc_sync_time_m = dsd_time_now_monotonic_s();
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
diag_return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    if (opts) {
        opts->trunk_is_tuned = 0;
    }
    if (state) {
        state->p25_vc_freq[0] = 0;
        state->trunk_vc_freq[0] = 0;
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
diag_tune_to_cc(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)request_id;
    (void)opts;
    (void)ted_sps;
    if (state) {
        state->trunk_cc_freq = freq;
        state->last_cc_sync_time = time(NULL);
        state->last_cc_sync_time_m = dsd_time_now_monotonic_s();
    }
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static void
install_diag_hooks(void) {
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.tune_to_freq_request = diag_tune_to_freq;
    hooks.return_to_cc_request = diag_return_to_cc;
    hooks.tune_to_cc_request = diag_tune_to_cc;
    dsd_trunk_tuning_hooks_set(hooks);
}

static void
seed_fdma_iden(dsd_state* state, int iden) {
    state->p25_iden_fdma[iden].base_freq = 851000000L / 5L;
    state->p25_iden_fdma[iden].chan_type = 1;
    state->p25_iden_fdma[iden].chan_spac = 100;
    state->p25_iden_fdma[iden].trust = 2;
    state->p25_iden_fdma[iden].populated = 1;
    state->p25_chan_tdma_explicit[iden] = 1;
}

static int
read_file(const char* path, char* out, size_t out_size) {
    if (!path || !out || out_size == 0) {
        return 1;
    }
    FILE* fp = fopen(path, "rb");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    size_t n = fread(out, 1, out_size - 1, fp);
    if (n == 0 && ferror(fp)) {
        DSD_FPRINTF(stderr, "fread(%s) failed: %s\n", path, strerror(errno));
        (void)fclose(fp);
        return 1;
    }
    out[n] = '\0';
    (void)fclose(fp);
    return 0;
}

static int
expect_contains(const char* output, const char* needle) {
    if (strstr(output, needle) != NULL) {
        return 0;
    }
    DSD_FPRINTF(stderr, "expected diagnostic log to contain \"%s\"\n--- log ---\n%s\n", needle, output);
    return 1;
}

int
main(void) {
    install_diag_hooks();

    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "dsdneo_p25_sm_diag");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 100;
    }
    (void)dsd_close(fd);

    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof opts);
    DSD_MEMSET(&state, 0, sizeof state);
    DSD_SNPRINTF(opts.p25_sm_log_file, sizeof opts.p25_sm_log_file, "%s", path);
    opts.p25_sm_log_file[sizeof opts.p25_sm_log_file - 1] = '\0';
    opts.trunk_enable = 1;
    opts.trunk_tune_group_calls = 1;
    opts.trunk_hangtime = 0.2f;
    state.p25_cc_freq = 851000000;
    state.trunk_cc_freq = 851000000;
    state.nac = 0x293;
    seed_fdma_iden(&state, 1);

    p25_sm_ctx_t ctx;
    p25_sm_init_ctx(&ctx, &opts, &state);
    p25_sm_event_t grant = p25_sm_ev_group_grant((1 << 12) | 10, 0, 1234, 5678, 0);
    p25_sm_event(&ctx, &opts, &state, &grant);
    p25_sm_release(&ctx, &opts, &state, "diag-release");

    static dsd_opts hunt_opts;
    static dsd_state hunt_state;
    DSD_MEMSET(&hunt_opts, 0, sizeof hunt_opts);
    DSD_MEMSET(&hunt_state, 0, sizeof hunt_state);
    DSD_SNPRINTF(hunt_opts.p25_sm_log_file, sizeof hunt_opts.p25_sm_log_file, "%s", path);
    hunt_opts.p25_sm_log_file[sizeof hunt_opts.p25_sm_log_file - 1] = '\0';
    hunt_opts.trunk_enable = 1;
    hunt_opts.p25_prefer_candidates = 1;
    hunt_state.p25_cc_freq = 851000000;
    hunt_state.trunk_cc_freq = 851000000;
    hunt_state.last_cc_sync_time_m = dsd_time_now_monotonic_s() - 10.0;
    (void)dsd_trunk_cc_candidates_add(&hunt_state, 852000000, 0, DSD_TRUNK_CC_CANDIDATE_CURRENT_SITE);
    p25_sm_ctx_t hunt_ctx;
    p25_sm_init_ctx(&hunt_ctx, &hunt_opts, &hunt_state);
    p25_sm_tick_ctx(&hunt_ctx, &hunt_opts, &hunt_state);

    dsd_p25_sm_log_close(&opts);
    dsd_p25_sm_log_close(&hunt_opts);

    char output[8192];
    int rc = read_file(path, output, sizeof output);
    (void)remove(path);
    if (rc != 0) {
        return rc;
    }

    rc |= expect_contains(output, "event=grant_freq");
    rc |= expect_contains(output, "source=iden-fdma");
    rc |= expect_contains(output, "event=grant_tune_result");
    rc |= expect_contains(output, "result=ok");
    rc |= expect_contains(output, "event=release_cc_result");
    rc |= expect_contains(output, "origin=return");
    rc |= expect_contains(output, "effective_grace=5.000");
    rc |= expect_contains(output, "event=cc_lost");
    rc |= expect_contains(output, "reason=timeout");
    rc |= expect_contains(output, "event=hunt_tune_attempt");
    rc |= expect_contains(output, "source=current-site-candidate");
    rc |= expect_contains(output, "origin=hunt-probe");
    rc |= expect_contains(output, "effective_grace=2.000");
    rc |= expect_contains(output, "freq=852000000");

    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    return rc;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
