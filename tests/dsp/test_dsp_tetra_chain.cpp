// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The TETRA receive chain inside full_demod(): the activation predicate (dsd_demod_tetra_chain_wanted()) and the
 * transitions full_demod() runs when it changes between blocks, the chain's order (mixer, channel LPF, AGC 0.1,
 * matched filter, FLL and transfer), its output's independence of where the input is cut into blocks (behind the
 * half-band cascade too, whichever buffer that leaves the block in) and the FLL's gear, the squelch and signal-search
 * behaviour, the once-per-stream warnings for a TETRA profile whose chain cannot run, the pull-in on a
 * clean synthetic stream, and a P25 stream after a TETRA session running exactly as a fresh one, and an uplink session,
 * which keeps only the TETRA channel filter. The tests force the TETRA profile on the demodulator directly.
 */

#include <atomic>
#include <cmath>
#include <cstring>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/costas.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/firdes.h>
#include <dsd-neo/dsp/snr_bias.h>
#include <dsd-neo/dsp/ted.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/mem.h>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include "demod_pipeline_test_support.h"
#include "dsd-neo/core/safe_api.h"

namespace {

const double kPi = 3.14159265358979323846;

double
hz_to_rad(double hz, int rate) {
    return 2.0 * kPi * hz / (double)rate;
}

double
rad_to_hz(double rad, int rate) {
    return rad * (double)rate / (2.0 * kPi);
}

double
wrap_pi(double a) {
    while (a > kPi) {
        a -= 2.0 * kPi;
    }
    while (a < -kPi) {
        a += 2.0 * kPi;
    }
    return a;
}

int
fail(const char* what) {
    DSD_FPRINTF(stderr, "TETRA chain: %s\n", what);
    return 1;
}

int
expect_near(const char* what, double got, double want, double tol) {
    if (!(std::fabs(got - want) <= tol)) {
        DSD_FPRINTF(stderr, "TETRA chain: %s: got %.9g want %.9g (tol %.3g)\n", what, got, want, tol);
        return 1;
    }
    return 0;
}

int
expect_int(const char* what, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "TETRA chain: %s: got %d want %d\n", what, got, want);
        return 1;
    }
    return 0;
}

/* The warnings a stream logs about a TETRA profile whose chain cannot run, counted by cause. */
struct WarnCounts {
    int total;
    int rate;
    int ted;
    int lpf;
};

WarnCounts g_warns;

void
count_tetra_warnings(dsd_neo_log_level_t level, const char* text, void* ctx) {
    (void)ctx;
    if (level != LOG_LEVEL_WARN || !text || !std::strstr(text, "TETRA receive chain inactive")) {
        return;
    }
    g_warns.total++;
    if (std::strstr(text, "DSP rate")) {
        g_warns.rate++;
    }
    if (std::strstr(text, "samples per symbol")) {
        g_warns.ted++;
    }
    if (std::strstr(text, "channel filter is off")) {
        g_warns.lpf++;
    }
}

demod_state*
new_demod(void) {
    demod_state* s = static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)));
    if (s) {
        DSD_MEMSET(s, 0, sizeof(*s));
    }
    return s;
}

void
free_demod(demod_state* s) {
    if (!s) {
        return;
    }
    dsd_neo_aligned_free(s->post_polydecim_taps);
    dsd_neo_aligned_free(s->post_polydecim_hist);
    dsd_neo_aligned_free(s);
}

/* A CQPSK stream on the TETRA profile at @p rate: 18 ksym/s, the sps the rate gives, the channel LPF on, no squelch. */
void
configure_cqpsk(demod_state* s, int rate, int profile, int symbol_rate, int sps) {
    s->rate_in = rate;
    s->rate_out = rate;
    s->rate_out2 = 0;
    s->mode_demod = &dsd_fm_demod;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    s->cqpsk_enable = 1;
    s->ted_enabled = 1;
    s->symbol_rate_hz = symbol_rate;
    s->symbol_levels = 4;
    s->ted_sps = sps;
    s->sps_is_integer = 1;
    s->channel_lpf_enable = 1;
    s->channel_lpf_profile = profile;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->squelch_mode = DSD_SQUELCH_MODE_LEVEL;
    s->channel_squelch_level.store(0.0f);
    s->squelch_env = 1.0f;
    s->squelch_env_attack = 0.125f;
    s->squelch_env_release = 0.03125f;
    ted_init_state(&s->ted_state);
}

void
configure_tetra(demod_state* s, int rate) {
    configure_cqpsk(s, rate, DSD_CH_LPF_PROFILE_TETRA, 18000, rate / 18000);
}

/* One block through full_demod() from @p buf (the input buffer unless a test names another); returns the symbols it
 * made. */
std::vector<float>
run_block(demod_state* s, const float* iq, int pairs, float* buf = nullptr) {
    float* in = buf ? buf : s->input_cb_buf;
    DSD_MEMCPY(in, iq, (size_t)pairs * 2U * sizeof(float));
    s->lowpassed = in;
    s->lp_len = pairs * 2;
    full_demod(s);
    std::vector<float> out;
    if (s->result_len > 0 && !s->front_end_empty) {
        out.assign(s->result, s->result + s->result_len);
    }
    return out;
}

std::vector<float>
constant_block(int pairs, float re, float im) {
    std::vector<float> iq((size_t)pairs * 2U);
    for (int k = 0; k < pairs; k++) {
        iq[(size_t)k * 2U] = re;
        iq[((size_t)k * 2U) + 1U] = im;
    }
    return iq;
}

struct Lcg {
    uint32_t s;

    uint32_t
    next() {
        s = s * 1664525u + 1013904223u;
        return s;
    }

    double
    uniform() {
        return (double)(next() >> 8) * (1.0 / 16777216.0);
    }
};

/* A clean pi/4-DQPSK stream at 18 ksym/s and @p rate, shaped by the TETRA root-raised-cosine (alpha 0.35), from a
 * fixed seeded dibit sequence, amplitude about @p amp, on a carrier @p offset_hz above the tuned centre. */
std::vector<float>
make_tetra_stream(int rate, double offset_hz, int pairs, float amp, uint32_t seed) {
    const int sps = rate / 18000;
    std::vector<float> rrc((size_t)DSD_TETRA_RRC_MAX_TAPS);
    const int taps = dsd_tetra_rrc_design(sps, rrc.data(), DSD_TETRA_RRC_MAX_TAPS);
    std::vector<float> iq((size_t)pairs * 2U, 0.0f);
    if (taps <= 0) {
        return iq;
    }
    const int c = (taps - 1) / 2;
    const int symbols = pairs / sps + taps;
    std::vector<double> sym_r((size_t)symbols);
    std::vector<double> sym_i((size_t)symbols);
    static const double kSteps[4] = {kPi / 4.0, 3.0 * kPi / 4.0, -kPi / 4.0, -3.0 * kPi / 4.0};
    Lcg rng{seed};
    double phase = 0.0;
    for (int k = 0; k < symbols; k++) {
        phase += kSteps[(rng.next() >> 16) & 3u];
        sym_r[(size_t)k] = std::cos(phase);
        sym_i[(size_t)k] = std::sin(phase);
    }
    /* Impulses every sps samples, convolved with the RRC (gain sps so a symbol's peak is about 1). */
    for (int n = 0; n < pairs; n++) {
        double re = 0.0;
        double im = 0.0;
        for (int t = 0; t < taps; t++) {
            const int m = n + c - t; /* the impulse train index this tap reads */
            if (m < 0 || (m % sps) != 0) {
                continue;
            }
            const int k = m / sps;
            if (k >= symbols) {
                continue;
            }
            re += (double)rrc[(size_t)t] * sym_r[(size_t)k];
            im += (double)rrc[(size_t)t] * sym_i[(size_t)k];
        }
        re *= (double)sps * (double)amp;
        im *= (double)sps * (double)amp;
        const double w = 2.0 * kPi * offset_hz * (double)n / (double)rate;
        const double cr = std::cos(w);
        const double ci = std::sin(w);
        iq[(size_t)n * 2U] = (float)(re * cr - im * ci);
        iq[((size_t)n * 2U) + 1U] = (float)(re * ci + im * cr);
    }
    return iq;
}

/* --- The predicate ------------------------------------------------------------------------------------------ */

