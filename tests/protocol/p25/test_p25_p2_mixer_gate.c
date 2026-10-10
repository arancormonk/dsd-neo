// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Validate that P25 Phase 2 output (the voice playout, issue #651) gates each
 * slot on its own verdict and does not cross-mute the opposite slot. A muted
 * companion slot (including an encryption-lockout call) must be transparent:
 * its channel mirrors the audible slot exactly as when the companion is idle,
 * and its own audio is never emitted. Also covers the other synthesized-voice
 * mixers' playback and the #574 audible-audio stamp.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/p25p2_playout.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/protocol/p25/p25_crypto.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

static unsigned char g_audio_capture[2048];
static size_t g_audio_capture_bytes = 0;
static int g_audio_capture_calls = 0;
/* Whether any call since reset_capture() carried a non-zero byte, not only the first one captured above. */
static int g_audio_capture_nonzero = 0;

static int
expect_eq(const char* tag, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got %d want %d\n", tag, got, want);
        return 1;
    }
    return 0;
}

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "%s failed\n", tag);
        return 1;
    }
    return 0;
}

static int
copy_capture_bytes(const char* tag, void* out, size_t expected_bytes) {
    if (g_audio_capture_bytes != expected_bytes) {
        DSD_FPRINTF(stderr, "%s: got %zu want %zu\n", tag, g_audio_capture_bytes, expected_bytes);
        return 1;
    }
    DSD_MEMCPY(out, g_audio_capture, expected_bytes);
    return 0;
}

static void
capture_blast(const dsd_opts* opts, dsd_state* state, size_t bytes, const void* data) {
    (void)opts;
    (void)state;
    g_audio_capture_calls++;
    for (size_t i = 0U; data != NULL && i < bytes && !g_audio_capture_nonzero; i++) {
        g_audio_capture_nonzero = ((const unsigned char*)data)[i] != 0U;
    }
    if (g_audio_capture_calls == 1 && data && bytes <= sizeof(g_audio_capture)) {
        DSD_MEMCPY(g_audio_capture, data, bytes);
        g_audio_capture_bytes = bytes;
    }
}

static void
reset_capture(void) {
    DSD_MEMSET(g_audio_capture, 0, sizeof(g_audio_capture));
    g_audio_capture_bytes = 0;
    g_audio_capture_calls = 0;
    g_audio_capture_nonzero = 0;
}

static void
fill_f32_frame(float frame[160], float value) {
    for (int i = 0; i < 160; i++) {
        frame[i] = value;
    }
}

static int
seed_group_call(dsd_state* state, uint8_t slot, uint64_t target) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_P25P2_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .observed_m = 1.0,
    };
    return dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN) > 0;
}

static void
seed_companion_crypto_state(dsd_state* state, int slot, int lockout_enabled, int locked_out, int algid, int aes_loaded,
                            unsigned long long scalar_key, int marker) {
    dsd_p25_crypto_state crypto_state = DSD_P25_CRYPTO_UNKNOWN;
    if (marker || (lockout_enabled && locked_out)) {
        crypto_state = DSD_P25_CRYPTO_ENCRYPTED_PENDING;
    } else if (algid == 0x80) {
        crypto_state = DSD_P25_CRYPTO_CLEAR;
    } else if (algid != 0 && (aes_loaded || scalar_key != 0ULL)) {
        crypto_state = DSD_P25_CRYPTO_DECRYPTABLE;
    }
    state->p25_crypto_state[slot] = crypto_state;
}

