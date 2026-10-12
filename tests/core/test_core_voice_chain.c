// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The digital voice chain (issue #653): one high-pass and one gain per slot, run once on each decoded frame before any
 * consumer, the same for both output formats.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/voice_chain.h>
#include <dsd-neo/runtime/udp_audio_hooks.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

static const double k_pi = 3.14159265358979323846;

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

/* Two frames agree sample for sample (the same arithmetic on the same input gives the same result). */
static int
frames_match(const float* a, const float* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (fabsf(a[i] - b[i]) > 1e-6f) {
            return 0;
        }
    }
    return 1;
}

static dsd_opts*
new_opts(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    if (opts) {
        opts->use_hpf_d = 1;
        opts->audio_gain = 1.0f;
        opts->audio_gainR = 1.0f;
    }
    return opts;
}

static dsd_state*
new_state(void) {
    dsd_state* state = calloc(1, sizeof(*state));
    if (state) {
        state->aout_gain = 25.0f;
        state->aout_gainR = 25.0f;
        state->aout_max_buf_p = state->aout_max_buf;
        state->aout_max_buf_pR = state->aout_max_bufR;
    }
    return state;
}

static void
free_state(dsd_state* state) {
    if (state) {
        dsd_state_ext_free_all(state);
        free(state);
    }
}

/* The closed form of a bilinear 2nd-order Butterworth high-pass prewarped to its corner. */
static double
butterworth_hp_db(double hz, double corner_hz, double rate_hz) {
    const double r = tan(k_pi * corner_hz / rate_hz) / tan(k_pi * hz / rate_hz);
    return -10.0 * log10(1.0 + (r * r * r * r));
}

static int
test_high_pass_design_passes_voice_at_unity(void) {
    int rc = 0;
    static const double freqs[] = {100.0, 200.0, 300.0, 500.0, 1000.0, 2000.0, 3000.0, 3900.0};
    for (size_t i = 0; i < sizeof(freqs) / sizeof(freqs[0]); i++) {
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "hpf design at %.0f Hz", freqs[i]);
        rc |= expect_near(tag, dsd_voice_hpf_gain_db(freqs[i]),
                          butterworth_hp_db(freqs[i], DSD_VOICE_HPF_CORNER_HZ, DSD_VOICE_HPF_RATE_HZ), 1e-6);
    }
    rc |= expect_near("hpf -3 dB at its corner", dsd_voice_hpf_gain_db(DSD_VOICE_HPF_CORNER_HZ), -3.0103, 1e-3);
    rc |= expect_near("hpf unity at Nyquist", dsd_voice_hpf_gain_db(3999.999), 0.0, 1e-6);
    rc |= expect_true("hpf takes 100 Hz at least 18 dB down", dsd_voice_hpf_gain_db(100.0) <= -18.0);
    for (int step = 0; step < 60; step++) {
        const double hz = 1000.0 + (50.0 * (double)step);
        if (dsd_voice_hpf_gain_db(hz) < -0.05 || dsd_voice_hpf_gain_db(hz) > 1e-9) {
            rc |= expect_true("hpf flat within 0.05 dB from 1 kHz up", 0);
            break;
        }
    }
    return rc;
}

/* A sine sweep through the running filter: past its settling, the RMS out over in reads the designed gain. */
static int
test_high_pass_sine_sweep_reads_design(void) {
    int rc = 0;
    static const double freqs[] = {150.0, 300.0, 700.0, 1000.0, 1500.0, 2500.0, 3500.0};
    dsd_state* state = new_state();
    if (!state) {
        return 1;
    }
    for (size_t f = 0; f < sizeof(freqs) / sizeof(freqs[0]); f++) {
        dsd_voice_hpf_reset(state, 0);
        double in_sq = 0.0;
        double out_sq = 0.0;
        float frame[DSD_VOICE_FRAME_SAMPLES];
        for (int block = 0; block < 50; block++) {
            for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
                const int t = (block * DSD_VOICE_FRAME_SAMPLES) + n;
                frame[n] = (float)(10000.0 * sin(2.0 * k_pi * freqs[f] * (double)t / DSD_VOICE_HPF_RATE_HZ));
            }
            float in[DSD_VOICE_FRAME_SAMPLES];
            DSD_MEMCPY(in, frame, sizeof(in));
            dsd_voice_hpf_process(state, 0, frame, DSD_VOICE_FRAME_SAMPLES);
            if (block >= 10) {
                for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
                    in_sq += (double)in[n] * (double)in[n];
                    out_sq += (double)frame[n] * (double)frame[n];
                }
            }
        }
        char tag[64];
        DSD_SNPRINTF(tag, sizeof(tag), "hpf sweep at %.0f Hz", freqs[f]);
        rc |= expect_near(tag, 10.0 * log10(out_sq / in_sq), dsd_voice_hpf_gain_db(freqs[f]), 0.05);
        if (freqs[f] >= 1000.0) {
            rc |= expect_near("hpf sweep reads 0 dB above 1 kHz", 10.0 * log10(out_sq / in_sq), 0.0, 0.1);
        }
    }
    free_state(state);
    return rc;
}

