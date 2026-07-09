// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file
 * @brief P25 trunking state-machine interfaces and constants.
 *
 * This is the unified P25 trunking state machine. Design goals:
 *   - Single state machine for both P25P1 and P25P2
 *   - Minimal timing parameters (hangtime, grant_timeout, cc_grace)
 *   - Single timestamp-based activity tracking per slot
 *   - Unified release path with clear semantics
 *   - Event-driven transitions matching OP25's simpler model
 */
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_TRUNK_SM_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_TRUNK_SM_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * High-level trunk SM mode (for UI/telemetry)
 * ============================================================================ */

typedef enum {
    DSD_P25_SM_MODE_UNKNOWN = 0,
    DSD_P25_SM_MODE_ON_CC = 1,
    DSD_P25_SM_MODE_ON_VC = 2,
    DSD_P25_SM_MODE_HANG = 3,
    DSD_P25_SM_MODE_HUNTING = 4,
    // Extended states for richer UI/telemetry across P1/P2
    DSD_P25_SM_MODE_ARMED = 5,     // tuned to VC, awaiting PTT/ACTIVE
    DSD_P25_SM_MODE_FOLLOW = 6,    // following active voice
    DSD_P25_SM_MODE_RETURNING = 7, // teardown in progress back to CC
} dsd_p25_sm_mode_e;

/* ============================================================================
 * State Machine States (4-state model aligned with OP25)
 * ============================================================================ */

typedef enum {
    P25_SM_IDLE = 0, // Not trunking or no CC known
    P25_SM_ON_CC,    // Parked on control channel, listening for grants
    P25_SM_TUNED,    // On voice channel (awaiting voice, active, or hangtime)
    P25_SM_HUNTING,  // Lost CC, searching candidates
} p25_sm_state_e;

enum {
    // Grant decoder sentinel for opcodes that do not carry service options.
    // Passing svc_bits=0 remains an explicit clear service option for compatibility.
    P25_SM_SVC_UNKNOWN = -1,
};

/* ============================================================================
 * Events
 * ============================================================================ */

typedef enum {
    P25_SM_EV_GRANT = 0, // Channel grant received (channel, freq, tg, src, svc_bits)
    P25_SM_EV_PTT,       // MAC_PTT on slot
    P25_SM_EV_ACTIVE,    // MAC_ACTIVE on slot
    P25_SM_EV_END,       // MAC_END on slot
    P25_SM_EV_IDLE,      // MAC_IDLE on slot
    P25_SM_EV_TDU,       // P1 Terminator Data Unit
    P25_SM_EV_CC_SYNC,   // Control channel sync acquired
    P25_SM_EV_VC_SYNC,   // Voice channel sync acquired
    P25_SM_EV_SYNC_LOST, // Sync lost
    P25_SM_EV_ENC,       // Encryption params detected on slot (algid, keyid)
} p25_sm_event_type_e;

typedef struct {
    p25_sm_event_type_e type;
    int slot;               // 0 or 1 for TDMA, -1 for P1/N/A
    int channel;            // 16-bit channel number (for GRANT)
    long freq_hz;           // Frequency in Hz (for GRANT)
    int tg;                 // Talkgroup (for GRANT, 0 if individual)
    int src;                // Source RID (for GRANT)
    int dst;                // Destination RID (for individual GRANT)
    int svc_bits;           // Service options (for GRANT), or P25_SM_SVC_UNKNOWN when absent
    int is_group;           // 1 for group grant, 0 for individual
    int algid;              // Algorithm ID (for ENC event)
    int keyid;              // Key ID (for ENC event)
    int data_call_override; // 0=infer from svc_bits, 1=force data, -1=force non-data
    double observed_m;      // Optional monotonic timestamp when the event was observed
} p25_sm_event_t;

/* ============================================================================
 * Configuration
 * ============================================================================ */

