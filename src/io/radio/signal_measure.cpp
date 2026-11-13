// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/runtime/analog_channel.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <iterator>
#include <mutex>
#include <new>
#include <stdint.h>
#include "rtl_fft_cache.h"
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
#include "rtl_stream_test_support.h"
#endif

namespace {
/* A located transmitter: its centre in FFT bins, the analysis bins of its own channel width (where a transmitter
   beside it does not lie), and where its own spectrum may lie (its segment, cut where a bridged gap left its centre
   without power). */
struct located {
    double centre;
    long long first, last;
    long long own_first, own_last;
    long long anchor; /* the analysis bin it was located from, or LLONG_MIN for a hump */
};

/* Every transmitter located in one spectrum: primaries strongest first, then the separate ones beside them. */
struct source_list {
    located items[2 * DSD_SEARCH_MAX_CANDIDATES];
    int count;

    /* The item @p s was already located as, or -1: either one's centre lies within the other's channel width. The
       item at @p beside_index, when given, is the source @p s was found beside, which it is separate from by
       construction. */
    int
    find(const located& s, int decimation, int beside_index = -1) const {
        /* Decimations are powers of two, so scaling by the reciprocal is exact. */
        const double scale = 1.0 / decimation, s_coarse = s.centre * scale;
        for (int i = 0; i < count; ++i) {
            if (i == beside_index) {
                continue;
            }
            const located& t = items[i];
            const double t_coarse = t.centre * scale;
            if ((s_coarse >= (double)t.first && s_coarse <= (double)t.last + 1)
                || (t_coarse >= (double)s.first && t_coarse <= (double)s.last + 1)) {
                return i;
            }
        }
        return -1;
    }

    bool
    known(const located& s, int decimation, int beside_index = -1) const {
        return find(s, decimation, beside_index) >= 0;
    }

    void
    add(const located& s) {
        if (count < (int)(sizeof(items) / sizeof(items[0]))) {
            items[count++] = s;
        }
    }
};
} // namespace

struct dsd_signal_detector {
    /* Keep aligned buffers first so records never require interior alignment padding. */
    alignas(16) float samples[2 * DSD_SEARCH_MAX_FFT]{};
    dsd_io::HannWindowCache<DSD_SEARCH_MAX_FFT> hann;
    dsd_io::FftSetupCache fft;
    dsd_signal_measure_request request{};
    /* This window's power, and the first window's once it is complete. */
    double power[DSD_SEARCH_MAX_FFT]{};
    double first_power[DSD_SEARCH_MAX_FFT]{};
    /* Scratch: prefix sums of a window (one longer than the spectrum), and leakage rejection's spectrum, its three-bin
       sums, their two envelopes and the transmitters located in it. */
    double prefix[DSD_SEARCH_MAX_FFT + 1]{};
    double analysed[DSD_SEARCH_MAX_FFT]{};
    double three[DSD_SEARCH_MAX_FFT]{};
    double envelope[DSD_SEARCH_MAX_FFT]{};
    double near_envelope[DSD_SEARCH_MAX_FFT]{};
    source_list sources{};
    double energy = 0;
    size_t filled = 0, window_samples = 0, total_samples = 0, clipped = 0;
    unsigned frames = 0, first_frames = 0, windows = 0;
    int n = 0;
    bool armed = false;
    /* Each candidate's threshold verdict in the first window, and in the current one. */
    bool first[DSD_SEARCH_MAX_CANDIDATES]{};
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
    detector->frames = detector->first_frames = detector->windows = 0;
    DSD_MEMSET(detector->power, 0, (size_t)n * sizeof(detector->power[0]));
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

/* Why the capture @p now is not the request's @p asked: dropped samples alone, any other change, or neither. */
static dsd_signal_measure_failure
capture_difference(const dsd_signal_capture& asked, const dsd_signal_capture& now) {
    dsd_signal_capture without_drops = now;
    without_drops.drops = asked.drops;
    if (!same_capture(asked, without_drops)) {
        return DSD_SIGNAL_MEASURE_CAPTURE_CHANGED;
    }
    return asked.drops != now.drops ? DSD_SIGNAL_MEASURE_DROPPED : DSD_SIGNAL_MEASURE_OK;
}

/* Retire the request for @p reason: the report names the request and says why, with no candidates. */
static int
fail_request(dsd_signal_detector* detector, dsd_signal_measure_report* report, dsd_signal_measure_failure reason) {
    detector->armed = false;
    DSD_MEMSET(report, 0, sizeof(*report));
    report->session_id = detector->request.session_id;
    report->request_id = detector->request.request_id;
    report->generation = detector->request.capture.generation;
    report->sample_count = detector->total_samples;
    report->failure = reason;
    return -1;
}

namespace {
/* Leakage rejection thresholds. A skirt has ended once the envelope falls this far below its peak (6 dB). A transmitter
   beside a source stands this far above the source's far side mirrored (10 dB), and is only compared with a mirror when
   the source's spectrum overlaps its own mirror image at least this much (half). A source centre with this little of
   the source's peak power (20 dB down) may lie between two transmitters, which are told apart from one when two sources
   each symmetric about their own centres explain at least as much of the power, less this margin. */
constexpr double kSkirtFallRatio = 3.9810717055349722;
constexpr double kMirrorRiseRatio = 10.0;
constexpr double kMirrorSymmetry = 0.5;
constexpr double kWeakCentreRatio = 0.01;
constexpr double kTwoSourceMargin = 0.06;
/* A transmitter is sustained when each window holds at least a tenth of the other's power over the bins it occupies. */
constexpr double kSustainedRatio = 10.0;
/* Leakage analysis aggregates bins until a channel's half width spans at most this many (a power of two apart). */
constexpr long long kAnalysisHalfBins = 32;
/* How far the envelopes reach either side. Within a source's segment, an FM transmitter's sideband line spacing for the
   highest voice tone (3 kHz), so a weak carrier between its first sidebands is not a valley; walking out past the
   source, half that, which still bridges the gaps between its sideband lines but not a gap before a neighbour. */
constexpr double kEnvelopeHz = 3000.0;
constexpr double kNearEnvelopeHz = 1500.0;
/* A channel whose window climbs toward a transmitter further than this many reaches away is only that transmitter's
   skirt; the analysis covers that much spectrum around its channels. */
constexpr long long kClimbReaches = 6;
constexpr long long kFarSource = LLONG_MIN;

/* A three-bin sum of a noise-like spectrum averaged over @p frames transforms fluctuates by about
   4.34 / sqrt(3 * frames) dB. A rise must clear three standard deviations of the difference of two such sums plus
   1.5 dB, and never less than 4.5 dB: at the 30 frames two windows of a 12.5 kHz channel average, 4.5 dB; at 2, 9 dB. */
double
valley_rise_ratio(unsigned frames) {
    const double sigma_db = 4.3429448190325175 / std::sqrt(3.0 * std::max(1U, frames));
    const double rise_db = std::max(4.5, 1.5 + 3.0 * std::sqrt(2.0) * sigma_db);
    return std::pow(10.0, rise_db / 10.0);
}

/* FFT sizes are powers of two, so a mask wraps a bin (negative ones too). */
int
wrap_index(long long b, int n) {
    return (int)((unsigned long long)b & (unsigned long long)(n - 1));
}

/* Sum of @p count values of the circular sequence whose prefix sums are @p prefix, from index @p first. */
double
circular_sum(const double* prefix, int n, long long first, long long count) {
    if (count <= 0) {
        return 0;
    }
    const long long a = wrap_index(first, n);
    if (a + count <= n) {
        return prefix[a + count] - prefix[a];
    }
    return prefix[n] - prefix[a] + prefix[a + count - n];
}

/* A spectrum for leakage rejection: one window's power or both windows', aggregated so a channel's half width spans at
   most kAnalysisHalfBins analysis bins, with its three-bin sums and their two envelopes. Positions are fractional bins from
   bin 0 before wrapping, so the ingest rotation is already applied; fine positions count FFT bins, coarse ones
   analysis bins. */
struct analysis {
    const double* first;
    const double* second; /* the other window's power, or null */
    const double* three;
    const double* envelope;
    const double* near_envelope;
    int fine_n, n, decimation;
    double fine_bin_hz;
    long long half, reach;
    double rise;

    double
    fine(long long b) const {
        const int i = wrap_index(b, fine_n);
        return first[i] + (second ? second[i] : 0.0);
    }

    /* Unaggregated, the analysis bins' three-bin sums are the FFT bins' own. */
    double
    fine_three(long long b) const {
        return decimation == 1 ? at(b) : fine(b - 1) + fine(b) + fine(b + 1);
    }

    double
    at(long long b) const {
        return three[wrap_index(b, n)];
    }

    double
    env(long long b) const {
        return envelope[wrap_index(b, n)];
    }

    double
    near_env(long long b) const {
        return near_envelope[wrap_index(b, n)];
    }

    /* The near envelope at a fractional position, interpolated linearly. */
    double
    near_env(double position) const {
        const double floor_position = std::floor(position);
        const double t = position - floor_position;
        const long long b = (long long)floor_position;
        return near_env(b) * (1.0 - t) + near_env(b + 1) * t;
    }

    double
    to_fine(double coarse) const {
        return coarse * decimation + (decimation - 1) / 2.0;
    }

    double
    to_coarse(double fine_position) const {
        return (fine_position - (decimation - 1) / 2.0) / decimation;
    }

