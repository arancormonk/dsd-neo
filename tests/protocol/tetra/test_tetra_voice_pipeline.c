// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * TETRA speech through the codec library to the digital voice output. The test codec (tetra_test_codec.c) is loaded
 * through --tetra-codec as a user's would be; its samples say which decode made them and whether it was a bad frame,
 * and the test reads them back from the raw output (-o -, here a temp file) to check what played, in what order:
 * speech starts at a CRC-passed burst, bad frames after it are concealed until a run of them ends it, a stolen half
 * slot keeps the frame timing, 240-sample frames leave the output whole 160-sample blocks, and the follow, encryption,
 * talkgroup and record gates hold back what they should: trunking on, only the followed call's proven timeslots play
 * (direct mode is not trunked and always plays), and each burst is judged by the call it belongs to.
 */

#include <dsd-neo/core/audio_activity.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_support.h"
#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_state.h"
#include "tetra_test_codec.h"
#include "tetra_test_pdu.h"
#include "tetra_voice_state.h"

#ifndef TETRA_TEST_CODEC_GOOD
#error TETRA_TEST_CODEC_GOOD names the test codec library
#endif

#define FRAME   ((size_t)DSD_TETRA_CODEC_FRAME_SAMPLES)
#define MAX_OUT (64U * FRAME)

static int g_fail = 0;

static void
expect(int ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", what);
        g_fail++;
    }
}

/* What the output should hold for one played frame: the codec's decode number and whether it was a bad frame. */
typedef struct {
    uint32_t frame;
    int bad;
    const uint8_t* bits; /* the 137 bits of the speech frame the codec got; NULL for a concealed frame */
} played_frame;

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    char out_path[DSD_TEST_PATH_MAX];
    int out_fd;
} fixture;

static int
fixture_open(fixture* f) {
    DSD_MEMSET(f, 0, sizeof(*f));
    f->opts = (dsd_opts*)calloc(1, sizeof(dsd_opts));
    f->state = (dsd_state*)calloc(1, sizeof(dsd_state));
    if (!f->opts || !f->state) {
        return -1;
    }
    initOpts(f->opts);
    initState(f->state);
    f->out_fd = dsd_test_mkstemp(f->out_path, sizeof(f->out_path), "tetra_voice_out");
    if (f->out_fd < 0) {
        return -1;
    }
    /* Raw 16-bit mono at 8 kHz, unity gain, no high-pass: the output carries the codec's samples unchanged. */
    f->opts->audio_out = 1;
    f->opts->audio_out_type = 1;
    f->opts->audio_out_fd = f->out_fd;
    f->opts->pulse_digi_rate_out = 8000;
    f->opts->pulse_digi_out_channels = 1;
    f->opts->floating_point = 0;
    f->opts->audio_gain = -1.0F;
    f->opts->use_hpf_d = 0;
    f->opts->slot1_on = 1;
    f->opts->dmr_mute_encL = 1;
    DSD_SNPRINTF(f->opts->tetra_codec_path, sizeof(f->opts->tetra_codec_path), "%s", TETRA_TEST_CODEC_GOOD);
    f->state->last_vc_sync_time = 0;
    f->state->last_vc_sync_time_m = 0.0;
    /* A BSCH has set the TDMA clock: the bursts are TN1's (a trunked or heard timeslot is one the clock places). */
    tetra_state_t* const ts = tetra_st(f->state);
    if (!ts) {
        return -1;
    }
    ts->tdma_valid = 1;
    ts->tn = 1;
    return 0;
}

static void
fixture_close(fixture* f) {
    if (f->out_fd >= 0) {
        (void)dsd_close(f->out_fd);
        (void)remove(f->out_path);
    }
    if (f->state) {
        freeState(f->state);
    }
    free(f->state);
    free(f->opts);
}

/* The samples written so far. */
static size_t
fixture_output(const fixture* f, int16_t* samples, size_t max) {
    FILE* in = fopen(f->out_path, "rb");
    if (!in) {
        return 0;
    }
    const size_t n = fread(samples, sizeof(int16_t), max, in);
    (void)fclose(in);
    return n;
}

static tetra_voice_t*
voice_of(fixture* f) {
    return &tetra_st(f->state)->voice;
}

/* A recognisable bit pattern per burst. */
static void
burst_bits(uint8_t* bits, int n, unsigned seed) {
    for (int i = 0; i < n; i++) {
        bits[i] = (uint8_t)((((unsigned)i * 7U) + seed) >> 2) & 1U;
    }
}

/* Check the output from sample @p from on holds @p count frames as @p frames describes, @p samples long in all (a
   whole number of output blocks, so the last frame can be cut short). */
static void
expect_output(const fixture* f, size_t from, const played_frame* frames, size_t count, size_t samples,
              const char* what) {
    static int16_t out[MAX_OUT];
    const size_t n = fixture_output(f, out, MAX_OUT);
    char label[160];
    DSD_SNPRINTF(label, sizeof(label), "%s: %zu samples written, expected %zu", what, n - from, samples);
    expect(n == from + samples, label);
    if (n != from + samples || samples > count * FRAME) {
        return;
    }
    for (size_t k = 0; k < samples; k++) {
        const played_frame* p = &frames[k / FRAME];
        const tetra_test_codec_sample s = tetra_test_codec_read(out[from + k]);
        int ok = s.valid && s.frame == p->frame && s.bad == p->bad;
        if (ok && p->bits) {
            ok = s.bit == (p->bits[(k % FRAME) % DSD_TETRA_CODEC_FRAME_BITS] & 1);
        }
        if (!ok) {
            DSD_SNPRINTF(label, sizeof(label), "%s: sample %zu (%d) is not decode %u%s", what, k, out[from + k],
                         p->frame, p->bad ? " (bad)" : "");
            expect(0, label);
            return;
        }
    }
}

static void
test_clean_stream_plays_every_frame(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "clean: fixture");
    uint8_t b1[TETRA_SPEECH_FULL_BITS];
    uint8_t b2[TETRA_SPEECH_FULL_BITS];
    uint8_t s1[2 * DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t s2[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(b1, TETRA_SPEECH_FULL_BITS, 1U);
    burst_bits(b2, TETRA_SPEECH_FULL_BITS, 5U);
    tetra_voice_reorder_full(b1, s1);
    tetra_voice_reorder_full(b2, s2);

    dsd_audio_activity_arm();
    dsd_audio_activity_reset();
    tetra_voice_full_slot(f.opts, f.state, b1, 1);
    tetra_voice_full_slot(f.opts, f.state, b2, 1);

    const played_frame want[] = {
        {1, 0, s1},
        {2, 0, s1 + DSD_TETRA_CODEC_FRAME_BITS},
        {3, 0, s2},
        {4, 0, s2 + DSD_TETRA_CODEC_FRAME_BITS},
    };
    expect_output(&f, 0, want, 4, 4 * FRAME, "clean stream");
    uint64_t stamp = 0;
    dsd_audio_activity_read(&stamp, NULL);
    expect(stamp != 0U, "clean: the played speech notes audible audio");
    expect(f.state->last_vc_sync_time != 0 && f.state->last_vc_sync_time_m > 0.0, "clean: speech marks reception");
    const tetra_voice_t* v = voice_of(&f);
    expect(v->codec_status == TETRA_CODEC_STATUS_READY && v->frames == 4 && v->concealed == 0 && v->carry_len == 0,
           "clean: codec ready, four frames, nothing carried");
    fixture_close(&f);
}

static void
test_integrity_starts_conceals_and_ends_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "integrity: fixture");
    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    uint8_t speech[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(bits, TETRA_SPEECH_FULL_BITS, 3U);
    tetra_voice_reorder_full(bits, speech);
    const uint8_t* a = speech;
    const uint8_t* b = speech + DSD_TETRA_CODEC_FRAME_BITS;

    /* A burst whose CRC failed proves nothing: no codec, no audio, no reception. */
    tetra_voice_full_slot(f.opts, f.state, bits, 0);
    expect_output(&f, 0, NULL, 0, 0, "integrity: bad first burst");
    expect(voice_of(&f)->codec_status == TETRA_CODEC_STATUS_UNLOADED, "integrity: no codec for an unproven burst");
    expect(f.state->last_vc_sync_time == 0, "integrity: an unproven burst is not reception");

    tetra_voice_full_slot(f.opts, f.state, bits, 1); /* decodes 1, 2 */
    tetra_voice_full_slot(f.opts, f.state, bits, 0); /* 3, 4 concealed */
    tetra_voice_full_slot(f.opts, f.state, bits, 0); /* 5, 6 concealed: four in a row */
    tetra_voice_full_slot(f.opts, f.state, bits, 0); /* a fifth bad frame ends the speech */
    expect(voice_of(&f)->confirmed == 0, "integrity: a run of bad frames ends the speech");
    tetra_voice_full_slot(f.opts, f.state, bits, 1); /* 7, 8: a good burst starts it again */

    const played_frame want[] = {
        {1, 0, a}, {2, 0, b}, {3, 1, NULL}, {4, 1, NULL}, {5, 1, NULL}, {6, 1, NULL}, {7, 0, a}, {8, 0, b},
    };
    expect_output(&f, 0, want, 8, 8 * FRAME, "integrity");
    expect(voice_of(&f)->concealed == 4, "integrity: four frames concealed");
    fixture_close(&f);
}

