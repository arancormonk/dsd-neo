// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/**
 * @file
 * @brief NXDN control-channel hunt for single-system trunking.
 *
 * NXDN has no trunk state machine: grants, the DISC return and the engine's hangtime return follow a control channel
 * the decoder already knows. This module finds that control channel. A session whose control channel yields no
 * validated NXDN content within the grace window walks its channel map, two seconds a row, until a CAC that passed
 * its CRC (only an outbound control channel carries one) names the control channel. It runs at session start, with no
 * protocol proven yet, and after a control channel a CAC confirmed goes quiet.
 *
 * An IDAS Type-D site has no dedicated control channel: a confirmed SCCH names NXDN the owner, which ends a startup
 * hunt where it is, and never arms a hunt afterwards.
 *
 * Every function runs on the decoder thread; the ticks run under the P25 SM tick guard. The context lives in the
 * DSD_STATE_EXT_PROTO_NXDN_CC_HUNT state extension.
 */
#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_CC_HUNT_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_CC_HUNT_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Seconds without validated NXDN content before the hunt starts. */
#define NXDN_CC_HUNT_GRACE_S        6.0
/** Seconds each probed row gets. */
#define NXDN_CC_HUNT_PROBE_S        2.0
/** Seconds a probe or settle tune may stay unanswered before it is failed. */
#define NXDN_CC_HUNT_TUNE_TIMEOUT_S 5.0

/**
 * @brief Periodic tick: arms the window, starts, runs, settles and parks the hunt.
 *
 * Only a session dsd_trunk_nxdn_recovery_allowed() admits arms or hunts; a hunt it no longer admits settles and parks
 * here. A tune in flight that the hunt does not own (a call's return to the control channel, say) restarts the window,
 * and the window is dropped while the receiver is on a granted voice channel.
 */
void nxdn_cc_hunt_tick_locked(dsd_opts* opts, dsd_state* state);

/**
 * @brief A CAC passed its CRC on the tuned frequency.
 *
 * With trunking on and the receiver off any voice channel, names NXDN the owner, makes the frequency the control
 * channel (p25_cc_freq and trunk_cc_freq) and ends a hunt there. A probe whose tune is still unresolved is not
 * credited. The frequency is attributed once per tuning generation, so a rigctl query is not repeated per CAC.
 */
void nxdn_cc_hunt_note_control(const dsd_opts* opts, dsd_state* state);

/**
 * @brief A Type-D SCCH confirmed the transmission: names NXDN the owner and ends a hunt where it is.
 *
 * Only SCCHs of NXDN_CONFIRM_WEAK_OBSERVES frames running count (nxdn_confirm_scch_confirmed()), on a control or a
 * voice channel.
 *
 * A Type-D site has no continuous control channel, so a control channel a CAC confirmed earlier is forgotten and no
 * hunt arms afterwards.
 */
void nxdn_cc_hunt_note_type_d(const dsd_opts* opts, dsd_state* state);

/**
 * @brief The operator moved the receiver.
 *
 * Forgets the frequency attribution and the per-carrier NXDN evidence and restarts an armed window at the move; a
 * running hunt drops its probe and parks, keeping that window, once the tuning layer is settled.
 */
void nxdn_cc_hunt_note_retune(const dsd_opts* opts, dsd_state* state);

/**
 * @brief A fresh tuner reading (a site message's query): the attribution takes it, so the next CAC agrees with the
 * control channel it anchored, a receiver moved outside the decoder (an external VFO change) included.
 */
void nxdn_cc_hunt_note_tuner_frequency(const dsd_state* state, long freq_hz);

/**
 * @brief The frequency the tuner says the receiver is on (rigctl, else the RTL centre), attributed once per tuning
 * generation as a CAC's is: a rigctl query is a network round trip, asked at most once per generation, a failed one
 * included. A receiver moved outside the decoder within a generation is not seen until the next one.
 *
 * @return the frequency in Hz; 0 when no tuner names one.
 */
long nxdn_cc_hunt_receiver_frequency(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Forget which frequency the receiver is on: a new rigctl connection's peer (a restarted receiver, say) may sit
 * anywhere, without any tune the decoder made. The next CAC asks the tuner again.
 */
void nxdn_cc_hunt_forget_attribution(const dsd_state* state);

/**
 * @brief The session was re-pointed at another system (a RadioReference apply).
 *
 * Forgets the control channel a CAC confirmed and returns an NXDN owner to unknown, so the new system's start is hunted
 * whatever the old one was (a Type-D site never hunts once proven).
 */
void nxdn_cc_hunt_note_system_replaced(dsd_state* state);

/** @brief Return 1 while the window is armed or a hunt runs. */
int nxdn_cc_hunt_active(const dsd_state* state);

/** @brief Return 1 while the hunt is active or the next tick would arm it. */
int nxdn_cc_hunt_due(const dsd_opts* opts, const dsd_state* state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_NXDN_NXDN_CC_HUNT_H */
