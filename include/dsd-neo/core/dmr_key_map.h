// SPDX-License-Identifier: GPL-3.0-or-later
/** @file @brief Owned, nonsecret DMR group-to-key references and scan scope. */
#ifndef DSD_NEO_CORE_DMR_KEY_MAP_H
#define DSD_NEO_CORE_DMR_KEY_MAP_H

#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { DSD_DMR_TG_KEY_MAP_MAX = 256 };

typedef struct {
    uint32_t tg[DSD_DMR_TG_KEY_MAP_MAX];
    uint8_t kid[DSD_DMR_TG_KEY_MAP_MAX];
    int count;
} dsd_dmr_key_map;

/** Parse without changing decoder state. Failure leaves out untouched. */
int dsd_dmr_key_map_load(const char* path, dsd_dmr_key_map* out);
int dsd_dmr_key_map_validate(const dsd_dmr_key_map* map);
void dsd_dmr_key_map_capture(const dsd_state* state, dsd_dmr_key_map* out);
/** Validate before installation; reset per-call selection-notice latches. */
int dsd_dmr_key_map_install(dsd_state* state, const dsd_dmr_key_map* map);
/** Update global references beneath an active explicit scan mapping. */
int dsd_scan_maps_suspend(dsd_state* state);
void dsd_scan_maps_resume(dsd_state* state);
/** Restore the global mapping and release the active scan scope. */
void dsd_scan_maps_leave(dsd_state* state);

#ifdef __cplusplus
}
#endif
#endif
