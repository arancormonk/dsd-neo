// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_FEC_DMR_LATE_ENTRY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_FEC_DMR_LATE_ENTRY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t mi;
    bool crc_ok;
} dsd_dmr_late_entry_result;

/** Compute the inverted x^4+x+1 CRC used by DMR late-entry messages. */
uint8_t dsd_dmr_crc4(const uint8_t* bits, unsigned int len);
/** Decode the Golay-protected DMR late-entry MI fragments for one slot. */
bool dsd_dmr_late_entry_decode(const uint64_t* fragments, dsd_dmr_late_entry_result* result);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_FEC_DMR_LATE_ENTRY_H_ */
