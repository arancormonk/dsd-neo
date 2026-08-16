// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Motorola MOTOTRBO Automatic Registration Service (ARS) message decode.
 *
 * Wire format follows the MOTOTRBO Application Developer Program "Development Specification -
 * Automatic Registration Service" v02.00. A message is a two octet size prefix (excluding the
 * size octets themselves) followed by a first header octet, zero or more extension header
 * octets, and a PDU specific payload:
 *
 *   1st header: bit 7 Ext, bit 6 Ack, bit 5 Pri, bit 4 Cntl, bits 3-0 PDU type (spec 3.2).
 *   Each header octet's Ext bit says another header octet follows.
 *
 * On an acknowledgement PDU the Ack bit is the verdict - clear is success, set is failure - and
 * the optional second header carries a refresh/session timer on success or a reason code on
 * failure. Registrations carry a length-value name field: device id, user id, password, each
 * preceded by a one octet size that may be zero when the field is unused (spec 3.4.1, 3.4.7).
 */

#include <dsd-neo/core/state.h>
#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/protocol/dmr/dmr_utf8_text.h>
#include <stdio.h>
#include "dmr_ars.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#define DMR_ARS_HDR_EXT         0x80U
#define DMR_ARS_HDR_ACK         0x40U

#define DMR_ARS_TYPE_DEV_REG    0x00U
#define DMR_ARS_TYPE_DEV_DEREG  0x01U
#define DMR_ARS_TYPE_QUERY      0x04U
#define DMR_ARS_TYPE_USER_REG   0x05U
#define DMR_ARS_TYPE_USER_DEREG 0x06U
#define DMR_ARS_TYPE_USER_ACK   0x07U
#define DMR_ARS_TYPE_REG_ACK    0x0FU

// Upper bound on the raw fallback dump. The record length already keeps the dump inside the
// received bytes, but an ARS record can be as long as a whole reassembled data PDU and
// utf8_to_text emits one unbuffered stderr write per byte; the two call sites this replaced
// capped the dump at 10 and 15 bytes, so keep a diagnostic-sized ceiling.
#define DMR_ARS_MAX_DUMP        64U

// Longest name field rendered. Device and user identifiers are short strings (the spec's own
// examples are a radio id and a nine digit user id); anything longer is not worth a log line.
#define DMR_ARS_ID_MAX          32U

typedef enum {
    DMR_ARS_LV_ERR = -1,  // field does not fit the record; stop walking
    DMR_ARS_LV_EMPTY = 0, // zero length or unrenderable, `*pos` still advanced
    DMR_ARS_LV_OK = 1     // `out` holds the decoded field
} dmr_ars_lv_result;

// Reads one length-value name field at `*pos` and advances `*pos` past it. A zero length is legal
// for any of the three fields, and a field that is not printable text is skipped rather than
// abandoned, so that one binary field cannot hide the ones after it.
static dmr_ars_lv_result
dmr_ars_lv_next(const uint8_t* rec, uint16_t rec_len, uint16_t* pos, char* out, size_t out_sz) {
    out[0] = '\0';
    if (*pos >= rec_len) {
        return DMR_ARS_LV_ERR;
    }

    uint8_t id_len = rec[*pos];
    // Compare in the promoted type: narrowing the sum back to uint16_t would wrap it past the
    // bound this check exists to enforce as the walk reaches the later fields.
    if ((uint32_t)*pos + 1U + id_len > rec_len) {
        return DMR_ARS_LV_ERR;
    }

    uint16_t value_pos = (uint16_t)(*pos + 1U);
    *pos = (uint16_t)(value_pos + id_len);
    if (id_len == 0U || (size_t)id_len + 1U > out_sz) {
        return DMR_ARS_LV_EMPTY;
    }

    for (uint8_t i = 0; i < id_len; i++) {
        uint8_t c = rec[value_pos + i];
        if (c < 0x20U || c >= 0x7FU) {
            out[0] = '\0';
            return DMR_ARS_LV_EMPTY;
        }
        out[i] = (char)c;
    }
    out[id_len] = '\0';
    return DMR_ARS_LV_OK;
}