    /* A requested frequency's position in analysis bins. */
    double
    position_of(const dsd_signal_measure_request& r, uint32_t frequency) const {
        return to_coarse(((double)frequency - r.capture.capture_hz + r.capture.rotation_hz) / fine_bin_hz);
    }
};

/* The envelope over bins @p lo..@p hi: the largest three-bin sum within @p spread bins, directly when that is a few
   bins, otherwise by a running maximum. */
void
build_envelope(const double* three, int n, long long lo, long long hi, long long spread, double* envelope) {
    if (spread <= 3) {
        for (long long j = lo; j <= hi; ++j) {
            double e = three[wrap_index(j - spread, n)];
            for (long long k = j - spread + 1; k <= j + spread; ++k) {
                e = std::max(e, three[wrap_index(k, n)]);
            }
            envelope[wrap_index(j, n)] = e;
        }
        return;
    }
    /* Bin indices of a non-increasing run of sums, oldest first; a window holds 2 * spread + 1. */
    constexpr unsigned long long kCapacity = 1024;
    long long queue[kCapacity];
    spread = std::min(spread, (long long)kCapacity / 2 - 1);
    unsigned long long head = 0, tail = 0;
    for (long long k = lo - spread; k <= hi + spread; ++k) {
        const double v = three[wrap_index(k, n)];
        while (tail > head && three[wrap_index(queue[(tail - 1) % kCapacity], n)] <= v) {
            --tail;
        }
        queue[tail++ % kCapacity] = k;
        const long long j = k - spread;
        if (queue[head % kCapacity] < j - spread) {
            ++head;
        }
        if (j >= lo) {
            envelope[wrap_index(j, n)] = three[wrap_index(queue[head % kCapacity], n)];
        }
    }
}

/* The source's own segment of spectrum: from its peak outwards until the envelope, after a valley clearly (by the valley
   rise) below the peak, rises as clearly again, the valley before another transmitter; or until reach ends. Shallower
   dips are the source's own (the plateau between a wide FM transmitter's horns). */
void
segment_of(const analysis& a, long long peak, long long* first, long long* last, bool* cut_low = nullptr,
           bool* cut_high = nullptr) {
    const double deep = a.env(peak) / a.rise;
    for (int side = -1; side <= 1; side += 2) {
        long long edge = peak;
        double lowest = a.env(peak);
        bool cut = false;
        for (long long i = 1; i <= a.reach && !cut; ++i) {
            const long long k = peak + side * i;
            const double e = a.env(k);
            cut = lowest <= deep && e >= a.rise * lowest;
            if (!cut) {
                lowest = std::min(lowest, e);
                edge = k;
            }
        }
        *(side < 0 ? first : last) = edge;
        if (side < 0 && cut_low) {
            *cut_low = cut;
        }
        if (side > 0 && cut_high) {
            *cut_high = cut;
        }
    }
}

/* The tallest bin of the segment around @p peak, reached by climbing and then moving to each segment's tallest in
   turn, so every channel that sees a source locates it from the same bin; kFarSource once that leads further than
   @p reaches from where it started. Each move is to a taller bin, so the moves end. */
long long
anchor_of(const analysis& a, long long peak, long long reaches = kClimbReaches) {
    const long long origin = peak, limit = reaches * a.reach;
    while (a.at(peak + 1) > a.at(peak) || a.at(peak - 1) > a.at(peak)) {
        peak += a.at(peak + 1) > a.at(peak - 1) ? 1 : -1;
        if (std::llabs(peak - origin) > limit) {
            return kFarSource;
        }
    }
    for (;;) {
        long long first = peak, last = peak, tallest = peak;
        segment_of(a, peak, &first, &last);
        for (long long b = first; b <= last; ++b) {
            if (a.at(b) > a.at(tallest)) {
                tallest = b;
            }
        }
        if (tallest == peak) {
            return peak;
        }
        if (std::llabs(tallest - origin) > limit) {
            return kFarSource;
        }
        peak = tallest;
    }
}

/* The half-bin position, from @p lowest_m / 2 to @p highest_m / 2, about which the most of @p value over
   @p first..@p last overlaps its own mirror image, refined parabolically. Writes that overlap's share of the power (all
   of it for a perfectly symmetric spectrum) to @p symmetry when given. A transmitter's extent is symmetric about its
   carrier even where its short-term power is not (a slow tone or voice part-way through a cycle leaves one side
   taller), a notch inside it overlaps its mirror notch, and a neighbour within the range overlaps only the source's far
   skirt. */
template <typename V>
double
symmetric_centre(long long first, long long last, long long lowest_m, long long highest_m, V value, double* symmetry) {
    auto overlap = [&](long long m) {
        double pairs = 0;
        long long b = std::max(first, m - last);
        for (; 2 * b < m; ++b) {
            pairs += std::min(value(b), value(m - b));
        }
        return 2 * pairs + (2 * b == m ? value(b) : 0.0);
    };
    long long best_m = lowest_m;
    double best = -1, before = 0, after = 0, previous = 0;
    for (long long m = lowest_m; m <= highest_m; ++m) {
        const double score = overlap(m);
        if (m == best_m + 1) {
            after = score;
        }
        if (score > best) {
            before = previous;
            best = score;
            best_m = m;
        }
        previous = score;
    }
    double offset = 0;
    if (best_m > lowest_m && best_m < highest_m) {
        const double curvature = before - 2 * best + after;
        if (curvature < 0) {
            offset = std::max(-0.5, std::min(0.5, 0.5 * (before - after) / curvature));
        }
    }
    const double centre = (best_m + offset) / 2;
    if (symmetry) {
        /* Its share at the refined centre, mirroring between bins by linear interpolation: a source mirrored about the
           nearest half bin overlaps itself less than two identical transmitters mirror onto each other. */
        double total = 0, overlapping = 0;
        for (long long b = first; b <= last; ++b) {
            total += value(b);
            const double mirror = 2 * centre - (double)b;
            if (mirror >= (double)first && mirror <= (double)last) {
                const long long low = (long long)std::floor(mirror);
                const double t = mirror - (double)low;
                const double mirrored = value(low) * (1.0 - t) + (t > 0 ? value(low + 1) * t : 0.0);
                overlapping += std::min(value(b), mirrored);
            }
        }
        *symmetry = total > 0 ? overlapping / total : 0;
    }
    return centre;
}

/* Whether two sources over @p low_first..@p low_last and @p high_first..@p high_last, each symmetric about its own
   centre, explain at least as much of the power as one over both (less kTwoSourceMargin). Writes the one source's
   symmetry to @p whole. */
bool
two_sources(const analysis& a, long long low_first, long long low_last, long long high_first, long long high_last,
            double* whole) {
    auto coarse = [&](long long b) { return a.at(b); };
    double low_symmetry = 0, high_symmetry = 0, low_power = 0, high_power = 0;
    symmetric_centre(low_first, high_last, 2 * low_first, 2 * high_last, coarse, whole);
    symmetric_centre(low_first, low_last, 2 * low_first, 2 * low_last, coarse, &low_symmetry);
    symmetric_centre(high_first, high_last, 2 * high_first, 2 * high_last, coarse, &high_symmetry);
    for (long long b = low_first; b <= low_last; ++b) {
        low_power += a.at(b);
    }
    for (long long b = high_first; b <= high_last; ++b) {
        high_power += a.at(b);
    }
    const double two = (low_symmetry * low_power + high_symmetry * high_power) / (low_power + high_power);
    return two >= *whole - kTwoSourceMargin;
}

/* Whether the peak at @p beyond, past the valley at @p edge, may be one of @p anchor's transmitter's lobes: within twice
   the channel width of it, with its own power filling the valley between them (within 20 dB of the lower, where a gap
   between two transmitters falls to the floor). */
bool
lobes_bridged(const analysis& a, long long anchor, long long edge, long long beyond) {
    if (std::llabs(beyond - anchor) > 4 * a.half) {
        return false;
    }
    double valley = a.env(edge);
    for (long long k = std::min(edge, beyond); k <= std::max(edge, beyond); ++k) {
        valley = std::min(valley, a.env(k));
    }
    return valley >= std::min(a.env(anchor), a.env(beyond)) * kWeakCentreRatio;
}

/* Whether the segment @p first..@p last and the next on @p side, @p next_first..@p next_last, are one transmitter's
   lobes: no more than a reach apart (each segment stops short of the valley between them, which the lobes' filled
   valley already showed is one transmitter's), and better explained by one source than two. */
bool
lobes_of_one(const analysis& a, int side, long long first, long long last, long long next_first, long long next_last) {
    if (side < 0 ? next_last < first - 1 - a.reach : next_first > last + 1 + a.reach) {
        return false;
    }
    double merged = 0;
    return side < 0 ? !two_sources(a, next_first, next_last, first, last, &merged)
                    : !two_sources(a, first, last, next_first, next_last, &merged);
}

/* Across a valley the segment of @p anchor stopped at on @p side (when @p cut), takes in the segments beyond while they
   are the same transmitter's lobes (the plateau between a wide FM transmitter's horns, each lopsided). */
void
merge_lobes(const analysis& a, long long anchor, int side, bool cut, long long* first, long long* last) {
    for (int merges = 0; merges < 2 && cut; ++merges) {
        const long long edge = side < 0 ? *first : *last;
        long long beyond = edge + side;
        for (long long i = 2; i <= a.reach; ++i) {
            if (a.at(edge + side * i) > a.at(beyond)) {
                beyond = edge + side * i;
            }
        }
        if (!lobes_bridged(a, anchor, edge, beyond)) {
            return;
        }
        long long next_first = beyond, next_last = beyond;
        bool next_low = false, next_high = false;
        segment_of(a, beyond, &next_first, &next_last, &next_low, &next_high);
        if (!lobes_of_one(a, side, *first, *last, next_first, next_last)) {
            return;
        }
        if (side < 0) {
            *first = next_first;
            cut = next_low;
        } else {
            *last = next_last;
            cut = next_high;
        }
    }
}

/* The centre of @p first..@p last, with its symmetry in @p whole. A centre with little power is a carrier its
   modulation weakens, or the gap between two transmitters the envelope bridged: when two sources, each symmetric about
   its own centre, explain the halves as well as one (a carrier-null FM transmitter's halves do not mirror themselves),
   the range is cut there and the anchor's side located alone. */
double
centre_or_split(const analysis& a, long long anchor, long long* first, long long* last, double* whole) {
    auto coarse = [&](long long b) { return a.at(b); };
    const double centre = symmetric_centre(*first, *last, 2 * *first, 2 * *last, coarse, whole);
    const long long cut = std::llround(centre);
    double unused = 0;
    if (a.at(cut) >= a.at(anchor) * kWeakCentreRatio || cut <= *first || cut >= *last
        || !two_sources(a, *first, cut - 1, cut + 1, *last, &unused)) {
        return centre;
    }
    if (anchor < cut) {
        *last = cut - 1;
    } else {
        *first = cut + 1;
    }
    return symmetric_centre(*first, *last, 2 * *first, 2 * *last, coarse, whole);
}

/* The centre within an analysis bin of @p fine_centre (FFT bins) of the source over analysis bins @p first..@p last,
   refined on the FFT bins of its body: every bin within its own width that is within 40 dB of the tallest there, the
   gaps between a modulated transmitter's spectral lines counting as empty, so all of its lines take part. */
double
refine_on_fft_bins(const analysis& a, long long first, long long last, double fine_centre) {
    const double coarse_centre = a.to_coarse(fine_centre);
    const long long limit_first = std::max(first, (long long)std::floor(coarse_centre - (double)a.half)) * a.decimation;
    const long long limit_last =
        std::min(last, (long long)std::ceil(coarse_centre + (double)a.half)) * a.decimation + a.decimation - 1;
    double tallest = 0;
    for (long long b = limit_first; b <= limit_last; ++b) {
        tallest = std::max(tallest, a.fine_three(b));
    }
    const double floor_level = tallest * 1e-4;
    long long fine_first = limit_first, fine_last = limit_last;
    while (fine_first < fine_last && a.fine_three(fine_first) < floor_level) {
        ++fine_first;
    }
    while (fine_last > fine_first && a.fine_three(fine_last) < floor_level) {
        --fine_last;
    }
    const long long twice = std::llround(2 * fine_centre);
    return symmetric_centre(
        fine_first, fine_last, twice - 2LL * a.decimation, twice + 2LL * a.decimation,
        [&](long long b) {
            const double value = a.fine_three(b);
            return value >= floor_level ? value : 0.0;
        },
        nullptr);
}

/* The source anchored at @p anchor, with its symmetry: centred at the most symmetric point of its segment, which takes
   in the same transmitter's lobes beyond a valley and is cut at a centre between two transmitters. With aggregated bins
   the centre is refined on the FFT bins. */
located
locate_source(const analysis& a, long long anchor, double* symmetry) {
    long long first = anchor, last = anchor;
    bool cut_low = false, cut_high = false;
    segment_of(a, anchor, &first, &last, &cut_low, &cut_high);
    merge_lobes(a, anchor, -1, cut_low, &first, &last);
    merge_lobes(a, anchor, 1, cut_high, &first, &last);
    const double centre = centre_or_split(a, anchor, &first, &last, symmetry);
    const double fine_centre =
        a.decimation > 1 ? refine_on_fft_bins(a, first, last, a.to_fine(centre)) : a.to_fine(centre);
    const double coarse_centre = a.to_coarse(fine_centre);
    return located{fine_centre,
                   std::max(first, (long long)std::floor(coarse_centre - a.half)),
                   std::min(last, (long long)std::ceil(coarse_centre + a.half)),
                   first,
                   last,
                   anchor};
}

/* The power-weighted centre in FFT bins of @p weight over analysis bins @p first..@p last. */
template <typename W>
double
fine_centroid(const analysis& a, long long first, long long last, W weight) {
    double total = 0, moment = 0;
    for (long long b = first * a.decimation; b <= last * a.decimation + a.decimation - 1; ++b) {
        const double w = weight(b);
        total += w;
        moment += w * (double)b;
    }
    return total > 0 ? moment / total : a.to_fine(0.5 * (double)(first + last));
}

/* Transmitters beside a located source on one side. A skirt only falls away from its transmitter, so walking out from
   the source a separate transmitter makes the envelope rise clearly above the lowest point after the skirt has fallen
   6 dB. A weak transmitter sitting on the skirt may never clear that valley; when the source's other side is a clean
   skirt and its spectrum mostly overlaps its mirror image, a transmitter beside it also stands 10 dB above that side
   mirrored about the source. Within the source's own channel width a rise is usually its own spectrum (an FM
   transmitter's sidebands); it counts there only when the source mostly overlaps its mirror image, the rise stands
   10 dB above its own mirror image (sideband lines come in mirrored pairs) and its valley falls 20 dB below its top
   (a lopsided edge of the source's own modulation leaves the valley filled). Each separate transmitter is located
   once, however it was found: the centre of what remains of its hump once the source's mirror image (or, without a
   clean mirror, the valley's level) is taken away. */
struct beside {
    const analysis& a;
    const located& source;
    source_list* sources;
    double centre; /* the source's, in analysis bins */
    long long start;
    int side;
    int source_index; /* the source's in @p sources */
    bool mirror;
    bool symmetric; /* the source mostly overlaps its mirror image */

