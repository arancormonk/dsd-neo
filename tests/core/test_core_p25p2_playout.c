// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 voice playout (issue #651): per-slot queues played per burst pair in both formats.
 *
 * Frames are tagged (the short frame's samples all hold the tag; float frames carry the slot in their sign) and every
 * block the playout writes is captured through the UDP blast hook, so each case can check how many blocks played,
 * what each channel carried and in what order.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "test_support.h"

enum { MAX_BLOCKS = 512 };

typedef struct {
    int channels; /* 1 or 2 */
    int left;     /* first left (or mono) sample: the tag */
    int right;
    int peak_left; /* largest left (or mono) sample magnitude in the block (short output) */
    int is_float;
    float fleft;
    float fright;
} captured_block;

static dsd_opts g_opts;
static dsd_state g_state;
static captured_block g_blocks[MAX_BLOCKS];
static int g_block_count = 0;
static int g_capture_float = 0;
/* An alert raised from the output hook while block g_raise_at_block is being written (as another thread could raise
   one then), and what dsd_p25p2_playout_defer_alert() answered. */
static int g_raise_at_block = -1;
static int g_raise_taken = -1;
/* A quieter alert raised through beeper() from the output hook while block g_beep_at_block is being written. */
static int g_beep_at_block = -1;

static void
capture_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)state;
    if (g_block_count >= MAX_BLOCKS || !data) {
        return;
    }
    captured_block* b = &g_blocks[g_block_count++];
    DSD_MEMSET(b, 0, sizeof(*b));
    b->is_float = g_capture_float;
    if (g_capture_float) {
        const float* f = (const float*)data;
        b->channels = (nsam == sizeof(float) * 160U * 2U) ? 2 : 1;
        b->fleft = f[0];
        b->fright = (b->channels == 2) ? f[1] : f[0];
    } else {
        const short* s = (const short*)data;
        b->channels = (nsam == sizeof(short) * 160U * 2U) ? 2 : 1;
        b->left = s[0];
        b->right = (b->channels == 2) ? s[1] : s[0];
        for (int i = 0; i < 160; i++) {
            const int v = s[(size_t)i * (size_t)b->channels];
            const int mag = (v < 0) ? -v : v;
            if (mag > b->peak_left) {
                b->peak_left = mag;
            }
        }
    }
    (void)opts;
    if (g_block_count == g_raise_at_block) {
        g_raise_at_block = -1;
        g_raise_taken = dsd_p25p2_playout_defer_alert(&g_opts, state, 0, 40, 86, 3);
    }
    if (g_block_count == g_beep_at_block) {
        g_beep_at_block = -1;
        beeper(&g_opts, state, 0, 40, 20, 1);
    }
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static short g_out_s[2][4096];
static float g_out_f[2][4096];
static const dsd_p25p2_playout_verdict k_open_verdict = {0U, 0U, 1U, 1U};

static void
seed_call(dsd_state* state, uint8_t slot, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

static void
setup(int floating, int channels) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.pulse_digi_rate_out = 8000;
    g_opts.pulse_digi_out_channels = channels;
    g_opts.floating_point = floating;
    g_opts.slot1_on = 1;
    g_opts.slot2_on = 1;
    g_state.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    g_state.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    g_state.p25_p2_audio_allowed[0] = 1;
    g_state.p25_p2_audio_allowed[1] = 1;
    g_state.audio_out_buf = g_out_s[0];
    g_state.audio_out_bufR = g_out_s[1];
    g_state.audio_out_buf_p = g_out_s[0] + 100;
    g_state.audio_out_buf_pR = g_out_s[1] + 100;
    g_state.audio_out_float_buf = g_out_f[0];
    g_state.audio_out_float_bufR = g_out_f[1];
    g_state.audio_out_float_buf_p = g_out_f[0] + 100;
    g_state.audio_out_float_buf_pR = g_out_f[1] + 100;
    dsd_p25p2_playout_reset(&g_state, -1);
    g_block_count = 0;
    g_capture_float = floating;
}

static uint32_t g_serial = 0;

/* One burst on @p slot at @p pair: 4V or 2V frames tagged tag, tag+1, ...; any other kind queues no frame. */
static void
burst(int slot, int pair, dsd_p25p2_burst_kind kind, int tag) {
    const int frames = (kind == DSD_P25P2_BURST_4V) ? 4 : ((kind == DSD_P25P2_BURST_2V) ? 2 : 0);
    g_serial++;
    for (int f = 0; f < frames; f++) {
        short* s = (slot == 0) ? g_state.s_l : g_state.s_r;
        float* fl = (slot == 0) ? g_state.audio_out_temp_buf : g_state.audio_out_temp_bufR;
        for (int i = 0; i < 160; i++) {
            s[i] = (short)(tag + f);
            fl[i] = (slot == 0) ? 1000.0f : -1000.0f;
        }
        g_state.mbe_short_silenced[slot] = 0U;
        dsd_p25p2_playout_stage(&g_opts, &g_state, slot, pair, g_serial, &k_open_verdict);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, slot, pair, kind);
}

/* A slot's 4V/2V/SACCH kind at @p pair for a transmission whose 2V sits at @p two_v. */
static dsd_p25p2_burst_kind
kind_for(int pair, int two_v) {
    if (pair == 5) {
        return DSD_P25P2_BURST_SACCH;
    }
    return (pair == two_v) ? DSD_P25P2_BURST_2V : DSD_P25P2_BURST_4V;
}

static int
frames_of(dsd_p25p2_burst_kind kind) {
    return (kind == DSD_P25P2_BURST_4V) ? 4 : ((kind == DSD_P25P2_BURST_2V) ? 2 : 0);
}

/* Both slots through one superframe; a NULL tag leaves that slot idle. Tags advance by the frames each burst carries,
   a lost one's included, and @p lost_pair0 (or -1) loses slot 1's burst at that pair. */
static void
superframe(int two_v0, int two_v1, int* tag0, int* tag1, int lost_pair0) {
    for (int pair = 0; pair < 6; pair++) {
        dsd_p25p2_burst_kind k0 = kind_for(pair, two_v0);
        if (pair == lost_pair0) {
            k0 = DSD_P25P2_BURST_LOST;
        }
        if (tag0) {
            burst(0, pair, k0, *tag0);
            if (pair != lost_pair0) {
                *tag0 += frames_of(k0);
            } else {
                *tag0 += frames_of(kind_for(pair, two_v0));
            }
        } else {
            dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, pair, DSD_P25P2_BURST_OTHER);
        }
        if (tag1) {
            const dsd_p25p2_burst_kind k1 = kind_for(pair, two_v1);
            burst(1, pair, k1, *tag1);
            *tag1 += frames_of(k1);
        } else {
            dsd_p25p2_playout_burst_done(&g_opts, &g_state, 1, pair, DSD_P25P2_BURST_OTHER);
        }
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    }
}

/* Two aligned calls: 18 blocks per superframe, each channel its own slot's frames in order, played 4/4/4/4/2/0. */
static int
test_aligned_two_slots(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    int tag0 = 1000;
    int tag1 = 2000;
    superframe(4, 4, &tag0, &tag1, -1);
    rc |= expect_int("aligned blocks", g_block_count, 18);
    for (int i = 0; i < g_block_count && i < 18; i++) {
        rc |= expect_int("aligned left order", g_blocks[i].left, 1000 + i);
        rc |= expect_int("aligned right order", g_blocks[i].right, 2000 + i);
    }
    superframe(4, 4, &tag0, &tag1, -1);
    rc |= expect_int("aligned blocks two superframes", g_block_count, 36);
    rc |= expect_int("aligned left continues", g_blocks[35].left, 1035);
    rc |= expect_int("aligned right continues", g_blocks[35].right, 2035);
    return rc;
}

/* Slot 1's 2V at pair 4, slot 2's at pair 0: never more blocks than air periods, both channels in order, carry <= 2. */
static int
test_rotated_slots(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    int tag0 = 1000;
    int tag1 = 2000;
    int max_carry = 0;
    for (int sf = 0; sf < 3; sf++) {
        for (int pair = 0; pair < 6; pair++) {
            const dsd_p25p2_burst_kind k0 = kind_for(pair, 4);
            const dsd_p25p2_burst_kind k1 = kind_for(pair, 0);
            burst(0, pair, k0, tag0);
            tag0 += frames_of(k0);
            burst(1, pair, k1, tag1);
            tag1 += frames_of(k1);
            dsd_p25p2_playout_pair_done(&g_opts, &g_state);
            for (int s = 0; s < 2; s++) {
                const int lvl = dsd_p25p2_playout_level(&g_state, s);
                max_carry = (lvl > max_carry) ? lvl : max_carry;
            }
        }
        rc |= expect_int("rotated blocks per superframe", g_block_count, 18L * (sf + 1));
    }
    rc |= expect_int("rotated carry bound", max_carry <= 2, 1);
    int want0 = 1000;
    int want1 = 2000;
    for (int i = 0; i < g_block_count; i++) {
        rc |= expect_int("rotated left order", g_blocks[i].left, want0++);
        rc |= expect_int("rotated right order", g_blocks[i].right, want1++);
    }
    return rc;
}

