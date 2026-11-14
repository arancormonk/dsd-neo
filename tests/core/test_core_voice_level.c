// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The digital voice chain's speech level (issue #518): what teaches the slot's speech AGC, where it starts, and which
 * reception boundaries start it over. Speech-like material comes from the deterministic generators
 * (test_support/speech_synth.h); a probe is a DC frame no estimate may learn from, which plays at exactly the gain the
 * AGC holds.
 */

#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/audio/voice_chain_internal.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"
#include "speech_synth.h"

#define FRAME       DSD_VOICE_FRAME_SAMPLES
#define RATE_HZ     8000
/* Three seconds of material: 150 frames. */
#define TEACH_N     ((size_t)3 * RATE_HZ)
/* A constant probe level the top of the AGC's range (+30 dB) still keeps under the -1 dBFS limiter. */
#define PROBE_LEVEL 500.0f
/* The start gain, the most a tone frame plays at, dB and linear. */
#define START_DB    ((double)DSD_VOICE_LEVEL_START_GAIN_DB)
#define START_LIN   pow(10.0, START_DB / 20.0)
/* The opening gain every reception from a talker not remembered plays at until it learns, dB and linear. */
#define OPEN_DB     ((double)DSD_VOICE_LEVEL_OPEN_GAIN_DB)
#define OPEN_LIN    pow(10.0, OPEN_DB / 20.0)

static int
expect_true(const char* tag, int cond) {
    if (!cond) {
        DSD_FPRINTF(stderr, "FAIL %s\n", tag);
        return 1;
    }
    return 0;
}

static int
expect_near(const char* tag, double got, double want, double tol) {
    if (!(fabs(got - want) <= tol)) {
        DSD_FPRINTF(stderr, "FAIL %s: got %.6f want %.6f (tol %.6f)\n", tag, got, want, tol);
        return 1;
    }
    return 0;
}

/* Auto gain (-g 0), the high-pass off, on a zeroed decoder state. */
static dsd_opts*
new_opts(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    if (opts) {
        opts->audio_gain = 0.0f;
        opts->audio_gainR = 0.0f;
        opts->use_hpf_d = 0;
    }
    return opts;
}

static dsd_state*
new_state(void) {
    return calloc(1, sizeof(dsd_state));
}

static void
free_state(dsd_state* state) {
    if (state) {
        dsd_state_ext_free_all(state);
        free(state);
    }
}

static dsd_voice_frame_info
frame_info(dsd_voice_media media, int decodable, int admitted) {
    const dsd_voice_frame_info info = {(uint8_t)media, (uint8_t)(decodable != 0), (uint8_t)(admitted != 0), 0U};
    return info;
}

/* Run @p n samples of @p material through slot @p slot's chain, frame by frame, all with @p info. */
static void
run_material(const dsd_opts* opts, dsd_state* state, int slot, const float* material, size_t n,
             const dsd_voice_frame_info* info) {
    float frame[FRAME];
    for (size_t off = 0; off + FRAME <= n; off += FRAME) {
        DSD_MEMCPY(frame, material + off, sizeof(frame));
        dsd_voice_chain_run(opts, state, slot, frame, info);
    }
}

/* A DC frame through the slot's chain as concealment (nothing learns from it): the gain the slot holds, linear, as
   the frame's last sample reads it (after any ramp a finished frame set). */
static float
probe_gain(const dsd_opts* opts, dsd_state* state, int slot) {
    float frame[FRAME];
    for (int n = 0; n < FRAME; n++) {
        frame[n] = PROBE_LEVEL;
    }
    const dsd_voice_frame_info info = frame_info(DSD_VOICE_MEDIA_CONCEALED, 1, 1);
    dsd_voice_chain_run(opts, state, slot, frame, &info);
    return frame[FRAME - 1] / PROBE_LEVEL;
}

/* Quiet running speech: a talker 21 dB under the target, which a learning AGC lifts. */
static int
quiet_speech(float* out, size_t n, uint64_t seed) {
    dsd_test_speech_params p;
    dsd_test_speech_defaults(&p, seed);
    p.level_dbfs = -45.0;
    return dsd_test_synth_speech(out, n, RATE_HZ, &p);
}

static double
db_of(float linear) {
    return 20.0 * log10((double)linear);
}

/* B3: only decoded, decodable, admitted speech teaches the level. The same quiet speech that lifts the gain as speech
   leaves it at the opening gain (DSD_VOICE_LEVEL_OPEN_GAIN_DB over the -g 1 level) as undecryptable garbage, repeats,
   frames the receive policy refuses, or tone frames; zeros of failed decodes, comfort noise, AMBE silence, near-silent
   decoded speech and loud ciphertext leave it there too. */
static int
test_frames_that_may_not_learn_do_not_teach(void) {
    float* speech = calloc(TEACH_N, sizeof(float));
    float* other = calloc(TEACH_N, sizeof(float));
    if (!speech || !other || quiet_speech(speech, TEACH_N, 7U) != 0) {
        free(speech);
        free(other);
        return 1;
    }
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(speech);
        free(other);
        free(opts);
        free_state(state);
        return 1;
    }
    const dsd_voice_frame_info learn = frame_info(DSD_VOICE_MEDIA_SPEECH, 1, 1);
    run_material(opts, state, 0, speech, TEACH_N, &learn);
    const float taught = probe_gain(opts, state, 0);
    rc |= expect_true("decoded speech teaches: quiet speech is lifted at least 6 dB", db_of(taught) >= OPEN_DB + 6.0);
    free_state(state);

    static const struct {
        const char* tag;
        dsd_voice_media media;
        int decodable;
        int admitted;
    } speech_cases[] = {
        {"undecryptable speech does not teach", DSD_VOICE_MEDIA_SPEECH, 0, 1},
        {"refused speech does not teach", DSD_VOICE_MEDIA_SPEECH, 1, 0},
        {"repeats do not teach", DSD_VOICE_MEDIA_CONCEALED, 1, 1},
        {"tone frames do not teach", DSD_VOICE_MEDIA_TONE, 1, 1},
        {"frames with no media do not teach", DSD_VOICE_MEDIA_NONE, 1, 1},
    };

    for (size_t i = 0; i < sizeof(speech_cases) / sizeof(speech_cases[0]); i++) {
        state = new_state();
        if (!state) {
            rc = 1;
            break;
        }
        const dsd_voice_frame_info info =
            frame_info(speech_cases[i].media, speech_cases[i].decodable, speech_cases[i].admitted);
        run_material(opts, state, 0, speech, TEACH_N, &info);
        rc |= expect_near(speech_cases[i].tag, db_of(probe_gain(opts, state, 0)), OPEN_DB, 1e-4);
        free_state(state);
    }

    static const struct {
        const char* tag;
        dsd_voice_media media;
        int decodable;
        int kind; /* 0 zeros, 1 comfort noise, 2 near silence, 3 loud ciphertext */
    } noise_cases[] = {
        {"zeros of failed decodes do not teach", DSD_VOICE_MEDIA_NONE, 1, 0},
        {"AMBE silence does not teach", DSD_VOICE_MEDIA_CONCEALED, 1, 0},
        {"comfort noise does not teach", DSD_VOICE_MEDIA_CONCEALED, 1, 1},
        {"near-silent decoded speech is not lifted", DSD_VOICE_MEDIA_SPEECH, 1, 2},
        {"loud ciphertext does not teach", DSD_VOICE_MEDIA_SPEECH, 0, 3},
    };

    for (size_t i = 0; i < sizeof(noise_cases) / sizeof(noise_cases[0]); i++) {
        state = new_state();
        if (!state) {
            rc = 1;
            break;
        }
        int ok = 0;
        switch (noise_cases[i].kind) {
            case 0: DSD_MEMSET(other, 0, TEACH_N * sizeof(float)); break;
            case 1: ok = dsd_test_synth_noise(other, TEACH_N, RATE_HZ, 11U, DSD_TEST_NOISE_WHITE, -55.0); break;
            case 2: ok = dsd_test_synth_noise(other, TEACH_N, RATE_HZ, 12U, DSD_TEST_NOISE_WHITE, -90.0); break;
            default: ok = dsd_test_synth_noise(other, TEACH_N, RATE_HZ, 13U, DSD_TEST_NOISE_VOICE_BAND, -12.0); break;
        }
        rc |= expect_true("noise material", ok == 0);
        const dsd_voice_frame_info info = frame_info(noise_cases[i].media, noise_cases[i].decodable, 1);
        run_material(opts, state, 0, other, TEACH_N, &info);
        rc |= expect_near(noise_cases[i].tag, db_of(probe_gain(opts, state, 0)), OPEN_DB, 1e-4);
        free_state(state);
    }
    free(opts);
    free(speech);
    free(other);
    return rc;
}

