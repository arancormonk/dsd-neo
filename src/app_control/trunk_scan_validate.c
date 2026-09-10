// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/engine/trunk_scan.h>
#include <limits.h>

int
dsd_app_trunk_scan_validate_targets_csv(const char* path, int* target_count, char* err, size_t err_sz) {
    dsd_trunk_scan_target_list list = {0};
    if (target_count) {
        *target_count = 0;
    }
    if (err && err_sz) {
        err[0] = '\0';
    }
    int rc = dsd_trunk_scan_load_targets_csv(path, NULL, &list, err, err_sz);
    if (rc == 0 && list.count > INT_MAX) {
        if (err && err_sz) {
            DSD_SNPRINTF(err, err_sz, "Too many trunk scan targets");
        }
        rc = -1;
    }
    if (rc == 0 && target_count) {
        *target_count = (int)list.count;
    }
    /* Reset owns the key erasure as well as the allocation. A validation-only
     * caller must not leave imported secrets alive until process exit. */
    dsd_trunk_scan_target_list_reset(&list);
    return rc;
}
