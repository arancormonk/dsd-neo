// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstring>
#include <dsd-neo/core/airspy_config.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/power.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/runtime/airspy_config.h>
#include <dsd-neo/runtime/config.h>
#include <stdio.h>
#include "test_support.h"

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

static void
test_roundtrip() {
    char path[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(path, sizeof path, "airspy-config");
    CHECK(fd >= 0);
    const char text[] = "[input]\nsource=airspy\nairspy_serial=0123456789ABCDEF\nrtl_freq=851.375M\n"
                        "airspy_gain_mode=manual\nairspy_lna_gain=12\nairspy_bias_tee=true\n";
    CHECK(dsd_write(fd, text, sizeof text - 1) == (int)(sizeof text - 1));
    CHECK(dsd_close(fd) == 0);
    static dsdneoUserConfig config;
    CHECK(dsd_user_config_load(path, &config) == 0);
    CHECK(config.input_source == DSDCFG_INPUT_AIRSPY && config.airspy.lna_gain == 12 && config.airspy.bias_tee == 1);
    CHECK(dsd_user_config_save_atomic(path, &config) == 0);
    static dsdneoUserConfig saved;
    CHECK(dsd_user_config_load(path, &saved) == 0);
    CHECK(saved.airspy.gain_mode == DSD_AIRSPY_MANUAL && saved.airspy.sample_rate == 0);
    CHECK(strcmp(saved.airspy.serial, "0123456789ABCDEF") == 0);
    static dsd_opts configured;
    static dsd_state state;
    dsd_apply_user_config_to_opts(&saved, &configured, &state);
    CHECK(dsd_opts_audio_in_dev_is_airspy_spec(configured.audio_in_dev));
    CHECK(configured.airspy.lna_gain == 12 && configured.rtlsdr_center_freq == 851375000);
    CHECK(dsd_airspy_config_set(&configured.airspy, "airspy_serial", "FEDCBA9876543210") == 0);
    CHECK(dsd_normalize_airspy_input_spec(&configured) == 0);
    CHECK(strcmp(configured.airspy.serial, "FEDCBA9876543210") == 0);
    CHECK(remove(path) == 0);
    /* A different CLI override cannot erase an invalid explicit INI selector. */
    saved.airspy_invalid = 1;
    dsd_apply_user_config_to_opts(&saved, &configured, &state);
    CHECK(dsd_airspy_config_set(&configured.airspy, "airspy_sample_rate", "auto") == 0);
    CHECK(dsd_normalize_airspy_input_spec(&configured) != 0);
}

int
main() {
    test_roundtrip();
    static dsd_opts opts;
    dsd_airspy_config_defaults(&opts.airspy);
    CHECK(opts.airspy.sensitivity_gain == 10 && !opts.airspy.bias_tee);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_gain_mode", "manual") == 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_lna_gain", "15") == 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_lna_gain", "16") != 0 && opts.airspy.lna_gain == 15);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_bias_tee", "true") == 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_bias_tee", "2") != 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_sample_rate", "10000000") == 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_sample_rate", "10000001") != 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_sample_rate", "auto") == 0 && opts.airspy.sample_rate == 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_serial", "1234") != 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_lna_gain", "auto") != 0);
    CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_sample_rate", "true") != 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:851.375M:24:0:3");
    CHECK(dsd_normalize_airspy_input_spec(&opts) == 0 && opts.rtl_dsp_bw_khz == 24 && opts.rtl_volume_multiplier == 3
          && dsd_squelch_is_off(opts.rtl_squelch_level));

    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:serial=0123456789abcdef:851.375M");
    CHECK(dsd_normalize_airspy_input_spec(&opts) == 0);
    CHECK(opts.audio_in_type == AUDIO_IN_RTL && opts.rtlsdr_center_freq == 851375000);
    CHECK(strcmp(opts.audio_in_dev, "airspy:serial=0123456789abcdef") == 0);
    CHECK(!dsd_opts_source_uses_effective_input_rate(&opts));
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:serial=invalid:851M");
    CHECK(dsd_normalize_airspy_input_spec(&opts) != 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:23M");
    CHECK(dsd_normalize_airspy_input_spec(&opts) != 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:1701M");
    CHECK(dsd_normalize_airspy_input_spec(&opts) != 0);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", "airspy:851M:extra");
    CHECK(dsd_normalize_airspy_input_spec(&opts) != 0);
    return 0;
}