static int
short_18_blocks_are_clear(short frames[18][160]) {
    for (int frame = 0; frame < 18; frame++) {
        for (int i = 0; i < 160; i++) {
            if (frames[frame][i] != 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int
short_block_is_clear(const short* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (samples[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static int
float_block_is_clear(const float* samples, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (samples[i] < -1.0e-6f || samples[i] > 1.0e-6f) {
            return 0;
        }
    }
    return 1;
}

static void
playout_setup(dsd_opts* opts, dsd_state* st, int floating, int channels) {
    DSD_MEMSET(opts, 0, sizeof(*opts));
    DSD_MEMSET(st, 0, sizeof(*st));
    reset_capture();
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->floating_point = floating;
    opts->pulse_digi_rate_out = 8000;
    opts->pulse_digi_out_channels = channels;
    opts->slot1_on = 1;
    opts->slot2_on = 1;
    opts->audio_gain = 25;
    st->aout_gain = 49.0f;
    st->aout_gainR = 49.0f;
    dsd_p25p2_playout_reset(st, -1);
}

/* One frame queued for @p slot at superframe pair @p pair, as the frame decoder queues a frame its decode gate
   admitted: every sample @p value (short, and float before the gain), its verdict taken from the slot's gates now. */
static void
queue_frame(dsd_opts* opts, dsd_state* st, int slot, int pair, int value) {
    static uint32_t serial = 0U;
    short* s = (slot == 0) ? st->s_l : st->s_r;
    float* f = (slot == 0) ? st->audio_out_temp_buf : st->audio_out_temp_bufR;
    for (int i = 0; i < 160; i++) {
        s[i] = (short)value;
        f[i] = (float)value;
    }
    st->mbe_short_silenced[slot] = 0U;
    dsd_p25p2_playout_stage(opts, st, slot, pair, ++serial, NULL);
}

/* The first captured block's two channels, as 1 when they carry @p left / @p right (short), or for float the sign of
   each (+1, -1 or 0). */
static int
first_block(int floating, int channels, int* left, int* right) {
    if (floating) {
        float out[160 * 2] = {0.0f};
        const int rc = copy_capture_bytes("first float block", out, (size_t)160 * (size_t)channels * sizeof(float));
        *left = (out[0] > 0.0f) ? 1 : ((out[0] < 0.0f) ? -1 : 0);
        *right = (channels == 2) ? ((out[1] > 0.0f) ? 1 : ((out[1] < 0.0f) ? -1 : 0)) : *left;
        return rc;
    }
    short out[160 * 2] = {0};
    const int rc = copy_capture_bytes("first short block", out, (size_t)160 * (size_t)channels * sizeof(short));
    *left = out[0];
    *right = (channels == 2) ? out[1] : out[0];
    return rc;
}

/* A clear slot beside a muted companion, in both output formats. The companion's frames reach the playout only past
   its decode gate; when its crypto refuses output (encryption lockout, a pending classification) the frames it
   queued carry that verdict. Either way the companion is transparent: its channel mirrors the clear slot, exactly as
   when it is idle, and its own audio is never emitted. */
static int
run_clear_beside_muted_companion_case(int floating, int clear_slot, int enc_lockout_enabled, int companion_locked_out,
                                      int muted_slot_algid, int muted_slot_aes_loaded,
                                      unsigned long long muted_slot_key, int muted_slot_svc, int muted_slot_marker) {
    static dsd_opts opts;
    static dsd_state st;
    const int companion = clear_slot ^ 1;
    int rc = 0;
    char tag[128];

    playout_setup(&opts, &st, floating, 2);
    opts.trunk_tune_enc_calls = enc_lockout_enabled ? 0 : 1;
    st.p25_p2_audio_allowed[clear_slot] = 1;
    st.p25_p2_audio_allowed[companion] = 0;
    st.p25_crypto_state[clear_slot] = DSD_P25_CRYPTO_CLEAR;
    if (companion == 1) {
        st.dmr_soR = muted_slot_svc;
        st.payload_algidR = muted_slot_algid;
        st.RR = muted_slot_key;
    } else {
        st.dmr_so = muted_slot_svc;
        st.payload_algid = muted_slot_algid;
        st.R = muted_slot_key;
    }
    st.aes_key_loaded[companion] = muted_slot_aes_loaded;
    st.aes_key_segments[companion] = muted_slot_aes_loaded ? 4U : 0U;
    seed_companion_crypto_state(&st, companion, enc_lockout_enabled, companion_locked_out, muted_slot_algid,
                                muted_slot_aes_loaded, muted_slot_key, muted_slot_marker);

    queue_frame(&opts, &st, clear_slot, 0, 100);
    if (!p25_crypto_audio_output_permitted(&opts, &st, companion)) {
        queue_frame(&opts, &st, companion, 0, -3000);
    }
    dsd_p25p2_playout_pair_done(&opts, &st);

    DSD_SNPRINTF(tag, sizeof(tag), "%s clear=%d lockout=%d/%d alg=0x%02X marker=%d: blocks",
                 floating ? "float" : "short", clear_slot, enc_lockout_enabled, companion_locked_out, muted_slot_algid,
                 muted_slot_marker);
    rc |= expect_eq(tag, g_audio_capture_calls, 1);
    int left = 0;
    int right = 0;
    const int copied = first_block(floating, 2, &left, &right);
    rc |= copied;
    if (copied == 0) {
        const int want = floating ? 1 : 100;
        DSD_SNPRINTF(tag, sizeof(tag), "%s clear=%d alg=0x%02X: clear slot heard", floating ? "float" : "short",
                     clear_slot, muted_slot_algid);
        rc |= expect_eq(tag, clear_slot == 0 ? left : right, want);
        DSD_SNPRINTF(tag, sizeof(tag), "%s clear=%d alg=0x%02X: companion mirrors clear slot",
                     floating ? "float" : "short", clear_slot, muted_slot_algid);
        rc |= expect_eq(tag, clear_slot == 0 ? right : left, want);
    }
    return rc;
}

static int
run_clear_beside_muted_companion_matrix(void) {
    static const struct {
        int lockout;
        int locked_out;
        int algid;
        int aes;
        unsigned long long key;
        int svc;
        int marker;
    } cases[] = {
        {1, 1, 0, 0, 0ULL, 0x40, 0},    {1, 1, 0, 0, 0ULL, 0, 1},       {0, 0, 0, 0, 0ULL, 0x40, 0},
        {1, 0, 0x80, 0, 0ULL, 0x40, 0}, {1, 0, 0x84, 1, 0ULL, 0x40, 0}, {1, 0, 0x81, 0, 1ULL, 0x40, 0},
    };

    int rc = 0;
    for (int floating = 0; floating < 2; floating++) {
        for (int clear_slot = 0; clear_slot < 2; clear_slot++) {
            for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
                rc |= run_clear_beside_muted_companion_case(floating, clear_slot, cases[c].lockout, cases[c].locked_out,
                                                            cases[c].algid, cases[c].aes, cases[c].key, cases[c].svc,
                                                            cases[c].marker);
            }
        }
    }
    return rc;
}

/* A talkgroup hold over both slots' calls does not override a crypto-blocked companion: the blocked slot's frames
   (-3000 sentinel) are never emitted, and its channel mirrors the clear slot. One-channel output plays the clear slot
   alone at its level. */
static int
run_clear_plus_blocked_hold_case(int floating, int channels, int clear_slot, int matching_hold) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    playout_setup(&opts, &st, floating, channels);
    opts.trunk_tune_enc_calls = 0;
    if (matching_hold) {
        rc |= expect_eq("hold seed left call", seed_group_call(&st, 0U, 100), 1);
        rc |= expect_eq("hold seed right call", seed_group_call(&st, 1U, 100), 1);
        st.tg_hold = 100;
    }
    st.p25_p2_audio_allowed[clear_slot] = 1;
    st.p25_p2_audio_allowed[clear_slot ^ 1] = 0;
    st.p25_crypto_state[clear_slot] = DSD_P25_CRYPTO_CLEAR;
    st.p25_crypto_state[clear_slot ^ 1] = DSD_P25_CRYPTO_BLOCKED;
    queue_frame(&opts, &st, clear_slot, 0, 100);
    queue_frame(&opts, &st, clear_slot ^ 1, 0, -3000);
    dsd_p25p2_playout_pair_done(&opts, &st);

    rc |= expect_eq("hold captured calls", g_audio_capture_calls, 1);
    int left = 0;
    int right = 0;
    const int copied = first_block(floating, channels, &left, &right);
    rc |= copied;
    if (copied == 0) {
        const int want = floating ? 1 : 100;
        rc |= expect_eq("hold left output", left, want);
        rc |= expect_eq("hold right output", right, want);
    }
    dsd_state_ext_free_all(&st);
    return rc;
}

/* Reverse mute (-q) plays only encrypted audio: a clear frame stays silent, a frame of a call the crypto marks
   encrypted plays. */
static int
run_reverse_mute_case(dsd_p25_crypto_state crypto_state, int expect_audio) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    playout_setup(&opts, &st, 1, 2);
    opts.trunk_tune_enc_calls = 1;
    opts.reverse_mute = 1;
    st.p25_p2_audio_allowed[0] = 1;
    st.p25_crypto_state[0] = crypto_state;
    queue_frame(&opts, &st, 0, 0, 384);
    dsd_p25p2_playout_pair_done(&opts, &st);

    rc |= expect_eq("reverse mute output state", g_audio_capture_calls >= 1, expect_audio);
    if (expect_audio) {
        int left = 0;
        int right = 0;
        const int copied = first_block(1, 2, &left, &right);
        rc |= copied;
        if (copied == 0) {
            rc |= expect_eq("reverse mute encrypted audio audible", left, 1);
        }
    }
    return rc;
}

// A muted companion (encryption lockout) must mirror the audible slot in every MAC state either slot can be sitting
// in. The burst hint only records the last MAC PDU seen for a slot; the playout routes from what each slot's frames
// may play, so no hint (MAC_PTT 20, ACTIVE 21, HANGTIME 22, END 23, IDLE 24, the LCCH marker 30, cleared) changes it.
static int
run_muted_companion_burst_hint_case(int clear_slot, int clear_burst, int companion_burst) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;
    char tag[96];

    playout_setup(&opts, &st, 0, 2);
    opts.trunk_tune_enc_calls = 0;
    st.synctype = DSD_SYNC_P25P2_POS;
    st.p25_p2_audio_allowed[clear_slot] = 1;
    st.p25_p2_audio_allowed[clear_slot ^ 1] = 0;
    st.p25_crypto_state[clear_slot] = DSD_P25_CRYPTO_CLEAR;
    st.p25_crypto_state[clear_slot ^ 1] = DSD_P25_CRYPTO_BLOCKED;
    if (clear_slot == 0) {
        st.dmrburstL = clear_burst;
        st.dmrburstR = companion_burst;
    } else {
        st.dmrburstR = clear_burst;
        st.dmrburstL = companion_burst;
    }
    for (int f = 0; f < 4; f++) {
        queue_frame(&opts, &st, clear_slot, 0, 100);
        queue_frame(&opts, &st, clear_slot ^ 1, 0, -3000);
    }
    dsd_p25p2_playout_pair_done(&opts, &st);

    DSD_SNPRINTF(tag, sizeof(tag), "burst hint clear=%d/%d companion=%d captured", clear_slot, clear_burst,
                 companion_burst);
    rc |= expect_eq(tag, g_audio_capture_calls, 4);
    int left = 0;
    int right = 0;
    const int copied = first_block(0, 2, &left, &right);
    rc |= copied;
    if (copied == 0) {
        DSD_SNPRINTF(tag, sizeof(tag), "burst hint clear=%d/%d companion=%d left", clear_slot, clear_burst,
                     companion_burst);
        rc |= expect_eq(tag, left, 100);
        DSD_SNPRINTF(tag, sizeof(tag), "burst hint clear=%d/%d companion=%d right", clear_slot, clear_burst,
                     companion_burst);
        rc |= expect_eq(tag, right, 100);
    }
    return rc;
}

static int
run_muted_companion_burst_hint_matrix(void) {
    static const int bursts[] = {0, 20, 21, 22, 23, 24, 30};
    const size_t count = sizeof(bursts) / sizeof(bursts[0]);
    int rc = 0;

    for (int clear_slot = 0; clear_slot < 2; clear_slot++) {
        for (size_t clear_idx = 0; clear_idx < count; clear_idx++) {
            for (size_t companion_idx = 0; companion_idx < count; companion_idx++) {
                rc |= run_muted_companion_burst_hint_case(clear_slot, bursts[clear_idx], bursts[companion_idx]);
            }
        }
    }
    return rc;
}

/* The release flush plays what both slots queued, in both formats, with the verdicts the frames were queued with
   (issue #651: it no longer reopens the gates, and -y no longer drops the tail). It leaves the gates alone. */
static int
run_partial_flush_case(int floating) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    playout_setup(&opts, &st, floating, 2);
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    st.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
    queue_frame(&opts, &st, 0, 0, 321);
    queue_frame(&opts, &st, 1, 0, -654);
    st.p25_p2_audio_allowed[0] = 0;
    st.p25_p2_audio_allowed[1] = 0;

    dsd_p25p2_flush_partial_audio(&opts, &st);

    rc |= expect_eq("partial flush captured calls", g_audio_capture_calls, 1);
    int left = 0;
    int right = 0;
    const int copied = first_block(floating, 2, &left, &right);
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("partial flush left sample", left, floating ? 1 : 321);
        rc |= expect_eq("partial flush right sample", right, floating ? -1 : -654);
    }
    rc |= expect_eq("partial flush leaves the left gate", st.p25_p2_audio_allowed[0], 0);
    rc |= expect_eq("partial flush leaves the right gate", st.p25_p2_audio_allowed[1], 0);
    rc |= expect_eq("partial flush empties the left queue", dsd_p25p2_playout_level(&st, 0), 0);
    rc |= expect_eq("partial flush empties the right queue", dsd_p25p2_playout_level(&st, 1), 0);
    return rc;
}