static void
test_half_slot_and_carry(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "half: fixture");
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);

    /* Before any speech the stolen frame is nothing to conceal; the half-slot frame starts the speech: one 160-sample
       block out, 80 samples carried. */
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "half: 80 samples carried");
    /* Now the stolen frame is concealed ahead of the speech frame. */
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    const played_frame want[] = {
        {1, 0, half_frame},
        {2, 1, NULL},
        {3, 0, half_frame},
        {4, 0, full_frames},
        {5, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS},
    };
    /* 5 frames are 1200 samples: 1120 played, 80 waiting. */
    expect_output(&f, 0, want, 5, 1120, "half slot");

    /* A carrier boundary drops what waits and starts the codec over: decodes count from 1 again. */
    tetra_voice_reset(f.state);
    expect(voice_of(&f)->carry_len == 0 && voice_of(&f)->confirmed == 0 && voice_of(&f)->codec == NULL,
           "reset: nothing carried, no speech, no decoder");
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    const played_frame after[] = {{1, 0, full_frames}, {2, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS}};
    expect_output(&f, 1120, after, 2, 2 * FRAME, "after reset");
    fixture_close(&f);
}

/* Samples written by one good full-slot burst under the fixture's current settings. */
static size_t
played_by_burst(fixture* f) {
    static int16_t out[MAX_OUT];
    uint8_t bits[TETRA_SPEECH_FULL_BITS];
    burst_bits(bits, TETRA_SPEECH_FULL_BITS, 4U);
    const size_t before = fixture_output(f, out, MAX_OUT);
    tetra_voice_full_slot(f->opts, f->state, bits, 1);
    return fixture_output(f, out, MAX_OUT) - before;
}

/* Trunking follows group call @p call_id to @p gssi on the timeslots of @p timeslots, its tune settled and its
   signalling decoded on the carrier. */
static void
follow_call(fixture* f, uint16_t call_id, uint32_t gssi, uint8_t timeslots) {
    tetra_call_event setup = {.call_id = call_id, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(NULL, f->state, &setup, NULL);
    tetra_calls_find(f->state, call_id)->called_ssi = gssi;
    tetra_state_t* ts = tetra_st(f->state);
    f->opts->trunk_enable = 1;
    ts->sm.state = (uint8_t)TETRA_SM_TUNED;
    ts->sm.followed.call_id = call_id;
    ts->sm.followed.timeslots = timeslots;
    tetra_confirm_note_followed_call(f->state);
}

static void
follow_group(fixture* f, uint32_t gssi, uint8_t timeslots) {
    follow_call(f, 9, gssi, timeslots);
}

static void
test_follow_gate(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "follow: fixture");
    tetra_state_t* ts = tetra_st(f.state);
    f.opts->trunk_enable = 1;
    ts->sm.state = (uint8_t)TETRA_SM_ON_CC;
    f.opts->trunk_is_tuned = 1; /* the state machine, not the shared flag, says what is followed */
    expect(played_by_burst(&f) == 0, "follow: nothing plays on the control channel");
    expect(f.state->last_vc_sync_time == 0, "follow: control-channel bursts are not speech reception");

    follow_group(&f, 1, 8); /* TN1 */
    ts->confirm.followed_call = 0;
    ts->tdma_valid = 1;
    ts->tn = 1;
    expect(played_by_burst(&f) == 0, "follow: not before the call's signalling proved it on the carrier");
    tetra_confirm_note_followed_call(f.state);
    ts->tn = 2;
    expect(played_by_burst(&f) == 0, "follow: another timeslot does not play");
    ts->tn = 1;
    expect(played_by_burst(&f) == 2 * FRAME, "follow: the followed timeslot plays");
    expect(ts->sm.t_voice_m > 0.0, "follow: its speech keeps the state machine on the call");

    /* A direct mode channel has no trunked call to follow: its speech plays with trunking on. */
    ts->sm.state = (uint8_t)TETRA_SM_IDLE;
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    expect(played_by_burst(&f) == 2 * FRAME, "follow: direct mode plays while trunking is enabled");
    fixture_close(&f);
}

static const uint8_t k_air_key[10] = {0x1F, 0x2E, 0x3D, 0x4C, 0x5B, 0x6A, 0x79, 0x88, 0x97, 0xA6};

/* Carrier 321 of band 3 with offset +6,25 kHz (EN 300 392-2 clause 21.4.4.1), in location area 77 with colour code 9. */
#define AIR_CARRIER    321u
#define AIR_CARRIER_HZ 308031250u
#define AIR_LA         77u
#define AIR_COLOUR     9u

/* An air-encrypted trunked call on network 262/1001 with TEA1 on the carrier the receiver is tuned to, its clock at TN1
   FN3 MN7 of hyperframe 5; the operator's cipher key is loaded when @p key_loaded. */
static void
air_encrypted_call(fixture* f, int key_loaded) {
    tetra_state_t* ts = tetra_st(f->state);
    ts->calls.slot_enc[0] = 1u + TETRA_ENC_MODE_ODD; /* the carrier's addressed signalling is encrypted */
    ts->net_known = 1;
    ts->mcc = 262;
    ts->mnc = 1001;
    ts->colour = AIR_COLOUR;
    ts->sysinfo_known = 1;
    ts->la = AIR_LA;
    ts->freq_band = 3;
    ts->freq_offset = 1;
    f->opts->audio_in_type = AUDIO_IN_RTL;
    f->opts->rtlsdr_center_freq = AIR_CARRIER_HZ;
    ts->hyperframe_valid = 1;
    ts->hyperframe = 5;
    ts->tdma_valid = 1;
    ts->tn = 1;
    ts->fn = 3;
    ts->mn = 7;
    expect(tetra_air_ksg_remember(f->state, 262, 1001, 0) == 1, "air: TEA1 for the network");
    dsd_tetra_air_key* key = &f->state->tetra_air_keys[0];
    key->loaded = (uint8_t)(key_loaded != 0);
    key->mcc = 262;
    key->mnc = 1001;
    DSD_MEMCPY(key->key, k_air_key, sizeof(k_air_key));
}