/* A lone call plays in both ears, 18 blocks per superframe. */
static int
test_single_call_mirrored(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 1U, 200U, 2000U);
    g_state.p25_p2_audio_allowed[0] = 0;
    int tag1 = 2000;
    superframe(4, 2, NULL, &tag1, -1);
    rc |= expect_int("single blocks", g_block_count, 18);
    for (int i = 0; i < g_block_count; i++) {
        rc |= expect_int("single left mirrors", g_blocks[i].left, 2000 + i);
        rc |= expect_int("single right", g_blocks[i].right, 2000 + i);
    }
    return rc;
}

/* With the 2V pair proven, a lost 4V plays 4 silent blocks where it was; the next frames follow, nothing reordered. */
static int
test_lost_burst_fills_in_place(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    int tag0 = 1000;
    superframe(4, 4, &tag0, NULL, -1);
    superframe(4, 4, &tag0, NULL, 2);
    rc |= expect_int("lost blocks", g_block_count, 36);
    for (int i = 18; i < 36; i++) {
        const int pos = i - 18;
        const int want = (pos >= 8 && pos < 12) ? 0 : 1000 + i;
        rc |= expect_int("lost in place", g_blocks[i].left, want);
    }
    return rc;
}

/* A new transmission loses its first 2V (pair 1) with no warm-up: a placeholder holds it until the later 4Vs prove
   pair 1, then it plays 2 silent blocks in place and the superframe totals 18. */
