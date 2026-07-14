// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for config profile support.
 */

#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/call_alert.h>
#include <dsd-neo/runtime/config.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <direct.h>
#else
#include <unistd.h>
#endif
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

static int
write_temp_config(const char* contents, char* out_path, size_t out_sz) {
    char tmpl[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(tmpl, sizeof(tmpl), "dsdneo_config_prof");
    if (fd < 0) {
        DSD_FPRINTF(stderr, "dsd_test_mkstemp failed: %s\n", strerror(errno));
        return 1;
    }
    size_t len = strlen(contents);
    ssize_t wr = dsd_write(fd, contents, len);
    if (wr < 0 || (size_t)wr != len) {
        DSD_FPRINTF(stderr, "write failed: %s\n", strerror(errno));
        (void)dsd_close(fd);
        (void)remove(tmpl);
        return 1;
    }
    (void)dsd_close(fd);
    DSD_SNPRINTF(out_path, out_sz, "%s", tmpl);
    out_path[out_sz - 1] = '\0';
    return 0;
}

static int
write_config_file(const char* path, const char* contents) {
    FILE* fp = dsd_fopen_private(path, "w");
    if (!fp) {
        DSD_FPRINTF(stderr, "fopen(%s) failed: %s\n", path, strerror(errno));
        return 1;
    }
    if (fputs(contents, fp) < 0) {
        DSD_FPRINTF(stderr, "write(%s) failed: %s\n", path, strerror(errno));
        fclose(fp);
        return 1;
    }
    return fclose(fp) == 0 ? 0 : 1;
}

static void
remove_dir(const char* path) {
#if DSD_PLATFORM_WIN_NATIVE
    (void)_rmdir(path);
#else
    (void)rmdir(path);
#endif
}

static int
test_load_without_profile(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"auto\"\n"
                             "\n"
                             "[profile.test]\n"
                             "mode.decode = \"dmr\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    // Load without profile - should get base config
    int rc = dsd_user_config_load_profile(path, NULL, &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load without profile failed (rc=%d)\n", rc);
        result = 1;
    }

    // Should have decode = auto (base config)
    if (cfg.decode_mode != DSDCFG_MODE_AUTO) {
        DSD_FPRINTF(stderr, "FAIL: expected auto mode, got %d\n", cfg.decode_mode);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_load_with_profile_override(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"auto\"\n"
                             "\n"
                             "[profile.dmr_mode]\n"
                             "mode.decode = \"dmr\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    // Load with profile - should override decode mode
    int rc = dsd_user_config_load_profile(path, "dmr_mode", &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with profile failed (rc=%d)\n", rc);
        result = 1;
    }

    // Should have decode = dmr (from profile)
    if (cfg.decode_mode != DSDCFG_MODE_DMR) {
        DSD_FPRINTF(stderr, "FAIL: profile did not override mode, expected DMR, got %d\n", cfg.decode_mode);
        result = 1;
    }

    // Input source should still be pulse (from base)
    if (cfg.input_source != DSDCFG_INPUT_PULSE) {
        DSD_FPRINTF(stderr, "FAIL: base config lost, expected pulse input, got %d\n", cfg.input_source);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_profile_multiple_overrides(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[output]\n"
                             "backend = \"pulse\"\n"
                             "frontend = \"none\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"auto\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = false\n"
                             "\n"
                             "[profile.p25_trunk]\n"
                             "mode.decode = \"p25p1\"\n"
                             "trunking.enabled = true\n"
                             "output.frontend = terminal\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(path, "p25_trunk", &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with p25_trunk profile failed (rc=%d)\n", rc);
        result = 1;
    }

    // Check all overrides
    if (cfg.decode_mode != DSDCFG_MODE_P25P1) {
        DSD_FPRINTF(stderr, "FAIL: expected p25p1 mode, got %d\n", cfg.decode_mode);
        result = 1;
    }
    if (!cfg.trunk_enabled) {
        DSD_FPRINTF(stderr, "FAIL: trunking should be enabled\n");
        result = 1;
    }
    if (!cfg.frontend_kind || !cfg.frontend_kind_is_set) {
        DSD_FPRINTF(stderr, "FAIL: frontend should be enabled\n");
        result = 1;
    }

    // Check base values not overridden
    if (cfg.input_source != DSDCFG_INPUT_PULSE) {
        DSD_FPRINTF(stderr, "FAIL: input source should still be pulse\n");
        result = 1;
    }
    if (cfg.output_backend != DSDCFG_OUTPUT_PULSE) {
        DSD_FPRINTF(stderr, "FAIL: output backend should still be pulse\n");
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_profile_bool_spellings(void) {
    static const char* ini = "[output]\n"
                             "backend = \"pulse\"\n"
                             "frontend = \"none\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = true\n"
                             "allow_list = false\n"
                             "tune_group_calls = true\n"
                             "tune_private_calls = true\n"
                             "tune_data_calls = false\n"
                             "tune_enc_calls = true\n"
                             "\n"
                             "[profile.bool_spellings]\n"
                             "output.frontend = terminal\n"
                             "trunking.enabled = off\n"
                             "trunking.allow_list = on\n"
                             "trunking.tune_group_calls = off\n"
                             "trunking.tune_private_calls = no\n"
                             "trunking.tune_data_calls = yes\n"
                             "trunking.tune_enc_calls = 0\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(path, "bool_spellings", &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with bool_spellings profile failed (rc=%d)\n", rc);
        result = 1;
    }
    if (!cfg.frontend_kind || !cfg.frontend_kind_is_set) {
        DSD_FPRINTF(stderr, "FAIL: expected frontend on from documented boolean spelling\n");
        result = 1;
    }
    if (cfg.trunk_enabled) {
        DSD_FPRINTF(stderr, "FAIL: expected trunking disabled by documented boolean spelling\n");
        result = 1;
    }
    if (!cfg.trunk_use_allow_list) {
        DSD_FPRINTF(stderr, "FAIL: expected allow_list enabled by documented boolean spelling\n");
        result = 1;
    }
    if (cfg.trunk_tune_group_calls != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected tune_group_calls disabled by documented boolean spelling\n");
        result = 1;
    }
    if (cfg.trunk_tune_private_calls != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected tune_private_calls disabled by documented boolean spelling\n");
        result = 1;
    }
    if (cfg.trunk_tune_data_calls != 1) {
        DSD_FPRINTF(stderr, "FAIL: expected tune_data_calls enabled by documented boolean spelling\n");
        result = 1;
    }
    if (cfg.trunk_tune_enc_calls != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected tune_enc_calls disabled by documented boolean spelling\n");
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_deprecated_keys_translate_to_canonical_fields(void) {
    static const char* ini = "[input]\n"
                             "pulse_input = \"compat-source\"\n"
                             "rtl_auto_ppm = true\n"
                             "[output]\n"
                             "pulse_output = \"compat-sink\"\n"
                             "ncurses_ui = true\n"
                             "[logging]\n"
                             "event_log_file = \"/tmp/dsd-neo-compat-events.log\"\n"
                             "[alerts]\n"
                             "call_alert = true\n"
                             "start = false\n"
                             "end = false\n"
                             "[mode]\n"
                             "decode = provoice\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load(path, &cfg);
    int result = 0;
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: deprecated config keys failed to load (rc=%d)\n", load_rc);
        result = 1;
    }
    if (strcmp(cfg.pulse_input, "compat-source") != 0 || !cfg.rtl_auto_ppm) {
        DSD_FPRINTF(stderr, "FAIL: deprecated input keys did not populate canonical input settings\n");
        result = 1;
    }
    if (strcmp(cfg.pulse_output, "compat-sink") != 0 || !cfg.frontend_kind_is_set || !cfg.frontend_kind) {
        DSD_FPRINTF(stderr, "FAIL: deprecated output keys did not populate canonical output settings\n");
        result = 1;
    }
    if (strcmp(cfg.event_log, "/tmp/dsd-neo-compat-events.log") != 0) {
        DSD_FPRINTF(stderr, "FAIL: event_log_file did not populate event_log\n");
        result = 1;
    }
    if (!cfg.call_alert_enabled || (cfg.call_alert_events & DSD_CALL_ALERT_EVENT_VOICE_START) != 0
        || (cfg.call_alert_events & DSD_CALL_ALERT_EVENT_VOICE_END) != 0) {
        DSD_FPRINTF(stderr, "FAIL: deprecated alert keys did not populate canonical alert settings\n");
        result = 1;
    }
    if (cfg.decode_mode != DSDCFG_MODE_EDACS_PV) {
        DSD_FPRINTF(stderr, "FAIL: provoice decode alias did not select EDACS/ProVoice mode\n");
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_profile_decode_mode_compat_aliases(void) {
    static const char* ini = "[mode]\n"
                             "decode = auto\n"
                             "[profile.alias_p25p1]\n"
                             "mode.decode = p25p1_only\n"
                             "[profile.alias_p25p2]\n"
                             "mode.decode = p25p2_only\n"
                             "[profile.alias_analog]\n"
                             "mode.decode = analog_monitor\n"
                             "[profile.alias_edacs]\n"
                             "mode.decode = edacs\n"
                             "[profile.alias_provoice]\n"
                             "mode.decode = provoice\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    struct decode_alias_case {
        const char* profile_name;
        dsdneoUserDecodeMode expected_mode;
    } cases[] = {
        {"alias_p25p1", DSDCFG_MODE_P25P1},       {"alias_p25p2", DSDCFG_MODE_P25P2},
        {"alias_analog", DSDCFG_MODE_ANALOG},     {"alias_edacs", DSDCFG_MODE_EDACS_PV},
        {"alias_provoice", DSDCFG_MODE_EDACS_PV},
    };

    int result = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        dsdneoUserConfig cfg;
        int load_rc = dsd_user_config_load_profile(path, cases[i].profile_name, &cfg);
        if (load_rc != 0 || cfg.decode_mode != cases[i].expected_mode) {
            DSD_FPRINTF(stderr, "FAIL: decode alias profile %s (rc=%d expected=%d actual=%d)\n", cases[i].profile_name,
                        load_rc, (int)cases[i].expected_mode, (int)cfg.decode_mode);
            result = 1;
        }
    }

    (void)remove(path);
    return result;
}

static int
test_profile_native_frontend_alias_selects_headless(void) {
    static const char* ini = "[output]\n"
                             "frontend = terminal\n"
                             "[profile.compat_native]\n"
                             "output.frontend = native\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load_profile(path, "compat_native", &cfg);
    int result = 0;
    if (load_rc != 0 || !cfg.frontend_kind_is_set || cfg.frontend_kind != DSD_FRONTEND_NONE) {
        DSD_FPRINTF(stderr, "FAIL: native frontend alias did not select headless mode (rc=%d set=%d kind=%d)\n",
                    load_rc, cfg.frontend_kind_is_set, (int)cfg.frontend_kind);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_unknown_profile(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.existing]\n"
                             "mode.decode = \"dmr\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    // Load with non-existent profile
    int rc = dsd_user_config_load_profile(path, "nonexistent", &cfg);

    int result = 0;
    // Should fail for unknown profile
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: unknown profile should return error\n");
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_list_profiles(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.alpha]\n"
                             "mode.decode = \"dmr\"\n"
                             "\n"
                             "[profile.beta]\n"
                             "mode.decode = \"p25p1\"\n"
                             "\n"
                             "[profile.gamma]\n"
                             "mode.decode = \"ysf\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    const char* names[16];
    char names_buf[256];
    int count = dsd_user_config_list_profiles(path, names, names_buf, sizeof(names_buf), 16);

    int result = 0;
    if (count != 3) {
        DSD_FPRINTF(stderr, "FAIL: expected 3 profiles, got %d\n", count);
        result = 1;
    }

    // Check that all profiles are listed
    int found_alpha = 0, found_beta = 0, found_gamma = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], "alpha") == 0) {
            found_alpha = 1;
        }
        if (strcmp(names[i], "beta") == 0) {
            found_beta = 1;
        }
        if (strcmp(names[i], "gamma") == 0) {
            found_gamma = 1;
        }
    }
    if (!found_alpha || !found_beta || !found_gamma) {
        DSD_FPRINTF(stderr, "FAIL: missing profiles in list (alpha=%d, beta=%d, gamma=%d)\n", found_alpha, found_beta,
                    found_gamma);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_list_profiles_empty(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    const char* names[16];
    char names_buf[256];
    int count = dsd_user_config_list_profiles(path, names, names_buf, sizeof(names_buf), 16);

    int result = 0;
    if (count != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected 0 profiles, got %d\n", count);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_profile_rtl_settings(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.rtl_scan]\n"
                             "input.source = \"rtl\"\n"
                             "input.rtl_device = 0\n"
                             "input.rtl_freq = \"851.375M\"\n"
                             "input.rtl_gain = 30\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(path, "rtl_scan", &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with rtl_scan profile failed (rc=%d)\n", rc);
        result = 1;
    }

    if (cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl source, got %d\n", cfg.input_source);
        result = 1;
    }
    if (cfg.rtl_device != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl_device 0, got %d\n", cfg.rtl_device);
        result = 1;
    }
    if (strcmp(cfg.rtl_freq, "851.375M") != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl_freq 851.375M, got %s\n", cfg.rtl_freq);
        result = 1;
    }
    if (cfg.rtl_gain != 30) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl_gain 30, got %d\n", cfg.rtl_gain);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_profile_invalid_int_preserves_inherited_value(void) {
    static const char* ini = "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_gain = 30\n"
                             "\n"
                             "[profile.invalid_gain]\n"
                             "input.rtl_gain = \"invalid\"\n"
                             "input.rtl_ppm = \"invalid\"\n"
                             "input.soapy_bandwidth_hz = \"invalid\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(path, "invalid_gain", &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with invalid_gain profile failed (rc=%d)\n", rc);
        result = 1;
    }
    if (cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl source, got %d\n", cfg.input_source);
        result = 1;
    }
    if (cfg.rtl_gain != 30) {
        DSD_FPRINTF(stderr, "FAIL: expected invalid profile rtl_gain to preserve inherited value 30, got %d\n",
                    cfg.rtl_gain);
        result = 1;
    }
    if (cfg.rtl_ppm != 0 || cfg.rtl_ppm_is_set != 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid profile rtl_ppm should remain unset (value=%d set=%d)\n", cfg.rtl_ppm,
                    cfg.rtl_ppm_is_set);
        result = 1;
    }
    if (cfg.soapy_bandwidth_hz != -1 || cfg.soapy_bandwidth_hz_is_set != 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid profile soapy bandwidth should remain unset (value=%d set=%d)\n",
                    cfg.soapy_bandwidth_hz, cfg.soapy_bandwidth_hz_is_set);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_profile_soapy_settings(void) {
    static const char* ini = "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.soapy_scan]\n"
                             "input.source = \"soapy\"\n"
                             "input.soapy_args = \"driver=airspy,serial=ABC123\"\n"
                             "input.soapy_settings = \"rfnotch_ctrl=true,biasT_ctrl=false\"\n"
                             "input.rtl_freq = \"162.550M\"\n"
                             "input.rtl_gain = 27\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(path, "soapy_scan", &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with soapy_scan profile failed (rc=%d)\n", rc);
        result = 1;
    }
    if (cfg.input_source != DSDCFG_INPUT_SOAPY) {
        DSD_FPRINTF(stderr, "FAIL: expected soapy source, got %d\n", cfg.input_source);
        result = 1;
    }
    if (strcmp(cfg.soapy_args, "driver=airspy,serial=ABC123") != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected soapy_args driver=airspy,serial=ABC123, got %s\n", cfg.soapy_args);
        result = 1;
    }
    if (strcmp(cfg.soapy_settings, "rfnotch_ctrl=true,biasT_ctrl=false") != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected soapy_settings to survive profile overlay, got %s\n", cfg.soapy_settings);
        result = 1;
    }
    if (strcmp(cfg.rtl_freq, "162.550M") != 0) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl_freq 162.550M, got %s\n", cfg.rtl_freq);
        result = 1;
    }
    if (cfg.rtl_gain != 27) {
        DSD_FPRINTF(stderr, "FAIL: expected rtl_gain 27, got %d\n", cfg.rtl_gain);
        result = 1;
    }

    (void)remove(path);
    return result;
}

