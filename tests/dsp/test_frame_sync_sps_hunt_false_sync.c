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
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/frame_sync_hooks.h>
#include <dsd-neo/runtime/trunk_scan_hooks.h>
#include <stdint.h>
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
/* The read, counted from 1, that leaves a silent input's wait for a queued command (issue #634): it reads nothing, so
   it pushes no symbol, as dsd_symbol.c's read does. 0 for none. */
static long g_interrupt_at_read = 0;
static long g_reads = 0;
/* Non-zero: every symbol read steps the TEST decode clock by one symbol period at this rate, so the clock is the
   stream's own (discussion #717). */
static int g_symbol_clock_rate = 0;
static const uint64_t k_symbol_clock_t0_ns = 1000000000000ULL;
/* The profile the hunt started on, and the symbols the source had emitted when a read first found the hunt on another:
   a step inside getFrameSync() that does not return (a hint) shows here. -1 / 0: not watched, or no step yet. */
static int g_watch_idx = -1;
static long g_idx_left_at = 0;
/* The symbols emitted at which a read drops the sync join histories, as a carrier change or a trunk retune does
   (dsd_state_forget_sync_join_histories()); 0 never. */
static long g_forget_histories_at = 0;

float
getSymbol(dsd_opts* opts, dsd_state* state, int have_sync) { // NOLINT(misc-use-internal-linkage)
    (void)opts;
    (void)have_sync;

    g_reads++;
    if (g_watch_idx >= 0 && g_idx_left_at == 0 && state->sps_hunt_idx != g_watch_idx) {
        g_idx_left_at = g_symbols_emitted;
    }
    if (g_forget_histories_at > 0 && g_symbols_emitted == g_forget_histories_at) {
        dsd_state_forget_sync_join_histories(state);
    }
    if (g_interrupt_at_read > 0 && g_reads == g_interrupt_at_read) {
        state->input_interrupted = 1;
        return 0.0f;
    }
    const char dibit = stream_next_dibit();
    const float symbol = (dibit == '3') ? -3.0f : 3.0f;
    dsd_symbol_history_push(state, symbol);
    state->symbolcnt++;
    if (g_symbol_clock_rate > 0) {
        dsd_decode_clock_test_set_ns(k_symbol_clock_t0_ns
                                     + (uint64_t)g_symbols_emitted * 1000000000ULL / (uint64_t)g_symbol_clock_rate);
    }
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
    const char* marker;          /* sync marker injected into the stream ("" = never syncs) */
    int marker_gap;              /* filler symbols between markers */
    int handler_symbols;         /* symbols the "protocol handler" consumes per returned sync */
    int handler_unproductive;    /* verdict the handler reports on every sync (#391) */
    int unproductive_lead_syncs; /* ... plus the first N syncs, always UNPRODUCTIVE */
    const int* verdict_cycle;    /* verdicts to report in turn, repeating; overrides the two above (#400) */
    int verdict_cycle_len;       /* how many entries verdict_cycle has */
    int stamp_verdict_syncs;     /* 0 = every sync stamps a verdict; N = only the first N */
    long symbol_budget;          /* stop after this many symbols leave the source */
    int expect_step;             /* 1 = the hunt must leave its starting profile */
    int mod_cli_lock;            /* 0 = AUTO (-fa), 1 = a generic modulation lock (-mc) */
    void (*configure)(dsd_opts* opts, dsd_state* state); /* optional setup on top of the defaults */
} HuntCase;

typedef struct {
    int stepped;
    long symbols_at_step;
    int syncs_seen;
    int final_idx;
    int final_sps; /* samplesPerSymbol after the step: what the profile change reprogrammed timing to */
    long left_at;  /* symbols emitted when a read first found the hunt off its start profile (0: never) */
} HuntResult;

/* The engine's shape: getFrameSync() in a loop, each sync handed to a handler that
 * consumes symbols, until the hunt steps or the symbol budget runs out. */
static HuntResult
drive_hunt(const HuntCase* tc) {
    static dsd_opts opts;
    static dsd_state state;
    HuntResult result = {0, 0, 0, 0, 0, 0};

    init_hunt_opts_state(&opts, &state, tc->mod_cli_lock);
    if (tc->configure) {
        tc->configure(&opts, &state);
    }
    if (!init_state_buffers(&state)) {
        DSD_FPRINTF(stderr, "%s: failed to allocate frame-sync state buffers\n", tc->label);
        return result;
    }
    const int start_idx = state.sps_hunt_idx;
    g_watch_idx = start_idx;
    g_idx_left_at = 0;
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
            if (tc->verdict_cycle != NULL) {
                state.sps_hunt_last_frame_verdict = tc->verdict_cycle[(result.syncs_seen - 1) % tc->verdict_cycle_len];
            } else {
                const int unproductive = tc->handler_unproductive || result.syncs_seen <= tc->unproductive_lead_syncs;
                state.sps_hunt_last_frame_verdict =
                    unproductive ? DSD_FRAME_VERDICT_UNPRODUCTIVE : DSD_FRAME_VERDICT_PRODUCTIVE;
            }
        }
    }

    result.final_idx = state.sps_hunt_idx;
    result.final_sps = state.samplesPerSymbol;
    result.left_at = g_idx_left_at;
    g_watch_idx = -1;
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
     * check of its own -- DMR, P25 Phase 2, dPMR and X2-TDMA, since #429 gave D-STAR voice
     * and ProVoice one. A frame's worth consumed still holds the profile for them, exactly
     * as before. */
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

