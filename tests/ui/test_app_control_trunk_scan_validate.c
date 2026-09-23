// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <stdio.h>
#include <string.h>
#include "../test_support/test_support.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/platform/file_compat.h"

/* --- Issue #521: per-target squelch --- */

typedef struct {
    size_t count;
    int squelch_set[4];
    int squelch_db[4];
} squelch_targets;

static void
collect_target(const dsd_app_scan_csv_target* target, void* context) {
    squelch_targets* seen = (squelch_targets*)context;
    if (seen->count < 4) {
        seen->squelch_set[seen->count] = target->squelch_db_set;
        seen->squelch_db[seen->count] = target->squelch_db;
    }
    seen->count++;
}

static void
write_targets(const char* path, const char* body) {
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    fputs("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes,options\n", fp);
    fputs(body, fp);
    fclose(fp);
}

/* Squelch is legal on every target type; the inspector a frontend previews through accepts the
 * same rows the engine does and reports each target's own value, and malformed, duplicate and
 * out-of-range values are refused with a diagnostic that names the row. */
static void
test_target_squelch(const char* path) {
    write_targets(path, "trunk,p25-trunk,851000000,,3000,,,--squelch-db -60\n"
                        "conv,dmr-conventional,461000000,,1500,1200,,--squelch-db=0\n"
                        "plain,nxdn48-conventional,461556250,,1500,1200,,\n"
                        "cc,nxdn-trunk,461037500,,3000,,,--squelch-db -100 --scan-max-visit-ms 5000\n");
    int count = 0;
    char err[256] = {0};
    assert(dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) == 0);
    assert(count == 4 && err[0] == '\0');
    squelch_targets seen = {0};
    const dsd_app_scan_csv_callbacks callbacks = {collect_target, NULL, &seen};
    assert(dsd_app_scan_csv_inspect(path, NULL, 0, &callbacks, err, sizeof err) == 0);
    assert(seen.count == 4);
    assert(seen.squelch_set[0] && seen.squelch_db[0] == -60);
    assert(seen.squelch_set[1] && seen.squelch_db[1] == 0);
    assert(!seen.squelch_set[2]);
    assert(seen.squelch_set[3] && seen.squelch_db[3] == -100);

    const struct {
        const char* cell;
        const char* reason;
    } bad[] = {{"--squelch-db 5", "row 3: --squelch-db: expects whole dB from -100 to 0 (0 = off)"},
               {"--squelch-db=-101", "row 3: --squelch-db: expects whole dB from -100 to 0 (0 = off)"},
               {"--squelch-db -5.5", "row 3: --squelch-db: expects whole dB from -100 to 0 (0 = off)"},
               {"--squelch-db", "row 3: --squelch-db: requires a valid argument"},
               {"--squelch-db 0 --squelch-db -40", "row 3: --squelch-db: duplicate option"}};

    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char body[256];
        DSD_SNPRINTF(body, sizeof body,
                     "ok,p25-trunk,851000000,,3000,,,--squelch-db -60\nbad,p25-trunk,852000000,,3000,,,%s\n",
                     bad[i].cell);
        write_targets(path, body);
        assert(dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) != 0);
        if (!strstr(err, bad[i].reason)) {
            DSD_FPRINTF(stderr, "want '%s', got '%s'\n", bad[i].reason, err);
            assert(0);
        }
        assert(count == 0);
    }
}

/* Every shipped trunk-scan example parses through the engine's own loader with no diagnostic, so
 * the documented option spellings cannot drift from the parser. */
static void
test_trunk_scan_examples_validate(void) {
    static const char* const examples[] = {"trunk_scan_targets.csv"};
    for (size_t i = 0; i < sizeof examples / sizeof examples[0]; i++) {
        char path[1024];
        DSD_SNPRINTF(path, sizeof path, "%s/%s", DSD_NEO_TEST_EXAMPLES_DIR, examples[i]);
        int count = 0;
        char err[256] = {0};
        if (dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) != 0 || count <= 0) {
            DSD_FPRINTF(stderr, "%s: %s\n", examples[i], err);
            assert(0);
        }
        assert(err[0] == '\0');
    }
}

int
main(void) {
    int count = 99;
    char err[256] = {0};
    assert(dsd_app_trunk_scan_validate_targets_csv(NULL, &count, err, sizeof err) != 0);
    assert(count == 0 && err[0] != '\0');
    char path[DSD_TEST_PATH_MAX];
    const int fd = dsd_test_mkstemp(path, sizeof path, "wp0-validate-");
    assert(fd >= 0);
    dsd_close(fd);
    FILE* fp = dsd_fopen_private(path, "w");
    assert(fp);
    fputs("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
          "conv,p25-conventional,851500000,,1500,1200,simplex\n",
          fp);
    fclose(fp);
    assert(dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) == 0);
    assert(count == 1 && err[0] == '\0');
    assert(dsd_app_trunk_scan_validate_targets_csv(path, NULL, NULL, 0) == 0);
    fp = dsd_fopen_private(path, "w");
    assert(fp);
    fputs("invalid header\n", fp);
    fclose(fp);
    assert(dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) != 0);
    assert(count == 0 && err[0] != '\0');
    test_target_squelch(path);
    test_trunk_scan_examples_validate();
    remove(path);
    puts("APP_CONTROL_TRUNK_SCAN_VALIDATE ok");
    return 0;
}
