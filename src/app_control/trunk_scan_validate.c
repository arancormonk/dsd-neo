// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/trunk_scan_validate.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/trunk_scan_lists.h>
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

/* Load the list at @p path into @p list; a parser error is prefixed with the list's @p name. */
static int
validate_load_list(const char* path, const char* name, dsd_trunk_scan_target_list* list, char* err, size_t err_sz) {
    char detail[512];
    detail[0] = '\0';
    if (dsd_trunk_scan_load_targets_csv(path, NULL, list, detail, sizeof detail) == 0) {
        return 0;
    }
    if (err && err_sz) {
        DSD_SNPRINTF(err, err_sz, "%s: %s", name, detail);
    }
    return -1;
}

static void
validate_set_error(char* err, size_t err_sz, const char* text) {
    if (err && err_sz) {
        DSD_SNPRINTF(err, err_sz, "%s", text);
    }
}

/* Load @p count lists, then merge them by the engine's rule across lists, into @p total distinct targets; every list
   loaded is reset (its keys erased). */
static int
validate_lists_together(const char* const* paths, const char* const* labels, size_t count, size_t* total, char* err,
                        size_t err_sz) {
    dsd_trunk_scan_target_list lists[DSD_TRUNK_SCAN_MAX_LISTS] = {{0}};
    int rc = 0;
    size_t loaded = 0U;
    for (; loaded < count && rc == 0; loaded++) {
        rc = validate_load_list(paths[loaded], labels[loaded], &lists[loaded], err, err_sz);
    }
    *total = 0U;
    if (rc == 0) {
        rc = dsd_trunk_scan_check_target_lists(lists, labels, count, NULL, 0U, total, err, err_sz);
    }
    for (size_t i = 0; i < loaded; i++) {
        /* Reset owns the key erasure as well as the allocation. */
        dsd_trunk_scan_target_list_reset(&lists[i]);
    }
    return rc;
}

int
dsd_app_trunk_scan_validate_target_lists(const char* const* paths, const char* const* names, size_t count,
                                         int* target_count, char* err, size_t err_sz) {
    if (target_count) {
        *target_count = 0;
    }
    validate_set_error(err, err_sz, "");
    if (!paths || count == 0U || count > DSD_TRUNK_SCAN_MAX_LISTS) {
        char msg[64];
        DSD_SNPRINTF(msg, sizeof msg, "A trunk scan takes 1 to %d target lists", DSD_TRUNK_SCAN_MAX_LISTS);
        validate_set_error(err, err_sz, msg);
        return -1;
    }
    size_t total = 0U;
    int rc = validate_lists_together(paths, names ? names : paths, count, &total, err, err_sz);
    if (rc == 0 && total > INT_MAX) {
        validate_set_error(err, err_sz, "Too many trunk scan targets");
        rc = -1;
    }
    if (rc == 0 && target_count) {
        *target_count = (int)total;
    }
    return rc;
}
