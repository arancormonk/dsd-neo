// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_ADDRESS_LAYOUT_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_ADDRESS_LAYOUT_H_

#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

/*
 * The address layout later LCs, data headers and preamble CSBKs are read with: Capacity Plus and Hytera XPT carry
 * 16-bit addresses where the standard layout has 24-bit ones. The one source is the carrier's current trunking system
 * as the received identity names it (dsd_rxid_dmr_layout_trunk()): only content that passed its own check notes a part
 * there, it counts while fresh (the no-carrier pass ends it until fresh evidence), a sync of another protocol ends it
 * with the record it drops, a carrier boundary resets it, and a checked note of another trunking system replaces it.
 * The branding strings (dsd_state::dmr_branding, dmr_branding_sub) are display text and select nothing.
 */
typedef enum {
    DMR_ADDRESS_LAYOUT_STANDARD = 0,
    DMR_ADDRESS_LAYOUT_CAP_PLUS = 1,
    DMR_ADDRESS_LAYOUT_XPT = 2,
} dmr_address_layout;

static inline dmr_address_layout
dmr_address_layout_of(const dsd_state* state) {
    const uint8_t trunk = dsd_rxid_dmr_layout_trunk(state);
    if (trunk == (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) {
        return DMR_ADDRESS_LAYOUT_CAP_PLUS;
    }
    if (trunk == (uint8_t)DSD_RXID_DMR_TRUNK_XPT) {
        return DMR_ADDRESS_LAYOUT_XPT;
    }
    return DMR_ADDRESS_LAYOUT_STANDARD;
}

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_ADDRESS_LAYOUT_H_ */
