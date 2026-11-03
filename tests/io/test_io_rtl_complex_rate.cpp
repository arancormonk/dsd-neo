// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Issue #806: the complex rate (demod_state::complex_rate) every rate chain publishes beside rate_out, and the paths
 * that run at it.
 *
 * - X10: an I/Q replay sidecar that decimates after the demodulator (post_downsample 2 to 4) runs the FSK
 *   discriminator and the CQPSK chain at the complex rate, ahead of that decimator, while only the audio monitor is
 *   decimated. The FSK modem, the TED, the output rate and its prediction follow the rate each path really emits at,
 *   so the samples the stream delivers per second match the output rate it publishes. Before, FSK and CQPSK were
 *   configured from the post-divided rate_out.
 * - The bandwidth x symbol rate x digital_resample matrix, Airspy against RTL-SDR: the complex rate is the DSP
 *   bandwidth on both; the decoder output still follows the forced flag. On an Airspy at 24 kHz, 4800 sym/s is a whole
 *   5 samples per symbol now, so auto no longer resamples it to 48 kHz (a documented change: the old 39,062.5 Hz chain
 *   did).
 * - X11: a symbol profile without an override lands with the TED of the complex rate, never the decoder-rate TED it
 *   was queued with, by a live request (the SPS hunt, a republish, a restored snapshot) or a retune profile, and the
 *   SNR bias pairs that TED with the complex rate. A symbol-rate change that moves the digital_resample decision
 *   designs the output chain again where it applies, so the output rate the decoder predicted is the one it gets.
 */

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/snr_bias.h>
#include <dsd-neo/io/rate_plan.h>
#include <dsd-neo/io/rtl_stream_c.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/rtl_stream_metrics_hooks.h>
#include <memory>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

/* The stream's output-rate getter, which the engine installs as the decoder's output-rate hook. */
extern "C" unsigned int dsd_rtl_stream_output_rate(void);

namespace {

int
expect_eq(const char* label, long long got, long long want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%lld want=%lld\n", label, got, want);
        return 1;
    }
    return 0;
}

int
expect_near(const char* label, long long got, long long want, long long tolerance) {
    if (llabs(got - want) > tolerance) {
        DSD_FPRINTF(stderr, "FAIL: %s: got=%lld want=%lld (+/-%lld)\n", label, got, want, tolerance);
        return 1;
    }
    return 0;
}

/* The channel filter holds back its look-ahead (half its taps, at most DSD_CHANNEL_LPF_MAX_TAPS / 2 input samples),
   so a run emits up to that many fewer outputs than its input gives at the output rate, never more. */
const long long kChannelLookaheadPairs = 144;

