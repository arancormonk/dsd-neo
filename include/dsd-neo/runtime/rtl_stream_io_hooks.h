// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for optional RTL stream I/O.
 *
 * Some protocol code wants to read RTL stream samples and query soft squelch
 * power without directly depending on IO backends. The engine installs real
 * hook functions at startup. Reads fail when no backend is installed; power
 * queries return a neutral value.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_IO_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_IO_HOOKS_H_

#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the dynamic squelch shows (issue #518 follow-up; rtl_stream_get_squelch_status()). */
typedef struct {
    int active;          /**< 1 when the stream's last block ran a dynamic squelch */
    int noise;           /**< 1 when that was the noise squelch, 0 when the floor tracker ran */
    double quieting_db;  /**< the noise squelch's last window's quieting (0 unless noise) */
    int state;           /**< dsd_squelch_floor_state */
    int gate_open;       /**< the gate at the end of that block */
    int plan_valid;      /**< 0 when the channel plan could not be designed */
    double floor_power;  /**< the floor, mean |z|^2 of the channel samples (0 while learning) */
    double window_power; /**< the last 40 ms window's mean |z|^2 */
} dsd_rtl_squelch_status;

typedef struct {
    int (*read)(void* rtl_ctx, float* out, size_t count, int* out_got);
    double (*return_pwr)(const void* rtl_ctx);
    /* read with each sample's auto squelch flag (DSD_SQUELCH_FLAG_CLOSED or 0). Optional: without it,
       dsd_rtl_stream_io_hook_read_ex() reads through read and reports every sample open. */
    int (*read_ex)(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got);
    /* The dynamic squelch's status. Optional. */
    int (*squelch_status)(const void* rtl_ctx, dsd_rtl_squelch_status* out);
} dsd_rtl_stream_io_hooks;

void dsd_rtl_stream_io_hooks_set(dsd_rtl_stream_io_hooks hooks);

int dsd_rtl_stream_io_hook_read(dsd_state* state, float* out, size_t count, int* out_got);

/**
 * @brief dsd_rtl_stream_io_hook_read() with one flag byte per sample read into @p flags (issue #518 follow-up): the
 * auto squelch's per-sample gate (DSD_SQUELCH_FLAG_CLOSED when closed). Through read_ex when installed, otherwise
 * through read with every flag 0 (open). @p flags may be NULL.
 */
int dsd_rtl_stream_io_hook_read_ex(dsd_state* state, float* out, uint8_t* flags, size_t count, int* out_got);

/** @brief The auto squelch's status through the hook: 0, or -1 (zeroed) with no stream or no hook. */
int dsd_rtl_stream_io_hook_squelch_status(const dsd_state* state, dsd_rtl_squelch_status* out);
double dsd_rtl_stream_io_hook_return_pwr(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_IO_HOOKS_H_ */
