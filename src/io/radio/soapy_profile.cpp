// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "soapy_profile.h"

#include <dsd-neo/core/input_level.h>
#include <dsd-neo/core/parse.h>
#include <dsd-neo/runtime/analog_channel.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits.h>
#include <limits>
#include <sstream>
#include <utility>

namespace dsdneo {

namespace {

std::string
lower_copy(const std::string& value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

bool
contains_lower(const std::string& lower_haystack, const char* needle) {
    if (!needle || !*needle) {
        return false;
    }
    return lower_haystack.find(needle) != std::string::npos;
}

bool
contains_any_lower(const std::string& lower_haystack, const char* const* needles, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (contains_lower(lower_haystack, needles[i])) {
            return true;
        }
    }
    return false;
}

bool
equals_ci(const std::string& lhs, const char* rhs) {
    return rhs && lower_copy(lhs) == rhs;
}

std::string
trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace((unsigned char)value[start])) {
        start++;
    }
    size_t end = value.size();
    while (end > start && std::isspace((unsigned char)value[end - 1])) {
        end--;
    }
    return value.substr(start, end - start);
}

bool
parse_setting_item(const std::string& item, SoapySettingRequest* out_request, std::string* out_error) {
    if (!out_request) {
        return false;
    }
    const std::string trimmed = trim_copy(item);
    if (trimmed.empty()) {
        if (out_error) {
            *out_error = "empty Soapy setting item";
        }
        return false;
    }

    const size_t eq = trimmed.find('=');
    if (eq == std::string::npos) {
        if (out_error) {
            *out_error = "Soapy setting '" + trimmed + "' must use key=value syntax";
        }
        return false;
    }

    SoapySettingScope scope = SoapySettingScope::Device;
    size_t key_start = 0;
    const size_t colon = trimmed.find(':');
    if (colon != std::string::npos && colon < eq) {
        const std::string scope_text = trim_copy(trimmed.substr(0, colon));
        if (scope_text == "rx" || scope_text == "rx0") {
            scope = SoapySettingScope::Rx0;
        } else {
            if (out_error) {
                *out_error = "unknown Soapy setting scope '" + scope_text + "'";
            }
            return false;
        }
        key_start = colon + 1;
    }

    const std::string key = trim_copy(trimmed.substr(key_start, eq - key_start));
    const std::string value = trim_copy(trimmed.substr(eq + 1));
    if (key.empty()) {
        if (out_error) {
            *out_error = "Soapy setting '" + trimmed + "' has an empty key";
        }
        return false;
    }
    if (value.empty()) {
        if (out_error) {
            *out_error = "Soapy setting '" + trimmed + "' has an empty value";
        }
        return false;
    }

    out_request->scope = scope;
    out_request->key = key;
    out_request->value = value;
    return true;
}

void
set_error(std::string* out_error, const std::string& text) {
    if (out_error) {
        *out_error = text;
    }
}

std::string
range_text(const SoapyRange& range) {
    std::ostringstream out;
    out << '[' << range.minimum << ", " << range.maximum << ']';
    return out.str();
}

bool
value_in_range(double value, const SoapyRange& range) {
    return std::isfinite(value) && range.minimum <= range.maximum && value >= range.minimum && value <= range.maximum;
}

bool
parse_finite_double(const std::string& value, double* out) {
    if (!out) {
        return false;
    }
    double parsed = 0.0;
    if (dsd_parse_double_strict(value.c_str(), -std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                                &parsed)
        != 0) {
        return false;
    }
    if (!std::isfinite(parsed)) {
        return false;
    }
    *out = parsed;
    return true;
}