int
expect_emitted(const char* label, long long got, long long input_pairs, long long num, long long den) {
    const long long most = input_pairs * num / den;
    const long long least = (input_pairs - kChannelLookaheadPairs) * num / den - 2;
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
nxdn48(dsd_opts* o) {
    o->frame_nxdn48 = 1;
    o->mod_c4fm = 1;
}

void
provoice(dsd_opts* o) {
    o->frame_provoice = 1;
    o->mod_gfsk = 1;
}

/* The open's timing for @p symbol_rate_hz at @p rate_hz: the rounded samples per symbol, clamped to 2..64. */
int
open_ted_sps(int rate_hz, int symbol_rate_hz) {
    const int sps = (rate_hz + symbol_rate_hz / 2) / symbol_rate_hz;
    if (sps < 2) {
        return 2;
    }
    return sps > 64 ? 64 : sps;
}

/* The output rate the FSK discriminator runs at for @p rate_hz and @p symbol_rate_hz under @p mode: off never
   resamples, on always resamples to the 48 kHz target, auto only a rate the device forced whose samples per symbol are
   not whole. */
int
fsk_output_rate(int rate_hz, int symbol_rate_hz, int mode, int forced) {
    if (mode == DSD_DIGITAL_RESAMPLE_OFF || rate_hz == 48000) {
        return rate_hz;
    }
    if (mode == DSD_DIGITAL_RESAMPLE_AUTO && ((rate_hz % symbol_rate_hz) == 0 || !forced)) {
        return rate_hz;
    }
    return 48000;
}

/* --- X10: post-demod decimation of a replay ---------------------------------------------------------------------- */

/* What the decoder reads through the metrics hooks while a post-chain replay runs: the live output rate and the
   family prediction (dsd_rtl_stream_metrics_hook_output_rate_for_family(), which a scan row's timing reads). */
struct hook_reads {
    unsigned int output_rate_hz;
    unsigned int predicted_rate_hz;
};

void
read_metrics_hooks(const rtl_stream_test_post_chain_result* r, void* ctx) {
    hook_reads* reads = static_cast<hook_reads*>(ctx);
    const int analog = r->output_kind == DSD_DEMOD_OUTPUT_AUDIO_MONITOR ? 1 : 0;
    reads->output_rate_hz = dsd_rtl_stream_metrics_hook_output_rate_hz();
    reads->predicted_rate_hz = dsd_rtl_stream_metrics_hook_output_rate_for_family(
        analog ? DSD_RX_FAMILY_ANALOG : DSD_RX_FAMILY_DIGITAL, r->cqpsk_enable, analog ? 0 : 4800, 0);
}

/* The two metrics hooks the decoder times by, wired as the engine installs them
   (dsd_engine_rtl_stream_metrics_hooks_install()). */
void
install_rate_hooks(void) {
    dsd_rtl_stream_metrics_hooks hooks = {};
    hooks.output_rate_hz = dsd_rtl_stream_output_rate;
    hooks.output_rate_for_family = rtl_stream_output_rate_for_family;
    dsd_rtl_stream_metrics_hooks_set(&hooks);
}

int
run_post_chain_at(const char* name, void (*configure)(dsd_opts*), int mode, uint32_t complex_hz, uint32_t post,
                  rtl_stream_test_post_chain_result* r) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    configure(&opts);
    opts.digital_resample_mode = mode;
    /* Heap-held: the stream keeps the probe's context in a global while it is installed. */
    auto held_reads = std::make_unique<hook_reads>();
    hook_reads& reads = *held_reads;
    rtl_stream_test_set_post_chain_probe(read_metrics_hooks, held_reads.get());
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "%s post %u mode %d: hook", name, post, mode);
    int rc =
        expect_eq(label, rtl_stream_test_replay_post_chain(&opts, complex_hz, post, 10, (int)(complex_hz / 10U), r), 0);
    rtl_stream_test_set_post_chain_probe(NULL, NULL);
    DSD_SNPRINTF(label, sizeof label, "%s post %u mode %d: complex rate", name, post, mode);
    rc |= expect_eq(label, r->complex_rate_hz, (long long)complex_hz);
    DSD_SNPRINTF(label, sizeof label, "%s post %u mode %d: rate_out keeps the post-decimated rate", name, post, mode);
    rc |= expect_eq(label, r->rate_out_hz, (long long)(complex_hz / post));
    DSD_SNPRINTF(label, sizeof label, "%s post %u mode %d: the prediction is the rate landed", name, post, mode);
    rc |= expect_eq(label, r->predicted_rate_hz, r->output_rate_hz);
    DSD_SNPRINTF(label, sizeof label, "%s post %u mode %d: the metrics hook reads the landed output rate", name, post,
                 mode);
    rc |= expect_eq(label, reads.output_rate_hz, r->output_rate_hz);
    DSD_SNPRINTF(label, sizeof label, "%s post %u mode %d: the metrics hook's family prediction", name, post, mode);
    rc |= expect_eq(label, reads.predicted_rate_hz, r->output_rate_hz);
    return rc;
}

int
run_post_chain(const char* name, void (*configure)(dsd_opts*), int mode, uint32_t post,
               rtl_stream_test_post_chain_result* r) {
    return run_post_chain_at(name, configure, mode, 48000U * post, post, r);
}

