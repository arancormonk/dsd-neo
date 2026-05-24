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

// Note: Callers must free the returned strings from alloc variants.
// All helpers zero-fill on localtime() failure; non-allocating variants are provided.

#include <dsd-neo/core/time_format.h>
#include <dsd-neo/platform/timing.h>
#include <stdlib.h>
#include <time.h>
#include "dsd-neo/core/safe_api.h"

static void
format_time_hms(time_t t, char* out, size_t out_size, int with_colons) {
    struct tm local_tm;
    if (dsd_localtime(&t, &local_tm) != 0) {
        DSD_SNPRINTF(out, out_size, with_colons ? "00:00:00" : "000000");
        return;
    }

    if (with_colons) {
        DSD_SNPRINTF(out, out_size, "%02d:%02d:%02d", local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    } else {
        DSD_SNPRINTF(out, out_size, "%02d%02d%02d", local_tm.tm_hour, local_tm.tm_min, local_tm.tm_sec);
    }
}

static void
format_date_ymd(time_t t, char* out, size_t out_size, char separator) {
    struct tm local_tm;
    if (dsd_localtime(&t, &local_tm) != 0) {
        if (separator == '-') {
            DSD_SNPRINTF(out, out_size, "0000-00-00");
        } else if (separator == '/') {
            DSD_SNPRINTF(out, out_size, "0000/00/00");
        } else {
            DSD_SNPRINTF(out, out_size, "00000000");
        }
        return;
    }

    if (separator == '-') {
        DSD_SNPRINTF(out, out_size, "%04u-%02u-%02u", (unsigned)(local_tm.tm_year + 1900),
                     (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
    } else if (separator == '/') {
        DSD_SNPRINTF(out, out_size, "%04u/%02u/%02u", (unsigned)(local_tm.tm_year + 1900),
                     (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
    } else {
        DSD_SNPRINTF(out, out_size, "%04u%02u%02u", (unsigned)(local_tm.tm_year + 1900),
                     (unsigned)(local_tm.tm_mon + 1), (unsigned)local_tm.tm_mday);
    }
}

static char*
alloc_time_string(size_t size) {
    return (char*)calloc(size, sizeof(char));
}

char*
getTime(void) {
    char* curr = alloc_time_string(7);
    if (curr) {
        format_time_hms(time(NULL), curr, 7, 0);
    }
    return curr;
}

//non-allocating variant
void
getTime_buf(char out[7]) {
    format_time_hms(time(NULL), out, 7, 0);
}

//get HH:mm:ss timestamp with colon (Sync/Console Display)
char*
getTimeC(void) {
    char* curr = alloc_time_string(9);
    if (curr) {
        format_time_hms(time(NULL), curr, 9, 1);
    }
    return curr;
}

void
getTimeC_buf(char out[9]) {
    format_time_hms(time(NULL), out, 9, 1);
}

//get HH:mm:ss timestamp with colon (Ncurses Call History)
char*
getTimeN(time_t t) {
    char* curr = alloc_time_string(9);
    if (curr) {
        format_time_hms(t, curr, 9, 1);
    }
    return curr;
}

void
getTimeN_buf(time_t t, char out[9]) {
    format_time_hms(t, out, 9, 1);
}

//get YYYYMMDD without hyphen (file operations)
char*
getDate(void) {
    char* curr = alloc_time_string(9);
    if (curr) {
        format_date_ymd(time(NULL), curr, 9, '\0');
    }
    return curr;
}

void
getDate_buf(char out[9]) {
    format_date_ymd(time(NULL), out, 9, '\0');
}

//get YYYY-MM-DD with hyphen (Sync/Console Display)
char*
getDateH(void) {
    char* curr = alloc_time_string(11);
    if (curr) {
        format_date_ymd(time(NULL), curr, 11, '-');
    }
    return curr;
}

void
getDateH_buf(char out[11]) {
    format_date_ymd(time(NULL), out, 11, '-');
}

//get YYYY/MM/DD with forward slash (LRRP files)
char*
getDateS(void) {
    char* curr = alloc_time_string(11);
    if (curr) {
        format_date_ymd(time(NULL), curr, 11, '/');
    }
    return curr;
}

void
getDateS_buf(char out[11]) {
    format_date_ymd(time(NULL), out, 11, '/');
}

//get YYYY-MM-DD with hyphen (Ncurses Call History)
char*
getDateN(time_t t) {
    char* curr = alloc_time_string(11);
    if (curr) {
        format_date_ymd(t, curr, 11, '-');
    }
    return curr;
}

void
getDateN_buf(time_t t, char out[11]) {
    format_date_ymd(t, out, 11, '-');
}

//get HHmmss timestamp no colon (file operations)
char*
getTimeF(time_t t) {
    char* curr = alloc_time_string(7);
    if (curr) {
        format_time_hms(t, curr, 7, 0);
    }
    return curr;
}

void
getTimeF_buf(time_t t, char out[7]) {
    format_time_hms(t, out, 7, 0);
}

//get YYYYMMDD without hyphen (file operations)
char*
getDateF(time_t t) {
    char* curr = alloc_time_string(9);
    if (curr) {
        format_date_ymd(t, curr, 9, '\0');
    }
    return curr;
}

void
getDateF_buf(time_t t, char out[9]) {
    format_date_ymd(t, out, 9, '\0');
}