const SoapyProfile k_profiles[] = {
    {SoapyProfileId::Auto, "auto", "auto", 0.0, nullptr, 0.0},
    {SoapyProfileId::Generic, "generic", "Generic SoapySDR", 0.0, nullptr, 0.0},
    {SoapyProfileId::Airspy, "airspy", "Airspy", 250000.0, nullptr, 0.0},
    {SoapyProfileId::Sdrplay, "sdrplay", "SDRplay", 200000.0, nullptr, 0.0},
    /* HackRF: its makers list 2 MS/s as the lowest rate and recommend 8 MS/s or more (the MAX2837's narrowest filter
       barely attenuates at the edges of a 2 MS/s capture), so it runs 10 MS/s, a rate the half-band cascade and the
       rate step already serve for a native Airspy. */
    {SoapyProfileId::Hackrf, "hackrf", "HackRF", 1750000.0, nullptr, 10000000.0},
    {SoapyProfileId::Lime, "lime", "LimeSDR", 300000.0, nullptr, 0.0},
    {SoapyProfileId::Pluto, "pluto", "ADALM-Pluto", 300000.0, nullptr, 0.0},
    {SoapyProfileId::Rtlsdr, "rtlsdr", "RTL-SDR", 0.0, nullptr, 0.0},
    {SoapyProfileId::Uhd, "uhd", "UHD/USRP", 0.0, nullptr, 0.0},
    /* SDDC (RX-888 family): no hardware bandwidth control, and the tuner path used by every
       digital voice band is reached through the "VHF" antenna rather than the HF default. */
    {SoapyProfileId::Sddc, "sddc", "SDDC/RX-888", 0.0, "VHF", 0.0},
};

/* A profile and the keywords that name it in a driver key, hardware key or device argument. */
struct keyword_profile {
    const char* const* keywords;
    size_t count;
    SoapyProfileId id;
};

template <size_t N>
keyword_profile
keywords_for(const char* const (&keywords)[N], SoapyProfileId id) {
    return {keywords, N, id};
}

/* The profile @p text names in @p out_id, or false when it names none. An Airspy HF+ (SoapyAirspyHF's "AirspyHF") is
 * named and runs the generic profile: its key holds "airspy", but it is not an Airspy. */
bool
detect_profile_from_text(const std::string& text, SoapyProfileId* out_id) {
    static const char* const k_airspyhf_keywords[] = {"airspyhf", "airspy hf"};
    static const char* const k_airspy_keywords[] = {"airspy"};
    static const char* const k_sdrplay_keywords[] = {"sdrplay", "rsp1", "rsp2", "rspduo", "rspdx"};
    static const char* const k_hackrf_keywords[] = {"hackrf"};
    static const char* const k_lime_keywords[] = {"lime"};
    static const char* const k_pluto_keywords[] = {"pluto", "ad936"};
    static const char* const k_rtlsdr_keywords[] = {"rtlsdr", "rtl-sdr", "rtl_sdr"};
    static const char* const k_uhd_keywords[] = {"uhd", "usrp"};
    static const char* const k_sddc_keywords[] = {"sddc",   "rx888", "rx-888", "rx999",
                                                  "rx-999", "rx666", "rx-666", "bbrf103"};

    static const keyword_profile k_order[] = {
        keywords_for(k_airspyhf_keywords, SoapyProfileId::Generic),
        keywords_for(k_airspy_keywords, SoapyProfileId::Airspy),
        keywords_for(k_sdrplay_keywords, SoapyProfileId::Sdrplay),
        keywords_for(k_hackrf_keywords, SoapyProfileId::Hackrf),
        keywords_for(k_lime_keywords, SoapyProfileId::Lime),
        keywords_for(k_pluto_keywords, SoapyProfileId::Pluto),
        keywords_for(k_rtlsdr_keywords, SoapyProfileId::Rtlsdr),
        keywords_for(k_uhd_keywords, SoapyProfileId::Uhd),
        keywords_for(k_sddc_keywords, SoapyProfileId::Sddc),
    };

    const std::string lower_text = lower_copy(text);
    const keyword_profile* named =
        std::find_if(std::begin(k_order), std::end(k_order), [&lower_text](const keyword_profile& entry) {
            return contains_any_lower(lower_text, entry.keywords, entry.count);
        });
    if (named == std::end(k_order)) {
        return false;
    }
    *out_id = named->id;
    return true;
}

/* Rates and bandwidths within this of each other are the same: drivers report them as doubles from integer tables. */
constexpr double kSoapyExactHz = 1.0;

