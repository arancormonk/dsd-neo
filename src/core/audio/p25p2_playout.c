// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 Phase 2 voice playout: per-slot queues played per burst pair, in both output formats (issue #651).
 *
 * Timing model. Within a physical superframe each slot carries exactly 18 frames: four 4V and one 2V across its five
 * voice pairs (0-4), and a SACCH at pair 5. Which pair carries the 2V depends on when the transmission started, so
 * the two slots' bursts are out of step by up to two frames. Each slot queues its frames in order; after slot 2's
 * burst of a pair the mixer pops, per block, one frame from every slot, as many blocks as every open stream has
 * ready. A slot that runs ahead keeps the difference as carry (at most two frames in steady state), so neither slot
 * is ever played early, late or twice.
 *
 * A voice burst an open stream misses (DUID error, FACCH in its place, a timeslot skipped by an abort or a missed
 * sync) queues silence in its position: 2 frames at the slot's 2V pair and 4 elsewhere. The 2V pair is proven by a
 * decoded 2V or by 4V at the other four pairs. Until then the size of a missing pair that could be the 2V is not
 * known: in the stream's first superframe covered from pair 0 it is held as a placeholder, which blocks that slot
 * (and the companion through the min rule) until the superframe ends; afterwards the pair the first superframe gave
 * 2 is provisional and fills at once. A fill that turns out mis-sized becomes debt, repaid only through fill, never
 * by dropping decoded audio.
 *
 * Output verdict. A burst's verdict (talkgroup block and hold, crypto output permission, static-WAV recordability) is
 * fixed when its first frame is queued, so a tail played after END keeps its own call's verdict. While the frame's
 * call is still active (same canonical epoch), emission also requires the slot's current talkgroup verdict, as the
 * mixers' emission-time gates did, and after the call ended the last one taken while it was: a Skip, lockout or policy
 * edit mutes what was queued before it, also in the drain when the release leaves the channel. The current verdict is
 * the decode gate's decision for the call's latest burst, or the one a path that changes verdicts or ends the calls
 * took (dsd_p25p2_playout_note_policy()); emission only checks, cheaply, whether anything a waiting call's verdict
 * reads moved (dsd_p25p2_policy_key) and takes its verdict again then. The slot switches are read at emission. Float frames are gained
 * (agf) when queued.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd_audio_internal.h"

enum {
    ENTRY_FRAME = DSD_P25P2_PLAYOUT_ENTRY_FRAME,
    ENTRY_FILL = DSD_P25P2_PLAYOUT_ENTRY_FILL,
    ENTRY_PENDING = DSD_P25P2_PLAYOUT_ENTRY_PENDING,
    ENTRY_SKIP = DSD_P25P2_PLAYOUT_ENTRY_SKIP,
    ENTRY_BARRIER = DSD_P25P2_PLAYOUT_ENTRY_BARRIER,
};

enum {
    VOICE_PAIRS = 5, /* pairs 0-4 carry voice; pair 5 is the SACCH */
    SUPERFRAME_FRAMES = 18,
    MISSED_CLOSE = 5, /* consecutive missed voice bursts that close a stream: one superframe */
    QUEUE_FORCE = 20, /* a queue past this many entries plays out regardless */
    DEBT_LIMIT = 8,
    BLOCK_FRAMES = 160,
};

static dsd_p25p2_playout_slot*
slot_of(dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return NULL;
    }
    return &state->p25p2_playout.slot[slot];
}

static dsd_p25p2_playout_entry*
q_at(dsd_p25p2_playout_slot* s, int i) {
    return &s->q[(s->head + i) % DSD_P25P2_PLAYOUT_CAP];
}

static const dsd_p25p2_playout_entry*
q_at_const(const dsd_p25p2_playout_slot* s, int i) {
    return &s->q[(s->head + i) % DSD_P25P2_PLAYOUT_CAP];
}

// Entries still to play: everything but removed ones.
static int
q_live(const dsd_p25p2_playout_slot* s) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        if (q_at_const(s, i)->kind != ENTRY_SKIP) {
            n++;
        }
    }
    return n;
}

// Whether pair clock @p a comes before @p b (the clock wraps).
static int
clock_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

// The next entry still to play in @p s from queue position @p *pos on (removed entries skipped), or NULL.
static const dsd_p25p2_playout_entry*
q_next_live(const dsd_p25p2_playout_slot* s, int* pos) {
    while (*pos < s->count && q_at_const(s, *pos)->kind == ENTRY_SKIP) {
        (*pos)++;
    }
    return (*pos < s->count) ? q_at_const(s, *pos) : NULL;
}

enum { LEVEL_STOP = -1 };

// One step of slot_level() with the slot at its barrier @p e beside the companion's next entry @p c, advancing both
// queue positions and the block count as play_blocks() would. LEVEL_STOP when an unresolved placeholder of the
// companion decides how long the slot waits.
static int
level_step_at_barrier(const dsd_p25p2_playout_entry* e, const dsd_p25p2_playout_entry* c, int* pos, int* opos, int* n) {
    if (c && c->kind == ENTRY_BARRIER) {
        // Two barriers resolve in pair-clock order, as settle_barriers() does: the earlier goes (both on a tie).
        *pos += !clock_before(c->clock, e->clock);
        *opos += !clock_before(e->clock, c->clock);
        return 0;
    }
    if (c && clock_before(c->clock, e->clock)) {
        if (c->kind == ENTRY_PENDING) {
            return LEVEL_STOP;
        }
        // The slot sits out the block while the companion plays an entry of an earlier pair.
        (*n)++;
        (*opos)++;
        return 0;
    }
    (*pos)++; // nothing earlier to wait out: the barrier goes
    return 0;
}

// Blocks slot @p idx can fill, beside the companion as play_blocks() pairs their entries: its entries ahead of its
// first unresolved placeholder, and at a barrier the blocks it sits out while the companion plays entries of earlier
// pairs (as far as the companion's own first unresolved placeholder). A companion at its barrier sits out while the
// slot plays entries of earlier pairs.
static int
slot_level(const dsd_p25p2_playout* p, int idx) {
    const dsd_p25p2_playout_slot* s = &p->slot[idx];
    const dsd_p25p2_playout_slot* o = &p->slot[idx ^ 1];
    int pos = 0;
    int opos = 0;
    int n = 0;
    for (;;) {
        const dsd_p25p2_playout_entry* e = q_next_live(s, &pos);
        const dsd_p25p2_playout_entry* c = q_next_live(o, &opos);
        if (!e || e->kind == ENTRY_PENDING) {
            return n;
        }
        if (e->kind == ENTRY_BARRIER) {
            if (level_step_at_barrier(e, c, &pos, &opos, &n) == LEVEL_STOP) {
                return n;
            }
            continue;
        }
        if (c && c->kind == ENTRY_BARRIER && !clock_before(e->clock, c->clock)) {
            opos++; // the companion's barrier goes once the slot plays no earlier pair
            continue;
        }
        n++;
        pos++;
        if (c && c->kind != ENTRY_PENDING && c->kind != ENTRY_BARRIER) {
            opos++; // the companion plays beside it
        }
    }
}

