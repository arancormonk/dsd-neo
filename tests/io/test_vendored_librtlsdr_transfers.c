// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The vendored librtlsdr's async reader (android/third_party/librtlsdr) accounting for its USB transfers.
 * rtlsdr_read_async() is compiled from the vendored source against stub libusb entry points: a submitted transfer
 * waits in a queue, and a second thread calls it back with the status the case picks. It does so while the reader
 * polls for events, as libusb runs callbacks inside an event poll, and in one case while the reader is still
 * submitting, as a synchronous control transfer polling on another thread does. A reader with no transfer left must
 * return -1 instead of waiting forever, and whatever ends it, it must keep handling events until every transfer it
 * submitted has been called back: libusb owns a transfer from its submission to its callback, so freeing it or its
 * buffer before then is undefined behaviour.
 */

/* Every system header librtlsdr.c uses comes in here, before its heap calls are renamed below. */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <libusb.h>
#include <pthread.h>
#include <rtl-sdr.h>
#include <signal.h> // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   // IWYU pragma: keep
#include <sys/time.h> // IWYU pragma: keep
#include <sys/types.h>
#include <time.h>
#include <unistd.h> // IWYU pragma: keep

enum { kMaxQueued = 64, kMaxLive = 128, kMaxQuarantined = 128 };

/* g_deliver_mu is held while a callback runs, by every free and by the end of an event poll, so neither a free nor the
 * reader's next step lands inside a callback; g_mu guards the queue and the counters. Lock order: g_deliver_mu, then
 * g_mu. */
static pthread_mutex_t g_deliver_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* A transfer libusb owns: submitted, its callback not yet started. */
struct queued_transfer {
    struct libusb_transfer* transfer;
    unsigned char* buffer;
    int cancel_requested;
    long long cancel_due_ms; /* when its CANCELLED callback comes */
};

static struct queued_transfer g_queue[kMaxQueued];
static int g_queued;

/* Heap blocks the reader holds (transfers, the arrays and the buffers). */
static void* g_live[kMaxLive];
static int g_live_count;

/* Blocks the reader freed while libusb still owned them: kept allocated, so the completing thread never touches freed
 * memory, and released once the case is over. */
static void* g_quarantined[kMaxQuarantined];
static int g_quarantined_count;

static long long
now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

static void
sleep_us(long us) {
    const struct timespec ts = {0, us * 1000L};
    nanosleep(&ts, NULL);
}

static void
track_locked(void* p) {
    assert(g_live_count < kMaxLive);
    g_live[g_live_count++] = p;
}

static void
untrack_locked(const void* p) {
    for (int i = 0; i < g_live_count; ++i) {
        if (g_live[i] == p) {
            g_live[i] = g_live[--g_live_count];
            return;
        }
    }
    /* A block freed twice, or one the reader never allocated. */
    assert(0 && "free of an untracked block");
}

static int
queued_transfer_locked(const struct libusb_transfer* transfer) {
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].transfer == transfer) {
            return 1;
        }
    }
    return 0;
}

static int
queued_buffer_locked(const void* buffer) {
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].buffer == buffer) {
            return 1;
        }
    }
    return 0;
}

static void
quarantine_locked(void* p) {
    assert(g_quarantined_count < kMaxQuarantined);
    g_quarantined[g_quarantined_count++] = p;
}

/* What the reader got wrong in the case running now. */
static int g_early_transfer_frees; /* transfers freed while libusb owned them */
static int g_early_buffer_frees;   /* buffers freed while their transfer was in flight */

/* External, so a source revision that never calls one of them still builds. */
// NOLINTBEGIN(misc-use-internal-linkage)
void* test_malloc(size_t size);
void* test_calloc(size_t count, size_t size);
void test_free(void* p);
// NOLINTEND(misc-use-internal-linkage)

void*
test_malloc(size_t size) {
    void* p = malloc(size);
    assert(p);
    pthread_mutex_lock(&g_mu);
    track_locked(p);
    pthread_mutex_unlock(&g_mu);
    return p;
}

void*
test_calloc(size_t count, size_t size) {
    void* p = calloc(count, size);
    assert(p);
    pthread_mutex_lock(&g_mu);
    track_locked(p);
    pthread_mutex_unlock(&g_mu);
    return p;
}

