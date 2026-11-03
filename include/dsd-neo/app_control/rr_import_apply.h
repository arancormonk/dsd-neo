// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend boundary for applying a finished RadioReference import.
 *
 * Carries the by-value payload the app-command queue copies to the decoder
 * thread, and the pure plan -> payload mapper both frontends share. Including
 * <dsd-neo/runtime/radioreference_import.h> here is legal: the ARCH_RULES
 * public-frontend rule bans only core/opts.h, core/state.h, dsd-neo/io/,
 * dsd-neo/protocol/, curses and Qt from include/dsd-neo/app_control/.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RR_IMPORT_APPLY_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RR_IMPORT_APPLY_H_

#include <dsd-neo/runtime/radioreference_import.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Everything DSD_APP_CMD_RR_APPLY_IMPORT needs, by value. */
typedef struct {
    int32_t decode_mode;           /**< dsdneoUserDecodeMode as int32. */
    uint8_t edacs_ea;              /**< state->ea_mode. */
    uint8_t edacs_esk;             /**< state->esk_mask = 0xA0 when set. */
    uint8_t simulcast_qpsk;        /**< force QPSK after the preset. */
    uint8_t p25_prefer_candidates; /**< opts->p25_prefer_candidates (-^). */
    uint8_t trunking;              /**< mutually exclusive with scanner. */
    uint8_t scanner;               /**< opts->scanner_mode (-Y). */
    uint8_t has_chan;              /**< chan_path is meaningful. */
    uint8_t has_group;             /**< group_path is meaningful. */
    uint32_t tune_hz;              /**< 0 = nothing to tune. */
    char chan_path[1024];
    char group_path[1024];
} dsd_app_rr_apply_payload;

/** @brief Account fields DSD_APP_CMD_RR_ACCOUNT_SET mirrors into dsd_opts. */
typedef struct {
    char username[128]; /**< matches dsd_rr_auth::username. */
    char app_key[64];   /**< matches dsd_rr_auth::app_key. */
} dsd_app_rr_account_payload;

/**
 * @brief Map a finished import plan onto the apply payload. Pure; no session.
 *
 * @param plan       Built by dsd_rr_import_plan_build(); must have plan->ok.
 * @param chan_path  Written channel-map path, or NULL/"" when none.
 * @param group_path Written group-list path, or NULL/"" when none.
 * @param out        Filled on success; zeroed first.
 * @return 0 on success, -1 when the plan is not ok, the protocol has no decode
 *         mode, or plan->tune_hz does not fit an unsigned 32-bit Hz value.
 */
int dsd_app_rr_fill_apply_payload(const dsd_rr_import_plan* plan, const char* chan_path, const char* group_path,
                                  dsd_app_rr_apply_payload* out);

/**
 * @brief Map only what an import selects for the session - its decode mode and its one tuner owner (trunking, or the
 *        -Y scanner) - onto the payload. Pure; no session.
 *
 * dsd_app_rr_fill_apply_payload() starts from it. A frontend uses it alone to ask dsd_app_rr_apply_block_reason()
 * about a plan that is not finished yet (still waiting for its selection), whose decode mode and tuner owner are
 * already known.
 *
 * @param plan Any plan from dsd_rr_import_plan_build() or dsd_rr_recipe_to_plan(); plan->ok is not required.
 * @param out  Zeroed, then filled with decode_mode, trunking and scanner.
 * @return 0 on success, -1 when the protocol has no decode mode (@p out is then untouched).
 */
int dsd_app_rr_fill_apply_mode(const dsd_rr_import_plan* plan, dsd_app_rr_apply_payload* out);

/** @brief What delivers the samples an import's decode mode would run on (dsd_app_rr_receiver_facts::input_kind). */
typedef enum {
    DSD_APP_RR_INPUT_AUDIO = 0,     /**< demodulated audio: PulseAudio, a WAV file, TCP/UDP PCM, stdin */
    DSD_APP_RR_INPUT_RTL = 1,       /**< an RTL-SDR or rtl_tcp radio, whose DSP rate is its DSP bandwidth */
    DSD_APP_RR_INPUT_SOAPY = 2,     /**< a SoapySDR device, whose DSP rate is its DSP bandwidth (issue #806) */
    DSD_APP_RR_INPUT_AIRSPY = 3,    /**< an Airspy, whose DSP rate is its DSP bandwidth too (issue #806) */
    DSD_APP_RR_INPUT_IQ_REPLAY = 4, /**< an I/Q replay, which a mode change re-plans or refuses (issue #701) */
    DSD_APP_RR_INPUT_SYMBOLS = 5,   /**< a symbol capture (.bin dibits, or float symbols) */
} dsd_app_rr_input_kind;

