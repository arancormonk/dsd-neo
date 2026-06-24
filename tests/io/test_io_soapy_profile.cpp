// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/io/rtl_device.h"
#include "soapy_profile.h"

using dsdneo::SoapyProfileId;
using dsdneo::SoapyProfileSelection;
using dsdneo::SoapyRange;
using dsdneo::SoapySettingScope;
using dsdneo::SoapySettingValueType;
using dsdneo::SoapyStreamFormat;

static_assert(offsetof(struct rtl_soapy_config, profile) < offsetof(struct rtl_soapy_config, antenna),
              "rtl_soapy_config legacy field order changed");
static_assert(offsetof(struct rtl_soapy_config, antenna) < offsetof(struct rtl_soapy_config, clock_source),
              "rtl_soapy_config legacy field order changed");
static_assert(offsetof(struct rtl_soapy_config, clock_source) < offsetof(struct rtl_soapy_config, gains),
              "rtl_soapy_config legacy field order changed");
static_assert(offsetof(struct rtl_soapy_config, gains) < offsetof(struct rtl_soapy_config, stream_format),
              "rtl_soapy_config legacy field order changed");
static_assert(offsetof(struct rtl_soapy_config, stream_format) < offsetof(struct rtl_soapy_config, bandwidth_hz),
              "rtl_soapy_config legacy field order changed");
static_assert(offsetof(struct rtl_soapy_config, bandwidth_hz) < offsetof(struct rtl_soapy_config, settings),
              "rtl_soapy_config settings must remain appended");

static int
expect_true(const char* label, bool value) {
    if (!value) {
        DSD_FPRINTF(stderr, "%s failed\n", label);
        return 1;
    }
    return 0;
}

static int
expect_profile(const char* label, const SoapyProfileSelection& selection, SoapyProfileId want) {
    SoapyProfileId got = dsdneo::soapy_select_profile_id(selection);
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got profile=%d want=%d\n", label, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int
expect_string(const char* label, const std::string& got, const char* want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", label, got.c_str(), want);
        return 1;
    }
    return 0;
}

static int
expect_cstr(const char* label, const char* got, const char* want) {
    if (std::string(got ? got : "") != want) {
        DSD_FPRINTF(stderr, "%s: got \"%s\" want \"%s\"\n", label, got ? got : "(null)", want);
        return 1;
    }
    return 0;
}

static int
expect_scope(const char* label, SoapySettingScope got, SoapySettingScope want) {
    if (got != want) {
        DSD_FPRINTF(stderr, "%s: got scope=%d want=%d\n", label, (int)got, (int)want);
        return 1;
    }
    return 0;
}

static int
expect_double(const char* label, double got, double want) {
    if (std::fabs(got - want) > 0.5) {
        DSD_FPRINTF(stderr, "%s: got %.3f want %.3f\n", label, got, want);
        return 1;
    }
    return 0;
}

static int
expect_bandwidth(const char* label, const dsdneo::SoapyBandwidthChoice& got, bool should_apply, int bandwidth_hz,
                 bool explicit_request) {
    if (got.should_apply != should_apply || got.bandwidth_hz != bandwidth_hz
        || got.explicit_request != explicit_request) {
        DSD_FPRINTF(stderr, "%s: got apply=%d hz=%d explicit=%d want apply=%d hz=%d explicit=%d\n", label,
                    got.should_apply ? 1 : 0, got.bandwidth_hz, got.explicit_request ? 1 : 0, should_apply ? 1 : 0,
                    bandwidth_hz, explicit_request ? 1 : 0);
        return 1;
    }
    return 0;
}