int
test_predicate_truth_table(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    rc |= expect_int("NULL demod", dsd_demod_tetra_chain_wanted(NULL), 0);
    for (int rate = 36000; rate <= 144000; rate += 18000) {
        DSD_MEMSET(s, 0, sizeof(*s));
        configure_tetra(s, rate);
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "valid TETRA stream at %d Hz", rate);
        rc |= expect_int(label, dsd_demod_tetra_chain_wanted(s), 1);
    }

    struct Row {
        const char* what;
        int rate;
        int profile;
        int cqpsk;
        int symbol_rate;
        int ted_sps;
        int override_sps;
        int lpf;
        int want;
    };

    static const Row rows[] = {
        {"72 kHz baseline", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 4, 0, 1, 1},
        {"WIDE profile", 72000, DSD_CH_LPF_PROFILE_WIDE, 1, 18000, 4, 0, 1, 0},
        {"P25 CQPSK profile", 72000, DSD_CH_LPF_PROFILE_P25_CQPSK, 1, 18000, 4, 0, 1, 0},
        {"CQPSK off", 72000, DSD_CH_LPF_PROFILE_TETRA, 0, 18000, 4, 0, 1, 0},
        {"4800 sym/s", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 4800, 4, 0, 1, 0},
        {"sps 5 at 72 kHz", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 5, 0, 1, 0},
        {"override 8 at 72 kHz (in range, inconsistent)", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 4, 8, 1, 0},
        /* The Gardner TED and the FLL run at ted_sps (an override reaches it only at the next rate refresh), so an
           override the rate agrees with does not run the chain until ted_sps agrees too. */
        {"override 4 at 72 kHz over ted_sps 10", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 10, 4, 1, 0},
        {"override 4 at 72 kHz over ted_sps 4", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 4, 4, 1, 1},
        {"override 16 at 72 kHz", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 4, 16, 1, 0},
        {"override 16 at 288 kHz", 288000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 16, 16, 1, 0},
        {"18 kHz (sps 1)", 18000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 1, 0, 1, 0},
        {"162 kHz (sps 9)", 162000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 9, 0, 1, 0},
        {"48 kHz (no whole sps)", 48000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 3, 0, 1, 0},
        {"channel LPF off", 72000, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 4, 0, 0, 0},
        {"rate 0", 0, DSD_CH_LPF_PROFILE_TETRA, 1, 18000, 4, 0, 1, 0},
    };

    for (const Row& r : rows) {
        DSD_MEMSET(s, 0, sizeof(*s));
        configure_cqpsk(s, r.rate, r.profile, r.symbol_rate, r.ted_sps);
        s->cqpsk_enable = r.cqpsk;
        s->ted_sps_override = r.override_sps;
        s->channel_lpf_enable = r.lpf;
        rc |= expect_int(r.what, dsd_demod_tetra_chain_wanted(s), r.want);
    }

    /* The plan is checked for the rate, not read from the installed plan: a stale plan for another rate and profile
     * changes nothing. */
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_tetra(s, 72000);
    s->channel_lpf_plan_rate_out = 48000;
    s->channel_lpf_plan_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->channel_lpf_plan_taps_len = 0;
    rc |= expect_int("stale plan for another rate", dsd_demod_tetra_chain_wanted(s), 1);
    free_demod(s);
    return rc;
}

/* A stream open evaluates the predicate before any block (dsd_demod_update_tetra_chain()): it enters the chain then,
 * as the first block's boundary would, splitting the estimate and loading the TETRA FLL. The first block then runs no
 * transition: its symbols are the same as a stream that entered at that block, and a further evaluation with nothing
 * changed leaves the histories the blocks built alone. */
int
test_open_evaluates_chain(void) {
    int rc = 0;
    demod_state* a = new_demod();
    demod_state* b = new_demod();
    if (!a || !b) {
        free_demod(a);
        free_demod(b);
        return fail("alloc");
    }
    configure_tetra(a, 72000);
    configure_tetra(b, 72000);
    a->fll_band_edge_state.freq = (float)hz_to_rad(-3000.0, 72000);
    b->fll_band_edge_state.freq = (float)hz_to_rad(-3000.0, 72000);
    dsd_demod_update_tetra_chain(a);
    rc |= expect_int("open: chain entered before any block", a->tetra_chain, 1);
    rc |= expect_near("open: FLL rolloff", a->fll_band_edge_state.rolloff, 0.35, 1e-6);
    rc |= expect_near("open: estimate in the mixer (Hz)", rad_to_hz(a->fll_band_edge_state.pre_freq, 72000), -3000.0,
                      0.05);
    rc |= expect_int("open: memo for the rate", a->tetra_design_rate_out, 72000);
    const std::vector<float> stream = make_tetra_stream(72000, 3000.0, 1440 * 6, 0.3f, 0x4321u);
    for (int k = 0; k < 6; k++) {
        const std::vector<float> sa = run_block(a, stream.data() + (size_t)k * 2880U, 1440);
        const std::vector<float> sb = run_block(b, stream.data() + (size_t)k * 2880U, 1440);
        if (sa != sb) {
            rc |= fail("open: symbols differ from a stream that entered at its first block");
            break;
        }
    }
    rc |= expect_int("open: still active", a->tetra_chain, 1);
    const int pending = a->tetra_rrc_pending;
    const float agc = a->cqpsk_agc_avg;
    const float hist = a->tetra_rrc_hist_i[DSD_TETRA_RRC_HIST_LEN - 1];
    dsd_demod_update_tetra_chain(a);
    rc |= expect_int("again: matched filter pending kept", a->tetra_rrc_pending, pending);
    rc |= expect_near("again: AGC average kept", a->cqpsk_agc_avg, agc, 0.0);
    rc |= expect_near("again: matched filter history kept", a->tetra_rrc_hist_i[DSD_TETRA_RRC_HIST_LEN - 1], hist, 0.0);
    if (std::fabs((double)agc - 1.0) < 1e-9) {
        rc |= fail("again: the blocks left the AGC average at its reset value, so the check proves nothing");
    }
    free_demod(a);
    free_demod(b);
    return rc;
}

/* The TETRA channel design's realizability is memoized by rate in the demod state: the block boundary checks it once
 * per rate and the predicate reads the memo, so no block designs the filter again. A memo for another rate is never
 * read, and a rate change refreshes it at the next boundary. */
int
test_design_memo(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, 72000);
    std::vector<float> block = make_tetra_stream(72000, 0.0, 1440, 0.3f, 0x2468u);
    (void)run_block(s, block.data(), 1440);
    rc |= expect_int("memo: rate", s->tetra_design_rate_out, 72000);
    rc |= expect_int("memo: realizable", s->tetra_design_ok, 1);
    s->tetra_design_ok = 0;
    rc |= expect_int("memo: the predicate reads it", dsd_demod_tetra_chain_wanted(s), 0);
    s->tetra_design_rate_out = 36000;
    rc |= expect_int("memo: one for another rate is not read", dsd_demod_tetra_chain_wanted(s), 1);
    s->tetra_design_rate_out = 72000;
    s->tetra_design_ok = 1;
    configure_tetra(s, 144000);
    block = make_tetra_stream(144000, 0.0, 2880, 0.3f, 0x2469u);
    (void)run_block(s, block.data(), 2880);
    rc |= expect_int("memo: refreshed for a new rate", s->tetra_design_rate_out, 144000);
    rc |= expect_int("memo: realizable at 144 kHz", s->tetra_design_ok, 1);
    rc |= expect_int("memo: chain active at 144 kHz", s->tetra_chain, 1);
    free_demod(s);
    return rc;
}

/* The CQPSK timing sps the Gardner TED, the FLL and the TETRA transition all run at: ted_sps, or 5 before one is
 * set. */
int
test_cqpsk_timing_sps(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    rc |= expect_int("timing sps: unset", dsd_cqpsk_timing_sps(s), 5);
    s->ted_sps = 4;
    rc |= expect_int("timing sps: ted_sps", dsd_cqpsk_timing_sps(s), 4);
    s->ted_sps_override = 8;
    rc |= expect_int("timing sps: an override not yet applied", dsd_cqpsk_timing_sps(s), 4);
    rc |= expect_int("timing sps: NULL", dsd_cqpsk_timing_sps(NULL), 5);
    free_demod(s);
    return rc;
}

/* A cold start (nothing planned, tetra_chain 0) is active after its first block's boundary: the TETRA channel design
 * and the matched filter are installed and the FLL carries the TETRA parameters. */
int
test_cold_start_activates(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, 72000);
    const std::vector<float> block = make_tetra_stream(72000, 0.0, 1440, 0.3f, 0x1234u);
    (void)run_block(s, block.data(), 1440);
    static float want[DSD_CHANNEL_LPF_MAX_TAPS];
    const int want_len = dsd_tetra_channel_design(72000, want, DSD_CHANNEL_LPF_MAX_TAPS);
    rc |= expect_int("cold start: chain active", s->tetra_chain, 1);
    rc |= expect_int("cold start: TETRA plan key", s->channel_lpf_plan_tetra, 1);
    rc |= expect_int("cold start: TETRA design taps", s->channel_lpf_plan_taps_len, want_len);
    if (want_len > 0 && std::memcmp(s->channel_lpf_plan_taps, want, (size_t)want_len * sizeof(float)) != 0) {
        rc |= fail("cold start: installed plan is not the TETRA design");
    }
    rc |= expect_int("cold start: matched filter taps", s->tetra_rrc_taps_len, 65);
    rc |= expect_int("cold start: matched filter sps", s->tetra_rrc_sps, 4);
    rc |= expect_near("cold start: FLL rolloff", s->fll_band_edge_state.rolloff, 0.35, 1e-6);
    rc |= expect_near("cold start: FLL loop scale", s->fll_band_edge_state.loop_bw_scale, 4.0, 1e-6);
    if (s->result_len <= 0) {
        rc |= fail("cold start: no symbols");
    }
    free_demod(s);
    return rc;
}

