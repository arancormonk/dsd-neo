// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/platform/atomic_compat.h>
#include <dsd-neo/platform/platform.h>
#if DSD_PLATFORM_WIN_NATIVE
#include <dsd-neo/platform/sockets.h>
#endif
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/input_failure.h>
#include <errno.h>

static dsd_mutex_t failure_mutex;
static atomic_int mutex_state = 0;
static dsd_input_failure failure;

static void
lock_failure(void) {
    int expected = 0;
    if (atomic_compare_exchange_strong(&mutex_state, &expected, 1)) {
        (void)dsd_mutex_init(&failure_mutex);
        atomic_store(&mutex_state, 2);
    } else {
        while (atomic_load(&mutex_state) != 2) {
            dsd_thread_yield();
        }
    }
    dsd_mutex_lock(&failure_mutex);
}

void
dsd_input_failure_report(dsd_input_failure_kind kind, int code) {
    lock_failure();
    failure.kind = kind;
    failure.native_code = code;
    dsd_mutex_unlock(&failure_mutex);
}

void
dsd_input_failure_clear(void) {
    dsd_input_failure_report(DSD_INPUT_FAILURE_NONE, 0);
}

void
dsd_input_failure_get(dsd_input_failure* out) {
    if (!out) {
        return;
    }
    lock_failure();
    *out = failure;
    dsd_mutex_unlock(&failure_mutex);
}

dsd_input_failure_kind
dsd_input_failure_classify_socket(int code) {
#if DSD_PLATFORM_WIN_NATIVE
    if (code == WSAECONNREFUSED) {
        return DSD_INPUT_FAILURE_REFUSED;
    }
    if (code == WSAETIMEDOUT) {
        return DSD_INPUT_FAILURE_TIMEOUT;
    }
#else
    if (code == ECONNREFUSED) {
        return DSD_INPUT_FAILURE_REFUSED;
    }
    if (code == ETIMEDOUT) {
        return DSD_INPUT_FAILURE_TIMEOUT;
    }
#endif
    return DSD_INPUT_FAILURE_NETWORK;
}
