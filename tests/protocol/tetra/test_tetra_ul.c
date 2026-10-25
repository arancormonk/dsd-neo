// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The trunked mode uplink reader (tetra_ul.h): the first MAC PDU of a CRC-passed SCH/HU (MAC-ACCESS, EN 300 392-2
 * table 21.41) or SCH/F (MAC-DATA, table 21.46) block, its basic link LLC header (table 21.1), the MLE protocol
 * discriminator and the leading type 1 elements of a U-LOCATION UPDATE DEMAND (table 16.18), whose ciphering
 * parameters (EN 300 392-7 table A.46) propose a KSG for the serving network.
 *
 * Bit layout convention: one byte per bit, value 0 or 1, MSB first.
 */

#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "test_support.h"
#include "tetra_state.h"
#include "tetra_ul.h"

enum { SCH_HU_BITS = 92, SCH_F_BITS = 268, MAX_BITS = 400 };

typedef struct {
    uint8_t bits[MAX_BITS];
    int n;
} bitbuf;

static void
put(bitbuf* b, uint32_t v, int width) {
    for (int i = width - 1; i >= 0; i--) {
        assert(b->n < MAX_BITS);
        b->bits[b->n++] = (uint8_t)((v >> i) & 1U);
    }
}

/* The ciphering parameters (table A.46): KSG number, security class bit (0 class 2, 1 class 3), then the SCK number or
 * the class 3 capability and reserved bits. */
static uint16_t
cipher_params(uint32_t ksg, uint32_t class3, uint32_t tail) {
    return (uint16_t)((ksg << 6) | (class3 << 5) | (tail & 0x1FU));
}

/* A TL-SDU of MLE protocol discriminator MM and a U-LOCATION UPDATE DEMAND: location update type 3 (ITSI attach),
 * request to append LA 0, cipher control @p control and, when on, @p params; then @p extra zero bits standing for its
 * type 2/3 elements. */
static void
put_lu_demand(bitbuf* b, uint32_t control, uint16_t params, int extra) {
    put(b, 1U, 3); /* MLE protocol discriminator: MM */
    put(b, 2U, 4); /* U-LOCATION UPDATE DEMAND */
    put(b, 3U, 3);
    put(b, 0U, 1);
    put(b, control, 1);
    if (control) {
        put(b, params, 10);
    }
    put(b, 0U, extra);
}

/* A MAC-ACCESS header: fill bit indication @p fill, encrypted flag @p enc, SSI @p ssi (or a 10-bit event label when
 * @p event_label), and no optional field. */
static void
put_access(bitbuf* b, uint32_t fill, uint32_t enc, int event_label, uint32_t addr) {
    put(b, 0U, 1);
    put(b, fill, 1);
    put(b, enc, 1);
    put(b, event_label ? 1U : 0U, 2);
    put(b, addr, event_label ? 10 : 24);
}

/* Overwrite @p width bits at @p at with @p v. */
static void
set_bits(bitbuf* b, int at, uint32_t v, int width) {
    for (int k = 0; k < width; k++) {
        b->bits[at + k] = (uint8_t)((v >> (width - 1 - k)) & 1U);
    }
}

/* Pad the block to @p nbits: a 1 then zeros when @p fill (clause 23.4.3.2), else zeros. */
static void
pad(bitbuf* b, int nbits, int fill) {
    if (fill && b->n < nbits) {
        put(b, 1U, 1);
    }
    while (b->n < nbits) {
        put(b, 0U, 1);
    }
}

/* A whole MAC-ACCESS from SSI 0x123456 carrying a BL-ADATA with a U-LOCATION UPDATE DEMAND proposing @p params. */
static void
access_lu(bitbuf* b, uint32_t enc, uint32_t control, uint16_t params) {
    DSD_MEMSET(b, 0, sizeof(*b));
    put_access(b, 1U, enc, 0, 0x123456U);
    put(b, 0U, 1); /* no optional field */
    put(b, 0U, 4); /* BL-ADATA */
    put(b, 0U, 2); /* N(R), N(S) */
    put_lu_demand(b, control, params, 8);
    pad(b, SCH_HU_BITS, 1);
}