/* Floating-point error in a range's step arithmetic, far below any step a driver advertises. */
constexpr double kSoapyRoundingHz = 1e-3;

/* How far below the Fs/4 channel floor a filter may be and still keep the channel: the filter chosen for the floor
 * (range_point_at_or_above()) and the warning (soapy_bandwidth_cuts_offset()) use the same margin. */
constexpr double kSoapyFloorToleranceHz = 0.5;

/* The last point of @p range: its maximum, or its last whole step at or below the maximum. A step that passes the
 * maximum by no more than rounding error is the maximum. */
double
range_last_point(const SoapyRange& range) {
    if (!(range.step > 0.0) || range.maximum < range.minimum) {
        return range.maximum;
    }
    double steps = std::floor((range.maximum - range.minimum) / range.step);
    if (range.minimum + (steps + 1.0) * range.step - range.maximum <= kSoapyRoundingHz) {
        steps += 1.0;
    }
    return std::min(range.minimum + std::max(0.0, steps) * range.step, range.maximum);
}

double
snap_to_step(double value, const SoapyRange& range) {
    if (range.step <= 0.0 || range.maximum < range.minimum) {
        return value;
    }
    double steps = std::round((value - range.minimum) / range.step);
    double snapped = range.minimum + steps * range.step;
    if (snapped < range.minimum) {
        snapped = range.minimum;
    }
    /* The last whole step, not the maximum, which need not be one. */
    return std::min(snapped, range_last_point(range));
}

double
nearest_in_one_range(double requested, const SoapyRange& range) {
    double clamped = requested;
    if (clamped < range.minimum) {
        clamped = range.minimum;
    }
    if (clamped > range.maximum) {
        clamped = range.maximum;
    }
    return snap_to_step(clamped, range);
}

} // namespace

const SoapyProfile&
soapy_profile_by_id(SoapyProfileId id) {
    const SoapyProfile* it = std::find_if(std::begin(k_profiles), std::end(k_profiles),
                                          [id](const SoapyProfile& profile) { return profile.id == id; });
    if (it != std::end(k_profiles)) {
        return *it;
    }
    return k_profiles[1];
}

bool
soapy_profile_parse_name(const std::string& value, SoapyProfileId* out_id) {
    if (!out_id) {
        return false;
    }
    if (value.empty()) {
        *out_id = SoapyProfileId::Auto;
        return true;
    }
    const SoapyProfile* it =
        std::find_if(std::begin(k_profiles), std::end(k_profiles),
                     [&value](const SoapyProfile& profile) { return equals_ci(value, profile.name); });
    if (it != std::end(k_profiles)) {
        *out_id = it->id;
        return true;
    }
    return false;
}

SoapyProfileId
soapy_select_profile_id(const SoapyProfileSelection& selection) {
    SoapyProfileId requested = SoapyProfileId::Auto;
    if (soapy_profile_parse_name(selection.requested_profile, &requested) && requested != SoapyProfileId::Auto) {
        return requested;
    }

    SoapyProfileId id = SoapyProfileId::Generic;
    if (detect_profile_from_text(selection.driver_key, &id) || detect_profile_from_text(selection.hardware_key, &id)
        || detect_profile_from_text(selection.args, &id)) {
        return id;
    }
    return SoapyProfileId::Generic;
}

uint8_t
soapy_identity_evidence(const std::string& factory, const std::string& driver_key, const std::string& hardware_key) {
    static const char* const k_local[] = {"hackrf", "rtlsdr", "sdrplay", "plutosdr", "bladerf", "lime", "uhd", "SDDC"};
    uint8_t bits = 0U;
    if (factory == "airspy") {
        bits |= SOAPY_IDENTITY_AIRSPY;
    } else if (factory == "airspyhf") {
        bits |= SOAPY_IDENTITY_AIRSPY_HF;
    } else if (factory == "remote") {
        bits |= SOAPY_IDENTITY_PROXY;
    } else if (std::any_of(std::begin(k_local), std::end(k_local), [&](const char* k) { return factory == k; })) {
        bits |= SOAPY_IDENTITY_OTHER;
    }
    const std::string driver = lower_copy(driver_key), hardware = lower_copy(hardware_key);
    if (driver == "airspy" || hardware == "airspy" || hardware == "airspy mini" || hardware == "airspy r2") {
        bits |= SOAPY_IDENTITY_AIRSPY;
    }
    return bits;
}

