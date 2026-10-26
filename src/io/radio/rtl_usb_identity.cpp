// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief RTL-SDR USB identity, and a USB reset of exactly that device (see rtl_usb_identity.h).
 *
 * The only translation unit that includes libusb.h. Cold path: open, recovery and their tests.
 */

#include "rtl_usb_identity.h"

#include <atomic>
#include <dsd-neo/core/safe_api.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(DSD_HAVE_LIBUSB) && defined(USE_RTLSDR)
#define RTL_USB_IDENTITY_REAL 1
#include <climits>
#include <dsd-neo/platform/timing.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <libusb.h>
#include <new>
#include <rtl-sdr.h>
#else
#define RTL_USB_IDENTITY_REAL 0
#endif

namespace {

#if RTL_USB_IDENTITY_REAL

/* How long a device that re-enumerated in the reset gets to come back, and how often it is looked for. */
constexpr uint64_t kRediscoverMs = 5000;
constexpr unsigned int kRediscoverPollMs = 100;
constexpr size_t kStringSize = 256;

int
map_libusb(int rc) {
    switch (rc) {
        case LIBUSB_SUCCESS: return RTL_USB_OP_OK;
        case LIBUSB_ERROR_ACCESS: return RTL_USB_OP_ACCESS;
        case LIBUSB_ERROR_NO_DEVICE:
        case LIBUSB_ERROR_NOT_FOUND: return RTL_USB_OP_GONE;
        default: return RTL_USB_OP_ERROR;
    }
}

struct real_session {
    libusb_context* ctx = nullptr;
    libusb_device** list = nullptr;
    int count = 0;
};

uint32_t
real_rtl_get_device_count(void) {
    return rtlsdr_get_device_count();
}

int
real_rtl_get_device_usb_strings(uint32_t index, char* manufacturer, char* product, char* serial) {
    return rtlsdr_get_device_usb_strings(index, manufacturer, product, serial);
}

int
real_rtl_get_usb_strings(rtlsdr_dev_t* dev, char* manufacturer, char* product, char* serial) {
    return rtlsdr_get_usb_strings(dev, manufacturer, product, serial);
}

int
real_usb_open_session(void** session, int* count) {
    auto* s = new (std::nothrow) real_session();
    if (s == nullptr) {
        return RTL_USB_OP_ERROR;
    }
    int rc = libusb_init(&s->ctx);
    if (rc != LIBUSB_SUCCESS) {
        delete s;
        return map_libusb(rc);
    }
    ssize_t n = libusb_get_device_list(s->ctx, &s->list);
    if (n < 0) {
        libusb_exit(s->ctx);
        delete s;
        return map_libusb((int)n);
    }
    s->count = n > (ssize_t)INT_MAX ? INT_MAX : (int)n;
    *session = s;
    *count = s->count;
    return RTL_USB_OP_OK;
}

void
real_usb_close_session(void* session) {
    auto* s = static_cast<real_session*>(session);
    libusb_free_device_list(s->list, 1);
    libusb_exit(s->ctx);
    delete s;
}

/* Every configuration, not only the active one: an unconfigured or reconfigured twin must still count. A descriptor
 * that cannot be read leaves the class unknown, which the caller treats as an unreadable candidate. */
int
real_usb_describe(void* session, int i, struct rtl_usb_device_info* out) {
    auto* s = static_cast<real_session*>(session);
    libusb_device* dev = s->list[i];
    struct libusb_device_descriptor dd;
    int rc = libusb_get_device_descriptor(dev, &dd);
    if (rc != LIBUSB_SUCCESS) {
        return map_libusb(rc);
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    out->vid = dd.idVendor;
    out->pid = dd.idProduct;
    out->bus = libusb_get_bus_number(dev);
    int depth = libusb_get_port_numbers(dev, out->ports, (int)sizeof(out->ports));
    if (depth < 0) {
        return map_libusb(depth);
    }
    out->port_depth = depth;
    out->vendor_specific = dd.bDeviceClass == LIBUSB_CLASS_VENDOR_SPEC;
    for (uint8_t c = 0; c < dd.bNumConfigurations && !out->vendor_specific; c++) {
        struct libusb_config_descriptor* cfg = nullptr;
        rc = libusb_get_config_descriptor(dev, c, &cfg);
        if (rc != LIBUSB_SUCCESS) {
            return map_libusb(rc);
        }
        for (uint8_t n = 0; n < cfg->bNumInterfaces && !out->vendor_specific; n++) {
            const struct libusb_interface* intf = &cfg->interface[n];
            for (int a = 0; a < intf->num_altsetting; a++) {
                if (intf->altsetting[a].bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC) {
                    out->vendor_specific = 1;
                    break;
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    return RTL_USB_OP_OK;
}

int
real_usb_open(void* session, int i, void** handle) {
    auto* s = static_cast<real_session*>(session);
    libusb_device_handle* h = nullptr;
    int rc = libusb_open(s->list[i], &h);
    if (rc != LIBUSB_SUCCESS) {
        return map_libusb(rc);
    }
    *handle = h;
    return RTL_USB_OP_OK;
}

void
real_usb_close(void* handle) {
    libusb_close(static_cast<libusb_device_handle*>(handle));
}

/* The strings as librtlsdr reads them (ASCII string descriptors, 256-byte buffers), except that a failed read is an
 * error here rather than an empty string, so a transient failure cannot hide a twin. */
int
real_usb_read_strings(void* handle, char* manufacturer, char* product, char* serial) {
    auto* h = static_cast<libusb_device_handle*>(handle);
    struct libusb_device_descriptor dd;
    int rc = libusb_get_device_descriptor(libusb_get_device(h), &dd);
    if (rc != LIBUSB_SUCCESS) {
        return map_libusb(rc);
    }
    const uint8_t indexes[3] = {dd.iManufacturer, dd.iProduct, dd.iSerialNumber};
    char* bufs[3] = {manufacturer, product, serial};
    for (int k = 0; k < 3; k++) {
        DSD_MEMSET(bufs[k], 0, kStringSize);
        if (indexes[k] == 0) {
            continue;
        }
        rc = libusb_get_string_descriptor_ascii(h, indexes[k], reinterpret_cast<unsigned char*>(bufs[k]),
                                                (int)kStringSize);
        if (rc < 0) {
            return map_libusb(rc) == RTL_USB_OP_GONE ? RTL_USB_OP_GONE : RTL_USB_OP_ERROR;
        }
        bufs[k][kStringSize - 1] = '\0';
    }
    return RTL_USB_OP_OK;
}

int
real_usb_reset(void* handle) {
    return map_libusb(libusb_reset_device(static_cast<libusb_device_handle*>(handle)));
}

uint64_t
real_now_ms(void) {
    return dsd_realtime_mono_ms();
}

void
real_sleep_ms(unsigned int ms) {
    dsd_sleep_ms(ms);
}

const rtl_usb_driver_ops k_real_ops = {
    real_rtl_get_device_count,
    real_rtl_get_device_usb_strings,
    real_rtl_get_usb_strings,
    real_usb_open_session,
    real_usb_close_session,
    real_usb_describe,
    real_usb_open,
    real_usb_close,
    real_usb_read_strings,
    real_usb_reset,
    real_now_ms,
    real_sleep_ms,
};

std::atomic<const rtl_usb_driver_ops*> g_ops{&k_real_ops};

const rtl_usb_driver_ops*
ops(void) {
    return g_ops.load(std::memory_order_acquire);
}

struct triple {
    char manufacturer[kStringSize];
    char product[kStringSize];
    char serial[kStringSize];
};

void
terminate_strings(triple* t) {
    t->manufacturer[kStringSize - 1] = '\0';
    t->product[kStringSize - 1] = '\0';
    t->serial[kStringSize - 1] = '\0';
}

bool
same_triple(const triple& t, const char* manufacturer, const char* product, const char* serial) {
    /* Bounded: an identity's strings come from the caller's struct. */
    return strncmp(t.manufacturer, manufacturer, kStringSize) == 0 && strncmp(t.product, product, kStringSize) == 0
           && strncmp(t.serial, serial, kStringSize) == 0;
}

bool
same_triple(const triple& t, const struct rtl_usb_identity* id) {
    return same_triple(t, id->manufacturer, id->product, id->serial);
}

int
read_rtl_index(const rtl_usb_driver_ops* o, uint32_t index, triple* t) {
    DSD_MEMSET(t, 0, sizeof(*t));
    int rc = o->rtl_get_device_usb_strings(index, t->manufacturer, t->product, t->serial);
    terminate_strings(t);
    return rc;
}

/* librtlsdr's indexes that carry a triple. */
struct rtl_scan {
    int matches = 0;
    int index = -1;
    int unreadable = 0;
};

rtl_scan
scan_rtl_indexes(const rtl_usb_driver_ops* o, const triple& want) {
    rtl_scan r;
    uint32_t n = o->rtl_get_device_count();
    for (uint32_t i = 0; i < n && i <= (uint32_t)INT_MAX; i++) {
        triple t;
        if (read_rtl_index(o, i, &t) != 0) {
            r.unreadable++;
            continue;
        }
        if (same_triple(t, want.manufacturer, want.product, want.serial)) {
            if (r.matches == 0) {
                r.index = (int)i;
            }
            r.matches++;
        }
    }
    return r;
}

/* The vendor-specific-class USB devices that carry a triple, whatever their VID:PID. A device whose class cannot be
 * told, or whose strings cannot be read, counts as unreadable: it might carry the triple. */
struct usb_scan {
    int ok = 0;
    int matches = 0;
    int unreadable = 0;
    struct rtl_usb_device_info match;
};

usb_scan
scan_usb(const rtl_usb_driver_ops* o, const triple& want) {
    usb_scan u;
    DSD_MEMSET(&u.match, 0, sizeof(u.match));
    void* session = nullptr;
    int count = 0;
    if (o->usb_open_session(&session, &count) != RTL_USB_OP_OK) {
        return u;
    }
    u.ok = 1;
    for (int i = 0; i < count; i++) {
        struct rtl_usb_device_info info;
        if (o->usb_describe(session, i, &info) != RTL_USB_OP_OK) {
            u.unreadable++;
            continue;
        }
        if (!info.vendor_specific) {
            continue;
        }
        void* handle = nullptr;
        if (o->usb_open(session, i, &handle) != RTL_USB_OP_OK) {
            u.unreadable++;
            continue;
        }
        triple t;
        int rc = o->usb_read_strings(handle, t.manufacturer, t.product, t.serial);
        o->usb_close(handle);
        if (rc != RTL_USB_OP_OK) {
            u.unreadable++;
            continue;
        }
        terminate_strings(&t);
        if (same_triple(t, want.manufacturer, want.product, want.serial)) {
            if (u.matches == 0) {
                u.match = info;
            }
            u.matches++;
        }
    }
    o->usb_close_session(session);
    return u;
}

bool
at_identity_port(const struct rtl_usb_device_info& info, const struct rtl_usb_identity* id) {
    if (info.vid != id->vid || info.pid != id->pid || info.bus != id->bus || info.port_depth != id->port_depth) {
        return false;
    }
    if (id->port_depth < 0 || id->port_depth > (int)sizeof(id->ports)) {
        return false;
    }
    return memcmp(info.ports, id->ports, (size_t)id->port_depth) == 0;
}

bool
is_cancelled(int (*cancelled)(void*), void* ctx) {
    return cancelled != nullptr && cancelled(ctx) != 0;
}

/* After a reset that made the device re-enumerate: wait until exactly one readable candidate carries the triple. A
 * twin ends the wait at once; an unreadable candidate (udev may not have applied permissions yet) is waited out. */
rtl_usb_id_result
rediscover(const rtl_usb_driver_ops* o, const struct rtl_usb_identity* id, int (*cancelled)(void*), void* ctx) {
    triple want;
    DSD_MEMCPY(want.manufacturer, id->manufacturer, kStringSize);
    DSD_MEMCPY(want.product, id->product, kStringSize);
    DSD_MEMCPY(want.serial, id->serial, kStringSize);
    terminate_strings(&want);
    const uint64_t deadline = o->now_ms() + kRediscoverMs;
    for (;;) {
        if (is_cancelled(cancelled, ctx)) {
            return RTL_USB_ID_NOT_FOUND;
        }
        usb_scan u = scan_usb(o, want);
        if (u.ok && u.matches > 1) {
            return RTL_USB_ID_DUPLICATE;
        }
        if (u.ok && u.matches == 1 && u.unreadable == 0) {
            return RTL_USB_ID_OK;
        }
        uint64_t now = o->now_ms();
        if (now >= deadline) {
            return (u.ok && u.unreadable > 0) ? RTL_USB_ID_UNREADABLE : RTL_USB_ID_NOT_FOUND;
        }
        uint64_t left = deadline - now;
        o->sleep_ms(left < kRediscoverPollMs ? (unsigned int)left : kRediscoverPollMs);
    }
}

#endif /* RTL_USB_IDENTITY_REAL */

} // namespace

rtl_usb_id_result
rtl_usb_identity_resolve(int index, struct rtl_usb_identity* out) {
    if (out != nullptr) {
        DSD_MEMSET(out, 0, sizeof(*out));
    }
#if RTL_USB_IDENTITY_REAL
    if (out == nullptr || index < 0) {
        return RTL_USB_ID_FAILED;
    }
    const rtl_usb_driver_ops* o = ops();
    if ((uint32_t)index >= o->rtl_get_device_count()) {
        return RTL_USB_ID_NOT_FOUND;
    }
    triple want;
    if (read_rtl_index(o, (uint32_t)index, &want) != 0) {
        return RTL_USB_ID_UNREADABLE;
    }
    if (want.serial[0] == '\0') {
        return RTL_USB_ID_NO_STRINGS;
    }
    rtl_scan r = scan_rtl_indexes(o, want);
    if (r.matches > 1) {
        return RTL_USB_ID_DUPLICATE;
    }
    usb_scan u = scan_usb(o, want);
    if (!u.ok) {
        return RTL_USB_ID_FAILED;
    }
    if (u.matches > 1) {
        return RTL_USB_ID_DUPLICATE;
    }
    if (r.unreadable > 0 || u.unreadable > 0) {
        return RTL_USB_ID_UNREADABLE;
    }
    if (r.matches == 0 || u.matches == 0) {
        return RTL_USB_ID_NOT_FOUND;
    }
    out->vid = u.match.vid;
    out->pid = u.match.pid;
    out->bus = u.match.bus;
    DSD_MEMCPY(out->ports, u.match.ports, sizeof(out->ports));
    out->port_depth = u.match.port_depth;
    DSD_MEMCPY(out->manufacturer, want.manufacturer, kStringSize);
    DSD_MEMCPY(out->product, want.product, kStringSize);
    DSD_MEMCPY(out->serial, want.serial, kStringSize);
    out->valid = 1;
    return RTL_USB_ID_OK;
#else
    (void)index;
    return RTL_USB_ID_NO_LIBUSB;
#endif
}

rtl_usb_id_result
rtl_usb_identity_verify_handle(rtlsdr_dev_t* dev, const struct rtl_usb_identity* id) {
#if RTL_USB_IDENTITY_REAL
    if (dev == nullptr || id == nullptr || !id->valid) {
        return RTL_USB_ID_FAILED;
    }
    triple t;
    DSD_MEMSET(&t, 0, sizeof(t));
    if (ops()->rtl_get_usb_strings(dev, t.manufacturer, t.product, t.serial) != 0) {
        return RTL_USB_ID_UNREADABLE;
    }
    terminate_strings(&t);
    return same_triple(t, id) ? RTL_USB_ID_OK : RTL_USB_ID_MISMATCH;
#else
    (void)dev;
    (void)id;
    return RTL_USB_ID_NO_LIBUSB;
#endif
}

rtl_usb_id_result
rtl_usb_identity_reset(const struct rtl_usb_identity* id, int (*cancelled)(void*), void* ctx) {
#if RTL_USB_IDENTITY_REAL
    if (id == nullptr || !id->valid || is_cancelled(cancelled, ctx)) {
        return RTL_USB_ID_FAILED;
    }
    const rtl_usb_driver_ops* o = ops();
    void* session = nullptr;
    int count = 0;
    if (o->usb_open_session(&session, &count) != RTL_USB_OP_OK) {
        return RTL_USB_ID_FAILED;
    }
    int found = -1;
    for (int i = 0; i < count && found < 0; i++) {
        struct rtl_usb_device_info info;
        if (o->usb_describe(session, i, &info) == RTL_USB_OP_OK && at_identity_port(info, id)) {
            found = i;
        }
    }
    if (found < 0) {
        o->usb_close_session(session);
        return RTL_USB_ID_NOT_FOUND;
    }
    void* handle = nullptr;
    int rc = o->usb_open(session, found, &handle);
    if (rc != RTL_USB_OP_OK) {
        o->usb_close_session(session);
        if (rc == RTL_USB_OP_ACCESS) {
            return RTL_USB_ID_ACCESS;
        }
        return rc == RTL_USB_OP_GONE ? RTL_USB_ID_NOT_FOUND : RTL_USB_ID_FAILED;
    }
    /* The strings are re-read on the handle the reset goes through, so nothing can take the port in between. */
    triple t;
    rc = o->usb_read_strings(handle, t.manufacturer, t.product, t.serial);
    rtl_usb_id_result result = RTL_USB_ID_FAILED;
    if (rc != RTL_USB_OP_OK) {
        result = RTL_USB_ID_UNREADABLE;
    } else {
        terminate_strings(&t);
        if (!same_triple(t, id)) {
            result = RTL_USB_ID_MISMATCH;
        } else if (is_cancelled(cancelled, ctx)) {
            result = RTL_USB_ID_FAILED;
        } else {
            rc = o->usb_reset(handle);
            if (rc == RTL_USB_OP_OK) {
                result = RTL_USB_ID_OK;
            } else if (rc == RTL_USB_OP_GONE) {
                result = RTL_USB_ID_NOT_FOUND;
            }
        }
    }
    o->usb_close(handle);
    o->usb_close_session(session);
    if (result == RTL_USB_ID_NOT_FOUND) {
        return rediscover(o, id, cancelled, ctx);
    }
    return result;
#else
    (void)id;
    (void)cancelled;
    (void)ctx;
    return RTL_USB_ID_NO_LIBUSB;
#endif
}

int
rtl_usb_identity_index(const struct rtl_usb_identity* id) {
#if RTL_USB_IDENTITY_REAL
    if (id == nullptr || !id->valid) {
        return -1;
    }
    triple want;
    DSD_MEMCPY(want.manufacturer, id->manufacturer, kStringSize);
    DSD_MEMCPY(want.product, id->product, kStringSize);
    DSD_MEMCPY(want.serial, id->serial, kStringSize);
    terminate_strings(&want);
    rtl_scan r = scan_rtl_indexes(ops(), want);
    if (r.unreadable > 0 || r.matches != 1) {
        return -1;
    }
    return r.index;
#else
    (void)id;
    return -1;
#endif
}

const char*
rtl_usb_identity_off_reason(rtl_usb_id_result r) {
    switch (r) {
        case RTL_USB_ID_OK: return "";
        case RTL_USB_ID_NO_STRINGS: return "the receiver reports no serial";
        case RTL_USB_ID_DUPLICATE: return "another device has the same serial (set a unique one with rtl_eeprom)";
        case RTL_USB_ID_UNREADABLE: return "could not read the USB strings of a candidate device";
        case RTL_USB_ID_NOT_FOUND: return "the receiver was not found on the USB bus";
        case RTL_USB_ID_MISMATCH: return "the opened receiver is not the USB device that was identified";
        case RTL_USB_ID_ACCESS: return "no permission to open the receiver's USB device";
        case RTL_USB_ID_NO_LIBUSB: return "built without libusb";
        case RTL_USB_ID_FAILED: break;
    }
    return "USB enumeration failed";
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
void
rtl_usb_identity_set_driver_ops(const struct rtl_usb_driver_ops* ops_in) {
#if RTL_USB_IDENTITY_REAL
    g_ops.store(ops_in != nullptr ? ops_in : &k_real_ops, std::memory_order_release);
#else
    (void)ops_in;
#endif
}
#endif
