// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #806: an I/Q replay runs the chain its live stream ran. A sidecar of a chain the device forced (an Airspy's
 * 2.5 MS/s, say) keeps its recorded passes and runs the live I/Q rate step after them, with the squelches' noise model
 * for that step; every other replay runs none. Each case plans a sidecar as a replay of the options plans it
 * (dsd_iq_replay_plan_rate() with the stream's request and filter domain), opens it, and runs noise through the
 * demodulator and the output chain.
 *
 * - The step, per family: FSK, CQPSK and the audio monitor emit at the rates the plan says, from the capture rate.
 * - post_downsample 1 to 4, for a recorded step and for a chain that does not step: the audio monitor runs at the
 *   complex rate over post, FSK and CQPSK at the complex rate.
 * - TETRA through the step: a forced chain whose passes reach 72 kHz steps there and the TETRA chain runs.
 * - A-B4: an RTL-SDR's 24 kHz EDACS sidecar is not forced, so digital_resample=auto does not resample its FSK output
 *   to 48 kHz (every replay ran as forced before).
 * - The filter domain the planner takes: none for the analog monitor and for TETRA where its own filter runs at the
 *   recorded rate; the digital ceiling for every other digital mode, and for TETRA under an override its filter does
 *   not run with or with the channel filter off.
 */

#include <cstdio>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/iq_replay.h>
#include <dsd-neo/io/iq_types.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/digital_channel.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"
#include "test_env.h"

namespace {

int
expect_eq(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%lld want=%lld\n", label, got, want);
        return 1;
    }
    return 0;
}

/* The step and the channel filter hold back their look-ahead, so a run emits up to that many fewer outputs than its
   input gives at the output rate, never more: at most the 288-tap channel filter's half and the step's tail (under
   100 inputs at these ratios) at the complex rate. */
int
expect_emitted(const char* label, long long got, long long input_pairs, long long out_rate_hz, long long capture_hz) {
    const long long most = input_pairs * out_rate_hz / capture_hz;
    const long long least = most - (144 + 100) * out_rate_hz / 48000 - 4;
    if (got < least || got > most) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%lld want %lld..%lld\n", label, got, least, most);
        return 1;
    }
    return 0;
}

void
dmr(dsd_opts* o) {
    o->frame_dmr = 1;
    o->mod_c4fm = 1;
}

void
p25_cqpsk(dsd_opts* o) {
    o->frame_p25p1 = 1;
    o->mod_qpsk = 1;
}

void
nfm(dsd_opts* o) {
    o->analog_only = 1;
    o->monitor_input_audio = 1;
    o->analog_demod = DSD_ANALOG_DEMOD_FM;
}

void
edacs(dsd_opts* o) {
    o->frame_provoice = 1;
    o->mod_gfsk = 1;
}

void
tetra(dsd_opts* o) {
    o->frame_tetra = 1;
    o->mod_qpsk = 1;
}

/* A sidecar of @p rate_hz over @p base and @p post for a @p bw_khz DSP bandwidth, with no rate chain recorded. */
dsd_iq_replay_config
sidecar(uint32_t rate_hz, uint32_t base, uint32_t post, int bw_khz) {
    dsd_iq_replay_config cfg;
    DSD_MEMSET(&cfg, 0, sizeof cfg);
    cfg.container = DSD_IQ_REPLAY_CONTAINER_SIDECAR;
    cfg.format = DSD_IQ_FORMAT_CF32;
    cfg.sample_rate_hz = rate_hz;
    cfg.base_decimation = base;
    cfg.post_downsample = post;
    cfg.demod_rate_hz = rate_hz / base / post;
    cfg.rtl_dsp_bw_khz = bw_khz;
    cfg.recorded_dsp_bw_khz = bw_khz;
    return cfg;
}

/* The sidecar a live forced chain writes: its nominal rate, forced, and the step to @p dsp_hz by @p L / @p M. */
dsd_iq_replay_config
stepped_sidecar(uint32_t rate_hz, uint32_t base, uint32_t post, int bw_khz, uint32_t dsp_hz, uint32_t L, uint32_t M) {
    dsd_iq_replay_config cfg = sidecar(rate_hz, base, post, bw_khz);
    cfg.rate_chain.has_nominal = 1;
    cfg.rate_chain.nominal_rate_hz = rate_hz;
    cfg.rate_chain.has_forced = 1;
    cfg.rate_chain.device_forced = 1;
    cfg.rate_chain.has_step = 1;
    cfg.rate_chain.dsp_rate_hz = dsp_hz;
    cfg.rate_chain.step_l = L;
    cfg.rate_chain.step_m = M;
    return cfg;
}