/* The FSK discriminator never post-decimates: its modem, TED and output run at the complex rate, and the samples it
   emits per second are the output rate it publishes (resampled to 48 kHz with digital_resample on). */
int
expect_fsk_post_chain(uint32_t post, int mode) {
    rtl_stream_test_post_chain_result r{};
    char label[160];
    int rc = run_post_chain("DMR", dmr, mode, post, &r);
    const int complex_hz = (int)(48000U * post);
    DSD_SNPRINTF(label, sizeof label, "DMR post %u mode %d: FSK discriminator", post, mode);
    rc |= expect_eq(label, r.output_kind, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR);
    DSD_SNPRINTF(label, sizeof label, "DMR post %u mode %d: FSK modem at the complex rate", post, mode);
    rc |= expect_eq(label, r.fsk_sample_rate_hz, complex_hz);
    DSD_SNPRINTF(label, sizeof label, "DMR post %u mode %d: TED at the complex rate", post, mode);
    rc |= expect_eq(label, r.ted_sps, open_ted_sps(complex_hz, 4800));
    const int want_rate = fsk_output_rate(complex_hz, 4800, mode, 1);
    DSD_SNPRINTF(label, sizeof label, "DMR post %u mode %d: output rate", post, mode);
    rc |= expect_eq(label, r.output_rate_hz, want_rate);
    DSD_SNPRINTF(label, sizeof label, "DMR post %u mode %d: samples emitted at the output rate", post, mode);
    rc |= expect_emitted(label, r.emitted, r.input_pairs, want_rate, complex_hz);
    return rc;
}

/* The CQPSK chain skips the post decimator too: the Gardner loop times symbols at the complex rate, one symbol per
   complex_rate / 4800 input samples, and the decoder is told the complex rate. */
int
expect_cqpsk_post_chain(uint32_t post) {
    rtl_stream_test_post_chain_result r{};
    char label[160];
    int rc = run_post_chain("P25 CQPSK", p25_cqpsk, DSD_DIGITAL_RESAMPLE_AUTO, post, &r);
    const int complex_hz = (int)(48000U * post);
    const int sps = open_ted_sps(complex_hz, 4800);
    DSD_SNPRINTF(label, sizeof label, "P25 CQPSK post %u: CQPSK symbols", post);
    rc |= expect_eq(label, r.output_kind == DSD_DEMOD_OUTPUT_SYMBOL_CQPSK && r.cqpsk_enable == 1, 1);
    DSD_SNPRINTF(label, sizeof label, "P25 CQPSK post %u: TED at the complex rate", post);
    rc |= expect_eq(label, r.ted_sps, sps);
    DSD_SNPRINTF(label, sizeof label, "P25 CQPSK post %u: output rate", post);
    rc |= expect_eq(label, r.output_rate_hz, complex_hz);
    DSD_SNPRINTF(label, sizeof label, "P25 CQPSK post %u: one symbol per %d samples", post, sps);
    rc |= expect_near(label, r.emitted, r.input_pairs / sps, r.input_pairs / sps / 100 + 2);
    return rc;
}

/* Only the audio monitor runs the post decimator: it emits one sample per post_downsample, at rate_out, the output
   rate it publishes. */
int
expect_analog_post_chain(uint32_t post) {
    rtl_stream_test_post_chain_result r{};
    char label[160];
    int rc = run_post_chain("NFM", nfm, DSD_DIGITAL_RESAMPLE_AUTO, post, &r);
    DSD_SNPRINTF(label, sizeof label, "NFM post %u: audio monitor", post);
    rc |= expect_eq(label, r.output_kind, DSD_DEMOD_OUTPUT_AUDIO_MONITOR);
    DSD_SNPRINTF(label, sizeof label, "NFM post %u: output rate", post);
    rc |= expect_eq(label, r.output_rate_hz, 48000);
    DSD_SNPRINTF(label, sizeof label, "NFM post %u: one sample per %u", post, post);
    rc |= expect_emitted(label, r.emitted, r.input_pairs, 1, post);
    return rc;
}