static void
test_encryption_gate(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "crypto: fixture");
    tetra_state_t* ts = tetra_st(f.state);
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: clear speech plays");

    air_encrypted_call(&f, 0);
    expect(played_by_burst(&f) == 0, "crypto: encrypted speech no key decrypts is muted");
    f.opts->dmr_mute_encL = 0;
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: with the encrypted-audio mute off it plays");
    f.opts->dmr_mute_encL = 1;
    air_encrypted_call(&f, 1);
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: speech the loaded key decrypts plays");

    f.opts->reverse_mute = 1;
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: reverse mute plays encrypted speech");
    ts->calls.slot_enc[1] = 1u + TETRA_ENC_MODE_NONE;
    expect(played_by_burst(&f) == 0, "crypto: reverse mute mutes speech its timeslot's signalling says is clear");
    f.opts->reverse_mute = 0;

    /* The followed call's own encryption decides, not the carrier's: end-to-end encryption is never decrypted. */
    follow_group(&f, 1234, 0x08);
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: a clear followed call plays on an encrypted carrier");
    tetra_calls_find(f.state, 9)->e2ee = 1;
    expect(played_by_burst(&f) == 0, "crypto: an end-to-end encrypted call is muted");
    f.opts->dmr_mute_encL = 0;
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: unless the encrypted-audio mute is off");
    f.opts->dmr_mute_encL = 1;
    tetra_calls_find(f.state, 9)->e2ee = 0;
    tetra_calls_find(f.state, 9)->air_enc = TETRA_ENC_MODE_EVEN;
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: an air encrypted call the key decrypts plays");
    f.opts->trunk_enable = 0;

    /* Direct mode has no key stream: an encrypted direct-mode call is undecryptable, whatever key is loaded. */
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_AIR;
    expect(played_by_burst(&f) == 0, "crypto: encrypted direct-mode speech is muted");
    ts->dmo_crypt = TETRA_DMO_CRYPT_UNKNOWN;
    expect(played_by_burst(&f) == 0, "crypto: direct-mode speech whose security class is unknown is muted");
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    expect(played_by_burst(&f) == 2 * FRAME, "crypto: clear direct-mode speech plays");
    fixture_close(&f);
}

/* @p count bits of @p plain XORed with the TEA1 key stream from bit @p kss on (EN 300 392-7 table 6.4), for the
   call air_encrypted_call() sets up: the key stream of the cipher key's ECK for its carrier (TB5, clause 6.3.2.2). */
static void
expected_clear(const fixture* f, const uint8_t* plain, int count, int kss, uint8_t* out) {
    const tetra_state_t* ts = tetra_st(f->state);
    uint8_t ks[64];
    uint8_t eck[10];
    const uint32_t iv = tetra_air_build_iv(ts->tn, ts->fn, ts->mn, ts->hyperframe, 0);
    tetra_tea_derive_eck(AIR_CARRIER, AIR_LA, AIR_COLOUR, k_air_key, eck);
    tetra_tea_keystream(0, eck, iv, ks, (kss + count + 7) / 8);
    for (int i = 0; i < count; i++) {
        const int bit = kss + i;
        out[i] = (uint8_t)((plain[i] ^ (ks[bit / 8] >> (7 - (bit % 8)))) & 1u);
    }
}

/* Air interface encryption sits in the upper MAC, above the channel coding (EN 300 392-7 clause 6.1), and the
   sensitivity-class order of EN 300 395-2 tables 5 and 6 is the first step of that channel coding (clause 5.2: the
   type-1 bits are ordered into classes at interface 2). So the key stream covers the speech frames in the order the
   codec made them: a full slot's frame A bits B1..B137 take KSS(0..136) and frame B's KSS(137..273); the speech
   frame of an STCH + TCH/S burst takes KSS(216..352) (table 6.4). The test undoes the class order first (the order
   TETRA_SPEECH_REORDER checks against tables 5 and 6) and only then applies the key stream. */
static void
test_air_key_stream_positions(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "key stream: fixture");
    air_encrypted_call(&f, 1);
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 7U);
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 9U);
    tetra_voice_reorder_full(full, full_frames);
    tetra_voice_reorder_half(half, half_frame);
    expected_clear(&f, full_frames, 2 * DSD_TETRA_CODEC_FRAME_BITS, 0, full_frames);
    expected_clear(&f, half_frame, DSD_TETRA_CODEC_FRAME_BITS, 216, half_frame);

    tetra_voice_full_slot(f.opts, f.state, full, 1);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    const played_frame want[] = {
        {1, 0, full_frames},
        {2, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS},
        {3, 1, NULL},
        {4, 0, half_frame},
    };
    expect_output(&f, 0, want, 4, 4 * FRAME, "key stream positions");
    fixture_close(&f);
}

static void
test_talkgroup_gate(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "talkgroup: fixture");
    follow_group(&f, 1234, 0x0F);
    expect(dsd_tg_policy_session_avoid_add(f.state, 1234) == 0, "talkgroup: avoid 1234");
    expect(played_by_burst(&f) == 0, "talkgroup: an avoided group is muted");
    tetra_calls_find(f.state, 9)->called_ssi = 1235;
    expect(played_by_burst(&f) == 2 * FRAME, "talkgroup: another group plays");

    /* Direct mode is judged by the destination of its call. */
    tetra_state_t* ts = tetra_st(f.state);
    ts->sm.state = (uint8_t)TETRA_SM_IDLE;
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    ts->calls.dmo.active = 1;
    ts->calls.dmo.group_known = 1;
    ts->calls.dmo.group = 1;
    ts->calls.dmo.src = 100;
    ts->calls.dmo.dst = 1234;
    expect(played_by_burst(&f) == 0, "talkgroup: a direct mode call to an avoided group is muted");
    ts->calls.dmo.dst = 1236;
    expect(played_by_burst(&f) == 2 * FRAME, "talkgroup: a direct mode call to another group plays");
    fixture_close(&f);
}