/* B4: auto gain opens at one level, the opening gain (DSD_VOICE_LEVEL_OPEN_GAIN_DB over the -g 1 level), whatever state
   the decoder came up in: a zeroed state, or one initState() made for any entry path (live, -r, M17). */
static int
test_auto_gain_starts_at_the_reference_level(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* zeroed = new_state();
    dsd_state* initialised = new_state();
    if (!opts || !zeroed || !initialised) {
        free(opts);
        free_state(zeroed);
        free_state(initialised);
        return 1;
    }
    initState(initialised);
    rc |= expect_near("a zeroed state plays its first frame at the opening gain", db_of(probe_gain(opts, zeroed, 0)),
                      OPEN_DB, 1e-4);
    rc |= expect_near("slot 2 too", db_of(probe_gain(opts, zeroed, 1)), OPEN_DB, 1e-4);
    rc |= expect_near("an initialised state plays its first frame at the opening gain",
                      db_of(probe_gain(opts, initialised, 0)), OPEN_DB, 1e-4);
    rc |= expect_near("its slot 2 too", db_of(probe_gain(opts, initialised, 1)), OPEN_DB, 1e-4);
    freeState(initialised);
    free(initialised);
    free_state(zeroed);
    free(opts);
    return rc;
}

/* D12: in auto mode a tone frame plays at no more than the start gain from its first sample, teaches nothing, and the
   gain ramps back to the learned one over one frame after it. */
static int
test_tone_frames_play_at_most_the_start_gain(void) {
    float* speech = calloc(TEACH_N, sizeof(float));
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    dsd_state* control = new_state();
    if (!speech || !opts || !state || !control || quiet_speech(speech, TEACH_N, 21U) != 0) {
        free(speech);
        free(opts);
        free_state(state);
        free_state(control);
        return 1;
    }
    int rc = 0;
    const dsd_voice_frame_info learn = frame_info(DSD_VOICE_MEDIA_SPEECH, 1, 1);
    run_material(opts, state, 0, speech, TEACH_N, &learn);
    run_material(opts, control, 0, speech, TEACH_N, &learn);
    const float learned = probe_gain(opts, control, 0);
    rc |= expect_true("the speech lifted the gain", db_of(learned) >= OPEN_DB + 6.0);
    (void)probe_gain(opts, state, 0);

    const dsd_voice_frame_info tone = frame_info(DSD_VOICE_MEDIA_TONE, 1, 1);
    float frame[FRAME];
    for (int k = 0; k < 5; k++) {
        for (int n = 0; n < FRAME; n++) {
            frame[n] = PROBE_LEVEL;
        }
        dsd_voice_chain_run(opts, state, 0, frame, &tone);
        float worst = 0.0f;
        for (int n = 0; n < FRAME; n++) {
            worst = frame[n] > worst ? frame[n] : worst;
        }
        rc |= expect_true("a tone frame plays at no more than the start gain from its first sample",
                          worst <= (float)(PROBE_LEVEL * START_LIN) + 1e-3f);
    }
    /* Back over one frame: the first frame after the tone ramps, the next plays at the learned gain. */
    const float ramp = probe_gain(opts, state, 0);
    rc |=
        expect_near("the gain is back at the learned one one frame after the tone", db_of(ramp), db_of(learned), 0.01);
    rc |= expect_near("the tone left the learned gain as it was", db_of(probe_gain(opts, state, 0)),
                      db_of(probe_gain(opts, control, 0)), 0.01);
    free_state(state);
    free_state(control);
    free(opts);
    free(speech);
    return rc;
}

/* A DMR group call to @p target from radio @p source on @p slot. */
static int
observe_call_from(dsd_state* state, uint8_t slot, uint32_t target, uint32_t source, double observed_m) {
    dsd_call_observation call = dsd_call_observation_data(DSD_SYNC_DMR_BS_VOICE_POS, slot, source, target);
    call.kind = DSD_CALL_KIND_GROUP_VOICE;
    call.policy_target_id = target;
    call.observed_m = observed_m;
    return dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN);
}

/* A DMR group call to @p target, each target's from a radio of its own, so the slot remembers no talker across the
   calls of a test about something else. */
static int
observe_call(dsd_state* state, uint8_t slot, uint32_t target, double observed_m) {
    return observe_call_from(state, slot, target, 0x1000U + target, observed_m);
}

/* B18: the high-pass follows the transmission, not the raw call epoch: a fade the call state heals (a sync loss
   reacquired within DSD_CALL_REACQUIRE_GAP_S, which mints a new epoch of the same transmission) keeps its state. */
static int
test_high_pass_keeps_its_state_across_a_healed_fade(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    opts->use_hpf_d = 1;
    opts->audio_gain = 1.0f;
    float frame[FRAME];
    rc |= expect_true("call observed", observe_call(state, 0U, 100U, 10.0) > 0);
    dsd_call_snapshot before;
    rc |= expect_true("call active", dsd_call_state_get(state, 0U, &before) > 0);
    for (int k = 0; k < 3; k++) {
        for (int n = 0; n < FRAME; n++) {
            frame[n] = 1000.0f;
        }
        dsd_voice_chain_run(opts, state, 0, frame, NULL);
    }
    rc |= expect_true("faded", dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) > 0);
    rc |= expect_true("reacquired", observe_call(state, 0U, 100U, 10.4) > 0);
    dsd_call_snapshot after;
    rc |= expect_true("healed call active", dsd_call_state_get(state, 0U, &after) > 0);
    rc |= expect_true("the heal is a new epoch of the same transmission",
                      after.epoch != before.epoch
                          && dsd_call_snapshot_transmission(&after) == dsd_call_snapshot_transmission(&before));
    DSD_MEMSET(frame, 0, sizeof(frame));
    dsd_voice_chain_run(opts, state, 0, frame, NULL);
    int carried = 0;
    for (int n = 0; n < FRAME; n++) {
        carried |= fabsf(frame[n]) > 1e-3f;
    }
    rc |= expect_true("a healed fade keeps the high-pass running", carried);
    free_state(state);
    free(opts);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------------------
 * The reception the slot's speech AGC runs for (spec: the tracker and the reset/preserve matrix).
 * ------------------------------------------------------------------------------------------------------------- */