SoapyReceiver
soapy_receiver_from_evidence(uint8_t evidence) {
    if (evidence & SOAPY_IDENTITY_PROXY) {
        return SoapyReceiver::Unknown;
    }
    if (evidence & SOAPY_IDENTITY_AIRSPY) {
        return SoapyReceiver::Airspy;
    }
    if (evidence & (SOAPY_IDENTITY_AIRSPY_HF | SOAPY_IDENTITY_OTHER)) {
        return SoapyReceiver::NotAirspy;
    }
    return SoapyReceiver::Unknown;
}

bool
soapy_stream_format_parse_name(const std::string& value, SoapyStreamFormat* out_format) {
    if (!out_format) {
        return false;
    }
    if (value.empty() || equals_ci(value, "auto")) {
        *out_format = SoapyStreamFormat::Auto;
        return true;
    }
    if (equals_ci(value, "cf32")) {
        *out_format = SoapyStreamFormat::CF32;
        return true;
    }
    if (equals_ci(value, "cs16")) {
        *out_format = SoapyStreamFormat::CS16;
        return true;
    }
    return false;
}

const char*
soapy_stream_format_name(SoapyStreamFormat format) {
    switch (format) {
        case SoapyStreamFormat::CF32: return "CF32";
        case SoapyStreamFormat::CS16: return "CS16";
        case SoapyStreamFormat::Auto:
        default: return "auto";
    }
}

bool
soapy_name_list_contains(const std::vector<std::string>& names, const std::string& wanted) {
    return std::any_of(names.begin(), names.end(), [&wanted](const std::string& name) { return name == wanted; });
}

SoapyAntennaChoice
soapy_choose_antenna(const char* profile_default_antenna, const std::string& configured_antenna,
                     double center_freq_hz) {
    const std::string configured = trim_copy(configured_antenna);
    if (!configured.empty()) {
        return {configured, false};
    }
    if (!profile_default_antenna || !*profile_default_antenna) {
        return {std::string(), false};
    }
    if (!(center_freq_hz > kSoapyHfVhfSplitHz)) {
        return {std::string(), false};
    }
    return {std::string(profile_default_antenna), true};
}

SoapyBandwidthChoice
soapy_choose_bandwidth_hz(int tuner_bw_hz, bool tuner_bw_hz_is_set, int soapy_bandwidth_hz,
                          double profile_default_bandwidth_hz) {
    int requested_hz = 0;
    bool explicit_request = false;

    if (tuner_bw_hz_is_set || tuner_bw_hz > 0) {
        requested_hz = tuner_bw_hz;
        explicit_request = true;
    } else if (soapy_bandwidth_hz >= 0) {
        requested_hz = soapy_bandwidth_hz;
        explicit_request = true;
    } else {
        requested_hz = (int)std::lround(profile_default_bandwidth_hz);
    }

    if (requested_hz <= 0) {
        return {0, false, explicit_request};
    }
    return {requested_hz, true, explicit_request};
}

std::string
soapy_choose_stream_format(const std::vector<std::string>& supported_formats, const std::string& requested_format,
                           const std::string& native_format) {
    SoapyStreamFormat requested = SoapyStreamFormat::Auto;
    if (!soapy_stream_format_parse_name(requested_format, &requested)) {
        return std::string();
    }

    if (requested == SoapyStreamFormat::CF32) {
        return soapy_name_list_contains(supported_formats, "CF32") ? "CF32" : std::string();
    }
    if (requested == SoapyStreamFormat::CS16) {
        return soapy_name_list_contains(supported_formats, "CS16") ? "CS16" : std::string();
    }
    if ((native_format == "CF32" || native_format == "CS16")
        && soapy_name_list_contains(supported_formats, native_format)) {
        return native_format;
    }
    if (soapy_name_list_contains(supported_formats, "CF32")) {
        return "CF32";
    }
    if (soapy_name_list_contains(supported_formats, "CS16")) {
        return "CS16";
    }
    return std::string();
}