/* Plan and run @p cfg for the options @p configure sets (digital_resample=auto), 10 blocks of 10 ms of input. */
int
run(const char* label, void (*configure)(dsd_opts*), const dsd_iq_replay_config* cfg, dsd_iq_replay_rate_plan* plan,
    rtl_stream_test_post_chain_result* r) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    configure(&opts);
    opts.digital_resample_mode = DSD_DIGITAL_RESAMPLE_AUTO;
    char sub[192];
    DSD_SNPRINTF(sub, sizeof sub, "%s: planned and opened", label);
    const int pairs = (int)(cfg->sample_rate_hz / 100U);
    return expect_eq(sub, rtl_stream_test_replay_planned_chain(&opts, cfg, 10, pairs, plan, r), 0);
}

/* The step's state after a stepped open: running @p L / @p M, its squelch model built, forced, at @p complex_hz. */
int
expect_stepped(const char* label, const rtl_stream_test_post_chain_result* r, int L, int M, int complex_hz) {
    char sub[192];
    int rc = 0;
    DSD_SNPRINTF(sub, sizeof sub, "%s: the step runs %d/%d", label, L, M);
    rc |= expect_eq(sub, r->step_enabled && r->step_l == L && r->step_m == M, 1);
    DSD_SNPRINTF(sub, sizeof sub, "%s: the squelches' noise model is the step's", label);
    rc |= expect_eq(sub, r->squelch_step_model, 1);
    DSD_SNPRINTF(sub, sizeof sub, "%s: forced", label);
    rc |= expect_eq(sub, r->forced, 1);
    DSD_SNPRINTF(sub, sizeof sub, "%s: complex rate", label);
    rc |= expect_eq(sub, r->complex_rate_hz, complex_hz);
    return rc;
}

/* An Airspy capture at 2.5 MS/s, 3 MS/s or 312.5 kS/s for a 48 kHz bandwidth, written before the step fields: its
   passes and the live step to 48 kHz, for FSK, CQPSK and the audio monitor. */
int
test_forced_capture_steps(void) {
    static const struct {
        const char* name;
        uint32_t rate_hz;
        uint32_t base;
        int passes;
        int L;
        int M;
    } k_cases[] = {
        {"2.5 MS/s base 32", 2500000U, 32U, 5, 384, 625},
        {"3 MS/s base 32", 3000000U, 32U, 5, 64, 125},
        {"312.5 kS/s base 4", 312500U, 4U, 2, 384, 625},
    };

    int rc = 0;
    for (size_t i = 0; i < sizeof(k_cases) / sizeof(k_cases[0]); i++) {
        const dsd_iq_replay_config cfg = sidecar(k_cases[i].rate_hz, k_cases[i].base, 1U, 48);
        const long long fs = k_cases[i].rate_hz;
        char label[160];
        dsd_iq_replay_rate_plan plan;
        rtl_stream_test_post_chain_result r{};

        DSD_SNPRINTF(label, sizeof label, "DMR %s", k_cases[i].name);
        rc |= run(label, dmr, &cfg, &plan, &r);
        rc |= expect_stepped(label, &r, k_cases[i].L, k_cases[i].M, 48000);
        rc |= expect_eq(label, r.passes, k_cases[i].passes);
        rc |= expect_eq(label, r.fsk_sample_rate_hz, 48000);
        rc |= expect_eq(label, r.output_rate_hz, 48000);
        rc |= expect_emitted(label, r.emitted, r.input_pairs, 48000, fs);

        DSD_SNPRINTF(label, sizeof label, "P25 CQPSK %s", k_cases[i].name);
        rc |= run(label, p25_cqpsk, &cfg, &plan, &r);
        rc |= expect_stepped(label, &r, k_cases[i].L, k_cases[i].M, 48000);
        rc |= expect_eq(label, r.ted_sps, 10);
        rc |= expect_emitted(label, r.emitted, r.input_pairs, 4800, fs);

        DSD_SNPRINTF(label, sizeof label, "NFM %s", k_cases[i].name);
        rc |= run(label, nfm, &cfg, &plan, &r);
        rc |= expect_stepped(label, &r, k_cases[i].L, k_cases[i].M, 48000);
        rc |= expect_eq(label, r.rate_out_hz, 48000);
        rc |= expect_emitted(label, r.emitted, r.input_pairs, (long long)r.output_rate_hz, fs);
    }
    return rc;
}

/* post_downsample 1 to 4, through the planner: a recorded step at 312.5 kS/s to 48 kHz, and a chain at 24 kHz x post
   that does not step (kept within the digital filter domain). FSK and CQPSK run at the complex rate; the audio monitor
   at it over post, which its output resampler takes to the output rate. */
