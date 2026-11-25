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
 * streams from it; elsewhere: no permission) is unreadable and listed as such; one whose strings read but report no
 * serial is listed with `SN: (none)` and can be opened by index only. Neither ever matches a serial or counts as a
 * duplicate, and each is counted apart. Matching is exact (strcmp), as librtlsdr's own
 * rtlsdr_get_index_by_serial() and SoapyRTLSDR's are. Indexes are librtlsdr's and change between calls when dongles
 * come and go, so a serial is looked up again at every open.
 *
 * Built into the radio library; without librtlsdr (USE_RTLSDR undefined) every call fails. Cold path only.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_SELECT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_SELECT_H_

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/** librtlsdr's USB string buffers, in bytes. */
#define RTL_DEVICE_STRING_SIZE     256

/** Room for one line of rtl_device_entry_describe() or rtl_device_list_line(): three strings shown with every byte
    escaped (four characters each), and the line's own text. */
#define RTL_DEVICE_LINE_SIZE       (3 * 4 * RTL_DEVICE_STRING_SIZE + 192)

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
    int unreadable; /**< of them, whose strings could not be read (never matched) */
    int no_serial;  /**< of them, read but reporting no serial: index only (never matched) */
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
 *
 * The strings are shown escaped: a backslash doubled and any byte other than printable ASCII as `\xNN`, so what a
 * device reports cannot reach the terminal as a control sequence or a line of its own. @p out holds
 * RTL_DEVICE_LINE_SIZE bytes for the whole line.
 */
void rtl_device_entry_describe(const struct rtl_device_entry* entry, char* out, size_t out_size);

/** Room for one USB string as rtl_device_string_shown() shows it, every byte escaped. */
#define RTL_DEVICE_SHOWN_SIZE (4 * RTL_DEVICE_STRING_SIZE)

/**
 * @brief A string a device reported (manufacturer, product or serial), shown for a log line or the terminal: printable
 *        ASCII as it is, a backslash doubled, any other byte (a newline, ESC, a control or non-ASCII byte) as `\xNN`,
 *        as the listings show it. @p out (RTL_DEVICE_SHOWN_SIZE bytes holds any string) is cut short rather than
 *        overrun. The string itself is what a serial is matched against; only its display is escaped.
 */
void rtl_device_string_shown(const char* text, char* out, size_t out_size);

/**
 * @brief One `dsd-neo --rtl-list` line for @p entry, without its newline.
 *
 * `RTL-SDR N: Manufacturer, Product, SN: serial` and, for a readable device whose serial is unique among the readable
 * ones (@p shared 0; 1 when another carries it, -1 when that is not known: no argument and no mark) and made only of
 * letters, digits, `.`, `_` and `-`, the `-i` argument that names it
 * (`(-i rtl:00000001)`, `(-i rtl:serial=1090)` for one that reads as an index), safe to paste into any shell. A serial
 * with other characters is shown as a quoted literal with a note to quote the whole `-i` argument, and no argument to
 * paste; a shared serial, or one a spec cannot hold, is marked so. The strings are escaped for display as in
 * rtl_device_entry_describe() (a quoted serial's '"' as `\"` too); @p out holds RTL_DEVICE_LINE_SIZE bytes.
 */
void rtl_device_list_line(const struct rtl_device_entry* entry, int shared, char* out, size_t out_size);

/**
 * @brief `dsd-neo --rtl-list`: every RTL-SDR librtlsdr enumerates, one rtl_device_list_line() each to @p out, and the
 *        notes (duplicate serials, unreadable devices, nothing found) to @p err.
 *
 * Dongles plugged in between the count and the listing are listed too: the listing is taken again, a bounded number of
 * times, while it finds more devices than it had room for. A list still growing after that is printed with no
 * `-i` argument and no duplicate mark on any line (whether a serial is unique is not known) and a note that it is
 * incomplete.
 *
 * @return 0 (an empty list too), or 1 without RTL-SDR support, when the enumeration failed or memory ran out, as
 *         `--airspy-list`.
 */
int rtl_device_list_cli(FILE* out, FILE* err);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_IO_RTL_DEVICE_SELECT_H_ */
