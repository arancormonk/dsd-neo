// SPDX-License-Identifier: GPL-3.0-or-later
/** @file @brief Admission and service identity from validated reception, with explicit protocol/slot context. */
#ifndef DSD_NEO_CORE_RECEIVE_H
#define DSD_NEO_CORE_RECEIVE_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

void dsd_receive_identity_current(const dsd_state* state, int protocol, int slot, dsd_receive_identity* out);
/** Whether @p a and @p b are different services: both carry a verified code and the codes differ, or both carry a
 * proven over-the-air slot and the slots differ (the single-slot DMR decoder's one lane can move between them). */
int dsd_receive_identity_changed(const dsd_receive_identity* a, const dsd_receive_identity* b);
/** Fold what @p received verifies into the identity a call @p held: a verified code and a proven slot replace
 * what it held, and a slot still pending leaves the slot it had proven, so the call keeps its service meanwhile. */
void dsd_receive_identity_merge(dsd_receive_identity* held, const dsd_receive_identity* received);
dsd_receive_verdict dsd_receive_status(const dsd_opts* opts, const dsd_state* state, int protocol, int slot);
int dsd_receive_allows(const dsd_opts* opts, const dsd_state* state, int protocol, int slot);
/** Whether any receive filter (slot, DCC, RAN, NAC) is enabled; the default policy has none. */
int dsd_receive_filters_enabled(const dsd_opts* opts);
/** A grant can constrain its slot before destination access-code evidence arrives. */
int dsd_receive_grant_allows(const dsd_opts* opts, int protocol, int slot);
/** Whether the policy in force admits a call by the identity it was received with (a call snapshot's), whatever the
 * decoder holds now: a sync loss forgets the live code before the carrier proves it again. */
int dsd_receive_identity_admitted(const dsd_opts* opts, const dsd_receive_identity* identity);
/** Whether the policy admits everything on the carrier the decoder is on: both over-the-air slots of a two-slot one,
 * each judged as that slot whatever the single-slot DMR decoder has proven for its one lane. */
int dsd_receive_carrier_allows(const dsd_opts* opts, const dsd_state* state);
/** For generic output paths only. Protocol paths should supply their protocol explicitly. */
int dsd_receive_current_allows(const dsd_opts* opts, const dsd_state* state, int slot);

/** What generic output paths judge by while a protocol decodes or plays through another's decoder under that
 * decoder's synctype (YSF and scrambled dPMR through NXDN's): begin names the owner (a DSD_SYNC_* value) and returns
 * the context to hand back to end. */
typedef struct {
    int protocol;
    uint8_t active;
} dsd_receive_context;

dsd_receive_context dsd_receive_context_begin(dsd_state* state, int protocol);
void dsd_receive_context_end(dsd_state* state, dsd_receive_context saved);
/** Remember the access code @p state has verified for @p protocol on the voice channel at @p freq_hz (nothing when
 * none is verified yet). A channel keeps its code, so the newest verified one replaces what was remembered. */
void dsd_receive_carriers_note(dsd_receive_carriers* carriers, const dsd_state* state, int protocol, long freq_hz);
/** Whether the receive policy in force rejects the access code last verified on @p freq_hz. A channel never verified
 * is not rejected: a grant to it gets the acquisition window. */
int dsd_receive_carriers_rejects(const dsd_receive_carriers* carriers, const dsd_opts* opts, long freq_hz);

#ifdef __cplusplus
}
#endif
#endif
