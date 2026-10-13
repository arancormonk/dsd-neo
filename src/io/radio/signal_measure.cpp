// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <mutex>
#include <new>
#include <stdint.h>
#include "rtl_fft_cache.h"

struct dsd_signal_detector {
    /* Keep aligned buffers first so records never require interior alignment padding. */
    alignas(16) float samples[2 * DSD_SEARCH_MAX_FFT]{};
    dsd_io::HannWindowCache<DSD_SEARCH_MAX_FFT> hann;
    dsd_io::FftSetupCache fft;
    dsd_signal_measure_request request{};
    double power[DSD_SEARCH_MAX_FFT]{};
    double energy = 0;
    size_t filled = 0, window_samples = 0, total_samples = 0, clipped = 0;
    unsigned frames = 0, windows = 0;
    int n = 0;
    bool armed = false;
    bool first[DSD_SEARCH_MAX_CANDIDATES]{};
    /* Per-window scratch: each candidate's threshold verdict before leakage rejection. */
    bool raw[DSD_SEARCH_MAX_CANDIDATES]{};
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

namespace {
/* Leakage rejection thresholds. A source's skirt has ended once the three-bin power falls this far below its peak
   (6 dB). A candidate is then a separate transmitter when its window rises clearly above the lowest point since
   (valley_rise_ratio()), or, where the source's other side is a clean skirt, this far above that side mirrored
   (10 dB). */
constexpr double kSkirtFallRatio = 3.9810717055349722;
constexpr double kMirrorRiseRatio = 10.0;

/* A three-bin sum of a noise-like spectrum averaged over @p frames transforms fluctuates by about
   4.34 / sqrt(3 * frames) dB. A rise must clear three standard deviations of the difference of two such sums plus
   1.5 dB, and never less than 4.5 dB: at the 15 frames a 12.5 kHz channel averages, 4.5 dB; at 4 frames, 6.8 dB. */
double
valley_rise_ratio(unsigned frames) {
    const double sigma_db = 4.3429448190325175 / std::sqrt(3.0 * std::max(1U, frames));
    const double rise_db = std::max(4.5, 1.5 + 3.0 * std::sqrt(2.0) * sigma_db);
    return std::pow(10.0, rise_db / 10.0);
}

double
bin_hz_of(const dsd_signal_detector* d) {
    return (double)d->request.capture.rate_hz / d->n;
}

int
wrap_bin(const dsd_signal_detector* d, long long b) {
    return (int)(((b % d->n) + d->n) % d->n);
}

double
power_of(const dsd_signal_detector* d, long long b) {
    return d->power[wrap_bin(d, b)];
}

double
three_bins(const dsd_signal_detector* d, long long b) {
    return power_of(d, b - 1) + power_of(d, b) + power_of(d, b + 1);
}

/* Three-bin power centred on a fractional bin position, interpolated linearly. */
double
three_bins_at(const dsd_signal_detector* d, double position) {
    const double floor_position = std::floor(position);
    const double t = position - floor_position;
    const long long b = (long long)floor_position;
    return three_bins(d, b) * (1.0 - t) + three_bins(d, b + 1) * t;
}

/* Visit every bin whose centre lies within @p half Hz of @p center (an offset from the capture centre), wrapping after
   the ingest rotation; @p f receives the bin and the offset of its centre. */
template <typename F>
void
for_window_bins(const dsd_signal_detector* d, double center, double half, F f) {
    const double bin_hz = bin_hz_of(d);
    const double rotation = d->request.capture.rotation_hz;
    const long long begin = (long long)std::ceil((center - half + rotation) / bin_hz);
    const long long end = (long long)std::floor((center + half + rotation) / bin_hz);
    for (long long b = begin; b <= end; ++b) {
        f(wrap_bin(d, b), (double)b * bin_hz - rotation);
    }
}

/* The power-weighted centre of the bins within @p half of @p around that @p weight keeps. */
template <typename W>
double
centre_of(long long around, long long half, W weight) {
    double total = 0, moment = 0;
    for (long long b = around - half; b <= around + half; ++b) {
        const double w = weight(b);
        total += w;
        moment += w * (double)b;
    }
    return total > 0 ? moment / total : (double)around;
}

double
window_power(const dsd_signal_detector* d, long long centre, long long half) {
    double sum = 0;
    for (long long b = centre - half; b <= centre + half; ++b) {
        sum += power_of(d, b);
    }
    return sum;
}

/* Where a transmitter's three-bin power, walking from its peak at @p peak_bin in direction @p step, first falls to
   @p floor, interpolated in dB between bins; 0 when it does not within @p limit bins. */
int
falls_to(const dsd_signal_detector* d, long long peak_bin, long long step, long long limit, double floor,
         double* position) {
    double previous = three_bins(d, peak_bin);
    for (long long i = 1; i <= limit; ++i) {
        const long long k = peak_bin + step * i;
        const double current = three_bins(d, k);
        if (current <= floor) {
            const double above = 10 * std::log10(std::max(previous, 1e-300));
            const double below = 10 * std::log10(std::max(current, 1e-300));
            const double level = 10 * std::log10(std::max(floor, 1e-300));
            const double t = above > below ? (above - level) / (above - below) : 0.0;
            *position = (double)(k - step) + (double)step * t;
            return 1;
        }
        previous = current;
    }
    return 0;
}

/* A transmitter located near bin @p rough: its peak three-bin power, where the skirt walk starts, and its centre,
   midway between the points where that power has fallen 10 dB below the peak on either side. Neither a window that
   truncates a wide transmitter nor its internal sideband structure biases that centre the way a windowed centroid is
   biased, which matters because the mirrored-skirt comparison mirrors about it; and 10 dB stays inside the
   transmitter's own body, so a weaker neighbour close by does not drag it. The centre stays @p rough when either side
   does not fall that far nearby. */
struct source_location {
    long long peak;
    double centre;
};

source_location
locate_source(const dsd_signal_detector* d, double rough, long long half, long long limit) {
    const long long around = std::llround(rough);
    long long peak_bin = around;
    double peak = three_bins(d, around);
    for (long long b = around - half; b <= around + half; ++b) {
        const double sum = three_bins(d, b);
        if (sum > peak) {
            peak = sum;
            peak_bin = b;
        }
    }
    double low = 0, high = 0;
    double centre = rough;
    if (falls_to(d, peak_bin, -1, limit, peak / 10.0, &low) && falls_to(d, peak_bin, 1, limit, peak / 10.0, &high)
        && std::abs((low + high) / 2 - rough) <= (double)half) {
        centre = (low + high) / 2;
    }
    return source_location{peak_bin, centre};
}

/* The strongest transmitter near candidate bin @p centre other than the candidate's own signal: the strongest local
   maximum of channel-window power within @p reach bins, counting only bins more than @p own bins from the candidate
   (its own grid position, where an off-grid signal of its own may sit). Returns 0 when there is none, else 1 with the
   transmitter's location (locate_source()). The spectrum is searched rather than the requested candidates, so a
   transmitter past a range or span boundary is found too. */
int
strongest_source(const dsd_signal_detector* d, long long centre, long long half, long long reach, double own,
                 source_location* source) {
    auto outside = [&](long long b) { return std::abs((double)(b - centre)) > own ? power_of(d, b) : 0.0; };
    const long long first = centre - reach - 1;
    double current = 0;
    for (long long b = first - half; b <= first + half; ++b) {
        current += outside(b);
    }
    double previous = -1, best = 0;
    long long best_p = 0;
    for (long long p = first; p <= centre + reach; ++p) {
        const double next = current - outside(p - half) + outside(p + 1 + half);
        if (previous >= 0 && current >= previous && current >= next && current > best) {
            best = current;
            best_p = p;
        }
        previous = current;
        current = next;
    }
    if (!(best > 0)) {
        return 0;
    }
    *source = locate_source(d, centre_of(best_p, half, outside), half, reach);
    return 1;
}

/* Walk @p length bins from bin @p start in direction @p step. Returns 1 when the three-bin power rises again after the
   source's skirt has fallen away (a separate transmitter there), counting rises only at bins @p judged accepts. */
template <typename J>
bool
rises_after_skirt(const dsd_signal_detector* d, long long start, long long step, long long length, J judged) {
    double peak = 0, valley = 0;
    bool fallen = false;
    for (long long i = 0; i <= length; ++i) {
        const long long k = start + step * i;
        const double sum = three_bins(d, k);
        if (!fallen) {
            peak = std::max(peak, sum);
            if (sum * kSkirtFallRatio <= peak) {
                fallen = true;
                valley = sum;
            }
            continue;
        }
        valley = std::min(valley, sum);
        if (judged(k, sum, valley)) {
            return true;
        }
    }
    return false;
}

/* Whether the candidate at bin @p centre is a transmitter of its own rather than the skirt of the stronger @p source.
   A skirt only falls away from its transmitter, so walking from the source through the candidate's window, a
   separate transmitter makes the power rise again inside that window after the skirt has fallen. Leakage, an
   off-grid neighbour truncated by the window and a wide transmitter spanning several channels all fall monotonically
   there, whatever lies on the source's other side. Rises within the source's own channel width are its own spectrum
   (an FM transmitter's sidebands, which need not fall monotonically) and never count. A weak transmitter sitting on
   the skirt may never clear that valley; when the source's other side is itself a clean skirt, the window is also
   compared with that side mirrored about the source, a transmitter's spectrum being close to symmetric. */
bool
independent_of(const dsd_signal_detector* d, const source_location& source, long long centre, long long half) {
    const long long start = source.peak;
    const long long step = centre >= start ? 1 : -1;
    const long long length = std::llabs(centre + step * half - start);
    const double rise = valley_rise_ratio(d->frames);
    auto beyond_source = [&](long long k) { return std::abs((double)k - source.centre) > (double)half; };
    const bool clean_far_side =
        !rises_after_skirt(d, start, -step, length, [&](long long k, double sum, double valley) {
            return beyond_source(k) && sum >= rise * valley;
        });
    return rises_after_skirt(d, start, step, length, [&](long long k, double sum, double valley) {
        if (std::llabs(k - centre) > half || !beyond_source(k)) {
            return false;
        }
        return sum >= rise * valley
               || (clean_far_side && sum >= kMirrorRiseRatio * three_bins_at(d, 2 * source.centre - (double)k));
    });
}

/* Clear raw detections that a stronger transmitter within two channel widths explains: its skirt and the window's
   leakage reach a neighbouring channel's window and can pass that channel's threshold on its own. */
void
reject_leakage(dsd_signal_detector* d) {
    const auto& request = d->request;
    const double bin_hz = bin_hz_of(d);
    const double width = request.range.bandwidth_hz;
    const double own = (double)request.range.step_num / request.range.step_den / 2 / bin_hz;
    const long long half = std::max(1LL, (long long)std::floor(width / 2 / bin_hz));
    const long long reach = (long long)std::ceil(2 * width / bin_hz);
    bool keep[DSD_SEARCH_MAX_CANDIDATES]{};
    for (uint32_t c = 0; c < request.count; ++c) {
        if (!d->raw[c]) {
            continue;
        }
        const double delta = (double)request.frequencies[c] - request.capture.capture_hz;
        const long long centre = std::llround((delta + request.capture.rotation_hz) / bin_hz);
        /* A transmitter belongs to the grid channel nearest its centre: one centred on this channel's own grid
           position, or no stronger than this channel's window, leaves it standing. */
        source_location source{0, 0};
        keep[c] = !strongest_source(d, centre, half, reach, own, &source)
                  || std::abs(source.centre - (double)centre) <= own
                  || window_power(d, std::llround(source.centre), half) <= window_power(d, centre, half)
                  || independent_of(d, source, centre, half);
    }
    for (uint32_t c = 0; c < request.count; ++c) {
        d->raw[c] = keep[c];
    }
}
} // namespace

static void
measure_window(dsd_signal_detector* detector, dsd_signal_measure_report* report) {
    const auto& request = detector->request;
    const double bin_hz = bin_hz_of(detector);
    const double width = request.range.bandwidth_hz;
    const double norm = 1.0 / (detector->n * detector->energy * detector->frames);
    for (uint32_t c = 0; c < request.count; ++c) {
        auto& out = report->candidates[c];
        detector->raw[c] = false;
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
        for_window_bins(detector, delta, 3 * width, [&](int bin, double offset) {
            const double distance = std::abs(offset - delta);
            if (distance <= width / 2) {
                signal += detector->power[bin];
                ++signal_bins;
            } else if (distance >= width * 1.5) {
                noise += detector->power[bin];
                ++noise_bins;
            }
        });
        if (!signal_bins || noise_bins < 8) {
            out.valid = 0;
            continue;
        }
        signal *= norm;
        noise *= norm * signal_bins / noise_bins;
        out.power_dbfs = (float)(10 * std::log10(std::max(signal, 1e-16)));
        out.noise_dbfs = (float)(10 * std::log10(std::max(noise, 1e-16)));
        detector->raw[c] = out.power_dbfs - out.noise_dbfs >= request.range.sensitivity_db
                           && (!request.range.minimum_set || out.power_dbfs >= request.range.minimum_dbfs);
    }
    reject_leakage(detector);
    for (uint32_t c = 0; c < request.count; ++c) {
        auto& out = report->candidates[c];
        out.detected = detector->raw[c] && detector->first[c];
        detector->first[c] = detector->raw[c];
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
