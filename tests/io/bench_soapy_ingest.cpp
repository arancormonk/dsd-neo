// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in SoapySDR ingest benchmark: the device's own reader runs its locked read loop and read accounting
 * against a stand-in driver (the stream seam) whose readStream() paces itself to a fixed sample rate, as a driver
 * blocking for its next buffer does, while a consumer drains the input ring as the demod does. The device runs as in a
 * session, its sample-progress monitor included. Measure the process under perf stat:
 *
 *   cmake --preset dev-release -DBUILD_TESTING=ON
 *   cmake --build --preset dev-release --target dsd-neo_bench_soapy_ingest -j
 *   perf stat -e task-clock build/dev-release/tests/dsd-neo_bench_soapy_ingest --format cs16 --rate 2400000 \
 *       --elems 16384 --seconds 60
 *
 * --elems is the complex samples of one read (CS16 or CF32 I/Q). --unpaced N returns N reads back to back instead (no
 * pacing; --rate and --seconds are ignored) and reports the reader's time per read.
 */

#include <atomic>
#include <cstdio>
#include <cstring>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/exitflag.h>
#include <dsd-neo/runtime/input_ring.h>
#include <memory>
#include <stdint.h>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "rtl_stream_test_support.h"

struct rtl_device;

namespace {

struct BenchOptions {
    const char* format = "cs16";
    uint32_t rate = 2400000U;
    uint32_t elems = 16384U;
    uint32_t seconds = 10U;
    uint32_t unpaced = 0U; /* reads back to back; 0 paces to --rate for --seconds */
};

/* The stand-in driver: a prepared buffer copied into each read, paced or back to back. Only the reader calls it. */
struct PacedDriver {
    BenchOptions options;
    std::vector<int16_t> cs16;
    std::vector<float> cf32;
    uint64_t read_ns = 0U; /* one read's span of samples at --rate */
    uint64_t started_ns = 0U;
    uint64_t end_ns = 0U;
    uint64_t delivered = 0U;
    uint64_t last_ns = 0U;
    std::atomic<int> done{0};
};

int
driver_setup(void* ctx, size_t* mtu_elems) {
    auto* driver = static_cast<PacedDriver*>(ctx);
    *mtu_elems = driver->options.elems;
    return 0;
}

/* Deliver the next read once it is due, catching up after a late wake as a driver's backlog would; past the run, a
 * timeout every millisecond until the device stops. */
int
driver_read(void* ctx, void* buff, size_t elems, long timeout_us) {
    auto* driver = static_cast<PacedDriver*>(ctx);
    (void)timeout_us;
    if (driver->done.load(std::memory_order_relaxed)) {
        dsd_sleep_ms(1U);
        return RTL_DEVICE_TEST_SOAPY_TIMEOUT;
    }
    const uint64_t now_ns = dsd_realtime_mono_ns();
    if (driver->delivered == 0U) {
        driver->started_ns = now_ns;
        driver->end_ns = now_ns + (uint64_t)driver->options.seconds * 1000000000ULL;
    }
    if (driver->options.unpaced == 0U) {
        if (now_ns >= driver->end_ns) {
            driver->last_ns = now_ns;
            driver->done.store(1, std::memory_order_release);
            return RTL_DEVICE_TEST_SOAPY_TIMEOUT;
        }
        const uint64_t due_ns = driver->started_ns + driver->delivered * driver->read_ns;
        if (due_ns > now_ns) {
            dsd_sleep_ns(due_ns - now_ns);
        }
    }
    if (driver->cs16.empty()) {
        DSD_MEMCPY(buff, driver->cf32.data(), elems * 2U * sizeof(float));
    } else {
        DSD_MEMCPY(buff, driver->cs16.data(), elems * 2U * sizeof(int16_t));
    }
    driver->delivered++;
    if (driver->options.unpaced > 0U && driver->delivered >= driver->options.unpaced) {
        driver->last_ns = dsd_realtime_mono_ns();
        driver->done.store(1, std::memory_order_release);
    }
    return (int)elems;
}

struct Consumer {
    input_ring_state* ring = nullptr;
    std::atomic<uint64_t> consumed{0U};
    dsd_thread_t thread{};
};

/* Drain the ring as the demod does, until the exit flag ends the read. */
DSD_THREAD_RETURN_TYPE
#if DSD_PLATFORM_WIN_NATIVE
__stdcall
#endif
consume(void* arg) {
    auto* consumer = static_cast<Consumer*>(arg);
    std::vector<float> block(16384U);
    for (;;) {
        const int got = input_ring_read_block(consumer->ring, block.data(), block.size());
        if (got < 0) {
            break;
        }
        consumer->consumed.fetch_add((uint64_t)got, std::memory_order_relaxed);
    }
    DSD_THREAD_RETURN;
}

void
print_usage(const char* argv0) {
    std::printf("Usage: %s [--format cs16|cf32] [--rate SAMPLES_PER_S] [--elems N] [--seconds N] [--unpaced N]\n",
                argv0);
}

int
parse_options(int argc, char** argv, BenchOptions* options) {
    /* Every option takes a value: each pass reads a pair. */
    for (int i = 1; i < argc; i += 2) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (std::strcmp(arg, "--format") == 0 && value) {
            if (std::strcmp(value, "cs16") != 0 && std::strcmp(value, "cf32") != 0) {
                return -1;
            }
            options->format = value;
        } else if (std::strcmp(arg, "--rate") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 100000000U, &options->rate) != 0 || options->rate == 0U) {
                return -1;
            }
        } else if (std::strcmp(arg, "--elems") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 1U << 20, &options->elems) != 0 || options->elems < 16U) {
                return -1;
            }
        } else if (std::strcmp(arg, "--unpaced") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 100000000U, &options->unpaced) != 0 || options->unpaced == 0U) {
                return -1;
            }
        } else if (std::strcmp(arg, "--seconds") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 3600U, &options->seconds) != 0 || options->seconds == 0U) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return 0;
}