    bool
    outside(long long k) const {
        return std::abs((double)k - centre) > (double)a.half || k < source.own_first || k > source.own_last;
    }

    double
    mirrored(long long k) const {
        return a.near_env(2 * centre - (double)k);
    }

    double
    remainder(long long k, double floor_level) const {
        return std::max(0.0, a.near_env(k) - (mirror ? mirrored(k) : floor_level));
    }

    /* Whether the side away from this one is a clean skirt: no valley rise there within reach. */
    bool
    clean_far_side() const {
        double peak = 0, valley = 0;
        bool fallen = false;
        for (long long i = 0; i <= a.reach; ++i) {
            const long long k = start - side * i;
            const double e = a.near_env(k);
            if (!fallen) {
                peak = std::max(peak, e);
                fallen = e * kSkirtFallRatio <= peak;
                valley = e;
                continue;
            }
            valley = std::min(valley, e);
            if (outside(k) && e >= a.rise * valley) {
                return false;
            }
        }
        return true;
    }

    /* Whether bin @p j may belong to a hump rising at @p k beyond @p inner on this side: outside the source's own
       width, or within it too when @p within. */
    bool
    in_hump(long long j, long long k, long long inner, bool within) const {
        return (within || outside(j)) && (j - inner) * side > 0 && std::llabs(j - k) <= a.reach;
    }

    /* Out from @p k, through the ripples of a modulated transmitter's lines, until what remains falls to a tenth of
       its tallest or rises clearly again from a clear valley (another transmitter): the tallest bin into @p top, the
       last bin reached into @p end. */
    void
    hump_span(long long k, double floor_level, long long inner, bool within, long long* top, long long* end) const {
        *top = *end = k;
        double low = remainder(k, floor_level);
        for (long long j = k + side; in_hump(j, k, inner, within); j += side) {
            const double r = remainder(j, floor_level), tallest = remainder(*top, floor_level);
            if (r * 10 <= tallest || (low * a.rise <= tallest && r >= a.rise * low)) {
                break;
            }
            if (r > tallest) {
                *top = j;
                low = r;
            } else {
                low = std::min(low, r);
            }
            *end = j;
        }
    }

    /* The hump rising at bin @p k beyond @p inner on this side: what remains above the mirror, or above @p floor_level
       without one, over its span out from where it rose and back toward the valley as far as it stays above a tenth
       of its tallest. Within the source's own width too when @p within. */
    located
    hump(long long k, double floor_level, long long inner, bool within = false) const {
        auto inside = [&](long long j) { return in_hump(j, k, inner, within); };
        auto rest = [&](long long j) { return remainder(j, floor_level); };
        long long top = k, end = k;
        hump_span(k, floor_level, inner, within, &top, &end);
        while (inside(top - side) && rest(top - side) > rest(top)) {
            top -= side;
        }
        const double level = rest(top) / 10;
        long long lo = top, hi = top;
        while (inside(lo - 1) && rest(lo - 1) > level && (lo - 1 - end) * side <= 0) {
            --lo;
        }
        while (inside(hi + 1) && rest(hi + 1) > level && (hi + 1 - end) * side <= 0) {
            ++hi;
        }
        const double fine_floor = floor_level / (3.0 * a.decimation);
        const double twice = 2 * source.centre;
        const double at = fine_centroid(a, lo, hi, [&](long long b) {
            return std::max(0.0,
                            a.fine_three(b) - (mirror ? a.fine_three(std::llround(twice - (double)b)) : fine_floor));
        });
        return located{at, lo, hi, lo, hi, LLONG_MIN};
    }

    /* The separate transmitter rising at @p k beyond @p inner: located as a source of its own when the tallest bin of
       its own segment lies within a reach beyond the valley, so all of it is centred, not only the part that rose
       first; otherwise (its segment runs back into this source's) the hump of what remains above the mirror or
       @p floor_level. */
    located
    separate(long long k, double floor_level, long long inner) const {
        const long long anchor = anchor_of(a, k, 1);
        if (anchor != kFarSource && (anchor - inner) * side > 0) {
            /* Located from the same bin, a source already located would be located again exactly as it was. */
            const located* same = nullptr;
            for (int i = 0; i < sources->count && !same; ++i) {
                if (sources->items[i].anchor == anchor) {
                    same = &sources->items[i];
                }
            }
            double symmetry = 0;
            const located s = same ? *same : locate_source(a, anchor, &symmetry);
            if ((a.to_coarse(s.centre) - (double)inner) * side > 0 && outside(std::llround(a.to_coarse(s.centre)))) {
                return s;
            }
        }
        return hump(k, floor_level, inner);
    }

    /* Records @p s unless already located (by another source than this one when it lies @p within this one's
       width); returns how far out it reaches. */
    long long
    record(const located& s, bool within = false) {
        if (!sources->known(s, a.decimation, within ? source_index : -1)) {
            sources->add(s);
        }
        return std::llabs((side > 0 ? s.last : s.first) - start);
    }

    /* The separate transmitter rising at @p k within the source's own width from a valley of @p valley at
       @p valley_at, if it is one: recorded, returning how far out it reaches; otherwise -1. */
    long long
    within_rise(long long k, double valley, long long valley_at) {
        if (!symmetric || !(a.near_env(k) >= kMirrorRiseRatio * mirrored(k))) {
            return -1;
        }
        const located h = hump(k, valley, valley_at, true);
        double top = 0;
        for (long long j = h.first; j <= h.last; ++j) {
            top = std::max(top, a.near_env(j));
        }
        return valley <= kWeakCentreRatio * top ? record(h, true) : -1;
    }

