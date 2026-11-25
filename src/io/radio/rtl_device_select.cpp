// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The RTL-SDR listing and the lookup of a dongle by USB serial (see <dsd-neo/io/rtl_device_select.h>), over
 *        librtlsdr's guarded string reads (rtl_select_driver.h).
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/io/rtl_device_select.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/runtime/input_spec.h>

#include <atomic>
#include <climits>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "rtl_select_driver.h"

#ifdef USE_RTLSDR
#include <rtl-sdr.h>
#if defined(_MSC_VER) && DSD_PLATFORM_WIN_NATIVE
#include <excpt.h>
#define RTL_SELECT_SEH 1
#endif
#endif

/* The guarded reads hold no C++ object: an MSVC __try block cannot unwind one. */
extern "C" int
rtl_select_guarded_device_count(uint32_t* count) {
    *count = 0U;
#if defined(RTL_SELECT_SEH)
    __try {
        *count = rtlsdr_get_device_count();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *count = 0U;
        return -1;
    }
    return 0;
#elif defined(USE_RTLSDR)
    *count = rtlsdr_get_device_count();
    return 0;
#else
    return -1;
#endif
}

extern "C" int
rtl_select_guarded_device_usb_strings(uint32_t index, char* manufacturer, char* product, char* serial) {
#if defined(RTL_SELECT_SEH)
    __try {
        return rtlsdr_get_device_usb_strings(index, manufacturer, product, serial);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#elif defined(USE_RTLSDR)
    return rtlsdr_get_device_usb_strings(index, manufacturer, product, serial);
#else
    (void)index;
    (void)manufacturer;
    (void)product;
    (void)serial;
    return -1;
#endif
}

extern "C" int
rtl_select_guarded_usb_strings(rtlsdr_dev_t* dev, char* manufacturer, char* product, char* serial) {
#if defined(RTL_SELECT_SEH)
    __try {
        return rtlsdr_get_usb_strings(dev, manufacturer, product, serial);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
#elif defined(USE_RTLSDR)
    return rtlsdr_get_usb_strings(dev, manufacturer, product, serial);
#else
    (void)dev;
    (void)manufacturer;
    (void)product;
    (void)serial;
    return -1;
#endif
}

namespace {

const rtl_select_driver_ops k_real_ops = {
    rtl_select_guarded_device_count,
    rtl_select_guarded_device_usb_strings,
    rtl_select_guarded_usb_strings,
};

std::atomic<const rtl_select_driver_ops*> g_ops{&k_real_ops};

const rtl_select_driver_ops*
ops(void) {
    return g_ops.load(std::memory_order_acquire);
}

/* The device count as an int, or -1 when it could not be read. */
int
device_count(const rtl_select_driver_ops* o) {
    uint32_t count = 0U;
    if (o->get_device_count(&count) != 0) {
        return -1;
    }
    return count > (uint32_t)INT_MAX ? INT_MAX : (int)count;
}

/* Index @p index's strings into @p e, zeroed first so a failed read leaves none of another device's behind. A read
 * that succeeded but returned no string at all is not taken as read (a driver can return 0 with empty strings), and
 * only a device that reports a serial is readable. */
void
read_entry(const rtl_select_driver_ops* o, int index, rtl_device_entry* e) {
    DSD_MEMSET(e, 0, sizeof *e);
    e->index = index;
    const int rc = o->get_device_usb_strings((uint32_t)index, e->manufacturer, e->product, e->serial);
    e->manufacturer[RTL_DEVICE_STRING_SIZE - 1] = '\0';
    e->product[RTL_DEVICE_STRING_SIZE - 1] = '\0';
    e->serial[RTL_DEVICE_STRING_SIZE - 1] = '\0';
    if (rc != 0 || (e->manufacturer[0] == '\0' && e->product[0] == '\0' && e->serial[0] == '\0')) {
        DSD_MEMSET(e->manufacturer, 0, sizeof e->manufacturer);
        DSD_MEMSET(e->product, 0, sizeof e->product);
        DSD_MEMSET(e->serial, 0, sizeof e->serial);
        return;
    }
    e->strings_read = 1;
    e->readable = e->serial[0] != '\0';
}

} // namespace

extern "C" int
rtl_select_handle_serial(rtlsdr_dev_t* dev, char* serial) {
    char manufacturer[RTL_DEVICE_STRING_SIZE];
    char product[RTL_DEVICE_STRING_SIZE];
    DSD_MEMSET(manufacturer, 0, sizeof manufacturer);
    DSD_MEMSET(product, 0, sizeof product);
    DSD_MEMSET(serial, 0, RTL_DEVICE_STRING_SIZE);
    if (dev == nullptr || ops()->get_usb_strings(dev, manufacturer, product, serial) != 0) {
        DSD_MEMSET(serial, 0, RTL_DEVICE_STRING_SIZE);
        return -1;
    }
    serial[RTL_DEVICE_STRING_SIZE - 1] = '\0';
    return serial[0] != '\0' ? 0 : -1;
}

extern "C" int
rtl_device_list(struct rtl_device_entry* out, int capacity, int* total) {
    if (total != nullptr) {
        *total = 0;
    }
    const rtl_select_driver_ops* o = ops();
    const int count = device_count(o);
    if (count < 0) {
        return -1;
    }
    if (total != nullptr) {
        *total = count;
    }
    if (out == nullptr || capacity <= 0) {
        return 0;
    }
    const int n = count < capacity ? count : capacity;
    for (int i = 0; i < n; i++) {
        read_entry(o, i, &out[i]);
    }
    return n;
}

extern "C" rtl_device_find_result
rtl_device_find_serial(const char* serial, struct rtl_device_find_report* report) {
    rtl_device_find_report rep{};
    rep.index = -1;
    rtl_device_find_result result = RTL_DEVICE_FIND_FAILED;
    const rtl_select_driver_ops* o = ops();
    const int count = (serial != nullptr && serial[0] != '\0') ? device_count(o) : -1;
    if (count == 0) {
        result = RTL_DEVICE_FIND_NO_DEVICES;
    } else if (count > 0) {
        rep.devices = count;
        rtl_device_entry e;
        for (int i = 0; i < count; i++) {
            read_entry(o, i, &e);
            if (!e.readable) {
                rep.unreadable++;
            } else if (strcmp(e.serial, serial) == 0) {
                rep.index = rep.matches == 0 ? i : rep.index;
                rep.matches++;
            }
        }
        result = rep.matches == 0   ? RTL_DEVICE_FIND_NOT_FOUND
                 : rep.matches == 1 ? RTL_DEVICE_FIND_FOUND
                                    : RTL_DEVICE_FIND_DUPLICATE;
    }
    if (report != nullptr) {
        *report = rep;
    }
    return result;
}

extern "C" void
rtl_device_entry_describe(const struct rtl_device_entry* entry, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0U) {
        return;
    }
    if (entry == nullptr || !entry->strings_read) {
        DSD_SNPRINTF(out, out_size, "%s", RTL_DEVICE_UNREADABLE_TEXT);
        return;
    }
    DSD_SNPRINTF(out, out_size, "%s, %s, SN: %s", entry->manufacturer, entry->product,
                 entry->readable ? entry->serial : "(none)");
}

namespace {

/* Whether every character of @p serial is a letter, a digit, '.', '_' or '-': a word no shell splits or expands. */
bool
serial_is_shell_safe(const char* serial) {
    for (const char* c = serial; *c != '\0'; c++) {
        const bool alnum = (*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9');
        if (!alnum && *c != '.' && *c != '_' && *c != '-') {
            return false;
        }
    }
    return serial[0] != '\0';
}

/* The -i device field that names @p serial: the serial itself when it reads back as that serial (00000001),
   serial=<sn> when it would read as an index (1090). */
void
spec_field(const char* serial, char* out, size_t out_size) {
    char parsed[RTL_DEVICE_STRING_SIZE];
    if (dsd_rtl_device_field_parse(serial, NULL, parsed, sizeof parsed) == DSD_RTL_DEV_SERIAL
        && strcmp(parsed, serial) == 0) {
        DSD_SNPRINTF(out, out_size, "%s", serial);
    } else {
        DSD_SNPRINTF(out, out_size, "serial=%s", serial);
    }
}

} // namespace

extern "C" void
rtl_device_list_line(const struct rtl_device_entry* entry, int shared, char* out, size_t out_size) {
    if (out == nullptr || out_size == 0U || entry == nullptr) {
        return;
    }
    if (!entry->readable) {
        char text[3 * RTL_DEVICE_STRING_SIZE + 16];
        rtl_device_entry_describe(entry, text, sizeof text);
        DSD_SNPRINTF(out, out_size, "RTL-SDR %d: %s", entry->index, text);
        return;
    }
    const bool safe = serial_is_shell_safe(entry->serial);
    const char* quote = safe ? "" : "\"";
    const char* note = "";
    char field[RTL_DEVICE_STRING_SIZE + 16];
    field[0] = '\0';
    if (shared) {
        note = "  [duplicate serial: cannot be chosen by serial]";
    } else if (!dsd_rtl_serial_is_valid(entry->serial)) {
        note = "  [this serial cannot be given in -i rtl:]";
    } else if (!safe) {
        note = "  [spaces or shell characters: quote the whole -i argument]";
    } else {
        char dev[RTL_DEVICE_STRING_SIZE + 8];
        spec_field(entry->serial, dev, sizeof dev);
        DSD_SNPRINTF(field, sizeof field, "  (-i rtl:%s)", dev);
    }
    DSD_SNPRINTF(out, out_size, "RTL-SDR %d: %s, %s, SN: %s%s%s%s%s", entry->index, entry->manufacturer, entry->product,
                 quote, entry->serial, quote, note, field);
}

#ifdef DSD_NEO_ENABLE_INTERNAL_TEST_HOOKS
extern "C" void
rtl_device_select_set_driver_ops(const struct rtl_select_driver_ops* driver_ops) {
    g_ops.store(driver_ops != nullptr ? driver_ops : &k_real_ops, std::memory_order_release);
}
#endif