/* A talkgroup that may be heard but not recorded plays, and the static WAV stays empty. */
static void
test_record_gate(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "record: fixture");
    char wav_path[DSD_TEST_PATH_MAX];
    const int wav_fd = dsd_test_mkstemp(wav_path, sizeof(wav_path), "tetra_voice_wav");
    expect(wav_fd >= 0, "record: temp WAV");
    if (wav_fd < 0) {
        fixture_close(&f);
        return;
    }
    (void)dsd_close(wav_fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    f.opts->wav_out_f = sf_open(wav_path, SFM_WRITE, &info);
    f.opts->static_wav_file = 1;
    expect(f.opts->wav_out_f != NULL, "record: WAV open");

    dsd_tg_policy_entry row;
    expect(dsd_tg_policy_make_exact_entry(500U, "A", "play only", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0,
           "record: policy row");
    row.record = 0U;
    expect(dsd_tg_policy_upsert_exact(f.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0, "record: policy set");
    dsd_call_observation call;
    DSD_MEMSET(&call, 0, sizeof(call));
    call.protocol = DSD_SYNC_TETRA_NDB_POS;
    call.slot = 0;
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.ota_target_id = 500U;
    call.policy_target_id = 500U;
    call.observed_m = 1.0;
    expect(dsd_call_state_observe(f.state, &call, DSD_CALL_BOUNDARY_BEGIN) == 1, "record: call active");
    follow_group(&f, 500, 0x0F);

    expect(played_by_burst(&f) == 2 * FRAME, "record: the call plays");
    if (f.opts->wav_out_f) {
        (void)sf_close(f.opts->wav_out_f);
        f.opts->wav_out_f = NULL;
    }
    SF_INFO read_info;
    DSD_MEMSET(&read_info, 0, sizeof(read_info));
    SNDFILE* wav = sf_open(wav_path, SFM_READ, &read_info);
    expect(wav != NULL && read_info.frames == 0, "record: and is not recorded");
    if (wav) {
        (void)sf_close(wav);
    }
    (void)remove(wav_path);
    fixture_close(&f);
}

/* A static WAV for the fixture's recordings; returns 0 when it is open. */
static int
open_static_wav(fixture* f, char* path, size_t path_size) {
    const int fd = dsd_test_mkstemp(path, path_size, "tetra_voice_wav");
    if (fd < 0) {
        return -1;
    }
    (void)dsd_close(fd);
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    info.samplerate = 8000;
    info.channels = 2;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    f->opts->wav_out_f = sf_open(path, SFM_WRITE, &info);
    f->opts->static_wav_file = 1;
    return f->opts->wav_out_f ? 0 : -1;
}

/* Close the fixture's static WAV and count the frames it took. */
static long
close_static_wav(fixture* f, const char* path) {
    if (f->opts->wav_out_f) {
        (void)sf_close(f->opts->wav_out_f);
        f->opts->wav_out_f = NULL;
    }
    SF_INFO info;
    DSD_MEMSET(&info, 0, sizeof(info));
    SNDFILE* wav = sf_open(path, SFM_READ, &info);
    const long frames = wav ? (long)info.frames : -1L;
    if (wav) {
        (void)sf_close(wav);
    }
    (void)remove(path);
    return frames;
}

/* Check that the @p count samples from sample @p from are samples @p first.. of decode @p frame (a good frame of
   @p bits). */
static void
expect_segment(const fixture* f, size_t from, uint32_t frame, const uint8_t* bits, size_t first, size_t count,
               const char* what) {
    static int16_t out[MAX_OUT];
    const size_t n = fixture_output(f, out, MAX_OUT);
    char label[160];
    DSD_SNPRINTF(label, sizeof(label), "%s: %zu samples written, %zu wanted", what, n, from + count);
    expect(n >= from + count, label);
    for (size_t k = 0; k < count && from + k < n; k++) {
        const tetra_test_codec_sample smp = tetra_test_codec_read(out[from + k]);
        if (!smp.valid || smp.bad || smp.frame != frame
            || smp.bit != (bits[(first + k) % DSD_TETRA_CODEC_FRAME_BITS] & 1)) {
            DSD_SNPRINTF(label, sizeof(label), "%s: sample %zu (%d) is not sample %zu of decode %u", what, from + k,
                         out[from + k], first + k, frame);
            expect(0, label);
            return;
        }
    }
}

/* What one call left of its speech -- the samples a half slot carries, the codec's state, the confirmation -- never
   reaches the next: not after the state machine moves to another call on the same carrier (whose record policy would
   take the carried samples), not when the direct mode channel carries another call, and not across the end of a
   transmission of the same call. */
static void
test_call_boundary_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "boundary: fixture");
    char wav_path[DSD_TEST_PATH_MAX];
    expect(open_static_wav(&f, wav_path, sizeof(wav_path)) == 0, "boundary: WAV open");
    dsd_tg_policy_entry row;
    expect(dsd_tg_policy_make_exact_entry(500U, "A", "play only", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0,
           "boundary: policy row");
    row.record = 0U;
    expect(dsd_tg_policy_upsert_exact(f.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0, "boundary: policy");
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);

    /* Call 9 to group 500, played but not recorded: one block out, 80 samples carried. */
    follow_call(&f, 9, 500, 0x0F);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "boundary: 80 samples carried");
    /* Call 10 to group 600, which records, on the same carrier: its half slot starts from nothing, so its stolen frame
       is not concealed with call 9's codec state and its block holds none of call 9's samples. */
    follow_call(&f, 10, 600, 0x0F);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    const size_t block = DSD_VOICE_FRAME_SAMPLES;
    expect_segment(&f, 0, 1, half_frame, 0, block, "boundary: call 9");
    expect_segment(&f, block, 1, half_frame, 0, block, "boundary: call 10");
    expect(close_static_wav(&f, wav_path) == (long)DSD_VOICE_FRAME_SAMPLES,
           "boundary: the recording holds call 10's block alone");

    /* Direct mode: the call on the channel changes. */
    tetra_state_t* ts = tetra_st(f.state);
    ts->sm.state = (uint8_t)TETRA_SM_IDLE;
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    ts->calls.dmo.active = 1;
    ts->calls.dmo.dst = 1234;
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    ts->calls.dmo.dst = 1236;
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 2 * block, 1, half_frame, 0, block, "boundary: direct mode call to 1234");
    expect_segment(&f, 3 * block, 1, full_frames, 0, FRAME, "boundary: direct mode call to 1236");
    expect_segment(&f, 3 * block + FRAME, 2, full_frames + DSD_TETRA_CODEC_FRAME_BITS, 0, FRAME,
                   "boundary: direct mode call to 1236, second frame");

    /* The same trunked call: its transmission ends (D-TX CEASED) and the next one is granted. */
    ts->dmo = 0;
    follow_call(&f, 11, 700, 0x0F);
    const size_t before_tx = 6 * block;
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    tetra_call_event ceased = {.call_id = 11, .kind = TETRA_CALL_EV_TX_CEASED};
    tetra_calls_on_event(f.opts, f.state, &ceased, NULL);
    tetra_call_event granted = {.call_id = 11, .kind = TETRA_CALL_EV_TX_GRANTED, .has = TETRA_CALL_HAS_GRANT};
    tetra_calls_on_event(f.opts, f.state, &granted, NULL);
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, before_tx, 1, half_frame, 0, block, "boundary: the first transmission");
    expect_segment(&f, before_tx + block, 1, full_frames, 0, FRAME, "boundary: the next transmission");
    expect_segment(&f, before_tx + block + FRAME, 2, full_frames + DSD_TETRA_CODEC_FRAME_BITS, 0, FRAME,
                   "boundary: the next transmission, second frame");
    static int16_t out[MAX_OUT];
    expect(voice_of(&f)->carry_len == 0 && fixture_output(&f, out, MAX_OUT) == before_tx + block + 2 * FRAME,
           "boundary: nothing more played, nothing carried");
    fixture_close(&f);
}

/* D-TX GRANTED or D-TX INTERRUPT (EN 300 392-2 clauses 14.7.1.15, 14.7.1.17) of call @p call_id granting the
   transmission to another user, @p talker transmitting. */
static void
grant_transmission(fixture* f, uint8_t kind, uint16_t call_id, uint32_t talker) {
    tetra_call_event ev = {.call_id = call_id, .kind = kind};
    ev.has = (uint8_t)(TETRA_CALL_HAS_GRANT | TETRA_CALL_HAS_TALKER);
    ev.grant = 3U;
    ev.talker_ssi = talker;
    tetra_calls_on_event(f->opts, f->state, &ev, NULL);
}

/* Another transmitting party in the same call is the next transmission, the D-TX CEASED between them heard or not:
   D-TX INTERRUPT or D-TX GRANTED naming another talker, or a direct mode call to the same destination from another
   source, starts the speech over, so neither the last talker's carried samples nor its codec state reach the next
   one. The same talker named again (a late-entry repeat) is the same transmission. */
