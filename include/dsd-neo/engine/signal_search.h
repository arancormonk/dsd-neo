// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_ENGINE_SIGNAL_SEARCH_H
#define DSD_NEO_ENGINE_SIGNAL_SEARCH_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Decoder-thread observer; must copy the records and must not perform file I/O. */
typedef void (*dsd_signal_search_publish_fn)(const dsd_signal_search_status*, const dsd_signal_search_hit*, void*);
int dsd_engine_signal_search_start(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                                   dsd_signal_search_publish_fn observer, void* user, char* error, size_t capacity);
int dsd_engine_signal_search_control(dsd_opts* opts, dsd_state* state, const dsd_signal_search_control* action);
int dsd_engine_signal_search_tick(dsd_opts* opts, dsd_state* state);
int dsd_engine_signal_search_active(const dsd_state* state);
int dsd_engine_signal_search_should_yield(const dsd_opts* opts, dsd_state* state);
void dsd_engine_signal_search_shutdown(dsd_opts* opts, dsd_state* state);
/** Only call with protocol-confirmed observations from runtime evidence hooks. */
void dsd_engine_signal_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                       const char* access);
#ifdef __cplusplus
}
#endif
#endif
