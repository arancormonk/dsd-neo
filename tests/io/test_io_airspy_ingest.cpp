// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/runtime/fp_mode.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include "rtl_stream_test_support.h"

#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
#include <xmmintrin.h>
#endif

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%d: %s\n", __LINE__, #condition);                                                     \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

/* The FTZ/DAZ bits in force inside the callback's own work, or ~0U before it ran. */
static unsigned int g_work_ftz_daz = ~0U;

static void
note_work_mode(void) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    g_work_ftz_daz = _mm_getcsr() & DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS;
#endif
}

/* libairspy calls back on its own thread, in its own mode (issue #672 B12): the callback runs dsd-neo's work in the
   run's mode and hands the library its MXCSR back exactly. Here the "library thread" holds FTZ/DAZ off (or on), the
   run's mode is the other one. */
static void
check_fp_mode_scope(const float* samples, dsd_fp_mode mode, unsigned int library_bits, unsigned int want_inside) {
#if DSD_RUNTIME_FP_MODE_HAVE_MXCSR
    const unsigned int original = _mm_getcsr();
    _mm_setcsr((original & ~DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS) | library_bits);
    const unsigned int library_csr = _mm_getcsr();
    dsd_runtime_fp_mode_publish(mode);
    rtl_device_test_set_airspy_work_probe(note_work_mode);
    g_work_ftz_daz = ~0U;
    float output[8] = {};
    size_t count = 8;
    uint64_t drops = 0;
    rtl_device_test_airspy_request request{samples, 4, 16, 0, 0, 0, 0};
    const int rc = rtl_device_test_airspy_ingest(&request, output, &count, &drops);
    const unsigned int after = _mm_getcsr();
    rtl_device_test_set_airspy_work_probe(nullptr);
    dsd_runtime_fp_mode_publish(DSD_FP_MODE_INHERIT);
    _mm_setcsr(original);
    CHECK(rc == 0 && count == 8);
    CHECK(g_work_ftz_daz == want_inside);
    CHECK(after == library_csr);
#else
    (void)samples;
    (void)mode;
    (void)library_bits;
    (void)want_inside;
#endif
}

int
main() {
    const float samples[] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    float output[8] = {};
    size_t count = 8;
    uint64_t drops = 0;
    rtl_device_test_airspy_request request{samples, 4, 16, 12, 0, 0, 0};
    CHECK(rtl_device_test_airspy_ingest(&request, output, &count, &drops) == 0);
    CHECK(count == 8 && drops == 0);
    for (size_t i = 0; i < count; ++i) {
        CHECK(std::fabs(output[i] - samples[i]) < 1e-6f);
    }
    request.mute = 3; // Legacy mute units: round partial I/Q pairs up, not float bytes.
    count = 8;
    CHECK(rtl_device_test_airspy_ingest(&request, output, &count, &drops) == 0 && count == 4);
    for (size_t i = 0; i < count; ++i) {
        CHECK(std::fabs(output[i] - samples[i + 4]) < 1e-6f);
    }
    request.hold = 1;
    count = 8;
    CHECK(rtl_device_test_airspy_ingest(&request, output, &count, &drops) == 0 && count == 0);
    request.hold = request.mute = 0;
    request.capacity = 8;
    request.start = 4;
    request.hardware_drops = 2;
    count = 8;
    CHECK(rtl_device_test_airspy_ingest(&request, output, &count, &drops) == 0);
    CHECK(count == 6 && drops == 6); // Two hardware pairs plus one full-ring pair.
    for (size_t i = 0; i < count; ++i) {
        CHECK(std::fabs(output[i] - samples[i]) < 1e-6f);
    }
    for (uint32_t rate : {2500000U, 3000000U, 6000000U, 10000000U}) {
        int passes = rtl_stream_test_passes_for_actual_rate(rate, 48000);
        CHECK(passes > 0 && passes <= 10);
        unsigned int actual = 0;
        int enabled = 0;
        CHECK(rtl_stream_test_digital_resample_chain(DSD_DEMOD_OUTPUT_FSK_DISCRIMINATOR, (int)(rate >> passes), 48000,
                                                     4800, 0, 1, &actual, &enabled)
              == 0);
        CHECK(actual == 48000 && enabled == 1);
    }
    check_fp_mode_scope(samples, DSD_FP_MODE_ON, 0U, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    check_fp_mode_scope(samples, DSD_FP_MODE_OFF, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS, 0U);
    check_fp_mode_scope(samples, DSD_FP_MODE_INHERIT, DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS,
                        DSD_RUNTIME_FP_MODE_FTZ_DAZ_BITS);
    return 0;
}
