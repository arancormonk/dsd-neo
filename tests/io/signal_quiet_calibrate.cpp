// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Calibration driver for the scan skip-silent check's quiet threshold (issue #834), deliberately not a CTest: it runs
 * the stand-alone quiet detector, the one the shared tap runs, over hundreds of rendered conditions and thousands of
 * trials each, minutes on every core. docs/signal-search.md "Quiet-channel calibration" gives the method and results.
 *
 *   dsd-neo_test_signal_quiet_calibrate --phase select|validate|snr|cost --out FILE [--sources DIR] [--trials N]
 *       [--jobs N] [--threshold DB --estimator trimmed|lowest] [--guard DB] [--filter TEXT] [--dump FILE]
 *       [--neighbour-levels DB,DB,...]
 *
 * DIR holds the clean 48 kHz sources tools/scan_quiet_reception.py --export-sources writes, from the same generators
 * that measured each mode's reception threshold. Each is interpolated to the capture rate in the frequency domain, so
 * its spectrum (and the power band_power() gave inside its class width) is unchanged, then placed at the channel in the
 * RTL tap's geometry (tuned Fs/4 above it, the ingest rotation bringing it to the samples' 0 Hz). Every phase but cost
 * needs DIR. The
 * S/N is the tool's: the signal's power inside the mode's class width (for a burst train, during its bursts) over the
 * noise power inside the same width around the signal.
 *
 * select    every condition, both floor estimators on the same samples: per condition the lowest, percentiles and
 *           highest max-of-9 ratio over its valid trials (a noise-only condition's invalid trials count as busy, at
 *           +999 dB), and the sideband floor's rise over the capture-wide reference (--guard, default off; --dump
 *           writes every trial's ratio and rise). The ratio is judged against that reference, so both estimators read
 *           the same ratio; the estimator sets only the floor the guard compares.
 * validate  every condition at one threshold, estimator and guard, fresh seeds: quiet and busy verdicts (an invalid
 *           result is busy)
 * snr       the rendered S/N of each wanted signal measured back from the samples, against the target
 * cost      CPU per quiet request on noise (shipped parameters), and per request of the floor guard's reference alone
 *           (the same bin selection and quartile, nine times)
 *
 * --neighbour-levels sets the adjacent-channel transmitters' levels over the wanted signal (default 30,50, the levels
 * the first calibration used; the shipped calibration ran 5 to 30 dB in 2.5 dB steps, then 35 to 50 dB in 5 dB
 * steps). A level's condition names, and so its seeds, do not depend on the other levels in the list.
 *
 * Every trial's samples come from its own seed (the condition's name, the phase and the trial number), so results do
 * not depend on --jobs. */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "rtl_fft_cache.h"
#include "rtl_finite.h"

namespace {

using cf = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kSourceRate = 48000;
constexpr uint32_t kChannelHz = 450000000;
/* Every capture rate a quiet request can arrive at and the check admits (dsd_signal_quiet_rate_supported()): the RTL
 * rate planner's 1.024, 1.152 and 1.536 Msps, an Airspy's 2.5, 3, 6 and 10 MS/s, and the common RTL-SDR rates a
 * SoapySDR device may be given. */
constexpr uint32_t kRates[] = {960000,  1024000, 1152000, 1200000, 1280000, 1440000, 1536000, 1600000,
                               1800000, 1920000, 2048000, 2400000, 2500000, 3000000, 6000000, 10000000};
constexpr uint32_t kSceneRate = 1536000;    /* neighbour, slope and broad-interferer scenes */
constexpr double kNoiseSigma = 1e-3;        /* per component; keeps a +50 dB neighbour far below the clip level */
constexpr double kMarginDb = 1.0;           /* active trials sit this far below each mode's reception threshold */
constexpr int kBlockIn = 9600;              /* interpolation block at 48 kHz: 200 ms, its middle half kept */
constexpr double kTrialSeconds = 0.128;     /* coloured-noise transform: longer than any nine sub-windows */
constexpr double kBurstGapSeconds = 0.0325; /* DMR MS: 60 ms frame less the 27.5 ms symbol transmission period */

/* ------------------------------------------------------------------------------------------------ modes */

struct Mode {
    const char* name;
    const char* row;     /* the source row that set the reception threshold */
    double reception_db; /* S/N in the class width; DMR MS: burst-on */
    uint32_t width_hz;
    uint8_t cls;
    bool analog;
};

/* Reception thresholds: the "kept" thresholds tools/scan_quiet_reception.py writes to its thresholds.json, one run per
 * mode and per NFM and AM width the Radio sheet offers, as the reception table in docs/signal-search.md ("Reception
 * thresholds") lists them. X2-TDMA and ProVoice are unsourced: no check, so not calibrated. */
const Mode kModes[] = {
    {"p25p1", "p25p1", 8.0, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"p25p2", "p25p2_harmonized", 9.5, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"dmr_bs", "dmr_bs", 7.5, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"dmr_ms", "dmr_ms", 7.5, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"nxdn96", "nxdn96", 10.0, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"ysf", "ysf_2700", 9.0, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"m17", "m17", 6.0, 12500, DSD_SIGNAL_QUIET_CLASS_12K5, false},
    {"nxdn48", "nxdn48", 11.0, 6250, DSD_SIGNAL_QUIET_CLASS_6K25, false},
    {"dpmr", "dpmr", 8.0, 6250, DSD_SIGNAL_QUIET_CLASS_6K25, false},
    {"dstar", "dstar_bt0.5", 12.0, 6250, DSD_SIGNAL_QUIET_CLASS_6K25, false},
    {"nfm_8k", "nfm_8k", 10.5, 8000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"nfm_11k25", "nfm_11k25_wide", 6.5, 11250, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"nfm_12k5", "nfm_12k5", 9.5, 12500, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"nfm_16k", "nfm_16k_wide", 6.0, 16000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"nfm_20k", "nfm_20k_wide", 6.0, 20000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"nfm_25k", "nfm_25k", 6.0, 25000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"am_5k", "am_5k", 23.0, 5000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"am_6k", "am_6k", 22.5, 6000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"am_8k", "am_8k", 21.5, 8000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"am_10k", "am_10k", 21.0, 10000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"am_15k", "am_15k", 19.0, 15000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
    {"am_20k", "am_20k", 18.0, 20000, DSD_SIGNAL_QUIET_CLASS_ANALOG, true},
};
constexpr const char* kNeighbourRow = "p25p1"; /* a C4FM transmitter on the adjacent channel */
constexpr const char* kOffsetCarrierRow = "am_20k";

/* ------------------------------------------------------------------------------------------------ sources */

struct Source {
    std::string row;
    double band_power = 0; /* tools/scan_quiet_reception.py band_power() in the row's class width */
    std::vector<cf> base;  /* 48 kHz */
    std::vector<uint8_t> on;
};

/* A source at one capture rate, with its bursts' symbol transmission periods in capture samples. */
struct Placed {
    std::vector<cf> x;
    std::vector<std::pair<int64_t, int64_t>> bursts;
};

bool
read_file(const std::string& path, std::vector<char>* out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool
parse_double(const std::string& text, double* out) {
    char* end = nullptr;
    const double v = std::strtod(text.c_str(), &end);
    /* std::isfinite() folds away under fast-math; rtl_is_finite() is built with IEEE semantics. */
    if (end == text.c_str() || *end != '\0' || !dsd::io::radio::rtl_is_finite(v)) {
        return false;
    }
    *out = v;
    return true;
}

bool
parse_count(const char* text, long lo, long hi, long* out) {
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = v;
    return true;
}

std::string
source_path(const std::string& dir, const std::string& row, const char* suffix) {
    std::string path = dir;
    path += '/';
    path += row;
    path += suffix;
    return path;
}

/* Load the sources.tsv rows this driver uses. */
bool
load_sources(const std::string& dir, std::vector<Source>* sources) {
    std::ifstream tsv(dir + "/sources.tsv");
    std::string line;
    if (!tsv || !std::getline(tsv, line)) {
        DSD_FPRINTF(stderr, "cannot read %s/sources.tsv\n", dir.c_str());
        return false;
    }
    while (std::getline(tsv, line)) {
        std::istringstream fields(line);
        std::string row, mode, width, rate, samples, power, burst;
        std::getline(fields, row, '\t');
        std::getline(fields, mode, '\t');
        std::getline(fields, width, '\t');
        std::getline(fields, rate, '\t');
        std::getline(fields, samples, '\t');
        std::getline(fields, power, '\t');
        std::getline(fields, burst, '\t');
        Source s;
        s.row = row;
        double rate_value = 0;
        if (!parse_double(power, &s.band_power) || !parse_double(rate, &rate_value)
            || std::fabs(rate_value - kSourceRate) > 0.5) {
            DSD_FPRINTF(stderr, "sources.tsv: bad row %s\n", row.c_str());
            return false;
        }
        std::vector<char> raw;
        if (!read_file(source_path(dir, row, ".cf32"), &raw) || raw.size() % sizeof(cf)) {
            DSD_FPRINTF(stderr, "cannot read %s.cf32\n", row.c_str());
            return false;
        }
        s.base.resize(raw.size() / sizeof(cf));
        DSD_MEMCPY(s.base.data(), raw.data(), raw.size());
        if (burst == "1") {
            std::vector<char> on;
            if (!read_file(source_path(dir, row, ".on"), &on) || on.size() != s.base.size()) {
                DSD_FPRINTF(stderr, "cannot read %s.on\n", row.c_str());
                return false;
            }
            s.on.assign(on.begin(), on.end());
        }
        sources->push_back(std::move(s));
    }
    return true;
}

int
find_source(const std::vector<Source>& sources, const char* row) {
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].row == row) {
            return (int)i;
        }
    }
    return -1;
}