/* Each slot filters on its own state: interleaving slot 1 and slot 2 frames gives each the output it gets alone. */
static int
test_high_pass_state_is_per_slot(void) {
    int rc = 0;
    dsd_state* mixed = new_state();
    dsd_state* only_left = new_state();
    dsd_state* only_right = new_state();
    if (!mixed || !only_left || !only_right) {
        free_state(mixed);
        free_state(only_left);
        free_state(only_right);
        return 1;
    }
    for (int block = 0; block < 6; block++) {
        float a[DSD_VOICE_FRAME_SAMPLES];
        float b[DSD_VOICE_FRAME_SAMPLES];
        for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
            const int t = (block * DSD_VOICE_FRAME_SAMPLES) + n;
            a[n] = (float)(8000.0 * sin(2.0 * k_pi * 440.0 * (double)t / 8000.0) + 3000.0);
            b[n] = (float)(-6000.0 * sin(2.0 * k_pi * 170.0 * (double)t / 8000.0));
        }
        float a_alone[DSD_VOICE_FRAME_SAMPLES];
        float b_alone[DSD_VOICE_FRAME_SAMPLES];
        DSD_MEMCPY(a_alone, a, sizeof(a));
        DSD_MEMCPY(b_alone, b, sizeof(b));
        dsd_voice_hpf_process(mixed, 0, a, DSD_VOICE_FRAME_SAMPLES);
        dsd_voice_hpf_process(mixed, 1, b, DSD_VOICE_FRAME_SAMPLES);
        dsd_voice_hpf_process(only_left, 0, a_alone, DSD_VOICE_FRAME_SAMPLES);
        dsd_voice_hpf_process(only_right, 1, b_alone, DSD_VOICE_FRAME_SAMPLES);
        rc |= expect_true("slot 1 output unaffected by slot 2", frames_match(a, a_alone, DSD_VOICE_FRAME_SAMPLES));
        rc |= expect_true("slot 2 output unaffected by slot 1", frames_match(b, b_alone, DSD_VOICE_FRAME_SAMPLES));
    }
    free_state(mixed);
    free_state(only_left);
    free_state(only_right);
    return rc;
}

static void
fill_dc(float* frame, float value) {
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        frame[n] = value;
    }
}

static int
hpf_state_clear(const dsd_state* state, int slot) {
    return fabs(state->voice_hpf[slot].s1) < 1e-12 && fabs(state->voice_hpf[slot].s2) < 1e-12;
}

static int
observe_call(dsd_state* state, int protocol, uint8_t slot, uint32_t target) {
    const dsd_call_observation call = {.protocol = protocol,
                                       .slot = slot,
                                       .kind = DSD_CALL_KIND_GROUP_VOICE,
                                       .ota_target_id = target,
                                       .policy_target_id = target,
                                       .ota_source_id = 7U};
    return dsd_call_state_observe(state, &call, DSD_CALL_BOUNDARY_BEGIN);
}

/* The slot's high-pass starts over at a reception boundary: a new call on the slot or another protocol family. A call
   that becomes known partway through a stream does not cut a step into it, nor does the other slot's call. */