static void
test_access_reads_the_proposal(void) {
    bitbuf b;
    tetra_ul_reading u;
    access_lu(&b, 0U, 1U, cipher_params(0U, 0U, 2U));
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1);
    assert(u.kind == TETRA_UL_PDU_ACCESS && u.header_ok && !u.encrypted && u.addr_type == 0U && u.addr == 0x123456U);
    assert(u.mm && u.mm_type == 2U && u.lu_demand && u.lu_type == 3U && u.cipher_control == 1U);
    assert(u.cipher_params == cipher_params(0U, 0U, 2U) && !u.fragment && !u.null_pdu);

    /* A class 3 vector with every capability bit and the reserved bit set: all ten bits are the parameters. */
    access_lu(&b, 0U, 1U, cipher_params(1U, 1U, 0x1FU));
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.cipher_params == 0x07FU);

    /* Cipher control off: a demand, but no parameters. */
    access_lu(&b, 0U, 0U, 0U);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.lu_demand && u.cipher_control == 0U);

    /* Encrypted: the header's flag counts, the TM-SDU is not read. */
    access_lu(&b, 1U, 1U, cipher_params(0U, 0U, 2U));
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.header_ok && u.encrypted && !u.mm);

    /* An event label is a 10-bit address. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 1U, 0U, 1, 0x2A5U);
    put(&b, 0U, 1);
    put(&b, 2U, 4); /* BL-UDATA */
    put_lu_demand(&b, 1U, cipher_params(1U, 0U, 0U), 0);
    pad(&b, SCH_HU_BITS, 1);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1);
    assert(u.addr_type == 1U && u.addr == 0x2A5U && u.lu_demand && u.cipher_params == cipher_params(1U, 0U, 0U));

    /* MAC-END-HU is a last fragment: not read. */
    DSD_MEMSET(&b, 0, sizeof b);
    put(&b, 1U, 1);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 0 && u.kind == TETRA_UL_PDU_END_HU && !u.header_ok);
}

/* MAC-ACCESS optional fields: the length indication in octets bounded by the block, its Null PDU and reserved values,
 * and a capacity request starting a fragmented TM-SDU, whose FCS lies past the fragment. */