/* Alerts (beeper()) raised for a slot while its queue still holds audio wait behind that audio, so an end-of-call
   alert follows the call's last frames rather than cutting in ahead of them. They can be raised on any thread, outside
   the tick guard too (a held VOICE_END sounds from the frame-sync pass), so the entry counts they compare and the
   alert queues sit behind a lock of their own. */
static dsd_mutex_t g_alert_mutex;
static atomic_int g_alert_mutex_state = 0; /* 0=uninit, 1=initing, 2=init */

static void
alert_lock(void) {
    if (atomic_load(&g_alert_mutex_state) != 2) {
        int expected = 0;
        if (atomic_compare_exchange_strong(&g_alert_mutex_state, &expected, 1)) {
            (void)dsd_mutex_init(&g_alert_mutex);
            atomic_store(&g_alert_mutex_state, 2);
        }
        while (atomic_load(&g_alert_mutex_state) != 2) {
            dsd_thread_yield();
        }
    }
    (void)dsd_mutex_lock(&g_alert_mutex);
}

static void
alert_unlock(void) {
    (void)dsd_mutex_unlock(&g_alert_mutex);
}

// @p n entries left @p s (played, removed or discarded).
static void
note_departed(dsd_p25p2_playout_slot* s, unsigned int n) {
    if (n == 0U) {
        return;
    }
    alert_lock();
    s->departed += n;
    alert_unlock();
}

// Drop removed entries from the head, so they never hold capacity.
static void
q_compact_head(dsd_p25p2_playout_slot* s) {
    unsigned int removed = 0U;
    while (s->count > 0 && s->q[s->head].kind == ENTRY_SKIP) {
        s->head = (uint8_t)((s->head + 1) % DSD_P25P2_PLAYOUT_CAP);
        s->count--;
        removed++;
    }
    note_departed(s, removed);
}

// Drop removed entries from anywhere in the queue, keeping the order of the rest: a resolved placeholder or repaid
// fill leaves them behind entries still to play.
static void
q_compact(dsd_p25p2_playout_slot* s) {
    int kept = 0;
    for (int i = 0; i < s->count; i++) {
        const dsd_p25p2_playout_entry* e = q_at_const(s, i);
        if (e->kind == ENTRY_SKIP) {
            continue;
        }
        if (kept != i) {
            *q_at(s, kept) = *e;
        }
        kept++;
    }
    note_departed(s, (unsigned int)(s->count - kept));
    s->count = (uint8_t)kept;
}

static dsd_p25p2_playout_entry*
q_push(dsd_p25p2_playout_slot* s) {
    q_compact_head(s);
    if (s->count >= DSD_P25P2_PLAYOUT_CAP) {
        q_compact(s);
    }
    if (s->count >= DSD_P25P2_PLAYOUT_CAP) {
        return NULL;
    }
    dsd_p25p2_playout_entry* e = q_at(s, s->count);
    s->count++;
    DSD_MEMSET(e, 0, sizeof(*e));
    alert_lock();
    s->pushed++;
    e->seq = s->pushed;
    alert_unlock();
    return e;
}

// Pop the next entry ready to play into @p out. 0 when none is ready (or a barrier is next). The caller counts it gone
// (note_departed()) once its block is out.
static int
q_pop(dsd_p25p2_playout_slot* s, dsd_p25p2_playout_entry* out) {
    q_compact_head(s);
    if (s->count == 0 || s->q[s->head].kind == ENTRY_PENDING || s->q[s->head].kind == ENTRY_BARRIER) {
        return 0;
    }
    *out = s->q[s->head];
    s->head = (uint8_t)((s->head + 1) % DSD_P25P2_PLAYOUT_CAP);
    s->count--;
    return 1;
}

static int
slot_call_epoch(const dsd_state* state, int slot, uint64_t* epoch) {
    dsd_call_snapshot call;
    if (dsd_call_state_get(state, (uint8_t)slot, &call) <= 0 || call.phase != DSD_CALL_PHASE_ACTIVE) {
        return 0;
    }
    *epoch = call.epoch;
    return 1;
}

static int
popcount5(unsigned int v) {
    int n = 0;
    for (int i = 0; i < VOICE_PAIRS; i++) {
        n += (int)((v >> i) & 1U);
    }
    return n;
}

static unsigned int
candidates_of(const dsd_p25p2_playout_slot* s) {
    return (~(unsigned int)s->seen_4v_mask) & 0x1FU;
}

static int
last_candidate(unsigned int candidates) {
    for (int p = VOICE_PAIRS - 1; p >= 0; p--) {
        if ((candidates >> p) & 1U) {
            return p;
        }
    }
    return -1;
}

static void
clamp_debt(dsd_p25p2_playout_slot* s, int debt) {
    if (debt > DEBT_LIMIT) {
        debt = DEBT_LIMIT;
    } else if (debt < -DEBT_LIMIT) {
        debt = -DEBT_LIMIT;
    }
    s->fill_debt = (int16_t)debt;
}

// Repay positive debt from fill still queued, newest first: it has not played, so removing it only takes back the
// silence that was too long.
static void
repay_from_queued_fill(dsd_p25p2_playout_slot* s) {
    for (int i = s->count - 1; i >= 0 && s->fill_debt > 0; i--) {
        dsd_p25p2_playout_entry* e = q_at(s, i);
        if (e->kind == ENTRY_FILL) {
            e->kind = ENTRY_SKIP;
            s->fill_debt--;
        }
    }
}

// The number of fill frames to queue for a missing burst assigned @p size frames, after debt: positive debt shrinks
// it, negative debt lengthens it by up to @p max_extra.
static int
fill_after_debt(dsd_p25p2_playout_slot* s, int size, int max_extra) {
    if (s->fill_debt > 0) {
        const int repay = (s->fill_debt < size) ? s->fill_debt : size;
        clamp_debt(s, s->fill_debt - repay);
        return size - repay;
    }
    if (s->fill_debt < 0) {
        int extra = -s->fill_debt;
        if (extra > max_extra) {
            extra = max_extra;
        }
        clamp_debt(s, s->fill_debt + extra);
        return size + extra;
    }
    return size;
}