/* At an output rate other than 8 kHz the playout writes nothing (that path has no P25 Phase 2 mixer): the flush plays
   nothing, and still empties the queues so nothing is left for a later carrier. */
static int
run_partial_flush_rate_guard_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    playout_setup(&opts, &st, 0, 2);
    st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
    queue_frame(&opts, &st, 0, 0, 111);
    opts.pulse_digi_rate_out = 48000;
    dsd_p25p2_flush_partial_audio(&opts, &st);
    rc |= expect_eq("partial flush rate guard calls", g_audio_capture_calls, 0);
    rc |= expect_eq("partial flush rate guard empties the queue", dsd_p25p2_playout_level(&st, 0), 0);
    return rc;
}

/* A slot END flush closes the slot's stream and plays nothing on its own: its tail plays at the end of the pair, in
   time order beside the companion, whose audio is neither masked nor lost. */
static int
run_partial_flush_slot_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    for (int slot = 0; slot < 2; slot++) {
        playout_setup(&opts, &st, 0, 2);
        st.p25_p2_audio_allowed[0] = 1;
        st.p25_p2_audio_allowed[1] = 1;
        st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
        st.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
        queue_frame(&opts, &st, 0, 0, 321);
        queue_frame(&opts, &st, 1, 0, -654);

        dsd_p25p2_flush_partial_audio_slot(&opts, &st, slot);
        rc |= expect_eq("slot flush plays nothing on its own", g_audio_capture_calls, 0);
        rc |= expect_eq("slot flush keeps the left frame", dsd_p25p2_playout_level(&st, 0), 1);
        rc |= expect_eq("slot flush keeps the right frame", dsd_p25p2_playout_level(&st, 1), 1);
        rc |= expect_eq("slot flush leaves the left gate", st.p25_p2_audio_allowed[0], 1);
        rc |= expect_eq("slot flush leaves the right gate", st.p25_p2_audio_allowed[1], 1);

        dsd_p25p2_playout_pair_done(&opts, &st);
        rc |= expect_eq("slot flush tail plays at the pair", g_audio_capture_calls, 1);
        int left = 0;
        int right = 0;
        const int copied = first_block(0, 2, &left, &right);
        rc |= copied;
        if (copied == 0) {
            rc |= expect_eq("slot flush tail left", left, 321);
            rc |= expect_eq("slot flush tail right", right, -654);
        }
    }
    return rc;
}