typedef struct {
    double hangtime_s;      // Hangtime after voice ends (default 2.0s)
    double grant_timeout_s; // Max wait for voice after grant (default 3.0s)
    double cc_grace_s;      // Wait before CC hunting (default 5.0s)
} p25_sm_config_t;

/* ============================================================================
 * Per-Slot Activity Context
 * ============================================================================ */

typedef struct {
    double last_active_m; // Monotonic timestamp of last activity (PTT/ACTIVE/voice)
    int voice_active;     // 1 if voice is currently active on this slot
    int algid;            // Current algorithm ID for this slot
    int keyid;            // Current key ID for this slot
    int tg;               // Current talkgroup for this slot
    int grant_active;     // 1 if this TDMA slot has an accepted grant context
    long freq_hz;         // Accepted grant RF frequency
    int channel;          // Accepted grant channel number
    int target_id;        // Policy-selected target ID, or OTA target when no policy remap
    int ota_tg;           // OTA talkgroup for group grants
    int src;              // Source RID
    int dst;              // Destination RID for individual grants
    int is_group;         // 1 for group call, 0 for individual/private
    int data_call;        // 1 for data grant, 0 for voice
    int svc_bits;         // Service options, or P25_SM_SVC_UNKNOWN when absent
    double last_grant_m;  // Monotonic timestamp of last accepted grant for this slot
} p25_sm_slot_ctx_t;

/* ============================================================================
 * State Machine Context
 * ============================================================================ */

typedef struct {
    // Current state
    p25_sm_state_e state;

    // Configuration (cached from opts or defaults)
    p25_sm_config_t config;

    // Voice channel context (valid when state >= ARMED)
    long vc_freq_hz;
    int vc_channel;
    int vc_tg;
    int vc_src;
    int vc_is_tdma;          // 1 if TDMA channel, 0 if single-carrier
    int vc_data_call;        // 1 if the accepted grant is data, 0 if voice
    int vc_cqpsk_retry_done; // 1 once we retried VC tune with alternate CQPSK DSP mode for this grant

    // Per-slot activity (index 0 = left/P1, index 1 = right)
    p25_sm_slot_ctx_t slots[2];

    // Timing
    double t_tune_m;     // Monotonic time of last VC tune
    double t_voice_m;    // Monotonic time of last voice activity
    double t_hangtime_m; // Monotonic time hangtime started
    double t_cc_sync_m;  // Monotonic time of last CC sync
    double t_cc_tune_m;  // Monotonic time of last CC tune awaiting decode
    double t_hunt_try_m; // Monotonic time of last CC candidate attempt
    int cc_sync_pending; // 1 until a CC tune is followed by decoded CC sync

    // Statistics (for debugging/UI)
    uint32_t tune_count;
    uint32_t release_count;
    uint32_t grant_count;
    uint32_t cc_return_count;

    // NAC mismatch tracking: counts consecutive frames where the decoded NAC
    // differs from the expected CC NAC. After a threshold, triggers cc-lost
    // to avoid dwelling on a wrong-NAC channel for seconds. Keep the expected
    // NAC in the SM because state->p2_cc can be refreshed by each decoded P1 NID.
    int expected_cc_nac;
    int nac_mismatch_count;

    // Initialized flag
    int initialized;
} p25_sm_ctx_t;

/* ============================================================================
 * Public API - Core State Machine
 * ============================================================================ */

/**
 * @brief Initialize the unified P25 state machine.
 *
 * Reads timing parameters from opts/env, sets initial state based on CC presence.
 *
 * @param ctx State machine context to initialize.
 * @param opts Decoder options (may be NULL for defaults).
 * @param state Decoder state (may be NULL).
 */
void p25_sm_init_ctx(p25_sm_ctx_t* ctx, const dsd_opts* opts, dsd_state* state);

/**
 * @brief Process an event and update state machine.
 *
 * This is the main entry point for all P25 signaling events.
 *
 * @param ctx State machine context.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param ev Event to process.
 */
void p25_sm_event(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev);

