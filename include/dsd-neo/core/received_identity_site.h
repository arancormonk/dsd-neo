// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The site a call was heard on: a received identity's site parts, compared and merged part by part.
 *
 * A call's history row keeps the system and site of the carrier it was heard on as a site identity: a
 * dsd_received_identity (<dsd-neo/core/received_identity.h>) holding only the parts that name the system and site.
 * What changes with the moment or the carrier (the access code, the Capacity Plus rest channel, the XPT free channel,
 * RAS, the vendor ID, the control-channel frequency and the EDACS control-channel LCN) is left out, with the epoch and
 * the fresh bits, so two rows of the same site hold the same bytes.
 *
 * Two site identities are compatible when no part both know differs: the families when both are known, and per part
 * the values (P25 WACN, SYSID, RFSS and site together, LRA; the DMR trunking system, Tier III system code, raw network,
 * site and Tier III model; the NXDN location ID, IDAS area, IDAS site ID and Type-D system ID; the EDACS site, area,
 * system and addressing mode; the TETRA network with its colour code, location area). The record's cross-part rules
 * hold across the two sides as well: an IDAS site ID's site code against a Type-D system ID's, and a Type-C location
 * against any IDAS part. DMR site text and brand are presentation (the text follows -D) and are never compared.
 *
 * No decoder state: the module needs libc only, so the Qt history store builds it as it is.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_RECEIVED_IDENTITY_SITE_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_RECEIVED_IDENTITY_SITE_H_

#include <dsd-neo/core/received_identity.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Writes to @p out the site parts of @p in (NULL: none) of @p in's own family, in zeroed storage: every other value,
 * the epoch and the fresh bits are 0, and each text is copied up to its NUL (cut to its field when it has none) with
 * zero past it. The qualifiers stay only with a part they qualify: dmr_trunk with a kept DMR trunking part (site text,
 * brand, system code, network or site), dmr_t3_model with the site, edacs_ea_mode with an EDACS site, area or system,
 * tetra_colour with the network. With no part left the family is NONE. @p out may be @p in. */
void dsd_rxid_site_snapshot(const dsd_received_identity* in, dsd_received_identity* out);

/** 1 when @p a (NULL counts) holds no site part. */
int dsd_rxid_site_empty(const dsd_received_identity* a);

/** 1 when @p a and @p b (either NULL or empty: compatible) can be the same site: see the file comment. Symmetric. */
int dsd_rxid_site_compatible(const dsd_received_identity* a, const dsd_received_identity* b);

/** Writes to @p out the site parts both know, as dsd_rxid_site_snapshot() lays them out. Precondition:
 * dsd_rxid_site_compatible(a, b); otherwise @p out is empty, never one side's pick. Order-free: where both carry text
 * for the same raw parts (DMR site text, brand), the strcmp-smaller one is kept, so union(a, b) and union(b, a) are the
 * same bytes. @p out may be @p a or @p b. */
void dsd_rxid_site_union(const dsd_received_identity* a, const dsd_received_identity* b, dsd_received_identity* out);

/** 1 when @p a, a stored site identity, is one a record could hold: family 0..7; valid bits only site parts of its
 * family; the cross-part rules (P25 WACN, site and LRA with a SYSID; DMR network with a site, numbers with a trunking
 * system, a system code only for Tier III; no Type-C location beside an IDAS part; an IDAS site code equal to a Type-D
 * system ID's; a TETRA location area with a network); value ranges (P25 WACN <= 0xFFFFF, SYSID 1..0xFFF; DMR trunk
 * <= XPT, model <= 3; the NXDN location by dsd_nxdn_location_valid(), IDAS area 0..1, site type 1..3 and code 1..250,
 * Type-D system <= 0x7FFFF; TETRA MCC <= 1023, MNC and LA <= 16383, colour <= 63); both texts NUL-terminated in their
 * fields. NULL is not. */
int dsd_rxid_site_consistent(const dsd_received_identity* a);

/** 1 when an NXDN location ID names a site (NXDN TS 1-A 6.5.2 Table 6.5-1, which reserves the all-zero and all-ones
 * codes): @p category in the record's encoding (dsd_received_identity::nxdn_category: 0 Global, 1 Regional, 2 Local;
 * the location ID's category bits are 00, 10 and 01), Global system 1..1022 and site 1..4094, Regional 1..16382 and
 * 1..254, Local 1..131070 and 1..30. Category 3 is reserved. */
int dsd_nxdn_location_valid(uint8_t category, uint32_t sys, uint16_t site);

/** The row-level check two call fragments must pass to be one call: the sites are compatible
 * (dsd_rxid_site_compatible()), the channel kinds (dsd_call_channel_kind, <dsd-neo/core/call_channel.h>) are equal when
 * both are known, and each known kind agrees with a known DMR trunking system on either side (LSN: Capacity Plus or
 * XPT; LPCN: Tier III; LCN: Connect Plus), so a channel-only Tier III fragment never joins a Connect Plus row. A known
 * kind beside a site of another family than DMR, or a kind value outside the enum, is incompatible. */
int dsd_call_site_compatible(const dsd_received_identity* site_a, uint8_t kind_a, const dsd_received_identity* site_b,
                             uint8_t kind_b);

/** dsd_call_site_compatible() for two whole records (dsd_state::rx_id, dsd_call_snapshot::ended_rx_id; NULL counts as
 * empty) rather than site identities: each record's DMR trunking system (dmr_trunk while any of its DMR trunking parts
 * is valid, the Capacity Plus rest LSN and the XPT free LCN that a site snapshot drops included) must also equal the
 * other's when both are known, and fit both channel kinds. */
int dsd_call_record_site_compatible(const dsd_received_identity* record_a, uint8_t kind_a,
                                    const dsd_received_identity* record_b, uint8_t kind_b);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_RECEIVED_IDENTITY_SITE_H_ */
