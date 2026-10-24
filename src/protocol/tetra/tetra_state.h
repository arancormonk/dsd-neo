// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief TETRA decoder state, held in the TETRA state extension slot (DSD_STATE_EXT_PROTO_TETRA).
 *
 * Decoder-thread private. Frontends never read it; they get TETRA data through published views.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_
#define DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_

#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/protocol/tetra/tetra_trunk_sm.h>
#include <stdint.h>
#include <time.h>

#include "tetra_voice_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The shared tuning state (dsd_opts, dsd_state) a trunking tune changes, kept to put back when the tune fails. */
typedef struct {
    long p25_vc_freq[2];
    long trunk_vc_freq[2];
    double last_vc_sync_time_m;
    double last_cc_sync_time_m;
    double p25_last_vc_tune_time_m;
    time_t last_vc_sync_time;
    time_t last_cc_sync_time;
    time_t p25_last_vc_tune_time;
    int trunk_is_tuned;
} tetra_sm_tune_snapshot;

/* Everything a tune puts back when it fails: the shared tuning state, the state machine's own, and the followed call's
 * proof on the carrier (tetra_confirm_t::followed_call), which only the carrier it was read on keeps (confirm_epoch). */
typedef struct {
    tetra_sm_tune_snapshot tuning;
    tetra_sm_grant followed;
    double t_tune_m;
    double t_voice_m;
    double t_maint_m;
    uint32_t confirm_epoch;
    uint8_t followed_proven;
    uint8_t followed_rx;
    uint8_t followed_retired;
    uint8_t state;
} tetra_sm_rollback;

/* The trunking state machine (tetra_trunk_sm.c). Times are decode-clock monotonic seconds. */
typedef struct {
    tetra_sm_rollback pending_prev; /* what the tune in flight puts back if it fails */
    tetra_sm_grant followed;        /* the call followed while TUNED */
    double t_tune_m;  /* the followed tune completed, or the grant was taken on the same carrier; 0 in flight */
    double t_voice_m; /* last speech on a followed timeslot since then; 0 for none */
    double t_maint_m; /* last channel maintenance event before the first speech (tetra_sm_note_maintenance()); 0 none */
    uint64_t pending_request_id; /* the correlated trunk-tuning request in flight; 0 for none */
    uint8_t state;               /* tetra_sm_state_e */
    uint8_t pending_kind;        /* what the request in flight does: 0 none, 1 grant, 2 return to the control channel */
    uint8_t followed_retired;    /* the followed call's identifier names another call now (tetra_sm_retire_call());
                                 read while TUNED, cleared by every new follow */
} tetra_sm_t;

/* Let go of the tune @p sm has in flight (tetra_trunk_sm.c): starting over, a hand-over to the engine, the TETRA state
 * dropped. The request's terminal cleanup goes to the runtime trunk-tuning layer, which outlives the TETRA state
 * (dsd_trunk_tuning_request_disown()): one the backend has not answered stays in flight, so its completion still
 * publishes the tuner boundary, and its failure, already answered or answered later, is retired with nothing to put
 * back, so it cannot keep the frame gate closed while trunking goes on. */
void tetra_sm_disown_pending(tetra_sm_t* sm);

/* CRC evidence about the carrier on air and the burst being decoded (tetra_confirm.c). A training sequence alone is
 * weak enough for noise to match; a block whose CRC-16 passed (EN 300 392-2 clause 8.2.3.3) is not. Cleared at every
 * carrier boundary, no-carrier pass and trunking retune (tetra_confirm_reset()). */
