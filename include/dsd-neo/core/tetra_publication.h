// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief What the TETRA decoder publishes for every frontend (dsd_state::tetra_pub).
 *
 * The TETRA decoder keeps its working state in a state extension that no snapshot copies. After each burst it copies
 * the few values the frontends show into this publication, which rides the dsd_state snapshot copy range
 * (ui_snapshot.c pins it with a static assert). Plain integers only: no pointer, which the byte-range copy would
 * alias into decoder memory, no float, and no key material: which kind of key applies is published, never a key.
 * app_control/tetra_view.h turns it into text. The type stays out of state.h so that view's public header can name
 * the values.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_TETRA_PUBLICATION_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_TETRA_PUBLICATION_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The working mode of the last burst the decoder took (dsd_tetra_publication::mode). The values are stable. */
typedef enum {
    DSD_TETRA_MODE_NONE = 0,                 /**< Nothing decoded since the last reset. */
    DSD_TETRA_MODE_TMO_DOWNLINK = 1,         /**< Trunked mode, base station downlink (EN 300 392-2). */
    DSD_TETRA_MODE_TMO_UPLINK = 2,           /**< Trunked mode, mobile normal uplink bursts (--tetra-uplink). */
    DSD_TETRA_MODE_TMO_CONTROL_UPLINK = 3,   /**< Trunked mode, a control uplink burst (--tetra-uplink). */
    DSD_TETRA_MODE_DMO_DIRECT = 4,           /**< Direct mode, mobile to mobile (EN 300 396-3). */
    DSD_TETRA_MODE_DMO_REPEATER = 5,         /**< Direct mode through a DM-REP. */
    DSD_TETRA_MODE_DMO_GATEWAY = 6,          /**< Direct mode through a DM-GATE. */
    DSD_TETRA_MODE_DMO_REPEATER_GATEWAY = 7, /**< Direct mode through a DM-REP/GATE. */
} dsd_tetra_mode;

/** Whether the air interface is encrypted (dsd_tetra_publication::air_encryption). */
typedef enum {
    DSD_TETRA_AIR_ENC_UNKNOWN = 0,   /**< No PDU has said yet. */
    DSD_TETRA_AIR_ENC_CLEAR = 1,     /**< The last PDU that said was clear. */
    DSD_TETRA_AIR_ENC_ENCRYPTED = 2, /**< The last PDU that said was encrypted. */
} dsd_tetra_air_enc;

/** Where call following stands (dsd_tetra_publication::trunk_state). */
typedef enum {
    DSD_TETRA_TRUNK_IDLE = 0,       /**< Not following: trunking off, or no control channel known. */
    DSD_TETRA_TRUNK_ON_CONTROL = 1, /**< Parked on the control channel, waiting for an allocation. */
    DSD_TETRA_TRUNK_ON_TRAFFIC = 2, /**< Tuned to a traffic channel (a call, or its hangtime). */
} dsd_tetra_trunk_state;

/** The speech decoder's health (dsd_tetra_publication::codec_status). */
typedef enum {
    DSD_TETRA_CODEC_UNKNOWN = 0,       /**< No speech frame has reached the decoder yet. */
    DSD_TETRA_CODEC_READY = 1,         /**< The decoder decodes. */
    DSD_TETRA_CODEC_DECODE_ERRORS = 2, /**< The last speech frame failed to decode, or no decoder could be made. */
} dsd_tetra_codec_status;

/** Whether the cell offers air interface encryption, as its BS service details say (dsd_tetra_publication::aie). */
typedef enum {
    DSD_TETRA_AIE_UNKNOWN = 0,     /**< No D-MLE-SYSINFO has said yet. */
    DSD_TETRA_AIE_UNAVAILABLE = 1, /**< Air interface encryption is not available on the cell: security class 1. */
    DSD_TETRA_AIE_AVAILABLE = 2,   /**< It is available: the security information says which classes. */
} dsd_tetra_aie;

/** The loaded air key that applies to the carrier being received (dsd_tetra_publication::key_kind). */
typedef enum {
    DSD_TETRA_KEY_NONE = 0,         /**< None does. */
    DSD_TETRA_KEY_CK = 1,           /**< The network's 80-bit cipher key. */
    DSD_TETRA_KEY_TEA1_REDUCED = 2, /**< A TEA1 reduced key of this very channel. */
} dsd_tetra_key_kind;

/** The security class of a direct mode transmission (dsd_tetra_publication::dmo_class, EN 300 396-3 clause 9.3.2). */
typedef enum {
    DSD_TETRA_DMO_CLASS_UNKNOWN = 0, /**< No transmission is in progress. */
    DSD_TETRA_DMO_CLASS_DM1 = 1,     /**< DM-1: the air interface is clear. */
    DSD_TETRA_DMO_CLASS_DM2A = 2,    /**< DM-2-A. */
    DSD_TETRA_DMO_CLASS_DM2B = 3,    /**< DM-2-B. */
    DSD_TETRA_DMO_CLASS_DM2C = 4,    /**< DM-2-C. */
} dsd_tetra_dmo_class;

/**
 * @brief The TETRA decoder's display state, for every frontend.
 *
 * Written only by the TETRA decoder on the decoder thread, at the end of every burst. A reset (a mode change, or the
 * TETRA decoder state being dropped) clears every field but @c generation, which it bumps.
 */