static const dsd_speech_agc*
agc_of(const dsd_state* state, int slot) {
    return dsd_voice_level_agc(state, slot);
}

/* The slot learned: committed speech and a gain off the opening gain. */
static int
slot_learned(const dsd_state* state, int slot) {
    const dsd_speech_agc* agc = agc_of(state, slot);
    return agc != NULL && agc->learned.commits > 0 && agc->learned.gain_db > DSD_VOICE_LEVEL_OPEN_GAIN_DB + 3.0f;
}

/* The slot's AGC was reset and has run no frame since: the start gain with nothing learned. */
static int
slot_reset(const dsd_state* state, int slot) {
    const dsd_speech_agc* agc = agc_of(state, slot);
    return agc != NULL && agc->learned.commits == 0 && agc->learned.steps == 0
           && fabsf(agc->learned.gain_db - DSD_VOICE_LEVEL_START_GAIN_DB) < 1e-6f;
}

/* The slot opened a new reception: at the opening gain with nothing learned. */
static int
slot_fresh(const dsd_state* state, int slot) {
    const dsd_speech_agc* agc = agc_of(state, slot);
    return agc != NULL && agc->learned.commits == 0 && agc->learned.steps == 0
           && fabsf(agc->learned.gain_db - DSD_VOICE_LEVEL_OPEN_GAIN_DB) < 1e-6f;
}

/* The slot's estimate and gain are exactly what @p before held (frames since then only moved its clocks). */
static int
slot_kept(const dsd_state* state, int slot, const dsd_speech_agc_learned* before) {
    const dsd_speech_agc* agc = agc_of(state, slot);
    return agc != NULL && agc->learned.commits == before->commits && agc->learned.steps == before->steps
           && agc->learned.commits > 0 && fabsf(agc->learned.power - before->power) <= 1e-12f
           && fabsf(agc->learned.gain_db - before->gain_db) <= 1e-6f
           && fabsf(agc->learned.noise_floor_db - before->noise_floor_db) <= 1e-6f;
}

/* The largest magnitude in a frame. */
static float
frame_peak(const float* frame) {
    float peak = 0.0f;
    for (int n = 0; n < FRAME; n++) {
        peak = fabsf(frame[n]) > peak ? fabsf(frame[n]) : peak;
    }
    return peak;
}

/* A slot's first frame plays at the opening gain from its first sample, with the acquisition stage on for both slots;
   dsd_voice_level_reset() leaves the opening gain in the readouts; a tone frame that opens a reception still plays at
   no more than the start gain. */
static int
test_a_reception_opens_at_the_opening_gain(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    float frame[FRAME];
    const dsd_voice_frame_info concealed = frame_info(DSD_VOICE_MEDIA_CONCEALED, 1, 1);
    for (int slot = 0; slot < 2; slot++) {
        for (int n = 0; n < FRAME; n++) {
            frame[n] = 100.0f;
        }
        dsd_voice_chain_run(opts, state, slot, frame, &concealed);
        rc |= expect_near("open: the first sample plays at the opening gain", frame[0], 100.0 * OPEN_LIN, 1e-3);
        rc |= expect_near("open: the whole first frame does", frame[FRAME - 1], 100.0 * OPEN_LIN, 1e-3);
        rc |= expect_true("open: the slot opened the reception", slot_fresh(state, slot));
        rc |= expect_true("open: the slot's AGC has the acquisition stage on", agc_of(state, slot)->acquisition == 1);
    }
    rc |= expect_near("open: the readout shows the opening gain", state->aout_gain, OPEN_DB, 1e-4);
    rc |= expect_near("open: slot 2's readout too", state->aout_gainR, OPEN_DB, 1e-4);

    /* The readouts after a reset show the gain the next frame opens at. */
    state->aout_gain = -50.0f;
    state->aout_gainR = -50.0f;
    dsd_voice_level_reset(state, 1);
    rc |= expect_near("open: one slot's reset leaves the other's readout", state->aout_gain, -50.0, 1e-6);
    rc |= expect_near("open: a reset shows the opening gain", state->aout_gainR, OPEN_DB, 1e-6);
    dsd_voice_level_reset(state, -1);
    rc |= expect_near("open: both slots' resets show it", state->aout_gain, OPEN_DB, 1e-6);
    rc |= expect_near("open: on slot 2 too", state->aout_gainR, OPEN_DB, 1e-6);

    /* A tone frame on a fresh reception: capped at the start gain, which stays the tone cap. */
    const dsd_voice_frame_info tone = frame_info(DSD_VOICE_MEDIA_TONE, 1, 1);
    for (int k = 0; k < 3; k++) {
        for (int n = 0; n < FRAME; n++) {
            frame[n] = PROBE_LEVEL;
        }
        dsd_voice_chain_run(opts, state, 0, frame, &tone);
        rc |= expect_true("open: a tone frame plays at no more than the start gain",
                          frame_peak(frame) <= (float)(PROBE_LEVEL * START_LIN) + 1e-3f);
    }
    rc |= expect_near("open: the tone left the opening gain", agc_of(state, 0)->learned.gain_db, OPEN_DB, 1e-6);
    free_state(state);
    free(opts);
    return rc;
}

/* One frame the AGC cannot learn from, on @p slot with @p talkspurt: the tracker sees its reception. */
static void
touch_slot(const dsd_opts* opts, dsd_state* state, int slot, uint32_t talkspurt) {
    float frame[FRAME];
    DSD_MEMSET(frame, 0, sizeof(frame));
    dsd_voice_frame_info info = frame_info(DSD_VOICE_MEDIA_CONCEALED, 1, 1);
    info.talkspurt = talkspurt;
    dsd_voice_chain_run(opts, state, slot, frame, &info);
}

/* Teach @p slot the quiet speech, every frame with @p talkspurt. */
static void
teach_slot(const dsd_opts* opts, dsd_state* state, int slot, const float* speech, uint32_t talkspurt) {
    dsd_voice_frame_info info = frame_info(DSD_VOICE_MEDIA_SPEECH, 1, 1);
    info.talkspurt = talkspurt;
    run_material(opts, state, slot, speech, TEACH_N, &info);
}

/* A new transmission on the slot starts its AGC over; a fade the call state heals (a new epoch of the same
   transmission) and the identity of a late-entry call arriving do not, nor does the other slot's call. A frame of
   another protocol family starts it over too. */
