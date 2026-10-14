// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Builders for downlink TETRA signalling blocks in tests. Each writes the fields of an EN 300 392-2 table in order,
 * for pi/4-DQPSK (length indication unit Y2 = Z2 = 1 octet, table 21.98). They do not use the decoder.
 */
#ifndef DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_PDU_H_
#define DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_PDU_H_

#include <assert.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <stdint.h>

#define TTB_SCH_HD_BITS 124 /* SCH/HD and STCH type-1 bits */
#define TTB_SCH_F_BITS  268 /* SCH/F type-1 bits */

typedef struct {
    uint8_t bits[TTB_SCH_F_BITS];
    int n;
} ttb_block;

/* Append @p width bits of @p value, MSB first. */
static inline void
ttb_put(ttb_block* b, uint32_t value, int width) {
    assert(width >= 0 && width <= 32 && b->n + width <= TTB_SCH_F_BITS);
    for (int i = width - 1; i >= 0; i--) {
        b->bits[b->n++] = (uint8_t)((value >> i) & 1u);
    }
}

static inline void
ttb_put_bits(ttb_block* b, const uint8_t* bits, int nbits) {
    for (int i = 0; i < nbits; i++) {
        ttb_put(b, bits[i] & 1u, 1);
    }
}

/* Overwrite @p width bits at @p pos. */
static inline void
ttb_set(ttb_block* b, int pos, uint32_t value, int width) {
    for (int i = width - 1; i >= 0; i--) {
        b->bits[pos++] = (uint8_t)((value >> i) & 1u);
    }
}

/* MAC-RESOURCE header through the address (table 21.55). The fill bit indication and length indication are left 0
 * for ttb_close_resource(); a first fragment sets length indication 111111 itself and lets ttb_finish() set the fill
 * bit indication. For address types 2, 5, 6 and 7 the caller appends the event label or usage marker. Returns the
 * PDU start. */
static inline int
ttb_resource(ttb_block* b, uint32_t enc_mode, uint32_t addr_type, uint32_t ssi) {
    const int start = b->n;
    ttb_put(b, 0, 2);         /* MAC PDU type 00: MAC-RESOURCE */
    ttb_put(b, 0, 1);         /* fill bit indication */
    ttb_put(b, 0, 1);         /* position of grant */
    ttb_put(b, enc_mode, 2);  /* encryption mode */
    ttb_put(b, 0, 1);         /* random access flag */
    ttb_put(b, 0, 6);         /* length indication */
    ttb_put(b, addr_type, 3); /* address type */
    if (addr_type != 0u && addr_type != 2u) {
        ttb_put(b, ssi, 24); /* SSI, USSI or SMI */
    }
    return start;
}

/* Power control flag and slot granting flag, both 0, then the channel allocation flag (table 21.55). */
static inline void
ttb_resource_flags(ttb_block* b, uint32_t channel_allocation) {
    ttb_put(b, 0, 1);
    ttb_put(b, 0, 1);
    ttb_put(b, channel_allocation, 1);
}

/* Basic channel allocation element (table 21.87): no extended carrier numbering, one monitoring pattern. */
static inline void
ttb_allocation(ttb_block* b, uint32_t timeslots, uint32_t up_down, uint32_t carrier) {
    ttb_put(b, 0, 2);         /* allocation type: replace current channel */
    ttb_put(b, timeslots, 4); /* timeslot assigned */
    ttb_put(b, up_down, 2);   /* up/downlink assigned */
    ttb_put(b, 0, 1);         /* CLCH permission */
    ttb_put(b, 0, 1);         /* cell change flag */
    ttb_put(b, carrier, 12);  /* carrier number */
    ttb_put(b, 0, 1);         /* extended carrier numbering flag */
    ttb_put(b, 1, 2);         /* monitoring pattern: one */
}

/* Fill bit addition (clause 23.4.2.2) up to the next octet, then the PDU length in octets into the length indication
 * at @p li_pos and the fill bit indication at @p fill_pos. */
static inline void
ttb_close_pdu(ttb_block* b, int start, int fill_pos, int li_pos) {
    if ((b->n - start) % 8 != 0) {
        ttb_set(b, fill_pos, 1, 1);
        ttb_put(b, 1, 1);
        while ((b->n - start) % 8 != 0) {
            ttb_put(b, 0, 1);
        }
    }
    ttb_set(b, li_pos, (uint32_t)((b->n - start) / 8), 6);
}

static inline void
ttb_close_resource(ttb_block* b, int start) {
    ttb_close_pdu(b, start, start + 2, start + 7);
}

/* Fill bits after the last PDU of a block (clauses 23.4.2.2 and 23.4.2.3): a Null PDU when 16 bits are left, then a 1
 * bit and 0 bits to the end. With @p fill_pos >= 0 the PDU before them has no length indication (first fragment,
 * MAC-FRAG), so these are its own fill bits and that indication is set. */
static inline void
ttb_finish(ttb_block* b, int block_bits, int fill_pos) {
    if (fill_pos < 0 && block_bits - b->n >= 16) {
        ttb_put(b, 0, 2); /* MAC-RESOURCE */
        ttb_put(b, 0, 5); /* fill, grant position, encryption, random access */
        ttb_put(b, 2, 6); /* length indication: 2 octets */
        ttb_put(b, 0, 3); /* address type 000: Null PDU */
    }
    if (b->n < block_bits) {
        if (fill_pos >= 0) {
            ttb_set(b, fill_pos, 1, 1);
        }
        ttb_put(b, 1, 1);
        while (b->n < block_bits) {
            ttb_put(b, 0, 1);
        }
    }
}