static int
test_early_2v_lost_without_warmup(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    burst(0, 1, DSD_P25P2_BURST_LOST, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("early 2v placeholder holds", g_block_count, 4);
    burst(0, 2, DSD_P25P2_BURST_4V, 1004);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    burst(0, 3, DSD_P25P2_BURST_4V, 1008);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    burst(0, 4, DSD_P25P2_BURST_4V, 1012);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    burst(0, 5, DSD_P25P2_BURST_SACCH, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("early 2v blocks", g_block_count, 18);
    const int want[18] = {1000, 1001, 1002, 1003, 0,    0,    1004, 1005, 1006,
                          1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015};
    for (int i = 0; i < 18 && i < g_block_count; i++) {
        rc |= expect_int("early 2v order", g_blocks[i].left, want[i]);
    }
    return rc;
}

/* Pairs 0/1/3 always decode, 2 (the true 2V) and 4 always lost: every superframe totals 18 against a good companion,
   and from the second superframe on each pair plays as it completes. */
static int
test_repeated_unknown_phase_loss(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    int tag0 = 1000;
    int tag1 = 2000;
    for (int sf = 0; sf < 4; sf++) {
        for (int pair = 0; pair < 6; pair++) {
            dsd_p25p2_burst_kind k0 = kind_for(pair, 2);
            if (pair == 2 || pair == 4) {
                k0 = DSD_P25P2_BURST_LOST;
            }
            burst(0, pair, k0, tag0);
            tag0 += frames_of(k0);
            const dsd_p25p2_burst_kind k1 = kind_for(pair, 4);
            burst(1, pair, k1, tag1);
            tag1 += frames_of(k1);
            const int before = g_block_count;
            dsd_p25p2_playout_pair_done(&g_opts, &g_state);
            if (sf > 0 && pair < 5) {
                rc |= expect_int("unknown phase plays each pair", g_block_count > before, 1);
            }
        }
        rc |= expect_int("unknown phase superframe total", g_block_count, 18L * (sf + 1));
    }
    int want1 = 2000;
    for (int i = 0; i < g_block_count; i++) {
        rc |= expect_int("unknown phase companion order", g_blocks[i].right, want1++);
    }
    return rc;
}

/* A provisional 2V pair later disproved: the extra silence becomes debt, and the next lost burst repays it. */
static int
test_provisional_correction_repays_debt(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    int tag0 = 1000;
    const int lost[3][2] = {{2, 4}, {2, -1}, {2, -1}};
    for (int sf = 0; sf < 3; sf++) {
        for (int pair = 0; pair < 6; pair++) {
            dsd_p25p2_burst_kind k0 = kind_for(pair, 2);
            if (pair == lost[sf][0] || pair == lost[sf][1]) {
                k0 = DSD_P25P2_BURST_LOST;
            }
            burst(0, pair, k0, tag0);
            tag0 += frames_of(k0);
            dsd_p25p2_playout_pair_done(&g_opts, &g_state);
        }
    }
    rc |= expect_int("debt repaid total", g_block_count, 54);
    int decoded = 0;
    for (int i = 0; i < g_block_count; i++) {
        decoded += (g_blocks[i].left != 0) ? 1 : 0;
    }
    /* Superframe 1 decodes pairs 0, 1 and 3; superframes 2 and 3 decode pairs 0, 1, 3 and 4. */
    rc |= expect_int("debt decoded frames all played", decoded, 12 + 16 + 16);
    return rc;
}

/* One-channel output: 160-sample blocks, a lone call at full level, two calls averaged. */
static int
test_mono_output(void) {
    int rc = 0;
    setup(0, 1);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_OTHER, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("mono blocks", g_block_count, 4);
    rc |= expect_int("mono channels", g_blocks[0].channels, 1);
    rc |= expect_int("mono full level", g_blocks[0].left, 1000);

    setup(0, 1);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_4V, 3000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("mono two calls blocks", g_block_count, 4);
    rc |= expect_int("mono two calls averaged", g_blocks[0].left, 2000);
    return rc;
}

/* A slot END (close) mid-superframe: its queued frames still play once, alongside the companion, which continues. */
static int
test_close_keeps_tail(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    /* Slot 1's 2V at pair 0 and slot 2's 4V: slot 2 carries 2 frames over. */
    burst(0, 0, DSD_P25P2_BURST_2V, 1000);
    burst(1, 0, DSD_P25P2_BURST_4V, 2000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("close carry before", dsd_p25p2_playout_level(&g_state, 1), 2);
    /* Slot 2 ends at pair 1 (its burst is the END FACCH); slot 1 goes on. */
    burst(0, 1, DSD_P25P2_BURST_4V, 1002);
    dsd_p25p2_playout_close(&g_state, 1);
    burst(1, 1, DSD_P25P2_BURST_OTHER, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("close blocks", g_block_count, 6);
    rc |= expect_int("close tail right 1", g_blocks[2].right, 2002);
    rc |= expect_int("close tail right 2", g_blocks[3].right, 2003);
    rc |= expect_int("close companion continues", g_blocks[5].left, 1005);
    rc |= expect_int("close tail drained", dsd_p25p2_playout_level(&g_state, 1), 0);
    return rc;
}

/* The verdict: a blocked burst is not heard; a hold plays from a switched-off slot; both slots off mutes all. */
static int
test_verdicts(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    const dsd_p25p2_playout_verdict blocked = {1U, 0U, 1U, 1U};
    const dsd_p25p2_playout_verdict held = {0U, 1U, 1U, 1U};
    g_serial++;
    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 160; i++) {
            g_state.s_l[i] = (short)(1000 + f);
        }
        dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 0, g_serial, &blocked);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 0, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("blocked not heard", g_block_count, 0);

    /* The hold is on the call's talkgroup: the slot's current verdict, applied while the call is on, holds it too. */
    g_state.tg_hold = 100U;
    g_opts.slot1_on = 0;
    g_serial++;
    for (int f = 0; f < 4; f++) {
        dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 1, g_serial, &held);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 1, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("hold plays from off slot", g_block_count, 4);
    rc |= expect_int("hold leaves the switch", g_opts.slot1_on, 0);

    g_opts.slot2_on = 0;
    g_serial++;
    for (int f = 0; f < 4; f++) {
        dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 2, g_serial, &held);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 2, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("both off mutes a hold", g_block_count, 4);
    return rc;
}

/* Float output plays every frame too, through the same queues: slot 1 positive, slot 2 negative. */
static int
test_float_output(void) {
    int rc = 0;
    setup(1, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    int tag0 = 1000;
    int tag1 = 2000;
    superframe(4, 0, &tag0, &tag1, -1);
    rc |= expect_int("float blocks", g_block_count, 18);
    for (int i = 0; i < g_block_count; i++) {
        rc |= expect_int("float left is slot 1", g_blocks[i].fleft > 0.0f, 1);
        rc |= expect_int("float right is slot 2", g_blocks[i].fright < 0.0f, 1);
    }
    return rc;
}

/* At a 48 kHz output nothing plays, but the queues still empty and the streams still run. */
static int
test_other_rate_discards(void) {
    int rc = 0;
    setup(0, 2);
    g_opts.pulse_digi_rate_out = 48000;
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("48k nothing played", g_block_count, 0);
    rc |= expect_int("48k queue emptied", dsd_p25p2_playout_level(&g_state, 0), 0);
    return rc;
}

/* The vocoder's output buffer pointers wrap at every pair, in either format and at any output rate (the playout writes
   nothing at 48 kHz, but the vocoder still advances them): SS18/FS4 used to do it. */
static int
test_buffer_wrap(void) {
    int rc = 0;
    for (int run = 0; run < 4; run++) {
        const int floating = run & 1;
        setup(floating, 2);
        g_opts.pulse_digi_rate_out = (run < 2) ? 8000 : 48000;
        g_state.audio_out_idx2 = 800000;
        g_state.audio_out_idx2R = 800000;
        g_state.audio_out_buf_p = g_out_s[0] + 3000;
        g_state.audio_out_buf_pR = g_out_s[1] + 3000;
        g_state.audio_out_idx = 5;
        g_state.audio_out_idxR = 7;
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
        rc |= expect_int("wrap play index left", g_state.audio_out_idx, 0);
        rc |= expect_int("wrap play index right", g_state.audio_out_idxR, 0);
        rc |= expect_int("wrap left", g_state.audio_out_buf_p == g_out_s[0] + 100, 1);
        rc |= expect_int("wrap right", g_state.audio_out_buf_pR == g_out_s[1] + 100, 1);
        rc |= expect_int("wrap index", g_state.audio_out_idx2, 0);
    }
    return rc;
}

static int
stamped(void) {
    uint64_t stamp = 0;
    int32_t age_ms = -1;
    dsd_audio_activity_read(&stamp, &age_ms);
    return age_ms >= 0;
}

/* The #574 stamp: decoded frames stamp; the vocoder's mute silence never does. */
static int
test_activity_stamp(void) {
    int rc = 0;
    dsd_audio_activity_arm();
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    dsd_audio_activity_reset();
    g_serial++;
    for (int f = 0; f < 4; f++) {
        g_state.mbe_short_silenced[0] = 1U;
        dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 0, g_serial, &k_open_verdict);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 0, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("silence played", g_block_count, 4);
    rc |= expect_int("silence never stamps", stamped(), 0);
    burst(0, 1, DSD_P25P2_BURST_4V, 1004);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("decoded frames stamp", stamped(), 1);
    dsd_audio_activity_reset();
    return rc;
}

/* A stream belongs to its call's epoch: a new transmission on the slot starts a new stream, the old tail still plays. */
static int
test_epoch_change(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_2V, 1000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    (void)dsd_call_state_end(&g_state, 0U, 0.0);
    seed_call(&g_state, 0U, 300U, 3000U);
    burst(0, 1, DSD_P25P2_BURST_OTHER, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("epoch old stream no fill", g_block_count, 2);
    burst(0, 2, DSD_P25P2_BURST_4V, 3000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("epoch new stream plays", g_block_count, 6);
    rc |= expect_int("epoch new frames", g_blocks[2].left, 3000);
    return rc;
}

/* A decoded frame of silence is audio: it plays in its place like any other frame. */
static int
test_decoded_silence_plays(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 0); /* frames tagged 0, 1, 2, 3: the first is all zeros */
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("silence frame plays", g_block_count, 4);
    rc |= expect_int("silence frame first", g_blocks[0].left, 0);
    rc |= expect_int("silence frame then audio", g_blocks[1].left, 1);
    return rc;
}

/* Issue #574: a switched-off slot's clear frames stay silent beside a crypto-muted companion. */
static int
test_switched_off_slot_beside_muted_companion(void) {
    int rc = 0;
    for (int off = 0; off < 2; off++) {
        setup(0, 2);
        seed_call(&g_state, 0U, 100U, 1000U);
        seed_call(&g_state, 1U, 200U, 2000U);
        if (off == 0) {
            g_opts.slot1_on = 0;
        } else {
            g_opts.slot2_on = 0;
        }
        g_state.p25_crypto_state[off ^ 1] = DSD_P25_CRYPTO_BLOCKED;
        burst(0, 0, DSD_P25P2_BURST_4V, 1000);
        burst(1, 0, DSD_P25P2_BURST_4V, 2000);
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
        rc |= expect_int(off == 0 ? "slot 1 off beside a crypto-muted slot 2 plays nothing"
                                  : "slot 2 off beside a crypto-muted slot 1 plays nothing",
                         g_block_count, 0);
        rc |= expect_int("muted pair still consumed", dsd_p25p2_playout_level(&g_state, off), 0);
    }
    return rc;
}

/* One-channel float output: a lone call at full level, two calls averaged (slot 1 positive, slot 2 negative). */
static int
test_float_mono_output(void) {
    int rc = 0;
    setup(1, 1);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_OTHER, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("float mono blocks", g_block_count, 4);
    rc |= expect_int("float mono channels", g_blocks[0].channels, 1);
    const float lone = g_blocks[0].fleft;
    rc |= expect_int("float mono lone call heard", lone > 0.0f, 1);

    setup(1, 1);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_4V, 2000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("float mono two calls blocks", g_block_count, 4);
    const float mixed = g_blocks[0].fleft;
    rc |= expect_int("float mono two calls averaged", (mixed < 0.01f * lone) && (mixed > -0.01f * lone), 1);
    return rc;
}

/* While a frame's call is still active, its slot's current talkgroup verdict applies at emission too: a Skip or a
   policy edit mutes frames queued before it. The commands that make them take the slot's verdict as they apply
   (dsd_p25p2_playout_note_policy()), as do the call's later bursts; the playout itself evaluates no policy as it plays.
   A frame of an ended call keeps the verdict it was queued with. */
static int
test_live_policy_mutes_queued_frames(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_2V, 1000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("live policy baseline plays", g_block_count, 2);
    burst(0, 1, DSD_P25P2_BURST_4V, 1002);
    rc |= expect_int("live policy skip armed",
                     dsd_tg_policy_call_skip_arm(&g_state, 100U, 1000U, 0, dsd_decode_now_mono_s()), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("skip mutes frames queued before it", g_block_count, 2);
    rc |= expect_int("skip consumes them in time", dsd_p25p2_playout_level(&g_state, 0), 0);

    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    rc |= expect_int("live policy lockout applied", dsd_tg_policy_set_mode(&g_state, 100U, 100U, "B"), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("lockout mutes the drained tail of the active call", g_block_count, 0);

    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    (void)dsd_call_state_end(&g_state, 0U, 0.0);
    rc |= expect_int("ended call lockout applied", dsd_tg_policy_set_mode(&g_state, 100U, 100U, "B"), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("an ended call's tail keeps its verdict", g_block_count, 4);
    return rc;
}

/* A call's later burst carries the decode gate's current decision for it: the frames the call queued before it play
   under that decision too (a skip was armed between the two bursts by a path that took no verdict, which moves nothing
   the emission's own check reads). */
static int
test_later_burst_refreshes_the_verdict(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    rc |= expect_int("skip armed", dsd_tg_policy_call_skip_arm(&g_state, 100U, 1000U, 0, dsd_decode_now_mono_s()), 0);
    const dsd_p25p2_playout_verdict blocked = {1U, 0U, 1U, 1U};
    g_serial++;
    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 160; i++) {
            g_state.s_l[i] = (short)(1004 + f);
        }
        g_state.mbe_short_silenced[0] = 0U;
        dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 1, g_serial, &blocked);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 1, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the earlier frames play under the later decision", g_block_count, 0);
    rc |= expect_int("and are consumed in time", dsd_p25p2_playout_level(&g_state, 0), 0);
    return rc;
}

/* A call on a patch's supergroup heard because its member talkgroup is allow-listed: the member leaves the patch
   (regroup signaling) or the patch goes stale while the call's frames wait, with no burst in between. The call is now
   judged on the unlisted supergroup, so the waiting frames are muted: the playout notices the policy target move. */
static int
test_patch_change_retakes_the_verdict(void) {
    int rc = 0;
    for (int stale = 0; stale < 2; stale++) {
        setup(0, 2);
        g_opts.trunk_use_allow_list = 1;
        g_state.synctype = g_state.lastsynctype = DSD_SYNC_P25P2_POS;
        rc |= expect_int("member listed", dsd_tg_policy_set_mode(&g_state, 0x0200U, 0x0200U, "A"), 0);
        g_state.p25_patch_count = 1;
        g_state.p25_patch_active[0] = 1;
        g_state.p25_patch_sgid[0] = 0x0100U;
        g_state.p25_patch_wgid_count[0] = 1;
        g_state.p25_patch_wgid[0][0] = 0x0200U;
        g_state.p25_patch_last_update[0] = dsd_decode_time();
        const dsd_call_observation observation = {
            .protocol = DSD_SYNC_P25P2_POS,
            .slot = 0U,
            .kind = DSD_CALL_KIND_GROUP_VOICE,
            .ota_target_id = 0x0100U,
            .policy_target_id = 0x0200U,
            .ota_source_id = 1000U,
        };
        (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
        g_state.p25_p2_audio_allowed[1] = 0;
        burst(0, 0, DSD_P25P2_BURST_4V, 1000);
        if (stale) {
            g_state.p25_patch_last_update[0] = dsd_decode_time() - 60;
        } else {
            g_state.p25_patch_wgid_count[0] = 0;
        }
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
        rc |= expect_int(stale ? "a stale patch mutes the waiting frames" : "a member leaving mutes the waiting frames",
                         g_block_count, 0);
        rc |= expect_int("they are consumed in time", dsd_p25p2_playout_level(&g_state, 0), 0);
    }
    return rc;
}

/* The decode gate allows a burst of a call heard through its patch's allow-listed member; the patch goes stale before
   the burst's frames are queued. The burst keeps the gate's verdict, and the slot's current verdict keeps what the gate
   judged the call on, so the emission notices the change and mutes the frames. */
static int
test_gate_decision_keeps_its_key(void) {
    int rc = 0;
    setup(0, 2);
    g_opts.trunk_use_allow_list = 1;
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    rc |= expect_int("member listed", dsd_tg_policy_set_mode(&g_state, 0x0200U, 0x0200U, "A"), 0);
    g_state.p25_patch_count = 1;
    g_state.p25_patch_active[0] = 1;
    g_state.p25_patch_sgid[0] = 0x0100U;
    g_state.p25_patch_wgid_count[0] = 1;
    g_state.p25_patch_wgid[0][0] = 0x0200U;
    g_state.p25_patch_last_update[0] = dsd_decode_time();
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 0x0100U,
        .policy_target_id = 0x0200U,
        .ota_source_id = 1000U,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    g_state.p25_p2_audio_allowed[1] = 0;
    dsd_p25p2_burst_decision decision;
    int decision_valid = 0;
    rc |= expect_int("the gate allows the patched call",
                     dsd_p25p2_decode_audio_allowed_verdict(&g_opts, &g_state, 0, 0x80, &decision, &decision_valid), 1);
    rc |= expect_int("and hands back its decision", decision_valid, 1);
    g_state.p25_patch_last_update[0] = dsd_decode_time() - 60;
    g_serial++;
    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 160; i++) {
            g_state.s_l[i] = (short)(1000 + f);
        }
        g_state.mbe_short_silenced[0] = 0U;
        dsd_p25p2_playout_stage_decided(&g_opts, &g_state, 0, 0, g_serial, &decision);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 0, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the stale patch mutes the frames", g_block_count, 0);
    return rc;
}

/* A private call whose source signaling names only after its first frames are queued (same epoch), the source being
   blocked: the call is now judged on both endpoints, so the waiting frames are muted. */
static int
test_source_learned_retakes_the_verdict(void) {
    int rc = 0;
    setup(0, 2);
    rc |= expect_int("source blocked", dsd_tg_policy_set_mode(&g_state, 3000U, 3000U, "B"), 0);
    dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_PRIVATE_VOICE,
        .ota_target_id = 500U,
        .policy_target_id = 500U,
        .ota_source_id = 0U,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    dsd_call_snapshot before;
    rc |= expect_int("private call active", dsd_call_state_get(&g_state, 0U, &before), 1);
    observation.ota_source_id = 3000U;
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_CONTINUE);
    dsd_call_snapshot after;
    rc |= expect_int("same transmission", dsd_call_state_get(&g_state, 0U, &after) == 1 && after.epoch == before.epoch,
                     1);
    rc |= expect_int("its source named", (int)after.ota_source_id, 3000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("a blocked source mutes the waiting frames", g_block_count, 0);
    rc |= expect_int("they are consumed in time", dsd_p25p2_playout_level(&g_state, 0), 0);
    return rc;
}

/* Entries still to play in @p s (removed ones aside). */
static int
q_live_entries(const dsd_p25p2_playout_slot* s) {
    int n = 0;
    for (int i = 0; i < s->count; i++) {
        n += s->q[(s->head + i) % DSD_P25P2_PLAYOUT_CAP].kind != DSD_P25P2_PLAYOUT_ENTRY_SKIP;
    }
    return n;
}

/* Fill @p s with @p count entries copied from @p model, tagged from @p tag on, and @p barriers barriers spread ahead of
   groups of four frames: a stream replaced at successive pairs while its companion held earlier entries. */
static int
fill_queue(dsd_p25p2_playout_slot* s, const dsd_p25p2_playout_entry* model, int count, int barriers, int tag) {
    int n = 0;
    for (int b = 0; n < count; b++) {
        if (b < barriers) {
            dsd_p25p2_playout_entry* e = &s->q[n++];
            *e = *model;
            e->kind = DSD_P25P2_PLAYOUT_ENTRY_BARRIER;
            e->clock = model->clock + 1U + (uint32_t)b;
        }
        for (int f = 0; f < 4 && n < count; f++) {
            dsd_p25p2_playout_entry* e = &s->q[n++];
            *e = *model;
            e->clock = model->clock + 1U + (uint32_t)(b < barriers ? b : barriers);
            for (int j = 0; j < 160; j++) {
                e->pcm.s16[j] = (int16_t)tag;
            }
            tag++;
        }
    }
    for (int i = 0; i < count; i++) {
        s->q[i].seq = (uint64_t)i + 1U;
    }
    s->head = 0;
    s->count = (uint8_t)count;
    s->pushed = (uint64_t)count;
    s->departed = 0U;
    return tag;
}

/* A queue full of live entries (four barriers and twenty frames, a stream replaced at successive pairs) while the
   companion's unresolved placeholder of an earlier pair holds every level at zero: the next decoded frame is still
   queued (the placeholders resolve as four and blocks play to make room), and every frame plays. */
static int
test_full_live_queue_keeps_the_frame(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_4V, 2000);
    dsd_p25p2_playout_slot* s0 = &g_state.p25p2_playout.slot[0];
    dsd_p25p2_playout_slot* s1 = &g_state.p25p2_playout.slot[1];
    const dsd_p25p2_playout_entry model = s0->q[s0->head];
    for (int i = 0; i < 4; i++) {
        s1->q[i] = s1->q[s1->head];
        s1->q[i].kind = DSD_P25P2_PLAYOUT_ENTRY_PENDING;
        s1->q[i].pair = 1U;
        s1->q[i].clock = model.clock;
        s1->q[i].seq = (uint64_t)i + 1U;
    }
    s1->head = 0;
    s1->count = 4;
    s1->pushed = 4U;
    s1->departed = 0U;
    const int tag = fill_queue(s0, &model, DSD_P25P2_PLAYOUT_CAP, 4, 1000);
    rc |= expect_int("every level held at zero", dsd_p25p2_playout_level(&g_state, 0), 0);
    g_serial++;
    for (int j = 0; j < 160; j++) {
        g_state.s_l[j] = (short)tag;
    }
    g_state.mbe_short_silenced[0] = 0U;
    dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 2, g_serial, &k_open_verdict);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    int frames = 0;
    int last = 0;
    for (int b = 0; b < g_block_count; b++) {
        if (g_blocks[b].left >= 1000 && g_blocks[b].left < 2000) {
            frames++;
            last = g_blocks[b].left;
        }
    }
    rc |= expect_int("every decoded frame plays", frames, 21);
    rc |= expect_int("the new one last", last, tag);
    return rc;
}

/* A queue three entries short of full, held back by its companion's placeholder of an earlier pair, misses a burst
   whose size is still unknown: a placeholder is four entries or none, so it is left out rather than cut short (a cut
   one's resolution would rewrite the queue's oldest frames), and every frame plays. */
static int
test_placeholder_is_whole_or_absent(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_4V, 2000);
    dsd_p25p2_playout_slot* s0 = &g_state.p25p2_playout.slot[0];
    dsd_p25p2_playout_slot* s1 = &g_state.p25p2_playout.slot[1];
    const dsd_p25p2_playout_entry model = s0->q[s0->head];
    for (int i = 0; i < 4; i++) {
        s1->q[i] = s1->q[s1->head];
        s1->q[i].kind = DSD_P25P2_PLAYOUT_ENTRY_PENDING;
        s1->q[i].pair = 1U;
        s1->q[i].clock = model.clock;
        s1->q[i].seq = (uint64_t)i + 1U;
    }
    s1->head = 0;
    s1->count = 4;
    s1->pushed = 4U;
    s1->departed = 0U;
    (void)fill_queue(s0, &model, DSD_P25P2_PLAYOUT_CAP - 3, 1, 1000);
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 1, DSD_P25P2_BURST_LOST);
    rc |= expect_int("no placeholder cut short", q_live_entries(s0) == DSD_P25P2_PLAYOUT_CAP - 3, 1);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    int expect_tag = 1000;
    int frames = 0;
    for (int b = 0; b < g_block_count; b++) {
        if (g_blocks[b].left >= 1000 && g_blocks[b].left < 2000) {
            rc |= expect_int("the frames in order", g_blocks[b].left, expect_tag);
            expect_tag++;
            frames++;
        }
    }
    rc |= expect_int("every frame plays", frames, DSD_P25P2_PLAYOUT_CAP - 3 - 1);
    return rc;
}

/* An alert raised behind ten frames, then four entries queued after it are removed (repaid fill) and a full queue
   reclaims them: the alert still sounds after all ten frames ahead of it, not after six. */
static int
test_alert_fence_survives_reclaim(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    dsd_p25p2_playout_slot* s = &g_state.p25p2_playout.slot[0];
    const dsd_p25p2_playout_entry model = s->q[s->head];
    (void)fill_queue(s, &model, 10, 0, 1000);
    beeper(&g_opts, &g_state, 0, 40, 86, 1);
    const int tag = fill_queue(s, &model, DSD_P25P2_PLAYOUT_CAP, 0, 1000);
    for (int i = 10; i < 14; i++) {
        s->q[i].kind = DSD_P25P2_PLAYOUT_ENTRY_SKIP;
    }
    g_serial++;
    for (int j = 0; j < 160; j++) {
        g_state.s_l[j] = (short)tag;
    }
    g_state.mbe_short_silenced[0] = 0U;
    dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 2, g_serial, &k_open_verdict);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("frames and the alert", g_block_count, 21 + 2);
    for (int b = 0; b < 10 && b < g_block_count; b++) {
        rc |= expect_int("the ten frames first", g_blocks[b].right, 1000 + b);
    }
    for (int b = 10; b < 12 && b < g_block_count; b++) {
        rc |= expect_int("then the alert", g_blocks[b].right, 0);
    }
    return rc;
}

