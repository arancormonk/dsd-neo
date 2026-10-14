// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_RUNTIME_SIGNAL_SEARCH_HOOKS_H
#define DSD_NEO_RUNTIME_SIGNAL_SEARCH_HOOKS_H
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void (*dsd_signal_search_evidence_fn)(const dsd_state*, const char*, uint32_t, uint32_t, const char*);
typedef int (*dsd_signal_search_active_fn)(const dsd_state*);
/** Decoder-thread query: during a search session the symbol readers take a failed (bounded) live read as an
 * interruption, not the end of the stream. */
void dsd_signal_search_active_set(dsd_signal_search_active_fn fn);
int dsd_signal_search_is_running(const dsd_state* state);
/** Install at engine startup; protocol code reports only CRC/FEC-confirmed observations. */
void dsd_signal_search_evidence_set(dsd_signal_search_evidence_fn fn);
void dsd_signal_search_note_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                     const char* access);
#ifdef __cplusplus
}
#endif
#endif
