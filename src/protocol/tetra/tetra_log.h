// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA decoder diagnostics gates.
 *
 * Decoded signalling summaries print by default, one line per CRC-passed PDU, like the other protocols. Per-burst
 * detail (sync hypotheses, FEC counters, raw bits) prints only with -Z; state-machine narration only with -v.
 * The macros take a tetra_state_t pointer (tetra_state.h), which the calling file includes.
 */
#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_LOG_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_LOG_H_

#include <dsd-neo/core/safe_api.h>
#include <stdio.h>

/** Print per-burst detail when -Z is on. @p ts may be NULL (prints nothing). */
#define TETRA_LOG_PAYLOAD(ts, ...)                                                                                     \
    do {                                                                                                               \
        if ((ts) != NULL && (ts)->log_payload) {                                                                       \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
        }                                                                                                              \
    } while (0)

/** Print state-machine narration at -v @p level or above. @p ts may be NULL (prints nothing). */
#define TETRA_LOG_VERBOSE(ts, level, ...)                                                                              \
    do {                                                                                                               \
        if ((ts) != NULL && (ts)->log_verbose >= (level)) {                                                            \
            DSD_FPRINTF(stderr, __VA_ARGS__);                                                                          \
        }                                                                                                              \
    } while (0)

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_LOG_H_ */