/* #391's residual, and the bound on it. Three handlers still report no verdict at any point --
 * DMR, P25 Phase 2 and X2-TDMA -- so a false match on one is credited in full, as before. What
 * stops that pinning a profile is arithmetic rather than a verdict: the symbols a handler eats
 * are symbols the search never sees, so one cycle nets (period - 2 x consumption) onto the
 * budget, and only a cadence tighter than twice the block it swallows can hold the profile.
 *
 * dPMR used to be the fourth, and the only one of them whose matcher fires on noise at all:
 * 372 symbols read behind a single 12-symbol pattern per polarity, a cliff at one sync per 744
 * symbols against a noise rate of 2/2^12, one per ~2048. It reports verdicts since #407, so the
 * set that depends on this arithmetic no longer contains anything noise reaches -- the other
 * three sit behind 20- and 24-symbol exact matchers. The bound is kept, and still driven at
 * dPMR's numbers, because it is the only case in reach of the cliff: were a future handler to
 * consume a frame this large behind a matcher this short, this is what would have to hold.
 * test_a_dpmr_frame_that_proves_nothing_rotates_promptly covers what dPMR does now.
 *
 * The hunt's arithmetic does not vary by profile, so this drives it from the harness's 4800/4
 * start like every case here; what is modelled is consumption and cadence, not a profile. */
/* What dPMR does now that its CCH CRC-7 carries a verdict (#407), tested where it decides
 * something. The case below bounds the sparse cadence, where consumption alone still lets
 * the profile go; this is the dense one, where it does not. FS2 arriving every 384 symbols
 * is inside the 744-symbol cliff, so on the default productive verdict a stream of frames
 * consuming 372 symbols each pins the profile outright and never rotates -- the failure
 * mode protocol_dispatch.h's arithmetic cannot reach.
 *
 * That is not hypothetical: it is the committed dpmr fixture, a carrier presenting FS2
 * whose CCH decodes nothing. Reporting UNPRODUCTIVE refuses those frames the credit their
 * consumption would buy, and the hunt moves on at the idle rate. */
static void
test_a_dpmr_carrier_that_decodes_nothing_still_rotates(void) {
    static const int cycle[] = {DSD_FRAME_VERDICT_UNPRODUCTIVE};
    const HuntCase tc = {
        .label = "dPMR FS2 on schedule, reporting no decode",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 384 - 88, /* the real frame period, well inside the 744-symbol cliff */
        .handler_symbols = 372, /* a dPMR FS2 frame, read in full before the check runs */
        .verdict_cycle = cycle,
        .verdict_cycle_len = 1,
        .symbol_budget = 400000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 0);
    assert_auto_stepped_to_2400_4(&r);
    /* At the idle rate: nothing here bought any dwell at all. */
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols * 2);
}

/* And the direction that matters more: a real dPMR carrier presents FS2 every 384 symbols
 * with a CCH that decodes, so the profile has to hold. A CRC that passes on one frame in
 * four still holds it, because a proof covers the two seconds after it (#407). */
static void
test_a_decoding_dpmr_carrier_holds_its_profile(void) {
    static const int cycle[] = {
        DSD_FRAME_VERDICT_PROFILE_PROVEN,
        DSD_FRAME_VERDICT_PROFILE_PROVEN,
        DSD_FRAME_VERDICT_PROFILE_PROVEN,
        DSD_FRAME_VERDICT_UNPRODUCTIVE,
    };
    const HuntCase tc = {
        .label = "a dPMR carrier decoding on schedule",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 384 - 88, /* FS2 every 384 symbols, the real frame period */
        .handler_symbols = 372,
        .verdict_cycle = cycle,
        .verdict_cycle_len = 4,
        .symbol_budget = 400000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 0);
    assert(r.stepped == 0);
}

static void
test_no_verdict_handlers_still_rotate_at_the_noise_cadence(void) {
    /* The marker is part of the period, so these are the cadences less its 88 symbols. */
    static const int gaps[] = {2048 - 88, 4096 - 88};

    for (size_t i = 0; i < sizeof(gaps) / sizeof(gaps[0]); i++) {
        const HuntCase tc = {
            .label = "no-verdict handler at the dPMR noise cadence",
            .marker = FALSE_SYNC_MARKER,
            .marker_gap = gaps[i],
            .handler_symbols = 372,    /* a dPMR FS2 frame, read in full before any check */
            .handler_unproductive = 0, /* the default verdict, which is what those four report */
            .symbol_budget = 400000,
            .expect_step = 1,
        };
        const HuntResult r = drive_hunt(&tc);

        assert(r.syncs_seen > 0);
        assert_auto_stepped_to_2400_4(&r);
        /* Held longer than an idle profile, because every match is credited in full -- but
         * bounded, which is the property #391's remaining set turns on. */
        const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
        assert(r.symbols_at_step >= dwell_symbols);
        assert(r.symbols_at_step < dwell_symbols * 3);
    }
}