static int
test_transmission_and_family_decide_the_reception(const float* speech) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    /* Frames before the call is known, then the call named partway: one reception. */
    teach_slot(opts, state, 0, speech, 0U);
    rc |= expect_true("tracker: the slot learned before its call was known", slot_learned(state, 0));
    dsd_speech_agc_learned before = agc_of(state, 0)->learned;
    rc |= expect_true("tracker: call observed", observe_call(state, 0U, 100U, 10.0) > 0);
    touch_slot(opts, state, 0, 0U);
    rc |= expect_true("tracker: a call known partway keeps the level", slot_kept(state, 0, &before));

    /* The other slot's new call is not this slot's reception. */
    teach_slot(opts, state, 1, speech, 0U);
    before = agc_of(state, 0)->learned;
    rc |= expect_true("tracker: slot 2's call observed", observe_call(state, 1U, 300U, 10.1) > 0);
    touch_slot(opts, state, 1, 0U);
    touch_slot(opts, state, 0, 0U);
    rc |= expect_true("tracker: the other slot's call leaves the slot's level", slot_kept(state, 0, &before));

    /* A healed fade: a new raw epoch of the same transmission. */
    before = agc_of(state, 0)->learned;
    rc |= expect_true("tracker: faded", dsd_call_state_end_ex(state, 0U, 10.2, DSD_CALL_END_SYNC_LOSS) > 0);
    rc |= expect_true("tracker: reacquired", observe_call(state, 0U, 100U, 10.4) > 0);
    touch_slot(opts, state, 0, 0U);
    rc |= expect_true("tracker: a healed fade keeps the level", slot_kept(state, 0, &before));

    /* The next transmission. */
    rc |= expect_true("tracker: call ended", dsd_call_state_end(state, 0U, 11.0) > 0);
    rc |= expect_true("tracker: next call observed", observe_call(state, 0U, 200U, 12.0) > 0);
    touch_slot(opts, state, 0, 0U);
    rc |= expect_true("tracker: a new transmission starts the slot over", slot_fresh(state, 0));
    rc |= expect_true("tracker: and not the other slot", slot_learned(state, 1));

    /* Another protocol family, with no call known. */
    dsd_state* other = new_state();
    if (!other) {
        free_state(state);
        free(opts);
        return 1;
    }
    other->synctype = DSD_SYNC_NXDN_POS;
    teach_slot(opts, other, 0, speech, 0U);
    rc |= expect_true("tracker: nxdn learned", slot_learned(other, 0));
    other->synctype = DSD_SYNC_DSTAR_VOICE_POS;
    touch_slot(opts, other, 0, 0U);
    rc |= expect_true("tracker: another protocol family starts the slot over", slot_fresh(other, 0));
    free_state(other);
    free_state(state);
    free(opts);
    return rc;
}

/* The producer's own transmission count (TETRA's) starts the AGC over when it moves from one count to another; a
   producer that names none (0), before or after a count, never cuts a reception. */
static int
test_talkspurt_decides_the_reception(const float* speech) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    teach_slot(opts, state, 0, speech, 0U);
    dsd_speech_agc_learned before = agc_of(state, 0)->learned;
    touch_slot(opts, state, 0, 7U);
    rc |= expect_true("talkspurt: the first count names the running reception", slot_kept(state, 0, &before));
    teach_slot(opts, state, 0, speech, 7U);
    before = agc_of(state, 0)->learned;
    touch_slot(opts, state, 0, 0U);
    touch_slot(opts, state, 0, 7U);
    rc |= expect_true("talkspurt: frames that name none keep it", slot_kept(state, 0, &before));
    touch_slot(opts, state, 0, 8U);
    rc |= expect_true("talkspurt: the next count starts the slot over", slot_fresh(state, 0));
    free_state(state);
    free(opts);
    return rc;
}

/* dsd_voice_level_reset() starts one slot over, or both; the crypto purge of a slot (a mid-call classification) and a
   rate change of the input (init_audio_filters()) keep the level. */
static int
test_explicit_resets_and_what_keeps_the_level(const float* speech) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    dsd_voice_level_reset(state, -1);
    rc |= expect_true("explicit: a reset before any frame is harmless", agc_of(state, 0) == NULL);
    teach_slot(opts, state, 0, speech, 0U);
    teach_slot(opts, state, 1, speech, 0U);
    dsd_speech_agc_learned before[2] = {agc_of(state, 0)->learned, agc_of(state, 1)->learned};
    dsd_mbe_purge_slot_audio(state, 0);
    dsd_mbe_purge_slot_audio(state, 1);
    init_audio_filters(state, 48000);
    rc |= expect_true("explicit: purges and a rate change keep the level",
                      slot_kept(state, 0, &before[0]) && slot_kept(state, 1, &before[1]));
    dsd_voice_level_reset(state, 1);
    rc |= expect_true("explicit: one slot starts over", slot_reset(state, 1) && slot_kept(state, 0, &before[0]));
    dsd_voice_level_reset(state, -1);
    rc |= expect_true("explicit: both start over", slot_reset(state, 0) && slot_reset(state, 1));
    rc |= expect_near("explicit: the next frame plays at the opening gain", db_of(probe_gain(opts, state, 0)), OPEN_DB,
                      1e-4);
    rc |= expect_true("explicit: and opens the reception", slot_fresh(state, 0));
    free_state(state);
    free(opts);
    return rc;
}

/* Fixed and auto: a fixed -g is that gain from the next frame (the limiter still runs) and learns nothing; back to auto
   the slot keeps what it learned and starts from the fixed gain, within the AGC's range (+30 dB over the start). */
static int
test_fixed_and_auto_keep_the_estimate(const float* speech) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    teach_slot(opts, state, 0, speech, 0U);
    const dsd_speech_agc_learned learned = agc_of(state, 0)->learned;
    opts->audio_gain = 2.0f;
    rc |= expect_near("fixed: the next frame plays at the fixed gain", db_of(probe_gain(opts, state, 0)),
                      20.0 * log10(2.0), 1e-3);
    teach_slot(opts, state, 0, speech, 0U);
    rc |= expect_true("fixed: speech teaches nothing in fixed mode",
                      agc_of(state, 0)->learned.commits == learned.commits);
    opts->audio_gain = 0.0f;
    rc |= expect_near("auto again: starts from the fixed gain", db_of(probe_gain(opts, state, 0)), 20.0 * log10(2.0),
                      1e-3);
    rc |= expect_true("auto again: the estimate is kept",
                      agc_of(state, 0)->learned.commits == learned.commits && agc_of(state, 0)->learned.power > 0.0f);
    opts->audio_gain = 50.0f;
    touch_slot(opts, state, 0, 0U);
    opts->audio_gain = 0.0f;
    rc |= expect_near("auto again: x50 starts at it, within the range's top", db_of(probe_gain(opts, state, 0)),
                      fmin(20.0 * log10(50.0), START_DB + DSD_SPEECH_AGC_MAX_BOOST_DB), 1e-3);
    free_state(state);
    free(opts);
    return rc;
}

/* The first sample a DC frame of 100 plays at on @p slot, as concealment. */
static float
first_sample_of(const dsd_opts* opts, dsd_state* state, int slot) {
    float frame[FRAME];
    for (int n = 0; n < FRAME; n++) {
        frame[n] = 100.0f;
    }
    const dsd_voice_frame_info info = frame_info(DSD_VOICE_MEDIA_CONCEALED, 1, 1);
    dsd_voice_chain_run(opts, state, slot, frame, &info);
    return frame[0];
}

/* A new reception opens at the opening gain even when auto takes over from a fixed -g between receptions: the
   fixed->auto start applies only within the reception the fixed gain played in. Both ways a reception starts over: an
   explicit reset (a carrier boundary) and the tracker (the next transmission, seen on the frame auto takes over on). */
