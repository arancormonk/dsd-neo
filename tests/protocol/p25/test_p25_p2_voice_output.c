// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 voice output end to end (issue #651): timeslots run through the frame decoder's DUID dispatch with the
 * real voice, MAC, trunking and audio code, and every block the output receives is checked.
 *
 * A stand-in vocoder tags each frame it synthesizes with a per-slot sequence number: every short sample holds the tag,
 * and the float frame carries it in the signs of its first 16 samples (the float gain scales and clips, but keeps
 * signs). Each case then checks that both slots play every frame once, in order and side by side, at the air rate:
 * 18 frames per slot per superframe, played as each timeslot pair completes. Only interfaces the decoder had before
 * the playout are used, so the cases also run against the earlier mixers.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/p25/p25_crc.h>
#include <dsd-neo/protocol/p25/p25_trunk_sm.h>
#include <dsd-neo/protocol/p25/p25_xcch.h>
#include <dsd-neo/protocol/p25/p25p2_frame.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/p25_optional_hooks.h>
#include <dsd-neo/runtime/trunk_tuning_hooks.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <sndfile.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"
#include "p25p2_frame_internal.h"
#include "test_support.h"

extern int p2bit[4320];
extern int16_t p2llr[1400];

enum {
    MAX_BLOCKS = 256,
    TAG_NONE = -1,   /* a block channel that carried silence */
    TAG_BITS = 16,   /* float frames carry the tag in the signs of their first 16 samples */
    FIRST_TAG0 = 1,  /* slot 1's tags start here ... */
    FIRST_TAG1 = 501 /* ... and slot 2's here */
};

/* DUID codewords (p25p2_frame.c duid_canonical). */
enum {
    DUID_4V = 0x00,
    DUID_SACCH = 0x39, /* scrambled SACCH */
    DUID_2V = 0x65,
    DUID_FACCH = 0x9A, /* scrambled FACCH */
    DUID_ERR = 0x03,   /* three bits from every codeword: undecodable */
};

typedef struct {
    int channels;
    int left;
    int right;
} block;

static block g_blocks[MAX_BLOCKS];
static int g_block_count = 0;
static int g_floating = 0;
static int g_next_tag[2] = {FIRST_TAG0, FIRST_TAG1};
static int g_frames_in_burst[2] = {0, 0};

/* Run once, when the stand-in vocoder synthesizes frame @p g_hook_frame (counted from 0 per slot within the window
   being dispatched) of slot @p g_hook_slot: a test's way to have a MAC message arrive between two timeslots of a
   window. */
static int g_hook_slot = -1;
static int g_hook_frame = -1;
static void (*g_hook)(dsd_opts* opts, dsd_state* state) = NULL;

static int
expect_int(const char* tag, long got, long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL %s: got %ld want %ld\n", tag, got, want);
        return 1;
    }
    return 0;
}

void
processMbeFrame(dsd_opts* opts, dsd_state* state, char imbe_fr[8][23], char ambe_fr[4][24], char imbe7100_fr[7][24]) {
    (void)opts;
    (void)state;
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
}

void
processMbeFrameSoft(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit imbe_fr[8][23],
                    dsd_vocoder_soft_bit ambe_fr[4][24], dsd_vocoder_soft_bit imbe7100_fr[7][24]) {
    (void)imbe_fr;
    (void)ambe_fr;
    (void)imbe7100_fr;
    const int slot = state->currentslot & 1;
    const int tag = g_next_tag[slot]++;
    short* s = (slot == 0) ? state->s_l : state->s_r;
    float* f = (slot == 0) ? state->audio_out_temp_buf : state->audio_out_temp_bufR;
    for (int i = 0; i < 160; i++) {
        s[i] = (short)tag;
        f[i] = (i < TAG_BITS) ? ((((unsigned)tag >> i) & 1U) ? 0.25f : -0.25f) : 0.1f;
    }
    const int frame = g_frames_in_burst[slot]++;
    if (g_hook && slot == g_hook_slot && frame == g_hook_frame) {
        void (*hook)(dsd_opts*, dsd_state*) = g_hook;
        g_hook = NULL;
        hook(opts, state);
    }
}