static void
test_access_length_and_fragments(void) {
    bitbuf b;
    tetra_ul_reading u;
    /* Length 8 octets: header 36 bits, BL-UDATA 4 and the demand's 26 bits; the rest of the block is not the PDU's. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 0U, 0U, 0, 7U);
    put(&b, 1U, 1);
    put(&b, 0U, 1);
    put(&b, 8U, 5);
    put(&b, 2U, 4);
    put_lu_demand(&b, 1U, cipher_params(2U, 0U, 0U), 2);
    assert(b.n == 64);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.lu_demand);
    assert(u.cipher_params == cipher_params(2U, 0U, 0U));

    /* Length 6 octets (48 bits) cuts the same PDU before its parameters: no demand. */
    set_bits(&b, 31, 6U, 5);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.header_ok && !u.lu_demand);

    /* Length 12 octets (96 bits) runs past the 92-bit block: the PDU ends with the block. */
    set_bits(&b, 31, 12U, 5);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.lu_demand);

    /* Reserved 29 and 30, and 31 (a QAM channel's): nothing read. A length shorter than the header: nothing read. */
    static const uint32_t k_bad[] = {29U, 30U, 31U, 4U};
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        set_bits(&b, 31, k_bad[i], 5);
        assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 0 && !u.header_ok);
    }

    /* A Null PDU: its encrypted flag counts, nothing else is read. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 0U, 1U, 0, 7U);
    put(&b, 1U, 1);
    put(&b, 0U, 1);
    put(&b, 0U, 5);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.null_pdu && u.encrypted && !u.mm);
    /* A clear Null PDU ends the block's useful content: what follows it, registration-like, is not read. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 0U, 0U, 0, 7U);
    put(&b, 1U, 1);
    put(&b, 0U, 1);
    put(&b, 0U, 5);
    put(&b, 2U, 4);
    put_lu_demand(&b, 1U, cipher_params(0U, 0U, 0U), 0);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.null_pdu && !u.encrypted && !u.mm);

    /* A capacity request starting a fragmented BL-DATA-FCS: the leading elements are read without the FCS. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 0U, 0U, 0, 7U);
    put(&b, 1U, 1);
    put(&b, 1U, 1);
    put(&b, 1U, 1); /* start of fragmentation */
    put(&b, 0U, 4); /* reservation requirement */
    put(&b, 5U, 4); /* BL-DATA-FCS */
    put(&b, 0U, 1); /* N(S) */
    put_lu_demand(&b, 1U, cipher_params(0U, 1U, 0x10U), 0);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.fragment && u.lu_demand);
    assert(u.cipher_params == cipher_params(0U, 1U, 0x10U));

    /* Fill bits announced but no 1 bit after the header: malformed. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 1U, 0U, 0, 0U);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 0 && !u.header_ok);
}

/* Only the first PDU of a block is read: an associated PDU after it lends it no bits, and is itself never read. */
static void
test_first_pdu_only(void) {
    bitbuf b;
    tetra_ul_reading u;
    /* A 5-octet first PDU (header 36 bits, BL-UDATA type and nothing more) followed by a whole registration. */
    DSD_MEMSET(&b, 0, sizeof b);
    put_access(&b, 0U, 0U, 0, 7U);
    put(&b, 1U, 1);
    put(&b, 0U, 1);
    put(&b, 5U, 5);
    put(&b, 2U, 4);
    assert(b.n == 40);
    put_lu_demand(&b, 1U, cipher_params(0U, 0U, 0U), 0);
    pad(&b, SCH_HU_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && !u.lu_demand);

    /* A whole first registration proposing TEA2, then a second proposing TEA1 in the same SCH/F: TEA2 only. */
    DSD_MEMSET(&b, 0, sizeof b);
    put(&b, 0U, 2); /* MAC-DATA */
    put(&b, 0U, 1);
    put(&b, 0U, 1);
    put(&b, 0U, 2);
    put(&b, 7U, 24);
    put(&b, 0U, 1);
    put(&b, 8U, 6); /* 64 bits */
    put(&b, 2U, 4);
    put_lu_demand(&b, 1U, cipher_params(1U, 0U, 0U), 1);
    assert(b.n == 64);
    put(&b, 0U, 2);
    put(&b, 0U, 1);
    put(&b, 0U, 1);
    put(&b, 0U, 2);
    put(&b, 8U, 24);
    put(&b, 0U, 1);
    put(&b, 8U, 6);
    put(&b, 2U, 4);
    put_lu_demand(&b, 1U, cipher_params(0U, 0U, 0U), 1);
    pad(&b, SCH_F_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 1 && u.addr == 7U);
    assert(u.lu_demand && u.cipher_params == cipher_params(1U, 0U, 0U));
}

/* MAC-DATA on SCH/F: the length indication's reserved, QAM-only and stealing-channel-only values, the Null PDU, the
 * capacity request with its reserved bit, and the other PDU types. */
