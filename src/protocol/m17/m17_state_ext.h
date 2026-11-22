// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The M17 protocol's state extension (DSD_STATE_EXT_PROTO_M17_GNSS): the M17 decoder's working state that no
 * frontend reads, kept out of dsd_state and so out of the UI snapshots. Decoder-thread private; created at first use
 * and freed with the run's extensions (dsd_state_ext_free_all()).
 */

#ifndef DSD_NEO_SRC_PROTOCOL_M17_M17_STATE_EXT_H_
#define DSD_NEO_SRC_PROTOCOL_M17_M17_STATE_EXT_H_

#include <dsd-neo/core/state_ext.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/m17/m17_parse.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What tells one GNSS META report's transmission from another's (m17_state_ext::gnss_last): the carrier and its call,
 * the whole checked LSF (sender, destination, type word; the META is compared apart), the reception, and on an IP stream
 * the stream admission opened (m17_state_ext::ip_stream_gen). kind 0: none (no report admitted yet, or its transmission
 * ended). */
typedef struct {
    uint8_t kind; /* 1 = RF, the slot 0 call epoch; 2 = network, the stream ID */
    uint64_t id;
    uint64_t epoch;
    uint64_t src;
    uint64_t dst;
    uint16_t type_word;
    uint64_t gen;
    uint32_t stream_gen; /* an IP stream frame: the admitted stream it belongs to; 0 otherwise */
} m17_gnss_tx_key;

/* Network frames remembered for the GNSS duplicate rule (m17_gnss_is_duplicate()): a few frames' worth, so a copy that
 * arrives a few frames late (two network paths of different delay) is still known. */
enum { M17_GNSS_DGRAM_RING = 16 };

/* One network frame (IP stream frame or MPKT, CRC included) whose GNSS META was admitted or repeated: its length and
 * 64-bit FNV-1a digest, the reception generation and the decode-clock time it was last heard at. len 0: none. */
typedef struct {
    uint64_t digest;
    uint64_t gen;
    uint64_t ms;
    uint32_t len;
} m17_gnss_dgram;

/* How many ended network streams are remembered (m17_state_ext::ip_ended). A fixed table: no allocation and no moves
 * on the decoder thread; past it the oldest is forgotten, and the rule set fails closed from then on
 * (dsd_state::m17_ip_ended_lost). */
enum { M17_IP_ENDED_CAPACITY = 64 };

/* One ended network stream: its key (station << 16 | stream ID) and when it ended (m17_state_ext::ip_ended_seq at
 * the time). used 0: a free slot. */
typedef struct {
    uint64_t key;
    uint32_t seq;
    uint8_t used;
} m17_ip_ended_key;

/* The 1600 bit/s stream's arbitrary data being assembled (m17_stream_1600_arbitrary_assemble()): one six-frame group's
 * chunks, which slots are in and the group. Emptied at a transmission boundary (m17_confirm_reset()). */
typedef struct {
    uint8_t data[48];
    uint8_t mask;
    uint16_t group;
} m17_adata_assembly;

