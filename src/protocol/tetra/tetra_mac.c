// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA downlink upper MAC (EN 300 392-2 clauses 21.4 and 23.4) and basic-link LLC (clauses 21.2 and 22.3).
 *
 * Input is the type-1 bits of one CRC-passed signalling block sent with pi/4-DQPSK modulation: SCH/HD and STCH
 * (124 bits) or SCH/F (268 bits). The block is split into its associated PDUs (clause 23.4.3.3); every PDU is read
 * only up to its own length, its fill bits are removed (clause 23.4.3.2) and its TM-SDU goes to the LLC, or into
 * the fragment context of the block's timeslot (clause 23.4.3.1.1). The LLC passes basic-link TL-SDUs whose FCS
 * checks to the MLE. TM-SDUs with an encryption mode other than 00 are ciphertext (EN 300 392-7 clause 6.7.1.2) and
 * are not parsed. The MAC-RESOURCE header, SYSINFO and ACCESS-DEFINE are read by pure decoders (tetra_mac_pdu.h).
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_bsch.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <stdint.h>
#include <stdio.h>

#include "tetra_calls.h"
#include "tetra_confirm.h"
#include "tetra_log.h"
#include "tetra_mac_pdu.h"
#include "tetra_state.h"

/* Size of the downlink Null PDU. A shorter remainder of a MAC block is fill (clause 23.4.3.3). */
#define MAC_NULL_PDU_BITS         16
/* Type-1 bits of a half-slot block (SCH/HD, STCH). */
#define MAC_HALF_SLOT_BITS        124
/* Length indication (tables 21.55 and 21.59). With pi/4-DQPSK Y2 = Z2 = 1 octet (table 21.98), so 1 to 58 is the
 * PDU length in octets and 0 and 59 to 61 are reserved. */
#define MAC_LI_MAX_OCTETS         58u
#define MAC_LI_SECOND_HALF_STOLEN 62u
#define MAC_LI_FRAGMENT_START     63u
/* N.203 (annex B.2): a partial TM-SDU is dropped after 4 slots of its control channel without a fragment. Counted
 * in half slots because SCH/HD blocks arrive per half slot. */
#define MAC_N203_HALF_SLOTS       8u
/* LLC frame check sequence (clause 21.1.2.3) and its generator polynomial without the x^32 term (equation C.1). */
#define LLC_FCS_BITS              32
#define LLC_FCS_POLYNOMIAL        0x04C11DB7u
/* Hex bytes in the -Z dump of a block; SCH/F has 268 bits. */
#define MAC_RAW_DUMP_BYTES        34

/* One MAC block being decoded. */
typedef struct {
    dsd_opts* opts;
    dsd_state* state;
    tetra_state_t* ts;
    int cc;
    int block_bits;           /* type-1 bits of the whole block */
    int first;                /* the PDU being decoded starts the block */
    int fragment_seen;        /* the block carried a fragment of its timeslot's TM-SDU */
    tetra_mac_fragment* frag; /* fragment context of the block's timeslot */
} mac_block;

/* 1 for the SSI family (SSI, USSI and their event label or usage marker forms), 2 for the SMI family, 0 for event
 * labels and the Null PDU. */
static int
address_family(uint8_t type) {
    switch (type) {
        case TETRA_MAC_ADDR_SSI:
        case TETRA_MAC_ADDR_USSI:
        case TETRA_MAC_ADDR_SSI_EVENT:
        case TETRA_MAC_ADDR_SSI_USAGE: return 1;
        case TETRA_MAC_ADDR_SMI:
        case TETRA_MAC_ADDR_SMI_EVENT: return 2;
        default: return 0;
    }
}

static int
address_has_event_label(uint8_t type) {
    return type == TETRA_MAC_ADDR_EVENT_LABEL || type == TETRA_MAC_ADDR_SSI_EVENT || type == TETRA_MAC_ADDR_SMI_EVENT;
}

/* Address element of table 21.55: the 24-bit SSI, USSI or SMI first, then the event label or usage marker. */
static void
read_address(tetra_bit_reader* r, uint8_t type, uint8_t enc_mode, tetra_mac_address* addr) {
    *addr = (tetra_mac_address){.type = type};
    if (address_family(type) != 0) {
        addr->ssi = tetra_bits_read(r, 24);
    }
    if (address_has_event_label(type)) {
        addr->event_label = (uint16_t)tetra_bits_read(r, 10);
    }
    if (type == TETRA_MAC_ADDR_SSI_USAGE) {
        addr->usage_marker = (uint8_t)tetra_bits_read(r, 6);
    }
    /* Encrypted signalling carries an encrypted short identity in place of the SSI; USSI, SMI, event labels and
     * usage markers stay clear (EN 300 392-7 clause 4.2.6). */
    addr->esi = (uint8_t)(enc_mode != TETRA_ENC_MODE_NONE
                          && (type == TETRA_MAC_ADDR_SSI || type == TETRA_MAC_ADDR_SSI_EVENT
                              || type == TETRA_MAC_ADDR_SSI_USAGE));
}

static void
format_address(const tetra_mac_address* addr, char* out, size_t cap) {
    const char* id = "SSI";
    if (addr->esi) {
        id = "ESI";
    } else if (address_family(addr->type) == 2) {
        id = "SMI";
    } else if (addr->type == TETRA_MAC_ADDR_USSI) {
        id = "USSI";
    }
    switch (addr->type) {
        case TETRA_MAC_ADDR_EVENT_LABEL: DSD_SNPRINTF(out, cap, "EventLabel(%u)", (unsigned)addr->event_label); break;
        case TETRA_MAC_ADDR_SSI_EVENT:
        case TETRA_MAC_ADDR_SMI_EVENT:
            DSD_SNPRINTF(out, cap, "%s(%u)+EventLabel(%u)", id, (unsigned)addr->ssi, (unsigned)addr->event_label);
            break;
        case TETRA_MAC_ADDR_SSI_USAGE:
            DSD_SNPRINTF(out, cap, "%s(%u)+UsageMarker(%u)", id, (unsigned)addr->ssi, (unsigned)addr->usage_marker);
            break;
        default: DSD_SNPRINTF(out, cap, "%s(%u)", id, (unsigned)addr->ssi); break;
    }
}

/* Fill bit deletion, clause 23.4.3.2: with the fill bit indication set, the last 1 bit of the PDU and the 0 bits
 * after it are fill. Returns the end of the PDU content, or -1 when no 1 bit follows @p start. */
static int
content_end(const uint8_t* bits, int start, int end, uint32_t fill) {
    if (fill == 0u) {
        return end;
    }
    for (int i = end - 1; i >= start; i--) {
        if ((bits[i] & 1u) != 0u) {
            return i;
        }
    }
    return -1;
}

