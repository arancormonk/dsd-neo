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
 * are counted, not parsed.
 */

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/talkgroup_policy.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_messages.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <stdint.h>
#include <stdio.h>

#include "tetra_confirm.h"
#include "tetra_log.h"
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

/* Channel allocation element, table 21.87. */
typedef struct {
    uint8_t type;       /* allocation type */
    uint8_t timeslots;  /* timeslot assigned */
    uint8_t up_down;    /* up/downlink assigned; for an augmented allocation, the augmented value */
    uint8_t bandwidth;  /* augmented allocations only; 0 is 25 kHz */
    uint8_t modulation; /* augmented allocations only; 0 is pi/4-DQPSK */
    uint16_t carrier;
    long freq_hz; /* downlink frequency, 0 when the band is unknown */
} mac_allocation;

/* MAC-RESOURCE header, table 21.55, up to the channel allocation flag. */
typedef struct {
    uint8_t fill;
    uint8_t grant_pos;
    uint8_t enc_mode;
    uint8_t random_access;
    uint8_t length_ind;
    uint8_t alloc_present;
    tetra_mac_address addr;
    int pdu_bits; /* PDU length in the block */
    int body;     /* first bit after the channel allocation flag */
} mac_resource_header;

static const char* const k_enc_mode_name[4] = {"none", "reserved", "on(even)", "on(odd)"};

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

/* Decapsulate an LLC PDU (clause 21.2) and hand a basic-link TL-SDU to the MLE. Advanced-link segments and layer 2
 * signalling are not TL-SDUs the MLE can use. */
static void
llc_deliver(const mac_block* b, const uint8_t* pdu, int nbits) {
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
    tetra_mle_dispatch(sdu, sdu_bits, b->cc, b->opts, b->state);
}

