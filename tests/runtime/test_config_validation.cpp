// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-multi-level-implicit-pointer-conversion,clang-analyzer-optin.core.EnumCastOutOfRange)
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Unit tests for config validation with diagnostics.
 */

#include <dsd-neo/runtime/config.h>
#include <dsd-neo/runtime/config_schema.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"
#include "test_support.h"

static int
write_temp_config(const char* contents, char* out_path, size_t out_sz) {
    char tmpl[DSD_TEST_PATH_MAX];
    int fd = dsd_test_mkstemp(tmpl, sizeof(tmpl), "dsdneo_config_val");
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
test_valid_config(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[output]\n"
                             "backend = \"pulse\"\n"
                             "\n"
                             "[mode]\n"
                             "decode = \"auto\"\n"
                             "\n"
                             "[trunking]\n"
                             "enabled = false\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"/tmp/targets.csv\"\n"
                             "idle_dwell_ms = 3000\n"
                             "activity_hold_ms = 1200\n"
                             "\n"
                             "[alerts]\n"
                             "enabled = true\n"
                             "voice_start = true\n"
                             "voice_end = false\n"
                             "data = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: valid config returned error %d\n", rc);
        result = 1;
    }
    if (diags.error_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: valid config has %d errors\n", diags.error_count);
        result = 1;
    }
    if (diags.warning_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: valid config has %d warnings\n", diags.warning_count);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
has_trunk_scan_required_diag(const dsdcfg_diagnostics_t* diags, const char* section, const char* key) {
    if (!diags || !section || !key) {
        return 0;
    }
    for (int i = 0; i < diags->count; i++) {
        if (diags->items[i].level == DSDCFG_DIAG_ERROR && strcmp(diags->items[i].section, section) == 0
            && strcmp(diags->items[i].key, key) == 0 && strstr(diags->items[i].message, "required")) {
            return 1;
        }
    }
    return 0;
}

static int
has_trunk_scan_channel_map_conflict_diag(const dsdcfg_diagnostics_t* diags, const char* section, const char* key) {
    if (!diags || !section || !key) {
        return 0;
    }
    for (int i = 0; i < diags->count; i++) {
        if (diags->items[i].level == DSDCFG_DIAG_ERROR && strcmp(diags->items[i].section, section) == 0
            && strcmp(diags->items[i].key, key) == 0 && strstr(diags->items[i].message, "trunking.chan_csv")) {
            return 1;
        }
    }
    return 0;
}

static int
test_trunk_scan_enabled_requires_targets_csv(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: trunk_scan enabled without targets_csv should cause error\n");
        result = 1;
    }
    if (!has_trunk_scan_required_diag(&diags, "trunk_scan", "targets_csv")) {
        DSD_FPRINTF(stderr, "FAIL: missing trunk_scan targets_csv diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_trunk_scan_rejects_global_channel_map(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[trunking]\n"
                             "chan_csv = \"/tmp/chan.csv\"\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = true\n"
                             "targets_csv = \"/tmp/targets.csv\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: trunk_scan with trunking.chan_csv should cause error\n");
        result = 1;
    }
    if (!has_trunk_scan_channel_map_conflict_diag(&diags, "trunking", "chan_csv")) {
        DSD_FPRINTF(stderr, "FAIL: missing trunk_scan/global channel map diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_trunk_scan_include_composed_targets_csv_is_valid(void) {
    static const char* inc = "version = 1\n"
                             "\n"
                             "[trunk_scan]\n"
                             "targets_csv = \"/tmp/included-targets.csv\"\n";

    char inc_path[DSD_TEST_PATH_MAX];
    if (write_temp_config(inc, inc_path, sizeof inc_path) != 0) {
        return 1;
    }

    char ini[DSD_TEST_PATH_MAX + 128];
    DSD_SNPRINTF(ini, sizeof ini, "version = 1\ninclude = \"%s\"\n\n[trunk_scan]\nenabled = true\n", inc_path);

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        (void)remove(inc_path);
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc != 0 || diags.error_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: include-composed trunk_scan targets_csv should validate (rc=%d errors=%d)\n", rc,
                    diags.error_count);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    (void)remove(inc_path);
    return result;
}

static int
test_profile_trunk_scan_rejects_inherited_channel_map(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[trunking]\n"
                             "chan_csv = \"/tmp/chan.csv\"\n"
                             "\n"
                             "[profile.scan]\n"
                             "trunk_scan.enabled = true\n"
                             "trunk_scan.targets_csv = \"/tmp/targets.csv\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: profile trunk_scan with inherited trunking.chan_csv should cause error\n");
        result = 1;
    }
    if (!has_trunk_scan_channel_map_conflict_diag(&diags, "profile.scan", "trunking.chan_csv")) {
        DSD_FPRINTF(stderr, "FAIL: missing profile trunk_scan/global channel map diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_profile_trunk_scan_enabled_requires_targets_csv(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[profile.scan]\n"
                             "trunk_scan.enabled = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: profile trunk_scan enabled without targets_csv should cause error\n");
        result = 1;
    }
    if (!has_trunk_scan_required_diag(&diags, "profile.scan", "trunk_scan.targets_csv")) {
        DSD_FPRINTF(stderr, "FAIL: missing profile trunk_scan targets_csv diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_profile_trunk_scan_inherits_base_targets_csv(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = false\n"
                             "targets_csv = \"/tmp/base-targets.csv\"\n"
                             "\n"
                             "[profile.scan]\n"
                             "trunk_scan.enabled = true\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc != 0 || diags.error_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: profile trunk_scan should inherit base targets_csv (rc=%d errors=%d)\n", rc,
                    diags.error_count);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
validate_config_stream_contents(const char* contents, dsdcfg_diagnostics_t* diags) {
    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "FAIL: tmpfile() failed for config validation stream\n");
        return -1;
    }

    size_t len = strlen(contents);
    if (len > 0U && fwrite(contents, 1U, len, tmp) != len) {
        DSD_FPRINTF(stderr, "FAIL: fwrite() failed for config validation stream\n");
        fclose(tmp);
        return -1;
    }
    if (fflush(tmp) != 0 || fseek(tmp, 0L, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "FAIL: stream setup failed for config validation stream\n");
        fclose(tmp);
        return -1;
    }

    int rc = dsd_user_config_validate_stream(tmp, diags);
    fclose(tmp);
    return rc;
}

