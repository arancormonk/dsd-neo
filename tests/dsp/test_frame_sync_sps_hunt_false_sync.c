// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief SPS hunt progress across sync returns (issue #388).
 *
 * Under AUTO, permissive matchers on the 4800/4 profile return isolated syncs that no
 * protocol ever turns into a frame. Each one used to pin the hunt twice over: it discarded
 * the per-call `synctest_pos` that armed the no-sync timeout, and it raised `state->carrier`,
 * which both blocked `frame_sync_no_sync_sps_hunt()` and zeroed the dwell counter. A signal
 * sitting on any other profile could then never be found.
 *
 * These cases drive `getFrameSync()` the way `live_scanner_*()` in src/engine/engine.c does:
 * every returned sync is "handled" by consuming symbols, exactly as a protocol dispatch
 * handler consumes dibits. What separates a false sync from a real one is how many symbols
 * the handler takes, so that is what the hunt is asked to measure.
 *
 * Since #391 a handler may also report a dsd_frame_verdict. processFrame() leaves it in
 * dsd_state::sps_hunt_last_frame_verdict for the next getFrameSync() entry to read, so
 * these cases write that field after "handling" a sync exactly as processFrame() does --
 * and one case deliberately stops writing it, to pin that a verdict is read once and
 * cleared rather than standing over the entries no processFrame() precedes.
 */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/sync_patterns.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/dsp/sync_calibration.h>
#include <dsd-neo/engine/protocol_dispatch.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <stdio.h>
#include <string.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "frame_sync_internal.h"
#include "frame_sync_state_buffers.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

/* An alternating run long enough to latch an M17 preamble candidate, followed by the link-setup
 * marker that spends it: what AUTO accepts on a signal that is not M17, and the shape a bit-sync
 * run on another protocol presents. A doubled marker alone no longer syncs -- that is the point
 * of #399 -- so the run is what makes this a false sync the hunt has to reckon with. */
#define FALSE_SYNC_PREAMBLE_RUN M17_PRE M17_PRE M17_PRE M17_PRE M17_PRE M17_PRE M17_PRE M17_PRE M17_PRE M17_PRE
#define FALSE_SYNC_MARKER       FALSE_SYNC_PREAMBLE_RUN M17_LSF

/* Symbol source: `gap` filler symbols, then the marker, repeating. A zero-length
 * marker makes the stream pure filler (the idle case). */
static const char* g_marker = "";
static int g_marker_gap = 0;
static int g_stream_pos = 0;
static long g_symbols_emitted = 0;

static void
stream_reset(const char* marker, int gap) {
    g_marker = marker ? marker : "";
    g_marker_gap = gap;
    g_stream_pos = 0;
    g_symbols_emitted = 0;
}

static char
stream_next_dibit(void) {
    const int marker_len = (int)strlen(g_marker);
    const int period = g_marker_gap + marker_len;
    char dibit = '1';
    if (period > 0) {
        const int phase = g_stream_pos % period;
        if (phase >= g_marker_gap) {
            dibit = g_marker[phase - g_marker_gap];
        }
    }
    g_stream_pos++;
    g_symbols_emitted++;
    return dibit;
}

/* Mirrors the production slicer's bookkeeping: history push plus the symbol counter
 * that src/dsp/dsd_symbol.c bumps on every getSymbol() path. */
float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;

    const char dibit = stream_next_dibit();
    const float symbol = (dibit == '3') ? -3.0f : 3.0f;
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
    return symbol;
}

/* Every digital candidate enabled, C4FM selected, timing on the 4800/4 profile: the
 * shape both AUTO and a generic modulation lock start from. @p mod_cli_lock is the only
 * thing separating them, and it is what decides how the hunt rotates:
 *
 * - 0 is `-fa`. decode_mode_apply_auto() only ever reads mod_cli_lock, so AUTO leaves the
 *   dsd_init.c default of 0 in place. The hunt is then a plain round-robin over enabled
 *   profiles (frame_sync_sps_hunt_next_index()) and reprograms symbol timing at each step.
 * - 1 is `-mc` and its siblings, the only writers of the flag. The hunt is confined to
 *   profiles whose timing already matches (frame_sync_sps_hunt_next_index_matching_timing())
 *   and frame_sync_apply_sps_profile_timing() returns without touching samplesPerSymbol.
 */