/**
 * @brief Seed an unknown P25 control-channel frequency from known CC state.
 *
 * Control-channel grant decoders call this before gating on state->p25_cc_freq
 * so the first live grant can establish the CC frequency for return-to-CC
 * behavior. The generic trunk CC alias is preferred when it is already known.
 * Otherwise the tuner is sampled only while legacy tune flags indicate the
 * decoder is parked on the control channel; voice-channel tunes are ignored.
 * Live P25 Phase 2 LCCH context seeds a TDMA CC modulation hint; ordinary
 * Phase 2 traffic sync leaves the hint unchanged. Live P25 Phase 1 sync clears
 * any stale TDMA CC hint.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_seed_cc_from_current_tuner_if_unknown(const dsd_opts* opts, dsd_state* state);

/**
 * @brief Restart a pending CC acquisition after an accepted external retune.
 *
 * This is a no-op unless the context is already awaiting decoded CC activity.
 * It refreshes the acquisition baseline and returns a hunting context to
 * ON_CC so the normal acquisition watchdog grants the new tune its full grace
 * period.
 *
 * @param ctx State machine context.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param tune_start_m Monotonic timestamp captured after retune acceptance.
 * @param source Short diagnostic source label.
 * @return 1 when pending acquisition was restarted, 0 otherwise.
 */
int p25_sm_restart_pending_cc_acquisition(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, double tune_start_m,
                                          const char* source);

/**
 * @brief Periodic tick for timeout-based transitions.
 *
 * Call at ~1-10 Hz. Handles:
 *   - ARMED -> ON_CC (grant timeout)
 *   - HANGTIME -> ON_CC (hangtime expired)
 *   - ON_CC -> HUNTING (CC lost)
 *
 * @param ctx State machine context.
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_tick_ctx(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state);

/**
 * @brief Get current state machine state.
 *
 * @param ctx State machine context.
 * @return Current state.
 */
static inline p25_sm_state_e
p25_sm_get_state(const p25_sm_ctx_t* ctx) {
    return ctx ? ctx->state : P25_SM_IDLE;
}

/**
 * @brief Get human-readable state name.
 *
 * @param state State to convert.
 * @return Static string with state name.
 */
const char* p25_sm_state_name(p25_sm_state_e state);

/**
 * @brief Access the global singleton state machine instance.
 *
 * Returns a process-global instance initialized with default callbacks.
 *
 * @return Pointer to global state machine context.
 */
p25_sm_ctx_t* p25_sm_get_ctx(void);

/**
 * @brief Trigger explicit release and return to CC.
 *
 * @param ctx State machine context.
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param reason Log tag for release reason.
 */
void p25_sm_release(p25_sm_ctx_t* ctx, dsd_opts* opts, dsd_state* state, const char* reason);

/**
 * @brief Check if audio output is allowed for a slot.
 *
 * Centralized audio gating decision. Decoders should call this before
 * pushing audio to output buffers.
 *
 * @param ctx State machine context (NULL to use global).
 * @param state Decoder state.
 * @param slot Slot index (0 or 1, -1 for P1).
 * @return 1 if audio is allowed, 0 if muted.
 */
int p25_sm_audio_allowed(const p25_sm_ctx_t* ctx, const dsd_state* state, int slot);

/**
 * @brief Check whether a TDMA slot has an accepted grant newer than a captured event time.
 *
 * Decoders use this after processing a MAC payload but before clearing IDLE
 * slot metadata, so grants carried inside the IDLE payload can keep their
 * policy and service context.
 *
 * @param slot Slot index (0 or 1).
 * @param observed_m Monotonic timestamp captured before payload processing.
 * @return 1 if the global state machine has a newer active grant for the slot, 0 otherwise.
 */
int p25_sm_slot_grant_newer_than(int slot, double observed_m);

/**
 * @brief Update audio gating for a slot based on current encryption state.
 *
 * Called when encryption parameters are received to update allow_audio.
 *
 * @param ctx State machine context.
 * @param state Decoder state.
 * @param slot Slot index.
 * @param algid Algorithm ID.
 * @param keyid Key ID.
 */