/* Downlink frequency of carrier number @p carrier, or 0 when there is no valid one. Band 0 would place the carrier
 * below 102.4 MHz, where TETRA has none; SYSINFO sends main carrier 0 and band 0 when the cell has no CA main
 * carrier (table 21.65 note 3). */
static long
carrier_frequency_hz(uint32_t carrier, uint32_t band, uint32_t offset) {
    if (band == 0u) {
        return 0L;
    }
    const long hz = tetra_carrier_to_dl_hz(carrier, band, offset);
    return hz > 0L ? hz : 0L;
}

uint32_t
tetra_llc_fcs32(const uint8_t* bits, int nbits) {
    if (!bits || nbits < 0) {
        return 0;
    }
    /* Shifting each message bit into the top of the register while dividing yields M(x) x^32 mod G(x) directly. */
    uint32_t remainder = 0;
    for (int i = 0; i < nbits; i++) {
        uint32_t bit = (uint32_t)(bits[i] & 1u);
        if (i < 32) {
            bit ^= 1u; /* step 1: complement the first 32 bits */
        }
        const uint32_t feedback = (remainder >> 31) ^ bit;
        remainder <<= 1;
        if (feedback != 0u) {
            remainder ^= LLC_FCS_POLYNOMIAL;
        }
    }
    return ~remainder; /* step 5 */
}

/* Basic-link PDUs, tables 21.1 and 21.4 to 21.11. */
typedef struct {
    const char* name;
    uint8_t header_bits;  /* LLC PDU type plus N(R) and N(S) where present */
    uint8_t fcs;          /* a 32-bit FCS follows the TL-SDU */
    uint8_t sdu_optional; /* BL-ACK without FCS may carry no TL-SDU */
} llc_basic_link_pdu;

static const llc_basic_link_pdu k_basic_link[8] = {
    {"BL-ADATA", 6, 0, 0},     {"BL-DATA", 5, 0, 0},     {"BL-UDATA", 4, 0, 0},     {"BL-ACK", 5, 0, 1},
    {"BL-ADATA-FCS", 6, 1, 0}, {"BL-DATA-FCS", 5, 1, 0}, {"BL-UDATA-FCS", 4, 1, 0}, {"BL-ACK-FCS", 5, 1, 0},
};

/* Advanced-link and layer 2 signalling PDUs, LLC PDU types 1000 to 1111 of table 21.1. */
static const char* const k_llc_other[8] = {
    "AL-SETUP",     "AL-DATA/AL-FINAL",      "AL-UDATA/AL-UFINAL",     "AL-ACK/AL-RNR",
    "AL-RECONNECT", "supplementary LLC PDU", "layer 2 signalling PDU", "AL-DISC",
};

/* Decapsulate an LLC PDU (clause 21.2) and hand a basic-link TL-SDU to the MLE, with the MAC context @p sdu of the PDU
 * that carried it. Advanced-link segments and layer 2 signalling are not TL-SDUs the MLE can use. */
static void
llc_deliver(const mac_block* b, const uint8_t* pdu, int nbits, tetra_mac_sdu* sdu_ctx) {
    tetra_bit_reader r = tetra_bits_reader(pdu, nbits, 0);
    const uint32_t type = tetra_bits_read(&r, 4);
    if (!tetra_bits_ok(&r)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA LLC] CC=%d  %d-bit TM-SDU has no LLC header\n", b->cc, nbits);
        return;
    }
    if (type >= 8u) {
        DSD_FPRINTF(stderr, "[TETRA LLC %s] CC=%d  %d bits; advanced link not decoded\n", k_llc_other[type - 8u], b->cc,
                    nbits);
        return;
    }
    const llc_basic_link_pdu* bl = &k_basic_link[type];
    const int fcs_bits = bl->fcs ? LLC_FCS_BITS : 0;
    const int sdu_bits = nbits - bl->header_bits - fcs_bits;
    if (sdu_bits <= 0) {
        if (sdu_bits < 0 || !bl->sdu_optional) {
            TETRA_LOG_PAYLOAD(b->ts, "[TETRA LLC %s] CC=%d  truncated (%d bits); discarded\n", bl->name, b->cc, nbits);
        }
        return;
    }
    const uint8_t* sdu = pdu + bl->header_bits;
    if (bl->fcs) {
        tetra_bit_reader f = tetra_bits_reader(pdu, nbits, bl->header_bits + sdu_bits);
        const uint32_t received = tetra_bits_read(&f, LLC_FCS_BITS);
        const uint32_t expected = tetra_llc_fcs32(sdu, sdu_bits);
        if (received != expected) {
            TETRA_LOG_PAYLOAD(b->ts, "[TETRA LLC %s] CC=%d  FCS mismatch rx=0x%08X calc=0x%08X; discarded\n", bl->name,
                              b->cc, (unsigned)received, (unsigned)expected);
            return;
        }
    }
    TETRA_LOG_PAYLOAD(b->ts, "[TETRA LLC %s] CC=%d  tl_sdu_bits=%d\n", bl->name, b->cc, sdu_bits);
    tetra_mle_dispatch_sdu(sdu, sdu_bits, b->cc, sdu_ctx, b->opts, b->state);
}

/* Hand a complete clear TM-SDU to the LLC. The channel allocation that came with it (in @p ctx) goes to the call a call
 * PDU in it names, and is followed only then; one no call PDU took attaches to the call of the addressed party, and a
 * header-only allocation likewise, without tuning (tetra_calls.h). */
static void
deliver_tm_sdu(const mac_block* b, const uint8_t* sdu, int sdu_bits, tetra_mac_sdu* ctx) {
    if (sdu_bits > 0) {
        llc_deliver(b, sdu, sdu_bits, ctx);
    }
    tetra_calls_on_allocation(b->opts, b->state, ctx);
}

/* Augmented part of table 21.87 (up/downlink assigned 00). Returns 0 when the receiver discards the allocation and
 * the TM-SDU with it: reserved augmented up/downlink value 00 (note 5) or further augmentation (note 15). */
static int
read_augmented_allocation(tetra_bit_reader* r, tetra_chan_alloc* a) {
    a->up_down = (uint8_t)tetra_bits_read(r, 2);
    a->bandwidth = (uint8_t)tetra_bits_read(r, 3);
    a->modulation = (uint8_t)tetra_bits_read(r, 3);
    /* Maximum uplink QAM modulation level or reserved, conforming channel status, BS link imbalance, BS transmit
     * power relative to main carrier. */
    (void)tetra_bits_skip(r, 3 + 3 + 4 + 5);
    if (tetra_bits_read(r, 2) == 1u) {
        (void)tetra_bits_skip(r, 11); /* napping information */
    }
    (void)tetra_bits_skip(r, 4); /* reserved */
    if (tetra_bits_read(r, 1) != 0u) {
        (void)tetra_bits_skip(r, 16); /* conditional element A */
    }
    if (tetra_bits_read(r, 1) != 0u) {
        (void)tetra_bits_skip(r, 16); /* conditional element B (note 14) */
    }
    const uint32_t further = tetra_bits_read(r, 1);
    return tetra_bits_ok(r) && a->up_down != 0u && further == 0u;
}