static void
init_hunt_opts_state(dsd_opts* opts, dsd_state* state, int mod_cli_lock) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(state, 0, sizeof(*state));

    opts->audio_in_type = AUDIO_IN_WAV;
    opts->wav_sample_rate = 96000;
    opts->wav_decimator = 48000;
    opts->mod_cli_lock = mod_cli_lock;
    opts->mod_c4fm = 1;
    opts->msize = 1;
    opts->ssize = 128;
    opts->frame_dstar = 1;
    opts->frame_x2tdma = 1;
    opts->frame_p25p1 = 1;
    opts->frame_p25p2 = 1;
    opts->frame_nxdn48 = 1;
    opts->frame_nxdn96 = 1;
    opts->frame_dmr = 1;
    opts->frame_dpmr = 1;
    opts->frame_provoice = 1;
    opts->frame_ysf = 1;
    opts->frame_m17 = 1;

    state->rf_mod = 0;
    state->p25_p2_active_slot = -1;
    state->center = 0.0f;
    state->min = -3.0f;
    state->max = 3.0f;
    state->lmid = -2.0f;
    state->umid = 2.0f;
    state->minref = -2.4f;
    state->maxref = 2.4f;

    state->sps_hunt_idx = DSD_FRAME_SYNC_SPS_PROFILE_4800_4;
    const int demod_rate = dsd_opts_current_input_timing_rate(opts);
    state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, 4800, demod_rate);
    state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
}

typedef struct {
    const char* label;
    const char* marker;                                  /* sync marker injected into the stream ("" = never syncs) */
    int marker_gap;                                      /* filler symbols between markers */
    int handler_symbols;                                 /* symbols the "protocol handler" consumes per returned sync */
    int handler_unproductive;                            /* verdict the handler reports on every sync (#391) */
    int unproductive_lead_syncs;                         /* ... plus the first N syncs, always UNPRODUCTIVE */
    int stamp_verdict_syncs;                             /* 0 = every sync stamps a verdict; N = only the first N */
    long symbol_budget;                                  /* stop after this many symbols leave the source */
    int expect_step;                                     /* 1 = the hunt must leave its starting profile */
    int mod_cli_lock;                                    /* 0 = AUTO (-fa), 1 = a generic modulation lock (-mc) */
    void (*configure)(dsd_opts* opts, dsd_state* state); /* optional setup on top of the defaults */
} HuntCase;

typedef struct {
    int stepped;
    long symbols_at_step;
    int syncs_seen;
    int final_idx;
    int final_sps; /* samplesPerSymbol after the step: what the profile change reprogrammed timing to */
} HuntResult;

/* The engine's shape: getFrameSync() in a loop, each sync handed to a handler that
 * consumes symbols, until the hunt steps or the symbol budget runs out. */
static HuntResult
drive_hunt(const HuntCase* tc) {
    static dsd_opts opts;
    static dsd_state state;
    HuntResult result = {0, 0, 0, 0, 0};

    init_hunt_opts_state(&opts, &state, tc->mod_cli_lock);
    if (tc->configure) {
        tc->configure(&opts, &state);
    }
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "%s: failed to allocate frame-sync state buffers\n", tc->label);
        return result;
    }
    const int start_idx = state.sps_hunt_idx;
    stream_reset(tc->marker, tc->marker_gap);
    dsd_frame_sync_reset_mod_state();

    while (g_symbols_emitted < tc->symbol_budget) {
        const int sync = getFrameSync(&opts, &state);
        if (state.sps_hunt_idx != start_idx) {
            result.stepped = 1;
            result.symbols_at_step = g_symbols_emitted;
            break;
        }
        if (sync == DSD_SYNC_NONE) {
            continue;
        }
        result.syncs_seen++;
        for (int i = 0; i < tc->handler_symbols; i++) {
            (void)getSymbol(&opts, &state, 1);
        }
        /* What processFrame() does with the handler's dsd_frame_verdict. stamp_verdict_syncs
         * stops the stamping partway, which is the engine's other shape: an entry no
         * processFrame() precedes leaves the field exactly as the last one left it. */
        if (tc->stamp_verdict_syncs == 0 || result.syncs_seen <= tc->stamp_verdict_syncs) {
            const int unproductive = tc->handler_unproductive || result.syncs_seen <= tc->unproductive_lead_syncs;
            state.sps_hunt_last_frame_verdict =
                unproductive ? DSD_FRAME_VERDICT_UNPRODUCTIVE : DSD_FRAME_VERDICT_PRODUCTIVE;
        }
    }

    result.final_idx = state.sps_hunt_idx;
    result.final_sps = state.samplesPerSymbol;
    free_state_buffers(&state);
    if (result.stepped != tc->expect_step) {
        DSD_FPRINTF(stderr, "%s: gap %d, handler %d -> stepped=%d after %ld symbols (%d syncs)\n", tc->label,
                    tc->marker_gap, tc->handler_symbols, result.stepped, g_symbols_emitted, result.syncs_seen);
    }
    assert(result.stepped == tc->expect_step);
    return result;
}

