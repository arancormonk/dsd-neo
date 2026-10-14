// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief What the TETRA decoder publishes for every frontend (dsd_state::tetra_pub).
 *
 * The TETRA decoder keeps its working state in a state extension that no snapshot copies. After each burst it copies
 * the few values the frontends show into this publication, which rides the dsd_state snapshot copy range
 * (ui_snapshot.c pins it with a static assert). Plain integers only: no pointer, which the byte-range copy would
 * alias into decoder memory, no float, and no key material. app_control/tetra_view.h turns it into text. The type
 * stays out of state.h so that view's public header can name the values.
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

/** The speech codec's health (dsd_tetra_publication::codec_status). */
typedef enum {
    DSD_TETRA_CODEC_UNKNOWN = 0,       /**< No speech frame has reached the codec yet. */
    DSD_TETRA_CODEC_READY = 1,         /**< The codec decodes. */
    DSD_TETRA_CODEC_MISSING = 2,       /**< No codec is installed or configured. */
    DSD_TETRA_CODEC_LOAD_FAILED = 3,   /**< A codec is configured but would not load or start. */
    DSD_TETRA_CODEC_DECODE_ERRORS = 4, /**< The codec runs but its last frames failed to decode. */
} dsd_tetra_codec_status;

/** The highest TEA number dsd_tetra_publication::tea names (TEA1..TEA4, EN 300 392-7 KSG numbers 0..3). */
enum { DSD_TETRA_TEA_MAX = 4 };

/**
 * @brief The TETRA decoder's display state, for every frontend.
 *
 * Written only by the TETRA decoder on the decoder thread, at the end of every burst. A reset (a mode change, or the
 * TETRA decoder state being dropped) clears every field but @c generation, which it bumps.
 */
typedef struct dsd_tetra_publication {
    uint32_t generation;      /**< Bumped by every reset, so a reader can tell one session from the next. */
    uint8_t mode;             /**< dsd_tetra_mode */
    uint8_t net_known;        /**< 1 once MCC, MNC and colour code are known (a BSCH, or --tetra-net); 0 in direct
                                   mode, whose carrier belongs to no trunked network, as for every trunked mode field
                                   below (the network, the control and traffic channels, the D-MM STATUS). */
    uint8_t colour;           /**< The 6-bit colour code of the BSCH SYNC PDU (EN 300 392-2). */
    uint8_t air_encryption;   /**< dsd_tetra_air_enc */
    uint16_t mcc;             /**< The 10-bit mobile country code. */
    uint16_t mnc;             /**< The 14-bit mobile network code. */
    uint8_t tea;              /**< The network's air-interface algorithm, 1..4 for TEA1..TEA4; 0 = not known. */
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
    uint32_t codec_frames;   /**< Speech frames the codec decoded since the last reset. */
    uint32_t codec_errors;   /**< Speech frames the codec failed to decode since the last reset. */
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
