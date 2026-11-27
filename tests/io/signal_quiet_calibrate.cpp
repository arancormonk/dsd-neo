// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Calibration driver for the scan skip-silent check's quiet threshold (issue #834), run by hand: it runs the
 * stand-alone quiet detector, the one the shared tap runs, over hundreds of rendered conditions and thousands of
 * trials each, minutes on every core. docs/signal-search.md "Quiet-channel calibration" gives the method and results.
 * CTest runs it only for two short checks of the driver itself: its outputs on rendered tones
 * (IO_SIGNAL_QUIET_CALIBRATE_OUTPUTS), and one condition at a time over a tiny source set
 * (IO_SIGNAL_QUIET_CALIBRATE_CHOOSE), to check that it refuses a short source a chosen condition uses and no other.
 *
 *   dsd-neo_test_signal_quiet_calibrate --phase select|validate|snr|cost --out FILE [--sources DIR] [--trials N]
 *       [--jobs N] [--threshold DB --estimator trimmed|lowest] [--guard DB] [--filter TEXT] [--dump FILE]
 *       [--neighbour-levels DB,DB,...] [--geometry default|centred] [--rates HZ,HZ,...]
 *       [--response flat|nominal|steeper|mirrored] [--depression DB] [--adc-noise-db DB] [--modes-file FILE]
 *       [--pilot MODE] [--memory-mb MB] [--windows N] [--scene-ceiling BOUND]
 *
 * --threshold and --estimator belong to validate, the one phase that judges at them; any other phase refuses them, so
 * no run attests parameters it did not use.
 *
 * --windows N (1..DSD_SIGNAL_QUIET_WINDOWS_MAX) judges N sub-windows of the shipped length instead of the calibrated
 * nine, through the request's own count (issue #834 follow-up): a short check of another reading length, not a
 * calibration. Without it every request asks for the default count, so the output is unchanged.
 *
 * DIR holds the clean 48 kHz sources tools/scan_quiet_reception.py --export-sources writes, from the same generators
 * that measured each mode's reception threshold. Each is interpolated to the capture rate in the frequency domain, so
 * its spectrum (and the power band_power() gave inside its class width) is unchanged, then placed at the channel in the
 * capture's geometry. Every phase but cost needs DIR. Rows whose samples are the same (one transmitter exported for
 * several widths) share one waveform, placed once per rate, each keeping its own in-width power. The S/N is the
 * tool's: the signal's power inside the mode's class width (for a burst train, during its bursts) over the noise power
 * inside the same width around the signal, both after the receiver's response when one is rendered.
 *
 * select    every condition, both floor estimators on the same samples: per condition the lowest, percentiles and
 *           highest max-of-9 ratio over its valid trials (a noise-only condition's invalid trials count as busy, at
 *           +999 dB), and the sideband floor's rise over the capture-wide reference (--guard, default off; --dump
 *           writes every trial's ratio and rise). The ratio is judged against the lower of that reference and the
 *           sideband floor on every geometry (quiet_judged_noise() in src/io/radio/signal_measure.cpp), and the
 *           estimator sets that floor: the two estimators read the same ratio only where both floors stand over the
 *           reference, so their rows differ wherever a floor reads under it, on a shifted capture too. The estimator
 *           also sets the floor the guard compares.
 * validate  every condition at one threshold, estimator and guard, fresh seeds: quiet and busy verdicts (an invalid
 *           result is busy)
 * snr       the rendered S/N of every chosen active condition's wanted signal (all its carriers but neighbours),
 *           measured back from the samples after the response, at the scene's scale (which keeps the S/N), against
 *           the target; a render whose wanted signal and floor reach the clip level fails the run
 * cost      CPU per quiet request on noise (shipped parameters), and per request of the floor guard's reference alone
 *           (the same bin selection and quartile over the geometry's policy, nine times)
 *
 * --neighbour-levels sets the adjacent-channel transmitters' levels over the wanted signal (default 30,50, the levels
 * the first calibration used; the shipped calibration ran 5 to 30 dB in 2.5 dB steps, then 35 to 50 dB in 5 dB
 * steps). A level's condition names, and so its seeds, do not depend on the other levels in the list.
 *
 * --geometry default renders the RTL tap's geometry (tuned Fs/4 above the channel, the ingest rotation bringing it to
 * the samples' 0 Hz; the policy every input but a native Airspy reports, a SoapySDR Airspy included); centred a native
 * Airspy's (tuned on the channel, no rotation, DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED).
 * --rates sets the capture rates of every condition, scenes included; without it the default geometry renders every
 * admitted rate and its neighbour, slope and broad-interferer scenes at 1.536 Msps, the centred one everything at
 * 2.5, 3, 6 and 10 MS/s.
 * --response renders a modelled Airspy receiver response, fixed in hardware coordinates (hardware frequency = sample
 * frequency less the rotation, folded into +-Fs/2) and applied to every transmitter and to the tuner noise, at 2.5,
 * 3, 6 and 10 MS/s only (tests/io/signal_quiet_scene.h has the model):
 *   half-band  0 dB to +-0.42 Fs, -0.35 dB at +-0.45 Fs, -6.02 dB at +-0.5 Fs (libairspy's, modelled from computed
 *              figures)
 *   nominal    tuner -3 dB at -0.335 / +0.302 Fs at 2.5 and 3 MS/s (edges 0.10 Fs wide), -0.4365 / +0.4035 Fs at 6 MS/s
 *              and -0.4665 / +0.4335 Fs at 10 MS/s (edges 0.06 Fs wide), a -40 dB stop band
 *   steeper    tuner -3 dB at +-0.28 Fs (2.5, 3 MS/s) and +-0.40 Fs (6, 10 MS/s)
 *   mirrored   nominal with its sides swapped
 * --depression DB lowers the response by DB over 3 channel widths centred on the channel (taper 0.25 widths).
 * --adc-noise-db DB adds flat ADC noise after the response, DB over the tuner noise's peak density (a mixture).
 * Both take whole tenths of a dB, the precision the CSVs record them at.
 * A response's conditions keep their names (and seeds); the CSVs end with geometry, response, depression_db and
 * adc_noise_db columns.
 * --modes-file FILE takes the modes, their source rows, reception thresholds, widths and classes from the reception
 * tool's driver_modes.tsv (header, then name, tag, row, reception_db, width_hz, class 12k5|6k25|analog, analog 0|1;
 * tag names the width's noise-only conditions) instead of the table below: the file's digital modes replace the
 * table's when it has any, and its analog modes the table's when it has any, so an analog-only reception run keeps the
 * table's digital modes (and their condition names and seeds). An analog mode's noise-only conditions are built once
 * per tag; the shipped widths' tags are the table's names, so those keep their names and seeds too.
 * --pilot MODE renders one rate (the last of the rate list), every scene kind there, and only MODE's conditions (and
 * its class's or width's noise-only ones, a digital mode's class's shaped-floor ones included): the cheap run that
 * times a full one.
 * --memory-mb bounds the waveforms placed at a capture rate at once (default 8192): a rate's conditions run in batches
 * whose waveforms fit, each batch's freed before the next. Results do not depend on it.
 * --add masks adds the analog monitor's masks (-8: a digital class beside the analog width) at every rate: each digital
 * mode in its own class and in both, beside every analog width; each analog mode beside either digital class; noise
 * alone under each two-class mask. --add occupancy adds occupied captures (a share of the reference span raised by
 * 1.5 to 10 dB, clear of the channel and its sidebands) for the lowest-margin rows of each class and kind, with noise
 * alone under each row's tag and then under every other noise tag (informational: the occupancy check's speed cost).
 * --add cross adds the -8 masks crossed with neighbours, offsets and a DMR mobile's droop (Builder::cross()); --add
 * raised the raised-sideband scenes, 80 to 99% of the span raised 1.5 to 10 dB with only the channel clear
 * (informational: a documented limit, Builder::raised()). Every group comes after the shipped matrix (the late ones
 * after everything else), so earlier conditions keep their order, names and seeds.
 * --seed-salt N gives every trial a fresh seed, the snr phase's renders too (0, the default, keeps the shipped
 * calibration's); --occupancy-veto SHARE
 * judges with the occupancy check (dsd_signal_quiet_params.occupancy_max_share; default 0, off), in selection as in
 * validation, as --guard does: a trial it voids is invalid, and selection's statistics leave it out (noise alone
 * counts it busy). --trial-log reports the share whether or not the check is on.
 * --trial-log FILE writes every trial's occupancy share, samples at or over the clip level and status (12 bytes a
 * trial, rows in the CSV's order; the status holds the completion, the detector's failure, and its decision: the result
 * valid, quiet and why it is void, write_trial_log()); --bounds FILE lists every condition's scene bound and the scale it renders
 * at. A scene whose bound (its carriers at their waveforms' peaks plus six sigma of its densest noise) passes 0.9 is
 * scaled down whole, which leaves every ratio as it was and every sample under the clip level.
 *
 * Every trial is rendered under the clip level the detector reads (issue #834 B8): a trial with a sample component at
 * or over it fails the run, whatever --trial-log says, the CSV, dump and trial log written but no completion record
 * printed. --scene-ceiling BOUND (default 0.9) moves the bound a scene is scaled under; only a check of that refusal
 * raises it, and the run and completion records then name it. So does a trial the detector did not measure (refused,
 * failed or unfinished: an analog width it does not admit, for one), which would read busy with nothing behind it; a
 * reading the guard or the occupancy check voids is a measurement and reads busy as it should. Two outputs (--out,
 * --dump, --trial-log, --bounds) that name one file are refused before any is opened.
 *
 * Every trial's samples come from its own seed (the condition's name, the phase and the trial number), so results do
 * not depend on --jobs or the batches. */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/signal_measure.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <dsd-neo/runtime/signal_search.h>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include "rtl_fft_cache.h"
#include "rtl_finite.h"
#include "signal_quiet_scene.h"

namespace {

namespace qs = dsd_quiet_scene;
using cf = qs::cf;
using qs::Rng;
constexpr double kPi = 3.14159265358979323846;
constexpr uint32_t kSourceRate = 48000;
constexpr uint32_t kChannelHz = 450000000;
/* Every capture rate a quiet request can arrive at and the check admits (dsd_signal_quiet_rate_supported()): the RTL
 * rate planner's 1.024, 1.152 and 1.536 Msps, an Airspy's 2.5, 3, 6 and 10 MS/s, and the common RTL-SDR rates a
 * SoapySDR device may be given. */
constexpr uint32_t kRates[] = {960000,  1024000, 1152000, 1200000, 1280000, 1440000, 1536000, 1600000,
                               1800000, 1920000, 2048000, 2400000, 2500000, 3000000, 6000000, 10000000};

/* A native Airspy's centred capture rates and the capture-wide reference's span at each: a copy of
 * kQuietAirspyCentredSpans in src/io/radio/signal_measure.cpp for the cost phase's reference model and the centred
 * rate list; keep them equal. centred_rates_match() checks the rates against the shipped policy at startup; the span
 * values are private to signal_measure.cpp, so a span change must be copied here by hand. */
struct CentredSpan {
    uint32_t rate;
    double span;
};

constexpr CentredSpan kCentredSpans[] = {{2500000, 0.25}, {3000000, 0.25}, {6000000, 0.375}, {10000000, 0.375}};

constexpr double kDefaultSpan = 0.45;

/* Whether kCentredSpans lists exactly the rates the shipped centred policy admits, checked over every admitted
 * default rate and an Airspy's other firmware rates (4.096 and 12 MS/s); says which rate differs when not. */
bool
centred_rates_match() {
    std::vector<uint32_t> candidates(std::begin(kRates), std::end(kRates));
    candidates.insert(candidates.end(), {4096000, 12000000});
    for (uint32_t rate : candidates) {
        const bool listed = std::any_of(std::begin(kCentredSpans), std::end(kCentredSpans),
                                        [&](const CentredSpan& e) { return e.rate == rate; });
        if (listed != (dsd_signal_quiet_airspy_centred_rate_supported(rate) != 0)) {
            DSD_FPRINTF(stderr, "kCentredSpans is out of date: rate %u is %s here but %s by the shipped policy\n", rate,
                        listed ? "listed" : "not listed", listed ? "refused" : "admitted");
            return false;
        }
    }
    return true;
}

constexpr uint32_t kSceneRate = 1536000;    /* neighbour, slope and broad-interferer scenes, default geometry */
constexpr double kNoiseSigma = 1e-3;        /* per component; keeps a +50 dB neighbour far below the clip level */
constexpr double kMarginDb = 1.0;           /* active trials sit this far below each mode's reception threshold */
constexpr int kBlockIn = 9600;              /* interpolation block at 48 kHz: 200 ms, its middle half kept */
constexpr int kPsdPoints = 9600;            /* a waveform's 48 kHz spectrum, 5 Hz bins, for its in-width gain */
constexpr double kTrialSeconds = 0.128;     /* coloured-noise transform: longer than any nine sub-windows */
constexpr double kLongTrialSeconds = 0.768; /* the same, for --windows above nine: longer than any fifty */
constexpr double kEdgeSeconds = 0.002;      /* a shaped record's circular edges kept this far from the trial */

/* --windows: the sub-window count every request asks for, 0 for the default (DSD_SIGNAL_QUIET_WINDOWS). Set once
   from the command line, before any condition is built. */
int g_windows = 0;

int
windows_judged() {
    return g_windows ? g_windows : DSD_SIGNAL_QUIET_WINDOWS;
}

constexpr double kBurstGapSeconds = 0.0325; /* DMR MS: 60 ms frame less the 27.5 ms symbol transmission period */

/* ------------------------------------------------------------------------------------------------ modes */

struct Mode {
    std::string name;
    std::string row;         /* the source row that set the reception threshold */
    std::string tag;         /* the width's noise-only conditions (an analog mode's reception mode) */
    double reception_db = 0; /* S/N in the class width; DMR MS: burst-on */
    uint32_t width_hz = 0;
    uint8_t cls = 0;
    bool analog = false;
};

struct BuiltinMode {
    const char* name;
    const char* row;
    double reception_db;
    uint32_t width_hz;
    uint8_t cls;
    bool analog;
};

/* Reception thresholds: the "kept" thresholds tools/scan_quiet_reception.py writes to its thresholds.json, one run per
 * mode and per NFM and AM width the Radio sheet offers, as the reception table in docs/signal-search.md ("Reception
 * thresholds") lists them. X2-TDMA and ProVoice are unsourced: no check, so not calibrated. --modes-file replaces
 * them with a reception run's own (its driver_modes.tsv). */
const BuiltinMode kModes[] = {
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
/* The tags of the two digital classes' noise-only conditions (Builder::noise_requests()). */
constexpr const char* kNoiseTag12k5 = "12k5";
constexpr const char* kNoiseTag6k25 = "6k25";

std::vector<Mode>
builtin_modes() {
    std::vector<Mode> modes(std::end(kModes) - std::begin(kModes));
    std::transform(std::begin(kModes), std::end(kModes), modes.begin(), [](const BuiltinMode& m) {
        Mode out;
        out.name = out.tag = m.name;
        out.row = m.row;
        out.reception_db = m.reception_db;
        out.width_hz = m.width_hz;
        out.cls = m.cls;
        out.analog = m.analog;
        return out;
    });
    return modes;
}

/* ------------------------------------------------------------------------------------------------ sources */

/* One transmitter's clean samples at 48 kHz, shared by every sources.tsv row that holds the same samples. */
struct Waveform {
    std::vector<cf> base;
    std::vector<uint8_t> on;
    std::vector<double> psd; /* kPsdPoints bins, 0 Hz first; filled only when a response is rendered */
    double peak = 0;         /* the highest sample magnitude at 48 kHz (scene_bound()) */
};

struct Source {
    std::string row;
    double band_power = 0; /* tools/scan_quiet_reception.py band_power() in the row's class width */
    int wave = -1;
};

/* A waveform at one capture rate, with its bursts' symbol transmission periods in capture samples. */
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

/* The index of the waveform holding exactly @p w's samples and burst mask, appended when none does. */
int
share_waveform(Waveform&& w, std::vector<Waveform>* waves) {
    for (size_t i = 0; i < waves->size(); ++i) {
        const Waveform& o = (*waves)[i];
        if (o.base.size() == w.base.size() && o.on == w.on
            && std::memcmp(o.base.data(), w.base.data(), w.base.size() * sizeof(cf)) == 0) {
            return (int)i;
        }
    }
    waves->push_back(std::move(w));
    return (int)waves->size() - 1;
}

/* One sources.tsv row's samples (and burst mask) into a waveform; false when they cannot be read. */
bool
read_waveform(const std::string& dir, const std::string& row, const std::string& burst, Waveform* w) {
    std::vector<char> raw;
    if (!read_file(source_path(dir, row, ".cf32"), &raw) || raw.size() % sizeof(cf)) {
        DSD_FPRINTF(stderr, "cannot read %s.cf32\n", row.c_str());
        return false;
    }
    w->base.resize(raw.size() / sizeof(cf));
    DSD_MEMCPY(w->base.data(), raw.data(), raw.size());
    for (const cf& v : w->base) {
        w->peak = std::max(w->peak, (double)std::abs(v));
    }
    if (burst == "1") {
        std::vector<char> on;
        if (!read_file(source_path(dir, row, ".on"), &on) || on.size() != w->base.size()) {
            DSD_FPRINTF(stderr, "cannot read %s.on\n", row.c_str());
            return false;
        }
        w->on.assign(on.begin(), on.end());
    }
    return true;
}

/* Load the sources.tsv rows, sharing the waveforms of rows with the same samples. */
bool
load_sources(const std::string& dir, std::vector<Source>* sources, std::vector<Waveform>* waves) {
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
        Waveform w;
        if (!read_waveform(dir, row, burst, &w)) {
            return false;
        }
        s.wave = share_waveform(std::move(w), waves);
        sources->push_back(std::move(s));
    }
    return true;
}

int
find_source(const std::vector<Source>& sources, const std::string& row) {
    for (size_t i = 0; i < sources.size(); ++i) {
        if (sources[i].row == row) {
            return (int)i;
        }
    }
    return -1;
}

/* The class a modes file names: 12k5, 6k25 or analog; 0 for anything else. */
uint8_t
class_from_text(const std::string& text) {
    return text == "12k5"     ? (uint8_t)DSD_SIGNAL_QUIET_CLASS_12K5
           : text == "6k25"   ? (uint8_t)DSD_SIGNAL_QUIET_CLASS_6K25
           : text == "analog" ? (uint8_t)DSD_SIGNAL_QUIET_CLASS_ANALOG
                              : (uint8_t)0;
}

/* One driver_modes.tsv line into @p m; false when a field does not parse. */
bool
parse_mode_line(const std::string& line, Mode* m) {
    std::istringstream fields(line);
    std::string reception, width, cls, analog;
    std::getline(fields, m->name, '\t');
    std::getline(fields, m->tag, '\t');
    std::getline(fields, m->row, '\t');
    std::getline(fields, reception, '\t');
    std::getline(fields, width, '\t');
    std::getline(fields, cls, '\t');
    std::getline(fields, analog, '\t');
    double width_hz = 0;
    m->cls = class_from_text(cls);
    m->analog = analog == "1";
    if (m->name.empty() || m->tag.empty() || m->row.empty() || !parse_double(reception, &m->reception_db)
        || !parse_double(width, &width_hz) || width_hz < 1000 || width_hz > 100000
        || std::fabs(width_hz - std::round(width_hz)) > 1e-9 || m->cls == 0
        || m->analog != (m->cls == DSD_SIGNAL_QUIET_CLASS_ANALOG) || (analog != "0" && analog != "1")) {
        return false;
    }
    m->width_hz = (uint32_t)std::lround(width_hz);
    return true;
}

/* The modes of a reception run's driver_modes.tsv (--modes-file): they replace the built-in digital modes when the file
 * has any digital one, and the built-in analog modes when it has any analog one. */
bool
load_modes(const std::string& path, std::vector<Mode>* modes) {
    std::ifstream tsv(path);
    std::string line;
    if (!tsv || !std::getline(tsv, line)) {
        DSD_FPRINTF(stderr, "cannot read %s\n", path.c_str());
        return false;
    }
    std::vector<Mode> loaded;
    while (std::getline(tsv, line)) {
        Mode m;
        if (!parse_mode_line(line, &m)) {
            DSD_FPRINTF(stderr, "%s: bad line: %s\n", path.c_str(), line.c_str());
            return false;
        }
        loaded.push_back(std::move(m));
    }
    if (loaded.empty()) {
        DSD_FPRINTF(stderr, "no modes in %s\n", path.c_str());
        return false;
    }
    for (const bool analog : {false, true}) {
        if (std::any_of(loaded.begin(), loaded.end(), [&](const Mode& m) { return m.analog == analog; })) {
            modes->erase(
                std::remove_if(modes->begin(), modes->end(), [&](const Mode& m) { return m.analog == analog; }),
                modes->end());
        }
    }
    modes->insert(modes->end(), loaded.begin(), loaded.end());
    for (auto it = modes->begin(); it != modes->end(); ++it) {
        /* An analog mode under a digital class's noise tag would be folded into that class's noise-only conditions, at
         * no analog width. */
        if (it->analog && (it->tag == kNoiseTag12k5 || it->tag == kNoiseTag6k25)) {
            DSD_FPRINTF(stderr,
                        "%s: analog mode %s uses tag %s, which names the digital class's noise-only conditions\n",
                        path.c_str(), it->name.c_str(), it->tag.c_str());
            return false;
        }
        if (std::any_of(std::next(it), modes->end(), [&](const Mode& m) { return m.name == it->name; })) {
            DSD_FPRINTF(stderr, "%s: mode %s is named twice\n", path.c_str(), it->name.c_str());
            return false;
        }
        /* A tag names one width's noise-only conditions, which take the first mode's width under that tag. */
        const auto other = std::find_if(std::next(it), modes->end(),
                                        [&](const Mode& m) { return m.tag == it->tag && m.width_hz != it->width_hz; });
        if (other != modes->end()) {
            DSD_FPRINTF(stderr, "%s: tag %s names two widths: %u Hz (mode %s) and %u Hz (mode %s)\n", path.c_str(),
                        it->tag.c_str(), (unsigned)it->width_hz, it->name.c_str(), (unsigned)other->width_hz,
                        other->name.c_str());
            return false;
        }
    }
    return true;
}

struct AlignedBuffer {
    std::unique_ptr<cf[]> data;

    explicit AlignedBuffer(size_t n) : data(new cf[n]) {}
};

/* The samples place_waveform() makes of @p s at @p rate: one hop of output per hop of input. */
int64_t
placed_size(const Waveform& s, uint32_t rate) {
    const int64_t hop_in = kBlockIn / 2, hop_out = (int64_t)kBlockIn * rate / kSourceRate / 2;
    return (int64_t)s.base.size() / hop_in * hop_out;
}

/* A waveform's 48 kHz power spectrum: Hann-windowed kPsdPoints transforms over its whole length, averaged. */
void
fill_psd(Waveform* w) {
    dsd_io::FftSetupCache fft;
    AlignedBuffer buf((size_t)kPsdPoints);
    w->psd.assign((size_t)kPsdPoints, 0.0);
    for (size_t b = 0; b + kPsdPoints <= w->base.size(); b += kPsdPoints) {
        for (int i = 0; i < kPsdPoints; ++i) {
            const double h = 0.5 - 0.5 * std::cos(2 * kPi * (i + 0.5) / kPsdPoints);
            buf.data[i] = w->base[b + (size_t)i] * (float)h;
        }
        fft.forward(kPsdPoints, reinterpret_cast<float*>(buf.data.get()));
        for (int k = 0; k < kPsdPoints; ++k) {
            w->psd[(size_t)k] += std::norm(buf.data[k]);
        }
    }
    fft.release();
}

/* Band-limited interpolation of a 48 kHz waveform to @p rate: 200 ms blocks every 100 ms, each zero-padded in the
 * frequency domain, keeping each block's middle 100 ms (the waveform is read circularly; trials stay clear of the
 * wrap). The 48 kHz spectrum and so each band's power are kept; nothing appears above 24 kHz. */
Placed
place_waveform(const Waveform& s, uint32_t rate) {
    const int n_in = kBlockIn, n_out = (int)((int64_t)kBlockIn * rate / kSourceRate), hop_in = n_in / 2;
    const int hop_out = n_out / 2;
    const long long total_in = (long long)s.base.size();
    Placed p;
    p.x.assign((size_t)placed_size(s, rate), cf(0, 0));
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
        qs::inverse(large, n_out, out.data.get());
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

/* The bytes place_waveform() holds for @p w at @p rate. */
double
placed_bytes(const Waveform& w, uint32_t rate) {
    /* Whole blocks, each placed as a whole number of pairs. */
    const size_t blocks = w.base.size() / (size_t)(kBlockIn / 2);
    const int64_t placed_pairs = ((int64_t)kBlockIn * rate / kSourceRate) / 2;
    return (double)blocks * (double)placed_pairs * sizeof(cf);
}

/* ------------------------------------------------------------------------------------------------ conditions */

enum Kind { ACTIVE, NOISE, INFO };

enum Floor { WHITE, SLOPE, HUMP, OCCUPIED };

struct Carrier {
    int source = -1;
    double offset_hz = 0; /* from the channel */
    double share = 1;     /* of the wanted power (carriers of one scene share it) */
    double rel_db = 0;    /* a neighbour: its power over the wanted power, before the response */
    bool neighbour = false;
    bool droop = false; /* DMR MS: region B power at the mask limits */
    double gain = 1;    /* the response's power gain over the row's in-width power (1 with none) */
};

struct Condition {
    std::string name, group;
    std::string mode; /* the mode, or a noise-only condition's width or class tag (--pilot) */
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
    double floor_a = 0, floor_b = 0; /* SLOPE: dB per kHz; HUMP: centre offset Hz, peak dB; OCCUPIED: -, raise dB */
    std::vector<std::pair<double, double>> occupied; /* OCCUPIED: the raised hardware bands, Hz */
    double occupied_share = 0;                       /* OCCUPIED: the share of the reference span asked for */
    double occupied_length = 0;                      /* and the band length that asks for, Hz */
    bool occupied_both = false;                      /* asked for half on each side (else the upper side first) */
    uint8_t geometry = DSD_SIGNAL_GEOMETRY_DEFAULT;
    /* Derived */
    int span = 0;
    int record = 0; /* a shaped condition's circular record (0: rendered without a response) */
    double noise_in_width = 0;
    double bound = 0; /* scene_bound(): the highest sample component the scene can reach */
    double scale = 1; /* the whole scene's scale, under 1 only when the bound passes kSceneCeiling */
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
    if (c.floor == OCCUPIED) {
        /* Raised over hardware bands (the sample frequency less the rotation, folded), as a receiver's floor is. */
        const double rate = c.rate, rotation = c.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED ? 0.0 : rate / 4;
        double hardware = from_channel_hz + c.grid_shift_hz - rotation;
        hardware -= rate * std::floor((hardware + rate / 2) / rate);
        const bool raised = std::any_of(c.occupied.begin(), c.occupied.end(), [&](const std::pair<double, double>& b) {
            return hardware >= b.first && hardware < b.second;
        });
        return raised ? std::pow(10.0, c.floor_b / 10.0) : 1.0;
    }
    return 1.0;
}

double
channel_hz(const Condition& c) {
    return c.grid_shift_hz;
}

/* The ingest rotation of @p c's capture: Fs/4 shifted, none centred. */
double
rotation_hz(const Condition& c) {
    return c.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED ? 0.0 : c.rate / 4.0;
}

qs::Placement
placement(const Condition& c) {
    return qs::Placement{c.rate, rotation_hz(c), channel_hz(c), (double)c.snr_width};
}

int
transform_points(uint32_t rate) {
    return (int)std::lround(rate * (windows_judged() > DSD_SIGNAL_QUIET_WINDOWS ? kLongTrialSeconds : kTrialSeconds));
}

/* A shaped condition's circular record: the trial with kEdgeSeconds clear of either edge, in a size the transform
 * takes. */
int
record_points(const Condition& c) {
    return qs::record_size(c.span + 2 * (int)std::lround(c.rate * kEdgeSeconds));
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

/* The sub-windows' length for the request (nine, or --windows) (the detector's rule: the smallest whole number of transforms of at
 * least 10 ms each). */
int
quiet_span(const Condition& c) {
    const int n = fft_points(c);
    if (n <= 0) {
        return 0;
    }
    const long long frames = ((long long)c.rate + 100LL * n - 1) / (100LL * n);
    return (int)(windows_judged() * frames * n);
}

int
bin_hz(const Condition& c) {
    return (int)(c.rate / (uint32_t)fft_points(c));
}

/* Noise power inside the S/N width: white noise 2 sigma^2 W / rate as the reception tool's; a coloured or shaped floor
 * summed over the transform bins it is shaped on (a shaped one after the response, its ADC share included). */
double
noise_in_width(const Condition& c, const qs::Response& response) {
    if (c.floor == WHITE && !c.record) {
        return 2 * kNoiseSigma * kNoiseSigma * c.snr_width / c.rate;
    }
    const double centre = channel_hz(c) + c.snr_offset_hz;
    double sum = 0;
    if (!c.record) {
        const int n = transform_points(c.rate);
        for (int k = 0; k < n; ++k) {
            const double f = (k < n / 2 ? k : k - n) * (double)c.rate / n;
            if (std::fabs(f - centre) <= c.snr_width / 2.0) {
                sum += floor_shape(c, f - channel_hz(c));
            }
        }
        return 2 * kNoiseSigma * kNoiseSigma * sum / n;
    }
    /* Shaped: the record's bins inside the width only, the response's density and the ADC share at each. */
    const int n = c.record;
    const double bin = (double)c.rate / n, adc = response.adc ? std::pow(10.0, response.adc_db / 10.0) : 0.0;
    const qs::Placement p = placement(c);
    for (long long m = (long long)std::ceil((centre - c.snr_width / 2.0) / bin);
         m <= (long long)std::floor((centre + c.snr_width / 2.0) / bin); ++m) {
        const double f = (double)m * bin;
        sum += qs::response_power(response, p, qs::hardware_hz(f, p.rotation_hz, c.rate))
                   * floor_shape(c, f - channel_hz(c))
               + adc;
    }
    return 2 * kNoiseSigma * kNoiseSigma * sum / n;
}

/* The response's power gain over carrier @p k's row in its class width, weighted by its waveform's spectrum: what
 * the in-width power band_power() gave becomes after the response, at the carrier's hardware frequency. */
double
carrier_gain(const Condition& c, const Carrier& k, const Waveform& w, const qs::Response& response) {
    const qs::Placement p = placement(c);
    const double bin = (double)kSourceRate / kPsdPoints;
    const int half = std::min(kPsdPoints / 2 - 1, (int)std::floor(c.snr_width / 2.0 / bin));
    double in = 0, out = 0;
    for (int m = -half; m <= half; ++m) {
        const double psd = w.psd[(size_t)((m + kPsdPoints) % kPsdPoints)];
        const double hw = qs::hardware_hz(channel_hz(c) + k.offset_hz + m * bin, p.rotation_hz, c.rate);
        in += psd;
        out += psd * qs::response_power(response, p, hw);
    }
    return in > 0 ? out / in : 1.0;
}

const char*
class_text(uint8_t classes) {
    switch (classes) {
        case DSD_SIGNAL_QUIET_CLASS_12K5: return "12k5";
        case DSD_SIGNAL_QUIET_CLASS_6K25: return "6k25";
        case DSD_SIGNAL_QUIET_CLASS_ANALOG: return "analog";
        case DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25: return "12k5+6k25";
        case DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_ANALOG: return "12k5+analog";
        case DSD_SIGNAL_QUIET_CLASS_6K25 | DSD_SIGNAL_QUIET_CLASS_ANALOG: return "6k25+analog";
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
    const std::vector<Mode>& modes;
    const std::vector<uint32_t>& rates;
    const std::vector<uint32_t>& scene_rates;
    uint8_t geometry;
    std::vector<Condition> out;

    Condition
    base(const char* group, const Mode& m, uint32_t rate) const {
        Condition c;
        c.group = group;
        c.mode = m.name;
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
        for (uint32_t rate : rates) {
            for (int i = 0; i < count; ++i) {
                Condition c = base("digital", m, rate);
                c.carriers[0].offset_hz = c.snr_offset_hz = offsets[i];
                add(c, "off" + signed_text(offsets[i], 0));
                if (m.name == "dmr_ms") {
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
        for (uint32_t rate : rates) {
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

    /* A scene's name: its carriers' offsets in kHz, "-5.0k+5.0k". */
    static std::string
    scene_tag(const std::vector<double>& scene) {
        std::string tag;
        for (double f : scene) {
            tag += signed_text(f / 1000, 1) + "k";
        }
        return tag;
    }

    void
    offset_carriers() {
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
        const Mode* am = nullptr;
        for (const Mode& m : modes) {
            if (m.row == kOffsetCarrierRow) {
                am = &m;
            }
        }
        if (!am) {
            /* A modes file without the row: its scenes are left out, and the run says so. */
            std::string dropped;
            for (const auto& scene : scenes) {
                dropped += (dropped.empty() ? "" : " ") + scene_tag(scene);
            }
            DSD_FPRINTF(stderr,
                        "no mode uses the %s row: left out %zu am_offset_carrier conditions, the scenes %s at %zu "
                        "rates\n",
                        kOffsetCarrierRow, scenes.size() * rates.size(), dropped.c_str(), rates.size());
            return;
        }
        for (uint32_t rate : rates) {
            for (const auto& scene : scenes) {
                Condition c = base("am_offset_carrier", *am, rate);
                c.carriers.clear();
                for (double f : scene) {
                    c.carriers.push_back(Carrier{find_source(sources, am->row), f, 1.0 / scene.size()});
                }
                add(c, scene_tag(scene));
            }
        }
    }

    void
    neighbours(const Mode& m) {
        const int nb = find_source(sources, kNeighbourRow);
        for (uint32_t rate : scene_rates) {
            for (double spacing : {12500.0, 25000.0}) {
                for (double level : neighbour_levels) {
                    for (int side : {1, -1, 0}) {
                        Condition c = base("neighbour", m, rate);
                        for (int s : {1, -1}) {
                            if (side == 0 || side == s) {
                                Carrier n{nb, s * spacing, 0, level, true};
                                c.carriers.push_back(n);
                            }
                        }
                        const char* where = side > 0 ? "upper" : side < 0 ? "lower" : "both";
                        /* whole levels keep the first calibration's names (and so its seeds): "+30dB"; others
                         * "+7.5dB" */
                        const int decimals = std::fabs(level - std::round(level)) < 1e-9 ? 0 : 1;
                        add(c, signed_text(spacing / 1000, 1).substr(1) + "k/" + signed_text(level, decimals) + "dB/"
                                   + where);
                    }
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
        for (uint32_t rate : scene_rates) {
            for (const Shape& s : shapes) {
                Condition c = base(s.floor == SLOPE ? "slope" : "broad", m, rate);
                if (kind == INFO) {
                    c.group = s.floor == SLOPE ? "noise_slope" : "noise_broad";
                    c.name = c.group + "/" + m.tag + "/" + std::to_string(rate / 1000) + "k";
                    c.mode = m.tag;
                    c.carriers.clear();
                }
                c.kind = kind;
                c.floor = s.floor;
                c.floor_a = s.a;
                c.floor_b = s.b;
                add(c, s.tag);
            }
        }
    }

    /* The analog monitor's masks (-8: the decoders' class beside the analog width, scan_quiet_monitor_classes()),
       plain scenes at every rate: each digital mode judged in its own class and every analog width, and in both digital
       classes and every width; each analog mode judged in its width beside either digital class; noise alone under
       each two-class mask and width. Names start with the group, so none meets an existing condition's name (or seed).
     */
    void
    masks() {
        const std::vector<Request> requests = noise_requests();
        for (uint32_t rate : rates) {
            for (const Request& r : requests) {
                if (!r.width) {
                    continue;
                }
                for (const Mode& m : modes) {
                    if (m.analog) {
                        continue;
                    }
                    for (uint8_t digital :
                         {m.cls, (uint8_t)(DSD_SIGNAL_QUIET_CLASS_12K5 | DSD_SIGNAL_QUIET_CLASS_6K25)}) {
                        Condition c = base("mask_digital", m, rate);
                        c.classes = digital | DSD_SIGNAL_QUIET_CLASS_ANALOG;
                        c.analog_width = r.width;
                        add(c, std::string(class_text(c.classes)) + "/" + r.tag);
                    }
                }
                for (uint8_t digital : {(uint8_t)DSD_SIGNAL_QUIET_CLASS_12K5, (uint8_t)DSD_SIGNAL_QUIET_CLASS_6K25}) {
                    Condition c;
                    c.group = "noise_mask";
                    c.mode = r.tag;
                    c.kind = NOISE;
                    c.rate = rate;
                    c.classes = digital | DSD_SIGNAL_QUIET_CLASS_ANALOG;
                    c.analog_width = r.width;
                    c.snr_width = r.width;
                    c.name = "noise_mask/" + std::string(class_text(c.classes)) + "/" + r.tag + "/"
                             + std::to_string(rate / 1000) + "k";
                    out.push_back(c);
                }
            }
            for (const Mode& m : modes) {
                if (!m.analog) {
                    continue;
                }
                for (uint8_t digital : {(uint8_t)DSD_SIGNAL_QUIET_CLASS_12K5, (uint8_t)DSD_SIGNAL_QUIET_CLASS_6K25}) {
                    Condition c = base("mask_analog", m, rate);
                    c.classes |= digital;
                    add(c, class_text(c.classes));
                }
            }
        }
    }

    /* The raised hardware bands that put @p share of the capture-wide reference's span over the noise, clear of the
       channel and its sidebands (@p clear_widths of the widest width a side; 3.5 keeps the sideband blocks clean, 1.25
       raises them and leaves only the channel): on the upper side first (@p both false) or half on each side, each
       side's overflow on the other. */
    std::vector<std::pair<double, double>>
    occupied_bands(uint32_t rate, double widest, double share, bool both, double clear_widths = 3.5,
                   double* asked_length = nullptr) const {
        const bool centred = geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED;
        const auto entry = std::find_if(std::begin(kCentredSpans), std::end(kCentredSpans),
                                        [&](const CentredSpan& e) { return e.rate == rate; });
        const double span = (centred && entry != std::end(kCentredSpans) ? entry->span : kDefaultSpan) * rate;
        const double channel = centred ? 0.0 : -(double)rate / 4, clear = clear_widths * widest;
        const double upper_room = span - (channel + clear), lower_room = (channel - clear) + span;
        const double length = share * (2 * span - widest);
        if (asked_length) {
            *asked_length = length;
        }
        double up = both ? length / 2 : length, down = both ? length / 2 : 0;
        if (up > upper_room) {
            down += up - upper_room;
            up = upper_room;
        }
        if (down > lower_room) {
            up = std::min(upper_room, up + down - lower_room);
            down = lower_room;
        }
        std::vector<std::pair<double, double>> bands;
        if (up > 0) {
            bands.emplace_back(channel + clear, channel + clear + up);
        }
        if (down > 0) {
            bands.emplace_back(channel - clear - down, channel - clear);
        }
        return bands;
    }

    /* Occupied captures (issue #834 B7): a share of the reference span raised by a few dB to well over the noise, clear
       of the channel and its sidebands, at every rate; active rows at 1 dB under reception, and (INFO, the check's
       speed cost) noise alone under each row's class or width. */
    void
    occupancy(const Mode& m, Kind kind) {
        static const double shares[] = {0.10, 0.20, 0.33, 0.50, 0.70};
        static const double raises[] = {1.5, 3.0, 10.0};
        for (uint32_t rate : rates) {
            for (double share : shares) {
                for (double raise : raises) {
                    for (bool both : {false, true}) {
                        Condition c = base("occupancy", m, rate);
                        if (kind == INFO) {
                            /* noise alone under the row's class (a digital row) or width (an analog row's tag) */
                            const std::string tag = m.analog ? m.tag : class_text(m.cls);
                            c.group = "noise_occupancy";
                            c.name = c.group + "/" + tag + "/" + std::to_string(rate / 1000) + "k";
                            c.mode = tag;
                            c.carriers.clear();
                        }
                        c.kind = kind;
                        c.floor = OCCUPIED;
                        c.floor_b = raise;
                        c.occupied_share = share;
                        c.occupied_both = both;
                        c.occupied = occupied_bands(rate, m.width_hz, share, both, 3.5, &c.occupied_length);
                        add(c, "p" + std::to_string((int)std::lround(share * 100)) + "/" + signed_text(raise, 1) + "dB/"
                                   + (both ? "both" : "upper"));
                    }
                }
            }
        }
    }

    /* The raised-sideband class (informational, issue #834 review): 80 to 99% of the reference span raised by 1.5 to
       10 dB, the raise reaching the channel's sidebands (clear 1.25 widths a side) and leaving only the channel on the
       noise. The occupancy check does not see it and the channel reads low by about the raise, a documented limit; the
       group measures how low (active rows, 1 dB under reception; no gate reads it). */
    void
    raised(const Mode& m) {
        static const double shares[] = {0.80, 0.90, 0.95, 0.99};
        static const double raises[] = {1.5, 3.0, 6.0, 10.0};
        for (uint32_t rate : rates) {
            for (double share : shares) {
                for (double raise : raises) {
                    for (bool both : {false, true}) {
                        Condition c = base("raised", m, rate);
                        c.floor = OCCUPIED;
                        c.floor_b = raise;
                        c.occupied_share = share;
                        c.occupied_both = both;
                        c.occupied = occupied_bands(rate, m.width_hz, share, both, 1.25, &c.occupied_length);
                        add(c, "p" + std::to_string((int)std::lround(share * 100)) + "/" + signed_text(raise, 1) + "dB/"
                                   + (both ? "both" : "upper"));
                    }
                }
            }
        }
    }

    /* The -8 masks crossed with the scenes the plain masks leave out: each digital mode judged in its class beside the
       narrowest and the widest analog width, with neighbours (12.5 and 25 kHz away, +10, +30 and +50 dB, upper, lower
       and both; at the neighbour scenes' rates), offset from the channel (the digital offsets but 0), and a DMR
       mobile's droop at each offset. Names start with the group, so none meets an existing condition's name or seed. */
    void
    cross() {
        std::vector<Request> widths;
        for (const Request& r : noise_requests()) {
            if (r.width) {
                widths.push_back(r);
            }
        }
        if (widths.empty()) {
            return;
        }
        const auto by_width = [](const Request& a, const Request& b) { return a.width < b.width; };
        std::vector<Request> masks = {*std::min_element(widths.begin(), widths.end(), by_width)};
        const Request wide = *std::max_element(widths.begin(), widths.end(), by_width);
        if (wide.width != masks[0].width) {
            masks.push_back(wide); /* one width only: one mask */
        }
        static const double offsets[] = {-500, 500, -1000, 1000, -2500, 2500};
        const int nb = find_source(sources, kNeighbourRow);
        for (const Mode& m : modes) {
            if (m.analog) {
                continue;
            }
            for (const Request& r : masks) {
                const auto masked = [&](uint32_t rate) {
                    Condition c = base("mask_cross", m, rate);
                    c.classes = m.cls | DSD_SIGNAL_QUIET_CLASS_ANALOG;
                    c.analog_width = r.width;
                    c.name += "/" + std::string(class_text(c.classes)) + "/" + r.tag;
                    return c;
                };
                for (uint32_t rate : scene_rates) {
                    for (double spacing : {12500.0, 25000.0}) {
                        for (double level : {10.0, 30.0, 50.0}) {
                            for (int side : {1, -1, 0}) {
                                Condition c = masked(rate);
                                for (int sgn : {1, -1}) {
                                    if (side == 0 || side == sgn) {
                                        c.carriers.push_back(Carrier{nb, sgn * spacing, 0, level, true});
                                    }
                                }
                                const char* where = side > 0 ? "upper" : side < 0 ? "lower" : "both";
                                add(c, signed_text(spacing / 1000, 1).substr(1) + "k/" + signed_text(level, 0) + "dB/"
                                           + where);
                            }
                        }
                    }
                }
                const int count = m.cls == DSD_SIGNAL_QUIET_CLASS_6K25 ? 6 : 4;
                for (uint32_t rate : rates) {
                    for (int i = 0; i < count; ++i) {
                        Condition c = masked(rate);
                        c.carriers[0].offset_hz = c.snr_offset_hz = offsets[i];
                        add(c, "off" + signed_text(offsets[i], 0));
                    }
                    if (m.name == "dmr_ms") {
                        for (double off : {0.0, -500.0, 500.0, -1000.0, 1000.0}) {
                            Condition c = masked(rate);
                            c.carriers[0].offset_hz = c.snr_offset_hz = off;
                            c.carriers[0].droop = true;
                            add(c, "droop/off" + signed_text(off, 0));
                        }
                    }
                }
            }
        }
    }

    struct Request {
        uint8_t classes = 0;
        uint32_t width = 0;
        std::string tag;
    };

    /* The noise-only requests: both digital classes and each analog mode's width (once per tag). */
    std::vector<Request>
    noise_requests() const {
        std::vector<Request> requests = {{DSD_SIGNAL_QUIET_CLASS_12K5, 0, kNoiseTag12k5},
                                         {DSD_SIGNAL_QUIET_CLASS_6K25, 0, kNoiseTag6k25}};
        for (const Mode& m : modes) {
            const bool known =
                std::any_of(requests.begin(), requests.end(), [&](const Request& r) { return r.tag == m.tag; });
            if (m.analog && !known) {
                requests.push_back(Request{DSD_SIGNAL_QUIET_CLASS_ANALOG, m.width_hz, m.tag});
            }
        }
        return requests;
    }

    void
    noise_only() {
        const std::vector<Request> requests = noise_requests();
        for (uint32_t rate : rates) {
            for (const Request& r : requests) {
                Condition c;
                c.group = "noise";
                c.mode = r.tag;
                c.kind = NOISE;
                c.rate = rate;
                c.classes = r.classes;
                c.analog_width = r.width;
                c.snr_width = r.width ? r.width : 12500;
                const int bin = bin_hz(c);
                for (int shift : {0, bin / 4, bin / 2}) {
                    c.grid_shift_hz = shift;
                    c.name = "noise/" + r.tag + "/" + std::to_string(rate / 1000) + "k/grid" + std::to_string(shift);
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

/* ------------------------------------------------------------------------------------------------ options */

constexpr double kGuardOff = 1000;       /* a guard no floor reaches: the raw ratios and rises */
constexpr double kSceneCeiling = 0.9;    /* a scene's bound is kept under this, clear of the 0.999 clip level */
constexpr float kInvalidBusyDb = 999.0f; /* a noise-only trial the guard voided, in the select quantiles */

struct Options {
    std::string sources, phase, out, filter, dump, modes_file, pilot, trial_log, bounds;
    long trials = 0, jobs = 0, memory_mb = 8192, seed_salt = 0;
    double threshold = 0, guard = kGuardOff, occupancy_veto = 0;
    bool threshold_given = false; /* --threshold: validation only */
    /* --scene-ceiling: the bound a scene is scaled under (kSceneCeiling); only a check of the clip refusal raises it */
    double scene_ceiling = kSceneCeiling;
    bool ceiling_given = false;
    /* --add masks,occupancy,cross,raised: the groups beyond the shipped matrix */
    bool add_masks = false, add_occupancy = false, add_cross = false, add_raised = false;
    int estimator = -1;
    std::vector<double> neighbour_levels{30.0, 50.0};
    uint8_t geometry = DSD_SIGNAL_GEOMETRY_DEFAULT;
    std::vector<uint32_t> rates, scene_rates; /* rates_given: from --rates, else the geometry's defaults */
    bool rates_given = false;
    qs::Response response;
};

const char*
geometry_text(uint8_t geometry) {
    return geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED ? "centred" : "default";
}

/* The run's tag columns every CSV row ends with. */
std::string
run_columns(const Options& o) {
    char text[96];
    char adc[16] = "none";
    if (o.response.adc) {
        DSD_SNPRINTF(adc, sizeof(adc), "%.1f", o.response.adc_db);
    }
    DSD_SNPRINTF(text, sizeof(text), "%s,%s,%.1f,%s", geometry_text(o.geometry), qs::shape_text(o.response.shape),
                 o.response.depression_db, adc);
    return text;
}

/* What the run executes, as the run and completion lines print it: phase, salt, veto, guard, threshold, estimator,
   the run's tag columns and, for a --windows run only, its sub-window count (a run of the calibrated nine prints the
   line it always did), and for a --scene-ceiling run its ceiling. */
std::string
run_text(const Options& o) {
    char windows[24] = "";
    if (g_windows) {
        DSD_SNPRINTF(windows, sizeof(windows), " windows=%d", g_windows);
    }
    char ceiling[32] = "";
    if (o.ceiling_given) {
        DSD_SNPRINTF(ceiling, sizeof(ceiling), " ceiling=%.6g", o.scene_ceiling);
    }
    char text[288];
    DSD_SNPRINTF(text, sizeof(text), "phase=%s salt=%ld veto=%.6g guard=%.6g threshold=%.6g estimator=%s %s%s%s",
                 o.phase.c_str(), o.seed_salt, o.occupancy_veto, o.guard, o.threshold,
                 o.estimator == DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN   ? "trimmed"
                 : o.estimator == DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK ? "lowest"
                                                                      : "both",
                 run_columns(o).c_str(), windows, ceiling);
    return text;
}

/* SHA-256 (FIPS 180-4) of the bytes given in turn, as lowercase hex: the completion record's digest of an output,
   taken as the run writes it, so an output it cannot read back (a FIFO) gets one too. */
class Sha256 {
  public:
    void
    update(const void* data, size_t size) {
        const auto* p = static_cast<const unsigned char*>(data);
        total_ += size;
        while (size > 0) {
            const size_t take = std::min(size, sizeof(buf_) - have_);
            std::copy(p, p + take, buf_ + have_);
            have_ += take;
            p += take;
            size -= take;
            if (have_ == sizeof(buf_)) {
                block(buf_);
                have_ = 0;
            }
        }
    }

    /* The digest of everything given; the hasher is spent after it. */
    std::string
    hex() {
        unsigned char tail[128] = {};
        std::copy(buf_, buf_ + have_, tail);
        tail[have_] = 0x80;
        const size_t len = have_ + 1 + 8 <= 64 ? 64 : 128;
        const uint64_t bits = total_ * 8;
        for (int i = 0; i < 8; ++i) {
            tail[len - 1 - i] = (unsigned char)(bits >> (8 * i));
        }
        block(tail);
        if (len == 128) {
            block(tail + 64);
        }
        std::string out;
        char part[9];
        for (uint32_t v : h_) {
            DSD_SNPRINTF(part, sizeof(part), "%08x", v);
            out += part;
        }
        return out;
    }

  private:
    static uint32_t
    rotr(uint32_t x, int n) {
        return (x >> n) | (x << (32 - n));
    }

    void
    block(const unsigned char* p) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        const uint32_t add[8] = {a, b, c, d, e, f, g, hh};
        for (int i = 0; i < 8; ++i) {
            h_[i] += add[i];
        }
    }

    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char buf_[64] = {};
    size_t have_ = 0;
    uint64_t total_ = 0;
};

/* Whether @p c belongs to the pilot mode @p m: its own conditions, and its class's or width's noise-only ones. */
bool
pilot_match(const Condition& c, const Mode& m) {
    /* A digital mode's class: its noise-only conditions are tagged by the class, its shaped-floor (INFO) ones by the
       mode that built them for the class (P25 Phase 1, NXDN48). */
    const bool same_class = !m.analog && c.classes == m.cls && c.analog_width == 0;
    return c.mode == m.name || (c.kind != ACTIVE && (c.mode == m.tag || (!m.analog && c.mode == class_text(m.cls))))
           || (c.kind == INFO && same_class);
}

/* The highest noise density across the capture, over the tuner noise's unit density: the response (when rendered)
 * times the floor's shape, plus the ADC share, sampled at 2048 points. */
double
noise_peak_density(const Condition& c, const qs::Response& response) {
    const qs::Placement p = placement(c);
    const double adc = c.record && response.adc ? std::pow(10.0, response.adc_db / 10.0) : 0.0;
    double peak = 0;
    for (int i = 0; i < 2048; ++i) {
        const double f = (i - 1024) * (double)c.rate / 2048;
        const double gain = c.record ? qs::response_power(response, p, qs::hardware_hz(f, p.rotation_hz, c.rate)) : 1.0;
        peak = std::max(peak, gain * floor_shape(c, f - channel_hz(c)) + adc);
    }
    return peak;
}

constexpr double kPeakMargin = 1.1;   /* a placed waveform's interpolation and a response's ringing over its peak */
constexpr double kDroopPeak = 1.13;   /* a DMR MS droop carrier's region B at +1 dBp, in amplitude */
constexpr double kNoisePeakSigma = 6; /* noise components this far out are not reached in any trial's samples */

/* An upper bound on any sample component @p c renders, at its scale 1: every carrier's amplitude (before the
 * response, which only lowers it) at its waveform's peak, plus kNoisePeakSigma of the noise at its densest. A scene
 * whose bound passes kSceneCeiling is scaled down whole (Condition.scale), which leaves every ratio the detector
 * judges as it was and keeps every sample under the clip level; the ADC share's extra noise raised the scenes with
 * the loudest neighbours over it before (issue #834 B8). */
double carrier_power(const Condition& c, const Carrier& k);

double
scene_bound(const Condition& c, const std::vector<Source>& sources, const std::vector<Waveform>& waves,
            const qs::Response& response) {
    double bound = kNoisePeakSigma * kNoiseSigma * std::sqrt(noise_peak_density(c, response));
    for (const Carrier& k : c.carriers) {
        if (k.source < 0) {
            continue;
        }
        const Source& src = sources[(size_t)k.source];
        const double amplitude = std::sqrt(carrier_power(c, k) / src.band_power);
        bound += amplitude * waves[(size_t)src.wave].peak * kPeakMargin * (k.droop ? kDroopPeak : 1.0);
    }
    return bound;
}

void
derive(std::vector<Condition>* all, const Options& o, const std::vector<Source>& sources,
       const std::vector<Waveform>& waves) {
    const bool shaped = o.response.shape != qs::Shape::FLAT;
    for (Condition& c : *all) {
        c.geometry = o.geometry;
        c.span = quiet_span(c);
        c.record = shaped ? record_points(c) : 0;
        c.noise_in_width = noise_in_width(c, o.response);
        for (Carrier& k : c.carriers) {
            if (shaped && !k.neighbour && k.source >= 0) {
                const Source& s = sources[(size_t)k.source];
                k.gain = carrier_gain(c, k, waves[(size_t)s.wave], o.response);
            }
        }
        c.bound = scene_bound(c, sources, waves, o.response);
        c.scale = c.bound > o.scene_ceiling ? o.scene_ceiling / c.bound : 1.0;
    }
}

/* The rows the occupied captures render (the lowest-margin rows of each class and kind), and the modes whose class or
 * width their noise-only conditions take. */
const char* const kOccupancyRows[] = {"m17",   "p25p1",   "nxdn48",       "dpmr",
                                      "dstar", "nfm_25k", "nfm_20k_wide", "am_5k_edge"};
const char* const kOccupancyNoise[] = {"m17", "nxdn48", "nfm_25k", "nfm_20k_wide", "am_5k_edge"};

/* --add's groups, after the shipped matrix (so its conditions keep their order, names and seeds). */
void
add_groups(Builder* b, const Options& o) {
    if (o.add_masks) {
        b->masks();
    }
    if (!o.add_occupancy) {
        return;
    }
    for (const Mode& m : b->modes) {
        const auto named = [&](const char* name) { return m.name == name; };
        if (std::any_of(std::begin(kOccupancyRows), std::end(kOccupancyRows), named)) {
            b->occupancy(m, ACTIVE);
        }
        if (std::any_of(std::begin(kOccupancyNoise), std::end(kOccupancyNoise), named)) {
            b->occupancy(m, INFO);
        }
    }
}

/* After every condition the build made before them, so those keep their order, names and seeds: --add occupancy's
   noise alone under every other noise tag (each analog width's first mode; the occupied captures' noise-only
   conditions under all 18 tags), then --add cross and --add raised. */
void
add_late_groups(Builder* b, const Options& o) {
    if (o.add_occupancy) {
        std::vector<std::string> noise_tags;
        for (const Mode& m : b->modes) {
            const auto named = [&](const char* name) { return m.name == name; };
            if (std::any_of(std::begin(kOccupancyNoise), std::end(kOccupancyNoise), named)) {
                noise_tags.push_back(m.analog ? m.tag : class_text(m.cls));
            }
        }
        for (const Mode& m : b->modes) {
            const std::string tag = m.analog ? m.tag : class_text(m.cls);
            if (std::find(noise_tags.begin(), noise_tags.end(), tag) == noise_tags.end()) {
                b->occupancy(m, INFO);
                noise_tags.push_back(tag);
            }
        }
    }
    if (o.add_cross) {
        b->cross();
    }
    if (!o.add_raised) {
        return;
    }
    for (const Mode& m : b->modes) {
        const auto named = [&](const char* name) { return m.name == name; };
        if (std::any_of(std::begin(kOccupancyRows), std::end(kOccupancyRows), named)) {
            b->raised(m);
        }
    }
}

std::vector<Condition>
build_conditions(const std::vector<Source>& sources, const std::vector<Waveform>& waves, const std::vector<Mode>& modes,
                 const Options& o) {
    Builder b{sources, o.neighbour_levels, modes, o.rates, o.scene_rates, o.geometry, {}};
    for (const Mode& m : modes) {
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
    add_groups(&b, o);
    /* Noise alone on the shaped floors: once per width (the first mode of each analog tag), and P25 Phase 1's and
       NXDN48's digital classes. */
    std::vector<std::string> floored;
    for (const Mode& m : modes) {
        const bool first = std::find(floored.begin(), floored.end(), m.tag) == floored.end();
        if (first && (m.name == "p25p1" || m.name == "nxdn48" || m.analog)) {
            floored.push_back(m.tag);
            b.floors(m, INFO);
        }
    }
    add_late_groups(&b, o);
    derive(&b.out, o, sources, waves);
    return b.out;
}

/* ------------------------------------------------------------------------------------------------ trials */

uint64_t
fnv1a(const std::string& text) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : text) {
        h = (h ^ ch) * 1099511628211ULL;
    }
    return h;
}

/* A shaped condition's response over its record, and whether every carrier's content lies where the response is within
 * kUnityTolerance of unity (carrier_unshaped()). */
struct RecordBins {
    qs::Bins bins;
    bool carriers_unshaped = false;
};

struct Worker {
    std::vector<cf> trial, noise, record;
    dsd_io::FftSetupCache fft;
    qs::TransformCache shapes; /* the shaped records' transforms */
    dsd_signal_quiet_detector* detectors[2] = {nullptr, nullptr};
    RecordBins bins; /* the response's bins for bins_for's record (the snr phase) */
    const Condition* bins_for = nullptr;

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

/* What a trial renders from: the run's sources, their waveforms placed at the trial's rate, and the response. */
struct Scene {
    const std::vector<Source>& sources;
    const std::vector<Placed>& placed; /* by waveform */
    const qs::Response& response;
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
    qs::inverse(w.fft, n, w.noise.data());
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

/* The starts [first, last) a @p span-sample trial at @p rate is drawn from, in a placed waveform of @p size samples
 * whose burst train (when @p bursts) runs from capture sample @p burst_on to @p burst_off: clear of the placement's
 * wrap at both ends, and for a burst train starting no earlier than one inter-burst gap before the first burst, as
 * at any phase of the train, and ending inside the last burst. Empty (last <= first) when the waveform is too short
 * for the span; choose() refuses such a condition before any trial runs. */
std::pair<int64_t, int64_t>
start_range(int64_t size, bool bursts, int64_t burst_on, int64_t burst_off, uint32_t rate, int span) {
    int64_t first = (int64_t)rate / 10, last = size - span - (int64_t)rate / 10;
    if (bursts) {
        first = std::max(first, burst_on - (int64_t)(kBurstGapSeconds * rate));
        last = std::min(last, burst_off - span);
    }
    return {first, last};
}

/* start_range() for @p s placed at @p rate, from its 48 kHz samples and burst mask as place_waveform() maps them. */
std::pair<int64_t, int64_t>
source_start_range(const Waveform& s, uint32_t rate, int span) {
    const auto keyed = [](uint8_t v) { return v != 0; };
    const auto on = std::find_if(s.on.begin(), s.on.end(), keyed);
    if (on == s.on.end()) {
        return start_range(placed_size(s, rate), false, 0, 0, rate, span);
    }
    const auto off = std::find_if(s.on.rbegin(), s.on.rend(), keyed).base();
    return start_range(placed_size(s, rate), true, (int64_t)(on - s.on.begin()) * rate / kSourceRate,
                       (int64_t)(off - s.on.begin()) * rate / kSourceRate, rate, span);
}

/* Carrier @p k's power at the antenna: a wanted carrier's share of the wanted power over its gain through the
 * response, a neighbour's level over the first wanted carrier's power there. */
double
carrier_power(const Condition& c, const Carrier& k) {
    const double wanted = std::pow(10.0, c.snr_db / 10.0) * c.noise_in_width;
    return k.neighbour ? wanted * std::pow(10.0, k.rel_db / 10.0) / c.carriers[0].gain : wanted * k.share / k.gain;
}

/* Add one carrier at a random start, phase and (DMR MS droop) side into @p out: @p length samples from @p lead before
 * the trial's start. Returns the trial's start in the placed waveform. */
int64_t
add_carrier(const Condition& c, const Carrier& k, const Scene& s, Rng& rng, cf* out, int lead, int length) {
    const Source& src = s.sources[(size_t)k.source];
    const Placed& p = s.placed[(size_t)src.wave];
    const double amplitude = std::sqrt(carrier_power(c, k) / src.band_power);
    const bool bursts = !p.bursts.empty();
    const std::pair<int64_t, int64_t> range =
        start_range((int64_t)p.x.size(), bursts, bursts ? p.bursts.front().first : 0,
                    bursts ? p.bursts.back().second : 0, c.rate, c.span);
    const int64_t first = range.first;
    const int64_t start = first + (int64_t)(rng.next() % (uint64_t)(range.second - first));
    const double step = 2 * kPi * (channel_hz(c) + k.offset_hz) / c.rate;
    std::complex<double> rot = std::polar(amplitude, 2 * kPi * rng.uniform());
    const std::complex<double> turn = std::polar(1.0, step);
    const bool late = (rng.next() & 1) != 0;
    if (lead > 0) {
        rot *= std::polar(1.0, -lead * step);
    }
    for (int i = 0; i < length; ++i) {
        const int64_t at = start - lead + i;
        double gain = 1.0;
        if (k.droop) {
            gain = droop_gain(p, at, late);
        }
        const std::complex<double> v(p.x[(size_t)at].real(), p.x[(size_t)at].imag());
        const std::complex<double> x = v * rot * gain;
        out[i] += cf((float)x.real(), (float)x.imag());
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

constexpr double kSourceBandHz = kSourceRate / 2.0; /* a placed waveform's content, either side of its carrier */
constexpr float kUnityTolerance = 1e-6f;            /* a signal bin this close to 1 leaves the carrier as it is */

/* Whether carrier @p k of @p c sees a response within kUnityTolerance of unity over all of its content. A placed
 * waveform's content lies within kSourceBandHz of its carrier, but a DMR MS droop carrier's is not confined there:
 * add_carrier() applies its droop step at the capture rate, which spreads it across the capture, so it always goes
 * through the response. */
bool
carrier_unshaped(const Condition& c, const Carrier& k, const qs::Bins& bins) {
    if (k.droop) {
        return false;
    }
    const long long n = c.record;
    const double bin = (double)c.rate / (double)n, f = channel_hz(c) + k.offset_hz;
    const long long last = (long long)std::ceil((f + kSourceBandHz) / bin);
    for (long long m = (long long)std::floor((f - kSourceBandHz) / bin); m <= last; ++m) {
        if (std::fabs(bins.signal[(size_t)((m % n + n) % n)] - 1.0f) > kUnityTolerance) {
            return false;
        }
    }
    return true;
}

/* The response's bins over @p c's record. */
void
record_bins(const Condition& c, const qs::Response& response, RecordBins* out) {
    const std::function<double(double)> colour = [&c](double f) { return floor_shape(c, f - channel_hz(c)); };
    qs::response_bins(response, placement(c), c.record, c.floor == WHITE ? nullptr : &colour, &out->bins);
    out->carriers_unshaped = std::all_of(c.carriers.begin(), c.carriers.end(),
                                         [&](const Carrier& k) { return carrier_unshaped(c, k, out->bins); });
}

/* The response's bins for @p c's record, kept by the worker while its renders run (the snr phase). */
const RecordBins&
condition_bins(const Condition& c, const qs::Response& response, Worker& w) {
    if (w.bins_for != &c) {
        record_bins(c, response, &w.bins);
        w.bins_for = &c;
    }
    return w.bins;
}

/* Add @p c's transmitters (all but neighbours when @p wanted_only) to @p out; the first one's start, or -1. */
int64_t
add_carriers(const Condition& c, const Scene& s, Rng& rng, bool wanted_only, cf* out, int lead, int length) {
    int64_t first = -1;
    bool any = false;
    for (const Carrier& k : c.carriers) {
        if (wanted_only && k.neighbour) {
            continue;
        }
        const int64_t start = add_carrier(c, k, s, rng, out, lead, length);
        first = any ? first : start;
        any = true;
    }
    return first;
}

/* A shaped condition's record: its transmitters (all but neighbours when @p wanted_only), from its edge to its edge,
 * through the response, with the response's noise when @p noise. The trial is its middle span, from
 * record_trial(). Where the response is within kUnityTolerance of unity over all of every carrier's content (@p rb,
 * carrier_unshaped()), only the noise goes through the response and the carriers are added to the trial as they are:
 * through the response they would differ by under 1e-5 dB, and otherwise only where the record's circular wrap rings,
 * which its edges keep out of the trial. Every random draw is the same either way: the
 * carriers' draws come first (add_carrier() draws the same whatever the length), then the noise's. Returns the first
 * carrier's start, or -1 with no carrier. */
int64_t
render_record(const Condition& c, const Scene& s, const RecordBins& rb, Rng& rng, Worker& w, bool wanted_only,
              bool noise) {
    const int lead = (c.record - c.span) / 2;
    const double sigma = noise ? kNoiseSigma : 0.0;
    bool shaped = true;
    int64_t first = -1;
    if (rb.carriers_unshaped) {
        w.record.resize((size_t)c.record);
        Rng carriers = rng;
        add_carriers(c, s, rng, wanted_only, w.record.data(), 0, 0); /* their draws only */
        if (noise) {
            shaped = qs::shape_record(rb.bins, c.record, w.record.data(), false, sigma, rng, w.shapes);
        } else {
            std::fill(w.record.begin() + lead, w.record.begin() + lead + c.span, cf(0, 0));
        }
        first = add_carriers(c, s, carriers, wanted_only, w.record.data() + lead, 0, c.span);
    } else {
        w.record.assign((size_t)c.record, cf(0, 0));
        first = add_carriers(c, s, rng, wanted_only, w.record.data(), lead, c.record);
        shaped = qs::shape_record(rb.bins, c.record, w.record.data(), first >= 0, sigma, rng, w.shapes);
    }
    if (!shaped) {
        DSD_FPRINTF(stderr, "condition %s: no transform of %d points\n", c.name.c_str(), c.record);
        std::abort();
    }
    return first;
}

/* The trial inside a shaped condition's record. */
const cf*
record_trial(const Condition& c, const Worker& w) {
    return w.record.data() + (c.record - c.span) / 2;
}

/* One trial's samples (c.span of them), at the scene's scale (Condition.scale); @p rb holds a shaped condition's
 * bins. */
const cf*
render(const Condition& c, const Scene& s, uint64_t seed, Worker& w, const RecordBins* rb) {
    Rng rng(seed);
    cf* out = nullptr;
    if (c.record) {
        render_record(c, s, *rb, rng, w, false, true);
        out = w.record.data() + (c.record - c.span) / 2;
    } else {
        w.trial.resize((size_t)c.span);
        render_noise(c, rng, w);
        for (const Carrier& k : c.carriers) {
            add_carrier(c, k, s, rng, w.trial.data(), 0, c.span);
        }
        out = w.trial.data();
    }
    if (c.scale < 1.0) {
        for (int i = 0; i < c.span; ++i) {
            out[i] *= (float)c.scale;
        }
    }
    return out;
}

/* The samples of a trial with a component at or over the clip level the detector reads (DSD_SIGNAL_CLIP_LEVEL_DEFAULT:
 * the driver's captures leave clip_level 0). */
uint32_t
clip_crossings(const cf* samples, int span) {
    uint32_t count = 0;
    for (int i = 0; i < span; ++i) {
        count += std::abs(samples[i].real()) >= DSD_SIGNAL_CLIP_LEVEL_DEFAULT
                 || std::abs(samples[i].imag()) >= DSD_SIGNAL_CLIP_LEVEL_DEFAULT;
    }
    return count;
}

dsd_signal_quiet_request
request_for(const Condition& c, long trial) {
    dsd_signal_quiet_request r{};
    r.request_id = (uint64_t)trial + 1;
    r.windows = (uint32_t)g_windows;
    r.capture.rate_hz = c.rate;
    if (c.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED) {
        /* A native Airspy: tuned on the channel, no rotation (src/io/radio/rtl_device.cpp
         * rtl_device_signal_geometry()). */
        r.capture.capture_hz = kChannelHz;
        r.capture.geometry = DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED;
    } else {
        /* The RTL tap's geometry: tuned Fs/4 above the channel, the ingest rotation moving the channel to the samples'
         * 0 Hz (src/io/radio/rtl_sdr_fm.cpp demod_feed_signal_search()). */
        r.capture.capture_hz = kChannelHz + c.rate / 4;
        r.capture.rotation_hz = (int32_t)(c.rate / 4);
        r.capture.capture_offset_hz = c.rate / 4;
    }
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
    float occupancy = 0;    /* the result's occupancy share, when done */
    uint32_t crossings = 0; /* samples of the trial with a component at or over the clip level */
    uint32_t status = 0;    /* TrialStatus, with the detector's reason and decision (write_trial_log()) */
};

/* Why a trial did not complete (Verdict.status, low byte): the detector's own reason is in the next byte. */
enum TrialStatus : uint32_t { TRIAL_DONE = 0, TRIAL_REFUSED = 1, TRIAL_UNFINISHED = 2, TRIAL_FAILED = 3 };

/* The detector's decision in a completed trial's status: its void causes (DSD_SIGNAL_QUIET_VOID_*) in the third byte,
 * then the result's valid and quiet bits. */
constexpr int kStatusVoidShift = 16;
constexpr uint32_t kStatusValid = 1u << 24;
constexpr uint32_t kStatusQuiet = 1u << 25;

Verdict
judge(const Condition& c, long trial, dsd_signal_quiet_detector* d, const dsd_signal_quiet_params& params,
      const cf* samples) {
    Verdict v;
    const dsd_signal_quiet_request r = request_for(c, trial);
    dsd_signal_request_failure why = DSD_SIGNAL_REQUEST_OK;
    if (dsd_signal_quiet_detector_begin(d, &r, &params, &why) != 0) {
        v.status = TRIAL_REFUSED | (uint32_t)why << 8;
        return v;
    }
    const int done =
        dsd_signal_quiet_detector_feed(d, reinterpret_cast<const float*>(samples), (size_t)c.span, &r.capture);
    dsd_signal_quiet_result res{};
    const int finished = dsd_signal_quiet_detector_result(d, &res);
    if (done != 1 || finished != 1) {
        v.status = finished == -1 ? TRIAL_FAILED | (uint32_t)res.failure << 8 : TRIAL_UNFINISHED;
        return v;
    }
    v.done = 1;
    v.status = TRIAL_DONE | (uint32_t)res.void_causes << kStatusVoidShift | (res.valid ? kStatusValid : 0u)
               | (res.quiet ? kStatusQuiet : 0u);
    v.occupancy = res.occupancy_share;
    v.valid = res.valid;
    v.quiet = res.quiet;
    v.ratio = res.max_ratio_db;
    v.rise = res.floor_rise_db;
    return v;
}

/* ------------------------------------------------------------------------------------------------ runs */

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

/* The run's loaded inputs. */
struct Inputs {
    const std::vector<Source>& sources;
    const std::vector<Waveform>& waves;
    const std::vector<Condition>& all;
};

/* Whether the phase renders carrier @p k: every carrier, but the snr phase renders only the wanted ones (its
   neighbours need no source there, renderable()). */
bool
renders_carrier(const Options& o, const Carrier& k) {
    return o.phase != "snr" || !k.neighbour;
}

/* The waveforms the phase renders for @p c (renders_carrier()), sorted. */
std::vector<int>
condition_waves(const Condition& c, const Options& o, const std::vector<Source>& sources) {
    std::vector<int> waves;
    for (const Carrier& k : c.carriers) {
        if (renders_carrier(o, k)) {
            waves.push_back(sources[(size_t)k.source].wave);
        }
    }
    std::sort(waves.begin(), waves.end());
    waves.erase(std::unique(waves.begin(), waves.end()), waves.end());
    return waves;
}

/* @p here (conditions at @p rate) in batches whose waveforms, placed at the rate, fit --memory-mb; a batch always
 * takes at least one condition. Conditions that need the same waveforms run in one batch. */
std::vector<std::vector<size_t>>
rate_batches(uint32_t rate, const Options& o, const Inputs& in, std::vector<size_t> here) {
    std::stable_sort(here.begin(), here.end(), [&](size_t a, size_t b) {
        return condition_waves(in.all[a], o, in.sources) < condition_waves(in.all[b], o, in.sources);
    });
    const double budget = (double)o.memory_mb * 1024.0 * 1024.0;
    std::vector<std::vector<size_t>> batches;
    std::vector<int> held;
    double bytes = 0;
    for (size_t i : here) {
        double extra = 0;
        const std::vector<int> waves = condition_waves(in.all[i], o, in.sources);
        for (int v : waves) {
            extra += std::find(held.begin(), held.end(), v) == held.end() ? placed_bytes(in.waves[(size_t)v], rate) : 0;
        }
        if (batches.empty() || (bytes + extra > budget && !held.empty())) {
            batches.emplace_back();
            held.clear();
            bytes = 0;
            extra = std::accumulate(waves.begin(), waves.end(), 0.0,
                                    [&](double sum, int v) { return sum + placed_bytes(in.waves[(size_t)v], rate); });
        }
        for (int v : waves) {
            if (std::find(held.begin(), held.end(), v) == held.end()) {
                held.push_back(v);
            }
        }
        bytes += extra;
        batches.back().push_back(i);
    }
    return batches;
}

/* The waveforms @p batch needs, placed at @p rate (the others left empty). */
std::vector<Placed>
place_batch(uint32_t rate, const Options& o, const Inputs& in, const std::vector<size_t>& batch) {
    std::vector<bool> needed(in.waves.size(), false);
    for (size_t i : batch) {
        for (int v : condition_waves(in.all[i], o, in.sources)) {
            needed[(size_t)v] = true;
        }
    }
    std::vector<Placed> placed(in.waves.size());
    parallel((long)in.waves.size(), o.jobs, [&](long i, Worker&) {
        if (needed[(size_t)i]) {
            placed[(size_t)i] = place_waveform(in.waves[(size_t)i], rate);
        }
    });
    return placed;
}

/* The chosen conditions at @p rate, in their order. */
std::vector<size_t>
at_rate(uint32_t rate, const Inputs& in, const std::vector<size_t>& chosen) {
    std::vector<size_t> here;
    std::copy_if(chosen.begin(), chosen.end(), std::back_inserter(here),
                 [&](size_t i) { return in.all[i].rate == rate; });
    return here;
}

/* A shaped condition's bins, built by the first of its trials to run and shared by the others, freed after its last. */
struct SharedBins {
    std::once_flag built;
    RecordBins bins;
    std::atomic<size_t> left{0};
};

void
run_batch(const std::vector<size_t>& batch, const Scene& scene, const Options& o, const Inputs& in,
          std::vector<Outcome>* outcomes) {
    const bool select = o.phase == "select";
    /* --seed-salt gives a run fresh seeds; 0 keeps the shipped calibration's. */
    const uint64_t phase = (select ? 1U : 2U) + 16U * (uint64_t)o.seed_salt;
    const size_t trials = (size_t)o.trials;
    std::unique_ptr<SharedBins[]> shared(new SharedBins[batch.size()]);
    for (size_t i = 0; i < batch.size(); ++i) {
        shared[i].left = trials;
    }
    parallel((long)(batch.size() * trials), o.jobs, [&](long job, Worker& w) {
        SharedBins& bins = shared[(size_t)job / trials];
        const size_t index = batch[(size_t)job / trials];
        const Condition& c = in.all[index];
        Outcome& out = (*outcomes)[index];
        const long trial = (long)((size_t)job % trials);
        if (c.record) {
            std::call_once(bins.built, [&] { record_bins(c, scene.response, &bins.bins); });
        }
        const cf* samples = render(c, scene, trial_seed(c, phase, trial), w, &bins.bins);
        const uint32_t crossings = clip_crossings(samples, c.span);
        for (int e = 0; e < 2; ++e) {
            if (!select && e != o.estimator) {
                continue;
            }
            const dsd_signal_quiet_params params{select ? 0.0f : (float)o.threshold, e, (float)o.guard,
                                                 (float)o.occupancy_veto, 0.0f};
            out.trials[e][(size_t)trial] = judge(c, trial, w.detectors[e], params, samples);
            out.trials[e][(size_t)trial].crossings = crossings;
        }
        if (--bins.left == 0) {
            bins.bins = RecordBins();
        }
    });
}

void
run_rate(uint32_t rate, const Options& o, const Inputs& in, const std::vector<size_t>& chosen,
         std::vector<Outcome>* outcomes) {
    const std::vector<size_t> here = at_rate(rate, in, chosen);
    if (here.empty()) {
        return;
    }
    const std::vector<std::vector<size_t>> batches = rate_batches(rate, o, in, here);
    for (const std::vector<size_t>& batch : batches) {
        const std::vector<Placed> placed = place_batch(rate, o, in, batch);
        run_batch(batch, Scene{in.sources, placed, o.response}, o, in, outcomes);
    }
    DSD_FPRINTF(stderr, "rate %u: %zu conditions done in %zu batch(es)\n", rate, here.size(), batches.size());
}

const char*
kind_text(Kind k) {
    return k == ACTIVE ? "active" : k == NOISE ? "noise" : "info";
}

/* One of a run's outputs (the CSV, --dump, --trial-log): the file, null for none, and the SHA-256 of every byte the run
   wrote to it (its completion record's digest). */
struct Output {
    FILE* f = nullptr;
    Sha256 sha;
    bool overlong = false; /* a CSV field too long to format, which fails the run */
};

/* Write @p size bytes to @p out and hash them; false when the write fails (the run fails at the close then too). */
bool
out_write(Output* out, const void* data, size_t size) {
    if (!out->f) {
        return true;
    }
    const size_t written = fwrite(data, 1, size, out->f);
    out->sha.update(data, written);
    return written == size;
}

void out_printf(Output* out, const char* fmt, ...) DSD_ATTR_FORMAT(printf, 2, 3);

/* fprintf() to @p out, hashed as written. */
void
out_printf(Output* out, const char* fmt, ...) {
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = DSD_VSNPRINTF(text, sizeof(text), fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof(text)) {
        out->overlong = true;
        return;
    }
    (void)out_write(out, text, (size_t)n);
}

/* The q-quantiles (nearest rank) of the ratios (or rises) of the trials @p take selects, printed comma-separated
 * with @p end after the last; empty fields when none is selected. */
template <typename Take>
void
print_quantiles(Output* f, const std::vector<Verdict>& trials, bool rise, Take take, std::initializer_list<double> qs,
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
            out_printf(f, "%c", sep);
            continue;
        }
        const size_t rank = (size_t)std::min((double)v.size() - 1, std::max(0.0, std::ceil(q * v.size()) - 1));
        out_printf(f, "%.3f%c", v[rank], sep);
    }
}

void
write_header(Output* f, bool select) {
    out_printf(f, "condition,group,kind,rate_hz,classes,analog_width_hz,grid_shift_hz,snr_db,span,trials,estimator,"
                  "guard_db,invalid,");
    if (select) {
        out_printf(f, "min,p001,p01,p05,p50,p95,p99,max,");
    } else {
        out_printf(f, "threshold_db,quiet,busy,min,p05,p50,p95,max,");
    }
    out_printf(f, "rise_min,rise_p05,rise_p50,rise_p95,rise_p99,rise_max,geometry,response,depression_db,"
                  "adc_noise_db\n");
}

void
write_row(Output* f, const Condition& c, const Outcome& out, int e, const Options& o) {
    const std::vector<Verdict>& trials = out.trials[e];
    long invalid = 0, quiet = 0;
    for (const Verdict& t : trials) {
        invalid += t.valid ? 0 : 1;
        quiet += t.quiet;
    }
    out_printf(f, "%s,%s,%s,%u,%s,%u,%d,%.2f,%d,%zu,%s,%.2f,%ld,", c.name.c_str(), c.group.c_str(), kind_text(c.kind),
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
        out_printf(f, "%.2f,%ld,%ld,", o.threshold, quiet, (long)trials.size() - quiet);
        print_quantiles(f, trials, false, valid, {0, 0.05, 0.5, 0.95, 1}, ',');
    }
    print_quantiles(f, trials, true, done, {0, 0.05, 0.5, 0.95, 0.99, 1}, ',');
    out_printf(f, "%s\n", run_columns(o).c_str());
}

/* Every trial's ratio and rise as little-endian float32 pairs, rows in the CSV's order (--dump; read it with the
 * guard off, where every completed trial is valid). False when the write fails. */
bool
write_dump(Output* dump, const Outcome& out, int e) {
    if (!dump->f) {
        return true;
    }
    std::vector<float> pairs;
    pairs.reserve(out.trials[e].size() * 2);
    for (const Verdict& t : out.trials[e]) {
        pairs.push_back(t.ratio);
        pairs.push_back(t.rise);
    }
    if (!out_write(dump, pairs.data(), pairs.size() * sizeof(float))) {
        DSD_FPRINTF(stderr, "dump write failed\n");
        return false;
    }
    return true;
}

/* Every trial's occupancy share (float32), clip-level crossings and status (uint32 each), 12 bytes little-endian a
 * trial, rows in the CSV's order (--trial-log). The status: TrialStatus in the low byte, the detector's refusal or
 * failure reason in the next; for a completed trial the result's void causes (DSD_SIGNAL_QUIET_VOID_*) in the third
 * byte, valid in bit 24 and quiet in bit 25, the detector's own decision. False when the write fails. */
bool
write_trial_log(Output* log, const Outcome& out, int e) {
    if (!log->f) {
        return true;
    }

    struct Record {
        float occupancy;
        uint32_t crossings, status;
    };

    static_assert(sizeof(Record) == 12, "trial log records are 12 bytes");
    std::vector<Record> records;
    records.reserve(out.trials[e].size());
    for (const Verdict& t : out.trials[e]) {
        records.push_back(Record{t.occupancy, t.crossings, t.status});
    }
    if (!out_write(log, records.data(), records.size() * sizeof(Record))) {
        DSD_FPRINTF(stderr, "trial log write failed\n");
        return false;
    }
    return true;
}

/* Open @p path for the run's output (@p mode as fopen() takes it), saying so when it cannot be: an empty path is no
 * output (null, and @p ok stays); a failure clears @p ok. */
FILE*
open_output(const std::string& path, const char* mode, bool* ok) {
    if (path.empty()) {
        return nullptr;
    }
    FILE* f = dsd_fopen_private(path.c_str(), mode);
    if (!f) {
        DSD_FPRINTF(stderr, "cannot write %s\n", path.c_str());
        *ok = false;
    }
    return f;
}

/* Close @p f, written to @p path: false when it held a failed write or its last bytes fail to land (a full disk). */
bool
close_output(FILE* f, const std::string& path) {
    if (!f) {
        return true;
    }
    const bool failed = ferror(f) != 0;
    if (fclose(f) != 0 || failed) {
        DSD_FPRINTF(stderr, "write to %s failed\n", path.c_str());
        return false;
    }
    return true;
}

/* The distinct rates of the chosen conditions, ascending. */
std::vector<uint32_t>
chosen_rates(const Inputs& in, const std::vector<size_t>& chosen) {
    std::vector<uint32_t> rates(chosen.size());
    std::transform(chosen.begin(), chosen.end(), rates.begin(), [&](size_t i) { return in.all[i].rate; });
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

/* The chosen conditions with a trial whose samples reach the clip level the detector reads, each said with its
   clipped trials and samples: a calibration renders every trial under it (B8), so any one fails the run. */
size_t
clipped_conditions(const Options& o, const Inputs& in, const std::vector<size_t>& chosen,
                   const std::vector<Outcome>& outcomes) {
    /* Both estimators of a selection judge the same samples: the first one's crossings are the trial's. */
    const int e = o.phase == "select" ? 0 : o.estimator;
    size_t clipped = 0;
    for (size_t i : chosen) {
        long trials = 0;
        uint64_t samples = 0;
        for (const Verdict& t : outcomes[i].trials[e]) {
            trials += t.crossings != 0;
            samples += t.crossings;
        }
        if (trials) {
            DSD_FPRINTF(stderr, "condition %s: %ld trial(s) clip (%llu samples at or over the clip level)\n",
                        in.all[i].name.c_str(), trials, (unsigned long long)samples);
            ++clipped;
        }
    }
    return clipped;
}

/* The chosen conditions with a trial the detector did not measure (TRIAL_REFUSED, _FAILED or _UNFINISHED), each said
   with its counts: such a trial reads busy in the CSV without any reading behind it, so any one fails the run. A
   completed reading that the guard or the occupancy check voided is a measurement, and counts busy as it should. */
size_t
unmeasured_conditions(const Options& o, const Inputs& in, const std::vector<size_t>& chosen,
                      const std::vector<Outcome>& outcomes) {
    size_t unmeasured = 0;
    for (size_t i : chosen) {
        long counts[4] = {0, 0, 0, 0};
        for (int e = 0; e < 2; ++e) {
            if (o.phase != "select" && e != o.estimator) {
                continue;
            }
            for (const Verdict& t : outcomes[i].trials[e]) {
                const uint32_t kind =
                    t.done ? (uint32_t)TRIAL_DONE
                           : std::max<uint32_t>(TRIAL_REFUSED, std::min<uint32_t>(t.status & 0xFFu, TRIAL_FAILED));
                counts[kind]++;
            }
        }
        if (counts[TRIAL_REFUSED] + counts[TRIAL_UNFINISHED] + counts[TRIAL_FAILED] != 0) {
            DSD_FPRINTF(stderr, "condition %s: trials not measured: %ld refused, %ld unfinished, %ld failed\n",
                        in.all[i].name.c_str(), counts[TRIAL_REFUSED], counts[TRIAL_UNFINISHED], counts[TRIAL_FAILED]);
            ++unmeasured;
        }
    }
    return unmeasured;
}

/* Write every chosen condition's rows, dump and trial log, then close the outputs; false when a write or close fails. */
bool
write_outputs(const Options& o, const Inputs& in, const std::vector<size_t>& chosen,
              const std::vector<Outcome>& outcomes, Output* csv, Output* dump, Output* log) {
    bool ok = true;
    write_header(csv, o.phase == "select");
    for (size_t i : chosen) {
        for (int e = 0; e < 2; ++e) {
            if (o.phase == "select" || e == o.estimator) {
                write_row(csv, in.all[i], outcomes[i], e, o);
                ok = write_dump(dump, outcomes[i], e) && ok;
                ok = write_trial_log(log, outcomes[i], e) && ok;
            }
        }
    }
    if (csv->overlong) {
        DSD_FPRINTF(stderr, "a CSV field is too long\n");
        ok = false;
    }
    ok = close_output(csv->f, o.out) && ok;
    ok = close_output(dump->f, o.dump) && ok;
    ok = close_output(log->f, o.trial_log) && ok;
    return ok;
}

/* Run the chosen conditions and write the CSV, --dump and --trial-log. Every output opens before the run, so a path it
   cannot write stops it at once; 1 when any output fails to open, write or close, or any trial clips
   (clipped_conditions()) or was not measured (unmeasured_conditions()), with no completion record then. */
int
run_trials(const Options& o, const Inputs& in, const std::vector<size_t>& chosen) {
    bool ok = true;
    Output csv, dump, log;
    csv.f = open_output(o.out, "wb", &ok);
    dump.f = open_output(o.dump, "wb", &ok);
    log.f = open_output(o.trial_log, "wb", &ok);
    if (!ok) {
        for (FILE* opened : {csv.f, dump.f, log.f}) {
            if (opened) {
                fclose(opened);
            }
        }
        return 1;
    }
    std::vector<Outcome> outcomes(in.all.size());
    for (size_t i : chosen) {
        for (int e = 0; e < 2; ++e) {
            outcomes[i].trials[e].assign((size_t)o.trials, Verdict{});
        }
    }
    for (uint32_t rate : chosen_rates(in, chosen)) {
        run_rate(rate, o, in, chosen, &outcomes);
    }
    if (!write_outputs(o, in, chosen, outcomes, &csv, &dump, &log)) {
        return 1;
    }
    const size_t clipped = clipped_conditions(o, in, chosen, outcomes);
    const size_t unmeasured = unmeasured_conditions(o, in, chosen, outcomes);
    if (clipped || unmeasured) {
        DSD_FPRINTF(stderr, "%zu condition(s) clip, %zu with trials not measured: the run is no calibration\n", clipped,
                    unmeasured);
        return 1;
    }
    /* The completion record: what ran, how much, and the digests of the bytes this run wrote to its three outputs, so
       a checker can tell these outputs from another run's beside this log. */
    const size_t rows = chosen.size() * (o.phase == "select" ? 2 : 1);
    const auto digest = [](const std::string& path, Output* out) {
        return path.empty() ? std::string("none") : out->sha.hex();
    };
    const std::string csv_sha = digest(o.out, &csv), dump_sha = digest(o.dump, &dump),
                      log_sha = digest(o.trial_log, &log);
    DSD_FPRINTF(stderr, "complete: %s trials=%ld conditions=%zu rows=%zu csv=%s dump=%s log=%s\n", run_text(o).c_str(),
                o.trials, chosen.size(), rows, csv_sha.c_str(), dump_sha.c_str(), log_sha.c_str());
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

/* One trial's wanted carriers alone (no neighbour, no noise), through the response when the condition has one (@p rb
 * its bins), in w.trial; returns the first carrier's start. */
int64_t
render_wanted(const Condition& c, const Scene& s, const RecordBins* rb, Rng& rng, Worker& w) {
    if (c.record) {
        const int64_t first = render_record(c, s, *rb, rng, w, true, false);
        w.trial.assign(record_trial(c, w), record_trial(c, w) + c.span);
        return first;
    }
    w.trial.assign((size_t)c.span, cf(0, 0));
    int64_t first = -1;
    for (const Carrier& k : c.carriers) {
        if (!k.neighbour) {
            const int64_t start = add_carrier(c, k, s, rng, w.trial.data(), 0, c.span);
            first = first < 0 ? start : first;
        }
    }
    return first;
}

/* One condition's measured S/N, and its renders whose wanted signal and floor together reach the clip level. */
struct SnrMeasure {
    double db = 0;
    long clipped = 0;
};

/* One condition's wanted S/N measured back from @p o.trials renders: its in-width power (a burst train's as
 * band_power(): its in-width share times its power during the bursts) over the in-width power of the floor rendered
 * alone, both after the response and both at the scene's scale (Condition.scale, which leaves the S/N as it was), with
 * the renders whose two together reach the clip level counted. --seed-salt gives fresh seeds, 0 the shipped ones. */
SnrMeasure
measured_snr_db(Condition c, const Options& o, const Scene& s, Worker& w) {
    c.span = transform_points(c.rate);
    c.record = c.record ? record_points(c) : 0;
    w.bins_for = nullptr; /* c is this call's copy: its record differs from the trials' */
    const Placed& p = s.placed[(size_t)s.sources[(size_t)c.carriers[0].source].wave];
    const double centre = channel_hz(c) + c.snr_offset_hz;
    double signal = 0, noise = 0, total = 0;
    dsd_io::FftSetupCache fft;
    const RecordBins* rb = c.record ? &condition_bins(c, s.response, w) : nullptr;
    const uint64_t phase = 3U + 16U * (uint64_t)o.seed_salt;
    const float scale = (float)c.scale;
    const auto scaled = [scale](cf v) { return v * scale; };
    SnrMeasure out;
    for (long t = 0; t < o.trials; ++t) {
        Rng rng(trial_seed(c, phase, t));
        const int64_t start = render_wanted(c, s, rb, rng, w);
        std::transform(w.trial.begin(), w.trial.end(), w.trial.begin(), scaled);
        const std::vector<cf> wanted = w.trial;
        const double in = in_width_power(fft, w.trial, c.rate, centre, c.snr_width, &total);
        double on = 0;
        long count = 0;
        for (int i = 0; !p.bursts.empty() && i < c.span; ++i) {
            if (burst_at(p, start + i)) {
                on += std::norm(w.trial[(size_t)i]);
                ++count;
            }
        }
        signal += p.bursts.empty() ? in : (count ? on / count : 0) * in / total;
        if (c.record) {
            /* the response's noise alone, through the same bins */
            w.record.resize((size_t)c.record);
            if (!qs::shape_record(rb->bins, c.record, w.record.data(), false, kNoiseSigma, rng, w.shapes)) {
                DSD_FPRINTF(stderr, "condition %s: no transform of %d points\n", c.name.c_str(), c.record);
                std::abort();
            }
            w.trial.assign(record_trial(c, w), record_trial(c, w) + c.span);
        } else {
            render_noise(c, rng, w);
        }
        std::transform(w.trial.begin(), w.trial.end(), w.trial.begin(), scaled);
        std::vector<cf> both(wanted.size());
        std::transform(wanted.begin(), wanted.end(), w.trial.begin(), both.begin(), std::plus<cf>());
        out.clipped += clip_crossings(both.data(), (int)both.size()) != 0;
        noise += in_width_power(fft, w.trial, c.rate, centre, c.snr_width, &total);
    }
    fft.release();
    w.bins_for = nullptr;
    out.db = 10 * std::log10(signal / noise);
    return out;
}

/* The S/N cross-check: each chosen condition's wanted signal (choose() keeps only active ones), rendered as the
 * trials are. 1 when the CSV fails to land or any render clips (each such condition said). */
int
run_snr(const Options& o, const Inputs& in, const std::vector<size_t>& chosen) {
    FILE* f = dsd_fopen_private(o.out.c_str(), "w");
    if (!f) {
        return 1;
    }
    DSD_FPRINTF(f, "condition,target_snr_db,measured_snr_db,trials,geometry,response,depression_db,adc_noise_db\n");
    std::vector<SnrMeasure> snr(in.all.size());
    for (uint32_t rate : chosen_rates(in, chosen)) {
        const std::vector<size_t> here = at_rate(rate, in, chosen);
        for (const std::vector<size_t>& batch : rate_batches(rate, o, in, here)) {
            const std::vector<Placed> placed = place_batch(rate, o, in, batch);
            const Scene scene{in.sources, placed, o.response};
            parallel((long)batch.size(), o.jobs, [&](long j, Worker& w) {
                snr[batch[(size_t)j]] = measured_snr_db(in.all[batch[(size_t)j]], o, scene, w);
            });
        }
        for (size_t i : here) {
            DSD_FPRINTF(f, "%s,%.2f,%.3f,%ld,%s\n", in.all[i].name.c_str(), in.all[i].snr_db, snr[i].db, o.trials,
                        run_columns(o).c_str());
        }
    }
    size_t clipped = 0;
    for (size_t i : chosen) {
        if (snr[i].clipped) {
            DSD_FPRINTF(stderr, "condition %s: %ld render(s) clip\n", in.all[i].name.c_str(), snr[i].clipped);
            ++clipped;
        }
    }
    if (!close_output(f, o.out)) {
        return 1;
    }
    if (clipped) {
        DSD_FPRINTF(stderr, "%zu condition(s) clip: the run is no calibration\n", clipped);
        return 1;
    }
    return 0;
}

/* The capture-wide reference's span for @p c's policy and rate (signal_measure.cpp quiet_reference_span()). */
double
reference_span(const Condition& c) {
    const auto* e = std::find_if(std::begin(kCentredSpans), std::end(kCentredSpans),
                                 [&](const CentredSpan& s) { return s.rate == c.rate; });
    return c.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED && e != std::end(kCentredSpans) ? e->span : kDefaultSpan;
}

/* The floor guard's capture-wide reference as the detector computes it (wide_reference() in
 * src/io/radio/signal_measure.cpp, the same bin selection over the request's geometry and policy and the same
 * quartile), nine times, once per sub-window. Returns the selected values' sum so the work is not optimised away. */
double
reference_work(const Condition& c, const std::vector<double>& power, std::vector<double>& scratch) {
    const dsd_signal_quiet_request r = request_for(c, 0);
    const int n = (int)power.size();
    const double rate = c.rate, bin_hz = rate / n;
    const double dc_guard =
        c.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED ? -1.0 : std::max(bin_hz * 3, rate / 100.0);
    const double span = reference_span(c);
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
    for (int w = 0; w < windows_judged(); ++w) {
        size_t count = 0;
        for (int m = -n / 2; m < n / 2; ++m) {
            double delta = m * bin_hz - r.capture.rotation_hz;
            delta -= rate * std::floor((delta + rate / 2) / rate);
            if (std::abs(delta) > dc_guard && std::abs(delta) < rate * span && std::abs(delta - channel) > half_width) {
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
    DSD_FPRINTF(f, "rate_hz,classes,analog_width_hz,fft,request_us,reference_us,geometry\n");

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
    for (uint32_t rate : o.rates) {
        for (const auto& q : requests) {
            Condition c;
            c.rate = rate;
            c.classes = q.classes;
            c.analog_width = q.width;
            c.geometry = o.geometry;
            c.span = quiet_span(c);
            Rng rng(fnv1a("cost"));
            w.trial.resize((size_t)c.span);
            render_noise(c, rng, w);
            uint64_t start = dsd_realtime_mono_ns();
            for (long t = 0; t < o.trials; ++t) {
                g_cost_sink = g_cost_sink + judge(c, t, w.detectors[0], params, w.trial.data()).ratio;
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
            DSD_FPRINTF(f, "%u,%s,%u,%d,%.1f,%.1f,%s\n", rate, class_text(q.classes), q.width, bins, request_us,
                        reference_us, geometry_text(o.geometry));
        }
    }
    return close_output(f, o.out) ? 0 : 1;
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

/* A comma-separated list of capture rates (Hz), each one the check admits (kRates); sorted, duplicates dropped. */
bool
parse_rates(const char* text, std::vector<uint32_t>* out) {
    std::vector<uint32_t> rates;
    std::istringstream fields(text);
    std::string field;
    while (std::getline(fields, field, ',')) {
        long v = 0;
        if (!parse_count(field.c_str(), 1, 100000000, &v)
            || std::find(std::begin(kRates), std::end(kRates), (uint32_t)v) == std::end(kRates)) {
            return false;
        }
        rates.push_back((uint32_t)v);
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    *out = std::move(rates);
    return !out->empty();
}

/* --add's comma-separated groups (masks, occupancy, cross, raised) into @p o; false for any other. */
bool
parse_groups(const char* text, Options* o) {
    std::istringstream fields(text);
    std::string field;
    bool any = false;
    while (std::getline(fields, field, ',')) {
        if (field == "masks") {
            o->add_masks = true;
        } else if (field == "occupancy") {
            o->add_occupancy = true;
        } else if (field == "cross") {
            o->add_cross = true;
        } else if (field == "raised") {
            o->add_raised = true;
        } else {
            return false;
        }
        any = true;
    }
    return any;
}

/* One option of the run itself (seeds, the occupancy check, added groups) into @p o; false when it does not parse. */
bool
parse_run_option(const std::string& key, const char* value, Options* o) {
    if (key == "--seed-salt") {
        return parse_count(value, 0, 1000000, &o->seed_salt);
    }
    if (key == "--occupancy-veto") {
        return parse_double(value, &o->occupancy_veto) && o->occupancy_veto >= 0 && o->occupancy_veto <= 1;
    }
    if (key == "--add") {
        return parse_groups(value, o);
    }
    if (key == "--scene-ceiling") {
        o->ceiling_given = true;
        return parse_double(value, &o->scene_ceiling) && o->scene_ceiling > 0 && o->scene_ceiling <= 1e6;
    }
    return false;
}

/* Whether @p db is a whole number of tenths of a dB: the CSV's response columns record --depression and --adc-noise-db
   to 0.1 dB (run_columns()), so a value between them would render one condition and record another. */
bool
on_tenth_grid(double db) {
    return std::fabs(db * 10.0 - std::round(db * 10.0)) <= 1e-9;
}

/* One option of the run's geometry or receiver response into @p o; false when it does not parse. */
bool
parse_scene_option(const std::string& key, const char* value, Options* o) {
    if (key == "--geometry") {
        const bool centred = std::strcmp(value, "centred") == 0;
        o->geometry = centred ? (uint8_t)DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED : (uint8_t)DSD_SIGNAL_GEOMETRY_DEFAULT;
        return centred || std::strcmp(value, "default") == 0;
    }
    if (key == "--rates") {
        o->rates_given = true;
        return parse_rates(value, &o->rates);
    }
    if (key == "--response") {
        return qs::parse_shape(value, &o->response.shape);
    }
    if (key == "--depression") {
        return parse_double(value, &o->response.depression_db) && o->response.depression_db > 0
               && o->response.depression_db <= 20 && on_tenth_grid(o->response.depression_db);
    }
    if (key == "--adc-noise-db") {
        o->response.adc = true;
        return parse_double(value, &o->response.adc_db) && std::fabs(o->response.adc_db) <= 40
               && on_tenth_grid(o->response.adc_db);
    }
    if (key == "--memory-mb") {
        return parse_count(value, 64, 1L << 22, &o->memory_mb);
    }
    return parse_run_option(key, value, o);
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
        o->threshold_given = true;
        return parse_double(value, &o->threshold);
    }
    if (key == "--guard") {
        return parse_double(value, &o->guard);
    }
    if (key == "--neighbour-levels") {
        return parse_levels(value, &o->neighbour_levels);
    }
    if (key == "--windows") {
        double windows = 0;
        if (!parse_double(value, &windows) || windows < 1 || windows > DSD_SIGNAL_QUIET_WINDOWS_MAX
            || std::abs(windows - std::round(windows)) > 1e-9) {
            return false;
        }
        g_windows = (int)std::lround(windows);
        return true;
    }
    if (key == "--estimator") {
        o->estimator = std::strcmp(value, "trimmed") == 0  ? DSD_SIGNAL_QUIET_FLOOR_TRIMMED_MEAN
                       : std::strcmp(value, "lowest") == 0 ? DSD_SIGNAL_QUIET_FLOOR_LOWEST_BLOCK
                                                           : -2;
        return o->estimator >= 0;
    }
    return parse_scene_option(key, value, o);
}

/* Why @p rate cannot be rendered under @p o's geometry and response, or nullptr. */
const char*
rate_refusal(const Options& o, uint32_t rate) {
    if (o.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED && !dsd_signal_quiet_airspy_centred_rate_supported(rate)) {
        return "the centred policy does not admit it";
    }
    if (!qs::response_supported(o.response, rate)) {
        return "the response is fitted for 2.5, 3, 6 and 10 MS/s only (give --rates)";
    }
    return nullptr;
}

/* The rate lists: --rates for every condition, else the geometry's defaults; --pilot keeps the list's last rate. */
bool
finish_rates(Options* o) {
    const bool centred = o->geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED;
    if (!o->rates_given && centred) {
        o->rates.resize(std::end(kCentredSpans) - std::begin(kCentredSpans));
        std::transform(std::begin(kCentredSpans), std::end(kCentredSpans), o->rates.begin(),
                       [](const CentredSpan& e) { return e.rate; });
    } else if (!o->rates_given) {
        o->rates.assign(std::begin(kRates), std::end(kRates));
    }
    o->scene_rates = o->rates_given || centred ? o->rates : std::vector<uint32_t>{kSceneRate};
    if (!o->pilot.empty()) {
        o->rates = o->scene_rates = {o->rates.back()};
    }
    const auto refused = std::find_if(o->rates.begin(), o->rates.end(),
                                      [&](uint32_t rate) { return rate_refusal(*o, rate) != nullptr; });
    if (refused != o->rates.end()) {
        DSD_FPRINTF(stderr, "rate %u: %s\n", *refused, rate_refusal(*o, *refused));
        return false;
    }
    return true;
}

/* Whether the phase has what it needs: a known phase, its estimator, sources (but cost) and an output; a depression
 * or ADC share only on a shaped response; and --threshold and --estimator in validation only, the one phase that
 * judges at them (selection runs both estimators at no threshold), so a run never attests parameters it did not use. */
bool
phase_ok(const Options& o) {
    const bool validate = o.phase == "validate";
    if (!validate && (o.threshold_given || o.estimator != -1)) {
        DSD_FPRINTF(stderr, "--threshold and --estimator are for --phase validate only\n");
        return false;
    }
    const bool known = o.phase == "select" || o.phase == "snr" || o.phase == "cost" || (validate && o.estimator >= 0);
    const bool response = o.response.shape != qs::Shape::FLAT || (o.response.depression_db <= 0 && !o.response.adc);
    return known && response && (o.phase == "cost" || !o.sources.empty()) && !o.out.empty();
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
    return phase_ok(*o) && finish_rates(o);
}

bool
parse_options(int argc, char** argv, Options* o) {
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        std::string* text = key == "--sources"      ? &o->sources
                            : key == "--phase"      ? &o->phase
                            : key == "--out"        ? &o->out
                            : key == "--filter"     ? &o->filter
                            : key == "--dump"       ? &o->dump
                            : key == "--modes-file" ? &o->modes_file
                            : key == "--pilot"      ? &o->pilot
                            : key == "--trial-log"  ? &o->trial_log
                            : key == "--bounds"     ? &o->bounds
                                                    : nullptr;
        if (text) {
            *text = argv[i + 1];
        } else if (!parse_number(key, argv[i + 1], o)) {
            return false;
        }
    }
    return argc % 2 != 0 && finish_options(o);
}

/* False, with the reason, when a carrier's source is too short to place a @p span-sample trial at the condition's
 * rate (source_start_range() empty). */
bool
source_long_enough(const Condition& c, const Carrier& k, int span, const Inputs& in) {
    const Source& src = in.sources[(size_t)k.source];
    const Waveform& s = in.waves[(size_t)src.wave];
    const std::pair<int64_t, int64_t> range = source_start_range(s, c.rate, span);
    if (range.second > range.first) {
        return true;
    }
    const bool bursts = std::any_of(s.on.begin(), s.on.end(), [](uint8_t v) { return v != 0; });
    DSD_FPRINTF(stderr,
                "condition %s: source %s (%.3f s) is too short to place a %d-sample trial at %u Hz (%.3f s) 100 ms "
                "clear of its ends%s; export longer sources\n",
                c.name.c_str(), src.row.c_str(), (double)s.base.size() / kSourceRate, span, c.rate,
                (double)span / c.rate, bursts ? ", from one burst gap before its first burst to inside its last" : "");
    return false;
}

/* False, with the reason, when condition @p c cannot be rendered for the phase: a missing source, a bad span, a
 * shaped record's edges outside the placed waveform's margins, or a source too short to place what the phase renders
 * (the trials' span for every carrier; the snr phase's longer one for the wanted carriers alone, the ones it
 * renders). */
bool
renderable(Condition c, const Options& o, const Inputs& in) {
    const bool snr = o.phase == "snr";
    if (snr) {
        c.span = transform_points(c.rate);
        c.record = c.record ? record_points(c) : 0;
    }
    const auto rendered = [&](const Carrier& k) { return renders_carrier(o, k); };
    const bool sources_ok = std::all_of(c.carriers.begin(), c.carriers.end(),
                                        [&](const Carrier& k) { return !rendered(k) || k.source >= 0; });
    const int lead = c.record ? (c.record - c.span) / 2 : 0;
    if (!sources_ok || c.span <= 0 || c.span > transform_points(c.rate) || lead >= (int)(c.rate / 10)) {
        DSD_FPRINTF(stderr, "condition %s: missing source or bad span %d\n", c.name.c_str(), c.span);
        return false;
    }
    return std::all_of(c.carriers.begin(), c.carriers.end(),
                       [&](const Carrier& k) { return !rendered(k) || source_long_enough(c, k, c.span, in); });
}

/* The conditions the run measures into @p chosen: --filter's and --pilot's, and of them the snr phase's active ones
 * (the ones it measures). 1 when one of them cannot be rendered (renderable()), said before any trial runs. Only the
 * chosen conditions are checked, so a source that none of them uses may be short or missing. */
int
choose(const Inputs& in, const std::vector<Mode>& modes, const Options& o, std::vector<size_t>* chosen) {
    const auto found = std::find_if(modes.begin(), modes.end(), [&](const Mode& m) { return m.name == o.pilot; });
    const Mode* pilot = o.pilot.empty() || found == modes.end() ? nullptr : &*found;
    if (!o.pilot.empty() && !pilot) {
        DSD_FPRINTF(stderr, "--pilot %s: no such mode\n", o.pilot.c_str());
        return 1;
    }
    for (size_t i = 0; i < in.all.size(); ++i) {
        const Condition& c = in.all[i];
        if ((!o.filter.empty() && c.name.find(o.filter) == std::string::npos) || (pilot && !pilot_match(c, *pilot))
            || (o.phase == "snr" && c.kind != ACTIVE)) {
            continue;
        }
        if (!renderable(c, o, in)) {
            return 1;
        }
        chosen->push_back(i);
    }
    return 0;
}

/* What an occupied capture actually raises, read on the detector's own reference bins (wide_reference() in
   signal_measure.cpp: the transform's bins whose hardware offset clears the DC guard on a shifted capture, lies inside
   the policy's span and outside the widest class round the channel): the share of those bins inside a raised band,
   the noise contrast there after the response and the ADC noise (the mean over those bins of
   (G R + a) / (G + a), R the raise, G the response, a the ADC share; R alone with no response), and the placement:
   "upper" or "both" as asked, "+spilled" when an upper raise ran out of room and went on below the channel,
   "+saturated" when the room on both sides held less than the length asked for. */
struct OccupiedScene {
    double achieved_share = 0, contrast_db = 0;
    std::string placement;
};

OccupiedScene
occupied_scene(const Condition& c, const qs::Response& response) {
    OccupiedScene out;
    const int n = fft_points(c);
    const double rate = c.rate, bin = rate / n, span = reference_span(c) * rate;
    const bool centred = c.geometry == DSD_SIGNAL_GEOMETRY_AIRSPY_CENTRED;
    const double dc_guard = centred ? -1.0 : std::max(bin * 3, rate / 100.0);
    const double channel = centred ? 0.0 : -rate / 4;
    double widest = c.analog_width;
    if (c.classes & DSD_SIGNAL_QUIET_CLASS_12K5) {
        widest = std::max(widest, (double)DSD_SIGNAL_QUIET_WIDTH_12K5_HZ);
    }
    if (c.classes & DSD_SIGNAL_QUIET_CLASS_6K25) {
        widest = std::max(widest, (double)DSD_SIGNAL_QUIET_WIDTH_6K25_HZ);
    }
    const bool shaped = response.shape != qs::Shape::FLAT;
    const double raise = std::pow(10.0, c.floor_b / 10.0),
                 adc = response.adc ? std::pow(10.0, response.adc_db / 10.0) : 0;
    const qs::Placement p = placement(c);
    long eligible = 0, raised = 0;
    double contrast = 0;
    for (int m = -n / 2; m < n / 2; ++m) {
        const double hw = m * bin; /* the bin's hardware offset (the detector's delta) */
        if (!(std::abs(hw) > dc_guard && std::abs(hw) < span && std::abs(hw - channel) > widest / 2)) {
            continue;
        }
        ++eligible;
        const bool in = std::any_of(c.occupied.begin(), c.occupied.end(),
                                    [&](const std::pair<double, double>& b) { return hw >= b.first && hw < b.second; });
        if (in) {
            ++raised;
            const double g = shaped ? qs::response_power(response, p, hw) : 1.0;
            contrast += (g * raise + (shaped ? adc : 0.0)) / (g + (shaped ? adc : 0.0));
        }
    }
    out.achieved_share = eligible ? (double)raised / eligible : 0.0;
    out.contrast_db = raised ? 10 * std::log10(contrast / raised) : 0.0;
    double placed = 0;
    bool below = false;
    for (const auto& b : c.occupied) {
        placed += b.second - b.first;
        below = below || b.second <= channel;
    }
    out.placement = c.occupied_both ? "both" : "upper";
    if (!c.occupied_both && below) {
        out.placement += "+spilled";
    }
    if (placed + 1.0 < c.occupied_length) {
        out.placement += "+saturated";
    }
    return out;
}

/* Every condition's scene bound and scale (--bounds), whatever --filter chooses (so a run with a filter that matches
 * nothing lists them all), with the row metadata the CSV will carry (the manifest pins it) and, for an occupied
 * capture, what it asked for and what it raises (occupied_scene()); the run says how many it scaled. */
int
write_bounds(const std::string& path, const std::vector<Condition>& all, const Options& o) {
    FILE* f = dsd_fopen_private(path.c_str(), "w");
    if (!f) {
        DSD_FPRINTF(stderr, "cannot write %s\n", path.c_str());
        return 1;
    }
    DSD_FPRINTF(f, "condition,bound,scale,group,kind,rate_hz,classes,analog_width_hz,grid_shift_hz,snr_db,geometry,"
                   "response,depression_db,adc_noise_db,requested_share,achieved_share,contrast_db,placement\n");
    size_t scaled = 0;
    for (const Condition& c : all) {
        DSD_FPRINTF(f, "%s,%.6f,%.6f,%s,%s,%u,%s,%u,%d,%.2f,%s,", c.name.c_str(), c.bound, c.scale, c.group.c_str(),
                    kind_text(c.kind), c.rate, class_text(c.classes), c.analog_width, c.grid_shift_hz,
                    c.kind == ACTIVE ? c.snr_db : 0.0, run_columns(o).c_str());
        if (c.floor == OCCUPIED) {
            const OccupiedScene scene = occupied_scene(c, o.response);
            DSD_FPRINTF(f, "%.4f,%.4f,%.3f,%s\n", c.occupied_share, scene.achieved_share, scene.contrast_db,
                        scene.placement.c_str());
        } else {
            DSD_FPRINTF(f, ",,,\n");
        }
        scaled += c.scale < 1.0;
    }
    if (!close_output(f, path)) {
        return 1;
    }
    DSD_FPRINTF(stderr, "%zu of %zu conditions scaled under the %.2f ceiling\n", scaled, all.size(), o.scene_ceiling);
    return 0;
}

/* Where @p path writes: an existing file's canonical path, else its folder's and its name (a file the run creates);
   "" when neither resolves (the open then says why). */
std::string
output_location(const std::string& path) {
    char resolved[4096];
    if (dsd_path_canonical(path.c_str(), resolved, sizeof(resolved)) == 0) {
        return resolved;
    }
    const size_t cut = path.find_last_of("/\\");
    const std::string folder = cut == std::string::npos ? "." : cut == 0 ? path.substr(0, 1) : path.substr(0, cut);
    if (dsd_path_canonical(folder.c_str(), resolved, sizeof(resolved)) != 0) {
        return std::string();
    }
    return std::string(resolved) + "/" + path.substr(cut == std::string::npos ? 0 : cut + 1);
}

/* False, with the two options named, when two of the run's outputs (--out, --dump, --trial-log, --bounds) are one file:
   the same path however spelled, or two names of one existing file (a hard link). Their writers would overwrite each
   other and the completion record's digests would describe neither; checked before any output is opened. */
bool
outputs_distinct(const Options& o) {
    const std::pair<const char*, const std::string*> outputs[] = {
        {"--out", &o.out}, {"--dump", &o.dump}, {"--trial-log", &o.trial_log}, {"--bounds", &o.bounds}};
    const size_t count = sizeof(outputs) / sizeof(outputs[0]);
    for (size_t a = 0; a < count; ++a) {
        for (size_t b = a + 1; b < count; ++b) {
            const std::string &pa = *outputs[a].second, &pb = *outputs[b].second;
            if (pa.empty() || pb.empty()) {
                continue;
            }
            const std::string la = output_location(pa), lb = output_location(pb);
            int same = 0;
            if (pa == pb || (!la.empty() && la == lb)
                || (dsd_path_same_file(pa.c_str(), pb.c_str(), &same) == 0 && same)) {
                DSD_FPRINTF(stderr, "%s and %s name one file (%s): give each output its own\n", outputs[a].first,
                            outputs[b].first, pa.c_str());
                return false;
            }
        }
    }
    return true;
}

constexpr const char* kUsage =
    "--phase select|validate|snr|cost --out FILE [--sources DIR] [--trials N] [--jobs N] "
    "[--threshold DB --estimator trimmed|lowest] [--guard DB] [--filter TEXT] [--dump FILE] "
    "[--neighbour-levels DB,DB,...] [--geometry default|centred] [--rates HZ,HZ,...] "
    "[--response flat|nominal|steeper|mirrored] [--depression DB] [--adc-noise-db DB] [--modes-file FILE] "
    "[--pilot MODE] [--memory-mb MB] [--trial-log FILE] [--bounds FILE] [--seed-salt N] [--occupancy-veto SHARE] "
    "[--add masks,occupancy] [--windows N] [--scene-ceiling BOUND]\n"
    "responses (2.5, 3, 6, 10 MS/s; tests/io/signal_quiet_scene.h): half-band 0 dB to +-0.42 Fs, -0.35 dB at "
    "+-0.45, -6.02 dB at +-0.5; nominal tuner -3 dB at -0.335/+0.302 Fs (2.5, 3 MS/s), -0.4365/+0.4035 (6), "
    "-0.4665/+0.4335 (10); steeper +-0.28 (2.5, 3), +-0.40 (6, 10); mirrored = nominal swapped; --depression DB over "
    "3 channel widths at the channel; --adc-noise-db DB flat ADC noise over the tuner noise's peak\n";

} // namespace

int
main(int argc, char** argv) {
    if (!centred_rates_match()) {
        return 1;
    }
    Options o;
    if (!parse_options(argc, argv, &o)) {
        DSD_FPRINTF(stderr, "usage: %s %s", argv[0], kUsage);
        return 2;
    }
    if (!outputs_distinct(o)) {
        return 2;
    }
    if (o.phase == "cost") {
        return run_cost(o);
    }
    std::vector<Mode> modes = builtin_modes();
    if (!o.modes_file.empty() && !load_modes(o.modes_file, &modes)) {
        return 1;
    }
    std::vector<Source> sources;
    std::vector<Waveform> waves;
    if (!load_sources(o.sources, &sources, &waves)) {
        return 1;
    }
    if (o.response.shape != qs::Shape::FLAT) {
        parallel((long)waves.size(), o.jobs, [&](long i, Worker&) { fill_psd(&waves[(size_t)i]); });
    }
    const std::vector<Condition> all = build_conditions(sources, waves, modes, o);
    const Inputs in{sources, waves, all};
    std::vector<size_t> chosen;
    if (choose(in, modes, o, &chosen) != 0) {
        return 1;
    }
    DSD_FPRINTF(stderr, "%zu of %zu conditions, %ld trials each, %zu waveform(s) from %zu source row(s)\n",
                chosen.size(), all.size(), o.trials, waves.size(), sources.size());
    /* What this run executes, for the campaign's checker to hold to the plan. */
    DSD_FPRINTF(stderr, "run: %s\n", run_text(o).c_str());
    if (!o.bounds.empty() && write_bounds(o.bounds, all, o) != 0) {
        return 1;
    }
    if (o.phase == "snr") {
        return run_snr(o, in, chosen);
    }
    return run_trials(o, in, chosen);
}
