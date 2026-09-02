// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The symbol grid reads the unfiltered discriminator stream until a sync names a
 * protocol, and the matched-filtered one from the accept onward. Those are not
 * the same signal: a symmetric FIR of L taps reports the signal (L-1)/2 samples
 * in the past, so at the moment the filter switches on the decoder starts
 * re-reading content it has already consumed -- 3.35 symbols for NXDN48 at 20
 * samples per symbol, 4.5 for P25p1, and for P25p1 the leftover is exactly half
 * a symbol of phase as well (issue #444).
 *
 * These cases pin what the filter module promises the symbolizer: the delay it
 * reports is the delay its output really has, asking for it disturbs nothing,
 * and a filter primed with the history it missed and then fed its delay's worth
 * of samples reads, from then on, exactly what it would have read had it been
 * running all along. The symbolizer's use of that is SYMBOL_MATCHED_FILTER_SEAM.
 */

#include <assert.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

#define MAX_TAPS DSD_SPS_FILTER_MAX_TAPS
#define SIGNAL   (2 * MAX_TAPS + 256)

static const char*
kind_name(dsd_sps_filter_kind kind) {
    switch (kind) {
        case DSD_SPS_FILTER_DMR: return "dmr";
        case DSD_SPS_FILTER_P25: return "p25";
        case DSD_SPS_FILTER_NXDN: return "nxdn";
        case DSD_SPS_FILTER_DPMR: return "dpmr";
        default: return "none";
    }
}

static const dsd_sps_filter_kind kKinds[] = {DSD_SPS_FILTER_DMR, DSD_SPS_FILTER_P25, DSD_SPS_FILTER_NXDN,
                                             DSD_SPS_FILTER_DPMR};
static const int kSps[] = {20, 10, 8, 5, 4, 3, 2};

/* A deterministic, non-repeating stimulus: every position is distinguishable,
   so a rewind or a skip of even one sample shows up as a mismatch. */
static float
stimulus(int n) {
    return sinf(0.13f * (float)n) * 8000.0f + cosf(0.021f * (float)n) * 3000.0f + 11.0f * (float)(n % 7);
}

/*
 * What @p kind produces at each position when it has been running since the
 * start of the stimulus: the reference every seam case is held to. Indexed by
 * position, so feeding sample n fills entry n - delay.
 */
static void
reference(dsd_sps_filter_kind kind, int sps, float* ref) {
    const int delay = dsd_sps_filter_group_delay(kind, sps);
    init_rrc_filter_memory();
    for (int n = 0; n < SIGNAL; n++) {
        const float y = dsd_sps_filter_apply(kind, stimulus(n), sps);
        if (n - delay >= 0) {
            ref[n - delay] = y;
        }
    }
    for (int p = SIGNAL - delay; p < SIGNAL; p++) {
        ref[p] = 0.0f;
    }
    init_rrc_filter_memory();
}

static int
close_enough(float got, float want) {
    return fabsf(got - want) <= 1e-3f * (fabsf(want) + 1.0f);
}

/*
 * The delay is only knowable because the impulse response is symmetric about
 * it and ends exactly one delay after it. A design that broke either would make
 * the seam correction a guess.
 */
static void
test_impulse_response_is_symmetric_about_the_delay(void) {
    static float y[MAX_TAPS + 16];
    for (unsigned k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        for (unsigned s = 0; s < sizeof(kSps) / sizeof(kSps[0]); s++) {
            const dsd_sps_filter_kind kind = kKinds[k];
            const int sps = kSps[s];
            const int delay = dsd_sps_filter_group_delay(kind, sps);
            DSD_FPRINTF(stderr, "%s sps=%d delay=%d\n", kind_name(kind), sps, delay);
            assert(delay > 0);
            assert(2 * delay + 1 <= MAX_TAPS);

            init_rrc_filter_memory();
            const int outputs = 2 * delay + 1 + 8;
            y[0] = dsd_sps_filter_apply(kind, 1.0f, sps);
            for (int n = 1; n < outputs; n++) {
                y[n] = dsd_sps_filter_apply(kind, 0.0f, sps);
            }
            for (int i = 0; i <= delay; i++) {
                const float a = y[delay - i];
                const float b = y[delay + i];
                const float scale = fabsf(a) > 1e-6f ? fabsf(a) : 1e-6f;
                assert(fabsf(a - b) <= 1e-5f * scale);
            }
            /* Nothing past 2 * delay: the response is exactly 2 * delay + 1 long. */
            for (int n = 2 * delay + 1; n < outputs; n++) {
                assert(fabsf(y[n]) <= 1e-7f);
            }
        }
    }
}

/* An inactive filter has no geometry and must not claim any. */
static void
test_none_and_degenerate_rates_have_no_delay(void) {
    assert(dsd_sps_filter_group_delay(DSD_SPS_FILTER_NONE, 20) == 0);
    assert(dsd_sps_filter_group_delay(DSD_SPS_FILTER_NXDN, 1) == 0);
    assert(dsd_sps_filter_group_delay(DSD_SPS_FILTER_NXDN, 0) == 0);
    /* Priming is a no-op for a filter that is not running, and NONE passes
       samples through untouched. */
    dsd_sps_filter_prime(DSD_SPS_FILTER_NONE, 1.0f, 20);
    const float through = dsd_sps_filter_apply(DSD_SPS_FILTER_NONE, 7.5f, 20);
    assert(fabsf(through - 7.5f) <= 1e-6f);
}

/*
 * Asking about geometry is a query. The seam asks for the delay of the filter it
 * is switching to while the one it is leaving is still running, so the question
 * must not redesign anything, or the answer costs a window of history.
 */
static void
test_group_delay_query_does_not_disturb_a_running_filter(void) {
    static float ref[SIGNAL];
    const dsd_sps_filter_kind kind = DSD_SPS_FILTER_NXDN;
    const int sps = 20;
    reference(kind, sps, ref);
    const int delay = dsd_sps_filter_group_delay(kind, sps);
    const int n0 = MAX_TAPS + 64;

    for (int n = 0; n < n0; n++) {
        (void)dsd_sps_filter_apply(kind, stimulus(n), sps);
    }
    /* Another rate, as the seam asks for at a samples-per-symbol change. */
    assert(dsd_sps_filter_group_delay(kind, 10) > 0);
    assert(dsd_sps_filter_group_delay(kind, 8) > 0);
    for (int p = 0; p < 40; p++) {
        const float got = dsd_sps_filter_apply(kind, stimulus(n0 + p), sps);
        const float want = ref[n0 + p - delay];
        if (!close_enough(got, want)) {
            DSD_FPRINTF(stderr, "after a delay query: p=%d got %.6f want %.6f\n", p, (double)got, (double)want);
        }
        assert(close_enough(got, want));
    }
}

/*
 * The property the seam fix has to have. After the switch-on the grid's next
 * read must return the filtered signal at the position it was about to read,
 * computed from a full window of real samples.
 */
static void
test_prime_and_catch_up_aligns_content(void) {
    static float ref[SIGNAL];
    for (unsigned k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        for (unsigned s = 0; s < sizeof(kSps) / sizeof(kSps[0]); s++) {
            const dsd_sps_filter_kind kind = kKinds[k];
            const int sps = kSps[s];
            reference(kind, sps, ref);
            const int delay = dsd_sps_filter_group_delay(kind, sps);
            const int taps = 2 * delay + 1;
            const int n0 = MAX_TAPS + 64; /* the position the grid was about to read */

            /* Whatever the previous transmission left in the history must not
               reach the output; start from something that is not the signal. */
            for (int n = 0; n < 96; n++) {
                (void)dsd_sps_filter_apply(kind, -20000.0f, sps);
            }
            /* The grid consumed everything before n0; hand the filter the tail
               that can still be in its window, oldest first. */
            for (int n = n0 - taps; n < n0; n++) {
                dsd_sps_filter_prime(kind, stimulus(n), sps);
            }
            /* Pay off the delay once: these outputs describe positions the grid
               has already read, so they are discarded. */
            for (int j = 0; j < delay; j++) {
                (void)dsd_sps_filter_apply(kind, stimulus(n0 + j), sps);
            }
            /* From here on, read p returns the filtered signal at n0 + p. */
            for (int p = 0; p < 40; p++) {
                const float got = dsd_sps_filter_apply(kind, stimulus(n0 + delay + p), sps);
                const float want = ref[n0 + p];
                if (!close_enough(got, want)) {
                    DSD_FPRINTF(stderr, "%s sps=%d p=%d: got %.6f want %.6f\n", kind_name(kind), sps, p, (double)got,
                                (double)want);
                }
                assert(close_enough(got, want));
            }
        }
    }
}

/*
 * Without the prime, the same switch-on reads a window half full of whatever
 * came before. This is the transient the fix removes; if it ever stops being
 * measurable the prime has become unnecessary and this test should say so.
 */
static void
test_unprimed_switch_on_is_measurably_wrong(void) {
    static float ref[SIGNAL];
    const dsd_sps_filter_kind kind = DSD_SPS_FILTER_NXDN;
    const int sps = 20;
    reference(kind, sps, ref);
    const int delay = dsd_sps_filter_group_delay(kind, sps);
    const int n0 = MAX_TAPS + 64;

    for (int n = 0; n < 96; n++) {
        (void)dsd_sps_filter_apply(kind, -20000.0f, sps);
    }
    for (int j = 0; j < delay; j++) {
        (void)dsd_sps_filter_apply(kind, stimulus(n0 + j), sps);
    }
    const float got = dsd_sps_filter_apply(kind, stimulus(n0 + delay), sps);
    const float want = ref[n0];
    DSD_FPRINTF(stderr, "unprimed first output: got %.1f want %.1f\n", (double)got, (double)want);
    assert(!close_enough(got, want));
}

int
main(void) {
    test_impulse_response_is_symmetric_about_the_delay();
    test_none_and_degenerate_rates_have_no_delay();
    test_group_delay_query_does_not_disturb_a_running_filter();
    test_prime_and_catch_up_aligns_content();
    test_unprimed_switch_on_is_measurably_wrong();
    DSD_FPRINTF(stderr, "matched filter seam: OK\n");
    return 0;
}