/* Inverse transform through the forward one: ifft(X) = conj(fft(conj(X))), unscaled. */
void
inverse(dsd_io::FftSetupCache& fft, int n, cf* data) {
    for (int i = 0; i < n; ++i) {
        data[i] = std::conj(data[i]);
    }
    fft.forward(n, reinterpret_cast<float*>(data));
    for (int i = 0; i < n; ++i) {
        data[i] = std::conj(data[i]);
    }
}

struct AlignedBuffer {
    std::unique_ptr<cf[]> data;

    explicit AlignedBuffer(size_t n) : data(new cf[n]) {}
};

/* Band-limited interpolation of a 48 kHz source to @p rate: 200 ms blocks every 100 ms, each zero-padded in the
 * frequency domain, keeping each block's middle 100 ms (the source is read circularly; trials stay clear of the wrap).
 * The 48 kHz spectrum and so each band's power are kept; nothing appears above 24 kHz. */
Placed
place_source(const Source& s, uint32_t rate) {
    const int n_in = kBlockIn, n_out = (int)((int64_t)kBlockIn * rate / kSourceRate), hop_in = n_in / 2;
    const int hop_out = n_out / 2;
    const long long total_in = (long long)s.base.size();
    Placed p;
    p.x.assign((size_t)(total_in / hop_in) * (size_t)hop_out, cf(0, 0));
    dsd_io::FftSetupCache small, large;
    AlignedBuffer in((size_t)n_in), out((size_t)n_out);
    for (long long b = 0; b < total_in / hop_in; ++b) {
        for (int i = 0; i < n_in; ++i) {
            const long long k = ((b * hop_in - hop_in / 2 + i) % total_in + total_in) % total_in;
            in.data[i] = s.base[(size_t)k];
        }
        small.forward(n_in, reinterpret_cast<float*>(in.data.get()));
        std::fill(out.data.get(), out.data.get() + n_out, cf(0, 0));
        for (int k = 0; k < n_in / 2; ++k) {
            out.data[k] = in.data[k];
        }
        for (int k = n_in / 2 + 1; k < n_in; ++k) {
            out.data[n_out - n_in + k] = in.data[k];
        }
        inverse(large, n_out, out.data.get());
        const float scale = 1.0f / (float)n_in;
        for (int i = 0; i < hop_out; ++i) {
            p.x[(size_t)(b * hop_out + i)] = out.data[hop_out / 2 + i] * scale;
        }
    }
    small.release();
    large.release();
    /* Burst periods: the 48 kHz mask's runs, mapped to the capture rate. */
    for (size_t i = 0; i < s.on.size();) {
        if (!s.on[i]) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < s.on.size() && s.on[j]) {
            ++j;
        }
        p.bursts.emplace_back((int64_t)i * rate / kSourceRate, (int64_t)j * rate / kSourceRate);
        i = j;
    }
    return p;
}

/* ------------------------------------------------------------------------------------------------ conditions */

enum Kind { ACTIVE, NOISE, INFO };

enum Floor { WHITE, SLOPE, HUMP };

struct Carrier {
    int source;
    double offset_hz;  /* from the channel */
    double share;      /* of the wanted power (carriers of one scene share it) */
    double rel_db = 0; /* a neighbour: its power over the wanted power */
    bool neighbour = false;
    bool droop = false; /* DMR MS: region B power at the mask limits */
};

struct Condition {
    std::string name, group;
    Kind kind = ACTIVE;
    uint32_t rate = 0;
    int grid_shift_hz = 0; /* channel position against the FFT bin grid */
    uint8_t classes = 0;
    uint32_t analog_width = 0;
    double snr_db = 0;
    uint32_t snr_width = 0;
    double snr_offset_hz = 0; /* where the S/N's width is centred, from the channel */
    std::vector<Carrier> carriers;
    Floor floor = WHITE;
    double floor_a = 0, floor_b = 0; /* SLOPE: dB per kHz; HUMP: centre offset Hz, peak dB */
    /* Derived */
    int span = 0;
    double noise_in_width = 0;
};

constexpr double kHumpSigmaHz = 30000;
constexpr double kSlopeClampHz = 100000;

