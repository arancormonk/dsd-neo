// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief P25 encryption LFSR helper interfaces.
 *
 * Declares the P25-specific LFSR helpers implemented in
 * `src/protocol/p25/p25_lfsr.c` and `src/protocol/p25/p25_lfsr_mi.c`.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_LFSR_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_LFSR_H_H

#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

void LFSRP(dsd_state* state);
void LFSR128(dsd_state* state);
void p25_lfsr128_slot(dsd_state* state, int slot);
/** The MI of the next superframe: @p mi advanced 64 steps by the P25 MI LFSR (what LFSRP() applies). */
unsigned long long p25_lfsr_next_mi(unsigned long long mi);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_LFSR_H_H */