/* #391, the other half of the same rule: an unproductive lead does not condemn the
 * transmission behind it. A transmission that starts with frames the protocol cannot yet
 * confirm -- YSF before its first FICH CRC -- and then decodes must hold its profile from
 * the frame it confirms on. This pins the hunt's arithmetic, not the clear: every sync here
 * stamps a verdict of its own, as processFrame() does. The protocols that answer a passing
 * check with PROFILE_PROVEN rather than PRODUCTIVE -- P25p1, dPMR, and NXDN since #445 --
 * hold their profile the stronger way, pinned by the proven cases below. */
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

/* #400: the case consumption credit cannot express. A P25p1 control channel reads 33 symbols
 * of a ~180-symbol slot when the NID fails and 134 when a one-block TSDU decodes -- it stops
 * on the standard's last-block bit, not on a budget -- so every frame leaves the search
 * holding the rest of the slot, and the floor at zero denies a decoded frame any reserve to
 * spend on the failures between. Off-air under -fa this ran at roughly two failures per
 * decode and rotated off a live control channel every ~30 s.
 *
 * The verdicts here are the ones dsd_dispatch_handle_p25p1() reports for that traffic, and
 * they are the enumerators themselves: the DSP layer compares against literals because it
 * includes no engine headers, so this case is also what pins the two to the same numbers. */