static int
test_include_directive(void) {
    /* Create included file first */
    static const char* included_ini = "[input]\n"
                                      "source = \"rtl\"\n"
                                      "rtl_device = 2\n"
                                      "rtl_gain = 25\n"
                                      "\n"
                                      "[mode]\n"
                                      "decode = \"dmr\"\n";

    char included_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(included_ini, included_path, sizeof included_path) != 0) {
        return 1;
    }

    /* Create main config that includes the first file */
    char main_ini[512];
    DSD_SNPRINTF(main_ini, sizeof main_ini,
                 "include = \"%s\"\n"
                 "[output]\n"
                 "frontend = \"terminal\"\n",
                 included_path);

    char main_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(main_ini, main_path, sizeof main_path) != 0) {
        (void)remove(included_path);
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(main_path, NULL, &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with include failed (rc=%d)\n", rc);
        result = 1;
    }

    /* Values from included file should be present */
    if (cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "FAIL: include: expected rtl source, got %d\n", cfg.input_source);
        result = 1;
    }
    if (cfg.rtl_device != 2) {
        DSD_FPRINTF(stderr, "FAIL: include: expected rtl_device 2, got %d\n", cfg.rtl_device);
        result = 1;
    }
    if (cfg.rtl_gain != 25) {
        DSD_FPRINTF(stderr, "FAIL: include: expected rtl_gain 25, got %d\n", cfg.rtl_gain);
        result = 1;
    }
    if (cfg.decode_mode != DSDCFG_MODE_DMR) {
        DSD_FPRINTF(stderr, "FAIL: include: expected dmr mode, got %d\n", cfg.decode_mode);
        result = 1;
    }

    /* Values from main file should also be present */
    if (!cfg.frontend_kind || !cfg.frontend_kind_is_set) {
        DSD_FPRINTF(stderr, "FAIL: include: frontend should be true from main config\n");
        result = 1;
    }

    (void)remove(main_path);
    (void)remove(included_path);
    return result;
}