/* The meter reads CS16 against the same full scale the reader divides by, so its upper bound is the default here. */
static_assert((long)kSoapyCs16DefaultFullScale == DSD_INPUT_LEVEL_CS16_MAX_FULL_SCALE,
              "the SoapySDR CS16 default full scale and the input meter's largest CS16 full scale differ");

double
soapy_cs16_full_scale(const std::string& stream_format, const std::string& native_format, double native_full_scale) {
    if (stream_format == "CS16" && native_format == "CS16" && std::isfinite(native_full_scale)
        && native_full_scale >= 1.0 && native_full_scale <= kSoapyCs16DefaultFullScale) {
        return native_full_scale;
    }
    return kSoapyCs16DefaultFullScale;
}

double
soapy_nearest_in_ranges(double requested, const std::vector<SoapyRange>& ranges, bool* out_supported) {
    if (out_supported) {
        *out_supported = false;
    }
    if (ranges.empty()) {
        return requested;
    }

    double best = 0.0;
    double best_error = 0.0;
    bool have_best = false;
    for (const SoapyRange& range : ranges) {
        if (range.maximum < range.minimum) {
            continue;
        }
        double candidate = nearest_in_one_range(requested, range);
        double error = std::fabs(candidate - requested);
        if (!have_best || error < best_error) {
            best = candidate;
            best_error = error;
            have_best = true;
        }
    }

    if (!have_best) {
        return requested;
    }
    if (out_supported) {
        *out_supported = true;
    }
    return best;
}

namespace {

/* The points of @p range within kSoapyExactHz of @p value, passed to @p consider: for a range with a step, the steps on
 * either side of @p value (both can be that close when the step is small), else the value held to the range's ends. */
template <typename Consider>
void
range_points_near(double value, const SoapyRange& range, Consider consider) {
    if (!std::isfinite(value) || range.maximum < range.minimum || value < range.minimum - kSoapyExactHz
        || value > range.maximum + kSoapyExactHz) {
        return;
    }
    if (!(range.step > 0.0)) {
        consider(std::min(std::max(value, range.minimum), range.maximum));
        return;
    }
    const double last = range_last_point(range);
    const double below = std::floor((value - range.minimum) / range.step);
    for (double steps : {below, below + 1.0}) {
        const double point = std::min(std::max(range.minimum + steps * range.step, range.minimum), last);
        if (std::fabs(point - value) <= kSoapyExactHz) {
            consider(point);
        }
    }
}

/* The narrowest point of @p range at or above @p floor_hz in @p out_hz (one within kSoapyFloorToleranceHz below it
 * counts), or false when it has none. */
bool
range_point_at_or_above(double floor_hz, const SoapyRange& range, double* out_hz) {
    const double last = range_last_point(range);
    if (range.maximum < range.minimum || last < floor_hz - kSoapyFloorToleranceHz) {
        return false;
    }
    double point = std::min(std::max(range.minimum, floor_hz), last);
    if (range.step > 0.0) {
        const double steps = std::ceil((floor_hz - kSoapyFloorToleranceHz - range.minimum) / range.step);
        point = std::min(range.minimum + std::max(0.0, steps) * range.step, last);
    }
    *out_hz = point;
    return true;
}

/* The advertised rate closest to @p rate_hz, and within kSoapyExactHz of it, in @p out_hz: a listed rate or a range
 * point, below @p below_hz. The closest wins whatever the order, so an exact point beats a near one listed first. */
bool
soapy_rate_supported_point(double rate_hz, const std::vector<double>& listed_rates,
                           const std::vector<SoapyRange>& ranges, double* out_hz,
                           double below_hz = std::numeric_limits<double>::max()) {
    if (!(rate_hz > 0.0)) {
        return false;
    }
    bool found = false;
    double best = 0.0;
    /* Of two points equally close, the one that rounds to the rate asked for: the rate is planned and written in whole
       hertz, and the point that rounds to it reads back as it. */
    const long long whole_hz = std::llround(rate_hz);
    const auto better = [&](double point) {
        const double gap = std::fabs(point - rate_hz) - std::fabs(best - rate_hz);
        if (gap < -kSoapyRoundingHz || gap > kSoapyRoundingHz) {
            return gap < 0.0;
        }
        return std::llround(point) == whole_hz && std::llround(best) != whole_hz;
    };
    const auto consider = [&](double point) {
        if (point < below_hz && std::fabs(point - rate_hz) <= kSoapyExactHz && (!found || better(point))) {
            best = point;
            found = true;
        }
    };
    for (double rate : listed_rates) {
        consider(rate);
    }
    for (const SoapyRange& range : ranges) {
        range_points_near(rate_hz, range, consider);
    }
    if (found) {
        *out_hz = best;
    }
    return found;
}

} // namespace