void
test_free(void* p) {
    if (!p) {
        return;
    }
    pthread_mutex_lock(&g_deliver_mu);
    pthread_mutex_lock(&g_mu);
    untrack_locked(p);
    if (queued_buffer_locked(p)) {
        g_early_buffer_frees++;
        quarantine_locked(p);
        p = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_deliver_mu);
    free(p);
}

/* The vendored source, with its heap calls routed through the accounting above and its zero-copy buffers enabled
 * (libusb_dev_mem_alloc() below hands them out or refuses, per case). It keeps upstream style, so only the warnings
 * it raises under the project's flags are silenced, and only for it. */
#define ENABLE_ZEROCOPY 1
#define malloc          test_malloc
#define calloc          test_calloc
#define free            test_free
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
#if defined(__GNUC__) && !defined(__clang__)
/* Upstream reads reply bytes after a failed transfer: rtlsdr_read_reg() and rtlsdr_demod_read_reg() their reply
 * buffer, rtlsdr_write_eeprom() the byte it read back. tools/gcc_fanalyzer.sh reports it, and so does GCC 13's
 * -Wuninitialized (-O1) or -Wmaybe-uninitialized (-O2), since this file's libusb stubs never fill the buffer. */
#pragma GCC diagnostic ignored "-Wanalyzer-use-of-uninitialized-value"
#pragma GCC diagnostic ignored "-Wuninitialized"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
// The test needs the source's private struct rtlsdr_dev and static helpers, so it compiles the file itself.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../android/third_party/librtlsdr/src/librtlsdr.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef malloc
#undef calloc
#undef free

/* ---- the case running now ---- */

struct scenario {
    const char* name;
    uint32_t buf_num;
    int fail_every;                /* every n-th completion fails, rotating STALL, ERROR, OVERFLOW; 0: none */
    int fail_after;                /* every completion after the first n fails with ERROR; -1: none */
    int refuse_resubmit_after;     /* once n completions are in, every resubmission is refused; -1: none */
    int refuse_submit_index;       /* this submission (0-based) is refused with LIBUSB_ERROR_NO_MEM; -1: none */
    int hold_completions;          /* from the start, a transfer comes back only when cancelled */
    int cancel_delay_ms;           /* a cancelled transfer's callback comes this much later */
    int events_error;              /* one event poll fails with this code, the next with LIBUSB_ERROR_OTHER; 0: none */
    int no_device_at;              /* this completion (0-based) is NO_DEVICE, and the rest are held; -1: none */
    int first_fails_during_submit; /* transfer 0 completes with ERROR before the second submission */
    int cancel_after_samples;      /* the sample callback calls rtlsdr_cancel_async() at this count; 0: never */
    int zerocopy;                  /* libusb_dev_mem_alloc() hands out buffers; otherwise it refuses */
    int cancelled_as_no_device;    /* a cancelled transfer comes back NO_DEVICE, as on a device unplugged meanwhile */
};

enum { kBufLen = 512, kEventsErrorAfterSamples = 20 };

/* With nothing delivered for this many event polls (about 1 ms each) the reader is waiting on transfers that will
 * never come back: the harness cancels it once, as a stop would, so the case reports instead of hanging; much longer
 * and the reader hangs even after a cancel. */
enum { kIdlePollsBeforeCancel = 2000, kIdlePollsBeforeAbort = 10000 };

/* Between cases the completing thread reads this stand-in, never a finished case's scenario. */
static const struct scenario g_idle = {
    .name = "idle",
    .buf_num = 1,
    .fail_after = -1,
    .refuse_resubmit_after = -1,
    .refuse_submit_index = -1,
    .no_device_at = -1,
};
/* The running case's scenario, copied in: a caller's scenario lives on its stack. */
static struct scenario g_case;
static const struct scenario* g_sc = &g_idle;
static rtlsdr_dev_t* g_dev;
static pthread_t g_completer;
static int g_active;      /* the completing thread delivers callbacks */
static int g_window_open; /* the reader is polling for events */
static int g_quit;        /* the completing thread exits */
static int g_submits;     /* submission calls, refused ones included */
static int g_refused_resubmits;
static int g_completions; /* callbacks with a status other than CANCELLED */
static int g_cancelled_callbacks;
static int g_deliveries; /* every callback */
static int g_first_failed;
static int g_samples;
static int g_events_errors_sent;
static int g_idle_polls;
static int g_last_deliveries;
static int g_forced_cancel;
/* Transfers come back only when cancelled: from the start when the case holds them, and once a stop or an event error
 * ends the stream, so that the reader's drain has cancellations to wait for. */
