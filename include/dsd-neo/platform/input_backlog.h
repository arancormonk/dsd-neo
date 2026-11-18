// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Where the audio input stood before a rigctl retune was asked for (issue #834), so its drop takes only the
 * input queued before the retune, never what arrived after it.
 *
 * The queue the decoder reads attaches itself while it exists (dsd_input_backlog_attach()): a function that returns
 * how much input the queue has taken in so far, in its own unit, and is safe to call from any thread while attached.
 * The PCM stream reader (stdin, TCP) counts bytes and the UDP ring samples (once the datagrams its socket holds are
 * in it). A mark (dsd_input_backlog_mark()) records
 * that count and the time; the queue's drop discards input up to the count (dsd_pcm_stream_discard_through(),
 * udp_input_discard_through()) and only on the attachment it was measured on (dsd_input_backlog_cut_applies()). A
 * device input (PulseAudio, PortAudio, AAudio) attaches nothing: its drop keeps the audio captured since the mark's
 * time (dsd_input_cut::marked_ns, dsd_audio_discard_input()).
 *
 * Every mark is taken immediately before the retune is asked for (before the rigctl leg sends the frequency, after any
 * passband request, which the peer answers on the old frequency), and used only once the peer confirmed it: a peer may
 * move and send the new channel's audio before its reply, and that audio must not count as queued before the retune. The retune the P25 watchdog confirms off the decoder thread is held here, with the mark
 * it took, until the decoder takes it (dsd_input_backlog_post(), dsd_input_backlog_take()). A connection being opened
 * (dsd_input_backlog_opening(): a TCP reconnect's, once it is made) is the measured input until its queue attaches,
 * which goes on from its count, so a mark taken in between is measured on it, and stands on the queue after the
 * attach. The opening takes a socket, so it is declared in <dsd-neo/platform/input_backlog_opening.h>: this header,
 * which <dsd-neo/core/state.h> includes, brings in no system socket or Windows header. One taken while no input is attached (the reconnect's backoff) measured nothing: posted before the next
 * input attaches, it is retired, and that input's audio is kept. An input replacement
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

/** A mark: the attached queue's count and the time, before a retune was asked for. */
typedef struct dsd_input_cut {
    uint64_t seq;       /**< the attachment the count was read on (dsd_input_backlog_cut_applies()) */
    uint64_t arrived;   /**< the queue's count at the mark, when measured */
    uint64_t marked_ns; /**< monotonic time of the mark */
    /** Where the mark stands among every mark taken, whichever thread took it: a later mark has a greater one (never 0).
     * Of two marks of one input the later discards at least as much (a count and a time never go back), so a drop
     * keeps the later one (dsd_frame_sync_request_input_drop()). */
    uint64_t order;
    uint8_t measured; /**< whether a queue was attached to count */
} dsd_input_cut;

/** What a queue's drop of a cut returns when the decoder had already read past the cut's mark (the retune's
 * confirmation was taken late, after the decoder read some of what arrived after the mark): nothing is dropped, and
 * what the decoder holds of the input was read after the mark (issue #834). */
#define DSD_INPUT_DISCARD_READ_PAST (-1L)

/** @p queue is the input the decoder reads from now on, its count read by @p arrived. While an input is opening
 * (dsd_input_backlog_opening()), @p queue is that input's and its count goes on from the connection's: a post measured
 * on the connection stands on it. A post made while no input was attached is retired. */
void dsd_input_backlog_attach(const void* queue, dsd_input_arrived_fn arrived);

/** @p queue attached (dsd_input_backlog_attach()), and then its input failed to start (a UDP input whose receive
 * thread did not start): detached, and while nothing attached, detached or opened since it attached, the queue its attach
 * replaced is the input again on the attachment it had, so an input switch that keeps the running input keeps its
 * registration too, and a mark measured on that queue before still counts it. An unmeasured post the attach retired
 * (a device input's, which attaches no queue) stands again unless a later one does, so the device that runs on still
 * drops its old audio. Call it before @p queue is freed. */
void dsd_input_backlog_attach_failed(const void* queue);

/** @p queue goes away: detached before it is freed, if it is the one attached. */
void dsd_input_backlog_detach(const void* queue);

/** Mark the attached queue's count and the time into @p out. Any thread. */
void dsd_input_backlog_mark(dsd_input_cut* out);

/** Whether @p cut counted @p queue, attached since without a break. */
int dsd_input_backlog_cut_applies(const dsd_input_cut* cut, const void* queue);

/** A retune confirmed off the decoder thread, held for the decoder: @p cut is its mark, taken before the tune was asked
 * for (dsd_input_backlog_mark()), so audio that came after the request, even before its confirmation, is not in it. Of
 * two posts the later mark is held (dsd_input_cut::order). A mark measured on a connection that was opening stands on
 * the queue that attached to it since (dsd_input_backlog_attach()). */
void dsd_input_backlog_post(const dsd_input_cut* cut);

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