double
floor_shape(const Condition& c, double from_channel_hz) {
    if (c.floor == SLOPE) {
        const double f = std::max(-kSlopeClampHz, std::min(kSlopeClampHz, from_channel_hz));
        return std::pow(10.0, c.floor_a * f / 1000.0 / 10.0);
    }
    if (c.floor == HUMP) {
        const double d = from_channel_hz - c.floor_a;
        return 1.0 + std::pow(10.0, c.floor_b / 10.0) * std::exp(-d * d / (2 * kHumpSigmaHz * kHumpSigmaHz));
    }
    return 1.0;
}

double
channel_hz(const Condition& c) {
    return c.grid_shift_hz;
}

int
transform_points(uint32_t rate) {
    return (int)std::lround(rate * kTrialSeconds);
}

/* The detector's transform size for the request: at least 8 bins across its narrowest class. */
int
fft_points(const Condition& c) {
    double narrowest = c.analog_width ? c.analog_width : 1e9;
    if (c.classes & DSD_SIGNAL_QUIET_CLASS_12K5) {
        narrowest = std::min(narrowest, (double)DSD_SIGNAL_QUIET_WIDTH_12K5_HZ);
    }
    if (c.classes & DSD_SIGNAL_QUIET_CLASS_6K25) {
        narrowest = std::min(narrowest, (double)DSD_SIGNAL_QUIET_WIDTH_6K25_HZ);
    }
    return dsd_signal_fft_size_for_width(c.rate, narrowest);
}

/* The nine sub-windows' length for the request (the detector's rule: the smallest whole number of transforms of at
 * least 10 ms each). */
int
quiet_span(const Condition& c) {
    const int n = fft_points(c);
    if (n <= 0) {
        return 0;
    }
    const long long frames = ((long long)c.rate + 100LL * n - 1) / (100LL * n);
    return (int)(DSD_SIGNAL_QUIET_WINDOWS * frames * n);
}

int
bin_hz(const Condition& c) {
    return (int)(c.rate / (uint32_t)fft_points(c));
}

/* Noise power inside the S/N width: white noise 2 sigma^2 W / rate as the reception tool's; a coloured floor summed
 * over the transform bins it is shaped on. */
double
noise_in_width(const Condition& c) {
    if (c.floor == WHITE) {
        return 2 * kNoiseSigma * kNoiseSigma * c.snr_width / c.rate;
    }
    const int n = transform_points(c.rate);
    const double centre = channel_hz(c) + c.snr_offset_hz;
    double sum = 0;
    for (int k = 0; k < n; ++k) {
        const double f = (k < n / 2 ? k : k - n) * (double)c.rate / n;
        if (std::fabs(f - centre) <= c.snr_width / 2.0) {
            sum += floor_shape(c, f - channel_hz(c));
        }
    }
    return 2 * kNoiseSigma * kNoiseSigma * sum / n;
}

const char*
class_text(uint8_t classes) {
    switch (classes) {
        case DSD_SIGNAL_QUIET_CLASS_12K5: return "12k5";
        case DSD_SIGNAL_QUIET_CLASS_6K25: return "6k25";
        case DSD_SIGNAL_QUIET_CLASS_ANALOG: return "analog";
        case DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25: return "12k5+6k25";
        default: return "12k5+6k25+analog";
    }
}

/* "+1.5" style text of @p value with @p decimals (0 or 1) decimals. */
std::string
signed_text(double value, int decimals) {
    char text[32];
    DSD_SNPRINTF(text, sizeof(text), "%+.*f", decimals, value);
    return text;
}

struct Builder {
    const std::vector<Source>& sources;
    const std::vector<double>& neighbour_levels;
    std::vector<Condition> out;

    Condition
    base(const char* group, const Mode& m, uint32_t rate) const {
        Condition c;
        c.group = group;
        c.rate = rate;
        c.classes = m.cls;
        c.analog_width = m.analog ? m.width_hz : 0;
        c.snr_db = m.reception_db - kMarginDb;
        c.snr_width = m.width_hz;
        c.carriers.push_back(Carrier{find_source(sources, m.row), 0, 1});
        c.name = std::string(group) + "/" + m.name + "/" + std::to_string(rate / 1000) + "k";
        return c;
    }

    void
    add(Condition c, const std::string& suffix) {
        if (!suffix.empty()) {
            c.name += "/" + suffix;
        }
        out.push_back(std::move(c));
    }

    void
    digital(const Mode& m) {
        static const double offsets[] = {0, -500, 500, -1000, 1000, -2500, 2500};
        const int count = m.cls == DSD_SIGNAL_QUIET_CLASS_6K25 ? 7 : 5;
        for (uint32_t rate : kRates) {
            for (int i = 0; i < count; ++i) {
                Condition c = base("digital", m, rate);
                c.carriers[0].offset_hz = c.snr_offset_hz = offsets[i];
                add(c, "off" + signed_text(offsets[i], 0));
                if (std::strcmp(m.name, "dmr_ms") == 0) {
                    c.group = "dmr_ms_droop";
                    c.name = "dmr_ms_droop/" + c.name.substr(c.name.find('/') + 1);
                    c.carriers[0].droop = true;
                    add(c, "off" + signed_text(offsets[i], 0));
                }
            }
            Condition mixed = base("digital_mixed", m, rate);
            mixed.classes = DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25;
            add(mixed, "");
        }
    }

    void
    analog(const Mode& m) {
        for (uint32_t rate : kRates) {
            Condition c = base("analog", m, rate);
            const int bin = bin_hz(c);
            for (int shift : {0, bin / 4, bin / 2}) {
                c.grid_shift_hz = shift;
                Condition copy = c;
                add(copy, "grid" + std::to_string(shift));
            }
            Condition mixed = base("analog_mixed", m, rate);
            mixed.classes |= DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25;
            add(mixed, "");
        }
    }

    void
    offset_carriers() {
        const Mode* am = nullptr;
        for (const Mode& m : kModes) {
            if (std::strcmp(m.row, kOffsetCarrierRow) == 0) {
                am = &m;
            }
        }
        /* ICAO Annex 10 Vol III 2.2.1.1.1 / Attachment A: carriers up to +/-8 kHz in a 25 kHz channel; the 2-, 3-, 4-
         * and 5-carrier examples, and single offset carriers. The S/N is the scene's total. */
        static const std::vector<std::vector<double>> scenes = {{-8000},
                                                                {-5000},
                                                                {-2500},
                                                                {2500},
                                                                {5000},
                                                                {8000},
                                                                {-5000, 5000},
                                                                {0, -7300, 7300},
                                                                {-2500, 2500, -7500, 7500},
                                                                {0, -4000, 4000, -8000, 8000}};
        for (uint32_t rate : kRates) {
            for (const auto& scene : scenes) {
                Condition c = base("am_offset_carrier", *am, rate);
                c.carriers.clear();
                std::string tag;
                for (double f : scene) {
                    c.carriers.push_back(Carrier{find_source(sources, am->row), f, 1.0 / scene.size()});
                    tag += signed_text(f / 1000, 1) + "k";
                }
                add(c, tag);
            }
        }
    }

