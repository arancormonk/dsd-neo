// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_ENGINE_DISPATCH_PROTOCOL_DISPATCH_IMPL_H_
#define DSD_NEO_SRC_ENGINE_DISPATCH_PROTOCOL_DISPATCH_IMPL_H_

#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

int dsd_dispatch_matches_nxdn(int synctype);
void dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_dstar(int synctype);
void dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_dmr(int synctype);
void dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_x2tdma(int synctype);
void dsd_dispatch_handle_x2tdma(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_provoice(int synctype);
void dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_edacs(int synctype);
void dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_ysf(int synctype);
void dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_m17(int synctype);
void dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_p25p2(int synctype);
void dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_dpmr(int synctype);
void dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state);

int dsd_dispatch_matches_p25p1(int synctype);
void dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

#endif /* DSD_NEO_SRC_ENGINE_DISPATCH_PROTOCOL_DISPATCH_IMPL_H_ */