static int
test_profile_selection(void) {
    int rc = 0;
    rc |= expect_profile("airspy driver", {"", "airspy", "", ""}, SoapyProfileId::Airspy);
    rc |= expect_profile("sdrplay args", {"", "", "", "driver=sdrplay,serial=123"}, SoapyProfileId::Sdrplay);
    rc |= expect_profile("hackrf hardware", {"", "", "HackRF One", ""}, SoapyProfileId::Hackrf);
    rc |= expect_profile("lime driver", {"", "lime", "", ""}, SoapyProfileId::Lime);
    rc |= expect_profile("pluto hardware", {"", "", "AD9363", ""}, SoapyProfileId::Pluto);
    rc |= expect_profile("rtlsdr args", {"", "", "", "driver=rtl_sdr"}, SoapyProfileId::Rtlsdr);
    rc |= expect_profile("uhd hardware", {"", "", "USRP B200", ""}, SoapyProfileId::Uhd);
    rc |= expect_profile("requested overrides auto", {"generic", "airspy", "", ""}, SoapyProfileId::Generic);
    rc |= expect_profile("requested alias rtl", {"rtl", "", "", ""}, SoapyProfileId::Rtlsdr);
    rc |= expect_profile("requested alias usrp", {"usrp", "", "", ""}, SoapyProfileId::Uhd);
    rc |= expect_profile("requested alias sdr-play", {"sdr-play", "", "", ""}, SoapyProfileId::Sdrplay);
    rc |= expect_profile("unknown falls back generic", {"", "custom", "unknown", ""}, SoapyProfileId::Generic);

    SoapyProfileId parsed = SoapyProfileId::Generic;
    rc |= expect_true("empty profile parses auto", dsdneo::soapy_profile_parse_name("", &parsed));
    rc |= expect_true("empty profile returns auto", parsed == SoapyProfileId::Auto);
    rc |= expect_true("profile parse rejects null out", !dsdneo::soapy_profile_parse_name("airspy", nullptr));
    rc |= expect_true("profile parse rejects unknown", !dsdneo::soapy_profile_parse_name("mystery", &parsed));
    rc |= expect_cstr("invalid profile id falls back generic", dsdneo::soapy_profile_by_id((SoapyProfileId)255).name,
                      "generic");
    return rc;
}

static int
test_bandwidth_selection(void) {
    int rc = 0;
    rc |= expect_bandwidth("profile default applies", dsdneo::soapy_choose_bandwidth_hz(0, false, -1, 250000.0), true,
                           250000, false);
    rc |= expect_bandwidth("env auto preserves driver automatic",
                           dsdneo::soapy_choose_bandwidth_hz(0, true, -1, 250000.0), false, 0, true);
    rc |= expect_bandwidth("env auto overrides soapy config default",
                           dsdneo::soapy_choose_bandwidth_hz(0, true, 200000, 250000.0), false, 0, true);
    rc |= expect_bandwidth("soapy config auto preserves driver automatic",
                           dsdneo::soapy_choose_bandwidth_hz(0, false, 0, 250000.0), false, 0, true);
    rc |= expect_bandwidth("soapy config explicit applies",
                           dsdneo::soapy_choose_bandwidth_hz(0, false, 200000, 250000.0), true, 200000, true);
    return rc;
}

static int
test_stream_format_selection(void) {
    int rc = 0;
    std::vector<std::string> both = {"CS16", "CF32"};
    rc |= expect_string("native supported wins", dsdneo::soapy_choose_stream_format(both, "", "CS16"), "CS16");
    rc |= expect_string("forced cf32", dsdneo::soapy_choose_stream_format(both, "cf32", "CS16"), "CF32");
    rc |= expect_string("forced cs16", dsdneo::soapy_choose_stream_format(both, "cs16", "CF32"), "CS16");
    rc |= expect_string("auto cf32 fallback", dsdneo::soapy_choose_stream_format({"CF32"}, "auto", ""), "CF32");
    rc |= expect_string("auto cs16 fallback", dsdneo::soapy_choose_stream_format({"CS16"}, "auto", ""), "CS16");
    rc |= expect_string("unsupported forced format", dsdneo::soapy_choose_stream_format({"CS16"}, "cf32", ""), "");
    rc |= expect_string("unknown forced format", dsdneo::soapy_choose_stream_format(both, "cu8", ""), "");
    rc |= expect_string("no supported formats", dsdneo::soapy_choose_stream_format({}, "", "CS16"), "");

    SoapyStreamFormat format = SoapyStreamFormat::CF32;
    rc |= expect_true("empty stream format parses auto", dsdneo::soapy_stream_format_parse_name("", &format));
    rc |= expect_true("empty stream format returns auto", format == SoapyStreamFormat::Auto);
    rc |= expect_true("stream format rejects null out", !dsdneo::soapy_stream_format_parse_name("CF32", nullptr));
    rc |= expect_true("stream format rejects unknown", !dsdneo::soapy_stream_format_parse_name("cu8", &format));
    rc |= expect_cstr("stream format name cf32", dsdneo::soapy_stream_format_name(SoapyStreamFormat::CF32), "CF32");
    rc |= expect_cstr("stream format name cs16", dsdneo::soapy_stream_format_name(SoapyStreamFormat::CS16), "CS16");
    rc |= expect_cstr("stream format name default", dsdneo::soapy_stream_format_name((SoapyStreamFormat)255), "auto");
    return rc;
}

