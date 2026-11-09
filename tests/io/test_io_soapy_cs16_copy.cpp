// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The SoapySDR reader's CS16 conversion (soapy_copy_cs16_samples() in rtl_device.cpp) rotates sample i at fs/4 phase
 * *phase + i in a lead-in up to the next phase-0 sample, a body of fixed-phase groups of four and a tail. Its output
 * must be the per-sample reference's bit for bit (the conversion it replaced, kept below): every start phase (0-3, and
 * 5, whose low bits are 1), every length from 0 to 9 and a long one, with and without the rotation, and the phase it
 * hands back. Nothing past the last pair is written.
 *
 * A read through the reader's submit path (soapy_submit_samples(): the input-level meter, then the ring) is scaled by
 * the driver's own full scale when CS16 is its native format (Pluto and bladeRF: 2048, so their 12-bit rails reach the
 * ring as +-1 and the meter as 0 dBFS and clipping), and by 1/32768 otherwise.
 */

#include <dsd-neo/core/input_level.h>
#include <math.h>
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
reference_copy(float* dst, const int16_t* src_iq, size_t elem_count, int apply_rot, int* phase, float scale = kScale) {
    for (size_t i = 0U; i < elem_count; i++) {
        const float in_i = (float)src_iq[2U * i] * scale;
        const float in_q = (float)src_iq[(2U * i) + 1U] * scale;
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

/* One read of @p pairs from @p src through the submit path, for a driver reporting @p native_format at
 * @p native_full_scale: the ring must hold the reference at @p want_scale, bit for bit, and the phase move on. */
int
check_ingest(const char* label, const std::vector<int16_t>& src, size_t pairs, const char* native_format,
             double native_full_scale, float want_scale, int apply_rot, int start_phase) {
    std::vector<float> want(pairs * 2U, kCanary);
    int want_phase = start_phase;
    reference_copy(want.data(), src.data(), pairs, apply_rot, &want_phase, want_scale);
    rtl_device_test_soapy_cs16_request request{};
    request.samples = src.data();
    request.pairs = pairs;
    request.capacity = (pairs * 2U) + 64U;
    request.start = 17U;
    request.fs4_shift = apply_rot;
    request.start_phase = start_phase;
    request.native_format = native_format;
    request.native_full_scale = native_full_scale;
    std::vector<float> got(pairs * 2U, kCanary);
    size_t got_count = got.size();
    int got_phase = -1;
    if (rtl_device_test_soapy_cs16_ingest(&request, got.data(), &got_count, &got_phase) != 0
        || got_count != want.size()) {
        DSD_FPRINTF(stderr, "FAIL: %s: ingest refused or short (%zu of %zu)\n", label, got_count, want.size());
        return 1;
    }
    if (memcmp(want.data(), got.data(), want.size() * sizeof(float)) != 0) {
        DSD_FPRINTF(stderr, "FAIL: %s: ring rot=%d phase=%d differs from the reference at scale %.9g\n", label,
                    apply_rot, start_phase, (double)want_scale);
        return 1;
    }
    if (apply_rot && got_phase != want_phase) {
        DSD_FPRINTF(stderr, "FAIL: %s: phase got %d want %d\n", label, got_phase, want_phase);
        return 1;
    }
    return 0;
}

/* 12-bit samples at full scale 2048: the rails reach the ring as -1 and 2047/2048, and the meter reads them as 0 dBFS,
 * clipping; a +-64 square reads -30.1 dBFS, not 24 dB lower. */
int
check_twelve_bit_ingest(void) {
    std::vector<int16_t> rails = {-2048, 2047, 2047, -2048, 1024, -1024, 0, 2028};
    rtl_device_test_soapy_cs16_request request{};
    request.samples = rails.data();
    request.pairs = rails.size() / 2U;
    request.capacity = 64U;
    request.native_format = "CS16";
    request.native_full_scale = 2048.0;
    float got[8];
    size_t got_count = 8U;
    int phase = 0;
    if (rtl_device_test_soapy_cs16_ingest(&request, got, &got_count, &phase) != 0 || got_count != 8U) {
        DSD_FPRINTF(stderr, "FAIL: 12-bit ingest refused or short\n");
        return 1;
    }
    const float want[8] = {-1.0f, 0.99951171875f, 0.99951171875f, -1.0f, 0.5f, -0.5f, 0.0f, 0.990234375f};
    for (size_t i = 0U; i < 8U; i++) {
        if (fabsf(got[i] - want[i]) > 1e-9f) {
            DSD_FPRINTF(stderr, "FAIL: 12-bit ingest [%zu] got %.9g want %.9g\n", i, (double)got[i], (double)want[i]);
            return 1;
        }
    }
    dsd_input_level_snapshot level{};
    if (rtl_stream_test_input_level_read(&level) != 1 || level.source != DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16
        || level.sample_count != 8U) {
        DSD_FPRINTF(stderr, "FAIL: 12-bit ingest published no CS16 level\n");
        return 1;
    }
    dsd_input_level_classify(&level, -40.0);
    /* Five of the eight samples are at or past 0.99 of full scale (2028). */
    if (level.peak_dbfs < -0.01 || fabs(level.clip_pct - 62.5) > 1e-9 || level.status != DSD_INPUT_LEVEL_CLIPPING) {
        DSD_FPRINTF(stderr, "FAIL: 12-bit rails read peak %.3f dBFS, clip %.3f%%, status %d\n", level.peak_dbfs,
                    level.clip_pct, (int)level.status);
        return 1;
    }

    std::vector<int16_t> square(512U);
    for (size_t i = 0U; i < square.size(); i++) {
        square[i] = (int16_t)(((i / 2U) & 1U) ? -64 : 64);
    }
    std::vector<float> ring(square.size());
    request.samples = square.data();
    request.pairs = square.size() / 2U;
    request.capacity = square.size() + 64U;
    got_count = ring.size();
    if (rtl_device_test_soapy_cs16_ingest(&request, ring.data(), &got_count, &phase) != 0
        || rtl_stream_test_input_level_read(&level) != 1) {
        DSD_FPRINTF(stderr, "FAIL: 12-bit square ingest\n");
        return 1;
    }
    const double want_rms = 20.0 * log10(64.0 / 2048.0);
    if (fabs(level.rms_dbfs - want_rms) > 1e-6 || level.clip_pct > 0.0) {
        DSD_FPRINTF(stderr, "FAIL: 12-bit square reads %.3f dBFS (want %.3f), clip %.3f%%\n", level.rms_dbfs, want_rms,
                    level.clip_pct);
        return 1;
    }
    return 0;
}

/* The scale the submit path applies: the driver's when CS16 is its native format with a full scale in [1, 32768],
 * else 1/32768, with and without the rotation, from every start phase. */
int
check_ingest_scales(const std::vector<int16_t>& src) {
    struct ingest_case {
        const char* label;
        const char* native_format;
        double native_full_scale;
        float want_scale;
    };

    const ingest_case cases[] = {
        {"native CS16 2048 (Pluto, bladeRF)", "CS16", 2048.0, 1.0f / 2048.0f},
        {"native CS16 32767 (Airspy, LMS7)", "CS16", 32767.0, (float)(1.0 / 32767.0)},
        {"native CS16 32768", "CS16", 32768.0, kScale},
        {"CS16 not native (RTL-SDR CS8)", "CS8", 128.0, kScale},
        {"native CS16, bad full scale", "CS16", 0.0, kScale},
    };
    int failures = 0;
    for (const ingest_case& c : cases) {
        for (int apply_rot = 0; apply_rot <= 1; apply_rot++) {
            for (int phase = 0; phase < 4; phase++) {
                failures += check_ingest(c.label, src, 1027U, c.native_format, c.native_full_scale, c.want_scale,
                                         apply_rot, phase);
            }
        }
    }
    return failures;
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
    failures += check_ingest_scales(src);
    failures += check_twelve_bit_ingest();
    if (failures != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "PASS: SoapySDR CS16 copy matches the per-sample rotation, at the driver's full scale\n");
    return 0;
}
