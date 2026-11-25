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
/** Called once by a start that goes on, on the decoder thread, after every check has passed and everything the start
 * could fail on is had, right before its first change (issue #748 B13): what a caller does only for a start that goes
 * on, and before the search takes the session's settings (issue #671: taking back a -Y resume still waiting on its
 * row, whose staged scopes must not become those settings). */
typedef void (*dsd_signal_search_commit_fn)(dsd_opts* opts, dsd_state* state, void* user);
/** Start a search, or restart a paused session's with edited ranges. Validate, then reserve, then commit: a start that
 * fails (-1, the reason in @p error) has changed nothing. */
int dsd_engine_signal_search_start(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                                   dsd_signal_search_publish_fn observer, void* user, char* error, size_t capacity);
/** dsd_engine_signal_search_start() calling @p commit (NULL: none) with @p commit_user at its commit point. */
int dsd_engine_signal_search_start_with_commit(dsd_opts* opts, dsd_state* state, const dsd_signal_search_config* config,
                                               dsd_signal_search_publish_fn observer, void* user,
                                               dsd_signal_search_commit_fn commit, void* commit_user, char* error,
                                               size_t capacity);
/** Whether dsd_engine_signal_search_start() would refuse @p config now, by the checks it makes before it changes
 * anything: 0 when it would go on, -1 (the reason in @p error) when it would refuse. Changes nothing. A start can still
 * fail past these checks for memory; what must happen only for a start that goes on belongs in its commit hook. */
int dsd_engine_signal_search_start_refused(const dsd_opts* opts, const dsd_state* state,
                                           const dsd_signal_search_config* config, char* error, size_t capacity);

/** The steps a fresh start can fail at (issue #748 B13), for dsd_engine_signal_search_test_fail_start(). */
typedef enum {
    DSD_SEARCH_START_FAIL_NONE = 0,
    /** The coordinator's own allocation. */
    DSD_SEARCH_START_FAIL_COORD,
    /** The scan scope that holds the Auto baseline. */
    DSD_SEARCH_START_FAIL_SCOPE,
    /** The session's P25 site, with its channel map's storage. */
    DSD_SEARCH_START_FAIL_SITE,
    /** The scan owner the coordinator attaches to. */
    DSD_SEARCH_START_FAIL_OWNER
} dsd_signal_search_start_fail;
#ifdef DSD_NEO_TEST_HOOKS
/** Have every later fresh start fail at @p point as if its allocation there failed (DSD_SEARCH_START_FAIL_NONE: none). */
void dsd_engine_signal_search_test_fail_start(int point);
#endif
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
/** Whether a visit's or the restore's tune is still outstanding (issue #899): the decoder writes no squelch gate until
 * it completes. */
int dsd_engine_signal_search_tune_pending(const dsd_state* state);
int dsd_engine_signal_search_should_yield(const dsd_opts* opts, dsd_state* state);
/** Runtime frame-sync signal_search_probe hook (issue #804): 1 while a visit to a range with Auto identification is
 * probing (no protocol evidence yet, the probe time running, the detection not ended); 0 for a typed or analog range,
 * the probe's analog fallback, a listening or ended visit, a span tune, a paused or stopped search. */
int dsd_engine_signal_search_probe(const dsd_opts* opts, const dsd_state* state);
void dsd_engine_signal_search_shutdown(dsd_opts* opts, dsd_state* state);
/** Puts the session's own P25 site back (issue #804) when the decoder session ended during a visit: the search's
 * shutdown left the visit's code and tables for the session's final call snapshots. The engine's teardown calls it
 * after them, before anything that reads the site (the band plan export). Unproven; a no-op otherwise, and again. A
 * search with a TETRA range starts the TETRA state cold here first and lets its hold go (issue #748). */
void dsd_engine_signal_search_release_site(dsd_opts* opts, dsd_state* state);
/** Only call with protocol-confirmed observations from runtime evidence hooks. */
void dsd_engine_signal_search_evidence_at(const dsd_state* state, const char* protocol, int sync, int slot,
                                          uint32_t target, uint32_t source, const char* access);
void dsd_engine_signal_search_evidence(const dsd_state* state, const char* protocol, uint32_t target, uint32_t source,
                                       const char* access);
