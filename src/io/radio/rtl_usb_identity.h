// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_USB_IDENTITY_H
#define DSD_NEO_IO_RADIO_RTL_USB_IDENTITY_H

/*
 * Which USB device an RTL-SDR stream runs on, and a USB reset of exactly that device.
 *
 * A wedged dongle can need a USB port reset (a close and reopen does not bring samples back), and another program
 * may be streaming from a second dongle, so the reset must never reach anything but the running one. The identity is
 * the (manufacturer, product, serial) string triple plus the bus, port path and VID:PID of the one device that
 * carries it:
 *
 * - rtl_usb_identity_resolve() runs before rtlsdr_open(). It takes the triple librtlsdr reads for the index, then
 *   reads the strings of every USB device with a vendor-specific (0xff) device or interface class, whatever its
 *   VID:PID. It succeeds only when exactly one of them carries the triple, none could not be read (an unreadable one
 *   might be the dongle, with a readable twin elsewhere), and librtlsdr's enumeration has exactly one index with it.
 *   A device this process may not open (access denied, or no usable driver: LIBUSB_ERROR_ACCESS or
 *   LIBUSB_ERROR_NOT_SUPPORTED) is not counted as unreadable: librtlsdr opens the dongle with the same permissions
 *   right after, so such a device cannot be it. Any other failure (I/O, pipe, timeout, descriptors unreadable on an
 *   opened device) still refuses. Accepted limit (like the open-time hotplug window): a dongle whose own open is
 *   refused in that instant while a readable device of another VID:PID carries the same triple records that device.
 * - rtl_usb_identity_verify_handle() checks that the opened handle carries the triple.
 * - rtl_usb_identity_reset() opens the device at the recorded bus + port path with the recorded VID:PID, re-reads its
 *   strings and resets it only when they are still the triple; a device that replaced it on that port is left alone.
 *   A device that re-enumerates in the reset is found again by its triple (bounded, cancellable; refused devices of
 *   another VID:PID are left out, a refused one with the recorded VID:PID is waited for). The port path
 *   survives a re-enumeration on the same port; a caller that finds the device elsewhere re-resolves it to record the
 *   new one.
 *
 * Every libusb and librtlsdr call goes through a driver-ops table, so tests run over a fake device list. Without
 * libusb (DSD_HAVE_LIBUSB undefined, which includes every build without a system librtlsdr and the vendored Android
 * tree) every call returns RTL_USB_ID_NO_LIBUSB. Cold path only: nothing here runs per sample.
 */

#include <stdint.h>

typedef struct rtlsdr_dev rtlsdr_dev_t;

typedef enum {
    RTL_USB_ID_OK,
    RTL_USB_ID_NO_STRINGS, /* the receiver reports no serial */
    RTL_USB_ID_DUPLICATE,  /* another device carries the same triple */
    RTL_USB_ID_UNREADABLE, /* the strings of a candidate device could not be read */
    RTL_USB_ID_NOT_FOUND,  /* no device carries the triple (at the recorded port, for a reset) */
    RTL_USB_ID_MISMATCH,   /* the handle, or the device at the recorded port, carries other strings */
    RTL_USB_ID_ACCESS,     /* the device at the recorded port cannot be opened for lack of permission */
    RTL_USB_ID_NO_LIBUSB,  /* built without libusb */
    RTL_USB_ID_FAILED      /* enumeration or the reset failed, the identity is not valid, or a cancel came first */
} rtl_usb_id_result;

struct rtl_usb_identity {
    int valid;
    uint16_t vid, pid;
    uint8_t bus;
    uint8_t ports[8];
    int port_depth;
    char manufacturer[256], product[256], serial[256];
};

/* For a receiver opened from an Android USB descriptor, which this unit cannot identify. */
#define RTL_USB_ID_OFF_REASON_ANDROID "opened from an Android descriptor"

/* Before rtlsdr_open(index). Fills out (valid only on RTL_USB_ID_OK). */
rtl_usb_id_result rtl_usb_identity_resolve(int index, struct rtl_usb_identity* out);
/* After rtlsdr_open(): the opened handle carries the identity's triple. */
rtl_usb_id_result rtl_usb_identity_verify_handle(rtlsdr_dev_t* dev, const struct rtl_usb_identity* id);
/*
 * With the dongle closed: reset the device at the identity's port, then wait for it to come back if the reset made it
 * re-enumerate (at most 5000 ms). cancelled (may be NULL) is polled before the reset and while waiting; a cancel
 * returns RTL_USB_ID_FAILED before the reset and RTL_USB_ID_NOT_FOUND after it, and the caller's own flag says which.
 */
rtl_usb_id_result rtl_usb_identity_reset(const struct rtl_usb_identity* id, int (*cancelled)(void*), void* ctx);
/* The librtlsdr index that carries the identity's triple now, or -1 when none or more than one does. */
int rtl_usb_identity_index(const struct rtl_usb_identity* id);
/* The "stall recovery is off for this receiver: <reason>" text for a result ("" for RTL_USB_ID_OK). */
const char* rtl_usb_identity_off_reason(rtl_usb_id_result r);

/* One enumerated USB device as the identity rules see it. */
struct rtl_usb_device_info {
    uint16_t vid, pid;
    uint8_t bus;
    uint8_t ports[8];
    int port_depth;
    int vendor_specific; /* the device class, or any interface class of its configuration, is 0xff */
};

/* Results of the libusb ops. */
enum {
    RTL_USB_OP_OK = 0,
    RTL_USB_OP_ACCESS = -1,      /* LIBUSB_ERROR_ACCESS */
    RTL_USB_OP_GONE = -2,        /* LIBUSB_ERROR_NO_DEVICE or LIBUSB_ERROR_NOT_FOUND: disconnected or re-enumerated */
    RTL_USB_OP_ERROR = -3,       /* anything else */
    RTL_USB_OP_UNSUPPORTED = -4, /* LIBUSB_ERROR_NOT_SUPPORTED: no driver this process can open it through */
};

/*
 * Every libusb and librtlsdr call the unit makes. A session is one libusb context and one device list; devices are
 * addressed by their position in it. String buffers are 256 bytes, as librtlsdr's.
 */
struct rtl_usb_driver_ops {
    uint32_t (*rtl_get_device_count)(void);
    /* RTL_USB_OP_*: ACCESS or UNSUPPORTED when librtlsdr's libusb_open() of that index was refused. */
    int (*rtl_get_device_usb_strings)(uint32_t index, char* manufacturer, char* product, char* serial);
    int (*rtl_get_usb_strings)(rtlsdr_dev_t* dev, char* manufacturer, char* product, char* serial);
    int (*usb_open_session)(void** session, int* count);
    void (*usb_close_session)(void* session);
    /* On ACCESS/UNSUPPORTED from a configuration read, out still holds VID:PID, bus and port path. */
    int (*usb_describe)(void* session, int i, struct rtl_usb_device_info* out);
    int (*usb_open)(void* session, int i, void** handle);
    void (*usb_close)(void* handle);
    int (*usb_read_strings)(void* handle, char* manufacturer, char* product, char* serial);
    int (*usb_reset)(void* handle);
    uint64_t (*now_ms)(void);
    void (*sleep_ms)(unsigned int ms);
};

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* Test hook: NULL restores the real drivers. Install while nothing else uses the unit. */
void rtl_usb_identity_set_driver_ops(const struct rtl_usb_driver_ops* driver_ops);
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_USB_IDENTITY_H */
