// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief An I/Q replay run through the engine decodes on its capture's clock (issue #572).
 *
 * The real engine opens committed fixtures the way the CLI does, in the CLI's initialization order: initOpts() and
 * initState() stamp their "now" seeds from the system clock first, then the run puts the decode clock on the capture's
 * (the sidecar's capture_started_utc), before common setup writes its first record. The lifecycle start hook looks at
 * the decoder's state once the stream has opened, before the decoder reads a sample (media time 0), and ends the run.
 *
 * - A capture older than the session: no init seed is ahead of the capture's now, so the elapsed times the DMR Cap+
 *   grant check and the NXDN recent-context check take from last_vc_sync_time are not negative.
 * - A `-mq --p25-sm-log` replay without `-T`: the P25 SM log's initial event=init record carries the capture's time.
 * - A 1970-anchored fixture: decode time starts at the 2000-01-01Z floor, clear of the "0 = never" sentinels, so a P25
 *   patch stamped in the first second expires after its TTL and the DMR single-fragment SLCO print is throttled to
 *   one per second from the first second on.
 * - Once the run ends, the decode clock is the system's again, with the init stamps rebased onto it.
 * - A second run on the state the first one used, as the Android service makes when a start races the previous run's
 *   stopSelfLatest(), stays on the system clock and says so once: a stamp the earlier run left on the system clock
 *   (a DMR single-fragment SLCO's) is never ahead of the run's now, so that line is not held back.
 */

#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/time_format.h>
#include <dsd-neo/engine/engine.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR must name tests/fixtures/iq"
#endif

/* p25p1_cqpsk_cc.iq.json: capture_started_utc 2026-07-30T00:00:00Z. */
static const time_t kCaptureAnchorS = 1785369600;
/* 2000-01-01T00:00:00Z, where a 1970-anchored capture's decode time starts. */
static const time_t kFloorS = 946684800;
#define NS_PER_S 1000000000ULL

static int g_failures = 0;

static void
check(const char* label, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        g_failures++;
    }
}

static void
check_time(const char* label, time_t got, time_t want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got %lld, want %lld\n", label, (long long)got, (long long)want);
        g_failures++;
    }
}

/* What the start hook saw: the decoder's clock and init stamps once the replay stream has opened. */
typedef struct {
    int calls;
    int source;
    time_t now;
    time_t last_cc_sync_time;
    time_t last_vc_sync_time;
    time_t last_t3_tune_time;
    time_t symbol_out_file_creation_time;
    int floor_checks; /* run the 1970-anchor checks inside the hook */
    int slco_check;   /* feed one single-fragment SLCO inside the hook */
    time_t slco_stamp_after;
    time_t slco_now_after; /* decode time read just after the SLCO, to bracket its stamp */
} hook_view;

static void
write_bits(uint8_t* bits, unsigned offset, unsigned value, unsigned width) {
    for (unsigned i = 0; i < width; i++) {
        bits[offset + i] = (uint8_t)((value >> (width - 1U - i)) & 1U);
    }
}

/* A single-fragment CACH burst carrying a valid SLCO Activity Update (dmr_cach(), LCSS 0). */
static void
build_single_fragment_cach(uint8_t cach[25]) {
    uint8_t slc[17];
    DSD_MEMSET(slc, 0, sizeof(slc));
    write_bits(slc, 0U, 0x1U, 4U);
    write_bits(slc, 4U, 0x8U, 4U);
    slc[12] = slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[6] ^ slc[7] ^ slc[9];
    slc[13] = slc[0] ^ slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[7] ^ slc[8] ^ slc[10];
    slc[14] = slc[1] ^ slc[2] ^ slc[3] ^ slc[4] ^ slc[5] ^ slc[8] ^ slc[9] ^ slc[11];
    slc[15] = slc[0] ^ slc[1] ^ slc[4] ^ slc[5] ^ slc[7] ^ slc[10];
    slc[16] = slc[0] ^ slc[1] ^ slc[2] ^ slc[5] ^ slc[6] ^ slc[8] ^ slc[11];
    DSD_MEMSET(cach, 0, 25U); /* TACT: AT 0, TC 0, LCSS 0 (single fragment) */
    for (int i = 0; i < 17; i++) {
        cach[i + 7] = slc[i];
    }
}

static size_t
count_text(const char* text, const char* needle) {
    size_t count = 0U;
    const size_t len = strlen(needle);
    for (const char* at = strstr(text, needle); at; at = strstr(at + len, needle)) {
        count++;
    }
    return count;
}

/* The first-second checks of a 1970-anchored capture, at media time 0: a patch stamped now still applies, and has
   expired once its 20 s TTL has passed; two single-fragment SLCOs in the first second print one line, a third a second
   later prints the second. */