/* The AGC averages with 0.1 on the TETRA chain and 0.45 elsewhere: from the open value 1.0, after n samples of
 * constant |x|^2 = m the average is m + (1 - m)(1 - alpha)^n. */
int
test_agc_alpha_wiring(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    const int n = 8;
    const float amp = 0.5f; /* m = 0.25 */
    const double m = (double)amp * (double)amp;
    double avg_by_chain[2] = {0.0, 0.0};
    for (int chain = 0; chain <= 1; chain++) {
        DSD_MEMSET(s, 0, sizeof(*s));
        s->tetra_chain = chain;
        s->cqpsk_agc_avg = 1.0f;
        for (int k = 0; k < n; k++) {
            s->input_cb_buf[(size_t)k * 2] = amp;
            s->input_cb_buf[((size_t)k * 2) + 1] = 0.0f;
        }
        s->lowpassed = s->input_cb_buf;
        s->lp_len = n * 2;
        dsd_demod_test_cqpsk_rms_agc(s);
        const double alpha = chain ? 0.1 : 0.45;
        const double want = m + (1.0 - m) * std::pow(1.0 - alpha, (double)n);
        avg_by_chain[chain] = (double)s->cqpsk_agc_avg;
        rc |= expect_near(chain ? "AGC average, TETRA chain" : "AGC average, P25 chain", s->cqpsk_agc_avg, want, 1e-5);
    }
    if (std::fabs(avg_by_chain[0] - avg_by_chain[1]) < 1e-3) {
        rc |= fail("AGC trajectory does not depend on the chain");
    }
    free_demod(s);
    return rc;
}

/* --- Transitions -------------------------------------------------------------------------------------------- */

/* Puts @p s on an active TETRA chain at 72 kHz (one block run), then sets its estimate to pre + freq and its phasor and
 * AGC average to values a reset would change. */
int
make_active(demod_state* s, double pre_hz, double freq_hz) {
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_tetra(s, 72000);
    const std::vector<float> block = make_tetra_stream(72000, 0.0, 1440, 0.3f, 0x77u);
    (void)run_block(s, block.data(), 1440);
    if (!s->tetra_chain) {
        return fail("could not make an active chain");
    }
    s->fll_band_edge_state.pre_freq = (float)hz_to_rad(pre_hz, 72000);
    s->fll_band_edge_state.freq = (float)hz_to_rad(freq_hz, 72000);
    s->fll_band_edge_state.pre_re = 0.6f;
    s->fll_band_edge_state.pre_im = 0.8f;
    s->cqpsk_agc_avg = 0.3f;
    return 0;
}

void
squelch_next_blocks(demod_state* s, int closed) {
    s->channel_squelch_level.store(closed ? 1000.0f : 0.0f);
}

int
expect_reset_histories(const char* what, const demod_state* s) {
    int rc = 0;
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "%s: AGC average starts over", what);
    rc |= expect_near(label, s->cqpsk_agc_avg, 1.0, 1e-7);
    DSD_SNPRINTF(label, sizeof label, "%s: matched filter pending", what);
    rc |= expect_int(label, s->tetra_rrc_pending, 0);
    for (int k = 0; k < DSD_TETRA_RRC_HIST_LEN; k++) {
        if (s->tetra_rrc_hist_i[k] != 0.0f || s->tetra_rrc_hist_q[k] != 0.0f) {
            DSD_SNPRINTF(label, sizeof label, "%s: matched filter history kept a sample", what);
            rc |= fail(label);
            break;
        }
    }
    return rc;
}

/* Entering (a closed block, so nothing after the channel filter runs): the combined estimate is split into the mixer
 * up to its bound and the FLL rest, the FLL takes the TETRA parameters, and the mixer phasor, the channel LPF, the
 * matched filter and the AGC average start over. The closed block still premixes and runs the channel LPF. */
int
test_entering_splits(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, 72000);
    s->ted_sps = 4;
    dsd_fll_band_edge_init(&s->fll_band_edge_state, 4); /* the P25 parameters, as an inactive TETRA stream has */
    s->fll_band_edge_state.freq = (float)hz_to_rad(10000.0, 72000);
    s->fll_band_edge_state.pre_re = 0.6f;
    s->fll_band_edge_state.pre_im = 0.8f;
    s->cqpsk_agc_avg = 0.3f;
    s->tetra_rrc_pending = 7;
    for (int k = 0; k < DSD_TETRA_RRC_HIST_LEN; k++) {
        s->tetra_rrc_hist_i[k] = 1.0f;
    }
    for (int k = 0; k < DSD_CHANNEL_LPF_HIST_LEN; k++) {
        s->channel_lpf_hist_i[k] = 1.0f;
        s->channel_lpf_hist_q[k] = 1.0f;
    }
    s->channel_lpf_pending = 3;
    squelch_next_blocks(s, 1);
    const int pairs = 100; /* fewer than the channel history holds: its oldest entries show whether it was reset */
    const std::vector<float> block = constant_block(pairs, 0.01f, 0.0f);
    const std::vector<float> out = run_block(s, block.data(), pairs);
    const dsd_fll_band_edge_state_t* f = &s->fll_band_edge_state;
    rc |= expect_int("entering: chain active", s->tetra_chain, 1);
    rc |= expect_int("entering: block squelched", s->channel_squelched, 1);
    rc |= expect_near("entering: mixer at its bound (Hz)", rad_to_hz(f->pre_freq, 72000), 8000.0, 0.05);
    rc |= expect_near("entering: FLL keeps the rest (Hz)", rad_to_hz(f->freq, 72000), 2000.0, 0.05);
    rc |= expect_near("entering: FLL rolloff", f->rolloff, 0.35, 1e-6);
    rc |= expect_near("entering: FLL loop scale", f->loop_bw_scale, 4.0, 1e-6);
    rc |= expect_near("entering: FLL phase reset", f->phase, 0.0, 1e-9);
    rc |= expect_reset_histories("entering", s);
    /* The phasor restarted at (1, 0) and advanced by the block at the mixer frequency. */
    const double angle = std::atan2((double)f->pre_im, (double)f->pre_re);
    rc |= expect_near("entering: phasor advanced from (1, 0)", wrap_pi(angle - (double)pairs * (double)f->pre_freq),
                      0.0, 1e-4);
    /* The channel LPF started over: its oldest history entries are the zeros of the reset, its newest the premixed
     * input. */
    rc |= expect_near("entering: channel history oldest I", s->channel_lpf_hist_i[0], 0.0, 0.0);
    rc |= expect_near("entering: channel history oldest Q", s->channel_lpf_hist_q[0], 0.0, 0.0);
    const double last = (double)(pairs - 1) * (double)f->pre_freq;
    rc |= expect_near("entering: newest channel input is premixed (I)",
                      s->channel_lpf_hist_i[DSD_CHANNEL_LPF_HIST_LEN - 1], 0.01 * std::cos(last), 1e-5);
    rc |= expect_near("entering: newest channel input is premixed (Q)",
                      s->channel_lpf_hist_q[DSD_CHANNEL_LPF_HIST_LEN - 1], 0.01 * std::sin(last), 1e-5);
    /* The channel FIR made pairs - 48 outputs from its reset (its 97 taps' look-ahead), and the closed block one zero
     * symbol per started symbol of them. */
    rc |= expect_int("entering: zero symbols for the closed block", (int)out.size(), (pairs - 48 + 3) / 4);
    free_demod(s);
    return rc;
}

/* Leaving by a CQPSK toggle, then coming back: leaving folds the mixer into the FLL, which takes the P25 parameters,
 * and resets the TETRA histories; the TETRA profile then runs the WIDE plan. Entering again splits again. */
int
test_cqpsk_toggle_leaves_and_enters(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    rc |= make_active(s, 8000.0, 2000.0);
    s->cqpsk_enable = 0;
    s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    const std::vector<float> block = constant_block(1440, 0.01f, 0.0f);
    (void)run_block(s, block.data(), 1440);
    const dsd_fll_band_edge_state_t* f = &s->fll_band_edge_state;
    rc |= expect_int("CQPSK off: chain inactive", s->tetra_chain, 0);
    rc |= expect_near("CQPSK off: mixer folded", f->pre_freq, 0.0, 0.0);
    rc |= expect_near("CQPSK off: FLL holds the sum (Hz)", rad_to_hz(f->freq, 72000), 10000.0, 0.05);
    rc |= expect_near("CQPSK off: phasor reset (re)", f->pre_re, 1.0, 0.0);
    rc |= expect_near("CQPSK off: phasor reset (im)", f->pre_im, 0.0, 0.0);
    rc |= expect_near("CQPSK off: FLL rolloff", f->rolloff, 0.2, 1e-6);
    rc |= expect_near("CQPSK off: FLL loop scale", f->loop_bw_scale, 1.0, 1e-6);
    rc |= expect_reset_histories("CQPSK off", s);
    rc |= expect_int("CQPSK off: WIDE plan", s->channel_lpf_plan_tetra, 0);
    if (s->channel_lpf_plan_taps_len == 97) {
        rc |= fail("CQPSK off: kept the TETRA design's length");
    }

    s->cqpsk_enable = 1;
    s->output_kind = DSD_DEMOD_OUTPUT_SYMBOL_CQPSK;
    squelch_next_blocks(s, 1);
    (void)run_block(s, block.data(), 1440);
    rc |= expect_int("CQPSK on: chain active", s->tetra_chain, 1);
    rc |= expect_near("CQPSK on: mixer split (Hz)", rad_to_hz(f->pre_freq, 72000), 8000.0, 0.05);
    rc |= expect_near("CQPSK on: FLL rest (Hz)", rad_to_hz(f->freq, 72000), 2000.0, 0.05);
    rc |= expect_near("CQPSK on: FLL rolloff", f->rolloff, 0.35, 1e-6);
    rc |= expect_int("CQPSK on: TETRA plan", s->channel_lpf_plan_tetra, 1);
    free_demod(s);
    return rc;
}

