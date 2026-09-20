// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * TETRA MAC PDU parser – SCH-HD (Half-Capacity Downlink Signaling Channel)
 * ETSI EN 300 392-2 §21.4 (Lower MAC) and §21.5 (MAC PDU Formats)
 *
 * Only MAC-BROADCAST(SYSINFO) and MAC-RESOURCE are decoded in detail.
 * MAC-FRAG/END and MAC-SUPPL log their raw bytes and PDU type.
 *
 * The decoded bit array fed into these functions uses one byte per bit
 * (value 0 or 1), MSB-first ordering, as produced by the Viterbi decoder
 * in tetra.c.
 */

#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/protocol/tetra/tetra_mm.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/talkgroup_policy.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <dsd-neo/protocol/tetra/tetra_bits.h>

/* Phase 81: bits_to_uint unified — see tetra_bits.h */
#define bits_to_uint  tetra_bits_to_uint

/* -----------------------------------------------------------------------
 * MAC PDU type name table
 * ----------------------------------------------------------------------- */

const char *tetra_mac_type_name(int pdu_type)
{
    switch (pdu_type) {
    case TETRA_MAC_TYPE_RESOURCE:  return "MAC-RESOURCE";
    case TETRA_MAC_TYPE_FRAG_END:  return "MAC-FRAG/END";
    case TETRA_MAC_TYPE_BROADCAST: return "MAC-BROADCAST";
    case TETRA_MAC_TYPE_SUPPL:     return "MAC-SUPPL";
    default:                       return "UNKNOWN";
    }
}

static const char *addr_type_name(int at)
{
    switch (at) {
    case TETRA_MAC_ADDR_NULL:        return "Null";
    case TETRA_MAC_ADDR_SSI:         return "SSI";
    case TETRA_MAC_ADDR_EVENT_LABEL: return "EventLabel";
    case TETRA_MAC_ADDR_USSI:        return "USSI";
    case TETRA_MAC_ADDR_SMI:         return "SMI";
    case TETRA_MAC_ADDR_SSI_EVENT:   return "SSI+Event";
    case TETRA_MAC_ADDR_SSI_USAGE:   return "SSI+Usage";
    case TETRA_MAC_ADDR_SMI_EVENT:   return "SMI+Event";
    default:                         return "?";
    }
}

/* -----------------------------------------------------------------------
 * Bit-length of address field, by type
 * (ETSI EN 300 392-2 Table 21.49)
 * ----------------------------------------------------------------------- */
static int addr_field_len(int addr_type)
{
    switch (addr_type) {
    case TETRA_MAC_ADDR_SSI:
    case TETRA_MAC_ADDR_USSI:
    case TETRA_MAC_ADDR_SMI:        return 24;
    case TETRA_MAC_ADDR_EVENT_LABEL: return 10;
    case TETRA_MAC_ADDR_SSI_EVENT:
    case TETRA_MAC_ADDR_SMI_EVENT:  return 34;   /* 24 + 10 */
    case TETRA_MAC_ADDR_SSI_USAGE:  return 30;   /* 24 + 6 */
    case TETRA_MAC_ADDR_NULL:
    default:                        return 0;
    }
}

static void clear_fragment_reassembly(dsd_state *state)
{
    if (!state) return;
    state->tetra_frag_active = 0;
    state->tetra_frag_seq = 0;
    state->tetra_frag_nbits = 0;
    state->tetra_frag_cc = 0;
    state->tetra_vc_grant_pending = 0;
    state->tetra_vc_grant_call_generation = 0;
    memset(state->tetra_frag_buf, 0, sizeof(state->tetra_frag_buf));
}

static int cmce_call_policy_allows(const dsd_opts *opts, const dsd_state *state)
{
    dsd_tg_policy_decision decision;
    int rc;
    if (!opts || !state || !state->tetra_call_active || !state->tetra_ssi_valid)
        return 1;

    int encrypted = state->tetra_enc_mode != TETRA_ENC_MODE_NONE;
    int data_call = state->tetra_call_type != 0;
    if (state->tetra_cmce_com_type == 0) {
        rc = dsd_tg_policy_evaluate_private_call(opts, state,
                                                 state->tetra_calling_ssi,
                                                 state->tetra_active_ssi,
                                                 encrypted, data_call, &decision);
    } else {
        rc = dsd_tg_policy_evaluate_group_call(opts, state, state->tetra_gssi,
                                               state->tetra_calling_ssi,
                                               encrypted, data_call, &decision);
    }
    if (rc != 0)
        return 0;
    if (!decision.tune_allowed && opts->verbose > 0) {
        fprintf(stderr, "[TETRA CHANNEL-ALLOCATION] target=%u blocked by %s\n",
                decision.target_id,
                dsd_tg_policy_block_reason_label(decision.block_reasons));
    }
    return decision.tune_allowed;
}

static void apply_channel_grant(dsd_opts *opts, dsd_state *state,
                                long vc_hz, uint8_t slots,
                                uint32_t generation_before)
{
    int fresh_call = state && state->tetra_cmce_call_generation != generation_before;
    if (fresh_call && cmce_call_policy_allows(opts, state))
        tetra_sm_on_grant(opts, state, vc_hz, slots);
}

