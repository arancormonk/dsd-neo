// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Opt-in USB ingest benchmark: a paced synthetic producer drives the production receive path of an RTL-SDR
 * (librtlsdr's callback, rtlsdr_callback()) or an Airspy (libairspy's receive callback, through the in-process SDK
 * double) at a fixed sample rate, while a consumer drains the input ring as the demod does. The device runs as in a
 * session, its USB sample-progress monitor included. Measure the process under perf stat:
 *
 *   cmake --build --preset dev-release --target dsd-neo_bench_usb_ingest -j
 *   perf stat -e task-clock build/dev-release/tests/dsd-neo_bench_usb_ingest --backend rtl --rate 2400000 \
 *       --buf 16384 --seconds 60
 *
 * --buf is the bytes of one callback: cu8 I/Q for the RTL-SDR, CF32 I/Q for the Airspy.
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

#ifdef DSD_BENCH_USB_INGEST_AIRSPY
#include <dsd-neo/core/airspy_config.h>
#include "airspy_sdk_double.h"
#endif

namespace {

struct BenchOptions {
    const char* backend = "rtl";
    uint32_t rate = 2400000U;
    uint32_t buf = 16384U;
    uint32_t seconds = 10U;
};

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
    std::printf("Usage: %s [--backend rtl|airspy] [--rate SAMPLES_PER_S] [--buf BYTES] [--seconds N]\n", argv0);
}

int
parse_options(int argc, char** argv, BenchOptions* options) {
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const char* value = i + 1 < argc ? argv[i + 1] : nullptr;
        if (std::strcmp(arg, "--backend") == 0 && value) {
            options->backend = value;
        } else if (std::strcmp(arg, "--rate") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 100000000U, &options->rate) != 0 || options->rate == 0U) {
                return -1;
            }
        } else if (std::strcmp(arg, "--buf") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 1U << 20, &options->buf) != 0 || options->buf < 16U) {
                return -1;
            }
        } else if (std::strcmp(arg, "--seconds") == 0 && value) {
            if (dsd_parse_uint32_strict(value, 10, 3600U, &options->seconds) != 0 || options->seconds == 0U) {
                return -1;
            }
        } else {
            return -1;
        }
        ++i;
    }
    return 0;
}

/* Call @p deliver once per callback period for @p seconds, catching up after a late wake as a USB backlog would.
 * Returns the callbacks delivered. */
template <typename Deliver>
uint64_t
run_paced(uint64_t callback_ns, uint32_t seconds, Deliver deliver) {
    const uint64_t started_ns = dsd_realtime_mono_ns();
    const uint64_t end_ns = started_ns + (uint64_t)seconds * 1000000000ULL;
    uint64_t delivered = 0U;
    for (;;) {
        const uint64_t now_ns = dsd_realtime_mono_ns();
        if (now_ns >= end_ns) {
            break;
        }
        const uint64_t due = (now_ns - started_ns) / callback_ns + 1U;
        while (delivered < due) {
            deliver();
            ++delivered;
        }
        const uint64_t next_ns = started_ns + delivered * callback_ns;
        if (next_ns > now_ns) {
            dsd_sleep_ns(next_ns - now_ns);
        }
    }
    return delivered;
}

int
bench_rtl(const BenchOptions& options, input_ring_state* ring, uint64_t* delivered) {
    std::vector<unsigned char> buf(options.buf & ~1U);
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = (unsigned char)(127U + ((i * 37U) & 15U));
    }
    rtl_device* dev = rtl_device_test_usb_stream_start(ring);
    if (!dev) {
        return -1;
    }
    const uint64_t samples_per_callback = buf.size() / 2U;
    const uint64_t callback_ns = samples_per_callback * 1000000000ULL / options.rate;
    *delivered = run_paced(callback_ns, options.seconds,
                           [&]() { rtl_device_test_usb_stream_deliver(dev, buf.data(), (uint32_t)buf.size()); });
    *delivered *= samples_per_callback;
    dsd_exitflag_store(1);
    rtl_device_test_usb_stream_stop(dev);
    return 0;
}

#ifdef DSD_BENCH_USB_INGEST_AIRSPY
int
bench_airspy(const BenchOptions& options, input_ring_state* ring, uint64_t* delivered) {
    const int pairs = (int)(options.buf / (2U * sizeof(float)));
    std::vector<float> samples((size_t)pairs * 2U);
    for (size_t i = 0; i < samples.size(); ++i) {
        samples[i] = (float)((int)(i % 17U) - 8) / 16.0f;
    }
    dsd_airspy_config cfg;
    dsd_airspy_config_defaults(&cfg);
    rtl_device* dev = rtl_device_create_airspy(&cfg, ring);
    if (!dev) {
        return -1;
    }
    if (rtl_device_set_frequency(dev, 851000000U) != 0 || rtl_device_start_async(dev, options.buf) != 0) {
        rtl_device_destroy(dev);
        return -1;
    }
    const uint64_t callback_ns = (uint64_t)pairs * 1000000000ULL / options.rate;
    *delivered =
        run_paced(callback_ns, options.seconds, [&]() { (void)airspy_double::deliver(samples.data(), pairs); });
    *delivered *= (uint64_t)pairs;
    dsd_exitflag_store(1);
    (void)rtl_device_stop_async(dev);
    rtl_device_destroy(dev);
    return 0;
}
#endif

} // namespace

int
main(int argc, char** argv) {
    BenchOptions options;
    if (parse_options(argc, argv, &options) != 0) {
        print_usage(argv[0]);
        return 2;
    }
    /* The producer never falls silent; the limit only keeps a build without the progress count from tripping. */
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

    uint64_t delivered = 0U;
    int rc = -1;
    if (std::strcmp(options.backend, "rtl") == 0) {
        rc = bench_rtl(options, ring.get(), &delivered);
#ifdef DSD_BENCH_USB_INGEST_AIRSPY
    } else if (std::strcmp(options.backend, "airspy") == 0) {
        rc = bench_airspy(options, ring.get(), &delivered);
#endif
    } else {
        print_usage(argv[0]);
    }
    dsd_exitflag_store(1);
    dsd_thread_join(consumer.thread);
    dsd_exitflag_store(0);
    input_ring_destroy(ring.get());
    if (rc != 0) {
        DSD_FPRINTF(stderr, "bench_usb_ingest: the %s device did not start\n", options.backend);
        return 1;
    }
    uint64_t per_second = 0U;
    if (options.seconds > 0U) {
        per_second = delivered / options.seconds;
    }
    std::printf("backend=%s rate=%u buf=%u seconds=%u delivered=%llu (%llu samples/s) consumed=%llu floats\n",
                options.backend, options.rate, options.buf, options.seconds, (unsigned long long)delivered,
                (unsigned long long)per_second, (unsigned long long)consumer.consumed.load(std::memory_order_relaxed));
    return 0;
}