static void
push_fill(dsd_p25p2_playout_slot* s, int frames, uint32_t clock) {
    for (int i = 0; i < frames; i++) {
        dsd_p25p2_playout_entry* e = q_push(s);
        if (!e) {
            return;
        }
        e->kind = ENTRY_FILL;
        e->clock = clock;
        e->epoch = s->epoch;
        e->verdict = s->verdict;
    }
}

// Resolve the placeholder for @p pair to @p size frames (before debt).
static void
resolve_placeholder(dsd_p25p2_playout_slot* s, int first, int size) {
    const int frames = fill_after_debt(s, size, 4 - size);
    for (int i = 0; i < 4; i++) {
        dsd_p25p2_playout_entry* e = q_at(s, first + i);
        if (i < frames) {
            e->kind = ENTRY_FILL;
            e->fresh = 0U;
        } else {
            e->kind = ENTRY_SKIP;
        }
    }
}

// The index of the first entry of each unresolved placeholder, in queue order; returns how many.
static int
find_placeholders(dsd_p25p2_playout_slot* s, int* first, int max) {
    int n = 0;
    for (int i = 0; i < s->count && n < max;) {
        if (q_at(s, i)->kind == ENTRY_PENDING) {
            first[n++] = i;
            i += 4;
        } else {
            i++;
        }
    }
    return n;
}

// Resolve every placeholder now that the 2V pair is proven: exact sizes.
static void
resolve_placeholders_proven(dsd_p25p2_playout_slot* s) {
    int first[DSD_P25P2_PLAYOUT_CAP / 4];
    const int n = find_placeholders(s, first, DSD_P25P2_PLAYOUT_CAP / 4);
    for (int k = 0; k < n; k++) {
        const int pair = q_at(s, first[k])->pair;
        const int size = (pair == s->phase_2v) ? 2 : 4;
        resolve_placeholder(s, first[k], size);
        s->sf_frames = (uint8_t)(s->sf_frames + size);
    }
    s->sf_placeholders = 0U;
}

// Resolve every placeholder as 4 frames (an unproven stream's partial superframe, a close, or a forced play-out).
// In the partial first superframe each one is logged, to be settled once the phase is proven.
static void
resolve_placeholders_as_four(dsd_p25p2_playout_slot* s, int log_partial) {
    int first[DSD_P25P2_PLAYOUT_CAP / 4];
    const int n = find_placeholders(s, first, DSD_P25P2_PLAYOUT_CAP / 4);
    for (int k = 0; k < n; k++) {
        const int pair = q_at(s, first[k])->pair;
        resolve_placeholder(s, first[k], 4);
        s->sf_frames = (uint8_t)(s->sf_frames + 4);
        if (log_partial && s->partial_log_count < 4U) {
            s->partial_log_pair[s->partial_log_count] = (uint8_t)pair;
            s->partial_log_size[s->partial_log_count] = 4U;
            s->partial_log_count++;
        }
    }
    s->sf_placeholders = 0U;
}

static void
prove_phase(dsd_p25p2_playout_slot* s, int pair) {
    if (s->phase_proven && s->phase_2v == pair) {
        return;
    }
    s->phase_2v = (int8_t)pair;
    s->phase_proven = 1U;
    s->provisional = -1;
    // The partial first superframe's fills are settled against the true sizes.
    int debt = s->fill_debt;
    for (int i = 0; i < s->partial_log_count; i++) {
        const int truth = (s->partial_log_pair[i] == pair) ? 2 : 4;
        debt += (int)s->partial_log_size[i] - truth;
    }
    s->partial_log_count = 0U;
    clamp_debt(s, debt);
    resolve_placeholders_proven(s);
    repay_from_queued_fill(s);
}

// A 4V decoded at @p pair: one candidate fewer for the 2V.
static void
note_4v(dsd_p25p2_playout_slot* s, int pair) {
    s->seen_4v_mask = (uint8_t)(s->seen_4v_mask | (1U << pair));
    if (s->phase_proven) {
        return;
    }
    const unsigned int candidates = candidates_of(s);
    if (popcount5(candidates) == 1) {
        prove_phase(s, last_candidate(candidates));
        return;
    }
    if (s->provisional == pair) {
        s->provisional = (int8_t)last_candidate(candidates);
    }
}

// The fill size for a missed voice burst at @p pair, or -1 when it must wait as a placeholder.
static int
fill_size_for_pair(const dsd_p25p2_playout_slot* s, int pair) {
    if (s->phase_proven) {
        return (pair == s->phase_2v) ? 2 : 4;
    }
    if ((s->seen_4v_mask >> pair) & 1U) {
        return 4;
    }
    if (s->provisional >= 0) {
        return (pair == s->provisional) ? 2 : 4;
    }
    return -1;
}

// A superframe covered from pair 0 ended with the 2V pair unproven: exactly one of its missing candidates was the 2V,
// and the last one gets 2 and becomes the provisional 2V pair.
static void
resolve_covered_placeholders(dsd_p25p2_playout_slot* s) {
    int first[DSD_P25P2_PLAYOUT_CAP / 4];
    const int n = find_placeholders(s, first, DSD_P25P2_PLAYOUT_CAP / 4);
    int two_at = -1;
    for (int k = n - 1; k >= 0 && two_at < 0; k--) {
        if ((candidates_of(s) >> q_at(s, first[k])->pair) & 1U) {
            two_at = k;
        }
    }
    for (int k = 0; k < n; k++) {
        const int pair = q_at(s, first[k])->pair;
        const int size = (k == two_at) ? 2 : 4;
        resolve_placeholder(s, first[k], size);
        s->sf_frames = (uint8_t)(s->sf_frames + size);
        if (k == two_at) {
            s->provisional = (int8_t)pair;
        }
    }
    s->sf_placeholders = 0U;
}

// End of a superframe for the slot: settle the placeholders its rules leave, and book a covered superframe's
// frame count against the 18 it truly carried.
static void
end_superframe(dsd_p25p2_playout_slot* s) {
    const int covered_unproven = s->sf_covered && s->sf_complete && !s->phase_proven;
    if (s->sf_placeholders > 0U && !covered_unproven) {
        resolve_placeholders_as_four(s, !s->sf_covered);
    } else if (covered_unproven) {
        if (s->sf_placeholders > 0U) {
            resolve_covered_placeholders(s);
        }
        s->first_covered_done = 1U;
    }
    if (s->sf_covered && s->sf_complete) {
        clamp_debt(s, s->fill_debt + (int)s->sf_frames - SUPERFRAME_FRAMES);
        repay_from_queued_fill(s);
    }
    s->sf_frames = 0U;
    s->sf_covered = s->open;
    s->sf_complete = s->open;
}

