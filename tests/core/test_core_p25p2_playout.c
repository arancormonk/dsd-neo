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

#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

enum { MAX_BLOCKS = 512 };

typedef struct {
    int channels; /* 1 or 2 */
    int left;     /* first left (or mono) sample: the tag */
    int right;
    int is_float;
    float fleft;
    float fright;
} captured_block;

static captured_block g_blocks[MAX_BLOCKS];
static int g_block_count = 0;
static int g_capture_float = 0;

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
    }
    (void)opts;
}

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

static dsd_opts g_opts;
static dsd_state g_state;
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

/* The vocoder's output buffer pointers wrap at every pair, in either format: SS18/FS4 used to do it. */
static int
test_buffer_wrap(void) {
    int rc = 0;
    for (int floating = 0; floating < 2; floating++) {
        setup(floating, 2);
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
    dsd_state_ext_free_all(&g_state);
    if (rc != 0) {
        return 1;
    }
    printf("CORE_P25P2_PLAYOUT: OK\n");
    return 0;
}