static int
run_short_mono_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    st.synctype = DSD_SYNC_NONE;
    st.audio_out_idx = 160;
    st.audio_out_idx2 = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    for (int i = 0; i < 160; i++) {
        st.s_l[i] = (short)(i + 1);
    }

    playSynthesizedVoiceMS(&opts, &st);

    short out[160] = {0};
    rc |= expect_eq("short mono captured calls", g_audio_capture_calls, 1);
    int copied = copy_capture_bytes("short mono captured bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("short mono first sample", out[0], 1);
        rc |= expect_eq("short mono last sample", out[159], 160);
    }
    rc |= expect_eq("short mono resets idx", st.audio_out_idx, 0);
    rc |= expect_eq("short mono resets ring span", st.audio_out_idx2, 0);
    rc |= expect_eq("short mono clears s_l", short_block_is_clear(st.s_l, 160), 1);
    rc |= expect_true("short mono resets short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("short mono resets float ptr", st.audio_out_float_buf_p == st.audio_out_float_buf + 100);
    return rc;
}

static int
run_float_mono_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.audio_gain = 25;
    st.synctype = DSD_SYNC_NONE;
    st.aout_gain = 49.0f;
    st.audio_out_idx2 = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    fill_f32_frame(st.f_l, 0.25f);

    playSynthesizedVoiceFM(&opts, &st);

    float out[160] = {0.0f};
    rc |= expect_eq("float mono captured calls", g_audio_capture_calls, 1);
    int copied = copy_capture_bytes("float mono captured bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_true("float mono audible", out[0] != 0.0f);
    }
    rc |= expect_eq("float mono resets ring span", st.audio_out_idx2, 0);
    rc |= expect_eq("float mono clears f_l", float_block_is_clear(st.f_l, 160), 1);
    rc |= expect_true("float mono resets short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("float mono resets float ptr", st.audio_out_float_buf_p == st.audio_out_float_buf + 100);
    return rc;
}

