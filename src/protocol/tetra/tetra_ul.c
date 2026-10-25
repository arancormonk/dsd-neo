// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA trunked mode uplink C-plane reader (tetra_ul.h).
 */

#include "tetra_ul.h"

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <stdint.h>
#include <stdio.h>
#include "tetra_log.h"
#include "tetra_mac_pdu.h"
#include "tetra_mm_pdu.h"
#include "tetra_state.h"

/* Address type 01 is a 10-bit event label; SSI, USSI and SMI are 24 bits (tables 21.41 and 21.46). */
#define TETRA_UL_ADDR_EVENT_LABEL 1u

/* LLC frame check sequence (clause 21.1.2.3) of the FCS variants of the basic link PDUs. */
#define TETRA_UL_LLC_FCS_BITS     32

/* MLE protocol discriminator of MM (table 18.87) and the uplink MM PDU type of U-LOCATION UPDATE DEMAND (table
 * 16.75). */
#define TETRA_UL_MLE_PD_MM        1u
#define TETRA_UL_MM_LU_DEMAND     2u
#define TETRA_UL_CIPHER_PARAMS_N  10

/* The basic link PDUs (table 21.1, LLC PDU types 0000 to 0111): LLC PDU type plus N(R) and N(S) where present, and
 * whether a 32-bit FCS follows the TL-SDU. BL-ACK alone may carry no TL-SDU. */
static const struct {
    uint8_t header_bits;
    uint8_t fcs;
} k_ul_basic_link[8] = {
    {6, 0}, {5, 0}, {4, 0}, {5, 0}, /* BL-ADATA, BL-DATA, BL-UDATA, BL-ACK */
    {6, 1}, {5, 1}, {4, 1}, {5, 1}, /* their FCS variants */
};

/* Uplink MM PDU types (table 16.75), for the summary line. */
static const char* const k_ul_mm_names[16] = {
    "U-AUTHENTICATION",
    "U-ITSI DETACH",
    "U-LOCATION UPDATE DEMAND",
    "U-MM STATUS",
    "U-CK CHANGE RESULT",
    "U-OTAR",
    "U-INFORMATION PROVIDE",
    "U-ATTACH/DETACH GROUP IDENTITY",
    "U-ATTACH/DETACH GROUP IDENTITY ACK",
    "U-TEI PROVIDE",
    "reserved",
    "U-DISABLE STATUS",
    "reserved",
    "reserved",
    "reserved",
    "MM PDU/FUNCTION NOT SUPPORTED",
};

/* The name of an address in a summary line: an event label is no subscriber identity, and the SSI of an encrypted PDU
 * is an encrypted short identity (EN 300 392-7 clause 4.2.6; USSI and SMI are never encrypted). */
static const char*
ul_addr_name(uint8_t addr_type, uint8_t encrypted) {
    static const char* const names[4] = {"ssi", "event_label", "ussi", "smi"};
    return (addr_type & 3u) == 0u && encrypted ? "esi" : names[addr_type & 3u];
}

/* The length a MAC-ACCESS length indication (5 bits, table 21.41) gives the PDU, in bits on pi/4-DQPSK (Y1 = Z1 = 1
 * octet, table 21.98): 1..14 x Y1 and 15..28 as 14 x Y1 + (value - 14) x Z1, both value octets there. 0 for the Null
 * PDU, -1 for the reserved values 29 and 30 and for 31, a QAM channel's length. */
static int
ul_access_length_bits(uint32_t li) {
    if (li == 0u) {
        return 0;
    }
    return li <= 28u ? (int)li * 8 : -1;
}

/* The length a MAC-DATA length indication (6 bits, table 21.46) gives the PDU, in bits on pi/4-DQPSK (Y2 = Z2 = 1
 * octet): 2..18 x Y2 and 19..56 as 18 x Y2 + (value - 18) x Z2, both value octets there. 0 for the Null PDU, -1 for
 * 1 and 57..60 (reserved), 61 (a QAM channel's length), and 62 and 63, which only the stealing channel uses. */
static int
ul_data_length_bits(uint32_t li) {
    if (li == 0u) {
        return 0;
    }
    return (li >= 2u && li <= 56u) ? (int)li * 8 : -1;
}

/* The MM PDU of a TL-SDU, @p r just after the MLE protocol discriminator: its uplink type, and the leading type 1
 * elements of a U-LOCATION UPDATE DEMAND (table 16.18). A TL-SDU too short for them reads no demand. */
