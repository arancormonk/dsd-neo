// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * A committed 48 or 72 kHz cu8 fixture as a device that forces its own rate (an Airspy's) would deliver it (issue
 * #806): an excerpt of the fixture, upsampled to the device rate with dsd_iq_resampler (a ratio no live step uses: the
 * step only decimates), with receiver noise before and after it, then quantized to cu8 as a capture would hold it.
 * The live decode tests (ENGINE_AIRSPY_FORCED_RATE_*) feed these samples through the Airspy SDK double, and the fixture
 * tool (dsd-neo_test_iq_forced_rate_fixture_tool) writes them as kept-chain I/Q sidecars for the DECODE_IQ_*_AIRSPY_*
 * replays, so both see the same samples.
 *
 * The noise is complex Gaussian receiver noise at noise_floor's level (16 LSB a component), seeded, never silence: no
 * receiver delivers exact zeros, and an exact-zero lead-in made the DMR decoder's audio error count swing with the
 * rate. It is at least as long as the chain's tail bound (the upsampler's, the half-band cascade's and the rate
 * step's), so the excerpt's last symbols reach the decoder.
 */

#ifndef DSD_NEO_TESTS_IO_FORCED_RATE_INPUT_H_
#define DSD_NEO_TESTS_IO_FORCED_RATE_INPUT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* fixture; /* a committed fixture, by its name without ".iq.json" */
    double start_s;      /* the excerpt, in the fixture's own time */
    double length_s;     /* 0: to the fixture's end */
    uint32_t device_rate_hz;
    uint32_t dsp_rate_hz; /* the DSP bandwidth rate the live chain steps to */
    double pad_s;         /* noise before and after the excerpt; raised to the tail bound when shorter */
} dsd_test_forced_rate_spec;

typedef struct {
    uint8_t* cu8; /* the samples as cu8, interleaved I/Q */
    float* iq;    /* the same samples as floats, (byte - 127.5) / 127.5 as a replay converts them */
    size_t pairs; /* complex samples in each */
    uint32_t device_rate_hz;
    uint32_t source_rate_hz;
    uint64_t center_frequency_hz;
    size_t pad_pairs;        /* noise before and after the excerpt, at the device rate */
    size_t tail_bound_pairs; /* the chain's tail bound the padding covers, at the device rate */
    size_t clipped;          /* components the cu8 quantization clipped */
    /* The chain the live stream plans for the device rate at dsp_rate_hz (an Airspy's, dsd_rate_plan_compute()). */
    int passes;
    int step_l;
    int step_m;
} dsd_test_forced_rate_input;

/* Make @p spec's samples from the fixture in @p fixture_dir into @p out. Returns 0, or -1 with the reason in @p err. */
int dsd_test_forced_rate_input_make(const char* fixture_dir, const dsd_test_forced_rate_spec* spec,
                                    dsd_test_forced_rate_input* out, char* err, size_t err_size);

void dsd_test_forced_rate_input_free(dsd_test_forced_rate_input* in);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_TESTS_IO_FORCED_RATE_INPUT_H_ */
