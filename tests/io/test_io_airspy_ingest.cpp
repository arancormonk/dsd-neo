// SPDX-License-Identifier: GPL-3.0-or-later
#include <cmath>
#include <cstdlib>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_state.h>
#include <dsd-neo/io/rate_plan.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include "dsd-neo/core/opts_fwd.h"
#include "rtl_stream_test_support.h"

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%d: %s\n", __LINE__, #condition);                                                     \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

/* Issue #806: an Airspy's own rates on the live rate chain. The half-band output (78,125 Hz from 2.5 or 10 MS/s at
   48 kHz, 93,750 Hz from 3 or 6 MS/s) steps to exactly the DSP bandwidth, so FSK at 4800 sym/s is a whole 10 samples a
   symbol with no output resampler, CQPSK times a whole 10, and TETRA at 72 kHz a whole 4. */
namespace {
struct airspy_rate {
    uint32_t rate;
    int passes_48k;
    int l_48k;
    int m_48k;
    int l_72k;
    int m_72k;
};

void
expect_live_plans() {
    static const airspy_rate kRates[] = {{2500000U, 5, 384, 625, 576, 625},
                                         {3000000U, 5, 64, 125, 96, 125},
                                         {6000000U, 6, 64, 125, 96, 125},
                                         {10000000U, 7, 384, 625, 576, 625}};
    static dsd_opts fsk;
    static dsd_opts cqpsk;
    static dsd_opts tetra;
    DSD_MEMSET(&fsk, 0, sizeof fsk);
    fsk.frame_p25p1 = 1;
    fsk.mod_c4fm = 1;
    DSD_MEMSET(&cqpsk, 0, sizeof cqpsk);
    cqpsk.frame_p25p1 = 1;
    cqpsk.mod_qpsk = 1;
    DSD_MEMSET(&tetra, 0, sizeof tetra);
    tetra.frame_tetra = 1;
    tetra.mod_qpsk = 1;
    for (const airspy_rate& a : kRates) {
        rtl_stream_test_live_chain_result r{};
        CHECK(rtl_stream_test_live_rate_chain(&fsk, DSD_RATE_PLAN_BACKEND_AIRSPY, 48000, a.rate, 0U, &r) == 0);
        CHECK(r.rc == 0 && r.passes == a.passes_48k && r.forced == 1);
        CHECK(r.step_enabled == 1 && r.step_l == a.l_48k && r.step_m == a.m_48k);
        CHECK(r.rate_out_hz == 48000 && r.ted_sps == 10);
        CHECK(r.resamp_enabled == 0 && r.output_rate_hz == 48000U);

        CHECK(rtl_stream_test_live_rate_chain(&cqpsk, DSD_RATE_PLAN_BACKEND_AIRSPY, 48000, a.rate, 0U, &r) == 0);
        CHECK(r.rc == 0 && r.passes == a.passes_48k && r.step_enabled == 1);
        CHECK(r.step_l == a.l_48k && r.step_m == a.m_48k);
        CHECK(r.rate_out_hz == 48000 && r.ted_sps == 10 && r.resamp_enabled == 0 && r.output_rate_hz == 48000U);
        CHECK(r.published_rate_out_hz == 48000 && r.published_forced == 1);

        CHECK(rtl_stream_test_live_rate_chain(&tetra, DSD_RATE_PLAN_BACKEND_AIRSPY, 72000, a.rate, 0U, &r) == 0);
        CHECK(r.rc == 0 && r.passes == a.passes_48k && r.step_enabled == 1);
        CHECK(r.step_l == a.l_72k && r.step_m == a.m_72k);
        CHECK(r.rate_out_hz == 72000 && r.ted_sps == 4 && r.resamp_enabled == 0 && r.output_rate_hz == 72000U);
    }
}
} // namespace

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
    expect_live_plans();
    return 0;
}