/* Policy that moves without a note while a call's frames wait: a row learned over the air (a talker alias) lists the
   call's talkgroup again under the allow list, and a hold set by a path that took no verdict mutes the call. Each
   emission notices the policy table's version and the hold moved and takes the call's verdict again. */
static int
test_policy_write_retakes_the_verdict(void) {
    int rc = 0;
    setup(0, 2);
    rc |= expect_int("an unrelated row", dsd_tg_policy_set_mode(&g_state, 999U, 999U, "A"), 0);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    g_opts.trunk_use_allow_list = 1;
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    rc |= expect_int("the allow list blocks the unlisted call", g_state.p25p2_playout.slot[0].live_verdict.blocked, 1);
    dsd_tg_policy_entry row;
    rc |= expect_int("learned row",
                     dsd_tg_policy_make_exact_entry(100U, "A", "alias", DSD_TG_POLICY_SOURCE_RUNTIME_ALIAS, &row), 0);
    rc |= expect_int("learned row added",
                     dsd_tg_policy_upsert_exact(&g_state, &row, DSD_TG_POLICY_UPSERT_ADD_IF_MISSING), 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the learned row lets the waiting frames play", g_block_count, 4);

    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    g_state.tg_hold = 4321U;
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("a hold on another talkgroup mutes the waiting frames", g_block_count, 0);
    return rc;
}

/* A full queue whose interior holds removed entries (repaid fill, a placeholder resolved short) still takes the next
   decoded frame: the push reclaims them first rather than dropping the frame. */
static int
test_full_queue_reclaims_removed_entries(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    dsd_p25p2_playout_slot* s = &g_state.p25p2_playout.slot[0];
    const dsd_p25p2_playout_entry frame = s->q[s->head];
    int tag = 1000;
    for (int i = 0; i < DSD_P25P2_PLAYOUT_CAP; i++) {
        dsd_p25p2_playout_entry* e = &s->q[i];
        *e = frame;
        if (i >= 10 && i < 14) {
            e->kind = DSD_P25P2_PLAYOUT_ENTRY_SKIP;
            continue;
        }
        for (int j = 0; j < 160; j++) {
            e->pcm.s16[j] = (int16_t)tag;
        }
        tag++;
    }
    s->head = 0;
    s->count = DSD_P25P2_PLAYOUT_CAP;
    g_serial++;
    for (int j = 0; j < 160; j++) {
        g_state.s_l[j] = (short)tag;
    }
    g_state.mbe_short_silenced[0] = 0U;
    dsd_p25p2_playout_stage(&g_opts, &g_state, 0, 1, g_serial, &k_open_verdict);
    rc |= expect_int("the new frame is queued", dsd_p25p2_playout_level(&g_state, 0), DSD_P25P2_PLAYOUT_CAP - 4 + 1);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("every frame plays", g_block_count, DSD_P25P2_PLAYOUT_CAP - 4 + 1);
    if (g_block_count > 0) {
        rc |= expect_int("the new frame last", g_blocks[g_block_count - 1].left, tag);
    }
    return rc;
}

/* The static WAV records the blocks that play, in stereo, without a slot whose talkgroup may be heard but not
   recorded: a record-blocked slot plays but leaves its channel silent in the file, a record-blocked lone slot (heard
   in both ears) writes nothing, and a slot that is not heard is never recorded. */
static int
read_wav(const char* path, short* out, int max, int* frames) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (!f) {
        DSD_FPRINTF(stderr, "FAIL wav open: %s\n", sf_strerror(NULL));
        return 1;
    }
    *frames = (int)sf_readf_short(f, out, (sf_count_t)(max / 2));
    sf_close(f);
    return info.channels == 2 ? 0 : 1;
}

