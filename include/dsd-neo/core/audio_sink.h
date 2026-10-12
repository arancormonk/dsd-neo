// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The one write path every audio writer uses for the local device streams and the `-o -` descriptor.
 *
 * Writers apply their own gates (mute, slot switches, talkgroup policy, crypto) and pick the output type; these
 * functions only write, report what went wrong once, and keep a failed output from taking the decoder with it:
 *
 * - **Device streams.** The backend closes a device whose write fails and reopens it behind the same stream handle
 *   (dsd_audio_output_health_get()). dsd_audio_sink_write_device() logs the first failure of each episode, at most
 *   one failure line a minute per stream with a count of the ones it held back, and one line when the reopened device
 *   plays again.
 * - **The `-o -` descriptor.** Writes go out in whole sample frames (dsd_write_frames()): interrupted and short writes
 *   are finished, and a closed reader never raises SIGPIPE. A write that fails logs one error and turns the
 *   descriptor's audio off for the rest of the session; decoding, recording and every other output carry on. A
 *   non-blocking descriptor that is full drops the rest of the block whole, never waiting, with one warning and a
 *   count at most once a minute after it. A blocking descriptor (the usual pipe) blocks as any pipe does.
 *
 * Decoder thread only: the reporting state is module-wide and unlocked.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_SINK_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_SINK_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/platform/audio.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The local device streams, each reported on its own. */
typedef enum {
    DSD_AUDIO_SINK_DIGITAL = 0, /**< opts->audio_out_stream: decoded voice */
    DSD_AUDIO_SINK_RAW = 1,     /**< opts->audio_raw_out: analog monitor, EDACS analog voice, M17 encoder monitor */
    DSD_AUDIO_SINK_DEVICE_COUNT
} dsd_audio_sink_device;

/**
 * @brief Write @p frames frames to a local device stream and report its device failures and recoveries.
 *
 * @return What dsd_audio_write() returned; 0 when @p stream is NULL (no stream open) or there is nothing to write.
 */
int dsd_audio_sink_write_device(dsd_audio_sink_device which, dsd_audio_stream* stream, const int16_t* samples,
                                size_t frames);

/** @brief Forget what was reported for the device streams: closeAudioOutput() closed them. */
void dsd_audio_sink_device_reset(void);

/**
 * @brief Write a block of whole @p frame_bytes frames to the `-o -` descriptor @p fd.
 *
 * @return 0 when the whole block went out; -1 when it did not (the descriptor is gone, failed now or earlier, or was
 *         full and the block was dropped).
 */
int dsd_audio_sink_write_fd(int fd, const void* data, size_t bytes, size_t frame_bytes);

/** @brief 1 when @p fd is a descriptor whose audio has not been turned off by a failed write. */
int dsd_audio_sink_fd_usable(int fd);

/** @brief Start the descriptor's reporting over: a session (re)binds `-o -`. */
void dsd_audio_sink_fd_reset(void);

/**
 * @brief Whether the analog monitor writes the `-o -` descriptor.
 *
 * Only in the analog-only modes (`-fA`, `-fM`, or a switch to Analog or AM), where no digital writer shares the
 * stream: it then carries the monitor's 48 kHz mono s16le, as the UDP port + 2 stream does. The `-8` source monitor
 * during digital decoding does not, since its 48 kHz audio would interleave with the digital voice's 8 kHz stream.
 */
int dsd_audio_sink_monitor_takes_fd(const dsd_opts* opts);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_SINK_H_ */