static int
test_include_override(void) {
    /* Create included file with base values */
    static const char* included_ini = "[input]\n"
                                      "source = \"rtl\"\n"
                                      "rtl_gain = 20\n"
                                      "\n"
                                      "[mode]\n"
                                      "decode = \"auto\"\n";

    char included_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(included_ini, included_path, sizeof included_path) != 0) {
        return 1;
    }

    /* Create main config that overrides some values */
    char main_ini[512];
    DSD_SNPRINTF(main_ini, sizeof main_ini,
                 "include = \"%s\"\n"
                 "[input]\n"
                 "rtl_gain = 35\n"
                 "\n"
                 "[mode]\n"
                 "decode = \"p25p1\"\n",
                 included_path);

    char main_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(main_ini, main_path, sizeof main_path) != 0) {
        (void)remove(included_path);
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));

    int rc = dsd_user_config_load_profile(main_path, NULL, &cfg);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: load with include override failed (rc=%d)\n", rc);
        result = 1;
    }

    /* source should come from included file (not overridden) */
    if (cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "FAIL: include override: expected rtl source, got %d\n", cfg.input_source);
        result = 1;
    }

    /* rtl_gain should be overridden by main file */
    if (cfg.rtl_gain != 35) {
        DSD_FPRINTF(stderr, "FAIL: include override: expected rtl_gain 35, got %d\n", cfg.rtl_gain);
        result = 1;
    }

    /* decode should be overridden by main file */
    if (cfg.decode_mode != DSDCFG_MODE_P25P1) {
        DSD_FPRINTF(stderr, "FAIL: include override: expected p25p1 mode, got %d\n", cfg.decode_mode);
        result = 1;
    }

    (void)remove(main_path);
    (void)remove(included_path);
    return result;
}

