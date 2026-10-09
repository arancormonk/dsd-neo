// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The audible-audio stamp: when the decoder last emitted audio the app plays (issue #574).
 *
 * Each writer of decoded or analog audio notes the stamp at its own final emit decision, after every gate it applies
 * (crypto and keys, forced clear, reverse mute, unmute overrides, talkgroup policy, slot switches, DMR mono, the mute)
 * and before it picks the output type (local stream, UDP, raw fd). It notes provenance, never amplitude: a block counts
 * when it carries decoded media, or squelch-open analog reception, from a slot whose gates pass, so valid all-zero
 * decoded audio counts and silence padding or a muted slot never does.
 *
 * The stamp is off until a reader arms it, and stays armed until the process exits. Every writer asks
 * dsd_audio_activity_armed() first, so an unarmed session does no added work per block: no scan, no clock read, no
 * store.
 *
 * One atomic 64-bit word holds the stamp, in real-time monotonic milliseconds (dsd_realtime_mono_ms()), 0 for none.
 * The value itself is the identity a reader compares, so there is no separate sequence that could tear against it.
 * Writers run on the decoder thread; arm, read and reset are safe from any thread.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_ACTIVITY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_ACTIVITY_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A stamp older than this many milliseconds reads with age -1 (dsd_audio_activity_read()). */
#define DSD_AUDIO_ACTIVITY_MAX_AGE_MS 60000

/** @brief Turn the stamp on for the rest of the process. Idempotent; any thread. */
void dsd_audio_activity_arm(void);

/** @brief 1 once dsd_audio_activity_arm() has run, else 0. One relaxed atomic load; writers ask it first. */
int dsd_audio_activity_armed(void);

/**
 * @brief Note audible audio now. While armed, stores max(1, dsd_realtime_mono_ms()) with release ordering; unarmed, it
 * does nothing at all, not even read the clock.
 */
void dsd_audio_activity_note(void);

/**
 * @brief Read the stamp: one acquire load of its word.
 *
 * @param stamp  The stamp (0: none since the process started or since the last reset). May be NULL.
 * @param age_ms Milliseconds since that same stamp, clamped at 0; -1 when the stamp is 0 or older than
 *               DSD_AUDIO_ACTIVITY_MAX_AGE_MS. May be NULL.
 */
void dsd_audio_activity_read(uint64_t* stamp, int32_t* age_ms);

/** @brief Clear the stamp to 0 (none). Leaves it armed. Any thread. */
void dsd_audio_activity_reset(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_ACTIVITY_H_ */
