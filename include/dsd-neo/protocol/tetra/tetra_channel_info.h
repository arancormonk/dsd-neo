// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA channel-info summary formatter.  Phase 32.
 *
 * Provides a single helper that renders the full current-channel state
 * (network identity + current carrier + VC assignment + enc mode) into
 * a compact one-line display string.
 */
#ifndef DSD_NEO_PROTOCOL_TETRA_CHANNEL_INFO_H
#define DSD_NEO_PROTOCOL_TETRA_CHANNEL_INFO_H

#include <stddef.h>
#include <dsd-neo/core/state.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * tetra_channel_info_fmt()
 *
 * Render a compact channel-summary string from the TETRA fields in @state
 * into @buf (NUL-terminated, at most @len bytes including the terminator).
 *
 * Output format (example):
 *   "MCC:234 MNC:7 CC:42 DL:390.200MHz TG:1234 SRC:5678 enc:none"
 *
 * If @state is NULL writes "?".
 */
void tetra_channel_info_fmt(const dsd_state *state, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_PROTOCOL_TETRA_CHANNEL_INFO_H */
