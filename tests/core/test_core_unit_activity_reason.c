// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The unit activity reason tables: the P25 tables read exactly as the P25 prints' own (so switching a print to
 * the shared table changes no output), and the DMR, NXDN and TETRA tables name only their standards' values.
 */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "p25_response_reason.h"

static void
expect_text(uint8_t set, uint16_t code, const char* want) {
    const char* got = dsd_ua_reason_text(set, code);
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "set %u code 0x%X: '%s', want '%s'\n", (unsigned)set, (unsigned)code, got ? got : "(null)",
                    want);
        assert(0);
    }
}

/* Every 8-bit P25 code reads as p25_deny_response_reason() / p25_queued_response_reason() print it. */
static void
test_p25_matches_the_print(void) {
    for (unsigned code = 0; code <= 0xFFU; code++) {
        expect_text(DSD_UA_REASON_P25_DENY, (uint16_t)code, p25_deny_response_reason((uint8_t)code));
        expect_text(DSD_UA_REASON_P25_QUEUED, (uint16_t)code, p25_queued_response_reason((uint8_t)code));
    }
    assert(dsd_ua_reason_text(DSD_UA_REASON_P25_DENY, 0x100U) == NULL);
}

static void
test_dmr(void) {
    expect_text(DSD_UA_REASON_DMR_TIER3, 0x62U, "Registration accepted");
    expect_text(DSD_UA_REASON_DMR_TIER3, 0x2AU, "Registration refused");
    expect_text(DSD_UA_REASON_DMR_TIER3, 0x2BU, "Registration denied");
    expect_text(DSD_UA_REASON_DMR_TIER3, 0x21U, "Not authorised for this user (permanent)");
    expect_text(DSD_UA_REASON_DMR_TIER3, 0x16U, "Radio does not support duplex");
    expect_text(DSD_UA_REASON_DMR_TIER3, 0xA1U, "Queued, called party busy");
    expect_text(DSD_UA_REASON_DMR_TIER3, 0xE0U, "Wait for further signalling");
    assert(dsd_ua_reason_text(DSD_UA_REASON_DMR_TIER3, 0x3AU) == NULL);
    assert(dsd_ua_reason_text(DSD_UA_REASON_DMR_TIER3, 0xE1U) == NULL);
    /* Tier II's 0x21 is not Tier III's (TS 102 361-2 Table 7.13). */
    expect_text(DSD_UA_REASON_DMR_TIER2_NACK, 0x21U, "Radio does not support this service or feature");
    assert(dsd_ua_reason_text(DSD_UA_REASON_DMR_TIER2_NACK, 0x20U) == NULL);
}

static void
test_nxdn(void) {
    expect_text(DSD_UA_REASON_NXDN_MM, 0x01U, "Registration is accepted");
    expect_text(DSD_UA_REASON_NXDN_MM, 0x05U, "Location is accepted, but Group is refused");
    expect_text(DSD_UA_REASON_NXDN_MM, 0x08U, "Registration is refused");
    expect_text(DSD_UA_REASON_NXDN_MM, 0x7FU, "Other class for procedure error");
    assert(dsd_ua_reason_text(DSD_UA_REASON_NXDN_MM, 0x21U) == NULL); /* single-source "ESN valid" values */
    assert(dsd_ua_reason_text(DSD_UA_REASON_NXDN_MM, 0x6CU) == NULL); /* Type-D only */
    expect_text(DSD_UA_REASON_NXDN_SS, 0x01U, "ACK (Receive success)");
    expect_text(DSD_UA_REASON_NXDN_SS, 0x15U, "No response from the called SU");
    expect_text(DSD_UA_REASON_NXDN_SS, 0x61U, "Request for unsupported service");
    assert(dsd_ua_reason_text(DSD_UA_REASON_NXDN_SS, 0x1EU) == NULL); /* a Cause (VD) value only */
    assert(dsd_ua_reason_text(DSD_UA_REASON_NXDN_SS, 0x30U) == NULL);
}

static void
test_tetra(void) {
    expect_text(DSD_UA_REASON_TETRA_LU_REJECT, 1U, "ITSI/ATSI unknown");
    expect_text(DSD_UA_REASON_TETRA_LU_REJECT, 16U, "Identified cipher key not available");
    expect_text(DSD_UA_REASON_TETRA_LU_REJECT, 21U, "Use of DA cell not permitted");
    assert(dsd_ua_reason_text(DSD_UA_REASON_TETRA_LU_REJECT, 0U) == NULL);
    assert(dsd_ua_reason_text(DSD_UA_REASON_TETRA_LU_REJECT, 17U) == NULL);
    assert(dsd_ua_reason_text(DSD_UA_REASON_TETRA_LU_REJECT, 22U) == NULL);
}

static void
test_no_table(void) {
    assert(dsd_ua_reason_text(DSD_UA_REASON_NONE, 0x20U) == NULL);
    assert(dsd_ua_reason_text(99U, 0x20U) == NULL);
}

int
main(void) {
    test_p25_matches_the_print();
    test_dmr();
    test_nxdn();
    test_tetra();
    test_no_table();
    printf("CORE_UNIT_ACTIVITY_REASON: OK\n");
    return 0;
}