typedef struct m17_state_ext {
    /* The GNSS META repeat rule (issue #800): a transmission repeats its LSF META (RF: every LICH superframe; IP: every
       stream frame), and the repeats are one report: the last admitted report's transmission key and META bytes, and
       the network frames that carried it. */
    m17_gnss_tx_key gnss_last;
    uint8_t gnss_meta_admitted[M17_META_BYTES];
    m17_gnss_dgram gnss_dgram[M17_GNSS_DGRAM_RING];
    uint8_t gnss_dgram_next;
    /* Network streams that ended (EOT, a matching EOTX or DISC, another stream replacing them, or a final frame heard
       while another stream was open), each by its stream ID and the station its LSF named, remembered until frame 0
       starts that stream again so that a datagram of one arriving late is ignored (issue #890). ip_ended_seq counts
       the ends. Once one was forgotten to make room, a stream neither open nor remembered is started only by its frame
       0 (dsd_state::m17_ip_ended_lost, outside this allocation). */
    m17_ip_ended_key ip_ended[M17_IP_ENDED_CAPACITY];
    uint32_t ip_ended_seq;
    /* Counts the network streams admission opened: the GNSS repeat rule's stream boundary (a stream numbers on through
       its counter's wrap, so a frame number that falls is no boundary). */
    uint32_t ip_stream_gen;
    /* When the open network stream's last frame was taken (dsd_realtime_mono_ms()): a stream with none taken for
       M17_IP_STREAM_IDLE_MS no longer holds back an MPKT's call (m17_ip_handle_mpkt_frame()). */
    uint64_t ip_stream_last_ms;
    /* The RF packet being assembled by the strict RF packet rule (m17_packet_take_chunk()). pkt_armed: a checked
       packet-mode LSF frame started it and every frame since was the next in order and in step; cleared by anything
       else, and set again only by the next checked LSF frame. pkt_ref_symbol: where the frame before the next packet
       frame ended (dsd_state::symbolcnt), so a wholly lost frame shows in the spacing (an EOF carries a byte count, not
       a frame number). */
    uint8_t pkt_armed;
    uint32_t pkt_ref_symbol;
    /* The symbol clock epoch (dsd_state::symbol_clock_epoch) pkt_ref_symbol was counted in: a frame counted in another
       (the input rewound or switched) is no next frame. */
    uint32_t pkt_ref_epoch;
    m17_adata_assembly adata;
    /* The scan side's view of the checked link setup bound to the transmission (issue #890), meaningful only while
       dsd_state::m17_lsf_checked is set. lsf_checked_head: the checked LSF's DST, SRC and TYPE (its first 112 bits,
       packed MSB first). lsf_disputed: a clean LICH chunk of the stream has since disagreed with them
       (m17_lich_note_other_link()): another transmission whose boundary went unheard, or a Golay miscorrection; a
       disputed binding is no identity of the call for a scan hold until an LSF is checked again, and the decoder's
       checked fields are left as they are. lsf_unbroken: RF continuity has not broken since the checked LSF bound the
       transmission; set by a checked LSF, cleared by an RF LSF or stream frame that does not end one M17 frame after
       the RF frame before it (a frame lost, or another transmission's frames after a missed EOS, EOT and LSF frame,
       which take at least three frame periods; m17_rf_frame_check()), by an RF frame whose read was cut
       (m17_rf_frame_cut()) and by one the symbol clock restarted inside; each break also drops the LICH chunks held
       from before it and the weak LICH streak (m17_rf_continuity_break()). The binding vouches for the stream (m17_link_setup_vouched(): its scan report's fields, its
       checked speech) only while it is undisputed and unbroken. All three are emptied at every transmission boundary
       (m17_confirm_reset()). rf_frame_valid,
       rf_frame_symbol, rf_frame_epoch: where the last RF LSF or stream frame M17 read whole ended
       (dsd_state::symbolcnt, dsd_state::symbol_clock_epoch; m17_rf_frame_record()); a cut read leaves none. */
    uint8_t lsf_checked_head[14];
    uint8_t lsf_disputed;
    uint8_t lsf_unbroken;
    uint8_t rf_frame_valid;
    uint32_t rf_frame_symbol;
    uint32_t rf_frame_epoch;
    /* The text META of the RF transmission or the admitted network stream being assembled from its blocks
       (m17_meta_text_assembler_push()), emptied at every transmission boundary (m17_confirm_reset()) so that no text
       is made of two transmissions' blocks. An MPKT has its own, on the stack. */
    struct m17_meta_text_assembler text_meta;
} m17_state_ext;

/* The state's M17 extension, or NULL when none was created yet. */
static inline m17_state_ext*
m17_state_ext_find(const dsd_state* state) {
    return (m17_state_ext*)dsd_state_ext_get(state, DSD_STATE_EXT_PROTO_M17_GNSS);
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_M17_M17_STATE_EXT_H_ */
