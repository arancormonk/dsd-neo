// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <stdio.h>
#include <string.h>
#include "../test_support/test_support.h"

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
    FILE* fp = fopen(path, "w");
    assert(fp);
    fputs("id,type,frequency_hz,chan_csv,dwell_ms,activity_hold_ms,notes\n"
          "conv,p25-conventional,851500000,,1500,1200,simplex\n",
          fp);
    fclose(fp);
    assert(dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) == 0);
    assert(count == 1 && err[0] == '\0');
    assert(dsd_app_trunk_scan_validate_targets_csv(path, NULL, NULL, 0) == 0);
    fp = fopen(path, "w");
    assert(fp);
    fputs("invalid header\n", fp);
    fclose(fp);
    assert(dsd_app_trunk_scan_validate_targets_csv(path, &count, err, sizeof err) != 0);
    assert(count == 0 && err[0] != '\0');
    remove(path);
    puts("APP_CONTROL_TRUNK_SCAN_VALIDATE ok");
    return 0;
}
