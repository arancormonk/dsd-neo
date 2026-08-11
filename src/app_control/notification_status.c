// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/call_view.h>
#include <dsd-neo/app_control/notification_status.h>
#include <dsd-neo/core/call_state.h>
#include <dsd-neo/core/dsd_time.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>
#include <stdint.h>
#include <string.h>

static dsd_app_notification_status g_status;
static int g_have = 0;
static dsd_mutex_t g_mu;
static atomic_int g_mu_state = 0; /* 0=uninit, 1=initing, 2=init */

/* Armed by the first read; see dsd_app_notification_get(). The publishers hang off the
   telemetry hook, which runs on the decode thread at protocol frame rate -- unthrottled
   at the DMR/P25p2/D-STAR/M17/EDACS sites, so tens of times a second -- and deriving the
   record is not cheap: dsd_app_vc_freq() alone copies the whole recent-activity snapshot
   under the canonical call-state lock whenever no voice frequency is cached, which is
   every publish of a conventional or not-yet-tuned session. The only reader in the tree
   is the Android service's 1 Hz poll, so a desktop run must not pay for any of it. */
static atomic_int g_wanted = 0;

/* Seconds a sync label survives a frame that found none. Matches the hold the Qt panel
   applies to the same field (kSyncHoldSeconds in src/ui/qt/metrics_model.cpp), so the
   two surfaces agree on when a session stops reading as locked. Guarded by g_mu with the
   record itself, because dsd_app_notification_reset() clears them together and can be
   called from a thread other than the publisher's. */
#define NOTIFICATION_SYNC_HOLD_S 3.0
static int g_sync_type = DSD_SYNC_NONE;
static double g_sync_seen_m = 0.0;

/* Same lazy-init shape as ui_snapshot.c's and ui_opts_snapshot.c's ensure_mu_init():
   the publisher can be reached before any explicit setup, and app-control has no start
   hook that is guaranteed to run first. The loser of the first-call race must wait --
   taking a mutex another thread has not finished initializing is undefined behavior. */
static void
ensure_mu_init(void) {
    if (atomic_load(&g_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&g_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&g_mu);
        atomic_store(&g_mu_state, 2);
        return;
    }
    while (atomic_load(&g_mu_state) != 2) {
        dsd_thread_yield();
    }
}

