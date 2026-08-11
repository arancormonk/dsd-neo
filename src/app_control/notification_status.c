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
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static dsd_app_notification_status g_status;
static int g_have = 0;
static dsd_mutex_t g_mu;
static atomic_int g_mu_state = 0; /* 0=uninit, 1=initing, 2=init */

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

double
dsd_app_notification_test_now_m(void) {
    return dsd_time_now_monotonic_s();
}

void
dsd_app_notification_publish_state(const dsd_state* state) {
    if (state == NULL) {
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
    const long int vc = dsd_app_vc_freq(state);
    const long int cc = dsd_app_cc_freq(state);

    /* Zeroed rather than just NUL-terminated at [0]: the whole buffer is copied into
       the published record below, and a partial DSD_SNPRINTF() only overwrites the
       label plus its NUL, leaving stack garbage past it otherwise. */
    char protocol[DSD_APP_NOTIFICATION_PROTOCOL_SIZE];
    DSD_MEMSET(protocol, 0, sizeof(protocol));
    const char* label = dsd_synctype_to_string(state->synctype);
    /* NONE means not synced and UNKNOWN means synced to something unnameable. Neither is
       worth a label, and an empty one is what distinguishes an unsynced session. */
    if (label != NULL && strcmp(label, "NONE") != 0 && strcmp(label, "UNKNOWN") != 0) {
        DSD_SNPRINTF(protocol, sizeof(protocol), "%s", label);
    }

    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    DSD_MEMCPY(g_status.slots, slots, sizeof(slots));
    DSD_MEMCPY(g_status.protocol, protocol, sizeof(protocol));
    g_status.vc_freq_hz = (int64_t)vc;
    g_status.cc_freq_hz = (int64_t)cc;
    g_status.revision++;
    g_have = 1;
    dsd_mutex_unlock(&g_mu);
}

void
dsd_app_notification_publish_opts(const dsd_opts* opts) {
    if (opts == NULL) {
        return;
    }

    /* Gated on RTL-family input for the same reason the Qt metrics are: on a WAV, UDP,
       TCP or symbol-file session the tuner readings are options the front end never
       applied, and publishing them would put a plausible frequency on a run with no
       tuner. Frontends omit the row rather than render a zero. */
    const int radio = (opts->audio_in_type == AUDIO_IN_RTL);

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
    ensure_mu_init();
    dsd_mutex_lock(&g_mu);
    const int have = g_have;
    if (have) {
        DSD_MEMCPY(out, &g_status, sizeof(*out));
    } else {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
    dsd_mutex_unlock(&g_mu);
    return have;
}

/**
 * @brief Copy @p in into @p out with control characters folded to spaces.
 *
 * Tabs separate fields and a newline would end the record, so neither may survive from
 * text the app did not author.
 */
static void
sanitize_field(char* out, size_t out_size, const char* in) {
    size_t i = 0;
    for (; in[i] != '\0' && i + 1 < out_size; i++) {
        const unsigned char c = (unsigned char)in[i];
        out[i] = (c < 0x20U || c == 0x7FU) ? ' ' : in[i];
    }
    out[i] = '\0';
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
    int used =
        DSD_SNPRINTF(scratch, sizeof(scratch), "v1\t%s\t%u\t%u\t%u\t%lld\t%lld\t%lld", protocol,
                     (unsigned)status.radio_input, (unsigned)status.trunking, (unsigned)status.trunk_tuned,
                     (long long)status.cc_freq_hz, (long long)status.vc_freq_hz, (long long)status.center_freq_hz);
    /* DSD_SNPRINTF forwards to vsnprintf: a negative return is an encoding error, and a
       return >= the buffer size means the formatted record was truncated. Either way,
       there is no whole record to hand back. */
    if (used < 0 || (size_t)used >= sizeof(scratch)) {
        return 0;
    }

    for (int slot = 0; slot < DSD_CALL_STATE_SLOT_COUNT; slot++) {
        const dsd_app_slot_call* call = &status.slots[slot];
        char name[DSD_CALL_IDENTITY_TEXT_SIZE];
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
