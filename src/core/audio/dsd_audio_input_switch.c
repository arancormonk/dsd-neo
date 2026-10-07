// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The runtime input switch (issue #634). A switch opens the new input before it closes the one that runs, so one that
 * cannot open its input changes nothing: the old handles are taken out of the options while the new one opens and put
 * back as they were. A UDP input cannot be taken out (its receive thread reads the options), so a switch to UDP stops
 * the one that runs first, since it may hold the port asked for, and starts it again on its own endpoint when the new
 * one does not bind.
 *
 * This lives in its own object file, apart from dsd_audio.c, so a link-time wrap of openAudioInput() (the app-control
 * queue tests) reaches the Pulse open here.
 */

#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/audio_filters.h>
#include <dsd-neo/core/audio_input_switch.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/dsp/frame_sync.h>
#include <dsd-neo/platform/audio.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/sockets.h>
#include <dsd-neo/runtime/log.h>
#include <dsd-neo/runtime/net_audio_input_hooks.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd_audio_internal.h"

enum {
    SWITCH_RRC_SAMPLE_RATE = 48000, /* an M17 .rrc file is 48 kHz whatever the raw rate, as `-i` opens it */
    SWITCH_PORT_MAX = 65535,
};

/* The handles a PCM or symbol input holds in the options, taken out of them while the new input opens. */
typedef struct {
    SNDFILE* file;
    SF_INFO* file_info;
    FILE* symbolfile;
    tcp_input_ctx* tcp_in_ctx;
    dsd_socket_t tcp_sockfd;
    dsd_audio_stream* stream;
} switch_handles;

/* The new input, opened but not yet in the options. */
typedef struct {
    SNDFILE* file;
    SF_INFO* file_info;
    FILE* symbolfile;
    tcp_input_ctx* tcp_in_ctx;
    int rate_hz; /* the rate the input runs at: a WAV header's or a .rrc file's, else the raw rate */
} switch_opened;

/* The UDP input a switch to UDP stopped, to start again if the new endpoint does not bind. */
typedef struct {
    int stopped;
    char bindaddr[sizeof(((dsd_opts*)0)->udp_in_bindaddr)];
    int port;
    int rate_hz;
} switch_udp_before;

dsd_audio_input_kind
dsd_audio_path_input_kind(const char* path) {
    /* The extension dispatch of openAudioInDevice() (dsd_audio_open_extension_input()), which reads the last '.' of
       the whole path and compares its first four characters. */
    const char* extension = path ? strrchr(path, '.') : NULL;
    if (extension == NULL) {
        return DSD_AUDIO_INPUT_PCM_FILE;
    }
    if (strncmp(extension, ".raw", 4) == 0 || strncmp(extension, ".sym", 4) == 0) {
        return DSD_AUDIO_INPUT_SYMBOL_FLT;
    }
    if (strncmp(extension, ".bin", 4) == 0) {
        return DSD_AUDIO_INPUT_SYMBOL_BIN;
    }
    return DSD_AUDIO_INPUT_PCM_FILE;
}

static int
switch_path_is_rrc(const char* path) {
    const char* extension = strrchr(path, '.');
    return extension != NULL && strncmp(extension, ".rrc", 4) == 0;
}

static int
switch_request_valid(const dsd_audio_input_request* req) {
    switch (req->kind) {
        case DSD_AUDIO_INPUT_PCM_FILE:
        case DSD_AUDIO_INPUT_SYMBOL_FLT:
        case DSD_AUDIO_INPUT_SYMBOL_BIN: return req->path != NULL && req->path[0] != '\0';
        case DSD_AUDIO_INPUT_UDP: return req->port >= 1 && req->port <= SWITCH_PORT_MAX;
        case DSD_AUDIO_INPUT_TCP:
            return req->host != NULL && req->host[0] != '\0' && req->port >= 1 && req->port <= SWITCH_PORT_MAX
                   && req->tcp_sockfd != DSD_INVALID_SOCKET;
        case DSD_AUDIO_INPUT_PULSE: return 1;
        default: return 0;
    }
}

