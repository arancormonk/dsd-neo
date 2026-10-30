// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_ENGINE_SIGNAL_SEARCH_H
#define DSD_NEO_ENGINE_SIGNAL_SEARCH_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/runtime/signal_search.h>
#include <dsd-neo/runtime/signal_search_hooks.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Decoder-thread observer; must copy the records and must not perform file I/O. */
typedef void (*dsd_signal_search_publish_fn)(const dsd_signal_search_status*, const dsd_signal_search_hit*, void*);
int dsd_engine_signal_search_start(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                                   dsd_signal_search_publish_fn observer, void* user, char* error, size_t capacity);
/** Whether dsd_engine_signal_search_start() would refuse @p config now, by the checks it makes before it changes
 * anything: 0 when it would go on, -1 (the reason in @p error) when it would refuse. Changes nothing. A caller that must
 * act only for a start that goes on (issue #671: taking back a -Y resume) asks first. */
int dsd_engine_signal_search_start_refused(const dsd_opts* opts, const dsd_state* state,
                                           const dsd_signal_search_config* config, char* error, size_t capacity);
int dsd_engine_signal_search_control(dsd_opts* opts, dsd_state* state, const dsd_signal_search_control* action);
/** Whether @p action is valid for the running search now, without acting on it: 0 with the frequency it acts on (the
 * current hit's, or frequency_hz for UNAVOID and AVOID_FREQUENCY), -1 for a stale session or hit. */
int dsd_engine_signal_search_target(const dsd_state* state, const dsd_signal_search_control* action,
                                    uint32_t* frequency);
/** Show @p message in the running search's status (an action's refusal decided outside the coordinator). It stays
 * until an action takes effect; a stopped or failed search, or another session, keeps its own message. */
void dsd_engine_signal_search_notice(dsd_state* state, uint64_t session_id, const char* message);
int dsd_engine_signal_search_tick(dsd_opts* opts, dsd_state* state);
int dsd_engine_signal_search_active(const dsd_state* state);
int dsd_engine_signal_search_should_yield(const dsd_opts* opts, dsd_state* state);
void dsd_engine_signal_search_shutdown(dsd_opts* opts, dsd_state* state);
/** Only call with protocol-confirmed observations from runtime evidence hooks. */
void dsd_engine_signal_search_evidence_at(const dsd_state* state, const char* protocol, int sync, int slot,
                                          uint32_t target, uint32_t source, const char* access);
void dsd_engine_signal_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                       const char* access);
/** Runtime site hook: joins a CRC/FEC-checked site note to the visit's site record and publishes the hit when its shown
 * site changes. Notes outside a visit, or from a frame an input interruption cut, are dropped. */
void dsd_engine_signal_search_site(const dsd_state* state, const dsd_signal_search_site_note* note);
/** Runtime call hook: reads @p slot's canonical call record (active, CRC-clean, a protocol the search identifies) and
 * makes it the hit's call tuple -- IDs, texts, slot and crypt from that one record -- when it is the tuple's own call or
 * a call that began no earlier; publishes the hit when the shown tuple changes. Outside a visit, or from a frame an
 * input interruption cut, it does nothing. */
void dsd_engine_signal_search_call(const dsd_state* state, uint8_t slot);
#ifdef __cplusplus
}
#endif
#endif
