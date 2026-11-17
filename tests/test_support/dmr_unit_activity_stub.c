// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * No-op DMR unit activity producers (src/protocol/dmr/dmr_unit_activity.h) for tests that compile the CSBK or data
 * decoders without the core ring. DMR_UNIT_ACTIVITY links the real ones.
 */

#include <stdint.h>
#include "dmr_unit_activity.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
dmr_ua_c_ackd(const dsd_opts* opts, dsd_state* state, const dmr_ua_ackd_fields* fields) {
    (void)opts;
    (void)state;
    (void)fields;
}

void
dmr_ua_c_ahoy(const dsd_opts* opts, dsd_state* state, int kind, uint8_t group, uint32_t target, uint32_t source) {
    (void)opts;
    (void)state;
    (void)kind;
    (void)group;
    (void)target;
    (void)source;
}

void
dmr_ua_ars(const dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t pdu_type, uint32_t source) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)pdu_type;
    (void)source;
}
