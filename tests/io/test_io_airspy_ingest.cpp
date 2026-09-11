// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/dsp/demod_state.h>
#include <initializer_list>
#include <stdint.h>
#include <stdio.h>
#include "rtl_stream_test_support.h"

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%d: %s\n", __LINE__, #condition);                                                     \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

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
        CHECK(output[i] == samples[i]);
    }
    request.mute = 3; // Legacy mute units: round partial I/Q pairs up, not float bytes.
    count = 8;
    CHECK(rtl_device_test_airspy_ingest(&request, output, &count, &drops) == 0 && count == 4);
    for (size_t i = 0; i < count; ++i) {
        CHECK(output[i] == samples[i + 4]);
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
        CHECK(output[i] == samples[i]);
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
    return 0;
}
