// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The one-channel mix of two slots (issue #518): it sums the slots that carry fresh decoded audio of their own instead
 * of averaging them, so one talker keeps its level beside a silent or duplicated companion, and it limits the sum of two
 * hot talkers at the -1 dBFS ceiling. Here the mixer itself and the DMR superframe mix (playSynthesizedVoiceSS3()) that
 * plays it, with the frames each slot staged the way the DMR voice bursts store them (dsd_dmr_mix_store_frame()).
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/dsp/speech_agc.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

/* The limiter's ceiling in int16 units: -1 dBFS of 32768, rounded down. */
enum { CEILING = 29204, FRAME = 160, BURST = 3 * FRAME };

static short g_out[4 * BURST];
static size_t g_out_count;
static int g_blocks;
static int g_wide_blocks;

static void
capture_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    g_blocks++;
    if (nsam != FRAME * sizeof(short)) {
        g_wide_blocks++;
        return;
    }
    if (data && g_out_count + FRAME <= sizeof(g_out) / sizeof(g_out[0])) {
        DSD_MEMCPY(&g_out[g_out_count], data, FRAME * sizeof(short));
        g_out_count += FRAME;
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

static void
fill(short* frame, short v) {
    for (int i = 0; i < FRAME; i++) {
        frame[i] = v;
    }
}

/* The mixer on its own: talkers summed, a limiter that holds two hot ones at the ceiling and then releases, the int16
   clamp alone without one. */
static int
test_mixer_sums_and_limits(void) {
    short left[FRAME];
    short right[FRAME];
    short out[FRAME];
    dsd_peak_limiter limiter;
    int rc = 0;
    rc |= expect_int("limiter init", dsd_peak_limiter_init(&limiter, 8000, DSD_SPEECH_AGC_CEILING_DBFS), 0);

    fill(left, 3000);
    fill(right, 5000);
    audio_mix_mono_sum_s16(left, right, FRAME, 1, 1, &limiter, out);
    rc |= expect_int("two talkers summed", out[FRAME - 1], 8000);
    audio_mix_mono_sum_s16(left, right, FRAME, 1, 0, &limiter, out);
    rc |= expect_int("one talker at its own level", out[FRAME - 1], 3000);
    audio_mix_mono_sum_s16(left, right, FRAME, 0, 0, &limiter, out);
    rc |= expect_int("no talker", out[FRAME - 1], 0);

    // Two hot talkers: louder than either, never above the ceiling.
    fill(left, 20000);
    fill(right, 20000);
    audio_mix_mono_sum_s16(left, right, FRAME, 1, 1, &limiter, out);
    int peak = 0;
    for (int i = 0; i < FRAME; i++) {
        peak = abs(out[i]) > peak ? abs(out[i]) : peak;
    }
    rc |= expect_int("two hot talkers held at the ceiling", peak, CEILING);
    // The gain it took releases at 50 dB/s: a talker just under the ceiling comes back up within half a second.
    fill(left, 29000);
    audio_mix_mono_sum_s16(left, right, FRAME, 1, 0, &limiter, out);
    rc |= expect_int("the gain carries into the next block", out[0] < 29000 - 3000, 1);
    for (int b = 0; b < 25; b++) {
        audio_mix_mono_sum_s16(left, right, FRAME, 1, 0, &limiter, out);
    }
    rc |= expect_int("released", out[FRAME - 1], 29000);
    dsd_peak_limiter_reset(&limiter);

    // Without a limiter only the int16 clamp holds the sum.
    audio_mix_mono_sum_s16(left, right, FRAME, 1, 1, NULL, out);
    rc |= expect_int("unlimited sum clamps", out[0], 32767);
    fill(left, -20000);
    fill(right, -20000);
    audio_mix_mono_sum_s16(left, right, FRAME, 1, 1, NULL, out);
    rc |= expect_int("unlimited negative sum clamps", out[0], -32768);
    return rc;
}

typedef struct {
    dsd_opts* opts;
    dsd_state* state;
} dmr_rig;

static int
dmr_rig_open(dmr_rig* rig) {
    rig->opts = calloc(1, sizeof(*rig->opts));
    rig->state = calloc(1, sizeof(*rig->state));
    if (!rig->opts || !rig->state) {
        free(rig->opts);
        free(rig->state);
        return 1;
    }
    rig->opts->audio_out = 1;
    rig->opts->audio_out_type = 8;
    rig->opts->pulse_digi_out_channels = 1;
    rig->opts->slot1_on = 1;
    rig->opts->slot2_on = 1;
    rig->opts->slot_preference = 2;
    rig->state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;
    rig->state->dmrburstL = 16;
    rig->state->dmrburstR = 16;
    return 0;
}

static void
dmr_rig_close(dmr_rig* rig) {
    dsd_state_ext_free_all(rig->state);
    free(rig->state);
    free(rig->opts);
}

/* What the vocoder leaves for one frame of @p slot and the DMR voice burst stores: @p level decoded (the slot's own
   audio), or, with @p level 0, silence the vocoder staged in place of a frame (a mute, or no short samples). */
static void
dmr_stage(dsd_state* state, int slot, int frame, short level) {
    fill(slot == 0 ? state->s_l : state->s_r, level);
    state->mbe_short_silenced[slot] = level != 0 ? 0U : 1U;
    dsd_dmr_mix_store_frame(state, slot, frame);
}

static void
dmr_seed_call(dsd_state* state, uint8_t slot, uint64_t target) {
    const dsd_call_observation observation = {
        .protocol = DSD_SYNC_DMR_BS_VOICE_POS,
        .slot = slot,
        .kind = DSD_CALL_KIND_GROUP_VOICE,
        .ota_target_id = target,
        .policy_target_id = target,
        .ota_source_id = 1000U + slot,
    };
    (void)dsd_call_state_observe(state, &observation, DSD_CALL_BOUNDARY_BEGIN);
}

static void
dmr_stage_burst(dsd_state* state, int slot, short level) {
    for (int k = 0; k < 3; k++) {
        dmr_stage(state, slot, k, level);
    }
}

static void
dmr_play(dmr_rig* rig) {
    g_out_count = 0;
    g_blocks = 0;
    g_wide_blocks = 0;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_blast});
    playSynthesizedVoiceSS3(rig->opts, rig->state);
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
}

