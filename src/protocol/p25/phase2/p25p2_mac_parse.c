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
p25p2_mac_positive_len(int len) {
    return (len > 0) ? len : -1;
}

/* TDMA 0x08 Null Avoid Zero Bias and 0x10 Multi-Fragment Continuation both carry length in octet 2. */
static int
p25p2_mac_length_coded_tdma_len(uint8_t opcode, int len) {
    switch (opcode) {
        case 0x08u:
        case 0x10u: return p25p2_mac_positive_len(len);
        default: return -1;
    }
}

static int
p25p2_mac_motorola_payload_len(uint8_t opcode, int len) {
    switch (opcode) {
        case 0x81u:
        case 0x82u:
        case 0x8Bu:
        case 0x89u:
        case 0x8Fu:
        case 0x91u:
        case 0x95u: return p25p2_mac_positive_len(len);
        case 0xBFu: return (len > 0) ? len : 3;
        default: return -1;
    }
}

static int
p25p2_mac_harris_payload_len(uint8_t opcode, int len) {
    int fixed_len = p25p2_mac_len_for(0xA4u, opcode);
    if (fixed_len > 0) {
        return fixed_len;
    }
    return p25p2_mac_positive_len(len);
}

static int
p25p2_mac_vendor_payload_len(uint8_t mfid, uint8_t opcode, int len) {
    switch (mfid) {
        case 0x90u: return p25p2_mac_motorola_payload_len(opcode, len);
        case 0xA4u: return p25p2_mac_harris_payload_len(opcode, len);
        case 0xD8u: return (opcode != 0xB5u) ? p25p2_mac_positive_len(len) : -1;
        default: return -1;
    }
}

static int
p25p2_mac_payload_len_override(const unsigned long long mac[24], int opcode_pos) {
    if (opcode_pos < 0 || opcode_pos + 2 >= 24) {
        return -1;
    }

    uint8_t opcode = (uint8_t)p25p2_mac_octet(mac, opcode_pos);
    uint8_t mfid = (uint8_t)p25p2_mac_octet(mac, opcode_pos + 1);
    uint8_t len_octet = (uint8_t)p25p2_mac_octet(mac, opcode_pos + 2);
    int length_coded_tdma_len = p25p2_mac_length_coded_tdma_len(opcode, (int)(mfid & 0x3Fu));

    if (opcode == 0x11u) {
        return 2 + (2 * (int)((mfid & 0x03u) + 1u));
    }
    if (opcode == 0x12u) {
        return 2 + (3 * (int)((mfid & 0x03u) + 1u));
    }

    if (length_coded_tdma_len >= 0) {
        return length_coded_tdma_len;
    }

    if (opcode == 0x80u && mfid == 0xAAu && len_octet == 0xA4u && opcode_pos + 4 < 24) {
        uint8_t shifted_len = (uint8_t)p25p2_mac_octet(mac, opcode_pos + 3);
        if ((shifted_len & 0x3Fu) > 0u) {
            return (int)(shifted_len & 0x3Fu);
        }
        return 17;
    }

    if (opcode >= 0x80u && opcode <= 0xBFu) {
        return p25p2_mac_vendor_payload_len(mfid, opcode, (int)(len_octet & 0x3Fu));
    }

    return -1;
}

static int
p25p2_mac_resolve_segment_len(int type, const unsigned long long mac[24], int offset, int capacity) {
    int opcode_pos = 1 + offset;
    if (opcode_pos < 0 || opcode_pos >= 24 || offset >= capacity) {
        return 0;
    }

    int len = p25p2_mac_payload_len_override(mac, opcode_pos);
    if (len >= 0) {
        return len;
    }

    if (opcode_pos + 1 < 24) {
        len =
            p25p2_mac_len_for((uint8_t)p25p2_mac_octet(mac, opcode_pos + 1), (uint8_t)p25p2_mac_octet(mac, opcode_pos));
        if (len > 0) {
            return len;
        }
    }

    if (offset == 0) {
        int guess = p25p2_mac_guess_len_b(type, mac, capacity);
        if (guess >= 0) {
            return guess;
        }
    }

    return capacity - offset;
}

static int
p25p2_mac_parse_segments(int type, const unsigned long long mac[24], struct p25p2_mac_result* out) {
    const int capacity = p25p2_mac_capacity(type);
    int offset = 0;

    for (int seg = 0; seg < P25P2_MAC_MAX_SEGMENTS && offset < capacity; seg++) {
        int len = p25p2_mac_resolve_segment_len(type, mac, offset, capacity);
        if (len <= 0) {
            break;
        }

        if (offset > 0 && len > capacity - offset) {
            break;
        }

        out->segments[out->segment_count].offset = offset;
        out->segments[out->segment_count].length = len;
        out->segment_count++;

        if (len > capacity - offset) {
            break;
        }
        offset += len;
    }

    return out->segment_count;
}

int
p25p2_mac_parse(int type, const unsigned long long mac[24], struct p25p2_mac_result* out) {
    if (out == NULL || mac == NULL) {
        return -1;
    }

    for (int i = 0; i < P25P2_MAC_MAX_SEGMENTS; i++) {
        out->segments[i].offset = 0;
        out->segments[i].length = 0;
    }
    out->type = type;
    out->len_a = 0;
    out->mfid = (uint8_t)mac[2];
    out->opcode = (uint8_t)mac[1];
    out->len_b = 0;
    out->len_c = 0;
    out->segment_count = 0;

    (void)p25p2_mac_parse_segments(type, mac, out);

    if (out->segment_count > 0) {
        out->len_a = out->segments[0].offset;
        out->len_b = out->segments[0].length;
    }
    if (out->segment_count > 1) {
        out->len_c = out->segments[1].length;
    }

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