// Renders a device or user registration. The name field is device id, user id, password in that
// order; a user registration is about the user that signed in, so that identifier leads and the
// device it signed in from follows. Passwords are credentials, so only their presence is noted.
static void
dmr_ars_format_registration(const uint8_t* rec, uint16_t rec_len, uint16_t pos, uint8_t is_user, char* out,
                            size_t out_sz) {
    char dev_id[DMR_ARS_ID_MAX];
    char user_id[DMR_ARS_ID_MAX];
    char password[DMR_ARS_ID_MAX];
    uint8_t have_dev = (dmr_ars_lv_next(rec, rec_len, &pos, dev_id, sizeof(dev_id)) == DMR_ARS_LV_OK);
    uint8_t have_user = (dmr_ars_lv_next(rec, rec_len, &pos, user_id, sizeof(user_id)) == DMR_ARS_LV_OK);
    uint8_t have_pw = (dmr_ars_lv_next(rec, rec_len, &pos, password, sizeof(password)) == DMR_ARS_LV_OK);

    out[0] = '\0';
    if (!have_dev && !have_user) {
        // Nothing renderable in the name field: let the caller fall back to the raw dump, which
        // is more diagnostic than a bare label.
        return;
    }

    if (is_user) {
        DSD_SNPRINTF(out, out_sz, "ARS User Reg: %s; ", have_user ? user_id : "?");
        if (have_dev) {
            char dev_str[DMR_ARS_ID_MAX + 8U];
            DSD_SNPRINTF(dev_str, sizeof(dev_str), "DEV: %s; ", dev_id);
            dsd_strncat_s(out, out_sz, dev_str, sizeof(dev_str));
        }
    } else {
        DSD_SNPRINTF(out, out_sz, "ARS Reg: %s; ", have_dev ? dev_id : "?");
    }

    if (have_pw) {
        dsd_strncat_s(out, out_sz, "PW set; ", 8U);
    }
}

// Reason codes for a failed acknowledgement (spec 3.4.3 for the device ack, 3.4.9 for the user
// ack). Only these are defined; anything else is reported numerically.
static void
dmr_ars_fail_reason(uint8_t is_user, uint8_t code, char* out, size_t out_sz) {
    if (is_user && code == 0x01U) {
        DSD_SNPRINTF(out, out_sz, "%s", "user validation failed");
    } else if (is_user && code == 0x02U) {
        DSD_SNPRINTF(out, out_sz, "%s", "user validation timeout");
    } else if (is_user) {
        DSD_SNPRINTF(out, out_sz, "%s", "transmission failure");
    } else if (code == 0x00U) {
        DSD_SNPRINTF(out, out_sz, "%s", "device not authorized");
    } else {
        DSD_SNPRINTF(out, out_sz, "reason %02X", code);
    }
}

// Renders a registration acknowledgement. Bit 6 of the first header is the verdict, so a refused
// registration and a successful one differ by one bit and must not print the same line.
static void
dmr_ars_format_ack(const uint8_t* rec, uint16_t rec_len, uint8_t is_user, char* out, size_t out_sz) {
    const char* label = is_user ? "ARS User Ack" : "ARS Ack";
    uint8_t hdr = rec[0];
    // The timer/reason lives in the second header. Without it the spec pins the refresh timer to
    // zero (disabled) and the session to "until power cycle", which is what a 0 renders as.
    uint8_t have_ext = (uint8_t)(((hdr & DMR_ARS_HDR_EXT) != 0U) && rec_len > 1U);
    uint8_t value = have_ext ? (uint8_t)(rec[1] & 0x7FU) : 0U;

    if ((hdr & DMR_ARS_HDR_ACK) != 0U) {
        char reason[32];
        dmr_ars_fail_reason(is_user, value, reason, sizeof(reason));
        DSD_SNPRINTF(out, out_sz, "%s: FAIL - %s; ", label, reason);
    } else if (is_user) {
        if (value == 0U) {
            DSD_SNPRINTF(out, out_sz, "%s: OK; session until power cycle; ", label);
        } else {
            DSD_SNPRINTF(out, out_sz, "%s: OK; session %u; ", label, (unsigned)value);
        }
    } else if (value == 0U) {
        DSD_SNPRINTF(out, out_sz, "%s: OK; refresh off; ", label);
    } else {
        // One unit is 30 minutes over the 7 bit field, range 1-127: 30 minutes to ~2.5 days
        // (spec 3.4.2).
        DSD_SNPRINTF(out, out_sz, "%s: OK; refresh %u min; ", label, (unsigned)value * 30U);
    }
}

