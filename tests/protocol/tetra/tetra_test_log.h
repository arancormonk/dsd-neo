// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief What the TETRA decoder writes to stderr while a test decodes a TM-SDU or a MAC block: one summary line per
 * decoded signalling PDU, with the elements it acts on. Tests read it for the elements of a PDU that reach no call
 * record or state.
 */
#ifndef DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_LOG_H_
#define DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_LOG_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <stddef.h>
#include <stdint.h>

#include "test_support.h"

/** Decode the TM-SDU @p bits with tetra_mle_dispatch() and keep in @p log (always terminated) what the decoder wrote to
 * stderr meanwhile. */
static inline void
tetra_test_mle_logged(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state, char* log,
                      size_t log_size) {
    dsd_test_capture_stderr cap;
    const int captured = dsd_test_capture_stderr_begin(&cap, "tetra_log") == 0;
    tetra_mle_dispatch(bits, nbits, cc, opts, state);
    log[0] = '\0';
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, log_size);
    }
}

/** Decode the signalling MAC block @p bits with tetra_mac_parse_schd() and keep in @p log (always terminated) what the
 * decoder wrote to stderr meanwhile. */
static inline void
tetra_test_mac_logged(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state, char* log,
                      size_t log_size) {
    dsd_test_capture_stderr cap;
    const int captured = dsd_test_capture_stderr_begin(&cap, "tetra_log") == 0;
    tetra_mac_parse_schd(bits, nbits, cc, opts, state);
    log[0] = '\0';
    if (captured) {
        (void)dsd_test_capture_stderr_end(&cap);
        (void)dsd_test_capture_stderr_read(&cap, log, log_size);
    }
}

#endif /* DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_LOG_H_ */
