// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The vendored libairspy's teardown of its USB transfers (android/third_party/libairspy). airspy.c is compiled from the
 * vendored source against stub libusb entry points: a submitted transfer waits in a queue, and whichever thread polls
 * for events calls it back, one poller at a time, as libusb does under its event lock. libairspy's own transfer thread
 * polls while it streams, and its stop, close, restart and packing change run on the caller's thread. A cancel returns
 * at once and its CANCELLED callback comes later; libusb owns a transfer from its submission to its callback, so
 * freeing it or its buffer before then is undefined behaviour, and submitting it again is refused as busy.
 */

/* Every system header airspy.c uses comes in here, before its heap calls are renamed below. */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <libusb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // IWYU pragma: keep
#include <sys/time.h>
#include <time.h>

enum { kMaxQueued = 64, kMaxLive = 128, kMaxQuarantined = 128 };

/* g_events_mu is libusb's event lock: a poll holds it, callbacks included, so one thread handles events at a time.
 * g_mu guards the queue, the heap accounting and the counters. Lock order: g_events_mu, then g_mu. */
static pthread_mutex_t g_events_mu = PTHREAD_MUTEX_INITIALIZER;
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

/* Heap blocks the library holds (the device, the transfers, their buffers and the sample queue). */
static void* g_live[kMaxLive];
static int g_live_count;

/* Blocks the library freed while libusb still owned them: kept allocated, so a later callback never touches freed
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
    /* A block freed twice, or one the library never allocated. */
    assert(0 && "free of an untracked block");
}

