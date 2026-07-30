// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Regression test for the retune mute window sizing.
 *
 * The controller arms the input mute several times per retune. The arms that
 * run after the hardware retune completes ("post"/"post-reset") used to
 * restart the full pre-retune window, discarding ~120 ms of on-frequency
 * signal after the tuner had already settled and clipping the start of short
 * transmissions. The post-retune (settle) window must stay much shorter than
 * the pre-retune window on local USB tuners, while buffered backends
 * (rtl_tcp, SoapySDR, replay) and explicit DSD_NEO_RETUNE_MUTE_MS overrides
 * keep the full window.
 */

#include <cstdint>
#include <cstdio>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

namespace {

constexpr uint32_t kSampleRateHz = 2400000U;
constexpr uint32_t kMinBytes = 16384U;

int
expect_int_eq(const char* label, int got, int want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "FAIL: %s got=%d want=%d\n", label, got, want);
        return 1;
    }
    return 0;
}

int
bytes_for_ms(uint32_t sample_rate_hz, int ms) {
    return (int)(((uint64_t)sample_rate_hz * 2ULL * (uint64_t)ms) / 1000ULL);
}

} // namespace

int
main(void) {
    int failed = 0;

    /* Default pre-retune window covers in-flight old-frequency samples. */
    const int pre_default = rtl_stream_test_retune_mute_plan(kSampleRateHz, 0, 0, /*post_retune=*/0,
                                                             /*buffered_backend=*/0, kMinBytes);
    failed |= expect_int_eq("default pre-retune window is 120ms", pre_default, bytes_for_ms(kSampleRateHz, 120));

    /* Regression: the post-retune settle window on a local USB tuner must not
     * restart the full pre-retune window. */
    const int post_local = rtl_stream_test_retune_mute_plan(kSampleRateHz, 0, 0, /*post_retune=*/1,
                                                            /*buffered_backend=*/0, kMinBytes);
    failed |= expect_int_eq("post-retune settle window is 25ms", post_local, bytes_for_ms(kSampleRateHz, 25));
    failed |= expect_int_eq("settle window is shorter than pre-retune window", post_local < pre_default, 1);

    /* Buffered backends can deliver stale pre-retune samples long after the
     * reconfigure finishes, so they keep the full window post-retune. */
    const int post_buffered = rtl_stream_test_retune_mute_plan(kSampleRateHz, 0, 0, /*post_retune=*/1,
                                                               /*buffered_backend=*/1, kMinBytes);
    failed |= expect_int_eq("buffered backend keeps full post window", post_buffered, bytes_for_ms(kSampleRateHz, 120));

    /* An explicit DSD_NEO_RETUNE_MUTE_MS override applies to both windows. */
    const int pre_override = rtl_stream_test_retune_mute_plan(kSampleRateHz, 40, 1, /*post_retune=*/0,
                                                              /*buffered_backend=*/0, kMinBytes);
    const int post_override = rtl_stream_test_retune_mute_plan(kSampleRateHz, 40, 1, /*post_retune=*/1,
                                                               /*buffered_backend=*/0, kMinBytes);
    failed |= expect_int_eq("override sizes pre window", pre_override, bytes_for_ms(kSampleRateHz, 40));
    failed |= expect_int_eq("override sizes post window", post_override, bytes_for_ms(kSampleRateHz, 40));

    /* A configured-but-unset (default-populated) value must not defeat the
     * short settle window. */
    const int post_unset_default = rtl_stream_test_retune_mute_plan(kSampleRateHz, 120, 0, /*post_retune=*/1,
                                                                    /*buffered_backend=*/0, kMinBytes);
    failed |=
        expect_int_eq("unset config default keeps settle window", post_unset_default, bytes_for_ms(kSampleRateHz, 25));

    /* Every window keeps the one-USB-buffer floor so at least one callback of
     * garbage is always discarded. */
    const int floor_bytes = rtl_stream_test_retune_mute_plan(48000U, 0, 0, /*post_retune=*/1,
                                                             /*buffered_backend=*/0, kMinBytes);
    failed |= expect_int_eq("settle window keeps buffer floor", floor_bytes, (int)kMinBytes);

    if (failed) {
        return 1;
    }
    DSD_FPRINTF(stderr, "RTL retune mute window tests passed\n");
    return 0;
}