void
dsd_app_notification_publish_state(const dsd_state* state) {
    if (state == NULL || atomic_load(&g_wanted) == 0) {
        return;
    }

    /* Everything below reads the live state, so do it before taking the lock: the
       decode thread must never wait on a JNI poll, and a poll must never see a
       half-written record. */
    dsd_app_slot_call slots[DSD_CALL_STATE_SLOT_COUNT];
    const double now_m = dsd_time_now_monotonic_s();
    /* int, not uint8_t: DSD_CALL_STATE_SLOT_COUNT is an int-typed enum constant, and
       comparing a narrower loop variable against it is what
       bugprone-too-small-loop-variable flags -- matches the loop shape already used
       for this same constant in call_view.c's app_canonical_active_p25_freq(). */
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        dsd_app_slot_call_view(state, (uint8_t)slot, now_m, &slots[slot]);
    }
    int line_states[DSD_CALL_STATE_SLOT_COUNT];
    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        line_states[slot] = slots[slot].state;
    }
    const int8_t lead = (int8_t)dsd_app_lead_slot(line_states, (unsigned)DSD_CALL_STATE_SLOT_COUNT);

    const long int vc = dsd_app_vc_freq(state);
    const long int cc = dsd_app_cc_freq(state);

    const int synctype = state->synctype;

    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    /* Held rather than sampled, the same way the Qt panel holds it and for the same
       reason: getFrameSync() re-stamps state->synctype every engine iteration and answers
       NONE whenever the current search window found nothing, which on a control channel
       is most of them. Published raw, the label blinks on and off between polls -- and
       because that changes the encoded record, the service re-posts the notification at
       the full poll rate on a channel carrying no traffic at all. */
    if (synctype != DSD_SYNC_NONE) {
        g_sync_type = synctype;
        g_sync_seen_m = now_m;
    } else if (g_sync_type != DSD_SYNC_NONE && (now_m - g_sync_seen_m) > NOTIFICATION_SYNC_HOLD_S) {
        g_sync_type = DSD_SYNC_NONE;
    }

    /* Zeroed rather than just NUL-terminated at [0]: the whole buffer is copied into
       the published record below, and a partial DSD_SNPRINTF() only overwrites the
       label plus its NUL, leaving stack garbage past it otherwise. */
    char protocol[DSD_APP_NOTIFICATION_PROTOCOL_SIZE];
    DSD_MEMSET(protocol, 0, sizeof(protocol));
    /* Not synced is asked of the sync id, not of its rendered label: round-tripping the
       question through display text would couple this filter to the wording of the table
       in synctype.c, where renaming a label is a display-only change nobody would expect
       to reach app-control. UNKNOWN has no sentinel of its own -- it is what the table
       returns for an id it does not map -- so that half still has to be a string test. */
    const char* label = (g_sync_type != DSD_SYNC_NONE) ? dsd_synctype_to_string(g_sync_type) : NULL;
    if (label != NULL && strcmp(label, "UNKNOWN") != 0) {
        DSD_SNPRINTF(protocol, sizeof(protocol), "%s", label);
    }

    DSD_MEMCPY(g_status.slots, slots, sizeof(slots));
    DSD_MEMCPY(g_status.protocol, protocol, sizeof(protocol));
    g_status.lead_slot = lead;
    g_status.vc_freq_hz = (int64_t)vc;
    g_status.cc_freq_hz = (int64_t)cc;
    g_status.revision++;
    g_have = 1;
    dsd_mutex_unlock(&g_mu);
}

void
dsd_app_notification_publish_opts(const dsd_opts* opts) {
    if (opts == NULL || atomic_load(&g_wanted) == 0) {
        return;
    }

    /* Gated on RTL-family input for the same reason the Qt metrics are: on a WAV, UDP,
       TCP or symbol-file session the tuner readings are options the front end never
       applied, and publishing them would put a plausible frequency on a run with no
       tuner. Frontends omit the row rather than render a zero. Through the shared
       predicate so this and dsd_app_frontend_get_metrics() cannot come to disagree about
       whether a session has a tuner behind it. */
    const int radio = dsd_opts_input_is_radio(opts);

    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    g_status.radio_input = radio ? 1U : 0U;
    g_status.trunking = (opts->trunk_enable == 1) ? 1U : 0U;
    g_status.trunk_tuned = (opts->trunk_is_tuned == 1) ? 1U : 0U;
    g_status.center_freq_hz = radio ? (int64_t)opts->rtlsdr_center_freq : 0;
    g_status.revision++;
    g_have = 1;
    dsd_mutex_unlock(&g_mu);
}

int
dsd_app_notification_get(dsd_app_notification_status* out) {
    if (out == NULL) {
        return 0;
    }
    /* Arms the publishers; see g_wanted. Sticky for the life of the process rather than
       per session, so a host that has polled once keeps a live record across restarts. */
    atomic_store(&g_wanted, 1);
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    const int have = g_have;
    if (have) {
        DSD_MEMCPY(out, &g_status, sizeof(*out));
    } else {
        DSD_MEMSET(out, 0, sizeof(*out));
        /* Not left at the zero the memset wrote: 0 is a valid slot index, so a caller
           rendering straight from the struct would headline slot 0 on a session that has
           published nothing at all. -1 is the "no slot" sentinel. */
        out->lead_slot = -1;
    }
    dsd_mutex_unlock(&g_mu);
    return have;
}