static int
has_global_diag_message(const dsdcfg_diagnostics_t* diags, const char* message) {
    if (!diags || !message) {
        return 0;
    }
    for (int i = 0; i < diags->count; i++) {
        if (diags->items[i].level == DSDCFG_DIAG_ERROR && diags->items[i].section[0] == '\0'
            && diags->items[i].key[0] == '\0' && strstr(diags->items[i].message, message)) {
            return 1;
        }
    }
    return 0;
}

static int
test_validate_stream_rejects_null_stream(void) {
    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate_stream(NULL, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: null config validation stream should return error\n");
        result = 1;
    }
    if (!has_global_diag_message(&diags, "No config stream provided")) {
        DSD_FPRINTF(stderr, "FAIL: missing null stream validation diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    return result;
}

static int
test_validate_stream_runs_composed_trunk_scan_checks(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = true\n";

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = validate_config_stream_contents(ini, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: stream trunk_scan without targets_csv should cause error\n");
        result = 1;
    }
    if (!has_trunk_scan_required_diag(&diags, "trunk_scan", "targets_csv")) {
        DSD_FPRINTF(stderr, "FAIL: missing stream trunk_scan targets_csv diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    return result;
}

static int
test_validate_stream_runs_profile_composed_checks(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[profile.scan]\n"
                             "trunk_scan.enabled = true\n";

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = validate_config_stream_contents(ini, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: stream profile trunk_scan without targets_csv should cause error\n");
        result = 1;
    }
    if (!has_trunk_scan_required_diag(&diags, "profile.scan", "trunk_scan.targets_csv")) {
        DSD_FPRINTF(stderr, "FAIL: missing stream profile trunk_scan targets_csv diagnostic\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    return result;
}

static int
test_validate_stream_accepts_profile_inherited_targets_csv(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[trunk_scan]\n"
                             "enabled = false\n"
                             "targets_csv = \"/tmp/base-targets.csv\"\n"
                             "\n"
                             "[profile.scan]\n"
                             "trunk_scan.enabled = true\n";

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = validate_config_stream_contents(ini, &diags);

    int result = 0;
    if (rc != 0 || diags.error_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: stream profile trunk_scan should inherit base targets_csv (rc=%d errors=%d)\n", rc,
                    diags.error_count);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    return result;
}

static int
test_unknown_key_warning(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "unknown_key = \"value\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    // Should succeed (warnings don't cause failure)
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: unknown key caused failure (rc=%d)\n", rc);
        result = 1;
    }
    // Should have a warning for unknown key
    if (diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no warning for unknown key\n");
        result = 1;
    }

    // Check that the warning mentions the unknown key
    int found_warning = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_WARNING && strstr(diags.items[i].message, "unknown_key")) {
            found_warning = 1;
            break;
        }
    }
    if (!found_warning) {
        DSD_FPRINTF(stderr, "FAIL: warning doesn't mention unknown_key\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_unknown_section_warning(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[unknown_section]\n"
                             "key = \"value\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    // Should succeed (warnings don't cause failure)
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: unknown section caused failure (rc=%d)\n", rc);
        result = 1;
    }
    // Should have a warning for unknown section
    if (diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no warning for unknown section\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_invalid_enum_error(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"invalid_source_type\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    // Should return error for invalid enum
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid enum should cause error\n");
        result = 1;
    }
    if (diags.error_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no error for invalid enum value\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_soapy_source_valid(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"soapy\"\n"
                             "soapy_args = \"driver=airspy\"\n"
                             "soapy_settings = \"rfnotch_ctrl=true,rx:agc_setpoint=-30\"\n"
                             "rtl_freq = \"162.550M\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: soapy source config returned error %d\n", rc);
        result = 1;
    }
    if (diags.error_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: soapy source config has %d errors\n", diags.error_count);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_invalid_source_rejected_after_soapy_added(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"soapyy\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: invalid source soapyy should cause error\n");
        result = 1;
    }
    if (diags.error_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no error for invalid source soapyy\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_int_out_of_range(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_device = 999\n"; // device index out of range [0, 255]

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);
    (void)rc;

    int result = 0;
    // Should have a warning for out-of-range value
    if (diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no warning for out-of-range rtl_device=999\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_int_out_of_range_negative_max(void) {
    // rtl_sql has range [-100, 0], so positive values are out of range
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"rtl\"\n"
                             "rtl_sql = 10\n"; // out of range [-100, 0]

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);
    (void)rc;

    int result = 0;
    // Should have a warning for out-of-range value
    if (diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no warning for out-of-range rtl_sql=10\n");
        result = 1;
    }

    // Check that the warning mentions the out-of-range value
    int found_warning = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_WARNING && strstr(diags.items[i].key, "rtl_sql")
            && strstr(diags.items[i].message, "out of range")) {
            found_warning = 1;
            break;
        }
    }
    if (!found_warning) {
        DSD_FPRINTF(stderr, "FAIL: missing out-of-range warning for rtl_sql=10\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_diags_have_line_numbers(void) {
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "bad_key = \"value\"\n"; // line 5

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    dsd_user_config_validate(path, &diags);

    int result = 0;
    // Check that diagnostics have line numbers
    int found_line_num = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].line_number > 0) {
            found_line_num = 1;
            // The bad_key should be on line 5
            if (strstr(diags.items[i].key, "bad_key") && diags.items[i].line_number == 5) {
                break;
            }
        }
    }
    if (diags.count > 0 && !found_line_num) {
        DSD_FPRINTF(stderr, "FAIL: diagnostics missing line numbers\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_empty_config(void) {
    static const char* ini = "";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    // Empty config should not crash and should succeed (no errors)
    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: empty config returned error %d\n", rc);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_nonexistent_file(void) {
    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate("/nonexistent/path/config.ini", &diags);

    // Should return error for nonexistent file
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: nonexistent file should return error\n");
        dsdcfg_diags_free(&diags);
        return 1;
    }

    dsdcfg_diags_free(&diags);
    return 0;
}

static int
test_profile_invalid_enum(void) {
    // Profile with invalid enum value - should produce error
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.test]\n"
                             "mode.decode = \"invalid_mode\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    // Should return error for invalid enum in profile
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: profile with invalid enum should cause error\n");
        result = 1;
    }
    if (diags.error_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no error for invalid enum in profile\n");
        result = 1;
    }

    // Check error mentions the invalid value
    int found_error = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_ERROR && strstr(diags.items[i].message, "invalid_mode")) {
            found_error = 1;
            break;
        }
    }
    if (!found_error) {
        DSD_FPRINTF(stderr, "FAIL: error doesn't mention invalid_mode\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_profile_int_out_of_range(void) {
    // Profile with out-of-range integer - should produce warning
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"rtl\"\n"
                             "\n"
                             "[profile.test]\n"
                             "input.rtl_device = 999\n"; // out of range [0, 255]

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    dsd_user_config_validate(path, &diags);

    int result = 0;
    // Should have warning for out-of-range value in profile
    if (diags.warning_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no warning for out-of-range value in profile\n");
        result = 1;
    }

    // Check warning mentions out of range
    int found_warning = 0;
    for (int i = 0; i < diags.count; i++) {
        if (diags.items[i].level == DSDCFG_DIAG_WARNING && strstr(diags.items[i].message, "out of range")) {
            found_warning = 1;
            break;
        }
    }
    if (!found_warning) {
        DSD_FPRINTF(stderr, "FAIL: warning doesn't mention out of range for profile value\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_profile_invalid_bool(void) {
    // Profile with invalid boolean - should produce error
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.test]\n"
                             "trunking.enabled = \"maybe\"\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    // Should return error for invalid boolean in profile
    if (rc == 0) {
        DSD_FPRINTF(stderr, "FAIL: profile with invalid bool should cause error\n");
        result = 1;
    }
    if (diags.error_count == 0) {
        DSD_FPRINTF(stderr, "FAIL: no error for invalid bool in profile\n");
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_profile_valid_values(void) {
    // Profile with valid values - should pass validation
    static const char* ini = "version = 1\n"
                             "\n"
                             "[input]\n"
                             "source = \"pulse\"\n"
                             "\n"
                             "[profile.p25_trunk]\n"
                             "mode.decode = \"p25p1\"\n"
                             "trunking.enabled = true\n"
                             "input.rtl_gain = 30\n";

    char path[DSD_TEST_PATH_MAX];
    if (write_temp_config(ini, path, sizeof path) != 0) {
        return 1;
    }

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));

    int rc = dsd_user_config_validate(path, &diags);

    int result = 0;
    if (rc != 0) {
        DSD_FPRINTF(stderr, "FAIL: valid profile config returned error %d\n", rc);
        result = 1;
    }
    if (diags.error_count > 0) {
        DSD_FPRINTF(stderr, "FAIL: valid profile config has %d errors\n", diags.error_count);
        result = 1;
    }

    dsdcfg_diags_free(&diags);
    (void)remove(path);
    return result;
}

static int
test_schema_accessors_and_type_names(void) {
    int result = 0;
    int count = dsdcfg_schema_count();
    if (count <= 0) {
        DSD_FPRINTF(stderr, "FAIL: schema count should be positive\n");
        return 1;
    }

    if (dsdcfg_schema_get(-1) != NULL || dsdcfg_schema_get(count) != NULL) {
        DSD_FPRINTF(stderr, "FAIL: schema_get accepted out-of-range index\n");
        result = 1;
    }

    const dsdcfg_schema_entry_t* source = dsdcfg_schema_find("INPUT", "SOURCE");
    if (!source || strcmp(source->section, "input") != 0 || strcmp(source->key, "source") != 0
        || source->type != DSDCFG_TYPE_ENUM || !source->allowed || !strstr(source->allowed, "rtltcp")) {
        DSD_FPRINTF(stderr, "FAIL: schema_find failed for case-insensitive input.source\n");
        result = 1;
    }
    if (dsdcfg_schema_find(NULL, "source") != NULL || dsdcfg_schema_find("input", NULL) != NULL
        || dsdcfg_schema_find("input", "missing") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: schema_find accepted null or unknown key\n");
        result = 1;
    }

    const char* desc = dsd_config_key_description("recording", "rdio_mode");
    if (!desc || !strstr(desc, "rdio-scanner")) {
        DSD_FPRINTF(stderr, "FAIL: description lookup failed for recording.rdio_mode\n");
        result = 1;
    }
    if (dsd_config_key_description("recording", "missing") != NULL) {
        DSD_FPRINTF(stderr, "FAIL: description lookup accepted unknown key\n");
        result = 1;
    }
    if (strcmp(dsdcfg_type_name(DSDCFG_TYPE_STRING), "string") != 0
        || strcmp(dsdcfg_type_name(DSDCFG_TYPE_INT), "int") != 0
        || strcmp(dsdcfg_type_name(DSDCFG_TYPE_BOOL), "bool") != 0
        || strcmp(dsdcfg_type_name(DSDCFG_TYPE_ENUM), "enum") != 0
        || strcmp(dsdcfg_type_name(DSDCFG_TYPE_PATH), "path") != 0
        || strcmp(dsdcfg_type_name(DSDCFG_TYPE_FREQ), "freq") != 0
        || strcmp(dsdcfg_type_name((dsdcfg_type_t)99), "unknown") != 0) {
        DSD_FPRINTF(stderr, "FAIL: type-name mapping mismatch\n");
        result = 1;
    }

    const char* sections[16];
    DSD_MEMSET(sections, 0, sizeof(sections));
    int section_count = dsdcfg_schema_sections(sections, 16);
    if (section_count < 8 || !sections[0] || strcmp(sections[0], "input") != 0) {
        DSD_FPRINTF(stderr, "FAIL: schema_sections did not return expected section list\n");
        result = 1;
    }
    if (dsdcfg_schema_sections(NULL, 16) != 0 || dsdcfg_schema_sections(sections, 0) != 0) {
        DSD_FPRINTF(stderr, "FAIL: schema_sections accepted invalid output arguments\n");
        result = 1;
    }
    const char* one_section[1] = {NULL};
    if (dsdcfg_schema_sections(one_section, 1) != 1 || !one_section[0] || strcmp(one_section[0], "input") != 0) {
        DSD_FPRINTF(stderr, "FAIL: schema_sections did not honor max_sections\n");
        result = 1;
    }

    return result;
}

static int
test_diagnostics_direct_api_and_print_formats(void) {
    dsdcfg_diags_init(NULL);
    dsdcfg_diags_add(NULL, DSDCFG_DIAG_ERROR, 1, "input", "source", "ignored");
    dsdcfg_diags_free(NULL);
    dsdcfg_diags_print(NULL, stderr, NULL);

    dsdcfg_diagnostics_t diags;
    DSD_MEMSET(&diags, 0, sizeof(diags));
    dsdcfg_diags_init(&diags);
    if (diags.items != NULL || diags.count != 0 || diags.capacity != 0 || diags.error_count != 0
        || diags.warning_count != 0) {
        DSD_FPRINTF(stderr, "FAIL: diagnostics init did not clear structure\n");
        return 1;
    }

    dsdcfg_diags_add(&diags, DSDCFG_DIAG_WARNING, 7, "input", "source", "source warning");
    dsdcfg_diags_add(&diags, DSDCFG_DIAG_ERROR, 9, "mode", "decode", "invalid mode");
    dsdcfg_diags_add(&diags, DSDCFG_DIAG_INFO, 3, "trunking", NULL, "section info");
    dsdcfg_diags_add(&diags, DSDCFG_DIAG_INFO, 0, NULL, NULL, "global info");
    if (diags.count != 4 || diags.warning_count != 1 || diags.error_count != 1) {
        DSD_FPRINTF(stderr, "FAIL: diagnostics counts mismatch\n");
        dsdcfg_diags_free(&diags);
        return 1;
    }

    int result = 0;
    if (strcmp(diags.items[0].section, "input") != 0 || strcmp(diags.items[0].key, "source") != 0
        || strcmp(diags.items[0].message, "source warning") != 0) {
        DSD_FPRINTF(stderr, "FAIL: diagnostic field storage mismatch\n");
        result = 1;
    }

    FILE* tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "FAIL: tmpfile() failed\n");
        dsdcfg_diags_free(&diags);
        return 1;
    }

    dsdcfg_diags_print(&diags, tmp, "config.ini");
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "FAIL: fseek() failed for diagnostics output\n");
        fclose(tmp);
        dsdcfg_diags_free(&diags);
        return 1;
    }
    char output[2048];
    size_t n = fread(output, 1, sizeof(output) - 1U, tmp);
    output[n] = '\0';
    fclose(tmp);

    if (!strstr(output, "config.ini:7: warning [input] source: source warning")
        || !strstr(output, "config.ini:9: error [mode] decode: invalid mode")
        || !strstr(output, "config.ini:3: info [trunking]: section info") || !strstr(output, "info: global info")
        || !strstr(output, "Summary: 1 error(s), 1 warning(s)")) {
        DSD_FPRINTF(stderr, "FAIL: diagnostics path print output mismatch\n%s\n", output);
        result = 1;
    }

    tmp = tmpfile();
    if (!tmp) {
        DSD_FPRINTF(stderr, "FAIL: tmpfile() failed for no-path diagnostics output\n");
        dsdcfg_diags_free(&diags);
        return 1;
    }
    dsdcfg_diags_print(&diags, tmp, NULL);
    if (fseek(tmp, 0, SEEK_SET) != 0) {
        DSD_FPRINTF(stderr, "FAIL: fseek() failed for no-path diagnostics output\n");
        fclose(tmp);
        dsdcfg_diags_free(&diags);
        return 1;
    }
    n = fread(output, 1, sizeof(output) - 1U, tmp);
    output[n] = '\0';
    fclose(tmp);
    if (!strstr(output, "line 7: warning [input] source: source warning")) {
        DSD_FPRINTF(stderr, "FAIL: diagnostics no-path line format mismatch\n%s\n", output);
        result = 1;
    }

    dsdcfg_diags_print(&diags, NULL, NULL);
    dsdcfg_diags_free(&diags);
    if (diags.items != NULL || diags.count != 0 || diags.capacity != 0 || diags.error_count != 0
        || diags.warning_count != 0) {
        DSD_FPRINTF(stderr, "FAIL: diagnostics free did not clear structure\n");
        result = 1;
    }

    return result;
}

