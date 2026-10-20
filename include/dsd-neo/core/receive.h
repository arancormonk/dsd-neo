// SPDX-License-Identifier: GPL-3.0-or-later
/** @file @brief Admission and service identity from validated reception, with explicit protocol/slot context. */
#ifndef DSD_NEO_CORE_RECEIVE_H
#define DSD_NEO_CORE_RECEIVE_H
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/receive_types.h>
#include <dsd-neo/core/state_fwd.h>
#ifdef __cplusplus
extern "C" {
#endif

void dsd_receive_identity_current(const dsd_state* state, int protocol, int slot, dsd_receive_identity* out);
int dsd_receive_identity_changed(const dsd_receive_identity* a, const dsd_receive_identity* b);
dsd_receive_verdict dsd_receive_status(const dsd_opts* opts, const dsd_state* state, int protocol, int slot);
int dsd_receive_allows(const dsd_opts* opts, const dsd_state* state, int protocol, int slot);
/** A grant can constrain its slot before destination access-code evidence arrives. */
int dsd_receive_grant_allows(const dsd_opts* opts, int protocol, int slot);
/** For generic output paths only. Protocol paths should supply their protocol explicitly. */
int dsd_receive_current_allows(const dsd_opts* opts, const dsd_state* state, int slot);

#ifdef __cplusplus
}
#endif
#endif