/* MAC-FRAG header (table 21.57); returns the position of its fill bit indication. */
static inline int
ttb_frag(ttb_block* b) {
    ttb_put(b, 1, 2); /* MAC PDU type 01 */
    ttb_put(b, 0, 1); /* subtype: MAC-FRAG */
    ttb_put(b, 0, 1); /* fill bit indication */
    return b->n - 1;
}

/* MAC-END header through the slot granting flag (table 21.59, pi/4-DQPSK); the caller writes the channel allocation
 * flag. Returns the PDU start for ttb_close_end(). */
static inline int
ttb_end(ttb_block* b) {
    const int start = b->n;
    ttb_put(b, 1, 2); /* MAC PDU type 01 */
    ttb_put(b, 1, 1); /* subtype: MAC-END */
    ttb_put(b, 0, 1); /* fill bit indication */
    ttb_put(b, 0, 1); /* position of grant */
    ttb_put(b, 0, 6); /* length indication */
    ttb_put(b, 0, 1); /* slot granting flag */
    return start;
}

static inline void
ttb_close_end(ttb_block* b, int start) {
    ttb_close_pdu(b, start, start + 3, start + 5);
}

/* CMCE D-SETUP (table 14.15) behind its protocol discriminator (table 18.87): 44 bits. */
static inline void
ttb_d_setup(ttb_block* b, uint32_t call_id, uint32_t basic_service) {
    ttb_put(b, TETRA_MLE_PD_CMCE, 3);
    ttb_put(b, TETRA_CMCE_D_SETUP, 5);
    ttb_put(b, call_id, 14);
    ttb_put(b, 4, 4);             /* call time-out */
    ttb_put(b, 0, 1);             /* hook method selection */
    ttb_put(b, 0, 1);             /* simplex/duplex selection */
    ttb_put(b, basic_service, 8); /* table 14.35 */
    ttb_put(b, 0, 2);             /* transmission grant */
    ttb_put(b, 0, 1);             /* transmission request permission */
    ttb_put(b, 0, 4);             /* call priority */
    ttb_put(b, 0, 1);             /* O-bit: no optional elements */
}

/* The SYNC PDU of a BSCH (table 21.76, with the D-MLE-SYNC PDU of table 18.17): network @p mcc / @p mnc, colour code
 * @p colour, timeslot 1 of frame 1 of multiframe 1. 60 bits, for tetra_bsch_parse(). */
static inline void
ttb_sync_pdu(ttb_block* b, uint32_t mcc, uint32_t mnc, uint32_t colour) {
    ttb_put(b, 0x4u, 4); /* system code */
    ttb_put(b, colour, 6);
    ttb_put(b, 0, 2); /* timeslot 1 */
    ttb_put(b, 1, 5); /* frame 1 */
    ttb_put(b, 1, 6); /* multiframe 1 */
    ttb_put(b, 0, 8); /* sharing mode, reserved frames, U-plane DTX, frame 18 extension, reserved */
    ttb_put(b, mcc, 10);
    ttb_put(b, mnc, 14);
    ttb_put(b, 0, 5); /* neighbour cell broadcast, cell load CA, late entry supported */
}

/* SYSINFO (table 21.65, pi/4-DQPSK) of main carrier @p carrier in band @p band with offset @p offset, with its
 * D-MLE-SYSINFO TM-SDU (clause 18.4.2.2) naming location area @p la: 124 bits, a whole SCH/HD or BNCH block. */
static inline void
ttb_sysinfo(ttb_block* b, uint32_t carrier, uint32_t band, uint32_t offset, uint32_t la) {
    ttb_put(b, 2, 2); /* MAC PDU type: broadcast */
    ttb_put(b, 0, 2); /* broadcast type: SYSINFO */
    ttb_put(b, carrier, 12);
    ttb_put(b, band, 4);
    ttb_put(b, offset, 2);
    ttb_put(b, 5, 3);       /* duplex spacing */
    ttb_put(b, 0, 1);       /* reverse operation */
    ttb_put(b, 2, 2);       /* common secondary control channels */
    ttb_put(b, 6, 3);       /* MS_TXPWR_MAX_CELL */
    ttb_put(b, 9, 4);       /* RXLEV_ACCESS_MIN */
    ttb_put(b, 3, 4);       /* ACCESS_PARAMETER */
    ttb_put(b, 1, 4);       /* RADIO_DOWNLINK_TIMEOUT */
    ttb_put(b, 0, 1);       /* hyperframe number follows */
    ttb_put(b, 0x1234, 16); /* hyperframe number */
    ttb_put(b, 3, 2);       /* optional field flag: extended services broadcast */
    ttb_put(b, 0, 20);
    ttb_put(b, la, 14);     /* location area */
    ttb_put(b, 0xBEEF, 16); /* subscriber class */
    ttb_put(b, 0xA5C, 12);  /* BS service details */
}

/* Basic service information (table 14.35) for clear speech: point to point or point to multipoint. */
#define TTB_SPEECH_P2P   0x00u /* point to point */
#define TTB_SPEECH_GROUP 0x04u /* point to multipoint */

#endif /* DSD_NEO_TESTS_PROTOCOL_TETRA_TETRA_TEST_PDU_H_ */
