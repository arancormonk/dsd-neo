// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Frontend-neutral text for the TETRA decoder's state.
 *
 * The TETRA decoder publishes what the frontends show in dsd_state::tetra_pub (core/tetra_publication.h). This view
 * turns a snapshot of it into the rows every surface shows -- the terminal's Call Info lines and the Qt/Android
 * monitor -- so they cannot drift on when the rows appear or how a value is spelled. Calls, short data and status
 * messages reach the frontends as canonical calls and event history notices, like every protocol's.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TETRA_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TETRA_VIEW_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Room for any one text this view writes, terminator included. */
enum { DSD_APP_TETRA_TEXT_SIZE = 80 };

/** @brief Flags for dsd_app_tetra_view(). */
enum {
    /** Write ASCII only, for a terminal without UTF-8: " - " between the parts of a row instead of " · ". */
    DSD_APP_TETRA_ASCII = 1u,
};

/**
 * @brief Display-ready TETRA state.
 *
 * Every text is "" when its row has nothing to show, and is always terminated; the enumerations the fields name are
 * in core/tetra_publication.h. @c network_text names the colour code the way the call history names one ("CC 25",
 * app_control/access_code_view.h), in decimal like every colour code.
 */
typedef struct {
    uint8_t visible;         /**< 1 = TETRA decoding runs and a block has decoded: the TETRA rows belong on screen. */
    uint8_t mode;            /**< dsd_tetra_mode */
    uint8_t trunked;         /**< 1 = trunked mode (TMO); 0 = direct mode (DMO). */
    uint8_t encrypted;       /**< 1 = the air interface is encrypted. */
    uint8_t network_known;   /**< 1 = @c network_text is set. */
    uint8_t trunk_visible;   /**< 1 = trunking follows calls on this downlink: @c trunk_text is set. */
    uint8_t trunk_state;     /**< dsd_tetra_trunk_state */
    uint8_t on_traffic;      /**< 1 = tuned to a traffic channel (a call, or its hangtime). */
    uint8_t control_known;   /**< 1 = @c control_text is set. */
    uint8_t traffic_known;   /**< 1 = @c traffic_text is set. */
    uint8_t mm_status_known; /**< 1 = @c mm_status_text is set. */
    uint8_t codec_known;     /**< 1 = @c codec_text is set. */
    uint8_t codec_status;    /**< dsd_tetra_codec_status */
    uint8_t codec_fault;     /**< 1 = no codec, one that failed to load, or one whose last frames failed. */
    uint32_t generation;     /**< The publication's reset counter. */
    char mode_text[DSD_APP_TETRA_TEXT_SIZE];      /**< "TMO downlink · encrypted (TEA2)", "DMO direct · clear". */
    char network_text[DSD_APP_TETRA_TEXT_SIZE];   /**< "MCC 262 · MNC 1001 · CC 25". */
    char trunk_text[DSD_APP_TETRA_TEXT_SIZE];     /**< "control channel", "traffic channel", "idle". */
    char control_text[DSD_APP_TETRA_TEXT_SIZE];   /**< "carrier 321 · 392.0125 MHz". */
    char traffic_text[DSD_APP_TETRA_TEXT_SIZE];   /**< "carrier 654 · TS 1,3 · 392.1250 MHz". */
    char mm_status_text[DSD_APP_TETRA_TEXT_SIZE]; /**< "7 · MS frequency bands request". */
    char codec_text[DSD_APP_TETRA_TEXT_SIZE];     /**< "ready · 18 frames · 2 errors", "missing". */
} dsd_app_tetra;

/**
 * @brief Fill @p out from a snapshot of the TETRA publication.
 *
 * Zeroes @p out first. The rows are visible while the options run the TETRA decoder (the TETRA preset) and the
 * publication has a working mode, which it takes only once a block passed its CRC. The trunk row shows while trunking
 * is on and the mode is the trunked downlink. A frequency reads in MHz with five decimals, the fifth dropped when it is
 * a zero. @p flags is DSD_APP_TETRA_ASCII or 0.
 *
 * @return 1 when the rows should be shown, 0 when not, and -1 for a NULL @p opts, @p state or @p out.
 */
int dsd_app_tetra_view(const dsd_opts* opts, const dsd_state* state, unsigned flags, dsd_app_tetra* out);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_TETRA_VIEW_H_ */