static int
test_inline_comments_in_include_and_profile(void) {
    static const char* included_ini = "[input]\n"
                                      "source = \"rtl\"\n"
                                      "rtl_device = 6\n";

    char included_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(included_ini, included_path, sizeof included_path) != 0) {
        return 1;
    }

    char main_ini[DSD_TEST_PATH_MAX + 256];
    DSD_SNPRINTF(main_ini, sizeof main_ini,
                 "include = \"%s\"  # relative and absolute include paths may have comments\n"
                 "[mode]\n"
                 "decode = \"auto\"\n"
                 "[profile.commented]\n"
                 "mode.decode = \"dmr\"  # profile value comment\n"
                 "trunking.enabled = true  ; profile boolean comment\n",
                 included_path);

    char main_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(main_ini, main_path, sizeof main_path) != 0) {
        (void)remove(included_path);
        return 1;
    }

    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load_profile(main_path, "commented", &cfg);
    int result = 0;
    if (load_rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: documented include/profile inline comments were rejected (rc=%d)\n", load_rc);
        result = 1;
    }
    if (cfg.input_source != DSDCFG_INPUT_RTL || cfg.rtl_device != 6) {
        DSD_FPRINTF(stderr, "FAIL: inline-comment include did not apply included input settings\n");
        result = 1;
    }
    if (cfg.decode_mode != DSDCFG_MODE_DMR || !cfg.trunk_enabled) {
        DSD_FPRINTF(stderr, "FAIL: inline comments prevented profile settings from applying\n");
        result = 1;
    }

    (void)remove(main_path);
    (void)remove(included_path);
    return result;
}