static void
switch_detach(dsd_opts* opts, switch_handles* held) {
    held->file = opts->audio_in_file;
    held->file_info = opts->audio_in_file_info;
    held->symbolfile = opts->symbolfile;
    held->tcp_in_ctx = opts->tcp_in_ctx;
    held->tcp_sockfd = opts->tcp_sockfd;
    held->stream = opts->audio_in_stream;
    opts->audio_in_file = NULL;
    opts->audio_in_file_info = NULL;
    opts->symbolfile = NULL;
    opts->tcp_in_ctx = NULL;
    opts->tcp_sockfd = DSD_INVALID_SOCKET;
    opts->audio_in_stream = NULL;
}

static void
switch_reattach(dsd_opts* opts, const switch_handles* held) {
    opts->audio_in_file = held->file;
    opts->audio_in_file_info = held->file_info;
    opts->symbolfile = held->symbolfile;
    opts->tcp_in_ctx = held->tcp_in_ctx;
    opts->tcp_sockfd = held->tcp_sockfd;
    opts->audio_in_stream = held->stream;
}

static void
switch_close_held(const switch_handles* held) {
    if (held->file) {
        sf_close(held->file);
    }
    free(held->file_info);
    if (held->symbolfile) {
        fclose(held->symbolfile);
    }
    if (held->tcp_in_ctx) {
        dsd_net_audio_input_hook_tcp_close(held->tcp_in_ctx);
    }
    if (held->tcp_sockfd != DSD_INVALID_SOCKET) {
        dsd_socket_close(held->tcp_sockfd);
    }
    if (held->stream) {
        dsd_audio_close(held->stream);
    }
}

/* A file a switch opens must be a regular file: a pipe or device would block the decoder thread in its open until a
   writer turns up, which leaves only startup (`-i`) for those. */
static int
switch_regular_file(const char* path) {
    dsd_stat_t st;
    if (dsd_stat_path(path, &st) != 0) {
        LOG_ERROR("Input switch: no such file %s\n", path);
        return 0;
    }
    if (!dsd_stat_is_regular(&st)) {
        LOG_ERROR("Input switch: %s is not a regular file (a pipe or device can be given at startup with -i)\n", path);
        return 0;
    }
    return 1;
}

static int
switch_open_pcm_file(const char* path, int raw_rate_hz, int file_rate_hz, switch_opened* out) {
    if (!switch_regular_file(path)) {
        return -1;
    }
    const int rrc = switch_path_is_rrc(path);
    const int requested_hz = rrc ? SWITCH_RRC_SAMPLE_RATE : file_rate_hz;
    int active_hz = requested_hz;
    int container = 0;
    if (dsd_audio_open_mono_file_input(path, requested_hz, &out->file, &out->file_info, &active_hz, &container) != 0) {
        LOG_ERROR("Input switch: couldn't open %s: %s\n", path, sf_strerror(NULL));
        return -1;
    }
    if (container && active_hz != raw_rate_hz) {
        LOG_INFO("NOTICE: WAV header sample rate %d Hz overrides configured %d Hz for %s\n", active_hz, raw_rate_hz,
                 path);
    }
    out->rate_hz = active_hz > 0 ? active_hz : requested_hz;
    return 0;
}

static int
switch_open_symbol_file(const char* path, switch_opened* out) {
    if (!switch_regular_file(path)) {
        return -1;
    }
    out->symbolfile = dsd_fopen_existing_regular_file(path, "rb");
    if (out->symbolfile == NULL) {
        LOG_ERROR("Input switch: couldn't open symbol file %s\n", path);
        return -1;
    }
    return 0;
}

static int
switch_open_tcp(const dsd_audio_input_request* req, int raw_rate_hz, switch_opened* out) {
    out->tcp_in_ctx = dsd_net_audio_input_hook_tcp_open(req->tcp_sockfd, raw_rate_hz);
    if (out->tcp_in_ctx == NULL) {
        LOG_ERROR("Input switch: couldn't open TCP audio input\n");
        return -1;
    }
    return 0;
}

