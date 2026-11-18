// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The teardown of libairspyhf's USB transfers, in the patched pinned source the Windows release builds
 * (DESKTOP_LIBAIRSPYHF_TRANSFERS, tests/CMakeLists.txt). The library is linked against the stub libusb entry points
 * below: a submitted transfer waits in a queue, and whichever thread polls for events calls it back, one poller at a
 * time, as libusb does under its event lock. libairspyhf's own transfer thread polls while it streams, and its stop,
 * close and restart run on the caller's thread. A cancel returns at once and its CANCELLED callback comes later;
 * libusb owns a transfer from its submission to its callback, so freeing it or its buffer before then is undefined
 * behaviour, and submitting it again is refused as busy.
 */

#ifdef _WIN32
/* struct timeval, which WIN32_LEAN_AND_MEAN keeps out of windows.h; libusb.h then brings windows.h. */
#include <winsock2.h>
/* timeBeginPeriod() */
#include <timeapi.h>
#endif
#ifdef _MSC_VER
/* As airspyhf.c does before it: the CRT's time.h has struct timespec, so PThreads4W's pthread.h must not declare it. */
#define HAVE_STRUCT_TIMESPEC
#endif
#include <airspyhf.h>
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/timing.h>
#include <libusb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

enum { kMaxQueued = 64, kMaxQuarantined = 128 };

/* g_events_mu is libusb's event lock: a poll holds it, callbacks included, so one thread handles events at a time.
 * g_mu guards the queue and the counters. Lock order: g_events_mu, then g_mu. */
static pthread_mutex_t g_events_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

/* A transfer libusb owns: submitted, its callback not yet started. */
struct queued_transfer {
    struct libusb_transfer* transfer;
    int cancel_requested;
    long long cancel_due_ms; /* when its CANCELLED callback comes */
};

static struct queued_transfer g_queue[kMaxQueued];
static int g_queued;

/* Transfers the library freed while libusb still owned them: kept allocated, so a later callback never touches freed
 * memory, and released once the case is over. The library frees a transfer's buffer just before the transfer. */
static struct libusb_transfer* g_quarantined[kMaxQuarantined];
static int g_quarantined_count;

static long long
now_ms(void) {
    return (long long)dsd_time_monotonic_ms();
}