static void
test_a_proven_verdict_holds_the_profile_through_failure_runs(void) {
    static const int cycle[] = {DSD_FRAME_VERDICT_PROFILE_PROVEN, DSD_FRAME_VERDICT_UNPRODUCTIVE,
                                DSD_FRAME_VERDICT_UNPRODUCTIVE};
    /* One decode per two failures, each frame reading the 33 symbols a NID costs and leaving
     * the balance of its slot to the search. Under consumption credit alone the budget climbs
     * ~206 symbols every three slots and reaches the dwell inside a few thousand. */
    const HuntCase tc = {
        .label = "one decode per two failed NIDs at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 120,
        .handler_symbols = 33,
        .verdict_cycle = cycle,
        .verdict_cycle_len = (int)(sizeof(cycle) / sizeof(cycle[0])),
        .symbol_budget = 400000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    /* Far more than the dwell's worth of symbols went by without a step, and the proofs are a
     * small minority of them: the profile is held by what decoded, not by what it consumed. */
    assert(r.syncs_seen > 500);
    assert(r.stepped == 0);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
}

/* #400 in the direction #388 guards: a proof restarts the dwell, it does not bank one. The
 * profile that stops proving itself must leave on the idle dwell, measured from the last
 * proof -- which is what makes a false proof cost one dwell rather than the profile. */
static void
test_a_proven_verdict_buys_one_dwell_and_no_more(void) {
    static const int cycle[] = {DSD_FRAME_VERDICT_PROFILE_PROVEN};
    const HuntCase lead = {
        .label = "proofs, then nothing but failures",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 120,
        .handler_symbols = 33,
        .verdict_cycle = cycle,
        .verdict_cycle_len = 1,
        .stamp_verdict_syncs = 8, /* eight proofs, then every sync reads the cleared field */
        .symbol_budget = 400000,
        .expect_step = 1,
    };
    /* stamp_verdict_syncs leaves the field cleared afterwards, which reads as PRODUCTIVE --
     * and 33 symbols is under the size floor, so nothing is credited past the eighth proof.
     * That is the shape of a channel that goes quiet: the syncs keep arriving, nothing
     * decodes, and the profile is given up. */
    const HuntResult r = drive_hunt(&lead);

    assert_auto_stepped_to_2400_4(&r);
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    /* Eight proofs in a row bought exactly what one would: the dwell runs from the last of
     * them, so the step lands within a dwell of it rather than eight. */
    assert(r.symbols_at_step >= dwell_symbols);
    assert(r.symbols_at_step < dwell_symbols * 2);
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

/* A trunk-scan dwell the coordinator would rotate on: due once the stream's clock reaches its deadline, once. */
static double g_dwell_deadline_s = 0.0;
static int g_dwell_yielded = 0;
static long g_dwell_asks = 0;

static int
fake_trunk_scan_dwell_due(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    g_dwell_asks++;
    if (g_dwell_yielded || dsd_decode_now_mono_s() < g_dwell_deadline_s) {
        return 0;
    }
    g_dwell_yielded = 1;
    return 1;
}

/* Symbols one getFrameSync() call reads on dead air, the stream clocked at @p symbol_rate, with a trunk-scan dwell of
   @p dwell_ms that started at the call's first symbol (0: no trunk scan installed). */
static long
dead_air_return_symbols(int symbol_rate, int dwell_ms, long* asks) {
    static dsd_opts opts;
    static dsd_state state;
    init_hunt_opts_state(&opts, &state, 0);
    assert(init_state_buffers(&state));
    stream_reset("", 0);
    dsd_frame_sync_reset_mod_state();
    g_symbol_clock_rate = symbol_rate;
    dsd_decode_clock_use_test(k_symbol_clock_t0_ns);
    g_dwell_deadline_s = dsd_decode_now_mono_s() + (double)dwell_ms / 1000.0;
    g_dwell_yielded = 0;
    g_dwell_asks = 0;
    dsd_trunk_scan_hooks dwell_hooks = {0};
    dwell_hooks.dwell_due = fake_trunk_scan_dwell_due;
    dsd_trunk_scan_hooks_set(dwell_ms > 0 ? &dwell_hooks : NULL);

    const int sync = getFrameSync(&opts, &state);
    const long read = g_symbols_emitted;

    dsd_trunk_scan_hooks_set(NULL);
    dsd_decode_clock_use_system();
    g_symbol_clock_rate = 0;
    free_state_buffers(&state);
    assert(sync == DSD_SYNC_NONE);
    *asks = g_dwell_asks;
    return read;
}

/*
 * Discussion #717: a trunk-scan target's idle dwell is honoured from inside the hunt. On dead air getFrameSync() hands
 * back once per 1800-symbol pass (375 ms at 4800 sym/s), and the coordinator used to see its dwell only then: a 250 ms
 * dwell ran a whole pass. The hunt now asks the coordinator every 300 symbols (62.5 ms at 4800/s) and hands back once
 * the dwell is due, so the rotation lands within 300 symbols of the deadline; the ask stays off the per-symbol path.
 */
static void
test_a_trunk_scan_dwell_returns_inside_the_pass(void) {
    long asks = 0;
    /* No trunk scan: the pass runs whole. */
    const long whole = dead_air_return_symbols(4800, 0, &asks);
    assert(whole >= DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS);

    /* 250 ms at 4800 sym/s is 1200 symbols, a multiple of the 300-symbol ask. */
    const long early = dead_air_return_symbols(4800, 250, &asks);
    if (early < 1200 - 1 || early > 1200 + 300) {
        DSD_FPRINTF(stderr, "FAIL #717: a 250 ms dwell at 4800 sym/s handed back after %ld symbols (want 1200..1500)\n",
                    early);
    }
    assert(early >= 1200 - 1 && early <= 1200 + 300);
    assert(asks >= 1 && asks <= early / 300 + 1);

    /* 400 ms is 1920 symbols, past one pass: the pass's own return comes first, and the hook was asked, never due. */
    const long past_pass = dead_air_return_symbols(4800, 400, &asks);
    assert(past_pass >= DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS && past_pass < DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS + 300);
    assert(!g_dwell_yielded);
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
static long g_no_carrier_at = 0; // symbols emitted at the first no-carrier pass since the order was reset

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
    if (g_no_carrier_at == 0) {
        g_no_carrier_at = g_symbols_emitted;
    }
}

/* #392: a sync the engine declined to dispatch consumes nothing, so the credit path refuses
 * it at its size floor and the search that found it stands charged with nothing to pay it
 * back. Trunked DMR skips the MS paths outright and a retune in flight skips processFrame()
 * entirely, so those charges accumulated until the hunt rotated the profile off a channel
 * the engine had just tuned. */
static void
test_withheld_syncs_do_not_rotate_a_profile(void) {
    static const int withheld_only[] = {DSD_FRAME_VERDICT_WITHHELD};
    const HuntCase tc = {
        .label = "withheld syncs at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 284,
        .handler_symbols = 0, /* the gate ran no handler, so nothing was consumed */
        .verdict_cycle = withheld_only,
        .verdict_cycle_len = 1,
        .symbol_budget = 400000,
        .expect_step = 0,
    };
    const HuntResult r = drive_hunt(&tc);

    /* Many dwells' worth of searching, every cycle of it withheld, and the profile the
     * engine chose is still in force. */
    assert(r.syncs_seen > 100);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
}

/* The refund is neutrality, not credit: it erases the search that produced the withheld
 * frame and nothing else, so it cannot bank a reserve. Interleaving withheld frames with
 * unproductive ones leaves the unproductive halves accumulating exactly as before, and a
 * profile matching nothing is still given up -- the #388 direction this must not re-open. */
static void
test_withheld_syncs_do_not_hold_a_wrong_profile(void) {
    static const int withheld_then_unproductive[] = {DSD_FRAME_VERDICT_WITHHELD, DSD_FRAME_VERDICT_UNPRODUCTIVE};
    const HuntCase tc = {
        .label = "withheld mixed with unproductive at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 284,
        .handler_symbols = 8,
        .verdict_cycle = withheld_then_unproductive,
        .verdict_cycle_len = 2,
        .symbol_budget = 400000,
        .expect_step = 1,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 10);
    assert_auto_stepped_to_2400_4(&r);
}

/* A trunked voice channel the engine tuned on a grant. */
static void
configure_tuned_vc(dsd_opts* opts, dsd_state* state) {
    (void)state;
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
}

/* #392, the other half: the profile on a tuned voice channel came with the grant that tuned
 * it, so the hunt holds it however the budget reads. A call fading toward the noise floor
 * credits less than the search between its syncs burns and reaches the dwell while it is
 * still decoding; rotating there takes the channel's timing away mid-call.
 *
 * This is test_unproductive_frames_never_buy_dwell()'s shape with one flag changed, so what
 * it pins is the flag and nothing else: the same stream rotates when the tuner is not parked
 * on a voice channel. */
static void
test_a_tuned_voice_channel_holds_its_profile(void) {
    const HuntCase tc = {
        .label = "tuned VC at 4800/4",
        .marker = FALSE_SYNC_MARKER,
        .marker_gap = 284,
        .handler_symbols = 8,
        .handler_unproductive = 1,
        .symbol_budget = 400000,
        .expect_step = 0,
        .configure = configure_tuned_vc,
    };
    const HuntResult r = drive_hunt(&tc);

    assert(r.syncs_seen > 100);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
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
 * Markers every 8 symbols keep the 1800-symbol timeout from ever arming, so the budget
 * exit is the only path that can run them here.
 *
 * #392 holds the profile on a tuned voice channel, so this no longer rotates -- but the
 * accounting is exactly what it must not take with it. These hooks are what gives a dead
 * voice channel up; without them nothing would clear trunk_is_tuned and the hold would
 * never end. */
static void
test_budget_exit_runs_the_no_sync_hooks(void) {
    g_hook_order = 0;
    g_vc_no_sync_order = 0;
    g_release_order = 0;
    g_no_carrier_order = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
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
        .expect_step = 0,
        .configure = configure_tuned_vc_past_hangtime,
    };
    const HuntResult r = drive_hunt(&tc);
    dsd_frame_sync_hooks_set(NULL);

    /* The profile the engine tuned is still the profile in force. */
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
    assert(g_vc_no_sync_order > 0);
    assert(g_release_order > g_vc_no_sync_order);
    assert(g_no_carrier_order > g_release_order);
}

/* Issue #834: a scan quiet check's in-hunt service, faked as the engine's. It answers 1 (hand back) from the first
   300-symbol point at or after g_quiet_ready_at symbols, but only while the scan-owner sync sequence the engine bumps
   at every returned sync (g_quiet_sync_seq) has not moved since its request (g_quiet_sync_at_request): the engine's
   rule (scan_quiet.c, ENGINE_TRUNK_SCAN). */
static long g_quiet_ready_at = 0;
static long g_quiet_asks = 0;
static long g_quiet_handbacks = 0;
static long g_quiet_sync_seq = 0;
static long g_quiet_sync_at_request = 0;

static int
fake_scan_quiet_service(const dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
    g_quiet_asks++;
    if (g_symbols_emitted < g_quiet_ready_at || g_quiet_sync_seq != g_quiet_sync_at_request) {
        return 0;
    }
    g_quiet_handbacks++;
    return 1;
}

static void
quiet_service_install(long ready_at, int skip_silent, dsd_opts* opts) {
    g_quiet_ready_at = ready_at;
    g_quiet_asks = g_quiet_handbacks = 0;
    g_quiet_sync_seq = g_quiet_sync_at_request = 0;
    g_hook_order = g_vc_no_sync_order = g_release_order = g_no_carrier_order = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
        .p25_sm_release = fake_p25_sm_release,
        .p25_sm_vc_no_sync = fake_p25_sm_vc_no_sync,
        .no_carrier = fake_no_carrier,
        .scan_quiet_service = fake_scan_quiet_service,
    });
    opts->scan_skip_silent = skip_silent;
}