void p25_sm_update_audio_gate(p25_sm_ctx_t* ctx, dsd_state* state, int slot, int algid, int keyid);

/* ============================================================================
 * Public API - Convenience Emit Functions (use global singleton)
 * ============================================================================ */

/**
 * @brief Emit an event to the global state machine.
 *
 * Convenience function for decoders to emit events without managing context.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param ev Event to emit.
 */
void p25_sm_emit(dsd_opts* opts, dsd_state* state, const p25_sm_event_t* ev);

/**
 * @brief Emit PTT event for a slot.
 */
void p25_sm_emit_ptt(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Emit ACTIVE event for a slot.
 */
void p25_sm_emit_active(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Emit END event for a slot.
 */
void p25_sm_emit_end(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Emit IDLE event for a slot.
 */
void p25_sm_emit_idle(dsd_opts* opts, dsd_state* state, int slot);

/**
 * @brief Emit IDLE event with a previously captured monotonic observation timestamp.
 */
void p25_sm_emit_idle_at(dsd_opts* opts, dsd_state* state, int slot, double observed_m);

/**
 * @brief Emit TDU (P1 terminator) event.
 */
void p25_sm_emit_tdu(dsd_opts* opts, dsd_state* state);

/**
 * @brief Emit ENC event for a slot (encryption params detected).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param slot Slot index (0 or 1).
 * @param algid Algorithm ID.
 * @param keyid Key ID.
 * @param tg Talkgroup associated with this call.
 */
void p25_sm_emit_enc(dsd_opts* opts, dsd_state* state, int slot, int algid, int keyid, int tg);

/* ============================================================================
 * Public API - Neighbor/CC Candidate Management
 * ============================================================================ */

/**
 * @brief Process neighbor frequency update from control channel.
 *
 * Adds frequencies to the CC candidate list for hunting.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param freqs Array of neighbor frequencies in Hz.
 * @param count Number of frequencies in array.
 */
void p25_sm_on_neighbor_update(dsd_opts* opts, dsd_state* state, const long* freqs, int count);

/**
 * @brief Get next CC candidate frequency for hunting.
 *
 * @param state Decoder state.
 * @param out_freq Output: next candidate frequency in Hz.
 * @return 1 if a candidate was found, 0 if none available.
 */
int p25_sm_next_cc_candidate(dsd_state* state, long* out_freq);

/* ============================================================================
 * Public API - Legacy Compatibility Wrappers
 * ============================================================================ */

/**
 * @brief Initialize any internal P25 trunking state.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_init(dsd_opts* opts, dsd_state* state);

/**
 * @brief Handle a group voice channel grant (explicit form).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Voice channel number.
 * @param svc_bits Service options associated with the grant, or P25_SM_SVC_UNKNOWN when absent.
 * @param tg Talkgroup.
 * @param src Source RID.
 */
void p25_sm_on_group_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src);

/**
 * @brief Handle a group data channel grant (explicit data policy form).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Data channel number.
 * @param svc_bits Service options associated with the grant, or P25_SM_SVC_UNKNOWN when absent.
 * @param tg Talkgroup.
 * @param src Source RID.
 */
void p25_sm_on_group_data_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src);

/**
 * @brief Apply group grant policy side effects without attempting route/tune.
 *
 * Use when a decoder has a valid group grant but cannot yet resolve or follow
 * its channel. This preserves encrypted lockout/cache and clear-key regroup
 * policy behavior until a tunable grant arrives.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Voice channel number.
 * @param svc_bits Service options associated with the grant, or P25_SM_SVC_UNKNOWN when absent.
 * @param tg Talkgroup.
 * @param src Source RID.
 */
void p25_sm_apply_group_grant_policy(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int tg, int src);

/**
 * @brief Handle an individual (unit-to-unit/telephone) voice channel grant.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Voice channel number.
 * @param svc_bits Service options associated with the grant, or P25_SM_SVC_UNKNOWN when absent.
 * @param dst Destination RID.
 * @param src Source RID.
 */
void p25_sm_on_indiv_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src);