/* Stop the running UDP input, which may hold the port the new one asks for, remembering its endpoint. */
static void
switch_stop_udp(dsd_opts* opts, switch_udp_before* before) {
    if (!opts->udp_in_ctx) {
        return;
    }
    before->stopped = 1;
    DSD_SNPRINTF(before->bindaddr, sizeof before->bindaddr, "%s", opts->udp_in_bindaddr);
    before->port = opts->udp_in_portno;
    before->rate_hz = opts->wav_sample_rate;
    dsd_net_audio_input_hook_udp_stop(opts);
}

static int
switch_open_udp(dsd_opts* opts, const char* bindaddr, int port, int raw_rate_hz) {
    if (dsd_net_audio_input_hook_udp_start(opts, bindaddr, port, raw_rate_hz) != 0) {
        LOG_ERROR("Input switch: couldn't bind UDP input on %s:%d\n", bindaddr, port);
        return -1;
    }
    return 0;
}

/* The device a Pulse switch opens: @p path when given ("" being the default device), else the configured one. */
static int
switch_open_pulse(dsd_opts* opts, const char* device, int device_given) {
    char configured[sizeof opts->pa_input_idx];
    if (device_given && strlen(device) >= sizeof configured) {
        LOG_ERROR("Input switch: the Pulse device name is longer than %zu characters\n", sizeof configured - 1U);
        return -1;
    }
    DSD_MEMCPY(configured, opts->pa_input_idx, sizeof configured);
    if (device_given) {
        DSD_SNPRINTF(opts->pa_input_idx, sizeof opts->pa_input_idx, "%s", device);
    }
    if (openAudioInput(opts) != 0) {
        LOG_ERROR("Input switch: couldn't open the Pulse input%s%s\n", opts->pa_input_idx[0] ? " " : "",
                  opts->pa_input_idx);
        DSD_MEMCPY(opts->pa_input_idx, configured, sizeof opts->pa_input_idx);
        return -1;
    }
    return 0;
}

/* Put the UDP input a failed switch to UDP stopped back on its endpoint. */
static int
switch_restart_udp(dsd_opts* opts, const switch_udp_before* before) {
    if (!before->stopped) {
        return DSD_AUDIO_INPUT_KEPT;
    }
    if (dsd_net_audio_input_hook_udp_start(opts, before->bindaddr, before->port, before->rate_hz) != 0) {
        LOG_ERROR("Input switch: the UDP input on %s:%d did not bind again; no UDP input runs\n", before->bindaddr,
                  before->port);
        return DSD_AUDIO_INPUT_LOST;
    }
    return DSD_AUDIO_INPUT_RESTARTED;
}

/* The rate the next PCM input opens at: the raw rate a running file's own rate replaced, or the raw rate itself. */
static int
switch_raw_rate(const dsd_opts* opts) {
    if (opts->wav_header_replaced_rate > 0) {
        return opts->wav_header_replaced_rate;
    }
    return opts->wav_sample_rate > 0 ? opts->wav_sample_rate : 48000;
}

static void
switch_commit_name(dsd_opts* opts, const char* name) {
    if (dsd_opts_audio_in_dev_is_radio_spec(opts->audio_in_dev)) {
        DSD_SNPRINTF(opts->radio_in_dev, sizeof opts->radio_in_dev, "%s", opts->audio_in_dev);
    }
    DSD_SNPRINTF(opts->audio_in_dev, sizeof opts->audio_in_dev, "%s", name);
}

static void
switch_commit_rate(dsd_opts* opts, int raw_rate_hz, int active_rate_hz) {
    if (active_rate_hz <= 0) {
        active_rate_hz = raw_rate_hz;
    }
    if (opts->wav_sample_rate != active_rate_hz) {
        dsd_opts_apply_input_sample_rate(opts, active_rate_hz);
    }
    opts->wav_header_replaced_rate = (active_rate_hz != raw_rate_hz) ? raw_rate_hz : 0;
    /* A staged file rate (a config's file_sample_rate while another input ran) stays: it is the rate every later file
       opens at, as at startup (dsd_opts_requested_file_sample_rate()). Resampler and staging cleared, and a new stream
       for the PCM noise squelch (issue #628). */
    dsd_opts_reset_pcm_input_state(opts);
}