// The slot's burst at @p pair: a pair lower than the last one, or the same pair again after its burst was reported,
// began a new superframe (one whose SACCH timeslot was never processed).
static void
enter_pair(dsd_p25p2_playout_slot* s, int pair) {
    if (s->cur_pair == pair && !s->cur_pair_done) {
        return;
    }
    if (s->cur_pair >= 0 && pair <= s->cur_pair) {
        end_superframe(s);
    }
    s->cur_pair = (int8_t)pair;
    s->cur_pair_done = 0U;
}

static void
open_stream(dsd_p25p2_playout_slot* s, uint64_t epoch, int pair) {
    s->open = 1U;
    s->epoch = epoch;
    s->phase_2v = -1;
    s->phase_proven = 0U;
    s->provisional = -1;
    s->seen_4v_mask = 0U;
    s->missed = 0U;
    s->sf_covered = (pair == 0) ? 1U : 0U;
    s->sf_complete = 1U;
    s->sf_frames = 0U;
    s->sf_placeholders = 0U;
    s->first_covered_done = 0U;
    s->partial_log_count = 0U;
    s->fill_debt = 0;
}

// Close the slot's stream: its placeholders resolve (exactly when proven, else as 4) and its queued frames stay. A
// superframe covered from pair 0 through all five voice pairs held exactly 18 frames, whatever closes it before its
// SACCH (an END in the companion's SACCH): its placeholders resolve by that rule first.
static void
close_stream(dsd_p25p2_playout_slot* s) {
    if (!s->open) {
        return;
    }
    if (!s->phase_proven && s->sf_covered && s->sf_complete && s->sf_placeholders > 0U && s->cur_pair == VOICE_PAIRS - 1
        && s->cur_pair_done) {
        resolve_covered_placeholders(s);
    }
    if (s->phase_proven) {
        resolve_placeholders_proven(s);
    } else {
        resolve_placeholders_as_four(s, 0);
    }
    s->open = 0U;
    s->sf_complete = 0U;
    s->phase_2v = -1;
    s->phase_proven = 0U;
    s->provisional = -1;
    s->seen_4v_mask = 0U;
    s->missed = 0U;
    s->partial_log_count = 0U;
    s->fill_debt = 0;
}

// A stream is open only while its slot's canonical call is active with the epoch it began with.
static void
check_epoch(dsd_state* state, int slot) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (!s || !s->open) {
        return;
    }
    uint64_t epoch = 0;
    if (!slot_call_epoch(state, slot, &epoch) || epoch != s->epoch) {
        close_stream(s);
    }
}

static int
frame_audible(const dsd_opts* opts, int slot, const dsd_p25p2_playout_verdict* v) {
    const int slot_on = (slot == 0) ? (opts->slot1_on != 0) : (opts->slot2_on != 0);
    if (opts->slot1_on == 0 && opts->slot2_on == 0) {
        return 0;
    }
    return (v->hold || (!v->blocked && slot_on)) && v->crypto_ok;
}

// The mix's line in the --p25-sm-log stream: logged when the routing or a stream's state changes.
static void
playout_diag(dsd_opts* opts, const dsd_state* state, int k, const int audible[2]) {
    if (!dsd_p25_sm_log_enabled(opts)) {
        return;
    }
    static int prev[6] = {-1, -1, -1, -1, -1, -1};
    const dsd_p25p2_playout_slot* s0 = &state->p25p2_playout.slot[0];
    const dsd_p25p2_playout_slot* s1 = &state->p25p2_playout.slot[1];
    const int cur[6] = {audible[0], audible[1], s0->open, s1->open, opts->slot1_on, opts->slot2_on};
    int changed = 0;
    for (int i = 0; i < 6; i++) {
        if (prev[i] != cur[i]) {
            changed = 1;
            prev[i] = cur[i];
        }
    }
    if (!changed) {
        return;
    }
    dsd_p25_sm_logf(opts, "event=audio_mix path=p25p2 audible=%d/%d open=%d/%d slot_on=%d/%d blocks=%d level=%d/%d",
                    cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], k, slot_level(&state->p25p2_playout, 0),
                    slot_level(&state->p25p2_playout, 1));
}

// Keep the vocoder's output buffers inside their allocation: processAudio() and the upsampler advance them per frame
// and only a mixer's working-state reset wraps them (the mixers the playout replaced did it after every pass).
static void
housekeeping(dsd_state* state) {
    dsd_audio_maybe_reset_output_ring_left(state);
    dsd_audio_maybe_reset_output_ring_right(state);
    state->audio_out_idx = 0;
    state->audio_out_idxR = 0;
}

// Routing: two audible slots play as stereo; one plays in both ears.
static void
emit_block_float(dsd_opts* opts, dsd_state* state, dsd_p25p2_playout_entry* e[2], const int audible[2]) {
    float left[BLOCK_FRAMES] = {0};
    float right[BLOCK_FRAMES] = {0};
    if (e[0]) {
        DSD_MEMCPY(left, e[0]->pcm.f32, sizeof(left));
    }
    if (e[1]) {
        DSD_MEMCPY(right, e[1]->pcm.f32, sizeof(right));
    }
    if (opts->pulse_digi_out_channels == 1) {
        float out[BLOCK_FRAMES];
        audio_mix_mono_from_slots_f32(left, right, BLOCK_FRAMES, audible[0], audible[1], out);
        dsd_output_float_block(opts, state, out, BLOCK_FRAMES, 1);
        return;
    }
    float out[BLOCK_FRAMES * 2];
    audio_mix_interleave_stereo_f32(audible[0] ? left : right, audible[1] ? right : left, BLOCK_FRAMES, 0, 0, out);
    dsd_output_float_block(opts, state, out, BLOCK_FRAMES, 2);
}

// The static WAV records what plays, in stereo, minus calls their talkgroup lets be heard but not recorded.
static void
write_static_wav(const dsd_opts* opts, const short* stereo, dsd_p25p2_playout_entry* e[2], const int audible[2]) {
    if (opts->wav_out_f == NULL || opts->static_wav_file != 1) {
        return;
    }
    const dsd_p25p2_playout_entry* src_l = e[audible[0] ? 0 : 1];
    const dsd_p25p2_playout_entry* src_r = e[audible[1] ? 1 : 0];
    const int rec_l = src_l && src_l->verdict.recordable;
    const int rec_r = src_r && src_r->verdict.recordable;
    if (rec_l || rec_r) {
        dsd_write_masked_stereo_wav_block(opts, stereo, (rec_l ? 0 : 1) | (rec_r ? 0 : 2), "p25p2 playout");
    }
}