    void
    neighbours(const Mode& m) {
        const int nb = find_source(sources, kNeighbourRow);
        for (double spacing : {12500.0, 25000.0}) {
            for (double level : neighbour_levels) {
                for (int side : {1, -1, 0}) {
                    Condition c = base("neighbour", m, kSceneRate);
                    for (int s : {1, -1}) {
                        if (side == 0 || side == s) {
                            Carrier n{nb, s * spacing, 0, level, true};
                            c.carriers.push_back(n);
                        }
                    }
                    const char* where = side > 0 ? "upper" : side < 0 ? "lower" : "both";
                    /* whole levels keep the first calibration's names (and so its seeds): "+30dB"; others "+7.5dB" */
                    const int decimals = std::fabs(level - std::round(level)) < 1e-9 ? 0 : 1;
                    add(c,
                        signed_text(spacing / 1000, 1).substr(1) + "k/" + signed_text(level, decimals) + "dB/" + where);
                }
            }
        }
    }

    void
    floors(const Mode& m, Kind kind) {
        struct Shape {
            Floor floor;
            double a, b;
            const char* tag;
        };

        static const Shape shapes[] = {{SLOPE, 0.1, 0, "slope+0.1dB/kHz"},   {SLOPE, -0.1, 0, "slope-0.1dB/kHz"},
                                       {SLOPE, 0.3, 0, "slope+0.3dB/kHz"},   {SLOPE, -0.3, 0, "slope-0.3dB/kHz"},
                                       {HUMP, -40000, 20, "broad-40k+20dB"}, {HUMP, 0, 20, "broad0k+20dB"},
                                       {HUMP, 40000, 20, "broad+40k+20dB"}};
        for (const Shape& s : shapes) {
            Condition c = base(s.floor == SLOPE ? "slope" : "broad", m, kSceneRate);
            if (kind == INFO) {
                c.group = s.floor == SLOPE ? "noise_slope" : "noise_broad";
                c.name = c.group + c.name.substr(c.name.find('/'));
                c.carriers.clear();
            }
            c.kind = kind;
            c.floor = s.floor;
            c.floor_a = s.a;
            c.floor_b = s.b;
            add(c, s.tag);
        }
    }

    void
    noise_only() {
        struct Request {
            uint8_t classes;
            uint32_t width;
            const char* tag;
        };

        std::vector<Request> requests = {{DSD_SIGNAL_QUIET_CLASS_12K5, 0, "12k5"},
                                         {DSD_SIGNAL_QUIET_CLASS_6K25, 0, "6k25"}};
        for (const Mode& m : kModes) {
            if (m.analog) {
                requests.push_back(Request{DSD_SIGNAL_QUIET_CLASS_ANALOG, m.width_hz, m.name});
            }
        }
        for (uint32_t rate : kRates) {
            for (const Request& r : requests) {
                Condition c;
                c.group = "noise";
                c.kind = NOISE;
                c.rate = rate;
                c.classes = r.classes;
                c.analog_width = r.width;
                c.snr_width = r.width ? r.width : 12500;
                const int bin = bin_hz(c);
                for (int shift : {0, bin / 4, bin / 2}) {
                    c.grid_shift_hz = shift;
                    c.name = "noise/" + std::string(r.tag) + "/" + std::to_string(rate / 1000) + "k/grid"
                             + std::to_string(shift);
                    out.push_back(c);
                }
                c.grid_shift_hz = 0;
                c.group = "noise_mixed";
                c.classes = r.classes | DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25;
                c.name = "noise_mixed/" + std::string(class_text(c.classes)) + (r.width ? "/" : "")
                         + (r.width ? r.tag : "") + "/" + std::to_string(rate / 1000) + "k";
                if (r.width || r.classes == DSD_SIGNAL_QUIET_CLASS_12K5) {
                    out.push_back(c);
                }
            }
        }
    }
};

std::vector<Condition>
build_conditions(const std::vector<Source>& sources, const std::vector<double>& neighbour_levels) {
    Builder b{sources, neighbour_levels, {}};
    for (const Mode& m : kModes) {
        if (m.analog) {
            b.analog(m);
        } else {
            b.digital(m);
        }
        b.neighbours(m);
        b.floors(m, ACTIVE);
    }
    b.offset_carriers();
    b.noise_only();
    for (const Mode& m : kModes) {
        if (std::strcmp(m.name, "p25p1") == 0 || std::strcmp(m.name, "nxdn48") == 0 || m.analog) {
            b.floors(m, INFO);
        }
    }
    for (Condition& c : b.out) {
        c.span = quiet_span(c);
        c.noise_in_width = noise_in_width(c);
    }
    return b.out;
}

/* ------------------------------------------------------------------------------------------------ trials */

struct Rng {
    uint64_t s[4];

    static uint64_t
    splitmix(uint64_t* x) {
        uint64_t z = (*x += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    explicit Rng(uint64_t seed) {
        for (uint64_t& v : s) {
            v = splitmix(&seed);
        }
    }

    uint64_t
    next() {
        const uint64_t result = rotl(s[1] * 5, 7) * 9, t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }

    static uint64_t
    rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    double
    uniform() { /* (0, 1] */
        return ((next() >> 11) + 1) * (1.0 / 9007199254740992.0);
    }

    /* Complex Gaussian, each component sigma (Box-Muller). */
    cf
    gauss(double sigma) {
        const double r = sigma * std::sqrt(-2.0 * std::log(uniform())), t = 2 * kPi * uniform();
        return cf((float)(r * std::cos(t)), (float)(r * std::sin(t)));
    }
};

uint64_t
fnv1a(const std::string& text) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        h = (h ^ ch) * 1099511628211ULL;
    }
    return h;
}

struct Worker {
    std::vector<cf> trial, noise;
    dsd_io::FftSetupCache fft;
    dsd_signal_quiet_detector* detectors[2] = {nullptr, nullptr};

    Worker() {
        detectors[0] = dsd_signal_quiet_detector_create();
        detectors[1] = dsd_signal_quiet_detector_create();
    }

    ~Worker() {
        dsd_signal_quiet_detector_destroy(detectors[0]);
        dsd_signal_quiet_detector_destroy(detectors[1]);
        fft.release();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;
};

void
render_noise(const Condition& c, Rng& rng, Worker& w) {
    if (c.floor == WHITE) {
        for (int i = 0; i < c.span; ++i) {
            w.trial[(size_t)i] = rng.gauss(kNoiseSigma);
        }
        return;
    }
    /* Circularly shaped: an exactly stationary Gaussian floor whose density follows floor_shape(). */
    const int n = transform_points(c.rate);
    w.noise.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        w.noise[(size_t)i] = rng.gauss(kNoiseSigma);
    }
    w.fft.forward(n, reinterpret_cast<float*>(w.noise.data()));
    for (int k = 0; k < n; ++k) {
        const double f = (k < n / 2 ? k : k - n) * (double)c.rate / n;
        w.noise[(size_t)k] *= (float)(std::sqrt(floor_shape(c, f - channel_hz(c))) / n);
    }
    inverse(w.fft, n, w.noise.data());
    std::copy(w.noise.begin(), w.noise.begin() + c.span, w.trial.begin());
}

