// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include "p25_response_reason.h"

#include <stddef.h>

const char*
p25_queued_response_reason(uint8_t code) {
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

    if (code <= 0x7F) {
        return "Reserved";
    }
    return "User/System Defined";
}

const char*
p25_deny_response_reason(uint8_t code) {
    static const struct {
        uint8_t code;
        const char* text;
    } k_deny_reasons[] = {
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

    for (size_t i = 0; i < sizeof(k_deny_reasons) / sizeof(k_deny_reasons[0]); i++) {
        if (k_deny_reasons[i].code == code) {
            return k_deny_reasons[i].text;
        }
    }

    if (code <= 0x5E) {
        return "Reserved";
    }
    return "User/System Defined";
}