/* Alias and location decoders the MAC messages reach; these cases carry none. */
// NOLINTNEXTLINE(misc-use-internal-linkage)
void apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input);
// NOLINTNEXTLINE(misc-use-internal-linkage)
void nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot);

void
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

void
dsd_mbe_log_ambe_soft_frame(dsd_opts* opts, dsd_state* state, dsd_vocoder_soft_bit ambe_fr[4][24]) {
    (void)opts;
    (void)state;
    (void)ambe_fr;
}

static int
decode_short(const short* s) {
    return (s[0] == 0) ? TAG_NONE : s[0];
}

static int
decode_float(const float* f, int stride) {
    if (f[0] > -1.0e-9f && f[0] < 1.0e-9f) {
        return TAG_NONE;
    }
    int tag = 0;
    for (int i = 0; i < TAG_BITS; i++) {
        if (f[(size_t)i * (size_t)stride] > 0.0f) {
            tag |= 1 << i;
        }
    }
    return tag;
}

static void
capture_blast(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    if (!data || g_block_count >= MAX_BLOCKS) {
        return;
    }
    block* b = &g_blocks[g_block_count++];
    const size_t sample = g_floating ? sizeof(float) : sizeof(short);
    b->channels = (int)(bytes / (160U * sample));
    if (g_floating) {
        const float* f = (const float*)data;
        b->left = decode_float(f, b->channels == 2 ? 2 : 1);
        b->right = (b->channels == 2) ? decode_float(f + 1, 2) : b->left;
    } else {
        const short* s = (const short*)data;
        b->left = decode_short(s);
        b->right = (b->channels == 2) ? ((s[1] == 0) ? TAG_NONE : s[1]) : b->left;
    }
}

static dsd_opts g_opts;
static dsd_state g_state;

static void
seed_call(uint8_t slot, uint64_t target, uint64_t source) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = source,
    };
    (void)dsd_call_state_observe(&g_state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

/* A clear call on @p slot whose voice the decoder plays. */
static void
open_slot(int slot, uint64_t target, uint64_t source) {
    seed_call((uint8_t)slot, target, source);
    g_state.p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
    g_state.p25_p2_audio_allowed[slot] = 1;
    if (slot == 0) {
        g_state.payload_algid = 0x80;
    } else {
        g_state.payload_algidR = 0x80;
    }
}

static void
setup(int floating, int channels) {
    dsd_state_ext_free_all(&g_state);
    DSD_MEMSET(&g_opts, 0, sizeof(g_opts));
    DSD_MEMSET(&g_state, 0, sizeof(g_state));
    p25_p2_frame_reset();
    g_opts.audio_out = 1;
    g_opts.audio_out_type = 8;
    g_opts.floating_point = floating;
    g_opts.pulse_digi_rate_out = 8000;
    g_opts.pulse_digi_out_channels = channels;
    g_opts.slot1_on = 1;
    g_opts.slot2_on = 1;
    g_opts.audio_gain = 0;
    g_state.aout_gain = 25.0f;
    g_state.aout_gainR = 25.0f;
    g_state.p2_wacn = 0xBEE00;
    g_state.p2_sysid = 0x1A2;
    g_state.p2_cc = 0x293;
    g_state.synctype = DSD_SYNC_P25P2_POS;
    g_state.lastsynctype = DSD_SYNC_P25P2_POS;
    init_audio_filters(&g_state, 48000);
    g_block_count = 0;
    g_floating = floating;
    g_next_tag[0] = FIRST_TAG0;
    g_next_tag[1] = FIRST_TAG1;
    g_hook = NULL;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_blast});
}

static void
seed_duid(int timeslot_index, uint8_t duid) {
    static const int duid_offsets[8] = {0, 1, 74, 75, 244, 245, 318, 319};
    const int base = timeslot_index * 360;
    for (int i = 0; i < 8; i++) {
        const int abs_bit = base + duid_offsets[i];
        p2bit[abs_bit] = (duid >> (7 - i)) & 1U;
        p2llr[abs_bit] = (int16_t)(((duid >> (7 - i)) & 1U) ? 200 : -200);
    }
}