typedef struct {
    uint32_t signalling_blocks; /* CRC-passed downlink BSCH and MAC blocks decoded on this carrier: a control channel */
    uint32_t carrier_blocks;    /* CRC-16 signalling blocks decoded on this carrier, the uplink's SCH/HU and MAC blocks
                                   included */
    uint32_t carrier_epoch;     /* counts the resets: evidence read at one epoch belongs to no other carrier */
    uint8_t followed_call;      /* CRC-passed signalling of the followed call was decoded on this carrier */
    /* Receive permission for the followed call's traffic on this carrier (EN 300 392-2 clause 23.8.2.3.2), and the
     * ACCESS-ASSIGN PDUs of its channel that grant and withdraw it (tetra_calls_on_aach()). Forgotten with
     * followed_call. */
    uint8_t rx_perm;     /* the followed call's traffic may be processed */
    uint8_t rx_run;      /* N.213: AACHs with the call's downlink usage marker in successive slots */
    uint8_t rx_adverse;  /* N.212: AACHs received since the last such one without it */
    uint8_t rx_chan_bad; /* N.208: successive AACHs saying the channel is no longer the call's */
    uint8_t rx_run_tn;   /* timeslot and frame of the last AACH rx_run counted */
    uint8_t rx_run_fn;
    uint8_t rx_qual_valid; /* rx_qual_start holds the last qualifying AACH or CC authorisation (T.212) */
    uint32_t rx_run_start; /* tdma_slot_start of the last AACH rx_run counted */
    uint32_t rx_qual_start;
    uint8_t burst_open;   /* a burst is being decoded (tetra_confirm_burst_begin() to _end()) */
    uint8_t burst_stale;  /* the carrier was reset while the burst was open: its blocks are the last carrier's */
    uint8_t burst_proven; /* the open (or last) burst decoded a block whose CRC passed */
} tetra_confirm_t;

/** One way the air interface encryption of downlink signalling may be undone (tetra_air_sig.c): an operator's 80-bit
 * cipher key (SCK or CCK) of a network with one of TEA1..TEA3, or a TEA1 reduced key of one channel. As a call record's
 * key (tetra_call_t::air_key) it is the one that proved the record's signalling, with the cell key epoch and the
 * encryption mode parity it proved under. Holds key material: zeroed with what holds it. */
typedef struct {
    uint8_t key[10]; /**< the 80-bit key; a reduced key's register in key[0..3] */
    uint8_t valid;   /**< holds a candidate */
    uint8_t kind;    /**< DSD_TETRA_AIR_KEY_* */
    uint8_t ksg;     /**< KSG number 0..2 (TEA1..TEA3) */
    uint8_t parity;  /**< the encryption mode's low bit (SCK-VN / CCK-id parity) it proved under */
    uint8_t cc;      /**< reduced key: its channel's colour code */
    uint16_t mcc;    /**< its network */
    uint16_t mnc;
    uint16_t la;      /**< reduced key: its channel's location area */
    uint16_t carrier; /**< reduced key: its channel's carrier */
    uint32_t epoch;   /**< tetra_state_t::air_epoch it proved under */
} tetra_air_cand;

/** A channel allocation element (EN 300 392-2 table 21.87) as the MAC read it from a clear PDU. */
typedef struct {
    long freq_hz;       /**< downlink frequency of the carrier, 0 when its band is unknown */
    uint16_t carrier;   /**< carrier number */
    uint8_t type;       /**< allocation type */
    uint8_t timeslots;  /**< timeslot assigned: bit 3 is timeslot 1 ... bit 0 timeslot 4; 0 is "go to the MCCH" */
    uint8_t up_down;    /**< up/downlink assigned; for an augmented allocation, the augmented value */
    uint8_t bandwidth;  /**< augmented allocations only; 0 is 25 kHz */
    uint8_t modulation; /**< augmented allocations only; 0 is pi/4-DQPSK */
} tetra_chan_alloc;

/* Phase of a CMCE call (EN 300 392-2 clause 14.5): set up (D-SETUP, D-ALERT, D-CALL PROCEEDING), through-connected
 * (D-CONNECT, D-CONNECT ACKNOWLEDGE), a transmission granted (D-TX GRANTED), the transmission ceased (D-TX CEASED). */
enum {
    TETRA_CALL_FREE = 0,
    TETRA_CALL_SETUP,
    TETRA_CALL_CONNECTED,
    TETRA_CALL_TX_GRANTED,
    TETRA_CALL_CEASED,
};

/* tetra_call_t::flags */
enum {
    TETRA_CALL_F_SETUP_SEEN = 1u << 0,   /* a D-SETUP named the called party and the basic service */
    TETRA_CALL_F_EVENT_LABEL = 1u << 1,  /* event_label is assigned to a party of the call */
    TETRA_CALL_F_USAGE_MARKER = 1u << 2, /* usage_marker is the downlink traffic usage marker of the call's allocation
                                            (EN 300 392-2 clause 23.5.5) */
};

