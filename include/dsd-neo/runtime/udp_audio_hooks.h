// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Runtime hook table for optional UDP audio output.
 *
 * Lower layers should not depend on IO backend headers directly. The engine
 * installs real hook functions at startup; the runtime provides safe wrappers
 * that no-op when hooks are not installed.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_UDP_AUDIO_HOOKS_H_
#define DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_UDP_AUDIO_HOOKS_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*blast)(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);
    void (*blast_analog)(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);
    /* Open the analog UDP socket (port + 2) for opts->udp_hostname:udp_portno. On failure the socket is left
       DSD_INVALID_SOCKET. Returns 0 on success, -1 on failure. */
    int (*connect_analog)(dsd_opts* opts);
} dsd_udp_audio_hooks;

void dsd_udp_audio_hooks_set(dsd_udp_audio_hooks hooks);

void dsd_udp_audio_hook_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);
void dsd_udp_audio_hook_blast_analog(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);
/**
 * @brief Open the analog UDP output socket (port + 2) a session did not open at start.
 *
 * @return 0 when the socket is open, -1 when opening failed or no UDP backend is installed.
 */
int dsd_udp_audio_hook_connect_analog(dsd_opts* opts);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_UDP_AUDIO_HOOKS_H_ */