int
test_post_counts(void) {
    int rc = 0;
    for (uint32_t post = 1U; post <= 4U; post++) {
        const dsd_iq_replay_config stepped = stepped_sidecar(312500U, 4U, post, 48, 48000U, 384U, 625U);
        const dsd_iq_replay_config plain = sidecar(24000U * post, 1U, post, 24);
        const dsd_iq_replay_config* cfgs[2] = {&stepped, &plain};
        for (int c = 0; c < 2; c++) {
            const dsd_iq_replay_config* cfg = cfgs[c];
            const long long fs = cfg->sample_rate_hz;
            const int complex_hz = c == 0 ? 48000 : (int)(24000U * post);
            const char* kind = c == 0 ? "stepped" : "unstepped";
            char label[160];
            dsd_iq_replay_rate_plan plan;
            rtl_stream_test_post_chain_result r{};

            DSD_SNPRINTF(label, sizeof label, "%s post %u DMR", kind, post);
            rc |= run(label, dmr, cfg, &plan, &r);
            rc |= expect_eq(label, r.step_enabled, c == 0);
            rc |= expect_eq(label, r.complex_rate_hz, complex_hz);
            rc |= expect_eq(label, r.fsk_sample_rate_hz, complex_hz);
            rc |= expect_emitted(label, r.emitted, r.input_pairs, (long long)r.output_rate_hz, fs);

            DSD_SNPRINTF(label, sizeof label, "%s post %u P25 CQPSK", kind, post);
            rc |= run(label, p25_cqpsk, cfg, &plan, &r);
            rc |= expect_eq(label, r.complex_rate_hz, complex_hz);
            rc |= expect_emitted(label, r.emitted, r.input_pairs, 4800, fs);

            DSD_SNPRINTF(label, sizeof label, "%s post %u NFM", kind, post);
            rc |= run(label, nfm, cfg, &plan, &r);
            rc |= expect_eq(label, r.complex_rate_hz, complex_hz);
            rc |= expect_eq(label, r.rate_out_hz, complex_hz / (int)post);
            rc |= expect_emitted(label, r.emitted, r.input_pairs, (long long)r.output_rate_hz, fs);
        }
    }
    return rc;
}

/* TETRA through the step: an Airspy capture recorded at a 48 kHz bandwidth replays TETRA at 72 kHz, stepped 576/625
   after its 5 passes, and the TETRA chain runs there (before, the reader resampled 2.5 MS/s to 2.304 MS/s). */
int
test_tetra_through_step(void) {
    int rc = 0;
    const dsd_iq_replay_config cfg = sidecar(2500000U, 32U, 1U, 48);
    dsd_iq_replay_rate_plan plan;
    rtl_stream_test_post_chain_result r{};
    rc |= run("TETRA 2.5 MS/s base 32", tetra, &cfg, &plan, &r);
    rc |= expect_stepped("TETRA 2.5 MS/s base 32", &r, 576, 625, 72000);
    rc |= expect_eq("TETRA 2.5 MS/s base 32: not resampled", plan.resample, 0);
    rc |= expect_eq("TETRA 2.5 MS/s base 32: 4 samples per symbol", r.ted_sps, 4);
    rc |= expect_eq("TETRA 2.5 MS/s base 32: the TETRA chain runs", r.tetra_chain, 1);
    return rc;
}

/* A-B4: an RTL-SDR's 24 kHz EDACS capture (1.536 MS/s, base 64, no forced flag recorded) is not forced, so auto leaves
   the 9600 sym/s FSK output at 24 kHz, as the live RTL-SDR ran it; a capture the device forced to 24 kHz is resampled
   to 48 kHz. */