/* The DUID codeword of slot @p slot's burst at pair @p pair for a transmission whose 2V sits at @p two_v (-1: the slot
   is idle and carries SACCH). */
static uint8_t
voice_duid(int pair, int two_v) {
    if (pair == 5) {
        return DUID_SACCH;
    }
    if (two_v < 0) {
        return DUID_SACCH;
    }
    return (uint8_t)((pair == two_v) ? DUID_2V : DUID_4V);
}

/* Bursts for superframe timeslots, per slot: kind[slot][pair] codewords. */
typedef struct {
    uint8_t duid[2][6];
} superframe_plan;

static void
plan_voice(superframe_plan* p, int two_v0, int two_v1) {
    for (int pair = 0; pair < 6; pair++) {
        p->duid[0][pair] = voice_duid(pair, two_v0);
        p->duid[1][pair] = voice_duid(pair, two_v1);
    }
}

/* Superframe timeslots @p first .. @p first + 4 * windows - 1 of @p plan, one window at a time, as processP2() hands
   them to the DUID dispatch once it has located the window in the superframe. */
static void
run_windows(const superframe_plan* plan, int first, int windows) {
    for (int w = 0; w < windows; w++) {
        const int start = (first + 4 * w) % 12;
        uint8_t duids[4];
        for (int i = 0; i < 4; i++) {
            const int ts = (start + i) % 12;
            duids[i] = plan->duid[ts & 1][ts / 2];
        }
        g_state.p2_scramble_offset = start;
        g_state.currentslot = start & 1;
        for (int i = 0; i < 4; i++) {
            seed_duid(i, duids[i]);
        }
        g_frames_in_burst[0] = 0;
        g_frames_in_burst[1] = 0;
        p25p2_process_duid(&g_opts, &g_state);
    }
}

/* Whole superframes from timeslot 0. */
static void
run_superframes(const superframe_plan* plan, int count) {
    run_windows(plan, 0, 3 * count);
}

static int
expect_sequence(const char* what, int first_block, int count, int channel, int first_tag) {
    int rc = 0;
    for (int i = 0; i < count; i++) {
        const int b = first_block + i;
        if (b >= g_block_count) {
            return expect_int(what, g_block_count, first_block + count);
        }
        const int got = (channel == 0) ? g_blocks[b].left : g_blocks[b].right;
        if (got != first_tag + i) {
            DSD_FPRINTF(stderr, "FAIL %s: block %d %s got %d want %d\n", what, b, channel == 0 ? "left" : "right", got,
                        first_tag + i);
            return 1;
        }
    }
    return rc;
}

static const char*
fmt_name(int floating) {
    return floating ? "float" : "short";
}

/* Two calls whose 2V sits at the same pair: 18 blocks per superframe, each channel its own slot's frames in order,
   played as each pair completes (4, 4, 4, 4, 2), not held for a whole superframe and not 5 of 18 (issue #651 item
   1). */
static int
test_aligned_two_slots(int floating) {
    int rc = 0;
    char tag[96];
    setup(floating, 2);
    open_slot(0, 100, 1000);
    open_slot(1, 200, 2000);
    superframe_plan plan;
    plan_voice(&plan, 4, 4);
    static const int want_after_window[3] = {8, 16, 18};
    for (int w = 0; w < 3; w++) {
        run_windows(&plan, 4 * w, 1);
        DSD_SNPRINTF(tag, sizeof(tag), "%s aligned blocks after window %d", fmt_name(floating), w);
        rc |= expect_int(tag, g_block_count, want_after_window[w]);
    }
    run_superframes(&plan, 1);
    DSD_SNPRINTF(tag, sizeof(tag), "%s aligned blocks after two superframes", fmt_name(floating));
    rc |= expect_int(tag, g_block_count, 36);
    DSD_SNPRINTF(tag, sizeof(tag), "%s aligned slot 1 in order", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 36, 0, FIRST_TAG0);
    DSD_SNPRINTF(tag, sizeof(tag), "%s aligned slot 2 in order", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 36, 1, FIRST_TAG1);
    return rc;
}