/* The symbol timing follows the new input's rate. Between PCM inputs it is rescaled, as the startup open rescales it
   for a WAV header. A radio input's timing is not in units of any PCM rate (one sample per symbol on symbol output,
   the rate's own on the FSK discriminator), so leaving one sets it afresh from the profile the hunt is on. */
static void
switch_commit_timing(const dsd_opts* opts, dsd_state* state, int old_type, int old_rate_hz) {
    int new_rate_hz = dsd_opts_current_input_timing_rate(opts);
    if (new_rate_hz <= 0) {
        new_rate_hz = 48000;
    }
    if (old_type == AUDIO_IN_RTL) {
        const int symbol_rate_hz = dsd_frame_sync_active_profile_symbol_rate_hz(state);
        state->samplesPerSymbol = dsd_opts_compute_sps_rate(opts, symbol_rate_hz, new_rate_hz);
        state->symbolCenter = dsd_opts_symbol_center(state->samplesPerSymbol);
        state->jitter = -1;
        init_audio_filters(state, new_rate_hz);
        return;
    }
    dsd_audio_rescale_symbol_timing(state, old_rate_hz, new_rate_hz);
}

static void
switch_install(dsd_opts* opts, dsd_state* state, const dsd_audio_input_request* req, const switch_opened* opened,
               const char* bindaddr) {
    dsd_audio_reset_symbol_replay_pacing(state);
    switch (req->kind) {
        case DSD_AUDIO_INPUT_PCM_FILE:
            opts->audio_in_file = opened->file;
            opts->audio_in_file_info = opened->file_info;
            opts->audio_in_type = AUDIO_IN_WAV;
            break;
        case DSD_AUDIO_INPUT_SYMBOL_FLT:
            opts->symbolfile = opened->symbolfile;
            opts->audio_in_type = AUDIO_IN_SYMBOL_FLT;
            break;
        case DSD_AUDIO_INPUT_SYMBOL_BIN:
            opts->symbolfile = opened->symbolfile;
            opts->audio_in_type = AUDIO_IN_SYMBOL_BIN;
            dsd_audio_enable_bin_symbol_replay(state);
            break;
        case DSD_AUDIO_INPUT_UDP:
            DSD_SNPRINTF(opts->udp_in_bindaddr, sizeof opts->udp_in_bindaddr, "%s", bindaddr);
            opts->udp_in_portno = req->port;
            opts->audio_in_type = AUDIO_IN_UDP;
            break;
        case DSD_AUDIO_INPUT_TCP:
            opts->tcp_in_ctx = opened->tcp_in_ctx;
            opts->tcp_sockfd = req->tcp_sockfd;
            opts->audio_in_type = AUDIO_IN_TCP;
            break;
        case DSD_AUDIO_INPUT_PULSE:
        default: opts->audio_in_type = AUDIO_IN_PULSE; break;
    }
}

/* The name the switched input runs under (dsd_opts::audio_in_dev), as startup and a saved config read it. */
static void
switch_input_name(const dsd_opts* opts, const dsd_audio_input_request* req, const char* path, const char* host,
                  char* name, size_t name_size) {
    switch (req->kind) {
        case DSD_AUDIO_INPUT_UDP: DSD_SNPRINTF(name, name_size, "udp:%s:%d", host, req->port); break;
        case DSD_AUDIO_INPUT_TCP: DSD_SNPRINTF(name, name_size, "tcp:%s:%d", host, req->port); break;
        case DSD_AUDIO_INPUT_PULSE:
            if (opts->pa_input_idx[0] != '\0') {
                DSD_SNPRINTF(name, name_size, "pulse:%s", opts->pa_input_idx);
            } else {
                DSD_SNPRINTF(name, name_size, "%s", "pulse");
            }
            break;
        default: DSD_SNPRINTF(name, name_size, "%s", path); break;
    }
}

