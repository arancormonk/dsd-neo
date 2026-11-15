// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief The unit activity reason tables: the P25 tables read exactly as the P25 prints' own table did before the
 * prints moved to the shared one (so the move changed no output), and the DMR, NXDN and TETRA tables name only their
 * standards' values.
 */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/unit_activity_reason.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* The P25 prints' own table (p25_response_reason.c) as it was when the prints moved to dsd_ua_reason_text(): every
 * code must still read the same. */
static const char*
print_queued_reason(uint8_t code) {
    switch (code) {
        case 0x10: return "Requesting Unit Busy Other Service";
        case 0x20: return "Target Unit Busy Other Service";
        case 0x2F: return "Target Unit Queued This Call";
        case 0x30: return "Target Group Currently Active";
        case 0x40: return "Channel Resources Unavailable";
        case 0x41: return "Telephone Resources Unavailable";
        case 0x42: return "Data Resources Unavailable";
        case 0x50: return "Superseding Service Currently Active";
        default: break;
    }
    return code <= 0x7F ? "Reserved" : "User/System Defined";
}

static const char*
print_deny_reason(uint8_t code) {
    static const struct {
        uint8_t code;
        const char* text;
    } k_deny[] = {
        {0x10, "Requesting Unit Not Valid"},
        {0x11, "Requesting Unit Not Authorized"},
        {0x20, "Target Unit Not Valid"},
        {0x21, "Target Unit Not Authorized"},
        {0x2F, "Target Unit Refused Call"},
        {0x30, "Target Group Not Valid"},
        {0x31, "Target Group Not Authorized"},
        {0x40, "Invalid Dialing"},
        {0x41, "Telephone Number Not Authorized"},
        {0x42, "PSTN Not Valid"},
        {0x50, "Call Timeout"},
        {0x51, "Landline Terminated Call"},
        {0x52, "Subscriber Unit Terminated Call"},
        {0x5F, "Call Preempted"},
        {0x60, "Site Access Denial"},
        {0x67, "PTT Collide"},
        {0x77, "PTT Bonk"},
        {0xF0, "Call Options Not Valid For Service"},
        {0xF1, "Protection Service Option Not Valid"},
        {0xF2, "Duplex Service Option Not Valid"},
        {0xF3, "Circuit/Packet Mode Option Not Valid"},
        {0xFF, "System Does Not Support Service"},
    };

    for (size_t i = 0; i < sizeof(k_deny) / sizeof(k_deny[0]); i++) {
        if (k_deny[i].code == code) {
            return k_deny[i].text;
        }
    }
    return code <= 0x5E ? "Reserved" : "User/System Defined";
}

static void
expect_text(uint8_t set, uint16_t code, const char* want) {
    const char* got = dsd_ua_reason_text(set, code);
    if (!got || strcmp(got, want) != 0) {
        DSD_FPRINTF(stderr, "set %u code 0x%X: '%s', want '%s'\n", (unsigned)set, (unsigned)code, got ? got : "(null)",
                    want);
        assert(0);
    }
}

/* Every 8-bit P25 code reads as the prints' own table read it. */
static void
test_p25_matches_the_print(void) {
    for (unsigned code = 0; code <= 0xFFU; code++) {
        expect_text(DSD_UA_REASON_P25_DENY, (uint16_t)code, print_deny_reason((uint8_t)code));
        expect_text(DSD_UA_REASON_P25_QUEUED, (uint16_t)code, print_queued_reason((uint8_t)code));
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
test_tetra_accept_type(void) {
    static const char* const want[8] = {
        "Roaming location updating",
        "Temporary registration",
        "Periodic location updating",
        "ITSI attach",
        "Service restoration roaming location updating",
        "Migrating or service restoration migrating location updating",
        "Demand location updating",
        "Disabled MS updating",
    };
    for (uint32_t t = 0; t < 8U; t++) {
        const char* got = dsd_ua_tetra_accept_type_text(t);
        assert(got && strcmp(got, want[t]) == 0);
    }
    assert(dsd_ua_tetra_accept_type_text(8U) == NULL);
    assert(dsd_ua_tetra_accept_type_text(13U) == NULL);
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
    test_tetra_accept_type();
    test_no_table();
    printf("CORE_UNIT_ACTIVITY_REASON: OK\n");
    return 0;
}