/* The MAC channel an event label is valid on (EN 300 392-2 clause 23.4.1.2.3, tetra_calls.c): an allocated channel,
 * named by its carrier number, which names that channel of the cell wherever the receiver is, or the channel the
 * assignment came on, named by the carrier the receiver was on then. */
typedef struct {
    uint32_t epoch;    /**< not @c allocated: the receiver's carrier then (tetra_confirm_carrier_epoch()) */
    uint16_t carrier;  /**< @c allocated: the carrier number */
    uint8_t slots;     /**< its timeslots (table 21.87 bitmap); 0 when not known: any */
    uint8_t allocated; /**< the channel the assigning PDU allocated */
} tetra_label_channel;

/* tetra_call_t::calling_net when the calling party's network is not known: no calling party extension equals it. */
#define TETRA_CALL_NET_UNKNOWN 0xFFFFFFFFu

/** One CMCE call the downlink signalled, keyed by its 14-bit call identifier (tetra_calls.c). */
typedef struct {
    double seen_m;          /**< decode clock: the last CRC-passed PDU that named the call, or its speech */
    double alloc_m;         /**< decode clock: when its channel allocation was last set; 0 for none */
    double followed_m;      /**< decode clock: when the state machine started following it; 0 never followed */
    tetra_chan_alloc alloc; /**< the traffic channel it was last allocated; no timeslots for none */
    uint32_t called_ssi;    /**< called party: the GSSI of a group call, the ISSI of an individual one; 0 unknown */
    uint32_t calling_ssi;   /**< calling party; 0 unknown */
    uint32_t calling_net;   /**< calling_ssi's network as a calling party extension holds it (MCC << 14 | MNC, EN 300
                                 392-2 table 14.44); TETRA_CALL_NET_UNKNOWN when not known */
    uint32_t talker_ssi;    /**< transmitting party of the transmission in progress; 0 none or unknown */
    uint16_t call_id;       /**< call identifier */
    uint16_t event_label;   /**< event label assigned to a party of the call (TETRA_CALL_F_EVENT_LABEL) */
    tetra_label_channel label_chan; /**< the channel @c event_label is valid on */
    uint8_t phase;                  /**< TETRA_CALL_*; TETRA_CALL_FREE for an unused entry */
    uint8_t flags;                  /**< TETRA_CALL_F_* */
    uint8_t basic_service;          /**< basic service information (table 14.35) as last signalled */
    uint8_t e2ee;           /**< end-to-end encrypted: the basic service or D-TX GRANTED encryption control says so */
    uint8_t air_enc;        /**< encryption mode (TETRA_ENC_MODE_*) of the MAC-RESOURCE that allocated its channel */
    uint8_t priority;       /**< call priority from D-SETUP */
    uint8_t timeout;        /**< call time-out (table 14.50) */
    uint8_t usage_marker;   /**< downlink traffic usage marker assigned with @c alloc (TETRA_CALL_F_USAGE_MARKER) */
    tetra_air_cand air_key; /**< the key whose decrypted signalling gave @c alloc, when it was ciphertext */
} tetra_call_t;

/* The CMCE calls tracked at once. The MCCH of a carrier with four timeslots signals the calls of every carrier of the
 * cell, so eight leave room for calls that are set up while others are on air. */
#define TETRA_CALLS_MAX 8

/* What canonical call slot 0 carries for TETRA (tetra_calls_t::published). */
enum {
    TETRA_PUB_NONE = 0, /* nothing */
    TETRA_PUB_CALL,     /* the CMCE call published_call_id */
    TETRA_PUB_ANON,     /* trunked-mode speech no call record names, on the timeslot published_call_id holds */
    TETRA_PUB_DMO,      /* the direct mode call */
};

/** What tells one direct mode short data message from another (EN 300 396-3 tables 45 and 46): its message type, who
 * sent it to whom, its short data type and user data length, and its whole DM-SDU, the first 64 bits of the user data
 * and the digest of every element (tetra_dmo_sync_h_t::sdu_digest). */