/**
 * @brief The session facts an import is checked against, in plain values (issue #803).
 *
 * The decoder thread fills them from the live options, a frontend from the published snapshots: every live radio
 * runs at its DSP bandwidth (issue #806), so the bandwidth decides and no measured rate is needed. has_group, has_chan, trunking and scanner are what the import
 * would leave (before its files exist, in a frontend), for the next-start check beside the verdict.
 */
typedef struct {
    int32_t decode_mode;     /**< dsdneoUserDecodeMode the import selects */
    int input_kind;          /**< dsd_app_rr_input_kind */
    int rtl_dsp_bw_khz;      /**< dsd_opts::rtl_dsp_bw_khz, the committed DSP bandwidth */
    int tetra_uplink;        /**< dsd_opts::tetra_uplink (--tetra-uplink) */
    int trunk_scan_running;  /**< dsd_opts::trunk_scan_enabled == 1 */
    int has_group, has_chan; /**< the import loads a group list / a channel map */
    int trunking, scanner;   /**< the tuner owner the import sets */
} dsd_app_rr_receiver_facts;

/** @brief Buffer size that holds every dsd_app_rr_receiver_verdict() and dsd_app_rr_apply_block_reason() text. */
#define DSD_APP_RR_VERDICT_WHY_MAX 128

/**
 * @brief Whether the session can take an import at all, and decode what it selects. Pure.
 *
 * Refuses an import while a trunk scan runs (it keeps its own channel maps), AM or TETRA on demodulated audio, and
 * TETRA where it decodes nothing: a live radio (RTL-SDR, rtl_tcp, SoapySDR or Airspy) off the 72 kHz DSP bandwidth (no
 * live reopen: it would restart an --iq-capture), or a --tetra-uplink session. An I/Q replay and a symbol capture
 * pass; a replay's mode change re-plans or refuses on its own.
 *
 * @param facts    The session and the import.
 * @param why      Receives the reason as a phrase for "Failed: RR import -> <why>", or "" when it passes; may be NULL.
 * @param why_size Size of @p why; DSD_APP_RR_VERDICT_WHY_MAX holds every text.
 * @return 0 when the import may go on, 1 when it is refused (and for NULL @p facts).
 */
int dsd_app_rr_receiver_verdict(const dsd_app_rr_receiver_facts* facts, char* why, size_t why_size);

/**
 * @brief A frontend's advisory pre-check of an import before it writes files or queues the apply.
 *
 * Reads only the published snapshots (dsd_app_get_latest_opts_snapshot(), dsd_app_get_latest_snapshot()), so call it
 * from the frontend's snapshot thread. Runs dsd_app_rr_receiver_verdict() with no measured rate (a SoapySDR device
 * passes to the decoder), then the next-start rule the decoder thread applies (a trunk scan waiting for the next start
 * cannot start beside a channel map or the -Y scanner), on the snapshot copies. The decoder thread checks again.
 *
 * @param p          The payload the import would queue; its paths may be empty before the files are written.
 * @param has_group  1 when the import will load a group list (from the plan, before any path exists).
 * @param has_chan   1 when the import will load a channel map.
 * @param why        Receives the reason as a phrase ("TETRA needs ..."), or "" when nothing blocks; may be NULL.
 * @param why_size   Size of @p why; DSD_APP_RR_VERDICT_WHY_MAX holds every text.
 * @return 1 when the import is blocked, 0 otherwise (also before the first publish and for NULL @p p).
 */
int dsd_app_rr_apply_block_reason(const dsd_app_rr_apply_payload* p, int has_group, int has_chan, char* why,
                                  size_t why_size);

/**
 * @brief A frontend's advisory pre-check of loading an import's files alone, which keeps the decode mode and the tuner
 *        owner (the Imported Systems browser's "Load these files", for an import with no saved settings).
 *
 * Reads only the published snapshots, like dsd_app_rr_apply_block_reason(), and asks what loading the files asks on
 * the decoder thread: no running trunk scan, and a channel map that the next-start rule allows beside a trunk scan
 * waiting for the next start. A group list has no session rule.
 *
 * @param chan_path The channel map to load, or NULL/"" when there is none.
 * @param why       Receives the reason as a phrase, or "" when nothing blocks; may be NULL.
 * @param why_size  Size of @p why; DSD_APP_RR_VERDICT_WHY_MAX holds every text.
 * @return 1 when loading is blocked, 0 otherwise (also before the first publish).
 */
int dsd_app_rr_files_block_reason(const char* chan_path, char* why, size_t why_size);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_RR_IMPORT_APPLY_H_ */