static int
run_static_wav_case(const char* what, int blocked_slot, int companion, int left_on, int want_frames, int want_left,
                    int want_right) {
    static short samples[160 * 2 * 8];
    char path[DSD_TEST_PATH_MAX];
    int rc = 0;
    const int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_p25p2_wav");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL wav temp file: dsd_test_mkstemp returned %d\n", fd);
        return 1;
    }
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    setup(0, 2);
    g_opts.wav_out_f = sf_open(path, SFM_WRITE, &info);
    g_opts.static_wav_file = 1;
    g_opts.slot1_on = left_on;
    seed_call(&g_state, 0U, 100U, 1000U);
    if (companion) {
        seed_call(&g_state, 1U, 200U, 2000U);
    } else {
        g_state.p25_p2_audio_allowed[1] = 0;
    }
    const dsd_p25p2_playout_verdict record_blocked = {0U, 0U, 1U, 0U};
    for (int slot = 0; slot < (companion ? 2 : 1); slot++) {
        g_serial++;
        for (int f = 0; f < 2; f++) {
            short* pcm = (slot == 0) ? g_state.s_l : g_state.s_r;
            for (int i = 0; i < 160; i++) {
                pcm[i] = (short)((slot == 0) ? 1111 : 2222);
            }
            g_state.mbe_short_silenced[slot] = 0U;
            dsd_p25p2_playout_stage(&g_opts, &g_state, slot, 0, g_serial,
                                    (slot == blocked_slot) ? &record_blocked : &k_open_verdict);
        }
        dsd_p25p2_playout_burst_done(&g_opts, &g_state, slot, 0, DSD_P25P2_BURST_2V);
    }
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int(what, g_block_count, 2);
    sf_close(g_opts.wav_out_f);
    g_opts.wav_out_f = NULL;
    int frames = 0;
    rc |= read_wav(path, samples, (int)(sizeof(samples) / sizeof(samples[0])), &frames);
    rc |= expect_int(what, frames, want_frames);
    if (frames > 0) {
        rc |= expect_int(what, samples[0], want_left);
        rc |= expect_int(what, samples[1], want_right);
    }
    (void)remove(path);
    return rc;
}

static int
test_static_wav_records_what_plays(void) {
    int rc = 0;
    rc |= run_static_wav_case("record-blocked slot 1 plays, its channel stays out of the wav", 0, 1, 1, 320, 0, 2222);
    rc |= run_static_wav_case("record-blocked slot 2 plays, its channel stays out of the wav", 1, 1, 1, 320, 1111, 0);
    rc |= run_static_wav_case("record-blocked lone slot writes nothing", 0, 0, 1, 0, 0, 0);
    rc |= run_static_wav_case("an unheard slot is never recorded", -1, 1, 0, 320, 2222, 2222);
    return rc;
}

/* A user block that ends the call before the drain (Skip, lockout) takes the call's verdict first: the frames the call
   queued stay muted in the drain after the call ended. */