/* Issue #834: a READY quiet result hands the hunt back within one 300-symbol point, as a plain no-sync return
   (DSD_SYNC_NONE, not the pass end's -1) that runs none of the no-sync hooks, the way the trunk-scan dwell does; the
   engine loop then ticks and runs noCarrier(). With skip-silent off the service is never asked and the pass runs whole.
 */
static void
test_a_quiet_result_hands_back_within_one_point(void) {
    static dsd_opts opts;
    static dsd_state state;
    for (int skip_silent = 1; skip_silent >= 0; skip_silent--) {
        init_hunt_opts_state(&opts, &state, 0);
        assert(init_state_buffers(&state));
        stream_reset("", 0);
        dsd_frame_sync_reset_mod_state();
        quiet_service_install(700, skip_silent, &opts);
        const int sync = getFrameSync(&opts, &state);
        const long read = g_symbols_emitted;
        dsd_frame_sync_hooks_set(NULL);
        free_state_buffers(&state);
        if (skip_silent) {
            /* Ready after 700 symbols: the point at 900 (the 899th read) hands back. */
            if (sync != DSD_SYNC_NONE || read < 700 - 1 || read > 700 + 300 || g_quiet_handbacks != 1) {
                DSD_FPRINTF(stderr, "FAIL #834: quiet hand-back returned %d after %ld symbols (%ld hand-backs)\n", sync,
                            read, g_quiet_handbacks);
            }
            assert(sync == DSD_SYNC_NONE);
            assert(read >= 700 - 1 && read <= 700 + 300);
            assert(g_quiet_handbacks == 1 && g_quiet_asks == read / 300 + 1);
            assert(g_vc_no_sync_order == 0 && g_release_order == 0 && g_no_carrier_order == 0);
        } else {
            assert(sync == -1);
            assert(read >= DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS);
            assert(g_quiet_asks == 0 && g_quiet_handbacks == 0);
        }
    }
}

/* Issue #834: no hand-back once a sync moved the sequence. Syncs arrive inside the hunt while a quiet result would
   otherwise be ready; each returned sync is bumped as engine.c's live_scanner_frame_sync() does, so every later
   300-symbol point is a no, and the hunt keeps returning its syncs. */