static int
test_invalid_optional_includes_preserve_root_config(void) {
    char missing_path[DSD_TEST_PATH_MAX];
    if (write_temp_config("", missing_path, sizeof missing_path) != 0) {
        return 1;
    }
    (void)remove(missing_path);

    char missing_include_ini[DSD_TEST_PATH_MAX + 160];
    DSD_SNPRINTF(missing_include_ini, sizeof missing_include_ini,
                 "include = \"%s\"\n"
                 "[mode]\n"
                 "decode = dmr\n"
                 "[profile.override]\n"
                 "mode.decode = p25p1\n",
                 missing_path);

    struct invalid_include_case {
        const char* label;
        const char* contents;
    } cases[] = {
        {"missing include", missing_include_ini},
        {"empty include path", "include = \"\"\n"
                               "[mode]\n"
                               "decode = dmr\n"
                               "[profile.override]\n"
                               "mode.decode = p25p1\n"},
    };

    int result = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char main_path[DSD_TEST_PATH_MAX];
        if (write_temp_config(cases[i].contents, main_path, sizeof main_path) != 0) {
            return 1;
        }

        dsdneoUserConfig cfg;
        int load_rc = dsd_user_config_load(main_path, &cfg);
        if (load_rc != 0 || cfg.decode_mode != DSDCFG_MODE_DMR) {
            DSD_FPRINTF(stderr, "FAIL: %s discarded the root config (load=%d mode=%d)\n", cases[i].label, load_rc,
                        (int)cfg.decode_mode);
            result = 1;
        }

        int profile_load_rc = dsd_user_config_load_profile(main_path, "override", &cfg);
        if (profile_load_rc != 0 || cfg.decode_mode != DSDCFG_MODE_P25P1) {
            DSD_FPRINTF(stderr, "FAIL: %s discarded the root profile (load=%d mode=%d)\n", cases[i].label,
                        profile_load_rc, (int)cfg.decode_mode);
            result = 1;
        }

        (void)remove(main_path);
    }
    return result;
}

