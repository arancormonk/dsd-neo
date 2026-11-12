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
#define DSD_RTL_READ_YIELD           (-2)
/* The read was empty for one wait slice. Only a reader that asks for it gets it
   (dsd_rtl_stream_io_hook_read_ex2()): the EDACS analog call loop, which plays the time that passed as closed squelch
   and ends its call by its own timers. */
#define DSD_RTL_READ_NO_STREAM       (-3)
/* The live stream delivered nothing for 100 ms (issue #834). Only a reader that asks for it gets it
   (DSD_RTL_READ_WANT_IDLE_BOUND): the decoder's frame-sync hunt, which then applies a queued command, ends its pass
   under a scan, or reads again. Frame reads, an I/Q replay's reads and a Signal search's or analog monitor's own bounds
   are not changed by it. */
#define DSD_RTL_READ_IDLE            (-4)

/* What a reader asks of a live read (the @p wants bits of dsd_rtl_stream_io_hook_read_ex2() and the reads under it). */
#define DSD_RTL_READ_WANT_NO_STREAM  0x1 /* DSD_RTL_READ_NO_STREAM while the device has no stream */
#define DSD_RTL_READ_WANT_IDLE_BOUND 0x2 /* DSD_RTL_READ_IDLE after 100 ms with no samples */

typedef struct {
    int (*read)(void* rtl_ctx, float* out, size_t count, int* out_got);
    double (*return_pwr)(const void* rtl_ctx);
    /* read with each sample's auto squelch flag (DSD_SQUELCH_FLAG_CLOSED or 0). Optional: without it,
       dsd_rtl_stream_io_hook_read_ex() reads through read and reports every sample open. */
    int (*read_ex)(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got);
    /* The dynamic squelch's status. Optional. */
    int (*squelch_status)(const void* rtl_ctx, dsd_rtl_squelch_status* out);
    /* read_ex for a reader that asks for more (@p wants, DSD_RTL_READ_WANT_* bits): DSD_RTL_READ_NO_STREAM,
       DSD_RTL_READ_IDLE. Optional: without it, dsd_rtl_stream_io_hook_read_ex2() reads through read_ex. */
    int (*read_ex2)(void* rtl_ctx, float* out, uint8_t* flags, size_t count, int* out_got, int wants);
    /* Start of the last NO_STREAM wait slice on this reader thread, in real monotonic milliseconds; 0 if unknown. */
    uint64_t (*no_stream_since_ms)(void);
    /* read_ex2 with the FSK voice lane into @p voice (or NULL), sample for sample (rtl_stream_read_lanes()). Optional:
       without it, dsd_rtl_stream_io_hook_read_lanes() reads through read_ex2 with every voice sample 0. */
    int (*read_lanes)(void* rtl_ctx, float* out, float* voice, uint8_t* flags, size_t count, int* out_got, int wants);
    /* Ask for the FSK voice lane, or stop it (rtl_stream_set_voice_lane()). Optional. */
    void (*set_voice_lane)(int enable);
    /* Whether the stream can carry the FSK voice lane (rtl_stream_voice_lane_available()). Optional: without it, a
       stream with read_lanes carries it. */
    int (*voice_lane_available)(void);
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
 * @brief dsd_rtl_stream_io_hook_read_ex() for a reader that asks for more of a live read (@p wants,
 * DSD_RTL_READ_WANT_* bits). DSD_RTL_READ_WANT_NO_STREAM: while the device has no stream, each read that found nothing
 * for one wait slice returns DSD_RTL_READ_NO_STREAM instead of waiting on, and never DSD_RTL_READ_YIELD.
 * DSD_RTL_READ_WANT_IDLE_BOUND: a live read that found nothing for 100 ms returns DSD_RTL_READ_IDLE (issue #834). With
 * @p wants 0 it reads as dsd_rtl_stream_io_hook_read_ex().
 */
int dsd_rtl_stream_io_hook_read_ex2(dsd_state* state, float* out, uint8_t* flags, size_t count, int* out_got,
                                    int wants);

/**
 * @brief dsd_rtl_stream_io_hook_read_ex2() with the FSK voice lane (issue #518): into @p voice (as many as @p out, or
 * NULL) each sample's voice-lane sample, FM monitor audio at the monitor's scale, 0 where the lane did not run for it
 * (dsd_rtl_stream_io_hook_set_voice_lane()). @p out and @p flags read as dsd_rtl_stream_io_hook_read_ex2() reads them.
 * Without the read_lanes hook every voice sample is 0.
 */
int dsd_rtl_stream_io_hook_read_lanes(dsd_state* state, float* out, float* voice, uint8_t* flags, size_t count,
                                      int* out_got, int wants);

/** @brief Ask the RTL stream for its FSK voice lane, or stop it (issue #518); nothing without the hook. */
void dsd_rtl_stream_io_hook_set_voice_lane(int enable);

/**
 * @brief Whether the RTL stream carries the FSK voice lane: 1 with the read_lanes hook, unless the voice_lane_available
 * hook says its stream cannot (its lane buffer was not allocated); 0 without read_lanes, where every voice sample
 * reads 0.
 */
int dsd_rtl_stream_io_hook_voice_lane_available(void);

/** @brief The auto squelch's status through the hook: 0, or -1 (zeroed) with no stream or no hook. */
int dsd_rtl_stream_io_hook_squelch_status(const dsd_state* state, dsd_rtl_squelch_status* out);
double dsd_rtl_stream_io_hook_return_pwr(const dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_RTL_STREAM_IO_HOOKS_H_ */
