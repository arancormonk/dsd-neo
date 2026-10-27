// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The SoapySDR reader's CS16 conversion (soapy_copy_cs16_samples() in rtl_device.cpp) rotates sample i at fs/4 phase
 * *phase + i in a lead-in up to the next phase-0 sample, a body of fixed-phase groups of four and a tail. Its output
 * must be the per-sample reference's bit for bit (the conversion it replaced, kept below): every start phase (0-3, and
 * 5, whose low bits are 1), every length from 0 to 9 and a long one, with and without the rotation, and the phase it
 * hands back. Nothing past the last pair is written.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

namespace {

constexpr float kScale = 1.0f / 32768.0f;
constexpr float kCanary = 12345.0f;

/* The conversion as it was before the grouped rotation: one switch per sample (rtl_apply_j4_rotation()). */
void
reference_copy(float* dst, const int16_t* src_iq, size_t elem_count, int apply_rot, int* phase) {
    for (size_t i = 0U; i < elem_count; i++) {
        const float in_i = (float)src_iq[2U * i] * kScale;
        const float in_q = (float)src_iq[(2U * i) + 1U] * kScale;
        float* out_i = &dst[2U * i];
        float* out_q = &dst[(2U * i) + 1U];
        if (!apply_rot) {
            *out_i = in_i;
            *out_q = in_q;
            continue;
        }
        switch (*phase & 3) {
            case 0:
                *out_i = in_i;
                *out_q = in_q;
                break;
            case 1:
                *out_i = -in_q;
                *out_q = in_i;
                break;
            case 2:
                *out_i = -in_i;
                *out_q = -in_q;
                break;
            default:
                *out_i = in_q;
                *out_q = -in_i;
                break;
        }
        *phase = (*phase + 1) & 3;
    }
}

/* Deterministic samples, the rails included. */
std::vector<int16_t>
make_samples(size_t pairs) {
    std::vector<int16_t> s(pairs * 2U);
    uint32_t lcg = 0x2545F491U;
    for (size_t k = 0; k < s.size(); k++) {
        lcg = (lcg * 1664525U) + 1013904223U;
        s[k] = (int16_t)(uint16_t)(lcg >> 16);
    }
    if (s.size() >= 4U) {
        s[0] = INT16_MIN;
        s[1] = INT16_MAX;
        s[2] = -1;
        s[3] = 0;
    }
    return s;
}

int
check_case(const std::vector<int16_t>& src, size_t pairs, int apply_rot, int start_phase) {
    std::vector<float> want(pairs * 2U + 2U, kCanary);
    std::vector<float> got(pairs * 2U + 2U, kCanary);
    int want_phase = start_phase;
    int got_phase = start_phase;
    reference_copy(want.data(), src.data(), pairs, apply_rot, &want_phase);
    if (rtl_device_test_soapy_cs16_copy(got.data(), src.data(), pairs, apply_rot, &got_phase) != 0) {
        DSD_FPRINTF(stderr, "FAIL: CS16 copy hook refused pairs=%zu\n", pairs);
        return 1;
    }
    /* Bit for bit, the two canaries past the last pair included. */
    if (memcmp(want.data(), got.data(), want.size() * sizeof(float)) != 0) {
        DSD_FPRINTF(stderr, "FAIL: CS16 copy output pairs=%zu rot=%d phase=%d\n", pairs, apply_rot, start_phase);
        return 1;
    }
    if (got_phase != want_phase) {
        DSD_FPRINTF(stderr, "FAIL: CS16 copy phase pairs=%zu rot=%d phase=%d: got %d want %d\n", pairs, apply_rot,
                    start_phase, got_phase, want_phase);
        return 1;
    }
    return 0;
}

} // namespace

int
main(void) {
    const std::vector<int16_t> src = make_samples(1027U);
    int probe_phase = 0;
    float probe[2];
    if (rtl_device_test_soapy_cs16_copy(probe, src.data(), 1U, 0, &probe_phase) == 1) {
        DSD_FPRINTF(stderr, "SKIP: SoapySDR CS16 copy: no SoapySDR in this build\n");
        return 0;
    }
    const int phases[] = {0, 1, 2, 3, 5};
    const size_t lengths[] = {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U, 1027U};
    int failures = 0;
    for (int apply_rot = 0; apply_rot <= 1; apply_rot++) {
        for (int phase : phases) {
            for (size_t pairs : lengths) {
                failures += check_case(src, pairs, apply_rot, phase);
            }
        }
    }
    if (failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: SoapySDR CS16 copy matches the per-sample rotation\n");
    return 0;
}