static void
emit_block_short(dsd_opts* opts, dsd_state* state, dsd_p25p2_playout_entry* e[2], const int audible[2]) {
    short left[BLOCK_FRAMES] = {0};
    short right[BLOCK_FRAMES] = {0};
    if (e[0]) {
        DSD_MEMCPY(left, e[0]->pcm.s16, sizeof(left));
    }
    if (e[1]) {
        DSD_MEMCPY(right, e[1]->pcm.s16, sizeof(right));
    }
    // The digital high-pass runs over exactly the blocks played, each slot on its own filter state.
    if (opts->use_hpf_d == 1) {
        hpf_dL(state, left, BLOCK_FRAMES);
        hpf_dR(state, right, BLOCK_FRAMES);
    }
    short stereo[BLOCK_FRAMES * 2];
    audio_mix_interleave_stereo_s16(audible[0] ? left : right, audible[1] ? right : left, BLOCK_FRAMES, 0, 0, stereo);
    if (opts->pulse_digi_out_channels == 1) {
        short out[BLOCK_FRAMES];
        audio_mix_mono_from_slots_s16(left, right, BLOCK_FRAMES, audible[0], audible[1], out);
        dsd_output_s16_block(opts, state, out, BLOCK_FRAMES, 1);
    } else {
        dsd_output_s16_block(opts, state, stereo, BLOCK_FRAMES, 2);
    }
    write_static_wav(opts, stereo, e, audible);
}

// @p verdict is now the slot's talkgroup verdict for the call of @p epoch, judged on @p key. A new call on the slot
// first hands the verdict last taken for the previous one to the frames that call still has queued.
static void
set_live_verdict(dsd_state* state, int slot, uint64_t epoch, const dsd_p25p2_policy_key* key,
                 const dsd_p25p2_playout_verdict* verdict) {
    dsd_p25p2_playout_slot* s = &state->p25p2_playout.slot[slot];
    if (s->live_valid && s->live_epoch != epoch) {
        for (int i = 0; i < s->count; i++) {
            dsd_p25p2_playout_entry* e = q_at(s, i);
            if (e->epoch == s->live_epoch && !e->has_live) {
                e->live = s->live_verdict;
                e->has_live = 1U;
            }
        }
    }
    s->live_verdict = *verdict;
    s->live_epoch = epoch;
    s->live_key = *key;
    s->live_valid = 1U;
}

static int
same_policy_key(const dsd_p25p2_policy_key* a, const dsd_p25p2_policy_key* b) {
    return a->table_context == b->table_context && a->table_generation == b->table_generation && a->second == b->second
           && a->tg_hold == b->tg_hold && a->allow_list == b->allow_list && a->ota_target == b->ota_target
           && a->policy_target == b->policy_target && a->source == b->source && a->kind == b->kind;
}

// Take the slot's current talkgroup verdict, if its call is active, for the frames of that call's epoch. The epoch and
// the verdict come from one snapshot of the call: the no-carrier pass may end the calls on the decoder thread while
// the watchdog takes this under the tick guard, and a call ended in between keeps the verdict last taken for it.
static void
take_live_verdict(const dsd_opts* opts, dsd_state* state, int slot) {
    dsd_p25p2_playout_verdict verdict;
    uint64_t epoch = 0;
    dsd_p25p2_policy_key key;
    if (dsd_p25p2_playout_policy_verdict(opts, state, slot, &verdict, &epoch, &key)) {
        set_live_verdict(state, slot, epoch, &key, &verdict);
    }
}

// The one policy check the playout makes as it plays, a cheap one: a queued frame's call for which anything its verdict
// reads moved since it was taken (signaling naming its source, a patch change, a policy row learned from an alias, the
// hold, the allow list, or simply the next second of the decode clock) has its verdict taken again.
static void
refresh_live_verdicts(const dsd_opts* opts, dsd_state* state) {
    for (int slot = 0; slot < 2; slot++) {
        const dsd_p25p2_playout_slot* s = &state->p25p2_playout.slot[slot];
        uint64_t epoch = 0;
        dsd_p25p2_policy_key key;
        if (s->live_valid && q_live(s) > 0 && dsd_p25p2_playout_policy_key(opts, state, slot, &epoch, &key)
            && epoch == s->live_epoch && !same_policy_key(&key, &s->live_key)) {
            take_live_verdict(opts, state, slot);
        }
    }
}

// The talkgroup verdict a frame must also pass: the one last taken for its call (by its latest burst, or by a command or
// path that changes verdicts), or the one its call had when another call replaced it. NULL for a frame of no call.
static const dsd_p25p2_playout_verdict*
live_verdict_for(const dsd_state* state, int slot, const dsd_p25p2_playout_entry* e) {
    if (e->has_live) {
        return &e->live;
    }
    if (e->epoch == 0U) {
        return NULL;
    }
    const dsd_p25p2_playout_slot* s = &state->p25p2_playout.slot[slot];
    return (s->live_valid && s->live_epoch == e->epoch) ? &s->live_verdict : NULL;
}

// Pop the slot's next frame into @p out; returns whether it may be heard (0 too when none was ready).
static int
pop_frame(const dsd_opts* opts, dsd_state* state, int slot, dsd_p25p2_playout_entry* out, dsd_p25p2_playout_entry** e) {
    *e = NULL;
    if (!q_pop(&state->p25p2_playout.slot[slot], out)) {
        return 0;
    }
    *e = out;
    if (!frame_audible(opts, slot, &out->verdict)) {
        return 0;
    }
    const dsd_p25p2_playout_verdict* now = live_verdict_for(state, slot, out);
    if (!now) {
        return 1;
    }
    // The policy verdict the frame's call has (or had when the call was last on) must let it play too. Only whether it
    // plays: its static-WAV recordability stays the one fixed when it was queued.
    dsd_p25p2_playout_verdict v = *now;
    v.crypto_ok = out->verdict.crypto_ok;
    return frame_audible(opts, slot, &v);
}

static int
head_is_barrier(dsd_p25p2_playout_slot* s) {
    q_compact_head(s);
    return s->count > 0 && s->q[s->head].kind == ENTRY_BARRIER;
}

static void
drop_head(dsd_p25p2_playout_slot* s) {
    s->head = (uint8_t)((s->head + 1) % DSD_P25P2_PLAYOUT_CAP);
    s->count--;
    note_departed(s, 1U);
}