/* Channel allocation element, table 21.87. Without extended carrier numbering the band and offset are those of the
 * cell's SYSINFO (clause 21.5.2). Returns 1 for a usable allocation; 0 when it runs past the PDU or is discarded. */
static int
read_allocation(tetra_bit_reader* r, const tetra_state_t* ts, tetra_chan_alloc* a) {
    static const long k_bandwidth_offset_hz[4] = {0L, 12500L, 37500L, 62500L}; /* (bandwidth / 2) - 12,5 kHz */
    *a = (tetra_chan_alloc){0};
    a->type = (uint8_t)tetra_bits_read(r, 2);
    a->timeslots = (uint8_t)tetra_bits_read(r, 4);
    a->up_down = (uint8_t)tetra_bits_read(r, 2);
    (void)tetra_bits_skip(r, 2); /* CLCH permission, cell change flag */
    a->carrier = (uint16_t)tetra_bits_read(r, 12);
    uint32_t band = ts->freq_band;
    uint32_t offset = ts->freq_offset;
    if (tetra_bits_read(r, 1) != 0u) {
        band = tetra_bits_read(r, 4);
        offset = tetra_bits_read(r, 2);
        (void)tetra_bits_skip(r, 4); /* duplex spacing, reverse operation */
    }
    if (tetra_bits_read(r, 2) == 0u) {
        (void)tetra_bits_skip(r, 2); /* frame 18 monitoring pattern */
    }
    if (a->up_down == 0u && !read_augmented_allocation(r, a)) {
        return 0;
    }
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    a->freq_hz = carrier_frequency_hz(a->carrier, band, offset);
    if (a->freq_hz > 0L && a->bandwidth < 4u) {
        a->freq_hz += k_bandwidth_offset_hz[a->bandwidth];
    }
    return 1;
}

static void
print_allocation(const mac_block* b, const tetra_chan_alloc* a) {
    DSD_FPRINTF(stderr, "[TETRA CHANNEL-ALLOCATION] CC=%d type=%u slots=0x%X dir=%u carrier=%u freq=%ld bw=%u mod=%u\n",
                b->cc, (unsigned)a->type, (unsigned)a->timeslots, (unsigned)a->up_down, (unsigned)a->carrier,
                a->freq_hz, (unsigned)a->bandwidth, (unsigned)a->modulation);
}

/* The fragment context of the block being decoded: its TDMA timeslot 1..4, or 0 while the clock is not running. */
static int
fragment_timeslot(const tetra_state_t* ts) {
    return ts->tdma_valid && ts->tn >= 1u && ts->tn <= 4u ? (int)ts->tn : 0;
}

/* Publish the encryption mode of an addressed PDU, and tell the call records (tetra_calls.h): an event label or usage
 * marker the address assigns, an encrypted allocation to a call's party, and the timeslot's signalling encryption. */
static void
publish_address(const mac_block* b, const tetra_mac_address* addr, uint8_t enc_mode, int alloc_present) {
    tetra_state_t* const ts = b->ts;
    ts->enc_mode = enc_mode;
    tetra_calls_on_addressed(b->opts, b->state, addr, enc_mode, alloc_present, (uint8_t)fragment_timeslot(ts));
}

static void
publish_resource_header(const mac_block* b, const tetra_mac_resource* h) {
    publish_address(b, &h->addr, h->enc_mode, h->alloc_present);
}

static void
discard_fragment(tetra_mac_fragment* f) {
    f->active = 0;
    f->enc_mode = TETRA_ENC_MODE_NONE;
    f->idle_half_slots = 0;
    f->nbits = 0;
}

static int
fragment_append(tetra_mac_fragment* f, const uint8_t* bits, int nbits) {
    if (nbits < 0 || nbits > (int)sizeof f->bits - (int)f->nbits) {
        return 0;
    }
    if (nbits > 0) {
        DSD_MEMCPY(f->bits + f->nbits, bits, (size_t)nbits);
        f->nbits = (uint16_t)(f->nbits + nbits);
    }
    return 1;
}

/* Clause 23.4.3.1.1 b) and i): a MAC-RESOURCE with length indication 111111 starts a TM-SDU on its control channel
 * and drops any partial one there, whatever its address. Its encryption mode @p enc_mode applies to every fragment. A
 * TM-SDU is rebuilt on the timeslot it started on, which the TDMA clock gives: while the clock is stopped (a new
 * carrier before its BSCH) the block's timeslot is not known, a continuation of any timeslot could join it, so it is
 * not kept. */
static void
start_fragment(mac_block* b, const tetra_mac_address* addr, uint8_t enc_mode, const uint8_t* sdu, int sdu_bits) {
    tetra_mac_fragment* const f = b->frag;
    if (fragment_timeslot(b->ts) == 0) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  first fragment on no known timeslot; discarded\n", b->cc);
        b->fragment_seen = 1;
        discard_fragment(f);
        return;
    }
    if (f->active) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  partial TM-SDU of %u bits dropped\n", b->cc,
                          (unsigned)f->nbits);
    }
    discard_fragment(f);
    f->active = 1;
    f->enc_mode = (uint8_t)(enc_mode & 3u);
    f->cc = (int8_t)b->cc;
    f->addr = *addr;
    b->fragment_seen = 1;
    if (!fragment_append(f, sdu, sdu_bits)) {
        discard_fragment(f);
    }
}

/* A MAC-FRAG or MAC-END continues the TM-SDU under way on the block's timeslot, if any, from the same cell. */
static int
fragment_continues(mac_block* b, const char* name) {
    tetra_mac_fragment* const f = b->frag;
    if (!f->active) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA %s] CC=%d  no first fragment on this timeslot; discarded\n", name, b->cc);
        return 0;
    }
    if (f->cc != (int8_t)b->cc) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA %s] CC=%d  TM-SDU started on CC=%d; discarded\n", name, b->cc, (int)f->cc);
        discard_fragment(f);
        return 0;
    }
    b->fragment_seen = 1;
    f->idle_half_slots = 0;
    return 1;
}

