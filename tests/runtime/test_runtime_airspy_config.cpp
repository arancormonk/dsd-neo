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
#include <dsd-neo/runtime/log.h>
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

static char last_warning[1024];
static char last_error[1024];

static void
capture_log(dsd_neo_log_level_t level, const char* text, void*) {
    if (level == LOG_LEVEL_WARN) {
        DSD_SNPRINTF(last_warning, sizeof last_warning, "%s", text);
    } else if (level == LOG_LEVEL_ERROR) {
        DSD_SNPRINTF(last_error, sizeof last_error, "%s", text);
    }
}

static void
test_snapshot_threshold() {
    static dsd_opts opts;
    static dsd_state state;
    static dsdneoUserConfig cfg;
    dsd_airspy_config_defaults(&opts.airspy);
    DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "airspy");
    opts.input_warn_db = -55.0;
    dsd_snapshot_opts_to_user_config(&opts, &state, &cfg);
    CHECK(cfg.input_warn_db_is_set && cfg.input_warn_db == -55.0);
    FILE* out = tmpfile();
    CHECK(out != NULL);
    dsd_user_config_render_ini(&cfg, out);
    rewind(out);
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof line, out)) {
        found |= strstr(line, "input_warn_db = -55.0") != NULL;
    }
    CHECK(fclose(out) == 0);
    CHECK(found);
}

static void
test_invalid_config() {
    const char* invalid[] = {"airspy_vga_gain=99",     "airspy_lna_agc=2",     "airspy_bias_tee=maybe",
                             "airspy_sample_rate=100", "airspy_gain_mode=bad", "airspy_serial=bad"};
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        char path[DSD_TEST_PATH_MAX];
        int fd = dsd_test_mkstemp(path, sizeof path, "airspy-invalid");
        CHECK(fd >= 0);
        char text[256];
        int n = DSD_SNPRINTF(text, sizeof text, "[input]\nsource=airspy\nairspy_vga_gain=5\n%s\n", invalid[i]);
        CHECK(dsd_write(fd, text, (size_t)n) == n);
        CHECK(dsd_close(fd) == 0);
        static dsdneoUserConfig cfg;
        last_warning[0] = '\0';
        CHECK(dsd_user_config_load(path, &cfg) == 0);
        char key[80];
        DSD_SNPRINTF(key, sizeof key, "%s", invalid[i]);
        *strchr(key, '=') = '\0';
        CHECK(strstr(last_warning, key) != NULL);
        CHECK(strstr(last_warning, strchr(invalid[i], '=') + 1) != NULL);
        CHECK(cfg.airspy.vga_gain == 5);
        dsdcfg_diagnostics_t diags;
        dsdcfg_diags_init(&diags);
        (void)dsd_user_config_validate(path, &diags);
        CHECK(diags.count > 0);
        bool diagnosed = false;
        for (int j = 0; j < diags.count; ++j) {
            diagnosed |= strcmp(diags.items[j].key, key) == 0;
        }
        CHECK(diagnosed);
        dsdcfg_diags_free(&diags);
        static dsd_opts opts;
        static dsd_state state;
        dsd_apply_user_config_to_opts(&cfg, &opts, &state);
        if (strcmp(key, "airspy_serial") == 0) {
            last_error[0] = '\0';
            CHECK(dsd_normalize_airspy_input_spec(&opts) != 0);
            CHECK(strstr(last_error, "airspy_serial") != NULL);
            CHECK(dsd_airspy_config_set(&opts.airspy, "airspy_vga_gain", "6") == 0);
            CHECK(dsd_normalize_airspy_input_spec(&opts) != 0);
            DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "airspy:serial=0123456789ABCDEF:851M");
            CHECK(dsd_normalize_airspy_input_spec(&opts) == 0);
            CHECK(opts.airspy_config_error == 0);
        } else {
            CHECK(dsd_normalize_airspy_input_spec(&opts) == 0);
        }
        CHECK(remove(path) == 0);
    }
}

static void
test_forgiving_tail() {
    static dsd_opts opts;
    dsd_airspy_config_defaults(&opts.airspy);

    const struct {
        const char* spec;
        int bandwidth;
        int volume;
        const char* warning_field;
    } cases[] = {{"airspy:851.375M:25:0:2", 48, 2, "bandwidth"},     {"airspy:851.375M:12:0:0", 12, 0, ""},
                 {"airspy:851.375M:24:-55.5:9", 24, 3, "volume"},    {"airspy:851.375M:24:bad:-1", 24, 0, "volume"},
                 {"airspy:851.375M:extra:bad:bad", 48, 0, "volume"}, {"airspy:851.375M:24:nan:2", 24, 2, "squelch"}};

    for (const auto& c : cases) {
        DSD_SNPRINTF(opts.audio_in_dev, sizeof opts.audio_in_dev, "%s", c.spec);
        double old_sql = opts.rtl_squelch_level;
        last_warning[0] = '\0';
        CHECK(dsd_normalize_airspy_input_spec(&opts) == 0);
        CHECK(opts.rtlsdr_center_freq == 851375000 && opts.rtl_dsp_bw_khz == c.bandwidth
              && opts.rtl_volume_multiplier == c.volume);
        CHECK(strstr(last_warning, c.warning_field) != NULL);
        if (strstr(c.spec, ":bad:") || strstr(c.spec, ":nan:")) {
            CHECK(opts.rtl_squelch_level == old_sql);
        }
    }
}

int
main(int argc, char** argv) {
    dsd_neo_log_set_tap(capture_log, NULL);
    if (argc > 1) {
        if (strcmp(argv[1], "M1") == 0) {
            test_snapshot_threshold();
        }
        if (strcmp(argv[1], "M2") == 0) {
            test_invalid_config();
        }
        if (strcmp(argv[1], "B4") == 0) {
            test_forgiving_tail();
        }
        return 0;
    }
    test_snapshot_threshold();
    test_invalid_config();
    test_forgiving_tail();
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
    CHECK(dsd_normalize_airspy_input_spec(&opts) == 0 && opts.rtl_dsp_bw_khz == 48);
    return 0;
}