static int g_holding;
static int g_submits_at_cancel; /* submissions when the sample callback cancelled; -1: it did not */

/* The status of the next completion that is not a cancellation. */
static enum libusb_transfer_status
completion_status_locked(void) {
    const int k = g_completions++;
    if (g_sc->first_fails_during_submit && k == 0) {
        return LIBUSB_TRANSFER_ERROR;
    }
    if (g_sc->no_device_at == k) {
        /* Unplugged: what is still out comes back only through its cancel. */
        g_holding = 1;
        return LIBUSB_TRANSFER_NO_DEVICE;
    }
    if (g_sc->fail_every > 0 && (k + 1) % g_sc->fail_every == 0) {
        static const enum libusb_transfer_status kRotation[] = {
            LIBUSB_TRANSFER_STALL,
            LIBUSB_TRANSFER_ERROR,
            LIBUSB_TRANSFER_OVERFLOW,
        };

        return kRotation[((k + 1) / g_sc->fail_every - 1) % 3];
    }
    if (g_sc->fail_after >= 0 && k >= g_sc->fail_after) {
        return LIBUSB_TRANSFER_ERROR;
    }
    return LIBUSB_TRANSFER_COMPLETED;
}

/* The queue entry the completing thread delivers next: a cancellation that is due, else (unless the case holds them)
 * the oldest transfer not cancelled; -1 when neither. */
static int
next_delivery_locked(void) {
    const long long now = now_ms();
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].cancel_requested && g_queue[i].cancel_due_ms <= now) {
            return i;
        }
    }
    if (g_holding) {
        return -1;
    }
    for (int i = 0; i < g_queued; ++i) {
        if (!g_queue[i].cancel_requested) {
            return i;
        }
    }
    return -1;
}

static void*
completer_main(void* arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_deliver_mu);
        pthread_mutex_lock(&g_mu);
        if (g_quit) {
            pthread_mutex_unlock(&g_mu);
            pthread_mutex_unlock(&g_deliver_mu);
            return NULL;
        }
        /* Outside an event poll, only the first transfer's failure while the reader is still submitting. */
        const int may_deliver = g_active && (g_window_open || (g_sc->first_fails_during_submit && !g_first_failed));
        const int pick = may_deliver ? next_delivery_locked() : -1;
        if (pick < 0) {
            pthread_mutex_unlock(&g_mu);
            pthread_mutex_unlock(&g_deliver_mu);
            sleep_us(200);
            continue;
        }
        struct libusb_transfer* transfer = g_queue[pick].transfer;
        const int cancelled = g_queue[pick].cancel_requested;
        for (int i = pick; i + 1 < g_queued; ++i) {
            g_queue[i] = g_queue[i + 1];
        }
        g_queued--;
        const enum libusb_transfer_status status =
            cancelled ? (g_sc->cancelled_as_no_device ? LIBUSB_TRANSFER_NO_DEVICE : LIBUSB_TRANSFER_CANCELLED)
                      : completion_status_locked();
        if (cancelled) {
            g_cancelled_callbacks++;
        }
        g_deliveries++;
        pthread_mutex_unlock(&g_mu);

        /* libusb hands the transfer back here; from now on the reader owns it. */
        transfer->status = status;
        transfer->actual_length = status == LIBUSB_TRANSFER_COMPLETED ? transfer->length : 0;
        transfer->callback(transfer);

        if (g_sc->first_fails_during_submit && status == LIBUSB_TRANSFER_ERROR) {
            pthread_mutex_lock(&g_mu);
            g_first_failed = 1;
            pthread_mutex_unlock(&g_mu);
        }
        pthread_mutex_unlock(&g_deliver_mu);
    }
}

/* ---- libusb stubs ---- */