static void
print_resource(const mac_block* b, const tetra_mac_resource* h, int sdu_bits) {
    char addr[64];
    const char* note = "";
    format_address(&h->addr, addr, sizeof addr);
    if (h->enc_mode != TETRA_ENC_MODE_NONE && (sdu_bits > 0 || h->alloc_present)) {
        note = h->alloc_present ? "  channel allocation and TM-SDU encrypted; not decoded" : "  encrypted; not decoded";
    } else if (h->length_ind == MAC_LI_FRAGMENT_START) {
        note = "  first fragment";
    }
    DSD_FPRINTF(stderr, "[TETRA MAC-RESOURCE] CC=%d  enc=%s  rand_acc=%u  len_ind=%u  addr=%s  sdu_bits=%d%s\n", b->cc,
                tetra_enc_mode_name(h->enc_mode), (unsigned)h->random_access, (unsigned)h->length_ind, addr, sdu_bits,
                note);
}

/* PDU length of a MAC-RESOURCE in a block with @p nbits bits left: the indicated length bounded by the block
 * (clause 23.4.3.3), the whole remainder for 111110 and 111111, or -1 for a reserved value. */
static int
resource_pdu_bits(uint32_t length_ind, int nbits) {
    if (length_ind == MAC_LI_SECOND_HALF_STOLEN || length_ind == MAC_LI_FRAGMENT_START) {
        return nbits;
    }
    if (length_ind == 0u || length_ind > MAC_LI_MAX_OCTETS) {
        return -1;
    }
    const int indicated = (int)length_ind * 8;
    return indicated < nbits ? indicated : nbits;
}

/* Table 21.55 up to the channel allocation flag. Every optional element is read inside the PDU length. */
int
tetra_mac_resource_decode(const uint8_t* bits, int nbits, tetra_mac_resource* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    if (tetra_bits_read(&r, 2) != TETRA_MAC_TYPE_RESOURCE) {
        return 0;
    }
    out->fill = (uint8_t)tetra_bits_read(&r, 1);
    out->grant_pos = (uint8_t)tetra_bits_read(&r, 1);
    out->enc_mode = (uint8_t)tetra_bits_read(&r, 2);
    out->random_access = (uint8_t)tetra_bits_read(&r, 1);
    out->length_ind = (uint8_t)tetra_bits_read(&r, 6);
    const uint8_t type = (uint8_t)tetra_bits_read(&r, 3);
    if (!tetra_bits_ok(&r) || type == TETRA_MAC_ADDR_NULL) {
        return 0;
    }
    out->addr.type = type;
    out->pdu_bits = resource_pdu_bits(out->length_ind, nbits);
    if (out->pdu_bits < 0) {
        return 0;
    }
    r = tetra_bits_reader(bits, out->pdu_bits, r.pos);
    read_address(&r, type, out->enc_mode, &out->addr);
    if (tetra_bits_read(&r, 1) != 0u) {
        (void)tetra_bits_skip(&r, 4); /* power control element, table 21.92 */
    }
    if (tetra_bits_read(&r, 1) != 0u) {
        (void)tetra_bits_skip(&r, 8); /* basic slot granting element, table 21.96 */
    }
    out->alloc_present = (uint8_t)tetra_bits_read(&r, 1);
    out->body = r.pos;
    return tetra_bits_ok(&r);
}

/* The MAC-RESOURCE header (tetra_mac_resource_decode()). Returns 0 for a Null PDU or a header that does not fit:
 * nothing more of the block is decoded then, since no PDU follows a Null PDU in its block (clause 23.4.3.3). */
static int
read_resource_header(const mac_block* b, const uint8_t* bits, int nbits, tetra_mac_resource* h) {
    if (tetra_mac_resource_decode(bits, nbits, h)) {
        return 1;
    }
    if (h->addr.type == TETRA_MAC_ADDR_NULL) {
        return 0;
    }
    if (h->pdu_bits < 0) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  reserved length indication %u\n", b->cc,
                          (unsigned)h->length_ind);
    } else {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  header longer than the %d-bit PDU\n", b->cc, h->pdu_bits);
    }
    return 0;
}

/* An encrypted PDU, or the MAC-END of a TM-SDU encrypted from its first fragment, delivered as far as the call records
 * can read it: its clear address, and whether it carried a channel allocation, which is ciphertext (EN 300 392-7 clause
 * 6.7.1.2). What the address assigns takes effect then, as for a clear PDU (tetra_calls_on_allocation()). */
static void
deliver_encrypted(const mac_block* b, const tetra_mac_address* addr, uint32_t alloc_present) {
    tetra_mac_sdu ctx = {
        .addr = *addr,
        .has_addr = 1,
        .has_alloc = (uint8_t)(alloc_present != 0u),
        .alloc_unread = (uint8_t)(alloc_present != 0u),
    };
    tetra_calls_on_allocation(b->opts, b->state, &ctx);
}

/* MAC-RESOURCE whose encryption mode is not 00: everything after the channel allocation flag is ciphertext
 * (EN 300 392-7 clause 6.7.1.2), so neither the allocation nor the TM-SDU is read. A first fragment is delivered at its
 * MAC-END. */
static void
resource_encrypted(mac_block* b, const tetra_mac_resource* h, int sdu_bits) {
    print_resource(b, h, sdu_bits);
    publish_resource_header(b, h);
    if (h->length_ind == MAC_LI_FRAGMENT_START) {
        start_fragment(b, &h->addr, h->enc_mode, NULL, 0);
    } else {
        deliver_encrypted(b, &h->addr, h->alloc_present);
    }
}

/* MAC-RESOURCE, clause 21.4.3.1. Returns the PDU length, or 0 when nothing more of the block can be decoded. */
static int
parse_mac_resource(mac_block* b, const uint8_t* bits, int nbits) {
    tetra_mac_resource h = {0};
    if (!read_resource_header(b, bits, nbits, &h)) {
        return 0;
    }
    b->ts->frames_resource++;
    tetra_bit_reader r = tetra_bits_reader(bits, h.pdu_bits, h.body);
    if (h.enc_mode != TETRA_ENC_MODE_NONE) {
        const int end = content_end(bits, h.body, h.pdu_bits, h.fill);
        resource_encrypted(b, &h, end > h.body ? end - h.body : 0);
        return h.pdu_bits;
    }
    tetra_mac_sdu ctx = {
        .addr = h.addr,
        .has_addr = 1,
        .has_alloc = h.alloc_present,
    };
    if (h.alloc_present && !read_allocation(&r, b->ts, &ctx.alloc)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA CHANNEL-ALLOCATION] CC=%d  invalid or reserved allocation; PDU discarded\n",
                          b->cc);
        return h.pdu_bits;
    }
    const int end = content_end(bits, r.pos, h.pdu_bits, h.fill);
    if (end < 0) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  fill bits without a final 1 bit; discarded\n", b->cc);
        return h.pdu_bits;
    }
    print_resource(b, &h, end - r.pos);
    if (h.length_ind == MAC_LI_FRAGMENT_START) {
        /* A fragmented TM-SDU carries its channel allocation in the MAC-END (clause 23.4.2.1.1). */
        if (h.alloc_present) {
            TETRA_LOG_VERBOSE(b->ts, 1, "[TETRA MAC-RESOURCE] CC=%d  allocation in a first fragment ignored\n", b->cc);
        }
        publish_resource_header(b, &h);
        start_fragment(b, &h.addr, TETRA_ENC_MODE_NONE, bits + r.pos, end - r.pos);
        return h.pdu_bits;
    }
    if (h.alloc_present) {
        print_allocation(b, &ctx.alloc);
    }
    publish_resource_header(b, &h);
    deliver_tm_sdu(b, bits + r.pos, end - r.pos, &ctx);
    return h.pdu_bits;
}