/* A downlink MAC-RESOURCE SSI is the called-party address for the enclosed
 * CMCE setup.  Communication type 0 is point-to-point; the remaining values
 * are point-to-multipoint/acknowledged/broadcast (table 14.54).  Bind the
 * address only after CMCE has published a complete call PDU so truncated
 * payloads cannot create a talkgroup. */
static void sync_cmce_call_target_from_resource(dsd_state *state)
{
    if (!state || !state->tetra_call_active || !state->tetra_ssi_valid)
        return;
    state->tetra_gssi = state->tetra_cmce_com_type == 0
                        ? 0u : state->tetra_active_ssi;
}

uint32_t
tetra_llc_fcs32(const uint8_t *bits, int nbits)
{
    if (!bits || nbits < 0) return 0;
    uint32_t remainder = 0;
    for (int i = 0; i < nbits; i++) {
        uint32_t input = (uint32_t)(bits[i] & 1u);
        if (i < 32) input ^= 1u; /* Annex C step 1. */
        uint32_t feedback = (remainder >> 31) ^ input;
        remainder <<= 1;
        if (feedback) remainder ^= 0x04C11DB7u;
    }
    /* Multiply the complemented message polynomial by x^32. */
    for (int i = 0; i < 32; i++) {
        uint32_t feedback = remainder >> 31;
        remainder <<= 1;
        if (feedback) remainder ^= 0x04C11DB7u;
    }
    return ~remainder; /* Annex C step 5. */
}

/* Decode the basic-link LLC header that sits between MAC and MLE. */
static int dispatch_basic_llc_tm_sdu(const uint8_t *bits, int nbits, int cc,
                                     const dsd_opts *opts, dsd_state *state)
{
    if (!bits || nbits < 4) return 0;
    const int pdu_type = (int)bits_to_uint(bits, 0, 4);
    int header_bits;
    int fcs_bits = 0;
    const char *name;
    switch (pdu_type) {
    case 0: header_bits = 6; name = "BL-ADATA"; break;
    case 1: header_bits = 5; name = "BL-DATA";  break;
    case 2: header_bits = 4; name = "BL-UDATA"; break;
    case 3: header_bits = 5; name = "BL-ACK"; break;
    case 4: header_bits = 6; fcs_bits = 32; name = "BL-ADATA-FCS"; break;
    case 5: header_bits = 5; fcs_bits = 32; name = "BL-DATA-FCS";  break;
    case 6: header_bits = 4; fcs_bits = 32; name = "BL-UDATA-FCS"; break;
    case 7: header_bits = 5; fcs_bits = 32; name = "BL-ACK-FCS"; break;
    default: return 0;
    }
    const int payload_bits = nbits - header_bits - fcs_bits;
    if (payload_bits < 3) {
        /* BL-ACK without FCS may acknowledge without carrying a TL-SDU. All
         * other recognized basic-link forms require layer-3 data. */
        if (pdu_type != 3)
            fprintf(stderr, "[TETRA LLC %s] CC=%d truncated; discarded\n", name, cc);
        return 1;
    }
    if (fcs_bits) {
        const uint32_t received = bits_to_uint(bits, header_bits + payload_bits, 32);
        const uint32_t expected = tetra_llc_fcs32(bits + header_bits, payload_bits);
        if (received != expected) {
            fprintf(stderr,
                    "[TETRA LLC %s] CC=%d FCS mismatch rx=0x%08X calc=0x%08X; discarded\n",
                    name, cc, (unsigned)received, (unsigned)expected);
            return 1;
        }
    }
    fprintf(stderr, "[TETRA LLC %s] CC=%d payload_bits=%d\n", name, cc, payload_bits);
    tetra_mle_dispatch(bits + header_bits, payload_bits, cc, opts, state);
    return 1;
}

/* Parse the mandatory tail of a pi/4-DQPSK MAC-RESOURCE header and its
 * optional basic Channel Allocation IE (EN 300 392-2, tables 21.55/21.87).
 * Returns the first TM-SDU bit, or -1 for a truncated header. */