struct libusb_transfer* LIBUSB_CALL
libusb_alloc_transfer(int iso_packets) {
    (void)iso_packets;
    return (struct libusb_transfer*)test_calloc(1, sizeof(struct libusb_transfer));
}

void LIBUSB_CALL
libusb_free_transfer(struct libusb_transfer* transfer) {
    if (!transfer) {
        return;
    }
    pthread_mutex_lock(&g_deliver_mu);
    pthread_mutex_lock(&g_mu);
    untrack_locked(transfer);
    if (queued_transfer_locked(transfer)) {
        g_early_transfer_frees++;
        quarantine_locked(transfer);
        transfer = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_deliver_mu);
    free(transfer);
}

int LIBUSB_CALL
libusb_submit_transfer(struct libusb_transfer* transfer) {
    /* A filled transfer carries the buffer the reader allocated for it and the reader's callback. */
    assert(transfer && transfer->buffer && transfer->callback);
    const int resubmit = pthread_equal(pthread_self(), g_completer);
    pthread_mutex_lock(&g_mu);
    const int index = g_submits++;
    if (g_sc->refuse_submit_index == index) {
        pthread_mutex_unlock(&g_mu);
        return LIBUSB_ERROR_NO_MEM;
    }
    if (resubmit && g_sc->refuse_resubmit_after >= 0 && g_completions >= g_sc->refuse_resubmit_after) {
        g_refused_resubmits++;
        pthread_mutex_unlock(&g_mu);
        return LIBUSB_ERROR_IO;
    }
    /* Hold the second submission until the first transfer has failed on the completing thread. */
    while (g_sc->first_fails_during_submit && index == 1 && !g_first_failed) {
        pthread_mutex_unlock(&g_mu);
        sleep_us(200);
        pthread_mutex_lock(&g_mu);
    }
    assert(g_queued < kMaxQueued);
    assert(!queued_transfer_locked(transfer));
    g_queue[g_queued].transfer = transfer;
    g_queue[g_queued].buffer = transfer->buffer;
    g_queue[g_queued].cancel_requested = 0;
    g_queue[g_queued].cancel_due_ms = 0;
    g_queued++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

/* libusb's contract: a cancel returns at once and the CANCELLED callback comes later; a transfer that is not in flight
 * (never submitted, already called back or already cancelled) is LIBUSB_ERROR_NOT_FOUND, with no callback for it. */
int LIBUSB_CALL
libusb_cancel_transfer(struct libusb_transfer* transfer) {
    int r = LIBUSB_ERROR_NOT_FOUND;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].transfer == transfer && !g_queue[i].cancel_requested) {
            g_queue[i].cancel_requested = 1;
            g_queue[i].cancel_due_ms = now_ms() + g_sc->cancel_delay_ms;
            r = 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* The reader's event poll: callbacks run on the completing thread while it waits a little. Closing the poll waits for
 * a callback under way, so the reader goes on only once that callback has returned, as it would from a real poll. */
int LIBUSB_CALL
libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int* completed) {
    (void)ctx;
    (void)tv;
    (void)completed;
    int r = 0;
    pthread_mutex_lock(&g_mu);
    if (g_sc->events_error && g_samples >= kEventsErrorAfterSamples && g_events_errors_sent < 2) {
        r = g_events_errors_sent++ == 0 ? g_sc->events_error : LIBUSB_ERROR_OTHER;
        g_holding = 1;
    }
    if (g_deliveries == g_last_deliveries) {
        g_idle_polls++;
    } else {
        g_last_deliveries = g_deliveries;
        g_idle_polls = 0;
    }
    const int cancel = g_idle_polls == kIdlePollsBeforeCancel;
    const int hung = g_idle_polls >= kIdlePollsBeforeAbort;
    if (cancel) {
        g_forced_cancel = 1;
    }
    pthread_mutex_unlock(&g_mu);
    if (hung) {
        DSD_FPRINTF(stderr, "%s: the reader still waits on transfers after a cancel\n", g_sc->name);
        abort();
    }
    if (cancel) {
        (void)rtlsdr_cancel_async(g_dev);
    }
    if (r == 0) {
        pthread_mutex_lock(&g_mu);
        g_window_open = 1;
        pthread_mutex_unlock(&g_mu);
        sleep_us(1000);
        pthread_mutex_lock(&g_deliver_mu);
        pthread_mutex_lock(&g_mu);
        g_window_open = 0;
        pthread_mutex_unlock(&g_mu);
        pthread_mutex_unlock(&g_deliver_mu);
    }
    return r;
}