/* MAC-FRAG, table 21.57. It is the last PDU of its block (clause 23.4.2.1.1). */
static int
parse_mac_frag(mac_block* b, const uint8_t* bits, int nbits) {
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 3);
    const uint32_t fill = tetra_bits_read(&r, 1);
    if (!tetra_bits_ok(&r) || !fragment_continues(b, "MAC-FRAG")) {
        return 0;
    }
    tetra_mac_fragment* const f = b->frag;
    if (f->enc_mode != TETRA_ENC_MODE_NONE) {
        return nbits;
    }
    const int end = content_end(bits, r.pos, nbits, fill);
    if (end < 0 || !fragment_append(f, bits + r.pos, end - r.pos)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-FRAG] CC=%d  malformed or oversized fragment; TM-SDU dropped\n", b->cc);
        discard_fragment(f);
        return nbits;
    }
    TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-FRAG] CC=%d  %d bits, %u rebuilt\n", b->cc, end - r.pos, (unsigned)f->nbits);
    return nbits;
}

/* Deliver the rebuilt TM-SDU of the block's timeslot. Its address is that of the MAC-RESOURCE that started it, and
 * the only allocation that applies is the one in this MAC-END. */
static void
complete_fragment(mac_block* b, const tetra_chan_alloc* alloc) {
    tetra_mac_fragment* const f = b->frag;
    tetra_mac_sdu ctx = {
        .addr = f->addr,
        .has_addr = 1,
        .has_alloc = alloc != NULL,
    };
    if (alloc) {
        ctx.alloc = *alloc;
        print_allocation(b, alloc);
    }
    publish_address(b, &f->addr, TETRA_ENC_MODE_NONE, alloc != NULL);
    TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  TM-SDU of %u bits rebuilt\n", b->cc, (unsigned)f->nbits);
    deliver_tm_sdu(b, f->bits, f->nbits, &ctx);
    discard_fragment(f);
}

/* The MAC-END after its slot granting and channel allocation flags (table 21.59). */
static void
finish_fragment(mac_block* b, const uint8_t* bits, int pdu_bits, int body, uint32_t fill, uint32_t alloc_present) {
    tetra_mac_fragment* const f = b->frag;
    if (f->enc_mode != TETRA_ENC_MODE_NONE) {
        /* The allocation and the last fragment are ciphertext like the rest (EN 300 392-7 clause 6.7.1.2). An
         * allocation there makes the traffic of the call of the addressed party air encrypted, as it does in an
         * encrypted MAC-RESOURCE that carries a whole TM-SDU (publish_resource_header()). */
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  encrypted TM-SDU ends\n", b->cc);
        if (alloc_present != 0u) {
            publish_address(b, &f->addr, f->enc_mode, 1);
        }
        deliver_encrypted(b, &f->addr, alloc_present);
        discard_fragment(f);
        return;
    }
    tetra_bit_reader r = tetra_bits_reader(bits, pdu_bits, body);
    tetra_chan_alloc alloc = {0};
    if (alloc_present != 0u && !read_allocation(&r, b->ts, &alloc)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA CHANNEL-ALLOCATION] CC=%d  invalid or reserved allocation; TM-SDU dropped\n",
                          b->cc);
        discard_fragment(f);
        return;
    }
    const int end = content_end(bits, r.pos, pdu_bits, fill);
    if (end < 0 || !fragment_append(f, bits + r.pos, end - r.pos)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  malformed or oversized fragment; TM-SDU dropped\n", b->cc);
        discard_fragment(f);
        return;
    }
    complete_fragment(b, alloc_present != 0u ? &alloc : NULL);
}

/* MAC-END, table 21.59 (pi/4-DQPSK: no immediate napping permission flag, basic slot granting only). Returns the PDU
 * length, or 0 when nothing more of the block can be decoded. */
static int
parse_mac_end(mac_block* b, const uint8_t* bits, int nbits) {
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 3);
    const uint32_t fill = tetra_bits_read(&r, 1);
    (void)tetra_bits_skip(&r, 1); /* position of grant */
    const uint32_t length_ind = tetra_bits_read(&r, 6);
    if (!tetra_bits_ok(&r)) {
        return 0;
    }
    if (length_ind == 0u || length_ind > MAC_LI_MAX_OCTETS) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  reserved length indication %u\n", b->cc, (unsigned)length_ind);
        return 0;
    }
    const int pdu_bits = (int)length_ind * 8 < nbits ? (int)length_ind * 8 : nbits;
    r = tetra_bits_reader(bits, pdu_bits, r.pos);
    if (tetra_bits_read(&r, 1) != 0u) {
        (void)tetra_bits_skip(&r, 8); /* basic slot granting element */
    }
    const uint32_t alloc_present = tetra_bits_read(&r, 1);
    if (!tetra_bits_ok(&r)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  header longer than the %d-bit PDU\n", b->cc, pdu_bits);
        return 0;
    }
    if (fragment_continues(b, "MAC-END")) {
        finish_fragment(b, bits, pdu_bits, r.pos, fill, alloc_present);
    }
    return pdu_bits;
}

/* SYSINFO fields of table 21.65 for pi/4-DQPSK, then the 42-bit D-MLE-SYSINFO TM-SDU (clause 18.4.2.2). */
enum {
    SI_MAIN_CARRIER,
    SI_BAND,
    SI_OFFSET,
    SI_DUPLEX,
    SI_REVERSE,
    SI_NUM_SCCH,
    SI_TXPWR,
    SI_RXLEV,
    SI_ACCESS_PARAMETER,
    SI_DL_TIMEOUT,
    SI_CCK_FLAG,
    SI_HF_OR_CCK,
    SI_OPTION_FLAG,
    SI_OPTION,
    SI_LA,
    SI_SUBSCRIBER_CLASS,
    SI_BS_SERVICE,
    SI_FIELDS,
};

