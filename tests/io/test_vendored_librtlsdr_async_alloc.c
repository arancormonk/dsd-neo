// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * The vendored librtlsdr's async reader (android/third_party/librtlsdr) when its USB transfer setup runs out of
 * memory. rtlsdr_read_async() is compiled from the vendored source against stub libusb entry points, with every
 * allocation the setup makes counted, and the k-th one failed for each k a complete setup reaches: the reader must
 * return -ENOMEM, free everything it obtained and go back to inactive, rather than submit NULL transfers.
 */

/* Every system header librtlsdr.c uses comes in here, before its heap calls are renamed below. */
#include <assert.h>
#include <dsd-neo/core/safe_api.h>
#include <errno.h>
#include <libusb.h>
#include <rtl-sdr.h>
#include <signal.h> // IWYU pragma: keep
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>   // IWYU pragma: keep
#include <sys/time.h> // IWYU pragma: keep
#include <sys/types.h>
#include <unistd.h> // IWYU pragma: keep

/* Allocation accounting: every live block the reader holds, and a countdown to the allocation that fails. */
enum { kMaxLive = 64 };

static void* g_live[kMaxLive];
static int g_live_count;
static int g_alloc_calls;
static int g_fail_at = -1; /* index of the allocation to fail; -1 fails none */

static int
take_alloc_slot(void) {
    const int index = g_alloc_calls++;
    return index != g_fail_at;
}

static void
track(void* p) {
    assert(g_live_count < kMaxLive);
    g_live[g_live_count++] = p;
}

static void
untrack(const void* p) {
    for (int i = 0; i < g_live_count; ++i) {
        if (g_live[i] == p) {
            g_live[i] = g_live[--g_live_count];
            return;
        }
    }
    /* A block freed twice, or one the reader never allocated. */
    assert(0 && "free of an untracked block");
}

/* External, so a source revision that never calls one of them still builds. */
// NOLINTBEGIN(misc-use-internal-linkage)
void* test_malloc(size_t size);
void* test_calloc(size_t count, size_t size);
void test_free(void* p);
// NOLINTEND(misc-use-internal-linkage)

void*
test_malloc(size_t size) {
    if (!take_alloc_slot()) {
        return NULL;
    }
    void* p = malloc(size);
    assert(p);
    track(p);
    return p;
}

void*
test_calloc(size_t count, size_t size) {
    if (!take_alloc_slot()) {
        return NULL;
    }
    void* p = calloc(count, size);
    assert(p);
    track(p);
    return p;
}

void
test_free(void* p) {
    if (!p) {
        return;
    }
    untrack(p);
    free(p);
}

/* The vendored source, with its heap calls routed through the accounting above. It keeps upstream style, so only the
 * warnings it raises under the project's flags are silenced, and only for it. */
#define malloc test_malloc
#define calloc test_calloc
#define free   test_free
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

/* ---- libusb stubs: transfers are counted allocations; submission is refused, so the reader cancels at once. ---- */

static int g_submit_calls;

struct libusb_transfer* LIBUSB_CALL
libusb_alloc_transfer(int iso_packets) {
    (void)iso_packets;
    if (!take_alloc_slot()) {
        return NULL;
    }
    struct libusb_transfer* transfer = (struct libusb_transfer*)calloc(1, sizeof(*transfer));
    assert(transfer);
    track(transfer);
    return transfer;
}

void LIBUSB_CALL
libusb_free_transfer(struct libusb_transfer* transfer) {
    if (!transfer) {
        return;
    }
    untrack(transfer);
    free(transfer);
}

int LIBUSB_CALL
libusb_submit_transfer(struct libusb_transfer* transfer) {
    /* A filled transfer carries the buffer the reader allocated for it. */
    assert(transfer && transfer->buffer);
    g_submit_calls++;
    return LIBUSB_ERROR_IO;
}

int LIBUSB_CALL
libusb_cancel_transfer(struct libusb_transfer* transfer) {
    (void)transfer;
    return LIBUSB_ERROR_NOT_FOUND;
}

int LIBUSB_CALL
libusb_handle_events_timeout_completed(libusb_context* ctx, struct timeval* tv, int* completed) {
    (void)ctx;
    (void)tv;
    (void)completed;
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
libusb_dev_mem_free(libusb_device_handle* dev_handle, unsigned char* buffer, size_t length) {
    (void)dev_handle;
    (void)buffer;
    (void)length;
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

enum { kBufNum = 4, kBufLen = 512 };

static void
on_samples(unsigned char* buf, uint32_t len, void* ctx) {
    (void)buf;
    (void)len;
    (void)ctx;
}

/* One reader run on a fresh device, failing allocation @p fail_at (-1: none); returns the reader's result. */
static int
run_reader(int fail_at, rtlsdr_dev_t* dev) {
    static char fake_ctx;
    static char fake_handle;
    DSD_MEMSET(dev, 0, sizeof(*dev));
    dev->ctx = (libusb_context*)(void*)&fake_ctx;
    dev->devh = (libusb_device_handle*)(void*)&fake_handle;
    dev->async_status = RTLSDR_INACTIVE;
    g_alloc_calls = 0;
    g_fail_at = fail_at;
    g_submit_calls = 0;
    const int rc = rtlsdr_read_async(dev, on_samples, NULL, kBufNum, kBufLen);
    g_fail_at = -1;
    return rc;
}

int
main(void) {
    static rtlsdr_dev_t dev;

    /* A complete setup: every allocation succeeds, the first submission is refused and the reader winds down. */
    (void)run_reader(-1, &dev);
    const int total_allocs = g_alloc_calls;
    assert(total_allocs == 2 * kBufNum + 2);
    assert(g_submit_calls == 1);
    assert(g_live_count == 0);
    assert(dev.async_status == RTLSDR_INACTIVE);
    assert(dev.xfer == NULL && dev.xfer_buf == NULL);

    /* Each allocation of that setup failing in turn. */
    for (int k = 0; k < total_allocs; ++k) {
        const int rc = run_reader(k, &dev);
        if (rc != -ENOMEM || g_live_count != 0 || g_submit_calls != 0 || dev.async_status != RTLSDR_INACTIVE) {
            DSD_FPRINTF(stderr, "allocation %d failed: rc=%d live=%d submits=%d status=%d\n", k, rc, g_live_count,
                        g_submit_calls, (int)dev.async_status);
        }
        assert(rc == -ENOMEM);
        assert(g_live_count == 0);
        assert(g_submit_calls == 0);
        assert(dev.async_status == RTLSDR_INACTIVE);
        assert(dev.xfer == NULL && dev.xfer_buf == NULL);
        /* Allocation stopped at the one that failed. */
        assert(g_alloc_calls == k + 1);
    }

    /* The device is usable again afterwards: a later run gets as far as a complete setup. */
    (void)run_reader(-1, &dev);
    assert(g_submit_calls == 1);
    assert(g_live_count == 0);
    assert(dev.async_status == RTLSDR_INACTIVE);
    return 0;
}
