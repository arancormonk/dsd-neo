// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Engine-owned single-tuner trunk scan coordinator.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    DSD_TRUNK_SCAN_MAX_TARGETS = 32,
    DSD_TRUNK_SCAN_DWELL_MIN_MS = 250,
    DSD_TRUNK_SCAN_DWELL_MAX_MS = 600000,
    DSD_TRUNK_SCAN_IDLE_DWELL_DEFAULT_MS = 3000,
    DSD_TRUNK_SCAN_ACTIVITY_HOLD_DEFAULT_MS = 1200,
};

typedef enum {
    DSD_TRUNK_SCAN_TARGET_P25_TRUNK = 0,
    DSD_TRUNK_SCAN_TARGET_DMR_TRUNK = 1,
    DSD_TRUNK_SCAN_TARGET_DMR_CONVENTIONAL = 2,
} dsd_trunk_scan_target_type;

typedef enum {
    DSD_TRUNK_SCAN_MODULATION_UNSET = 0,
    DSD_TRUNK_SCAN_MODULATION_AUTO = 1,
    DSD_TRUNK_SCAN_MODULATION_C4FM = 2,
    DSD_TRUNK_SCAN_MODULATION_CQPSK = 3,
    DSD_TRUNK_SCAN_MODULATION_GFSK = 4,
} dsd_trunk_scan_modulation;

typedef struct {
    char id[64];
    dsd_trunk_scan_target_type type;
    uint32_t frequency_hz;
    char chan_csv[1024];
    int dwell_ms;
    int activity_hold_ms;
    dsd_trunk_scan_modulation modulation;
    int rtl_gain_is_set;
    int rtl_gain_db;
} dsd_trunk_scan_target;

typedef struct {
    dsd_trunk_scan_target targets[DSD_TRUNK_SCAN_MAX_TARGETS];
    size_t count;
} dsd_trunk_scan_target_list;

int dsd_trunk_scan_load_targets_csv(const char* path, const dsd_opts* opts, dsd_trunk_scan_target_list* out, char* err,
                                    size_t err_sz);

int dsd_engine_trunk_scan_init(dsd_opts* opts, dsd_state* state, char* err, size_t err_sz);
void dsd_engine_trunk_scan_shutdown(dsd_opts* opts, dsd_state* state);
void dsd_engine_trunk_scan_tick(dsd_opts* opts, dsd_state* state);
void* dsd_engine_trunk_scan_active_p25_ctx(void);
void* dsd_engine_trunk_scan_active_dmr_ctx(void);
void dsd_engine_trunk_scan_dmr_conventional_activity(const dsd_opts* opts, const dsd_state* state, uint32_t target,
                                                     uint32_t source, int is_private, int encrypted, int data_call);
size_t dsd_engine_trunk_scan_active_index(const dsd_state* state);
size_t dsd_engine_trunk_scan_target_count(const dsd_state* state);
int dsd_engine_trunk_scan_saved_tuner_autogain(const dsd_state* state, int* out_on);
int dsd_engine_trunk_scan_active_p25_cqpsk_request(const dsd_state* state, int* out_enable);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_TRUNK_SCAN_H_ */