static int
run_short_stereo_fdma_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    st.synctype = DSD_SYNC_NONE;
    st.audio_out_idx = 160;
    st.audio_out_idxR = 160;
    st.audio_out_idx2 = 800000;
    st.audio_out_idx2R = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_buf_pR = st.audio_out_bufR + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    st.audio_out_float_buf_pR = st.audio_out_float_bufR + 100;
    for (int i = 0; i < 160; i++) {
        st.s_l[i] = (short)(i + 10);
        st.s_r[i] = (short)(-i - 10);
    }

    playSynthesizedVoiceSS(&opts, &st);

    short out[160 * 2] = {0};
    rc |= expect_eq("short stereo fdma calls", g_audio_capture_calls, 1);
    int copied = copy_capture_bytes("short stereo fdma bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("short stereo fdma left sample", out[0], 10);
        rc |= expect_eq("short stereo fdma right sample", out[1], 10);
        rc |= expect_eq("short stereo fdma last left", out[318], 169);
        rc |= expect_eq("short stereo fdma last right", out[319], 169);
    }
    rc |= expect_eq("short stereo fdma clears left", short_block_is_clear(st.s_l, 160), 1);
    rc |= expect_eq("short stereo fdma clears right", short_block_is_clear(st.s_r, 160), 1);
    rc |= expect_eq("short stereo fdma resets left idx", st.audio_out_idx, 0);
    rc |= expect_eq("short stereo fdma resets right idx", st.audio_out_idxR, 0);
    rc |= expect_eq("short stereo fdma resets left ring", st.audio_out_idx2, 0);
    rc |= expect_eq("short stereo fdma resets right ring", st.audio_out_idx2R, 0);
    rc |= expect_true("short stereo fdma resets left short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("short stereo fdma resets right short ptr", st.audio_out_buf_pR == st.audio_out_bufR + 100);
    return rc;
}