static int parse_resource_optional_elements(const uint8_t *bits, int nbits, int off,
                                            int cc, dsd_opts *opts, dsd_state *state,
                                            long *grant_hz, uint8_t *grant_slots)
{
    if (off + 1 > nbits) return -1;
    uint32_t power_flag = bits_to_uint(bits, off, 1); off++;
    if (power_flag) {
        if (off + 4 > nbits) return -1;
        off += 4;
    }

    if (off + 1 > nbits) return -1;
    uint32_t slot_grant_flag = bits_to_uint(bits, off, 1); off++;
    if (slot_grant_flag) {
        if (off + 8 > nbits) return -1;
        off += 8; /* basic slot granting element for pi/4-DQPSK */
    }

    if (off + 1 > nbits) return -1;
    uint32_t allocation_flag = bits_to_uint(bits, off, 1); off++;
    if (!allocation_flag)
        return off;

    /* Fixed part through the extended-carrier-numbering flag. */
    if (off + 23 > nbits) return -1;
    uint32_t allocation_type = bits_to_uint(bits, off, 2); off += 2;
    uint32_t timeslots       = bits_to_uint(bits, off, 4); off += 4;
    uint32_t up_down         = bits_to_uint(bits, off, 2); off += 2;
    off += 1; /* CLCH permission */
    off += 1; /* cell change flag */
    uint32_t carrier         = bits_to_uint(bits, off, 12); off += 12;
    uint32_t extended        = bits_to_uint(bits, off, 1); off += 1;

    uint32_t band = state ? state->tetra_freq_band : 0;
    uint32_t freq_offset = state ? state->tetra_freq_offset : 0;
    if (extended) {
        if (off + 10 > nbits) return -1;
        band        = bits_to_uint(bits, off, 4); off += 4;
        freq_offset = bits_to_uint(bits, off, 2); off += 2;
        off += 3; /* duplex spacing */
        off += 1; /* reverse operation */
    }

    if (off + 2 > nbits) return -1;
    uint32_t monitoring = bits_to_uint(bits, off, 2); off += 2;
    if (monitoring == 0) {
        if (off + 2 > nbits) return -1;
        off += 2; /* frame-18 monitoring pattern */
    }

    uint32_t bandwidth_code = 0;
    uint32_t modulation = 0; /* pi/4-DQPSK */
    if (up_down == 0) {
        if (off + 2 + 3 + 3 + 3 + 3 + 4 + 5 + 2 > nbits) return -1;
        up_down        = bits_to_uint(bits, off, 2); off += 2;
        bandwidth_code = bits_to_uint(bits, off, 3); off += 3;
        modulation     = bits_to_uint(bits, off, 3); off += 3;
        off += 3; /* maximum QAM level, or reserved for non-QAM */
        off += 3; /* conforming channel status */
        off += 4; /* BS link imbalance */
        off += 5; /* BS transmit power relative to main carrier */
        uint32_t napping = bits_to_uint(bits, off, 2); off += 2;
        if (napping == 1) {
            if (off + 11 > nbits) return -1;
            off += 11;
        }
        if (off + 4 + 1 > nbits) return -1;
        off += 4; /* reserved */
        uint32_t conditional_a = bits_to_uint(bits, off, 1); off++;
        if (conditional_a) {
            if (off + 16 > nbits) return -1;
            off += 16;
        }
        if (off + 1 > nbits) return -1;
        uint32_t conditional_b = bits_to_uint(bits, off, 1); off++;
        if (conditional_b) {
            if (off + 16 > nbits) return -1;
            off += 16;
        }
        if (off + 1 > nbits) return -1;
        uint32_t further = bits_to_uint(bits, off, 1); off++;
        if (further) {
            /* EN 300 392-2 table 21.87 note 15 requires the receiver to
             * discard both the allocation and any following TM-SDU. The
             * extension has no defined length, so continuing would also lose
             * the payload boundary. */
            fprintf(stderr, "[TETRA CHANNEL-ALLOCATION] CC=%d invalid further augmentation; PDU discarded\n", cc);
            return -1;
        }
    }

    long vc_hz = tetra_carrier_to_dl_hz(carrier, band, freq_offset);
    if (vc_hz > 0 && bandwidth_code < 4) {
        static const long wide_center_adjust_hz[4] = {0L, 12500L, 37500L, 62500L};
        vc_hz += wide_center_adjust_hz[bandwidth_code];
    }
    fprintf(stderr, "[TETRA CHANNEL-ALLOCATION] CC=%d type=%u slots=0x%X dir=%u carrier=%u freq=%ld bw=%u mod=%u\n",
            cc, allocation_type, timeslots, up_down, carrier, vc_hz,
            bandwidth_code, modulation);

    if (state) {
        state->tetra_vc_assignment_valid = 1;
        state->tetra_vc_assignment_type = (uint8_t)allocation_type;
        state->tetra_vc_timeslot_bitmap = (uint8_t)(timeslots & 0x0Fu);
        state->tetra_vc_slot = (uint8_t)(timeslots & 0x0Fu); /* compatibility */
        state->tetra_vc_uplink_downlink = (uint8_t)up_down;
        state->tetra_vc_carrier = (uint16_t)carrier;
        state->tetra_vc_freq_hz = vc_hz;

        if (timeslots == 0) {
            /* A zero bitmap explicitly removes the traffic-channel assignment.
             * Keep assignment_valid set so the audio gate can distinguish this
             * release from conventional monitoring before any allocation. */
            state->tetra_vc_carrier = 0;
            state->tetra_vc_freq_hz = 0;
            state->tetra_tx_granted_valid = 0;
            tetra_sm_on_release(opts, state);
        } else if ((up_down == 1 || up_down == 3) && vc_hz > 0 && modulation == 0) {
            if (grant_hz) *grant_hz = vc_hz;
            if (grant_slots) *grant_slots = (uint8_t)timeslots;
        }
    }
    return off;
}

/* -----------------------------------------------------------------------
 * Subtype parsers
 * ----------------------------------------------------------------------- */

/*
 * MAC-RESOURCE (ETSI EN 300 392-2 §21.4.3.1)
 *
 *   Bit  0-1  : PDU type  (= 00)
 *   Bit    2  : Fill bits indicator
 *   Bit    3  : Position of grant
 *   Bit  4-5  : Encryption mode
 *   Bit    6  : Random access flag
 *   Bit  7-12 : Length indicator (6 bits)
 *   Bit 13-15 : Address type (3 bits)
 *   Bit 16-.. : Address field (variable, see addr_field_len)
 *   ...
 */
