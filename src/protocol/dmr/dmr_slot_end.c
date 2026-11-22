// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/* The end of one DMR slot's call (dmr_slot_end.h): the slot's live crypto fields, their stash for a heal, and the
   resets a terminator or an addressed P_CLEAR applies to that slot alone. */

#include "dmr_slot_end.h"

#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/protocol/dmr/dmr.h>
#include <dsd-neo/protocol/dmr/dmr_trunk_sm.h>
#include <stdint.h>
#include "dmr_vcrypt.h"

dmr_slot_crypto
dmr_slot_crypto_fields(dsd_state* state, uint8_t idx) {
    if (idx == 0U) {
        return (dmr_slot_crypto){&state->dmr_fid, &state->dmr_so, &state->payload_algid, &state->payload_keyid,
                                 &state->payload_mi};
    }
    return (dmr_slot_crypto){&state->dmr_fidR, &state->dmr_soR, &state->payload_algidR, &state->payload_keyidR,
                             &state->payload_miR};
}

// The terminator reset, split by what the heal can give back. The crypto half is stashed before
// an unverified end and restored when the epoch heals, so it is safe to clear on every
// terminator. The metadata half -- the partially assembled talker alias, the dmr_pdu_sf
// superframe scratch, and the embedded/LRRP GPS -- has no stash: clearing it on a burst that was
// really mid-call voice mis-typed as a terminator would permanently lose the transmission's
// alias, so it is only cleared when the terminator's LC could actually be read.
static void
dmr_slot_reset_crypto(dsd_state* state, uint8_t idx) {
    const dmr_slot_crypto live = dmr_slot_crypto_fields(state, idx);
    *live.fid = 0;
    *live.so = 0;
    *live.algid = 0;
    *live.keyid = 0;
    *live.mi = 0;
    dmr_enc_class_reset(state, idx);
    dmr_le_vendor_reset(state, idx);
    dmr_pi_precall_reset(state, idx);
    // The call's superframe chain ends with it (issue #729).
    dmr_vcrypt_forget(state, idx);
}

static void
dmr_slot_reset_metadata(dsd_state* state, uint8_t idx) {
    state->dmr_alias_block_len[idx] = 0;
    state->dmr_alias_char_size[idx] = 0;
    state->dmr_alias_format[idx] = 0;
    DSD_SNPRINTF(state->generic_talker_alias[idx], sizeof(state->generic_talker_alias[idx]), "%s", "");
    DSD_MEMSET(state->dmr_pdu_sf[idx], 0, sizeof(state->dmr_pdu_sf[idx]));
    state->dmr_embedded_gps[idx][0] = '\0';
    state->dmr_lrrp_gps[idx][0] = '\0';
}

// The heal side of the unverified-terminator end. The terminator reset below clears the live
// dmr_fid/dmr_so/payload_algid/payload_keyid/payload_mi the vocoder decrypts and gates with;
// when the "terminator" was really a mis-typed voice burst, the identity-less media mark that
// reopens the epoch a burst later needs them back, and nothing on that path re-signals them (a
// PI header or late-entry rebuild may never come mid-transmission). So the handler stashes the
// live fields -- not the canonical snapshot's copies: the snapshot's MI can lag the superframe
// machinery's advances, and it never carried the FID the Basic Privacy gates require -- and the
// canonical layer's reacquire hook restores them when, and only when, the ended epoch itself is
// healed. The epoch tie plus the all-cleared gate mean a stale stash can never overwrite
// fresher signaling or leak into a later call.
int
dmr_slot_crypto_is_clear(const dsd_state* state, uint8_t idx, const dmr_slot_crypto* live) {
    return *live->fid == 0U && *live->so == 0U && *live->algid == 0 && *live->keyid == 0 && *live->mi == 0ULL
           && state->dmr_le_vendor[idx] == DMR_LE_VENDOR_NONE;
}

