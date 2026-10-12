// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <mutex>
#include <new>
#include <stddef.h>
#include <stdint.h>
#include "rtl_fft_cache.h"

struct dsd_signal_detector {
    dsd_io::FftSetupCache fft;
    dsd_io::HannWindowCache<DSD_SEARCH_MAX_FFT> hann;
    dsd_signal_measure_request request{};
    alignas(16) float samples[2 * DSD_SEARCH_MAX_FFT]{};
    double power[DSD_SEARCH_MAX_FFT]{};
    double energy = 0;
    size_t filled = 0, window_samples = 0, total_samples = 0, clipped = 0;
    unsigned frames = 0, windows = 0;
    int n = 0;
    bool armed = false;
    bool first[DSD_SEARCH_MAX_CANDIDATES]{};
};

extern "C" dsd_signal_detector*
dsd_signal_detector_create(void) {
    return new (std::nothrow) dsd_signal_detector;
}

extern "C" void
dsd_signal_detector_destroy(dsd_signal_detector* detector) {
    if (detector) {
        detector->fft.release();
        delete detector;
    }
}

extern "C" int
dsd_signal_detector_begin(dsd_signal_detector* detector, const dsd_signal_measure_request* request) {
    if (!detector) {
        return -1;
    }
    detector->armed = false;
    if (!request || !request->session_id || !request->request_id || !request->count
        || request->count > DSD_SEARCH_MAX_CANDIDATES) {
        return -1;
    }
    int n = dsd_signal_search_fft_size(request->capture.rate_hz, &request->range);
    if (!n) {
        return -1;
    }
    const float* window = detector->hann.get(n);
    if (!window) {
        return -1;
    }
    detector->n = n;
    detector->request = *request;
    detector->energy = 0;
    for (int i = 0; i < n; ++i) {
        detector->energy += (double)window[i] * window[i];
    }
    detector->filled = detector->window_samples = detector->total_samples = detector->clipped = 0;
    detector->frames = detector->windows = 0;
    DSD_MEMSET(detector->power, 0, sizeof(detector->power));
    DSD_MEMSET(detector->first, 0, sizeof(detector->first));
    detector->armed = true;
    return 0;
}

static bool
same_capture(const dsd_signal_capture& a, const dsd_signal_capture& b) {
    return a.rate_hz == b.rate_hz && a.capture_hz == b.capture_hz && a.generation == b.generation
           && a.rotation_hz == b.rotation_hz && a.drops == b.drops && a.gain == b.gain
           && a.capture_offset_hz == b.capture_offset_hz;
}

static void
measure_window(dsd_signal_detector* detector, dsd_signal_measure_report* report) {
    const auto& request = detector->request;
    const double bin_hz = (double)request.capture.rate_hz / detector->n;
    const double width = request.range.bandwidth_hz;
    const double norm = 1.0 / (detector->n * detector->energy * detector->frames);
    for (uint32_t c = 0; c < request.count; ++c) {
        auto& out = report->candidates[c];
        out.frequency_hz = request.frequencies[c];
        double delta = (double)out.frequency_hz - request.capture.capture_hz;
        const double dc_guard = std::max(bin_hz * 3, request.capture.rate_hz / 100.0);
        out.valid =
            std::abs(delta) > dc_guard + 3 * width && std::abs(delta) + 3 * width < request.capture.rate_hz * 0.45;
        if (!out.valid) {
            continue;
        }
        double signal = 0, noise = 0;
        int signal_bins = 0, noise_bins = 0;
        /* Iterate only the candidate and its sidebands, wrapping after ingest rotation. */
        int begin = (int)std::ceil((delta - 3 * width + request.capture.rotation_hz) / bin_hz);
        int end = (int)std::floor((delta + 3 * width + request.capture.rotation_hz) / bin_hz);
        for (int b = begin; b <= end; ++b) {
            int bin = (b % detector->n + detector->n) % detector->n;
            double distance = std::abs(b * bin_hz - request.capture.rotation_hz - delta);
            if (distance <= width / 2) {
                signal += detector->power[bin];
                ++signal_bins;
            } else if (distance >= width * 1.5) {
                noise += detector->power[bin];
                ++noise_bins;
            }
        }
        if (!signal_bins || noise_bins < 8) {
            out.valid = 0;
            continue;
        }
        signal *= norm;
        noise *= norm * signal_bins / noise_bins;
        out.power_dbfs = (float)(10 * std::log10(std::max(signal, 1e-16)));
        out.noise_dbfs = (float)(10 * std::log10(std::max(noise, 1e-16)));
        bool detected = out.power_dbfs - out.noise_dbfs >= request.range.sensitivity_db
                        && (!request.range.minimum_set || out.power_dbfs >= request.range.minimum_dbfs);
        out.detected = detected && detector->first[c];
        detector->first[c] = detected;
    }
}