static void
ul_read_mm(tetra_bit_reader* r, tetra_ul_reading* out) {
    const uint8_t type = (uint8_t)tetra_bits_read(r, 4);
    if (!tetra_bits_ok(r)) {
        return;
    }
    out->mm = 1;
    out->mm_type = type;
    if (type != TETRA_UL_MM_LU_DEMAND) {
        return;
    }
    const uint8_t lu_type = (uint8_t)tetra_bits_read(r, 3);
    (void)tetra_bits_read(r, 1); /* request to append LA */
    const uint8_t control = (uint8_t)tetra_bits_read(r, 1);
    const uint16_t params = control ? (uint16_t)tetra_bits_read(r, TETRA_UL_CIPHER_PARAMS_N) : 0U;
    if (!tetra_bits_ok(r)) {
        return;
    }
    out->lu_demand = 1;
    out->lu_type = lu_type;
    out->cipher_control = control;
    out->cipher_params = params;
}

/* A clear TM-SDU, whole or a fragment start: the basic link LLC (table 21.1), then the MLE protocol discriminator and
 * an MM PDU. A whole FCS variant whose FCS does not match is not read; a fragment start's FCS lies past it. */
static void
ul_read_tm_sdu(const uint8_t* sdu, int nbits, int fragment, tetra_ul_reading* out) {
    tetra_bit_reader r = tetra_bits_reader(sdu, nbits, 0);
    const uint32_t type = tetra_bits_read(&r, 4);
    if (!tetra_bits_ok(&r) || type >= 8u) {
        return; /* no LLC header, or an advanced link or layer 2 signalling PDU */
    }
    const int header = k_ul_basic_link[type].header_bits;
    int tl_bits = nbits - header;
    if (k_ul_basic_link[type].fcs && !fragment) {
        tl_bits -= TETRA_UL_LLC_FCS_BITS;
        if (tl_bits <= 0) {
            return;
        }
        tetra_bit_reader f = tetra_bits_reader(sdu, nbits, header + tl_bits);
        if (tetra_bits_read(&f, TETRA_UL_LLC_FCS_BITS) != tetra_llc_fcs32(sdu + header, tl_bits)) {
            return;
        }
    }
    if (tl_bits <= 0) {
        return;
    }
    tetra_bit_reader m = tetra_bits_reader(sdu + header, tl_bits, 0);
    if (tetra_bits_read(&m, 3) != TETRA_UL_MLE_PD_MM || !tetra_bits_ok(&m)) {
        return;
    }
    ul_read_mm(&m, out);
}

/* The MAC PDU type of an uplink block (tables 21.40 and 21.38): 1 for a MAC-ACCESS or MAC-DATA, whose header follows
 * in @p r; otherwise 0, with what the block holds in @c kind. */
static int
ul_read_type(tetra_ul_channel ch, tetra_bit_reader* r, tetra_ul_reading* out) {
    if (ch == TETRA_UL_SCH_HU) {
        if (tetra_bits_read(r, 1) == 0u) {
            return tetra_bits_ok(r);
        }
        out->kind = tetra_bits_ok(r) ? (uint8_t)TETRA_UL_PDU_END_HU : (uint8_t)TETRA_UL_PDU_UNREAD;
        return 0;
    }
    const uint32_t type = tetra_bits_read(r, 2);
    if (!tetra_bits_ok(r) || type == 0u) {
        return tetra_bits_ok(r);
    }
    /* 01 MAC-FRAG or MAC-END, 10 broadcast (downlink only), 11 supplementary (table 21.38). */
    out->kind = type == 1u   ? (uint8_t)TETRA_UL_PDU_FRAG_END
                : type == 3u ? (uint8_t)TETRA_UL_PDU_SUPPLEMENTARY
                             : (uint8_t)TETRA_UL_PDU_UNREAD;
    return 0;
}

/* The header's length indication or capacity request: @p length the bits the length indication gives (0 a Null PDU,
 * -1 without one), @p fragment the capacity request's start of fragmentation. MAC-ACCESS has them behind its optional
 * field flag; MAC-DATA always has one of them, a capacity request with its reserved bit. Returns 0 for a reserved
 * length indication. */
static int
ul_read_length(tetra_ul_channel ch, tetra_bit_reader* r, int* length, int* fragment) {
    *length = -1;
    *fragment = 0;
    if (ch == TETRA_UL_SCH_HU && tetra_bits_read(r, 1) == 0u) {
        return 1; /* no optional field */
    }
    if (tetra_bits_read(r, 1) == 0u) {
        *length = ch == TETRA_UL_SCH_HU ? ul_access_length_bits(tetra_bits_read(r, 5))
                                        : ul_data_length_bits(tetra_bits_read(r, 6));
        return *length >= 0;
    }
    *fragment = (int)tetra_bits_read(r, 1);
    (void)tetra_bits_read(r, 4); /* reservation requirement */
    if (ch == TETRA_UL_SCH_F) {
        (void)tetra_bits_read(r, 1); /* reserved */
    }
    return 1;
}

/* The end of a PDU's content, its header ending at @p body: the lesser of its indicated @p length and the block
 * (clause 23.4.3.3), never inside its header, without its fill bits (@p fill). A Null PDU (@p length 0) has none; its
 * header, encrypted flag included, counts as any other's (clause 21.4.2.1 note 1). -1 for a length shorter than the
 * header, or fill bits without their final 1 bit. */
