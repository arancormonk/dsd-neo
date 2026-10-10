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
 * fixed when its first frame is queued, so a tail played after END, release or a policy change keeps its own call's
 * verdict. Only the slot switches are read at emission. Float frames are gained (agf) when queued, for the same
 * reason.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "dsd_audio_internal.h"

enum {
    ENTRY_FRAME = 1,   /* a decoded frame */
    ENTRY_FILL = 2,    /* silence for a missed voice burst */
    ENTRY_PENDING = 3, /* an unresolved placeholder frame: four reserve one missing pair */
    ENTRY_SKIP = 4,    /* removed: popped without playing */
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

// Entries ready to play: those ahead of the first unresolved placeholder.
static int
q_level(const dsd_p25p2_playout_slot* s) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        const dsd_p25p2_playout_entry* e = q_at_const(s, i);
        if (e->kind == ENTRY_PENDING) {
            break;
        }
        if (e->kind != ENTRY_SKIP) {
            n++;
        }
    }
    return n;
}

// Drop removed entries from the head, so they never hold capacity.
static void
q_compact_head(dsd_p25p2_playout_slot* s) {
    while (s->count > 0 && s->q[s->head].kind == ENTRY_SKIP) {
        s->head = (uint8_t)((s->head + 1) % DSD_P25P2_PLAYOUT_CAP);
        s->count--;
    }
}

static dsd_p25p2_playout_entry*
q_push(dsd_p25p2_playout_slot* s) {
    q_compact_head(s);
    if (s->count >= DSD_P25P2_PLAYOUT_CAP) {
        return NULL;
    }
    dsd_p25p2_playout_entry* e = q_at(s, s->count);
    s->count++;
    DSD_MEMSET(e, 0, sizeof(*e));
    return e;
}