void
dsd_app_notification_reset(void) {
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    DSD_MEMSET(&g_status, 0, sizeof(g_status));
    /* The sync hold goes with the record: carried across, it would let the next session's
       first frames publish the previous one's protocol label. */
    g_sync_type = DSD_SYNC_NONE;
    g_sync_seen_m = 0.0;
    g_have = 0;
    dsd_mutex_unlock(&g_mu);
}

/**
 * @brief Split a UTF-8 lead byte into its continuation count and leading bits.
 *
 * @param cp Receives the code-point bits the lead byte carries. Untouched on failure.
 * @return How many continuation bytes must follow (0 for ASCII), or -1 when @p lead
 *         cannot begin a sequence at all: 0x80..0xBF is a continuation byte with
 *         nothing leading it, and 0xF8..0xFF leads a length UTF-8 has not had since it
 *         was capped at four bytes.
 */
static int
utf8_lead_bits(unsigned char lead, uint32_t* cp) {
    if (lead < 0x80U) {
        *cp = lead;
        return 0;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        *cp = (uint32_t)(lead & 0x1FU);
        return 1;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        *cp = (uint32_t)(lead & 0x0FU);
        return 2;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        *cp = (uint32_t)(lead & 0x07U);
        return 3;
    }
    return -1;
}

/** @brief Lowest code point encodable with @p trailing continuation bytes. */
static uint32_t
utf8_min_code_point(int trailing) {
    if (trailing == 1) {
        return 0x80U;
    }
    if (trailing == 2) {
        return 0x800U;
    }
    return 0x10000U;
}

/**
 * @brief Whether @p cp is a code point UTF-8 may encode with @p trailing bytes.
 *
 * Rejects surrogates and anything past U+10FFFF, plus overlong forms -- a code point
 * that fits in fewer bytes must use them.
 */
static int
utf8_code_point_is_valid(uint32_t cp, int trailing) {
    if (cp > 0x10FFFFU) {
        return 0;
    }
    if (cp >= 0xD800U && cp <= 0xDFFFU) {
        return 0;
    }
    return cp >= utf8_min_code_point(trailing);
}

/**
 * @brief Classify the UTF-8 sequence starting at @p in.
 *
 * Stricter than the modified-UTF-8 decoder on the other side of JNI: overlong forms,
 * surrogate code points and anything past U+10FFFF are rejected here even though ART's
 * CheckJNI would let them by. An overlong is the classic way to smuggle a tab or a NUL
 * past a byte-wise filter, and rejecting more than the consumer does can only ever
 * produce input it still accepts.
 *
 * @return The sequence length in bytes (1..4) when @p in starts a complete, well-formed,
 *         minimally-encoded sequence; 0 when the byte at @p in cannot be part of one;
 *         -1 when a well-formed sequence starts but the string ends inside it.
 */
static int
utf8_sequence_len(const unsigned char* in) {
    uint32_t cp = 0U;
    const int trailing = utf8_lead_bits(in[0], &cp);
    if (trailing < 0) {
        return 0;
    }
    if (trailing == 0) {
        return 1;
    }

    for (int k = 1; k <= trailing; k++) {
        const unsigned char c = in[k];
        if (c == '\0') {
            return -1; /* The field ends inside the sequence. */
        }
        if ((c & 0xC0U) != 0x80U) {
            return 0;
        }
        cp = (cp << 6) | (uint32_t)(c & 0x3FU);
    }

    return utf8_code_point_is_valid(cp, trailing) ? trailing + 1 : 0;
}

/**
 * @brief Copy @p in into @p out, folded to control-free, well-formed UTF-8.
 *
 * Tabs separate fields and a newline would end the record, so neither may survive from
 * text the app did not author; C0 controls and DEL become spaces.
 *
 * The charset half matters just as much, and for a different consumer: this record is
 * handed to JNI's NewStringUTF(), which requires modified UTF-8 and aborts the process
 * under CheckJNI when it does not get it. Nothing upstream filters for charset --
 * D-STAR and YSF callsigns are raw decoded octets off the air, and CSV names are
 * imported unvalidated -- so every byte that is not part of a well-formed sequence
 * becomes a '?' here. A sequence the field ends part-way through is dropped rather than
 * emitted, which is also what keeps a byte-count truncation upstream from leaving a
 * split sequence behind.
 *
 * A multi-byte sequence is written whole or not at all, so the output can be shorter
 * than @p out_size - 1 even with input left over.
 */