/* A forced rate whose complex stream gives DMR no whole SPS: 156,250 Hz ahead of a post-demod decimation of 2
   (78,125 Hz after it). auto resamples the FSK output to 48 kHz from the complex rate, and the decoder, through the
   metrics hooks, reads and predicts that rate. */
int
expect_forced_resampled_post_chain(void) {
    rtl_stream_test_post_chain_result r{};
    int rc = run_post_chain_at("DMR forced", dmr, DSD_DIGITAL_RESAMPLE_AUTO, 156250U, 2U, &r);
    rc |= expect_eq("DMR forced post 2: FSK modem at the complex rate", r.fsk_sample_rate_hz, 156250);
    rc |= expect_eq("DMR forced post 2: TED at the complex rate", r.ted_sps, open_ted_sps(156250, 4800));
    rc |= expect_eq("DMR forced post 2: resampled to 48 kHz", r.output_rate_hz, 48000);
    rc |= expect_emitted("DMR forced post 2: samples emitted at 48 kHz", r.emitted, r.input_pairs, 48000, 156250);
    return rc;
}

int
test_post_chain(void) {
    install_rate_hooks();
    int rc = expect_forced_resampled_post_chain();
    for (uint32_t post = 1U; post <= 4U; post++) {
        rc |= expect_fsk_post_chain(post, DSD_DIGITAL_RESAMPLE_AUTO);
        rc |= expect_fsk_post_chain(post, DSD_DIGITAL_RESAMPLE_ON);
        rc |= expect_fsk_post_chain(post, DSD_DIGITAL_RESAMPLE_OFF);
        rc |= expect_cqpsk_post_chain(post);
        rc |= expect_analog_post_chain(post);
    }
    dsd_rtl_stream_metrics_hooks_set(NULL);
    return rc;
}

/* --- The bandwidth x symbol rate x digital_resample matrix, Airspy against RTL-SDR -------------------------------- */

struct matrix_profile {
    const char* name;
    void (*configure)(dsd_opts*);
    int symbol_rate_hz;
};

int
expect_matrix_point(const matrix_profile& p, int airspy, int bw_hz, int mode) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    p.configure(&opts);
    opts.digital_resample_mode = mode;
    const int backend = airspy ? DSD_RATE_PLAN_BACKEND_AIRSPY : DSD_RATE_PLAN_BACKEND_RTL_USB;
    rtl_stream_test_live_chain_result r{};
    char label[192];
    DSD_SNPRINTF(label, sizeof label, "%s %s %d Hz mode %d: open", airspy ? "Airspy" : "RTL-SDR", p.name, bw_hz, mode);
    int rc = expect_eq(
        label, rtl_stream_test_live_rate_chain(&opts, backend, bw_hz, airspy ? 2500000U : 0U, 0U, &r) == 0 && r.rc == 0,
        1);
    DSD_SNPRINTF(label, sizeof label, "%s %s %d Hz mode %d: complex rate is the DSP bandwidth",
                 airspy ? "Airspy" : "RTL-SDR", p.name, bw_hz, mode);
    rc |= expect_eq(label, r.complex_rate_hz == bw_hz && r.published_complex_rate_hz == bw_hz, 1);
    DSD_SNPRINTF(label, sizeof label, "%s %s %d Hz mode %d: forced flag", airspy ? "Airspy" : "RTL-SDR", p.name, bw_hz,
                 mode);
    rc |= expect_eq(label, r.forced, airspy);
    DSD_SNPRINTF(label, sizeof label, "%s %s %d Hz mode %d: TED at the complex rate", airspy ? "Airspy" : "RTL-SDR",
                 p.name, bw_hz, mode);
    rc |= expect_eq(label, r.ted_sps, open_ted_sps(bw_hz, p.symbol_rate_hz));
    DSD_SNPRINTF(label, sizeof label, "%s %s %d Hz mode %d: output rate", airspy ? "Airspy" : "RTL-SDR", p.name, bw_hz,
                 mode);
    rc |= expect_eq(label, r.output_rate_hz, fsk_output_rate(bw_hz, p.symbol_rate_hz, mode, airspy));
    return rc;
}