// Sound the alerts whose slot has since let go of every entry queued ahead of them, oldest first, on the thread that
// plays the queues (so a tone never cuts into a block), without the lock held. Queue order is push order, so once
// removed entries at the head are let go (nothing pops them) the head is the oldest entry left: an alert is due once
// that is newer than the alert, whatever else was removed behind it.
static void
fire_alerts(dsd_opts* opts, dsd_state* state) {
    dsd_p25p2_playout* p = &state->p25p2_playout;
    for (int slot = 0; slot < 2; slot++) {
        dsd_p25p2_playout_slot* s = &p->slot[slot];
        q_compact_head(s);
        const uint64_t oldest = (s->count > 0) ? s->q[s->head].seq : UINT64_MAX;
        for (;;) {
            dsd_p25p2_playout_alert a = {0};
            int due = 0;
            alert_lock();
            if (p->alert_count[slot] > 0 && oldest > p->alerts[slot][0].after) {
                a = p->alerts[slot][0];
                due = 1;
            }
            alert_unlock();
            if (!due) {
                break;
            }
            // The alert stays queued until its tone is out: one raised meanwhile, on any thread, waits behind it rather
            // than sounding into it. Only the thread that plays the queues takes alerts off.
            dsd_beeper_emit(opts, state, slot, a.id, a.ad, a.len);
            alert_lock();
            for (int i = 1; i < p->alert_count[slot]; i++) {
                p->alerts[slot][i - 1] = p->alerts[slot][i];
            }
            p->alert_count[slot]--;
            alert_unlock();
        }
    }
}

// Which slot pops first in a block: the one not at a barrier. Two barriers resolve in pair-clock order: the earlier has
// nothing earlier to wait out (what the companion holds behind its own barrier is of later pairs) and goes; the later
// goes second and still waits while the companion plays entries of earlier pairs. A tie has nothing to wait out.
static int
settle_barriers(dsd_p25p2_playout* p) {
    const int b0 = head_is_barrier(&p->slot[0]);
    const int b1 = head_is_barrier(&p->slot[1]);
    if (b0 && b1) {
        const uint32_t c0 = p->slot[0].q[p->slot[0].head].clock;
        const uint32_t c1 = p->slot[1].q[p->slot[1].head].clock;
        if (!clock_before(c1, c0)) {
            drop_head(&p->slot[0]);
        }
        if (!clock_before(c0, c1)) {
            drop_head(&p->slot[1]);
        }
        return clock_before(c1, c0) ? 1 : 0;
    }
    return b0 ? 1 : 0;
}

// Whether slot @p idx sits out this block at its barrier: the companion played an entry of an earlier pair. Once it
// plays none (or a later one), the barrier goes.
static int
barrier_waits(dsd_p25p2_playout* p, int idx, const dsd_p25p2_playout_entry* companion) {
    dsd_p25p2_playout_slot* s = &p->slot[idx];
    if (!head_is_barrier(s)) {
        return 0;
    }
    if (companion && clock_before(companion->clock, s->q[s->head].clock)) {
        return 1;
    }
    drop_head(s);
    return 0;
}

// A stream opening while the companion still holds entries of earlier pairs (waiting behind a placeholder, or a
// carry) starts at this pair's air time, not at the companion's head: a barrier holds the slot's first frame until the
// companion has played those entries.
static int
holds_earlier_pair(const dsd_p25p2_playout_slot* s, uint32_t clock) {
    for (int i = 0; i < s->count; i++) {
        const dsd_p25p2_playout_entry* e = q_at_const(s, i);
        if (e->kind != ENTRY_SKIP && e->kind != ENTRY_BARRIER && clock_before(e->clock, clock)) {
            return 1;
        }
    }
    return 0;
}

static void
queue_barrier(dsd_state* state, int slot) {
    dsd_p25p2_playout* p = &state->p25p2_playout;
    if (!holds_earlier_pair(&p->slot[slot ^ 1], p->pair_clock)) {
        return;
    }
    dsd_p25p2_playout_entry* b = q_push(&p->slot[slot]);
    if (b) {
        b->kind = ENTRY_BARRIER;
        b->clock = p->pair_clock;
    }
}

// Pop one block from both slots and play it when audible. Returns whether it carried an audible fresh frame (#574).
static int
play_block(dsd_opts* opts, dsd_state* state, int output, int last_audible[2]) {
    dsd_p25p2_playout_entry popped[2];
    dsd_p25p2_playout_entry* e[2] = {NULL, NULL};
    int audible[2] = {0, 0};
    // A slot at a barrier goes second: it waits while the companion's entry for this block is of an earlier pair.
    const int first = settle_barriers(&state->p25p2_playout);
    const int second = first ^ 1;
    audible[first] = pop_frame(opts, state, first, &popped[first], &e[first]);
    if (!barrier_waits(&state->p25p2_playout, second, e[first])) {
        audible[second] = pop_frame(opts, state, second, &popped[second], &e[second]);
    }
    int fresh = 0;
    if (audible[0] || audible[1]) {
        last_audible[0] = audible[0];
        last_audible[1] = audible[1];
        if (output) {
            if (opts->floating_point == 1) {
                emit_block_float(opts, state, e, audible);
            } else {
                emit_block_short(opts, state, e, audible);
            }
            fresh = (audible[0] && e[0]->fresh) || (audible[1] && e[1]->fresh);
        }
    }
    // What the block popped is gone only now that its samples are out: an alert waiting on it sounds after them.
    for (int slot = 0; slot < 2; slot++) {
        if (e[slot]) {
            note_departed(&state->p25p2_playout.slot[slot], 1U);
        }
    }
    return fresh;
}

// Pop @p k blocks from both slots and play the audible ones; an alert whose audio has gone sounds right after it.
static void
play_blocks(dsd_opts* opts, dsd_state* state, int k) {
    const int output = opts->pulse_digi_rate_out == 8000;
    int stamp = 0;
    int last_audible[2] = {0, 0};
    refresh_live_verdicts(opts, state);
    for (int j = 0; j < k; j++) {
        stamp |= play_block(opts, state, output, last_audible);
        fire_alerts(opts, state);
    }
    if (stamp && dsd_audio_activity_armed() && dsd_mix_output_plays(opts)) {
        dsd_audio_activity_note();
    }
    playout_diag(opts, state, k, last_audible);
}