double
soapy_nearest_sample_rate(double requested, const std::vector<double>& listed_rates,
                          const std::vector<SoapyRange>& ranges, bool* out_adjusted) {
    if (out_adjusted) {
        *out_adjusted = false;
    }
    if (requested <= 0.0) {
        return requested;
    }
    /* A rate the list or a range holds is that rate; drivers that list a few rates may also advertise a range. */
    double point = 0.0;
    if (soapy_rate_supported_point(requested, listed_rates, ranges, &point)) {
        if (out_adjusted) {
            *out_adjusted = std::fabs(point - requested) > 0.5;
        }
        return point;
    }
    if (!listed_rates.empty()) {
        double best = listed_rates.front();
        double best_error = std::fabs(best - requested);
        for (double rate : listed_rates) {
            double error = std::fabs(rate - requested);
            if (error < best_error) {
                best = rate;
                best_error = error;
            }
        }
        if (out_adjusted && std::fabs(best - requested) > 0.5) {
            *out_adjusted = true;
        }
        return best;
    }

    bool supported = false;
    double nearest = soapy_nearest_in_ranges(requested, ranges, &supported);
    if (supported && out_adjusted && std::fabs(nearest - requested) > 0.5) {
        *out_adjusted = true;
    }
    return supported ? nearest : requested;
}

/* The point of @p range nearest @p requested among those that round (llround()) to @p max_rate or less, in
 * @p out_point: a point the range holds, which a write of it keeps. A stepped range's points are its grid, whose last
 * one within the limit is found by stepping down from the limit; a continuous range is cut at the limit's whole hertz,
 * or at its own maximum when that rounds within it. False when the range has no such point. Values are compared with
 * the cap's rounding boundary as doubles: a driver's range ends need not fit a 64-bit integer. */
static bool
range_point_within(const SoapyRange& range, double requested, double max_rate, double* out_point) {
    const double cap_boundary = max_rate + 0.5;
    if (!(range.maximum >= range.minimum) || !(range.minimum < cap_boundary)) {
        return false;
    }
    if (!(range.step > 0.0)) {
        const double top =
            range.maximum < cap_boundary ? range.maximum : std::max(range.minimum, std::min(range.maximum, max_rate));
        *out_point = std::min(std::max(requested, range.minimum), top);
        return true;
    }
    /* The highest point within the cap: the range's last point as range_last_point() gives it (a step that passes the
       maximum by rounding error is the maximum, which the setter keeps), else the grid point at or below the cap's
       rounding boundary, stepped down while it rounds over the cap. Each grid point is the setter's own
       minimum + index * step. A grid finer than a double resolves at that magnitude has no distinguishable point below
       the boundary to step to; its largest representable value below the boundary stands for it. */
    /* A step so fine that the range's step count overflows a double names no grid point the setter could keep. */
    if (!std::isfinite((range.maximum - range.minimum) / range.step)) {
        return false;
    }
    double top = range_last_point(range);
    if (!(top < cap_boundary)) {
        double index = std::floor((cap_boundary - range.minimum) / range.step);
        top = range.minimum + index * range.step;
        for (int i = 0; i < 2 && top > range.minimum && !(top < cap_boundary); i++) {
            const double lower_index = index - 1.0;
            const double lower = range.minimum + lower_index * range.step;
            if (!(lower_index < index) || !(lower < top)) {
                top = std::nextafter(cap_boundary, 0.0);
                break;
            }
            index = lower_index;
            top = std::max(lower, range.minimum);
        }
        if (!(top < cap_boundary)) {
            top = std::max(range.minimum, std::nextafter(cap_boundary, 0.0));
        }
    }
    const double nearest = range.minimum + std::floor((requested - range.minimum) / range.step + 0.5) * range.step;
    *out_point = std::min(std::max(nearest, range.minimum), top);
    return true;
}

