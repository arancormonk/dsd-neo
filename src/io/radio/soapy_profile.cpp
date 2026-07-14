// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "soapy_profile.h"

#include <dsd-neo/core/parse.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iterator>
#include <limits.h>
#include <limits>
#include <sstream>

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
    static const char* const k_airspy_keywords[] = {"airspy"};
    static const char* const k_sdrplay_keywords[] = {"sdrplay", "rsp1", "rsp2", "rspduo", "rspdx"};
    static const char* const k_hackrf_keywords[] = {"hackrf"};
    static const char* const k_lime_keywords[] = {"lime"};
    static const char* const k_pluto_keywords[] = {"pluto", "ad936"};
    static const char* const k_rtlsdr_keywords[] = {"rtlsdr", "rtl-sdr", "rtl_sdr"};
    static const char* const k_uhd_keywords[] = {"uhd", "usrp"};

    const std::string lower_text = lower_copy(text);
    if (contains_any_lower(lower_text, k_airspy_keywords, sizeof k_airspy_keywords / sizeof k_airspy_keywords[0])) {
        return SoapyProfileId::Airspy;
    }
    if (contains_any_lower(lower_text, k_sdrplay_keywords, sizeof k_sdrplay_keywords / sizeof k_sdrplay_keywords[0])) {
        return SoapyProfileId::Sdrplay;
    }
    if (contains_any_lower(lower_text, k_hackrf_keywords, sizeof k_hackrf_keywords / sizeof k_hackrf_keywords[0])) {
        return SoapyProfileId::Hackrf;
    }
    if (contains_any_lower(lower_text, k_lime_keywords, sizeof k_lime_keywords / sizeof k_lime_keywords[0])) {
        return SoapyProfileId::Lime;
    }
    if (contains_any_lower(lower_text, k_pluto_keywords, sizeof k_pluto_keywords / sizeof k_pluto_keywords[0])) {
        return SoapyProfileId::Pluto;
    }
    if (contains_any_lower(lower_text, k_rtlsdr_keywords, sizeof k_rtlsdr_keywords / sizeof k_rtlsdr_keywords[0])) {
        return SoapyProfileId::Rtlsdr;
    }
    if (contains_any_lower(lower_text, k_uhd_keywords, sizeof k_uhd_keywords / sizeof k_uhd_keywords[0])) {
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
    return std::any_of(names.begin(), names.end(), [&wanted](const std::string& name) { return name == wanted; });
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
        *out_requests = parsed;
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