static const uint8_t k_sysinfo_bits[SI_FIELDS] = {12, 4, 2, 3, 1, 2, 3, 4, 4, 4, 1, 16, 2, 20, 14, 16, 12};

/* SYSINFO, clause 21.4.4.1. With pi/4-DQPSK it fills the whole 124-bit block; @p r is after the broadcast type. */
static int
decode_sysinfo(tetra_bit_reader* r, tetra_mac_sysinfo* si) {
    uint32_t v[SI_FIELDS] = {0};
    for (int i = 0; i < SI_LA; i++) {
        v[i] = tetra_bits_read(r, k_sysinfo_bits[i]);
    }
    if (!tetra_bits_ok(r)) {
        return 0;
    }
    si->mle_present = (uint8_t)(tetra_bits_left(r) >= 42);
    if (si->mle_present) {
        for (int i = SI_LA; i < SI_FIELDS; i++) {
            v[i] = tetra_bits_read(r, k_sysinfo_bits[i]);
        }
    }
    si->main_carrier = (uint16_t)v[SI_MAIN_CARRIER];
    si->band = (uint8_t)v[SI_BAND];
    si->offset = (uint8_t)v[SI_OFFSET];
    si->duplex_spacing = (uint8_t)v[SI_DUPLEX];
    si->reverse = (uint8_t)v[SI_REVERSE];
    si->num_scch = (uint8_t)v[SI_NUM_SCCH];
    si->ms_txpwr_max = (uint8_t)v[SI_TXPWR];
    si->rxlev_access_min = (uint8_t)v[SI_RXLEV];
    si->access_parameter = (uint8_t)v[SI_ACCESS_PARAMETER];
    si->radio_dl_timeout = (uint8_t)v[SI_DL_TIMEOUT];
    si->cck_flag = (uint8_t)v[SI_CCK_FLAG];
    si->hyperframe_or_cck = (uint16_t)v[SI_HF_OR_CCK];
    si->option_flag = (uint8_t)v[SI_OPTION_FLAG];
    si->option = v[SI_OPTION];
    si->la = (uint16_t)v[SI_LA];
    si->subscriber_class = (uint16_t)v[SI_SUBSCRIBER_CLASS];
    si->bs_service = (uint16_t)v[SI_BS_SERVICE];
    return 1;
}

/* ACCESS-DEFINE fields of table 21.79 before the optional field. */
enum {
    AD_ASSIGNED,
    AD_ACCESS_CODE,
    AD_IMM,
    AD_WT,
    AD_NU,
    AD_FRAME_LENGTH_FACTOR,
    AD_TIMESLOT_POINTER,
    AD_MIN_PRIORITY,
    AD_OPTION_FLAG,
    AD_FIELDS,
};

static const uint8_t k_access_define_bits[AD_FIELDS] = {1, 2, 4, 4, 4, 1, 4, 3, 2};

/* ACCESS-DEFINE, clause 21.4.4.3: 32, 48 or 56 bits; @p r is after the broadcast type. */
static int
decode_access_define(tetra_bit_reader* r, tetra_mac_access_define* ad) {
    uint32_t v[AD_FIELDS];
    for (int i = 0; i < AD_FIELDS; i++) {
        v[i] = tetra_bits_read(r, k_access_define_bits[i]);
    }
    if (v[AD_OPTION_FLAG] == 1u) {
        ad->subscriber_class = (uint16_t)tetra_bits_read(r, 16);
    } else if (v[AD_OPTION_FLAG] == 2u) {
        ad->gssi = tetra_bits_read(r, 24);
    }
    (void)tetra_bits_skip(r, 3); /* filler bits making the PDU octet bounded */
    ad->assigned = (uint8_t)v[AD_ASSIGNED];
    ad->access_code = (uint8_t)v[AD_ACCESS_CODE];
    ad->imm = (uint8_t)v[AD_IMM];
    ad->wait_time = (uint8_t)v[AD_WT];
    ad->num_ra = (uint8_t)v[AD_NU];
    ad->frame_length_factor = (uint8_t)v[AD_FRAME_LENGTH_FACTOR];
    ad->timeslot_pointer = (uint8_t)v[AD_TIMESLOT_POINTER];
    ad->min_priority = (uint8_t)v[AD_MIN_PRIORITY];
    ad->option_flag = (uint8_t)v[AD_OPTION_FLAG];
    ad->nbits = (uint8_t)r->pos;
    return tetra_bits_ok(r);
}

int
tetra_mac_broadcast_decode(const uint8_t* bits, int nbits, tetra_mac_broadcast* out) {
    if (!out) {
        return 0;
    }
    DSD_MEMSET(out, 0, sizeof *out);
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    if (tetra_bits_read(&r, 2) != TETRA_MAC_TYPE_BROADCAST) {
        return 0;
    }
    out->type = (uint8_t)tetra_bits_read(&r, 2);
    if (!tetra_bits_ok(&r)) {
        return 0;
    }
    switch (out->type) {
        case TETRA_MAC_BC_SYSINFO: return decode_sysinfo(&r, &out->sysinfo);
        case TETRA_MAC_BC_ACCESS_DEF: return decode_access_define(&r, &out->access_define);
        default: return 0;
    }
}

/* Cache SYSINFO, once a main carrier or location area other than the cell's has forgotten the last cell
 * (tetra_bsch_sysinfo_cell()). The control channel moves to the main carrier only when it is a valid frequency. */
static void
apply_sysinfo(const mac_block* b, const tetra_mac_sysinfo* si) {
    tetra_state_t* const ts = b->ts;
    const long dl_hz = carrier_frequency_hz(si->main_carrier, si->band, si->offset);
    tetra_bsch_sysinfo_cell(b->opts, b->state, dl_hz, si->mle_present, si->la);
    if (dl_hz > 0L) {
        ts->dl_carrier_hz = dl_hz;
        ts->freq_band = si->band;
        ts->freq_offset = si->offset;
        b->state->trunk_cc_freq = dl_hz;
        tetra_sm_on_cc_sync(b->opts, b->state);
    } else {
        TETRA_LOG_VERBOSE(b->ts, 1, "[TETRA SYSINFO] CC=%d  no valid main carrier; control channel unchanged\n", b->cc);
    }
    if (!si->cck_flag) {
        ts->hyperframe = si->hyperframe_or_cck;
        ts->hyperframe_valid = 1;
    }
    ts->sysinfo_main_carrier = si->main_carrier;
    if (si->mle_present) {
        ts->la = si->la;
        ts->sysinfo_known = 1;
        tetra_calls_note_location_area(b->opts, b->state, si->la);
    }
}