static int
test_high_pass_restarts_at_reception_boundaries(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    float frame[DSD_VOICE_FRAME_SAMPLES];
    state->synctype = DSD_SYNC_DMR_BS_VOICE_POS;

    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 1, frame);
    rc |= expect_true("frames before any call leave state", !hpf_state_clear(state, 1));

    rc |= expect_true("first call observed", observe_call(state, DSD_SYNC_DMR_BS_VOICE_POS, 1U, 100U) > 0);
    fill_dc(frame, 1000.0f);
    const double s1_before = state->voice_hpf[1].s1;
    dsd_voice_chain_run(opts, state, 1, frame);
    rc |= expect_true("a call known partway keeps the state",
                      fabs(state->voice_hpf[1].s1 - s1_before) > 1e-9 && !hpf_state_clear(state, 1));
    // Settle, then a frame of zeros shows whether the state started over: clear state turns zeros into zeros.
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 1, frame);

    rc |= expect_true("first call ended", dsd_call_state_end(state, 1U, 0.0) > 0);
    rc |= expect_true("second call observed", observe_call(state, DSD_SYNC_DMR_BS_VOICE_POS, 1U, 200U) > 0);
    fill_dc(frame, 0.0f);
    frame[0] = 0.0f;
    dsd_voice_chain_run(opts, state, 1, frame);
    int silent = 1;
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        silent &= fabsf(frame[n]) < 1e-6f;
    }
    rc |= expect_true("a new call on the slot starts from clear state", silent);

    // A reception boundary (the carrier left, a file ended) forgets the call too: frames with no call after it, then
    // the next call learned partway through them, run on without a step.
    dsd_voice_hpf_reset(state, 1);
    rc |= expect_true("second call ended", dsd_call_state_end(state, 1U, 0.0) > 0);
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 1, frame);
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 1, frame);
    rc |= expect_true("third call observed", observe_call(state, DSD_SYNC_DMR_BS_VOICE_POS, 1U, 300U) > 0);
    fill_dc(frame, 0.0f);
    dsd_voice_chain_run(opts, state, 1, frame);
    int carried = 0;
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        carried |= fabsf(frame[n]) > 1e-3f;
    }
    rc |= expect_true("a call learned after a boundary reset keeps the running state", carried);

    // A protocol change with no call: the family decides.
    dsd_state* other = new_state();
    if (!other) {
        free(opts);
        free_state(state);
        return 1;
    }
    other->synctype = DSD_SYNC_NXDN_POS;
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, other, 0, frame);
    rc |= expect_true("nxdn frames leave state", !hpf_state_clear(other, 0));
    other->synctype = DSD_SYNC_DSTAR_VOICE_POS;
    fill_dc(frame, 0.0f);
    dsd_voice_chain_run(opts, other, 0, frame);
    silent = 1;
    for (int n = 0; n < DSD_VOICE_FRAME_SAMPLES; n++) {
        silent &= fabsf(frame[n]) < 1e-6f;
    }
    rc |= expect_true("another protocol family starts from clear state", silent);

    // YSF decodes full rate under a borrowed P25 Phase 1 synctype: the dispatched protocol keeps it one family.
    other->frame_dispatch_active = 1;
    other->frame_dispatch_synctype = DSD_SYNC_YSF_POS;
    other->synctype = DSD_SYNC_YSF_POS;
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, other, 0, frame);
    other->synctype = DSD_SYNC_P25P1_POS;
    const double ysf_s1 = other->voice_hpf[0].s1;
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, other, 0, frame);
    rc |= expect_true("a borrowed decoder keeps the dispatched family",
                      fabs(other->voice_hpf[0].s1 - ysf_s1) > 1e-9 && !hpf_state_clear(other, 0));

    // The high-pass off leaves the frame and clears the state, so turning it back on starts clean.
    opts->use_hpf_d = 0;
    fill_dc(frame, 1234.0f);
    dsd_voice_chain_run(opts, other, 0, frame);
    rc |= expect_near("high-pass off passes the frame", frame[5], 1234.0, 1e-3);
    rc |= expect_true("high-pass off clears the state", hpf_state_clear(other, 0));

    free_state(other);
    free_state(state);
    free(opts);
    return rc;
}

/* The chain's gain: a fixed -g is a plain multiplier in both formats; below 0, unity; 0, the auto gain. */
static int
test_chain_gain_laws(void) {
    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    opts->use_hpf_d = 0;
    float frame[DSD_VOICE_FRAME_SAMPLES];

    opts->audio_gain = 3.0f;
    state->aout_gain = 9.0f; // a stale level the fixed gain does not read
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 0, frame);
    rc |= expect_near("fixed gain multiplies by the setting", frame[17], 3000.0, 1e-3);
    rc |= expect_near("fixed gain is what the level reads", state->aout_gain, 3.0, 1e-6);

    opts->audio_gainR = 50.0f;
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 1, frame);
    rc |= expect_near("fixed gain saturates at full scale", frame[3], 32767.0, 1e-3);

    opts->audio_gain = -1.0f;
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, state, 0, frame);
    rc |= expect_near("negative gain is unity", frame[3], 1000.0, 1e-3);

    // Auto: a loud frame is cut at once to the 30000 peak target.
    opts->audio_gain = 0.0f;
    state->aout_gain = 25.0f;
    fill_dc(frame, 2000.0f);
    dsd_voice_chain_run(opts, state, 0, frame);
    rc |= expect_near("auto gain attacks at once", state->aout_gain, 15.0, 1e-4);
    rc |= expect_near("auto gain output at the target", frame[159], 30000.0, 1e-2);

    // A peak above 30000 takes the gain below unity, which the next frames keep and release by 5% each.
    fill_dc(frame, 32000.0f);
    dsd_voice_chain_run(opts, state, 0, frame);
    rc |= expect_true("auto gain may go below unity", state->aout_gain < 1.0f);
    const float below = state->aout_gain;
    fill_dc(frame, 100.0f);
    dsd_voice_chain_run(opts, state, 0, frame);
    rc |= expect_near("a legitimate gain below unity is kept", state->aout_gain, below, 1e-6);

    // A level no gain law leaves (a zeroed state) starts over at unity instead of freezing at 0.
    dsd_state* zeroed = new_state();
    if (!zeroed) {
        free(opts);
        free_state(state);
        return 1;
    }
    zeroed->aout_gain = 0.0f;
    fill_dc(frame, 1000.0f);
    dsd_voice_chain_run(opts, zeroed, 0, frame);
    rc |= expect_true("zeroed auto gain recovers", zeroed->aout_gain > 1.0f && frame[0] >= 999.0f);
    free_state(zeroed);

    free_state(state);
    free(opts);
    return rc;
}