/* Leaving by a cause the stream warns about: the chain falls back exactly as on a CQPSK toggle, and the warning names
 * the cause once per stream. */
int
test_warned_fallbacks(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }

    /* The channel LPF off (DSD_NEO_CHANNEL_LPF=0). */
    rc |= make_active(s, 8000.0, 2000.0);
    g_warns = WarnCounts{};
    s->channel_lpf_enable = 0;
    squelch_next_blocks(s, 1); /* nothing after the channel filter runs, so the folded estimate stays exact */
    const std::vector<float> block = constant_block(1440, 0.01f, 0.0f);
    (void)run_block(s, block.data(), 1440);
    (void)run_block(s, block.data(), 1440);
    rc |= expect_int("LPF off: chain inactive", s->tetra_chain, 0);
    rc |= expect_near("LPF off: mixer folded", s->fll_band_edge_state.pre_freq, 0.0, 0.0);
    rc |= expect_near("LPF off: FLL holds the sum (Hz)", rad_to_hz(s->fll_band_edge_state.freq, 72000), 10000.0, 0.05);
    rc |= expect_near("LPF off: FLL rolloff", s->fll_band_edge_state.rolloff, 0.2, 1e-6);
    rc |= expect_reset_histories("LPF off", s);
    rc |= expect_int("LPF off: one warning", g_warns.total, 1);
    rc |= expect_int("LPF off: names the channel filter", g_warns.lpf, 1);

    /* A TED override inconsistent with the rate (sps 8 at 72 kHz). */
    rc |= make_active(s, 0.0, 0.0);
    g_warns = WarnCounts{};
    s->ted_sps_override = 8;
    squelch_next_blocks(s, 1);
    (void)run_block(s, block.data(), 1440);
    (void)run_block(s, block.data(), 1440);
    rc |= expect_int("override 8: chain inactive", s->tetra_chain, 0);
    rc |= expect_int("override 8: one warning", g_warns.total, 1);
    rc |= expect_int("override 8: names the samples per symbol", g_warns.ted, 1);

    /* A rate out of the domain, from a cold start: inactive from the first block, warned once. */
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_cqpsk(s, 48000, DSD_CH_LPF_PROFILE_TETRA, 18000, 3);
    g_warns = WarnCounts{};
    const std::vector<float> b48 = constant_block(960, 0.01f, 0.0f);
    (void)run_block(s, b48.data(), 960);
    (void)run_block(s, b48.data(), 960);
    rc |= expect_int("48 kHz: chain inactive", s->tetra_chain, 0);
    rc |= expect_int("48 kHz: one warning", g_warns.total, 1);
    rc |= expect_int("48 kHz: names the DSP rate", g_warns.rate, 1);

    /* A stream open clears what was warned (the stream does this in its demod init); the next stream warns again. */
    s->tetra_chain_warned = 0;
    (void)run_block(s, b48.data(), 960);
    rc |= expect_int("48 kHz, next stream: warns again", g_warns.rate, 2);

    /* Nothing is warned for an inactive chain the mode does not ask for (CQPSK off). */
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_cqpsk(s, 48000, DSD_CH_LPF_PROFILE_TETRA, 18000, 3);
    s->cqpsk_enable = 0;
    s->output_kind = DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR;
    g_warns = WarnCounts{};
    (void)run_block(s, b48.data(), 960);
    rc |= expect_int("CQPSK off at 48 kHz: no warning", g_warns.total, 0);
    free_demod(s);
    return rc;
}

/* While inactive, the TETRA profile runs the generic CQPSK path exactly: the same symbols as the WIDE profile on the
 * same input (the WIDE plan, the AGC at 0.45, the FLL at 0.2 x1). */
int
test_inactive_runs_wide_path(void) {
    int rc = 0;
    demod_state* a = new_demod();
    demod_state* b = new_demod();
    if (!a || !b) {
        free_demod(a);
        free_demod(b);
        return fail("alloc");
    }
    configure_cqpsk(a, 48000, DSD_CH_LPF_PROFILE_TETRA, 18000, 3);
    configure_cqpsk(b, 48000, DSD_CH_LPF_PROFILE_WIDE, 18000, 3);
    const std::vector<float> iq = make_tetra_stream(72000, 1500.0, 4800 * 4, 0.3f, 0x99u);
    for (int blk = 0; blk < 4; blk++) {
        const float* p = iq.data() + (size_t)blk * 4800U * 2U;
        const std::vector<float> oa = run_block(a, p, 4800);
        const std::vector<float> ob = run_block(b, p, 4800);
        if (oa.size() != ob.size()
            || (!oa.empty() && std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) != 0)) {
            rc |= fail("inactive TETRA profile differs from the WIDE profile");
            break;
        }
    }
    rc |= expect_int("inactive TETRA: chain stays off", a->tetra_chain, 0);
    rc |= expect_near("inactive TETRA: AGC average as WIDE's", a->cqpsk_agc_avg, b->cqpsk_agc_avg, 0.0);
    rc |= expect_near("inactive TETRA: FLL as WIDE's", a->fll_band_edge_state.freq, b->fll_band_edge_state.freq, 0.0);
    rc |= expect_near("inactive TETRA: no mixer", a->fll_band_edge_state.pre_freq, 0.0, 0.0);
    free_demod(a);
    free_demod(b);
    return rc;
}

/* An uplink session (demod_state::tetra_uplink, D10) never runs the chain: the predicate is false at every rate of the
 * domain, and nothing is warned, since nothing the operator set keeps it off. Its stream keeps the path TETRA ran
 * before the chain (the AGC at 0.45, the FLL at 0.2 x1, no mixer and no matched filter) with the TETRA channel filter
 * in place of WIDE, where the domain admits the rate; off the domain it runs WIDE. */