/* Zero-copy buffers: zeroed (so the source's usbfs mmap() check accepts them) when the case wants them, else refused,
 * which sends the source to ordinary buffers. */
unsigned char* LIBUSB_CALL
libusb_dev_mem_alloc(libusb_device_handle* dev_handle, size_t length) {
    (void)dev_handle;
    if (!g_sc->zerocopy) {
        return NULL;
    }
    return (unsigned char*)test_calloc(1, length);
}

int LIBUSB_CALL
libusb_dev_mem_free(libusb_device_handle* dev_handle, unsigned char* buffer, size_t length) {
    (void)dev_handle;
    (void)length;
    test_free(buffer);
    return 0;
}

/* Entry points the rest of the source links against; the reader under test never reaches them. */
int LIBUSB_CALL
libusb_bulk_transfer(libusb_device_handle* dev_handle, unsigned char endpoint, unsigned char* data, int length,
                     int* transferred, unsigned int timeout) {
    (void)dev_handle;
    (void)endpoint;
    (void)data;
    (void)length;
    (void)transferred;
    (void)timeout;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_control_transfer(libusb_device_handle* dev_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        uint16_t wIndex, unsigned char* data, uint16_t wLength, unsigned int timeout) {
    (void)dev_handle;
    (void)bmRequestType;
    (void)bRequest;
    (void)wValue;
    (void)wIndex;
    (void)data;
    (void)wLength;
    (void)timeout;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_claim_interface(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_release_interface(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL
libusb_close(libusb_device_handle* dev_handle) {
    (void)dev_handle;
}

libusb_device* LIBUSB_CALL
libusb_get_device(libusb_device_handle* dev_handle) {
    (void)dev_handle;
    return NULL;
}

int LIBUSB_CALL
libusb_get_device_descriptor(libusb_device* dev, struct libusb_device_descriptor* desc) {
    (void)dev;
    (void)desc;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

ssize_t LIBUSB_CALL
libusb_get_device_list(libusb_context* ctx, libusb_device*** list) {
    (void)ctx;
    (void)list;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL
libusb_free_device_list(libusb_device** list, int unref_devices) {
    (void)list;
    (void)unref_devices;
}

int LIBUSB_CALL
libusb_get_string_descriptor_ascii(libusb_device_handle* dev_handle, uint8_t desc_index, unsigned char* data,
                                   int length) {
    (void)dev_handle;
    (void)desc_index;
    (void)data;
    (void)length;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_init(libusb_context** ctx) {
    (void)ctx;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void LIBUSB_CALL
libusb_exit(libusb_context* ctx) {
    (void)ctx;
}

int LIBUSB_CALL
libusb_open(libusb_device* dev, libusb_device_handle** dev_handle) {
    (void)dev;
    (void)dev_handle;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_reset_device(libusb_device_handle* dev_handle) {
    (void)dev_handle;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_kernel_driver_active(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return 0;
}

int LIBUSB_CALL
libusb_detach_kernel_driver(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_attach_kernel_driver(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_wrap_sys_device(libusb_context* ctx, intptr_t sys_dev, libusb_device_handle** dev_handle) {
    (void)ctx;
    (void)sys_dev;
    (void)dev_handle;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALLV
libusb_set_option(libusb_context* ctx, enum libusb_option option, ...) {
    (void)ctx;
    (void)option;
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

/* ---- harness ---- */

static void
on_samples(unsigned char* buf, uint32_t len, void* ctx) {
    (void)buf;
    (void)len;
    (void)ctx;
    pthread_mutex_lock(&g_mu);
    const int samples = ++g_samples;
    const int cancel = g_sc->cancel_after_samples > 0 && samples == g_sc->cancel_after_samples;
    if (cancel) {
        g_holding = 1;
        g_submits_at_cancel = g_submits;
    }
    pthread_mutex_unlock(&g_mu);
    if (cancel) {
        (void)rtlsdr_cancel_async(g_dev);
    }
}

struct outcome {
    int rc;
    int dev_lost;
    int status;
    int arrays_freed;   /* dev->xfer and dev->xfer_buf are NULL again */
    int live;           /* blocks the reader still holds */
    int left_in_flight; /* transfers still submitted when the reader returned */
    int early_transfer_frees;
    int early_buffer_frees;
    int forced_cancel;
    int submits;
    int submits_at_cancel;
    int refused_resubmits;
    int completions;
    int cancelled_callbacks;
    int samples;
    int use_zerocopy;
};

static struct outcome
run_case(const struct scenario* sc) {
    static rtlsdr_dev_t dev;
    static char fake_ctx;
    static char fake_handle;
    DSD_MEMSET(&dev, 0, sizeof(dev));
    dev.ctx = (libusb_context*)(void*)&fake_ctx;
    dev.devh = (libusb_device_handle*)(void*)&fake_handle;
    dev.async_status = RTLSDR_INACTIVE;

    pthread_mutex_lock(&g_deliver_mu);
    pthread_mutex_lock(&g_mu);
    g_case = *sc;
    g_sc = &g_case;
    g_dev = &dev;
    g_queued = 0;
    g_submits = 0;
    g_refused_resubmits = 0;
    g_completions = 0;
    g_cancelled_callbacks = 0;
    g_deliveries = 0;
    g_first_failed = 0;
    g_samples = 0;
    g_events_errors_sent = 0;
    g_submits_at_cancel = -1;
    g_idle_polls = 0;
    g_last_deliveries = 0;
    g_forced_cancel = 0;
    g_holding = sc->hold_completions;
    g_window_open = 0;
    g_early_transfer_frees = 0;
    g_early_buffer_frees = 0;
    g_active = 1;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_deliver_mu);

    struct outcome out;
    DSD_MEMSET(&out, 0, sizeof(out));
    out.rc = rtlsdr_read_async(&dev, on_samples, NULL, sc->buf_num, kBufLen);

    /* Stop delivering; whatever is still queued was in flight when the reader returned, and goes without a callback. */
    pthread_mutex_lock(&g_deliver_mu);
    pthread_mutex_lock(&g_mu);
    g_active = 0;
    g_sc = &g_idle;
    out.left_in_flight = g_queued;
    g_queued = 0;
    out.dev_lost = dev.dev_lost;
    out.status = (int)dev.async_status;
    out.arrays_freed = dev.xfer == NULL && dev.xfer_buf == NULL;
    out.live = g_live_count;
    out.early_transfer_frees = g_early_transfer_frees;
    out.early_buffer_frees = g_early_buffer_frees;
    out.forced_cancel = g_forced_cancel;
    out.submits = g_submits;
    out.submits_at_cancel = g_submits_at_cancel;
    out.refused_resubmits = g_refused_resubmits;
    out.completions = g_completions;
    out.cancelled_callbacks = g_cancelled_callbacks;
    out.samples = g_samples;
    out.use_zerocopy = dev.use_zerocopy;
    for (int i = 0; i < g_quarantined_count; ++i) {
        free(g_quarantined[i]);
    }
    g_quarantined_count = 0;
    /* A reader that returned holding blocks leaks them; drop them here so the next case starts clean. */
    for (int i = 0; i < g_live_count; ++i) {
        free(g_live[i]);
    }
    g_live_count = 0;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_deliver_mu);
    return out;
}

static int g_failures;

static void
expect(const char* name, int ok, const char* what, const struct outcome* out) {
    if (ok) {
        return;
    }
    g_failures++;
    DSD_FPRINTF(stderr,
                "FAIL %s: %s (rc=%d dev_lost=%d status=%d live=%d left_in_flight=%d early_frees=%d/%d forced_cancel=%d "
                "submits=%d (at stop %d) refused_resubmits=%d completions=%d cancelled=%d samples=%d zerocopy=%d)\n",
                name, what, out->rc, out->dev_lost, out->status, out->live, out->left_in_flight,
                out->early_transfer_frees, out->early_buffer_frees, out->forced_cancel, out->submits,
                out->submits_at_cancel, out->refused_resubmits, out->completions, out->cancelled_callbacks,
                out->samples, out->use_zerocopy);
}

/* What every case needs: the reader went back to inactive and released everything, after libusb had returned every
 * transfer, and it ended by itself rather than by the harness's cancel. */
static void
expect_clean_exit(const char* name, const struct outcome* out) {
    expect(name, !out->forced_cancel, "the reader returned by itself", out);
    expect(name, out->left_in_flight == 0, "no transfer in flight when the reader returned", out);
    expect(name, out->early_transfer_frees == 0, "no transfer freed before its callback", out);
    expect(name, out->early_buffer_frees == 0, "no buffer freed before its transfer's callback", out);
    expect(name, out->live == 0, "every block released", out);
    expect(name, out->arrays_freed, "transfer and buffer arrays released", out);
    expect(name, out->status == RTLSDR_INACTIVE, "back to inactive", out);
}

static struct scenario
base_scenario(const char* name, uint32_t buf_num) {
    struct scenario sc;
    DSD_MEMSET(&sc, 0, sizeof(sc));
    sc.name = name;
    sc.buf_num = buf_num;
    sc.fail_after = -1;
    sc.refuse_resubmit_after = -1;
    sc.refuse_submit_index = -1;
    sc.no_device_at = -1;
    return sc;
}

/* One bad completion in seven (STALL, ERROR, OVERFLOW in turn), none of which the reader resubmits: all 16 transfers
 * are gone after 112 completions, and with nothing left to complete the reader reports a lost device. ERROR alone
 * never reaches the 16 consecutive errors that upstream's own check counts. */
static void
depletes_to_minus_one(void) {
    struct scenario sc = base_scenario("depletes_to_minus_one", 16);
    sc.fail_every = 7;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == -1, "rc == -1", &out);
    expect(sc.name, out.dev_lost == 1, "device lost", &out);
    expect(sc.name, out.completions == 16 * 7, "every transfer gone after 112 completions", &out);
    expect_clean_exit(sc.name, &out);
}

static void
completions_then_all_fail(void) {
    struct scenario sc = base_scenario("completions_then_all_fail", 16);
    sc.fail_after = 100;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == -1, "rc == -1", &out);
    expect(sc.name, out.dev_lost == 1, "device lost", &out);
    expect(sc.name, out.completions == 100 + 16, "each transfer failed once after 100 completions", &out);
    expect_clean_exit(sc.name, &out);
}

/* A refused resubmission takes its transfer out of the pool as surely as a failed completion. */
static void
failed_resubmit_counts(void) {
    struct scenario sc = base_scenario("failed_resubmit_counts", 16);
    sc.refuse_resubmit_after = 30;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == -1, "rc == -1", &out);
    expect(sc.name, out.dev_lost == 1, "device lost", &out);
    expect(sc.name, out.refused_resubmits == 16, "each transfer's resubmission refused once", &out);
    expect_clean_exit(sc.name, &out);
}

/* The third submission is refused: the two in flight come back only when cancelled, 50 ms later, and the reader returns
 * the submission's error once both have. */
static void
partial_initial_submit(void) {
    struct scenario sc = base_scenario("partial_initial_submit", 8);
    sc.refuse_submit_index = 2;
    sc.hold_completions = 1;
    sc.cancel_delay_ms = 50;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == LIBUSB_ERROR_NO_MEM, "rc == LIBUSB_ERROR_NO_MEM", &out);
    expect(sc.name, out.dev_lost == 0, "device not lost", &out);
    expect(sc.name, out.submits == 3, "submission stopped at the refused one", &out);
    expect(sc.name, out.cancelled_callbacks == 2, "both submitted transfers called back as cancelled", &out);
    expect_clean_exit(sc.name, &out);
}

/* The third submission is refused, and the device is unplugged meanwhile: the two in flight come back NO_DEVICE
 * during the drain, which marks the device lost, but the reader still returns the submission's error, the first. */
static void
partial_initial_submit_then_unplug(void) {
    struct scenario sc = base_scenario("partial_initial_submit_then_unplug", 8);
    sc.refuse_submit_index = 2;
    sc.hold_completions = 1;
    sc.cancel_delay_ms = 20;
    sc.cancelled_as_no_device = 1;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == LIBUSB_ERROR_NO_MEM, "rc == LIBUSB_ERROR_NO_MEM", &out);
    expect(sc.name, out.dev_lost == 1, "device lost while draining", &out);
    expect(sc.name, out.submits == 3, "submission stopped at the refused one", &out);
    expect(sc.name, out.cancelled_callbacks == 2, "both submitted transfers called back", &out);
    expect_clean_exit(sc.name, &out);
}

/* A stop while streaming: the stream stops with it, so each transfer still out comes back only as cancelled, 50 ms
 * after its cancel. */
static void
deferred_cancellation(int zerocopy) {
    struct scenario sc =
        base_scenario(zerocopy ? "deferred_cancellation (zero-copy)" : "deferred_cancellation (ordinary)", 8);
    sc.cancel_after_samples = 20;
    sc.cancel_delay_ms = 50;
    sc.zerocopy = zerocopy;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == 0, "rc == 0", &out);
    expect(sc.name, out.dev_lost == 0, "device not lost", &out);
    expect(sc.name, out.use_zerocopy == zerocopy, "buffer kind as set up", &out);
    expect(sc.name, out.cancelled_callbacks > 0, "transfers called back as cancelled", &out);
    /* The transfer whose callback stopped the stream is not resubmitted, nor is any after it. */
    expect(sc.name, out.submits == out.submits_at_cancel, "no submission after the stop", &out);
    expect_clean_exit(sc.name, &out);
}

/* An unplug while streaming: one transfer completes with NO_DEVICE, which marks the device lost, and the other seven
 * come back only as cancelled, 50 ms after their cancels. The reader must wait for them before it frees anything. */
static void
unplug_drains(void) {
    struct scenario sc = base_scenario("unplug_drains", 8);
    sc.no_device_at = 30;
    sc.cancel_delay_ms = 50;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == -1, "rc == -1", &out);
    expect(sc.name, out.dev_lost == 1, "device lost", &out);
    expect(sc.name, out.cancelled_callbacks == 7, "the seven transfers still out called back as cancelled", &out);
    expect_clean_exit(sc.name, &out);
}

/* An event poll fails while streaming; the reader drains (another poll failing on the way) and returns the first. The
 * brief's LIBUSB_ERROR_IO equals -1, a lost device's return, so a second run fails with LIBUSB_ERROR_PIPE. */
static void
events_error_keeps_first_error(int error) {
    struct scenario sc = base_scenario(
        error == LIBUSB_ERROR_IO ? "events_error_keeps_first_error (IO)" : "events_error_keeps_first_error (PIPE)", 8);
    sc.events_error = error;
    sc.cancel_delay_ms = 20;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == error, "rc == the first event error", &out);
    expect(sc.name, out.dev_lost == 0, "device not lost", &out);
    expect(sc.name, out.cancelled_callbacks > 0, "transfers called back as cancelled", &out);
    expect_clean_exit(sc.name, &out);
}

/* The first transfer fails on the other thread while the reader is still submitting the rest: three transfers are
 * still out, so the stream goes on until a stop. */
static void
error_during_initial_submission_on_other_thread(void) {
    struct scenario sc = base_scenario("error_during_initial_submission_on_other_thread", 4);
    sc.first_fails_during_submit = 1;
    sc.cancel_after_samples = 40;
    const struct outcome out = run_case(&sc);
    expect(sc.name, out.rc == 0, "rc == 0", &out);
    expect(sc.name, out.dev_lost == 0, "device not lost", &out);
    expect(sc.name, out.samples >= 40, "stream continued", &out);
    expect_clean_exit(sc.name, &out);
}

int
main(void) {
    if (pthread_create(&g_completer, NULL, completer_main, NULL) != 0) {
        DSD_FPRINTF(stderr, "could not start the completing thread\n");
        return 1;
    }

    depletes_to_minus_one();
    completions_then_all_fail();
    failed_resubmit_counts();
    partial_initial_submit();
    partial_initial_submit_then_unplug();
    deferred_cancellation(0);
    deferred_cancellation(1);
    unplug_drains();
    events_error_keeps_first_error(LIBUSB_ERROR_IO);
    events_error_keeps_first_error(LIBUSB_ERROR_PIPE);
    error_during_initial_submission_on_other_thread();

    pthread_mutex_lock(&g_mu);
    g_quit = 1;
    pthread_mutex_unlock(&g_mu);
    (void)pthread_join(g_completer, NULL);

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