int
test_derived_forced_edacs(void) {
    int rc = 0;
    const dsd_iq_replay_config rtl = sidecar(1536000U, 64U, 1U, 24);
    dsd_iq_replay_rate_plan plan;
    rtl_stream_test_post_chain_result r{};
    rc |= run("EDACS RTL 24 kHz", edacs, &rtl, &plan, &r);
    rc |= expect_eq("EDACS RTL 24 kHz: not forced, derived",
                    r.forced == 0 && plan.forced_provenance == DSD_IQ_REPLAY_FORCED_DERIVED, 1);
    rc |= expect_eq("EDACS RTL 24 kHz: no step", r.step_enabled, 0);
    rc |= expect_eq("EDACS RTL 24 kHz: no FSK resample", r.output_rate_hz, 24000);
    rc |= expect_emitted("EDACS RTL 24 kHz: samples at 24 kHz", r.emitted, r.input_pairs, 24000, 1536000);
    /* An RTL-SDR that read back 1 Hz under its request (§2): planned from the request, kept, not forced, no step. */
    const dsd_iq_replay_config rtl_off = sidecar(1535999U, 32U, 1U, 48);
    rc |= run("DMR RTL read back 1535999", dmr, &rtl_off, &plan, &r);
    rc |= expect_eq("DMR RTL read back 1535999: kept, not resampled", plan.resample == 0 && r.passes == 5, 1);
    rc |= expect_eq("DMR RTL read back 1535999: 48000 Hz, no step, not forced",
                    r.complex_rate_hz == 48000 && r.step_enabled == 0 && r.forced == 0, 1);
    dsd_iq_replay_config forced = sidecar(1536000U, 64U, 1U, 24);
    forced.rate_chain.has_forced = 1;
    forced.rate_chain.device_forced = 1;
    rc |= run("EDACS forced 24 kHz", edacs, &forced, &plan, &r);
    rc |= expect_eq("EDACS forced 24 kHz: recorded forced", r.forced, 1);
    rc |= expect_eq("EDACS forced 24 kHz: resampled to 48 kHz", r.output_rate_hz, 48000);
    return rc;
}

/* The filter domain the replay planner takes (§13.8): which channel filter the mode will run decides it. */
int
test_filter_domain_input(void) {
    int rc = 0;
    static dsd_opts opts;
    const dsd_iq_replay_config cc144 = sidecar(144000U, 1U, 1U, 144);
    const dsd_iq_replay_config cc72 = sidecar(144000U, 2U, 1U, 72);
    const uint32_t digital = DSD_DIGITAL_CHANNEL_MAX_RATE_HZ;
    DSD_MEMSET(&opts, 0, sizeof opts);
    tetra(&opts);
    rc |= expect_eq("TETRA at 144 kHz: its own filter, no ceiling",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 0), 0);
    rc |= expect_eq("TETRA at 144 kHz with a 4 sps override: the digital ceiling",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 4), digital);
    rc |= expect_eq("TETRA at 144 kHz with an 8 sps override: no ceiling",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 8), 0);
    const dsd_iq_replay_config cc144_off = sidecar(144010U, 1U, 1U, 144);
    rc |= expect_eq("TETRA at 144010 Hz (in tolerance of 144 kHz): its own filter",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc144_off, 0), 0);
    rc |= expect_eq("TETRA at 72 kHz with a 4 sps override: no ceiling",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc72, 4), 0);
    opts.tetra_uplink = 1;
    rc |= expect_eq("TETRA uplink: an override does not reach its filter",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 4), 0);
    opts.tetra_uplink = 0;
    (void)dsd_test_setenv("DSD_NEO_CHANNEL_LPF", "0", 1);
    dsd_neo_config_init();
    rc |= expect_eq("TETRA with the channel filter off: the digital ceiling",
                    rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 0), digital);
    (void)dsd_test_unsetenv("DSD_NEO_CHANNEL_LPF");
    dsd_neo_config_init();
    DSD_MEMSET(&opts, 0, sizeof opts);
    p25_cqpsk(&opts);
    rc |= expect_eq("P25: the digital ceiling", rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 0), digital);
    DSD_MEMSET(&opts, 0, sizeof opts);
    nfm(&opts);
    rc |= expect_eq("the analog monitor: no ceiling", rtl_stream_test_replay_filter_domain_hz(&opts, &cc144, 0), 0);

    /* Planned and run: P25 on the 144 kHz capture folds a pass to 72 kHz; TETRA keeps 144 kHz. */
    dsd_iq_replay_rate_plan plan;
    rtl_stream_test_post_chain_result r{};
    rc |= run("P25 on a 144 kHz capture", p25_cqpsk, &cc144, &plan, &r);
    rc |=
        expect_eq("P25 on a 144 kHz capture: one more pass to 72 kHz", r.passes == 1 && r.complex_rate_hz == 72000, 1);
    rc |= run("TETRA on a 144 kHz capture", tetra, &cc144, &plan, &r);
    rc |= expect_eq("TETRA on a 144 kHz capture: 144 kHz", r.passes == 0 && r.complex_rate_hz == 144000, 1);
    return rc;
}

} // namespace

int
main(void) {
    int rc = 0;
    rc |= test_forced_capture_steps();
    rc |= test_post_counts();
    rc |= test_tetra_through_step();
    rc |= test_derived_forced_edacs();
    rc |= test_filter_domain_input();
    if (rc == 0) {
        std::printf("IO_RTL_REPLAY_STEP: OK\n");
    }
    return rc;
}
