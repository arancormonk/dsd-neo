// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * The P25 producers of the verified talkgroup proof (issue #903, dsd_call_state_note_verified_target()). Each proof
 * comes from the voice channel's own checked message for the transmission, never from a grant or the trunk state
 * machine's verdicts:
 *
 * - Phase 1: an LDU1 group voice-user link control (LCO $00, Motorola LCGRGR) recovered by a hard RS decode, not a
 *   soft-erasure retry (TIA-102.BAAA-A §5.9), trunked or conventional.
 * - Phase 2: a MAC_PTT that passed its CRC naming the group on the slot; or a CRC-passed group voice-user message
 *   once traffic of the same transmission is decoded on the slot. A voice user alone proves nothing: the base station
 *   sends it during call setup even when the setup fails (TIA-102.BBAC §7.3.1).
 *
 * Supergroup evidence is taken from the message itself (a regroup voice user) or from a patch entry naming the group
 * when the proof is noted, and frozen there with the proof.
 */

#ifndef DSD_NEO_SRC_PROTOCOL_P25_P25_HEARD_TG_H_
#define DSD_NEO_SRC_PROTOCOL_P25_P25_HEARD_TG_H_

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/state_fwd.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 1 when an active entry of the patch table names @p group as its supergroup, whatever its freshness. */
int p25_heard_tg_patch_names_sg(const dsd_state* state, uint32_t group);

/* Note that a checked message proved @p group for @p call, slot @p slot's ACTIVE P25 group call naming that group, with
 * @p supergroup 1 when the message itself named a supergroup. Nothing for any other call, or one already noted. */
void p25_heard_tg_note(dsd_state* state, uint8_t slot, const dsd_call_snapshot* call, uint32_t group, int supergroup);

/* Phase 2: a CRC-passed, live group voice-user message observed slot @p slot's call as @p group (@p regroup 1 for a
 * regroup voice user): kept for that call (its epoch) until its traffic proves it (p25p2_heard_tg_traffic()). */
void p25p2_heard_tg_voice_user(dsd_state* state, uint8_t slot, uint32_t group, int regroup);

/* Phase 2: a voice burst on slot @p slot proves the group a voice user of the same call named. A field check when none
 * waits. */
void p25p2_heard_tg_traffic(dsd_state* state, uint8_t slot);

/* Phase 2: a CRC-passed MAC_PTT naming @p group started or continued slot @p slot's call, which it proves. */
void p25p2_heard_tg_ptt(dsd_state* state, uint8_t slot, uint32_t group);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_PROTOCOL_P25_P25_HEARD_TG_H_ */