int
test_resample_matrix(void) {
    static const matrix_profile kProfiles[] = {
        {"DMR", dmr, 4800}, {"NXDN48", nxdn48, 2400}, {"ProVoice", provoice, 9600}};
    static const int kBandwidths[] = {12000, 24000, 48000};
    static const int kModes[] = {DSD_DIGITAL_RESAMPLE_AUTO, DSD_DIGITAL_RESAMPLE_ON, DSD_DIGITAL_RESAMPLE_OFF};
    int rc = 0;
    for (const matrix_profile& p : kProfiles) {
        for (const int bw : kBandwidths) {
            for (const int mode : kModes) {
                rc |= expect_matrix_point(p, 1, bw, mode);
                rc |= expect_matrix_point(p, 0, bw, mode);
            }
        }
    }
    /* The points above include the documented change: an Airspy at 24 kHz runs DMR at a whole 5 samples per symbol,
       unresampled, as an RTL-SDR at 24 kHz does; at 12 kHz the device still forced the rate, so auto resamples 2.5
       samples per symbol to 48 kHz where an RTL-SDR does not. */
    return rc;
}

/* --- X11: the TED a symbol profile lands on, and the output chain a symbol-rate change moves ---------------------- */

struct profile_expect {
    int ted_sps;
    int ted_sps_override;
    int output_kind;
    int output_rate_hz;
};

int
expect_step(const char* label, const rtl_stream_test_profile_step_result& r, const profile_expect& want,
            int complex_hz) {
    char name[224];
    int rc = 0;
    DSD_SNPRINTF(name, sizeof name, "%s: taken", label);
    rc |= expect_eq(name, r.taken, 1);
    DSD_SNPRINTF(name, sizeof name, "%s: TED", label);
    rc |= expect_eq(name, r.ted_sps, want.ted_sps);
    DSD_SNPRINTF(name, sizeof name, "%s: TED override", label);
    rc |= expect_eq(name, r.ted_sps_override, want.ted_sps_override);
    DSD_SNPRINTF(name, sizeof name, "%s: output kind", label);
    rc |= expect_eq(name, r.output_kind, want.output_kind);
    DSD_SNPRINTF(name, sizeof name, "%s: output rate", label);
    rc |= expect_eq(name, r.output_rate_hz, want.output_rate_hz);
    DSD_SNPRINTF(name, sizeof name, "%s: published TED and complex rate", label);
    rc |= expect_eq(name, r.published_ted_sps == want.ted_sps && r.published_complex_rate_hz == complex_hz, 1);
    /* The SNR bias reads the symbol rate as complex rate / TED: the TED is the one that rate runs. */
    const double want_bias = dsd_snr_bias_evm_db(complex_hz, want.ted_sps, r.channel_profile, r.published_tetra_stage);
    DSD_SNPRINTF(name, sizeof name, "%s: SNR bias pairs the TED with the complex rate", label);
    rc |= expect_eq(name, std::fabs(r.snr_bias_evm_db - want_bias) < 1e-9 ? 1 : 0, 1);
    return rc;
}

/* An Airspy (forced) or RTL-SDR at a 24 kHz DSP bandwidth opened on DMR under @p mode. The decoder rotates its SPS hunt
   to ProVoice and back to DMR (live requests timed for the output rate it read), lands the same through retune
   profiles, and a P25 control channel republishes a CQPSK profile with a TED its decoder timed for 48 kHz. Each lands
   the TED of the 24 kHz complex rate and, on the FSK discriminator, the output rate the digital_resample decision for
   its symbol rate gives, which is the rate the decoder predicted before it asked. */