typedef struct {
    uint64_t value;    /**< user data or status: its first 64 bits */
    uint32_t digest;   /**< the DM-SDU's digest (tetra_dmo_sync_h_t::sdu_digest), its FCS left out */
    uint32_t src;      /**< source address */
    uint32_t dst;      /**< destination address; 0 when DM-2-B hides it */
    uint16_t length;   /**< user data length in bits */
    uint16_t sdu_bits; /**< DM-SDU bits the digest covers */
    uint8_t type;      /**< message type; 0 for none */
    uint8_t sdti;      /**< short data type identifier */
} tetra_dmo_sds_key;

/** The direct mode call on the channel, from its DMAC-SYNC and DMAC-DATA PDUs (EN 300 396-3 clause 9). */
typedef struct {
    double seen_m;         /**< decode clock: the last PDU or speech of the call */
    double sds_m;          /**< decode clock: when the last short data notice went out */
    tetra_dmo_sds_key sds; /**< the message of the last short data notice; type 0 for none */
    uint32_t src;          /**< source address (talker) */
    uint32_t dst;          /**< destination address; 0 when DM-2-B hides it */
    uint32_t tx_seq;       /**< the transmission it is a call of (tetra_state_t::dmo_tx_seq) */
    uint8_t active;        /**< a DM-SETUP, DM-CONNECT ACK or DM-OCCUPIED set it up and no end has come since */
    uint8_t in_tx;         /**< it was published with a transmission in progress, not only announced by its set-up */
    uint8_t addressed;     /**< the addresses were clear (no DM-2-C) */
    uint8_t group;         /**< call type element: 1 group call, 0 individual call (group_known) */
    uint8_t group_known;   /**< the DM-SDU with the call type was clear */
    uint8_t circuit_mode;  /**< circuit mode type (0 speech); circuit_known */
    uint8_t circuit_known;
} tetra_dmo_call_t;

/** Calls (tetra_calls.c): the trunked mode call table, the direct mode call and what the decoder publishes. */
typedef struct {
    tetra_call_t call[TETRA_CALLS_MAX];
    tetra_dmo_call_t dmo;
    double heard_m;             /* decode clock: the last good speech of the heard timeslot (not trunking) */
    double held_m;              /* decode clock: its last good speech that played (held) */
    uint16_t published_call_id; /* the call TETRA_PUB_CALL names; the timeslot of the speech TETRA_PUB_ANON shows */
    uint16_t heard_call_id;     /* the call the heard speech belongs to; heard_known */
    uint8_t published;          /* TETRA_PUB_* */
    uint8_t heard;              /* canonical slot 0 shows the speech of heard_tn (not trunking) */
    uint8_t held;               /* and its speech played: heard_tn holds the receiver, other timeslots are not heard */
    uint8_t heard_tn;           /* its timeslot, 0 while the TDMA clock does not place it */
    uint8_t heard_known;        /* heard_call_id names its call */
    uint32_t ends;              /* counts the ends of what canonical slot 0 carried: the speech path starts over */
    uint16_t la;                /* the location area the records were made in (la_known): its SwMI's call identifiers */
    uint8_t la_known;
    /* 1 + the encryption mode of the last addressed MAC-RESOURCE decoded on this carrier since it was taken, 0 for
     * none: per timeslot at index TN, and at index 0 on any timeslot (or while the TDMA clock is stopped). */
    uint8_t slot_enc[5];
} tetra_calls_t;

/** Address element of a downlink MAC-RESOURCE PDU (EN 300 392-2 table 21.55). */
typedef struct {
    uint8_t type;          /* address type, TETRA_MAC_ADDR_* */
    uint8_t esi;           /* 1 when the SSI part is an encrypted short identity (EN 300 392-7 clause 4.2.6) */
    uint8_t esi_decrypted; /* the SSI part was decrypted from @c esi_value (TA61) */
    uint32_t esi_value;    /* with esi_decrypted: the encrypted short identity it was sent as */
    uint8_t usage_marker;  /* 6 bits, address types SSI + usage marker */
    uint16_t event_label;  /* 10 bits, address types with an event label */
    uint32_t ssi;          /* SSI, USSI or SMI */
} tetra_mac_address;

/* N.202, the largest TM-SDU (EN 300 392-2 annex B.2). */
#define TETRA_MAC_TM_SDU_MAX_BITS 2632