static int
test_noted_policy_mutes_ended_call_tail(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    rc |= expect_int("noted policy skip armed",
                     dsd_tg_policy_call_skip_arm(&g_state, 100U, 1000U, 0, dsd_decode_now_mono_s()), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    (void)dsd_call_state_end(&g_state, 0U, 0.0);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("noted skip mutes the ended call's tail", g_block_count, 0);
    rc |= expect_int("noted skip consumes it", dsd_p25p2_playout_level(&g_state, 0), 0);
    return rc;
}

/* Seeded random transmissions (2V phases, start pairs, a late companion, burst losses): every decoded frame plays
   exactly once and in order, in both channels' streams. A placeholder resolved to 2 frames or repaid fill leaves
   removed entries inside the queue, and they must not cost a decoded frame its place. */
static uint32_t g_lcg;

static int
lcg(int mod) {
    g_lcg = (g_lcg * 1103515245U) + 12345U;
    return (int)((g_lcg >> 8) % (uint32_t)mod);
}

// Slot @p slot's tags are base..base+3999: a block carries one in its own channel, or in both when it plays alone.
static int
check_played_once_in_order(int staged, int base) {
    int played = 0;
    int last = 0;
    for (int b = 0; b < g_block_count; b++) {
        const int l = g_blocks[b].left;
        const int r = g_blocks[b].right;
        const int tag = (l >= base && l < base + 4000) ? l : ((r >= base && r < base + 4000) ? r : 0);
        if (tag == 0) {
            continue;
        }
        if (tag <= last) {
            return 1;
        }
        last = tag;
        played++;
    }
    return played != staged;
}

static int
test_random_losses_play_every_frame(void) {
    int failures = 0;
    for (int trial = 0; trial < 5000; trial++) {
        g_lcg = ((uint32_t)trial * 2654435761U) + 1U;
        setup(0, 2);
        const int two_v[2] = {lcg(5), lcg(5)};
        const int start_sf[2] = {0, lcg(3)};
        const int start_pair[2] = {lcg(5), lcg(5)};
        const int loss_pct = lcg(40);
        const int superframes = 4 + lcg(6);
        int started[2] = {0, 0};
        int tag[2] = {1000, 5000};
        int staged[2] = {0, 0};
        for (int sf = 0; sf < superframes; sf++) {
            for (int pair = 0; pair < 6; pair++) {
                for (int slot = 0; slot < 2; slot++) {
                    if (!started[slot] && pair < 5
                        && (sf > start_sf[slot] || (sf == start_sf[slot] && pair >= start_pair[slot]))) {
                        started[slot] = 1;
                        seed_call(&g_state, (uint8_t)slot, 100U + (uint64_t)slot, 1000U);
                    }
                    if (!started[slot]) {
                        dsd_p25p2_playout_burst_done(&g_opts, &g_state, slot, pair, DSD_P25P2_BURST_OTHER);
                    } else if (pair == 5) {
                        burst(slot, pair, DSD_P25P2_BURST_SACCH, 0);
                    } else {
                        const dsd_p25p2_burst_kind kind = kind_for(pair, two_v[slot]);
                        if (lcg(100) < loss_pct) {
                            burst(slot, pair, DSD_P25P2_BURST_LOST, 0);
                        } else {
                            burst(slot, pair, kind, tag[slot]);
                            staged[slot] += frames_of(kind);
                        }
                        tag[slot] += frames_of(kind);
                    }
                }
                dsd_p25p2_playout_pair_done(&g_opts, &g_state);
            }
        }
        dsd_p25p2_playout_drain(&g_opts, &g_state);
        if (check_played_once_in_order(staged[0], 1000) || check_played_once_in_order(staged[1], 5000)) {
            DSD_FPRINTF(stderr, "random losses trial %d: a decoded frame did not play once in order\n", trial);
            failures++;
        }
    }
    return expect_int("random losses every decoded frame plays once", failures, 0);
}

/* A slot admitted while its companion's earlier air time is still queued (here behind the placeholder for the
   companion's lost pair-1 burst) starts at its own pair: its first frames play beside the companion's frames of that
   pair, not beside the companion's pair-1 silence, and no standing carry is left once the companion's phase is proven
   (both 2V positions at pair 4). */
static int
test_late_admission_keeps_its_air_time(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 1, 0, DSD_P25P2_BURST_OTHER);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    burst(0, 1, DSD_P25P2_BURST_LOST, 0);
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 1, 1, DSD_P25P2_BURST_OTHER);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    seed_call(&g_state, 1U, 200U, 2000U);
    int tag0 = 1008;
    int tag1 = 2000;
    for (int pair = 2; pair < 6; pair++) {
        const dsd_p25p2_burst_kind k = kind_for(pair, 4);
        burst(0, pair, k, tag0);
        burst(1, pair, k, tag1);
        tag0 += frames_of(k);
        tag1 += frames_of(k);
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    }
    rc |= expect_int("late admission blocks", g_block_count, 18);
    for (int b = 4; b < 8 && b < g_block_count; b++) {
        rc |= expect_int("late admission companion's pair-1 silence plays alone", g_blocks[b].right, 0);
    }
    for (int b = 8; b < 18 && b < g_block_count; b++) {
        rc |= expect_int("late admission companion frames", g_blocks[b].left, 1008 + (b - 8));
        rc |= expect_int("late admission frames beside them", g_blocks[b].right, 2000 + (b - 8));
    }
    superframe(4, 4, &tag0, &tag1, -1);
    rc |= expect_int("late admission next superframe", g_block_count, 36);
    rc |= expect_int("late admission leaves no carry", dsd_p25p2_playout_level(&g_state, 0), 0);
    rc |= expect_int("late admission stays beside", g_blocks[35].right, tag1 - 1);
    return rc;
}

#if defined(DSD_TEST_WRAP_CALL_STATE)
/* The no-carrier pass ends the calls on the decoder thread while the watchdog, under the tick guard, may be taking a
   slot's talkgroup verdict. GNU ld --wrap ends slot 1's call right after the playout's first look at it while armed,
   so the case exists only where that links, like the other wrap tests. */
static int g_end_after_look = 0;

// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __real_dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out);
int __wrap_dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out);

int
__wrap_dsd_call_state_get(const dsd_state* state, uint8_t slot, dsd_call_snapshot* out) {
    const int rc = __real_dsd_call_state_get(state, slot, out);
    if (g_end_after_look && slot == 0U && rc > 0 && out->phase == DSD_CALL_PHASE_ACTIVE) {
        g_end_after_look = 0;
        (void)dsd_call_state_end(&g_state, 0U, 0.0);
    }
    return rc;
}

/* While armed, the first patch goes stale right after the next group-call policy evaluation returns: the clock crossing
   the patch's TTL inside the decode gate. */
static int g_stale_patch_after_evaluation = 0;

int __real_dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                             int encrypted, int data_call, dsd_tg_policy_decision* out);
int __wrap_dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                             int encrypted, int data_call, dsd_tg_policy_decision* out);

int
__wrap_dsd_tg_policy_evaluate_group_call(const dsd_opts* opts, const dsd_state* state, uint32_t tg, uint32_t src,
                                         int encrypted, int data_call, dsd_tg_policy_decision* out) {
    const int rc = __real_dsd_tg_policy_evaluate_group_call(opts, state, tg, src, encrypted, data_call, out);
    if (g_stale_patch_after_evaluation) {
        g_stale_patch_after_evaluation = 0;
        g_state.p25_patch_last_update[0] = dsd_decode_time() - 60;
    }
    return rc;
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)

/* The patch through whose allow-listed member a call is heard goes stale while the decode gate evaluates the call's
   burst: the decision and its key come from the same look (the member), so the emission notices the patch gone and
   mutes the burst's frames. */
static int
test_gate_key_matches_its_decision(void) {
    int rc = 0;
    setup(0, 2);
    g_opts.trunk_use_allow_list = 1;
    g_state.synctype = g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    rc |= expect_int("member listed", dsd_tg_policy_set_mode(&g_state, 0x0200U, 0x0200U, "A"), 0);
    g_state.p25_patch_count = 1;
    g_state.p25_patch_active[0] = 1;
    g_state.p25_patch_sgid[0] = 0x0100U;
    g_state.p25_patch_wgid_count[0] = 1;
    g_state.p25_patch_wgid[0][0] = 0x0200U;
    g_state.p25_patch_last_update[0] = dsd_decode_time();
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = 0U,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = 0x0100U,
        .policy_target_id = 0x0200U,
        .ota_source_id = 1000U,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
    g_state.p25_p2_audio_allowed[1] = 0;
    dsd_p25p2_burst_decision decision;
    int decision_valid = 0;
    g_stale_patch_after_evaluation = 1;
    rc |= expect_int("the gate allows the patched call",
                     dsd_p25p2_decode_audio_allowed_verdict(&g_opts, &g_state, 0, 0x80, &decision, &decision_valid), 1);
    rc |= expect_int("the patch went stale during it", g_stale_patch_after_evaluation, 0);
    rc |= expect_int("its key is the member it judged", (int)decision.key.policy_target, 0x0200);
    g_serial++;
    for (int f = 0; f < 4; f++) {
        for (int i = 0; i < 160; i++) {
            g_state.s_l[i] = (short)(1000 + f);
        }
        g_state.mbe_short_silenced[0] = 0U;
        dsd_p25p2_playout_stage_decided(&g_opts, &g_state, 0, 0, g_serial, &decision);
    }
    dsd_p25p2_playout_burst_done(&g_opts, &g_state, 0, 0, DSD_P25P2_BURST_4V);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the stale patch mutes the frames", g_block_count, 0);
    return rc;
}

/* A blocked call's verdict is taken (the watchdog's release takes it before its return tune) just as the call ends: the
   verdict comes from the one look that found the call active, never from the ended call's defaults, so the frames the
   call left queued stay silent. */
