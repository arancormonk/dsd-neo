// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA BSCH display formatter.  Phase 31.
 *
 * Provides a single helper that formats the current network identity
 * (MCC, MNC, colour code) into a human-readable presentation string.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_BSCH_FMT_H
#define DSD_NEO_PROTOCOL_TETRA_BSCH_FMT_H

#include <stddef.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * tetra_bsch_fmt_net()
 *
 * Format the TETRA network identity cached in @state into human-readable
 * text stored in @buf (NUL-terminated, at most @len bytes including the
 * terminator).
 *
 * Output format: "MCC:NNN MNC:NNNN CC:NN"
 *
 * If tetra_net_known == 0 the string "MCC:? MNC:? CC:?" is written.
 *
 * @state  – pointer to dsd_state (may be NULL → writes "?")
 * @buf    – output buffer
 * @len    – size of @buf in bytes (must be > 0)
 */
void tetra_bsch_fmt_net(const dsd_state *state, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_BSCH_FMT_H */