int
test_uplink_keeps_channel_filter_only(void) {
    int rc = 0;
    rc |= expect_int("NULL demod: channel filter", dsd_demod_tetra_channel_runs(NULL), 0);
    rc |= expect_int("NULL demod: stage", dsd_demod_tetra_stage(NULL), DSD_SNR_TETRA_STAGE_WIDE);
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    for (int rate = 36000; rate <= 144000; rate += 18000) {
        DSD_MEMSET(s, 0, sizeof(*s));
        configure_tetra(s, rate);
        char label[96];
        DSD_SNPRINTF(label, sizeof label, "downlink at %d Hz: stage", rate);
        dsd_demod_update_tetra_chain(s);
        rc |= expect_int(label, dsd_demod_tetra_stage(s), DSD_SNR_TETRA_STAGE_CHAIN);
        DSD_MEMSET(s, 0, sizeof(*s));
        configure_tetra(s, rate);
        s->tetra_uplink = 1;
        DSD_SNPRINTF(label, sizeof label, "uplink at %d Hz: predicate", rate);
        rc |= expect_int(label, dsd_demod_tetra_chain_wanted(s), 0);
        DSD_SNPRINTF(label, sizeof label, "uplink at %d Hz: channel filter", rate);
        rc |= expect_int(label, dsd_demod_tetra_channel_runs(s), 1);
        DSD_SNPRINTF(label, sizeof label, "uplink at %d Hz: stage", rate);
        rc |= expect_int(label, dsd_demod_tetra_stage(s), DSD_SNR_TETRA_STAGE_CHANNEL);
    }

    DSD_MEMSET(s, 0, sizeof(*s));
    configure_tetra(s, 72000);
    s->tetra_uplink = 1;
    g_warns = WarnCounts{};
    const std::vector<float> iq = make_tetra_stream(72000, 1500.0, 1440 * 3, 0.3f, 0x55u);
    for (int blk = 0; blk < 3; blk++) {
        (void)run_block(s, iq.data() + (size_t)blk * 1440U * 2U, 1440);
    }
    static float want[DSD_CHANNEL_LPF_MAX_TAPS];
    const int want_len = dsd_tetra_channel_design(72000, want, DSD_CHANNEL_LPF_MAX_TAPS);
    rc |= expect_int("uplink: chain off", s->tetra_chain, 0);
    rc |= expect_int("uplink: TETRA plan key", s->channel_lpf_plan_tetra, 1);
    rc |= expect_int("uplink: TETRA design taps", s->channel_lpf_plan_taps_len, want_len);
    if (want_len > 0 && std::memcmp(s->channel_lpf_plan_taps, want, (size_t)want_len * sizeof(float)) != 0) {
        rc |= fail("uplink: installed plan is not the TETRA design");
    }
    rc |= expect_int("uplink: no matched filter", s->tetra_rrc_taps_len, 0);
    rc |= expect_near("uplink: no mixer", s->fll_band_edge_state.pre_freq, 0.0, 0.0);
    rc |= expect_near("uplink: FLL rolloff", s->fll_band_edge_state.rolloff, 0.2, 1e-6);
    rc |= expect_near("uplink: FLL loop scale", s->fll_band_edge_state.loop_bw_scale, 1.0, 1e-6);
    rc |= expect_int("uplink: nothing warned", g_warns.total, 0);
    if (s->result_len <= 0) {
        rc |= fail("uplink: no symbols");
    }

    /* The AGC averages with the earlier path's 0.45: from 1.0, after 8 samples of |x|^2 = 0.25 the average is
       0.25 + 0.75 * 0.55^8, where the chain's 0.1 would leave 0.25 + 0.75 * 0.9^8. */
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_tetra(s, 72000);
    s->tetra_uplink = 1;
    dsd_demod_update_tetra_chain(s);
    s->cqpsk_agc_avg = 1.0f;
    for (int k = 0; k < 8; k++) {
        s->input_cb_buf[(size_t)k * 2] = 0.5f;
        s->input_cb_buf[((size_t)k * 2) + 1] = 0.0f;
    }
    s->lowpassed = s->input_cb_buf;
    s->lp_len = 16;
    dsd_demod_test_cqpsk_rms_agc(s);
    rc |= expect_near("uplink: AGC average at 0.45", s->cqpsk_agc_avg, 0.25 + 0.75 * std::pow(0.55, 8.0), 1e-5);

    /* Off the domain (48 kHz) the uplink runs WIDE, as a downlink stream there does, and is not warned about either. */
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_cqpsk(s, 48000, DSD_CH_LPF_PROFILE_TETRA, 18000, 3);
    s->tetra_uplink = 1;
    g_warns = WarnCounts{};
    const std::vector<float> b48 = constant_block(960, 0.01f, 0.0f);
    (void)run_block(s, b48.data(), 960);
    rc |= expect_int("uplink at 48 kHz: channel filter", dsd_demod_tetra_channel_runs(s), 0);
    rc |= expect_int("uplink at 48 kHz: WIDE plan key", s->channel_lpf_plan_tetra, 0);
    rc |= expect_int("uplink at 48 kHz: stage", dsd_demod_tetra_stage(s), DSD_SNR_TETRA_STAGE_WIDE);
    rc |= expect_int("uplink at 48 kHz: nothing warned", g_warns.total, 0);

    /* With the channel LPF off (DSD_NEO_CHANNEL_LPF=0) no TETRA filter runs either, and nothing is warned. */
    DSD_MEMSET(s, 0, sizeof(*s));
    configure_tetra(s, 72000);
    s->tetra_uplink = 1;
    s->channel_lpf_enable = 0;
    g_warns = WarnCounts{};
    (void)run_block(s, iq.data(), 1440);
    rc |= expect_int("uplink, LPF off: channel filter", dsd_demod_tetra_channel_runs(s), 0);
    rc |= expect_int("uplink, LPF off: stage", dsd_demod_tetra_stage(s), DSD_SNR_TETRA_STAGE_WIDE);
    rc |= expect_int("uplink, LPF off: nothing warned", g_warns.total, 0);
    free_demod(s);
    return rc;
}

/* --- Pull-in ------------------------------------------------------------------------------------------------ */

/* A clean stream at 72 kHz, @p offset_hz above the centre: the combined estimate reaches the truth (minus the offset,
 * the FLL's sign) within @p tol_hz inside 0.5 s of samples and stays there. */
int
pull_in(double offset_hz, double tol_hz, double* out_total_hz, double* out_freq_hz, double* out_pre_hz) {
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    const int rate = 72000;
    const int block = 2400;
    const int blocks = 24; /* 0.8 s */
    configure_tetra(s, rate);
    const std::vector<float> iq = make_tetra_stream(rate, offset_hz, block * blocks, 0.3f, 0xC0FFEEu);
    const double truth = -offset_hz;
    int rc = 0;
    double settled_s = -1.0; /* the end of the first block from which every block is within tol_hz */
    for (int b = 0; b < blocks; b++) {
        (void)run_block(s, iq.data() + (size_t)b * (size_t)block * 2U, block);
        const double samples = (double)(b + 1) * (double)block;
        const double total_hz = rad_to_hz(dsd_fll_band_edge_total_freq(&s->fll_band_edge_state), rate);
        if (std::fabs(total_hz - truth) > tol_hz) {
            settled_s = -1.0;
        } else if (settled_s < 0.0) {
            settled_s = samples / rate;
        }
        if (samples >= 0.5 * rate && std::fabs(total_hz - truth) > tol_hz) {
            DSD_FPRINTF(stderr, "TETRA chain: %+.0f Hz offset: combined CFO %.1f Hz at %.3f s, truth %.1f Hz\n",
                        offset_hz, total_hz, samples / rate, truth);
            rc = 1;
            break;
        }
    }
    *out_total_hz = rad_to_hz(dsd_fll_band_edge_total_freq(&s->fll_band_edge_state), rate);
    *out_freq_hz = rad_to_hz(s->fll_band_edge_state.freq, rate);
    *out_pre_hz = rad_to_hz(s->fll_band_edge_state.pre_freq, rate);
    DSD_FPRINTF(stdout,
                "TETRA chain pull-in %+.0f Hz: within %.0f Hz of %.0f Hz from %.3f s; at %.2f s total %.1f Hz "
                "(mixer %.1f, FLL %.1f)\n",
                offset_hz, tol_hz, truth, settled_s, (double)(block * blocks) / rate, *out_total_hz, *out_pre_hz,
                *out_freq_hz);
    if (!s->tetra_chain) {
        rc |= fail("pull-in: chain not active");
    }
    free_demod(s);
    return rc;
}

int
test_pull_in(void) {
    int rc = 0;
    double total = 0.0;
    double freq = 0.0;
    double pre = 0.0;
    rc |= pull_in(6000.0, 200.0, &total, &freq, &pre);
    rc |= pull_in(600.0, 200.0, &total, &freq, &pre);
    /* Within the mixer's bound the transfer leaves the FLL nothing: the mixer carries the estimate. */
    rc |= expect_near("+600 Hz: FLL residual (Hz)", freq, 0.0, 1.0);
    rc |= expect_near("+600 Hz: mixer carries the total (Hz)", pre, total, 1.0);
    return rc;
}

/* --- Squelch and signal search ------------------------------------------------------------------------------ */

/* A closed block premixes and runs the channel LPF (their state advances) but not the matched filter, FLL, Gardner or
 * Costas; at reopen the matched filter starts over. Every block's output stays within its symbols. */
int
test_squelch_close_reopen(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    const int rate = 72000;
    const int block = 2400;
    configure_tetra(s, rate);
    const std::vector<float> iq = make_tetra_stream(rate, 1000.0, block * 9, 0.3f, 0xABCDu);
    const int pattern[9] = {0, 0, 0, 1, 1, 0, 0, 0, 0};
    for (int b = 0; b < 9; b++) {
        squelch_next_blocks(s, pattern[b]);
        const dsd_fll_band_edge_state_t before = s->fll_band_edge_state;
        const float mu = s->ted_state.mu;
        const float omega = s->ted_state.omega;
        const float costas_phase = s->costas_state.phase;
        const std::vector<float> out = run_block(s, iq.data() + (size_t)b * (size_t)block * 2U, block);
        char label[96];
        if ((int)out.size() > block / 4 + 2 || s->result_len > MAXIMUM_BUF_LENGTH) {
            DSD_SNPRINTF(label, sizeof label, "block %d made %d symbols from %d samples", b, (int)out.size(), block);
            rc |= fail(label);
        }
        if (pattern[b]) {
            DSD_SNPRINTF(label, sizeof label, "closed block %d: zero symbols", b);
            rc |= expect_int(label, (int)out.size(), (block + 3) / 4);
            const dsd_fll_band_edge_state_t* f = &s->fll_band_edge_state;
            DSD_SNPRINTF(label, sizeof label, "closed block %d: FLL not run", b);
            rc |= expect_near(label, f->freq, before.freq, 0.0);
            rc |= expect_near(label, f->phase, before.phase, 0.0);
            rc |= expect_near(label, f->pre_freq, before.pre_freq, 0.0);
            DSD_SNPRINTF(label, sizeof label, "closed block %d: Gardner and Costas not run", b);
            rc |= expect_near(label, s->ted_state.mu, mu, 0.0);
            rc |= expect_near(label, s->ted_state.omega, omega, 0.0);
            rc |= expect_near(label, s->costas_state.phase, costas_phase, 0.0);
            DSD_SNPRINTF(label, sizeof label, "closed block %d: mixer advances", b);
            const double a0 = std::atan2((double)before.pre_im, (double)before.pre_re);
            const double a1 = std::atan2((double)f->pre_im, (double)f->pre_re);
            rc |= expect_near(label, wrap_pi(a1 - a0 - (double)block * (double)f->pre_freq), 0.0, 1e-3);
            DSD_SNPRINTF(label, sizeof label, "closed block %d: channel LPF advances", b);
            rc |= expect_int(label, s->channel_lpf_pending, 48);
            DSD_SNPRINTF(label, sizeof label, "closed block %d: matched filter held for reset", b);
            rc |= expect_int(label, s->tetra_rrc_pending, 0);
        } else if (b > 0 && out.empty()) {
            DSD_SNPRINTF(label, sizeof label, "open block %d made no symbols", b);
            rc |= fail(label);
        }
        if (b == 5) {
            /* The reopen block ran the matched filter from a reset: its look-ahead is pending again. */
            DSD_SNPRINTF(label, sizeof label, "reopen block: matched filter running");
            rc |= expect_int(label, s->tetra_rrc_pending, 32);
        }
    }
    free_demod(s);
    return rc;
}