static int
test_verdict_taken_as_the_call_ends(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    rc |= expect_int("ending call queued", dsd_p25p2_playout_level(&g_state, 0), 4);
    rc |= expect_int("ending call blocked", dsd_tg_policy_set_mode(&g_state, 100U, 100U, "B"), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    g_end_after_look = 1;
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    rc |= expect_int("the call ended during the look", g_end_after_look, 0);
    dsd_call_snapshot call;
    rc |= expect_int("the call is over",
                     dsd_call_state_get(&g_state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE, 0);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("its queued frames stay silent", g_block_count, 0);
    rc |= expect_int("and are gone", dsd_p25p2_playout_level(&g_state, 0), 0);
    return rc;
}
#endif

/* Two streams that each opened behind a barrier, at different pairs (successive replacements with their tails still
   queued): the earlier barrier has nothing to wait out, and the later one waits while the earlier stream plays, so the
   two play one after the other rather than over each other. */
static int
test_barriers_resolve_in_pair_order(void) {
    int rc = 0;
    setup(0, 2);
    for (int slot = 0; slot < 2; slot++) {
        dsd_p25p2_playout_slot* s = &g_state.p25p2_playout.slot[slot];
        const uint32_t clock = 6U + (uint32_t)slot;
        s->q[0].kind = DSD_P25P2_PLAYOUT_ENTRY_BARRIER;
        s->q[0].clock = clock;
        for (int i = 1; i <= 4; i++) {
            dsd_p25p2_playout_entry* e = &s->q[i];
            e->kind = DSD_P25P2_PLAYOUT_ENTRY_FRAME;
            e->clock = clock;
            e->verdict = k_open_verdict;
            for (int j = 0; j < 160; j++) {
                e->pcm.s16[j] = (int16_t)(1000 * (slot + 1) + i);
            }
        }
        s->head = 0;
        s->count = 5;
    }
    g_state.p25p2_playout.pair_clock = 8U;
    rc |= expect_int("the earlier stream's level", dsd_p25p2_playout_level(&g_state, 0), 4);
    rc |= expect_int("the later stream waits it out", dsd_p25p2_playout_level(&g_state, 1), 8);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("they play one after the other", g_block_count, 8);
    for (int b = 0; b < 4 && b < g_block_count; b++) {
        rc |= expect_int("the earlier stream first", g_blocks[b].left, 1001 + b);
        rc |= expect_int("on its own", g_blocks[b].right, 1001 + b);
    }
    for (int b = 4; b < 8 && b < g_block_count; b++) {
        rc |= expect_int("the later stream after it", g_blocks[b].left, 2001 + (b - 4));
    }
    return rc;
}

/* A break in the air timeline while slot 1's burst waits for a pair slot 2 never completes: what follows starts a later
   pair, so slot 2's next stream plays after that tail, not over it, and both slots then run side by side. */
static int
test_break_starts_a_later_pair(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 1, DSD_P25P2_BURST_4V, 1000);
    dsd_p25p2_playout_break(&g_state);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 3, DSD_P25P2_BURST_4V, 1004);
    burst(1, 3, DSD_P25P2_BURST_4V, 2000);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("the tail, then both slots", g_block_count, 8);
    for (int b = 0; b < 4 && b < g_block_count; b++) {
        rc |= expect_int("the tail first", g_blocks[b].left, 1000 + b);
        rc |= expect_int("on its own", g_blocks[b].right, 1000 + b);
    }
    for (int b = 4; b < 8 && b < g_block_count; b++) {
        rc |= expect_int("slot 1 after the break", g_blocks[b].left, 1004 + (b - 4));
        rc |= expect_int("beside slot 2", g_blocks[b].right, 2000 + (b - 4));
    }
    return rc;
}

/* An alert raised for a slot while its queue still holds the call's last frames (an END with frames still to play)
   sounds after them, not ahead of them; with nothing queued it sounds at once, and a discard sounds the alert it held
   back. The alert's tone goes to its slot's channel only, so the other channel of its blocks is silent. */
static int
test_alert_waits_for_queued_audio(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    rc |= expect_int("the alert waits behind the call's frames", g_block_count, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the frames, then the alert", g_block_count, 4 + 6);
    for (int b = 0; b < 4 && b < g_block_count; b++) {
        rc |= expect_int("the frames first", g_blocks[b].right, 1000 + b);
    }
    for (int b = 4; b < 10 && b < g_block_count; b++) {
        rc |= expect_int("then the alert", g_blocks[b].right, 0);
    }
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    rc |= expect_int("with nothing queued it sounds at once", g_block_count, 16);
    burst(0, 1, DSD_P25P2_BURST_4V, 1004);
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    rc |= expect_int("queued again, it waits", g_block_count, 16);
    dsd_p25p2_playout_discard(&g_opts, &g_state);
    rc |= expect_int("a discard sounds it without the frames", g_block_count, 22);
    for (int b = 16; b < 22 && b < g_block_count; b++) {
        rc |= expect_int("only the alert", g_blocks[b].right, 0);
    }

    // Slot 1's alert follows its own last frame, while slot 2 plays on after it.
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 0, DSD_P25P2_BURST_2V, 1000);
    burst(1, 0, DSD_P25P2_BURST_4V, 2000);
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("both slots, the alert, slot 2 alone", g_block_count, 2 + 6 + 2);
    for (int b = 0; b < 2 && b < g_block_count; b++) {
        rc |= expect_int("slot 1 beside slot 2", g_blocks[b].left, 1000 + b);
    }
    for (int b = 2; b < 8 && b < g_block_count; b++) {
        rc |= expect_int("the alert right after slot 1's last frame", g_blocks[b].right, 0);
    }
    for (int b = 8; b < 10 && b < g_block_count; b++) {
        rc |= expect_int("then slot 2 plays on", g_blocks[b].right, 2002 + (b - 8));
    }
    return rc;
}

/* Raising an alert never sounds one, even one already due (here behind audio a reset discarded without sounding it):
   alerts sound only from the playout's own steps, on the thread that plays the queues, so a tone raised from another
   thread can never cut into a block going out. The next step sounds both, in order. */
