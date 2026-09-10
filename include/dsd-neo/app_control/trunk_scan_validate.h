// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TRUNK_SCAN_VALIDATE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TRUNK_SCAN_VALIDATE_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Validate with the engine's CSV parser without exposing owned targets or decoder
 * state to frontends. Returns 0 on success; count is zero on failure. Optional
 * output pointers may be NULL. Referenced side files (channel/key CSVs and
 * profiles) are not opened or checked for readability. Parsed key storage is erased before returning. */
int dsd_app_trunk_scan_validate_targets_csv(const char* path, int* target_count, char* err, size_t err_sz);

#ifdef __cplusplus
}
#endif
#endif
