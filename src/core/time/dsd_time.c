// SPDX-License-Identifier: ISC
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */
/*-------------------------------------------------------------------------------
 * dsd_time.c
 * Time and Date Functions
 *
 * LWVMOBILE
 * 2024-04 DSD-FME Florida Man Edition
 *-----------------------------------------------------------------------------*/

#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/timing.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"

int
dsd_format_local_datetime(time_t timestamp, dsd_local_datetime_format format, char* out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }

    const char* fallback = NULL;
    switch (format) {
        case DSD_LOCAL_DATETIME_TIME_COMPACT: fallback = "000000"; break;
        case DSD_LOCAL_DATETIME_TIME_COLON: fallback = "00:00:00"; break;
        case DSD_LOCAL_DATETIME_DATE_COMPACT: fallback = "00000000"; break;
        case DSD_LOCAL_DATETIME_DATE_SLASH: fallback = "0000/00/00"; break;
        case DSD_LOCAL_DATETIME_DATE_HYPHEN: fallback = "0000-00-00"; break;
        default: out[0] = '\0'; return 0;
    }

    struct tm local_tm;
    if (dsd_localtime(&timestamp, &local_tm) != 0) {
        DSD_SNPRINTF(out, out_size, "%s", fallback);
        return 0;
    }

    switch (format) {
        case DSD_LOCAL_DATETIME_TIME_COMPACT:
            DSD_SNPRINTF(out, out_size, "%02d%02d%02d", local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
            break;
        case DSD_LOCAL_DATETIME_TIME_COLON:
            DSD_SNPRINTF(out, out_size, "%02d:%02d:%02d", local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
            break;
        case DSD_LOCAL_DATETIME_DATE_COMPACT:
            DSD_SNPRINTF(out, out_size, "%04u%02u%02u", (unsigned)(local_tm.tm_year + 1900),
                         (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
            break;
        case DSD_LOCAL_DATETIME_DATE_SLASH:
            DSD_SNPRINTF(out, out_size, "%04u/%02u/%02u", (unsigned)(local_tm.tm_year + 1900),
                         (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
            break;
        case DSD_LOCAL_DATETIME_DATE_HYPHEN:
            DSD_SNPRINTF(out, out_size, "%04u-%02u-%02u", (unsigned)(local_tm.tm_year + 1900),
                         (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
            break;
        default: return 0;
    }
    return 1;
}