static int
switch_open(dsd_opts* opts, const dsd_audio_input_request* req, const char* path, const char* host, int raw_rate_hz,
            switch_opened* opened) {
    switch (req->kind) {
        case DSD_AUDIO_INPUT_PCM_FILE: {
            const int file_rate_hz = opts->staged_file_sample_rate > 0 ? opts->staged_file_sample_rate : raw_rate_hz;
            return switch_open_pcm_file(path, raw_rate_hz, file_rate_hz, opened);
        }
        case DSD_AUDIO_INPUT_SYMBOL_FLT:
        case DSD_AUDIO_INPUT_SYMBOL_BIN: return switch_open_symbol_file(path, opened);
        case DSD_AUDIO_INPUT_UDP: return switch_open_udp(opts, host, req->port, raw_rate_hz);
        case DSD_AUDIO_INPUT_TCP: return switch_open_tcp(req, raw_rate_hz, opened);
        case DSD_AUDIO_INPUT_PULSE:
        default: return switch_open_pulse(opts, req->path ? path : "", req->path != NULL);
    }
}

/* Copy the request's path and host, which may name the options' own strings (Replay last names a path in them): the
   switch rewrites those. A UDP request with no host binds 127.0.0.1. Returns 0, or -1 when one does not fit. */
static int
switch_copy_request_strings(const dsd_audio_input_request* req, char* path, size_t path_size, char* host,
                            size_t host_size) {
    path[0] = '\0';
    host[0] = '\0';
    if (req->path) {
        if (strlen(req->path) >= path_size) {
            LOG_ERROR("Input switch: the path is longer than %zu characters\n", path_size - 1U);
            return -1;
        }
        DSD_SNPRINTF(path, path_size, "%s", req->path);
    }
    if (req->host && req->host[0] != '\0') {
        if (strlen(req->host) >= host_size) {
            LOG_ERROR("Input switch: the host name is longer than %zu characters\n", host_size - 1U);
            return -1;
        }
        DSD_SNPRINTF(host, host_size, "%s", req->host);
    } else if (req->kind == DSD_AUDIO_INPUT_UDP) {
        DSD_SNPRINTF(host, host_size, "%s", "127.0.0.1");
    }
    return 0;
}

int
dsd_audio_switch_input(dsd_opts* opts, dsd_state* state, const dsd_audio_input_request* req) {
    if (!opts || !state || !req || !switch_request_valid(req)) {
        return DSD_AUDIO_INPUT_KEPT;
    }
    char path[sizeof opts->audio_in_dev];
    char host[sizeof opts->udp_in_bindaddr];
    if (switch_copy_request_strings(req, path, sizeof path, host, sizeof host) != 0) {
        return DSD_AUDIO_INPUT_KEPT;
    }

    const int old_type = opts->audio_in_type;
    const int old_rate_hz = dsd_opts_current_input_timing_rate(opts);
    const int raw_rate_hz = switch_raw_rate(opts);

    switch_udp_before udp_before;
    DSD_MEMSET(&udp_before, 0, sizeof udp_before);
    if (req->kind == DSD_AUDIO_INPUT_UDP) {
        switch_stop_udp(opts, &udp_before);
    }
    switch_handles held;
    switch_detach(opts, &held);
    switch_opened opened;
    DSD_MEMSET(&opened, 0, sizeof opened);
    opened.rate_hz = raw_rate_hz;

    if (switch_open(opts, req, path, host, raw_rate_hz, &opened) != 0) {
        switch_reattach(opts, &held);
        return switch_restart_udp(opts, &udp_before);
    }

    switch_close_held(&held);
    if (req->kind != DSD_AUDIO_INPUT_UDP && opts->udp_in_ctx) {
        dsd_net_audio_input_hook_udp_stop(opts);
    }
    switch_install(opts, state, req, &opened, host);
    if (req->kind == DSD_AUDIO_INPUT_TCP) {
        DSD_SNPRINTF(opts->tcp_hostname, sizeof opts->tcp_hostname, "%s", host);
        opts->tcp_portno = req->port;
    }
    char name[sizeof opts->audio_in_dev];
    switch_input_name(opts, req, path, host, name, sizeof name);
    switch_commit_name(opts, name);
    switch_commit_rate(opts, raw_rate_hz, req->kind == DSD_AUDIO_INPUT_PCM_FILE ? opened.rate_hz : raw_rate_hz);
    switch_commit_timing(opts, state, old_type, old_rate_hz);
    return DSD_AUDIO_INPUT_SWITCHED;
}