static void parse_mac_resource(const uint8_t *bits, int nbits, int cc,
                               const dsd_opts *opts, dsd_state *state)
{
    if (nbits < 17) {
        fprintf(stderr, "[TETRA MAC-RESOURCE] CC=%d  (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    int off = 2; /* skip 2-bit PDU type */
    long grant_hz = 0;
    uint8_t grant_slots = 0;
    uint32_t call_generation_before = state ? state->tetra_cmce_call_generation : 0;
    uint8_t fill_bits   = (uint8_t)bits_to_uint(bits, off, 1); off += 1;
    uint8_t grant_pos   = (uint8_t)bits_to_uint(bits, off, 1); off += 1;
    uint8_t enc_mode    = (uint8_t)bits_to_uint(bits, off, 2); off += 2;
    uint8_t rand_acc    = (uint8_t)bits_to_uint(bits, off, 1); off += 1;
    uint32_t len_ind    =          bits_to_uint(bits, off, 6); off += 6;

    if (off + 3 > nbits) goto short_out;
    uint8_t addr_type   = (uint8_t)bits_to_uint(bits, off, 3); off += 3;

    int alen = addr_field_len(addr_type);
    uint32_t ssi = 0, event_label = 0, usage_marker = 0;
    if (off + alen > nbits) goto short_out;

    if (addr_type == TETRA_MAC_ADDR_SSI ||
        addr_type == TETRA_MAC_ADDR_USSI ||
        addr_type == TETRA_MAC_ADDR_SMI) {
        ssi = bits_to_uint(bits, off, 24);
    } else if (addr_type == TETRA_MAC_ADDR_EVENT_LABEL) {
        event_label = bits_to_uint(bits, off, 10);
    } else if (addr_type == TETRA_MAC_ADDR_SSI_EVENT ||
               addr_type == TETRA_MAC_ADDR_SMI_EVENT) {
        ssi         = bits_to_uint(bits, off,      24);
        event_label = bits_to_uint(bits, off + 24, 10);
    } else if (addr_type == TETRA_MAC_ADDR_SSI_USAGE) {
        ssi          = bits_to_uint(bits, off,      24);
        usage_marker = bits_to_uint(bits, off + 24,  6);
    }
    off += alen;

    fprintf(stderr, "[TETRA MAC-RESOURCE] CC=%d  enc=%s  rand_acc=%d"
                    "  len_ind=%u  addr=%s",
            cc,
            enc_mode == TETRA_ENC_MODE_NONE ? "none" :
            enc_mode == TETRA_ENC_MODE_ON   ? "on"   :
            enc_mode == TETRA_ENC_MODE_ON_AUTH ? "on+auth" : "rsvd",
            rand_acc, len_ind,
            addr_type_name(addr_type));

    if (addr_type == TETRA_MAC_ADDR_SSI ||
        addr_type == TETRA_MAC_ADDR_USSI ||
        addr_type == TETRA_MAC_ADDR_SMI) {
        fprintf(stderr, "(%u)", ssi);
    } else if (addr_type == TETRA_MAC_ADDR_EVENT_LABEL) {
        fprintf(stderr, "(%u)", event_label);
    } else if (addr_type == TETRA_MAC_ADDR_SSI_EVENT ||
               addr_type == TETRA_MAC_ADDR_SMI_EVENT) {
        fprintf(stderr, "(%u/E%u)", ssi, event_label);
    } else if (addr_type == TETRA_MAC_ADDR_SSI_USAGE) {
        fprintf(stderr, "(%u/U%u)", ssi, usage_marker);
    }

    fprintf(stderr, "  fill=%d  grant_pos=%d\n", fill_bits, grant_pos);

    /* The optional MAC header elements precede the TM-SDU. Older captures and
     * unit vectors that end exactly after the address are accepted as a
     * header-only PDU. */
    if (off < nbits) {
        int payload_off = parse_resource_optional_elements(bits, nbits, off, cc,
                                                           (dsd_opts *)(uintptr_t)opts, state,
                                                           &grant_hz, &grant_slots);
        if (payload_off < 0) goto short_out;
        off = payload_off;
    }

    /* --- Update dsd_state --- */
    if (state) {
        state->tetra_enc_mode = enc_mode;
        if (addr_type == TETRA_MAC_ADDR_SSI ||
            addr_type == TETRA_MAC_ADDR_USSI ||
            addr_type == TETRA_MAC_ADDR_SMI) {
            state->tetra_active_ssi = ssi;
            state->tetra_ssi_valid  = 1;
        } else if (addr_type == TETRA_MAC_ADDR_SSI_EVENT ||
                   addr_type == TETRA_MAC_ADDR_SSI_USAGE ||
                   addr_type == TETRA_MAC_ADDR_SMI_EVENT) {
            state->tetra_active_ssi = ssi;
            state->tetra_ssi_valid  = 1;
        } else {
            state->tetra_ssi_valid  = 0;
        }

        /* Phase 77: populate dropped MAC variables */
        state->tetra_mac_fill_bits    = fill_bits;
        state->tetra_mac_grant_pos    = grant_pos;
        state->tetra_mac_rand_acc     = rand_acc;
        state->tetra_mac_len_ind      = (uint8_t)(len_ind & 0x3Fu);
        state->tetra_mac_addr_type    = addr_type;
        state->tetra_mac_event_label  = (uint16_t)(event_label & 0x3FFu);
        state->tetra_mac_usage_marker = (uint8_t)(usage_marker & 0x3Fu);
    }

    /* ---------------------------------------------------------------
     * Phase 9: pass the remaining TM-SDU bits (after the MAC header
     * and address field) to the MLE dispatcher for CMCE / MM decoding.
     *
     * Phase 69: if fill_bits==0 (no padding = TM-SDU continues in
     * following MAC-FRAG/END blocks), save first fragment to the
     * reassembly buffer instead of dispatching immediately.
     * --------------------------------------------------------------- */
    if (state && off < nbits) {
        if (len_ind == 63) {
            /* First fragment: seed the reassembly buffer */
            int payload_bits = nbits - off;
            clear_fragment_reassembly(state);
            if (payload_bits > (int)sizeof(state->tetra_frag_buf)) {
                fprintf(stderr,
                        "[TETRA MAC-RESOURCE] CC=%d fragment start overflow (%d bits)\n",
                        cc, payload_bits);
                return;
            }
            memcpy(state->tetra_frag_buf, bits + off, (size_t)payload_bits);
            state->tetra_frag_nbits  = (uint16_t)payload_bits;
            state->tetra_frag_cc     = (int8_t)cc;
            state->tetra_frag_active = 1;
            if (grant_hz > 0) {
                state->tetra_vc_grant_pending = 1;
                state->tetra_vc_grant_call_generation = call_generation_before;
            }
            fprintf(stderr, "[TETRA MAC-RESOURCE] CC=%d  fragment start (%d bits buffered)\n",
                    cc, payload_bits);
        } else if (len_ind != 2) {
            /* Complete TM-SDU: dispatch directly */
            int pdu_end = nbits;
            int llc_expected = 0;
            if (len_ind >= 3 && len_ind <= 62) {
                const int indicated_end = (int)len_ind * 8;
                if (indicated_end >= off && indicated_end <= nbits) {
                    pdu_end = indicated_end;
                    llc_expected = 1;
                }
            }
            if (!llc_expected
                || !dispatch_basic_llc_tm_sdu(bits + off, pdu_end - off,
                                               cc, opts, state))
                tetra_mle_dispatch(bits + off, pdu_end - off, cc, opts, state);
            sync_cmce_call_target_from_resource(state);
            if (grant_hz > 0)
                apply_channel_grant((dsd_opts *)(uintptr_t)opts, state,
                                    grant_hz, grant_slots, call_generation_before);
        }
    }
    if (grant_hz > 0 && (off >= nbits || len_ind == 2))
        tetra_sm_on_grant((dsd_opts *)(uintptr_t)opts, state, grant_hz, grant_slots);
    return;

short_out:
    fprintf(stderr, "[TETRA MAC-RESOURCE] CC=%d  (truncated at bit %d of %d)\n",
            cc, off, nbits);
}

/*
 * MAC-BROADCAST / SYSINFO (ETSI EN 300 392-2 §21.4.4.1 / §21.5.9)
 *
 *   Bit 0-1   : PDU type       (= 10, BROADCAST)
 *   Bit 2-3   : Broadcast type (= 00, SYSINFO)
 *   Bit 4-15  : Main carrier      (12 bits)
 *   Bit 16-19 : Frequency band    ( 4 bits)
 *   Bit 20-21 : Freq offset       ( 2 bits)
 *   Bit 22-24 : Duplex spacing    ( 3 bits)
 *   Bit    25 : Reverse operation ( 1 bit )
 *   Bit 26-27 : Num SCH           ( 2 bits)
 *   Bit 28-30 : MS TX power max   ( 3 bits)
 *   Bit 31-34 : RXLEV min         ( 4 bits)
 *   Bit 35-38 : Access parameter  ( 4 bits)
 *   Bit 39-42 : Radio DL timeout  ( 4 bits)
 *   Bit    43 : CCK valid/no HF   ( 1 bit )
 *   Bit 44-59 : CCK-ID / Hyperframe number (16 bits)
 *   Bit 60-61 : Option field type ( 2 bits)
 *   Bit 62-81 : Option field data (20 bits)
 *   (+ MLE SYSINFO  Bit 82-..)
 */
static void parse_mac_sysinfo(const uint8_t *bits, int nbits, int cc,
                              dsd_opts *opts, dsd_state *state)
{
    if (nbits < 82) {
        fprintf(stderr, "[TETRA SYSINFO] CC=%d  (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    int off = 4; /* skip PDU type (2) + broadcast type (2) */

    uint32_t main_carrier   = bits_to_uint(bits, off, 12); off += 12;
    uint32_t freq_band      = bits_to_uint(bits, off,  4); off +=  4;
    uint32_t freq_offset    = bits_to_uint(bits, off,  2); off +=  2;
    uint32_t duplex_spacing = bits_to_uint(bits, off,  3); off +=  3;
    uint32_t rev_op         = bits_to_uint(bits, off,  1); off +=  1;
    uint32_t num_csch       = bits_to_uint(bits, off,  2); off +=  2;
    uint32_t ms_txpwr       = bits_to_uint(bits, off,  3); off +=  3;
    uint32_t rxlev          = bits_to_uint(bits, off,  4); off +=  4;
    uint32_t acc_param      = bits_to_uint(bits, off,  4); off +=  4;
    uint32_t radio_dl_tmo   = bits_to_uint(bits, off,  4); off +=  4;
    uint32_t cck_valid      = bits_to_uint(bits, off,  1); off +=  1;
    uint32_t cck_or_hf      = bits_to_uint(bits, off, 16); off += 16;
    uint32_t opt_field_type = bits_to_uint(bits, off,  2); off +=  2;
    uint32_t opt_field_data = bits_to_uint(bits, off, 20); off += 20;

    const char *opt_names[] = { "even-MF", "odd-MF", "access-code", "ext-svc" };

    fprintf(stderr, "[TETRA SYSINFO] CC=%d"
                    "  carrier=%u  band=%u  foff=%u  ds=%u  rev=%u"
                    "  num_csch=%u  pwr=%u  rxlev=%u  acc=%u  dl_tmo=%u"
                    "  %s=%u  opt=%s(0x%05X)\n",
            cc,
            main_carrier, freq_band, freq_offset, duplex_spacing, rev_op,
            num_csch, ms_txpwr, rxlev, acc_param, radio_dl_tmo,
            cck_valid ? "cck_id" : "hyperframe", cck_or_hf,
            opt_field_type < 4 ? opt_names[opt_field_type] : "?",
            opt_field_data);

    /* Phase 11+12: cache DL carrier frequency + band params; arm trunking CC. */
    if (state) {
        long dl_hz = tetra_carrier_to_dl_hz(main_carrier, freq_band, freq_offset);
        if (dl_hz != 0L) {
            state->tetra_dl_carrier_hz = dl_hz;
            state->tetra_freq_band     = (uint8_t)freq_band;
            state->tetra_freq_offset   = (uint8_t)freq_offset;
            state->trunk_cc_freq       = dl_hz;
            tetra_sm_on_cc_sync(opts, state);
        }
        /* Phase 20-22: cache remaining SYSINFO fields */
        state->tetra_cck_valid          = (uint8_t)(cck_valid & 1u);
        state->tetra_cck_id             = (uint16_t)(cck_or_hf & 0xFFFFu);
        state->tetra_duplex_spacing     = (uint8_t)(duplex_spacing & 0x07u);
        state->tetra_num_csch           = (uint8_t)(num_csch & 0x03u);
        state->tetra_ms_txpwr_max       = (uint8_t)(ms_txpwr & 0x07u);
        state->tetra_rxlev_access_min   = (uint8_t)(rxlev & 0x0Fu);
        /* Phase 77: SYSINFO dropped vars */
        state->tetra_sysinfo_main_carrier   = (uint16_t)(main_carrier & 0xFFFu);
        state->tetra_sysinfo_rev_op         = (uint8_t)(rev_op & 1u);
        state->tetra_sysinfo_acc_param      = (uint8_t)(acc_param & 0x0Fu);
        state->tetra_sysinfo_radio_dl_tmo   = (uint8_t)(radio_dl_tmo & 0x0Fu);
        state->tetra_sysinfo_opt_field_type = (uint8_t)(opt_field_type & 0x03u);
        state->tetra_sysinfo_opt_field_data = (uint32_t)(opt_field_data & 0xFFFFFu);
    }

    /* MLE SYSINFO (§21.6.1): LA (14) + Subscr class (16) + BS service details (12) */
    if (nbits >= off + 42) {
        uint32_t la          = bits_to_uint(bits, off, 14); off += 14;
        uint32_t subscr_cls  = bits_to_uint(bits, off, 16); off += 16;
        uint32_t bs_svc_det  = bits_to_uint(bits, off, 12);
        fprintf(stderr, "[TETRA SYSINFO/MLE] LA=%u  subscr_class=0x%04X"
                        "  bs_svc_det=0x%03X\n",
                la, subscr_cls, bs_svc_det);

        /* --- Update dsd_state --- */
        if (state) {
            state->tetra_la             = (uint16_t)la;
            state->tetra_subscr_class   = (uint16_t)subscr_cls;
            state->tetra_bs_service_det = (uint16_t)bs_svc_det;
            state->tetra_sysinfo_known  = 1;
        }
    }
}

/*
 * MAC-BROADCAST / ACCESS-DEFINE (ETSI EN 300 392-2 §21.5.10)
 *
 *   Bit 0-1  : PDU type        (= 10, BROADCAST)
 *   Bit 2-3  : Broadcast type  (= 01, ACCESS-DEFINE)
 *   Bit 4    : Common / Dedicated flag
 *   Bit 5-8  : Immediate
 *   Bit 9-12 : Waiting time
 *   Bit 13-16: Number of random access transmissions
 *   Bit 17   : Frame length factor
 *   Bit 18-21: Timeslot pointer
 *   Bit 22-24: Min PDU priority
 * (further optional fields omitted for brevity)
 */
static void parse_mac_access_define(const uint8_t *bits, int nbits, int cc,
                                     dsd_state *state)
{
    if (nbits < 25) {
        fprintf(stderr, "[TETRA ACCESS-DEFINE] CC=%d  (too short: %d bits)\n",
                cc, nbits);
        return;
    }

    int off = 4; /* skip PDU type (2) + broadcast type (2) */

    uint32_t common_flag = bits_to_uint(bits, off, 1); off += 1;
    uint32_t immediate   = bits_to_uint(bits, off, 4); off += 4;
    uint32_t wait_time   = bits_to_uint(bits, off, 4); off += 4;
    uint32_t num_ra      = bits_to_uint(bits, off, 4); off += 4;
    uint32_t frame_len_f = bits_to_uint(bits, off, 1); off += 1;
    uint32_t ts_ptr      = bits_to_uint(bits, off, 4); off += 4;
    uint32_t min_pdu_pri = bits_to_uint(bits, off, 3);

    fprintf(stderr, "[TETRA ACCESS-DEFINE] CC=%d  %s"
                    "  imm=%u  wait=%u  num_ra=%u"
                    "  frame_len_f=%u  ts_ptr=%u  min_prio=%u\n",
            cc,
            common_flag ? "common" : "dedicated",
            immediate, wait_time, num_ra,
            frame_len_f, ts_ptr, min_pdu_pri);

    /* Phase 23: cache ACCESS-DEFINE params to state */
    if (state) {
        state->tetra_access_imm           = (uint8_t)(immediate & 0x0Fu);
        state->tetra_access_wait_time     = (uint8_t)(wait_time & 0x0Fu);
        /* Phase 74: save remaining fields */
        state->tetra_access_num_ra        = (uint8_t)(num_ra & 0x0Fu);
        state->tetra_access_frame_len_f   = (uint8_t)(frame_len_f & 1u);
        state->tetra_access_ts_ptr        = (uint8_t)(ts_ptr & 0x0Fu);
        state->tetra_access_min_pdu_pri   = (uint8_t)(min_pdu_pri & 0x07u);
        /* Phase 77: store common_flag */
        state->tetra_access_common_flag   = (uint8_t)(common_flag & 1u);
    }
}

/*
 * MAC-FRAG / MAC-END  (ETSI EN 300 392-2 §21.4.4.3 / §21.4.4.4)
 *
 *   Bit 0-1 : PDU type  (= 01)
 *   Bit   2 : Reservation requirement (MAC-FRAG) / 0 (MAC-END)
 *   Bits 3+  : payload (LLC PDU fragment)
 *
 * Phase 69: accumulate fragments into state->tetra_frag_buf and
 * dispatch on MAC-END.
 */
static void parse_mac_frag_end(const uint8_t *bits, int nbits, int cc,
                                const dsd_opts *opts, dsd_state *state)
{
    if (nbits < 3) {
        fprintf(stderr, "[TETRA MAC-FRAG/END] CC=%d  (too short)\n", cc);
        return;
    }
    uint8_t subtype     = (uint8_t)bits_to_uint(bits, 2, 1);
    int     payload_off = 3;
    int     payload_bits = (nbits > payload_off) ? nbits - payload_off : 0;

    fprintf(stderr, "[TETRA MAC-%s] CC=%d  payload_bits=%d\n",
            subtype ? "END" : "FRAG", cc, payload_bits);

    if (!state) return;

    if (!state->tetra_frag_active) {
        fprintf(stderr, "[TETRA MAC-%s] CC=%d ignored without fragment start\n",
                subtype ? "END" : "FRAG", cc);
        return;
    }
    if (state->tetra_frag_cc != (int8_t)cc) {
        fprintf(stderr,
                "[TETRA MAC-%s] CC=%d rejected; fragment started on CC=%d\n",
                subtype ? "END" : "FRAG", cc, (int)state->tetra_frag_cc);
        clear_fragment_reassembly(state);
        return;
    }

    int space = (int)sizeof(state->tetra_frag_buf) - (int)state->tetra_frag_nbits;
    if (payload_bits > space) {
        fprintf(stderr, "[TETRA MAC-%s] CC=%d reassembly overflow; discarding sequence\n",
                subtype ? "END" : "FRAG", cc);
        clear_fragment_reassembly(state);
        return;
    }

    if (!subtype) {
        /* ----------------------------------------------------------------
         * MAC-FRAG: continuation fragment — append to reassembly buffer.
         * ---------------------------------------------------------------- */
        state->tetra_frag_seq    = (uint8_t)((state->tetra_frag_seq + 1u) & 0xFFu);

        if (payload_bits > 0) {
            memcpy(state->tetra_frag_buf + state->tetra_frag_nbits,
                   bits + payload_off, (size_t)payload_bits);
            state->tetra_frag_nbits = (uint16_t)(state->tetra_frag_nbits + (uint16_t)payload_bits);
        }
    } else {
        /* ----------------------------------------------------------------
         * MAC-END: final fragment — append and dispatch whole TM-SDU.
         * ---------------------------------------------------------------- */
        if (payload_bits > 0) {
            memcpy(state->tetra_frag_buf + state->tetra_frag_nbits,
                   bits + payload_off, (size_t)payload_bits);
            state->tetra_frag_nbits = (uint16_t)(state->tetra_frag_nbits + (uint16_t)payload_bits);
        }

        uint8_t pending_grant = state->tetra_vc_grant_pending;
        uint32_t grant_generation = state->tetra_vc_grant_call_generation;

        /* A fragmented TM-SDU still contains the LLC PDU. Decode its basic
         * link header only after the final fragment has arrived so the FCS
         * variants can be bounded against the complete payload. */
        if (state->tetra_frag_nbits >= 9) {
            fprintf(stderr, "[TETRA MAC-END] CC=%d  dispatching %u reassembled bits\n",
                    cc, state->tetra_frag_nbits);
            if (!dispatch_basic_llc_tm_sdu((const uint8_t *)state->tetra_frag_buf,
                                           (int)state->tetra_frag_nbits,
                                           cc, opts, state))
                tetra_mle_dispatch((const uint8_t *)state->tetra_frag_buf,
                                   (int)state->tetra_frag_nbits,
                                   cc, opts, state);
            sync_cmce_call_target_from_resource(state);
        }

        if (pending_grant && state->tetra_vc_freq_hz > 0 &&
            state->tetra_vc_timeslot_bitmap != 0) {
            apply_channel_grant((dsd_opts *)(uintptr_t)opts, state,
                                state->tetra_vc_freq_hz,
                                state->tetra_vc_timeslot_bitmap,
                                grant_generation);
        }

        /* Clear reassembly state */
        clear_fragment_reassembly(state);
    }
}

/*
 * MAC-SUPPL / D-BLCK  (ETSI EN 300 392-2 §21.4.5 / 21.4.5.3)
 *
 *   Bit 0-1 : PDU type  (= 11)
 *   Bit   2 : (reserved / fill-bit indicator)
 *   Bits 3+ : payload
 */
static void parse_mac_suppl(const uint8_t *bits, int nbits, int cc,
                            const dsd_opts *opts, dsd_state *state)
{
    int payload_bits = (nbits > 3) ? nbits - 3 : 0;
    fprintf(stderr, "[TETRA MAC-SUPPL] CC=%d  payload_bits=%d\n",
            cc, payload_bits);
    /* Phase 51: dispatch payload to MLE (same as MAC-RESOURCE TM-SDU) */
    if (payload_bits >= 9)
        tetra_mle_dispatch(bits + 3, payload_bits, cc, opts, state);
}

/* -----------------------------------------------------------------------
 * Public entry point
 * ----------------------------------------------------------------------- */

void tetra_mac_parse_schd(const uint8_t *bits, int nbits,
                          int cc, const dsd_opts *opts, dsd_state *state)
{
    if (!bits || nbits < 2) {
        fprintf(stderr, "[TETRA SCH-HD] CC=%d  (empty PDU)\n", cc);
        return;
    }

    /* Phase 25: total frame counter */
    if (state)
        state->tetra_frames_total++;

    int pdu_type = (int)bits_to_uint(bits, 0, 2);

    switch (pdu_type) {

    case TETRA_MAC_TYPE_RESOURCE:
        if (state) state->tetra_frames_resource++;
        parse_mac_resource(bits, nbits, cc, opts, state);
        break;

    case TETRA_MAC_TYPE_FRAG_END:
        if (state) state->tetra_frames_frag++;
        parse_mac_frag_end(bits, nbits, cc, opts, state);
        break;

    case TETRA_MAC_TYPE_BROADCAST:
        if (nbits < 4) {
            fprintf(stderr, "[TETRA BROADCAST] CC=%d  (too short)\n", cc);
            break;
        }
        {
            int bcast_type = (int)bits_to_uint(bits, 2, 2);
            if (bcast_type == TETRA_MAC_BC_SYSINFO) {
                if (state) state->tetra_frames_sysinfo++;
                parse_mac_sysinfo(bits, nbits, cc, (dsd_opts *)(uintptr_t)opts, state);
            } else if (bcast_type == TETRA_MAC_BC_ACCESS_DEF)
                parse_mac_access_define(bits, nbits, cc, state);
            else if (bcast_type == TETRA_MAC_BC_RESTORE
                     || bcast_type == TETRA_MAC_BC_NWRK_BCAST) {
                /* MLE PDU starts immediately after the 4-bit MAC header. */
                if (nbits > 4)
                    tetra_mle_dispatch(bits + 4, nbits - 4, cc, opts, state);
            } else
                fprintf(stderr, "[TETRA BROADCAST] CC=%d  bcast_type=%d  (unhandled)\n",
                        cc, bcast_type);
        }
        break;

    case TETRA_MAC_TYPE_SUPPL:
        parse_mac_suppl(bits, nbits, cc, opts, state);
        break;

    default:
        fprintf(stderr, "[TETRA SCH-HD] CC=%d  unknown PDU type %d\n",
                cc, pdu_type);
        break;
    }

    /* If full payload dump requested, also hex-dump first 16 decoded bytes. */
    if (opts && opts->payload) {
        int nbytes = (nbits + 7) / 8;
        if (nbytes > 16) nbytes = 16;
        fprintf(stderr, "  [TETRA SCH-HD raw] pdu_type=%d  %d bits:", pdu_type, nbits);
        for (int i = 0; i < nbytes; i++) {
            uint8_t byte = 0;
            for (int b = 0; b < 8 && (i * 8 + b) < nbits; b++)
                byte = (uint8_t)((byte << 1) | (bits[i * 8 + b] & 1));
            fprintf(stderr, " %02X", byte);
        }
        fprintf(stderr, "\n");
    }
}