    /* The valley rise. It must also climb above the ridge just before its valley and every ridge it has passed since:
       the source's outer sideband lines decay one after another, a transmitter beside it stands above the last of
       them. */
    void
    valley_rises() {
        double peak = 0, valley = 0, ridge = 0, ridge_before = 0, passed = 0, previous = 0;
        long long valley_at = start;
        bool fallen = false, own_rise = false;
        for (long long i = 0, next = 0; i <= a.reach; i = next) {
            next = i + 1;
            const long long k = start + side * i;
            const double e = a.near_env(k);
            if (!fallen) {
                peak = std::max(peak, e);
                fallen = e * kSkirtFallRatio <= peak;
                valley = ridge = ridge_before = passed = previous = e;
                valley_at = k;
                continue;
            }
            ridge = std::max(ridge, e);
            if (e < valley) {
                valley = e;
                valley_at = k;
                ridge_before = ridge;
                ridge = passed = e;
            } else if (e < previous) {
                /* Falling again: the bin before was on or past a ridge this climb must clear. */
                passed = std::max(passed, previous);
            }
            previous = e;
            /* A rise that began within the source's width and is not a separate transmitter stays the source's own
               spectrum (its sideband line's envelope reaching past that width) until it falls back. */
            own_rise = own_rise && e >= a.rise * valley && e > ridge_before && e > passed;
            if (own_rise || !(e >= a.rise * valley && e > ridge_before && e > passed)) {
                continue;
            }
            const long long reached =
                outside(k) ? record(separate(k, valley, valley_at)) : within_rise(k, valley, valley_at);
            if (reached < 0) {
                own_rise = true;
                continue;
            }
            /* Past this hump, the next must fall away from it first. */
            const long long past = std::max(i, reached);
            next = past + 1;
            fallen = false;
            peak = a.near_env(start + side * past);
        }
    }

    /* What the mirror does not explain, where at least half the power is that remainder: it must peak inside that
       region (a skirt merely taller than its mirror, or mirrored about a slightly wrong centre, leaves more and more
       toward the source's own edge). The envelope itself must also rise to that peak from a dip: a transmitter on the
       skirt bumps it, while a lopsided transmitter's own skirt only falls, however much taller than its mirror. Only up
       to the first transmitter other than the source: beyond it, what the source's mirror does not explain is that
       transmitter's own skirt, and the valley rises find any other. */
    bool
    bumps(long long top) const {
        double dip = a.near_env(top);
        for (long long j = start; (top - j) * side > 0; j += side) {
            dip = std::min(dip, a.near_env(j));
        }
        return a.near_env(top) > dip;
    }

    /* The top of what the mirror does not explain, climbed from @p k within the region where at least half the power
       is that remainder. */
    long long
    excess_top(long long k) const {
        auto unexplained = [&](long long j) {
            return outside(j) && a.near_env(j) >= 2 * mirrored(j) && std::llabs(j - k) <= a.reach;
        };
        long long top = k;
        while (unexplained(top + 1) && remainder(top + 1, 0) > remainder(top, 0)) {
            ++top;
        }
        while (unexplained(top - 1) && remainder(top - 1, 0) > remainder(top, 0)) {
            --top;
        }
        return top;
    }