static void
test_talker_change_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "talker: fixture");
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);
    const size_t block = DSD_VOICE_FRAME_SAMPLES;

    /* The talker not known yet: decode 1 plays one block and carries 80 samples. */
    follow_call(&f, 12, 800, 0x0F);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "talker: 80 samples carried");
    /* D-TX GRANTED names talker 0x4444: the same transmission, so the carried samples and decodes 2 and 3 play. */
    const uint8_t* const b = full_frames + DSD_TETRA_CODEC_FRAME_BITS;
    grant_transmission(&f, TETRA_CALL_EV_TX_GRANTED, 12, 0x4444U);
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 0, 1, half_frame, 0, block, "talker: the speech before the talker was named");
    expect_segment(&f, block, 1, half_frame, block, FRAME - block,
                   "talker: naming the talker keeps the carried samples");
    expect_segment(&f, FRAME, 2, full_frames, 0, FRAME, "talker: naming the talker keeps the codec");
    /* Named again (a late-entry repeat): decode 3's carried samples, then decodes 4 and 5. */
    grant_transmission(&f, TETRA_CALL_EV_TX_GRANTED, 12, 0x4444U);
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 4 * block, 3, b, block, FRAME - block, "talker: the same talker keeps the carried samples");
    expect_segment(&f, (4 * block) + (FRAME - block), 4, full_frames, 0, FRAME,
                   "talker: the same talker keeps the codec");
    expect(voice_of(&f)->carry_len == 80, "talker: 80 samples of decode 5 carried");

    /* D-TX INTERRUPT to talker 0x5555: decode 5's carried samples go, the codec starts over. */
    size_t at = 7 * block;
    grant_transmission(&f, TETRA_CALL_EV_TX_INTERRUPT, 12, 0x5555U);
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, at, 1, full_frames, 0, FRAME, "talker: D-TX INTERRUPT starts over");
    expect_segment(&f, at + FRAME, 2, full_frames + DSD_TETRA_CODEC_FRAME_BITS, 0, FRAME,
                   "talker: D-TX INTERRUPT, second frame");
    at += 2 * FRAME;

    /* D-TX GRANTED to talker 0x6666. */
    grant_transmission(&f, TETRA_CALL_EV_TX_GRANTED, 12, 0x6666U);
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, at, 1, full_frames, 0, FRAME, "talker: D-TX GRANTED to another talker starts over");
    at += 2 * FRAME;

    /* Direct mode: the call to 1234 from source 0x10, then from source 0x11. */
    tetra_state_t* ts = tetra_st(f.state);
    ts->sm.state = (uint8_t)TETRA_SM_IDLE;
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    ts->calls.dmo.active = 1;
    ts->calls.dmo.dst = 1234;
    ts->calls.dmo.src = 0x10;
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "talker: direct mode, 80 samples carried");
    ts->calls.dmo.src = 0x11;
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, at, 1, half_frame, 0, block, "talker: direct mode source 0x10");
    expect_segment(&f, at + block, 1, full_frames, 0, FRAME, "talker: direct mode source 0x11 starts over");
    static int16_t out[MAX_OUT];
    expect(fixture_output(&f, out, MAX_OUT) == at + block + 2 * FRAME, "talker: nothing else played");
    fixture_close(&f);
}

/* Speech that trails D-TX CEASED belongs to the transmission that ended, though its talker is no longer known: a grant
   after D-TX CEASED starts the next transmission, which takes nothing of that trail, neither its carried samples nor
   its codec state, whatever talker it names; and so does a grant that names no talker where one was known. */
static void
test_grant_after_cease_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "next transmission: fixture");
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);
    const size_t block = DSD_VOICE_FRAME_SAMPLES;

    follow_call(&f, 12, 800, 0x0F);
    grant_transmission(&f, TETRA_CALL_EV_TX_GRANTED, 12, 0x4444U);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    tetra_call_event ceased = {.call_id = 12, .kind = TETRA_CALL_EV_TX_CEASED};
    tetra_calls_on_event(f.opts, f.state, &ceased, NULL);
    /* The trail of talker 0x4444's transmission: its speech starts over at the end, and carries 80 samples. */
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80 && voice_of(&f)->confirmed == 1, "next transmission: the trail carries");

    grant_transmission(&f, TETRA_CALL_EV_TX_GRANTED, 12, 0x5555U);
    tetra_voice_full_slot(f.opts, f.state, full, 0);
    static int16_t out[MAX_OUT];
    expect(fixture_output(&f, out, MAX_OUT) == 2 * block,
           "next transmission: nothing of the trail, nothing concealed with its codec");
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 0, 1, half_frame, 0, block, "next transmission: the transmission");
    expect_segment(&f, block, 1, half_frame, 0, block, "next transmission: its trail");
    expect_segment(&f, 2 * block, 1, full_frames, 0, FRAME, "next transmission: from its own first decode");
    fixture_close(&f);

    /* D-TX CEASED unheard: a grant that names no talker where talker 0x5555 was known is the next transmission too. */
    expect(fixture_open(&f) == 0, "next transmission: fixture");
    follow_call(&f, 12, 800, 0x0F);
    grant_transmission(&f, TETRA_CALL_EV_TX_GRANTED, 12, 0x5555U);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "next transmission: talker 0x5555 carries");
    tetra_call_event unnamed = {.call_id = 12, .kind = TETRA_CALL_EV_TX_GRANTED, .has = TETRA_CALL_HAS_GRANT};
    unnamed.grant = 3U;
    tetra_calls_on_event(f.opts, f.state, &unnamed, NULL);
    tetra_voice_full_slot(f.opts, f.state, full, 0);
    expect(fixture_output(&f, out, MAX_OUT) == block, "next transmission: no talker named, nothing of 0x5555's speech");
    fixture_close(&f);
}

/* Trunking follows individual call @p call_id from @p caller to @p callee (a D-SETUP naming its calling party, table
   14.15), @p talker transmitting (D-TX GRANTED to another user). */
static void
follow_private_call(fixture* f, uint16_t call_id, uint32_t caller, uint32_t callee, uint32_t talker) {
    tetra_call_event setup = {.call_id = call_id, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x00};
    setup.has = TETRA_CALL_HAS_CALLING;
    setup.calling_ssi = caller;
    tetra_calls_on_event(NULL, f->state, &setup, NULL);
    tetra_calls_find(f->state, call_id)->called_ssi = callee;
    tetra_state_t* ts = tetra_st(f->state);
    f->opts->trunk_enable = 1;
    ts->sm.state = (uint8_t)TETRA_SM_TUNED;
    ts->sm.followed.call_id = call_id;
    ts->sm.followed.timeslots = 0x0F;
    tetra_confirm_note_followed_call(f->state);
    grant_transmission(f, TETRA_CALL_EV_TX_GRANTED, call_id, talker);
}

/* An individual call's speech is judged by both its parties whoever transmits, as DMR and P25 judge a unit-to-unit
   transmission: the talker is the canonical source and the other party the target. Call 13 from ISSI 111 to ISSI 222,
   the callee transmitting (D-TX GRANTED naming 222): the caller's record=0 keeps the callee's speech out of the
   recording, and an allowlist naming only the caller lets the callee's replies play. */
static void
test_private_call_judged_by_both_parties(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "private: fixture");
    char wav_path[DSD_TEST_PATH_MAX];
    expect(open_static_wav(&f, wav_path, sizeof(wav_path)) == 0, "private: WAV open");
    dsd_tg_policy_entry row;
    expect(dsd_tg_policy_make_exact_entry(111U, "A", "caller", DSD_TG_POLICY_SOURCE_IMPORTED, &row) == 0,
           "private: policy row");
    row.record = 0U;
    expect(dsd_tg_policy_upsert_exact(f.state, &row, DSD_TG_POLICY_UPSERT_REPLACE_FIRST) == 0, "private: policy");
    follow_private_call(&f, 13, 111U, 222U, 222U);
    expect(played_by_burst(&f) == 2 * FRAME, "private: the callee's reply plays");
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.kind == DSD_CALL_KIND_PRIVATE_VOICE
               && call.ota_source_id == 222U && call.ota_target_id == 111U,
           "private: the talker the canonical source, the caller the target");
    expect(close_static_wav(&f, wav_path) == 0L, "private: and stays out of the recording the caller's policy refuses");
    /* The allowlist on: the caller listed, the callee not. */
    f.opts->trunk_use_allow_list = 1;
    expect(played_by_burst(&f) == 2 * FRAME, "private: the callee's reply plays under the caller's allowlist entry");
    fixture_close(&f);
}