static int
run_short_stereo_dmr3_playback_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.slot1_on = 1;
    opts.slot2_on = 1;
    st.audio_out_idx = 160;
    st.audio_out_idxR = 160;
    st.audio_out_idx2 = 800000;
    st.audio_out_idx2R = 800000;
    st.audio_out_buf_p = st.audio_out_buf + 100;
    st.audio_out_buf_pR = st.audio_out_bufR + 100;
    st.audio_out_float_buf_p = st.audio_out_float_buf + 100;
    st.audio_out_float_buf_pR = st.audio_out_float_bufR + 100;
    for (int i = 0; i < 160; i++) {
        st.s_l4[0][i] = (short)(100 + i);
        st.s_r4[0][i] = (short)(-100 - i);
        st.s_l4[1][i] = (short)(200 + i);
        st.s_r4[1][i] = (short)(-200 - i);
        st.s_l4[2][i] = (short)(300 + i);
        st.s_r4[2][i] = (short)(-300 - i);
    }

    playSynthesizedVoiceSS3(&opts, &st);

    short out[160 * 2] = {0};
    rc |= expect_eq("short stereo dmr3 calls", g_audio_capture_calls, 3);
    int copied = copy_capture_bytes("short stereo dmr3 first bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        rc |= expect_eq("short stereo dmr3 first left", out[0], 100);
        rc |= expect_eq("short stereo dmr3 first right", out[1], -100);
        rc |= expect_eq("short stereo dmr3 last left", out[318], 259);
        rc |= expect_eq("short stereo dmr3 last right", out[319], -259);
    }
    rc |= expect_eq("short stereo dmr3 clears left", short_18_blocks_are_clear(st.s_l4), 1);
    rc |= expect_eq("short stereo dmr3 clears right", short_18_blocks_are_clear(st.s_r4), 1);
    rc |= expect_eq("short stereo dmr3 resets left ring", st.audio_out_idx2, 0);
    rc |= expect_eq("short stereo dmr3 resets right ring", st.audio_out_idx2R, 0);
    rc |= expect_true("short stereo dmr3 resets left short ptr", st.audio_out_buf_p == st.audio_out_buf + 100);
    rc |= expect_true("short stereo dmr3 resets right short ptr", st.audio_out_buf_pR == st.audio_out_bufR + 100);
    return rc;
}

static int
run_beeper_float_stereo_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 1;
    opts.pulse_digi_out_channels = 2;

    beeper(&opts, &st, 1, 40, 86, 1);

    float out[160 * 2] = {0.0f};
    rc |= expect_eq("beeper float stereo calls", g_audio_capture_calls, 2);
    int copied = copy_capture_bytes("beeper float stereo bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        int saw_right_audio = 0;
        int saw_left_audio = 0;
        for (int i = 0; i < 160; i++) {
            if (out[(i * 2) + 0] != 0.0f) {
                saw_left_audio = 1;
            }
            if (out[(i * 2) + 1] != 0.0f) {
                saw_right_audio = 1;
            }
        }
        rc |= expect_eq("beeper float stereo left silent", saw_left_audio, 0);
        rc |= expect_eq("beeper float stereo right audible", saw_right_audio, 1);
    }
    return rc;
}

static int
run_beeper_short_mono_case(void) {
    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;

    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();

    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.floating_point = 0;
    opts.pulse_digi_out_channels = 1;

    beeper(&opts, &st, 0, 40, 86, 1);

    short out[160] = {0};
    rc |= expect_eq("beeper short mono calls", g_audio_capture_calls, 2);
    int copied = copy_capture_bytes("beeper short mono bytes", out, sizeof(out));
    rc |= copied;
    if (copied == 0) {
        int saw_audio = 0;
        for (int i = 0; i < 160; i++) {
            if (out[i] != 0) {
                saw_audio = 1;
            }
        }
        rc |= expect_eq("beeper short mono audible", saw_audio, 1);
    }
    return rc;
}

/* Issue #574: whether the code under test noted the audible-audio stamp since stamp_clear(). main() arms it. */
static void
stamp_clear(void) {
    dsd_audio_activity_reset();
}

static int
stamp_noted(void) {
    uint64_t stamp = 0U;
    dsd_audio_activity_read(&stamp, NULL);
    return stamp != 0U;
}

/* Issue #574: the P25 Phase 2 playout stamps audible audio when it plays a frame the slot's decode gate admitted and
   whose verdict and slot switch let it be heard; frames of decoded silence count, in both formats. A slot whose decode
   gate is closed queues nothing; a crypto-refused frame, a switched-off slot and a muted output stamp nothing. */
