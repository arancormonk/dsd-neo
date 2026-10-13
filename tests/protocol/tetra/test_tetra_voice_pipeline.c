// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * TETRA speech through the codec library to the digital voice output. The test codec (tetra_test_codec.c) is loaded
 * through --tetra-codec as a user's would be; its samples say which decode made them and whether it was a bad frame,
 * and the test reads them back from the raw output (-o -, here a temp file) to check what played, in what order:
 * speech starts at a CRC-passed burst, bad frames after it are concealed until a run of them ends it, a stolen half
 * slot keeps the frame timing, 240-sample frames leave the output whole 160-sample blocks, and the channel, timeslot,
 * encryption, talkgroup and record gates hold back what they should.
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
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_codec_abi.h>
#include <dsd-neo/protocol/tetra/tetra_dmo.h>
#include <dsd-neo/protocol/tetra/tetra_fec.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_voice.h>
#include <sndfile.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "test_support.h"
#include "tetra_state.h"
#include "tetra_test_codec.h"
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
        {1, 0, s1}, {2, 0, s1 + DSD_TETRA_CODEC_FRAME_BITS}, {3, 0, s2}, {4, 0, s2 + DSD_TETRA_CODEC_FRAME_BITS}};
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

    const played_frame want[] = {{1, 0, a},    {2, 0, b},    {3, 1, NULL}, {4, 1, NULL},
                                 {5, 1, NULL}, {6, 1, NULL}, {7, 0, a},    {8, 0, b}};
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
    const played_frame want[] = {{1, 0, half_frame},
                                 {2, 1, NULL},
                                 {3, 0, half_frame},
                                 {4, 0, full_frames},
                                 {5, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS}};
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

static void
test_channel_and_slot_gates(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "gates: fixture");
    f.opts->trunk_enable = 1;
    f.opts->trunk_is_tuned = 0;
    expect(played_by_burst(&f) == 0, "gates: nothing plays on the control channel");
    expect(f.state->last_vc_sync_time == 0, "gates: control-channel bursts are not speech reception");
    f.opts->trunk_is_tuned = 1;
    expect(played_by_burst(&f) == 2 * FRAME, "gates: the traffic channel plays");

    tetra_state_t* ts = tetra_st(f.state);
    ts->vc_assignment_valid = 1;
    ts->vc_timeslot_bitmap = 8; /* TN1 */
    ts->tdma_valid = 1;
    ts->tn = 2;
    expect(played_by_burst(&f) == 0, "gates: another timeslot does not play");
    ts->tn = 1;
    expect(played_by_burst(&f) == 2 * FRAME, "gates: the allocated timeslot plays");
    fixture_close(&f);
}

static const uint8_t k_air_key[10] = {0x1F, 0x2E, 0x3D, 0x4C, 0x5B, 0x6A, 0x79, 0x88, 0x97, 0xA6};

/* An air-encrypted trunked call on network 262/1001 with TEA1, its clock at TN1 FN3 MN7 of hyperframe 5; the operator
   key is loaded when @p key_loaded. */
static void
air_encrypted_call(fixture* f, int key_loaded) {
    tetra_state_t* ts = tetra_st(f->state);
    ts->enc_mode = TETRA_ENC_MODE_ON;
    ts->net_known = 1;
    ts->mcc = 262;
    ts->mnc = 1001;
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
    ts->enc_mode = TETRA_ENC_MODE_NONE;
    expect(played_by_burst(&f) == 0, "crypto: reverse mute mutes clear speech");
    f.opts->reverse_mute = 0;

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
   call air_encrypted_call() sets up. */
static void
expected_clear(const fixture* f, const uint8_t* plain, int count, int kss, uint8_t* out) {
    const tetra_state_t* ts = tetra_st(f->state);
    uint8_t ks[64];
    const uint32_t iv = tetra_air_build_iv(ts->tn, ts->fn, ts->mn, ts->hyperframe, 0);
    tetra_tea_keystream(0, k_air_key, iv, ks, (kss + count + 7) / 8);
    for (int i = 0; i < count; i++) {
        const int bit = kss + i;
        out[i] = (uint8_t)((plain[i] ^ (ks[bit / 8] >> (7 - (bit % 8)))) & 1u);
    }
}

/* A full slot's speech takes key stream bits 0 to 273, the speech half of an STCH + TCH/S burst bits 216 to 352. */
static void
test_air_key_stream_positions(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "key stream: fixture");
    air_encrypted_call(&f, 1);
    uint8_t full[TETRA_SPEECH_FULL_BITS];
    uint8_t half[TETRA_SPEECH_HALF_BITS];
    uint8_t clear_full[TETRA_SPEECH_FULL_BITS];
    uint8_t clear_half[TETRA_SPEECH_HALF_BITS];
    uint8_t full_frames[2 * DSD_TETRA_CODEC_FRAME_BITS];
    uint8_t half_frame[DSD_TETRA_CODEC_FRAME_BITS];
    burst_bits(full, TETRA_SPEECH_FULL_BITS, 7U);
    burst_bits(half, TETRA_SPEECH_HALF_BITS, 9U);
    expected_clear(&f, full, TETRA_SPEECH_FULL_BITS, 0, clear_full);
    expected_clear(&f, half, TETRA_SPEECH_HALF_BITS, 216, clear_half);
    tetra_voice_reorder_full(clear_full, full_frames);
    tetra_voice_reorder_half(clear_half, half_frame);

    tetra_voice_full_slot(f.opts, f.state, full, 1);
    tetra_voice_half_slot(f.opts, f.state, half, 1);
    const played_frame want[] = {
        {1, 0, full_frames}, {2, 0, full_frames + DSD_TETRA_CODEC_FRAME_BITS}, {3, 1, NULL}, {4, 0, half_frame}};
    expect_output(&f, 0, want, 4, 4 * FRAME, "key stream positions");
    fixture_close(&f);
}

static void
test_talkgroup_gate(void) {
    fixture f;
    expect(fixture_open(&f) == 0, "talkgroup: fixture");
    tetra_state_t* ts = tetra_st(f.state);
    ts->gssi = 1234;
    expect(dsd_tg_policy_session_avoid_add(f.state, 1234) == 0, "talkgroup: avoid 1234");
    expect(played_by_burst(&f) == 0, "talkgroup: an avoided group is muted");
    ts->gssi = 1235;
    expect(played_by_burst(&f) == 2 * FRAME, "talkgroup: another group plays");
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
    tetra_st(f.state)->gssi = 500;

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

int
main(void) {
    test_clean_stream_plays_every_frame();
    test_integrity_starts_conceals_and_ends_speech();
    test_half_slot_and_carry();
    test_channel_and_slot_gates();
    test_encryption_gate();
    test_air_key_stream_positions();
    test_talkgroup_gate();
    test_record_gate();
    if (g_fail) {
        DSD_FPRINTF(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("PASS test_tetra_voice_pipeline");
    return 0;
}