/* Not trunking, call @p call_id to group @p gssi whose traffic channel is timeslot 2 of the tuned carrier, allocated at
   @p alloc_m on the decode clock. */
static void
hear_call(fixture* f, uint16_t call_id, uint32_t gssi, double alloc_m) {
    tetra_call_event setup = {.call_id = call_id, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(NULL, f->state, &setup, NULL);
    tetra_call_t* const c = tetra_calls_find(f->state, call_id);
    c->called_ssi = gssi;
    c->alloc.freq_hz = (long)f->opts->rtlsdr_center_freq;
    c->alloc.timeslots = tetra_slot_bit(2);
    c->alloc.up_down = 1;
    c->alloc_m = alloc_m;
}

/* Not trunking, a D-SETUP that allocates the heard timeslot to another call of the same group, the last call's release
   unheard, makes the speech there the new call's: it starts over at the new call's first burst even when that burst
   fails its CRC and nothing of the new call has been published yet, so none of its bad frames is concealed with the
   last call's codec state and none of the last call's carried samples play under it. */
static void
test_call_identifier_change_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "call id: fixture");
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);
    const size_t block = DSD_VOICE_FRAME_SAMPLES;
    f.opts->audio_in_type = AUDIO_IN_RTL;
    f.opts->rtlsdr_center_freq = 400243750U;
    tetra_state_t* const ts = tetra_st(f.state);
    ts->tdma_valid = 1;
    ts->tn = 2;

    /* Call 9 to group 500: decode 1 plays one block and carries 80 samples. */
    hear_call(&f, 9, 500, 1.0);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "call id: 80 samples of call 9 carried");
    /* Call 10 to group 500 on the same timeslot: its first burst fails its CRC and plays nothing. */
    hear_call(&f, 10, 500, 2.0);
    tetra_voice_full_slot(f.opts, f.state, full, 0);
    static int16_t out[MAX_OUT];
    expect(fixture_output(&f, out, MAX_OUT) == block, "call id: no frame of call 10 concealed with call 9's codec");
    expect(voice_of(&f)->carry_len == 0 && voice_of(&f)->confirmed == 0, "call id: call 10's speech starts over");
    /* Its first good burst starts a codec of its own: decodes 1 and 2. */
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 0, 1, half_frame, 0, block, "call id: call 9's speech");
    expect_segment(&f, block, 1, full_frames, 0, FRAME, "call id: call 10's speech from its own first decode");

    /* D-INFO gives call 10 the identifier 14: the same call, so its codec goes on with decodes 3 and 4. */
    tetra_call_event info = {
        .call_id = 10,
        .kind = TETRA_CALL_EV_INFO,
        .has = TETRA_CALL_HAS_NEW_ID,
        .new_call_id = 14,
    };
    tetra_calls_on_event(f.opts, f.state, &info, NULL);
    expect(tetra_calls_peek(f.state, 14) != NULL, "call id: call 10 renamed 14");
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    const uint8_t* const b = full_frames + DSD_TETRA_CODEC_FRAME_BITS;
    expect_segment(&f, block + (2 * FRAME), 3, full_frames, 0, FRAME, "call id: the renamed call's codec goes on");
    expect_segment(&f, block + (3 * FRAME), 4, b, 0, FRAME, "call id: the renamed call's second frame");
    fixture_close(&f);
}

/* The speech in progress belongs to a transmission of the carrier and network it was heard on: a BSCH naming another
   cell, of the same network or another, says the receiver is on another carrier and starts it over, carried samples and
   codec state with it, while a BSCH of the same cell does not. */
static void
test_network_change_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "network: fixture");
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);
    const size_t block = DSD_VOICE_FRAME_SAMPLES;
    tetra_state_t* const ts = tetra_st(f.state);
    ts->net_known = 1;
    ts->mcc = 262;
    ts->mnc = 30;
    ts->colour = 1;

    /* Not trunking, speech no record names: decode 1 plays one block and carries 80 samples. */
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "network: 80 samples carried");
    ttb_block same = {0};
    ttb_sync_pdu(&same, 262, 30, 1);
    expect(tetra_bsch_parse(same.bits, same.n, f.opts, f.state) == 1, "network: the same cell's BSCH");
    expect(voice_of(&f)->carry_len == 80, "network: the same cell keeps the speech");
    ttb_block cell = {0};
    ttb_sync_pdu(&cell, 262, 30, 2);
    expect(tetra_bsch_parse(cell.bits, cell.n, f.opts, f.state) == 1, "network: another cell's BSCH");
    expect(voice_of(&f)->carry_len == 0 && voice_of(&f)->confirmed == 0,
           "network: another cell of the network starts the speech over");
    tetra_st(f.state)->tdma_valid = 1; /* the clock the BSCH set, at TN1 */
    tetra_st(f.state)->tn = 1;
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "network: the new cell's speech carries 80 samples");
    ttb_block other = {0};
    ttb_sync_pdu(&other, 262, 31, 2);
    expect(tetra_bsch_parse(other.bits, other.n, f.opts, f.state) == 1, "network: another network's BSCH");
    tetra_st(f.state)->tdma_valid = 1;
    tetra_st(f.state)->tn = 1;
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 0, 1, half_frame, 0, block, "network: the last cell's speech");
    expect_segment(&f, block, 1, half_frame, 0, block, "network: the new cell's speech starts over");
    expect_segment(&f, 2 * block, 1, full_frames, 0, FRAME, "network: the new network's speech starts over");
    static int16_t out[MAX_OUT];
    expect(fixture_output(&f, out, MAX_OUT) == 2 * block + 2 * FRAME, "network: none of the carried samples played");
    fixture_close(&f);
}

/* Not trunking, call @p call_id to group @p gssi whose traffic channel is timeslot @p tn of the tuned carrier. */
static void
hear_call_on(fixture* f, uint16_t call_id, uint32_t gssi, uint8_t tn) {
    tetra_call_event setup = {.call_id = call_id, .kind = TETRA_CALL_EV_SETUP, .basic_service = 0x04};
    tetra_calls_on_event(NULL, f->state, &setup, NULL);
    tetra_call_t* const c = tetra_calls_find(f->state, call_id);
    c->called_ssi = gssi;
    c->alloc.freq_hz = (long)f->opts->rtlsdr_center_freq;
    c->alloc.timeslots = tetra_slot_bit(tn);
    c->alloc.up_down = 1;
    c->alloc_m = 1.0;
}

/* Samples one good full-slot burst on timeslot @p tn plays, at @p seconds on the decode clock. */
static size_t
played_on(fixture* f, uint8_t tn, double seconds) {
    dsd_decode_clock_test_set_ns((uint64_t)(seconds * 1e9));
    tetra_state_t* const ts = tetra_st(f->state);
    ts->tdma_valid = 1;
    ts->tn = tn;
    return played_by_burst(f);
}

/* Not trunking, only speech that would be heard holds the receiver on its timeslot: a burst the talkgroup policy mutes,
 * or one whose encryption no key undoes while encrypted audio is muted, holds nothing and renews nothing. So call 9 of
 * blocked group 500 on TN2 leaves call 10 of group 600 on TN3 audible; once the receiver holds TN3, TN2 waits. Speech
 * that does not play shows its call while no other timeslot's speech is shown, so two muted calls do not take canonical
 * slot 0 from each other at every burst; and once the held call turns muted, its hold runs out
 * TETRA_CALL_SPEECH_END_S after its last burst that played, its speech going on or not. */
