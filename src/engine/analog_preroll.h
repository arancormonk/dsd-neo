// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Engine-private: the NFM call's pre-roll ring (issue #668), the newest allowed audio heard before a reception, kept at
 * 48 kHz as the per-call recording's CALL_WAV lane converted it. The ring holds samples only; when they are kept,
 * released or dropped is the call's to decide (engine/analog_call.c). Decoder thread only.
 */

#ifndef DSD_NEO_SRC_ENGINE_ANALOG_PREROLL_H
#define DSD_NEO_SRC_ENGINE_ANALOG_PREROLL_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/analog_sink.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    short* buf;
    size_t cap;  /* samples the ring holds: the setting at 48 kHz; 0 while it has none */
    size_t head; /* where the next sample goes */
    size_t len;  /* samples held, at most cap */
    /* The setting the ring was last sized for (0: none). A size that could not be allocated is kept too, so it is not
       tried again, nor logged again, until the pre-roll the options ask for changes: the setting, per-call recording,
       or a mode in which no NFM call can be (leaving the FM monitor and coming back tries again). */
    int ms;
    /* The CALL_WAV lane's continuity generation the held samples were converted under: they are continuous with the
       lane's next write while it still reads the same. */
    int has_generation;
    uint32_t generation;
} analog_preroll;

/* Size the ring for @p ms (0: free it). Whatever it held is gone. Without memory there is no ring (one log line). */
void analog_preroll_resize(analog_preroll* p, int ms);

/* A ring to fill: one was allocated. */
int analog_preroll_active(const analog_preroll* p);

/* Drop what the ring holds and the generation it was converted under. */
void analog_preroll_clear(analog_preroll* p);

/* The CALL_WAV lane broke since the ring last saw it (or its generation cannot be read): what the ring holds does not
   run on into the lane's next write. */
int analog_preroll_lane_moved(dsd_state* state, const analog_preroll* p);

/*
 * Convert @p count samples at @p sample_rate through the CALL_WAV lane into the ring, the oldest falling out once it is
 * full. A lane that started over inside this write (a restart no break announced, such as its first conversion) keeps
 * only this block, which is all that is continuous with what follows. Returns the 48 kHz samples the lane made, those
 * that fell out included, and sets @p moved when the lane started over.
 */
uint64_t analog_preroll_append(const dsd_opts* opts, dsd_state* state, analog_preroll* p, const float* samples,
                               size_t count, int sample_rate, int* moved);

/* Hand the newest @p count samples (at most what the ring holds), oldest first, to @p write. */
void analog_preroll_take(const analog_preroll* p, size_t count, dsd_analog_sink_write_fn write, const void* ctx);

/* The ring's audio is in the file, or no longer needed: hold nothing, and note the lane's generation now, which the
   lane's next write runs on from. */
void analog_preroll_consume(dsd_state* state, analog_preroll* p);

#ifdef DSD_NEO_TEST_HOOKS
/* Make the next ring allocations fail (nonzero) or succeed again (0). */
void analog_preroll_test_fail_alloc(int fail);
#endif

#endif