static int
test_a_new_reception_wins_over_the_fixed_gain(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    opts->audio_gain = 50.0f;
    rc |= expect_near("reset wins: the fixed call plays x50", first_sample_of(opts, state, 0), 5000.0, 1e-3);
    dsd_voice_level_reset(state, 0);
    opts->audio_gain = 0.0f;
    rc |= expect_near("reset wins: auto after a reset opens at the opening gain", first_sample_of(opts, state, 0),
                      100.0 * OPEN_LIN, 1e-3);

    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    rc |= expect_true("reset wins: call observed", observe_call(state, 0U, 100U, 10.0) > 0);
    opts->audio_gain = 50.0f;
    rc |= expect_near("reset wins: the fixed call plays x50 again", first_sample_of(opts, state, 0), 5000.0, 1e-3);
    rc |= expect_true("reset wins: call ended", dsd_call_state_end(state, 0U, 11.0) > 0);
    rc |= expect_true("reset wins: next call observed", observe_call(state, 0U, 200U, 12.0) > 0);
    opts->audio_gain = 0.0f;
    rc |= expect_near("reset wins: auto on the next transmission opens at the opening gain",
                      first_sample_of(opts, state, 0), 100.0 * OPEN_LIN, 1e-3);

    /* Within one reception the switch still starts from the fixed gain. */
    opts->audio_gain = 2.0f;
    rc |= expect_near("within: the fixed gain plays", first_sample_of(opts, state, 0), 200.0, 1e-3);
    opts->audio_gain = 0.0f;
    rc |= expect_near("within: auto starts from it", first_sample_of(opts, state, 0), 200.0, 1e-2);
    free_state(state);
    free(opts);
    return rc;
}

/* Decodability per slot: ciphertext past the ceiling drives the slot's limiter down; when the slot's frames turn
   decodable the limiter starts over at once (a clear frame plays at its gain), while the other slot's undecodable
   frames keep theirs, and decodable frames after a loud decodable one release at the limiter's own pace. */
static int
test_decodability_returns_per_slot(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    float frame[FRAME];
    const dsd_voice_frame_info cipher = frame_info(DSD_VOICE_MEDIA_SPEECH, 0, 1);
    const dsd_voice_frame_info clear = frame_info(DSD_VOICE_MEDIA_CONCEALED, 1, 1);
    for (int slot = 0; slot < 2; slot++) {
        for (int n = 0; n < FRAME; n++) {
            frame[n] = (n & 1) ? 32000.0f : -32000.0f;
        }
        dsd_voice_chain_run(opts, state, slot, frame, &cipher);
    }
    for (int n = 0; n < FRAME; n++) {
        frame[n] = PROBE_LEVEL;
    }
    dsd_voice_chain_run(opts, state, 0, frame, &clear);
    rc |= expect_near("decodable: slot 1 plays its first clear sample at its gain", frame[0], PROBE_LEVEL * OPEN_LIN,
                      1e-3);
    for (int n = 0; n < FRAME; n++) {
        frame[n] = PROBE_LEVEL;
    }
    dsd_voice_chain_run(opts, state, 1, frame, &cipher);
    rc |= expect_true("decodable: slot 2, still undecodable, keeps its limiter",
                      frame[0] < PROBE_LEVEL * OPEN_LIN * 0.95);

    /* The control: loud decodable audio, then quiet decodable audio, releases at 50 dB/s. */
    dsd_state* control = new_state();
    if (!control) {
        free_state(state);
        free(opts);
        return 1;
    }
    for (int n = 0; n < FRAME; n++) {
        frame[n] = (n & 1) ? 32000.0f : -32000.0f;
    }
    dsd_voice_chain_run(opts, control, 0, frame, &clear);
    for (int n = 0; n < FRAME; n++) {
        frame[n] = PROBE_LEVEL;
    }
    dsd_voice_chain_run(opts, control, 0, frame, &clear);
    rc |= expect_true("decodable: decodable audio after a loud frame releases gradually",
                      frame[0] < PROBE_LEVEL * OPEN_LIN * 0.95);
    free_state(control);
    free_state(state);
    free(opts);
    return rc;
}

/* Exact float equality, which the unity and untouched-state checks below mean: the same arithmetic on the same input
   gives the same bits. Written as a zero tolerance because the float-compare rule wants one. */
static int
float_same(float a, float b) {
    return fabsf(a - b) <= 0.0f;
}

/* @p a holds what @p b holds: what it learned, its gains and limiter, and how far its signal state has run. */
static int
agc_same(const dsd_speech_agc* a, const dsd_speech_agc* b) {
    const dsd_speech_agc_learned* la = &a->learned;
    const dsd_speech_agc_learned* lb = &b->learned;
    return a->frames == b->frames && a->frame_pos == b->frame_pos && a->decim_pos == b->decim_pos
           && a->cp_count == b->cp_count && a->fixed == b->fixed && la->commits == lb->commits && la->steps == lb->steps
           && la->run_len == lb->run_len && la->since_commit == lb->since_commit && float_same(la->power, lb->power)
           && float_same(la->gain_db, lb->gain_db) && float_same(la->atten_db, lb->atten_db)
           && float_same(la->noise_floor_db, lb->noise_floor_db) && float_same(a->g_cur, b->g_cur)
           && float_same(a->g_lim, b->g_lim) && float_same(a->frame_sumsq, b->frame_sumsq);
}

/* The M17 encoder's monitor (DSD_VOICE_MEDIA_TX_MONITOR, issue #518 B17): its frames play at unity under auto and
   under a fixed -g, through a limiter of their own at the ceiling, and the slot's AGC, taught or not, is left exactly
   as it was: it neither learns from them nor starts over for them, and their limiting does not reach its frames. */
static int
test_tx_monitor_plays_at_unity(const float* speech) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    teach_slot(opts, state, 0, speech, 0U);
    rc |= expect_true("tx monitor: the slot learned first", slot_learned(state, 0));
    dsd_speech_agc before;
    DSD_MEMCPY(&before, agc_of(state, 0), sizeof before);
    const dsd_voice_frame_info monitor = frame_info(DSD_VOICE_MEDIA_TX_MONITOR, 1, 1);
    float frame[FRAME];
    const float gains[2] = {0.0f, 3.0f};
    for (int g = 0; g < 2; g++) {
        opts->audio_gain = gains[g];
        int unity = 1;
        for (size_t off = 0; off + FRAME <= TEACH_N; off += FRAME) {
            DSD_MEMCPY(frame, speech + off, sizeof(frame));
            dsd_voice_chain_run(opts, state, 0, frame, &monitor);
            for (int n = 0; n < FRAME; n++) {
                unity &= float_same(frame[n], speech[off + (size_t)n]);
            }
        }
        rc |= expect_true(g == 0 ? "tx monitor: unity under auto" : "tx monitor: unity under -g 3", unity);
        rc |= expect_near("tx monitor: the readout shows unity", state->aout_gain, 0.0, 1e-6);
    }
    /* Full scale: held at the ceiling. */
    for (int n = 0; n < FRAME; n++) {
        frame[n] = (n & 1) ? 32767.0f : -32767.0f;
    }
    dsd_voice_chain_run(opts, state, 0, frame, &monitor);
    float peak = 0.0f;
    for (int n = 0; n < FRAME; n++) {
        peak = fabsf(frame[n]) > peak ? fabsf(frame[n]) : peak;
    }
    const double ceiling = 32768.0 * pow(10.0, DSD_SPEECH_AGC_CEILING_DBFS / 20.0);
    rc |= expect_true("tx monitor: limited at the ceiling", peak <= ceiling + 1.0 && frame[FRAME - 1] != 0.0f);
    rc |= expect_true("tx monitor: the slot's AGC is exactly as it was", agc_same(agc_of(state, 0), &before));
    /* The slot's own next frame plays at the gain it held, its limiter untouched by the monitor's. */
    opts->audio_gain = 0.0f;
    rc |= expect_near("tx monitor: the slot plays at its learned gain", db_of(probe_gain(opts, state, 0)),
                      before.learned.gain_db, 1e-3);
    free_state(state);
    free(opts);
    return rc;
}

