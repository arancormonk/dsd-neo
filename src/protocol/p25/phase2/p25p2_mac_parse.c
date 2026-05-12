// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * P25 Phase 2 MAC VPDU parsing helpers.
 */

#include <dsd-neo/protocol/p25/p25p2_mac_parse.h>
#include <dsd-neo/protocol/p25/p25p2_mac_tables.h>
#include <stddef.h>
#include <stdint.h>

static unsigned int
p25p2_mac_octet(const unsigned long long mac[24], int pos) {
    return (unsigned int)(mac[pos] & 0xFFu);
}

static int
p25p2_signed_offset_units(int sign_bit, int raw_offset) {
    return sign_bit ? raw_offset : -raw_offset;
}

int
p25p2_mac_parse(int type, const unsigned long long mac[24], struct p25p2_mac_result* out) {
    if (out == NULL || mac == NULL) {
        return -1;
    }

    out->type = type;
    out->len_a = 0;
    out->mfid = (uint8_t)mac[2];
    out->opcode = (uint8_t)mac[1];

    int len_a = 0;
    int len_b = p25p2_mac_len_for(out->mfid, out->opcode);
    int len_c = 0;

    /* Compute per-channel capacity for message-carrying octets (excludes opcode byte itself). */
    const int capacity = (type == 1) ? 19 : 16;

    /* If table/override gives no guidance, try deriving from MCO when header is present. */
    if (len_b == 0 || len_b > capacity) {
        int mco = (int)(mac[1] & 0x3F);
        if ((mac[0] != 0 || type == 1) && mco > 0) {
            int guess = mco - 1;
            if (guess > capacity) {
                guess = capacity;
            }
            if (guess < 0) {
                guess = 0;
            }
            len_b = guess;
        }
    }

    /* Derive second message length when possible using the same table. */
    if (type == 1 && len_b < 19) {
        len_c = p25p2_mac_len_for((uint8_t)mac[3 + len_a], (uint8_t)mac[1 + len_b]);
    }
    if (type == 0 && len_b < 16) {
        len_c = p25p2_mac_len_for((uint8_t)mac[3 + len_a], (uint8_t)mac[1 + len_b]);
    }

    /* If the second message length is unknown, fill with remaining capacity as a last resort. */
    if ((type == 1 && len_b < 19 && len_c == 0) || (type == 0 && len_b < 16 && len_c == 0)) {
        int remain = capacity - len_b;
        if (remain > 0) {
            len_c = remain;
        }
    }

    out->len_a = len_a;
    out->len_b = len_b;
    out->len_c = len_c;

    return 0;
}

int
p25p2_mac_decode_iden_standard(const unsigned long long mac[24], int pos, struct p25p2_iden_update* out) {
    if (mac == NULL || out == NULL || pos < 0 || pos + 7 >= 24) {
        return -1;
    }

    unsigned int b0 = p25p2_mac_octet(mac, pos);
    unsigned int b1 = p25p2_mac_octet(mac, pos + 1);
    unsigned int b2 = p25p2_mac_octet(mac, pos + 2);
    unsigned int b3 = p25p2_mac_octet(mac, pos + 3);
    unsigned int b4 = p25p2_mac_octet(mac, pos + 4);
    unsigned int b5 = p25p2_mac_octet(mac, pos + 5);
    unsigned int b6 = p25p2_mac_octet(mac, pos + 6);
    unsigned int b7 = p25p2_mac_octet(mac, pos + 7);

    int sign_bit = (int)((b1 >> 2) & 0x01u);
    int tx_raw = (int)(((b1 & 0x03u) << 6) | (b2 >> 2));

    out->iden = (uint8_t)((b0 >> 4) & 0x0Fu);
    out->chan_type = 1;
    out->bw_vu = 0;
    out->bandwidth = (int)(((b0 & 0x0Fu) << 5) | ((b1 & 0xF8u) >> 3));
    out->trans_off = p25p2_signed_offset_units(sign_bit, tx_raw);
    out->chan_spac = (int)(((b2 & 0x03u) << 8) | b3);
    out->base_freq = (long int)((b4 << 24) | (b5 << 16) | (b6 << 8) | b7);
    return 0;
}

int
p25p2_mac_decode_iden_vuhf(const unsigned long long mac[24], int pos, struct p25p2_iden_update* out) {
    if (mac == NULL || out == NULL || pos < 0 || pos + 7 >= 24) {
        return -1;
    }

    unsigned int b0 = p25p2_mac_octet(mac, pos);
    unsigned int b1 = p25p2_mac_octet(mac, pos + 1);
    unsigned int b2 = p25p2_mac_octet(mac, pos + 2);
    unsigned int b3 = p25p2_mac_octet(mac, pos + 3);
    unsigned int b4 = p25p2_mac_octet(mac, pos + 4);
    unsigned int b5 = p25p2_mac_octet(mac, pos + 5);
    unsigned int b6 = p25p2_mac_octet(mac, pos + 6);
    unsigned int b7 = p25p2_mac_octet(mac, pos + 7);

    int sign_bit = (int)((b1 >> 7) & 0x01u);
    int tx_raw = (int)(((b1 & 0x7Fu) << 6) | (b2 >> 2));

    out->iden = (uint8_t)((b0 >> 4) & 0x0Fu);
    out->chan_type = 1;
    out->bw_vu = (uint8_t)(b0 & 0x0Fu);
    out->bandwidth = 0;
    out->trans_off = p25p2_signed_offset_units(sign_bit, tx_raw);
    out->chan_spac = (int)(((b2 & 0x03u) << 8) | b3);
    out->base_freq = (long int)((b4 << 24) | (b5 << 16) | (b6 << 8) | b7);
    return 0;
}

int
p25p2_mac_decode_iden_tdma(const unsigned long long mac[24], int pos, struct p25p2_iden_update* out) {
    int rc = p25p2_mac_decode_iden_vuhf(mac, pos, out);
    if (rc != 0) {
        return rc;
    }
    out->chan_type = out->bw_vu & 0x0Fu;
    out->bw_vu = 0;
    return 0;
}