// Pop the next entry ready to play into @p out. 0 when none is ready.
static int
q_pop(dsd_p25p2_playout_slot* s, dsd_p25p2_playout_entry* out) {
    q_compact_head(s);
    if (s->count == 0 || s->q[s->head].kind == ENTRY_PENDING) {
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
push_fill(dsd_p25p2_playout_slot* s, int frames) {
    for (int i = 0; i < frames; i++) {
        dsd_p25p2_playout_entry* e = q_push(s);
        if (!e) {
            return;
        }
        e->kind = ENTRY_FILL;
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

// End of a superframe for the slot: settle the placeholders its rules leave, and book a covered superframe's
// frame count against the 18 it truly carried.
static void
end_superframe(dsd_p25p2_playout_slot* s) {
    if (s->sf_placeholders > 0U) {
        if (s->sf_covered && s->sf_complete && !s->phase_proven) {
            // Exactly one of this superframe's missing candidates was the 2V: the last one gets 2.
            int first[DSD_P25P2_PLAYOUT_CAP / 4];
            const int n = find_placeholders(s, first, DSD_P25P2_PLAYOUT_CAP / 4);
            int two_at = -1;
            for (int k = n - 1; k >= 0 && two_at < 0; k--) {
                const int pair = q_at(s, first[k])->pair;
                if ((candidates_of(s) >> pair) & 1U) {
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
            s->first_covered_done = 1U;
        } else {
            resolve_placeholders_as_four(s, !s->sf_covered);
        }
    } else if (s->sf_covered && s->sf_complete && !s->phase_proven) {
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

// Close the slot's stream: its placeholders resolve (exactly when proven, else as 4) and its queued frames stay.
static void
close_stream(dsd_p25p2_playout_slot* s) {
    if (!s->open) {
        return;
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

// The SS18 diagnostic's successor in the --p25-sm-log stream: logged when the routing or a stream's state changes.
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
                    cur[0], cur[1], cur[2], cur[3], cur[4], cur[5], k, q_level(s0), q_level(s1));
}

// Keep the vocoder's output buffers inside their allocation: processAudio() and the upsampler advance them per frame
// and only a mixer's working-state reset wraps them (as SS18 and FS4 did after every pass).
static void
housekeeping(dsd_state* state) {
    dsd_audio_maybe_reset_output_ring_left(state);
    dsd_audio_maybe_reset_output_ring_right(state);
    state->audio_out_idx = 0;
    state->audio_out_idxR = 0;
}

static void
emit_block(dsd_opts* opts, dsd_state* state, dsd_p25p2_playout_entry* e[2], const int audible[2]) {
    const int mono = opts->pulse_digi_out_channels == 1;
    if (opts->floating_point == 1) {
        float left[BLOCK_FRAMES] = {0};
        float right[BLOCK_FRAMES] = {0};
        if (e[0]) {
            DSD_MEMCPY(left, e[0]->pcm.f32, sizeof(left));
        }
        if (e[1]) {
            DSD_MEMCPY(right, e[1]->pcm.f32, sizeof(right));
        }
        if (mono) {
            float out[BLOCK_FRAMES];
            audio_mix_mono_from_slots_f32(left, right, BLOCK_FRAMES, audible[0], audible[1], out);
            dsd_output_float_block(opts, state, out, BLOCK_FRAMES, 1);
            return;
        }
        const float* l = audible[0] ? left : right;
        const float* r = audible[1] ? right : left;
        float out[BLOCK_FRAMES * 2];
        audio_mix_interleave_stereo_f32(l, r, BLOCK_FRAMES, 0, 0, out);
        dsd_output_float_block(opts, state, out, BLOCK_FRAMES, 2);
        return;
    }

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
    const short* l = audible[0] ? left : right;
    const short* r = audible[1] ? right : left;
    short stereo[BLOCK_FRAMES * 2];
    audio_mix_interleave_stereo_s16(l, r, BLOCK_FRAMES, 0, 0, stereo);
    if (mono) {
        short out[BLOCK_FRAMES];
        audio_mix_mono_from_slots_s16(left, right, BLOCK_FRAMES, audible[0], audible[1], out);
        dsd_output_s16_block(opts, state, out, BLOCK_FRAMES, 1);
    } else {
        dsd_output_s16_block(opts, state, stereo, BLOCK_FRAMES, 2);
    }
    // The static WAV records what plays, in stereo, minus calls their talkgroup lets be heard but not recorded.
    if (opts->wav_out_f != NULL && opts->static_wav_file == 1) {
        const int src_l = audible[0] ? 0 : 1;
        const int src_r = audible[1] ? 1 : 0;
        const int rec_l = e[src_l] && e[src_l]->verdict.recordable;
        const int rec_r = e[src_r] && e[src_r]->verdict.recordable;
        if (rec_l || rec_r) {
            const int mask = (rec_l ? 0 : 1) | (rec_r ? 0 : 2);
            dsd_write_masked_stereo_wav_block(opts, stereo, mask, "p25p2 playout");
        }
    }
}

// Pop @p k blocks from both slots and play the audible ones.
static void
play_blocks(dsd_opts* opts, dsd_state* state, int k) {
    const int output = opts->pulse_digi_rate_out == 8000;
    int stamp = 0;
    int last_audible[2] = {0, 0};
    for (int j = 0; j < k; j++) {
        dsd_p25p2_playout_entry popped[2];
        dsd_p25p2_playout_entry* e[2] = {NULL, NULL};
        int audible[2] = {0, 0};
        for (int slot = 0; slot < 2; slot++) {
            if (q_pop(&state->p25p2_playout.slot[slot], &popped[slot])) {
                e[slot] = &popped[slot];
                audible[slot] = frame_audible(opts, slot, &popped[slot].verdict);
            }
        }
        if (!audible[0] && !audible[1]) {
            continue;
        }
        last_audible[0] = audible[0];
        last_audible[1] = audible[1];
        if (!output) {
            continue;
        }
        for (int slot = 0; slot < 2; slot++) {
            if (audible[slot] && e[slot]->fresh) {
                stamp = 1;
            }
        }
        emit_block(opts, state, e, audible);
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
    const int level[2] = {q_level(s[0]), q_level(s[1])};
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

void
dsd_p25p2_playout_stage(dsd_opts* opts, dsd_state* state, int slot, int pair, uint32_t burst_serial,
                        const dsd_p25p2_playout_verdict* burst_verdict) {
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
    }
    if (s->burst_serial != burst_serial) {
        s->burst_serial = burst_serial;
        if (burst_verdict) {
            s->verdict = *burst_verdict;
        } else {
            dsd_p25p2_playout_policy_verdict(opts, state, slot, &s->verdict);
        }
        s->verdict.crypto_ok = p25_crypto_audio_output_permitted(opts, state, slot) ? 1U : 0U;
    }
    if (q_live(s) >= DSD_P25P2_PLAYOUT_CAP) {
        emit(opts, state);
    }
    dsd_p25p2_playout_entry* e = q_push(s);
    if (!e) {
        return;
    }
    e->kind = ENTRY_FRAME;
    e->verdict = s->verdict;
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
    if (s->sf_frames < UINT8_MAX) {
        s->sf_frames++;
    }
}

void
dsd_p25p2_playout_note_muted(dsd_state* state, int slot) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (s) {
        close_stream(s);
    }
}

void
dsd_p25p2_playout_burst_done(dsd_opts* opts, dsd_state* state, int slot, int pair, dsd_p25p2_burst_kind kind) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (!s || !opts || pair < 0 || pair > VOICE_PAIRS) {
        return;
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
            s->missed++;
            if (s->missed >= MISSED_CLOSE) {
                close_stream(s);
            } else {
                const int size = fill_size_for_pair(s, pair);
                if (size < 0) {
                    // A pair that could be the 2V, before anything says: four reserved frames until the superframe
                    // (or a proof) settles it.
                    if (q_live(s) + 4 > DSD_P25P2_PLAYOUT_CAP) {
                        emit(opts, state);
                    }
                    for (int i = 0; i < 4; i++) {
                        dsd_p25p2_playout_entry* e = q_push(s);
                        if (!e) {
                            break;
                        }
                        e->kind = ENTRY_PENDING;
                        e->pair = (uint8_t)pair;
                        e->verdict = s->verdict;
                    }
                    s->sf_placeholders++;
                } else {
                    s->sf_frames = (uint8_t)(s->sf_frames + size);
                    push_fill(s, fill_after_debt(s, size, 4));
                }
            }
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
    housekeeping(state);
}

void
dsd_p25p2_playout_close(dsd_state* state, int slot) {
    dsd_p25p2_playout_slot* s = slot_of(state, slot);
    if (s) {
        close_stream(s);
    }
}

void
dsd_p25p2_playout_drain(dsd_opts* opts, dsd_state* state) {
    if (!opts || !state) {
        return;
    }
    for (int slot = 0; slot < 2; slot++) {
        close_stream(&state->p25p2_playout.slot[slot]);
    }
    const int level[2] = {q_level(&state->p25p2_playout.slot[0]), q_level(&state->p25p2_playout.slot[1])};
    const int k = (level[0] > level[1]) ? level[0] : level[1];
    if (k > 0) {
        play_blocks(opts, state, k);
    }
    housekeeping(state);
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
        DSD_MEMSET(s, 0, sizeof(*s));
        s->cur_pair = -1;
        s->phase_2v = -1;
        s->provisional = -1;
    }
}

int
dsd_p25p2_playout_level(const dsd_state* state, int slot) {
    if (!state || slot < 0 || slot > 1) {
        return 0;
    }
    return q_level(&state->p25p2_playout.slot[slot]);
}