/* The talkgroup policy decision for the call the CMCE just published. */
static int
call_policy_allows(const mac_block* b) {
    const tetra_state_t* const ts = b->ts;
    dsd_tg_policy_decision decision;
    int rc;
    if (!b->opts || !ts->call_active || !ts->ssi_valid) {
        return 1;
    }
    const int encrypted = ts->enc_mode != TETRA_ENC_MODE_NONE;
    const int data_call = ts->call_type != 0;
    if (ts->cmce_com_type == 0) {
        rc = dsd_tg_policy_evaluate_private_call(b->opts, b->state, ts->calling_ssi, ts->active_ssi, encrypted,
                                                 data_call, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_group_call(b->opts, b->state, ts->gssi, ts->calling_ssi, encrypted, data_call,
                                               &decision);
    }
    if (rc != 0) {
        return 0;
    }
    if (!decision.tune_allowed) {
        TETRA_LOG_VERBOSE(ts, 1, "[TETRA CHANNEL-ALLOCATION] target=%u blocked by %s\n", decision.target_id,
                          dsd_tg_policy_block_reason_label(decision.block_reasons));
    }
    return decision.tune_allowed;
}

/* A downlink MAC-RESOURCE SSI is the called party of the CMCE call PDU it carries. Communication type 0 is point to
 * point; the other values are point to multipoint, acknowledged or broadcast (table 14.54). The address is bound
 * only after the CMCE published a complete call PDU, so truncated payloads cannot create a talkgroup. */
static void
sync_call_target(tetra_state_t* ts) {
    if (!ts->call_active || !ts->ssi_valid) {
        return;
    }
    ts->gssi = ts->cmce_com_type == 0 ? 0u : ts->active_ssi;
}

/* Hand the trunking state machine the traffic channel of @p a, for the call @p call_id (0 when none is known). */
static void
follow_allocation(const mac_block* b, const mac_allocation* a, uint16_t call_id) {
    const tetra_sm_grant grant = {a->freq_hz, a->carrier, a->timeslots, call_id};
    (void)tetra_sm_on_grant(b->opts, b->state, &grant);
}

/* An allocation whose PDU also carried a TM-SDU tunes only when that TM-SDU established a call the policy allows. */
static void
grant_with_call(const mac_block* b, const mac_allocation* a, uint32_t generation_before) {
    if (b->ts->cmce_call_generation != generation_before && call_policy_allows(b)) {
        follow_allocation(b, a, b->ts->call_id);
    }
}

/* Hand a complete clear TM-SDU to the LLC, then act on the downlink allocation that came with it, if any. */
static void
deliver_tm_sdu(const mac_block* b, const uint8_t* sdu, int sdu_bits, const mac_allocation* grant,
               uint32_t generation_before) {
    if (sdu_bits > 0) {
        llc_deliver(b, sdu, sdu_bits);
        sync_call_target(b->ts);
        if (grant) {
            grant_with_call(b, grant, generation_before);
        }
    } else if (grant) {
        follow_allocation(b, grant, 0U);
    }
}

/* Augmented part of table 21.87 (up/downlink assigned 00). Returns 0 when the receiver discards the allocation and
 * the TM-SDU with it: reserved augmented up/downlink value 00 (note 5) or further augmentation (note 15). */
static int
read_augmented_allocation(tetra_bit_reader* r, mac_allocation* a) {
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
read_allocation(tetra_bit_reader* r, const tetra_state_t* ts, mac_allocation* a) {
    static const long k_bandwidth_offset_hz[4] = {0L, 12500L, 37500L, 62500L}; /* (bandwidth / 2) - 12,5 kHz */
    *a = (mac_allocation){0};
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

/* @p addr is the party that holds the traffic-channel assignment: the same SSI or SMI, or an event label assigned
 * to it. */
static int
is_assignment_holder(const tetra_state_t* ts, const tetra_mac_address* addr) {
    const int family = address_family(addr->type);
    if (family != 0 && !addr->esi && family == address_family(ts->vc_addr.type) && addr->ssi == ts->vc_addr.ssi) {
        return 1;
    }
    return address_has_event_label(addr->type) && ts->vc_addr_event_label_valid
           && addr->event_label == ts->vc_addr.event_label;
}

static void
push_allocation_summary(const mac_block* b, const mac_allocation* a) {
    char summary[DSD_TETRA_MESSAGE_TEXT_SIZE];
    DSD_SNPRINTF(summary, sizeof summary, "Allocation CC=%d carrier=%u slots=0x%X freq=%ld", b->cc,
                 (unsigned)a->carrier, (unsigned)a->timeslots, a->freq_hz);
    tetra_message_push(b->state, DSD_TETRA_MESSAGE_CONTROL, summary);
}

/* Timeslot assigned 0000 sends the addressed party back to the common control channel (table 21.87). It ends the
 * followed assignment only when it is addressed to that assignment's holder. */
static void
release_assignment(const mac_block* b, const mac_allocation* a, const tetra_mac_address* addr) {
    tetra_state_t* const ts = b->ts;
    if (!ts->vc_assignment_valid || ts->vc_timeslot_bitmap == 0u || !is_assignment_holder(ts, addr)) {
        TETRA_LOG_VERBOSE(ts, 1, "[TETRA CHANNEL-ALLOCATION] CC=%d return to control channel for another party\n",
                          b->cc);
        return;
    }
    push_allocation_summary(b, a);
    ts->vc_assignment_type = a->type;
    ts->vc_uplink_downlink = a->up_down;
    ts->vc_timeslot_bitmap = 0;
    ts->vc_carrier = 0;
    ts->vc_freq_hz = 0;
    ts->tx_granted_valid = 0;
    tetra_sm_grant followed;
    if (tetra_sm_followed(b->state, &followed)) {
        tetra_sm_on_release(b->opts, b->state, followed.call_id);
    }
}

/* Record the allocation of a clear PDU addressed to @p addr. Returns 1 when it assigns a pi/4-DQPSK downlink the
 * trunking state machine can follow. */
static int
apply_allocation(const mac_block* b, const mac_allocation* a, const tetra_mac_address* addr) {
    tetra_state_t* const ts = b->ts;
    DSD_FPRINTF(stderr, "[TETRA CHANNEL-ALLOCATION] CC=%d type=%u slots=0x%X dir=%u carrier=%u freq=%ld bw=%u mod=%u\n",
                b->cc, (unsigned)a->type, (unsigned)a->timeslots, (unsigned)a->up_down, (unsigned)a->carrier,
                a->freq_hz, (unsigned)a->bandwidth, (unsigned)a->modulation);
    if (a->timeslots == 0u) {
        release_assignment(b, a, addr);
        return 0;
    }
    if (!ts->vc_assignment_valid || ts->vc_timeslot_bitmap != a->timeslots || ts->vc_carrier != a->carrier) {
        push_allocation_summary(b, a);
    }
    ts->vc_assignment_valid = 1;
    ts->vc_assignment_type = a->type;
    ts->vc_timeslot_bitmap = a->timeslots;
    ts->vc_uplink_downlink = a->up_down;
    ts->vc_carrier = a->carrier;
    ts->vc_freq_hz = a->freq_hz;
    ts->vc_addr = *addr;
    ts->vc_addr_event_label_valid = (uint8_t)address_has_event_label(addr->type);
    return (a->up_down == 1u || a->up_down == 3u) && a->freq_hz > 0L && a->modulation == 0u;
}

/* Publish the address and encryption mode of an addressed PDU. An event label assignment (address types 101 and
 * 111) to the holder of the traffic-channel assignment lets later PDUs address that holder by event label. */
static void
publish_address(tetra_state_t* ts, const tetra_mac_address* addr, uint8_t enc_mode) {
    ts->enc_mode = enc_mode;
    ts->ssi_valid = (uint8_t)(address_family(addr->type) != 0 && !addr->esi);
    if (ts->ssi_valid) {
        ts->active_ssi = addr->ssi;
    }
    ts->mac_addr_type = addr->type;
    ts->mac_event_label = addr->event_label;
    ts->mac_usage_marker = addr->usage_marker;
    if ((addr->type == TETRA_MAC_ADDR_SSI_EVENT || addr->type == TETRA_MAC_ADDR_SMI_EVENT) && !addr->esi
        && address_family(addr->type) == address_family(ts->vc_addr.type) && addr->ssi == ts->vc_addr.ssi) {
        ts->vc_addr.event_label = addr->event_label;
        ts->vc_addr_event_label_valid = 1;
    }
}

static void
publish_resource_header(tetra_state_t* ts, const mac_resource_header* h) {
    ts->mac_fill_bits = h->fill;
    ts->mac_grant_pos = h->grant_pos;
    ts->mac_rand_acc = h->random_access;
    ts->mac_len_ind = h->length_ind;
    publish_address(ts, &h->addr, h->enc_mode);
}

static void
discard_fragment(tetra_mac_fragment* f) {
    f->active = 0;
    f->encrypted = 0;
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
 * and drops any partial one there, whatever its address. The encryption mode applies to every fragment. */
static void
start_fragment(mac_block* b, const tetra_mac_address* addr, int encrypted, const uint8_t* sdu, int sdu_bits) {
    tetra_mac_fragment* const f = b->frag;
    if (f->active) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  partial TM-SDU of %u bits dropped\n", b->cc,
                          (unsigned)f->nbits);
    }
    discard_fragment(f);
    f->active = 1;
    f->encrypted = (uint8_t)encrypted;
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
print_resource(const mac_block* b, const mac_resource_header* h, int sdu_bits) {
    char addr[64];
    const char* note = "";
    format_address(&h->addr, addr, sizeof addr);
    if (h->enc_mode != TETRA_ENC_MODE_NONE && (sdu_bits > 0 || h->alloc_present)) {
        note = h->alloc_present ? "  channel allocation and TM-SDU encrypted; not decoded" : "  encrypted; not decoded";
    } else if (h->length_ind == MAC_LI_FRAGMENT_START) {
        note = "  first fragment";
    }
    DSD_FPRINTF(stderr, "[TETRA MAC-RESOURCE] CC=%d  enc=%s  rand_acc=%u  len_ind=%u  addr=%s  sdu_bits=%d%s\n", b->cc,
                k_enc_mode_name[h->enc_mode & 3u], (unsigned)h->random_access, (unsigned)h->length_ind, addr, sdu_bits,
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

/* Table 21.55 up to the channel allocation flag; pi/4-DQPSK has no immediate napping permission flag and no multiple
 * slot granting. Every optional element is read inside the PDU length. Returns 0 for a Null PDU or a header that
 * does not fit: nothing more of the block is decoded then. */
static int
read_resource_header(const mac_block* b, const uint8_t* bits, int nbits, mac_resource_header* h) {
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 2);
    h->fill = (uint8_t)tetra_bits_read(&r, 1);
    h->grant_pos = (uint8_t)tetra_bits_read(&r, 1);
    h->enc_mode = (uint8_t)tetra_bits_read(&r, 2);
    h->random_access = (uint8_t)tetra_bits_read(&r, 1);
    h->length_ind = (uint8_t)tetra_bits_read(&r, 6);
    const uint8_t type = (uint8_t)tetra_bits_read(&r, 3);
    if (!tetra_bits_ok(&r) || type == TETRA_MAC_ADDR_NULL) {
        /* No PDU follows a Null PDU in its block (clause 23.4.3.3). */
        return 0;
    }
    h->pdu_bits = resource_pdu_bits(h->length_ind, nbits);
    if (h->pdu_bits < 0) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  reserved length indication %u\n", b->cc,
                          (unsigned)h->length_ind);
        return 0;
    }
    r = tetra_bits_reader(bits, h->pdu_bits, r.pos);
    read_address(&r, type, h->enc_mode, &h->addr);
    if (tetra_bits_read(&r, 1) != 0u) {
        (void)tetra_bits_skip(&r, 4); /* power control element, table 21.92 */
    }
    if (tetra_bits_read(&r, 1) != 0u) {
        (void)tetra_bits_skip(&r, 8); /* basic slot granting element, table 21.96 */
    }
    h->alloc_present = (uint8_t)tetra_bits_read(&r, 1);
    h->body = r.pos;
    if (!tetra_bits_ok(&r)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-RESOURCE] CC=%d  header longer than the %d-bit PDU\n", b->cc, h->pdu_bits);
        return 0;
    }
    return 1;
}

/* MAC-RESOURCE whose encryption mode is not 00: everything after the channel allocation flag is ciphertext
 * (EN 300 392-7 clause 6.7.1.2), so neither the allocation nor the TM-SDU is read. */
static void
resource_encrypted(mac_block* b, const mac_resource_header* h, int sdu_bits) {
    print_resource(b, h, sdu_bits);
    publish_resource_header(b->ts, h);
    if (h->length_ind == MAC_LI_FRAGMENT_START) {
        b->ts->mac_encrypted_sdus++;
        start_fragment(b, &h->addr, 1, NULL, 0);
    } else if (sdu_bits > 0 || h->alloc_present) {
        b->ts->mac_encrypted_sdus++;
    }
}

/* MAC-RESOURCE, clause 21.4.3.1. Returns the PDU length, or 0 when nothing more of the block can be decoded. */
static int
parse_mac_resource(mac_block* b, const uint8_t* bits, int nbits) {
    mac_resource_header h = {0};
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
    mac_allocation alloc = {0};
    if (h.alloc_present && !read_allocation(&r, b->ts, &alloc)) {
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
        publish_resource_header(b->ts, &h);
        start_fragment(b, &h.addr, 0, bits + r.pos, end - r.pos);
        return h.pdu_bits;
    }
    const uint32_t generation_before = b->ts->cmce_call_generation;
    const int grant = h.alloc_present && apply_allocation(b, &alloc, &h.addr);
    publish_resource_header(b->ts, &h);
    deliver_tm_sdu(b, bits + r.pos, end - r.pos, grant ? &alloc : NULL, generation_before);
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
    if (f->encrypted) {
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
complete_fragment(mac_block* b, const mac_allocation* alloc) {
    tetra_state_t* const ts = b->ts;
    tetra_mac_fragment* const f = b->frag;
    const uint32_t generation_before = ts->cmce_call_generation;
    const int grant = alloc && apply_allocation(b, alloc, &f->addr);
    publish_address(ts, &f->addr, TETRA_ENC_MODE_NONE);
    TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  TM-SDU of %u bits rebuilt\n", b->cc, (unsigned)f->nbits);
    deliver_tm_sdu(b, f->bits, f->nbits, grant ? alloc : NULL, generation_before);
    discard_fragment(f);
}

/* The MAC-END after its slot granting and channel allocation flags (table 21.59). */
static void
finish_fragment(mac_block* b, const uint8_t* bits, int pdu_bits, int body, uint32_t fill, uint32_t alloc_present) {
    tetra_mac_fragment* const f = b->frag;
    if (f->encrypted) {
        /* The allocation and the last fragment are ciphertext like the rest (EN 300 392-7 clause 6.7.1.2). */
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC-END] CC=%d  encrypted TM-SDU ends\n", b->cc);
        discard_fragment(f);
        return;
    }
    tetra_bit_reader r = tetra_bits_reader(bits, pdu_bits, body);
    mac_allocation alloc = {0};
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

/* Cache SYSINFO. The control channel moves to the main carrier only when it is a valid frequency. */
static void
apply_sysinfo(const mac_block* b, const uint32_t* v, int mle_present) {
    tetra_state_t* const ts = b->ts;
    if (ts->frames_sysinfo == 1u || ts->sysinfo_main_carrier != v[SI_MAIN_CARRIER]) {
        char summary[DSD_TETRA_MESSAGE_TEXT_SIZE];
        DSD_SNPRINTF(summary, sizeof summary, "SYSINFO CC=%d carrier=%u band=%u", b->cc, (unsigned)v[SI_MAIN_CARRIER],
                     (unsigned)v[SI_BAND]);
        tetra_message_push(b->state, DSD_TETRA_MESSAGE_CONTROL, summary);
    }
    const long dl_hz = carrier_frequency_hz(v[SI_MAIN_CARRIER], v[SI_BAND], v[SI_OFFSET]);
    if (dl_hz > 0L) {
        ts->dl_carrier_hz = dl_hz;
        ts->freq_band = (uint8_t)v[SI_BAND];
        ts->freq_offset = (uint8_t)v[SI_OFFSET];
        b->state->trunk_cc_freq = dl_hz;
        tetra_sm_on_cc_sync(b->opts, b->state);
    } else {
        TETRA_LOG_VERBOSE(b->ts, 1, "[TETRA SYSINFO] CC=%d  no valid main carrier; control channel unchanged\n", b->cc);
    }
    ts->cck_valid = (uint8_t)v[SI_CCK_FLAG];
    if (ts->cck_valid) {
        ts->cck_id = (uint16_t)v[SI_HF_OR_CCK];
    } else {
        ts->hyperframe = (uint16_t)v[SI_HF_OR_CCK];
        ts->hyperframe_valid = 1;
    }
    ts->duplex_spacing = (uint8_t)v[SI_DUPLEX];
    ts->num_csch = (uint8_t)v[SI_NUM_SCCH];
    ts->ms_txpwr_max = (uint8_t)v[SI_TXPWR];
    ts->rxlev_access_min = (uint8_t)v[SI_RXLEV];
    ts->sysinfo_main_carrier = (uint16_t)v[SI_MAIN_CARRIER];
    ts->sysinfo_rev_op = (uint8_t)v[SI_REVERSE];
    ts->sysinfo_acc_param = (uint8_t)v[SI_ACCESS_PARAMETER];
    ts->sysinfo_radio_dl_tmo = (uint8_t)v[SI_DL_TIMEOUT];
    ts->sysinfo_opt_field_type = (uint8_t)v[SI_OPTION_FLAG];
    ts->sysinfo_opt_field_data = v[SI_OPTION];
    if (mle_present) {
        ts->la = (uint16_t)v[SI_LA];
        ts->subscr_class = (uint16_t)v[SI_SUBSCRIBER_CLASS];
        ts->bs_service_det = (uint16_t)v[SI_BS_SERVICE];
        ts->sysinfo_known = 1;
    }
}

/* SYSINFO, clause 21.4.4.1. With pi/4-DQPSK it fills the whole 124-bit block. */
static void
parse_sysinfo(const mac_block* b, tetra_bit_reader* r) {
    static const char* const k_option_name[4] = {"even-MF", "odd-MF", "access-code", "ext-svc"};
    uint32_t v[SI_FIELDS] = {0};
    for (int i = 0; i < SI_LA; i++) {
        v[i] = tetra_bits_read(r, k_sysinfo_bits[i]);
    }
    if (!tetra_bits_ok(r)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA SYSINFO] CC=%d  too short (%d bits)\n", b->cc, r->nbits);
        return;
    }
    const int mle_present = tetra_bits_left(r) >= 42;
    char mle[64] = "";
    if (mle_present) {
        for (int i = SI_LA; i < SI_FIELDS; i++) {
            v[i] = tetra_bits_read(r, k_sysinfo_bits[i]);
        }
        DSD_SNPRINTF(mle, sizeof mle, "  LA=%u  subscr_class=0x%04X  bs_svc_det=0x%03X", (unsigned)v[SI_LA],
                     (unsigned)v[SI_SUBSCRIBER_CLASS], (unsigned)v[SI_BS_SERVICE]);
    }
    DSD_FPRINTF(stderr,
                "[TETRA SYSINFO] CC=%d  carrier=%u  band=%u  foff=%u  ds=%u  rev=%u  num_csch=%u  pwr=%u  rxlev=%u  "
                "acc=%u  dl_tmo=%u  %s=%u  opt=%s(0x%05X)%s\n",
                b->cc, (unsigned)v[SI_MAIN_CARRIER], (unsigned)v[SI_BAND], (unsigned)v[SI_OFFSET],
                (unsigned)v[SI_DUPLEX], (unsigned)v[SI_REVERSE], (unsigned)v[SI_NUM_SCCH], (unsigned)v[SI_TXPWR],
                (unsigned)v[SI_RXLEV], (unsigned)v[SI_ACCESS_PARAMETER], (unsigned)v[SI_DL_TIMEOUT],
                v[SI_CCK_FLAG] != 0u ? "cck_id" : "hyperframe", (unsigned)v[SI_HF_OR_CCK],
                k_option_name[v[SI_OPTION_FLAG] & 3u], (unsigned)v[SI_OPTION], mle);
    apply_sysinfo(b, v, mle_present);
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

/* ACCESS-DEFINE, clause 21.4.4.3. Returns the PDU length (32, 48 or 56 bits), or 0 when it does not fit. */
static int
parse_access_define(const mac_block* b, tetra_bit_reader* r) {
    uint32_t v[AD_FIELDS];
    for (int i = 0; i < AD_FIELDS; i++) {
        v[i] = tetra_bits_read(r, k_access_define_bits[i]);
    }
    char option[40] = "";
    if (v[AD_OPTION_FLAG] == 1u) {
        DSD_SNPRINTF(option, sizeof option, "  subscr_class=0x%04X", (unsigned)tetra_bits_read(r, 16));
    } else if (v[AD_OPTION_FLAG] == 2u) {
        DSD_SNPRINTF(option, sizeof option, "  gssi=%u", (unsigned)tetra_bits_read(r, 24));
    }
    (void)tetra_bits_skip(r, 3); /* filler bits making the PDU octet bounded */
    if (!tetra_bits_ok(r)) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA ACCESS-DEFINE] CC=%d  truncated\n", b->cc);
        return 0;
    }
    DSD_FPRINTF(stderr,
                "[TETRA ACCESS-DEFINE] CC=%d  %s  access_code=%c  imm=%u  wait=%u  num_ra=%u  frame_len_f=%u  "
                "ts_ptr=%u  min_prio=%u%s\n",
                b->cc, v[AD_ASSIGNED] != 0u ? "assigned" : "common", (char)('A' + (int)v[AD_ACCESS_CODE]),
                (unsigned)v[AD_IMM], (unsigned)v[AD_WT], (unsigned)v[AD_NU], (unsigned)v[AD_FRAME_LENGTH_FACTOR],
                (unsigned)v[AD_TIMESLOT_POINTER], (unsigned)v[AD_MIN_PRIORITY], option);
    tetra_state_t* const ts = b->ts;
    ts->access_common_flag = (uint8_t)v[AD_ASSIGNED];
    ts->access_code = (uint8_t)v[AD_ACCESS_CODE];
    ts->access_imm = (uint8_t)v[AD_IMM];
    ts->access_wait_time = (uint8_t)v[AD_WT];
    ts->access_num_ra = (uint8_t)v[AD_NU];
    ts->access_frame_len_f = (uint8_t)v[AD_FRAME_LENGTH_FACTOR];
    ts->access_ts_ptr = (uint8_t)v[AD_TIMESLOT_POINTER];
    ts->access_min_pdu_pri = (uint8_t)v[AD_MIN_PRIORITY];
    return r->pos;
}

/* Broadcast PDUs, table 21.64. Returns the PDU length, or 0 when nothing more of the block can be decoded. */
static int
parse_broadcast(mac_block* b, const uint8_t* bits, int nbits) {
    tetra_bit_reader r = tetra_bits_reader(bits, nbits, 2);
    const uint32_t type = tetra_bits_read(&r, 2);
    if (!tetra_bits_ok(&r)) {
        return 0;
    }
    if (type == TETRA_MAC_BC_ACCESS_DEF) {
        return parse_access_define(b, &r);
    }
    if (type == TETRA_MAC_BC_SYSINFO && b->first) {
        b->ts->frames_sysinfo++;
        parse_sysinfo(b, &r);
        return nbits;
    }
    /* A SYSINFO behind another PDU is not a pi/4-DQPSK SYSINFO; SYSINFO-DA and the reserved type have no length
     * this decoder knows. */
    TETRA_LOG_PAYLOAD(b->ts, "[TETRA BROADCAST] CC=%d  broadcast type %u not decoded\n", b->cc, (unsigned)type);
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
                k_enc_mode_name[enc_mode & 3u], (unsigned)event_label, end - r.pos,
                enc_mode != TETRA_ENC_MODE_NONE ? "  encrypted; not decoded" : "");
    if (enc_mode != TETRA_ENC_MODE_NONE) {
        b->ts->mac_encrypted_sdus++;
    } else if (end > r.pos) {
        llc_deliver(b, bits + r.pos, end - r.pos);
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

/* N.203: drop a partial TM-SDU of this timeslot after 4 slots without a fragment (clause 23.4.3.1.1 iii). */
static void
age_fragment(mac_block* b) {
    tetra_mac_fragment* const f = b->frag;
    if (!f->active || b->fragment_seen) {
        return;
    }
    f->idle_half_slots = (uint8_t)(f->idle_half_slots + (b->block_bits > MAC_HALF_SLOT_BITS ? 2u : 1u));
    if (f->idle_half_slots >= MAC_N203_HALF_SLOTS) {
        TETRA_LOG_PAYLOAD(b->ts, "[TETRA MAC] CC=%d  partial TM-SDU of %u bits timed out\n", b->cc, (unsigned)f->nbits);
        discard_fragment(f);
    }
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

/* The fragment context of the block being decoded: its TDMA timeslot 1..4, or 0 while the clock is not running. */
static int
fragment_timeslot(const tetra_state_t* ts) {
    return ts->tdma_valid && ts->tn >= 1u && ts->tn <= 4u ? (int)ts->tn : 0;
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
    ts->frames_total++;
    log_raw_block(ts, bits, nbits);
    if (ts->uplink) {
        /* Uplink blocks hold MAC-DATA, MAC-U-BLCK and uplink MAC-FRAG/MAC-END PDUs (clause 21.4.2), whose layouts
         * differ from the downlink ones decoded here. */
        TETRA_LOG_PAYLOAD(ts, "[TETRA MAC] CC=%d  uplink block of %d bits not decoded\n", cc, nbits);
        return;
    }
    mac_block b = {opts, state, ts, cc, nbits, 1, 0, &ts->mac_frag[fragment_timeslot(ts)]};
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
