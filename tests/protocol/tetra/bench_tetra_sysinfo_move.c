// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Opt-in microbenchmark for what a TETRA trunking session does per burst for the move to the main carrier: a burst
 * without SYSINFO (the end-of-burst flag read alone), and a burst whose SYSINFO goes through the real MAC on the main
 * carrier, or off it with the move already made, the tuner busy, or the backend deferring or failing every move (the
 * decode clock advancing one timeslot per burst, so the hold and the backoff expire and moves are asked for again).
 * Every SYSINFO prints its report line, as in a live session: run with stderr to /dev/null.
 *
 * Built with -DDSD_BENCH_BASE it measures the same bursts on a tree without the move (no end-of-burst call).
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd-neo/platform/sockets.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_pdu.h"

enum { SECONDARY_HZ = 401268750, MAIN_HZ = 401243750 };

/* One TETRA timeslot (EN 300 392-2 clause 9.3): 85/6 ms. */
#define BENCH_SLOT_NS 14166667ULL

typedef enum { CASE_PLAIN, CASE_MAIN, CASE_CONSUMED, CASE_BUSY, CASE_DEFERRED, CASE_FAILED } bench_case_kind;

typedef struct {
    const char* name;
    bench_case_kind kind;
} bench_case;

static const bench_case k_cases[] = {
    {"plain", CASE_PLAIN},
    {"sysinfo_main", CASE_MAIN},
    {"sysinfo_off_consumed", CASE_CONSUMED},
    {"sysinfo_off_busy", CASE_BUSY},
    {"sysinfo_off_deferred", CASE_DEFERRED},
    {"sysinfo_off_failed", CASE_FAILED},
};

static dsd_trunk_tune_result g_return_result;
static int g_tuner_busy;
static uint64_t g_returns;

static dsd_trunk_tune_result
bench_return(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)opts;
    (void)state;
    (void)request_id;
    g_returns++;
    return g_return_result;
}

#ifndef DSD_BENCH_BASE
static int
bench_tuner_free(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return !g_tuner_busy;
}
#endif

static int
parse_count(const char* value, int fallback) {
    char* end = NULL;
    const long parsed = value ? strtol(value, &end, 10) : 0;
    return (value && end != value && *end == '\0' && parsed > 0 && parsed <= 100000000L) ? (int)parsed : fallback;
}

/* A fresh trunking session (@p opts and @p state zeroed) on a live RTL stream whose carrier a CRC-passed block proved,
 * set up for @p kind. */
static void
bench_open(dsd_opts* opts, dsd_state* state, bench_case_kind kind) {
    opts->trunk_enable = 1;
    opts->frame_tetra = 1;
    opts->audio_in_type = AUDIO_IN_RTL;
    opts->rigctl_sockfd = DSD_INVALID_SOCKET;
    opts->rtlsdr_center_freq = kind == CASE_MAIN ? MAIN_HZ : SECONDARY_HZ;
    (void)tetra_st(state);
    tetra_confirm_note_signalling(state);
    g_tuner_busy = kind == CASE_BUSY;
    g_return_result = kind == CASE_DEFERRED ? DSD_TRUNK_TUNE_RESULT_DEFERRED : DSD_TRUNK_TUNE_RESULT_FAILED;
#ifndef DSD_BENCH_BASE
    if (kind == CASE_CONSUMED) {
        tetra_state_t* const ts = tetra_st(state);
        ts->sm.acq.moves[0].hz = MAIN_HZ;
        ts->sm.acq.moves[0].consumed = 1;
    }
#endif
}

/* One burst as tetra.c takes it: its block decoded (SYSINFO through the MAC, or none), noted as evidence, the burst
 * closed, and the staged move run when one is staged. */
static void
bench_burst(dsd_opts* opts, dsd_state* state, const ttb_block* sysinfo) {
    tetra_confirm_burst_begin(state);
    if (sysinfo) {
        tetra_mac_parse_schd(sysinfo->bits, sysinfo->n, 0, opts, state);
    }
    tetra_confirm_note_signalling(state);
    (void)tetra_confirm_burst_end(state);
#ifndef DSD_BENCH_BASE
    const tetra_state_t* const ts = tetra_st_peek(state);
    if (ts->sm.acq.staged_hz != 0) {
        tetra_sm_after_burst(opts, state);
    }
#endif
}

static double
bench_run(bench_case_kind kind, int bursts) {
    dsd_opts* opts = (dsd_opts*)calloc(1U, sizeof *opts);
    dsd_state* state = (dsd_state*)calloc(1U, sizeof *state);
    if (!opts || !state) {
        free(opts);
        free(state);
        return -1.0;
    }
    bench_open(opts, state, kind);
    ttb_block sysinfo = {0};
    ttb_sysinfo(&sysinfo, 50U, 4U, 2U, 987U);
    const ttb_block* block = kind == CASE_PLAIN ? NULL : &sysinfo;
    uint64_t now_ns = 1000ULL * 1000000000ULL;
    dsd_decode_clock_test_set_ns(now_ns);
    const uint64_t start = dsd_realtime_mono_ns();
    for (int i = 0; i < bursts; i++) {
        now_ns += BENCH_SLOT_NS;
        dsd_decode_clock_test_set_ns(now_ns);
        bench_burst(opts, state, block);
    }
    const uint64_t elapsed = dsd_realtime_mono_ns() - start;
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return (double)elapsed / (double)bursts;
}

static int
compare_doubles(const void* a, const void* b) {
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

int
main(int argc, char** argv) {
    int bursts = 200000;
    int repeat = 5;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--bursts") == 0) {
            bursts = parse_count(argv[i + 1], bursts);
        } else if (strcmp(argv[i], "--repeat") == 0) {
            repeat = parse_count(argv[i + 1], repeat);
        }
    }
    if (repeat > 64) {
        repeat = 64;
    }
    dsd_decode_clock_use_test(1000ULL * 1000000000ULL);
    dsd_trunk_tuning_hooks hooks = {0};
    hooks.return_to_cc_request = bench_return;
#ifndef DSD_BENCH_BASE
    hooks.tuner_free_for_protocol = bench_tuner_free;
#endif
    dsd_trunk_tuning_hooks_set(hooks);
    for (size_t c = 0; c < sizeof k_cases / sizeof k_cases[0]; c++) {
        double runs[64];
        g_returns = 0U;
        for (int r = 0; r < repeat; r++) {
            runs[r] = bench_run(k_cases[c].kind, bursts);
        }
        qsort(runs, (size_t)repeat, sizeof runs[0], compare_doubles);
        DSD_FPRINTF(stdout, "%-22s %9.1f ns/burst (median of %d x %d bursts; %llu move requests)\n", k_cases[c].name,
                    runs[repeat / 2], repeat, bursts, (unsigned long long)g_returns);
    }
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_decode_clock_use_system();
    return 0;
}