/* The burst period holding capture sample @p index, or nullptr. */
const std::pair<int64_t, int64_t>*
burst_at(const Placed& p, int64_t index) {
    const auto it = std::upper_bound(p.bursts.begin(), p.bursts.end(), std::make_pair(index, INT64_MAX));
    if (it == p.bursts.begin() || index >= std::prev(it)->second) {
        return nullptr;
    }
    return &*std::prev(it);
}

/* DMR MS at the region B mask limits (ETSI TS 102 361-1 figure 10.3): +1 dBp then -3 dBp (or the reverse), the 0 dBp
 * mean kept, so the burst-on S/N is unchanged and a sub-window inside the burst can sit up to 3 dB lower. */
double
droop_gain(const Placed& p, int64_t index, bool late) {
    static const double hi = std::pow(10.0, 0.1), lo = std::pow(10.0, -0.3), low_share = (hi - 1) / (hi - lo);
    const std::pair<int64_t, int64_t>* b = burst_at(p, index);
    if (!b) {
        return 1.0;
    }
    const double pos = (double)(index - b->first) / (double)(b->second - b->first);
    const bool low = late ? pos >= 1 - low_share : pos < low_share;
    return std::sqrt(low ? lo : hi);
}

/* Add one carrier at a random start, phase and (DMR MS droop) side; returns the start in the placed source. */
int64_t
add_carrier(const Condition& c, const Carrier& k, const std::vector<Placed>& placed, const std::vector<Source>& sources,
            Rng& rng, Worker& w) {
    const Placed& p = placed[(size_t)k.source];
    const double wanted = std::pow(10.0, c.snr_db / 10.0) * c.noise_in_width;
    const double power = k.neighbour ? wanted * std::pow(10.0, k.rel_db / 10.0) : wanted * k.share;
    const double amplitude = std::sqrt(power / sources[(size_t)k.source].band_power);
    int64_t first = (int64_t)c.rate / 10, last = (int64_t)p.x.size() - c.span - (int64_t)c.rate / 10;
    if (!p.bursts.empty()) {
        /* An ongoing burst train: the trial starts no earlier than one inter-burst gap before the first burst, as at
         * any phase of the train, and ends inside the last burst. */
        first = std::max(first, p.bursts.front().first - (int64_t)(kBurstGapSeconds * c.rate));
        last = std::min(last, p.bursts.back().second - c.span);
    }
    const int64_t start = first + (int64_t)(rng.next() % (uint64_t)(last - first));
    const double step = 2 * kPi * (channel_hz(c) + k.offset_hz) / c.rate;
    std::complex<double> rot = std::polar(amplitude, 2 * kPi * rng.uniform());
    const std::complex<double> turn = std::polar(1.0, step);
    const bool late = (rng.next() & 1) != 0;
    for (int i = 0; i < c.span; ++i) {
        double gain = 1.0;
        if (k.droop) {
            gain = droop_gain(p, start + i, late);
        }
        const std::complex<double> v(p.x[(size_t)(start + i)].real(), p.x[(size_t)(start + i)].imag());
        const std::complex<double> s = v * rot * gain;
        w.trial[(size_t)i] += cf((float)s.real(), (float)s.imag());
        rot *= turn;
        if ((i & 1023) == 1023) {
            rot *= amplitude / std::abs(rot);
        }
    }
    return start;
}

uint64_t
trial_seed(const Condition& c, uint64_t phase, long trial) {
    return fnv1a(c.name) ^ (phase * 0x9E3779B97F4A7C15ULL) ^ ((uint64_t)trial * 0xD1B54A32D192ED03ULL);
}

void
render(const Condition& c, const std::vector<Placed>& placed, const std::vector<Source>& sources, uint64_t seed,
       Worker& w) {
    Rng rng(seed);
    w.trial.resize((size_t)c.span);
    render_noise(c, rng, w);
    for (const Carrier& k : c.carriers) {
        add_carrier(c, k, placed, sources, rng, w);
    }
}

dsd_signal_quiet_request
request_for(const Condition& c, long trial) {
    dsd_signal_quiet_request r{};
    r.request_id = (uint64_t)trial + 1;
    /* The RTL tap's geometry: tuned Fs/4 above the channel, the ingest rotation moving the channel to the samples'
     * 0 Hz (src/io/radio/rtl_sdr_fm.cpp demod_feed_signal_search()). */
    r.capture.rate_hz = c.rate;
    r.capture.capture_hz = kChannelHz + c.rate / 4;
    r.capture.rotation_hz = (int32_t)(c.rate / 4);
    r.capture.capture_offset_hz = c.rate / 4;
    r.capture.generation = 1;
    r.frequency_hz = kChannelHz + (uint32_t)c.grid_shift_hz;
    r.classes = c.classes;
    r.analog_width_hz = c.analog_width;
    return r;
}

/* One trial through one detector. No NaN marks a missing value: the driver may be built with finite-only math. */
struct Verdict {
    float ratio = 0, rise = 0; /* the highest ratio and floor rise, when done */
    uint8_t done = 0;          /* the request completed */
    uint8_t valid = 0;         /* geometry fits and no floor over the guard */
    uint8_t quiet = 0;
};

Verdict
judge(const Condition& c, long trial, dsd_signal_quiet_detector* d, const dsd_signal_quiet_params& params,
      const std::vector<cf>& samples) {
    Verdict v;
    const dsd_signal_quiet_request r = request_for(c, trial);
    dsd_signal_request_failure why = DSD_SIGNAL_REQUEST_OK;
    if (dsd_signal_quiet_detector_begin(d, &r, &params, &why) != 0) {
        return v;
    }
    const int done =
        dsd_signal_quiet_detector_feed(d, reinterpret_cast<const float*>(samples.data()), samples.size(), &r.capture);
    dsd_signal_quiet_result res{};
    if (done != 1 || dsd_signal_quiet_detector_result(d, &res) != 1) {
        return v;
    }
    v.done = 1;
    v.valid = res.valid;
    v.quiet = res.quiet;
    v.ratio = res.max_ratio_db;
    v.rise = res.floor_rise_db;
    return v;
}

/* ------------------------------------------------------------------------------------------------ runs */

constexpr double kGuardOff = 1000;       /* a guard no floor reaches: the raw ratios and rises */
constexpr float kInvalidBusyDb = 999.0f; /* a noise-only trial the guard voided, in the select quantiles */