static int
test_raising_an_alert_sounds_none(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    dsd_p25p2_playout_reset(&g_state, -1);
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    rc |= expect_int("raising an alert sounds none", g_block_count, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the next step sounds both", g_block_count, 12);

    // Raised while the last queued frame's block is going out: that frame has not gone yet, so the alert waits and
    // sounds right after the block.
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    g_raise_at_block = 4;
    g_raise_taken = -1;
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("an alert raised during the last block waits", g_raise_taken, 1);
    rc |= expect_int("and sounds right after it", g_block_count, 4 + 6);
    return rc;
}

/* An alert raised through beeper() while a deferred alert's tone is going out, its slot's queue now empty: it waits
   for that tone to finish rather than sounding into it, so the two tones play whole, one after the other. */
static int
test_alert_raised_during_a_tone_waits(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    beeper(&g_opts, &g_state, 0, 40, 86, 1);
    g_beep_at_block = 5;
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    g_beep_at_block = -1;
    rc |= expect_int("the frames and both tones", g_block_count, 4 + 2 + 2);
    if (g_block_count >= 8) {
        const int loud = g_blocks[4].peak_left;
        const int quiet = g_blocks[7].peak_left;
        rc |= expect_int("the first tone louder", loud > quiet && quiet > 0, 1);
        rc |= expect_int("the first tone whole", g_blocks[5].peak_left, loud);
        rc |= expect_int("then the second", g_blocks[6].peak_left, quiet);
    }
    return rc;
}

/* A stream that closes after pair 4 of a superframe it covered with its 2V position unknown (4V, lost, 4V, 4V, lost)
   resolves its last placeholder into two fill frames and two removed entries at the queue's tail. Nothing pops those,
   yet the end-of-call alert raised behind them sounds right after the last block, not at the next call. */
static int
test_alert_after_trailing_removed_entries(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    const dsd_p25p2_burst_kind kinds[5] = {
        DSD_P25P2_BURST_4V, DSD_P25P2_BURST_LOST, DSD_P25P2_BURST_4V, DSD_P25P2_BURST_4V, DSD_P25P2_BURST_LOST,
    };
    int tag = 1000;
    for (int pair = 0; pair < 5; pair++) {
        burst(0, pair, kinds[pair], tag);
        tag += 4;
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    }
    dsd_p25p2_playout_close(&g_state, 0);
    beeper(&g_opts, &g_state, 0, 40, 86, 3);
    const int before = g_block_count;
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("the superframe's 18 blocks, then the alert", g_block_count, 18 + 6);
    rc |= expect_int("the alert after the last block", g_block_count - before > 6, 1);
    return rc;
}

/* More alerts than the slot holds back, raised through beeper() while its audio waits: none sounds ahead of the audio
   or of older alerts; those past the limit are dropped. */
static int
test_alert_overflow_keeps_order(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    for (int i = 0; i < DSD_P25P2_PLAYOUT_ALERTS + 2; i++) {
        beeper(&g_opts, &g_state, 0, 40, 86, 1);
    }
    rc |= expect_int("no alert sounds ahead of the audio", g_block_count, 0);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("the frames, then every alert held back", g_block_count, 4 + DSD_P25P2_PLAYOUT_ALERTS * 2);
    for (int b = 0; b < 4 && b < g_block_count; b++) {
        rc |= expect_int("the frames first", g_blocks[b].right, 1000 + b);
    }
    return rc;
}

/* A talkgroup set to play but not record after its frames were queued: they still play, and the static WAV still
   records them, as their verdict at queue time said. The live policy decides only whether a queued frame plays. */
static int
test_live_policy_leaves_recordability(void) {
    static short samples[160 * 2 * 8];
    char path[DSD_TEST_PATH_MAX];
    int rc = 0;
    const int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_p25p2_wav");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL wav temp file: dsd_test_mkstemp returned %d\n", fd);
        return 1;
    }
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    setup(0, 2);
    g_opts.wav_out_f = sf_open(path, SFM_WRITE, &info);
    g_opts.static_wav_file = 1;
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    burst(0, 0, DSD_P25P2_BURST_2V, 1000);
    dsd_tg_policy_entry row;
    rc |= expect_int("policy row",
                     dsd_tg_policy_make_exact_entry(100U, "A", "play only", DSD_TG_POLICY_SOURCE_IMPORTED, &row), 0);
    row.record = 0U;
    rc |=
        expect_int("policy row set", dsd_tg_policy_upsert_exact(&g_state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    rc |=
        expect_int("the live verdict says play, not record", g_state.p25p2_playout.slot[0].live_verdict.recordable, 0);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("the queued frames play", g_block_count, 2);
    sf_close(g_opts.wav_out_f);
    g_opts.wav_out_f = NULL;
    int frames = 0;
    rc |= read_wav(path, samples, (int)(sizeof(samples) / sizeof(samples[0])), &frames);
    rc |= expect_int("and are recorded", frames, 320);
    (void)remove(path);
    return rc;
}

enum { RAISED_ALERTS = 400 };

static DSD_THREAD_RETURN_TYPE
raise_alerts(void* arg) {
    (void)arg;
    for (int i = 0; i < RAISED_ALERTS; i++) {
        if (!dsd_p25p2_playout_defer_alert(&g_opts, &g_state, i & 1, 40, 86, 1)) {
            dsd_thread_yield();
        }
    }
    DSD_THREAD_RETURN;
}

/* Alerts raised on another thread (a held VOICE_END sounds from the frame-sync pass, outside the tick guard) while the
   decoder queues and plays: every one taken sounds once its audio has gone, and the two sides share nothing but the
   playout's alert lock (run under TSan). */
static int
test_alerts_raised_on_another_thread(void) {
    int rc = 0;
    setup(0, 2);
    g_opts.audio_out = 0;
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    dsd_thread_t raiser;
    rc |= expect_int("raiser started", dsd_thread_create(&raiser, raise_alerts, NULL), 0);
    for (int sf = 0; sf < 40; sf++) {
        for (int pair = 0; pair < 5; pair++) {
            burst(0, pair, (pair == 4) ? DSD_P25P2_BURST_2V : DSD_P25P2_BURST_4V, 1000);
            burst(1, pair, (pair == 4) ? DSD_P25P2_BURST_2V : DSD_P25P2_BURST_4V, 2000);
            dsd_p25p2_playout_pair_done(&g_opts, &g_state);
        }
        burst(0, 5, DSD_P25P2_BURST_SACCH, 0);
        burst(1, 5, DSD_P25P2_BURST_SACCH, 0);
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    }
    dsd_thread_join(raiser);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("slot 1's alerts all sounded", g_state.p25p2_playout.alert_count[0], 0);
    rc |= expect_int("slot 2's alerts all sounded", g_state.p25p2_playout.alert_count[1], 0);
    return rc;
}

/* A call blocked while some of its frames wait (the companion's 2V held them as carry), then replaced on the slot by
   another call before they drain: they keep the verdict last taken for their call and stay silent. */
static int
test_replaced_call_keeps_its_verdict(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    seed_call(&g_state, 1U, 200U, 2000U);
    burst(0, 0, DSD_P25P2_BURST_4V, 1000);
    burst(1, 0, DSD_P25P2_BURST_2V, 2000);
    rc |= expect_int("replaced call blocked", dsd_tg_policy_set_mode(&g_state, 100U, 100U, "B"), 0);
    dsd_p25p2_playout_note_policy(&g_opts, &g_state);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    rc |= expect_int("replaced call carry", dsd_p25p2_playout_level(&g_state, 0), 2);
    (void)dsd_call_state_end(&g_state, 0U, 0.0);
    seed_call(&g_state, 0U, 300U, 3000U);
    burst(0, 1, DSD_P25P2_BURST_4V, 3000);
    burst(1, 1, DSD_P25P2_BURST_4V, 2002);
    dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    int heard = 0;
    for (int b = 0; b < g_block_count; b++) {
        heard |= (g_blocks[b].left >= 1000 && g_blocks[b].left < 1004)
                 || (g_blocks[b].right >= 1000 && g_blocks[b].right < 1004);
    }
    rc |= expect_int("replaced call's blocked frames stay silent", heard, 0);
    rc |= expect_int("replaced call output", g_block_count > 0, 1);
    if (g_block_count < 1) {
        return rc;
    }
    rc |= expect_int("the new call plays", g_blocks[g_block_count - 1].left, 3003);
    return rc;
}

/* A stream closed after all five voice pairs of a superframe it covered from pair 0, before that superframe's SACCH
   (an END in the companion's SACCH), with the 2V position still unknown (4V at pairs 0, 1 and 3, pairs 2 and 4 lost):
   the superframe still totals 18 frames. */
static int
test_close_after_covered_superframe_keeps_18(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    const dsd_p25p2_burst_kind kinds[5] = {
        DSD_P25P2_BURST_4V, DSD_P25P2_BURST_4V, DSD_P25P2_BURST_LOST, DSD_P25P2_BURST_4V, DSD_P25P2_BURST_LOST,
    };
    int tag = 1000;
    for (int pair = 0; pair < 5; pair++) {
        burst(0, pair, kinds[pair], tag);
        tag += 4;
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    }
    dsd_p25p2_playout_close(&g_state, 0);
    dsd_p25p2_playout_drain(&g_opts, &g_state);
    rc |= expect_int("covered superframe closed before its SACCH", g_block_count, 18);
    return rc;
}

/* A voice position whose DUID reads as SACCH lost its voice burst: it plays as silence in place, and the superframe
   still totals 18. */
static int
test_sacch_at_voice_position_fills(void) {
    int rc = 0;
    setup(0, 2);
    seed_call(&g_state, 0U, 100U, 1000U);
    g_state.p25_p2_audio_allowed[1] = 0;
    int tag = 1000;
    superframe(4, 4, &tag, NULL, -1);
    for (int pair = 0; pair < 6; pair++) {
        const dsd_p25p2_burst_kind k = (pair == 2) ? DSD_P25P2_BURST_SACCH : kind_for(pair, 4);
        burst(0, pair, k, tag);
        tag += frames_of(kind_for(pair, 4));
        dsd_p25p2_playout_burst_done(&g_opts, &g_state, 1, pair, DSD_P25P2_BURST_OTHER);
        dsd_p25p2_playout_pair_done(&g_opts, &g_state);
    }
    rc |= expect_int("sacch at a voice position fills", g_block_count, 36);
    for (int b = 26; b < 30 && b < g_block_count; b++) {
        rc |= expect_int("sacch at a voice position plays silence in place", g_blocks[b].left, 0);
    }
    return rc;
}

int
main(void) {
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_blast});
    int rc = 0;
    rc |= test_aligned_two_slots();
    rc |= test_rotated_slots();
    rc |= test_single_call_mirrored();
    rc |= test_lost_burst_fills_in_place();
    rc |= test_early_2v_lost_without_warmup();
    rc |= test_repeated_unknown_phase_loss();
    rc |= test_provisional_correction_repays_debt();
    rc |= test_mono_output();
    rc |= test_close_keeps_tail();
    rc |= test_verdicts();
    rc |= test_float_output();
    rc |= test_other_rate_discards();
    rc |= test_buffer_wrap();
    rc |= test_activity_stamp();
    rc |= test_epoch_change();
    rc |= test_decoded_silence_plays();
    rc |= test_switched_off_slot_beside_muted_companion();
    rc |= test_float_mono_output();
    rc |= test_live_policy_mutes_queued_frames();
    rc |= test_static_wav_records_what_plays();
    rc |= test_noted_policy_mutes_ended_call_tail();
    rc |= test_random_losses_play_every_frame();
    rc |= test_late_admission_keeps_its_air_time();
    rc |= test_replaced_call_keeps_its_verdict();
    rc |= test_later_burst_refreshes_the_verdict();
    rc |= test_patch_change_retakes_the_verdict();
    rc |= test_source_learned_retakes_the_verdict();
    rc |= test_gate_decision_keeps_its_key();
    rc |= test_full_queue_reclaims_removed_entries();
    rc |= test_policy_write_retakes_the_verdict();
    rc |= test_full_live_queue_keeps_the_frame();
    rc |= test_alert_fence_survives_reclaim();
    rc |= test_placeholder_is_whole_or_absent();
    rc |= test_barriers_resolve_in_pair_order();
    rc |= test_break_starts_a_later_pair();
    rc |= test_alert_waits_for_queued_audio();
    rc |= test_alerts_raised_on_another_thread();
    rc |= test_raising_an_alert_sounds_none();
    rc |= test_alert_raised_during_a_tone_waits();
    rc |= test_alert_after_trailing_removed_entries();
    rc |= test_alert_overflow_keeps_order();
    rc |= test_live_policy_leaves_recordability();
#if defined(DSD_TEST_WRAP_CALL_STATE)
    rc |= test_verdict_taken_as_the_call_ends();
    rc |= test_gate_key_matches_its_decision();
#endif
    rc |= test_close_after_covered_superframe_keeps_18();
    rc |= test_sacch_at_voice_position_fills();
    dsd_state_ext_free_all(&g_state);
    if (rc != 0) {
        return 1;
    }
    printf("CORE_P25P2_PLAYOUT: OK\n");
    return 0;
}