static void
test_the_hunt_returns_its_syncs_once_the_quiet_service_answers_no(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_hunt_opts_state(&opts, &state, 0);
    assert(init_state_buffers(&state));
    stream_reset(FALSE_SYNC_MARKER, 400);
    dsd_frame_sync_reset_mod_state();
    quiet_service_install(0, 1, &opts);
    int syncs = 0;
    while (g_symbols_emitted < 6000) {
        const int sync = getFrameSync(&opts, &state);
        if (sync > DSD_SYNC_NONE) {
            syncs++;
            g_quiet_sync_seq++; /* the engine's bump */
        }
    }
    dsd_frame_sync_hooks_set(NULL);
    free_state_buffers(&state);
    /* The first point (300) handed back before the first marker completed; none after the first sync. */
    assert(syncs > 1);
    assert(g_quiet_handbacks == 1);
    assert(g_quiet_asks > g_quiet_handbacks);
}

/* Issue #634: a hunt read that left a silent input's wait for a queued command is no symbol and no pass. getFrameSync()
   returns at once, reads nothing more, runs no no-sync accounting and steps no profile, so the hunt resumes where it
   stood once the engine has applied the command. */
static void
test_an_interrupted_read_returns_at_once_and_counts_nothing(void) {
    static dsd_opts opts;
    static dsd_state state;
    init_hunt_opts_state(&opts, &state, 0);
    const int buffers = init_state_buffers(&state);
    assert(buffers);
    stream_reset("", 0);
    dsd_frame_sync_reset_mod_state();
    g_hook_order = 0;
    g_vc_no_sync_order = 0;
    g_release_order = 0;
    g_no_carrier_order = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){
        .p25_sm_release = fake_p25_sm_release,
        .p25_sm_vc_no_sync = fake_p25_sm_vc_no_sync,
        .no_carrier = fake_no_carrier,
    });

    const int idx = state.sps_hunt_idx;
    const int sps = state.samplesPerSymbol;
    const uint32_t symbolcnt = state.symbolcnt;
    g_reads = 0;
    g_interrupt_at_read = 5;
    assert(getFrameSync(&opts, &state) == DSD_SYNC_NONE);
    assert(state.input_interrupted == 1);
    /* Four symbols read, the fifth read interrupted, none after it. */
    assert(g_reads == 5);
    assert(state.symbolcnt == symbolcnt + 4);
    assert(state.sps_hunt_idx == idx && state.samplesPerSymbol == sps);
    assert(g_vc_no_sync_order == 0 && g_release_order == 0 && g_no_carrier_order == 0);

    dsd_frame_sync_hooks_set(NULL);
    g_interrupt_at_read = 0;
    state.input_interrupted = 0;
}

/*
 * A keyed C4FM P25p1 carrier read from a binary profile (frame_sync_p25p1_word_hint_due()): its sync word on D-STAR's
 * 4800/2 (4800/4's timing), or read on each symbol's first or second reading on ProVoice's 9600/2 (twice the rate),
 * twice within a P25p1 frame, moves the unlocked hunt to 4800/4 at once, where it waited out the dwell and the rotation
 * behind it (9600/2 -> 6000/4 -> 4800/2 -> 4800/4, about 2.6 s). Not a no-sync exit: no no-carrier pass runs for it.
 * Only the unlocked rotation with P25p1 on takes it, only the exact word, and only a second one within the frame.
 */
static char g_p25_word_doubled[49];
static char g_p25_word_delayed[49];
static char g_p25_word_near_miss[25];
/* A word of each polarity in turn, 524 symbols apart: a pair of one polarity is 1048 apart, more than a frame. */
static char g_p25_words_alternating[24 + 500 + 24 + 1];
static int g_probe_in_flight;

static int
fake_p25_probe_in_flight(const dsd_opts* opts, const dsd_state* state) {
    (void)opts;
    (void)state;
    return g_probe_in_flight;
}

static void
build_p25_word_markers(void) {
    for (size_t i = 0; i < 24U; i++) {
        g_p25_word_doubled[2U * i] = P25P1_SYNC[i];
        g_p25_word_doubled[(2U * i) + 1U] = P25P1_SYNC[i];
        /* Each symbol's first reading on the other side, as a 9600/2 grid that sits on the transitions reads it. */
        g_p25_word_delayed[2U * i] = i > 0U ? P25P1_SYNC[i - 1U] : '1';
        g_p25_word_delayed[(2U * i) + 1U] = P25P1_SYNC[i];
    }
    g_p25_word_doubled[48] = '\0';
    g_p25_word_delayed[48] = '\0';
    DSD_MEMCPY(g_p25_word_near_miss, INV_P25P1_SYNC, sizeof(g_p25_word_near_miss));
    g_p25_word_near_miss[11] = g_p25_word_near_miss[11] == '1' ? '3' : '1';
    DSD_MEMSET(g_p25_words_alternating, '1', sizeof(g_p25_words_alternating) - 1U);
    DSD_MEMCPY(g_p25_words_alternating, P25P1_SYNC, 24U);
    DSD_MEMCPY(g_p25_words_alternating + 24 + 500, INV_P25P1_SYNC, 24U);
    g_p25_words_alternating[sizeof(g_p25_words_alternating) - 1U] = '\0';
}

static void
configure_on_4800_2(dsd_opts* opts, dsd_state* state) {
    frame_sync_apply_sps_hunt_profile(opts, state, DSD_FRAME_SYNC_SPS_PROFILE_4800_2, 0);
}