/* The chain runs on across a gap in its input (a block the signal search drops never reaches full_demod(), so the
 * block after it comes spliced to the one before): the block after the gap makes symbols, no more than a block's
 * worth. What it checks is the output's length only; the drop itself, and the uplink look-ahead it clears, are the
 * demod thread's (demod_drop_signal_search_block()), outside full_demod(). */
int
test_block_after_gap_makes_symbols(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, 72000);
    const int block = 2400;
    const std::vector<float> iq = make_tetra_stream(72000, 2000.0, block * 3, 0.3f, 0x5151u);
    (void)run_block(s, iq.data(), block);
    /* Block 1 is dropped: the next full_demod() call takes block 2. */
    const std::vector<float> out = run_block(s, iq.data() + (size_t)block * 4U, block);
    if ((int)out.size() > block / 4 + 2 || out.empty()) {
        rc |= fail("block after a gap: output length");
    }
    free_demod(s);
    return rc;
}

/* --- Partition invariance ----------------------------------------------------------------------------------- */

/* The linear front end alone (mixer, channel LPF, matched filter) gives the same output however the stream is cut,
 * within the SIMD kernels' and the phasor renormalization's rounding. */
int
run_linear(const std::vector<float>& iq, const std::vector<int>& cuts, std::vector<float>* out) {
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, 72000);
    s->tetra_chain = 1;
    s->fll_band_edge_state.pre_freq = (float)hz_to_rad(3000.0, 72000);
    size_t pos = 0;
    out->clear();
    for (int n : cuts) {
        DSD_MEMCPY(s->input_cb_buf, iq.data() + pos * 2U, (size_t)n * 2U * sizeof(float));
        pos += (size_t)n;
        s->lowpassed = s->input_cb_buf;
        s->lp_len = n * 2;
        dsd_demod_test_tetra_linear_front_end(s);
        out->insert(out->end(), s->lowpassed, s->lowpassed + s->lp_len);
    }
    free_demod(s);
    return 0;
}

int
test_partition_invariance(void) {
    int rc = 0;
    const int total = 7000;
    std::vector<float> iq((size_t)total * 2U);
    Lcg rng{0x2468u};
    for (float& v : iq) {
        v = (float)(rng.uniform() - 0.5);
    }
    std::vector<float> whole;
    rc |= run_linear(iq, std::vector<int>{total}, &whole);
    const std::vector<std::vector<int>> partitions = {
        {1, total - 1},
        {1000, 1000, 1000, 1000, 1000, 1000, 1000},
        {13, 977, 1, 2048, 3961},
        {3500, 3500},
    };
    for (const std::vector<int>& cuts : partitions) {
        std::vector<float> got;
        rc |= run_linear(iq, cuts, &got);
        if (got.size() != whole.size()) {
            DSD_FPRINTF(stderr, "TETRA chain: partition of %d cuts made %d floats, whole %d\n", (int)cuts.size(),
                        (int)got.size(), (int)whole.size());
            rc = 1;
            continue;
        }
        double worst = 0.0;
        for (size_t k = 0; k < got.size(); k++) {
            worst = std::fmax(worst, std::fabs((double)got[k] - (double)whole[k]));
        }
        if (worst > 1e-5) {
            DSD_FPRINTF(stderr, "TETRA chain: partition of %d cuts differs by %.3g\n", (int)cuts.size(), worst);
            rc = 1;
        }
    }
    /* Every input made an output once the look-aheads (channel 48, matched filter 32) are filled. */
    rc |= expect_int("linear front end output count", (int)(whole.size() / 2U), total - 48 - 32);
    return rc;
}

/* The whole chain through full_demod() (mixer, channel LPF, AGC, matched filter, FLL and its transfers into the mixer,
 * Gardner, differential, Costas) makes the same symbols however its input is cut into blocks: the transfer falls on
 * the sub-block boundaries of the input's own sample count, and nothing after the FLL decides per block. A carrier
 * well off centre makes the transfers matter: the mixer takes thousands of hertz in steps over the first blocks. */
/* The chain at the DSP rate @p rate, behind @p passes half-band stages when the input is undecimated (rate << passes),
 * which leave the block in hb_workbuf (odd passes: the matched filter then writes behind the read cursor in the
 * input's own buffer) or in the input buffer (even passes: the mixer rotates the input in place). *track is the FLL's
 * gear at the end. */
int
run_chain(int rate, int passes, const std::vector<float>& iq, const std::vector<int>& cuts, std::vector<float>* out,
          double* total_hz, int* track) {
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, rate);
    s->rate_in = rate << passes;
    s->downsample_passes = passes;
    size_t pos = 0;
    out->clear();
    float first_gain = -1.0f;
    for (int n : cuts) {
        const std::vector<float> sym = run_block(s, iq.data() + pos * 2U, n);
        pos += (size_t)n;
        out->insert(out->end(), sym.begin(), sym.end());
        if (first_gain < 0.0f && !sym.empty()) {
            first_gain = s->ted_effective_gain;
        }
    }
    *total_hz = rad_to_hz(dsd_fll_band_edge_total_freq(&s->fll_band_edge_state), rate);
    *track = s->fll_band_edge_state.tetra_track;
    const int active = s->tetra_chain;
    /* Locked long since, the Gardner still runs the gain it started with: lowering it is a block's decision. */
    const float gain = s->ted_effective_gain;
    free_demod(s);
    if (!active) {
        return fail("block partitions: chain not active");
    }
    return expect_near("block partitions: Gardner gain after lock", gain, first_gain, 0.0);
}

/* Cuts of @p total samples: every block @p size long, the last one short. */
std::vector<int>
cuts_of(int total, int size) {
    std::vector<int> cuts;
    for (int left = total; left > 0; left -= size) {
        cuts.push_back(left < size ? left : size);
    }
    return cuts;
}

/* Cuts drawn uniformly from 1..max from a fixed seed. */
std::vector<int>
irregular_cuts(int total, int max, uint32_t seed) {
    std::vector<int> cuts;
    Lcg rng{seed};
    for (int left = total; left > 0;) {
        int n = 1 + (int)(rng.next() % (uint32_t)max);
        n = n < left ? n : left;
        cuts.push_back(n);
        left -= n;
    }
    return cuts;
}

/* The partitions at the DSP rate @p rate behind @p passes half-band stages: the input runs at rate << passes. The FLL
 * must have locked (a gear above acquiring) by the end of every partition, on the same gear, so its gear decisions
 * fall on the same samples however the input is cut, as the symbols show. */