// The emission step: as many blocks as every open stream has ready; with none open, what the closed ones hold.
static void
emit(dsd_opts* opts, dsd_state* state) {
    dsd_p25p2_playout_slot* s[2] = {&state->p25p2_playout.slot[0], &state->p25p2_playout.slot[1]};
    for (int i = 0; i < 2; i++) {
        if (q_live(s[i]) > QUEUE_FORCE) {
            // A safety bound, not part of the timing model: settle the placeholders and play the excess out.
            resolve_placeholders_as_four(s[i], 0);
        }
    }
    const int level[2] = {slot_level(&state->p25p2_playout, 0), slot_level(&state->p25p2_playout, 1)};
    int k = -1;
    for (int i = 0; i < 2; i++) {
        if (s[i]->open && (k < 0 || level[i] < k)) {
            k = level[i];
        }
    }
    if (k < 0) {
        k = (level[0] > level[1]) ? level[0] : level[1];
    }
    for (int i = 0; i < 2; i++) {
        if (level[i] - k > QUEUE_FORCE) {
            k = level[i] - QUEUE_FORCE;
        }
    }
    if (k > 0) {
        play_blocks(opts, state, k);
    }
}

// Room in @p s for one more decoded frame, whatever holds the queues back: a decoded frame is never dropped. A normal
// emission comes first; past it, beyond the timing model (as the QUEUE_FORCE bound is), both slots' placeholders
// resolve as four and blocks play until the queue has room: a stream waiting at its barrier, say, while the companion's
// placeholder of an earlier pair holds every level at zero.
static void
make_room(dsd_opts* opts, dsd_state* state, const dsd_p25p2_playout_slot* s) {
    if (q_live(s) < DSD_P25P2_PLAYOUT_CAP) {
        return;
    }
    emit(opts, state);
    if (q_live(s) < DSD_P25P2_PLAYOUT_CAP) {
        return;
    }
    resolve_placeholders_as_four(&state->p25p2_playout.slot[0], 0);
    resolve_placeholders_as_four(&state->p25p2_playout.slot[1], 0);
    for (int n = 0; n < 2 * DSD_P25P2_PLAYOUT_CAP + 2 && q_live(s) >= DSD_P25P2_PLAYOUT_CAP; n++) {
        play_blocks(opts, state, 1);
    }
}

// The first frame of a burst fixes its verdict: the decode gate's decision when it took one (@p decision, or a verdict
// a caller supplies), else one evaluation here. That decision is also the slot's current talkgroup verdict for its
// call, which the frames the call already queued must pass too, kept with what it was judged on: the gate's own key,
// so a change between the gate and this frame still shows at emission. The playout evaluates no policy as it plays.
static void
take_burst_verdict(const dsd_opts* opts, dsd_state* state, int slot, dsd_p25p2_playout_slot* s, uint32_t burst_serial,
                   const dsd_p25p2_playout_verdict* burst_verdict, const dsd_p25p2_burst_decision* decision) {
    s->burst_serial = burst_serial;
    if (decision) {
        s->verdict = decision->verdict;
    } else if (burst_verdict) {
        s->verdict = *burst_verdict;
    } else {
        (void)dsd_p25p2_playout_policy_verdict(opts, state, slot, &s->verdict, NULL, NULL);
    }
    s->verdict.crypto_ok = p25_crypto_audio_output_permitted(opts, state, slot) ? 1U : 0U;
    if (s->epoch == 0U) {
        return;
    }
    if (decision) {
        if (decision->epoch == s->epoch) {
            set_live_verdict(state, slot, decision->epoch, &decision->key, &s->verdict);
        }
        return;
    }
    uint64_t epoch = 0;
    dsd_p25p2_policy_key key;
    if (dsd_p25p2_playout_policy_key(opts, state, slot, &epoch, &key) && epoch == s->epoch) {
        set_live_verdict(state, slot, epoch, &key, &s->verdict);
    }
}

// The frame the session's format plays: the float frame through agf(), or the short frame the vocoder staged (zeros,
// not fresh, when it left the slot out).
static void
copy_frame(const dsd_opts* opts, dsd_state* state, int slot, dsd_p25p2_playout_entry* e) {
    if (opts->floating_point == 1) {
        DSD_MEMCPY(e->pcm.f32, (slot == 0) ? state->audio_out_temp_buf : state->audio_out_temp_bufR,
                   sizeof(e->pcm.f32));
        agf(opts, state, e->pcm.f32, slot);
        e->fresh = 1U;
    } else if (state->mbe_short_silenced[slot]) {
        e->fresh = 0U;
    } else {
        DSD_MEMCPY(e->pcm.s16, (slot == 0) ? state->s_l : state->s_r, sizeof(e->pcm.s16));
        e->fresh = 1U;
    }
}

static void
stage_frame(dsd_opts* opts, dsd_state* state, int slot, int pair, uint32_t burst_serial,
            const dsd_p25p2_playout_verdict* burst_verdict, const dsd_p25p2_burst_decision* decision) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (!s || !opts || pair < 0 || pair >= VOICE_PAIRS) {
        return;
    }
    enter_pair(s, pair);
    uint64_t epoch = 0;
    const int active = slot_call_epoch(state, slot, &epoch);
    if (s->open && active && epoch != s->epoch) {
        close_stream(s);
    }
    if (!s->open) {
        open_stream(s, active ? epoch : 0U, pair);
        queue_barrier(state, slot);
    }
    if (s->burst_serial != burst_serial) {
        take_burst_verdict(opts, state, slot, s, burst_serial, burst_verdict, decision);
    }
    make_room(opts, state, s);
    dsd_p25p2_playout_entry* e = q_push(s);
    if (!e) {
        return;
    }
    e->kind = ENTRY_FRAME;
    e->clock = state->p25p2_playout.pair_clock;
    e->epoch = s->epoch;
    e->verdict = s->verdict;
    copy_frame(opts, state, slot, e);
    if (s->sf_frames < UINT8_MAX) {
        s->sf_frames++;
    }
}

void
dsd_p25p2_playout_stage(dsd_opts* opts, dsd_state* state, int slot, int pair, uint32_t burst_serial,
                        const dsd_p25p2_playout_verdict* burst_verdict) {
    stage_frame(opts, state, slot, pair, burst_serial, burst_verdict, NULL);
}

void
dsd_p25p2_playout_stage_decided(dsd_opts* opts, dsd_state* state, int slot, int pair, uint32_t burst_serial,
                                const dsd_p25p2_burst_decision* decision) {
    stage_frame(opts, state, slot, pair, burst_serial, NULL, decision);
}

void
dsd_p25p2_playout_note_muted(dsd_state* state, int slot) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (s) {
        close_stream(s);
    }
}