static int
ul_content_end(const uint8_t* bits, int nbits, int body, int length, uint32_t fill) {
    if (length == 0) {
        return body;
    }
    if (length > 0 && length < body) {
        return -1;
    }
    const int pdu_bits = (length > 0 && length < nbits) ? length : nbits;
    return tetra_mac_content_end(bits, body, pdu_bits, fill);
}

int
tetra_ul_read(tetra_ul_channel ch, const uint8_t* bits, int nbits, tetra_ul_reading* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (!bits || nbits <= 0) {
        return 0;
    }
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    if (!ul_read_type(ch, &r, out)) {
        return 0;
    }
    const uint32_t fill = tetra_bits_read(&r, 1);
    const uint8_t encrypted = (uint8_t)tetra_bits_read(&r, 1);
    const uint8_t addr_type = (uint8_t)tetra_bits_read(&r, 2);
    const uint32_t addr = tetra_bits_read(&r, addr_type == TETRA_UL_ADDR_EVENT_LABEL ? 10 : 24);
    int length = -1;
    int fragment = 0;
    if (!ul_read_length(ch, &r, &length, &fragment) || !tetra_bits_ok(&r)) {
        return 0;
    }
    const int end = ul_content_end(bits, nbits, r.pos, length, fill);
    if (end < 0) {
        return 0;
    }
    out->kind = ch == TETRA_UL_SCH_HU ? (uint8_t)TETRA_UL_PDU_ACCESS : (uint8_t)TETRA_UL_PDU_DATA;
    out->addr_type = addr_type;
    out->addr = addr;
    out->header_ok = 1;
    out->encrypted = encrypted;
    out->null_pdu = (uint8_t)(length == 0);
    out->fragment = (uint8_t)fragment;
    if (!encrypted && end > r.pos) {
        ul_read_tm_sdu(bits + r.pos, end - r.pos, fragment, out);
    }
    return 1;
}

/* A U-LOCATION UPDATE DEMAND read off a CRC-passed block: its summary line, and the KSG it proposes noted for the
 * SwMI the MS registers with, the one being received: the address extension of a migrating MS is its own MNI (table
 * 16.18). */
static void
ul_note_demand(dsd_state* state, const tetra_state_t* ts, const tetra_ul_reading* u, int cc) {
    char cipher[40];
    tetra_mm_cipher_detail(u->cipher_control, u->cipher_params, cipher, sizeof cipher);
    DSD_FPRINTF(stderr, "[TETRA MM U-LU-DEMAND] CC=%d %s=%u type=%u cipher=%u%s\n", cc,
                ul_addr_name(u->addr_type, u->encrypted), (unsigned)u->addr, (unsigned)u->lu_type,
                (unsigned)u->cipher_control, cipher);
    if (u->cipher_control && ts->net_known && !ts->dmo) {
        (void)tetra_air_ksg_request(state, ts->mcc, ts->mnc, (uint8_t)((u->cipher_params >> 6) & 0xFu));
    }
}

void
tetra_ul_block(dsd_state* state, tetra_ul_channel ch, const uint8_t* bits, int nbits, int cc) {
    static const char* const kinds[] = {"PDU not read", "MAC-ACCESS",       "MAC-DATA",
                                        "MAC-END-HU",   "MAC-FRAG/MAC-END", "MAC-U-BLCK"};
    _Static_assert(sizeof kinds / sizeof kinds[0] == (size_t)TETRA_UL_PDU_SUPPLEMENTARY + 1U,
                   "one name per uplink PDU kind");
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !bits) {
        return;
    }
    const char* const channel = ch == TETRA_UL_SCH_HU ? "SCH/HU" : "SCH/F";
    tetra_ul_reading u;
    (void)tetra_ul_read(ch, bits, nbits, &u);
    const char* const kind = u.kind <= TETRA_UL_PDU_SUPPLEMENTARY ? kinds[u.kind] : kinds[0];
    if (!u.header_ok) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA %s] CC=%d %s\n", channel, cc, kind);
        return;
    }
    ts->ul_enc = u.encrypted ? (uint8_t)TETRA_UL_ENC_ENCRYPTED : (uint8_t)TETRA_UL_ENC_CLEAR;
    DSD_FPRINTF(stderr, "[TETRA %s] CC=%d %s enc=%u %s=%u%s%s\n", channel, cc, kind, (unsigned)u.encrypted,
                ul_addr_name(u.addr_type, u.encrypted), (unsigned)u.addr, u.null_pdu ? " null" : "",
                u.fragment ? " fragment" : "");
    if (u.lu_demand) {
        ul_note_demand(state, ts, &u, cc);
    } else if (u.mm) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA MM] CC=%d uplink %s not decoded\n", cc, k_ul_mm_names[u.mm_type & 15u]);
    }
}
