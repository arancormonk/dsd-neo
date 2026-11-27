// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The dynamic squelches behind a replay or network I/Q reader's resampler (issue #899), held to the production reader
 * (a dsd_iq_resampler of the reader's plan, run ahead of full_demod() as the IO layer runs it, its phase kept across
 * block cuts) and the real cascade and channel filter.
 *
 * - The installed floor plan against the chain's own covariance, exactly: an impulse at every input phase of the
 *   chain's period through the reader (primed with silence) and full_demod(), the products of each response summed
 *   into the row of the output phase they fall on (the reader's bank makes the noise periodic, so one sum over output
 *   positions would lose it). 8 kHz upsampled to 16 kHz (no pass, with no channel filter and with NFM 12.5k), 8 kHz to
 *   48 kHz (NFM 16k), 40 kHz to 32 kHz and one pass (two output phases survive the decimator), 250 kHz to 192 kHz and
 *   two passes (a downsample by 96/125), 48,001 Hz read into 48,000 Hz (L/M 1/1, NFM 16k), and a source at the
 *   cascade's own input rate (no reader: the cascade's plan). Against independent figures at 8 to 16 kHz: beta
 *   0.54180, N_eff 317.75, noise gain 0.98030.
 * - The reader model against the same impulses: every output phase's covariance, its period, the mean over it and the
 *   largest distance from it.
 * - From cold start on a fixed seed, white noise at the source rate through the reader and full_demod() under AUTO
 *   (8 kHz to 16 kHz unfiltered, 8 kHz to 48 kHz NFM 16k): not one sample opens on the idle channel, the floor is known
 *   by the 8th window, an FM carrier inside the source band opens the gate, which closes within the close delay after
 *   it drops and stays shut after.
 * - NOISE on the reader: native 48 kHz channels (unfiltered at noise+10, NFM 16k at noise+3) and an 8 kHz source read
 *   into them all stay shut on idle noise; the reader's passband edge (3.6 kHz) leaves no noise band, so the tracker
 *   runs it as auto. Where a band fits (a 25 kHz source read into 48 kHz, NFM 12.5k), the reader's magnitude shapes the
 *   calibration and idle noise stays shut.
 * - The reader names the plan and the floor's context: one demod reopening the same replay as 8 kHz read into 16 kHz,
 *   then a native 16 kHz source, then 8 kHz into 16 kHz again designs a plan for each, learns afresh on the native
 *   source, and seeds the returning context from its stored floor.
 * - The plan is designed when the rate plan commits, so the first blocks behind the reader design nothing.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/core/power.h>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/dsp/halfband.h>
#include <dsd-neo/dsp/iq_resampler.h>
#include <dsd-neo/dsp/live_iq_step.h>
#include <dsd-neo/dsp/nfm_noise_squelch.h>
#include <dsd-neo/dsp/squelch_floor.h>
#include <dsd-neo/dsp/squelch_reader_model.h>
#include <dsd-neo/dsp/squelch_step_model.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/mem.h>
#include <limits>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "squelch_cascade_noise.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {

int g_failures = 0;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            DSD_FPRINTF(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                   \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
            DSD_FPRINTF(stderr, "\n");                                                                                 \
            g_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

/* full_demod()'s blocks: at most this many complex samples each. */
constexpr int kBlockPairs = 16384;
/* The reader's input cut: an odd count, so every block boundary falls at another phase of the reader's period. */
constexpr size_t kReaderCut = 997U;

/* xorshift64* with Box-Muller, both values used. */
struct Rng {
    uint64_t s;
    int have = 0;
    double spare = 0.0;

    explicit Rng(uint64_t seed) : s(seed) {}

    double
    uniform() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return ((double)((s * 0x2545F4914F6CDD1DULL) >> 11) + 0.5) * (1.0 / 9007199254740992.0);
    }

    double
    gauss() {
        if (have) {
            have = 0;
            return spare;
        }
        const double mag = std::sqrt(-2.0 * std::log(uniform()));
        const double ang = 2.0 * M_PI * uniform();
        spare = mag * std::sin(ang);
        have = 1;
        return mag * std::cos(ang);
    }
};

struct DemodBox {
    demod_state* s;

    DemodBox() : s(static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)))) {
        if (s) {
            DSD_MEMSET(s, 0, sizeof(*s));
        }
    }

    ~DemodBox() {
        if (s) {
            dsd_live_iq_step_disable(&s->live_iq_step);
            dsd_squelch_step_model_release(&s->squelch_step_model);
            dsd_squelch_reader_model_release(&s->squelch_reader_model);
            dsd_neo_aligned_free(s->post_polydecim_taps);
            dsd_neo_aligned_free(s->post_polydecim_hist);
            dsd_neo_aligned_free(s);
        }
    }

    DemodBox(const DemodBox&) = delete;
    DemodBox& operator=(const DemodBox&) = delete;
};

/* A chain: a source at @p source_hz read into @p dsp_in_hz (the reader resamples when they differ), @p passes half-band
   passes, the channel filter at NFM @p width (0: off) at the channel rate dsp_in_hz / 2^passes. */
struct ReaderChain {
    const char* name;
    uint32_t source_hz;
    uint32_t dsp_in_hz;
    int passes;
    int width;

    int
    rate() const {
        return (int)(dsp_in_hz >> passes);
    }
};

/* The context the IO layer builds for a replay (demod_take_squelch_setting()): the chain key, the rate, and the
   reader's plan. */
void
key_context(demod_state* s) {
    DSD_MEMSET(&s->squelch_context, 0, sizeof(s->squelch_context));
    s->squelch_context.freq_hz = 162475000;
    s->squelch_context.rate_hz = s->complex_rate;
    s->squelch_context.chain = 1 | (s->downsample_passes << 1) | (1 << 8);
    if (s->iq_reader.enabled) {
        s->squelch_context.reader_in_hz = s->iq_reader.in_hz;
        s->squelch_context.reader_out_hz = s->iq_reader.out_hz;
        s->squelch_context.reader_protect_hz = s->iq_reader.protect_hz;
    }
}

/* The FM monitor on @p c under @p mode (margin 10 dB), with the reader's plan published and its model built as the
   rate plan's commit does (dsd_demod_squelch_reader_model_sync()), and the context keyed as the IO layer keys it.
   Returns the sync's result. */
int
configure(demod_state* s, const ReaderChain& c, int mode) {
    s->mode_demod = &dsd_fm_demod;
    s->analog_family = 1;
    s->analog_demod = DSD_ANALOG_DEMOD_FM;
    s->output_kind = DSD_DEMOD_OUTPUT_AUDIO_MONITOR;
    s->channel_lpf_profile = DSD_CH_LPF_PROFILE_WIDE;
    s->channel_lpf_enable = c.width > 0 ? 1 : 0;
    s->channel_lpf_width_hz = c.width;
    s->rate_in = c.rate();
    s->rate_out = c.rate();
    s->complex_rate = c.rate();
    s->post_downsample = 1;
    s->downsample_passes = c.passes;
    s->squelch_mode = mode;
    s->squelch_margin_db = 10;
    s->squelch_env = 1.0f;
    const int planned = dsd_iq_reader_plan_for(&s->iq_reader, c.source_hz, c.dsp_in_hz, (uint64_t)c.rate());
    CHECK(planned == 0, "%s: no reader plan", c.name);
    key_context(s);
    return dsd_demod_squelch_reader_model_sync(s);
}

/* The production reader for a chain: a resampler of its plan, or none (the source passes through). */
struct Reader {
    dsd_iq_reader_plan plan;
    dsd_iq_resampler r;
    std::vector<float> out;

