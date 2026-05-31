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

static int
p25p2_mac_capacity(int type) {
    return (type == 1) ? 19 : 16;
}

static int
p25p2_clamp_int(int value, int min, int max) {
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static int
p25p2_mac_guess_len_b(int type, const unsigned long long mac[24], int capacity) {
    int mco = (int)(mac[1] & 0x3F);
    if (mco <= 0) {
        return -1;
    }
    if (mac[0] == 0 && type != 1) {
        return -1;
    }
    return p25p2_clamp_int(mco - 1, 0, capacity);
}

static int
p25p2_mac_payload_len_override(const unsigned long long mac[24], int opcode_pos) {
    if (opcode_pos < 0 || opcode_pos + 2 >= 24) {
        return -1;
    }

    uint8_t opcode = (uint8_t)p25p2_mac_octet(mac, opcode_pos);
    uint8_t mfid = (uint8_t)p25p2_mac_octet(mac, opcode_pos + 1);
    if (mfid != 0x90u) {
        return -1;
    }

    if (opcode == 0x81u) {
        return (int)p25p2_mac_octet(mac, opcode_pos + 2);
    }
    if (opcode == 0x8Fu) {
        int len = (int)p25p2_mac_octet(mac, opcode_pos + 2);
        return p25p2_clamp_int(len, 0, 12);
    }
    if (opcode == 0xBFu) {
        return 3;
    }

    return -1;
}

static int
p25p2_mac_resolve_len_b(int type, const unsigned long long mac[24], int capacity, int len_b) {
    if (len_b != 0 && len_b <= capacity) {
        return len_b;
    }
    if (len_b == 0 || len_b > capacity) {
        int guess = p25p2_mac_guess_len_b(type, mac, capacity);
        if (guess >= 0) {
            return guess;
        }
    }
    return len_b;
}

static int
p25p2_mac_has_second_message(int type, int len_b) {
    if (type == 1) {
        return len_b < 19;
    }
    if (type == 0) {
        return len_b < 16;
    }
    return 0;
}

static int
p25p2_mac_resolve_len_c(int type, const unsigned long long mac[24], int len_b, int capacity) {
    if (!p25p2_mac_has_second_message(type, len_b)) {
        return 0;
    }

    int next_opcode_pos = 1 + len_b;
    if (next_opcode_pos >= 24) {
        return 0;
    }

    int len_c = p25p2_mac_payload_len_override(mac, next_opcode_pos);
    if (len_c >= 0) {
        return len_c;
    }
    if (next_opcode_pos + 1 >= 24) {
        return 0;
    }

    len_c = p25p2_mac_len_for((uint8_t)mac[next_opcode_pos + 1], (uint8_t)mac[next_opcode_pos]);
    if (len_c != 0) {
        return len_c;
    }

    int remain = capacity - len_b;
    if (remain > 0) {
        return remain;
    }
    return 0;
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
    int payload_len = p25p2_mac_payload_len_override(mac, 1);
    int len_b = (payload_len >= 0) ? payload_len : p25p2_mac_len_for(out->mfid, out->opcode);
    const int capacity = p25p2_mac_capacity(type);
    if (payload_len < 0) {
        len_b = p25p2_mac_resolve_len_b(type, mac, capacity, len_b);
    }
    int len_c = p25p2_mac_resolve_len_c(type, mac, len_b, capacity);

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