static void
test_muted_call_holds_no_timeslot(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "muted hold: fixture");
    dsd_decode_clock_use_test(1000000000000ULL);
    f.opts->audio_in_type = AUDIO_IN_RTL;
    f.opts->rtlsdr_center_freq = 400243750U;
    hear_call_on(&f, 9, 500, 2);
    hear_call_on(&f, 10, 600, 3);
    hear_call_on(&f, 11, 700, 4);
    expect(dsd_tg_policy_session_avoid_add(f.state, 500) == 0 && dsd_tg_policy_session_avoid_add(f.state, 700) == 0,
           "muted hold: avoid 500 and 700");

    double t = 1000.0;
    expect(played_on(&f, 2, t) == 0, "muted hold: blocked group 500 is muted");
    dsd_call_snapshot call;
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               && call.ota_target_id == 500U,
           "muted hold: and shows its call");
    const uint64_t shown = call.epoch;
    expect(played_on(&f, 4, t + 0.01) == 0, "muted hold: blocked group 700 is muted too");
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.epoch == shown && call.ota_target_id == 500U,
           "muted hold: and does not take canonical slot 0 from group 500");
    for (int i = 0; i < 3; i++) {
        t += 0.06;
        expect(played_on(&f, 2, t) == 0, "muted hold: group 500 goes on muted");
        expect(played_on(&f, 3, t + 0.01) == 2 * FRAME, "muted hold: group 600 plays beside it");
        expect(played_on(&f, 4, t + 0.02) == 0, "muted hold: group 700 stays muted");
    }
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               && call.ota_target_id == 600U,
           "muted hold: group 600 holds canonical slot 0");

    /* Group 600 muted from here: its speech goes on but holds nothing past its last burst that played. */
    const double last_audible = t + 0.01;
    dsd_tg_policy_session_avoid_clear(f.state);
    expect(dsd_tg_policy_session_avoid_add(f.state, 600) == 0 && dsd_tg_policy_session_avoid_add(f.state, 700) == 0,
           "muted hold: avoid 600 and 700, not 500");
    expect(played_on(&f, 3, last_audible + 0.3) == 0, "muted hold: group 600 is muted");
    expect(played_on(&f, 2, last_audible + 0.4) == 0, "muted hold: TN3 still holds the receiver");
    expect(played_on(&f, 3, last_audible + TETRA_CALL_SPEECH_END_S - 0.05) == 0, "muted hold: group 600 goes on");
    expect(played_on(&f, 2, last_audible + TETRA_CALL_SPEECH_END_S + 0.05) == 2 * FRAME,
           "muted hold: once its hold ran out group 500 is heard");
    expect(played_on(&f, 3, last_audible + TETRA_CALL_SPEECH_END_S + 0.1) == 0, "muted hold: and holds TN2");
    expect(played_on(&f, 2, last_audible + TETRA_CALL_SPEECH_END_S + 0.15) == 2 * FRAME, "muted hold: TN2 goes on");

    /* Speech whose air encryption no key undoes, while encrypted audio is muted, holds nothing either. */
    dsd_tg_policy_session_avoid_clear(f.state);
    t = 1100.0;
    air_encrypted_call(&f, 0);
    hear_call_on(&f, 9, 500, 2);
    hear_call_on(&f, 10, 600, 3);
    tetra_calls_find(f.state, 9)->air_enc = TETRA_ENC_MODE_ODD;
    tetra_calls_find(f.state, 10)->air_enc = TETRA_ENC_MODE_NONE;
    expect(played_on(&f, 2, t) == 0, "muted hold: undecryptable group 500 is muted");
    expect(played_on(&f, 3, t + 0.01) == 2 * FRAME, "muted hold: and clear group 600 plays");
    expect(played_on(&f, 2, t + 0.07) == 0, "muted hold: TN3 holds the receiver");
    dsd_decode_clock_use_system();
    fixture_close(&f);
}

/* Speech no call record names has no identity but its timeslot, and each timeslot is a traffic channel of its own
 * (EN 300 392-2 clause 9.4.2.2), so the speech in progress starts over when the heard speech moves to another one. Not
 * trunking: TN2's clear speech starts a decoder, then its encrypted speech, muted, keeps TN2 heard while its hold runs
 * out; after clear signalling on TN3 its first burst, whose CRC fails, is heard but conceals nothing with TN2's
 * decoder, and TN3's first good burst starts a decoder of its own. */
static void
test_timeslot_change_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "timeslot: fixture");
    dsd_decode_clock_use_test(1000000000000ULL);
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_full(full, full_frames);
    tetra_state_t* const ts = tetra_st(f.state);

    expect(played_on(&f, 2, 1000.0) == 2 * FRAME, "timeslot: TN2's clear speech plays");
    ts->calls.slot_enc[2] = 1u + TETRA_ENC_MODE_ODD;
    expect(played_on(&f, 2, 1000.5) == 0 && played_on(&f, 2, 1001.1) == 0, "timeslot: TN2's encrypted speech is muted");
    expect(voice_of(&f)->confirmed == 1 && ts->calls.heard_tn == 2, "timeslot: and keeps TN2 heard");

    ts->calls.slot_enc[3] = 1u + TETRA_ENC_MODE_NONE;
    ts->tn = 3;
    dsd_decode_clock_test_set_ns(1001200000000ULL);
    static int16_t out[MAX_OUT];
    const size_t before = fixture_output(&f, out, MAX_OUT);
    tetra_voice_full_slot(f.opts, f.state, full, 0);
    expect(fixture_output(&f, out, MAX_OUT) == before, "timeslot: TN3's bad first burst conceals nothing of TN2");
    expect(voice_of(&f)->confirmed == 0 && voice_of(&f)->codec == NULL, "timeslot: TN3's speech starts over");
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, before, 1, full_frames, 0, FRAME, "timeslot: TN3's speech from its own first decode");
    dsd_decode_clock_use_system();
    fixture_close(&f);
}

/* Not trunking, heard speech that stops for TETRA_CALL_SPEECH_END_S is over, published or not (no block proved this
 * carrier, so no anonymous call was): the speech heard on the same timeslot after the gap is another transmission's,
 * so none of the last one's carried samples play and its first bad burst conceals nothing with the last one's
 * decoder. */
static void
test_speech_gap_resets_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "gap: fixture");
    dsd_decode_clock_use_test(1000000000000ULL);
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 2U);
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
    tetra_voice_reorder_half(half, half_frame);
    tetra_voice_reorder_full(full, full_frames);
    const size_t block = DSD_VOICE_FRAME_SAMPLES;
    expect(!tetra_confirm_carrier_proven(f.state), "gap: an unproven carrier");
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    expect(voice_of(&f)->carry_len == 80, "gap: 80 samples carried");
    dsd_decode_clock_test_set_ns(1000000000000ULL + (uint64_t)((TETRA_CALL_SPEECH_END_S + 0.5) * 1e9));
    tetra_calls_tick(f.opts, f.state);
    tetra_voice_full_slot(f.opts, f.state, full, 0);
    static int16_t out[MAX_OUT];
    expect(fixture_output(&f, out, MAX_OUT) == block, "gap: nothing of the last speech, nothing concealed");
    tetra_voice_full_slot(f.opts, f.state, full, 1);
    expect_segment(&f, 0, 1, half_frame, 0, block, "gap: the speech before the gap");
    expect_segment(&f, block, 1, full_frames, 0, FRAME, "gap: the speech after it from its own first decode");
    dsd_decode_clock_use_system();
    fixture_close(&f);
}

/* Trunking, the followed call's speech that stops for TETRA_CALL_SPEECH_END_S has ended its transmission too, as the
 * heard speech's has when not trunking: a PTT release whose D-TX CEASED went unheard, with the next talker's D-TX
 * GRANTED, leaves the call followed through the hangtime, and the speech on its timeslot after the gap is the next
 * transmission's. Its first bad burst conceals nothing with the last transmission's decoder, nothing the last one left
 * waiting plays, and its speech decodes with a decoder of its own. The same holds in direct mode, whose call goes on
 * across the gap too. */