/* Timing the profiles carry at this suite's 96 kHz input, per dsd_opts_compute_sps_rate():
 * both 4800 symbol/s profiles land on 20 samples per symbol, the 2400 one on 40. Asserting
 * these is how a case sees that a step reprogrammed timing rather than only moving the index. */
#define HUNT_SPS_4800 20
#define HUNT_SPS_2400 40

/* Where an unlocked (AUTO) hunt goes from the 4800/4 start: plain round-robin lands on the
 * next enabled profile regardless of timing, and drags samplesPerSymbol with it. This is the
 * step the -fa I/Q replay checks watch for as "SPS hunt: trying <n> sps". */
static void
assert_auto_stepped_to_2400_4(const HuntResult* r) {
    assert(r->stepped == 1);
    assert(r->final_idx == DSD_FRAME_SYNC_SPS_PROFILE_2400_4);
    assert(r->final_sps == HUNT_SPS_2400);
}

/* #388: isolated syncs that no handler turns into a frame must not pin the hunt. */
static void
test_false_syncs_do_not_starve_the_hunt(void) {
    /* One marker per ~300 symbols is far more often than the 1800-symbol no-sync
     * timeout, so before the fix the timeout never armed and the hunt never ran. */
    const HuntCase tc = {
        .label = "false syncs at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 284,
        .handler_symbols = 8, /* a handler that recognised a marker and read no frame */
        .symbol_budget = 40000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 10);
    assert_auto_stepped_to_2400_4(&r);
    /* The hunt owes the profile one full dwell before it may leave, and must not
     * need much more than that. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols * 2);
}

/* #388, the density half: an unproductive matcher that fires every few symbols must not
 * hold the profile any longer than one that fires every few hundred. A refund that
 * accumulates across syncs makes the dwell a function of the false-sync cadence, so a
 * dense matcher pins the profile forever while a sparse one does not. */
static void
test_dense_false_syncs_do_not_starve_the_hunt(void) {
    /* An alternating bit-sync run matches the M17 preamble at nearly every offset, so a
     * near-zero gap is the realistic shape here, not a contrived one. These span the cliff
     * an accumulating refund put at gap = handler cost x dwell passes (8 x 3): below it the
     * refund outran the budget and pinned the profile for good, above it the hunt stepped
     * normally. The dwell must not know which side of that line the cadence fell on. */
    static const int gaps[] = {0, 8, 16, 24, 40};

    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
        const HuntCase tc = {
            .label = "dense false syncs at 4800/4",
            .marker = FALSE_SYNC_MARKER,
            .marker_gap = gaps[i],
            .handler_symbols = 8, /* a handler that recognised a marker and read no frame */
            .symbol_budget = 400000,
            .expect_step = 1,
        };
        const HuntResult r = drive_hunt(&tc);

        assert(r.syncs_seen > 10);
        assert_auto_stepped_to_2400_4(&r);
        /* The dwell the idle case gets, regardless of how often the matcher fired. */
        const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
        assert(r.symbols_at_step >= dwell_symbols);
        assert(r.symbols_at_step < dwell_symbols * 2);
    }
}

/* The floor is the whole of the density fix, so state where it sits. One symbol short of a
 * frame's worth, at the densest cadence the stream can produce, is the worst case an
 * unproductive matcher can present -- and it still gets exactly the idle dwell.
 *
 * Deliberately left on the default PRODUCTIVE verdict after #391: the floor is what stops
 * a marker-and-bail matcher buying dwell, and it must keep doing that on its own, for the
 * protocols that have no verdict to report. */