struct Options {
    std::string sources, phase, out, filter, dump;
    long trials = 0, jobs = 0;
    double threshold = 0, guard = kGuardOff;
    int estimator = -1;
    std::vector<double> neighbour_levels{30.0, 50.0};
};

struct Outcome {
    std::vector<Verdict> trials[2];
};

template <typename F>
void
parallel(long count, long jobs, F&& body) {
    std::atomic<long> next{0};
    std::vector<std::thread> threads;
    threads.reserve((size_t)jobs);
    for (long t = 0; t < jobs; ++t) {
        threads.emplace_back([&] {
            Worker w;
            for (long i = next.fetch_add(1); i < count; i = next.fetch_add(1)) {
                body(i, w);
            }
        });
    }
    for (std::thread& t : threads) {
        t.join();
    }
}

std::vector<Placed>
place_sources(uint32_t rate, const Options& o, const std::vector<Source>& sources, const std::vector<bool>& needed) {
    std::vector<Placed> placed(sources.size());
    parallel((long)sources.size(), o.jobs, [&](long i, Worker&) {
        if (needed[(size_t)i]) {
            placed[(size_t)i] = place_source(sources[(size_t)i], rate);
        }
    });
    return placed;
}

void
run_rate(uint32_t rate, const Options& o, const std::vector<Source>& sources, const std::vector<Condition>& all,
         const std::vector<size_t>& chosen, std::vector<Outcome>* outcomes) {
    std::vector<size_t> here;
    std::vector<bool> needed(sources.size(), false);
    for (size_t i : chosen) {
        if (all[i].rate == rate) {
            here.push_back(i);
            for (const Carrier& k : all[i].carriers) {
                needed[(size_t)k.source] = true;
            }
        }
    }
    if (here.empty()) {
        return;
    }
    const std::vector<Placed> placed = place_sources(rate, o, sources, needed);
    const bool select = o.phase == "select";
    const uint64_t phase = select ? 1 : 2;
    const size_t trials = (size_t)o.trials;
    parallel((long)(here.size() * trials), o.jobs, [&](long job, Worker& w) {
        const size_t index = here[(size_t)job / trials];
        const Condition& c = all[index];
        Outcome& out = (*outcomes)[index];
        const long trial = (long)((size_t)job % trials);
        render(c, placed, sources, trial_seed(c, phase, trial), w);
        for (int e = 0; e < 2; ++e) {
            if (!select && e != o.estimator) {
                continue;
            }
            const dsd_signal_quiet_params params{select ? 0.0f : (float)o.threshold, e, (float)o.guard};
            out.trials[e][(size_t)trial] = judge(c, trial, w.detectors[e], params, w.trial);
        }
    });
    DSD_FPRINTF(stderr, "rate %u: %zu conditions done\n", rate, here.size());
}

const char*
kind_text(Kind k) {
    return k == ACTIVE ? "active" : k == NOISE ? "noise" : "info";
}

/* The q-quantiles (nearest rank) of the ratios (or rises) of the trials @p take selects, printed comma-separated
 * with @p end after the last; empty fields when none is selected. */
template <typename Take>
void
print_quantiles(FILE* f, const std::vector<Verdict>& trials, bool rise, Take take, std::initializer_list<double> qs,
                char end) {
    std::vector<float> v;
    for (const Verdict& t : trials) {
        if (take(t)) {
            v.push_back(rise ? t.rise : t.ratio);
        }
    }
    std::sort(v.begin(), v.end());
    size_t i = 0;
    for (double q : qs) {
        const char sep = ++i == qs.size() ? end : ',';
        if (v.empty()) {
            DSD_FPRINTF(f, "%c", sep);
            continue;
        }
        const size_t rank = (size_t)std::min((double)v.size() - 1, std::max(0.0, std::ceil(q * v.size()) - 1));
        DSD_FPRINTF(f, "%.3f%c", v[rank], sep);
    }
}

void
write_header(FILE* f, bool select) {
    DSD_FPRINTF(f, "condition,group,kind,rate_hz,classes,analog_width_hz,grid_shift_hz,snr_db,span,trials,estimator,"
                   "guard_db,invalid,");
    if (select) {
        DSD_FPRINTF(f, "min,p001,p01,p05,p50,p95,p99,max,");
    } else {
        DSD_FPRINTF(f, "threshold_db,quiet,busy,min,p05,p50,p95,max,");
    }
    DSD_FPRINTF(f, "rise_min,rise_p05,rise_p50,rise_p95,rise_p99,rise_max\n");
}

void
write_row(FILE* f, const Condition& c, const Outcome& out, int e, const Options& o) {
    const std::vector<Verdict>& trials = out.trials[e];
    long invalid = 0, quiet = 0;
    for (const Verdict& t : trials) {
        invalid += t.valid ? 0 : 1;
        quiet += t.quiet;
    }
    DSD_FPRINTF(f, "%s,%s,%s,%u,%s,%u,%d,%.2f,%d,%zu,%s,%.2f,%ld,", c.name.c_str(), c.group.c_str(), kind_text(c.kind),
                c.rate, class_text(c.classes), c.analog_width, c.grid_shift_hz, c.kind == ACTIVE ? c.snr_db : 0.0,
                c.span, trials.size(), e == DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN ? "trimmed" : "lowest", o.guard,
                invalid);
    const auto valid = [](const Verdict& t) { return t.valid != 0; };
    const auto done = [](const Verdict& t) { return t.done != 0; };
    if (o.phase == "select" && c.kind != ACTIVE) {
        /* noise only: an invalid (guarded) trial is busy, so it ranks above every ratio */
        std::vector<Verdict> busy = trials;
        for (Verdict& t : busy) {
            t.ratio = t.valid ? t.ratio : kInvalidBusyDb;
        }
        print_quantiles(f, busy, false, done, {0, 0.001, 0.01, 0.05, 0.5, 0.95, 0.99, 1}, ',');
    } else if (o.phase == "select") {
        print_quantiles(f, trials, false, valid, {0, 0.001, 0.01, 0.05, 0.5, 0.95, 0.99, 1}, ',');
    } else {
        /* busy: every verdict that is not quiet, an invalid (guarded) result included */
        DSD_FPRINTF(f, "%.2f,%ld,%ld,", o.threshold, quiet, (long)trials.size() - quiet);
        print_quantiles(f, trials, false, valid, {0, 0.05, 0.5, 0.95, 1}, ',');
    }
    print_quantiles(f, trials, true, done, {0, 0.05, 0.5, 0.95, 0.99, 1}, '\n');
}

/* Every trial's ratio and rise as little-endian float32 pairs, rows in the CSV's order (--dump; read it with the
 * guard off, where every completed trial is valid). */
void
write_dump(FILE* dump, const Outcome& out, int e) {
    if (!dump) {
        return;
    }
    std::vector<float> pairs;
    pairs.reserve(out.trials[e].size() * 2);
    for (const Verdict& t : out.trials[e]) {
        pairs.push_back(t.ratio);
        pairs.push_back(t.rise);
    }
    if (fwrite(pairs.data(), sizeof(float), pairs.size(), dump) != pairs.size()) {
        DSD_FPRINTF(stderr, "dump write failed\n");
    }
}