    void
    mirror_excess() {
        for (long long i = a.half + 1, next = 0; i <= a.reach; i = next) {
            next = i + 1;
            const long long k = start + side * i;
            if (!outside(k) || !(a.near_env(k) >= kMirrorRiseRatio * mirrored(k))) {
                continue;
            }
            const long long top = excess_top(k);
            if (remainder(top + 1, 0) <= remainder(top, 0) && remainder(top - 1, 0) <= remainder(top, 0)
                && bumps(top)) {
                const located h = separate(top, 0, start);
                const int match = sources->find(h, a.decimation);
                if (match == source_index) {
                    /* The source's own edge: resume past it. */
                    next = std::max(i, std::llabs((side > 0 ? h.last : h.first) - start)) + 1;
                    continue;
                }
                if (match < 0) {
                    sources->add(h);
                }
                return;
            }
        }
    }
};

/* The separate transmitters on either side of @p source (with @p symmetry, item @p source_index of @p sources), into
   @p sources. */
void
add_neighbours(const analysis& a, const located& source, double symmetry, int source_index, source_list* sources) {
    const double centre = a.to_coarse(source.centre);
    for (int side = -1; side <= 1; side += 2) {
        beside search{
            a, source, sources, centre, std::llround(centre), side, source_index, false, symmetry >= kMirrorSymmetry};
        search.mirror = search.symmetric && search.clean_far_side();
        search.valley_rises();
        if (search.mirror) {
            search.mirror_excess();
        }
    }
}

/* The candidate's grid channel, [@p low, @p high) in FFT bins: half a step either side of its exact position on the
   range's rational grid, so neighbouring channels share boundaries however the grid frequencies were rounded; or of its
   own frequency when it is not one of the range's grid frequencies. */
void
channel_bounds(const dsd_signal_measure_request& r, double fine_bin_hz, uint32_t frequency, double* low, double* high) {
    const double step = (double)r.range.step_num / r.range.step_den;
    double exact = frequency;
    if (frequency >= r.range.start_hz) {
        const uint64_t index =
            ((uint64_t)(frequency - r.range.start_hz) * r.range.step_den + r.range.step_num / 2) / r.range.step_num;
        const uint64_t grid = r.range.start_hz + (index * r.range.step_num + r.range.step_den / 2) / r.range.step_den;
        if (grid == frequency) {
            exact = r.range.start_hz + (double)index * step;
        }
    }
    const double offset = (double)r.capture.rotation_hz - r.capture.capture_hz;
    *low = (exact - step / 2 + offset) / fine_bin_hz;
    *high = (exact + step / 2 + offset) / fine_bin_hz;
}

/* Builds the analysis of @p first (plus @p second) power over @p frames transforms into the detector's scratch, over
   the spectrum around the channels @p seeds marks. Every walk stays within this of its channel: its window, an anchor
   kClimbReaches away, a segment and its lobes a few reaches about that, and from the source's centre a reach out, then
   a reach of climb and a reach of hump (or their mirror images), or a separate transmitter's anchor a reach on with
   its own segment and lobes about it. */
analysis
build_analysis(dsd_signal_detector* d, const double* first, const double* second, unsigned frames, const bool* seeds) {
    const auto& request = d->request;
    const double fine_bin_hz = (double)request.capture.rate_hz / d->n;
    const double width = request.range.bandwidth_hz;
    int decimation = 1;
    while (std::floor(width / 2 / (fine_bin_hz * decimation)) > kAnalysisHalfBins && d->n / decimation > 64) {
        decimation *= 2;
    }
    analysis a{};
    a.first = first;
    a.second = second;
    a.fine_n = d->n;
    a.n = d->n / decimation;
    a.decimation = decimation;
    a.fine_bin_hz = fine_bin_hz;
    const double bin_hz = fine_bin_hz * decimation;
    a.half = std::max(1LL, (long long)std::floor(width / 2 / bin_hz));
    a.reach = (long long)std::ceil(2 * width / bin_hz);
    a.rise = valley_rise_ratio(frames);
    /* Three-bin sums of Hann-windowed bins already reach about two bins either side. */
    const long long spread = std::max(0LL, (long long)std::ceil(kEnvelopeHz / bin_hz - 2));
    const long long near_spread = std::max(0LL, (long long)std::ceil(kNearEnvelopeHz / bin_hz - 2));
    long long lo = LLONG_MAX, hi = LLONG_MIN;
    for (uint32_t i = 0; i < request.count; ++i) {
        if (seeds[i]) {
            const long long position = std::llround(a.position_of(request, request.frequencies[i]));
            lo = std::min(lo, position);
            hi = std::max(hi, position);
        }
    }
    const long long margin = a.half + (kClimbReaches + 9) * a.reach + 2;
    if (lo > hi || hi - lo + 1 + 2 * (margin + spread + 1) >= a.n) {
        lo = 0;
        hi = a.n - 1;
    } else {
        lo -= margin;
        hi += margin;
    }
    for (long long j = lo - spread - 1; j <= hi + spread + 1; ++j) {
        double sum = 0;
        for (int i = 0; i < decimation; ++i) {
            sum += a.fine(j * decimation + i);
        }
        d->analysed[wrap_index(j, a.n)] = sum;
    }
    for (long long j = lo - spread; j <= hi + spread; ++j) {
        d->three[wrap_index(j, a.n)] =
            d->analysed[wrap_index(j - 1, a.n)] + d->analysed[wrap_index(j, a.n)] + d->analysed[wrap_index(j + 1, a.n)];
    }
    a.three = d->three;
    a.envelope = d->three;
    a.near_envelope = d->three;
    if (spread > 0) {
        build_envelope(d->three, a.n, lo, hi, spread, d->envelope);
        a.envelope = d->envelope;
    }
    if (near_spread > 0) {
        build_envelope(d->three, a.n, lo, hi, near_spread, d->near_envelope);
        a.near_envelope = d->near_envelope;
    }
    return a;
}

/* Locates the transmitters around the channels @p seeds marks into the detector's source list: the source dominating
   each one's window, strongest first, then the separate transmitters beside each. */
void
locate_sources(dsd_signal_detector* d, const analysis& a, const bool* seeds) {
    const auto& request = d->request;
    source_list* sources = &d->sources;
    sources->count = 0;
    long long anchors[DSD_SEARCH_MAX_CANDIDATES];
    int anchor_count = 0;
    /* Neighbouring channels on a fine grid share their window's tallest bin; climb from each once. */
    long long climbed_from[16], climbed_to[16];
    std::fill(climbed_from, climbed_from + 16, LLONG_MIN);
    for (uint32_t i = 0; i < request.count; ++i) {
        if (!seeds[i]) {
            continue;
        }
        const double position = a.position_of(request, request.frequencies[i]);
        long long peak = (long long)std::ceil(position - a.half);
        for (long long b = peak; b <= (long long)std::floor(position + a.half); ++b) {
            if (a.at(b) > a.at(peak)) {
                peak = b;
            }
        }
        const int slot = (int)(peak & 15);
        if (climbed_from[slot] != peak) {
            climbed_from[slot] = peak;
            climbed_to[slot] = anchor_of(a, peak);
        }
        const long long anchor = climbed_to[slot];
        if (anchor != kFarSource && std::find(anchors, anchors + anchor_count, anchor) == anchors + anchor_count) {
            anchors[anchor_count++] = anchor;
        }
    }
    std::sort(anchors, anchors + anchor_count, [&](long long x, long long y) { return a.at(x) > a.at(y); });
    located primaries[DSD_SEARCH_MAX_CANDIDATES];
    double symmetries[DSD_SEARCH_MAX_CANDIDATES];
    int primary_count = 0;
    for (int i = 0; i < anchor_count; ++i) {
        double symmetry = 0;
        const located s = locate_source(a, anchors[i], &symmetry);
        if (!sources->known(s, a.decimation)) {
            sources->add(s);
            primaries[primary_count] = s;
            symmetries[primary_count++] = symmetry;
        }
    }
    /* The primaries are the list's first items, in order. */
    for (int i = 0; i < primary_count; ++i) {
        add_neighbours(a, primaries[i], symmetries[i], i, sources);
    }
}

/* The power of window @p w (0 the first, 1 the second) per transform over analysis bin @p b. */
double
window_power(const dsd_signal_detector* d, const analysis& both, int w, long long b) {
    const double* power = w ? d->power : d->first_power;
    double sum = 0;
    for (long long f = b * both.decimation; f < (b + 1) * both.decimation; ++f) {
        sum += power[wrap_index(f, d->n)];
    }
    return sum / (w ? d->frames : d->first_frames);
}

/* Whether a transmitter located in @p both holds its own power in both windows: within 10 dB of each other over the
   bins it occupies (within 10 dB of its peak), not a neighbour's skirt beside it. */
bool
sustained(const dsd_signal_detector* d, const analysis& both, const located& s) {
    double peak = 0, power[2] = {0, 0};
    for (long long b = s.first; b <= s.last; ++b) {
        peak = std::max(peak, both.at(b));
    }
    for (long long b = s.first; b <= s.last; ++b) {
        if (!(both.at(b) * 10 < peak)) {
            for (int w = 0; w < 2; ++w) {
                power[w] += window_power(d, both, w, b);
            }
        }
    }
    return power[0] * kSustainedRatio >= power[1] && power[1] * kSustainedRatio >= power[0];
}

/* Whether a sustained transmitter located in @p both is centred in [@p low, @p high). */
bool
owns_sustained(const dsd_signal_detector* d, const analysis& both, double low, double high) {
    for (int j = 0; j < d->sources.count; ++j) {
        const located& s = d->sources.items[j];
        if (s.centre >= low && s.centre < high && sustained(d, both, s)) {
            return true;
        }
    }
    return false;
}

/* Clear raw detections that a transmitter centred on another channel explains: its skirt and the window's leakage
   reach a neighbouring channel's window and can pass that channel's threshold on its own. A transmitter belongs to the
   grid channel nearest its centre. That is decided once, on both windows' power, so a transmitter is assigned the same
   way for the whole request; and it must hold its own power in both windows, so energy that one window only owes to
   another transmitter is not sustained. */
void
reject_leakage(dsd_signal_detector* d, const bool* second_raw) {
    const auto& request = d->request;
    bool keep[DSD_SEARCH_MAX_CANDIDATES]{};
    bool any = false;
    for (uint32_t i = 0; i < request.count; ++i) {
        keep[i] = d->first[i] && second_raw[i];
        any = any || keep[i];
    }
    /* A request always has a transform of at least 32 points; nothing to analyse without one. */
    if (any && d->n >= 32) {
        const analysis both = build_analysis(d, d->first_power, d->power, d->first_frames + d->frames, keep);
        locate_sources(d, both, keep);
        for (uint32_t i = 0; i < request.count; ++i) {
            double low = 0, high = 0;
            channel_bounds(request, both.fine_bin_hz, request.frequencies[i], &low, &high);
            keep[i] = keep[i] && owns_sustained(d, both, low, high);
        }
    } else {
        DSD_MEMSET(keep, 0, sizeof(keep));
    }
    DSD_MEMCPY(d->raw, keep, sizeof(keep));
}

constexpr int kSidebandBlocks = 8;

/* The mean power per FFT bin of each of a channel's eight noise sideband blocks, from 1.5 to 3 widths either side of
   @p position (FFT bins, rotation applied), four blocks a side, over prefix sums @p bins. False when a side has fewer
   than four bins. */
template <typename S>
bool
sideband_blocks(const S& bins, double position, double width_bins, double* blocks) {
    for (int side = -1; side <= 1; side += 2) {
        const double inner = position + side * 1.5 * width_bins, outer = position + side * 3 * width_bins;
        const long long lowest = (long long)std::ceil(std::min(inner, outer));
        const long long highest = (long long)std::floor(std::max(inner, outer));
        const long long count = highest - lowest + 1;
        if (count < 4) {
            return false;
        }
        for (int j = 0; j < 4; ++j) {
            const long long from = lowest + count * j / 4, to = lowest + count * (j + 1) / 4 - 1;
            blocks[(side + 1) * 2 + j] = bins(from, to) / (double)(to - from + 1);
        }
    }
    return true;
}

/* Signal search's floor: the mean of the blocks within 6 dB of the third lowest. A block a transmitter raises more
   than that is left out (up to five of the eight); one it raises less still counts, so with up to five raised blocks
   the mean reads at most 2.875 times the third lowest (about 4.6 dB high over a flat floor). Noise alone (a sloping
   floor too) is measured over all of them. Sorts @p blocks. */
double
trimmed_mean_floor(double* blocks) {
    std::sort(blocks, blocks + kSidebandBlocks);
    double quiet = 0;
    int quiet_count = 0;
    for (int j = 0; j < kSidebandBlocks; ++j) {
        if (blocks[j] <= 4 * blocks[2]) {
            quiet += blocks[j];
            ++quiet_count;
        }
    }
    return quiet / quiet_count;
}

/* The local noise per FFT bin beside a channel by @p estimator (a dsd_signal_quiet_floor_estimator); negative when a
   side has too few bins. */
template <typename S>
double
sideband_noise(const S& bins, double position, double width_bins, int estimator) {
    double blocks[kSidebandBlocks];
    if (!sideband_blocks(bins, position, width_bins, blocks)) {
        return -1;
    }
    if (estimator == DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK) {
        return *std::min_element(blocks, blocks + kSidebandBlocks);
    }
    return trimmed_mean_floor(blocks);
}

/* One channel measured in one window's spectrum: its total power and the local noise over the same bins, both in
   linear full-scale units, and whether its geometry lets it be measured at all. */
struct channel_level {
    bool valid;
    double signal, noise;
    /* The sideband floor per FFT bin, scaled as signal and noise, and the channel's bin count. */
    double floor, bins;
};

/* Measure the channel at @p frequency_hz, @p width Hz wide, in the @p n-point spectrum whose prefix sums @p bins reads,
   scaled by @p norm. Valid when the channel and its sidebands, three widths either side, clear the hardware DC guard
   and stay inside 0.45 Fs, with at least one channel bin and four sideband bins a side. Signal search and quiet
   requests measure every channel through this. The window assumes the tap is the whole capture shifted circularly by
   the rotation, as every live capture is. A shifted I/Q replay resampled to another rate keeps only the band around
   its rotated DC, so a channel far from it would read another channel's bins: such a capture is safe only because
   measurement is refused on replay, which callers key on the stream (opts->iq_replay_active / iq_replay_requested),
   never on the decode clock. */
template <typename S>
channel_level
measure_channel(const S& bins, const dsd_signal_capture& capture, int n, double frequency_hz, double width, double norm,
                int estimator) {
    channel_level out{false, 0, 0, 0, 0};
    const double bin_hz = (double)capture.rate_hz / n;
    const double rotation = capture.rotation_hz;
    const double delta = frequency_hz - capture.capture_hz;
    const double dc_guard = std::max(bin_hz * 3, capture.rate_hz / 100.0);
    if (!(std::abs(delta) > dc_guard + 3 * width && std::abs(delta) + 3 * width < capture.rate_hz * 0.45)) {
        return out;
    }
    /* The channel's bins, and the local noise of its sidebands. */
    const long long signal_first = (long long)std::ceil((delta - width / 2 + rotation) / bin_hz);
    const long long signal_last = (long long)std::floor((delta + width / 2 + rotation) / bin_hz);
    const long long signal_bins = signal_last - signal_first + 1;
    const double per_bin = sideband_noise(bins, (delta + rotation) / bin_hz, width / bin_hz, estimator);
    if (signal_bins <= 0 || per_bin < 0) {
        return out;
    }
    out.valid = true;
    out.signal = bins(signal_first, signal_last) * norm;
    out.noise = per_bin * norm * (double)signal_bins;
    out.floor = per_bin * norm;
    out.bins = (double)signal_bins;
    return out;
}

/* Prefix sums of an @p n-bin spectrum @p power into @p prefix (n + 1 long). */
void
prefix_sums(const double* power, int n, double* prefix) {
    prefix[0] = 0;
    for (int i = 0; i < n; ++i) {
        prefix[i + 1] = prefix[i] + power[i];
    }
}

double
power_db(double linear) {
    return 10 * std::log10(std::max(linear, 1e-16));
}
} // namespace

static void
measure_window(dsd_signal_detector* detector, dsd_signal_measure_report* report, bool last) {
    const auto& request = detector->request;
    const int n = detector->n;
    const double norm = 1.0 / (n * detector->energy * detector->frames);
    prefix_sums(detector->power, n, detector->prefix);
    auto bins = [&](long long first, long long last) {
        return circular_sum(detector->prefix, n, first, last - first + 1);
    };
    for (uint32_t c = 0; c < request.count; ++c) {
        auto& out = report->candidates[c];
        detector->raw[c] = false;
        out.frequency_hz = request.frequencies[c];
        const channel_level level =
            measure_channel(bins, request.capture, n, (double)out.frequency_hz, request.range.bandwidth_hz, norm,
                            DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN);
        out.valid = level.valid;
        if (!out.valid) {
            continue;
        }
        out.power_dbfs = (float)power_db(level.signal);
        out.noise_dbfs = (float)power_db(level.noise);
        detector->raw[c] = out.power_dbfs - out.noise_dbfs >= request.range.sensitivity_db
                           && (!request.range.minimum_set || out.power_dbfs >= request.range.minimum_dbfs);
    }
    if (!last) {
        DSD_MEMCPY(detector->first, detector->raw, sizeof(detector->first));
        DSD_MEMCPY(detector->first_power, detector->power, (size_t)n * sizeof(detector->power[0]));
        detector->first_frames = detector->frames;
        return;
    }
    bool second[DSD_SEARCH_MAX_CANDIDATES];
    DSD_MEMCPY(second, detector->raw, sizeof(second));
    reject_leakage(detector, second);
    for (uint32_t c = 0; c < request.count; ++c) {
        report->candidates[c].detected = detector->raw[c];
    }
}

static int
finish_window(dsd_signal_detector* detector, dsd_signal_measure_report* report) {
    if (detector->clipped * 100 > detector->total_samples) {
        return fail_request(detector, report, DSD_SIGNAL_MEASURE_CLIPPED);
    }
    DSD_MEMSET(report, 0, sizeof(*report));
    report->session_id = detector->request.session_id;
    report->request_id = detector->request.request_id;
    report->generation = detector->request.capture.generation;
    report->count = detector->request.count;
    report->sample_count = detector->total_samples;
    const bool last = ++detector->windows == 2;
    measure_window(detector, report, last);
    if (last) {
        detector->armed = false;
        return 1;
    }
    detector->window_samples = 0;
    detector->frames = 0;
    DSD_MEMSET(detector->power, 0, (size_t)detector->n * sizeof(detector->power[0]));
    return 0;
}

/* Windows as many of the @p pairs samples at @p iq as the frame has room for into it, in one pass, counting a pair
   clipped when either component reaches @p clip_level: how many it took, or -1 on a non-finite sample. */
template <typename D>
static long long
fill_frame(D* detector, const float* iq, size_t pairs, const float* window, float clip_level) {
    const size_t start = detector->filled;
    const size_t take = std::min(pairs, (size_t)detector->n - start);
    float* out = detector->samples + 2 * start;
    const float* weights = window + start;
    /* A sample below full scale is finite and unclipped; without a non-finite one, the others are the clipped ones. */
    size_t clipped = 0, wild = 0;
    for (size_t p = 0; p < take; ++p) {
        const float i = iq[2 * p], q = iq[2 * p + 1];
        out[2 * p] = i * weights[p];
        out[2 * p + 1] = q * weights[p];
        if (!(std::abs(i) < clip_level) || !(std::abs(q) < clip_level)) {
            ++clipped;
            wild += (size_t)(!std::isfinite(i) || !std::isfinite(q));
        }
    }
    if (wild) {
        return -1;
    }
    detector->clipped += clipped;
    detector->filled += take;
    detector->total_samples += take;
    return (long long)take;
}

/* Transform a full frame and add its power to the window's: false when the transform cannot run. */
template <typename D>
static bool
accumulate_frame(D* detector) {
    if (!detector->fft.forward(detector->n, detector->samples)) {
        return false;
    }
    for (int b = 0; b < detector->n; ++b) {
        double re = detector->samples[2 * (size_t)b], im = detector->samples[2 * (size_t)b + 1];
        detector->power[b] += re * re + im * im;
    }
    detector->filled = 0;
    ++detector->frames;
    detector->window_samples += (size_t)detector->n;
    return true;
}

/* Feed @p pairs into an armed detector's Hann-windowed frames. A window ends at the first frame boundary at or past
   10 ms; @p finish analyses it and returns nonzero once the request is done. A capture change, a non-finite sample or a
   transform that cannot run retires the request through @p fail. Both the search and the quiet detector measure
   through this. */
template <typename D, typename Finish, typename Fail>
static int
feed_frames(D* detector, const float* iq, size_t pairs, const dsd_signal_capture& capture, Finish finish, Fail fail) {
    const dsd_signal_measure_failure changed = capture_difference(detector->request.capture, capture);
    if (changed != DSD_SIGNAL_MEASURE_OK) {
        return fail(changed);
    }
    /* The source's level, read once for the block: the per-sample test compares against a register. */
    const float clip_level = capture.clip_level > 0.0f ? capture.clip_level : DSD_SIGNAL_CLIP_LEVEL_DEFAULT;
    const float* window = detector->hann.get(detector->n);
    for (size_t p = 0; p < pairs;) {
        const long long took = fill_frame(detector, iq + 2 * p, pairs - p, window, clip_level);
        if (took < 0) {
            return fail(DSD_SIGNAL_MEASURE_NONFINITE);
        }
        p += (size_t)took;
        if (detector->filled != (size_t)detector->n) {
            continue;
        }
        if (!accumulate_frame(detector)) {
            return fail(DSD_SIGNAL_MEASURE_FAILED);
        }
        if (detector->window_samples * 100 < capture.rate_hz) {
            continue;
        }
        int ready = finish();
        if (ready) {
            return ready;
        }
    }
    return 0;
}

extern "C" int
dsd_signal_detector_feed(dsd_signal_detector* detector, const float* iq, size_t pairs,
                         const dsd_signal_capture* capture, dsd_signal_measure_report* report) {
    if (!detector || !detector->armed || !iq || !capture || !report) {
        return 0;
    }
    return feed_frames(
        detector, iq, pairs, *capture, [&] { return finish_window(detector, report); },
        [&](dsd_signal_measure_failure reason) { return fail_request(detector, report, reason); });
}

/* The shipped quiet threshold, floor estimator and floor guard, from tests/io/signal_quiet_calibrate.cpp
   (docs/signal-search.md "Quiet-channel calibration"). The ratio is judged against the capture-wide noise reference,
   so the estimator sets only the floor the guard compares. The guard is 0.5 dB under the lowest floor rise of any
   active trial whose ratio sat within the threshold's margin when the ratio was judged against the sideband floor
   (9.23 dB, a weak NFM 8 kHz channel between +30 dB neighbours), rounded down; noise alone rises at most 2.4 dB.
   Judged against the reference, the threshold's interval is (2.02, 5.08] dB and the threshold is its top, rounded
   down. */
#define DSD_SIGNAL_QUIET_THRESHOLD_DB    5.0f
#define DSD_SIGNAL_QUIET_FLOOR_ESTIMATOR DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN
#define DSD_SIGNAL_QUIET_GUARD_DB        8.7f
/* The capture rates and analog widths the calibration rendered and validated; nothing between them is admitted. Rates:
   the RTL rate planner's (1.024, 1.152, 1.536 Msps for every selectable DSP bandwidth, rtl_sdr_fm.cpp
   rtl_downsample_passes_for_rate_in()), an Airspy's (2.5, 3, 6, 10 MS/s), and the common RTL-SDR rates a SoapySDR device
   may be given. Widths, per kind: the Radio sheet's NFM steps and its AM steps (src/ui/qt/qml/Util.js), each rendered
   as that kind only. */
static const uint32_t kQuietRatesHz[] = {960000,  1024000, 1152000, 1200000, 1280000, 1440000, 1536000, 1600000,
                                         1800000, 1920000, 2048000, 2400000, 2500000, 3000000, 6000000, 10000000};
static const uint32_t kQuietNfmWidthsHz[] = {8000, 11250, 12500, 16000, 20000, 25000};
static const uint32_t kQuietAmWidthsHz[] = {5000, 6000, 8000, 10000, 15000, 20000};

struct dsd_signal_quiet_detector {
    /* Keep aligned buffers first so records never require interior alignment padding. */
    alignas(16) float samples[2 * DSD_SEARCH_MAX_FFT]{};
    dsd_io::HannWindowCache<DSD_SEARCH_MAX_FFT> hann;
    dsd_io::FftSetupCache fft;
    dsd_signal_quiet_request request{};
    dsd_signal_quiet_params params{};
    /* This sub-window's power, and scratch prefix sums of it (one longer than the spectrum; the capture-wide noise
       reference uses it first as its own scratch). */
    double power[DSD_SEARCH_MAX_FFT]{};
    double prefix[DSD_SEARCH_MAX_FFT + 1]{};
    /* The judged classes' widths. */
    double widths[3]{};
    int width_count = 0;
    double energy = 0;
    size_t filled = 0, window_samples = 0, total_samples = 0, clipped = 0;
    unsigned frames = 0, windows = 0;
    int n = 0;
    bool armed = false;
    /* Over the sub-windows so far: whether every class measured was valid, whether any was, and the highest ratio with
       its power and floor (dB). */
    bool all_valid = true, any_valid = false, any_rise = false;
    double max_ratio_db = 0, max_power_db = 0, max_floor_db = 0, max_rise_db = 0;
    /* The result of the request last begun once it finished: 1 complete, -1 failed, 0 not yet. */
    dsd_signal_quiet_result result{};
    int finished = 0;
};

extern "C" dsd_signal_quiet_params
dsd_signal_quiet_params_default(void) {
    dsd_signal_quiet_params params{};
    params.threshold_db = DSD_SIGNAL_QUIET_THRESHOLD_DB;
    params.floor_estimator = DSD_SIGNAL_QUIET_FLOOR_ESTIMATOR;
    params.guard_db = DSD_SIGNAL_QUIET_GUARD_DB;
    return params;
}

extern "C" int
dsd_signal_quiet_rate_supported(uint32_t rate_hz) {
    return std::find(std::begin(kQuietRatesHz), std::end(kQuietRatesHz), rate_hz) != std::end(kQuietRatesHz);
}

extern "C" int
dsd_signal_quiet_analog_supported(int kind, uint32_t width_hz, uint32_t rate_hz) {
    if (!dsd_signal_quiet_rate_supported(rate_hz)) {
        return 0;
    }
    if (kind == DSD_ANALOG_DEMOD_FM) {
        return std::find(std::begin(kQuietNfmWidthsHz), std::end(kQuietNfmWidthsHz), width_hz)
               != std::end(kQuietNfmWidthsHz);
    }
    if (kind == DSD_ANALOG_DEMOD_AM) {
        return std::find(std::begin(kQuietAmWidthsHz), std::end(kQuietAmWidthsHz), width_hz)
               != std::end(kQuietAmWidthsHz);
    }
    return 0;
}

extern "C" dsd_signal_quiet_detector*
dsd_signal_quiet_detector_create(void) {
    return new (std::nothrow) dsd_signal_quiet_detector;
}

extern "C" void
dsd_signal_quiet_detector_destroy(dsd_signal_quiet_detector* detector) {
    if (detector) {
        detector->fft.release();
        delete detector;
    }
}

/* The widths @p request judges, into @p widths: false for an unknown class, no class, or an analog width missing,
   outside DSD_ANALOG_AM_WIDTH_MIN_HZ..DSD_ANALOG_NFM_WIDTH_MAX_HZ, or given without the analog class. */
static bool
quiet_widths(const dsd_signal_quiet_request& request, double* widths, int* count) {
    const unsigned known = DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25 | DSD_SIGNAL_QUIET_CLASS_ANALOG;
    const bool analog = (request.classes & DSD_SIGNAL_QUIET_CLASS_ANALOG) != 0;
    if (!request.classes || (request.classes & ~known) || analog != (request.analog_width_hz != 0)
        || (analog
            && (request.analog_width_hz < DSD_ANALOG_AM_WIDTH_MIN_HZ
                || request.analog_width_hz > DSD_ANALOG_NFM_WIDTH_MAX_HZ))) {
        return false;
    }
    *count = 0;
    if (request.classes & DSD_SIGNAL_QUIET_CLASS_12K5) {
        widths[(*count)++] = DSD_SIGNAL_QUIET_CLASS_12K5_HZ;
    }
    if (request.classes & DSD_SIGNAL_QUIET_CLASS_6K25) {
        widths[(*count)++] = DSD_SIGNAL_QUIET_CLASS_6K25_HZ;
    }
    if (analog) {
        widths[(*count)++] = request.analog_width_hz;
    }
    return true;
}

/* Arm @p detector, whose transform buffers are kept between requests of one size. */
static dsd_signal_request_failure
quiet_arm(dsd_signal_quiet_detector* detector, const dsd_signal_quiet_request* request,
          const dsd_signal_quiet_params& params) {
    double widths[3] = {0, 0, 0};
    int count = 0;
    if (!request || !request->request_id || !std::isfinite(params.threshold_db) || !std::isfinite(params.guard_db)
        || (params.floor_estimator != DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN
            && params.floor_estimator != DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK)
        || !quiet_widths(*request, widths, &count)) {
        return DSD_SIGNAL_REQUEST_UNSUPPORTED;
    }
    const int n = dsd_signal_fft_size_for_width(request->capture.rate_hz, *std::min_element(widths, widths + count));
    const float* window = n ? detector->hann.get(n) : nullptr;
    if (!window) {
        return DSD_SIGNAL_REQUEST_UNSUPPORTED;
    }
    if (!detector->fft.prepare(n)) {
        return DSD_SIGNAL_REQUEST_NO_MEMORY;
    }
    detector->n = n;
    detector->request = *request;
    detector->params = params;
    DSD_MEMCPY(detector->widths, widths, sizeof(widths));
    detector->width_count = count;
    detector->energy = 0;
    for (int i = 0; i < n; ++i) {
        detector->energy += (double)window[i] * window[i];
    }
    detector->filled = detector->window_samples = detector->total_samples = detector->clipped = 0;
    detector->frames = detector->windows = 0;
    detector->all_valid = true;
    detector->any_valid = detector->any_rise = false;
    detector->max_ratio_db = detector->max_power_db = detector->max_floor_db = detector->max_rise_db = 0;
    DSD_MEMSET(detector->power, 0, (size_t)n * sizeof(detector->power[0]));
    detector->armed = true;
    return DSD_SIGNAL_REQUEST_OK;
}

extern "C" int
dsd_signal_quiet_detector_begin(dsd_signal_quiet_detector* detector, const dsd_signal_quiet_request* request,
                                const dsd_signal_quiet_params* params, dsd_signal_request_failure* out_failure) {
    dsd_signal_request_failure why = DSD_SIGNAL_REQUEST_UNSUPPORTED;
    if (detector) {
        detector->armed = false;
        detector->finished = 0;
        why = quiet_arm(detector, request, params ? *params : dsd_signal_quiet_params_default());
    }
    if (out_failure) {
        *out_failure = why;
    }
    return why == DSD_SIGNAL_REQUEST_OK ? 0 : -1;
}

/* Finish the request with @p reason: its id and capture, neither valid nor quiet. */
static int
quiet_finish(dsd_signal_quiet_detector* detector, dsd_signal_measure_failure reason) {
    detector->armed = false;
    detector->result = dsd_signal_quiet_result{};
    detector->result.request_id = detector->request.request_id;
    detector->result.capture = detector->request.capture;
    detector->result.failure = reason;
    detector->finished = reason == DSD_SIGNAL_MEASURE_OK ? 1 : -1;
    return detector->finished;
}

/* The capture-wide noise reference per FFT bin, in the units of the sub-window's power (summed over its transforms):
   the 25th percentile of the bins whose hardware offset (the bin's frequency less the ingest rotation, folded into
   -Fs/2..Fs/2 as measure_channel() reads its bins) clears the hardware DC guard, stays inside 0.45 Fs and lies outside
   the channel's widest class, scaled to the mean that percentile implies for Gaussian noise (a chi-square of
   2 * frames degrees of freedom, Wilson-Hilferty). A low percentile of the whole span stays on the noise however many
   transmitters stand in up to three quarters of it. @p scratch (n doubles) takes the copies the selection reorders.
   Negative when no bin qualifies. */
static double
wide_reference(const dsd_signal_quiet_detector* detector, double* scratch) {
    const dsd_signal_capture& capture = detector->request.capture;
    const int n = detector->n;
    const double rate = capture.rate_hz, bin_hz = rate / n;
    const double dc_guard = std::max(bin_hz * 3, rate / 100.0);
    const double channel = (double)detector->request.frequency_hz - capture.capture_hz;
    const double half_width = *std::max_element(detector->widths, detector->widths + detector->width_count) / 2;
    int count = 0;
    for (int m = -n / 2; m < n / 2; ++m) {
        double delta = m * bin_hz - capture.rotation_hz;
        delta -= rate * std::floor((delta + rate / 2) / rate);
        if (std::abs(delta) > dc_guard && std::abs(delta) < rate * 0.45 && std::abs(delta - channel) > half_width) {
            scratch[count++] = detector->power[(m + n) % n];
        }
    }
    if (count == 0) {
        return -1;
    }
    double* rank = scratch + count / 4;
    std::nth_element(scratch, rank, scratch + count);
    constexpr double kLowerQuartileZ = -0.6744897501960817;
    const double k = 2.0 * detector->frames, wh = 2.0 / (9.0 * k);
    return *rank / std::pow(1.0 - wh + kLowerQuartileZ * std::sqrt(wh), 3);
}

/* Measure one sub-window in every judged class, keeping the highest ratio and floor rise over the request. The ratio
   is the channel's power over the capture-wide noise reference across the same bins, so transmitters beside the
   channel that lift its sideband floor do not lower it; the sideband floor serves only the guard, and a floor over the
   guard leaves the request invalid. */
static void
quiet_measure_window(dsd_signal_quiet_detector* detector) {
    const int n = detector->n;
    const double norm = 1.0 / (n * detector->energy * detector->frames);
    const double reference = wide_reference(detector, detector->prefix);
    prefix_sums(detector->power, n, detector->prefix);
    auto bins = [&](long long first, long long last) {
        return circular_sum(detector->prefix, n, first, last - first + 1);
    };
    for (int c = 0; c < detector->width_count; ++c) {
        const channel_level level =
            measure_channel(bins, detector->request.capture, n, (double)detector->request.frequency_hz,
                            detector->widths[c], norm, detector->params.floor_estimator);
        if (!level.valid || reference <= 0) {
            detector->all_valid = false;
            continue;
        }
        const double rise = power_db(level.floor) - power_db(reference * norm);
        if (!detector->any_rise || rise > detector->max_rise_db) {
            detector->max_rise_db = rise;
        }
        detector->any_rise = true;
        if (rise > detector->params.guard_db) {
            detector->all_valid = false;
        }
        const double power = power_db(level.signal), floor = power_db(reference * norm * level.bins);
        if (!detector->any_valid || power - floor > detector->max_ratio_db) {
            detector->max_ratio_db = power - floor;
            detector->max_power_db = power;
            detector->max_floor_db = floor;
        }
        detector->any_valid = true;
    }
}

static int
quiet_finish_window(dsd_signal_quiet_detector* detector) {
    if (detector->clipped * 100 > detector->total_samples) {
        return quiet_finish(detector, DSD_SIGNAL_MEASURE_CLIPPED);
    }
    quiet_measure_window(detector);
    if (++detector->windows < DSD_SIGNAL_QUIET_WINDOWS) {
        detector->window_samples = 0;
        detector->frames = 0;
        DSD_MEMSET(detector->power, 0, (size_t)detector->n * sizeof(detector->power[0]));
        return 0;
    }
    quiet_finish(detector, DSD_SIGNAL_MEASURE_OK);
    dsd_signal_quiet_result& result = detector->result;
    result.valid = detector->all_valid && detector->any_valid;
    result.max_ratio_db = (float)detector->max_ratio_db;
    result.power_dbfs = (float)detector->max_power_db;
    result.floor_dbfs = (float)detector->max_floor_db;
    result.floor_rise_db = (float)detector->max_rise_db;
    result.quiet = result.valid && result.max_ratio_db < detector->params.threshold_db;
    return 1;
}

extern "C" int
dsd_signal_quiet_detector_feed(dsd_signal_quiet_detector* detector, const float* iq, size_t pairs,
                               const dsd_signal_capture* capture) {
    if (!detector || !detector->armed || !iq || !capture) {
        return 0;
    }
    return feed_frames(
        detector, iq, pairs, *capture, [&] { return quiet_finish_window(detector); },
        [&](dsd_signal_measure_failure reason) { return quiet_finish(detector, reason); });
}

extern "C" int
dsd_signal_quiet_detector_result(const dsd_signal_quiet_detector* detector, dsd_signal_quiet_result* result) {
    if (!detector || !result || !detector->finished) {
        return 0;
    }
    *result = detector->result;
    return detector->finished;
}

namespace {
/* The shared tap. feed_mutex serialises the detectors: the demod thread holds it while it transforms and analyses, and
   requests and cancels take it to reset or disarm a detector. tap_mutex guards only what is published (the capture,
   the time of its latest samples, readiness, the finished report and quiet result, and the status word's writes), so
   polling never waits for analysis. Locks are always taken feed first, then tap.

   The tap holds one request: Signal search's (tap_detector) or a scan quiet request (tap_quiet). The status word is
   ticket << 2 | quiet owner << 1 | done. The 30-bit ticket moves at every request (search or quiet), cancel and reset,
   never to 0, so an earlier ticket compares unequal at once. It is written only under tap_mutex, the done bit with
   release after the quiet result, and read with acquire without a lock. While tap_quiet is armed the word names its
   ticket as the quiet owner, not done: everything that moves the word away disarms it first under feed_mutex. */
std::mutex feed_mutex;
std::mutex tap_mutex;
dsd_signal_detector* tap_detector = nullptr;
dsd_signal_quiet_detector* tap_quiet = nullptr; /* under feed_mutex */
dsd_signal_measure_report feed_report{};
dsd_signal_capture tap_capture{};
dsd_signal_measure_report tap_report{};
dsd_signal_quiet_result tap_quiet_result{};
int tap_ready = 0;
uint64_t tap_last_ms = 0;
std::atomic<uint32_t> tap_status{0};
std::atomic<int> bypass{0};
std::atomic<uint64_t> search_owner{0};
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
int test_fail_allocation = 0; /* under feed_mutex */
#endif

/* std::atomic<uint32_t>::is_always_lock_free is C++17; on every target uint32_t is an unsigned int. */
static_assert(ATOMIC_INT_LOCK_FREE == 2 && sizeof(uint32_t) == sizeof(unsigned int),
              "the quiet status word must be a lock-free 32-bit atomic");
constexpr uint32_t kTicketMask = 0x3FFFFFFFu;
constexpr uint32_t kStatusQuiet = 2u;
constexpr uint32_t kStatusDone = 1u;

dsd_signal_detector*
tap_detector_create() {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (test_fail_allocation) {
        return nullptr;
    }
#endif
    return dsd_signal_detector_create();
}

dsd_signal_quiet_detector*
tap_quiet_create() {
#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
    if (test_fail_allocation) {
        return nullptr;
    }
#endif
    return dsd_signal_quiet_detector_create();
}

/* Under feed_mutex: drop any quiet measurement in progress. */
void
disarm_quiet() {
    if (tap_quiet) {
        tap_quiet->armed = false;
    }
}

/* Under both locks: move the tap to its next ticket, held by a quiet request or by no quiet owner. */
uint32_t
advance_ticket(bool quiet) {
    uint32_t next = ((tap_status.load(std::memory_order_relaxed) >> 2) + 1) & kTicketMask;
    if (!next) {
        next = 1;
    }
    tap_status.store(next << 2 | (quiet ? kStatusQuiet : 0u), std::memory_order_release);
    return next;
}

/* Under feed_mutex: whether Signal search owns the tuner, or holds a request or a report not yet read. */
bool
search_holds_tap() {
    if (search_owner.load(std::memory_order_acquire) != 0 || (tap_detector && tap_detector->armed)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(tap_mutex);
    return tap_ready != 0;
}

/* Under feed_mutex, after tap_quiet finished: publish its result, then the done bit. */
void
publish_quiet(uint64_t received) {
    std::lock_guard<std::mutex> lock(tap_mutex);
    const uint32_t word = tap_status.load(std::memory_order_relaxed);
    if ((word & (kStatusQuiet | kStatusDone)) != kStatusQuiet) {
        return;
    }
    (void)dsd_signal_quiet_detector_result(tap_quiet, &tap_quiet_result);
    tap_quiet_result.completed_realtime_ns = received;
    tap_status.store(word | kStatusDone, std::memory_order_release);
}
} // namespace

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" void
rtl_signal_measure_test_fail_allocation(int fail) {
    std::lock_guard<std::mutex> feed(feed_mutex);
    test_fail_allocation = fail;
    if (fail) {
        dsd_signal_detector_destroy(tap_detector);
        tap_detector = nullptr;
        dsd_signal_quiet_detector_destroy(tap_quiet);
        tap_quiet = nullptr;
        std::lock_guard<std::mutex> lock(tap_mutex);
        advance_ticket(false);
    }
}

extern "C" void
rtl_signal_measure_test_set_ticket(uint32_t ticket) {
    std::lock_guard<std::mutex> feed(feed_mutex);
    disarm_quiet();
    std::lock_guard<std::mutex> lock(tap_mutex);
    tap_status.store((ticket & kTicketMask) << 2, std::memory_order_release);
}
#endif

extern "C" int
rtl_signal_measure_request(const dsd_signal_measure_request* request, dsd_signal_request_failure* out_failure) {
    std::lock_guard<std::mutex> feed(feed_mutex);
    if (!tap_detector) {
        tap_detector = tap_detector_create();
    }
    const int rc = dsd_signal_detector_begin(tap_detector, request);
    if (out_failure) {
        *out_failure = DSD_SIGNAL_REQUEST_OK;
        if (rc != 0) {
            /* Without a detector the plan was never checked: the failure is the allocation's. */
            *out_failure = tap_detector ? DSD_SIGNAL_REQUEST_UNSUPPORTED : DSD_SIGNAL_REQUEST_NO_MEMORY;
        }
    }
    disarm_quiet();
    std::lock_guard<std::mutex> lock(tap_mutex);
    tap_ready = 0;
    advance_ticket(false);
    return rc;
}

extern "C" void
rtl_signal_measure_feed(const float* iq, size_t pairs, const dsd_signal_capture* capture) {
    const uint64_t received = dsd_realtime_mono_ns();
    std::lock_guard<std::mutex> feed(feed_mutex);
    {
        std::lock_guard<std::mutex> lock(tap_mutex);
        /* A completed report must not publish power measured on a capture that has since changed: it keeps its ids
           and becomes that request's failure. */
        if (!same_capture(tap_capture, *capture) && tap_ready == 1) {
            tap_ready = -1;
            tap_report.count = 0;
            DSD_MEMSET(tap_report.candidates, 0, sizeof(tap_report.candidates));
            tap_report.failure = DSD_SIGNAL_MEASURE_CAPTURE_CHANGED;
        }
        tap_capture = *capture;
        tap_last_ms = dsd_realtime_mono_ms();
    }
    const int rc = dsd_signal_detector_feed(tap_detector, iq, pairs, capture, &feed_report);
    if (rc) {
        std::lock_guard<std::mutex> lock(tap_mutex);
        /* A failure already waiting keeps its reason: the first one is why the request failed. */
        if (tap_ready != -1) {
            tap_report = feed_report;
            tap_report.received_realtime_ns = received;
            tap_ready = rc;
        }
    }
    if (dsd_signal_quiet_detector_feed(tap_quiet, iq, pairs, capture)) {
        publish_quiet(received);
    }
}

extern "C" int
rtl_signal_measure_get(dsd_signal_measure_report* report) {
    if (!report) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(tap_mutex);
    int rc = tap_ready;
    if (rc != 0) {
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
    std::lock_guard<std::mutex> feed(feed_mutex);
    if (tap_detector) {
        tap_detector->armed = false;
    }
    std::lock_guard<std::mutex> lock(tap_mutex);
    tap_ready = 0;
    /* A search cancel never touches a quiet request. */
    if (!(tap_status.load(std::memory_order_relaxed) & kStatusQuiet)) {
        advance_ticket(false);
    }
}

extern "C" void
rtl_signal_measure_reset(void) {
    std::lock_guard<std::mutex> feed(feed_mutex);
    if (tap_detector) {
        tap_detector->armed = false;
    }
    disarm_quiet();
    std::lock_guard<std::mutex> lock(tap_mutex);
    tap_ready = 0;
    tap_capture = dsd_signal_capture{};
    tap_last_ms = 0;
    advance_ticket(false);
}

extern "C" int
rtl_signal_quiet_request(const dsd_signal_quiet_request* request, uint32_t* ticket,
                         dsd_signal_request_failure* out_failure) {
    std::lock_guard<std::mutex> feed(feed_mutex);
    dsd_signal_request_failure why = DSD_SIGNAL_REQUEST_UNSUPPORTED;
    int rc = -1;
    /* Signal search's request, report or tuner is never taken from it. */
    if (ticket && !search_holds_tap()) {
        if (!tap_quiet) {
            tap_quiet = tap_quiet_create();
        }
        why = DSD_SIGNAL_REQUEST_NO_MEMORY;
        rc = tap_quiet ? dsd_signal_quiet_detector_begin(tap_quiet, request, nullptr, &why) : -1;
        std::lock_guard<std::mutex> lock(tap_mutex);
        const uint32_t next = advance_ticket(rc == 0);
        if (rc == 0) {
            *ticket = next;
        }
    }
    if (out_failure) {
        *out_failure = why;
    }
    return rc;
}

extern "C" int
rtl_signal_quiet_status(uint32_t ticket) {
    const uint32_t word = tap_status.load(std::memory_order_acquire);
    if (!ticket || ticket > kTicketMask || (word >> 2) != ticket || !(word & kStatusQuiet)) {
        return -1;
    }
    return (word & kStatusDone) ? 1 : 0;
}

extern "C" int
rtl_signal_quiet_take(uint32_t ticket, dsd_signal_quiet_result* result) {
    if (!result) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(tap_mutex);
    const int state = rtl_signal_quiet_status(ticket);
    if (state == 0) {
        return 0;
    }
    if (state < 0) {
        *result = dsd_signal_quiet_result{};
        result->failure = DSD_SIGNAL_MEASURE_SUPERSEDED;
        return -1;
    }
    *result = tap_quiet_result;
    /* Taken: the ticket no longer names a quiet request. */
    tap_status.store(ticket << 2, std::memory_order_release);
    return result->failure == DSD_SIGNAL_MEASURE_OK ? 1 : -1;
}

extern "C" void
rtl_signal_quiet_cancel(uint32_t ticket) {
    std::lock_guard<std::mutex> feed(feed_mutex);
    std::lock_guard<std::mutex> lock(tap_mutex);
    if (rtl_signal_quiet_status(ticket) < 0) {
        return;
    }
    disarm_quiet();
    advance_ticket(false);
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
