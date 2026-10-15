// SPDX-License-Identifier: GPL-3.0-or-later
/* Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com> */

#include <assert.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "test_support.h"

static void
check_announcement(dsd_opts* opts, dsd_state* state, uint32_t address, const char* expected, uint32_t crc_ok,
                   uint32_t errors) {
    /* ETSI TS 102 361-4 V1.11.1, Table 7.76: Reg_Window=B, Aloha Mask=15, and all 24 bits of BParms2 are the address.
       The zero tail supplies the unused MBC continuation read by the shared C_BCAST parser. */
    uint8_t bytes[24] = {0xA8, 0x00, 0x20, 0x2E, 0xA0};
    uint8_t bits[sizeof(bytes) * 8];
    bytes[7] = (uint8_t)(address >> 16);
    bytes[8] = (uint8_t)(address >> 8);
    bytes[9] = (uint8_t)address;
    for (size_t i = 0; i < sizeof(bits); ++i) {
        bits[i] = (uint8_t)((bytes[i / 8] >> (7 - i % 8)) & 1U);
    }
    dsd_test_capture_stderr capture;
    char output[4096];
    assert(dsd_test_capture_stderr_begin(&capture, "dsd_dmr_mass_reg") == 0);
    dmr_cspdu(opts, state, bits, bytes, crc_ok, errors);
    assert(dsd_test_capture_stderr_end(&capture) == 0);
    assert(dsd_test_capture_stderr_read(&capture, output, sizeof(output)) == 0);
    if (crc_ok && errors == 0) {
        assert(strstr(output, "Mass Registration (MassReg)") != NULL);
        assert(strstr(output, "Reg Window: B; Aloha Mask: 15;") != NULL);
        assert(strstr(output, expected) != NULL);
        if (address == 0xFFFFFFU) {
            assert(strstr(output, "ALLMSID;") != NULL);
        }
    } else {
        assert(strstr(output, "Mass Registration (MassReg)") == NULL);
        assert(strstr(output, "Target:") == NULL);
    }
}

int
main(void) {
    dsd_opts* opts = calloc(1, sizeof(*opts));
    dsd_state* state = calloc(1, sizeof(*state));
    assert(opts && state);

    static const struct {
        uint32_t address;
        const char* expected;
    } cases[] = {{0U, "Target: 0;"},
                 {0xFFU, "Target: 255;"},
                 {0x100U, "Target: 256;"},
                 {0x10000U, "Target: 65536;"},
                 {0x123456U, "Target: 1193046;"},
                 {0xABCDEFU, "Target: 11259375;"},
                 {0xFFFFFFU, "Target: 16777215;"}};

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        check_announcement(opts, state, cases[i].address, cases[i].expected, 1, 0);
    }
    for (int relaxed = 0; relaxed <= 1; ++relaxed) {
        opts->dmr_crc_relaxed_default = relaxed;
        check_announcement(opts, state, 0x123456U, NULL, 0, 0);
        check_announcement(opts, state, 0x123456U, NULL, 1, 1);
    }
    dsd_state_ext_free_all(state);
    free(state);
    free(opts);
    return 0;
}