static int
test_range_selection(void) {
    int rc = 0;
    bool supported = false;
    std::vector<SoapyRange> ranges = {{200000.0, 1000000.0, 100000.0}, {1750000.0, 3000000.0, 250000.0}};
    rc |=
        expect_double("inside stepped range", dsdneo::soapy_nearest_in_ranges(240000.0, ranges, &supported), 200000.0);
    rc |= expect_true("inside supported", supported);
    rc |= expect_double("between ranges", dsdneo::soapy_nearest_in_ranges(1200000.0, ranges, &supported), 1000000.0);
    rc |= expect_double("above ranges", dsdneo::soapy_nearest_in_ranges(4000000.0, ranges, &supported), 3000000.0);
    rc |= expect_double("empty ranges preserve request", dsdneo::soapy_nearest_in_ranges(123.0, {}, &supported), 123.0);
    rc |= expect_true("empty ranges unsupported", !supported);
    rc |= expect_double("invalid ranges preserve request",
                        dsdneo::soapy_nearest_in_ranges(123.0, {{10.0, 1.0, 1.0}}, &supported), 123.0);
    rc |= expect_true("invalid ranges unsupported", !supported);

    bool adjusted = false;
    rc |= expect_double("listed sample rate",
                        dsdneo::soapy_nearest_sample_rate(768000.0, {1000000.0, 2500000.0}, {}, &adjusted), 1000000.0);
    rc |= expect_true("listed sample rate adjusted", adjusted);
    rc |= expect_double("nonpositive sample rate preserved",
                        dsdneo::soapy_nearest_sample_rate(0.0, {1000000.0}, ranges, &adjusted), 0.0);
    rc |= expect_true("nonpositive sample rate not adjusted", !adjusted);
    rc |= expect_double("range sample rate", dsdneo::soapy_nearest_sample_rate(1200000.0, {}, ranges, &adjusted),
                        1000000.0);
    rc |= expect_true("range sample rate adjusted", adjusted);
    rc |= expect_double("no rate ranges preserves request", dsdneo::soapy_nearest_sample_rate(123.0, {}, {}, &adjusted),
                        123.0);
    rc |= expect_true("no rate ranges not adjusted", !adjusted);

    rc |= expect_true("name list contains exact value", dsdneo::soapy_name_list_contains({"CF32", "CS16"}, "CS16"));
    rc |= expect_true("name list misses case mismatch", !dsdneo::soapy_name_list_contains({"CF32"}, "cf32"));
    return rc;
}

static int
test_join_and_scope_names(void) {
    int rc = 0;
    rc |= expect_string("join empty", dsdneo::soapy_join_names({}, 0), "-");
    rc |= expect_string("join unlimited", dsdneo::soapy_join_names({"CF32", "CS16"}, 0), "CF32,CS16");
    rc |= expect_string("join truncated", dsdneo::soapy_join_names({"CF32", "CS16", "CU8"}, 8), "CF32,...");
    rc |= expect_cstr("device scope name", dsdneo::soapy_setting_scope_name(SoapySettingScope::Device), "device");
    rc |= expect_cstr("rx0 scope name", dsdneo::soapy_setting_scope_name(SoapySettingScope::Rx0), "rx0");
    rc |= expect_cstr("default scope name", dsdneo::soapy_setting_scope_name((SoapySettingScope)255), "device");
    return rc;
}

static int
test_settings_parser_accepts_device_and_rx_scopes(void) {
    int rc = 0;
    std::vector<dsdneo::SoapySettingRequest> settings;
    std::string error;
    if (!dsdneo::soapy_parse_settings("rfnotch_ctrl=true; rx:agc_setpoint=-30, rx0:rfgain_sel=4", &settings, &error)) {
        DSD_FPRINTF(stderr, "settings parser rejected valid input: %s\n", error.c_str());
        return 1;
    }
    if (settings.size() != 3U) {
        DSD_FPRINTF(stderr, "settings parser count mismatch: got %zu want 3\n", settings.size());
        return 1;
    }
    rc |= expect_scope("device setting scope", settings[0].scope, SoapySettingScope::Device);
    rc |= expect_string("device setting key", settings[0].key, "rfnotch_ctrl");
    rc |= expect_string("device setting value", settings[0].value, "true");
    rc |= expect_scope("rx setting scope", settings[1].scope, SoapySettingScope::Rx0);
    rc |= expect_string("rx setting key", settings[1].key, "agc_setpoint");
    rc |= expect_string("rx setting value", settings[1].value, "-30");
    rc |= expect_scope("rx0 setting scope", settings[2].scope, SoapySettingScope::Rx0);
    rc |= expect_string("rx0 setting key", settings[2].key, "rfgain_sel");
    rc |= expect_string("rx0 setting value", settings[2].value, "4");

    error = "stale";
    rc |= expect_true("blank settings parse without outputs", dsdneo::soapy_parse_settings("   ", nullptr, &error));
    rc |= expect_true("blank settings clears error", error.empty());
    return rc;
}

