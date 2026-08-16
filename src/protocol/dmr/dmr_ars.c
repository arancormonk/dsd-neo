// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Motorola MOTOTRBO Automatic Registration Service (ARS) message decode.
 *
 * ARS messages are length prefixed: a two octet message size (excluding the size octets)
 * followed by a header octet carrying Ext/Ack/Pri/Cntl flags in bits 7-4 and a PDU type in
 * bits 3-0, an optional second header octet when the Ext flag is set, and a PDU-specific
 * payload. Registrations carry length-value fields (device id, user id, password).
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <stdio.h>
#include <string.h>
#include "dmr_ars.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define DMR_ARS_HDR_EXT        0x80U
#define DMR_ARS_TYPE_DEV_REG   0x00U
#define DMR_ARS_TYPE_DEV_DEREG 0x01U
#define DMR_ARS_TYPE_QUERY     0x04U
#define DMR_ARS_TYPE_USER_REG  0x05U
#define DMR_ARS_TYPE_REG_ACK   0x0FU

static void
dmr_ars_append(char* dst, size_t dstsz, const char* src) {
    if (!dst || !src || dstsz == 0) {
        return;
    }
    size_t len = strlen(dst);
    if (len >= dstsz) {
        return;
    }
    DSD_SNPRINTF(dst + len, dstsz - len, "%s", src);
}

// Registration payloads are length-value encoded and start with the device identifier:
// <id size> <id octets>. Returns 0 when the field does not fit the record or the identifier
// is not printable ASCII, so the caller can fall back to a raw dump instead of a bogus id.
static uint8_t
dmr_ars_lv_device_id(const uint8_t* rec, uint16_t rec_len, uint16_t pos, char* out, size_t out_sz) {
    if (pos >= rec_len) {
        return 0;
    }

    uint8_t id_len = rec[pos];
    if (id_len == 0 || (uint16_t)(pos + 1U + id_len) > rec_len || (size_t)id_len + 1U > out_sz) {
        return 0;
    }

    for (uint8_t i = 0; i < id_len; i++) {
        uint8_t c = rec[pos + 1U + i];
        if (c < 0x20U || c >= 0x7FU) {
            return 0;
        }
        out[i] = (char)c;
    }
    out[id_len] = '\0';
    return 1;
}

// Print an ARS message. `msg` points at the two octet size prefix; `len` is the number of
// received octets available from there. The size prefix bounds every read, so a dump can
// never run past the record into block trailer or padding bytes.
void
dmr_ars_print_message(dsd_state* state, const uint8_t* msg, uint16_t len) {
    uint8_t slot = state->currentslot & 1;
    char summary[64];
    char dev_id[32];

    if (msg == NULL || len < 3U) {
        return;
    }

    uint16_t rec_len = (uint16_t)(((uint16_t)msg[0] << 8) | msg[1]);
    uint16_t avail = (uint16_t)(len - 2U);
    if (rec_len > avail) {
        rec_len = avail;
    }
    if (rec_len == 0U) {
        return;
    }

    const uint8_t* rec = msg + 2;
    uint8_t hdr = rec[0];
    uint8_t pdu_type = hdr & 0x0FU;
    // The second header octet (encoding or refresh timer) is only present with the Ext flag.
    uint16_t payload_pos = (hdr & DMR_ARS_HDR_EXT) ? 2U : 1U;

    summary[0] = '\0';
    if ((pdu_type == DMR_ARS_TYPE_DEV_REG || pdu_type == DMR_ARS_TYPE_USER_REG)
        && dmr_ars_lv_device_id(rec, rec_len, payload_pos, dev_id, sizeof(dev_id))) {
        DSD_SNPRINTF(summary, sizeof(summary), "ARS %sReg: %s; ", pdu_type == DMR_ARS_TYPE_USER_REG ? "User " : "",
                     dev_id);
    } else if (pdu_type == DMR_ARS_TYPE_DEV_DEREG) {
        DSD_SNPRINTF(summary, sizeof(summary), "ARS Dereg; ");
    } else if (pdu_type == DMR_ARS_TYPE_QUERY) {
        DSD_SNPRINTF(summary, sizeof(summary), "ARS Query; ");
    } else if (pdu_type == DMR_ARS_TYPE_REG_ACK) {
        DSD_SNPRINTF(summary, sizeof(summary), "ARS Ack; ");
    }

    if (summary[0] != '\0') {
        DSD_FPRINTF(stderr, "\n %s", summary);
        dmr_ars_append(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), summary);
        return;
    }

    utf8_to_text(state, 0, rec_len, rec);
}
