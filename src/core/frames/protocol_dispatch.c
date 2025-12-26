// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/core/protocol_dispatch.h>

#include <stddef.h>

extern int dsd_dispatch_matches_nxdn(int synctype);
extern void dsd_dispatch_handle_nxdn(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_dstar(int synctype);
extern void dsd_dispatch_handle_dstar(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_dmr(int synctype);
extern void dsd_dispatch_handle_dmr(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_x2tdma(int synctype);
extern void dsd_dispatch_handle_x2tdma(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_provoice(int synctype);
extern void dsd_dispatch_handle_provoice(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_edacs(int synctype);
extern void dsd_dispatch_handle_edacs(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_ysf(int synctype);
extern void dsd_dispatch_handle_ysf(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_m17(int synctype);
extern void dsd_dispatch_handle_m17(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_p25p2(int synctype);
extern void dsd_dispatch_handle_p25p2(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_dpmr(int synctype);
extern void dsd_dispatch_handle_dpmr(dsd_opts* opts, dsd_state* state);

extern int dsd_dispatch_matches_p25p1(int synctype);
extern void dsd_dispatch_handle_p25p1(dsd_opts* opts, dsd_state* state);

const dsd_protocol_handler dsd_protocol_handlers[] = {
    {"NXDN", dsd_dispatch_matches_nxdn, dsd_dispatch_handle_nxdn, NULL},
    {"D-STAR", dsd_dispatch_matches_dstar, dsd_dispatch_handle_dstar, NULL},
    {"DMR", dsd_dispatch_matches_dmr, dsd_dispatch_handle_dmr, NULL},
    {"X2-TDMA", dsd_dispatch_matches_x2tdma, dsd_dispatch_handle_x2tdma, NULL},
    {"ProVoice", dsd_dispatch_matches_provoice, dsd_dispatch_handle_provoice, NULL},
    {"EDACS", dsd_dispatch_matches_edacs, dsd_dispatch_handle_edacs, NULL},
    {"YSF", dsd_dispatch_matches_ysf, dsd_dispatch_handle_ysf, NULL},
    {"M17", dsd_dispatch_matches_m17, dsd_dispatch_handle_m17, NULL},
    {"P25P2", dsd_dispatch_matches_p25p2, dsd_dispatch_handle_p25p2, NULL},
    {"dPMR", dsd_dispatch_matches_dpmr, dsd_dispatch_handle_dpmr, NULL},
    {"P25P1", dsd_dispatch_matches_p25p1, dsd_dispatch_handle_p25p1, NULL},
    {0, 0, 0, 0},
};
