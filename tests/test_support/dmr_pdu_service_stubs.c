// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * No-op UDP service decoders the DMR PDU decoder (dmr_pdu.c) dispatches to, for tests that compile it without the
 * core GPS decoder: LIP (UDP 5017) and Cellocator (UDP 231). Tests that define their own take theirs instead; this
 * archive member is pulled only when neither is defined.
 */

#include <dsd-neo/core/gps.h>
#include <dsd-neo/protocol/pdu.h>
#include <stddef.h>
#include <stdint.h>
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state_fwd.h"

void
lip_pdu_decoder(const dsd_opts* opts, dsd_state* state, const uint8_t* bits, size_t bit_count,
                const dsd_gps_address* src) {
    (void)opts;
    (void)state;
    (void)bits;
    (void)bit_count;
    (void)src;
}

void
decode_cellocator(dsd_opts* opts, dsd_state* state, const uint8_t* input, int len) {
    (void)opts;
    (void)state;
    (void)input;
    (void)len;
}