static void
test_mac_data(void) {
    bitbuf b;
    tetra_ul_reading u;
    DSD_MEMSET(&b, 0, sizeof b);
    put(&b, 0U, 2);
    put(&b, 1U, 1); /* fill */
    put(&b, 0U, 1);
    put(&b, 0U, 2);
    put(&b, 0x00ABCDU, 24);
    put(&b, 1U, 1); /* capacity request */
    put(&b, 0U, 1);
    put(&b, 3U, 4);
    put(&b, 1U, 1); /* reserved: not used in this edition, ignored */
    put(&b, 0U, 4); /* BL-ADATA */
    put(&b, 0U, 2);
    put_lu_demand(&b, 1U, cipher_params(2U, 0U, 31U), 20);
    pad(&b, SCH_F_BITS, 1);
    assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 1 && u.kind == TETRA_UL_PDU_DATA && u.addr == 0x00ABCDU);
    assert(u.lu_demand && u.cipher_params == cipher_params(2U, 0U, 31U) && !u.fragment);

    /* Length indications: 1, 57..60 reserved, 61 QAM, 62 and 63 stealing channel only. */
    static const uint32_t k_bad[] = {1U, 57U, 60U, 61U, 62U, 63U};
    for (size_t i = 0; i < sizeof k_bad / sizeof k_bad[0]; i++) {
        bitbuf c;
        DSD_MEMSET(&c, 0, sizeof c);
        put(&c, 0U, 6);
        put(&c, 7U, 24);
        put(&c, 0U, 1);
        put(&c, k_bad[i], 6);
        pad(&c, SCH_F_BITS, 0);
        assert(tetra_ul_read(TETRA_UL_SCH_F, c.bits, c.n, &u) == 0 && !u.header_ok);
    }
    /* Null PDU, encrypted. */
    DSD_MEMSET(&b, 0, sizeof b);
    put(&b, 0U, 3);
    put(&b, 1U, 1);
    put(&b, 0U, 2);
    put(&b, 7U, 24);
    put(&b, 0U, 1);
    put(&b, 0U, 6);
    pad(&b, SCH_F_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 1 && u.null_pdu && u.encrypted && !u.mm);

    /* 01 MAC-FRAG/MAC-END, 10 broadcast (downlink only), 11 MAC-U-BLCK. */
    static const struct {
        uint32_t type;
        uint8_t kind;
    } k_types[] = {{1U, TETRA_UL_PDU_FRAG_END}, {2U, TETRA_UL_PDU_UNREAD}, {3U, TETRA_UL_PDU_SUPPLEMENTARY}};

    for (size_t i = 0; i < sizeof k_types / sizeof k_types[0]; i++) {
        DSD_MEMSET(&b, 0, sizeof b);
        put(&b, k_types[i].type, 2);
        pad(&b, SCH_F_BITS, 0);
        assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 0 && u.kind == k_types[i].kind && !u.header_ok);
    }
}

/* The LLC: a whole FCS variant is read only when its FCS matches; advanced link and non-MM PDUs are not read; an MM
 * PDU of another type reads its type only. */
static void
test_llc_and_mle(void) {
    bitbuf b;
    tetra_ul_reading u;
    bitbuf tl;
    DSD_MEMSET(&tl, 0, sizeof tl);
    put_lu_demand(&tl, 1U, cipher_params(1U, 0U, 4U), 1); /* 23 bits: the PDU is 12 octets */
    const uint32_t fcs = tetra_llc_fcs32(tl.bits, tl.n);
    DSD_MEMSET(&b, 0, sizeof b);
    put(&b, 0U, 6);
    put(&b, 7U, 24);
    put(&b, 0U, 1);
    put(&b, 0U, 6); /* the length, set once the PDU is built */
    const int header = b.n;
    put(&b, 6U, 4); /* BL-UDATA-FCS */
    for (int i = 0; i < tl.n; i++) {
        put(&b, tl.bits[i], 1);
    }
    put(&b, fcs, 32);
    const int pdu = b.n;
    assert((pdu % 8) == 0);
    set_bits(&b, header - 6, (uint32_t)(pdu / 8), 6);
    pad(&b, SCH_F_BITS, 0);
    assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 1 && u.lu_demand);
    assert(u.cipher_params == cipher_params(1U, 0U, 4U));
    b.bits[pdu - 1] ^= 1U;
    assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 1 && u.header_ok && !u.mm);
    b.bits[pdu - 1] ^= 1U;

    /* Advanced link (LLC PDU type 1000 and up): not read. */
    b.bits[header] = 1U;
    assert(tetra_ul_read(TETRA_UL_SCH_F, b.bits, b.n, &u) == 1 && !u.mm);

    /* CMCE, not MM: not read. An MM PDU of another type: its type only. */
    access_lu(&b, 0U, 1U, cipher_params(0U, 0U, 0U));
    b.bits[36 + 1] = 1U; /* protocol discriminator 001 -> 011 */
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && !u.mm);
    access_lu(&b, 0U, 1U, cipher_params(0U, 0U, 0U));
    b.bits[39 + 1] = 1U; /* MM type 0010 -> 0110, U-INFORMATION PROVIDE */
    assert(tetra_ul_read(TETRA_UL_SCH_HU, b.bits, b.n, &u) == 1 && u.mm && u.mm_type == 6U && !u.lu_demand);
}

