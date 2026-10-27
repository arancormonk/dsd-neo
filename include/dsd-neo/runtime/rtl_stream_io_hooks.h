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
    int quieting_valid;  /**< 1 once the noise squelch has measured a window since it started */
    double quieting_db;  /**< the noise squelch's last window's quieting (0 until quieting_valid) */
    int state;           /**< dsd_squelch_floor_state */
    int gate_open;       /**< the gate at the end of that block */
    int plan_valid;      /**< 0 when the channel plan could not be designed */
    double floor_power;  /**< the floor, mean |z|^2 of the channel samples (0 while learning) */
    double window_power; /**< the last 40 ms window's mean |z|^2 */
} dsd_rtl_squelch_status;

/* What a live RTL read returns besides samples and -1 (a failure: the stream ended, or a Signal search visit's bound),
   while the device has no stream at all (issue #518 D4: a USB stall recovery runs; rtl_tcp is not connected). A
   stream that is only delayed is still a stream: its reads wait for samples. */
/* The read was empty and gave way to a queued command: the consumer stands erasures in and lets the engine loop apply
   it (the DSD_INTERRUPT_YIELD interruption), then reads on. */
#define DSD_RTL_READ_YIELD     (-2)
/* The read was empty for one wait slice. Only a reader that asks for it gets it
   (dsd_rtl_stream_io_hook_read_ex2()): the EDACS analog call loop, which plays the time that passed as closed squelch
   and ends its call by its own timers. */
#define DSD_RTL_READ_NO_STREAM (-3)

typedef struct {
    int (*read)(void* rtl_ctx, float* out, size_t count, int* out_got);
    double (*return_pwr)(const void* rtl_ctx);
    /* read with each sample's auto squelch flag (DSD_SQUELCH_FLAG_CLOSED or 0). Optional: without it,
       dsd_rtl_stream_io_hook_read_ex() reads through read and reports every sample open. */
    int (*read_ex)(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got);
    /* The dynamic squelch's status. Optional. */
    int (*squelch_status)(const void* rtl_ctx, dsd_rtl_squelch_status* out);
    /* read_ex that returns DSD_RTL_READ_NO_STREAM to a reader that asks for it (@p want_no_stream). Optional: without
       it, dsd_rtl_stream_io_hook_read_ex2() reads through read_ex. */
    int (*read_ex2)(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got, int want_no_stream);
    /* Start of the last NO_STREAM wait slice on this reader thread, in real monotonic milliseconds; 0 if unknown. */
    uint64_t (*no_stream_since_ms)(void);
} dsd_rtl_stream_io_hooks;

void dsd_rtl_stream_io_hooks_set(dsd_rtl_stream_io_hooks hooks);

/** @brief Start of the last NO_STREAM wait slice, or 0 without the optional hook. */
uint64_t dsd_rtl_stream_io_hook_no_stream_since_ms(void);

int dsd_rtl_stream_io_hook_read(dsd_state* state, float* out, size_t count, int* out_got);

/**
 * @brief dsd_rtl_stream_io_hook_read() with one flag byte per sample read into @p flags (issue #518 follow-up): the
 * auto squelch's per-sample gate (DSD_SQUELCH_FLAG_CLOSED when closed). Through read_ex when installed, otherwise
 * through read with every flag 0 (open). @p flags may be NULL. A live NFM monitor may return success with
 * @p out_got set to 0 on an idle input, letting its decoder thread service call deadlines.
 */
int dsd_rtl_stream_io_hook_read_ex(dsd_state* state, float* out, uint8_t* flags, size_t count, int* out_got);

/**
 * @brief dsd_rtl_stream_io_hook_read_ex() for a reader that takes DSD_RTL_READ_NO_STREAM (@p want_no_stream): while the
 * device has no stream, each read that found nothing for one wait slice returns it instead of waiting on, and never
 * DSD_RTL_READ_YIELD. With @p want_no_stream 0 it reads as dsd_rtl_stream_io_hook_read_ex().
 */
int dsd_rtl_stream_io_hook_read_ex2(dsd_state* state, float* out, uint8_t* flags, size_t count, int* out_got,
                                    int want_no_stream);

/** @brief The auto squelch's status through the hook: 0, or -1 (zeroed) with no stream or no hook. */
int dsd_rtl_stream_io_hook_squelch_status(const dsd_state* state, dsd_rtl_squelch_status* out);
double dsd_rtl_stream_io_hook_return_pwr(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_IO_HOOKS_H_ */