int
expect_profile_rotation(int airspy, int mode) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    dmr(&opts);
    opts.digital_resample_mode = mode;
    const int complex_hz = 24000;
    const int pv_rate = fsk_output_rate(complex_hz, 9600, mode, airspy);
    const int dmr_rate = fsk_output_rate(complex_hz, 4800, mode, airspy);
    const rtl_stream_test_profile_step steps[] = {
        {0, 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE, open_ted_sps(dmr_rate, 9600), 0, 0},
        {0, 4800, 4, DSD_CH_LPF_PROFILE_12K5, open_ted_sps(pv_rate, 4800), 0, 0},
        {0, 9600, 2, DSD_CH_LPF_PROFILE_PROVOICE, open_ted_sps(dmr_rate, 9600), 0, 1},
        {0, 4800, 4, DSD_CH_LPF_PROFILE_12K5, open_ted_sps(pv_rate, 4800), 0, 1},
        {1, 4800, 4, DSD_CH_LPF_PROFILE_P25_CQPSK, open_ted_sps(48000, 4800), 0, 0},
    };
    const profile_expect want[] = {
        {open_ted_sps(complex_hz, 9600), 0, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, pv_rate},
        {open_ted_sps(complex_hz, 4800), 0, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, dmr_rate},
        {open_ted_sps(complex_hz, 9600), 0, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, pv_rate},
        {open_ted_sps(complex_hz, 4800), 0, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, dmr_rate},
        {open_ted_sps(complex_hz, 4800), 0, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, -1},
    };
    static const char* const kStepNames[] = {"hunt to ProVoice", "hunt back to DMR", "ProVoice retune", "DMR retune",
                                             "P25 CQPSK republish"};
    const size_t n = sizeof steps / sizeof steps[0];
    rtl_stream_test_live_chain_result open{};
    rtl_stream_test_profile_step_result r[sizeof steps / sizeof steps[0]];
    const int backend = airspy ? DSD_RATE_PLAN_BACKEND_AIRSPY : DSD_RATE_PLAN_BACKEND_RTL_USB;
    char label[192];
    DSD_SNPRINTF(label, sizeof label, "%s mode %d: rotation hook", airspy ? "Airspy" : "RTL-SDR", mode);
    int rc = expect_eq(label,
                       rtl_stream_test_live_chain_profile_steps(&opts, backend, complex_hz, airspy ? 2500000U : 0U,
                                                                steps, n, &open, r),
                       0);
    DSD_SNPRINTF(label, sizeof label, "%s mode %d: DMR open", airspy ? "Airspy" : "RTL-SDR", mode);
    rc |= expect_eq(label, open.rc == 0 && (int)open.output_rate_hz == dmr_rate && open.ted_sps == 5, 1);
    unsigned int previous_rate_hz = open.output_rate_hz;
    for (size_t i = 0; i < n; i++) {
        profile_expect w = want[i];
        /* A step that moved the output rate dropped the ring the decoder reads (a new output generation), so the
           decoder re-times from the rate its next samples come at. */
        if (r[i].output_rate_hz != previous_rate_hz) {
            DSD_SNPRINTF(label, sizeof label, "%s mode %d: %s moves the output generation with the rate",
                         airspy ? "Airspy" : "RTL-SDR", mode, kStepNames[i]);
            rc |= expect_eq(label, r[i].generation_moved, 1);
        }
        previous_rate_hz = r[i].output_rate_hz;
        if (w.output_rate_hz < 0) {
            /* CQPSK keeps the output chain a CQPSK toggle finds: it never resamples its symbols. */
            w.output_rate_hz = (int)r[i].output_rate_hz;
        }
        DSD_SNPRINTF(label, sizeof label, "%s mode %d: %s", airspy ? "Airspy" : "RTL-SDR", mode, kStepNames[i]);
        rc |= expect_step(label, r[i], w, complex_hz);
        if (w.output_kind == DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR) {
            DSD_SNPRINTF(label, sizeof label, "%s mode %d: %s lands the predicted rate", airspy ? "Airspy" : "RTL-SDR",
                         mode, kStepNames[i]);
            rc |= expect_eq(label, r[i].output_rate_hz, r[i].predicted_rate_hz);
        }
    }
    return rc;
}