static void
report_sysinfo(const mac_block* b, const tetra_mac_sysinfo* si) {
    static const char* const k_option_name[4] = {"even-MF", "odd-MF", "access-code", "ext-svc"};
    char mle[64] = "";
    if (si->mle_present) {
        DSD_SNPRINTF(mle, sizeof mle, "  LA=%u  subscr_class=0x%04X  bs_svc_det=0x%03X", (unsigned)si->la,
                     (unsigned)si->subscriber_class, (unsigned)si->bs_service);
    }
    DSD_FPRINTF(stderr,
                "[TETRA SYSINFO] CC=%d  carrier=%u  band=%u  foff=%u  ds=%u  rev=%u  num_csch=%u  pwr=%u  rxlev=%u  "
                "acc=%u  dl_tmo=%u  %s=%u  opt=%s(0x%05X)%s\n",
                b->cc, (unsigned)si->main_carrier, (unsigned)si->band, (unsigned)si->offset,
                (unsigned)si->duplex_spacing, (unsigned)si->reverse, (unsigned)si->num_scch, (unsigned)si->ms_txpwr_max,
                (unsigned)si->rxlev_access_min, (unsigned)si->access_parameter, (unsigned)si->radio_dl_timeout,
                si->cck_flag ? "cck_id" : "hyperframe", (unsigned)si->hyperframe_or_cck,
                k_option_name[si->option_flag & 3u], (unsigned)si->option, mle);
    apply_sysinfo(b, si);
}

static void
report_access_define(const mac_block* b, const tetra_mac_access_define* ad) {
    char option[40] = "";
    if (ad->option_flag == 1u) {
        DSD_SNPRINTF(option, sizeof option, "  subscr_class=0x%04X", (unsigned)ad->subscriber_class);
    } else if (ad->option_flag == 2u) {
        DSD_SNPRINTF(option, sizeof option, "  gssi=%u", (unsigned)ad->gssi);
    }
    DSD_FPRINTF(stderr,
                "[TETRA ACCESS-DEFINE] CC=%d  %s  access_code=%c  imm=%u  wait=%u  num_ra=%u  frame_len_f=%u  "
                "ts_ptr=%u  min_prio=%u%s\n",
                b->cc, ad->assigned ? "assigned" : "common", (char)('A' + (int)ad->access_code), (unsigned)ad->imm,
                (unsigned)ad->wait_time, (unsigned)ad->num_ra, (unsigned)ad->frame_length_factor,
                (unsigned)ad->timeslot_pointer, (unsigned)ad->min_priority, option);
}

/* Broadcast PDUs, table 21.64. Returns the PDU length, or 0 when nothing more of the block can be decoded. */
static int
parse_broadcast(mac_block* b, const uint8_t* bits, int nbits) {
    if (nbits < 4) {
        return 0;
    }
    tetra_mac_broadcast bc;
    const int complete = tetra_mac_broadcast_decode(bits, nbits, &bc);
    if (bc.type == TETRA_MAC_BC_ACCESS_DEF) {
        if (!complete) {
            TETRA_LOG_PAYLOAD(b->ts, "[TETRA ACCESS-DEFINE] CC=%d  truncated\n", b->cc);
            return 0;
        }
        report_access_define(b, &bc.access_define);
        return bc.access_define.nbits;
    }
    if (bc.type == TETRA_MAC_BC_SYSINFO && b->first) {
        if (complete) {
            report_sysinfo(b, &bc.sysinfo);
        } else {
            TETRA_LOG_PAYLOAD(b->ts, "[TETRA SYSINFO] CC=%d  too short (%d bits)\n", b->cc, nbits);
        }
        /* Counted once it is taken: a SYSINFO of another cell first forgets the last cell's. */
        b->ts->frames_sysinfo++;
        return nbits;
    }
    /* A SYSINFO behind another PDU is not a pi/4-DQPSK SYSINFO; SYSINFO-DA and the reserved type have no length
     * this decoder knows. */
    TETRA_LOG_PAYLOAD(b->ts, "[TETRA BROADCAST] CC=%d  broadcast type %u not decoded\n", b->cc, (unsigned)bc.type);
    return 0;
}

/* MAC PDU type 11: MAC-U-SIGNAL on STCH (table 21.80), MAC-D-BLCK on a full slot (table 21.61). */
static int
parse_supplementary(mac_block* b, const uint8_t* bits, int nbits) {
    if (b->block_bits <= MAC_HALF_SLOT_BITS) {
        /* End-to-end U-plane signalling for the user application, not C-plane signalling. */
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-U-SIGNAL] CC=%d  %d bits of U-plane signalling\n", b->cc, nbits - 3);
        return nbits;
    }
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 2);
    const uint32_t subtype = tetra_bits_read(&r, 1);
    const uint32_t fill = tetra_bits_read(&r, 1);
    const uint32_t enc_mode = tetra_bits_read(&r, 2);
    const uint32_t event_label = tetra_bits_read(&r, 10);
    (void)tetra_bits_skip(&r, 1); /* immediate napping permission flag */
    if (tetra_bits_read(&r, 1) != 0u) {
        (void)tetra_bits_skip(&r, 8); /* basic slot granting element */
    }
    const int end = tetra_bits_ok(&r) && subtype == 0u ? content_end(bits, r.pos, nbits, fill) : -1;
    if (end < 0) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-D-BLCK] CC=%d  reserved subtype or malformed PDU\n", b->cc);
        return 0;
    }
    DSD_FPRINTF(stderr, "[TETRA MAC-D-BLCK] CC=%d  enc=%s  event_label=%u  sdu_bits=%d%s\n", b->cc,
                tetra_enc_mode_name((uint8_t)enc_mode), (unsigned)event_label, end - r.pos,
                enc_mode != TETRA_ENC_MODE_NONE ? "  encrypted; not decoded" : "");
    if (enc_mode == TETRA_ENC_MODE_NONE && end > r.pos) {
        /* MAC-D-BLCK addresses its party by event label alone. */
        tetra_mac_sdu ctx = {
            .addr = {.type = TETRA_MAC_ADDR_EVENT_LABEL, .event_label = (uint16_t)event_label},
            .has_addr = 1,
        };
        llc_deliver(b, bits + r.pos, end - r.pos, &ctx);
    }
    /* The implicit length of a pi/4-DQPSK MAC-D-BLCK is the rest of the block (clause 23.4.2.3). */
    return nbits;
}

/* Decode the PDU at the start of @p bits. Returns its length, or 0 when nothing more of the block can be decoded. */
static int
parse_pdu(mac_block* b, const uint8_t* bits, int nbits) {
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    const uint32_t type = tetra_bits_read(&r, 2);
    const uint32_t subtype = tetra_bits_read(&r, 1);
    switch (type) {
        case TETRA_MAC_TYPE_RESOURCE: return parse_mac_resource(b, bits, nbits);
        case TETRA_MAC_TYPE_FRAG_END:
            return subtype == TETRA_MAC_FRAGE_END ? parse_mac_end(b, bits, nbits) : parse_mac_frag(b, bits, nbits);
        case TETRA_MAC_TYPE_BROADCAST: return parse_broadcast(b, bits, nbits);
        default: return parse_supplementary(b, bits, nbits);
    }
}

