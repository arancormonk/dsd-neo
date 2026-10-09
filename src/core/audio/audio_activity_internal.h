// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* Test seam of the audible-audio stamp (<dsd-neo/core/audio_activity.h>). Module-private, and compiled only into
   builds with DSD_NEO_TEST_HOOKS: the shipped core carries no settable clock. */

#ifndef DSD_NEO_SRC_CORE_AUDIO_AUDIO_ACTIVITY_INTERNAL_H_
#define DSD_NEO_SRC_CORE_AUDIO_AUDIO_ACTIVITY_INTERNAL_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DSD_NEO_TEST_HOOKS
typedef uint64_t (*dsd_audio_activity_clock_fn)(void);

/* The clock dsd_audio_activity_note() and dsd_audio_activity_read() take "now" from, in milliseconds; NULL restores
   dsd_realtime_mono_ms(). Set it before any thread notes or reads the stamp. */
void dsd_audio_activity_set_clock_for_test(dsd_audio_activity_clock_fn clock);
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSD_NEO_SRC_CORE_AUDIO_AUDIO_ACTIVITY_INTERNAL_H_ */