int
block_partitions_at(int rate, int passes, double offset_hz) {
    int rc = 0;
    const int sps = rate / 18000;
    const int sub = (DSD_TETRA_SUBBLOCK_SYMBOLS * sps) << passes; /* in input samples */
    const int in_rate = rate << passes;
    const int total = in_rate / 2; /* 0.5 s: the pull-in and the tracking after it */
    const std::vector<float> iq = make_tetra_stream(in_rate, offset_hz, total, 0.3f, 0x1357u);
    std::vector<float> ref;
    double ref_hz = 0.0;
    int ref_track = -1;
    rc |= run_chain(rate, passes, iq, cuts_of(total, sub), &ref, &ref_hz, &ref_track);
    char label[128];
    DSD_SNPRINTF(label, sizeof label, "%d Hz behind %d half-band passes: symbols", rate, passes);
    if (ref.size() < 1000U) {
        rc |= fail(label);
    }
    for (float v : ref) {
        if (!std::isfinite(v)) {
            rc |= fail(label);
            break;
        }
    }
    if (ref_track <= 0) {
        DSD_SNPRINTF(label, sizeof label, "%d Hz behind %d half-band passes: FLL still acquiring", rate, passes);
        rc |= fail(label);
    }
    double worst_all = 0.0;
    const std::vector<std::vector<int>> partitions = {
        cuts_of(total, 2400),
        cuts_of(total, sub / 7),
        cuts_of(total, sub / 2 + 1),
        cuts_of(total, 2 * sub),
        irregular_cuts(total, 2 * sub, 1u),
        irregular_cuts(total, 61, 7u),
    };
    for (size_t p = 0; p < partitions.size(); p++) {
        std::vector<float> got;
        double got_hz = 0.0;
        int got_track = -1;
        rc |= run_chain(rate, passes, iq, partitions[p], &got, &got_hz, &got_track);
        DSD_SNPRINTF(label, sizeof label, "%d Hz/%d passes, %+.0f Hz offset, partition %d: symbols", rate, passes,
                     offset_hz, (int)p);
        rc |= expect_int(label, (int)got.size(), (int)ref.size());
        DSD_SNPRINTF(label, sizeof label, "%d Hz/%d passes, %+.0f Hz offset, partition %d: FLL gear", rate, passes,
                     offset_hz, (int)p);
        rc |= expect_int(label, got_track, ref_track);
        double worst = 0.0;
        for (size_t k = 0; k < got.size() && k < ref.size(); k++) {
            worst = std::fmax(worst, std::fabs((double)got[k] - (double)ref[k]));
        }
        /* The SIMD kernels' and the phasor renormalization's rounding, not a different trajectory. */
        DSD_SNPRINTF(label, sizeof label, "%d Hz/%d passes, %+.0f Hz offset, partition %d: worst symbol difference",
                     rate, passes, offset_hz, (int)p);
        rc |= expect_near(label, worst, 0.0, 1e-3);
        worst_all = std::fmax(worst_all, worst);
        DSD_SNPRINTF(label, sizeof label, "%d Hz/%d passes, %+.0f Hz offset, partition %d: carrier estimate (Hz)", rate,
                     passes, offset_hz, (int)p);
        rc |= expect_near(label, got_hz, ref_hz, 0.05);
    }
    DSD_FPRINTF(stdout,
                "TETRA chain block partitions at %d Hz behind %d half-band passes, %+.0f Hz offset: %d symbols, FLL "
                "gear %d, worst difference %.3g\n",
                rate, passes, offset_hz, (int)ref.size(), ref_track, worst_all);
    return rc;
}

int
test_block_partitions(void) {
    int rc = 0;
    rc |= block_partitions_at(72000, 0, 3500.0);
    rc |= block_partitions_at(54000, 0, -2200.0);
    rc |= block_partitions_at(144000, 0, 6000.0);
    /* Undecimated 144 kHz input: one pass leaves the block in hb_workbuf, two in the input buffer. */
    rc |= block_partitions_at(72000, 1, 3500.0);
    rc |= block_partitions_at(36000, 2, 1500.0);
    return rc;
}

/* A block that arrives in hb_workbuf without a half-band pass (the matched filter then writes the input's own buffer
 * behind its read cursor, as after an odd pass count) makes the same symbols as the same block in the input buffer. */
int
test_block_in_workbuf(void) {
    int rc = 0;
    const int rate = 72000;
    const int block = 2400;
    const int total = block * 15;
    const std::vector<float> iq = make_tetra_stream(rate, 2000.0, total, 0.3f, 0x2468u);
    demod_state* a = new_demod();
    demod_state* b = new_demod();
    if (!a || !b) {
        free_demod(a);
        free_demod(b);
        return fail("alloc");
    }
    configure_tetra(a, rate);
    configure_tetra(b, rate);
    size_t made = 0;
    for (int k = 0; k < total / block && rc == 0; k++) {
        const float* p = iq.data() + (size_t)k * (size_t)block * 2U;
        const std::vector<float> oa = run_block(a, p, block);
        const std::vector<float> ob = run_block(b, p, block, b->hb_workbuf);
        made += ob.size();
        if (oa.size() != ob.size()
            || (!oa.empty() && std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) != 0)) {
            DSD_FPRINTF(stderr, "TETRA chain: block %d from hb_workbuf differs from the input buffer's\n", k);
            rc = 1;
        }
    }
    if (made < 1000U) {
        rc |= fail("block in hb_workbuf: too few symbols");
    }
    rc |= expect_int("block in hb_workbuf: chain active", b->tetra_chain, 1);
    free_demod(a);
    free_demod(b);
    return rc;
}

/* --- The FLL's gears ---------------------------------------------------------------------------------------- */

/* The loop gains GNU Radio's control loop sets at @p scale times OP25's bandwidth 2*pi/sps/350, critically damped. */
double
fll_beta_for(int sps, double scale) {
    const double bw = 2.0 * kPi / (double)sps / 350.0 * scale;
    return 4.0 * bw * bw / (1.0 + 2.0 * 0.70710678118654752440 * bw + bw * bw);
}

/* A noisy pi/4-DQPSK stream (about 10 dB SNR in the 25 kHz channel) whose carrier sits @p hz1 above the centre for
 * the first @p switch_at samples and @p hz2 after, phase continuous. */
std::vector<float>
gear_stream(int rate, int pairs, double hz1, double hz2, int switch_at) {
    std::vector<float> iq = make_tetra_stream(rate, 0.0, pairs, 0.3f, 0x6E6Eu);
    Lcg rng{0x5EEDu};
    double ph = 0.0;
    for (int n = 0; n < pairs; n++) {
        ph += 2.0 * kPi * (n < switch_at ? hz1 : hz2) / (double)rate;
        const double c = std::cos(ph);
        const double s = std::sin(ph);
        const double re = (double)iq[(size_t)n * 2U];
        const double im = (double)iq[((size_t)n * 2U) + 1U];
        /* Box-Muller from the stream's own generator. */
        const double u1 = rng.uniform() + 1e-12;
        const double u2 = rng.uniform();
        const double r = std::sqrt(-2.0 * std::log(u1)) * 0.06;
        iq[(size_t)n * 2U] = (float)(re * c - im * s + r * std::cos(2.0 * kPi * u2));
        iq[((size_t)n * 2U) + 1U] = (float)(re * s + im * c + r * std::sin(2.0 * kPi * u2));
    }
    return iq;
}

/* The chain acquires with the wide loop, locks to the narrow tracking loop (a quarter of OP25's bandwidth) once the
 * estimate holds, and drops back to acquiring when the carrier moves: here 2 kHz in one step, after which it locks
 * again on the new carrier. Locked, the estimate's spread is a fraction of the acquiring loop's. */
int
test_fll_gears(void) {
    int rc = 0;
    const int rate = 72000;
    const int block = 2400;
    const int total = rate * 3; /* 3 s: the jump at 1.5 s */
    const std::vector<float> iq = gear_stream(rate, total, 2500.0, 4500.0, total / 2);
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    configure_tetra(s, rate);
    std::vector<double> est;
    std::vector<int> gear;
    for (int b = 0; b < total / block; b++) {
        (void)run_block(s, iq.data() + (size_t)b * (size_t)block * 2U, block);
        est.push_back(rad_to_hz(dsd_fll_band_edge_total_freq(&s->fll_band_edge_state), rate));
        gear.push_back(s->fll_band_edge_state.tetra_track);
    }
    const dsd_fll_band_edge_state_t* f = &s->fll_band_edge_state;
    const int per_s = rate / block; /* 30 blocks a second */
    auto spread = [&](int from, int to) {
        double m = 0.0;
        for (int k = from; k < to; k++) {
            m += est[(size_t)k];
        }
        m /= (double)(to - from);
        double v = 0.0;
        for (int k = from; k < to; k++) {
            v += (est[(size_t)k] - m) * (est[(size_t)k] - m);
        }
        return std::sqrt(v / (double)(to - from));
    };
    int first_lock = -1;
    int dropped = -1;
    int relock = -1;
    for (size_t k = 0; k < gear.size(); k++) {
        if (first_lock < 0 && gear[k] > 0) {
            first_lock = (int)k;
        }
        if (k >= (size_t)(per_s * 3 / 2) && dropped < 0 && gear[k] == 0) {
            dropped = (int)k;
        }
        if (dropped >= 0 && relock < 0 && gear[k] > 0) {
            relock = (int)k;
        }
    }
    const double locked_spread = spread(per_s, per_s * 3 / 2);
    const double final_spread = spread(per_s * 5 / 2, per_s * 3);
    DSD_FPRINTF(stdout,
                "TETRA chain FLL gears: locked at %.3f s, dropped at %.3f s, locked again at %.3f s; spread %.1f Hz "
                "before the step, %.1f Hz at the end; estimate %.1f Hz before the step (truth -2500), %.1f Hz at the "
                "end (truth -4500)\n",
                first_lock >= 0 ? (double)(first_lock + 1) * block / rate : -1.0,
                dropped >= 0 ? (double)(dropped + 1) * block / rate : -1.0,
                relock >= 0 ? (double)(relock + 1) * block / rate : -1.0, locked_spread, final_spread,
                est[(size_t)(per_s * 3 / 2 - 1)], est.back());
    rc |= expect_int("gears: acquiring at the start", gear[0], 0);
    if (first_lock < 0 || first_lock >= per_s / 2) {
        rc |= fail("gears: no lock within 0.5 s");
    }
    for (int k = per_s; k < per_s * 3 / 2; k++) {
        if (gear[(size_t)k] == 0) {
            rc |= fail("gears: dropped out of tracking on a steady carrier");
            break;
        }
    }
    if (dropped < 0 || dropped > per_s * 3 / 2 + per_s / 5) {
        rc |= fail("gears: the 2 kHz step did not drop back to acquiring within 0.2 s");
    }
    if (relock < 0 || relock > per_s * 5 / 2) {
        rc |= fail("gears: no lock again within 1 s of the step");
    }
    rc |= expect_near("gears: tracking spread (Hz)", locked_spread, 0.0, 15.0);
    rc |= expect_near("gears: estimate after the step (Hz)", est.back(), -4500.0, 150.0);
    rc |= expect_int("gears: narrowest tracking gear at the end", gear.back(), 4);
    rc |= expect_near("gears: tracking loop gain", f->beta, fll_beta_for(4, 0.25), 1e-9);
    free_demod(s);
    return rc;
}

