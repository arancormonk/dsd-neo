// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/events.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/synctype_ids.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/**
 * @brief Whether the call reads as encrypted over the air, including DECRYPTABLE.
 *
 * Same definition the event ring stamps on history rows, so the live view and the call
 * log agree on which transmissions were encrypted. Disagreeing would let a decrypted
 * call play clear in one surface and hide under the log's ENC filter in another.
 */
static int
call_reads_encrypted(const dsd_call_snapshot* call) {
    return call->crypto == DSD_CALL_CRYPTO_ENCRYPTED || call->crypto == DSD_CALL_CRYPTO_ENCRYPTED_PENDING
           || call->crypto == DSD_CALL_CRYPTO_DECRYPTABLE;
}

/**
 * @brief Whether the epoch carries nothing that could name a transmission.
 *
 * The decoder opens one on a frame that synced far enough to be a frame and no further,
 * which happens routinely on a control channel and on noise while hunting. Rendered, it
 * becomes a call from talkgroup 0 by nobody -- and if a stale crypto header is still on
 * the slot, an encrypted one.
 *
 * The field list mirrors dsd_call_state_snapshot_has_identity() (call_state.c),
 * route_text included: D-STAR, NXDN and YSF enrich the repeater pair, and a call whose
 * route decoded before its callsigns is a named transmission the canonical layer already
 * counts as one. That helper lives behind call_state_internal.h and cannot be called from
 * here, which is why this mirror exists -- but a mirror that drops a field reports idle
 * for calls the rest of the app is showing.
 *
 * X2-TDMA and standalone ProVoice are the deliberate exception, matching
 * dsd_call_state_protocol_voice_is_anonymous(): those modes never parse voice into a
 * talkgroup or a source at all, so an identity-less voice epoch is the whole story the
 * protocol has. Suppressed, their transmissions would play audio and log history rows
 * while every status surface said nothing was happening.
 */
static int
call_has_no_identity(const dsd_call_snapshot* call) {
    if (DSD_SYNC_IS_X2TDMA(call->protocol) || DSD_SYNC_IS_PROVOICE(call->protocol)) {
        return 0;
    }
    return call->ota_target_id == 0U && call->policy_target_id == 0U && call->target_text[0] == '\0'
           && call->ota_source_id == 0U && call->source_text[0] == '\0' && call->route_text[0][0] == '\0'
           && call->route_text[1][0] == '\0';
}

/**
 * @brief Copy the CSV-imported group name staged on the slot's active history row.
 *
 * The staged row's target_id is stamped OTA-then-policy by the event layer, so it must be
 * compared against the same preference (the caller's resolved @p tg_id), not the OTA id
 * alone -- a policy-resolved talkgroup would never match otherwise. Nonzero required: a
 * text-only target's 0 would "match" a stale staged row.
 *
 * @return Non-zero when a name was written.
 */
static int
staged_group_name(const dsd_state* state, uint8_t slot, uint64_t tg_id, char* out, size_t out_size) {
    if (state->event_history_s == NULL || tg_id == 0U) {
        return 0;
    }
    const Event_History* staged = &state->event_history_s[slot].Event_History_Items[0];
    if (staged->t_name[0] == '\0' || staged->target_id != (uint32_t)tg_id) {
        return 0;
    }
    DSD_SNPRINTF(out, out_size, "%s", staged->t_name);
    return 1;
}

int
dsd_app_call_line_state(int lookup, const dsd_call_snapshot* call, double now_m, double hold_s) {
    if (lookup <= 0 || call == NULL) {
        return DSD_APP_CALL_LINE_NONE;
    }
    if (call->phase == DSD_CALL_PHASE_ACTIVE) {
        return DSD_APP_CALL_LINE_ACTIVE;
    }
    if (call->phase != DSD_CALL_PHASE_ENDED) {
        return DSD_APP_CALL_LINE_IDLE;
    }
    return ((now_m - call->ended_m) < hold_s) ? DSD_APP_CALL_LINE_ENDED : DSD_APP_CALL_LINE_IDLE;
}