static void
expire_if_idle(const tetra_state_t* ts, tetra_mac_fragment* f, int cc) {
    if (f->idle_half_slots >= MAC_N203_HALF_SLOTS) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA MAC] CC=%d  partial TM-SDU of %u bits timed out\n", cc, (unsigned)f->nbits);
        discard_fragment(f);
    }
}

/* N.203 counts the slots of the TM-SDU's control channel, decoded or not: before a block of timeslot TN is parsed, the
 * slots of that timeslot the TDMA clock counted since its last block, and that went by undecoded, count as slots
 * without a fragment, so a continuation arriving after them cannot revive an expired TM-SDU. While the clock is stopped
 * there is no timeslot to count, and no TM-SDU is started without one (start_fragment()). */
static void
catch_up_fragment(const tetra_state_t* ts, tetra_mac_fragment* f, int cc) {
    if (fragment_timeslot(ts) == 0) {
        return;
    }
    const uint32_t symbols = ts->tdma_slot_start - f->slot_start; /* both wrap with dsd_state::symbolcnt */
    f->slot_start = ts->tdma_slot_start;
    if (!f->active || symbols > UINT32_MAX / 2u) {
        return;
    }
    /* The timeslot comes round once every TDMA frame of four slots (EN 300 392-2 clause 7.3). */
    const uint32_t frames = (symbols + TETRA_TDMA_SLOT_SYMBOLS / 2u) / (4u * TETRA_TDMA_SLOT_SYMBOLS);
    if (frames <= 1u) {
        return;
    }
    /* Two half slots for each slot of the timeslot between the last block and this one. */
    const uint32_t idle = (uint32_t)f->idle_half_slots + (2u * (frames - 1u));
    f->idle_half_slots = (uint8_t)(idle < MAC_N203_HALF_SLOTS ? idle : MAC_N203_HALF_SLOTS);
    expire_if_idle(ts, f, cc);
}

/* N.203: drop a partial TM-SDU of this timeslot after 4 slots without a fragment (clause 23.4.3.1.1 iii). */
static void
age_fragment(mac_block* b) {
    tetra_mac_fragment* const f = b->frag;
    if (!f->active || b->fragment_seen) {
        return;
    }
    f->idle_half_slots = (uint8_t)(f->idle_half_slots + (b->block_bits > MAC_HALF_SLOT_BITS ? 2u : 1u));
    expire_if_idle(b->ts, f, b->cc);
}

static void
log_raw_block(const tetra_state_t* ts, const uint8_t* bits, int nbits) {
    if (!ts->log_payload) {
        return;
    }
    char hex[(3 * MAC_RAW_DUMP_BYTES) + 1] = "";
    size_t used = 0;
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 0);
    for (int i = 0; i < MAC_RAW_DUMP_BYTES && tetra_bits_left(&r) > 0; i++) {
        const int take = tetra_bits_left(&r) < 8 ? tetra_bits_left(&r) : 8;
        const uint32_t byte = tetra_bits_read(&r, take) << (8 - take);
        DSD_SNPRINTF(hex + used, sizeof hex - used, " %02X", (unsigned)byte);
        used += 3;
    }
    TETRA_LOG_PAYLOAD(ts, "[TETRA MAC raw] %d bits:%s\n", nbits, hex);
}

void
tetra_mac_block_failed(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || ts->uplink || fragment_timeslot(ts) == 0) {
        return;
    }
    tetra_mac_fragment* const f = &ts->mac_frag[fragment_timeslot(ts)];
    if (f->active) {
        TETRA_LOG_PAYLOAD(ts, "[TETRA MAC] partial TM-SDU of %u bits dropped: a block of its channel failed its CRC\n",
                          (unsigned)f->nbits);
        discard_fragment(f);
    }
}

void
tetra_mac_forget_fragments(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    for (size_t i = 0; i < sizeof ts->mac_frag / sizeof ts->mac_frag[0]; i++) {
        discard_fragment(&ts->mac_frag[i]);
    }
}

void
tetra_mac_parse_schd(const uint8_t* bits, int nbits, int cc, dsd_opts* opts, dsd_state* state) {
    tetra_state_t* const ts = tetra_st(state);
    if (!ts || !bits || nbits < 2) {
        return;
    }
    log_raw_block(ts, bits, nbits);
    if (ts->uplink) {
        /* Uplink blocks hold MAC-DATA, MAC-U-BLCK and uplink MAC-FRAG/MAC-END PDUs (clause 21.4.2), whose layouts
         * differ from the downlink ones decoded here. */
        TETRA_LOG_PAYLOAD(ts, "[TETRA MAC] CC=%d  uplink block of %d bits not decoded\n", cc, nbits);
        return;
    }
    mac_block b = {opts, state, ts, cc, nbits, 1, 0, &ts->mac_frag[fragment_timeslot(ts)]};
    catch_up_fragment(ts, b.frag, cc);
    int off = 0;
    while (nbits - off >= (b.first ? 2 : MAC_NULL_PDU_BITS)) {
        const int used = parse_pdu(&b, bits + off, nbits - off);
        /* A PDU that moved the receiver (a followed channel allocation) ends the block: the PDUs after it were sent
         * on the carrier the receiver left (tetra_confirm.h). */
        if (used <= 0 || !tetra_confirm_burst_current(state)) {
            break;
        }
        off += used;
        b.first = 0;
    }
    age_fragment(&b);
}

tetra_dl_usage
tetra_access_assign_dl_usage(uint16_t pdu, uint8_t* usage_marker) {
    const unsigned header = (pdu >> 12) & 3u;
    const unsigned field1 = (pdu >> 6) & 0x3Fu;
    tetra_dl_usage usage = TETRA_DL_TRAFFIC;
    /* Header 00 gives field 1 to the uplink's access rights; the other headers make it the downlink usage marker. */
    if (header == 0u || field1 == 2u) {
        usage = TETRA_DL_COMMON_CONTROL;
    } else if (field1 == 0u) {
        usage = TETRA_DL_UNALLOCATED;
    } else if (field1 == 1u) {
        usage = TETRA_DL_ASSIGNED_CONTROL;
    } else if (field1 == 3u) {
        usage = TETRA_DL_COMMON_ASSIGNED_CONTROL;
    }
    if (usage_marker) {
        *usage_marker = usage == TETRA_DL_TRAFFIC ? (uint8_t)field1 : 0u;
    }
    return usage;
}
