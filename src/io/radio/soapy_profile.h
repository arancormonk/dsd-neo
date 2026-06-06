// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_SOAPY_PROFILE_H
#define DSD_NEO_IO_RADIO_SOAPY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace dsdneo {

enum class SoapyProfileId : uint8_t {
    Auto = 0,
    Generic,
    Airspy,
    Sdrplay,
    Hackrf,
    Lime,
    Pluto,
    Rtlsdr,
    Uhd,
};

enum class SoapyStreamFormat : uint8_t {
    Auto = 0,
    CF32,
    CS16,
};

enum class SoapySettingScope : uint8_t {
    Device = 0,
    Rx0,
};

enum class SoapySettingValueType : uint8_t {
    Bool = 0,
    Int,
    Float,
    String,
};

struct SoapyRange {
    double minimum;
    double maximum;
    double step;
};

struct SoapyProfile {
    SoapyProfileId id;
    const char* name;
    const char* display_name;
    double default_bandwidth_hz;
};

struct SoapyProfileSelection {
    std::string requested_profile;
    std::string driver_key;
    std::string hardware_key;
    std::string args;
};

struct SoapyBandwidthChoice {
    int bandwidth_hz;
    bool should_apply;
    bool explicit_request;
};

struct SoapySettingRequest {
    SoapySettingScope scope = SoapySettingScope::Device;
    std::string key;
    std::string value;
};

const SoapyProfile& soapy_profile_by_id(SoapyProfileId id);
bool soapy_profile_parse_name(const std::string& value, SoapyProfileId* out_id);
SoapyProfileId soapy_select_profile_id(const SoapyProfileSelection& selection);

bool soapy_stream_format_parse_name(const std::string& value, SoapyStreamFormat* out_format);
const char* soapy_stream_format_name(SoapyStreamFormat format);
std::string soapy_choose_stream_format(const std::vector<std::string>& supported_formats,
                                       const std::string& requested_format, const std::string& native_format);

bool soapy_name_list_contains(const std::vector<std::string>& names, const std::string& wanted);
SoapyBandwidthChoice soapy_choose_bandwidth_hz(int tuner_bw_hz, bool tuner_bw_hz_is_set, int soapy_bandwidth_hz,
                                               double profile_default_bandwidth_hz);
double soapy_nearest_in_ranges(double requested, const std::vector<SoapyRange>& ranges, bool* out_supported);
double soapy_nearest_sample_rate(double requested, const std::vector<double>& listed_rates,
                                 const std::vector<SoapyRange>& ranges, bool* out_adjusted);
std::string soapy_join_names(const std::vector<std::string>& names, size_t max_chars);
const char* soapy_setting_scope_name(SoapySettingScope scope);
bool soapy_parse_settings(const std::string& spec, std::vector<SoapySettingRequest>* out_requests,
                          std::string* out_error);
bool soapy_validate_setting_value(SoapySettingValueType type, const SoapyRange* range, const std::string& value,
                                  std::string* out_error);

} // namespace dsdneo

#endif /* DSD_NEO_IO_RADIO_SOAPY_PROFILE_H */