static int
test_playout_stamps_frames_of_audible_slots(void) {
    static const struct {
        const char* tag;
        int queued[2];
        int slot_on[2];
        int blocked_left;
        int audio_out;
        int channels;
        int want_stamp;
    } cases[] = {
        {"slot 1 frame", {1, 0}, {1, 1}, 0, 1, 2, 1},
        {"slot 2 frame", {0, 1}, {1, 1}, 0, 1, 2, 1},
        {"slot 1 frame, mono output", {1, 0}, {1, 1}, 0, 1, 1, 1},
        {"slot 1 gated, queued nothing", {0, 0}, {1, 1}, 0, 1, 2, 0},
        {"slot 2 frame beside a gated slot 1", {0, 1}, {1, 1}, 0, 1, 2, 1},
        {"slot 1 frame, slot 1 switched off", {1, 0}, {0, 1}, 0, 1, 2, 0},
        {"slot 1 frame, slot 1 crypto blocked", {1, 0}, {1, 1}, 1, 1, 2, 0},
        {"slot 1 frame, output muted", {1, 0}, {1, 1}, 0, 0, 2, 0},
    };

    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;
    for (int floating = 0; floating < 2; floating++) {
        for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
            playout_setup(&opts, &st, floating, cases[c].channels);
            opts.audio_out = cases[c].audio_out;
            opts.slot1_on = cases[c].slot_on[0];
            opts.slot2_on = cases[c].slot_on[1];
            st.synctype = DSD_SYNC_P25P2_POS;
            for (int slot = 0; slot < 2; slot++) {
                st.p25_p2_audio_allowed[slot] = 1;
                st.p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
            }
            if (cases[c].blocked_left) {
                st.payload_algid = 0x81;
                st.p25_crypto_state[0] = DSD_P25_CRYPTO_BLOCKED;
            }
            for (int slot = 0; slot < 2; slot++) {
                if (cases[c].queued[slot]) {
                    queue_frame(&opts, &st, slot, 0, 0);
                }
            }
            stamp_clear();
            dsd_p25p2_playout_pair_done(&opts, &st);
            const int plays = cases[c].audio_out && cases[c].want_stamp;
            char tag[128];
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: plays", floating ? "float" : "short", cases[c].tag);
            rc |= expect_eq(tag, g_audio_capture_calls >= 1, plays);
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: stamp", floating ? "float" : "short", cases[c].tag);
            rc |= expect_eq(tag, stamp_noted(), cases[c].want_stamp);
        }
    }

    /* The release flush plays and stamps what it drains, and a crypto-refused tail neither plays nor stamps. A slot
       END flush only closes the stream. */
    for (int rejected = 1; rejected >= 0; rejected--) {
        playout_setup(&opts, &st, 0, 2);
        st.p25_p2_audio_allowed[0] = 1;
        st.p25_crypto_state[0] = rejected ? DSD_P25_CRYPTO_BLOCKED : DSD_P25_CRYPTO_CLEAR;
        st.payload_algid = rejected ? 0x81 : 0x80;
        for (int f = 0; f < 5; f++) {
            queue_frame(&opts, &st, 0, f / 4, 321);
        }
        stamp_clear();
        dsd_p25p2_flush_partial_audio_slot(&opts, &st, 0);
        rc |= expect_eq("slot flush stamps nothing", stamp_noted(), 0);
        dsd_p25p2_flush_partial_audio(&opts, &st);
        const char* what = rejected ? "rejected release flush" : "allowed release flush";
        char tag[128];
        DSD_SNPRINTF(tag, sizeof(tag), "%s: blocks", what);
        rc |= expect_eq(tag, g_audio_capture_calls, rejected ? 0 : 5);
        DSD_SNPRINTF(tag, sizeof(tag), "%s: stamp", what);
        rc |= expect_eq(tag, stamp_noted(), rejected ? 0 : 1);
    }

    /* The beeper plays tones, never decoded audio. */
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&st, 0, sizeof(st));
    reset_capture();
    opts.audio_out = 1;
    opts.audio_out_type = 8;
    opts.pulse_digi_out_channels = 2;
    stamp_clear();
    beeper(&opts, &st, 0, 40, 86, 1);
    rc |= expect_eq("beeper plays", g_audio_capture_calls >= 1, 1);
    rc |= expect_eq("beeper stamps nothing", stamp_noted(), 0);
    return rc;
}

/* Issue #574 and #651: the playout routes from what each slot's frames may play, so a slot with frames is heard and
   stamps, whichever slot a talkgroup hold or the last voice burst would have preferred. SS18 preferred a slot by hold
   or burst hint even when it had filled nothing, and played its silence over the companion's audio. */