/* ---------------------------------------------------------------------------------------------------------------
 * The talker memory: a reception from a radio heard before opens at the gain its speech last settled at.
 * ------------------------------------------------------------------------------------------------------------- */

/* Invented radio IDs. */
#define SRC_X   ((uint64_t)0x1A6)
#define SRC_Y   ((uint64_t)0x2B7)
#define SRC_Z   ((uint64_t)0x3C8)
/* A burst of speech too short to establish an estimate: some commits, fewer than 20. */
#define BRIEF_N ((size_t)RATE_HZ / 2)

/* The talkers the memory tests play: two levels whose settled gains are far apart, and a brief burst. */
typedef struct {
    float* low;   /* about 20 dB under the -g 1 reference (-44 dBFS P.56) */
    float* high;  /* -34 dBFS P.56 */
    float* brief; /* BRIEF_N samples of another talker at -44 dBFS P.56 */
} talkers;

static int
make_talker(float* out, size_t n, uint64_t seed, double level_dbfs) {
    dsd_test_speech_params p;
    dsd_test_speech_defaults(&p, seed);
    p.level_dbfs = level_dbfs;
    return dsd_test_synth_speech(out, n, RATE_HZ, &p);
}

static int
family_dmr(void) {
    return dsd_call_state_protocol_family(DSD_SYNC_DMR_BS_VOICE_POS);
}

static int
family_nxdn(void) {
    return dsd_call_state_protocol_family(DSD_SYNC_NXDN_POS);
}

/* A reception as the chain reads it from the call state. */
static dsd_voice_reception
reception(uint64_t transmission, int family, uint64_t source) {
    dsd_voice_reception r;
    DSD_MEMSET(&r, 0, sizeof r);
    r.transmission = transmission;
    r.family = family;
    r.source = source;
    return r;
}

/* @p n samples of @p material through slot @p slot's speech level as decoded speech of reception @p r. */
static void
level_speech(const dsd_opts* opts, dsd_state* state, int slot, const float* material, size_t n,
             const dsd_voice_reception* r) {
    float frame[FRAME];
    const dsd_voice_frame_info info = frame_info(DSD_VOICE_MEDIA_SPEECH, 1, 1);
    for (size_t off = 0; off + FRAME <= n; off += FRAME) {
        DSD_MEMCPY(frame, material + off, sizeof(frame));
        dsd_voice_level_run(opts, state, slot, frame, &info, r);
    }
}

/* A DC frame through slot @p slot's speech level as concealment of reception @p r: nothing learns from it, so it plays
   at the gain the slot holds, ramp included. */
static void
level_probe(const dsd_opts* opts, dsd_state* state, int slot, const dsd_voice_reception* r, float* frame) {
    for (int n = 0; n < FRAME; n++) {
        frame[n] = PROBE_LEVEL;
    }
    const dsd_voice_frame_info info = frame_info(DSD_VOICE_MEDIA_CONCEALED, 1, 1);
    dsd_voice_level_run(opts, state, slot, frame, &info, r);
}

/* The settled gain of slot @p slot's reception. */
static int
settled_at(const dsd_state* state, int slot, float* gain_db) {
    const dsd_speech_agc* agc = agc_of(state, slot);
    return agc != NULL && dsd_speech_agc_settled_gain_db(agc, gain_db) == 1;
}

/* Reception @p r's first frame, a probe, opens at exactly @p want_db (the controller gain, bit for bit) and plays at it
   from its first sample. */
static int
opens_at(const char* tag, const dsd_opts* opts, dsd_state* state, int slot, const dsd_voice_reception* r,
         float want_db) {
    float frame[FRAME];
    level_probe(opts, state, slot, r, frame);
    const dsd_speech_agc* agc = agc_of(state, slot);
    const float got = agc != NULL ? agc->learned.gain_db : -1000.0f;
    int rc = expect_near(tag, got, want_db, 0.0);
    rc |= expect_true(tag, agc != NULL && agc->learned.commits == 0);
    rc |= expect_near(tag, db_of(frame[0] / PROBE_LEVEL), want_db, 1e-4);
    rc |= expect_near(tag, db_of(frame[FRAME - 1] / PROBE_LEVEL), want_db, 1e-4);
    return rc;
}

/* A reception of @p material from @p r that establishes its estimate; its settled gain in @p gain_db. */
static int
settle(const char* tag, const dsd_opts* opts, dsd_state* state, int slot, const float* material,
       const dsd_voice_reception* r, float* gain_db) {
    level_speech(opts, state, slot, material, TEACH_N, r);
    return expect_true(tag, settled_at(state, slot, gain_db));
}

/* Brief 2: a reception from radio X settles at W; the next from X opens at W, one from Y at the opening gain, and a
   second reception from X that settles at W2 leaves X's memory at W + 0.5 (W2 - W). */