static void
check_floor_decisions(dsd_opts* opts, dsd_state* state) {
    const int sgid = 0x0234;
    const int wgid = 0x0345;
    p25_patch_update(state, sgid, 1, 1);
    p25_patch_add_wgid(state, sgid, wgid);
    p25_patch_set_kas(state, sgid, 0, 0x80, -1);
    check("1970 anchor: a clear-key patch stamped in the first second applies", p25_patch_tg_key_is_clear(state, wgid));

    uint8_t cach[25];
    build_single_fragment_cach(cach);
    const int slot = state->currentslot;
    state->currentslot = 0;
    char out[8192];
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "replay_clock_slco") != 0) {
        check("stderr capture", 0);
        return;
    }
    (void)dmr_cach(opts, state, cach);
    (void)dmr_cach(opts, state, cach);
    dsd_decode_clock_set_media_ns(NS_PER_S);
    (void)dmr_cach(opts, state, cach);
    (void)dsd_test_capture_stderr_end(&cap);
    if (dsd_test_capture_stderr_read(&cap, out, sizeof(out)) != 0) {
        check("stderr capture read", 0);
        return;
    }
    state->currentslot = slot;
    if (count_text(out, "SLC Activity (single)") != 2U) {
        DSD_FPRINTF(stderr, "FAIL: 1970 anchor: SLCO single-fragment prints: got %zu, want 2 (one per second)\n",
                    count_text(out, "SLC Activity (single)"));
        g_failures++;
    }
    check_time("1970 anchor: the SLCO throttle stamp", state->slco_sfrag_last[0], kFloorS + 1);

    dsd_decode_clock_set_media_ns(21ULL * NS_PER_S);
    check("1970 anchor: the first-second patch has expired 21 s on", !p25_patch_tg_key_is_clear(state, wgid));
}

static int
observe_start(dsd_opts* opts, dsd_state* state, void* context) {
    hook_view* view = (hook_view*)context;
    view->calls++;
    view->source = (int)dsd_decode_clock_source();
    view->now = dsd_decode_time();
    view->last_cc_sync_time = state->last_cc_sync_time;
    view->last_vc_sync_time = state->last_vc_sync_time;
    view->last_t3_tune_time = state->last_t3_tune_time;
    view->symbol_out_file_creation_time = opts->symbol_out_file_creation_time;
    if (view->floor_checks) {
        check_floor_decisions(opts, state);
    }
    if (view->slco_check) {
        uint8_t cach[25];
        build_single_fragment_cach(cach);
        const int slot = state->currentslot;
        state->currentslot = 0;
        (void)dmr_cach(opts, state, cach);
        state->currentslot = slot;
        view->slco_stamp_after = state->slco_sfrag_last[0];
        view->slco_now_after = dsd_decode_time();
    }
    /* Seen what it needed: end the run before the decoder reads a sample. */
    dsd_exitflag_store(1);
    return 0;
}

/* Configure @p opts and @p state for a replay of @p fixture under @p mode, the way the CLI (or a host's configure step)
   does. */
static int
bootstrap_replay(const char* fixture, const char* mode, const char* sm_log, dsd_opts* opts, dsd_state* state) {
    char meta[DSD_TEST_PATH_MAX];
    char leaf[96];
    DSD_SNPRINTF(leaf, sizeof(leaf), "%s.iq.json", fixture);
    if (dsd_test_path_join(meta, sizeof(meta), DSD_NEO_TEST_IQ_FIXTURE_DIR, leaf) != 0) {
        return -1;
    }
    char arg0[] = "dsd-neo";
    char arg_frontend[] = "--frontend";
    char arg_none[] = "none";
    char arg_mode[16];
    char arg_mod[] = "-mq";
    char arg_replay[] = "--iq-replay";
    char arg_log[] = "--p25-sm-log";
    char arg_out[] = "-o";
    char arg_null[] = "null";
    char log_path[DSD_TEST_PATH_MAX];
    DSD_SNPRINTF(arg_mode, sizeof(arg_mode), "%s", mode);
    DSD_SNPRINTF(log_path, sizeof(log_path), "%s", sm_log ? sm_log : "");
    char* argv[] = {arg0, arg_frontend, arg_none, arg_mode, arg_mod,  arg_replay,
                    meta, arg_out,      arg_null, arg_log,  log_path, NULL};
    const int argc = sm_log ? 11 : 9;
    if (!sm_log) {
        argv[9] = NULL;
    }
    int boot_rc = 1;
    if (dsd_runtime_bootstrap(argc, argv, opts, state, NULL, &boot_rc) != DSD_BOOTSTRAP_CONTINUE) {
        DSD_FPRINTF(stderr, "FAIL: bootstrap did not continue for %s (rc %d)\n", fixture, boot_rc);
        return -1;
    }
    return 0;
}

