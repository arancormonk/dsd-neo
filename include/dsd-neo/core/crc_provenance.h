// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief CRC provenance bits carried by decoded content, calls and history rows.
 *
 * `dsd_state::event_crc_invalid`, `dsd_state::data_header_crc_invalid`, `dsd_call_snapshot::crc_invalid` and
 * `Event_History::crc_invalid` hold these bits. Zero means no check is known to have failed or been bypassed; it is
 * not a verification guarantee for every protocol. Any non-zero value means the content is not verified, which is
 * what every gate tests (location files, strict-mode publication). Only renderers tell the bits apart, and a failure
 * outranks RAS: a transmission that carries both renders as a failure.
 *
 * The bits accumulate. A writer ORs its bits in and never clears another writer's, so a failure later in a RAS
 * transmission escalates it rather than replacing it.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CRC_PROVENANCE_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CRC_PROVENANCE_H_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * The values are pinned: P25 and DMR scopes OR a 0/1 failure verdict straight into these fields, so the failure bit
 * must stay 1.
 */
enum {
    /** A CRC or checksum the protocol defines failed. Rendered `[CRC ERR]`, with warning severity. */
    DSD_CRC_FAILED = 0x01U,
    /**
     * The standard CRC could not be verified because DMR's Motorola RAS heuristic accepted the burst under `-F`. RAS
     * systems alter the check on purpose, so this is not evidence of corruption, but nothing vouches for the content
     * either. Rendered `[RAS]`, with the row's ordinary severity.
     */
    DSD_CRC_RAS_UNVERIFIED = 0x02U,
};

/** The history and `-J` marker for a set of provenance bits, including its trailing space; "" when there is none. */
static inline const char*
dsd_crc_marker(uint8_t flags) {
    if ((flags & DSD_CRC_FAILED) != 0U) {
        return "[CRC ERR] ";
    }
    if ((flags & DSD_CRC_RAS_UNVERIFIED) != 0U) {
        return "[RAS] ";
    }
    return "";
}

/** Whether the bits record a failed check, which outranks RAS and is the only bit that raises severity. */
static inline int
dsd_crc_is_failed(uint8_t flags) {
    return (flags & DSD_CRC_FAILED) != 0U;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CRC_PROVENANCE_H_H */