static int
test_include_depth_boundary(void) {
    char config_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(config_dir, sizeof config_dir, "dsdneo_config_depth")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    const char* names[] = {"root.ini", "level1.ini", "level2.ini", "level3.ini", "level4.ini"};
    char paths[5][DSD_TEST_PATH_MAX];
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        if (dsd_test_path_join(paths[i], sizeof paths[i], config_dir, names[i]) != 0) {
            remove_dir(config_dir);
            return 1;
        }
    }

    int write_failed = write_config_file(paths[0], "include = \"level1.ini\"\n[mode]\ndecode = dmr\n")
                       || write_config_file(paths[1], "include = \"level2.ini\"\n")
                       || write_config_file(paths[2], "include = \"level3.ini\"\n")
                       || write_config_file(paths[3], "[input]\nsource = \"rtl\"\n")
                       || write_config_file(paths[4], "[input]\nsource = \"pulse\"\n");
    if (write_failed) {
        for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
            (void)remove(paths[i]);
        }
        remove_dir(config_dir);
        return 1;
    }

    dsdneoUserConfig cfg;
    int load_rc = dsd_user_config_load_profile(paths[0], NULL, &cfg);
    int result = 0;
    if (load_rc != 0 || cfg.input_source != DSDCFG_INPUT_RTL) {
        DSD_FPRINTF(stderr, "FAIL: include level 3 should load (rc=%d source=%d)\n", load_rc, cfg.input_source);
        result = 1;
    }

    if (write_config_file(paths[3], "include = \"level4.ini\"\n") != 0) {
        result = 1;
    } else {
        load_rc = dsd_user_config_load_profile(paths[0], NULL, &cfg);
        if (load_rc != 0 || cfg.decode_mode != DSDCFG_MODE_DMR || cfg.input_source != DSDCFG_INPUT_UNSET) {
            DSD_FPRINTF(stderr,
                        "FAIL: over-depth include should be skipped without discarding root settings "
                        "(rc=%d source=%d mode=%d)\n",
                        load_rc, (int)cfg.input_source, (int)cfg.decode_mode);
            result = 1;
        }
    }

    for (size_t i = 0; i < sizeof paths / sizeof paths[0]; i++) {
        (void)remove(paths[i]);
    }
    remove_dir(config_dir);
    return result;
}