/**
 * @brief Handle an individual data channel grant (explicit data policy form).
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param channel Data channel number.
 * @param svc_bits Service options associated with the grant, or P25_SM_SVC_UNKNOWN when absent.
 * @param dst Destination RID.
 * @param src Source RID.
 */
void p25_sm_on_indiv_data_grant(dsd_opts* opts, dsd_state* state, int channel, int svc_bits, int dst, int src);

/**
 * @brief Handle an explicit release/end-of-call indication.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_on_release(dsd_opts* opts, dsd_state* state);

/**
 * @brief Notify the SM that a Queued Response (QUE_RSP) was received.
 *
 * Requests a release with reason "queued-rsp"; the release path is idempotent
 * unless the SM or legacy tune flags indicate a voice-channel tune is active.
 * Always increments the queued response telemetry counter.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param svc_type Service type from the QUE_RSP message.
 * @param reason_code Reason code from the QUE_RSP message.
 * @param target Target address from the QUE_RSP message.
 */
void p25_sm_on_queued_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target);

/**
 * @brief Notify the SM that a Deny Response (DENY_RSP) was received.
 *
 * Requests a release with reason "deny-rsp"; the release path is idempotent
 * unless the SM or legacy tune flags indicate a voice-channel tune is active.
 * Always increments the deny response telemetry counter.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param svc_type Service type from the DENY_RSP message.
 * @param reason_code Reason code from the DENY_RSP message.
 * @param target Target address from the DENY_RSP message.
 */
void p25_sm_on_deny_response(dsd_opts* opts, dsd_state* state, int svc_type, int reason_code, int target);

/**
 * @brief Optional periodic heartbeat/tick for safety fallback.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 */
void p25_sm_tick(dsd_opts* opts, dsd_state* state);

/* ============================================================================
 * Helper: SACCH slot mapping
 * ============================================================================ */

/**
 * @brief Convert SACCH currentslot to voice channel slot.
 *
 * P25 Phase 2 SACCH uses inverted slot mapping relative to voice frames.
 * Use this helper at SM event emission points for consistency.
 *
 * @param currentslot The currentslot value from state.
 * @return Voice channel slot index (0 or 1).
 */
static inline int
p25_sacch_to_voice_slot(int currentslot) {
    return (currentslot ^ 1) & 1;
}

/* ============================================================================
 * Helper: Create events from common scenarios
 * ============================================================================ */