static void
sanitize_field(char* out, size_t out_size, const char* in) {
    if (out_size == 0) {
        return;
    }

    size_t used = 0;
    size_t i = 0;
    while (in[i] != '\0' && used + 1U < out_size) {
        const int len = utf8_sequence_len((const unsigned char*)in + i);
        if (len < 0) {
            break; /* Trailing incomplete sequence: drop it. */
        }
        if (len == 0) {
            out[used++] = '?';
            i++;
            continue;
        }
        if (len == 1) {
            const unsigned char c = (unsigned char)in[i];
            out[used++] = (c < 0x20U || c == 0x7FU) ? ' ' : in[i];
            i++;
            continue;
        }
        if (used + (size_t)len + 1U > out_size) {
            break; /* Splitting it here would emit the truncation this guards against. */
        }
        for (int k = 0; k < len; k++) {
            out[used++] = in[i + (size_t)k];
        }
        i += (size_t)len;
    }
    out[used] = '\0';
}

size_t
dsd_app_notification_encode(char* out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return 0;
    }

    dsd_app_notification_status status;
    if (!dsd_app_notification_get(&status)) {
        return 0;
    }

    char protocol[DSD_APP_NOTIFICATION_PROTOCOL_SIZE];
    sanitize_field(protocol, sizeof(protocol), status.protocol);

    /* scratch is built up front and only copied into *out once it is known to hold the
       whole record: a reader must never be handed a prefix it could parse as a complete,
       shorter record. */
    char scratch[DSD_APP_NOTIFICATION_RECORD_SIZE];
    int used = DSD_SNPRINTF(scratch, sizeof(scratch), "v1\t%s\t%u\t%u\t%u\t%lld\t%lld\t%lld\t%d", protocol,
                            (unsigned)status.radio_input, (unsigned)status.trunking, (unsigned)status.trunk_tuned,
                            (long long)status.cc_freq_hz, (long long)status.vc_freq_hz,
                            (long long)status.center_freq_hz, (int)status.lead_slot);
    /* DSD_SNPRINTF forwards to vsnprintf: a negative return is an encoding error, and a
       return >= the buffer size means the formatted record was truncated. Either way,
       there is no whole record to hand back. */
    if (used < 0 || (size_t)used >= sizeof(scratch)) {
        return 0;
    }

    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        const dsd_app_slot_call* call = &status.slots[slot];
        char name[DSD_APP_CALL_NAME_SIZE];
        char tg[DSD_CALL_IDENTITY_TEXT_SIZE];
        char src[DSD_CALL_IDENTITY_TEXT_SIZE];
        sanitize_field(name, sizeof(name), call->name);
        sanitize_field(tg, sizeof(tg), call->tg_text);
        sanitize_field(src, sizeof(src), call->src_text);

        const int added =
            DSD_SNPRINTF(scratch + used, sizeof(scratch) - (size_t)used, "\t%d\t%s\t%s\t%s\t%llu\t%u\t%u\t%u\t%u",
                         call->state, name, tg, src, (unsigned long long)call->tg_id, (unsigned)call->enc,
                         (unsigned)call->algid, (unsigned)call->kid, (unsigned)call->elapsed_ms);
        if (added < 0 || (size_t)added >= sizeof(scratch) - (size_t)used) {
            return 0;
        }
        used += added;
    }

    if ((size_t)used + 1U > out_size) {
        return 0;
    }
    DSD_MEMCPY(out, scratch, (size_t)used + 1U);
    return (size_t)used;
}
