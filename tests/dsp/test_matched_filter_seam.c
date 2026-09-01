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
 * These cases pin the geometry that makes the delay knowable, and the property
 * the fix has to have: after the switch, the value the grid reads for a position
 * is the filtered signal at that position, with no rewind and no transient.
 */

#include <assert.h>
#include <dsd-neo/dsp/sps_filters.h>
#include <math.h>
#include <stdio.h>
#include "dsd-neo/core/safe_api.h"

#define MAX_TAPS DSD_SPS_FILTER_MAX_TAPS

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

/*
 * The delay is only knowable because the taps are symmetric and odd in length.
 * A design that broke either would make the seam correction a guess.
 */
static void
test_taps_are_symmetric_and_odd(void) {
    static float taps[MAX_TAPS];
    for (unsigned k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        for (unsigned s = 0; s < sizeof(kSps) / sizeof(kSps[0]); s++) {
            const int sps = kSps[s];
            const int len = dsd_sps_filter_copy_taps(kKinds[k], sps, taps, MAX_TAPS);
            DSD_FPRINTF(stderr, "%s sps=%d taps=%d delay=%d\n", kind_name(kKinds[k]), sps, len,
                        dsd_sps_filter_group_delay(kKinds[k], sps));
            assert(len > 0);
            assert((len & 1) == 1);
            assert(dsd_sps_filter_taps_len(kKinds[k], sps) == len);
            assert(dsd_sps_filter_group_delay(kKinds[k], sps) == (len - 1) / 2);
            for (int i = 0; i < len / 2; i++) {
                const float a = taps[i];
                const float b = taps[len - 1 - i];
                const float scale = fabsf(a) > 1e-6f ? fabsf(a) : 1e-6f;
                assert(fabsf(a - b) <= 1e-5f * scale);
            }
        }
    }
}

/* An inactive filter has no geometry and must not claim any. */
static void
test_none_and_degenerate_rates_have_no_delay(void) {
    static float taps[MAX_TAPS];
    assert(dsd_sps_filter_group_delay(DSD_SPS_FILTER_NONE, 20) == 0);
    assert(dsd_sps_filter_taps_len(DSD_SPS_FILTER_NONE, 20) == 0);
    assert(dsd_sps_filter_copy_taps(DSD_SPS_FILTER_NONE, 20, taps, MAX_TAPS) == 0);
    assert(dsd_sps_filter_group_delay(DSD_SPS_FILTER_NXDN, 1) == 0);
    assert(dsd_sps_filter_group_delay(DSD_SPS_FILTER_NXDN, 0) == 0);
    /* A buffer that cannot hold the taps gets nothing rather than a prefix. */
    assert(dsd_sps_filter_copy_taps(DSD_SPS_FILTER_NXDN, 20, taps, 4) == 0);
    /* Priming is a no-op for a filter that is not running. */
    const float probe[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    dsd_sps_filter_prime(DSD_SPS_FILTER_NONE, probe, 4, 20);
    assert(dsd_sps_filter_apply(DSD_SPS_FILTER_NONE, 7.5f, 20) == 7.5f);
}

/* A deterministic, non-repeating stimulus: every position is distinguishable,
   so a rewind or a skip of even one sample shows up as a mismatch. */
static float
stimulus(int n) {
    return sinf(0.13f * (float)n) * 8000.0f + cosf(0.021f * (float)n) * 3000.0f + 11.0f * (float)(n % 7);
}

/*
 * The property the seam fix has to have. After the switch-on the grid's next
 * read must return the filtered signal at the position it was about to read,
 * computed from a full window of real samples.
 */
static void
test_prime_and_catch_up_aligns_content(void) {
    static float taps[MAX_TAPS];
    static float ring[MAX_TAPS];

    for (unsigned k = 0; k < sizeof(kKinds) / sizeof(kKinds[0]); k++) {
        for (unsigned s = 0; s < sizeof(kSps) / sizeof(kSps[0]); s++) {
            const dsd_sps_filter_kind kind = kKinds[k];
            const int sps = kSps[s];
            const int len = dsd_sps_filter_copy_taps(kind, sps, taps, MAX_TAPS);
            assert(len > 0);
            const int delay = (len - 1) / 2;
            const int n0 = MAX_TAPS + 64; /* the first position read through the filter */

            /* Whatever the previous transmission left in the history must not
               reach the output; start from something that is not the signal. */
            init_rrc_filter_memory();
            for (int n = 0; n < 96; n++) {
                (void)dsd_sps_filter_apply(kind, -20000.0f, sps);
            }

            /* The grid consumed n0 raw samples; the symbolizer keeps the tail. */
            for (int i = 0; i < len; i++) {
                ring[i] = stimulus(n0 - len + i);
            }
            dsd_sps_filter_prime(kind, ring, len, sps);

            /* Pay off the delay once: these outputs describe samples the grid
               has already consumed, so they are discarded. */
            for (int j = 0; j < delay; j++) {
                (void)dsd_sps_filter_apply(kind, stimulus(n0 + j), sps);
            }

            /* From here on, read k returns the filtered signal at n0 + k. */
            for (int p = 0; p < 40; p++) {
                const float got = dsd_sps_filter_apply(kind, stimulus(n0 + delay + p), sps);
                double want = 0.0;
                for (int i = 0; i < len; i++) {
                    want += (double)taps[i] * (double)stimulus(n0 + delay + p - (len - 1) + i);
                }
                const double tol = 1e-3 * (fabs(want) + 1.0);
                if (fabs((double)got - want) > tol) {
                    DSD_FPRINTF(stderr, "%s sps=%d p=%d: got %.6f want %.6f\n", kind_name(kind), sps, p, (double)got,
                                want);
                }
                assert(fabs((double)got - want) <= tol);
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
    static float taps[MAX_TAPS];
    const dsd_sps_filter_kind kind = DSD_SPS_FILTER_NXDN;
    const int sps = 20;
    const int len = dsd_sps_filter_copy_taps(kind, sps, taps, MAX_TAPS);
    assert(len > 0);
    const int delay = (len - 1) / 2;
    const int n0 = MAX_TAPS + 64;

    init_rrc_filter_memory();
    for (int n = 0; n < 96; n++) {
        (void)dsd_sps_filter_apply(kind, -20000.0f, sps);
    }
    for (int j = 0; j < delay; j++) {
        (void)dsd_sps_filter_apply(kind, stimulus(n0 + j), sps);
    }
    const float got = dsd_sps_filter_apply(kind, stimulus(n0 + delay), sps);
    double want = 0.0;
    for (int i = 0; i < len; i++) {
        want += (double)taps[i] * (double)stimulus(n0 + delay - (len - 1) + i);
    }
    DSD_FPRINTF(stderr, "unprimed first output: got %.1f want %.1f\n", (double)got, want);
    assert(fabs((double)got - want) > 1.0);
}

int
main(void) {
    test_taps_are_symmetric_and_odd();
    test_none_and_degenerate_rates_have_no_delay();
    test_prime_and_catch_up_aligns_content();
    test_unprimed_switch_on_is_measurably_wrong();
    DSD_FPRINTF(stderr, "matched filter seam: OK\n");
    return 0;
}