/* Slot 1's 2V at pair 4 and slot 2's at pair 0: still 18 blocks per superframe, both channels in order, and each pair
   plays what both slots have (2, 4, 4, 4, 4): neither slot waits for the other's superframe (issue #651 item 2). */
static int
test_rotated_two_slots(int floating) {
    int rc = 0;
    char tag[96];
    setup(floating, 2);
    open_slot(0, 100, 1000);
    open_slot(1, 200, 2000);
    superframe_plan plan;
    plan_voice(&plan, 4, 0);
    static const int want_after_window[3] = {6, 14, 18};
    for (int sf = 0; sf < 2; sf++) {
        for (int w = 0; w < 3; w++) {
            run_windows(&plan, 4 * w, 1);
            DSD_SNPRINTF(tag, sizeof(tag), "%s rotated blocks after superframe %d window %d", fmt_name(floating), sf,
                         w);
            rc |= expect_int(tag, g_block_count, 18 * sf + want_after_window[w]);
        }
    }
    DSD_SNPRINTF(tag, sizeof(tag), "%s rotated slot 1 in order", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 36, 0, FIRST_TAG0);
    DSD_SNPRINTF(tag, sizeof(tag), "%s rotated slot 2 in order", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 36, 1, FIRST_TAG1);
    return rc;
}

/* A lone call plays in both ears, 18 blocks per superframe. */
static int
test_single_call(int floating) {
    int rc = 0;
    char tag[96];
    setup(floating, 2);
    open_slot(1, 200, 2000);
    superframe_plan plan;
    plan_voice(&plan, -1, 2);
    run_superframes(&plan, 2);
    DSD_SNPRINTF(tag, sizeof(tag), "%s single call blocks", fmt_name(floating));
    rc |= expect_int(tag, g_block_count, 36);
    DSD_SNPRINTF(tag, sizeof(tag), "%s single call left", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 36, 0, FIRST_TAG1);
    DSD_SNPRINTF(tag, sizeof(tag), "%s single call right", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 36, 1, FIRST_TAG1);
    return rc;
}

/* A MAC_END_PTT FACCH payload for @p slot's call, with a valid CRC-12. */
static void
build_mac_end(int payload[156], uint32_t source, uint16_t target) {
    unsigned char octets[19] = {0};
    octets[0] = 0x40; /* opcode 2 (MAC_END_PTT), offset 0, reserved 0 */
    octets[13] = (unsigned char)(source >> 16);
    octets[14] = (unsigned char)(source >> 8);
    octets[15] = (unsigned char)source;
    octets[16] = (unsigned char)(target >> 8);
    octets[17] = (unsigned char)target;
    DSD_MEMSET(payload, 0, 156 * sizeof(int));
    for (int i = 0; i < 144; i++) {
        payload[i] = (octets[i / 8] >> (7 - (i % 8))) & 1;
    }
    for (int crc = 0; crc < 4096; crc++) {
        for (int b = 0; b < 12; b++) {
            payload[144 + b] = (crc >> (11 - b)) & 1;
        }
        if (crc12_xb_bridge(payload, 144) == 0) {
            return;
        }
    }
}

static void
end_slot2_call(dsd_opts* opts, dsd_state* state) {
    int payload[156];
    build_mac_end(payload, 2000U, 200U);
    const int slot = state->currentslot;
    state->currentslot = 1;
    process_FACCH_MAC_PDU(opts, state, payload);
    state->currentslot = slot;
}

/* Slot 2's call ends mid-superframe (its END FACCH takes its burst at pair 2) while slot 1's goes on: what both slots
   shared plays once, side by side, and slot 1 then plays alone in both ears; nothing is replayed (issue #651 item
   2). */
