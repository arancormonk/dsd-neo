// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "soapy_profile.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>

namespace dsdneo {

namespace {

std::string
lower_copy(const std::string& value) {
    std::string out(value);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return out;
}

bool
contains_ci(const std::string& haystack, const char* needle) {
    if (!needle || !*needle) {
        return false;
    }
    return lower_copy(haystack).find(needle) != std::string::npos;
}

bool
equals_ci(const std::string& lhs, const char* rhs) {
    return rhs && lower_copy(lhs) == rhs;
}

const SoapyProfile k_profiles[] = {
    {SoapyProfileId::Auto, "auto", "auto", 0.0},
    {SoapyProfileId::Generic, "generic", "Generic SoapySDR", 0.0},
    {SoapyProfileId::Airspy, "airspy", "Airspy", 250000.0},
    {SoapyProfileId::Sdrplay, "sdrplay", "SDRplay", 200000.0},
    {SoapyProfileId::Hackrf, "hackrf", "HackRF", 1750000.0},
    {SoapyProfileId::Lime, "lime", "LimeSDR", 300000.0},
    {SoapyProfileId::Pluto, "pluto", "ADALM-Pluto", 300000.0},
    {SoapyProfileId::Rtlsdr, "rtlsdr", "RTL-SDR", 0.0},
    {SoapyProfileId::Uhd, "uhd", "UHD/USRP", 0.0},
};

SoapyProfileId
detect_profile_from_text(const std::string& text) {
    if (contains_ci(text, "airspy")) {
        return SoapyProfileId::Airspy;
    }
    if (contains_ci(text, "sdrplay") || contains_ci(text, "rsp1") || contains_ci(text, "rsp2")
        || contains_ci(text, "rspduo") || contains_ci(text, "rspdx")) {
        return SoapyProfileId::Sdrplay;
    }
    if (contains_ci(text, "hackrf")) {
        return SoapyProfileId::Hackrf;
    }
    if (contains_ci(text, "lime")) {
        return SoapyProfileId::Lime;
    }
    if (contains_ci(text, "pluto") || contains_ci(text, "ad936")) {
        return SoapyProfileId::Pluto;
    }
    if (contains_ci(text, "rtlsdr") || contains_ci(text, "rtl-sdr") || contains_ci(text, "rtl_sdr")) {
        return SoapyProfileId::Rtlsdr;
    }
    if (contains_ci(text, "uhd") || contains_ci(text, "usrp")) {
        return SoapyProfileId::Uhd;
    }
    return SoapyProfileId::Generic;
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
    if (snapped > range.maximum) {
        snapped = range.maximum;
    }
    return snapped;
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
    for (const SoapyProfile& profile : k_profiles) {
        if (profile.id == id) {
            return profile;
        }
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
    for (const SoapyProfile& profile : k_profiles) {
        if (equals_ci(value, profile.name)) {
            *out_id = profile.id;
            return true;
        }
    }
    if (equals_ci(value, "sdr-play")) {
        *out_id = SoapyProfileId::Sdrplay;
        return true;
    }
    if (equals_ci(value, "rtl")) {
        *out_id = SoapyProfileId::Rtlsdr;
        return true;
    }
    if (equals_ci(value, "usrp")) {
        *out_id = SoapyProfileId::Uhd;
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

    SoapyProfileId id = detect_profile_from_text(selection.driver_key);
    if (id != SoapyProfileId::Generic) {
        return id;
    }
    id = detect_profile_from_text(selection.hardware_key);
    if (id != SoapyProfileId::Generic) {
        return id;
    }
    id = detect_profile_from_text(selection.args);
    if (id != SoapyProfileId::Generic) {
        return id;
    }
    return SoapyProfileId::Generic;
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
    for (const std::string& name : names) {
        if (name == wanted) {
            return true;
        }
    }
    return false;
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
        requested_hz = (int)(profile_default_bandwidth_hz + 0.5);
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

double
soapy_nearest_in_ranges(double requested, const std::vector<SoapyRange>& ranges, bool* out_supported) {
    if (out_supported) {
        *out_supported = false;
    }
    if (ranges.empty()) {
        return requested;
    }

    double best = 0.0;
    double best_error = std::numeric_limits<double>::infinity();
    for (const SoapyRange& range : ranges) {
        if (range.maximum < range.minimum) {
            continue;
        }
        double candidate = nearest_in_one_range(requested, range);
        double error = std::fabs(candidate - requested);
        if (error < best_error) {
            best = candidate;
            best_error = error;
        }
    }

    if (!std::isfinite(best_error)) {
        return requested;
    }
    if (out_supported) {
        *out_supported = true;
    }
    return best;
}

double
soapy_nearest_sample_rate(double requested, const std::vector<double>& listed_rates,
                          const std::vector<SoapyRange>& ranges, bool* out_adjusted) {
    if (out_adjusted) {
        *out_adjusted = false;
    }
    if (requested <= 0.0) {
        return requested;
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
            out += ",";
        }
        out += name;
    }
    return out.empty() ? "-" : out;
}

} // namespace dsdneo
