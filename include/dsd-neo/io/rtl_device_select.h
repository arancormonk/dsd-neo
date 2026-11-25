// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The RTL-SDR dongles librtlsdr can see, and which one carries a USB serial.
 *
 * One place for the listing the engine prints at startup and `--rtl-list` prints, and for the lookup of a serial an
 * `rtl:` spec names (dsd_rtl_device_field_parse()). A device whose strings cannot be read (Windows: another program
 * streams from it; elsewhere: no permission), or that reports an empty serial, is unreadable: it is listed as such,
 * never matches a serial and never counts as a duplicate. Matching is exact (strcmp), as librtlsdr's own
 * rtlsdr_get_index_by_serial() and SoapyRTLSDR's are. Indexes are librtlsdr's and change between calls when dongles
 * come and go, so a serial is looked up again at every open.
 *
 * Built into the radio library; without librtlsdr (USE_RTLSDR undefined) every call fails. Cold path only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_SELECT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_SELECT_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** librtlsdr's USB string buffers, in bytes. */
#define RTL_DEVICE_STRING_SIZE     256

/** What an unreadable device shows in place of its strings. */
#define RTL_DEVICE_UNREADABLE_TEXT "(could not read: in use by another program, or no permission)"

/** One device librtlsdr enumerates. */
struct rtl_device_entry {
    int index;        /**< librtlsdr's index for it, at the time of the listing */
    int strings_read; /**< its USB strings were read (all "" otherwise; a read that returned none counts as failed) */
    int readable;     /**< and it reports a serial: it can be chosen by serial */
    char manufacturer[RTL_DEVICE_STRING_SIZE];
    char product[RTL_DEVICE_STRING_SIZE];
    char serial[RTL_DEVICE_STRING_SIZE];
};

/**
 * @brief List the devices librtlsdr enumerates, in index order.
 *
 * Every entry written is zeroed first, so an unreadable device never shows another's strings.
 *
 * @param out Receives up to @p capacity entries (may be NULL when @p capacity is 0, to count only).
 * @param capacity Entries @p out holds.
 * @param total Receives how many devices there are, beyond @p capacity too (may be NULL).
 * @return The entries written, or -1 without RTL-SDR support or when enumeration failed.
 */
int rtl_device_list(struct rtl_device_entry* out, int capacity, int* total);

/** How a lookup by serial ended (rtl_device_find_serial()). */
typedef enum {
    RTL_DEVICE_FIND_FOUND = 0,  /**< exactly one readable device carries the serial */
    RTL_DEVICE_FIND_NOT_FOUND,  /**< none does */
    RTL_DEVICE_FIND_DUPLICATE,  /**< two or more readable devices do */
    RTL_DEVICE_FIND_NO_DEVICES, /**< librtlsdr enumerates no device at all */
    RTL_DEVICE_FIND_FAILED      /**< no RTL-SDR support, bad arguments, or enumeration failed */
} rtl_device_find_result;

/** What a lookup saw. */
struct rtl_device_find_report {
    int devices;    /**< devices enumerated */
    int unreadable; /**< of them, unreadable (never matched) */
    int matches;    /**< readable devices carrying the serial, all of them counted */
    int index;      /**< the first match's index, -1 for none */
};

/**
 * @brief Find the one device that carries @p serial, exactly.
 *
 * @param serial The serial (non-empty).
 * @param report Receives what the lookup saw (may be NULL).
 * @return RTL_DEVICE_FIND_FOUND with the index in report->index, or why not.
 */
rtl_device_find_result rtl_device_find_serial(const char* serial, struct rtl_device_find_report* report);

/**
 * @brief One line's worth of @p entry: `Manufacturer, Product, SN: serial`, `Manufacturer, Product, SN: (none)` for a
 *        device that reports no serial, or RTL_DEVICE_UNREADABLE_TEXT when its strings could not be read.
 */
void rtl_device_entry_describe(const struct rtl_device_entry* entry, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_SELECT_H_ */