static int
test_slot_end_mid_superframe(int floating) {
    int rc = 0;
    char tag[96];
    setup(floating, 2);
    open_slot(0, 100, 1000);
    open_slot(1, 200, 2000);
    superframe_plan plan;
    plan_voice(&plan, 4, 0);
    plan.duid[1][2] = DUID_FACCH;
    plan.duid[1][3] = DUID_SACCH;
    plan.duid[1][4] = DUID_SACCH;
    run_windows(&plan, 0, 1);
    /* The next window holds pairs 2 and 3. The END arrives once slot 1's pair-2 burst is decoded, before slot 2's
       timeslot of the pair, which its FACCH takes. */
    g_hook = end_slot2_call;
    g_hook_slot = 0;
    g_hook_frame = 3;
    run_windows(&plan, 4, 2);
    rc |= expect_int("slot end hook ran", g_hook == NULL, 1);
    DSD_SNPRINTF(tag, sizeof(tag), "%s slot end blocks", fmt_name(floating));
    rc |= expect_int(tag, g_block_count, 18);
    DSD_SNPRINTF(tag, sizeof(tag), "%s slot end slot 1 in order", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 18, 0, FIRST_TAG0);
    DSD_SNPRINTF(tag, sizeof(tag), "%s slot end slot 2 before its end", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 6, 1, FIRST_TAG1);
    DSD_SNPRINTF(tag, sizeof(tag), "%s slot end slot 1 alone after", fmt_name(floating));
    rc |= expect_sequence(tag, 6, 12, 1, FIRST_TAG0 + 6);
    return rc;
}

/* Slot 2's call starts at pair 2 of a superframe slot 1's call already plays in: slot 2's frames play at once beside
   slot 1's, and no silence is inserted later in its stream (issue #651 item 4). */
static int
test_companion_starts_mid_superframe(int floating) {
    int rc = 0;
    char tag[96];
    setup(floating, 2);
    open_slot(0, 100, 1000);
    superframe_plan plan;
    plan_voice(&plan, 4, -1);
    run_windows(&plan, 0, 1); /* pairs 0 and 1: slot 1 alone */
    open_slot(1, 200, 2000);
    plan.duid[1][2] = DUID_4V;
    plan.duid[1][3] = DUID_4V;
    plan.duid[1][4] = DUID_4V;
    run_windows(&plan, 4, 2);
    superframe_plan next;
    plan_voice(&next, 4, 1);
    run_superframes(&next, 2);
    DSD_SNPRINTF(tag, sizeof(tag), "%s late start blocks", fmt_name(floating));
    rc |= expect_int(tag, g_block_count, 54);
    DSD_SNPRINTF(tag, sizeof(tag), "%s late start slot 1 in order", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 54, 0, FIRST_TAG0);
    DSD_SNPRINTF(tag, sizeof(tag), "%s late start slot 1 alone first", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 8, 1, FIRST_TAG0);
    /* From pair 2 slot 2 runs without a gap: 46 blocks, every one its own next frame. */
    DSD_SNPRINTF(tag, sizeof(tag), "%s late start slot 2 without later silence", fmt_name(floating));
    rc |= expect_sequence(tag, 8, 46, 1, FIRST_TAG1);
    return rc;
}

/* An undecodable DUID in place of slot 1's 2V: the burst is lost and plays as two silent blocks in its place; the next
   superframe's frames follow in order and overwrite nothing (issue #651 item 5). */
static int
test_invalid_2v_duid(int floating) {
    int rc = 0;
    char tag[96];
    setup(floating, 2);
    open_slot(0, 100, 1000);
    superframe_plan plan;
    plan_voice(&plan, 4, -1);
    plan.duid[0][4] = DUID_ERR;
    run_superframes(&plan, 1);
    plan.duid[0][4] = DUID_2V;
    run_superframes(&plan, 1);
    DSD_SNPRINTF(tag, sizeof(tag), "%s invalid 2v blocks", fmt_name(floating));
    rc |= expect_int(tag, g_block_count, 36);
    DSD_SNPRINTF(tag, sizeof(tag), "%s invalid 2v frames before", fmt_name(floating));
    rc |= expect_sequence(tag, 0, 16, 0, FIRST_TAG0);
    for (int b = 16; b < 18 && b < g_block_count; b++) {
        DSD_SNPRINTF(tag, sizeof(tag), "%s invalid 2v silence in place %d", fmt_name(floating), b);
        rc |= expect_int(tag, g_blocks[b].left, TAG_NONE);
    }
    DSD_SNPRINTF(tag, sizeof(tag), "%s invalid 2v next superframe intact", fmt_name(floating));
    rc |= expect_sequence(tag, 18, 18, 0, FIRST_TAG0 + 16);
    return rc;
}

