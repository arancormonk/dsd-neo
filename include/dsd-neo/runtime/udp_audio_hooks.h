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
} dsd_udp_audio_hooks;

void dsd_udp_audio_hooks_set(dsd_udp_audio_hooks hooks);

void dsd_udp_audio_hook_blast(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);
void dsd_udp_audio_hook_blast_analog(const dsd_opts* opts, dsd_state* state, size_t nsam, const void* data);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_RUNTIME_UDP_AUDIO_HOOKS_H_ */