static int
finish_window(dsd_signal_detector* detector, dsd_signal_measure_report* report) {
    if (detector->clipped * 100 > detector->total_samples) {
        detector->armed = false;
        return -1;
    }
    DSD_MEMSET(report, 0, sizeof(*report));
    report->session_id = detector->request.session_id;
    report->request_id = detector->request.request_id;
    report->generation = detector->request.capture.generation;
    report->count = detector->request.count;
    report->sample_count = detector->total_samples;
    measure_window(detector, report);
    if (++detector->windows == 2) {
        detector->armed = false;
        return 1;
    }
    detector->window_samples = 0;
    detector->frames = 0;
    DSD_MEMSET(detector->power, 0, sizeof(detector->power));
    return 0;
}

static int
accumulate_sample(dsd_signal_detector* detector, float i, float q, const float* window) {
    if (!std::isfinite(i) || !std::isfinite(q)) {
        detector->armed = false;
        return -1;
    }
    if (std::abs(i) >= 0.999f || std::abs(q) >= 0.999f) {
        ++detector->clipped;
    }
    detector->samples[2 * detector->filled] = i * window[detector->filled];
    detector->samples[2 * detector->filled + 1] = q * window[detector->filled];
    ++detector->filled;
    ++detector->total_samples;
    if (detector->filled != (size_t)detector->n) {
        return 0;
    }
    if (!detector->fft.forward(detector->n, detector->samples)) {
        detector->armed = false;
        return -1;
    }
    return 1;
}

extern "C" int
dsd_signal_detector_feed(dsd_signal_detector* detector, const float* iq, size_t pairs,
                         const dsd_signal_capture* capture, dsd_signal_measure_report* report) {
    if (!detector || !detector->armed || !iq || !capture || !report) {
        return 0;
    }
    if (!same_capture(*capture, detector->request.capture)) {
        detector->armed = false;
        return -1;
    }
    const float* window = detector->hann.get(detector->n);
    for (size_t p = 0; p < pairs; ++p) {
        int frame = accumulate_sample(detector, iq[2 * p], iq[2 * p + 1], window);
        if (frame < 0) {
            return -1;
        }
        if (!frame) {
            continue;
        }
        for (int b = 0; b < detector->n; ++b) {
            double re = detector->samples[2 * (size_t)b], im = detector->samples[2 * (size_t)b + 1];
            detector->power[b] += re * re + im * im;
        }
        detector->filled = 0;
        ++detector->frames;
        detector->window_samples += (size_t)detector->n;
        if (detector->window_samples * 100 < capture->rate_hz) {
            continue;
        }
        int ready = finish_window(detector, report);
        if (ready) {
            return ready;
        }
    }
    return 0;
}

namespace {
std::mutex tap_mutex;
dsd_signal_capture tap_capture{};
dsd_signal_detector* tap_detector = nullptr;
dsd_signal_measure_report tap_report{};
int tap_ready = 0;
uint64_t tap_last_ms = 0;
std::atomic<int> bypass{0};
std::atomic<uint64_t> search_owner{0};
} // namespace

extern "C" int
rtl_signal_measure_request(const dsd_signal_measure_request* request) {
    std::lock_guard<std::mutex> lock(tap_mutex);
    if (!tap_detector) {
        tap_detector = dsd_signal_detector_create();
    }
    tap_ready = 0;
    return dsd_signal_detector_begin(tap_detector, request);
}

extern "C" void
rtl_signal_measure_feed(const float* iq, size_t pairs, const dsd_signal_capture* capture) {
    const uint64_t received = dsd_realtime_mono_ns();
    std::lock_guard<std::mutex> lock(tap_mutex);
    if (!same_capture(tap_capture, *capture) && tap_ready) {
        tap_ready = -1;
    }
    tap_capture = *capture;
    tap_last_ms = dsd_realtime_mono_ms();
    int rc = dsd_signal_detector_feed(tap_detector, iq, pairs, capture, &tap_report);
    if (rc) {
        tap_report.received_realtime_ns = received;
        tap_ready = rc;
    }
}

extern "C" int
rtl_signal_measure_get(dsd_signal_measure_report* report) {
    if (!report) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(tap_mutex);
    int rc = tap_ready;
    if (rc == 1) {
        *report = tap_report;
    }
    tap_ready = 0;
    return rc;
}

extern "C" int
rtl_signal_capture_get(dsd_signal_capture* capture) {
    if (!capture) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(tap_mutex);
    *capture = tap_capture;
    return capture->rate_hz != 0;
}

extern "C" uint64_t
rtl_signal_capture_last_ms(void) {
    std::lock_guard<std::mutex> lock(tap_mutex);
    return tap_last_ms;
}

extern "C" void
rtl_signal_measure_cancel(void) {
    std::lock_guard<std::mutex> lock(tap_mutex);
    if (tap_detector) {
        tap_detector->armed = false;
    }
    tap_ready = 0;
}

extern "C" void
rtl_signal_search_bypass(int enabled) {
    bypass.store(enabled != 0, std::memory_order_release);
}

extern "C" int
rtl_signal_search_bypassed(void) {
    return bypass.load(std::memory_order_acquire);
}

extern "C" void
rtl_signal_search_set_owner(uint64_t session) {
    search_owner.store(session, std::memory_order_release);
}

extern "C" uint64_t
rtl_signal_search_owner(void) {
    return search_owner.load(std::memory_order_acquire);
}
