// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * In-process double of the libairspy SDK. IO_AIRSPY_SOURCE, IO_RTL_STREAM_START_FAILURE and
 * IO_USB_PROGRESS_MONITOR link the real native adapter against it in place of the library, so no
 * device or USB access is needed.
 *
 * The entry points (airspy_sdk_double.cpp) act on and record into the state declared below. The
 * two tests read different parts of it: the adapter test checks what the SDK was asked to do (the
 * serial opened, the sample rate, the gains, the receive callback, start and stop counts), the
 * stream test only whether the device is open and streaming and how often it was closed. The state
 * is defined once, in the double, rather than in each test.
 *
 * deliver() stands in for libairspy's consumer thread: it runs the receive callback while the
 * stream is up, and airspy_stop_rx() waits for a delivery in flight, as the SDK joins its consumer.
 */

#ifndef DSD_NEO_TESTS_IO_AIRSPY_SDK_DOUBLE_H_
#define DSD_NEO_TESTS_IO_AIRSPY_SDK_DOUBLE_H_

#include <airspy.h>
#include <atomic>
#include <cstdlib>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            DSD_FPRINTF(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                                        \
            std::abort();                                                                                              \
        }                                                                                                              \
    } while (0)

struct airspy_device {
    int unused;
};

namespace airspy_double {

/* The one device the double enumerates and opens. */
extern airspy_device device;
/* The receive callback and context the last airspy_start_rx() installed. */
extern airspy_sample_block_cb_fn rx;
extern void* rx_context;
/* Cleared by a test to stand for a stream the SDK lost on its own; any thread may read it. */
extern std::atomic<bool> streaming;
extern bool device_open;
extern int starts;
extern int stops;
extern int closes;
extern int opens;
/* Injected failure: 1 the open, 2 the receive start (after it submitted transfers), 3 every frequency
   change, 4 a frequency change to a new value. */
extern int failure;
extern uint32_t frequency;
extern uint32_t rate;
extern uint64_t selected;
extern int sensitivity;
extern int linearity;
extern int lna;
extern int mixer;
extern int vga;
extern int lna_agc;
extern int mixer_agc;
extern int bias;
/* The sample rates airspy_get_samplerates() reports, highest first. */
extern uint32_t rates[2];
/* How long airspy_start_rx() takes before the stream is up (a slow restart). */
extern int start_delay_ms;
/* How long airspy_set_rf_bias() takes (a slow control change, which holds the adapter's lock). */
extern int control_delay_ms;

/* One transfer of @p pairs complex samples into the receive callback, if the stream is up; returns
   whether it was delivered. */
bool deliver(const float* samples, int pairs);

} // namespace airspy_double

#endif /* DSD_NEO_TESTS_IO_AIRSPY_SDK_DOUBLE_H_ */