/** Runtime site hook: joins a CRC/FEC-checked site note to the visit's site record and publishes the hit when its shown
 * site changes. Notes outside a visit, or from a frame an input interruption cut, are dropped. */
void dsd_engine_signal_search_site(const dsd_state* state, const dsd_signal_search_site_note* note);
/** Runtime P25 Phase 2 codes hook (issue #804): copies up to @p cap of the session's learned scrambling codes, most
 * recently confirmed first, into @p out while a digital visit is receiving; 0 otherwise. */
size_t dsd_engine_signal_search_p25p2_codes(const dsd_state* state, uint64_t* out, size_t cap);
/** Runtime P25 Phase 2 code hook (issue #804): a complete scrambling code from content that passed its CRC/FEC check,
 * with its dsd_p25p2_code_source, joins the session's list (or moves to its front) while a visit is receiving. A code
 * with a reserved field, or an unknown source, joins nothing. The list holds 16; Pause and Resume keep it, a new
 * session starts with none. */
void dsd_engine_signal_search_p25p2_code(const dsd_state* state, uint64_t code, int source);
/** Runtime call hook: reads @p slot's canonical call record (active, CRC-clean, a protocol the search identifies) and
 * makes it the hit's call tuple -- IDs, texts, slot and crypt from that one record -- when it is the tuple's own call or
 * a call that began no earlier; publishes the hit when the shown tuple changes. Outside a visit, or from a frame an
 * input interruption cut, it does nothing. */
void dsd_engine_signal_search_call(const dsd_state* state, uint8_t slot);
/** Runtime access hook. With @p note NULL, a checked proof gave the access code of the decoder's current protocol a
 * new value (a P25 Phase 2 NAC proven after the transmission's last evidence, issue #802): while a visit receives a
 * confirmed hit of that sync type's family, it publishes the hit as changed, so the observer reads the code again.
 * With @p note, the producer carried the code itself (an M17 call-free LSF's CAN, issue #748 B14): while a visit
 * receives and the receive policy admits that code, it is the visit's noted code, which the hit carries
 * (dsd_signal_search_hit::access_noted) while it is confirmed as a protocol of the note's sync type's family, publishing
 * when that changes; the hit's protocol changing to another family forgets it. Either way it confirms nothing and changes no phase or activity. Anything else it drops. */
void dsd_engine_signal_search_access(const dsd_state* state, const dsd_signal_search_access_note* note);
/** Runtime activity hook (issue #748): content of a transmission the decoder already reported as evidence keeps the
 * visit listening. While a visit receives a confirmed hit, it refreshes the visit's activity time on the decode clock;
 * it publishes, confirms and changes nothing else. Anything else it drops. */
void dsd_engine_signal_search_activity(const dsd_state* state);
/** The Signal search family name of @p synctype ("DMR", "P25", "NXDN", "dPMR", "M17", "YSF", "D-STAR", "TETRA"), the
 * name a hit's protocol starts with when that sync type's protocol confirmed it ("NXDN" for NXDN48 and NXDN96); NULL
 * for a sync type the search does not identify (ProVoice, EDACS, X2-TDMA, analog, none). TETRA direct mode shares the
 * trunked-mode sync words, so no sync type names "TETRA DMO" (issue #748). */
const char* dsd_engine_signal_search_sync_protocol(int synctype);
/** Whether @p synctype is of the family of a hit whose protocol is @p hit_protocol: the sync type has a family name
 * (dsd_engine_signal_search_sync_protocol()) and @p hit_protocol starts with it ("NXDN" for NXDN48 and NXDN96), except
 * that TETRA's family is "TETRA" exactly, never "TETRA DMO". 0 for a sync type with no family name, or a NULL
 * @p hit_protocol. */
int dsd_engine_signal_search_family_matches(const char* hit_protocol, int synctype);
/** Whether @p protocol is a TETRA identification, "TETRA" (trunked) or "TETRA DMO" (issue #748): the hit's access and
 * site then come from the TETRA decoder's own notes alone, and it shows no call details. */
int dsd_engine_signal_search_protocol_is_tetra(const char* protocol);
#ifdef __cplusplus
}
#endif
#endif