static int
expect_settings_parse_rejects(const char* label, const char* spec) {
    std::vector<dsdneo::SoapySettingRequest> settings;
    std::string error;
    if (dsdneo::soapy_parse_settings(spec, &settings, &error)) {
        DSD_FPRINTF(stderr, "%s: parser accepted invalid spec '%s'\n", label, spec);
        return 1;
    }
    if (error.empty()) {
        DSD_FPRINTF(stderr, "%s: parser rejected without an error\n", label);
        return 1;
    }
    return 0;
}

static int
test_settings_parser_rejects_invalid_items(void) {
    int rc = 0;
    rc |= expect_settings_parse_rejects("missing equals", "rfnotch_ctrl");
    rc |= expect_settings_parse_rejects("unknown scope", "tx:rfnotch_ctrl=true");
    rc |= expect_settings_parse_rejects("empty key", "=true");
    rc |= expect_settings_parse_rejects("empty rx key", "rx:=true");
    rc |= expect_settings_parse_rejects("empty value", "rfnotch_ctrl=");
    rc |= expect_settings_parse_rejects("empty separated item", "rfnotch_ctrl=true,,biasT_ctrl=false");
    return rc;
}

static int
expect_setting_value(const char* label, SoapySettingValueType type, const SoapyRange* range, const char* value,
                     bool want_valid) {
    std::string error;
    const bool got_valid = dsdneo::soapy_validate_setting_value(type, range, value, &error);
    if (got_valid != want_valid) {
        DSD_FPRINTF(stderr, "%s: got valid=%d want=%d error='%s'\n", label, got_valid ? 1 : 0, want_valid ? 1 : 0,
                    error.c_str());
        return 1;
    }
    if (!want_valid && error.empty()) {
        DSD_FPRINTF(stderr, "%s: invalid value produced no error\n", label);
        return 1;
    }
    return 0;
}

static int
test_setting_value_validation(void) {
    int rc = 0;
    const SoapyRange agc_range = {-60.0, 0.0, 1.0};
    const SoapyRange float_range = {0.0, 2.0, 0.0};

    rc |= expect_setting_value("bool true accepted", SoapySettingValueType::Bool, NULL, "true", true);
    rc |= expect_setting_value("bool false accepted", SoapySettingValueType::Bool, NULL, "false", true);
    rc |= expect_setting_value("bool typo rejected", SoapySettingValueType::Bool, NULL, "treu", false);
    rc |= expect_setting_value("bool numeric rejected", SoapySettingValueType::Bool, NULL, "1", false);

    rc |= expect_setting_value("int in range accepted", SoapySettingValueType::Int, &agc_range, "-30", true);
    rc |= expect_setting_value("int out of range rejected", SoapySettingValueType::Int, &agc_range, "-90", false);
    rc |= expect_setting_value("int decimal rejected", SoapySettingValueType::Int, &agc_range, "-30.5", false);
    rc |= expect_setting_value("int without range accepted", SoapySettingValueType::Int, NULL, "1234", true);

    rc |= expect_setting_value("float in range accepted", SoapySettingValueType::Float, &float_range, "1.25", true);
    rc |= expect_setting_value("float out of range rejected", SoapySettingValueType::Float, &float_range, "2.5", false);
    rc |= expect_setting_value("float nan rejected", SoapySettingValueType::Float, NULL, "nan", false);
    rc |= expect_setting_value("float suffix rejected", SoapySettingValueType::Float, NULL, "1.0dB", false);

    rc |= expect_setting_value("string accepted", SoapySettingValueType::String, NULL, "driver-specific", true);
    return rc;
}

int
main(void) {
    int rc = 0;
    rc |= test_profile_selection();
    rc |= test_bandwidth_selection();
    rc |= test_stream_format_selection();
    rc |= test_range_selection();
    rc |= test_join_and_scope_names();
    rc |= test_settings_parser_accepts_device_and_rx_scopes();
    rc |= test_settings_parser_rejects_invalid_items();
    rc |= test_setting_value_validation();
    return rc;
}

// NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