/* Run the engine on @p fixture with @p extra arguments after the mode, the way the CLI does. */
static int
run_replay(const char* fixture, const char* mode, const char* sm_log, hook_view* view, dsd_opts** out_opts,
           dsd_state** out_state) {
    dsd_opts* opts = (dsd_opts*)calloc(1, sizeof(*opts));
    dsd_state* state = (dsd_state*)calloc(1, sizeof(*state));
    *out_opts = opts;
    *out_state = state;
    if (!opts || !state) {
        return -1;
    }
    /* The CLI's order: init stamps its seeds on the system clock before the run. */
    initOpts(opts);
    initState(state);
    if (bootstrap_replay(fixture, mode, sm_log, opts, state) != 0) {
        return -1;
    }
    const time_t seeded = state->last_vc_sync_time;
    if (seeded <= kCaptureAnchorS + 86400) {
        DSD_FPRINTF(stderr, "FAIL: the system clock (%lld) is not past the capture (%lld); the case needs it to be\n",
                    (long long)seeded, (long long)kCaptureAnchorS);
        return -1;
    }
    const dsd_engine_lifecycle_hooks hooks = {.start = observe_start, .stop = NULL, .context = view};
    return dsd_engine_run_with_lifecycle(opts, state, &hooks);
}

static void
free_run(dsd_opts* opts, dsd_state* state) {
    if (state) {
        freeState(state);
    }
    free(state);
    free(opts);
}

