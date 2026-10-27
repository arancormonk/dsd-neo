// SPDX-License-Identifier: GPL-3.0-or-later
#include <assert.h>
#include <dsd-neo/platform/threading.h>
#include <dsd-neo/runtime/stream_notice_hooks.h>
#include <stddef.h>

static int g_initializations;
static int g_locks;
static int g_notices;

int
dsd_test_notice_mutex_init(dsd_mutex_t* mutex) {
    (void)mutex;
    g_initializations++;
    return -1;
}

int
dsd_test_notice_mutex_lock(dsd_mutex_t* mutex) {
    (void)mutex;
    g_locks++;
    return -1;
}

int
dsd_test_notice_mutex_unlock(dsd_mutex_t* mutex) {
    (void)mutex;
    return -1;
}

static void
notice(int kind) {
    (void)kind;
    g_notices++;
}

int
main(void) {
    dsd_runtime_stream_notice_set(notice);
    dsd_runtime_stream_notice_post(DSD_STREAM_NOTICE_USB_RECOVERED);
    dsd_runtime_stream_notice_set(NULL);
    dsd_runtime_stream_notice_post(DSD_STREAM_NOTICE_USB_RECOVERED);
    assert(g_initializations == 1);
    assert(g_locks == 0);
    assert(g_notices == 0);
    return 0;
}