static void
test_sub_frame_consumption_never_buys_dwell(void) {
    const HuntCase tc = {
        .label = "sub-frame handler at the densest cadence",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 0,
        .handler_symbols = DSD_FRAME_SYNC_MIN_FRAME_SYMBOLS - 1,
        .handler_unproductive = 0,
        .symbol_budget = 400000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert_auto_stepped_to_2400_4(&r);
    /* The budget is spent in searched symbols; symbols_at_step also counts what the
     * handler swallowed, which at this cadence is nearly one per symbol searched. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols * 3);
}

/* The other side of the contract: a signal being decoded holds its profile. */
static void
test_consumed_frames_hold_the_profile(void) {
    /* Same marker, same cadence -- only the handler's appetite differs. A protocol
     * reading real frames consumes far more than it searches (NXDN96 spends 182
     * symbols per frame, P25p1 408, M17 184).
     *
     * On the default PRODUCTIVE verdict, which is what #391 leaves every protocol with no
     * check of its own -- D-STAR voice, ProVoice, dPMR, X2-TDMA, P25p2, DMR. A frame's
     * worth consumed still holds the profile for them, exactly as before. */
    const HuntCase tc = {
        .label = "decoded frames at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 84,
        .handler_symbols = 400,
        .handler_unproductive = 0,
        .symbol_budget = 60000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 50);
    assert(r.stepped == 0);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
}

/* #391: the new property. Same marker, same cadence and the same frame's worth of symbols
 * as test_consumed_frames_hold_the_profile() -- the one difference is that the handler
 * reports it validated nothing, which is what a failed FICH CRC, a failed EDACS BCH, a
 * failed D-STAR header CRC, a failed P25p1 NID or an unconfirmed NXDN frame say. That
 * profile must reach its dwell like an idle one instead of being held by symbols no CRC
 * would have accepted. */
static void
test_unproductive_frames_never_buy_dwell(void) {
    const HuntCase tc = {
        .label = "unproductive frames at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 84,
        .handler_symbols = 400,
        .handler_unproductive = 1,
        .symbol_budget = 400000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 10);
    assert_auto_stepped_to_2400_4(&r);
    /* The idle dwell, no more: an unproductive verdict buys nothing however large the
     * block behind it was. The upper bound allows for the handler's own symbols, which
     * leave the source without counting against the searched budget. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols * 6);
}

/* #391, the other half of the same rule: an unproductive lead does not condemn the
 * transmission behind it. A transmission that starts with frames the protocol cannot yet
 * confirm -- NXDN before its first FACCH, YSF before its first FICH CRC -- and then decodes
 * must hold its profile from the frame it confirms on. This pins the hunt's arithmetic, not
 * the clear: every sync here stamps a verdict of its own, as processFrame() does. */
static void
test_a_confirming_transmission_holds_its_profile(void) {
    const HuntCase tc = {
        .label = "unproductive lead, then decoded frames",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 84,
        .handler_symbols = 400,
        .handler_unproductive = 0,
        .unproductive_lead_syncs = 3,
        .symbol_budget = 60000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 50);
    assert(r.stepped == 0);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
}

/* #391: one verdict answers for one handler call. frame_sync_sps_hunt_note_handler_consumption()
 * clears dsd_state::sps_hunt_last_frame_verdict as it reads it, so a verdict cannot be charged
 * against consumption that is not the handler's -- the entries no processFrame() precedes, such
 * as a frame live_scanner_process_synced_frames() finds undispatchable after a retune.
 *
 * Only the first sync stamps anything here, and it stamps UNPRODUCTIVE; every sync after it
 * consumes a frame's worth with the field left exactly as it was. Without the clear that one
 * verdict stands for the rest of the run, every later frame is refused its credit, and the
 * profile that is decoding is rotated away from. */
static void
test_a_verdict_is_read_once_and_cleared(void) {
    const HuntCase tc = {
        .label = "one unproductive verdict, then unstamped frames",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 84,
        .handler_symbols = 400,
        .handler_unproductive = 1,
        .stamp_verdict_syncs = 1,
        .symbol_budget = 60000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 50);
    assert(r.stepped == 0);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
}

/* No signal at all: the rotation period is the one docs/cli.md documents. */
static void
test_idle_rotation_period_is_unchanged(void) {
    const HuntCase tc = {
        .label = "idle hunt",
        .marker = "",
        .marker_gap = 0,
        .handler_symbols = 0,
        .symbol_budget = 40000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen == 0);
    assert_auto_stepped_to_2400_4(&r);
    /* Three no-sync passes of 1800 symbols, as before this change. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols + 200);
}

/* The locked half of the contract, and the other rotation the hunt has: under a generic
 * modulation lock (-mc) frame_sync_no_sync_sps_hunt() takes the equal-timing walk instead,
 * so from 4800/4 it reaches the only other 20-sps profile, D-STAR's 4800/2, and
 * frame_sync_apply_sps_profile_timing() leaves samplesPerSymbol alone. Same dwell either way:
 * the lock decides where the hunt goes, never how long it stays. */
static void
test_locked_modulation_rotates_within_equal_timing(void) {
    const HuntCase tc = {
        .label = "idle hunt under -mc",
        .marker = "",
        .marker_gap = 0,
        .handler_symbols = 0,
        .symbol_budget = 40000,
        .expect_step = 1,
        .mod_cli_lock = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen == 0);
    assert(r.stepped == 1);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_2);
    assert(r.final_sps == HUNT_SPS_4800);
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols + 200);
}

/* Frame-sync hook fakes that record the order they were called in. */
static int g_hook_order = 0;
static int g_vc_no_sync_order = 0;
static int g_release_order = 0;
static int g_no_carrier_order = 0;

static void
fake_p25_sm_vc_no_sync(dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_vc_no_sync_order = ++g_hook_order;
}

static void
fake_p25_sm_release(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_release_order = ++g_hook_order;
}

static void
fake_no_carrier(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_no_carrier_order = ++g_hook_order;
}

/* A P25 voice channel the trunk SM has tuned, whose hangtime has already run out: the
 * shape in which frame_sync_no_sync_try_p25_release() has a release to make. */
static void
configure_tuned_vc_past_hangtime(dsd_opts* opts, dsd_state* state) {
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
    opts->trunk_hangtime = 0;
    state->last_vc_sync_time = 0;
    state->last_vc_sync_time_m = 0.0;
    state->p25_last_vc_tune_time = 0;
    state->p25_last_vc_tune_time_m = 0.0;
}

/* #393: the budget exit is a no-sync exit like the timeout, so it owes the P25 SM the
 * same accounting in the same order -- VC no-sync pass, release check, then no-carrier.
 * Markers every 8 symbols keep the 1800-symbol timeout from ever arming, so only the
 * budget exit can step here. */
static void
test_budget_exit_runs_the_no_sync_hooks(void) {
    g_hook_order = 0;
    g_vc_no_sync_order = 0;
    g_release_order = 0;
    g_no_carrier_order = 0;
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){
        .p25_sm_release = fake_p25_sm_release,
        .p25_sm_vc_no_sync = fake_p25_sm_vc_no_sync,
        .no_carrier = fake_no_carrier,
    });

    const HuntCase tc = {
        .label = "budget exit hooks",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 0,
        .handler_symbols = 8,
        .symbol_budget = 40000,
        .expect_step = 1,
        .configure = configure_tuned_vc_past_hangtime,
    };
    const HuntResult r = drive_hunt(&tc);
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});

    /* Same AUTO step the other stepping cases assert: the budget exit is a different way out
     * of the dwell, not a different rotation. */
    assert_auto_stepped_to_2400_4(&r);
    assert(g_vc_no_sync_order > 0);
    assert(g_release_order > g_vc_no_sync_order);
    assert(g_no_carrier_order > g_release_order);
}

int
main(void) {
    test_false_syncs_do_not_starve_the_hunt();
    test_dense_false_syncs_do_not_starve_the_hunt();
    test_sub_frame_consumption_never_buys_dwell();
    test_consumed_frames_hold_the_profile();
    test_unproductive_frames_never_buy_dwell();
    test_a_confirming_transmission_holds_its_profile();
    test_a_verdict_is_read_once_and_cleared();
    test_idle_rotation_period_is_unchanged();
    test_locked_modulation_rotates_within_equal_timing();
    test_budget_exit_runs_the_no_sync_hooks();
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
