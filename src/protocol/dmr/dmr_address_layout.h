// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_PROTOCOL_DMR_DMR_ADDRESS_LAYOUT_H_
#define DSD_NEO_SRC_PROTOCOL_DMR_DMR_ADDRESS_LAYOUT_H_

#include <dsd-neo/core/received_identity.h>
#include <dsd-neo/core/state.h>

/*
 * The address layout later LCs, data headers and preamble CSBKs are read with: Capacity Plus and Hytera XPT carry
 * 16-bit addresses where the standard layout has 24-bit ones. The one source is the carrier's current trunking system
 * in the received identity record (dsd_state::rx_id): only content that passed its own check notes a part there, a
 * carrier boundary resets it, and a checked note of another trunking system replaces it. The branding strings
 * (dsd_state::dmr_branding, dmr_branding_sub) are display text and select nothing.
 */
typedef enum {
    DMR_ADDRESS_LAYOUT_STANDARD = 0,
    DMR_ADDRESS_LAYOUT_CAP_PLUS = 1,
    DMR_ADDRESS_LAYOUT_XPT = 2,
} dmr_address_layout;

/* The trunking parts a checked message of that system notes (the Capacity Max marker, which a RAS-accepted C_ALOHA may
   set, and the Tier III system code are no Cap+ or XPT evidence). */
#define DMR_ADDRESS_LAYOUT_PARTS                                                                                       \
    (DSD_RXID_DMR_REST_LSN | DSD_RXID_DMR_SITE_TEXT | DSD_RXID_DMR_NET | DSD_RXID_DMR_SITE | DSD_RXID_DMR_BRAND        \
     | DSD_RXID_DMR_XPT_FREE_LCN)

static inline dmr_address_layout
dmr_address_layout_of(const dsd_state* state) {
    const dsd_received_identity* const id = &state->rx_id;
    if (id->family != (uint8_t)DSD_RXID_FAMILY_DMR || (id->valid & DMR_ADDRESS_LAYOUT_PARTS) == 0U) {
        return DMR_ADDRESS_LAYOUT_STANDARD;
    }
    if (id->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_CAPACITY_PLUS) {
        return DMR_ADDRESS_LAYOUT_CAP_PLUS;
    }
    if (id->dmr_trunk == (uint8_t)DSD_RXID_DMR_TRUNK_XPT) {
        return DMR_ADDRESS_LAYOUT_XPT;
    }
    return DMR_ADDRESS_LAYOUT_STANDARD;
}

#endif /* DSD_NEO_SRC_PROTOCOL_DMR_DMR_ADDRESS_LAYOUT_H_ */