void
dsd_app_slot_call_view(const dsd_state* state, uint8_t slot, double now_m, dsd_app_slot_call* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->state = DSD_APP_CALL_LINE_NONE;
    if (state == NULL) {
        return;
    }

    dsd_call_snapshot call;
    DSD_MEMSET(&call, 0, sizeof(call));
    const int line =
        dsd_app_call_line_state(dsd_call_state_get(state, slot, &call), &call, now_m, DSD_APP_CALL_LINE_ENDED_HOLD_S);
    out->state = line;
    if (line != DSD_APP_CALL_LINE_ACTIVE && line != DSD_APP_CALL_LINE_ENDED) {
        return;
    }

    /* An epoch with nothing in it is not a transmission anyone can be shown. */
    if (call_has_no_identity(&call)) {
        out->state = DSD_APP_CALL_LINE_IDLE;
        return;
    }

    if (call.target_text[0] != '\0') {
        DSD_SNPRINTF(out->tg_text, sizeof(out->tg_text), "%s", call.target_text);
    } else {
        DSD_SNPRINTF(out->tg_text, sizeof(out->tg_text), "%llu", (unsigned long long)call.ota_target_id);
    }
    /* Same preference order the event layer uses: the OTA id when one decoded, the
       policy-resolved id otherwise. Text-only targets stay 0. */
    out->tg_id = (call.ota_target_id != 0U) ? call.ota_target_id : call.policy_target_id;

    if (call.source_text[0] != '\0') {
        DSD_SNPRINTF(out->src_text, sizeof(out->src_text), "%s", call.source_text);
    } else {
        DSD_SNPRINTF(out->src_text, sizeof(out->src_text), "%llu", (unsigned long long)call.ota_source_id);
    }

    /* The staged row outlives a call by design and must not caption the next one, which
       is what the target_id comparison inside staged_group_name() guards. */
    if (!staged_group_name(state, slot, out->tg_id, out->name, sizeof(out->name))) {
        DSD_SNPRINTF(out->name, sizeof(out->name), "%s", out->tg_text);
    }

    out->enc = call_reads_encrypted(&call) ? 1U : 0U;
    if (out->enc) {
        out->algid = call.algid;
        out->kid = call.kid;
    }

    const double ref_m = (line == DSD_APP_CALL_LINE_ENDED) ? call.ended_m : now_m;
    const double elapsed = ref_m - call.started_m;
    out->elapsed_ms = (elapsed > 0.0) ? (uint32_t)(elapsed * 1000.0) : 0U;
}

static int
app_is_p25_synctype(int synctype) {
    return DSD_SYNC_IS_P25P1(synctype) || DSD_SYNC_IS_P25P2(synctype);
}

static long int
app_canonical_active_p25_freq(const dsd_call_state_snapshot* calls) {
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        const dsd_call_snapshot* call = &calls->slots[slot];
        if (call->phase == DSD_CALL_PHASE_ACTIVE && app_is_p25_synctype(call->protocol) && call->frequency_hz > 0) {
            return (long int)call->frequency_hz;
        }
    }
    return 0;
}

static int
app_extract_channel_token(const char* channel_str, char* tok, size_t tok_len) {
    const char* p = strstr(channel_str, "Ch:");
    if (!p || tok_len == 0) {
        return 0;
    }
    p += 3;
    while (*p == ' ') {
        p++;
    }
    size_t t = 0;
    while (*p && t + 1 < tok_len) {
        char c = *p;
        int is_hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!is_hex) {
            break;
        }
        tok[t++] = c;
        p++;
    }
    tok[t] = '\0';
    return t > 0;
}

static long int
app_lookup_trunk_chan_map(const dsd_state* state, const char* tok) {
    char* endp = NULL;
    long ch_hex = strtol(tok, &endp, 16);
    if (endp && *endp == '\0' && ch_hex > 0 && ch_hex < 65535) {
        long int freq = state->trunk_chan_map[ch_hex];
        if (freq != 0) {
            return freq;
        }
    }
    long ch_dec = strtol(tok, &endp, 10);
    if (endp && *endp == '\0' && ch_dec > 0 && ch_dec < 65535) {
        return state->trunk_chan_map[ch_dec];
    }
    return 0;
}

static long int
app_recent_activity_vc_freq(const dsd_state* state) {
    dsd_recent_activity_snapshot recent;
    if (dsd_recent_activity_copy_snapshot(state, &recent) <= 0) {
        return 0;
    }
    const uint64_t now_ms = (uint64_t)(dsd_time_now_monotonic_s() * 1000.0);
    for (int i = 0; i < DSD_RECENT_ACTIVITY_COUNT; i++) {
        const dsd_recent_activity_entry* entry = &recent.entries[i];
        if (entry->updated_m_ms != 0U && now_ms >= entry->updated_m_ms
            && now_ms - entry->updated_m_ms > DSD_RECENT_ACTIVITY_TTL_MS) {
            continue;
        }
        if (entry->observation.frequency_hz > 0) {
            return (long int)entry->observation.frequency_hz;
        }
        const char* activity = entry->notice;
        if (!activity || activity[0] == '\0') {
            continue;
        }
        char channel[8] = {0};
        if (!app_extract_channel_token(activity, channel, sizeof(channel))) {
            continue;
        }
        const long int frequency = app_lookup_trunk_chan_map(state, channel);
        if (frequency != 0) {
            return frequency;
        }
    }
    return 0;
}

long int
dsd_app_vc_freq(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    dsd_call_state_snapshot calls;
    const int has_canonical = dsd_call_state_copy_snapshot(state, &calls) > 0;
    if (has_canonical) {
        const long int canonical_frequency = app_canonical_active_p25_freq(&calls);
        if (canonical_frequency != 0) {
            return canonical_frequency;
        }
    }
    if (state->trunk_vc_freq[0] != 0) {
        return state->trunk_vc_freq[0];
    }
    if (state->p25_vc_freq[0] != 0) {
        return state->p25_vc_freq[0];
    }
    return app_recent_activity_vc_freq(state);
}

long int
dsd_app_cc_freq(const dsd_state* state) {
    if (!state) {
        return 0;
    }
    return (state->trunk_cc_freq != 0) ? state->trunk_cc_freq : state->p25_cc_freq;
}