/** Reconstruction of one fragmented downlink TM-SDU (EN 300 392-2 clause 23.4.3.1.1). */
/* Ciphertext pieces an encrypted fragmented TM-SDU keeps until its MAC-END: one per fragment, each under the key
 * stream segment of its own slot (EN 300 392-7 clause 6.4.2.2). The smallest fragment fills what an SCH/HD leaves. */
#define TETRA_MAC_FRAG_PIECES     32

/** One encrypted piece of a fragmented TM-SDU: @c nbits ciphertext bits at @c start of the fragment buffer, sent in
 * the slot of initial value @c iv on carrier @c carrier from key stream bit @c offset. */
typedef struct {
    uint32_t iv;
    uint16_t start;
    uint16_t nbits;
    uint16_t offset;
    uint16_t carrier;
} tetra_mac_piece;

typedef struct {
    uint8_t active;      /* a MAC-RESOURCE with length indication 111111 started a TM-SDU */
    uint8_t enc_mode;    /* its encryption mode (TETRA_ENC_MODE_*): not 00, every fragment is ciphertext */
    uint8_t npieces;     /* encrypted: the pieces of bits[] so far */
    uint8_t cipher_lost; /* encrypted: a piece could not be kept, so the TM-SDU cannot be decrypted */
    tetra_mac_piece piece[TETRA_MAC_FRAG_PIECES];
    uint8_t idle_half_slots; /* half slots of this timeslot since the last fragment, for the N.203 limit */
    uint32_t slot_start;     /* tdma_slot_start of the last block of this timeslot: the slots the clock counted since */
    int8_t cc;               /* colour code of the block that started the TM-SDU */
    tetra_mac_address addr;  /* address of the starting MAC-RESOURCE; MAC-FRAG and MAC-END carry none */
    uint16_t nbits;          /* TM-SDU bits rebuilt so far */
    uint8_t bits[TETRA_MAC_TM_SDU_MAX_BITS]; /* one bit per byte */
} tetra_mac_fragment;

/** The security information a cell broadcasts in its SYSINFO extended services broadcast (EN 300 392-7 table A.104,
 * EN 300 392-2 table 21.67). Elements a note of table A.104 voids read zero. */
typedef struct {
    uint8_t auth;          /* authentication required on this cell */
    uint8_t class1;        /* security class 1 MSs supported */
    uint8_t class3;        /* 1: security class 3 MSs supported; 0: class 2 */
    uint8_t sckn;          /* class 2: the SCK number, 1..32; 0 in a class 3 cell */
    uint8_t dck_initial;   /* class 3: DCK retrieval during initial cell selection */
    uint8_t dck_reselect;  /* class 3: DCK retrieval during cell re-selection */
    uint8_t linked_gck;    /* class 3 with GCK supported: linked GCK crypto-periods */
    uint8_t short_gck_vn;  /* with linked GCK crypto-periods: the 2 low bits of their GCK-VN */
    uint8_t gck_supported; /* GCK supported on this cell */
} tetra_cell_security;

/* Encrypted TM-SDUs kept as evidence that a key decrypts the signalling (tetra_air_sig.c): enough for the encrypted
 * PDUs a carrier sends in TETRA_AIR_EVIDENCE_S (four associated PDUs in each of four slots of 17 frames a second, at
 * most), and at most TETRA_AIR_EVIDENCE_PER_SIG of one clear header. */
#define TETRA_AIR_EVIDENCE_MAX     512
#define TETRA_AIR_EVIDENCE_PER_SIG 16
#define TETRA_AIR_CANDIDATES       4

/** What one encrypted TM-SDU decrypted to under each candidate: its clear header and slot, and a digest of each
 * candidate's plaintext (tetra_air_sig.c). */
typedef struct {
    double at_m;                            /* decode clock when it was taken */
    uint64_t digest[TETRA_AIR_CANDIDATES];  /* plaintext digest under candidate cand_id[i] */
    uint32_t cand_id[TETRA_AIR_CANDIDATES]; /* fingerprint of each candidate it was decrypted under */
    uint32_t iv;                            /* initial value of its (first) slot */
    uint32_t addr;                          /* SSI, ESI, USSI or SMI as sent */
    uint16_t length;                        /* encrypted bits */
    uint16_t carrier;
    uint16_t la;
    uint16_t label; /* event label */
    uint8_t used;
    uint8_t ncand;
    uint8_t kind; /* MAC-RESOURCE, rebuilt TM-SDU or MAC-D-BLCK */
    uint8_t addr_type;
    uint8_t enc_mode;
    uint8_t alloc;
    uint8_t um;
    uint8_t cc;
} tetra_air_evidence;