/* A P25 control channel at a 24 kHz Airspy DSP bandwidth: the voice channel's retune carries a Phase 2 TED override
   (6000 sym/s, 4 samples per symbol), which stands; the control channel's snapshot restore, after the voice tune or
   after it failed, republishes the CQPSK profile with the TED the snapshot saved before (10, from a 48 kHz session),
   which the stream times for the complex rate instead, clearing the override. */
int
expect_p25_vc_and_restore(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    p25_cqpsk(&opts);
    const rtl_stream_test_profile_step steps[] = {
        {1, 6000, 4, DSD_CH_LPF_PROFILE_P25_CQPSK, 4, 1, 1},
        {1, 4800, 4, DSD_CH_LPF_PROFILE_P25_CQPSK, 10, 0, 0},
        {1, 6000, 4, DSD_CH_LPF_PROFILE_P25_CQPSK, 4, 1, 1},
        {1, 4800, 4, DSD_CH_LPF_PROFILE_P25_CQPSK, 10, 0, 1},
    };
    const profile_expect want[] = {
        {4, 4, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 24000},
        {5, 0, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 24000},
        {4, 4, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 24000},
        {5, 0, DSD_DEMOD_OUTPUT_SYMBOL_CQPSK, 24000},
    };
    static const char* const kStepNames[] = {"P25 VC override", "CC snapshot restore", "P25 VC override again",
                                             "CC restore through the return retune"};
    const size_t n = sizeof steps / sizeof steps[0];
    rtl_stream_test_live_chain_result open{};
    rtl_stream_test_profile_step_result r[sizeof steps / sizeof steps[0]];
    int rc = expect_eq("P25 CC/VC hook",
                       rtl_stream_test_live_chain_profile_steps(&opts, DSD_RATE_PLAN_BACKEND_AIRSPY, 24000, 2500000U,
                                                                steps, n, &open, r),
                       0);
    rc |= expect_eq("P25 CC open", open.rc == 0 && open.ted_sps == 5 && open.output_rate_hz == 24000U, 1);
    for (size_t i = 0; i < n; i++) {
        rc |= expect_step(kStepNames[i], r[i], want[i], 24000);
    }
    return rc;
}

/* A retune profile with a TED and no symbol profile (a Signal search hand-off queues the decoder's samples per symbol
   alone) keeps the symbol rate the stream runs and is timed for the complex rate too: an Airspy at 24 kHz on DMR,
   queued 10 for a 48 kHz decoder, runs 5. */
int
expect_ted_only_retune(void) {
    static dsd_opts opts;
    DSD_MEMSET(&opts, 0, sizeof opts);
    dmr(&opts);
    const rtl_stream_test_profile_step steps[] = {{-1, 0, 0, -1, 10, 0, 1}};
    rtl_stream_test_live_chain_result open{};
    rtl_stream_test_profile_step_result r[1];
    int rc = expect_eq("TED-only retune hook",
                       rtl_stream_test_live_chain_profile_steps(&opts, DSD_RATE_PLAN_BACKEND_AIRSPY, 24000, 2500000U,
                                                                steps, 1U, &open, r),
                       0);
    rc |= expect_eq("TED-only retune: symbol rate kept", r[0].symbol_rate_hz, 4800);
    rc |= expect_step("TED-only retune", r[0], {5, 0, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, 24000}, 24000);
    return rc;
}