/* Every sample of the burst's three mono blocks is @p want. */
static int
expect_burst(const char* tag, int want) {
    int rc = expect_int(tag, g_blocks, 3);
    rc |= expect_int(tag, g_wide_blocks, 0);
    rc |= expect_int(tag, (long)g_out_count, BURST);
    for (size_t i = 0; i < g_out_count && rc == 0; i++) {
        rc |= expect_int(tag, g_out[i], want);
    }
    return rc;
}

/* The DMR superframe mix in one channel. Two talkers sum; one talker plays at its own level whether its companion is
   silent but audible (before: averaged with that silence, -6 dB), duplicated by the preference or the burst hint
   (a copy is no second talker: a plain sum would double it), held out, switched off or not the DMR mono slot. */
static int
test_dmr_mono_sums_its_talkers(void) {
    int rc = 0;
    for (int c = 0; c < 8; c++) {
        dmr_rig rig;
        if (dmr_rig_open(&rig) != 0) {
            return 1;
        }
        dsd_opts* opts = rig.opts;
        dsd_state* state = rig.state;
        const char* tag = "";
        int want = 3000;
        dmr_stage_burst(state, 0, 3000);
        dmr_stage_burst(state, 1, 5000);
        switch (c) {
            case 0:
                tag = "two talkers summed";
                want = 8000;
                break;
            case 1:
                // The companion's last frame was clear, but it staged nothing in this burst.
                tag = "a talker beside a silent audible slot";
                DSD_MEMSET(state->s_r4, 0, sizeof(state->s_r4));
                DSD_MEMSET(state->s4_fresh[1], 0, sizeof(state->s4_fresh[1]));
                break;
            case 2:
                tag = "a talker beside a slot the vocoder left out";
                dmr_stage_burst(state, 1, 0);
                break;
            case 3:
                tag = "the preferred slot fills both channels";
                opts->slot_preference = 1;
                want = 5000;
                break;
            case 4:
                // Slot 1 carries no voice burst any more: the burst hint copies slot 2 into its channel.
                tag = "the burst hint copies one talker into both channels";
                state->dmrburstL = 9;
                want = 5000;
                break;
            case 5:
                tag = "a hold on slot 1's talkgroup";
                dmr_seed_call(state, 0U, 101U);
                dmr_seed_call(state, 1U, 202U);
                state->tg_hold = 101;
                break;
            case 6:
                tag = "slot 2 switched off";
                opts->slot2_on = 0;
                break;
            case 7:
                tag = "DMR mono plays slot 2";
                opts->dmr_mono = 1;
                state->dmr_mono_slot = 1;
                want = 5000;
                break;
            default: break;
        }
        dmr_play(&rig);
        rc |= expect_burst(tag, want);
        dmr_rig_close(&rig);
    }
    return rc;
}

/* Two hot talkers stay under the ceiling; the mixer's limiter keeps its gain into the next burst and goes back to
   unity at a carrier boundary (dsd_voice_level_reset() of both slots). */
static int
test_dmr_mono_limits_two_hot_talkers(void) {
    dmr_rig rig;
    if (dmr_rig_open(&rig) != 0) {
        return 1;
    }
    int rc = 0;
    dmr_stage_burst(rig.state, 0, 20000);
    dmr_stage_burst(rig.state, 1, 20000);
    dmr_play(&rig);
    rc |= expect_burst("two hot talkers held at the ceiling", CEILING);

    dmr_stage_burst(rig.state, 0, 29000);
    rig.state->mbe_slot_muted[1] = 1U;
    dmr_play(&rig);
    rc |= expect_int("the gain carries into the next burst", g_out_count > 0 && g_out[0] < 29000 - 3000, 1);

    dmr_stage_burst(rig.state, 0, 20000);
    rig.state->mbe_slot_muted[1] = 0U;
    dmr_stage_burst(rig.state, 1, 20000);
    dmr_play(&rig);
    dsd_voice_level_reset(rig.state, -1);
    dmr_stage_burst(rig.state, 0, 29000);
    rig.state->mbe_slot_muted[1] = 1U;
    dmr_play(&rig);
    rc |= expect_burst("a carrier boundary resets it", 29000);
    dmr_rig_close(&rig);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_mixer_sums_and_limits();
    rc |= test_dmr_mono_sums_its_talkers();
    rc |= test_dmr_mono_limits_two_hot_talkers();
    if (rc == 0) {
        printf("CORE_MONO_MIX: OK\n");
    }
    return rc;
}
