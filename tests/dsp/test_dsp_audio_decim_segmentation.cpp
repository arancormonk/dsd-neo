// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief full_demod()'s post-demod audio decimator does not depend on where the blocks are cut (issue #572).
 *
 * An I/Q replay whose sidecar sets post_downsample above 1 decimates the demodulated audio by that factor M: through
 * a 16-tap polyphase decimator, or, when its allocation fails (forced here with
 * dsd_demod_test_fail_post_polydecim_alloc()), through a fallback, a one-pole low-pass and then the mean of each M
 * samples. On both paths:
 *  - the whole stream matches a double-precision reference;
 *  - the stream cut into blocks shorter than M (1-sample blocks, M-1, a cycle of short sizes, random short sizes, and
 *    random sizes mixed with long ones) gives the whole stream's output;
 *  - a block that completes no output publishes 0 samples, not its input;
 *  - dsd_demod_reset_filter_state(), a factor change, a rate change and a switch between the two paths, each after a
 *    part-filled group, leave the next block's output equal to a fresh state's.
 *
 * Each split's deviation from the whole stream is printed whether it passes or not.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <dsd-neo/dsp/demod_pipeline.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/mem.h>
#include <vector>
#include "demod_pipeline_test_support.h"
#include "dsd-neo/core/safe_api.h"

namespace {

constexpr int kRateOutHz = 12000;
constexpr int kStreamSamples = 4001; /* a multiple of no factor tested, so the stream ends on a part-filled group */
/* Measured: every split equals the whole stream exactly on both paths, since the carried state runs the same float
   operations in the same order whatever the cut. */
constexpr float kSplitTol = 0.0f;
constexpr double kRefTol = 1e-5;   /* the whole stream against the double-precision reference */
constexpr int kPrefixSamples = 23; /* before a transition: whole groups, then a part-filled one, for any M 2..5 */
constexpr int kAfterSamples = 64;  /* the block after a transition */
constexpr int kAfterOffset = 1000; /* where that block starts in the stream: other samples than the prefix's */
constexpr double kPi = 3.14159265358979323846;

enum class Path { kPoly, kFallback };

const char*
path_name(Path p) {
    return p == Path::kPoly ? "polyphase" : "fallback";
}

struct DemodBox {
    demod_state* s;

    DemodBox() : s(static_cast<demod_state*>(dsd_neo_aligned_malloc(sizeof(demod_state)))) {
        if (s) {
            DSD_MEMSET(s, 0, sizeof(*s));
        }
    }

    ~DemodBox() {
        if (s) {
            dsd_neo_aligned_free(s->post_polydecim_taps);
            dsd_neo_aligned_free(s->post_polydecim_hist);
            dsd_neo_aligned_free(s);
        }
    }