/* A one-channel output gets 160-sample blocks: one call at its level, two calls averaged (issue #651 item 6). */
static int
test_mono_output(void) {
    int rc = 0;
    setup(0, 1);
    open_slot(0, 100, 1000);
    open_slot(1, 200, 2000);
    superframe_plan plan;
    plan_voice(&plan, 4, 4);
    run_superframes(&plan, 1);
    rc |= expect_int("mono blocks", g_block_count, 18);
    for (int b = 0; b < g_block_count; b++) {
        rc |= expect_int("mono block size", g_blocks[b].channels, 1);
        rc |= expect_int("mono two calls averaged", g_blocks[b].left, (FIRST_TAG0 + b + FIRST_TAG1 + b) / 2);
    }
    return rc;
}

static int
read_wav(const char* path, short* out, int max_frames, int* frames) {
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* f = sf_open(path, SFM_READ, &info);
    if (!f) {
        DSD_FPRINTF(stderr, "FAIL wav open: %s\n", sf_strerror(NULL));
        return 1;
    }
    *frames = (int)sf_readf_short(f, out, (sf_count_t)max_frames);
    sf_close(f);
    return 0;
}

static short g_played[24 * 320];
static int g_played_count = 0;

static void
capture_played(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    capture_blast(opts, state, bytes, data);
    const size_t n = bytes / sizeof(short);
    for (size_t i = 0; data && i < n && g_played_count < (int)(sizeof(g_played) / sizeof(g_played[0])); i++) {
        g_played[g_played_count++] = ((const short*)data)[i];
    }
}

/* A short call's tail flushed at release, with the digital high-pass on and a static WAV open: the output and the WAV
   get exactly the six frames decoded, the same samples, and no high-pass decay block (issue #651 item 3). */
static int
test_partial_flush_with_hpf_and_static_wav(void) {
    static short wav[24 * 320];
    char path[DSD_TEST_PATH_MAX];
    int rc = 0;
    const int fd = dsd_test_mkstemp(path, sizeof(path), "dsd_p25p2_out");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "FAIL wav temp file: dsd_test_mkstemp returned %d\n", fd);
        return 1;
    }
    (void)dsd_close(fd);
    setup(0, 2);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_played});
    g_played_count = 0;
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    g_opts.wav_out_f = sf_open(path, SFM_WRITE, &info);
    g_opts.static_wav_file = 1;
    g_opts.use_hpf_d = 1;
    open_slot(0, 100, 1000);
    superframe_plan plan;
    plan_voice(&plan, 0, -1); /* 2V at pair 0, a 4V at pair 1: six frames */
    /* Timeslots 11, 0, 1 and 2: the window ends with slot 1's pair-1 burst decoded and its pair still open. */
    run_windows(&plan, 11, 1);
    dsd_p25p2_flush_partial_audio(&g_opts, &g_state);
    sf_close(g_opts.wav_out_f);
    g_opts.wav_out_f = NULL;
    rc |= expect_int("flush played blocks", g_block_count, 6);
    int frames = 0;
    rc |= read_wav(path, wav, 24 * 160, &frames);
    rc |= expect_int("flush wav frames", frames, 6L * 160L);
    int same = (frames * 2 == g_played_count);
    for (int i = 0; same && i < frames * 2; i++) {
        same = wav[i] == g_played[i];
    }
    rc |= expect_int("flush wav records what played", same, 1);
    (void)remove(path);
    return rc;
}