static int
test_include_persisted_v1_load_boundary(void) {
    struct persisted_include_case {
        const char* label;
        const char* contents;
        int include_should_apply;
    } cases[] = {
        {"persisted version 1 include", "version = 1\n\n[input]\nsource = \"rtl\"\nrtl_device = 4\n", 1},
        {"unsupported version include", "version = 2\n\n[input]\nsource = \"rtl\"\nrtl_device = 4\n", 0},
        {"non-integer version include", "version = old\n\n[input]\nsource = \"rtl\"\nrtl_device = 4\n", 0},
    };

    int result = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        char included_path[DSD_TEST_PATH_MAX];
        if (write_temp_config(cases[i].contents, included_path, sizeof included_path) != 0) {
            return 1;
        }

        char main_ini[DSD_TEST_PATH_MAX + 96];
        DSD_SNPRINTF(main_ini, sizeof main_ini, "include = \"%s\"\n[mode]\ndecode = dmr\n", included_path);
        char main_path[DSD_TEST_PATH_MAX];
        if (write_temp_config(main_ini, main_path, sizeof main_path) != 0) {
            (void)remove(included_path);
            return 1;
        }

        dsdneoUserConfig cfg;
        int load_rc = dsd_user_config_load_profile(main_path, NULL, &cfg);
        if (load_rc != 0 || cfg.decode_mode != DSDCFG_MODE_DMR) {
            DSD_FPRINTF(stderr, "%s discarded the root config (rc=%d mode=%d)\n", cases[i].label, load_rc,
                        (int)cfg.decode_mode);
            result = 1;
        } else if (cases[i].include_should_apply) {
            if (load_rc != 0 || cfg.input_source != DSDCFG_INPUT_RTL || cfg.rtl_device != 4) {
                DSD_FPRINTF(stderr, "%s should load through include/profile processing (rc=%d)\n", cases[i].label,
                            load_rc);
                result = 1;
            }
        } else if (cfg.input_source == DSDCFG_INPUT_RTL || cfg.rtl_device == 4) {
            DSD_FPRINTF(stderr, "%s should be skipped without applying values\n", cases[i].label);
            result = 1;
        }

        (void)remove(main_path);
        (void)remove(included_path);
    }
    return result;
}