double
soapy_nearest_sample_rate_within(double requested, double max_rate, const std::vector<double>& listed_rates,
                                 const std::vector<SoapyRange>& ranges, bool* out_found) {
    *out_found = false;
    if (listed_rates.empty() && ranges.empty()) {
        *out_found = requested <= max_rate;
        return *out_found ? requested : 0.0;
    }
    /* A rate is programmed and read back in whole hertz (llround()), so one that rounds to max_rate is within it. */
    const double below = max_rate + 0.5;
    double best = 0.0;
    double best_error = 0.0;
    const auto consider = [&](double rate) {
        const double error = std::fabs(rate - requested);
        if (std::isfinite(rate) && rate > 0.0 && rate < below && (!*out_found || error < best_error)) {
            best = rate;
            best_error = error;
            *out_found = true;
        }
    };
    std::for_each(listed_rates.begin(), listed_rates.end(), consider);
    /* A range's nearest point within the limit competes with the listed rates, whether or not one of them is the
       request itself. */
    for (const SoapyRange& range : ranges) {
        double point = 0.0;
        if (range_point_within(range, requested, max_rate, &point)) {
            consider(point);
        }
    }
    return *out_found ? best : 0.0;
}

bool
soapy_rate_supported_exactly(double rate_hz, const std::vector<double>& listed_rates,
                             const std::vector<SoapyRange>& ranges) {
    double point = 0.0;
    return soapy_rate_supported_point(rate_hz, listed_rates, ranges, &point);
}

double
soapy_profile_sample_rate(const SoapyProfile& profile, double requested, const std::vector<double>& listed_rates,
                          const std::vector<SoapyRange>& ranges, bool* out_adjusted) {
    double point = 0.0;
    /* Only a point that rounds to the preference or below it: the preferred 10 MS/s is the capture ceiling too. */
    if (requested > 0.0 && profile.preferred_sample_rate_hz > 0.0
        && soapy_rate_supported_point(profile.preferred_sample_rate_hz, listed_rates, ranges, &point,
                                      profile.preferred_sample_rate_hz + 0.5)) {
        if (out_adjusted) {
            *out_adjusted = std::fabs(point - requested) > 0.5;
        }
        return point;
    }
    return soapy_nearest_sample_rate(requested, listed_rates, ranges, out_adjusted);
}

bool
soapy_driver_stream_queue(const std::string& driver_key, SoapyStreamQueue* out) {
    /* SoapyHackRF's BUF_LEN (262,144 bytes of CS8) and libhackrf's TRANSFER_COUNT (4), each buffer a USB transfer;
       SoapyHackRF's MTU is one buffer, so a reader holds at most one between its read and the mute. */
    constexpr size_t kHackrfBufferSamples = 131072U;
    constexpr size_t kHackrfTransfersInFlight = 4U;
    constexpr size_t kHackrfQueueBuffers = 4U;
    constexpr size_t kReaderBlocks = 1U;
    static_assert(kHackrfQueueBuffers == 4U, "the queue depth asked for is the one the backlog counts");
    if (!out || !equals_ci(driver_key, "hackrf")) {
        return false;
    }
    out->arg_key = "buffers";
    out->arg_value = "4";
    out->backlog_samples = (kHackrfQueueBuffers + kHackrfTransfersInFlight + kReaderBlocks) * kHackrfBufferSamples;
    return true;
}

