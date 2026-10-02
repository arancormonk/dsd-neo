// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * In-process double of the libairspy SDK. IO_AIRSPY_SOURCE and IO_RTL_STREAM_START_FAILURE link
 * the real native adapter against it in place of the library, so no device or USB access is needed.
 *
 * The entry points (airspy_sdk_double.cpp) act on and record into the state declared below. The
 * two tests read different parts of it: the adapter test checks what the SDK was asked to do (the
 * serial opened, the sample rate, the gains, the receive callback, start and stop counts), the
 * stream test only whether the device is open and streaming and how often it was closed. The state
 * is defined once, in the double, rather than in each test.
 */

#ifndef DSD_NEO_TESTS_IO_AIRSPY_SDK_DOUBLE_H_
#define DSD_NEO_TESTS_IO_AIRSPY_SDK_DOUBLE_H_

#include <airspy.h>
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
extern bool streaming;
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

} // namespace airspy_double

#endif /* DSD_NEO_TESTS_IO_AIRSPY_SDK_DOUBLE_H_ */