static void
sleep_us(long us) {
    dsd_sleep_us((uint64_t)us);
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

/* ---- the case running now (all under g_mu) ---- */

enum { kSamplesPerRun = 20, kWaitMs = 5000, kCancelDelayMs = 50, kTransfers = 16 };

/* What the library got wrong in the case running now. */
static int g_early_transfer_frees; /* transfers (and their buffers) freed while libusb owned them */
static int g_busy_submits;         /* submissions of a transfer still in flight */

static int g_holding;          /* transfers come back only when cancelled */
static int g_fail_next;        /* the next completion fails with ERROR, and every one after it is held */
static int g_refuse_submit_at; /* this submission (0-based, per case) is refused with LIBUSB_ERROR_NO_MEM; -1: none */
static int g_submits;          /* submission calls, refused ones included */
static int g_submits_at_hold;  /* submissions when the stream was last held */
static int g_completions;      /* callbacks with a status other than CANCELLED */
static int g_cancelled_callbacks;
static int g_samples; /* sample blocks the library handed to the consumer callback */
/* The next resubmission from a callback is refused with LIBUSB_ERROR_IO, and every transfer after it is held. */
static int g_refuse_next_resubmit;
static int g_refused_resubmits;
/* The next resubmission from a callback (on g_park_thread when g_park_on_thread is set) waits until this many full
 * passes of cancels have gone by, then goes in; 0: none. */
static int g_park_cancel_passes;
static int g_park_on_thread;
static pthread_t g_park_thread;
static int g_parked; /* a resubmission is waiting */
static int g_cancels_while_parked;
static int g_late_resubmits; /* resubmissions that went in after those cancels */
static int g_park_done;      /* the parked resubmission has gone in */
static int g_idle_polls;     /* polls in a row with nothing to deliver and no cancellation to come */
static const char* g_case_name = "";
static pthread_t g_main_thread;

/* With nothing to deliver and no cancellation to come for this many polls (about 1 ms each), whoever polls waits on a
 * transfer libusb will never call back: the library lost count of one. The case reports it instead of hanging. */
enum { kIdlePollsBeforeAbort = 3000 };

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

/* airspyhf.c's airspyhf_usb_vid and airspyhf_usb_pid. */
enum { kAirspyhfVid = 0x03EB, kAirspyhfPid = 0x800C };

static char g_fake_context;
static char g_fake_handle;
static char g_fake_device;
static libusb_device* g_device_list[] = {(libusb_device*)(void*)&g_fake_device, NULL};

struct libusb_transfer* LIBUSB_CALL
libusb_alloc_transfer(int iso_packets) {
    (void)iso_packets;
    struct libusb_transfer* transfer = (struct libusb_transfer*)calloc(1, sizeof(struct libusb_transfer));
    assert(transfer);
    return transfer;
}

void LIBUSB_CALL
libusb_free_transfer(struct libusb_transfer* transfer) {
    if (!transfer) {
        return;
    }
    pthread_mutex_lock(&g_mu);
    if (queued_index_locked(transfer) >= 0) {
        g_early_transfer_frees++;
        assert(g_quarantined_count < kMaxQuarantined);
        g_quarantined[g_quarantined_count++] = transfer;
        transfer = NULL;
    }
    pthread_mutex_unlock(&g_mu);
    free(transfer);
}

/* A resubmission from a callback that waits until the stop has made @p passes full passes of cancels. */
static void
park_resubmit_locked(int passes) {
    g_parked = 1;
    g_cancels_while_parked = 0;
    /* A library that never makes those passes gets the resubmission after kWaitMs, so the case reports. */
    const long long deadline = now_ms() + kWaitMs;
    while (g_cancels_while_parked < passes * kTransfers && now_ms() <= deadline) {
        pthread_mutex_unlock(&g_mu);
        sleep_us(200);
        pthread_mutex_lock(&g_mu);
    }
    g_late_resubmits += g_cancels_while_parked >= passes * kTransfers;
    g_parked = 0;
    g_park_done = 1;
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
    const int resubmit = !pthread_equal(pthread_self(), g_main_thread);
    if (resubmit && g_refuse_next_resubmit) {
        g_refuse_next_resubmit = 0;
        g_refused_resubmits++;
        g_holding = 1;
        pthread_mutex_unlock(&g_mu);
        return LIBUSB_ERROR_IO;
    }
    if (resubmit && g_park_cancel_passes > 0 && (!g_park_on_thread || pthread_equal(pthread_self(), g_park_thread))) {
        /* The callback has passed its streaming check; the stop's cancels run while it waits here. */
        const int passes = g_park_cancel_passes;
        g_park_cancel_passes = 0;
        park_resubmit_locked(passes);
    }
    if (queued_index_locked(transfer) >= 0) {
        g_busy_submits++;
        pthread_mutex_unlock(&g_mu);
        return LIBUSB_ERROR_BUSY;
    }
    assert(g_queued < kMaxQueued);
    g_queue[g_queued].transfer = transfer;
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
    if (g_parked) {
        g_cancels_while_parked++;
    }
    const int i = queued_index_locked(transfer);
    if (i >= 0 && !g_queue[i].cancel_requested) {
        g_queue[i].cancel_requested = 1;
        g_queue[i].cancel_due_ms = now_ms() + kCancelDelayMs;
        r = 0;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* A poll with nothing due: counts it, and reports a library that waits on a transfer that will never come back. */
static void
idle_poll_locked(void) {
    int cancel_to_come = 0;
    for (int i = 0; i < g_queued; ++i) {
        cancel_to_come |= g_queue[i].cancel_requested;
    }
    g_idle_polls = cancel_to_come ? 0 : g_idle_polls + 1;
    if (g_idle_polls >= kIdlePollsBeforeAbort) {
        DSD_FPRINTF(stderr, "FAIL %s: still polling with %d transfer(s) in flight and no cancellation to come\n",
                    g_case_name, g_queued);
        abort();
    }
}

/* An event poll: under the event lock, it calls back one transfer that is due on the polling thread, or waits about
 * 1 ms when none is. While another thread handles events it waits about 1 ms for the lock and returns, as a libusb
 * event waiter does. */
int LIBUSB_CALL
libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int* completed) {
    (void)ctx;
    (void)tv;
    (void)completed;
    int locked = 0;
    for (int i = 0; i < 20 && !locked; ++i) {
        locked = pthread_mutex_trylock(&g_events_mu) == 0;
        if (!locked) {
            sleep_us(50);
        }
    }
    if (!locked) {
        return 0;
    }
    pthread_mutex_lock(&g_mu);
    const int pick = next_delivery_locked();
    if (pick < 0) {
        idle_poll_locked();
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
    g_idle_polls = 0;
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

/* The stop wakes an event poller; the stub's pollers wake by themselves within about 1 ms. */
void LIBUSB_CALL
libusb_interrupt_event_handler(libusb_context* ctx) {
    (void)ctx;
}

/* Receiver-mode, frequency and other OUT requests succeed; the IN reads fail, so the open falls back to its default
 * sample rates, attenuator steps and calibration. */
int LIBUSB_CALL
libusb_control_transfer(libusb_device_handle* dev_handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                        uint16_t wIndex, unsigned char* data, uint16_t wLength, unsigned int timeout) {
    (void)dev_handle;
    (void)bRequest;
    (void)wValue;
    (void)wIndex;
    (void)data;
    (void)timeout;
    if ((bmRequestType & LIBUSB_ENDPOINT_IN) == 0) {
        return (int)wLength;
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
    desc->idVendor = kAirspyhfVid;
    desc->idProduct = kAirspyhfPid;
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
libusb_set_interface_alt_setting(libusb_device_handle* dev_handle, int interface_number, int alternate_setting) {
    (void)dev_handle;
    (void)interface_number;
    (void)alternate_setting;
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

/* ---- harness ---- */

static int
on_samples(airspyhf_transfer_t* transfer) {
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
                "FAIL %s: %s (in_flight=%d early_frees=%d busy_submits=%d submits=%d (at hold %d) completions=%d "
                "cancelled=%d samples=%d)\n",
                name, what, g_queued, g_early_transfer_frees, g_busy_submits, g_submits, g_submits_at_hold,
                g_completions, g_cancelled_callbacks, g_samples);
    pthread_mutex_unlock(&g_mu);
}

static int
read_counter(const int* counter) {
    pthread_mutex_lock(&g_mu);
    const int value = *counter;
    pthread_mutex_unlock(&g_mu);
    return value;
}

/* Opens the stub device with the library's DSP off, so the consumer only counts blocks, and resets the counters. */
static airspyhf_device_t*
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
    g_busy_submits = 0;
    g_refuse_next_resubmit = 0;
    g_refused_resubmits = 0;
    g_park_cancel_passes = 0;
    g_park_on_thread = 0;
    g_park_done = 0;
    g_parked = 0;
    g_cancels_while_parked = 0;
    g_late_resubmits = 0;
    g_idle_polls = 0;
    g_case_name = name;
    pthread_mutex_unlock(&g_mu);

    airspyhf_device_t* dev = NULL;
    const int rc = airspyhf_open(&dev);
    if (rc != AIRSPYHF_SUCCESS || dev == NULL) {
        DSD_FPRINTF(stderr, "%s: airspyhf_open failed (%d)\n", name, rc);
        abort();
    }
    (void)airspyhf_set_lib_dsp(dev, 0);
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
wait_for_stream_end(airspyhf_device_t* dev) {
    const long long deadline = now_ms() + kWaitMs;
    while (airspyhf_is_streaming(dev)) {
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
stream_run(const char* name, airspyhf_device_t* dev, const char* what) {
    const int base = read_counter(&g_samples);
    const int rc = airspyhf_start(dev, on_samples, NULL);
    expect(name, rc == AIRSPYHF_SUCCESS, what);
    if (rc == AIRSPYHF_SUCCESS) {
        expect(name, wait_for_samples(base + kSamplesPerRun), "the stream delivered samples");
    }
}

/* Closes the device and checks what every case needs: nothing freed while libusb owned it, nothing left in flight and
 * no busy resubmission. Then drops whatever the case left, so the next one starts clean. */
static void
close_case(const char* name, airspyhf_device_t* dev) {
    (void)airspyhf_close(dev);
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight after close");
    expect(name, read_counter(&g_early_transfer_frees) == 0, "no transfer or buffer freed before its callback");
    expect(name, read_counter(&g_busy_submits) == 0, "no transfer submitted while still in flight");

    pthread_mutex_lock(&g_mu);
    g_queued = 0;
    for (int i = 0; i < g_quarantined_count; ++i) {
        free(g_quarantined[i]);
    }
    g_quarantined_count = 0;
    pthread_mutex_unlock(&g_mu);
}

/* A stop while streaming: every transfer is in flight, and each comes back only as cancelled, 50 ms after its cancel.
 * The stop must return only after all of them have, with nothing resubmitted. */
static void
stop_waits_for_deferred_cancel(void) {
    const char* name = "stop_waits_for_deferred_cancel";
    airspyhf_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    hold_stream();
    const int rc = airspyhf_stop(dev);
    expect(name, rc == AIRSPYHF_SUCCESS, "stop");
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight when the stop returned");
    expect(name, read_counter(&g_cancelled_callbacks) == kTransfers,
           "every transfer called back as cancelled before the stop returned");
    expect(name, read_counter(&g_submits) == read_counter(&g_submits_at_hold), "no submission after the stop");
    expect(name, !airspyhf_is_streaming(dev), "not streaming after the stop");
    close_case(name, dev);
}

/* Close after a stop, and close that stops by itself, both with cancellations still to come: neither may free a
 * transfer or a buffer libusb still owns. */
static void
close_after_stop_frees_nothing_outstanding(int explicit_stop) {
    const char* name = explicit_stop ? "close_after_stop_frees_nothing_outstanding (explicit stop)"
                                     : "close_after_stop_frees_nothing_outstanding (stop inside close)";
    airspyhf_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    hold_stream();
    if (explicit_stop) {
        (void)airspyhf_stop(dev);
    }
    close_case(name, dev);
    expect(name, read_counter(&g_cancelled_callbacks) == kTransfers, "every transfer called back as cancelled");
}

/* A restart, as SoapyAirspyHF's sample-rate change does: stop, then start again. The start must submit only transfers
 * libusb has returned, both after a stop whose cancellations come late and after a stream that ended by itself (one
 * transfer failed, the rest still out) with no stop in between. */
static void
restart_reuses_only_returned_transfers(int after_stop) {
    const char* name = after_stop ? "restart_reuses_only_returned_transfers (after a stop)"
                                  : "restart_reuses_only_returned_transfers (after the stream ended by itself)";
    airspyhf_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    if (after_stop) {
        hold_stream();
        (void)airspyhf_stop(dev);
    } else {
        fail_next_completion();
        expect(name, wait_for_stream_end(dev), "the stream ended after the failed transfer");
    }
    release_stream();
    stream_run(name, dev, "restart");
    expect(name, read_counter(&g_busy_submits) == 0, "the restart submitted no transfer still in flight");
    hold_stream();
    (void)airspyhf_stop(dev);
    close_case(name, dev);
}

/* The third submission of a start is refused: the two in flight come back only when cancelled, 50 ms later. The stop
 * after the failed start must wait for both, and a later start must work. */
static void
partial_start_submit(void) {
    const char* name = "partial_start_submit";
    airspyhf_device_t* dev = open_case(name);
    pthread_mutex_lock(&g_mu);
    g_refuse_submit_at = 2;
    g_holding = 1;
    pthread_mutex_unlock(&g_mu);
    const int rc = airspyhf_start(dev, on_samples, NULL);
    expect(name, rc == AIRSPYHF_ERROR, "the start fails");
    expect(name, read_counter(&g_submits) == 3, "the start stopped at the refused submission");
    (void)airspyhf_stop(dev);
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight when the stop returned");
    expect(name, read_counter(&g_cancelled_callbacks) == 2, "both submitted transfers called back as cancelled");
    release_stream();
    stream_run(name, dev, "start after the failed start");
    hold_stream();
    (void)airspyhf_stop(dev);
    close_case(name, dev);
}

/* A resubmission from a callback is refused: that transfer is back for good, and the others are held. The stream ends
 * by itself, and the stop must still drain to zero, which it cannot if the refused transfer kept its slot. */
static void
failed_resubmit_returns_its_slot(void) {
    const char* name = "failed_resubmit_returns_its_slot";
    airspyhf_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    pthread_mutex_lock(&g_mu);
    g_refuse_next_resubmit = 1;
    pthread_mutex_unlock(&g_mu);
    expect(name, wait_for_stream_end(dev), "the stream ended after the refused resubmission");
    expect(name, read_counter(&g_refused_resubmits) == 1, "one resubmission refused");
    (void)airspyhf_stop(dev);
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight when the stop returned");
    expect(name, read_counter(&g_cancelled_callbacks) == kTransfers - 1,
           "the other transfers called back as cancelled before the stop returned");
    close_case(name, dev);
}

/* Polls for events from outside libairspyhf, as a synchronous control transfer on another thread does, until its
 * parked resubmission has gone in. */
static void*
other_poller_main(void* arg) {
    (void)arg;
    while (read_counter(&g_park_done) == 0) {
        (void)libusb_handle_events_timeout_completed(NULL, NULL, NULL);
    }
    return NULL;
}

/* A completion passes the callback's streaming check just before a stop, and its resubmission goes in only after the
 * stop has cancelled every transfer: on the transfer thread, after the stop's first cancels (the drain must cancel it
 * again), or on another polling thread, after the drain's first pass too (each pass must cancel again). */
static void
drain_cancels_a_late_resubmit(int on_other_thread) {
    const char* name = on_other_thread ? "drain_cancels_a_late_resubmit (on another polling thread)"
                                       : "drain_cancels_a_late_resubmit (on the transfer thread)";
    airspyhf_device_t* dev = open_case(name);
    stream_run(name, dev, "start");
    /* A value on every path, for compilers that cannot tell the join below is as conditional as the start. */
    pthread_t poller = g_main_thread;
    pthread_mutex_lock(&g_mu);
    g_park_cancel_passes = on_other_thread ? 2 : 1;
    g_park_on_thread = on_other_thread;
    if (on_other_thread) {
        /* Before the thread runs, so its first resubmission is the parked one. */
        if (pthread_create(&poller, NULL, other_poller_main, NULL) != 0) {
            DSD_FPRINTF(stderr, "%s: could not start the polling thread\n", name);
            abort();
        }
        g_park_thread = poller;
    }
    pthread_mutex_unlock(&g_mu);
    const long long deadline = now_ms() + kWaitMs;
    while (!read_counter(&g_parked) && now_ms() <= deadline) {
        sleep_us(200);
    }
    expect(name, read_counter(&g_parked), "a resubmission waits inside the callback");
    /* The callback under way holds the event lock, so hold the rest without it: they come back only when cancelled. */
    pthread_mutex_lock(&g_mu);
    g_holding = 1;
    pthread_mutex_unlock(&g_mu);
    const int rc = airspyhf_stop(dev);
    expect(name, rc == AIRSPYHF_SUCCESS, "stop");
    expect(name, read_counter(&g_late_resubmits) == 1, "the resubmission went in after the stop's cancels");
    expect(name, read_counter(&g_queued) == 0, "no transfer in flight when the stop returned");
    expect(name, read_counter(&g_cancelled_callbacks) == kTransfers,
           "every transfer, the late one too, called back as cancelled");
    if (on_other_thread) {
        (void)pthread_join(poller, NULL);
    }
    close_case(name, dev);
}

int
main(void) {
#ifdef _WIN32
    /* The harness paces in steps of about 1 ms, which Windows stretches to its 15.6 ms timer tick unless asked. */
    (void)timeBeginPeriod(1);
#endif
    g_main_thread = pthread_self();
    stop_waits_for_deferred_cancel();
    close_after_stop_frees_nothing_outstanding(1);
    close_after_stop_frees_nothing_outstanding(0);
    restart_reuses_only_returned_transfers(1);
    restart_reuses_only_returned_transfers(0);
    partial_start_submit();
    failed_resubmit_returns_its_slot();
    drain_cancels_a_late_resubmit(0);
    drain_cancels_a_late_resubmit(1);

    if (g_failures != 0) {
        DSD_FPRINTF(stderr, "%d expectation(s) failed\n", g_failures);
        return 1;
    }
    return 0;
}