    DemodBox(const DemodBox&) = delete;
    DemodBox& operator=(const DemodBox&) = delete;
};

/* The demodulator under test: the I channel is the audio, so the decimator's input is exactly the samples fed. */
void
copy_i_demod(struct demod_state* d) {
    const int n = d->lp_len >> 1;
    for (int i = 0; i < n; i++) {
        d->result[i] = d->lowpassed[(size_t)i << 1];
    }
    d->result_len = n;
}

/* A replay's audio chain with only the post-demod decimator acting: no half-band pass, no channel filter, no
   de-emphasis, DC block or audio filter, the squelch level 0 and its envelope open (a gain of exactly 1). */
void
configure(demod_state* s, int factor, int rate_out) {
    s->downsample_passes = 0;
    s->channel_lpf_enable = 0;
    s->post_downsample = factor;
    s->rate_out = rate_out;
    s->rate_out2 = 0;
    s->mode_demod = &copy_i_demod;
    s->squelch_gate_open = 1;
    s->squelch_env = 1.0f;
    s->squelch_env_attack = 0.125f;
    s->squelch_env_release = 0.03125f;
}

void
select_path(Path p) {
    dsd_demod_test_fail_post_polydecim_alloc(p == Path::kFallback ? 1 : 0);
}

/* One block of n samples through full_demod(); its output is appended to out. Returns the published length. */
int
run_block(demod_state* s, const float* x, int n, std::vector<float>& out) {
    for (int i = 0; i < n; i++) {
        s->input_cb_buf[(size_t)i << 1] = x[i];
        s->input_cb_buf[((size_t)i << 1) + 1] = 0.0f;
    }
    s->lowpassed = s->input_cb_buf;
    s->lp_len = n << 1;
    s->result_len = -1;
    full_demod(s);
    for (int i = 0; i < s->result_len; i++) {
        out.push_back(s->result[i]);
    }
    return s->result_len;
}

uint32_t g_rand_state = 0xA0D10DECu;

uint32_t
rand_u32() {
    uint32_t x = g_rand_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rand_state = x;
    return x;
}

/* Two tones, one near the decimated band's edge, and noise. */
std::vector<float>
make_stream() {
    std::vector<float> x((size_t)kStreamSamples);
    for (int n = 0; n < kStreamSamples; n++) {
        const double t = (double)n / (double)kRateOutHz;
        const double noise = ((double)(rand_u32() & 0xFFFFu) / 65535.0) - 0.5;
        x[(size_t)n] =
            (float)(0.6 * std::sin(2.0 * kPi * 700.0 * t) + 0.3 * std::sin(2.0 * kPi * 2900.0 * t + 0.3) + 0.1 * noise);
    }
    return x;
}

/* The fallback's one-pole coefficient, as the pipeline derives it. */
double
fallback_alpha(int rate_out, int factor) {
    const int fs = rate_out > 0 ? rate_out : 48000;
    double fc = 0.2 * ((double)fs / (double)factor);
    if (fc < 50.0) {
        fc = 50.0;
    }
    double a = 1.0 - std::exp(-2.0 * kPi * fc / (double)fs);
    a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
    return (double)(float)a;
}

/* The polyphase decimator: output j is the FIR over the M*(j+1) samples so far, newest first, with zeros before the
   stream. */
std::vector<double>
reference_poly(const std::vector<float>& x, int factor, const float* taps, int k_taps) {
    std::vector<double> y;
    for (int end = factor - 1; end < (int)x.size(); end += factor) {
        double acc = 0.0;
        for (int k = 0; k < k_taps; k++) {
            if (end - k >= 0) {
                acc += (double)taps[k] * (double)x[(size_t)(end - k)];
            }
        }
        y.push_back(acc);
    }
    return y;
}

/* The fallback: a one-pole low-pass started from the first sample, then the mean of each whole group of M. */
std::vector<double>
reference_fallback(const std::vector<float>& x, int factor, int rate_out) {
    const double a = fallback_alpha(rate_out, factor);
    std::vector<double> y;
    double lp = x.empty() ? 0.0 : (double)x[0];
    double sum = 0.0;
    int count = 0;
    for (float v : x) {
        lp += ((double)v - lp) * a;
        sum += lp;
        if (++count == factor) {
            y.push_back(sum / (double)factor);
            sum = 0.0;
            count = 0;
        }
    }
    return y;
}

/* Whether the block that just ran took the path the test asked for. */
int
check_path_taken(const char* what, const demod_state* s, Path p) {
    const int poly = s->post_polydecim_enabled && s->post_polydecim_taps && s->post_polydecim_hist;
    if (poly != (p == Path::kPoly ? 1 : 0)) {
        DSD_FPRINTF(stderr, "%s: ran the %s path, want %s\n", what, poly ? "polyphase" : "fallback", path_name(p));
        return 1;
    }
    return 0;
}

int
compare_outputs(const char* what, const std::vector<float>& got, const std::vector<float>& want) {
    if (got.size() != want.size()) {
        DSD_FPRINTF(stderr, "%s: %zu outputs, want %zu\n", what, got.size(), want.size());
        return 1;
    }
    float max_dev = 0.0f;
    size_t worst = 0;
    for (size_t i = 0; i < got.size(); i++) {
        const float dev = std::fabs(got[i] - want[i]);
        if (dev > max_dev) {
            max_dev = dev;
            worst = i;
        }
    }
    std::printf("  %s: %zu outputs, max dev %.3g\n", what, got.size(), (double)max_dev);
    if (max_dev > kSplitTol) {
        DSD_FPRINTF(stderr, "%s: output %zu is %.9g, want %.9g (tolerance %.3g)\n", what, worst, (double)got[worst],
                    (double)want[worst], (double)kSplitTol);
        return 1;
    }
    return 0;
}

/* The whole stream in one block, against the reference. */
int
run_whole(Path p, int factor, const std::vector<float>& x, std::vector<float>& out) {
    select_path(p);
    DemodBox box;
    if (!box.s) {
        return 1;
    }
    configure(box.s, factor, kRateOutHz);
    (void)run_block(box.s, x.data(), (int)x.size(), out);
    char what[96];
    DSD_SNPRINTF(what, sizeof what, "WHOLE %s M=%d", path_name(p), factor);
    int failed = check_path_taken(what, box.s, p);
    const std::vector<double> ref = p == Path::kPoly
                                        ? reference_poly(x, factor, box.s->post_polydecim_taps, box.s->post_polydecim_K)
                                        : reference_fallback(x, factor, kRateOutHz);
    if (out.size() != ref.size()) {
        DSD_FPRINTF(stderr, "%s: %zu outputs, want %zu (one per whole group of %d)\n", what, out.size(), ref.size(),
                    factor);
        return 1;
    }
    double max_dev = 0.0;
    for (size_t i = 0; i < out.size(); i++) {
        const double dev = std::fabs((double)out[i] - ref[i]);
        max_dev = dev > max_dev ? dev : max_dev;
    }
    std::printf("  %s: %zu outputs, max dev from reference %.3g\n", what, out.size(), max_dev);
    if (max_dev > kRefTol) {
        DSD_FPRINTF(stderr, "%s: off the reference by %.3g (tolerance %.3g)\n", what, max_dev, kRefTol);
        failed = 1;
    }
    return failed;
}

struct Split {
    const char* name;
    int min_size;
    int max_size;
    int cycle;      /* 1: sizes min_size..max_size in turn instead of at random */
    int long_every; /* above 0: every this many blocks is 200..599 samples long */
};

int
run_split(Path p, int factor, const std::vector<float>& x, const Split& sp, const std::vector<float>& whole) {
    select_path(p);
    DemodBox box;
    if (!box.s) {
        return 1;
    }
    configure(box.s, factor, kRateOutHz);
    std::vector<float> out;
    int pos = 0;
    int block = 0;
    int next_cycle = sp.min_size;
    const int total = (int)x.size();
    while (pos < total) {
        int size;
        if (sp.long_every > 0 && (block % sp.long_every) == sp.long_every - 1) {
            size = 200 + (int)(rand_u32() % 400u);
        } else if (sp.cycle) {
            size = next_cycle;
            next_cycle = next_cycle >= sp.max_size ? sp.min_size : next_cycle + 1;
        } else {
            size = sp.min_size + (int)(rand_u32() % (uint32_t)(sp.max_size - sp.min_size + 1));
        }
        size = size < total - pos ? size : total - pos;
        (void)run_block(box.s, x.data() + pos, size, out);
        pos += size;
        block++;
    }
    char what[128];
    DSD_SNPRINTF(what, sizeof what, "SPLIT %s M=%d %s (%d blocks)", path_name(p), factor, sp.name, block);
    int failed = check_path_taken(what, box.s, p);
    failed |= compare_outputs(what, out, whole);
    return failed;
}

int
test_segmentation(Path p, int factor, const std::vector<float>& x) {
    std::vector<float> whole;
    int failed = run_whole(p, factor, x, whole);
    const int short_max = factor - 1;
    const Split splits[] = {
        {"1-sample blocks", 1, 1, 1, 0},
        {"M-1 blocks", short_max, short_max, 1, 0},
        {"cycle 1..M-1", 1, short_max, 1, 0},
        {"random 1..M-1", 1, short_max, 0, 0},
        {"random 1..3M+5 with long blocks", 1, 3 * factor + 5, 0, 8},
    };
    for (const Split& sp : splits) {
        failed |= run_split(p, factor, x, sp, whole);
    }
    return failed;
}

/* A block that completes no output publishes 0 samples; the samples it took come out of the next block. */
int
test_zero_output_blocks(Path p, const std::vector<float>& x) {
    const int factor = 4;
    std::vector<float> whole;
    int failed = run_whole(p, factor, x, whole);
    select_path(p);
    DemodBox box;
    if (!box.s) {
        return 1;
    }
    configure(box.s, factor, kRateOutHz);

    struct Step {
        int size;
        int want_len;
    };

    /* From a fresh state: 3 samples make none, the 4th the first output, 2 more none, then 6 finish two groups. */
    const Step steps[] = {{3, 0}, {1, 1}, {2, 0}, {6, 2}, {1, 0}};
    std::vector<float> out;
    int pos = 0;
    for (const Step& st : steps) {
        const int got = run_block(box.s, x.data() + pos, st.size, out);
        pos += st.size;
        std::printf("  ZERO %s M=%d: a %d-sample block published %d, want %d\n", path_name(p), factor, st.size, got,
                    st.want_len);
        if (got != st.want_len) {
            DSD_FPRINTF(stderr, "ZERO %s: a %d-sample block published %d samples, want %d\n", path_name(p), st.size,
                        got, st.want_len);
            failed = 1;
        }
    }
    /* What the blocks made is the whole stream's first outputs. */
    if (out.size() > whole.size()) {
        DSD_FPRINTF(stderr, "ZERO %s: %zu outputs from %d samples\n", path_name(p), out.size(), pos);
        return 1;
    }
    std::vector<float> want;
    want.reserve(out.size());
    for (size_t i = 0; i < out.size(); i++) {
        want.push_back(whole[i]);
    }
    char what[64];
    DSD_SNPRINTF(what, sizeof what, "ZERO %s M=%d outputs", path_name(p), factor);
    failed |= compare_outputs(what, out, want);
    return failed;
}

/* State @p used has run a part-filled prefix and then had a transition applied; @p fresh is configured the way @p used
   is now. The next block must give both the same output. */
int
expect_fresh(const char* what, demod_state* used, demod_state* fresh, const std::vector<float>& x, Path p) {
    std::vector<float> a;
    std::vector<float> b;
    (void)run_block(used, x.data() + kAfterOffset, kAfterSamples, a);
    int failed = check_path_taken(what, used, p);
    (void)run_block(fresh, x.data() + kAfterOffset, kAfterSamples, b);
    failed |= check_path_taken(what, fresh, p);
    failed |= compare_outputs(what, a, b);
    return failed;
}

/* Run the prefix on @p s with path @p p, factor @p factor and rate @p rate_out. */
void
fill_part(demod_state* s, Path p, int factor, int rate_out, const std::vector<float>& x) {
    select_path(p);
    configure(s, factor, rate_out);
    std::vector<float> scratch;
    (void)run_block(s, x.data(), kPrefixSamples, scratch);
}

int
test_reset(Path p, const std::vector<float>& x) {
    DemodBox used;
    DemodBox fresh;
    if (!used.s || !fresh.s) {
        return 1;
    }
    fill_part(used.s, p, 4, kRateOutHz, x);
    dsd_demod_reset_filter_state(used.s);
    configure(fresh.s, 4, kRateOutHz);
    char what[96];
    DSD_SNPRINTF(what, sizeof what, "RESET %s: the block after dsd_demod_reset_filter_state()", path_name(p));
    return expect_fresh(what, used.s, fresh.s, x, p);
}

/* Both paths' state part-filled at once (the polyphase decimator by a run, the fallback's fields set as a part-filled
   group leaves them, which no run does while the decimator is allocated): the reset clears all of it. */
int
test_reset_both_paths_state(const std::vector<float>& x) {
    DemodBox box;
    if (!box.s) {
        return 1;
    }
    demod_state* s = box.s;
    fill_part(s, Path::kPoly, 4, kRateOutHz, x);
    int failed = check_path_taken("RESET both", s, Path::kPoly);
    if (s->post_polydecim_phase == 0) {
        DSD_FPRINTF(stderr, "RESET both: the prefix left no part-filled group\n");
        failed = 1;
    }
    s->post_fallback_lp_y = 0.5f;
    s->post_fallback_lp_valid = 1;
    s->post_fallback_box_acc = -1.25f;
    s->post_fallback_box_phase = 3;
    dsd_demod_reset_filter_state(s);
    float hist_max = 0.0f;
    for (int k = 0; k < s->post_polydecim_K; k++) {
        const float v = std::fabs(s->post_polydecim_hist[k]);
        hist_max = v > hist_max ? v : hist_max;
    }
    std::printf("  RESET both: polyphase hist max %.3g head %d phase %d; fallback y %.3g valid %d acc %.3g phase %d\n",
                (double)hist_max, s->post_polydecim_hist_head, s->post_polydecim_phase, (double)s->post_fallback_lp_y,
                s->post_fallback_lp_valid, (double)s->post_fallback_box_acc, s->post_fallback_box_phase);
    if (hist_max > 1e-12f || s->post_polydecim_hist_head != 0 || s->post_polydecim_phase != 0
        || std::fabs(s->post_fallback_lp_y) > 1e-12f || s->post_fallback_lp_valid != 0
        || std::fabs(s->post_fallback_box_acc) > 1e-12f || s->post_fallback_box_phase != 0) {
        DSD_FPRINTF(stderr, "RESET both: dsd_demod_reset_filter_state() left decimator state behind\n");
        failed = 1;
    }
    /* The decimator stays allocated, so the next block runs it from its start. */
    if (!s->post_polydecim_taps || !s->post_polydecim_hist || s->post_polydecim_K != 16) {
        DSD_FPRINTF(stderr, "RESET both: the reset released the decimator\n");
        failed = 1;
    }
    return failed;
}

int
test_factor_change(Path p, const std::vector<float>& x) {
    DemodBox used;
    DemodBox fresh;
    if (!used.s || !fresh.s) {
        return 1;
    }
    fill_part(used.s, p, 4, kRateOutHz, x);
    used.s->post_downsample = 3;
    configure(fresh.s, 3, kRateOutHz);
    char what[96];
    DSD_SNPRINTF(what, sizeof what, "FACTOR %s: M 4 -> 3", path_name(p));
    return expect_fresh(what, used.s, fresh.s, x, p);
}

int
test_rate_change(Path p, const std::vector<float>& x) {
    DemodBox used;
    DemodBox fresh;
    if (!used.s || !fresh.s) {
        return 1;
    }
    fill_part(used.s, p, 4, kRateOutHz, x);
    used.s->rate_out = 16000;
    configure(fresh.s, 4, 16000);
    char what[96];
    DSD_SNPRINTF(what, sizeof what, "RATE %s: rate_out %d -> 16000 at M 4", path_name(p), kRateOutHz);
    return expect_fresh(what, used.s, fresh.s, x, p);
}

/* The fallback ran part of a group, then the decimator can be allocated: the polyphase decimator starts fresh. The
   polyphase decimator ran part of a group, then a factor change cannot allocate one: the fallback starts fresh (with
   the decimator allocated, only a redesign allocates, so the switch the other way comes with a factor change). */
int
test_path_switch(const std::vector<float>& x) {
    int failed = 0;
    {
        DemodBox used;
        DemodBox fresh;
        if (!used.s || !fresh.s) {
            return 1;
        }
        fill_part(used.s, Path::kFallback, 4, kRateOutHz, x);
        select_path(Path::kPoly);
        configure(fresh.s, 4, kRateOutHz);
        failed |= expect_fresh("SWITCH fallback -> polyphase at M 4", used.s, fresh.s, x, Path::kPoly);
    }
    {
        DemodBox used;
        DemodBox fresh;
        if (!used.s || !fresh.s) {
            return 1;
        }
        fill_part(used.s, Path::kPoly, 4, kRateOutHz, x);
        select_path(Path::kFallback);
        used.s->post_downsample = 3;
        configure(fresh.s, 3, kRateOutHz);
        failed |= expect_fresh("SWITCH polyphase -> fallback, M 4 -> 3", used.s, fresh.s, x, Path::kFallback);
    }
    return failed;
}

} // namespace

int
main(void) {
    const std::vector<float> x = make_stream();
    int failed = 0;
    const Path paths[] = {Path::kPoly, Path::kFallback};
    for (Path p : paths) {
        for (int factor = 2; factor <= 5; factor++) {
            failed |= test_segmentation(p, factor, x);
        }
        failed |= test_zero_output_blocks(p, x);
        failed |= test_reset(p, x);
        failed |= test_factor_change(p, x);
        failed |= test_rate_change(p, x);
    }
    failed |= test_reset_both_paths_state(x);
    failed |= test_path_switch(x);
    select_path(Path::kPoly);
    if (failed) {
        DSD_FPRINTF(stderr, "DSP_AUDIO_DECIM_SEGMENTATION: FAILED\n");
        return 1;
    }
    std::printf("DSP_AUDIO_DECIM_SEGMENTATION: OK\n");
    return 0;
}