    Reader(const ReaderChain& c, int silent) {
        dsd_iq_resampler_init(&r);
        (void)dsd_iq_reader_plan_for(&plan, c.source_hz, c.dsp_in_hz, (uint64_t)c.rate());
        if (plan.enabled) {
            const int rc = dsd_iq_reader_plan_configure(&plan, &r, kReaderCut);
            CHECK(rc == DSD_IQ_RESAMPLER_OK, "%s: the reader did not design (%d)", c.name, rc);
            if (silent) {
                dsd_iq_resampler_clear_silent(&r);
            }
        }
    }

    ~Reader() { dsd_iq_resampler_free(&r); }

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    int
    L() const {
        return plan.enabled ? r.L : 1;
    }

    int
    M() const {
        return plan.enabled ? r.M : 1;
    }
};

/* @p pairs source samples @p in through the reader, cut into kReaderCut inputs, and each cut's output through
   full_demod() in blocks; @p sink sees every block's channel output. */
template <class Sink>
void
feed(demod_state* s, Reader& rd, const float* in, size_t pairs, Sink&& sink) {
    for (size_t at = 0; at < pairs; at += kReaderCut) {
        const size_t n = std::min(kReaderCut, pairs - at);
        const float* src = in + (2U * at);
        size_t made = n;
        if (rd.plan.enabled) {
            rd.out.resize(2U * dsd_iq_resampler_max_out(&rd.r, n));
            const int got = dsd_iq_resampler_process(&rd.r, src, n, rd.out.data(), rd.out.size() / 2U);
            CHECK(got >= 0, "the reader refused a cut");
            made = got > 0 ? (size_t)got : 0U;
            src = rd.out.data();
        }
        for (size_t done = 0; done < made; done += (size_t)kBlockPairs) {
            const int k = (int)std::min((size_t)kBlockPairs, made - done);
            DSD_MEMCPY(s->input_cb_buf, src + (2U * done), (size_t)k * 2U * sizeof(float));
            s->lowpassed = s->input_cb_buf;
            s->lp_len = k * 2;
            full_demod(s);
            if (!s->front_end_empty && s->lowpassed != NULL && s->lp_len > 0) {
                sink(s->lowpassed, s->lp_len / 2);
            }
        }
    }
}