/* Old anchor, real init order, P25 SM log, and the return to the system clock. */
static void
test_capture_clock_from_an_older_capture(const char* dir) {
    char sm_log[DSD_TEST_PATH_MAX];
    if (dsd_test_path_join(sm_log, sizeof(sm_log), dir, "sm.log") != 0) {
        check("sm log path", 0);
        return;
    }
    hook_view view = {0};
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    const int rc = run_replay("p25p1_cqpsk_cc", "-f1", sm_log, &view, &opts, &state);
    const time_t after = dsd_realtime_time();
    check("the replay run ended cleanly", rc == 0);
    check("the start hook ran once", view.calls == 1);
    check("the decode clock ran on the capture's during the run", view.source == DSD_DECODE_CLOCK_REPLAY);
    check_time("decode now at media time 0", view.now, kCaptureAnchorS);
    check_time("last_cc_sync_time rebased onto the capture", view.last_cc_sync_time, kCaptureAnchorS);
    check_time("last_vc_sync_time rebased onto the capture", view.last_vc_sync_time, kCaptureAnchorS);
    check_time("last_t3_tune_time rebased onto the capture", view.last_t3_tune_time, kCaptureAnchorS);
    check_time("the symbol-file rotation stamp rebased onto the capture", view.symbol_out_file_creation_time,
               kCaptureAnchorS);
    /* dmr_csbk.c's Cap+ grant check and nxdn_element.c's recent-context check: now - last_vc_sync_time. */
    check("no negative elapsed time from last_vc_sync_time", view.now - view.last_vc_sync_time >= 0);
    check("no negative elapsed time from last_cc_sync_time", view.now - view.last_cc_sync_time >= 0);

    check("the decode clock is the system's after the run", dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    if (state) {
        check("last_vc_sync_time rebased back onto the system clock",
              state->last_vc_sync_time > kCaptureAnchorS + 86400 && state->last_vc_sync_time <= after);
        check("the rotation stamp rebased back onto the system clock",
              opts->symbol_out_file_creation_time > kCaptureAnchorS + 86400
                  && opts->symbol_out_file_creation_time <= after);
    }
    free_run(opts, state);

    /* The P25 SM log's first record: event=init, at the capture's time. */
    char want[64];
    char date[16];
    char clock[16];
    (void)dsd_format_local_datetime(kCaptureAnchorS, DSD_LOCAL_DATETIME_DATE_HYPHEN, date, sizeof(date));
    (void)dsd_format_local_datetime(kCaptureAnchorS, DSD_LOCAL_DATETIME_TIME_COLON, clock, sizeof(clock));
    DSD_SNPRINTF(want, sizeof(want), "%s %s event=init ", date, clock);
    char line[512] = {0};
    FILE* fp = dsd_fopen_existing_regular_file(sm_log, "r");
    if (!fp || !fgets(line, sizeof(line), fp)) {
        check("the P25 SM log has a first record", 0);
    } else if (strncmp(line, want, strlen(want)) != 0) {
        DSD_FPRINTF(stderr, "FAIL: the P25 SM log's first record is \"%.80s\", want it to start \"%s\"\n", line, want);
        g_failures++;
    }
    if (fp) {
        (void)fclose(fp);
    }
}

/* A 1970-anchored capture decodes from the 2000-01-01Z floor. */
static void
test_1970_anchor_starts_at_the_floor(void) {
    hook_view view = {0};
    view.floor_checks = 1;
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    const int rc = run_replay("p25p1_cqpsk_cc_simulcast", "-f1", NULL, &view, &opts, &state);
    check("the 1970-anchored replay run ended cleanly", rc == 0);
    check("the start hook ran once for the 1970 anchor", view.calls == 1);
    check("the 1970-anchored replay ran on the capture clock", view.source == DSD_DECODE_CLOCK_REPLAY);
    check_time("1970 anchor: decode now at media time 0 is the floor", view.now, kFloorS);
    check_time("1970 anchor: last_vc_sync_time rebased onto the floor", view.last_vc_sync_time, kFloorS);
    check("the decode clock is the system's after the 1970 run", dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    free_run(opts, state);
}

/* A second run on the state the first one used: the Android service configures and runs its one state again when a
   start races the previous run's stopSelfLatest(). That state keeps what the earlier run stamped on the system clock,
   which no rebase reaches, so the run must not move the clock onto the capture's older time. */
static void
test_reused_state_stays_on_the_system_clock(void) {
    hook_view first = {0};
    dsd_opts* opts = NULL;
    dsd_state* state = NULL;
    int rc = run_replay("p25p1_cqpsk_cc", "-f1", NULL, &first, &opts, &state);
    check("the first run on a fresh state ended cleanly", rc == 0);
    check("the first run on a fresh state ran on the capture clock", first.source == DSD_DECODE_CLOCK_REPLAY);
    if (rc != 0 || !opts || !state) {
        free_run(opts, state);
        return;
    }
    /* What an earlier live run leaves behind: a single-fragment SLCO stamped 5 s ago on the system clock. */
    const time_t left = dsd_realtime_time() - 5;
    state->slco_sfrag_last[0] = left;

    hook_view second = {0};
    second.slco_check = 1;
    char out[16384];
    out[0] = '\0';
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "replay_clock_reuse") != 0) {
        check("stderr capture", 0);
        free_run(opts, state);
        return;
    }
    rc = bootstrap_replay("p25p1_cqpsk_cc", "-f1", NULL, opts, state);
    if (rc == 0) {
        const dsd_engine_lifecycle_hooks hooks = {.start = observe_start, .stop = NULL, .context = &second};
        rc = dsd_engine_run_with_lifecycle(opts, state, &hooks);
    }
    (void)dsd_test_capture_stderr_end(&cap);
    if (dsd_test_capture_stderr_read(&cap, out, sizeof(out)) != 0) {
        check("stderr capture read", 0);
    }
    check("the run on the reused state ended cleanly", rc == 0);
    check("the start hook ran once on the reused state", second.calls == 1);
    check("a replay run on a reused state stays on the system clock", second.source == DSD_DECODE_CLOCK_SYSTEM);
    check("no stamp the earlier run left is ahead of the reused run's now", second.now >= left);
    /* Printed, it is restamped at the decode time it ran at, which lies between the two reads around it (on the system
       clock a second can turn between them). Held back, it keeps the earlier run's stamp, 5 s before both. */
    if (second.slco_stamp_after < second.now || second.slco_stamp_after > second.slco_now_after) {
        DSD_FPRINTF(stderr,
                    "FAIL: the single-fragment SLCO is not held back by the earlier run's stamp: stamp %lld, want "
                    "within [%lld, %lld]\n",
                    (long long)second.slco_stamp_after, (long long)second.now, (long long)second.slco_now_after);
        g_failures++;
    }
    const size_t warnings = count_text(out, "reuses a decoder state an earlier run used");
    if (warnings != 1U) {
        DSD_FPRINTF(stderr, "FAIL: the reused-state warning printed %zu times, want once\n", warnings);
        g_failures++;
    }
    check("the decode clock is the system's after the reused run",
          dsd_decode_clock_source() == DSD_DECODE_CLOCK_SYSTEM);
    free_run(opts, state);
}

int
main(void) {
    char dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(dir, sizeof(dir), "dsdneo_replay_decode_clock")) {
        DSD_FPRINTF(stderr, "FAIL: could not create a temp directory\n");
        return 1;
    }
    test_capture_clock_from_an_older_capture(dir);
    test_1970_anchor_starts_at_the_floor();
    test_reused_state_stays_on_the_system_clock();

    const char* const files[] = {"sm.log", NULL};
    if (dsd_test_remove_temp_dir(dir, files) != 0) {
        DSD_FPRINTF(stderr, "FAIL: could not remove %s\n", dir);
        g_failures++;
    }
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d replay decode clock check(s) failed\n", g_failures);
        return 1;
    }
    DSD_FPRINTF(stderr, "ENGINE_REPLAY_DECODE_CLOCK: OK\n");
    return 0;
}