static inline p25_sm_event_t
p25_sm_ev_group_grant(int channel, long freq_hz, int tg, int src, int svc_bits) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_GRANT;
    ev.slot = -1;
    ev.channel = channel;
    ev.freq_hz = freq_hz;
    ev.tg = tg;
    ev.src = src;
    ev.svc_bits = svc_bits;
    ev.is_group = 1;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_indiv_grant(int channel, long freq_hz, int dst, int src, int svc_bits) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_GRANT;
    ev.slot = -1;
    ev.channel = channel;
    ev.freq_hz = freq_hz;
    ev.dst = dst;
    ev.src = src;
    ev.svc_bits = svc_bits;
    ev.is_group = 0;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_group_data_grant(int channel, long freq_hz, int tg, int src, int svc_bits) {
    p25_sm_event_t ev = p25_sm_ev_group_grant(channel, freq_hz, tg, src, svc_bits);
    ev.data_call_override = 1;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_indiv_data_grant(int channel, long freq_hz, int dst, int src, int svc_bits) {
    p25_sm_event_t ev = p25_sm_ev_indiv_grant(channel, freq_hz, dst, src, svc_bits);
    ev.data_call_override = 1;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_ptt(int slot) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_PTT;
    ev.slot = slot;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_active(int slot) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_ACTIVE;
    ev.slot = slot;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_end(int slot) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_END;
    ev.slot = slot;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_idle(int slot) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_IDLE;
    ev.slot = slot;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_idle_at(int slot, double observed_m) {
    p25_sm_event_t ev = p25_sm_ev_idle(slot);
    ev.observed_m = observed_m;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_tdu(void) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_TDU;
    ev.slot = -1;
    return ev;
}

static inline p25_sm_event_t
p25_sm_ev_enc(int slot, int algid, int keyid, int tg) {
    p25_sm_event_t ev = {0};
    ev.type = P25_SM_EV_ENC;
    ev.slot = slot;
    ev.algid = algid;
    ev.keyid = keyid;
    ev.tg = tg;
    return ev;
}

/* ============================================================================
 * Patch group (P25 regroup/patch) tracking helpers
 * ============================================================================ */

/**
 * @brief Record or update a P25 regroup/patch state for a Super Group ID (SGID).
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param is_patch 1 for two-way patch, 0 for simulselect (one-way regroup).
 * @param active 1 to activate, 0 to deactivate/clear.
 */
void p25_patch_update(dsd_state* state, int sgid, int is_patch, int active);

/**
 * @brief Compose a compact summary string for active patch SGIDs.
 *
 * Example output: "P: 069,142".
 *
 * @param state Decoder state (read-only).
 * @param out Destination buffer for summary string.
 * @param cap Capacity of destination buffer.
 * @return Length written (0 when none active).
 */
int p25_patch_compose_summary(const dsd_state* state, char* out, size_t cap);

/**
 * @brief Add a Working Group ID to an SGID entry (creates/activates if needed).
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wgid Working Group ID to add.
 */
void p25_patch_add_wgid(dsd_state* state, int sgid, int wgid);

/**
 * @brief Add a Working Unit ID to an SGID entry (creates/activates if needed).
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wuid Working Unit ID to add.
 */
void p25_patch_add_wuid(dsd_state* state, int sgid, uint32_t wuid);

/**
 * @brief Compose a detailed status string including WGID/WUID context.
 *
 * Example: "SG069[P] WG:2(0345,0789); SG142[S] U:3".
 *
 * @param state Decoder state (read-only).
 * @param out Destination buffer.
 * @param cap Capacity of destination buffer.
 * @return Length written.
 */
int p25_patch_compose_details(const dsd_state* state, char* out, size_t cap);

/**
 * @brief Remove a WGID membership or clear the entire SG record.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wgid Working Group ID to remove.
 */
void p25_patch_remove_wgid(dsd_state* state, int sgid, int wgid);
/**
 * @brief Remove a WUID membership from an SG record.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param wuid Working Unit ID to remove.
 */
void p25_patch_remove_wuid(dsd_state* state, int sgid, uint32_t wuid);
/**
 * @brief Clear all membership and status for an SG record.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID to clear.
 */
void p25_patch_clear_sg(dsd_state* state, int sgid);

/**
 * @brief Prepare an MFID GRG update, clearing inactive or version-replaced state.
 *
 * Inactive GRG commands remove the supergroup record. Active commands with a
 * changed SSN clear stale membership before the caller adds the new members.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param is_patch 1 for two-way patch, 0 for simulselect.
 * @param active 1 to activate/update, 0 to deactivate/clear.
 * @param ssn SSN from GRG options, or -1 when absent.
 * @return 1 if members should be processed, 0 when the command cleared state.
 */
int p25_patch_prepare_grg_update(dsd_state* state, int sgid, int is_patch, int active, int ssn);

/**
 * @brief Set optional Key/Alg/SSN context for an SG.
 *
 * Values of -1 leave the existing field unchanged.
 *
 * @param state Decoder state holding patch context.
 * @param sgid Super Group ID.
 * @param key Key ID (or -1 to leave unchanged).
 * @param alg Algorithm ID (or -1 to leave unchanged).
 * @param ssn SSN ID (or -1 to leave unchanged).
 */
void p25_patch_set_kas(dsd_state* state, int sgid, int key, int alg, int ssn);

/**
 * @brief Return 1 if TG is a WGID within an active SG that is explicitly clear.
 *
 * Used to override ENC lockout when GRG commands indicate a clear operation
 * for a patch/regroup.
 *
 * @param state Decoder state (read-only).
 * @param tg Talkgroup to test.
 * @return 1 if the TG maps to a clear SG, 0 otherwise.
 */
int p25_patch_tg_key_is_clear(const dsd_state* state, int tg);
/**
 * @brief Return 1 if an SGID has explicit KEY=0 (clear) policy and is active.
 *
 * @param state Decoder state (read-only).
 * @param sgid Super Group ID to query.
 * @return 1 if active and explicitly clear; 0 otherwise.
 */
int p25_patch_sg_key_is_clear(const dsd_state* state, int sgid);

/**
 * @brief Collect active WGID members for a supergroup.
 *
 * Stale, inactive, and non-existent supergroups return 0.
 *
 * @param state Decoder state (read-only).
 * @param sgid Super Group ID.
 * @param out Optional WGID destination array.
 * @param cap Capacity of destination array.
 * @return Number of active WGID members known for the SG.
 */
int p25_patch_collect_active_wgids(const dsd_state* state, int sgid, uint16_t* out, size_t cap);

/* ============================================================================
 * Affiliation (RID) tracking
 * ============================================================================ */

/**
 * @brief Record a RID as affiliated/registered (updates last_seen or adds new entry).
 *
 * @param state Decoder state holding affiliation table.
 * @param rid Radio ID to register.
 */
void p25_aff_register(dsd_state* state, uint32_t rid);

/**
 * @brief Remove a RID from the affiliation table (explicit deregistration or aging).
 *
 * @param state Decoder state holding affiliation table.
 * @param rid Radio ID to remove.
 */
void p25_aff_deregister(dsd_state* state, uint32_t rid);

/**
 * @brief Periodic aging/cleanup of the affiliation table (call at ~1 Hz).
 *
 * @param state Decoder state holding affiliation table.
 */
void p25_aff_tick(dsd_state* state);

/* ============================================================================
 * Group Affiliation (RID ↔ TG) helpers
 * ============================================================================ */

/**
 * @brief Add a group affiliation (RID to TG).
 *
 * @param state Decoder state holding group affiliation table.
 * @param rid Radio ID.
 * @param tg Talkgroup to associate.
 */
void p25_ga_add(dsd_state* state, uint32_t rid, uint16_t tg);
/**
 * @brief Remove a group affiliation (RID to TG).
 *
 * @param state Decoder state holding group affiliation table.
 * @param rid Radio ID.
 * @param tg Talkgroup to remove.
 */
void p25_ga_remove(dsd_state* state, uint32_t rid, uint16_t tg);
/**
 * @brief Age/cleanup group affiliation entries (call at ~1 Hz).
 *
 * @param state Decoder state holding group affiliation table.
 */
void p25_ga_tick(dsd_state* state);

/**
 * @brief Emit a single encryption lockout event for a talkgroup.
 *
 * Records transient encrypted-call state and pushes the corresponding event to history/log exactly once per TG until
 * scrubbed.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param slot 0 for FDMA/left, 1 for TDMA/right.
 * @param tg Talkgroup.
 * @param svc_bits Optional service bits (pass 0 if unknown).
 */
void p25_emit_enc_lockout_once(dsd_opts* opts, dsd_state* state, uint8_t slot, int tg, int svc_bits);

/**
 * @brief Record transient encrypted-call state for a talkgroup without emitting a user-visible event.
 *
 * Used by lockout paths that already own event-history reporting. The record expires with the effective P25 retune
 * backoff window and is never written to persistent talkgroup policy.
 *
 * @param opts Decoder options.
 * @param state Decoder state.
 * @param tg Talkgroup proven encrypted.
 */
void p25_sm_note_encrypted_call(dsd_opts* opts, dsd_state* state, int tg);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_PROTOCOL_P25_P25_TRUNK_SM_H_H */