/* Networks whose air interface algorithm evidence is kept: room for every network an air key can be loaded for
 * (DSD_TETRA_AIR_KEY_SLOTS), whose evidence is never evicted, and four more (tetra_air.c). */
#define TETRA_AIR_KSG_NETS 20

/** The KSG numbers one network's signalling named (tetra_state_t::air_ksg). */
typedef struct {
    uint32_t seq;  /* when the entry was made: the oldest keyless entry is the one a full cache replaces */
    uint16_t mcc;  /* 10-bit MCC */
    uint16_t mnc;  /* 14-bit MNC */
    uint16_t mask; /* bit k: KSG number k was named (0..3 standard, 8..15 proprietary; 4..7 are reserved) */
    uint8_t valid; /* the entry holds a network */
} tetra_air_ksg_net;

typedef struct tetra_state {
    /* The speech path: speech decoder, speech integrity and PCM carry (tetra_voice.c). */
    tetra_voice_t voice;

    /* Call following (tetra_trunk_sm.c) and the CRC evidence it and the speech path rely on (tetra_confirm.c). */
    tetra_sm_t sm;
    tetra_confirm_t confirm;

    /* The calls the decoder tracks, follows and publishes (tetra_calls.c). */
    tetra_calls_t calls;

    /* Fragmented TM-SDU reconstruction (tetra_mac.c), one context per downlink timeslot (index = TN 1..4; index 0 while
     * the TDMA timeslot is unknown, which never holds one: a first fragment of no known timeslot is not kept). A
     * fragment run belongs to the control channel of its timeslot, so fragments of different timeslots are never
     * joined (EN 300 392-2 clause 23.4.3.1.1). */
    tetra_mac_fragment mac_frag[5];

    /* Concatenated SDS reassembly contexts (EN 300 392-2 clause 29.5.14), allocated on first use and freed with the
     * TETRA state (tetra_mle.c). */
    struct tetra_sds_concat_store* sds_concat;

    /* The cell's SYSINFO (EN 300 392-2 table 21.65, tetra_mac.c): the downlink frequency in Hz of its main carrier (0
     * while unknown) and that carrier's number; the frequency band and offset of the last SYSINFO with a valid main
     * carrier, which a channel allocation without extended carrier numbering uses (clause 21.5.2; freq_band 0 while
     * none is known); and the location area of its D-MLE-SYSINFO (clause 18.4.2.2), sysinfo_known once one was
     * decoded. frames_sysinfo counts SYSINFO PDUs: the publication shows the main carrier once there was one. */
    long dl_carrier_hz;
    uint32_t frames_sysinfo;
    uint16_t sysinfo_main_carrier;
    /* The cell's broadcast security: sec from the last SYSINFO with the extended services broadcast (sec_valid once
     * one was decoded; the optional field alternates with others, EN 300 392-7 clause 6.2.0.0), and whether the BS
     * service details of the last D-MLE-SYSINFO offer air interface encryption (aie_available, aie_known once one was
     * decoded). Cell properties: bsch_forget_cell() clears them, a retune within the cell keeps them. */
    tetra_cell_security sec;
    uint8_t sec_valid;
    uint8_t aie_known;
    uint8_t aie_available;
    uint16_t la;
    uint8_t freq_band;
    uint8_t freq_offset;
    uint8_t sysinfo_known;

    /* Network identity from the BSCH (EN 300 392-2 table 21.76) or --tetra-net: net_known once MCC, MNC and the 6-bit
     * colour code are set, and lfsr_seed, the scrambling seed they give (tetra_compute_scramb_seed()). */
    uint32_t lfsr_seed;
    uint16_t mcc;
    uint16_t mnc;
    uint8_t net_known;
    uint8_t colour;

    /* The encryption mode (TETRA_ENC_MODE_*) of the last addressed MAC-RESOURCE, or of the MAC-RESOURCE that started
     * the TM-SDU a MAC-END completed; Null PDUs leave it alone. It describes the cell's signalling, for display: the
     * calls keep the encryption mode of their own allocation (tetra_call_t::air_enc). frames_resource counts
     * MAC-RESOURCE PDUs other than Null PDUs, which tells a clear cell from one nothing was decoded on yet. */
    uint32_t frames_resource;
    uint8_t enc_mode;

    /* Bursts and blocks whose CRC passed; TETRA reads as decoded once one did (tetra_publish()). */
    uint32_t decode_ok;

    /* The frequency the receiver was tuned to when the last burst began (dsd_opts_tuned_freq_hz(); 0 off a radio
     * input), set by tetra_frame_enter() and cleared by tetra_forget_carrier(): what tetra_calls_rx_carrier() reads the
     * carrier being received from where no options are at hand. */
    long rx_tuned_hz;

    /* TDMA clock (EN 300 392-2 clause 7.3), set by a BSCH and run by tetra_tdma_follow(): while tdma_valid, the slot
     * that started at symbol count tdma_slot_start is timeslot tn 1..4 of frame fn 1..18 of multiframe mn 1..60. The
     * hyperframe number is the SYSINFO value from a block whose CCK flag is clear, counted on by the clock at each
     * MN 60 to MN 1; hyperframe_valid drops with the clock and on a change of cell. */
    uint32_t tdma_slot_start;
    uint16_t hyperframe;
    uint8_t hyperframe_valid;
    uint8_t tdma_valid;
    uint8_t tn;
    uint8_t fn;
    uint8_t mn;

    /* The KSG numbers (EN 300 392-7 table A.73) the network's CRC-passed signalling named -- a D-OTAR SCK or GCK
     * Provide, the ciphering parameters of a D-LOCATION UPDATE COMMAND or REJECT -- per network keyed by MCC/MNC, for
     * the session (tetra_air.c); air_ksg_seq numbers the entries in the order they were made. The operator air keys
     * live in the key set (dsd_state::tetra_air_keys). */
    tetra_air_ksg_net air_ksg[TETRA_AIR_KSG_NETS];
    uint32_t air_ksg_seq;

    /* Air encrypted signalling (tetra_air_sig.c). air_evidence: the encrypted TM-SDUs of this carrier a repeat may prove
     * a key with, for TETRA_AIR_EVIDENCE_S. air_epoch moves when the cell's key may have changed: another key version,
     * SCKN or GCK support announced (EN 300 392-7 clauses 6.3.2.1, 6.5.1, 4.5.8), the encrypted MAC headers' parity
     * switching (air_parity, settled after a run of them, air_parity_run of air_parity_next), another cell, location
     * area or network; a call's key proved under an older epoch decrypts nothing. */
    tetra_air_evidence air_evidence[TETRA_AIR_EVIDENCE_MAX];
    uint32_t air_epoch;
    uint16_t air_version; /* the last SCK-VN / CCK-id a SYSINFO announced (air_version_known) */
    uint8_t air_version_known;
    uint8_t air_sckn; /* the last SCKN (class 2) and GCK support the extended services broadcast gave */
    uint8_t air_gck;
    uint8_t air_sec_known;
    uint8_t air_parity; /* the parity the cell's encrypted signalling settled on (air_parity_known) */
    uint8_t air_parity_known;
    uint8_t air_parity_next;
    uint8_t air_parity_run;

    /* Direct mode (EN 300 396-2, EN 300 396-3), set by tetra_dmo_apply_sync(); all of it is the carrier's, forgotten with
     * it (tetra_dmo_forget_carrier()).
     * dmo:            1 once a DSB with a DMO system code was decoded: normal bursts have no AACH and use the DM colour
     *                 code. Cleared, with all the rest, by a CRC-passed trunked burst: a BSCH of a trunked system code,
     *                 or on the uplink an SCH/HU (tetra_dmo_leave()).
     * dmo_dcc_valid:  1 while dmo_dcc_seed holds the DM colour code of the current transmission (clause 8.2.4).
     * dmo_comm_type:  communication type of the last DSB: 0 direct, 1 DM-REP, 2 DM-GATE, 3 DM-REP/GATE.
     * dmo_crypt:      tetra_dmo_crypt_e of the current call's traffic, what its speech is classified by.
     * dmo_tx_crypt:   tetra_dmo_crypt_e the set-up of the traffic transmission in progress gave (EN 300 396-3 clause
     *                 8.5.3), which its DM-OCCUPIED repeats; UNKNOWN while none is in progress.
     * dmo_tx_aie:     the security class (TETRA_DMO_AIE_*) of that set-up's SCH/S, which the transmission keeps
     *                 (clause 8.5.3): DM-2-A, DM-2-B and DM-2-C are all TETRA_DMO_CRYPT_AIR, but each a transmission of
     *                 its own; valid while dmo_tx_crypt is not UNKNOWN.
     * dmo_tx_ksg, dmo_tx_key_number: the KSG number and encryption key number (SCK dmo_tx_key_number + 1) of the
 *                 DMAC-SYNC that set it up, when its class is not DM-1 (EN 300 396-3 clauses 9.3.7 and 9.3.14; the
 *                 ciphering parameters do not change within a call transaction, EN 300 396-6 clause 5.2); 0 otherwise.
 * dmo_tx_seed:    the DM colour code seed of that transmission's source, 0 when none is known.
     * dmo_tx_seq:     counts the transmissions started: the direct mode call canonical slot 0 shows is of one of them.
     * dmo_since_sync: DNBs processed since the last DMAC-SYNC (colour code staleness). */
    uint32_t dmo_dcc_seed;
    uint32_t dmo_tx_seed;
    uint32_t dmo_tx_seq;
    uint16_t dmo_since_sync;
    uint8_t dmo;
    uint8_t dmo_dcc_valid;
    uint8_t dmo_comm_type;
    uint8_t dmo_crypt;
    uint8_t dmo_tx_crypt;
    uint8_t dmo_tx_aie;
    uint8_t dmo_tx_ksg;
    uint8_t dmo_tx_key_number;

    /* The status downlink of the last complete D-MM STATUS (EN 300 392-2 table 16.3), for the frontends; mm_status_valid
     * once one was decoded (tetra_mm.c). */
    uint8_t mm_status_code;
    uint8_t mm_status_valid;

    /* Diagnostic gates mirrored from the options at every burst (tetra_frame_enter()), so parsers that are not handed
     * the options can still keep per-burst detail behind -Z (payload) and -v (verbose). */
    uint8_t log_payload;
    uint8_t log_verbose;

    /* 1 with --tetra-uplink: normal bursts are then NUBs (EN 300 392-2 table 9.5), the training sequence of a downlink
     * NDB with block 1 directly against it (no AACH), scrambled with the TMO colour code from a BSCH or --tetra-net.
     * uplink_no_net_noted once the uplink logged that it has no colour code to descramble with. */
    uint8_t uplink;
    uint8_t uplink_no_net_noted;
} tetra_state_t;

