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
        .expect_step = 0,
        .configure = configure_tuned_vc_past_hangtime,
    };
    const HuntResult r = drive_hunt(&tc);
    dsd_frame_sync_hooks_set((dsd_frame_sync_hooks){0});

    /* The profile the engine tuned is still the profile in force. */
    assert(r.final_idx == DSD_FRAME_SYNC_SPS_PROFILE_4800_4);
    assert(r.final_sps == HUNT_SPS_4800);
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
    test_a_dpmr_carrier_that_decodes_nothing_still_rotates();
    test_a_decoding_dpmr_carrier_holds_its_profile();
    test_no_verdict_handlers_still_rotate_at_the_noise_cadence();
    test_a_confirming_transmission_holds_its_profile();
    test_a_proven_verdict_holds_the_profile_through_failure_runs();
    test_a_proven_verdict_buys_one_dwell_and_no_more();
    test_a_verdict_is_read_once_and_cleared();
    test_idle_rotation_period_is_unchanged();
    test_locked_modulation_rotates_within_equal_timing();
    test_withheld_syncs_do_not_rotate_a_profile();
    test_withheld_syncs_do_not_hold_a_wrong_profile();
    test_a_tuned_voice_channel_holds_its_profile();
    test_budget_exit_runs_the_no_sync_hooks();
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