static dsd_trunk_tune_result g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
static int g_return_calls = 0;

static dsd_trunk_tune_result
tune_ok(dsd_opts* opts, dsd_state* state, long int freq, int ted_sps, uint64_t request_id) {
    (void)ted_sps;
    (void)request_id;
    opts->trunk_is_tuned = 1;
    state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = freq;
    return DSD_TRUNK_TUNE_RESULT_OK;
}

static dsd_trunk_tune_result
return_to_cc(dsd_opts* opts, dsd_state* state, uint64_t request_id) {
    (void)request_id;
    g_return_calls++;
    if (dsd_trunk_tune_result_is_ok(g_return_result)) {
        opts->trunk_is_tuned = 0;
        state->trunk_vc_freq[0] = state->trunk_vc_freq[1] = 0;
    }
    return g_return_result;
}

/* A trunked TDMA voice channel the state machine follows, for slot 1's call. */
static void
follow_tdma_channel(void) {
    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.frame_p25p2 = 1;
    g_state.p25_cc_freq = 851000000;
    g_state.trunk_cc_freq = 851000000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    g_state.trunk_chan_map[0x1234] = 852000000;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune_ok, .tune_to_cc_request = tune_ok, .return_to_cc_request = return_to_cc});
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){.p25p2_flush_partial_audio = dsd_p25p2_flush_partial_audio});
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1234, 852000000, 100, 1000, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, 100, 0, 1000, 1, 0);
}

/* The state machine releases the voice channel between two windows, and the window boundary falls inside a timeslot
   pair: slot 1's burst of the pair is decoded, slot 2's timeslot is the next window's first. A return to the control
   channel the tuner refuses keeps the channel and must not play the half pair early: it plays when the pair completes
   in the next window. An accepted return plays what is queued (issue #651). */