static int
test_relative_include_resolves_from_config_directory(void) {
    char config_dir[DSD_TEST_PATH_MAX];
    if (!dsd_test_mkdtemp(config_dir, sizeof config_dir, "dsdneo_config_prof_dir")) {
        DSD_FPRINTF(stderr, "dsd_test_mkdtemp failed: %s\n", strerror(errno));
        return 1;
    }

    char included_path[DSD_TEST_PATH_MAX];
    char main_path[DSD_TEST_PATH_MAX];
    int result = 0;
    if (dsd_test_path_join(included_path, sizeof included_path, config_dir, "base.ini") != 0
        || dsd_test_path_join(main_path, sizeof main_path, config_dir, "main.ini") != 0) {
        remove_dir(config_dir);
        return 1;
    }

    static const char* included_ini = "[input]\n"
                                      "source = \"rtl\"\n"
                                      "rtl_device = 3\n";
    static const char* main_ini = "include = \"base.ini\"\n"
                                  "[mode]\n"
                                  "decode = \"dmr\"\n";

    if (write_config_file(included_path, included_ini) != 0 || write_config_file(main_path, main_ini) != 0) {
        (void)remove(main_path);
        (void)remove(included_path);
        remove_dir(config_dir);
        return 1;
    }

    dsdneoUserConfig cfg;
    DSD_MEMSET(&cfg, 0, sizeof(cfg));
    int rc = dsd_user_config_load_profile(main_path, NULL, &cfg);
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: relative include load failed (rc=%d)\n", rc);
        result = 1;
    }
    if (cfg.input_source != DSDCFG_INPUT_RTL || cfg.rtl_device != 3) {
        DSD_FPRINTF(stderr, "FAIL: relative include did not apply input source/device\n");
        result = 1;
    }
    if (cfg.decode_mode != DSDCFG_MODE_DMR) {
        DSD_FPRINTF(stderr, "FAIL: relative include main config decode mode missing\n");
        result = 1;
    }

    (void)remove(main_path);
    (void)remove(included_path);
    remove_dir(config_dir);
    return result;
}

int
main(void) {
    int rc = 0;

    rc |= test_load_without_profile();
    rc |= test_load_with_profile_override();
    rc |= test_profile_multiple_overrides();
    rc |= test_profile_bool_spellings();
    rc |= test_deprecated_keys_translate_to_canonical_fields();
    rc |= test_profile_decode_mode_compat_aliases();
    rc |= test_profile_native_frontend_alias_selects_headless();
    rc |= test_unknown_profile();
    rc |= test_list_profiles();
    rc |= test_list_profiles_empty();
    rc |= test_profile_rtl_settings();
    rc |= test_profile_invalid_int_preserves_inherited_value();
    rc |= test_profile_soapy_settings();
    rc |= test_include_directive();
    rc |= test_include_override();
    rc |= test_inline_comments_in_include_and_profile();
    rc |= test_invalid_optional_includes_preserve_root_config();
    rc |= test_include_depth_boundary();
    rc |= test_include_persisted_v1_load_boundary();
    rc |= test_relative_include_resolves_from_config_directory();

    if (rc == 0) {
        printf("All config_profile tests passed\n");
    }

    return rc;
}