static void
configure_on_9600_2(dsd_opts* opts, dsd_state* state) {
    frame_sync_apply_sps_hunt_profile(opts, state, DSD_FRAME_SYNC_SPS_PROFILE_9600_2, 0);
}

static void
configure_on_9600_2_p25p1_off(dsd_opts* opts, dsd_state* state) {
    opts->frame_p25p1 = 0;
    configure_on_9600_2(opts, state);
}

static void
configure_on_9600_2_search_order(dsd_opts* opts, dsd_state* state) {
    configure_on_9600_2(opts, state);
    state->sps_hunt_search_order = 1U; // a Signal search probe's order, sampled as the profile was adopted
}

static void
configure_on_9600_2_trunk_scan(dsd_opts* opts, dsd_state* state) {
    opts->trunk_scan_enabled = 1;
    configure_on_9600_2(opts, state);
}

/* A word one polarity took on 4800/2 a frame's span before 9600/2's first word: neither the profile nor (in the second
   variant) the polarity is the next word's, so that word pairs with nothing. */
static void
configure_on_9600_2_after_4800_2_word(dsd_opts* opts, dsd_state* state) {
    configure_on_9600_2(opts, state);
    state->p25p1_word_hint_seen = 1U;
    state->p25p1_word_hint_type = DSD_SYNC_P25P1_POS;
    state->p25p1_word_hint_profile = DSD_FRAME_SYNC_SPS_PROFILE_4800_2;
    state->p25p1_word_hint_symbol = state->symbolcnt + 100U;
}

static void
configure_on_9600_2_after_inverted_word(dsd_opts* opts, dsd_state* state) {
    configure_on_9600_2(opts, state);
    state->p25p1_word_hint_seen = 1U;
    state->p25p1_word_hint_type = DSD_SYNC_P25P1_NEG;
    state->p25p1_word_hint_profile = DSD_FRAME_SYNC_SPS_PROFILE_9600_2;
    state->p25p1_word_hint_symbol = state->symbolcnt + 100U;
}

static void
configure_on_9600_2_tuned_vc(dsd_opts* opts, dsd_state* state) {
    configure_on_9600_2(opts, state);
    opts->trunk_enable = 1;
    opts->trunk_is_tuned = 1;
}

/* The C4FM word read on a binary profile: the hunt is on 4800/4 within a symbol of the end of the @p nth word, the
 * second of a pair a P25p1 frame apart at most, with no no-sync exit in between. */
static void
expect_p25_word_hint_at(const char* label, const char* marker, void (*configure)(dsd_opts*, dsd_state*), long nth) {
    g_hook_order = 0;
    g_no_carrier_order = 0;
    g_no_carrier_at = 0;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){.no_carrier = fake_no_carrier});
    const int gap = 300;
    const HuntCase tc = {
        .label = label,
        .marker = marker,
        .marker_gap = gap,
        .symbol_budget = 40000,
        .expect_step = 1,
        .configure = configure,
    };
    const HuntResult r = drive_hunt(&tc);
    dsd_frame_sync_hooks_set(NULL);

    /* The doubled word reads whole on its first readings a symbol before its end. */
    const long word_end = nth * (gap + (long)strlen(marker));
    if (r.final_idx != DSD_FRAME_SYNC_SPS_PROFILE_4800_4 || r.left_at < word_end - 1 || r.left_at > word_end) {
        DSD_FPRINTF(stderr, "FAIL %s: left for profile %d after %ld symbols (want 4800/4 after %ld)\n", label,
                    r.final_idx, r.left_at, word_end);
    }
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
    assert(r.left_at >= word_end - 1 && r.left_at <= word_end);
    /* A step, not a no-sync exit: no no-carrier pass came before it. */
    assert(g_no_carrier_order == 0 || g_no_carrier_at > r.left_at);
}

static void
expect_p25_word_hint(const char* label, const char* marker, void (*configure)(dsd_opts*, dsd_state*)) {
    expect_p25_word_hint_at(label, marker, configure, 2L);
}

/* Nothing moves the hunt before its dwell is spent: the words, @p gap apart, are not taken here. */
static void
expect_no_p25_word_hint(const char* label, const char* marker, int gap, void (*configure)(dsd_opts*, dsd_state*),
                        int mod_cli_lock) {
    const HuntCase tc = {
        .label = label,
        .marker = marker,
        .marker_gap = gap,
        .symbol_budget = 40000,
        .expect_step = 1,
        .mod_cli_lock = mod_cli_lock,
        .configure = configure,
    };
    const HuntResult r = drive_hunt(&tc);
    const long dwell_symbols = (long)DSD_FRAME_SYNC_NO_SYNC_PASS_SYMBOLS * 3;
    if (r.symbols_at_step < dwell_symbols) {
        DSD_FPRINTF(stderr, "FAIL %s: stepped to profile %d after %ld symbols, inside the dwell\n", label, r.final_idx,
                    r.symbols_at_step);
    }
    assert(r.symbols_at_step >= dwell_symbols);
}

