// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Where the audio input stood when a rigctl retune was confirmed (issue #834), so its drop takes only the input
 * queued before the retune, never what arrived after it.
 *
 * The queue the decoder reads attaches itself while it exists (dsd_input_backlog_attach()): a function that returns
 * how much input the queue has taken in so far, in its own unit, and is safe to call from any thread while attached.
 * The PCM stream reader (stdin, TCP) counts bytes and the UDP ring samples. A mark (dsd_input_backlog_mark()) records
 * that count and the time; the queue's drop discards input up to the count (dsd_pcm_stream_discard_through(),
 * udp_input_discard_through()) and only on the attachment it was measured on (dsd_input_backlog_cut_applies()). A
 * device input (PulseAudio, PortAudio, AAudio) attaches nothing: its drop keeps the audio captured since the mark
 * (dsd_input_cut_age_ns(), dsd_audio_discard_input()).
 *
 * The retune the P25 watchdog confirms off the decoder thread is held here until the decoder takes it
 * (dsd_input_backlog_post(), dsd_input_backlog_take()), measured when it is posted: the cutoff is what had arrived on
 * the input then, nothing later. A connection being opened (dsd_input_backlog_opening(): a TCP reconnect's, once it is
 * made) is the measured input until its queue attaches, which goes on from its count, so a post made in between is
 * measured on it at the post. One posted while no input is attached (the reconnect's backoff) moved the peer before the
 * next input existed, so nothing is queued before it there: it is retired. An input replacement
 * (dsd_input_backlog_forget()) retires a post that was not measured on the queue attached then, and a run boundary
 * (dsd_input_backlog_retire()) any post.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_INPUT_BACKLOG_H_
#define DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_INPUT_BACKLOG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** How much input @p queue has taken in so far, in the queue's unit. Called with the backlog's lock held. */
typedef uint64_t (*dsd_input_arrived_fn)(const void* queue);

/** A mark: the attached queue's count and the time, when a retune was confirmed. */
typedef struct dsd_input_cut {
    uint64_t seq;       /**< the attachment the count was read on (dsd_input_backlog_cut_applies()) */
    uint64_t arrived;   /**< the queue's count at the mark, when measured */
    uint64_t marked_ns; /**< monotonic time of the mark */
    uint8_t measured;   /**< whether a queue was attached to count */
} dsd_input_cut;

/** @p queue is the input the decoder reads from now on, its count read by @p arrived. While an input is opening
 * (dsd_input_backlog_opening()), @p queue is that input's and its count goes on from the connection's: a post measured
 * on the connection stands on it. A post made while no input was attached is retired. */
void dsd_input_backlog_attach(const void* queue, dsd_input_arrived_fn arrived);

/** A replacement input begins: its connection @p conn exists, and is the measured input until its queue attaches next,
 * counted by @p arrived in the queue's unit as the queue will count it (nothing reads the connection before then), so a
 * post made meanwhile is measured when it is made. A post made before now while no input was attached is retired.
 * Decoder thread. The next attach ends the opening; dsd_input_backlog_forget() or dsd_input_backlog_retire() end it with
 * the connection detached and a post measured on it retired. */
void dsd_input_backlog_opening(const void* conn, dsd_input_arrived_fn arrived);

/** @p queue goes away: detached before it is freed, if it is the one attached. */
void dsd_input_backlog_detach(const void* queue);

/** Mark the attached queue's count and the time into @p out. Any thread. */
void dsd_input_backlog_mark(dsd_input_cut* out);

/** Whether @p cut counted @p queue, attached since without a break. */
int dsd_input_backlog_cut_applies(const dsd_input_cut* cut, const void* queue);

/** Nanoseconds since @p cut was marked (0 for NULL or a mark in the future). */
uint64_t dsd_input_cut_age_ns(const dsd_input_cut* cut);

/** A retune confirmed off the decoder thread: marked and held for the decoder (the latest post wins). */
void dsd_input_backlog_post(void);

/** The decoder takes the held post into @p out: 1 when one was held and not retired, else 0. */
int dsd_input_backlog_take(dsd_input_cut* out);

/** The input was replaced: a held post not measured on the queue attached now is retired. Decoder thread. */
void dsd_input_backlog_forget(void);

/** A run boundary: a held post is retired. */
void dsd_input_backlog_retire(void);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PLATFORM_INPUT_BACKLOG_H_ */
