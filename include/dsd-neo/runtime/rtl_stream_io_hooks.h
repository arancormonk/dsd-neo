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

typedef struct {
    int (*read)(void* rtl_ctx, float* out, size_t count, int* out_got);
    double (*return_pwr)(const void* rtl_ctx);
    /* read with each sample's auto squelch flag (DSD_SQUELCH_FLAG_CLOSED or 0). Optional: without it,
       dsd_rtl_stream_io_hook_read_ex() reads through read and reports every sample open. */
    int (*read_ex)(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got);
} dsd_rtl_stream_io_hooks;

void dsd_rtl_stream_io_hooks_set(dsd_rtl_stream_io_hooks hooks);

int dsd_rtl_stream_io_hook_read(dsd_state* state, float* out, size_t count, int* out_got);

/**
 * @brief dsd_rtl_stream_io_hook_read() with one flag byte per sample read into @p flags (issue #518 follow-up): the
 * auto squelch's per-sample gate (DSD_SQUELCH_FLAG_CLOSED when closed). Through read_ex when installed, otherwise
 * through read with every flag 0 (open). @p flags may be NULL.
 */
int dsd_rtl_stream_io_hook_read_ex(dsd_state* state, float* out, uint8_t* flags, size_t count, int* out_got);
double dsd_rtl_stream_io_hook_return_pwr(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_IO_HOOKS_H_ */