int
run_trials(const Options& o, const std::vector<Source>& sources, const std::vector<Condition>& all,
           const std::vector<size_t>& chosen) {
    std::vector<Outcome> outcomes(all.size());
    for (size_t i : chosen) {
        for (int e = 0; e < 2; ++e) {
            outcomes[i].trials[e].assign((size_t)o.trials, Verdict{});
        }
    }
    for (uint32_t rate : kRates) {
        run_rate(rate, o, sources, all, chosen, &outcomes);
    }
    FILE* f = dsd_fopen_private(o.out.c_str(), "w");
    if (!f) {
        DSD_FPRINTF(stderr, "cannot write %s\n", o.out.c_str());
        return 1;
    }
    write_header(f, o.phase == "select");
    FILE* dump = o.dump.empty() ? nullptr : dsd_fopen_private(o.dump.c_str(), "wb");
    for (size_t i : chosen) {
        for (int e = 0; e < 2; ++e) {
            if (o.phase == "select" || e == o.estimator) {
                write_row(f, all[i], outcomes[i], e, o);
                write_dump(dump, outcomes[i], e);
            }
        }
    }
    fclose(f);
    if (dump) {
        fclose(dump);
    }
    return 0;
}

/* In-width power of @p x around @p centre_hz (Hann-windowed transform of the first transform_points() samples). */
double
in_width_power(dsd_io::FftSetupCache& fft, const std::vector<cf>& x, uint32_t rate, double centre_hz, double width,
               double* total) {
    const int n = transform_points(rate);
    std::vector<cf> buf((size_t)n);
    double energy = 0;
    for (int i = 0; i < n; ++i) {
        const double h = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / n);
        buf[(size_t)i] = x[(size_t)i] * (float)h;
        energy += h * h;
    }
    fft.forward(n, reinterpret_cast<float*>(buf.data()));
    double in = 0, all = 0;
    for (int k = 0; k < n; ++k) {
        const double p = std::norm(buf[(size_t)k]) / (n * energy);
        const double f = (k < n / 2 ? k : k - n) * (double)rate / n;
        all += p;
        if (std::fabs(f - centre_hz) <= width / 2) {
            in += p;
        }
    }
    *total = all;
    return in;
}

/* One wanted signal's S/N measured back from @p o.trials renders: its in-width power (a burst train's as band_power():
 * its in-width share times its power during the bursts) over the in-width power of the floor rendered alone. */
double
measured_snr_db(Condition c, const Options& o, const std::vector<Placed>& placed, const std::vector<Source>& sources,
                dsd_io::FftSetupCache& fft) {
    c.span = transform_points(c.rate);
    const Placed& p = placed[(size_t)c.carriers[0].source];
    double signal = 0, noise = 0, total = 0;
    Worker w;
    for (long t = 0; t < o.trials; ++t) {
        Rng rng(trial_seed(c, 3, t));
        w.trial.assign((size_t)c.span, cf(0, 0));
        const int64_t start = add_carrier(c, c.carriers[0], placed, sources, rng, w);
        const double in = in_width_power(fft, w.trial, c.rate, channel_hz(c), c.snr_width, &total);
        double on = 0;
        long count = 0;
        for (int i = 0; !p.bursts.empty() && i < c.span; ++i) {
            if (burst_at(p, start + i)) {
                on += std::norm(w.trial[(size_t)i]);
                ++count;
            }
        }
        signal += p.bursts.empty() ? in : (count ? on / count : 0) * in / total;
        render_noise(c, rng, w);
        noise += in_width_power(fft, w.trial, c.rate, channel_hz(c), c.snr_width, &total);
    }
    return 10 * std::log10(signal / noise);
}

/* The S/N cross-check: each wanted signal on frequency over a white floor, rendered as the trials are. */
int
run_snr(const Options& o, const std::vector<Source>& sources, const std::vector<Condition>& all) {
    FILE* f = dsd_fopen_private(o.out.c_str(), "w");
    if (!f) {
        return 1;
    }
    DSD_FPRINTF(f, "condition,target_snr_db,measured_snr_db,trials\n");
    for (uint32_t rate : kRates) {
        std::vector<size_t> chosen;
        std::vector<bool> needed(sources.size(), false);
        for (size_t i = 0; i < all.size(); ++i) {
            const Condition& c = all[i];
            const bool plain = (c.group == "digital" && std::fabs(c.carriers[0].offset_hz) < 0.5)
                               || (c.group == "analog" && !c.grid_shift_hz);
            if (plain && c.rate == rate) {
                chosen.push_back(i);
                needed[(size_t)c.carriers[0].source] = true;
            }
        }
        const std::vector<Placed> placed = place_sources(rate, o, sources, needed);
        dsd_io::FftSetupCache fft;
        for (size_t i : chosen) {
            DSD_FPRINTF(f, "%s,%.2f,%.3f,%ld\n", all[i].name.c_str(), all[i].snr_db,
                        measured_snr_db(all[i], o, placed, sources, fft), o.trials);
        }
        fft.release();
    }
    fclose(f);
    return 0;
}

/* The floor guard's capture-wide reference as the detector computes it (wide_reference() in
 * src/io/radio/signal_measure.cpp, the same bin selection over the request's geometry and the same quartile), nine
 * times, once per sub-window. Returns the selected values' sum so the work is not optimised away. */
double
reference_work(const Condition& c, const std::vector<double>& power, std::vector<double>& scratch) {
    const dsd_signal_quiet_request r = request_for(c, 0);
    const int n = (int)power.size();
    const double rate = c.rate, bin_hz = rate / n;
    const double dc_guard = std::max(bin_hz * 3, rate / 100.0);
    const double channel = (double)r.frequency_hz - r.capture.capture_hz;
    double widest = c.analog_width;
    if (c.classes & DSD_SIGNAL_QUIET_CLASS_6K25) {
        widest = std::max(widest, (double)DSD_SIGNAL_QUIET_WIDTH_6K25_HZ);
    }
    if (c.classes & DSD_SIGNAL_QUIET_CLASS_12K5) {
        widest = std::max(widest, (double)DSD_SIGNAL_QUIET_WIDTH_12K5_HZ);
    }
    const double half_width = widest / 2;
    double sum = 0;
    for (int w = 0; w < DSD_SIGNAL_QUIET_WINDOWS; ++w) {
        size_t count = 0;
        for (int m = -n / 2; m < n / 2; ++m) {
            double delta = m * bin_hz - r.capture.rotation_hz;
            delta -= rate * std::floor((delta + rate / 2) / rate);
            if (std::abs(delta) > dc_guard && std::abs(delta) < rate * 0.45 && std::abs(delta - channel) > half_width) {
                scratch[count++] = power[(size_t)((m + n) % n)];
            }
        }
        std::nth_element(scratch.begin(), scratch.begin() + (long)(count / 4), scratch.begin() + (long)count);
        sum += scratch[count / 4];
    }
    return sum;
}

