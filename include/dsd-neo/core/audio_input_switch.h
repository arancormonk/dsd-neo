// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Switching the running audio input to another one at runtime (issue #634).
 *
 * The decoder thread replaces the PCM or symbol input it reads: a WAV or raw file, a symbol capture, UDP or TCP audio,
 * or the platform capture device (Pulse). The new input is opened before the old one is closed, so a switch that
 * cannot open its input changes nothing: the old input keeps running exactly as it was. A radio stream running behind
 * a PCM input is never touched.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_INPUT_SWITCH_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_INPUT_SWITCH_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/platform/sockets.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The input a switch opens. */
typedef enum {
    DSD_AUDIO_INPUT_PCM_FILE = 0, /**< A WAV at its header's rate, or headerless PCM16LE mono at the raw rate. */
    DSD_AUDIO_INPUT_SYMBOL_FLT,   /**< A float symbol stream (`.raw`, `.sym`). */
    DSD_AUDIO_INPUT_SYMBOL_BIN,   /**< A symbol capture, replayed at symbol pace (`.bin`). */
    DSD_AUDIO_INPUT_UDP,          /**< PCM16LE datagrams bound on host:port. */
    DSD_AUDIO_INPUT_TCP,          /**< PCM16LE over tcp_sockfd, already connected to host:port. */
    DSD_AUDIO_INPUT_PULSE,        /**< The platform capture device. */
} dsd_audio_input_kind;

/** What to open. Every string may point into the options; the switch copies them first. */
typedef struct {
    dsd_audio_input_kind kind;
    /** File kinds: the path, a regular file. PULSE: the device name, "" for the default device, NULL for the
        configured one (dsd_opts::pa_input_idx). */
    const char* path;
    /** UDP: the bind address (NULL or "" binds 127.0.0.1). TCP: the host tcp_sockfd is connected to. */
    const char* host;
    /** UDP bind port or TCP port, 1..65535. */
    int port;
    /** TCP: the connected socket. The input owns it once the switch succeeds; the caller still owns it otherwise. */
    dsd_socket_t tcp_sockfd;
} dsd_audio_input_request;

/**
 * What a switch did. The values are positive so they never collide with the DSD_ERR_* codes a service may return
 * instead (DSD_ERR_NOT_SUPPORTED is -2).
 */
typedef enum {
    /** The new input runs; every PCM and symbol input that ran before is closed. */
    DSD_AUDIO_INPUT_SWITCHED = 0,
    /** The new input did not open, or the request was invalid. The old input runs as it was. */
    DSD_AUDIO_INPUT_KEPT = 1,
    /** The new UDP endpoint did not bind, and the UDP input it replaced runs again on its own endpoint: a new stream
        on the old input (its socket and queue were closed to free the port). */
    DSD_AUDIO_INPUT_RESTARTED = 2,
    /** The new UDP endpoint did not bind, and the one it replaced did not bind again either: no UDP input runs. */
    DSD_AUDIO_INPUT_LOST = 3,
} dsd_audio_input_switch_result;

/**
 * @brief Replace the running PCM or symbol input with the one @p req names.
 *
 * Runs on the decoder thread, which owns the options and the state. The new input is opened first; only then is
 * every PCM and symbol input the options hold closed: a file and its SF_INFO, a symbol file, a Pulse stream, a TCP
 * context and socket, and a UDP input (stopped first when the new input is UDP, since it may hold the port asked
 * for). On success the input type and name (audio_in_dev: the path, `udp:<bind>:<port>`, `tcp:<host>:<port>`,
 * `pulse` or `pulse:<dev>`) describe the new input, a radio spec the name replaced is kept in
 * dsd_opts::radio_in_dev, the raw PCM rate a WAV header replaced comes back, the PCM input state is reset as for a new
 * stream (dsd_opts::pcm_input_generation moves), and the symbol timing follows the new input's rate. A failure never
 * writes the input-failure latch, and logs why.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param req The input to open.
 * @return A dsd_audio_input_switch_result.
 */
int dsd_audio_switch_input(dsd_opts* opts, dsd_state* state, const dsd_audio_input_request* req);

/**
 * @brief The input kind `-i <path>` opens a file path as: `.raw` and `.sym` are float symbol streams, `.bin` a symbol
 * capture, anything else PCM audio (a WAV by its header, `.rrc` at 48 kHz, otherwise headerless PCM16LE).
 *
 * The extension is read from the last '.' of the whole path, as the startup open reads it.
 */
dsd_audio_input_kind dsd_audio_path_input_kind(const char* path);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_AUDIO_INPUT_SWITCH_H_ */