int
gcd(int a, int b) {
    while (b != 0) {
        const int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* The chain's covariance at every output phase, from impulses, and the plan it designs. */
struct Oracle {
    int period = 1;
    int lags = 0;
    std::vector<double> cov; /* period rows of lags */
    std::vector<float> taps;
    dsd_squelch_floor_plan plan;
};

/* An impulse at every input phase of the chain's period through the production reader (primed with silence) and
   full_demod() (LEVEL squelch: nothing gated): a source shift of Q = M 2^passes / gcd(L, 2^passes) inputs shifts the
   output by P = L / gcd(L, 2^passes), so the impulses, gap = 1 mod Q apart and far enough that no two responses meet,
   hit each input phase once, and the products of each response summed into the row of the output phase they fall on
   are every phase's covariance for white unit noise. Scaled by 2^(passes - 1), the noise_gain convention, and designed
   at that period with the tracker's mean window. */
Oracle
oracle(const ReaderChain& c) {
    Oracle o;
    DSD_MEMSET(&o.plan, 0, sizeof o.plan);
    const DemodBox box;
    configure(box.s, c, DSD_SQUELCH_MODE_LEVEL);
    Reader rd(c, 1);
    const int L = rd.L();
    const int M = rd.M();
    const int g = gcd(L, 1 << c.passes);
    const int inputs = (M << c.passes) / g;
    o.period = L / g;
    const double out_per_in = (double)L / ((double)M * (double)(1 << c.passes));
    std::vector<float> designed(DSD_NOISE_SQUELCH_MAX_TAPS, 0.0f);
    const int taps_len =
        c.width > 0 ? dsd_channel_lpf_design_analog(c.rate(), c.width, designed.data(), DSD_NOISE_SQUELCH_MAX_TAPS) : 0;
    const int reader_span = rd.plan.enabled ? (rd.r.taps_per_phase * L) / M + 2 : 1;
    o.lags = (reader_span >> c.passes) + 48 + (taps_len > 0 ? taps_len : DSD_NOISE_SQUELCH_MAX_TAPS);
    long gap = inputs + 1;
    while ((double)gap * out_per_in < (double)(2 * o.lags + 16)) {
        gap += inputs;
    }
    const long lead = 4L * gap;
    const long total = lead + ((long)inputs * gap) + (4L * gap);
    std::vector<float> in((size_t)total * 2U, 0.0f);
    for (int k = 0; k < inputs; k++) {
        in[(size_t)(lead + (long)k * gap) * 2U] = 1.0f;
    }
    std::vector<float> out;
    feed(box.s, rd, in.data(), (size_t)total, [&](const float* iq, int pairs) {
        for (size_t i = 0; i < (size_t)pairs; i++) {
            out.push_back(iq[2U * i]);
        }
    });
    if (c.width > 0) {
        o.taps.assign(box.s->channel_lpf_plan_taps, box.s->channel_lpf_plan_taps + box.s->channel_lpf_plan_taps_len);
    }
    o.cov.assign((size_t)o.period * (size_t)o.lags, 0.0);
    for (size_t n = 0; n < out.size(); n++) {
        const double y = (double)out[n];
        double* row = o.cov.data() + ((n % (size_t)o.period) * (size_t)o.lags);
        for (int m = 0; m < o.lags && n + (size_t)m < out.size(); m++) {
            row[m] += y * (double)out[n + (size_t)m];
        }
    }
    for (double& v : o.cov) {
        v = std::ldexp(v, c.passes > 0 ? c.passes - 1 : 0);
    }
    dsd_squelch_floor_periodic_noise noise;
    noise.period = o.period;
    noise.lags = o.lags;
    noise.cov = o.cov.data();
    const float* plan_taps = o.taps.empty() ? NULL : o.taps.data();
    const int rc =
        dsd_squelch_floor_plan_design_periodic_mean(&o.plan, plan_taps, (int)o.taps.size(), &noise, c.rate());
    CHECK(rc == 0 && o.plan.valid, "%s: the oracle plan is not valid", c.name);
    return o;
}

/* Complex Gaussian noise of variance @p var at the source rate, and from @p on_s to @p off_s an FM carrier of amplitude
   @p amp (a 1 kHz tone at 1.5 kHz deviation, inside an 8 kHz source's band): @p pairs samples from sample @p at. */
struct SourceNoise {
    Rng rng;
    double sd;
    double amp;
    double on_s;
    double off_s;
    double mod = 0.0;

    void
    fill(std::vector<float>* out, long at, size_t pairs, uint32_t source_hz) {
        out->resize(pairs * 2U);
        for (size_t k = 0; k < pairs; k++) {
            const double t = (double)(at + (long)k) / (double)source_hz;
            double re = sd * rng.gauss();
            double im = sd * rng.gauss();
            if (t >= on_s && t < off_s) {
                mod += 2.0 * M_PI * 1000.0 / (double)source_hz;
                re += amp * std::cos(1.5 * std::sin(mod));
                im += amp * std::sin(1.5 * std::sin(mod));
            }
            (*out)[k * 2U] = (float)re;
            (*out)[(k * 2U) + 1U] = (float)im;
        }
    }
};

/* @p seconds of the source through the reader and @p s in 10 ms pieces; each block's flags appended to @p flags (may be
   NULL). The first time the tracker has closed 8 windows, whether its floor is known lands in @p known_by_8 (may be
   NULL). */
void
run_source(demod_state* s, Reader& rd, SourceNoise* src, uint32_t source_hz, long* at, double seconds,
           std::vector<uint8_t>* flags, int* known_by_8) {
    const long end = *at + (long)(seconds * (double)source_hz);
    const size_t piece = source_hz / 100U;
    std::vector<float> block;
    while (*at < end) {
        const size_t n = (size_t)std::min<long>((long)piece, end - *at);
        src->fill(&block, *at, n, source_hz);
        *at += (long)n;
        feed(s, rd, block.data(), n, [&](const float*, int pairs) {
            if (flags && s->result_len == pairs) {
                flags->insert(flags->end(), s->result_flags, s->result_flags + pairs);
            }
        });
        dsd_squelch_floor_status st;
        dsd_squelch_floor_get_status(&s->squelch_floor, &st);
        if (known_by_8 && *known_by_8 < 0 && st.windows >= 8U) {
            *known_by_8 = st.state == DSD_SQUELCH_FLOOR_KNOWN ? 1 : 0;
        }
    }
}

/* How far @p got is from @p want: beta and rho absolute, N_eff and the noise gain relative; the largest double (a finite sentinel, the
   fast-math builds reject infinity) for another lag
   or rate, or a plan not valid. */
double
plan_distance(const dsd_squelch_floor_plan& got, const dsd_squelch_floor_plan& want) {
    if (!got.valid || !want.valid || got.lag != want.lag || got.rate_hz != want.rate_hz) {
        return std::numeric_limits<double>::max();
    }
    const double rel =
        std::fmax(std::fabs(got.neff / want.neff - 1.0), std::fabs(got.noise_gain / want.noise_gain - 1.0));
    return std::fmax(rel, std::fmax(std::fabs(got.beta - want.beta), std::fabs(got.rho_lag - want.rho_lag)));
}

/* The float chain's rounding against the plans' doubles: the bound the cascade's own plans are held to (1e-5, under
   1e-7 on x86). The upsampling reader's periodic design and its phase-averaged one differ by 1.5e-4 in beta at 8 to
   16 kHz, and a plan that leaves the reader out misses by 0.54. */
constexpr double kReaderTolerance = 1e-5;

/* The plan full_demod() installs under AUTO behind the reader after the first block that leaves channel samples, and
   the taps it ran; the model's period, whether the plan is its every-phase design (@p exact), and the every-phase
   design by each route (@p routes: the responses', the covariance's; not valid with no model). */
dsd_squelch_floor_plan
installed_plan(const ReaderChain& c, std::vector<float>* taps, int* period, int* exact,
               dsd_squelch_floor_plan routes[2]) {
    const DemodBox box;
    demod_state* s = box.s;
    const int synced = configure(s, c, DSD_SQUELCH_MODE_AUTO);
    CHECK(synced == 0, "%s: the reader model did not build", c.name);
    *period = s->squelch_reader_model.ready ? s->squelch_reader_model.period : 0;
    Reader rd(c, 0);
    SourceNoise src{Rng{0x899E0000ULL + c.source_hz}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
    long at = 0;
    for (int i = 0; i < 64 && !s->squelch_auto_ran; i++) {
        run_source(s, rd, &src, c.source_hz, &at, 0.01, NULL, NULL);
    }
    CHECK(s->squelch_auto_ran, "%s: no channel block under AUTO", c.name);
    taps->assign(s->channel_lpf_plan_taps, s->channel_lpf_plan_taps + s->channel_lpf_plan_taps_len);
    if (s->channel_lpf_enable == 0) {
        taps->clear();
    }
    *exact = 0;
    DSD_MEMSET(routes, 0, 2 * sizeof(routes[0]));
    const dsd_squelch_floor_plan installed = s->squelch_floor.plan;
    if (s->squelch_reader_model.ready) {
        dsd_squelch_floor_plan full;
        const float* run = taps->empty() ? NULL : taps->data();
        const int len = (int)taps->size();
        *exact = dsd_squelch_reader_model_floor_plan_exact(&s->squelch_reader_model, &full, run, len, c.rate()) == 0
                 && dsd_squelch_floor_plan_equal(&full, &installed);
        (void)dsd_squelch_reader_model_floor_plan_route(&s->squelch_reader_model, &routes[0], run, len, c.rate(),
                                                        DSD_SQUELCH_READER_ROUTE_RESPONSES);
        (void)dsd_squelch_reader_model_floor_plan_route(&s->squelch_reader_model, &routes[1], run, len, c.rate(),
                                                        DSD_SQUELCH_READER_ROUTE_COVARIANCE);
    }
    return installed;
}

/* ------------------------------------------------------------------------------------------- the installed plan */

/* Behind the reader the auto squelch's plan is the chain's own (issue #899): white noise at the source rate through the
   reader's bank, the cascade and the channel filter, at every output phase. Every row's installed plan, and the
   every-phase design by either route, matches the impulse oracle's; 8 to 16 kHz unfiltered matches the independent
   figures (and is not the cascade's plan without the reader: beta 0); two output phases survive the pass at 40 to 32
   to 16 kHz; with no reader the cascade's own plan. */
void
test_installed_plan_is_the_chain(void) {
    struct Row {
        ReaderChain c;
        int period;              /* the output period the model holds, 0: not checked */
        double beta, neff, gain; /* independent figures, 0: none */
    };

    const Row rows[] = {
        {{"8 kHz to 16 kHz unfiltered", 8000U, 16000U, 0, 0}, 2, 0.54180, 317.75, 0.98030},
        {{"8 kHz to 16 kHz, NFM 12.5k", 8000U, 16000U, 0, 12500}, 2, 0.0, 0.0, 0.0},
        {{"8 kHz to 48 kHz, NFM 16k", 8000U, 48000U, 0, 16000}, 6, 0.0, 0.0, 0.0},
        {{"40 kHz to 32 kHz, 1 pass to 16 kHz", 40000U, 32000U, 1, 0}, 2, 0.0, 0.0, 0.0},
        {{"250 kHz to 192 kHz, 2 passes to 48 kHz, NFM 12.5k", 250000U, 192000U, 2, 12500}, 24, 0.0, 0.0, 0.0},
        {{"48,001 Hz to 48,000 Hz, NFM 16k", 48001U, 48000U, 0, 16000}, 1, 0.0, 0.0, 0.0},
        {{"64 kHz, 2 passes to 16 kHz: no reader", 64000U, 64000U, 2, 0}, 0, 0.0, 0.0, 0.0},
    };

    double worst = 0.0;
    int failed = 0;
    for (const Row& row : rows) {
        std::vector<float> taps;
        int period = 0;
        int exact = 0;
        dsd_squelch_floor_plan routes[2];
        const dsd_squelch_floor_plan got = installed_plan(row.c, &taps, &period, &exact, routes);
        const Oracle want = oracle(row.c);
        double off = plan_distance(got, want.plan);
        if (period > 0) {
            off = std::fmax(off, std::fmax(plan_distance(routes[0], want.plan), plan_distance(routes[1], want.plan)));
        }
        worst = std::fmax(worst, off);
        bool ok = off <= kReaderTolerance && taps == want.taps;
        if (row.period > 0) {
            ok = ok && period == row.period && want.period == row.period;
        }
        if (row.beta > 0.0) {
            ok = ok && std::fabs(got.beta - row.beta) <= 1e-5 && std::fabs(got.neff - row.neff) <= 1e-2
                 && std::fabs(got.noise_gain - row.gain) <= 1e-5;
        }
        failed += ok ? 0 : 1;
        printf("reader: %s (%zu taps): period %d, %s design, lag %d beta %.5f N_eff %.2f gain %.8f, off the impulses' "
               "by %.2e\n",
               row.c.name, taps.size(), period, exact && period > 1 ? "every-phase" : "phase-averaged", got.lag,
               got.beta, got.neff, got.noise_gain, off);
        CHECK(ok,
              "%s (%zu taps, period %d): installed lag %d beta %.6f N_eff %.3f gain %.8f, the real chain (period %d) "
              "lag %d beta %.6f N_eff %.3f gain %.8f (off by %.3e)",
              row.c.name, taps.size(), period, got.lag, got.beta, got.neff, got.noise_gain, want.period, want.plan.lag,
              want.plan.beta, want.plan.neff, want.plan.noise_gain, off);
    }
    /* With no reader the plan is the cascade's own, designed as with no replay. */
    {
        const ReaderChain c = {"64 kHz, 2 passes", 64000U, 64000U, 2, 0};
        std::vector<float> taps;
        int period = 0;
        int exact = 0;
        dsd_squelch_floor_plan routes[2];
        const dsd_squelch_floor_plan got = installed_plan(c, &taps, &period, &exact, routes);
        dsd_noise_squelch_stage st[2] = {{hb31_q15_taps, 31}, {hb_q15_taps, HB_TAPS}};
        dsd_squelch_floor_plan want;
        (void)dsd_squelch_cascade_noise_floor_plan(&want, NULL, 0, st, 2, 16000);
        CHECK(dsd_squelch_floor_plan_equal(&got, &want) && period == 0,
              "64 kHz, 2 passes: no reader, but not the cascade's plan");
    }
    printf("reader: the installed floor plan is the real chain's at %zu chains, within %.2e (%d off)\n",
           sizeof(rows) / sizeof(rows[0]), worst, failed);
}

/* ------------------------------------------------------------------------------------------------ the model */

/* The largest distance between the model's rows (@p got, period P, @p got_lags) and the oracle's (@p want), over the
   lags both hold, at the rotation of the period that fits best (a constant shift of the output count moves nothing a
   plan uses), relative to the mean power. */
double
rows_distance(const double* got, int got_lags, const Oracle& want) {
    const int lags = std::min(got_lags, want.lags);
    const int P = want.period;
    double power = 0.0;
    for (int n = 0; n < P; n++) {
        power += want.cov[(size_t)n * (size_t)want.lags] / (double)P;
    }
    double best = std::numeric_limits<double>::max();
    for (int r = 0; r < P; r++) {
        double worst = 0.0;
        for (int n = 0; n < P; n++) {
            const double* a = got + ((size_t)((n + r) % P) * (size_t)got_lags);
            const double* b = want.cov.data() + ((size_t)n * (size_t)want.lags);
            for (int m = 0; m < lags; m++) {
                worst = std::fmax(worst, std::fabs(a[m] - b[m]));
            }
        }
        best = std::fmin(best, worst);
    }
    return power > 0.0 ? best / power : std::numeric_limits<double>::max();
}

/* The reader model against the impulses, unfiltered: every output phase's covariance (its exact plan's table, by each
   route: from the responses, and from the reader's covariance through the cascade), the period, and the mean over it
   and the largest distance from it (from the reader's covariance carried through the cascade phase by phase). Within
   1e-6 of the power: the float chain's rounding. That bound checks the reader-to-cascade alignment only to about 1e-6
   of the power: on these chains the cosets of the reader's phases the decimators keep differ that little (a
   one-sample shift moves the 250 to 192 kHz mean by 1.2e-6, the 40 to 32 kHz rows not at all, their cosets mirroring
   each other; elsewhere a coset moves a plan more, 5e-5 at 112 kHz into 64 kHz, which the IO layer keeps aligned), and
   the rows are compared up to a rotation of the period (a constant shift of the output count moves nothing a plan
   uses). */
void
test_model_is_the_chain(void) {
    const ReaderChain chains[] = {
        {"8 kHz to 16 kHz", 8000U, 16000U, 0, 0},
        {"8 kHz to 48 kHz", 8000U, 48000U, 0, 0},
        {"40 kHz to 32 kHz, 1 pass", 40000U, 32000U, 1, 0},
        {"250 kHz to 192 kHz, 2 passes", 250000U, 192000U, 2, 0},
    };
    for (const ReaderChain& c : chains) {
        const Oracle want = oracle(c);
        dsd_iq_reader_plan plan;
        (void)dsd_iq_reader_plan_for(&plan, c.source_hz, c.dsp_in_hz, (uint64_t)c.rate());
        dsd_noise_squelch_stage st[DSD_NOISE_SQUELCH_MAX_STAGES];
        for (int i = 0; i < c.passes; i++) {
            st[i].taps = i == 0 ? hb31_q15_taps : hb_q15_taps;
            st[i].len = i == 0 ? 31 : HB_TAPS;
        }
        dsd_squelch_reader_model m;
        dsd_squelch_reader_model_init(&m);
        const int built = dsd_squelch_reader_model_build(&m, &plan, st, c.passes);
        CHECK(built == 0 && m.ready && m.period == want.period, "%s: the model did not build at period %d", c.name,
              want.period);
        if (built != 0 || !m.ready) {
            dsd_squelch_reader_model_release(&m);
            continue;
        }
        /* Each route's table, held to the impulses on its own. */
        dsd_squelch_floor_plan exact;
        int rc = dsd_squelch_reader_model_floor_plan_route(&m, &exact, NULL, 0, c.rate(),
                                                           DSD_SQUELCH_READER_ROUTE_RESPONSES);
        double rows = rows_distance(m.cov, m.exact_lags, want);
        dsd_squelch_floor_plan by_cov;
        rc |= dsd_squelch_reader_model_floor_plan_route(&m, &by_cov, NULL, 0, c.rate(),
                                                        DSD_SQUELCH_READER_ROUTE_COVARIANCE);
        rows = std::fmax(rows, rows_distance(m.cov, m.exact_lags, want));
        /* The oracle's mean and largest distance from it. */
        std::vector<double> mean((size_t)want.lags, 0.0);
        for (int n = 0; n < want.period; n++) {
            for (int k = 0; k < want.lags; k++) {
                mean[(size_t)k] += want.cov[((size_t)n * (size_t)want.lags) + (size_t)k] / (double)want.period;
            }
        }
        double dev = 0.0;
        for (int n = 0; n < want.period; n++) {
            for (int k = 0; k < want.lags; k++) {
                dev =
                    std::fmax(dev, std::fabs(want.cov[((size_t)n * (size_t)want.lags) + (size_t)k] - mean[(size_t)k]));
            }
        }
        double mean_off = 0.0;
        for (int k = 0; k < std::max(m.lags, want.lags); k++) {
            const double a = k < m.lags ? m.mean[k] : 0.0;
            const double b = k < want.lags ? mean[(size_t)k] : 0.0;
            mean_off = std::fmax(mean_off, std::fabs(a - b));
        }
        const double power = mean[0];
        CHECK(rc == 0 && rows <= 1e-6 && mean_off <= 1e-6 * power && std::fabs(m.cyclo_dev - dev) <= 1e-6 * power,
              "%s: the model's phases off the impulses' by %.3e, its mean by %.3e, its largest distance %.6f against "
              "%.6f",
              c.name, rows, mean_off / power, m.cyclo_dev / power, dev / power);
        printf("reader model: %s: %d output phases, %d input phases, largest distance from the mean %.4f %% of the "
               "power, every phase within %.2e of the impulses'\n",
               c.name, m.period, m.inputs, 100.0 * m.cyclo_dev / power, rows);
        dsd_squelch_reader_model_release(&m);
    }
}

/* ------------------------------------------------------------------------------------------- from cold start */

/* White complex noise at the source rate through the reader and full_demod() from cold start on a fixed seed under
   AUTO: not one sample opens on the idle channel, the floor is known by the 8th window, an FM carrier 25 dB over the
   noise opens the gate inside it, the gate closes within the close delay (and 10 ms of chain delay) after it drops, and
   not one sample opens on the idle channel after. */
void
test_cold_start_stays_shut(void) {
    const ReaderChain chains[] = {
        {"8 kHz to 16 kHz unfiltered", 8000U, 16000U, 0, 0},
        {"8 kHz to 48 kHz, NFM 16k", 8000U, 48000U, 0, 16000},
    };
    for (const ReaderChain& c : chains) {
        const DemodBox box;
        demod_state* s = box.s;
        configure(s, c, DSD_SQUELCH_MODE_AUTO);
        Reader rd(c, 0);
        const double var = 1e-3;
        SourceNoise src{Rng{0x899F0000ULL + c.dsp_in_hz}, std::sqrt(var / 2.0), std::sqrt(std::pow(10.0, 2.5) * var),
                        1.0, 1.5};
        long at = 0;
        std::vector<uint8_t> flags;
        int known_by_8 = -1;
        run_source(s, rd, &src, c.source_hz, &at, 3.0, &flags, &known_by_8);
        const int rate = c.rate();
        const size_t burst_from = (size_t)(1.05 * rate);
        const size_t burst_to = (size_t)(1.5 * rate);
        const size_t tail_from = burst_to + (size_t)((DSD_SQUELCH_CLOSE_DELAY_MS + 10) * rate / 1000);
        long idle_open = 0;
        long burst_open = 0;
        long tail_open = 0;
        for (size_t k = 0; k < flags.size(); k++) {
            const int open = (flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U;
            if (k < (size_t)rate) {
                idle_open += open;
            } else if (k >= burst_from && k < burst_to) {
                burst_open += open;
            } else if (k >= tail_from) {
                tail_open += open;
            }
        }
        printf("DSP_SQUELCH_READER: %s, AUTO from cold start: %ld idle samples open, known by window 8 %d, carrier "
               "%.1f %% open, %ld samples open after the drop\n",
               c.name, idle_open, known_by_8, 100.0 * (double)burst_open / (double)(burst_to - burst_from), tail_open);
        CHECK(flags.size() >= tail_from + (size_t)rate && idle_open == 0 && known_by_8 == 1
                  && burst_open >= (long)((burst_to - burst_from) * 95U / 100U) && tail_open == 0,
              "%s: the idle channel opened, the floor was not known by window 8, or the carrier did not open", c.name);
    }
}

/* Idle noise at the source rate for @p seconds through the reader and full_demod() under @p mode from cold start:
   the samples that open. */
long
idle_opens(demod_state* s, const ReaderChain& c, double seconds) {
    Reader rd(c, 0);
    SourceNoise src{Rng{0x89A00000ULL + c.source_hz + c.dsp_in_hz}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
    long at = 0;
    std::vector<uint8_t> flags;
    run_source(s, rd, &src, c.source_hz, &at, seconds, &flags, NULL);
    long open = 0;
    for (uint8_t f : flags) {
        open += (f & DSD_SQUELCH_FLAG_CLOSED) == 0U;
    }
    CHECK(flags.size() >= (size_t)(0.9 * seconds * c.rate()), "%s: too few flags (%zu)", c.name, flags.size());
    return open;
}

/* NOISE on a reader chain (issue #899): its calibration must see the reader. Native 48 kHz channels (unfiltered at
   noise+10, NFM 16k at noise+3) run the noise squelch and stay shut on idle noise; an 8 kHz source read into the same
   chains has nothing past the reader's 3.6 kHz edge, so no band fits, the tracker runs the setting as auto, and it
   stays shut too (a plan calibrated without the reader reads idle noise there as 13 dB and 6 dB of quieting, and
   opens). A 12 kHz source has no band either: its edge is 5.4 kHz, and the band ends 800 Hz under it, 4.6 kHz, less
   than the band's 1200 Hz over its 3.8 kHz floor. A 25 kHz source read into 48 kHz keeps a band (its edge is 11.25
   kHz): the plan is valid, its calibration noise carries the reader's magnitude (the cascade FIR runs with no pass),
   and idle noise stays shut. */
void
test_noise_behind_the_reader(void) {
    struct Case {
        ReaderChain c;
        int margin;     /* N, dB */
        int noise_runs; /* 1: the noise squelch's own plan runs; 0: it has none and the tracker runs auto */
    };

    const Case cases[] = {
        {{"native 48 kHz unfiltered", 48000U, 48000U, 0, 0}, 10, 1},
        {{"8 kHz read into 48 kHz unfiltered", 8000U, 48000U, 0, 0}, 10, 0},
        {{"native 48 kHz, NFM 16k", 48000U, 48000U, 0, 16000}, 3, 1},
        {{"8 kHz read into 48 kHz, NFM 16k", 8000U, 48000U, 0, 16000}, 3, 0},
        {{"25 kHz read into 48 kHz, NFM 12.5k", 25000U, 48000U, 0, 12500}, 10, 1},
        {{"12 kHz read into 48 kHz, NFM 12.5k", 12000U, 48000U, 0, 12500}, 3, 0},
        {{"12 kHz read into 48 kHz unfiltered", 12000U, 48000U, 0, 0}, 3, 0},
    };
    for (const Case& k : cases) {
        const DemodBox box;
        demod_state* s = box.s;
        configure(s, k.c, DSD_SQUELCH_MODE_NOISE);
        s->squelch_margin_db = k.margin;
        const long open = idle_opens(s, k.c, 2.0);
        const int noise_valid = s->noise_squelch.plan.valid;
        printf("DSP_SQUELCH_READER: %s, noise+%d: %ld idle samples open, %s\n", k.c.name, k.margin, open,
               noise_valid ? "the noise squelch ran" : "run as auto");
        CHECK(open == 0 && noise_valid == k.noise_runs && (k.noise_runs ? s->squelch_noise_ran : s->squelch_auto_ran),
              "%s: NOISE opened on idle noise, or ran the wrong squelch (noise plan valid %d)", k.c.name, noise_valid);
        if (k.noise_runs && s->iq_reader.enabled) {
            dsd_noise_squelch_plan plain;
            (void)dsd_noise_squelch_plan_design(&plain, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len, NULL, 0,
                                                k.c.rate());
            CHECK(s->noise_squelch.plan.cascade_len == DSD_NOISE_SQUELCH_CASCADE_TAPS && plain.cascade_len == 0
                      && plain.valid && !dsd_noise_squelch_plan_equal(&plain, &s->noise_squelch.plan)
                      && s->noise_squelch.plan.hi_hz <= s->squelch_reader_model.reader.passband_hz,
                  "%s: the noise plan did not take the reader's magnitude and edge", k.c.name);
        }
    }
}

/* A reader whose passband edge cuts into the noise band but leaves more than its 1200 Hz (issue #899): 16 kHz read into
   48 kHz (edge 7.2 kHz) under NFM 16k and with no channel filter, where the channel alone would leave the band's top
   higher. The band ends the 800 Hz guard under the reader's edge (6.4 kHz; its transition band would cut a carrier's
   sidebands into the band's top), and the plan's calibration noise carries the reader's magnitude, so idle noise reads
   no quieting: shut from cold start at noise+3 and noise+10, and a carrier inside the source band (25 dB over the
   noise, 1.0 to 1.5 s) opens it. Calibrated on noise that runs flat to the channel's edge (the magnitude left out of
   the cascade FIR), the band's idle noise reads as quieting. */
/* The guard the band's top keeps under the reader's passband edge (k_edge_guard_hz, src/dsp/nfm_noise_squelch.c). */
constexpr double kReaderBandGuardHz = 800.0;

/* Idle noise reads about 0 dB of quieting when the calibration saw the same noise: within 0.16 dB on these rows with
   the reader's magnitude folded in, 0.43 to 4.6 dB without it. */
constexpr double kIdleQuietingDb = 0.3;

void
test_noise_band_clipped_by_the_reader(void) {
    const ReaderChain chains[] = {
        {"16 kHz read into 48 kHz, NFM 16k", 16000U, 48000U, 0, 16000},
        {"16 kHz read into 48 kHz unfiltered", 16000U, 48000U, 0, 0},
    };
    const int margins[] = {3, 10};
    for (const ReaderChain& c : chains) {
        for (const int margin : margins) {
            const DemodBox box;
            demod_state* s = box.s;
            configure(s, c, DSD_SQUELCH_MODE_NOISE);
            s->squelch_margin_db = margin;
            Reader rd(c, 0);
            const double var = 1e-3;
            SourceNoise src{Rng{0x89A50000ULL + (uint64_t)margin + (uint64_t)c.width}, std::sqrt(var / 2.0),
                            std::sqrt(std::pow(10.0, 2.5) * var), 1.0, 1.5};
            long at = 0;
            std::vector<uint8_t> flags;
            /* The idle quieting the windows read from 0.2 s to 1 s (10 ms pieces, the last window's each time). */
            double q_sum = 0.0;
            int q_count = 0;
            for (int piece = 0; piece < 250; piece++) {
                run_source(s, rd, &src, c.source_hz, &at, 0.01, &flags, NULL);
                dsd_noise_squelch_status st;
                dsd_noise_squelch_get_status(&s->noise_squelch, &st);
                if (piece >= 20 && piece < 100) {
                    q_sum += st.quieting_db;
                    q_count++;
                }
            }
            const double idle_q = q_count > 0 ? q_sum / (double)q_count : 0.0;
            const int rate = c.rate();
            const size_t burst_from = (size_t)(1.05 * rate);
            const size_t burst_to = (size_t)(1.5 * rate);
            long idle_open = 0;
            long burst_open = 0;
            for (size_t k = 0; k < flags.size(); k++) {
                const int open = (flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U;
                if (k < (size_t)rate) {
                    idle_open += open;
                } else if (k >= burst_from && k < burst_to) {
                    burst_open += open;
                }
            }
            const dsd_noise_squelch_plan* plan = &s->noise_squelch.plan;
            const double edge = s->squelch_reader_model.ready ? s->squelch_reader_model.reader.passband_hz : 0.0;
            printf("DSP_SQUELCH_READER: %s, noise+%d: band %.0f to %.0f Hz (reader edge %.0f Hz), idle quieting "
                   "%.2f dB, %ld idle samples open, carrier %.1f %% open\n",
                   c.name, margin, plan->lo_hz, plan->hi_hz, edge, idle_q, idle_open,
                   100.0 * (double)burst_open / (double)(burst_to - burst_from));
            const double top = edge - kReaderBandGuardHz;
            CHECK(plan->valid && s->squelch_noise_ran && edge > 0.0 && std::fabs(plan->hi_hz - top) <= 1e-9 * edge
                      && plan->hi_hz - plan->lo_hz >= 1200.0 && flags.size() >= burst_to && idle_open == 0
                      && std::fabs(idle_q) <= kIdleQuietingDb
                      && burst_open >= (long)((burst_to - burst_from) * 95U / 100U),
                  "%s, noise+%d: the band is not the reader's clipped one, the idle channel opened, or the carrier did "
                  "not open",
                  c.name, margin);
        }
    }
}

/* -------------------------------------------------------------------------------------------------- identity */

/* The reader names the plan and the floor's context (issue #899): one demod reopens the same replay path as 8 kHz read
   into 16 kHz (learns its floor), as a native 16 kHz source (another plan, designed once, learned afresh: the 8 kHz
   floor is not the native channel's), then as 8 kHz into 16 kHz again (its kept plan taken, nothing designed, its floor
   seeded from the density stored when it left, times the gain). Each leg opens as production does: the reader model's
   sync and the plan's prime at the commit, then blocks, so the new plan is in before the first block takes the
   context. */
void
test_reader_names_the_context(void) {
    const ReaderChain read = {"8 kHz read into 16 kHz", 8000U, 16000U, 0, 0};
    const ReaderChain native = {"native 16 kHz", 16000U, 16000U, 0, 0};
    const DemodBox box;
    demod_state* s = box.s;
    configure(s, read, DSD_SQUELCH_MODE_AUTO);
    dsd_demod_squelch_prime_reader_plan(s);
    {
        Reader rd(read, 0);
        SourceNoise src{Rng{0x89A1ULL}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
        long at = 0;
        run_source(s, rd, &src, read.source_hz, &at, 1.0, NULL, NULL);
    }
    const dsd_squelch_floor_key key_read = s->squelch_context;
    const double floor_read = s->squelch_floor.floor_power;
    const double gain_read = s->squelch_floor.plan.noise_gain;
    const uint64_t designs_read = s->squelch_plan_designs;
    CHECK(s->squelch_floor.state == DSD_SQUELCH_FLOOR_KNOWN && designs_read == 1U,
          "8 kHz into 16 kHz: no floor (state %d), or not one design (%llu)", s->squelch_floor.state,
          (unsigned long long)designs_read);

    /* The native source: its own plan, and a context the 8 kHz floor does not belong to. The prime does nothing with
       the reader off; the first block designs. */
    configure(s, native, DSD_SQUELCH_MODE_AUTO);
    dsd_demod_squelch_prime_reader_plan(s);
    {
        Reader rd(native, 0);
        SourceNoise src{Rng{0x89A2ULL}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
        long at = 0;
        run_source(s, rd, &src, native.source_hz, &at, 0.01, NULL, NULL);
    }
    dsd_squelch_floor_plan native_plan;
    (void)dsd_squelch_floor_plan_design(&native_plan, NULL, 0, NULL, 0, 16000);
    double density = 0.0;
    const int stored = dsd_squelch_floor_cache_find(&s->squelch_cache, &key_read, &density);
    CHECK(s->squelch_plan_designs == designs_read + 1U
              && dsd_squelch_floor_plan_equal(&native_plan, &s->squelch_floor.plan)
              && s->squelch_floor.state == DSD_SQUELCH_FLOOR_LEARNING && stored == 1
              && std::fabs(density - (floor_read / gain_read)) <= 1e-12 * (floor_read / gain_read),
          "native 16 kHz: not its own plan (designs %llu), not learning afresh (state %d), or the 8 kHz floor not "
          "stored at its density",
          (unsigned long long)s->squelch_plan_designs, s->squelch_floor.state);

    /* Back to 8 kHz into 16 kHz: its kept plan (the model built again for the same reader), its floor seeded from the
       stored density times the gain. */
    configure(s, read, DSD_SQUELCH_MODE_AUTO);
    dsd_demod_squelch_prime_reader_plan(s);
    {
        Reader rd(read, 0);
        SourceNoise src{Rng{0x89A3ULL}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
        long at = 0;
        run_source(s, rd, &src, read.source_hz, &at, 0.01, NULL, NULL);
    }
    CHECK(s->squelch_plan_designs == designs_read + 1U
              && std::fabs(s->squelch_floor.plan.noise_gain - gain_read) <= 1e-12 * gain_read
              && s->squelch_floor.state == DSD_SQUELCH_FLOOR_SEEDED
              && std::fabs(s->squelch_floor.floor_power - floor_read) <= 1e-12 * floor_read,
          "back to 8 kHz into 16 kHz: not its kept plan (designs %llu), or not seeded from its floor (state %d)",
          (unsigned long long)s->squelch_plan_designs, s->squelch_floor.state);
}

/* 10 ms of noise at the source rate through the reader and @p s. */
void
run_piece(demod_state* s, const ReaderChain& c, uint64_t seed) {
    Reader rd(c, 0);
    SourceNoise src{Rng{seed}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
    long at = 0;
    run_source(s, rd, &src, c.source_hz, &at, 0.01, NULL, NULL);
}

/* The floor plans are kept by plan key (issue #899), as the noise squelch's are: a channel alternating NFM widths
   behind a reader designs each width once; another reader identity or pass count is another key and designs; the last
   DSD_DEMOD_FLOOR_PLAN_CACHE plans stay, the one taken longest ago giving way. */
void
test_floor_plans_kept_by_key(void) {
    const DemodBox box;
    demod_state* s = box.s;
    ReaderChain c = {"8 kHz read into 48 kHz", 8000U, 48000U, 0, 12500};
    configure(s, c, DSD_SQUELCH_MODE_AUTO);
    const int widths[] = {12500, 16000, 12500, 16000};
    const uint64_t want[] = {1U, 2U, 2U, 2U};
    for (int i = 0; i < 4; i++) {
        s->channel_lpf_width_hz = widths[i];
        run_piece(s, c, 0x89A60000ULL + (uint64_t)i);
        CHECK(s->squelch_plan_designs == want[i] && s->squelch_floor.plan.valid,
              "8 kHz into 48 kHz, NFM %d: %llu designs, %llu expected", widths[i],
              (unsigned long long)s->squelch_plan_designs, (unsigned long long)want[i]);
    }
    /* The same width read from another rate (another reader identity), then natively: other keys. */
    c = ReaderChain{"12 kHz read into 48 kHz", 12000U, 48000U, 0, 16000};
    configure(s, c, DSD_SQUELCH_MODE_AUTO);
    run_piece(s, c, 0x89A60010ULL);
    const ReaderChain native = {"native 48 kHz", 48000U, 48000U, 0, 16000};
    configure(s, native, DSD_SQUELCH_MODE_AUTO);
    run_piece(s, native, 0x89A60011ULL);
    CHECK(s->squelch_plan_designs == 4U, "another reader identity took a kept plan (%llu designs)",
          (unsigned long long)s->squelch_plan_designs);
    /* Another pass count at the same channel rate (96 kHz and one pass): another key. */
    const ReaderChain one_pass = {"native 96 kHz, 1 pass", 96000U, 96000U, 1, 16000};
    configure(s, one_pass, DSD_SQUELCH_MODE_AUTO);
    run_piece(s, one_pass, 0x89A60012ULL);
    CHECK(s->squelch_plan_designs == 5U, "another pass count took a kept plan (%llu designs)",
          (unsigned long long)s->squelch_plan_designs);
    /* The bound: five widths natively, then the first again (given way), then the last (kept). */
    configure(s, native, DSD_SQUELCH_MODE_AUTO);
    const int cycle[] = {10000, 12500, 20000, 25000, 11000};
    for (int i = 0; i < 5; i++) {
        s->channel_lpf_width_hz = cycle[i];
        run_piece(s, native, 0x89A60020ULL + (uint64_t)i);
    }
    const uint64_t filled = s->squelch_plan_designs;
    s->channel_lpf_width_hz = cycle[0];
    run_piece(s, native, 0x89A60030ULL);
    const uint64_t evicted = s->squelch_plan_designs;
    s->channel_lpf_width_hz = cycle[4];
    run_piece(s, native, 0x89A60031ULL);
    CHECK(filled == 10U && evicted == 11U && s->squelch_plan_designs == 11U,
          "the kept plans are not bounded at %d (%llu, %llu, %llu designs)", (int)DSD_DEMOD_FLOOR_PLAN_CACHE,
          (unsigned long long)filled, (unsigned long long)evicted, (unsigned long long)s->squelch_plan_designs);
}

/* A reader with hundreds of thousands of input phases (2,097,152 Hz read into 2,096,640 Hz by 4095/4096 ahead of seven
   passes: 524,288) keeps its every-phase plan: the covariance route's tables do not grow with Q, and the responses
   route runs them in batches. Both routes give the same plan, which its small spread keeps within 1e-6 of the
   averaged one. */
void
test_model_with_many_input_phases(void) {
    dsd_iq_reader_plan plan;
    (void)dsd_iq_reader_plan_for(&plan, 2097152U, 2096640U, 16380U);
    dsd_noise_squelch_stage st[7];
    for (int i = 0; i < 7; i++) {
        st[i].taps = i == 0 ? hb31_q15_taps : hb_q15_taps;
        st[i].len = i == 0 ? 31 : HB_TAPS;
    }
    dsd_squelch_reader_model m;
    dsd_squelch_reader_model_init(&m);
    const int built = dsd_squelch_reader_model_build(&m, &plan, st, 7);
    dsd_squelch_floor_plan averaged;
    dsd_squelch_floor_plan by_cov;
    dsd_squelch_floor_plan by_resp;
    const int rc_avg = dsd_squelch_reader_model_floor_plan(&m, &averaged, NULL, 0, 16380);
    const int rc_cov =
        dsd_squelch_reader_model_floor_plan_route(&m, &by_cov, NULL, 0, 16380, DSD_SQUELCH_READER_ROUTE_COVARIANCE);
    const int rc_resp =
        dsd_squelch_reader_model_floor_plan_route(&m, &by_resp, NULL, 0, 16380, DSD_SQUELCH_READER_ROUTE_RESPONSES);
    const double off = std::fmax(plan_distance(by_cov, by_resp), plan_distance(by_cov, averaged));
    printf("reader model: 2,097,152 Hz into 2,096,640 Hz, 7 passes: built %d, %d input phases in batches of %d, "
           "every-phase tables %d, averaged beta %.5f, every-phase beta %.5f, the routes and the average within %.2e\n",
           built, m.inputs, m.batch, m.exact_ready, averaged.beta, by_cov.beta, off);
    CHECK(built == 0 && m.ready && m.exact_ready && m.inputs == 524288 && m.batch < m.inputs && rc_avg == 0
              && rc_cov == 0 && rc_resp == 0 && off <= 1e-6,
          "a model with many input phases lost its every-phase plan, or its routes disagree (%.3e)", off);
    dsd_squelch_reader_model_release(&m);
}

/* A reader whose every-phase design the channel taps call for at thousands of input phases (issue #899): 864,058 Hz
   read into 576 kHz (L/M 2729/4094), three passes to 72 kHz, and the 5 kHz analog channel filter (201 taps): Q =
   32,752 input phases and P = 2729 output phases, and the taps' bound sends the plan to every phase. The plan is
   valid, the one the covariance route designs (the responses route agrees within 1e-9), and from cold start under AUTO
   not one idle sample opens while a carrier 25 dB over the noise opens the gate. */
void
test_large_reader_filtered(void) {
    const ReaderChain c = {"864,058 Hz read into 576 kHz, 3 passes to 72 kHz, 5 kHz filter", 864058U, 576000U, 3, 5000};
    const DemodBox box;
    demod_state* s = box.s;
    const int synced = configure(s, c, DSD_SQUELCH_MODE_AUTO);
    dsd_demod_squelch_prime_reader_plan(s);
    const dsd_squelch_floor_plan primed = s->squelch_floor.plan;
    dsd_squelch_reader_model* m = &s->squelch_reader_model;
    dsd_squelch_floor_plan by_cov;
    const int rc_cov =
        dsd_squelch_reader_model_floor_plan_route(m, &by_cov, s->channel_lpf_plan_taps, s->channel_lpf_plan_taps_len,
                                                  c.rate(), DSD_SQUELCH_READER_ROUTE_COVARIANCE);
    CHECK(synced == 0 && m->ready && m->exact_ready && m->inputs == 32752 && m->period == 2729
              && s->channel_lpf_plan_taps_len == 201 && primed.valid && rc_cov == 0
              && plan_distance(primed, by_cov) <= 1e-12,
          "%s: no valid every-phase plan (model %d, every-phase tables %d, Q %d, P %d, %d taps, plan valid %d)", c.name,
          m->ready, m->exact_ready, m->inputs, m->period, s->channel_lpf_plan_taps_len, primed.valid);
    if (m->exact_ready) {
        dsd_squelch_floor_plan by_resp;
        const int rc_resp = dsd_squelch_reader_model_floor_plan_route(m, &by_resp, s->channel_lpf_plan_taps,
                                                                      s->channel_lpf_plan_taps_len, c.rate(),
                                                                      DSD_SQUELCH_READER_ROUTE_RESPONSES);
        CHECK(rc_resp == 0 && plan_distance(by_resp, by_cov) <= 1e-9, "%s: the routes disagree by %.3e", c.name,
              plan_distance(by_resp, by_cov));
        printf("reader model: %s: %d input phases, %d output phases, beta %.5f N_eff %.2f gain %.8f, the routes within "
               "%.2e\n",
               c.name, m->inputs, m->period, by_cov.beta, by_cov.neff, by_cov.noise_gain,
               plan_distance(by_resp, by_cov));
    }
    Reader rd(c, 0);
    const double var = 1e-3;
    SourceNoise src{Rng{0x89A8ULL}, std::sqrt(var / 2.0), std::sqrt(std::pow(10.0, 2.5) * var), 1.0, 1.4};
    long at = 0;
    std::vector<uint8_t> flags;
    run_source(s, rd, &src, c.source_hz, &at, 1.6, &flags, NULL);
    const int rate = c.rate();
    const size_t burst_from = (size_t)(1.05 * rate);
    const size_t burst_to = (size_t)(1.4 * rate);
    long idle_open = 0;
    long burst_open = 0;
    for (size_t k = 0; k < flags.size() && k < burst_to; k++) {
        const int open = (flags[k] & DSD_SQUELCH_FLAG_CLOSED) == 0U;
        idle_open += k < (size_t)rate ? open : 0;
        burst_open += k >= burst_from ? open : 0;
    }
    printf("DSP_SQUELCH_READER: %s, AUTO from cold start: %ld idle samples open, carrier %.1f %% open\n", c.name,
           idle_open, 100.0 * (double)burst_open / (double)(burst_to - burst_from));
    CHECK(flags.size() >= burst_to && idle_open == 0 && burst_open >= (long)((burst_to - burst_from) * 95U / 100U),
          "%s: the idle channel opened (%ld samples), or the carrier did not open", c.name, idle_open);
}

/* The reader and the live step never run together (the IO layer refuses such an open): should both be on, the DSP
   side designs no plan either way. 2.5 MS/s through five passes and the 384/625 step to 48 kHz with a reader plan
   published beside it, NFM 16k under NOISE: no noise plan, no floor plan, and the gate open on every sample (the fail-
   open the gate keeps without a plan). The reader model is built for that chain, so only the guard keeps a plan out. */
void
test_reader_and_step_design_nothing(void) {
    const int modes[] = {DSD_SQUELCH_MODE_NOISE, DSD_SQUELCH_MODE_AUTO};
    for (const int mode : modes) {
        const DemodBox box;
        demod_state* s = box.s;
        configure(s, ReaderChain{"48 kHz", 48000U, 48000U, 0, 16000}, mode);
        s->downsample_passes = 5;
        dsd_live_iq_step_plan step;
        DSD_MEMSET(&step, 0, sizeof step);
        step.enabled = 1;
        step.passes = 5;
        step.hb_out_num = 78125;
        step.L = 384;
        step.M = 625;
        step.target_hz = 48000;
        step.protect_hz = 21600;
        step.stop_hz = 26400;
        int rc = dsd_demod_live_step_prepare(s, &step);
        if (rc == DSD_LIVE_IQ_STEP_OK) {
            rc = dsd_live_iq_step_commit(&s->live_iq_step);
        }
        dsd_live_iq_step_finish(&s->live_iq_step);
        /* A reader the model can design for this chain (1.6 MS/s read into 48 kHz x 2^5), so only the guard stands
           between it and a plan. */
        (void)dsd_iq_reader_plan_for(&s->iq_reader, 1600000U, 1536000U, 48000U);
        const int synced = dsd_demod_squelch_reader_model_sync(s);
        CHECK(rc == DSD_LIVE_IQ_STEP_OK && s->live_iq_step.enabled && s->iq_reader.enabled && synced == 0
                  && s->squelch_reader_model.ready,
              "the step did not commit, or the reader model did not build");
        Rng rng(0x89A7ULL + (uint64_t)mode);
        std::vector<float> block((size_t)kBlockPairs * 2U);
        long closed = 0;
        long flagged = 0;
        for (int b = 0; b < 32; b++) {
            for (float& v : block) {
                v = (float)(std::sqrt(1e-3 / 2.0) * rng.gauss());
            }
            DSD_MEMCPY(s->input_cb_buf, block.data(), block.size() * sizeof(float));
            s->lowpassed = s->input_cb_buf;
            s->lp_len = kBlockPairs * 2;
            full_demod(s);
            for (int i = 0; i < s->result_len; i++) {
                closed += (s->result_flags[i] & DSD_SQUELCH_FLAG_CLOSED) != 0U;
                flagged++;
            }
        }
        const char* name = mode == DSD_SQUELCH_MODE_AUTO ? "AUTO" : "NOISE";
        CHECK(s->squelch_auto_ran && s->squelch_plan_designs >= 1U && !s->squelch_floor.plan.valid
                  && !s->noise_squelch.plan.valid && flagged > 0 && closed == 0,
              "%s with the reader and the step both on: a plan was designed (floor %d, noise %d), or the gate closed "
              "(%ld of %ld)",
              name, s->squelch_floor.plan.valid, s->noise_squelch.plan.valid, closed, flagged);
    }
}

/* The plan is designed when the rate plan commits (dsd_demod_squelch_prime_reader_plan(), after the model's sync),
   for the channel plan the chain runs, so the demod thread's first blocks design nothing: a reader with thousands of
   output phases takes tens of milliseconds to design from every phase. 8 kHz read into 48 kHz, NFM 16k. With the
   reader off it designs nothing. */
void
test_plan_designed_at_commit(void) {
    const ReaderChain c = {"8 kHz read into 48 kHz, NFM 16k", 8000U, 48000U, 0, 16000};
    const DemodBox box;
    demod_state* s = box.s;
    configure(s, c, DSD_SQUELCH_MODE_AUTO);
    dsd_demod_squelch_prime_reader_plan(s);
    const uint64_t at_commit = s->squelch_plan_designs;
    const dsd_squelch_floor_plan primed = s->squelch_floor.plan;
    Reader rd(c, 0);
    SourceNoise src{Rng{0x89A4ULL}, std::sqrt(1e-3 / 2.0), 0.0, 0.0, 0.0};
    long at = 0;
    run_source(s, rd, &src, c.source_hz, &at, 0.2, NULL, NULL);
    CHECK(at_commit == 1U && primed.valid && s->squelch_auto_ran && s->squelch_plan_designs == 1U
              && dsd_squelch_floor_plan_equal(&primed, &s->squelch_floor.plan),
          "%s: the plan was not designed at the commit (%llu), or the blocks designed it again (%llu)", c.name,
          (unsigned long long)at_commit, (unsigned long long)s->squelch_plan_designs);

    const DemodBox native;
    configure(native.s, ReaderChain{"native 48 kHz", 48000U, 48000U, 0, 16000}, DSD_SQUELCH_MODE_AUTO);
    dsd_demod_squelch_prime_reader_plan(native.s);
    CHECK(native.s->squelch_plan_designs == 0U, "native 48 kHz: a plan was designed with no reader");
}

} // namespace

int
main(void) {
    test_installed_plan_is_the_chain();
    test_model_is_the_chain();
    test_cold_start_stays_shut();
    test_noise_behind_the_reader();
    test_noise_band_clipped_by_the_reader();
    test_reader_names_the_context();
    test_floor_plans_kept_by_key();
    test_model_with_many_input_phases();
    test_large_reader_filtered();
    test_reader_and_step_design_nothing();
    test_plan_designed_at_commit();
    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "DSP_SQUELCH_READER: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("DSP_SQUELCH_READER: OK\n");
    return 0;
}