// The stash holds only what the canonical snapshot cannot supply: the FID and service options
// the Basic Privacy gates require, which the snapshot never carried, and the MI as the
// superframe machinery last advanced it, which the snapshot's copy can lag, and the slot's vendor
// late-entry mode (issue #657), which is call state the snapshot never carried. The ALGID and key id
// are not duplicated here: the heal reads back what DMR last published into the ending epoch
// (dmr_live_pub_*, which the call record no longer holds when the values were carried from a call
// whose terminator was missed), else the `previous` snapshot the canonical layer hands it.
void
dmr_slot_crypto_stash(dsd_state* state, uint8_t slot, uint64_t epoch) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    const dmr_slot_crypto live = dmr_slot_crypto_fields(state, idx);
    state->dmr_heal_valid[idx] = 0U;
    if (dmr_slot_crypto_is_clear(state, idx, &live)) {
        return;
    }
    state->dmr_heal_fid[idx] = *live.fid;
    state->dmr_heal_so[idx] = *live.so;
    state->dmr_heal_mi[idx] = *live.mi;
    state->dmr_heal_le_vendor[idx] = state->dmr_le_vendor[idx];
    // The reset clears the mark that the live ALG ID and key id are a replaced call's (dmr_crypto_inherited); the heal
    // puts the values back, so it must put the mark back with them, or the healed call would publish them as its own.
    state->dmr_heal_inherited[idx] = state->dmr_crypto_inherited[idx];
    state->dmr_heal_inherited_algid[idx] = state->dmr_crypto_inherited_algid[idx];
    state->dmr_heal_inherited_keyid[idx] = state->dmr_crypto_inherited_keyid[idx];
    state->dmr_heal_vc_ref_state[idx] = state->dmr_vc_ref_state[idx];
    state->dmr_heal_vc_ref_clock[idx] = state->dmr_vc_ref_clock[idx];
    state->dmr_heal_vc_ref_epoch[idx] = state->dmr_vc_ref_epoch[idx];
    state->dmr_heal_vc_ref_carrier[idx] = state->dmr_vc_ref_carrier[idx];
    state->dmr_heal_vc_untrusted[idx] = state->dmr_vc_untrusted[idx];
    state->dmr_heal_pi_verified[idx] = state->dmr_crypto_pi_verified[idx];
    state->dmr_heal_epoch[idx] = epoch;
    state->dmr_heal_valid[idx] = 1U;
}

void
dmr_slot_end_call(dsd_opts* opts, dsd_state* state, uint8_t slot, dsd_call_end_reason reason, int metadata_readable) {
    const uint8_t idx = slot != 0U ? 1U : 0U;
    if (dsd_call_state_end_reason_is_recoverable((uint8_t)reason)) {
        // The stash must cover every epoch the reset below can strand: one still active, and
        // equally one a recoverable end already closed but that may still reacquire. The fade
        // case is the one that bites -- sync loss ends the epoch, then the terminator that
        // explains the fade arrives with an unreadable LC. It cannot tighten a sync-loss end,
        // but its reset still clears the slot, so without a stash a resumption inside the
        // window comes back with the canonical snapshot claiming encryption and the live
        // decoder holding no ALGID, key or MI.
        dsd_call_snapshot ending;
        if (dsd_call_state_get(state, slot, &ending) > 0 && ending.epoch != 0U
            && (ending.phase == DSD_CALL_PHASE_ACTIVE || dsd_call_state_end_reason_is_recoverable(ending.end_reason))) {
            dmr_slot_crypto_stash(state, slot, ending.epoch);
        }
    }
    const int ended = dsd_call_state_end_ex(state, slot, 0.0, reason);
    // Whatever call a Capacity Plus channel status named for this slot has ended with it.
    dmr_sm_capplus_seed_clear(dmr_sm_get_ctx(), (int)slot);
    if (ended > 0) {
        dsd_event_sync_slot(opts, state, slot);
    }
    // A final end -- a verified terminator, or an unverified one corroborated by a repeat -- can
    // never be followed by a heal, so no stash may linger to be matched against a future epoch.
    // The epoch counter only grows, but dsd_call_context_restore_snapshot() swaps in another
    // trunk-scan target's epochs wholesale, so a stale stash can meet an equal epoch number.
    dsd_call_snapshot settled;
    if (dsd_call_state_get(state, slot, &settled) > 0 && settled.phase == DSD_CALL_PHASE_ENDED
        && !dsd_call_state_end_reason_is_recoverable(settled.end_reason)) {
        state->dmr_heal_valid[idx] = 0U;
    }
    dmr_slot_reset_crypto(state, idx);
    // The call ended: the slot's next call keys from no talkgroup of this one's (keyring_dmr_tg_autoload_apply()).
    keyring_dmr_tg_autoload_forget_slot(state, idx);
    if (metadata_readable) {
        dmr_slot_reset_metadata(state, idx);
    }
}
