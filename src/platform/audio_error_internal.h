// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Shared last-error store for the audio backends.
 *
 * Every backend used to carry its own `static char s_last_error[512]` written by a
 * bare strncpy. That is a data race: the async output pump reports device write,
 * drain and reopen failures from its own thread while the decoder thread can call
 * dsd_audio_get_error() at any time. The message is short and the buffer never
 * moves, so it rarely misbehaves in practice, but it is undefined behavior and
 * ThreadSanitizer flags it.
 *
 * The store below serializes writes and hands readers a private copy, so the
 * pointer dsd_audio_get_error() returns can never be rewritten underneath the
 * caller. Cross-thread visibility is preserved: an error the pump reports is still
 * what the decoder thread reads.
 *
 * Exactly one backend translation unit is compiled per build (see
 * src/platform/CMakeLists.txt), so defining the store here costs no duplication.
 */

#ifndef DSD_NEO_SRC_PLATFORM_AUDIO_ERROR_INTERNAL_H
#define DSD_NEO_SRC_PLATFORM_AUDIO_ERROR_INTERNAL_H

#include <dsd-neo/core/string_utils.h>
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/threading.h>

#if defined(_MSC_VER)
#define DSD_AUDIO_ERROR_TLS __declspec(thread)
#else
#define DSD_AUDIO_ERROR_TLS _Thread_local
#endif

enum { DSD_AUDIO_ERROR_CAPACITY = 512 };

static char s_last_error[DSD_AUDIO_ERROR_CAPACITY] = "";
static dsd_mutex_t s_last_error_mu;
/* 0 = uninitialized, 1 = initialization in flight, 2 = ready. The loser of the
 * first-call race must wait: taking a mutex another thread has not finished
 * initializing is undefined behavior. */
static atomic_int s_last_error_mu_state = 0;

static void
audio_error_mu_init(void) {
    if (atomic_load(&s_last_error_mu_state) == 2) {
        return;
    }
    int expected = 0;
    if (atomic_compare_exchange_strong(&s_last_error_mu_state, &expected, 1)) {
        (void)dsd_mutex_init(&s_last_error_mu);
        atomic_store(&s_last_error_mu_state, 2);
        return;
    }
    while (atomic_load(&s_last_error_mu_state) != 2) {
        dsd_thread_yield();
    }
}

/**
 * @brief Record the message dsd_audio_get_error() will report.
 *
 * Safe from any thread. A NULL message clears the store.
 */
static void
set_error(const char* msg) {
    audio_error_mu_init();
    dsd_mutex_lock(&s_last_error_mu);
    if (msg) {
        DSD_STRNCPY(s_last_error, msg, sizeof(s_last_error) - 1);
        s_last_error[sizeof(s_last_error) - 1] = '\0';
    } else {
        s_last_error[0] = '\0';
    }
    dsd_mutex_unlock(&s_last_error_mu);
}

/**
 * @brief Take a private copy of the last error.
 *
 * The returned pointer is thread-local and stays valid until this thread calls
 * again, which is what lets a concurrent set_error() be safe.
 */
static const char*
audio_error_get(void) {
    static DSD_AUDIO_ERROR_TLS char copy[DSD_AUDIO_ERROR_CAPACITY];

    audio_error_mu_init();
    dsd_mutex_lock(&s_last_error_mu);
    DSD_STRNCPY(copy, s_last_error, sizeof(copy) - 1);
    dsd_mutex_unlock(&s_last_error_mu);
    copy[sizeof(copy) - 1] = '\0';
    return copy;
}

#endif /* DSD_NEO_SRC_PLATFORM_AUDIO_ERROR_INTERNAL_H */