static short g_tone_s16[16 * 160];
static float g_tone_f32[16 * 160];
static size_t g_tone_count;
static int g_tone_float;

static void
capture_tone(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data) {
    (void)opts;
    (void)state;
    if (g_tone_float) {
        const size_t n = nsam / sizeof(float);
        for (size_t i = 0; i < n && g_tone_count < sizeof(g_tone_f32) / sizeof(g_tone_f32[0]); i++) {
            g_tone_f32[g_tone_count++] = ((const float*)data)[i];
        }
    } else {
        const size_t n = nsam / sizeof(short);
        for (size_t i = 0; i < n && g_tone_count < sizeof(g_tone_s16) / sizeof(g_tone_s16[0]); i++) {
            g_tone_s16[g_tone_count++] = ((const short*)data)[i];
        }
    }
}

/* The alert tone is built once on the int16 scale: short output plays a clean sine at its level, float output the same
   samples / 32768, and the tone runs on across its 20 ms blocks. */
static int
test_beeper_tone_is_the_same_in_both_formats(void) {
    static const struct {
        int id;
        int ad;
    } alerts[] = {{40, 86}, {80, 20}, {41, 86}}; // ID 41 ends a block partway through a cycle

    int rc = 0;
    dsd_opts* opts = new_opts();
    dsd_state* state = new_state();
    if (!opts || !state) {
        free(opts);
        free_state(state);
        return 1;
    }
    opts->audio_out = 1;
    opts->audio_out_type = 8;
    opts->pulse_digi_out_channels = 1;
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){.blast = capture_tone});
    for (size_t a = 0; a < sizeof(alerts) / sizeof(alerts[0]); a++) {
        const double amplitude = ((double)alerts[a].ad * 75.0 / 8000.0) * 32767.0;
        const double step = 2.0 * k_pi * (31.25 * (double)alerts[a].id) / 8000.0;

        g_tone_float = 0;
        g_tone_count = 0;
        opts->floating_point = 0;
        beeper(opts, state, 0, alerts[a].id, alerts[a].ad, 3);
        // UDP output doubles the length.
        rc |= expect_true("short tone length", g_tone_count == (size_t)6U * 160U);
        int worst = 0;
        for (size_t i = 0; i < g_tone_count; i++) {
            const double want = amplitude * sin((double)i * step);
            const int err = abs((int)g_tone_s16[i] - (int)lrint(want));
            worst = err > worst ? err : worst;
        }
        rc |= expect_true("short tone is the sine at its level, phase running across blocks", worst == 0);

        g_tone_float = 1;
        g_tone_count = 0;
        opts->floating_point = 1;
        beeper(opts, state, 0, alerts[a].id, alerts[a].ad, 3);
        rc |= expect_true("float tone length", g_tone_count == (size_t)6U * 160U);
        float worst_f = 0.0f;
        for (size_t i = 0; i < g_tone_count; i++) {
            const float diff = fabsf(g_tone_f32[i] - ((float)g_tone_s16[i] / 32768.0f));
            worst_f = diff > worst_f ? diff : worst_f;
        }
        rc |= expect_true("float tone carries the short tone's samples", worst_f < 1e-7f);
    }
    dsd_udp_audio_hooks_set((dsd_udp_audio_hooks){0});
    free_state(state);
    free(opts);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_high_pass_design_passes_voice_at_unity();
    rc |= test_high_pass_sine_sweep_reads_design();
    rc |= test_high_pass_state_is_per_slot();
    rc |= test_high_pass_restarts_at_reception_boundaries();
    rc |= test_chain_gain_laws();
    rc |= test_beeper_tone_is_the_same_in_both_formats();
    if (rc == 0) {
        DSD_FPRINTF(stdout, "CORE_VOICE_CHAIN: OK\n");
    }
    return rc;
}