/* --- A P25 stream after a TETRA session --------------------------------------------------------------------- */

/* What a stream switch at the same frequency resets: the filters, and the loops as a family switch starts them, but
 * not the FLL's carrier estimate, which a switch that does not retune keeps (the retune layer clears or seeds it). */
void
stream_switch_reset_keeping_estimate(demod_state* s) {
    dsd_demod_reset_filter_state(s);
    ted_init_state(&s->ted_state);
    s->ted_mu = 0.0f;
    s->costas_state.phase = 0.0f;
    s->costas_state.freq = 0.0f;
    s->costas_state.error = 0.0f;
    s->costas_state.error_smooth = 0.0f;
    s->costas_state.initialized = 0;
    s->cqpsk_diff_prev_r = 1.0f;
    s->cqpsk_diff_prev_j = 0.0f;
    s->cqpsk_agc_avg = 1.0f;
}

/* ... and with the estimate cleared too, as on a fresh stream. */
void
stream_switch_reset(demod_state* s) {
    stream_switch_reset_keeping_estimate(s);
    s->fll_band_edge_state.freq = 0.0f;
    s->fll_band_edge_state.pre_freq = 0.0f;
    s->fll_band_edge_state.pre_re = 1.0f;
    s->fll_band_edge_state.pre_im = 0.0f;
    s->fll_band_edge_state.phase = 0.0f;
}

/* P25 CQPSK at the same 72 kHz rate after a TETRA session runs exactly as on a fresh demodulator seeded with the
 * session's carrier estimate: the switch keeps the estimate, the block boundary leaves the chain (the mixer folded into
 * the FLL, which is designed with the P25 parameters again), and no mixer, TETRA FLL design, AGC weight or matched
 * filter is left behind. */
int
test_p25_after_tetra_session(void) {
    int rc = 0;
    demod_state* a = new_demod();
    demod_state* b = new_demod();
    if (!a || !b) {
        free_demod(a);
        free_demod(b);
        return fail("alloc");
    }
    configure_tetra(a, 72000);
    const std::vector<float> tetra = make_tetra_stream(72000, 3000.0, 2400 * 6, 0.3f, 0x7777u);
    for (int blk = 0; blk < 6; blk++) {
        (void)run_block(a, tetra.data() + (size_t)blk * 2400U * 2U, 2400);
    }
    if (!a->tetra_chain || std::fabs(a->fll_band_edge_state.pre_freq) < 1e-4f) {
        rc |= fail("TETRA session did not run its mixer");
    }
    /* The combined estimate the session leaves, mostly in the mixer. */
    const float total = dsd_fll_band_edge_total_freq(&a->fll_band_edge_state);
    stream_switch_reset_keeping_estimate(a);
    configure_cqpsk(a, 72000, DSD_CH_LPF_PROFILE_P25_CQPSK, 4800, 15);
    /* The fresh demod: its FLL designed for P25 at the stream's sps and seeded with the total, as the retune layer
     * seeds one. */
    stream_switch_reset(b);
    configure_cqpsk(b, 72000, DSD_CH_LPF_PROFILE_P25_CQPSK, 4800, 15);
    dsd_fll_band_edge_init_params(&b->fll_band_edge_state, dsd_cqpsk_timing_sps(b), dsd_fll_band_edge_params_for(0));
    b->fll_band_edge_state.freq = total;

    /* A pi/4 stream at 4800 sym/s stands in for P25: the content does not matter, only that both see the same. */
    std::vector<float> p25((size_t)7200 * 4U * 2U);
    Lcg rng{0x2525u};
    double ph = 0.0;
    for (size_t n = 0; n < p25.size() / 2U; n++) {
        if (n % 15U == 0U) {
            ph += kPi / 4.0 * (double)(1 + 2 * (int)((rng.next() >> 16) & 3u));
        }
        const double w = ph + 2.0 * kPi * 400.0 * (double)n / 72000.0;
        p25[n * 2U] = (float)(0.3 * std::cos(w));
        p25[(n * 2U) + 1U] = (float)(0.3 * std::sin(w));
    }
    for (int blk = 0; blk < 4; blk++) {
        const float* p = p25.data() + (size_t)blk * 7200U * 2U;
        const std::vector<float> oa = run_block(a, p, 7200);
        const std::vector<float> ob = run_block(b, p, 7200);
        if (oa.size() != ob.size()
            || (!oa.empty() && std::memcmp(oa.data(), ob.data(), oa.size() * sizeof(float)) != 0)) {
            DSD_FPRINTF(stderr, "TETRA chain: P25 block %d after a TETRA session differs from a fresh stream\n", blk);
            rc = 1;
            break;
        }
    }
    rc |= expect_int("P25 after TETRA: chain off", a->tetra_chain, 0);
    rc |= expect_near("P25 after TETRA: no mixer", a->fll_band_edge_state.pre_freq, 0.0, 0.0);
    rc |= expect_near("P25 after TETRA: phasor reset (re)", a->fll_band_edge_state.pre_re, 1.0, 0.0);
    rc |= expect_near("P25 after TETRA: phasor reset (im)", a->fll_band_edge_state.pre_im, 0.0, 0.0);
    rc |= expect_near("P25 after TETRA: FLL rolloff", a->fll_band_edge_state.rolloff, 0.2, 1e-6);
    rc |= expect_near("P25 after TETRA: AGC as fresh", a->cqpsk_agc_avg, b->cqpsk_agc_avg, 0.0);
    rc |= expect_near("P25 after TETRA: FLL as fresh", a->fll_band_edge_state.freq, b->fll_band_edge_state.freq, 0.0);
    free_demod(a);
    free_demod(b);
    return rc;
}

/* A filter reset (every retune, family switch and reacquire runs one) starts the matched filter over, and on the
 * TETRA chain the AGC average with it; off the chain the AGC average is kept, as before the chain. */
int
test_filter_reset(void) {
    int rc = 0;
    demod_state* s = new_demod();
    if (!s) {
        return fail("alloc");
    }
    rc |= make_active(s, 1000.0, 0.0);
    s->tetra_rrc_pending = 9;
    s->tetra_rrc_hist_q[3] = 1.0f;
    dsd_demod_reset_filter_state(s);
    rc |= expect_reset_histories("filter reset on the TETRA chain", s);
    s->tetra_chain = 0;
    s->cqpsk_agc_avg = 0.3f;
    dsd_demod_reset_filter_state(s);
    rc |= expect_near("filter reset off the TETRA chain keeps the AGC", s->cqpsk_agc_avg, (double)0.3f, 0.0);
    free_demod(s);
    return rc;
}

} // namespace

int
main(void) {
    dsd_neo_log_set_tap(count_tetra_warnings, NULL);
    int rc = 0;
    rc |= test_predicate_truth_table();
    rc |= test_open_evaluates_chain();
    rc |= test_design_memo();
    rc |= test_cqpsk_timing_sps();
    rc |= test_cold_start_activates();
    rc |= test_agc_alpha_wiring();
    rc |= test_entering_splits();
    rc |= test_cqpsk_toggle_leaves_and_enters();
    rc |= test_warned_fallbacks();
    rc |= test_inactive_runs_wide_path();
    rc |= test_uplink_keeps_channel_filter_only();
    rc |= test_pull_in();
    rc |= test_squelch_close_reopen();
    rc |= test_block_after_gap_makes_symbols();
    rc |= test_partition_invariance();
    rc |= test_block_partitions();
    rc |= test_block_in_workbuf();
    rc |= test_fll_gears();
    rc |= test_p25_after_tetra_session();
    rc |= test_filter_reset();
    if (rc == 0) {
        DSD_FPRINTF(stdout, "DSP_TETRA_CHAIN: OK\n");
    }
    return rc;
}