static void
test_a_p25p1_word_on_a_binary_profile_steps_to_4800_4(void) {
    build_p25_word_markers();
    expect_p25_word_hint("P25p1 word on 4800/2", P25P1_SYNC, configure_on_4800_2);
    expect_p25_word_hint("inverted P25p1 word on 4800/2", INV_P25P1_SYNC, configure_on_4800_2);
    expect_p25_word_hint("P25p1 word doubled on 9600/2", g_p25_word_doubled, configure_on_9600_2);
    expect_p25_word_hint("P25p1 word on its second readings on 9600/2", g_p25_word_delayed, configure_on_9600_2);
    /* The word before is another profile's, or of the other polarity: the first word here pairs with nothing. */
    expect_p25_word_hint("first word after a 4800/2 word", g_p25_word_doubled, configure_on_9600_2_after_4800_2_word);
    expect_p25_word_hint("first word after an inverted word", g_p25_word_doubled,
                         configure_on_9600_2_after_inverted_word);
    /* A carrier change between two words drops the first (dsd_state_forget_sync_join_histories()): the second pairs
       with nothing, the third with the second. */
    g_forget_histories_at = 500;
    expect_p25_word_hint_at("words across a carrier change", g_p25_word_doubled, configure_on_9600_2, 3L);
    g_forget_histories_at = 0;
}

static void
test_a_p25p1_word_hint_is_the_unlocked_rotations_alone(void) {
    build_p25_word_markers();
    /* One symbol off the word is no word. */
    expect_no_p25_word_hint("near-miss word on 4800/2", g_p25_word_near_miss, 300, configure_on_4800_2, 0);
    /* Words of the other polarity are no pair, however near. */
    expect_no_p25_word_hint("words of each polarity in turn on 4800/2", g_p25_words_alternating, 500,
                            configure_on_4800_2, 0);
    /* A word at 9600/2's own rate is not a C4FM carrier's. */
    expect_no_p25_word_hint("undoubled word on 9600/2", P25P1_SYNC, 300, configure_on_9600_2, 0);
    /* Words more than a P25p1 frame apart are each alone: one word is a 2^-24 event on any signal. */
    expect_no_p25_word_hint("words frames apart on 4800/2", P25P1_SYNC, 1000, configure_on_4800_2, 0);
    expect_no_p25_word_hint("doubled words frames apart on 9600/2", g_p25_word_doubled, 1800, configure_on_9600_2, 0);
    expect_no_p25_word_hint("-m lock on 4800/2", P25P1_SYNC, 300, configure_on_4800_2, 1);
    expect_no_p25_word_hint("P25p1 off on 9600/2", g_p25_word_doubled, 300, configure_on_9600_2_p25p1_off, 0);
    expect_no_p25_word_hint("Signal search order on 9600/2", g_p25_word_doubled, 300, configure_on_9600_2_search_order,
                            0);
    expect_no_p25_word_hint("trunk scan on 9600/2", g_p25_word_doubled, 300, configure_on_9600_2_trunk_scan, 0);

    g_probe_in_flight = 1;
    dsd_frame_sync_hooks_set(&(dsd_frame_sync_hooks){.p25_probe_in_flight = fake_p25_probe_in_flight});
    expect_no_p25_word_hint("P25 probe on 9600/2", g_p25_word_doubled, 300, configure_on_9600_2, 0);
    dsd_frame_sync_hooks_set(NULL);
    g_probe_in_flight = 0;

    /* A trunked voice channel the engine tuned holds its profile whatever the word says. */
    const HuntCase tuned = {
        .label = "tuned voice channel on 9600/2",
        .marker = g_p25_word_doubled,
        .marker_gap = 300,
        .symbol_budget = 20000,
        .expect_step = 0,
        .configure = configure_on_9600_2_tuned_vc,
    };
    const HuntResult r = drive_hunt(&tuned);
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_9600_2);
}

int
main(void) {
    test_an_interrupted_read_returns_at_once_and_counts_nothing();
    test_false_syncs_do_not_starve_the_hunt();
    test_dense_false_syncs_do_not_starve_the_hunt();
    test_sub_frame_consumption_never_buys_dwell();
    test_consumed_frames_hold_the_profile();
    test_unproductive_frames_never_buy_dwell();
    test_a_dpmr_carrier_that_decodes_nothing_still_rotates();
    test_a_decoding_dpmr_carrier_holds_its_profile();
    test_no_verdict_handlers_still_rotate_at_the_noise_cadence();
    test_a_confirming_transmission_holds_its_profile();
    test_a_proven_verdict_holds_the_profile_through_failure_runs();
    test_a_proven_verdict_buys_one_dwell_and_no_more();
    test_a_verdict_is_read_once_and_cleared();
    test_idle_rotation_period_is_unchanged();
    test_a_trunk_scan_dwell_returns_inside_the_pass();
    test_a_quiet_result_hands_back_within_one_point();
    test_the_hunt_returns_its_syncs_once_the_quiet_service_answers_no();
    test_locked_modulation_rotates_within_equal_timing();
    test_withheld_syncs_do_not_rotate_a_profile();
    test_withheld_syncs_do_not_hold_a_wrong_profile();
    test_a_tuned_voice_channel_holds_its_profile();
    test_budget_exit_runs_the_no_sync_hooks();
    test_a_p25p1_word_on_a_binary_profile_steps_to_4800_4();
    test_a_p25p1_word_hint_is_the_unlocked_rotations_alone();
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