/** 1 when the TDMA clock places the burst being decoded in frame 18 of a trunked mode downlink carrier: the control
 * frame, which carries signalling only (EN 300 392-2 clause 19.4.3.1: traffic, speech included, takes frames 1 to 17;
 * the mapping of clause 9.5.1), so nothing in it is speech. Direct mode, the uplink, which has no BSCH to set the clock
 * by and whose bursts do not run it, and a stopped clock place no burst there. */
static inline int
tetra_tdma_control_frame(const tetra_state_t* ts) {
    return ts != NULL && ts->tdma_valid && !ts->dmo && !ts->uplink && ts->fn == 18u;
}

/** The TETRA state of @p state, created on first use; NULL only when @p state is NULL or memory runs out. */
tetra_state_t* tetra_st(dsd_state* state);

/** The TETRA state of @p state without creating it: a zeroed default when it has none; NULL only for a NULL @p state. */
const tetra_state_t* tetra_st_peek(const dsd_state* state);

/** The TETRA state of @p state if it has one, without creating it; NULL otherwise. */
tetra_state_t* tetra_st_existing(dsd_state* state);

/** Drop the TETRA state of @p state; the next tetra_st() starts from zero. Never from inside a TETRA decode, state
 * machine or trunk-tuning call: their callers hold the pointer. */
void tetra_state_reset(dsd_state* state);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_TETRA_TETRA_STATE_H_ */