static void
test_followed_speech_gap_resets_speech(void) {
    for (int direct = 0; direct < 2; direct++) {
        fixture f;
        expect(fixture_open(&f) == 0, "followed gap: fixture");
        dsd_decode_clock_use_test(1000000000000ULL);
        uint8_t full[TETRA_SPEECH_FULL_BITS];
        uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
        burst_bits(full, TETRA_SPEECH_FULL_BITS, 6U);
        tetra_voice_reorder_full(full, full_frames);
        if (direct) {
            tetra_st(f.state)->dmo = 1;
            tetra_st(f.state)->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
        } else {
            follow_group(&f, 1, 8); /* TN1 */
            f.opts->trunk_is_tuned = 1;
        }
        tetra_voice_full_slot(f.opts, f.state, full, 1); /* decodes 1, 2 */
        dsd_decode_clock_test_set_ns(1000000000000ULL + (uint64_t)((TETRA_CALL_SPEECH_END_S + 0.5) * 1e9));
        tetra_calls_tick(f.opts, f.state);
        uint16_t followed = 0;
        expect(direct || (tetra_sm_followed_call_id(f.state, &followed) == 1 && followed == 9),
               "followed gap: the call is still followed, inside the hangtime");
        tetra_voice_full_slot(f.opts, f.state, full, 0);
        static int16_t out[MAX_OUT];
        expect(fixture_output(&f, out, MAX_OUT) == 2 * FRAME, "followed gap: the next transmission conceals nothing");
        tetra_voice_full_slot(f.opts, f.state, full, 1);
        const played_frame want[] = {
            {1, 0, full_frames},
            {2, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS},
            {1, 0, full_frames},
            {2, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS},
        };
        expect_output(&f, 0, want, 4, 4 * FRAME, "followed gap: the next transmission from its own first decode");
        dsd_decode_clock_use_system();
        fixture_close(&f);
    }
}

/* Frame 18 is the control frame (EN 300 392-2 clause 19.4.3.1: traffic in frames 1 to 17, frame 18 signalling only,
 * the mapping of clause 9.5.1): the speech codec's 17 bursts of 60 ms fill the 1.02 s multiframe exactly. A burst the
 * TDMA clock places in frame 18 is no speech: one whose signalling block failed its CRC, which the decoder would
 * otherwise try as full-slot or half-slot speech, conceals nothing, and none plays even when its speech CRC passes by
 * chance. Each concealed frame 18 would add 60 ms the air never sent, and the playout would fall behind by that much
 * every multiframe. The speech in progress goes on in frame 1 as if frame 18 had not been there. */
static void
test_frame_18_is_no_speech(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "frame 18: fixture");
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 4U);
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 8U);
    tetra_voice_reorder_full(full, full_frames);
    tetra_state_t* const ts = tetra_st(f.state);
    ts->mn = 1;
    ts->fn = 17;
    tetra_voice_full_slot(f.opts, f.state, full, 1); /* decodes 1, 2 */
    ts->fn = 18;
    tetra_voice_full_slot(f.opts, f.state, full, 0); /* an SCH/F that failed its CRC, tried as speech */
    tetra_voice_half_slot(f.opts, f.state, half, 0); /* an SCH/HD that failed its CRC, tried as a half slot */
    tetra_voice_full_slot(f.opts, f.state, full, 1); /* a speech CRC that passed by chance */
    ts->mn = 2;
    ts->fn = 1;
    tetra_voice_full_slot(f.opts, f.state, full, 1); /* decodes 3, 4 */

    const played_frame want[] = {
        {1, 0, full_frames},
        {2, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS},
        {3, 0, full_frames},
        {4, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS},
    };
    expect_output(&f, 0, want, 4, 4 * FRAME, "frame 18: the speech of frames 17 and 1 only");
    const tetra_voice_t* v = voice_of(&f);
    expect(v->frames == 4 && v->concealed == 0 && v->bad_run == 0 && v->confirmed == 1,
           "frame 18: nothing decoded or concealed for it, and the speech goes on");

    /* Direct mode and the uplink run no TDMA clock of a trunked downlink: their bursts are speech whatever a stale
     * frame number says. */
    ts->dmo = 1;
    ts->dmo_crypt = TETRA_DMO_CRYPT_CLEAR;
    ts->fn = 18;
    expect(played_by_burst(&f) == 2 * FRAME, "frame 18: direct mode speech plays");
    fixture_close(&f);

    expect(fixture_open(&f) == 0, "frame 18: uplink fixture");
    f.opts->tetra_uplink = 1;
    tetra_st(f.state)->uplink = 1;
    tetra_st(f.state)->fn = 18;
    expect(played_by_burst(&f) == 2 * FRAME, "frame 18: uplink speech plays");
    fixture_close(&f);
}

/* The crypto class canonical slot 0 shows is the one the speech it carries has now, which can change with no PDU of
 * the call: the followed air-encrypted call is pending while no key is loaded and decryptable once the operator's
 * cipher key is, and anonymous speech, which its timeslot's signalling classifies, turns encrypted with it. */
static void
test_speech_reclassifies_the_call(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "reclassify: fixture");
    air_encrypted_call(&f, 0);
    follow_group(&f, 1234, 0x08);
    tetra_calls_find(f.state, 9)->air_enc = TETRA_ENC_MODE_EVEN;
    expect(played_by_burst(&f) == 0, "reclassify: no key, muted");
    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
           "reclassify: the call is pending");
    const uint64_t epoch = call.epoch;
    f.state->tetra_air_keys[0].loaded = 1;
    expect(played_by_burst(&f) == 2 * FRAME, "reclassify: the loaded key decrypts it");
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.epoch == epoch
               && call.crypto == DSD_CALL_CRYPTO_DECRYPTABLE && call.audio_permitted,
           "reclassify: and the call is decryptable");
    fixture_close(&f);

    expect(fixture_open(&f) == 0, "reclassify: fixture");
    tetra_confirm_note_signalling(f.state);
    expect(played_by_burst(&f) == 2 * FRAME, "reclassify: clear anonymous speech");
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               && call.crypto == DSD_CALL_CRYPTO_CLEAR,
           "reclassify: an anonymous clear call");
    tetra_st(f.state)->calls.slot_enc[1] = 1u + TETRA_ENC_MODE_ODD;
    expect(played_by_burst(&f) == 0, "reclassify: its timeslot's signalling turns encrypted");
    expect(dsd_call_state_get(f.state, 0U, &call) > 0 && call.phase == DSD_CALL_PHASE_ACTIVE
               && call.crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING,
           "reclassify: and so does the anonymous call");
    fixture_close(&f);
}

int
main(void) {
    test_clean_stream_plays_every_frame();
    test_integrity_starts_conceals_and_ends_speech();
    test_half_slot_and_carry();
    test_follow_gate();
    test_encryption_gate();
    test_air_key_stream_positions();
    test_talkgroup_gate();
    test_record_gate();
    test_call_boundary_resets_speech();
    test_talker_change_resets_speech();
    test_grant_after_cease_resets_speech();
    test_private_call_judged_by_both_parties();
    test_network_change_resets_speech();
    test_call_identifier_change_resets_speech();
    test_muted_call_holds_no_timeslot();
    test_timeslot_change_resets_speech();
    test_speech_reclassifies_the_call();
    test_speech_gap_resets_speech();
    test_followed_speech_gap_resets_speech();
    test_frame_18_is_no_speech();
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_voice_pipeline");
    return 0;
}