static int
test_memory_round_trip(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    const int fam = family_dmr();
    float w = 0.0f;
    float w2 = 0.0f;
    dsd_voice_reception r = reception(1U, fam, SRC_X);
    rc |= settle("memory: X settles", opts, state, 0, t->low, &r, &w);
    rc |= expect_true("memory: the low talker needs far more than the opening gain", w > OPEN_DB + 10.0);
    r = reception(2U, fam, SRC_X);
    rc |= opens_at("memory: the next reception from X opens at W", opts, state, 0, &r, w);
    r = reception(3U, fam, SRC_Y);
    rc |= opens_at("memory: a reception from Y opens at the opening gain", opts, state, 0, &r, (float)OPEN_DB);
    r = reception(4U, fam, SRC_X);
    rc |= opens_at("memory: X again opens at W", opts, state, 0, &r, w);
    rc |= settle("memory: X settles again", opts, state, 0, t->high, &r, &w2);
    rc |= expect_true("memory: at another gain", fabsf(w2 - w) > 5.0f);
    r = reception(5U, fam, SRC_X);
    rc |= opens_at("memory: X's memory moved half way to W2", opts, state, 0, &r, w + 0.5f * (w2 - w));
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 3: a reception too short to establish its estimate teaches the memory nothing. */
static int
test_memory_short_reception_stores_nothing(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    const int fam = family_dmr();
    dsd_voice_reception r = reception(1U, fam, SRC_Z);
    level_speech(opts, state, 0, t->brief, BRIEF_N, &r);
    const dsd_speech_agc* agc = agc_of(state, 0);
    float w = 0.0f;
    rc |= expect_true("short: the burst committed frames but did not establish",
                      agc != NULL && agc->learned.commits > 0 && !settled_at(state, 0, &w));
    rc |= expect_true("short: and moved the gain", agc != NULL && agc->learned.gain_db > (float)OPEN_DB + 3.0f);
    r = reception(2U, fam, SRC_Z);
    rc |= opens_at("short: the next reception from Z opens at the opening gain", opts, state, 0, &r, (float)OPEN_DB);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 4: a source named partway through a reception, before its first commit, eases the gain to the remembered one
   over one frame; named after the first commit, it changes nothing. */
static int
test_memory_late_source(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    dsd_state* control = new_state();
    if (!opts || !state || !control) {
        free(opts);
        free_state(state);
        free_state(control);
        return 1;
    }
    const int fam = family_dmr();
    float w = 0.0f;
    float wc = 0.0f;
    dsd_voice_reception r = reception(1U, fam, SRC_X);
    rc |= settle("late: X settles", opts, state, 0, t->low, &r, &w);
    rc |= settle("late: X settles on the control", opts, control, 0, t->low, &r, &wc);
    rc |= expect_near("late: the control is the same", wc, w, 0.0);

    /* Frames before the source is known, then a P25 Phase 1 style link control names it. */
    float frame[FRAME];
    r = reception(2U, fam, 0U);
    for (int k = 0; k < 3; k++) {
        level_probe(opts, state, 0, &r, frame);
        rc |= expect_near("late: unnamed frames play at the opening gain", db_of(frame[FRAME - 1] / PROBE_LEVEL),
                          OPEN_DB, 1e-4);
    }
    r = reception(2U, fam, SRC_X);
    level_probe(opts, state, 0, &r, frame);
    rc |= expect_near("late: the AGC takes the remembered gain", agc_of(state, 0)->learned.gain_db, w, 0.0);
    const double from = OPEN_LIN;
    const double to = pow(10.0, (double)w / 20.0);
    const double span = fabs(to - from);
    rc |= expect_true("late: the first sample is one step of a ramp, not a step",
                      fabs(frame[0] / PROBE_LEVEL - from) <= span / 100.0);
    rc |= expect_true("late: half way at mid-frame",
                      fabs(frame[FRAME / 2 - 1] / PROBE_LEVEL - (from + (to - from) * 0.5)) <= span / 100.0);
    rc |=
        expect_near("late: at the remembered gain by the frame's end", db_of(frame[FRAME - 1] / PROBE_LEVEL), w, 1e-4);
    int monotone = 1;
    for (int n = 1; n < FRAME; n++) {
        monotone &= frame[n] >= frame[n - 1];
    }
    rc |= expect_true("late: rising all the way", monotone);

    /* Named after the reception's first commit: the same as never named. */
    r = reception(3U, fam, 0U);
    level_speech(opts, state, 0, t->brief, BRIEF_N, &r);
    level_speech(opts, control, 0, t->brief, BRIEF_N, &r);
    rc |= expect_true("late: the burst committed", agc_of(state, 0)->learned.commits > 0);
    float named[FRAME];
    float unnamed[FRAME];
    r = reception(3U, fam, SRC_X);
    level_probe(opts, state, 0, &r, named);
    r = reception(3U, fam, 0U);
    level_probe(opts, control, 0, &r, unnamed);
    int same = 1;
    for (int n = 0; n < FRAME; n++) {
        same &= float_same(named[n], unnamed[n]);
    }
    const dsd_speech_agc* a = agc_of(state, 0);
    const dsd_speech_agc* b = agc_of(control, 0);
    rc |= expect_true("late: a source named after a commit changes nothing",
                      same && a->learned.commits == b->learned.commits && a->learned.steps == b->learned.steps
                          && float_same(a->learned.gain_db, b->learned.gain_db) && float_same(a->g_cur, b->g_cur)
                          && float_same(a->g_target, b->g_target) && a->ramp_left == b->ramp_left);
    free_state(control);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 5: a reception that names no source neither stores nor applies a memory; one radio ID under another protocol
   family is another talker. */
static int
test_memory_source_zero_and_family(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    const int dmr = family_dmr();
    const int nxdn = family_nxdn();
    float w = 0.0f;
    dsd_voice_reception r = reception(1U, dmr, 0U);
    rc |= settle("source 0: a reception with no source settles", opts, state, 0, t->low, &r, &w);
    r = reception(2U, dmr, 0U);
    rc |= opens_at("source 0: the next one opens at the opening gain", opts, state, 0, &r, (float)OPEN_DB);

    r = reception(3U, dmr, SRC_X);
    rc |= settle("family: X settles on DMR", opts, state, 0, t->low, &r, &w);
    r = reception(4U, nxdn, SRC_X);
    rc |= opens_at("family: the same ID on NXDN is another talker", opts, state, 0, &r, (float)OPEN_DB);
    r = reception(5U, dmr, SRC_X);
    rc |= opens_at("family: X on DMR is still remembered", opts, state, 0, &r, w);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 6: the memory holds 64 talkers. With all 64 remembered, a reception with no source changes nothing; a new
   talker then replaces the one least recently stored or applied, and a talker just applied stays. Each talker speaks
   at a level of its own, so each remembered gain is its own. */
static int
test_memory_least_recently_used(const talkers* t) {
    enum { TALKERS = 64 };

    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    float* voice = calloc(TEACH_N, sizeof(float));
    if (!opts || !state || !voice) {
        free(opts);
        free_state(state);
        free(voice);
        return 1;
    }
    const int fam = family_dmr();
    const uint64_t base = 0x4000U;
    float w[TALKERS + 1];
    uint64_t tx = 1U;
    char tag[96];
    for (int i = 0; i <= TALKERS; i++) {
        if (i == TALKERS) {
            /* All 64 stored (the last at this reception's start); this one names no source. */
            float w0 = 0.0f;
            dsd_voice_reception r = reception(tx++, fam, 0U);
            rc |= settle("lru: a reception with no source settles", opts, state, 0, t->high, &r, &w0);
            r = reception(tx++, fam, base);
            rc |= opens_at("lru: it replaced nobody: the oldest still opens at its gain", opts, state, 0, &r, w[0]);
        }
        dsd_voice_reception r = reception(tx++, fam, base + (uint64_t)i);
        DSD_SNPRINTF(tag, sizeof tag, "lru: talker %d settles", i);
        rc |= expect_true(tag, make_talker(voice, TEACH_N, 100U + (uint64_t)i, -28.0 - (0.25 * (double)i)) == 0);
        rc |= settle(tag, opts, state, 0, voice, &r, &w[i]);
    }
    int distinct = 1;
    for (int i = 1; i <= TALKERS; i++) {
        distinct &= fabsf(w[i] - w[i - 1]) > 0.0f;
    }
    rc |= expect_true("lru: each talker settled at a gain of its own", distinct);
    /* The 65th is stored at this reception's start: the least recently used is talker 1 (talker 0 was just applied). */
    dsd_voice_reception r = reception(tx++, fam, base + 1U);
    rc |= opens_at("lru: the oldest untouched talker is gone", opts, state, 0, &r, (float)OPEN_DB);
    r = reception(tx++, fam, base);
    rc |= opens_at("lru: the talker just applied stays", opts, state, 0, &r, w[0]);
    r = reception(tx++, fam, base + (uint64_t)TALKERS);
    rc |= opens_at("lru: the new talker is remembered", opts, state, 0, &r, w[TALKERS]);
    for (int i = 2; i < TALKERS; i++) {
        r = reception(tx++, fam, base + (uint64_t)i);
        DSD_SNPRINTF(tag, sizeof tag, "lru: talker %d stays", i);
        rc |= opens_at(tag, opts, state, 0, &r, w[i]);
    }
    free(voice);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 7: dsd_voice_level_reset() of one slot or both remembers what each slot learned before it forgets it. */
static int
test_memory_explicit_reset_stores(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    const int fam = family_dmr();
    float wx = 0.0f;
    float wy = 0.0f;
    dsd_voice_reception rx = reception(1U, fam, SRC_X);
    dsd_voice_reception ry = reception(2U, fam, SRC_Y);
    rc |= settle("reset: X settles on slot 1", opts, state, 0, t->low, &rx, &wx);
    rc |= settle("reset: Y settles on slot 2", opts, state, 1, t->high, &ry, &wy);
    dsd_voice_level_reset(state, 0);
    rc |= opens_at("reset: one slot's reset remembered X", opts, state, 0, &rx, wx);
    dsd_voice_level_reset(state, 0);

    /* Both slots learn other talkers, then one reset of both remembers both. */
    float wz = 0.0f;
    float wx2 = 0.0f;
    dsd_voice_reception rz = reception(3U, fam, SRC_Z);
    rc |= settle("reset: Z settles on slot 1", opts, state, 0, t->high, &rz, &wz);
    rx = reception(4U, fam, SRC_X);
    dsd_voice_level_reset(state, 1);
    rc |= settle("reset: X settles on slot 2", opts, state, 1, t->high, &rx, &wx2);
    dsd_voice_level_reset(state, -1);
    rc |= opens_at("reset: both slots' reset remembered Z", opts, state, 0, &rz, wz);
    rc |= opens_at("reset: and X", opts, state, 1, &rx, wx + 0.5f * (wx2 - wx));
    rc |= opens_at("reset: Y, remembered at slot 2's first reset", opts, state, 0, &ry, wy);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 8: both slots share the memory: slot 2 opens a reception from what slot 1 stored. */
static int
test_memory_shared_by_slots(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    const int fam = family_dmr();
    float w = 0.0f;
    dsd_voice_reception r = reception(1U, fam, SRC_X);
    rc |= settle("shared: X settles on slot 1", opts, state, 0, t->low, &r, &w);
    r = reception(2U, fam, SRC_Y);
    float frame[FRAME];
    level_probe(opts, state, 0, &r, frame);
    r = reception(3U, fam, SRC_X);
    rc |= opens_at("shared: slot 2 opens X at what slot 1 learned", opts, state, 1, &r, w);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 9: a fixed -g learns nothing, so it stores nothing; back to auto, receptions open as before it. */
static int
test_memory_fixed_gain_stores_nothing(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    const int fam = family_dmr();
    float w = 0.0f;
    float unused = 0.0f;
    dsd_voice_reception r = reception(1U, fam, SRC_X);
    rc |= settle("fixed: X settles under auto", opts, state, 0, t->low, &r, &w);
    opts->audio_gain = 2.0f;
    r = reception(2U, fam, SRC_Y);
    level_speech(opts, state, 0, t->high, TEACH_N, &r);
    rc |= expect_true("fixed: Y under a fixed gain does not settle", !settled_at(state, 0, &unused));
    r = reception(3U, fam, SRC_X);
    level_speech(opts, state, 0, t->high, TEACH_N, &r);
    rc |= expect_true("fixed: nor X", !settled_at(state, 0, &unused));
    opts->audio_gain = 0.0f;
    r = reception(4U, fam, SRC_Y);
    rc |= opens_at("fixed: back to auto, Y opens at the opening gain", opts, state, 0, &r, (float)OPEN_DB);
    r = reception(5U, fam, SRC_X);
    rc |= opens_at("fixed: X opens at what auto learned", opts, state, 0, &r, w);
    free_state(state);
    free(opts);
    return rc;
}

/* Brief 10: the chain takes the reception's source from the slot's active call, and none when no call is active. */
static int
test_memory_source_of_the_active_call(const talkers* t) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    const dsd_voice_frame_info speech = frame_info(DSD_VOICE_MEDIA_SPEECH, 1, 1);
    float w = 0.0f;
    float w_none = 0.0f;
    rc |= expect_true("call source: call observed", observe_call_from(state, 0U, 100U, (uint32_t)SRC_X, 10.0) > 0);
    run_material(opts, state, 0, t->low, TEACH_N, &speech);
    rc |= expect_true("call source: X settles", settled_at(state, 0, &w));
    rc |= expect_true("call source: call ended", dsd_call_state_end(state, 0U, 11.0) > 0);
    rc |= expect_true("call source: X's next call", observe_call_from(state, 0U, 200U, (uint32_t)SRC_X, 12.0) > 0);
    (void)probe_gain(opts, state, 0);
    rc |= expect_near("call source: X's next call opens at W", agc_of(state, 0)->learned.gain_db, w, 0.0);

    /* No call active: the ended call's radio is not the reception's source. */
    rc |= expect_true("call source: that call ended", dsd_call_state_end(state, 0U, 13.0) > 0);
    dsd_voice_level_reset(state, 0);
    run_material(opts, state, 0, t->high, TEACH_N, &speech);
    rc |= expect_true("call source: speech with no call settles", settled_at(state, 0, &w_none));
    rc |= expect_true("call source: at another gain", fabsf(w_none - w) > 5.0f);
    dsd_voice_level_reset(state, 0);
    rc |= expect_true("call source: X's third call", observe_call_from(state, 0U, 300U, (uint32_t)SRC_X, 14.0) > 0);
    (void)probe_gain(opts, state, 0);
    rc |= expect_near("call source: X's memory ignored the speech with no call", agc_of(state, 0)->learned.gain_db, w,
                      0.0);
    free_state(state);
    free(opts);
    return rc;
}

static int
test_talker_memory(void) {
    talkers t = {calloc(TEACH_N, sizeof(float)), calloc(TEACH_N, sizeof(float)), calloc(BRIEF_N, sizeof(float))};
    int rc = 0;
    if (!t.low || !t.high || !t.brief || make_talker(t.low, TEACH_N, 61U, -44.0) != 0
        || make_talker(t.high, TEACH_N, 62U, -34.0) != 0 || make_talker(t.brief, BRIEF_N, 63U, -44.0) != 0) {
        rc = 1;
    } else {
        rc |= test_memory_round_trip(&t);
        rc |= test_memory_short_reception_stores_nothing(&t);
        rc |= test_memory_late_source(&t);
        rc |= test_memory_source_zero_and_family(&t);
        rc |= test_memory_least_recently_used(&t);
        rc |= test_memory_explicit_reset_stores(&t);
        rc |= test_memory_shared_by_slots(&t);
        rc |= test_memory_fixed_gain_stores_nothing(&t);
        rc |= test_memory_source_of_the_active_call(&t);
    }
    free(t.low);
    free(t.high);
    free(t.brief);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_frames_that_may_not_learn_do_not_teach();
    rc |= test_auto_gain_starts_at_the_reference_level();
    rc |= test_a_reception_opens_at_the_opening_gain();
    rc |= test_tone_frames_play_at_most_the_start_gain();
    rc |= test_high_pass_keeps_its_state_across_a_healed_fade();
    float* speech = calloc(TEACH_N, sizeof(float));
    if (!speech || quiet_speech(speech, TEACH_N, 51U) != 0) {
        free(speech);
        return 1;
    }
    rc |= test_transmission_and_family_decide_the_reception(speech);
    rc |= test_talkspurt_decides_the_reception(speech);
    rc |= test_explicit_resets_and_what_keeps_the_level(speech);
    rc |= test_fixed_and_auto_keep_the_estimate(speech);
    rc |= test_a_new_reception_wins_over_the_fixed_gain();
    rc |= test_decodability_returns_per_slot();
    rc |= test_tx_monitor_plays_at_unity(speech);
    free(speech);
    rc |= test_talker_memory();
    if (rc == 0) {
        DSD_FPRINTF(stdout, "CORE_VOICE_LEVEL: OK\n");
    }
    return rc;
}