static int
test_release_between_windows(void) {
    int rc = 0;
    setup(0, 2);
    open_slot(0, 100, 1000);
    follow_tdma_channel();
    superframe_plan plan;
    plan_voice(&plan, 4, -1);
    /* Timeslots 1-4: pair 1 completes, and slot 1's half of pair 2 is decoded. */
    run_windows(&plan, 1, 1);
    const int before = g_block_count;
    rc |= expect_int("release fixture plays the completed pair", before, 4);

    g_return_result = DSD_TRUNK_TUNE_RESULT_FAILED;
    g_return_calls = 0;
    p25_sm_release(p25_sm_get_ctx(), &g_opts, &g_state, "test-refused");
    rc |= expect_int("refused release asked the tuner", g_return_calls, 1);
    rc |= expect_int("refused release plays nothing", g_block_count, before);

    /* Timeslots 5-8: slot 2's timeslot completes pair 2, then pair 3 and slot 1's half of pair 4. */
    run_windows(&plan, 5, 1);
    rc |= expect_int("kept channel plays pairs 2 and 3", g_block_count, before + 8);
    rc |= expect_sequence("kept channel plays in order", before, 8, 0, FIRST_TAG0 + 4);

    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_release(p25_sm_get_ctx(), &g_opts, &g_state, "test-accepted");
    rc |= expect_int("accepted release left the channel", g_return_calls, 2);
    rc |= expect_int("accepted release plays the queued half pair", g_block_count, before + 10);
    rc |= expect_sequence("accepted release tail", before + 8, 2, 0, FIRST_TAG0 + 12);

    p25_sm_init_ctx(p25_sm_get_ctx(), &g_opts, &g_state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    return rc;
}

static void
end_call(int slot, uint32_t source, uint16_t target) {
    int payload[156];
    build_mac_end(payload, source, target);
    const int current = g_state.currentslot;
    g_state.currentslot = slot;
    process_FACCH_MAC_PDU(&g_opts, &g_state, payload);
    g_state.currentslot = current;
}

/* Issue #651 item 7: a slot's voice leaves nothing behind that reads as activity once the receiver leaves its carrier.
   Short output, the receiver follows slot 2's call on one voice channel and leaves it mid-call; on the next channel it
   follows slot 1's call, whose MAC_END_PTT then repeats. With slot 2 idle there, the repeat releases the channel at
   once: nothing of slot 2's voice on the carrier left may hold it. */
static int
test_departed_slot_leaves_no_activity(void) {
    int rc = 0;
    setup(0, 2);
    g_opts.trunk_enable = 1;
    g_opts.trunk_tune_group_calls = 1;
    g_opts.frame_p25p2 = 1;
    g_state.p25_cc_freq = 851000000;
    g_state.trunk_cc_freq = 851000000;
    g_state.p25_chan_tdma_explicit[1] = 2;
    g_state.p25_chan_tdma_explicit[2] = 2;
    g_state.trunk_chan_map[0x1235] = 852000000;
    g_state.trunk_chan_map[0x2234] = 853000000;
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){
        .tune_to_freq_request = tune_ok, .tune_to_cc_request = tune_ok, .return_to_cc_request = return_to_cc});
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){.p25p2_flush_partial_audio = dsd_p25p2_flush_partial_audio});
    g_return_result = DSD_TRUNK_TUNE_RESULT_OK;
    p25_sm_ctx_t* ctx = p25_sm_get_ctx();
    p25_sm_init_ctx(ctx, &g_opts, &g_state);

    /* Slot 2's call on the first voice channel, left mid-superframe. */
    p25_sm_event_t grant = p25_sm_ev_group_grant(0x1235, 852000000, 200, 2000, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    open_slot(1, 200, 2000);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 1, 200, 0, 2000, 1, 0);
    superframe_plan plan;
    plan_voice(&plan, -1, 4);
    run_windows(&plan, 0, 1);
    rc |= expect_int("departure fixture plays slot 2", g_block_count, 8);
    g_return_calls = 0;
    p25_sm_release(ctx, &g_opts, &g_state, "test-departure");
    rc |= expect_int("departure returns to the control channel", g_return_calls, 1);

    /* Slot 1's call on the next voice channel, granted once the control channel is decoded again. */
    g_state.p25_last_cc_msg_time_m = dsd_decode_now_mono_s();
    grant = p25_sm_ev_group_grant(0x2234, 853000000, 100, 1000, 0);
    p25_sm_event(ctx, &g_opts, &g_state, &grant);
    open_slot(0, 100, 1000);
    (void)p25_sm_emit_active_call(&g_opts, &g_state, 0, 100, 0, 1000, 1, 0);
    plan_voice(&plan, 4, -1);
    run_windows(&plan, 0, 1);
    rc |= expect_int("next channel plays slot 1", g_block_count, 16);
    rc |= expect_int("next channel followed", p25_sm_get_state(ctx), P25_SM_TUNED);

    g_return_calls = 0;
    end_call(0, 1000U, 100U);
    end_call(0, 1000U, 100U);
    rc |= expect_int("repeated END releases the channel at once", g_return_calls, 1);

    p25_sm_init_ctx(ctx, &g_opts, &g_state);
    dsd_trunk_tuning_hooks_set((dsd_trunk_tuning_hooks){0});
    dsd_p25_optional_hooks_set((dsd_p25_optional_hooks){0});
    return rc;
}

int
main(void) {
    int rc = 0;
    for (int floating = 0; floating < 2; floating++) {
        rc |= test_aligned_two_slots(floating);
        rc |= test_rotated_two_slots(floating);
        rc |= test_single_call(floating);
        rc |= test_slot_end_mid_superframe(floating);
        rc |= test_companion_starts_mid_superframe(floating);
        rc |= test_invalid_2v_duid(floating);
    }
    rc |= test_mono_output();
    rc |= test_partial_flush_with_hpf_and_static_wav();
    rc |= test_release_between_windows();
    rc |= test_departed_slot_leaves_no_activity();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    dsd_state_ext_free_all(&g_state);
    if (rc != 0) {
        return 1;
    }
    printf("P25_P2_VOICE_OUTPUT: OK\n");
    return 0;
}
