// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_SELECT_DRIVER_H
#define DSD_NEO_IO_RADIO_RTL_SELECT_DRIVER_H

/*
 * librtlsdr's device count and USB string reads, for choosing an RTL-SDR by serial (rtl_device_select.cpp, the public
 * side in <dsd-neo/io/rtl_device_select.h>) and for the stall recovery's identity (rtl_usb_identity.cpp).
 *
 * The rtl_select_guarded_*() calls are librtlsdr's own, guarded on MSVC against the exception a libusb DLL that does
 * not match the build raises inside them; built without librtlsdr (USE_RTLSDR undefined) they fail. String buffers are
 * 256 bytes, as librtlsdr's. Cold path only.
 */

#include <stdint.h>

typedef struct rtlsdr_dev rtlsdr_dev_t;

#ifdef __cplusplus
extern "C" {
#endif

/* rtlsdr_get_device_count() into @p count: 0, or -1 (and *count 0) when it raised an exception or there is no
 * librtlsdr. */
int rtl_select_guarded_device_count(uint32_t* count);
/* rtlsdr_get_device_usb_strings(): its result, or -1 when it raised an exception or there is no librtlsdr. */
int rtl_select_guarded_device_usb_strings(uint32_t index, char* manufacturer, char* product, char* serial);
/* rtlsdr_get_usb_strings() on an opened handle: its result, or -1 when it raised an exception or there is no
 * librtlsdr. */
int rtl_select_guarded_usb_strings(rtlsdr_dev_t* dev, char* manufacturer, char* product, char* serial);

/* The three reads the selection makes, as rtl_select_guarded_*() answer them. */
struct rtl_select_driver_ops {
    int (*get_device_count)(uint32_t* count);
    int (*get_device_usb_strings)(uint32_t index, char* manufacturer, char* product, char* serial);
    int (*get_usb_strings)(rtlsdr_dev_t* dev, char* manufacturer, char* product, char* serial);
};

/*
 * The serial the opened @p dev reports, into @p serial (256 bytes, zeroed first): 0, or -1 when its strings could not
 * be read or it reports none (an empty serial never matches a selection).
 */
int rtl_select_handle_serial(rtlsdr_dev_t* dev, char* serial);

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
/* Test hook: NULL restores librtlsdr. Install while nothing else uses the unit. */
void rtl_device_select_set_driver_ops(const struct rtl_select_driver_ops* driver_ops);
#endif

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_SELECT_DRIVER_H */