/* A -Y scan on a forced 78,125 Hz chain (the FSK output resampled to 48 kHz under auto and on, kept under off) walks
   an NFM row, a DMR row, the NFM row again and the DMR row again, each retune attaching its family. The analog rows
   run the monitor at its 48 kHz output; each DMR row lands where the decoder's prediction said, with the TED of the
   78,125 Hz complex rate (16), never the 48 kHz one the row's profile was queued with (10). */
int
expect_y_scan_transitions(int mode) {
    const rtl_stream_test_retune_step nfm_row = {DSD_RX_FAMILY_ANALOG,
                                                 DSD_ANALOG_DEMOD_FM,
                                                 0,
                                                 RTL_STREAM_TEST_SYMBOL_NONE,
                                                 -1,
                                                 -1,
                                                 RTL_STREAM_TEST_QUEUED_NONE,
                                                 0};
    const rtl_stream_test_retune_step dmr_row = {DSD_RX_FAMILY_DIGITAL,
                                                 DSD_ANALOG_DEMOD_FM,
                                                 0,
                                                 RTL_STREAM_TEST_SYMBOL_DMR_FSK,
                                                 -1,
                                                 -1,
                                                 RTL_STREAM_TEST_QUEUED_NONE,
                                                 0};
    const rtl_stream_test_retune_step steps[] = {nfm_row, dmr_row, nfm_row, dmr_row};
    rtl_stream_test_retune_landing r[4];
    char label[160];
    DSD_SNPRINTF(label, sizeof label, "-Y mode %d: hook", mode);
    int rc = expect_eq(label, rtl_stream_test_retune_profile_sequence_mode(steps, 4U, 78125, mode, 0U, r), 0);
    const int dmr_rate = fsk_output_rate(78125, 4800, mode, 1);
    for (int i = 0; i < 4; i++) {
        const int digital = (i % 2) == 1;
        DSD_SNPRINTF(label, sizeof label, "-Y mode %d: row %d taken", mode, i + 1);
        rc |= expect_eq(label, r[i].taken, 1);
        DSD_SNPRINTF(label, sizeof label, "-Y mode %d: row %d family", mode, i + 1);
        rc |= expect_eq(label, r[i].applied_family, digital ? 0 : 1);
        DSD_SNPRINTF(label, sizeof label, "-Y mode %d: row %d output rate", mode, i + 1);
        rc |= expect_eq(label, r[i].applied_output_rate, digital ? dmr_rate : 48000);
        if (digital) {
            DSD_SNPRINTF(label, sizeof label, "-Y mode %d: row %d FSK discriminator", mode, i + 1);
            rc |= expect_eq(label, r[i].applied_output_kind, DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR);
            DSD_SNPRINTF(label, sizeof label, "-Y mode %d: row %d lands the prediction", mode, i + 1);
            rc |= expect_eq(label, r[i].applied_output_rate, r[i].predicted_output_rate);
            DSD_SNPRINTF(label, sizeof label, "-Y mode %d: row %d TED at the complex rate", mode, i + 1);
            rc |= expect_eq(label, r[i].applied_ted_sps, open_ted_sps(78125, 4800));
        }
    }
    return rc;
}

int
test_profile_timing(void) {
    int rc = 0;
    static const int kModes[] = {DSD_DIGITAL_RESAMPLE_AUTO, DSD_DIGITAL_RESAMPLE_ON, DSD_DIGITAL_RESAMPLE_OFF};
    for (const int mode : kModes) {
        rc |= expect_profile_rotation(1, mode);
        rc |= expect_profile_rotation(0, mode);
    }
    rc |= expect_p25_vc_and_restore();
    rc |= expect_ted_only_retune();
    for (const int mode : kModes) {
        rc |= expect_y_scan_transitions(mode);
    }
    return rc;
}

} // namespace

int
main(void) {
    dsd_neo_config_init();
    int rc = test_post_chain();
    rc |= test_resample_matrix();
    rc |= test_profile_timing();
    if (rc == 0) {
        std::printf("IO_RTL_COMPLEX_RATE: OK\n");
    }
    return rc == 0 ? 0 : 1;
}
