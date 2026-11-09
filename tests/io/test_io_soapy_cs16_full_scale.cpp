// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * A driver whose native CS16 is 12-bit with full scale 2048 (Pluto, bladeRF) hands the demod the same samples whether
 * the stream is that CS16 or the CF32 the driver converts by dividing by 2048. The device's own reader runs against a
 * stand-in driver (the stream seam): its setup, locked read loop, input-level meter and ring writer, the fs/4 rotation
 * included. The samples are a real off-air capture (the P25 control channel fixture p25p1_c4fm_cc.iq) as 12-bit codes, as
 * recorded and overdriven 6 dB into the rails, delivered once as CS16 and once as CF32: the input ring must hold the
 * same floats bit for bit, and the input-level meter must read the same level. Everything the decoder does starts
 * from that ring, so the decode is the same too.
 */

#include <algorithm>
#include <assert.h>
#include <atomic>
#include <dsd-neo/core/input_level.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_failure.h>
#include <dsd-neo/runtime/input_ring.h>
#include <math.h>
#include <memory>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

#ifndef DSD_NEO_TEST_IQ_FIXTURE_DIR
#error "DSD_NEO_TEST_IQ_FIXTURE_DIR must name tests/fixtures/iq"
#endif

struct rtl_device;

// LLVM 22+ misclassifies these runtime test oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {

constexpr size_t kReadPairs = 4096U;
constexpr uint32_t kBufLen = 16384U;
constexpr double kTwelveBitFullScale = 2048.0;

/* The stand-in driver: the capture's codes, read by read, as CS16 or as the driver's CF32 (code / 2048, as
 * SoapyPlutoSDR and SoapyBladeRF convert them). Only the reader calls read(). */
struct capture_driver {
    const std::vector<int16_t>* codes = nullptr;
    size_t next = 0U; /* the next code to deliver */
    std::atomic<int> drained{0};
};

int
capture_setup(void* ctx, size_t* mtu_elems) {
    (void)ctx;
    *mtu_elems = kReadPairs;
    return 0;
}

int
capture_read(void* ctx, void* buff, size_t elems, long timeout_us, int cs16) {
    auto* driver = static_cast<capture_driver*>(ctx);
    (void)timeout_us;
    const std::vector<int16_t>& codes = *driver->codes;
    if (driver->next >= codes.size()) {
        driver->drained.store(1, std::memory_order_release);
        dsd_sleep_ms(1U);
        return RTL_DEVICE_TEST_SOAPY_TIMEOUT;
    }
    const size_t pairs = std::min(elems, (codes.size() - driver->next) / 2U);
    const int16_t* src = codes.data() + driver->next;
    if (cs16 != 0) {
        DSD_MEMCPY(buff, src, pairs * 2U * sizeof(int16_t));
    } else {
        auto* out = static_cast<float*>(buff);
        for (size_t k = 0U; k < pairs * 2U; k++) {
            out[k] = float(src[k]) / 2048.0f;
        }
    }
    driver->next += pairs * 2U;
    return (int)pairs;
}

int
capture_read_cs16(void* ctx, void* buff, size_t elems, long timeout_us) {
    return capture_read(ctx, buff, elems, timeout_us, 1);
}

int
capture_read_cf32(void* ctx, void* buff, size_t elems, long timeout_us) {
    return capture_read(ctx, buff, elems, timeout_us, 0);
}

/* The cu8 fixture as 12-bit codes: the production widening's (x - 127.5) / 127.5, at full scale 2048, the top code
 * clamped to 2047 as the ADC's is. */
std::vector<int16_t>
load_twelve_bit_codes(const char* path) {
    FILE* fp = dsd_fopen_existing_regular_file(path, "rb");
    assert(fp != nullptr);
    std::vector<int16_t> codes;
    unsigned char block[4096];
    size_t got = 0U;
    while ((got = fread(block, 1U, sizeof block, fp)) > 0U) {
        for (size_t i = 0U; i < got; i++) {
            const double x = ((double)block[i] - 127.5) / 127.5;
            long code = lround(x * kTwelveBitFullScale);
            code = code > 2047L ? 2047L : code;
            code = code < -2048L ? -2048L : code;
            codes.push_back((int16_t)code);
        }
    }
    (void)fclose(fp);
    assert(codes.size() >= 4U * kReadPairs);
    codes.resize(codes.size() & ~(size_t)1U);
    return codes;
}

struct ring_run {
    std::vector<float> ring;
    dsd_input_level_snapshot level{};
};