static int
test_playout_plays_the_slot_with_frames(void) {
    static const struct {
        const char* tag;
        int hold;
        int filled_slot;
    } cases[] = {
        {"hold on both calls, only slot 2 has frames", 1, 1},
        {"slot 1's voice burst last, only slot 2 has frames", 0, 1},
        {"hold on both calls, only slot 1 has frames", 1, 0},
        {"slot 1's voice burst last, only slot 1 has frames", 0, 0},
    };

    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;
    for (size_t c = 0U; c < sizeof(cases) / sizeof(cases[0]); c++) {
        playout_setup(&opts, &st, 0, 2);
        st.dmrburstL = 21;
        if (cases[c].hold) {
            rc |= expect_eq("copy seed left call", seed_group_call(&st, 0U, 100), 1);
            rc |= expect_eq("copy seed right call", seed_group_call(&st, 1U, 100), 1);
            st.tg_hold = 100;
            st.dmrburstR = 21;
        }
        for (int slot = 0; slot < 2; slot++) {
            st.p25_p2_audio_allowed[slot] = 1;
            st.p25_crypto_state[slot] = DSD_P25_CRYPTO_CLEAR;
        }
        for (int f = 0; f < 4; f++) {
            queue_frame(&opts, &st, cases[c].filled_slot, 0, 1234);
        }
        stamp_clear();
        dsd_p25p2_playout_pair_done(&opts, &st);
        char tag[128];
        DSD_SNPRINTF(tag, sizeof(tag), "copy %s: plays", cases[c].tag);
        rc |= expect_eq(tag, g_audio_capture_calls, 4);
        DSD_SNPRINTF(tag, sizeof(tag), "copy %s: audio emitted", cases[c].tag);
        rc |= expect_eq(tag, g_audio_capture_nonzero, 1);
        DSD_SNPRINTF(tag, sizeof(tag), "copy %s: stamp", cases[c].tag);
        rc |= expect_eq(tag, stamp_noted(), 1);
        dsd_state_ext_free_all(&st);
    }
    return rc;
}

/* Issue #574: the playout stamps only what an output receives. The null output (-o null, which keeps output type 9
   once unmuted) and a local stream that is not open receive nothing, so a frame the mix would play stamps nothing. */
static int
test_playout_stamps_nothing_no_output_takes(void) {
    static const struct {
        const char* tag;
        int out_type;
    } outputs[] = {
        {"null output", 9},
        {"local stream not open", 0},
    };

    static dsd_opts opts;
    static dsd_state st;
    int rc = 0;
    for (int floating = 0; floating <= 1; floating++) {
        for (size_t o = 0U; o < sizeof(outputs) / sizeof(outputs[0]); o++) {
            playout_setup(&opts, &st, floating, 2);
            opts.audio_out_type = outputs[o].out_type;
            opts.audio_out_stream = NULL;
            st.p25_p2_audio_allowed[0] = 1;
            st.p25_p2_audio_allowed[1] = 1;
            st.p25_crypto_state[0] = DSD_P25_CRYPTO_CLEAR;
            st.p25_crypto_state[1] = DSD_P25_CRYPTO_CLEAR;
            queue_frame(&opts, &st, 0, 0, 0);
            stamp_clear();
            dsd_p25p2_playout_pair_done(&opts, &st);
            char tag[128];
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: blocks", floating ? "float" : "short", outputs[o].tag);
            rc |= expect_eq(tag, g_audio_capture_calls, 0);
            DSD_SNPRINTF(tag, sizeof(tag), "%s %s: stamp", floating ? "float" : "short", outputs[o].tag);
            rc |= expect_eq(tag, stamp_noted(), 0);
        }
    }
    return rc;
}

int
main(void) {
    int rc = 0;
    static dsd_state st;
    DSD_MEMSET(&st, 0, sizeof(st));

    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_blast});
    rc |= run_clear_beside_muted_companion_matrix();
    for (int floating = 0; floating < 2; floating++) {
        for (int clear_slot = 0; clear_slot < 2; clear_slot++) {
            rc |= run_clear_plus_blocked_hold_case(floating, 2, clear_slot, /*matching_hold*/ 1);
            rc |= run_clear_plus_blocked_hold_case(floating, 1, clear_slot, /*matching_hold*/ 0);
            rc |= run_clear_plus_blocked_hold_case(floating, 1, clear_slot, /*matching_hold*/ 1);
        }
    }
    rc |= run_reverse_mute_case(DSD_P25_CRYPTO_CLEAR, /*expect_audio*/ 0);
    rc |= run_reverse_mute_case(DSD_P25_CRYPTO_BLOCKED, /*expect_audio*/ 1);
    rc |= run_muted_companion_burst_hint_matrix();
    rc |= run_partial_flush_case(/*floating*/ 0);
    rc |= run_partial_flush_case(/*floating*/ 1);
    rc |= run_partial_flush_rate_guard_case();
    rc |= run_partial_flush_slot_case();
    rc |= run_short_mono_playback_case();
    rc |= run_float_mono_playback_case();
    rc |= run_short_stereo_fdma_playback_case();
    rc |= run_short_stereo_dmr3_playback_case();
    rc |= run_beeper_float_stereo_case();
    rc |= run_beeper_short_mono_case();
    dsd_audio_activity_arm();
    rc |= test_playout_stamps_frames_of_audible_slots();
    rc |= test_playout_plays_the_slot_with_frames();
    rc |= test_playout_stamps_nothing_no_output_takes();
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});

    return rc;
}