double
soapy_fs4_channel_floor_hz(double capture_rate_hz) {
    return capture_rate_hz / 2.0 + (double)DSD_ANALOG_RTL_DSP_BW_MAX_KHZ * 1000.0;
}

double
soapy_floor_bandwidth(double floor_hz, const std::vector<SoapyRange>& ranges) {
    double best = 0.0;
    double widest = 0.0;
    bool have_best = false;
    bool have_widest = false;
    for (const SoapyRange& range : ranges) {
        if (range.maximum < range.minimum) {
            continue;
        }
        double point = 0.0;
        if (range_point_at_or_above(floor_hz, range, &point) && (!have_best || point < best)) {
            best = point;
            have_best = true;
        }
        const double last = range_last_point(range);
        if (!have_widest || last > widest) {
            widest = last;
            have_widest = true;
        }
    }
    if (have_best) {
        return best;
    }
    return have_widest ? widest : floor_hz;
}

bool
soapy_bandwidth_cuts_offset(double applied_hz, double capture_rate_hz) {
    return capture_rate_hz > 0.0 && applied_hz < soapy_fs4_channel_floor_hz(capture_rate_hz) - kSoapyFloorToleranceHz;
}

std::string
soapy_join_names(const std::vector<std::string>& names, size_t max_chars) {
    std::string out;
    for (size_t i = 0; i < names.size(); i++) {
        const std::string& name = names[i];
        size_t needed = out.size() + name.size() + (out.empty() ? 0U : 1U);
        if (max_chars > 0 && needed > max_chars) {
            if (!out.empty()) {
                out += ",...";
            }
            break;
        }
        if (!out.empty()) {
            out += ',';
        }
        out += name;
    }
    return out.empty() ? "-" : out;
}

const char*
soapy_setting_scope_name(SoapySettingScope scope) {
    switch (scope) {
        case SoapySettingScope::Rx0: return "rx0";
        case SoapySettingScope::Device:
        default: return "device";
    }
}

bool
soapy_parse_settings(const std::string& spec, std::vector<SoapySettingRequest>* out_requests, std::string* out_error) {
    if (out_requests) {
        out_requests->clear();
    }
    if (out_error) {
        out_error->clear();
    }
    const std::string trimmed = trim_copy(spec);
    if (trimmed.empty()) {
        return true;
    }

    std::vector<SoapySettingRequest> parsed;
    size_t pos = 0;
    while (pos <= spec.size()) {
        const size_t next = spec.find_first_of(",;", pos);
        const std::string item = spec.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        SoapySettingRequest request = {};
        if (!parse_setting_item(item, &request, out_error)) {
            return false;
        }
        parsed.push_back(request);
        if (next == std::string::npos) {
            break;
        }
        pos = next + 1;
    }

    if (out_requests) {
        *out_requests = std::move(parsed);
    }
    return true;
}

bool
soapy_validate_setting_value(SoapySettingValueType type, const SoapyRange* range, const std::string& value,
                             std::string* out_error) {
    if (out_error) {
        out_error->clear();
    }

    switch (type) {
        case SoapySettingValueType::Bool:
            if (value == "true" || value == "false") {
                return true;
            }
            set_error(out_error, "expected boolean true or false");
            return false;

        case SoapySettingValueType::Int: {
            long parsed = 0;
            if (dsd_parse_long_strict(value.c_str(), 10, LONG_MIN, LONG_MAX, &parsed) != 0) {
                set_error(out_error, "expected integer");
                return false;
            }
            if (range && !value_in_range((double)parsed, *range)) {
                set_error(out_error, "expected integer in reported range " + range_text(*range));
                return false;
            }
            return true;
        }

        case SoapySettingValueType::Float: {
            double parsed = 0.0;
            if (!parse_finite_double(value, &parsed)) {
                set_error(out_error, "expected finite number");
                return false;
            }
            if (range && !value_in_range(parsed, *range)) {
                set_error(out_error, "expected number in reported range " + range_text(*range));
                return false;
            }
            return true;
        }

        case SoapySettingValueType::String:
        default: return true;
    }
}

} // namespace dsdneo