/* Run the reader over the whole capture as @p cs16 (full scale 2048) or CF32 and take what reached the ring. */
ring_run
run_capture(const std::vector<int16_t>& codes, int cs16) {
    auto ring = std::make_unique<input_ring_state>();
    const int initialized = input_ring_init(ring.get(), codes.size() + 1024U);
    assert(initialized == 0);
    capture_driver driver;
    driver.codes = &codes;
    const rtl_device_test_soapy_driver seam = {
        capture_setup,
        cs16 != 0 ? capture_read_cs16 : capture_read_cf32,
        &driver,
        cs16,
        cs16 != 0 ? kTwelveBitFullScale : 0.0,
    };
    dsd_input_failure_clear();
    dsd_exitflag_store(0);
    rtl_device* dev = rtl_device_test_soapy_device(ring.get(), &seam);
    assert(dev != nullptr);
    const int started = rtl_device_start_async(dev, kBufLen);
    assert(started == 0);
    const uint64_t deadline_ms = dsd_realtime_mono_ms() + 10000U;
    while ((driver.drained.load(std::memory_order_acquire) == 0 || input_ring_used(ring.get()) < codes.size())
           && dsd_realtime_mono_ms() < deadline_ms) {
        dsd_sleep_ms(1U);
    }
    /* The level of the last read, before the stop drops it. */
    ring_run run;
    const int have_level = rtl_stream_test_input_level_read(&run.level);
    (void)rtl_device_stop_async(dev);
    rtl_device_test_soapy_device_free(dev);
    assert(have_level == 1);
    assert(ring->producer_drops.load() == 0U);
    const size_t used = input_ring_used(ring.get());
    assert(used == codes.size());
    const size_t tail = ring->tail.load();
    run.ring.resize(used);
    for (size_t i = 0U; i < used; i++) {
        run.ring[i] = ring->buffer[(tail + i) % ring->capacity];
    }
    input_ring_destroy(ring.get());
    return run;
}

uint32_t
float_bits(float value) {
    uint32_t bits = 0U;
    DSD_MEMCPY(&bits, &value, sizeof bits);
    return bits;
}

/* The capture @p codes through the reader as CS16 at full scale 2048 and as CF32: 0 when the ring holds the same
 * floats bit for bit and the meter reads the same level. */
int
compare_runs(const char* label, const std::vector<int16_t>& codes) {
    size_t rails = 0U;
    for (const int16_t code : codes) {
        rails += (code == 2047 || code == -2048) ? 1U : 0U;
    }
    const ring_run cs16 = run_capture(codes, 1);
    const ring_run cf32 = run_capture(codes, 0);
    assert(cs16.ring.size() == cf32.ring.size());
    if (memcmp(cs16.ring.data(), cf32.ring.data(), cs16.ring.size() * sizeof(float)) != 0) {
        size_t first = 0U;
        while (first < cs16.ring.size() && float_bits(cs16.ring[first]) == float_bits(cf32.ring[first])) {
            first++;
        }
        DSD_FPRINTF(stderr, "FAIL: %s: CS16 at full scale 2048 differs from CF32 at float %zu: %.9g vs %.9g\n", label,
                    first, (double)cs16.ring[first], (double)cf32.ring[first]);
        return 1;
    }
    /* The meter reads the last read the same both ways (its clip rules differ by format: CS16 0.99, CF32 0.98). */
    assert(cs16.level.source == DSD_INPUT_LEVEL_SOURCE_SOAPY_CS16);
    assert(cf32.level.source == DSD_INPUT_LEVEL_SOURCE_SOAPY_CF32);
    assert(cs16.level.sample_count == cf32.level.sample_count);
    if (fabs(cs16.level.rms_dbfs - cf32.level.rms_dbfs) > 1e-9
        || fabs(cs16.level.peak_dbfs - cf32.level.peak_dbfs) > 1e-9) {
        DSD_FPRINTF(stderr, "FAIL: %s: CS16 level %.6f/%.6f dBFS, CF32 %.6f/%.6f dBFS (RMS/peak)\n", label,
                    cs16.level.rms_dbfs, cs16.level.peak_dbfs, cf32.level.rms_dbfs, cf32.level.peak_dbfs);
        return 1;
    }
    DSD_FPRINTF(stderr,
                "PASS: %s: %zu pairs (%zu rail codes) reach the ring the same as CS16 at 2048 and as CF32; last read "
                "%.2f dBFS RMS, %.2f dBFS peak, CS16 clip %.2f%%\n",
                label, codes.size() / 2U, rails, cs16.level.rms_dbfs, cs16.level.peak_dbfs, cs16.level.clip_pct);
    return 0;
}

} // namespace

int
main(void) {
    {
        auto probe_ring = std::make_unique<input_ring_state>();
        const int initialized = input_ring_init(probe_ring.get(), 1024U);
        assert(initialized == 0);
        capture_driver probe_driver;
        const rtl_device_test_soapy_driver probe_seam = {
            capture_setup, capture_read_cs16, &probe_driver, 1, kTwelveBitFullScale,
        };
        rtl_device* probe = rtl_device_test_soapy_device(probe_ring.get(), &probe_seam);
        if (probe == nullptr) {
            DSD_FPRINTF(stderr, "SKIP: SoapySDR CS16 full scale: no SoapySDR in this build\n");
            input_ring_destroy(probe_ring.get());
            return 0;
        }
        rtl_device_test_soapy_device_free(probe);
        input_ring_destroy(probe_ring.get());
    }

    const std::vector<int16_t> codes = load_twelve_bit_codes(DSD_NEO_TEST_IQ_FIXTURE_DIR "/p25p1_c4fm_cc.iq");
    /* The same capture overdriven by 6 dB into the ADC's rails, where the CS16 scale decides what clips. */
    std::vector<int16_t> overdriven(codes.size());
    for (size_t i = 0U; i < codes.size(); i++) {
        const int code = std::max(-2048, std::min(2047, 2 * (int)codes[i]));
        overdriven[i] = (int16_t)code;
    }
    if (compare_runs("off-air", codes) != 0 || compare_runs("overdriven", overdriven) != 0) {
        return 1;
    }
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