// An open stream missed its voice burst at @p pair: silence in its place, or a placeholder while its size is unknown.
static void
note_missed(dsd_opts* opts, dsd_state* state, dsd_p25p2_playout_slot* s, int pair) {
    s->missed++;
    if (s->missed >= MISSED_CLOSE) {
        close_stream(s);
        return;
    }
    const int size = fill_size_for_pair(s, pair);
    if (size >= 0) {
        s->sf_frames = (uint8_t)(s->sf_frames + size);
        const int frames = fill_after_debt(s, size, 4);
        if (q_live(s) + frames > DSD_P25P2_PLAYOUT_CAP) {
            emit(opts, state);
        }
        push_fill(s, frames, state->p25p2_playout.pair_clock);
        return;
    }
    // A pair that could be the 2V, before anything says: four reserved frames until the superframe (or a proof)
    // settles it. All four or none: its resolution rewrites the four in place. A queue too full to take them even
    // after an emission (its companion's placeholder holding it back) leaves the pair out; the next decoded frame
    // makes room.
    if (q_live(s) + 4 > DSD_P25P2_PLAYOUT_CAP) {
        emit(opts, state);
    }
    if (q_live(s) + 4 > DSD_P25P2_PLAYOUT_CAP) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        dsd_p25p2_playout_entry* e = q_push(s);
        if (!e) {
            break;
        }
        e->kind = ENTRY_PENDING;
        e->clock = state->p25p2_playout.pair_clock;
        e->pair = (uint8_t)pair;
        e->epoch = s->epoch;
        e->verdict = s->verdict;
    }
    s->sf_placeholders++;
}

void
dsd_p25p2_playout_burst_done(dsd_opts* opts, dsd_state* state, int slot, int pair, dsd_p25p2_burst_kind kind) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (!s || !opts || pair < 0 || pair > VOICE_PAIRS) {
        return;
    }
    if (pair < VOICE_PAIRS && kind == DSD_P25P2_BURST_SACCH) {
        // A voice position never carries a SACCH: a DUID read as one there is a lost voice burst.
        kind = DSD_P25P2_BURST_LOST;
    }
    enter_pair(s, pair);
    check_epoch(state, slot);
    if (pair < VOICE_PAIRS) {
        if (kind == DSD_P25P2_BURST_2V) {
            prove_phase(s, pair);
            s->missed = 0U;
        } else if (kind == DSD_P25P2_BURST_4V) {
            note_4v(s, pair);
            s->missed = 0U;
        } else if ((kind == DSD_P25P2_BURST_OTHER || kind == DSD_P25P2_BURST_LOST) && s->open) {
            note_missed(opts, state, s, pair);
        }
    }
    s->cur_pair_done = 1U;
    if (pair == VOICE_PAIRS) {
        end_superframe(s);
        s->cur_pair = -1;
        s->cur_pair_done = 0U;
    }
}

void
dsd_p25p2_playout_pair_done(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        check_epoch(state, slot);
        dsd_p25p2_playout_slot* s = &state->p25p2_playout.slot[slot];
        if (s->open && !state->p25_p2_audio_allowed[slot]) {
            close_stream(s);
        }
    }
    emit(opts, state);
    fire_alerts(opts, state);
    housekeeping(state);
    state->p25p2_playout.pair_clock++;
}

void
dsd_p25p2_playout_close(dsd_state* state, int slot) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (s) {
        close_stream(s);
    }
}

void
dsd_p25p2_playout_break(dsd_state* state) {
    if (!state) {
        return;
    }
    close_stream(&state->p25p2_playout.slot[0]);
    close_stream(&state->p25p2_playout.slot[1]);
    // A pair left unfinished before the break (one slot's burst queued, the other's never to come) is not the pair the
    // next frames belong to: they start a later one, and queue_barrier() holds a stream opening beside that tail.
    state->p25p2_playout.pair_clock++;
}

void
dsd_p25p2_playout_drain(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        close_stream(&state->p25p2_playout.slot[slot]);
    }
    const int level[2] = {slot_level(&state->p25p2_playout, 0), slot_level(&state->p25p2_playout, 1)};
    const int k = (level[0] > level[1]) ? level[0] : level[1];
    if (k > 0) {
        play_blocks(opts, state, k);
    }
    fire_alerts(opts, state);
    housekeeping(state);
}

void
dsd_p25p2_playout_note_policy(const dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    take_live_verdict(opts, state, 0);
    take_live_verdict(opts, state, 1);
}

void
dsd_p25p2_playout_reset(dsd_state* state, int slot) {
    if (!state) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        if (slot >= 0 && slot != i) {
            continue;
        }
        dsd_p25p2_playout_slot* s = &state->p25p2_playout.slot[i];
        // Everything queued is gone; alerts held behind it stay queued and sound at the next chance.
        alert_lock();
        const uint64_t pushed = s->pushed;
        DSD_MEMSET(s, 0, sizeof(*s));
        s->pushed = pushed;
        s->departed = pushed;
        alert_unlock();
        s->cur_pair = -1;
        s->phase_2v = -1;
        s->provisional = -1;
    }
}

void
dsd_p25p2_playout_discard(dsd_opts* opts, dsd_state* state) {
    if (!state) {
        return;
    }
    dsd_p25p2_playout_reset(state, -1);
    if (opts) {
        fire_alerts(opts, state);
    }
}

int
dsd_p25p2_playout_holds_audio(dsd_state* state) {
    if (!state) {
        return 0;
    }
    const dsd_p25p2_playout* p = &state->p25p2_playout;
    alert_lock();
    const int holds = p->slot[0].pushed != p->slot[0].departed || p->slot[1].pushed != p->slot[1].departed;
    alert_unlock();
    return holds;
}

int
dsd_p25p2_playout_defer_alert(const dsd_opts* opts, dsd_state* state, int slot, int id, int ad, int len) {
    if (!opts || !state || slot < 0 || slot > 1) {
        return 0;
    }
    dsd_p25p2_playout* p = &state->p25p2_playout;
    int taken = 0;
    alert_lock();
    const dsd_p25p2_playout_slot* s = &p->slot[slot];
    // Behind queued audio, or behind earlier alerts still waiting their turn. It sounds from the playout's own next
    // step, never from here: this may run on another thread while a block is going out. One more than a full alert
    // queue holds (far more than a superframe's call events) is dropped rather than sounded ahead of them.
    if (s->departed != s->pushed || p->alert_count[slot] > 0) {
        if (p->alert_count[slot] < DSD_P25P2_PLAYOUT_ALERTS) {
            dsd_p25p2_playout_alert* a = &p->alerts[slot][p->alert_count[slot]++];
            a->after = s->pushed;
            a->id = id;
            a->ad = ad;
            a->len = len;
        }
        taken = 1;
    }
    alert_unlock();
    return taken;
}

int
dsd_p25p2_playout_level(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return slot_level(&state->p25p2_playout, slot);
}