/* Run the device until the driver delivered its run, then stop it. 0, -1 when the device did not start, 1 when the
 * build has no SoapySDR. */
int
bench_soapy(PacedDriver* driver, input_ring_state* ring) {
    const int cs16 = std::strcmp(driver->options.format, "cs16") == 0;
    const size_t floats = (size_t)driver->options.elems * 2U;
    if (cs16) {
        driver->cs16.resize(floats);
        for (size_t i = 0; i < floats; ++i) {
            driver->cs16[i] = (int16_t)(((int)(i % 17U) - 8) * 1024);
        }
    } else {
        driver->cf32.resize(floats);
        for (size_t i = 0; i < floats; ++i) {
            driver->cf32[i] = (float)((int)(i % 17U) - 8) / 16.0f;
        }
    }
    driver->read_ns = (uint64_t)driver->options.elems * 1000000000ULL / driver->options.rate;
    const rtl_device_test_soapy_driver seam = {driver_setup, driver_read, driver, cs16};
    rtl_device* dev = rtl_device_test_soapy_device(ring, &seam);
    if (!dev) {
        return 1;
    }
    if (rtl_device_start_async(dev, driver->options.elems) != 0) {
        rtl_device_test_soapy_device_free(dev);
        return -1;
    }
    while (!driver->done.load(std::memory_order_acquire)) {
        dsd_sleep_ms(10U);
    }
    (void)rtl_device_stop_async(dev);
    rtl_device_test_soapy_device_free(dev);
    return 0;
}

} // namespace

int
main(int argc, char** argv) {
    auto driver = std::make_unique<PacedDriver>();
    if (parse_options(argc, argv, &driver->options) != 0) {
        print_usage(argv[0]);
        return 2;
    }
    /* The driver never falls silent; the limit only keeps a build without the progress count from tripping. */
    rtl_device_test_set_progress_limit_ms(UINT32_MAX);

    auto ring = std::make_unique<input_ring_state>();
    if (input_ring_init(ring.get(), 1U << 20) != 0) {
        return 1;
    }
    Consumer consumer;
    consumer.ring = ring.get();
    if (dsd_thread_create(&consumer.thread, consume, &consumer) != 0) {
        return 1;
    }

    const int rc = bench_soapy(driver.get(), ring.get());
    dsd_exitflag_store(1);
    dsd_thread_join(consumer.thread);
    dsd_exitflag_store(0);
    input_ring_destroy(ring.get());
    if (rc == 1) {
        DSD_FPRINTF(stderr, "bench_soapy_ingest: no SoapySDR in this build\n");
        return 1;
    }
    if (rc != 0) {
        DSD_FPRINTF(stderr, "bench_soapy_ingest: the SoapySDR device did not start\n");
        return 1;
    }
    /* Integer rates, so nothing divides by a value read from the command line unchecked. */
    const uint64_t reads = driver->delivered;
    const uint64_t samples = reads * driver->options.elems;
    const uint64_t elapsed_ns = driver->last_ns - driver->started_ns;
    uint64_t samples_per_second = 0U;
    uint64_t ns_per_read_x10 = 0U;
    if (elapsed_ns > 0U) {
        samples_per_second = samples * 1000000000ULL / elapsed_ns;
    }
    if (reads > 0U) {
        ns_per_read_x10 = elapsed_ns * 10U / reads;
    }
    std::printf("format=%s elems=%u mode=%s reads=%llu samples=%llu (%llu samples/s) ns_per_read=%llu.%llu "
                "consumed=%llu floats\n",
                driver->options.format, driver->options.elems, driver->options.unpaced > 0U ? "unpaced" : "paced",
                (unsigned long long)reads, (unsigned long long)samples, (unsigned long long)samples_per_second,
                (unsigned long long)(ns_per_read_x10 / 10U), (unsigned long long)(ns_per_read_x10 % 10U),
                (unsigned long long)consumer.consumed.load(std::memory_order_relaxed));
    return 0;
}