static int
queued_index_locked(const struct libusb_transfer* transfer) {
    for (int i = 0; i < g_queued; ++i) {
        if (g_queue[i].transfer == transfer) {
            return i;
        }
    }
    return -1;
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

/* What the library got wrong in the case running now. */
static int g_early_transfer_frees; /* transfers freed while libusb owned them */
static int g_early_buffer_frees;   /* buffers freed while their transfer was in flight */
static int g_busy_submits;         /* submissions of a transfer still in flight */

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
    pthread_mutex_lock(&g_mu);
    untrack_locked(p);
    if (queued_buffer_locked(p)) {
        g_early_buffer_frees++;
        quarantine_locked(p);
        p = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    free(p);
}

/* The vendored source, with its heap calls routed through the accounting above. It keeps upstream style, so only the
 * warnings it raises under the project's flags are silenced, and only for it. */
#define malloc test_malloc
#define calloc test_calloc
#define free   test_free
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#endif
// The test needs the source's private struct airspy_device and static helpers, so it compiles the file itself.
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../android/third_party/libairspy/airspy.c"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef malloc
#undef calloc
#undef free

/* ---- the case running now (all under g_mu) ---- */

enum { kSamplesPerRun = 20, kWaitMs = 5000, kCancelDelayMs = 50, kTransfers = 16 };

static int g_holding;          /* transfers come back only when cancelled */
static int g_fail_next;        /* the next completion fails with ERROR, and every one after it is held */
static int g_refuse_submit_at; /* this submission (0-based, per case) is refused with LIBUSB_ERROR_NO_MEM; -1: none */
static int g_submits;          /* submission calls, refused ones included */
static int g_submits_at_hold;  /* submissions when the stream was last held */
static int g_completions;      /* callbacks with a status other than CANCELLED */
static int g_cancelled_callbacks;
static int g_samples; /* sample blocks the library handed to the consumer callback */

/* The status of the next completion that is not a cancellation. */
static enum libusb_transfer_status
completion_status_locked(void) {
    g_completions++;
    if (g_fail_next) {
        g_fail_next = 0;
        g_holding = 1;
        return LIBUSB_TRANSFER_ERROR;
    }
    return LIBUSB_TRANSFER_COMPLETED;
}

/* The queue entry the poll delivers next: a cancellation that is due, else (unless the stream is held) the oldest
 * transfer not cancelled; -1 when neither. */
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

/* ---- libusb stubs ---- */

static char g_fake_context;
static char g_fake_handle;
static char g_fake_device;
static libusb_device* g_device_list[] = {(libusb_device*)(void*)&g_fake_device, NULL};

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
    pthread_mutex_lock(&g_mu);
    untrack_locked(transfer);
    if (queued_index_locked(transfer) >= 0) {
        g_early_transfer_frees++;
        quarantine_locked(transfer);
        transfer = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    free(transfer);
}

/* libusb refuses a transfer that is already in flight with LIBUSB_ERROR_BUSY. */
int LIBUSB_CALL
libusb_submit_transfer(struct libusb_transfer* transfer) {
    assert(transfer && transfer->buffer && transfer->callback);
    pthread_mutex_lock(&g_mu);
    const int index = g_submits++;
    if (g_refuse_submit_at == index) {
        pthread_mutex_unlock(&g_mu);
        return LIBUSB_ERROR_NO_MEM;
    }
    if (queued_index_locked(transfer) >= 0) {
        g_busy_submits++;
        pthread_mutex_unlock(&g_mu);
        return LIBUSB_ERROR_BUSY;
    }
    assert(g_queued < kMaxQueued);
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
    const int i = queued_index_locked(transfer);
    if (i >= 0 && !g_queue[i].cancel_requested) {
        g_queue[i].cancel_requested = 1;
        g_queue[i].cancel_due_ms = now_ms() + kCancelDelayMs;
        r = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* An event poll: under the event lock, it calls back one transfer that is due on the polling thread, or waits about
 * 1 ms when none is. */
int LIBUSB_CALL
libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int* completed) {
    (void)ctx;
    (void)tv;
    (void)completed;
    pthread_mutex_lock(&g_events_mu);
    pthread_mutex_lock(&g_mu);
    const int pick = next_delivery_locked();
    if (pick < 0) {
        pthread_mutex_unlock(&g_mu);
        sleep_us(1000);
        pthread_mutex_unlock(&g_events_mu);
        return 0;
    }
    struct libusb_transfer* transfer = g_queue[pick].transfer;
    const int cancelled = g_queue[pick].cancel_requested;
    for (int i = pick; i + 1 < g_queued; ++i) {
        g_queue[i] = g_queue[i + 1];
    }
    g_queued--;
    const enum libusb_transfer_status status = cancelled ? LIBUSB_TRANSFER_CANCELLED : completion_status_locked();
    if (cancelled) {
        g_cancelled_callbacks++;
    }
    pthread_mutex_unlock(&g_mu);

    /* libusb hands the transfer back here; from now on the library owns it. */
    transfer->status = status;
    transfer->actual_length = status == LIBUSB_TRANSFER_COMPLETED ? transfer->length : 0;
    transfer->callback(transfer);
    pthread_mutex_unlock(&g_events_mu);
    /* Pace the stream a little, outside the lock, so a stop does not wait behind a busy poller. */
    sleep_us(100);
    return 0;
}

/* Receiver-mode and other OUT requests succeed, as does the packing request; the IN reads fail, so the open falls back
 * to its default sample rates. */
int LIBUSB_CALL
libusb_control_transfer(libusb_device_handle* dev_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        uint16_t wIndex, unsigned char* data, uint16_t wLength, unsigned int timeout) {
    (void)dev_handle;
    (void)wValue;
    (void)wIndex;
    (void)timeout;
    if ((bmRequestType & LIBUSB_ENDPOINT_IN) == 0) {
        return (int)wLength;
    }
    if (bRequest == AIRSPY_SET_PACKING && wLength == 1) {
        data[0] = 1;
        return 1;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int LIBUSB_CALL
libusb_clear_halt(libusb_device_handle* dev_handle, unsigned char endpoint) {
    (void)dev_handle;
    (void)endpoint;
    return 0;
}

int LIBUSB_CALL
libusb_init(libusb_context** ctx) {
    *ctx = (libusb_context*)(void*)&g_fake_context;
    return 0;
}

void LIBUSB_CALL
libusb_exit(libusb_context* ctx) {
    (void)ctx;
}

ssize_t LIBUSB_CALL
libusb_get_device_list(libusb_context* ctx, libusb_device*** list) {
    (void)ctx;
    *list = g_device_list;
    return 1;
}

void LIBUSB_CALL
libusb_free_device_list(libusb_device** list, int unref_devices) {
    (void)list;
    (void)unref_devices;
}

int LIBUSB_CALL
libusb_get_device_descriptor(libusb_device* dev, struct libusb_device_descriptor* desc) {
    (void)dev;
    DSD_MEMSET(desc, 0, sizeof(*desc));
    desc->idVendor = airspy_usb_vid;
    desc->idProduct = airspy_usb_pid;
    return 0;
}

int LIBUSB_CALL
libusb_open(libusb_device* dev, libusb_device_handle** dev_handle) {
    (void)dev;
    *dev_handle = (libusb_device_handle*)(void*)&g_fake_handle;
    return 0;
}

void LIBUSB_CALL
libusb_close(libusb_device_handle* dev_handle) {
    (void)dev_handle;
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
    return 0;
}

int LIBUSB_CALL
libusb_set_configuration(libusb_device_handle* dev_handle, int configuration) {
    (void)dev_handle;
    (void)configuration;
    return 0;
}

int LIBUSB_CALL
libusb_claim_interface(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return 0;
}

int LIBUSB_CALL
libusb_release_interface(libusb_device_handle* dev_handle, int interface_number) {
    (void)dev_handle;
    (void)interface_number;
    return 0;
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

/* The IQ converters live in their own upstream files; with raw samples the consumer never runs them. */
static iqconverter_float_t g_cnv_f;
static iqconverter_int16_t g_cnv_i;

iqconverter_float_t*
iqconverter_float_create(const float* hb_kernel, int len) {
    (void)hb_kernel;
    (void)len;
    return &g_cnv_f;
}

void
iqconverter_float_free(iqconverter_float_t* cnv) {
    (void)cnv;
}

void
iqconverter_float_reset(iqconverter_float_t* cnv) {
    (void)cnv;
}

void
iqconverter_float_process(iqconverter_float_t* cnv, float* samples, int len) {
    (void)cnv;
    (void)samples;
    (void)len;
}

iqconverter_int16_t*
iqconverter_int16_create(const int16_t* hb_kernel, int len) {
    (void)hb_kernel;
    (void)len;
    return &g_cnv_i;
}

void
iqconverter_int16_free(iqconverter_int16_t* cnv) {
    (void)cnv;
}

void
iqconverter_int16_reset(iqconverter_int16_t* cnv) {
    (void)cnv;
}

void
iqconverter_int16_process(iqconverter_int16_t* cnv, int16_t* samples, int len) {
    (void)cnv;
    (void)samples;
    (void)len;
}

/* ---- harness ---- */

static int
on_samples(airspy_transfer* transfer) {
    (void)transfer;
    pthread_mutex_lock(&g_mu);
    g_samples++;
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static int g_failures;

static void
expect(const char* name, int ok, const char* what) {
    if (ok) {
        return;
    }
    g_failures++;
    pthread_mutex_lock(&g_mu);
    DSD_FPRINTF(stderr,
                "FAIL %s: %s (in_flight=%d early_frees=%d/%d busy_submits=%d submits=%d (at hold %d) completions=%d "
                "cancelled=%d samples=%d)\n",
                name, what, g_queued, g_early_transfer_frees, g_early_buffer_frees, g_busy_submits, g_submits,
                g_submits_at_hold, g_completions, g_cancelled_callbacks, g_samples);
    pthread_mutex_unlock(&g_mu);
}

static int
read_counter(const int* counter) {
    pthread_mutex_lock(&g_mu);
    const int value = *counter;
    pthread_mutex_unlock(&g_mu);
    return value;
}

/* Opens the stub device with raw samples, so the consumer only counts blocks, and resets the accounting. */
static airspy_device_t*
open_case(const char* name) {
    pthread_mutex_lock(&g_mu);
    g_queued = 0;
    g_holding = 0;
    g_fail_next = 0;
    g_refuse_submit_at = -1;
    g_submits = 0;
    g_submits_at_hold = -1;
    g_completions = 0;
    g_cancelled_callbacks = 0;
    g_samples = 0;
    g_early_transfer_frees = 0;
    g_early_buffer_frees = 0;
    g_busy_submits = 0;
    pthread_mutex_unlock(&g_mu);

    airspy_device_t* dev = NULL;
    const int rc = airspy_open(&dev);
    if (rc != AIRSPY_SUCCESS || dev == NULL) {
        DSD_FPRINTF(stderr, "%s: airspy_open failed (%d)\n", name, rc);
        abort();
    }
    (void)airspy_set_sample_type(dev, AIRSPY_SAMPLE_RAW);
    return dev;
}

/* Waits until the consumer has seen this many sample blocks; 0 after kWaitMs. */
static int
wait_for_samples(int count) {
    const long long deadline = now_ms() + kWaitMs;
    while (read_counter(&g_samples) < count) {
        if (now_ms() > deadline) {
            return 0;
        }
        sleep_us(1000);
    }
    return 1;
}

/* Waits until the stream has ended by itself; 0 after kWaitMs. */
static int
wait_for_stream_end(airspy_device_t* dev) {
    const long long deadline = now_ms() + kWaitMs;
    while (airspy_is_streaming(dev)) {
        if (now_ms() > deadline) {
            return 0;
        }
        sleep_us(1000);
    }
    return 1;
}

/* From now on transfers come back only when cancelled, kCancelDelayMs after the cancel. Taking the event lock waits
 * out a callback under way, so every transfer is in flight here. */
static void
hold_stream(void) {
    pthread_mutex_lock(&g_events_mu);
    pthread_mutex_lock(&g_mu);
    g_holding = 1;
    g_submits_at_hold = g_submits;
    pthread_mutex_unlock(&g_mu);
    pthread_mutex_unlock(&g_events_mu);
}

static void
release_stream(void) {
    pthread_mutex_lock(&g_mu);
    g_holding = 0;
    pthread_mutex_unlock(&g_mu);
}

/* The next completion fails with ERROR, which ends the stream; the transfers still out come back only when cancelled. */
static void
fail_next_completion(void) {
    pthread_mutex_lock(&g_mu);
    g_fail_next = 1;
    pthread_mutex_unlock(&g_mu);
}

/* Starts the stream and waits for one run of sample blocks; a failed start or missing blocks fail the case. */
static void
stream_run(const char* name, airspy_device_t* dev, const char* what) {
    const int base = read_counter(&g_samples);
    const int rc = airspy_start_rx(dev, on_samples, NULL);
    expect(name, rc == AIRSPY_SUCCESS, what);
    if (rc == AIRSPY_SUCCESS) {
        expect(name, wait_for_samples(base + kSamplesPerRun), "the stream delivered samples");
    }
}

/* Closes the device and checks what every case needs: nothing freed while libusb owned it, nothing left in flight, no
 * busy resubmission, and every block released. Then drops whatever the case left, so the next one starts clean. */
static void
close_case(const char* name, airspy_device_t* dev) {
    (void)airspy_close(dev);
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight after close");
    expect(name, read_counter(&g_early_transfer_frees) == 0, "no transfer freed before its callback");
    expect(name, read_counter(&g_early_buffer_frees) == 0, "no buffer freed before its transfer's callback");
    expect(name, read_counter(&g_busy_submits) == 0, "no transfer submitted while still in flight");
    expect(name, read_counter(&g_live_count) == 0, "every block released");

    pthread_mutex_lock(&g_mu);
    g_queued = 0;
    for (int i = 0; i < g_quarantined_count; ++i) {
        free(g_quarantined[i]);
    }
    g_quarantined_count = 0;
    for (int i = 0; i < g_live_count; ++i) {
        free(g_live[i]);
    }
    g_live_count = 0;
    pthread_mutex_unlock(&g_mu);
}

/* A stop while streaming: every transfer is in flight, and each comes back only as cancelled, 50 ms after its cancel.
 * The stop must return only after all of them have, with nothing resubmitted. */
static void
stop_waits_for_deferred_cancel(void) {
    const char* name = "stop_waits_for_deferred_cancel";
    airspy_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    hold_stream();
    const int rc = airspy_stop_rx(dev);
    expect(name, rc == AIRSPY_SUCCESS, "stop");
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight when the stop returned");
    expect(name, read_counter(&g_cancelled_callbacks) == kTransfers,
           "every transfer called back as cancelled before the stop returned");
    expect(name, read_counter(&g_submits) == read_counter(&g_submits_at_hold), "no submission after the stop");
    expect(name, !airspy_is_streaming(dev), "not streaming after the stop");
    close_case(name, dev);
}

/* Close after a stop, and close that stops by itself, both with cancellations still to come: neither may free a
 * transfer or a buffer libusb still owns. */
static void
close_after_stop_frees_nothing_outstanding(int explicit_stop) {
    const char* name = explicit_stop ? "close_after_stop_frees_nothing_outstanding (explicit stop)"
                                     : "close_after_stop_frees_nothing_outstanding (stop inside close)";
    airspy_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    hold_stream();
    if (explicit_stop) {
        (void)airspy_stop_rx(dev);
    }
    close_case(name, dev);
    expect(name, read_counter(&g_cancelled_callbacks) == kTransfers, "every transfer called back as cancelled");
}

/* A restart, as a retune does: stop, then start again. The start must submit only transfers libusb has returned, both
 * after a stop whose cancellations come late and after a stream that ended by itself (one transfer failed, the rest
 * still out) with no stop in between. */
static void
restart_reuses_only_returned_transfers(int after_stop) {
    const char* name = after_stop ? "restart_reuses_only_returned_transfers (after a stop)"
                                  : "restart_reuses_only_returned_transfers (after the stream ended by itself)";
    airspy_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    if (after_stop) {
        hold_stream();
        (void)airspy_stop_rx(dev);
    } else {
        fail_next_completion();
        expect(name, wait_for_stream_end(dev), "the stream ended after the failed transfer");
    }
    release_stream();
    stream_run(name, dev, "restart");
    expect(name, read_counter(&g_busy_submits) == 0, "the restart submitted no transfer still in flight");
    hold_stream();
    (void)airspy_stop_rx(dev);
    close_case(name, dev);
}

/* The third submission of a start is refused: the two in flight come back only when cancelled, 50 ms later. The stop
 * that airspy_source makes after a failed start must wait for both, and a later start must work. */
static void
partial_start_submit(void) {
    const char* name = "partial_start_submit";
    airspy_device_t* dev = open_case(name);
    pthread_mutex_lock(&g_mu);
    g_refuse_submit_at = 2;
    g_holding = 1;
    pthread_mutex_unlock(&g_mu);
    const int rc = airspy_start_rx(dev, on_samples, NULL);
    expect(name, rc == AIRSPY_ERROR_LIBUSB, "the start fails");
    expect(name, read_counter(&g_submits) == 3, "the start stopped at the refused submission");
    (void)airspy_stop_rx(dev);
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight when the stop returned");
    expect(name, read_counter(&g_cancelled_callbacks) == 2, "both submitted transfers called back as cancelled");
    release_stream();
    stream_run(name, dev, "start after the failed start");
    hold_stream();
    (void)airspy_stop_rx(dev);
    close_case(name, dev);
}

/* A packing change reallocates the transfers. After a stream that ended by itself, with transfers still out, it must
 * wait for them before freeing any. */
static void
packing_change_waits_for_ended_stream(void) {
    const char* name = "packing_change_waits_for_ended_stream";
    airspy_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    fail_next_completion();
    expect(name, wait_for_stream_end(dev), "the stream ended after the failed transfer");
    const int rc = airspy_set_packing(dev, 1);
    expect(name, rc == AIRSPY_SUCCESS, "packing change");
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight after the packing change");
    expect(name, read_counter(&g_early_transfer_frees) == 0, "no transfer freed before its callback");
    expect(name, read_counter(&g_early_buffer_frees) == 0, "no buffer freed before its transfer's callback");
    release_stream();
    stream_run(name, dev, "start with packing");
    hold_stream();
    (void)airspy_stop_rx(dev);
    close_case(name, dev);
}

int
main(void) {
    stop_waits_for_deferred_cancel();
    close_after_stop_frees_nothing_outstanding(1);
    close_after_stop_frees_nothing_outstanding(0);
    restart_reuses_only_returned_transfers(1);
    restart_reuses_only_returned_transfers(0);
    partial_start_submit();
    packing_change_waits_for_ended_stream();

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