// Walks the header extension chain and returns the offset of the payload within the record.
static uint16_t
dmr_ars_payload_pos(const uint8_t* rec, uint16_t rec_len) {
    uint16_t pos = 1U;
    while ((uint16_t)(pos - 1U) < rec_len && (rec[pos - 1U] & DMR_ARS_HDR_EXT) != 0U) {
        pos++;
    }
    return pos;
}

static void
dmr_ars_format(const uint8_t* rec, uint16_t rec_len, char* out, size_t out_sz) {
    // The Cntl bit namespaces the type values, but ARS defines no user messages (spec 3.4), and a
    // decoder is better off reading a non-conformant transmitter than dropping it, so the type
    // nibble is taken at face value.
    uint8_t pdu_type = rec[0] & 0x0FU;
    uint16_t payload_pos = dmr_ars_payload_pos(rec, rec_len);

    out[0] = '\0';
    switch (pdu_type) {
        case DMR_ARS_TYPE_DEV_REG:
        case DMR_ARS_TYPE_USER_REG:
            dmr_ars_format_registration(rec, rec_len, payload_pos, (uint8_t)(pdu_type == DMR_ARS_TYPE_USER_REG), out,
                                        out_sz);
            break;
        case DMR_ARS_TYPE_DEV_DEREG: DSD_SNPRINTF(out, out_sz, "ARS Dereg; "); break;
        case DMR_ARS_TYPE_USER_DEREG: DSD_SNPRINTF(out, out_sz, "ARS User Dereg; "); break;
        case DMR_ARS_TYPE_QUERY: DSD_SNPRINTF(out, out_sz, "ARS Query; "); break;
        case DMR_ARS_TYPE_USER_ACK: dmr_ars_format_ack(rec, rec_len, 1U, out, out_sz); break;
        case DMR_ARS_TYPE_REG_ACK: dmr_ars_format_ack(rec, rec_len, 0U, out, out_sz); break;
        default: break;
    }
}

// Print an ARS message. `msg` points at the two octet size prefix; `len` is the number of
// received octets available from there. The size prefix bounds every read, so a dump can
// never run past the record into block trailer or padding bytes.
void
dmr_ars_print_message(dsd_state* state, const uint8_t* msg, uint16_t len) {
    char summary[128];

    if (state == NULL || msg == NULL || len < 3U) {
        return;
    }

    uint8_t slot = state->currentslot & 1;
    uint16_t rec_len = (uint16_t)(((uint16_t)msg[0] << 8) | msg[1]);
    uint16_t avail = (uint16_t)(len - 2U);
    if (rec_len > avail) {
        rec_len = avail;
    }
    if (rec_len == 0U) {
        return;
    }

    const uint8_t* rec = msg + 2;
    dmr_ars_format(rec, rec_len, summary, sizeof(summary));

    if (summary[0] != '\0') {
        DSD_FPRINTF(stderr, "\n %s", summary);
        dsd_strncat_s(state->dmr_lrrp_gps[slot], sizeof(state->dmr_lrrp_gps[slot]), summary, sizeof(summary));
        return;
    }

    if (rec_len > DMR_ARS_MAX_DUMP) {
        rec_len = DMR_ARS_MAX_DUMP;
    }
    utf8_to_text(state, 0, rec_len, rec);
}