typedef struct dsd_tetra_publication {
    uint32_t generation;    /**< Bumped by every reset, so a reader can tell one session from the next. */
    uint8_t mode;           /**< dsd_tetra_mode */
    uint8_t net_known;      /**< 1 once MCC, MNC and colour code are known (a BSCH, or --tetra-net); 0 in direct
                                   mode, whose carrier belongs to no trunked network, as for every trunked mode field
                                   below (the network, the control and traffic channels, the D-MM STATUS). */
    uint8_t colour;         /**< The 6-bit colour code of the BSCH SYNC PDU (EN 300 392-2). */
    uint8_t air_encryption; /**< dsd_tetra_air_enc */
    uint16_t mcc;           /**< The 10-bit mobile country code. */
    uint16_t mnc;           /**< The 14-bit mobile network code. */
    /** The air interface algorithms signalling named, bit k for KSG number k (EN 300 392-7 table 6.2: 0..3 TEA1..TEA4,
        8..15 proprietary): in trunked mode every one the network's CRC-passed signalling named (a D-OTAR SCK Provide
        of a trunked mode SCK or a GCK Provide, D-LOCATION UPDATE COMMAND and REJECT, a repeat proof), in direct mode
        the one of the DMAC-SYNC that set up the transmission in progress; 0 = none known. A network does not broadcast
        its algorithm, so this is often 0. */
    uint16_t ksg_mask;
    /** Trunked mode only: the algorithms radios proposed in clear U-LOCATION UPDATE DEMANDs on the network's uplink,
        bit k for KSG number k as @c ksg_mask. A proposal is not the network's confirmation (EN 300 392-7 clause
        6.6.2.1), so it is kept apart from @c ksg_mask; 0 = none seen, and always 0 in direct mode. */
    uint16_t ksg_requested_mask;
    uint8_t trunk_state;      /**< dsd_tetra_trunk_state */
    uint8_t cc_known;         /**< 1 once a SYSINFO broadcast named the main (control) carrier. */
    uint8_t traffic_valid;    /**< 1 while a channel allocation names a traffic channel. */
    uint16_t cc_carrier;      /**< The 12-bit main carrier number from SYSINFO. */
    uint16_t traffic_carrier; /**< The 12-bit carrier number of the allocated traffic channel. */
    uint32_t cc_freq_hz;      /**< The control channel's downlink frequency; 0 = not resolved. */
    uint32_t traffic_freq_hz; /**< The traffic channel's downlink frequency; 0 = not resolved. */
    /** The allocated timeslots as the channel allocation element's Timeslot assigned field carries them (EN 300
        392-2), timeslot 1 first: bit 3 is timeslot 1 and bit 0 timeslot 4. */
    uint8_t traffic_slots;
    uint8_t mm_status_valid; /**< 1 once a D-MM STATUS has been decoded. */
    uint8_t mm_status_code;  /**< Its 6-bit Status downlink value (EN 300 392-2, D-MM STATUS). */
    uint8_t codec_status;    /**< dsd_tetra_codec_status */
    uint32_t codec_frames;   /**< Speech frames the decoder decoded since the last reset. */
    uint32_t codec_errors;   /**< Speech frames the decoder failed to decode since the last reset. */
    /* Trunked mode only, like the network fields above. */
    uint16_t la;              /**< The 14-bit location area of the cell's D-MLE-SYSINFO, when la_known. */
    uint16_t rx_carrier;      /**< The 12-bit number of the carrier being received, when rx_carrier_known. */
    uint8_t la_known;         /**< 1 once a D-MLE-SYSINFO gave the location area. */
    uint8_t rx_carrier_known; /**< 1 while the carrier being received is known (none while a retune is in flight). */
    uint8_t key_kind;         /**< dsd_tetra_key_kind: the air key that applies to the carrier being received. */
    uint8_t key_tea;          /**< The algorithm that key runs, 1..3 for TEA1..TEA3; 0 with no key. */
    uint8_t aie;              /**< dsd_tetra_aie */
    /** The cell's security information, from its SYSINFO extended services broadcast (EN 300 392-7 table A.104). */
    uint8_t sec_known;  /**< 1 once one was decoded; the rest are 0 before. */
    uint8_t sec_auth;   /**< Authentication is required on the cell. */
    uint8_t sec_class1; /**< Security class 1 MSs are supported. */
    uint8_t sec_class3; /**< 1: security class 3 MSs are supported; 0: class 2. */
    uint8_t sec_sckn;   /**< Class 2: the SCK number, 1..32; 0 otherwise. */
    /* Direct mode only. */
    uint8_t dmo_class; /**< dsd_tetra_dmo_class of the transmission in progress. */
    uint8_t dmo_sckn;  /**< Its SCK number, 1..32, when its class is not DM-1; 0 otherwise. */
} dsd_tetra_publication;

/** @brief Clear @p pub for a new session, bumping its generation. NULL is ignored. */
static inline void
dsd_tetra_publication_reset(dsd_tetra_publication* pub) {
    if (pub == NULL) {
        return;
    }
#ifdef __cplusplus
    dsd_tetra_publication cleared{};
#else
    dsd_tetra_publication cleared = {0};
#endif
    cleared.generation = pub->generation + 1U;
    *pub = cleared;
}

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_TETRA_PUBLICATION_H_ */