static dsd_state*
cell_state(void) {
    dsd_state* st = (dsd_state*)calloc(1U, sizeof(dsd_state));
    assert(st != NULL);
    tetra_state_t* ts = tetra_st(st);
    ts->net_known = 1;
    ts->uplink = 1;
    ts->mcc = 262;
    ts->mnc = 1001;
    return st;
}

static void
free_state(dsd_state* st) {
    dsd_state_ext_free_all(st);
    free(st);
}

/* A block acted on: the uplink's encryption, and the proposed KSG for the serving network only, never as the
 * network's own algorithm. */
static void
test_block_notes_the_proposal(void) {
    bitbuf b;
    dsd_state* st = cell_state();
    tetra_state_t* ts = tetra_st(st);
    access_lu(&b, 0U, 1U, cipher_params(0U, 0U, 2U));
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(ts->ul_enc == TETRA_UL_ENC_CLEAR);
    assert(tetra_air_ksg_requested(st, 262, 1001) == 0x0001U && tetra_air_ksg_seen(st, 262, 1001) == 0U);
    assert(tetra_air_ksg_lookup(st, 262, 1001) == -1);

    /* Reserved KSG 4..7: nothing. Cipher control off: nothing. */
    access_lu(&b, 0U, 1U, cipher_params(5U, 0U, 0U));
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    access_lu(&b, 0U, 0U, 0U);
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(tetra_air_ksg_requested(st, 262, 1001) == 0x0001U);

    /* Encrypted: the state says so, nothing is noted. */
    access_lu(&b, 1U, 1U, cipher_params(1U, 0U, 0U));
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(ts->ul_enc == TETRA_UL_ENC_ENCRYPTED && tetra_air_ksg_requested(st, 262, 1001) == 0x0001U);

    /* A block not read leaves the encryption state as it was. */
    DSD_MEMSET(&b, 0, sizeof b);
    put(&b, 1U, 1);
    pad(&b, SCH_HU_BITS, 0);
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(ts->ul_enc == TETRA_UL_ENC_ENCRYPTED);

    /* No network known, or direct mode: nothing noted. */
    access_lu(&b, 0U, 1U, cipher_params(1U, 0U, 0U));
    ts->net_known = 0;
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    ts->net_known = 1;
    ts->dmo = 1;
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    ts->dmo = 0;
    assert(tetra_air_ksg_requested(st, 262, 1001) == 0x0001U);
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(tetra_air_ksg_requested(st, 262, 1001) == 0x0003U);
    free_state(st);
}

/* The summary line names an encrypted PDU's SSI as an encrypted short identity (EN 300 392-7 clause 4.2.6), never as
 * the subscriber's SSI; a clear one as the SSI. */
static void
test_summary_names_the_address(void) {
    bitbuf b;
    char text[512];
    dsd_state* st = cell_state();
    dsd_test_capture_stderr cap;
    assert(dsd_test_capture_stderr_begin(&cap, "tetra_ul_log") == 0);
    access_lu(&b, 1U, 1U, cipher_params(0U, 0U, 0U));
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(dsd_test_capture_stderr_end(&cap) == 0 && dsd_test_capture_stderr_read(&cap, text, sizeof text) == 0);
    assert(strstr(text, "MAC-ACCESS enc=1 esi=1193046") != NULL && strstr(text, "ssi=") == NULL);
    assert(dsd_test_capture_stderr_begin(&cap, "tetra_ul_log") == 0);
    access_lu(&b, 0U, 1U, cipher_params(0U, 0U, 0U));
    tetra_ul_block(st, TETRA_UL_SCH_HU, b.bits, b.n, 17);
    assert(dsd_test_capture_stderr_end(&cap) == 0 && dsd_test_capture_stderr_read(&cap, text, sizeof text) == 0);
    assert(strstr(text, "MAC-ACCESS enc=0 ssi=1193046") != NULL);
    assert(strstr(text, "[TETRA MM U-LU-DEMAND] CC=17 ssi=1193046 type=3 cipher=1 ksg=TEA1 class=2 sckn=1") != NULL);
    free_state(st);
}

int
main(void) {
    test_summary_names_the_address();
    test_access_reads_the_proposal();
    test_access_length_and_fragments();
    test_first_pdu_only();
    test_mac_data();
    test_llc_and_mle();
    test_block_notes_the_proposal();
    return 0;
}