int
main(void) {
    int rc = 0;

    rc |= test_schema_accessors_and_type_names();
    rc |= test_diagnostics_direct_api_and_print_formats();
    rc |= test_valid_config();
    rc |= test_trunk_scan_enabled_requires_targets_csv();
    rc |= test_trunk_scan_rejects_global_channel_map();
    rc |= test_trunk_scan_include_composed_targets_csv_is_valid();
    rc |= test_profile_trunk_scan_rejects_inherited_channel_map();
    rc |= test_profile_trunk_scan_enabled_requires_targets_csv();
    rc |= test_profile_trunk_scan_inherits_base_targets_csv();
    rc |= test_validate_stream_rejects_null_stream();
    rc |= test_validate_stream_runs_composed_trunk_scan_checks();
    rc |= test_validate_stream_runs_profile_composed_checks();
    rc |= test_validate_stream_accepts_profile_inherited_targets_csv();
    rc |= test_unknown_key_warning();
    rc |= test_unknown_section_warning();
    rc |= test_invalid_enum_error();
    rc |= test_soapy_source_valid();
    rc |= test_invalid_source_rejected_after_soapy_added();
    rc |= test_int_out_of_range();
    rc |= test_int_out_of_range_negative_max();
    rc |= test_diags_have_line_numbers();
    rc |= test_empty_config();
    rc |= test_nonexistent_file();
    rc |= test_profile_invalid_enum();
    rc |= test_profile_int_out_of_range();
    rc |= test_profile_invalid_bool();
    rc |= test_profile_valid_values();

    if (rc == 0) {
        printf("All config_validation tests passed\n");
    }

    return rc;
}

// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion,clang-analyzer-optin.core.EnumCastOutOfRange)