volatile double g_cost_sink = 0; /* keeps the timed reference work */

/* CPU per quiet request (one thread, white noise, shipped parameters) and per request of the guard's reference. */
int
run_cost(const Options& o) {
    FILE* f = dsd_fopen_private(o.out.c_str(), "w");
    if (!f) {
        return 1;
    }
    DSD_FPRINTF(f, "rate_hz,classes,analog_width_hz,fft,request_us,reference_us\n");

    const struct {
        uint8_t classes;
        uint32_t width;
    } requests[] = {{DSD_SIGNAL_QUIET_CLASS_12K5, 0},
                    {DSD_SIGNAL_QUIET_CLASS_6K25, 0},
                    {DSD_SIGNAL_QUIET_CLASS_ANALOG, 5000},
                    {DSD_SIGNAL_QUIET_CLASS_ANALOG, 25000},
                    {DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25 | DSD_SIGNAL_QUIET_CLASS_ANALOG, 5000}};

    Worker w;
    const dsd_signal_quiet_params params = dsd_signal_quiet_params_default();
    for (uint32_t rate : kRates) {
        for (const auto& q : requests) {
            Condition c;
            c.rate = rate;
            c.classes = q.classes;
            c.analog_width = q.width;
            c.span = quiet_span(c);
            Rng rng(fnv1a("cost"));
            w.trial.resize((size_t)c.span);
            render_noise(c, rng, w);
            uint64_t start = dsd_realtime_mono_ns();
            for (long t = 0; t < o.trials; ++t) {
                g_cost_sink = g_cost_sink + judge(c, t, w.detectors[0], params, w.trial).ratio;
            }
            const double request_us = (double)(dsd_realtime_mono_ns() - start) / 1000.0 / (double)o.trials;
            const int bins = fft_points(c);
            std::vector<double> power((size_t)bins), scratch((size_t)bins);
            for (int k = 0; k < bins; ++k) {
                power[(size_t)k] = std::norm(w.trial[(size_t)k]);
            }
            start = dsd_realtime_mono_ns();
            for (long t = 0; t < o.trials; ++t) {
                g_cost_sink = g_cost_sink + reference_work(c, power, scratch);
            }
            const double reference_us = (double)(dsd_realtime_mono_ns() - start) / 1000.0 / (double)o.trials;
            DSD_FPRINTF(f, "%u,%s,%u,%d,%.1f,%.1f\n", rate, class_text(q.classes), q.width, bins, request_us,
                        reference_us);
        }
    }
    fclose(f);
    return 0;
}

/* A comma-separated list of neighbour levels (dB over the wanted signal, 0 to 60, in 0.1 dB steps so each names its
 * conditions exactly); false when any does not parse. */
bool
parse_levels(const std::string& text, std::vector<double>* out) {
    std::vector<double> levels;
    std::istringstream fields(text);
    std::string field;
    while (std::getline(fields, field, ',')) {
        double v = 0;
        if (!parse_double(field, &v) || v < 0 || v > 60 || std::fabs(v * 10 - std::round(v * 10)) > 1e-6) {
            return false;
        }
        levels.push_back(v);
    }
    if (levels.empty()) {
        return false;
    }
    *out = std::move(levels);
    return true;
}

/* One numeric option's value into @p o; false when it does not parse. */
bool
parse_number(const std::string& key, const char* value, Options* o) {
    if (key == "--trials") {
        return parse_count(value, 1, 1000000, &o->trials);
    }
    if (key == "--jobs") {
        return parse_count(value, 1, 1024, &o->jobs);
    }
    if (key == "--threshold") {
        return parse_double(value, &o->threshold);
    }
    if (key == "--guard") {
        return parse_double(value, &o->guard);
    }
    if (key == "--neighbour-levels") {
        return parse_levels(value, &o->neighbour_levels);
    }
    if (key == "--estimator") {
        o->estimator = std::strcmp(value, "trimmed") == 0  ? DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN
                       : std::strcmp(value, "lowest") == 0 ? DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK
                                                           : -2;
        return o->estimator >= 0;
    }
    return false;
}

/* Per-phase defaults; false when a required option is missing. */
bool
finish_options(Options* o) {
    if (o->trials == 0) {
        o->trials = o->phase == "validate" ? 3000 : o->phase == "select" ? 2000 : 20;
    }
    if (o->jobs == 0) {
        o->jobs = std::max(1u, std::thread::hardware_concurrency());
    }
    const bool phase_ok = o->phase == "select" || o->phase == "snr" || o->phase == "cost"
                          || (o->phase == "validate" && o->estimator >= 0);
    return (o->phase == "cost" || !o->sources.empty()) && !o->out.empty() && phase_ok;
}

bool
parse_options(int argc, char** argv, Options* o) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        std::string* text = key == "--sources"  ? &o->sources
                            : key == "--phase"  ? &o->phase
                            : key == "--out"    ? &o->out
                            : key == "--filter" ? &o->filter
                            : key == "--dump"   ? &o->dump
                                                : nullptr;
        if (text) {
            *text = argv[i + 1];
        } else if (!parse_number(key, argv[i + 1], o)) {
            return false;
        }
    }
    return argc % 2 == 1 && finish_options(o);
}

} // namespace

int
main(int argc, char** argv) {
    Options o;
    if (!parse_options(argc, argv, &o)) {
        DSD_FPRINTF(stderr,
                    "usage: %s --phase select|validate|snr|cost --out FILE [--sources DIR] [--trials N] [--jobs N] "
                    "[--threshold DB --estimator trimmed|lowest] [--guard DB] [--filter TEXT] [--dump FILE] "
                    "[--neighbour-levels DB,DB,...]\n",
                    argv[0]);
        return 2;
    }
    if (o.phase == "cost") {
        return run_cost(o);
    }
    std::vector<Source> sources;
    if (!load_sources(o.sources, &sources)) {
        return 1;
    }
    std::vector<Condition> all = build_conditions(sources, o.neighbour_levels);
    std::vector<size_t> chosen;
    for (size_t i = 0; i < all.size(); ++i) {
        const Condition& c = all[i];
        bool sources_ok = true;
        for (const Carrier& k : c.carriers) {
            sources_ok = sources_ok && k.source >= 0;
        }
        if (!sources_ok || c.span <= 0 || c.span > transform_points(c.rate)) {
            DSD_FPRINTF(stderr, "condition %s: missing source or bad span %d\n", c.name.c_str(), c.span);
            return 1;
        }
        if (o.filter.empty() || c.name.find(o.filter) != std::string::npos) {
            chosen.push_back(i);
        }
    }
    DSD_FPRINTF(stderr, "%zu of %zu conditions, %ld trials each\n", chosen.size(), all.size(), o.trials);
    if (o.phase == "snr") {
        return run_snr(o, sources, all);
    }
    return run_trials(o, sources, all, chosen);
}
