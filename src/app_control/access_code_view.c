// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <dsd-neo/app_control/access_code_view.h>
#include <dsd-neo/core/access_code.h>
#include <dsd-neo/core/safe_api.h>
#include <stddef.h>
#include <stdint.h>

/* How one kind of code is spelled, and the values its protocol can carry. */
typedef struct {
    const char* short_label;
    const char* long_label;
    uint32_t min;
    uint32_t max;
    int hex; /* 1 = three uppercase hex digits (a NAC), 0 = decimal */
} access_code_format;

/* The format for @p kind, or NULL for a kind this build does not know (DSD_ACCESS_CODE_NONE included). */
static const access_code_format*
access_code_format_for(uint8_t kind) {
    /* dPMR's colour code runs to 63, DMR's to 15. The P25 NID reserves NAC 0x000 and 0xFFF. */
    static const access_code_format color_code = {"CC", "Color code", 0U, 63U, 0};
    static const access_code_format nac = {"NAC", "Network access code", 0x001U, 0xFFEU, 1};
    static const access_code_format ran = {"RAN", "Radio access number", 0U, 63U, 0};
    static const access_code_format can = {"CAN", "Channel access number", 0U, 15U, 0};
    switch (kind) {
        case DSD_ACCESS_CODE_COLOR_CODE: return &color_code;
        case DSD_ACCESS_CODE_NAC: return &nac;
        case DSD_ACCESS_CODE_RAN: return &ran;
        case DSD_ACCESS_CODE_CAN: return &can;
        default: return NULL;
    }
}

int
dsd_app_access_code_format(uint8_t kind, uint32_t value, int form, char* out, size_t out_size) {
    if (out == NULL || out_size == 0U) {
        return 0;
    }
    out[0] = '\0';
    const access_code_format* format = access_code_format_for(kind);
    if (format == NULL || value < format->min || value > format->max
        || (form != DSD_APP_ACCESS_CODE_SHORT && form != DSD_APP_ACCESS_CODE_LONG)) {
        return 0;
    }
    const char* label = form == DSD_APP_ACCESS_CODE_LONG ? format->long_label : format->short_label;
    const unsigned code = (unsigned)value;
    if (format->hex) {
        DSD_SNPRINTF(out, out_size, "%s %03X", label, code);
    } else {
        DSD_SNPRINTF(out, out_size, "%s %u", label, code);
    }
    return 1;
}

void
dsd_app_access_code_view(uint8_t kind, uint32_t value, dsd_app_access_code* out) {
    if (out == NULL) {
        return;
    }
    DSD_MEMSET(out, 0, sizeof(*out));
    if (dsd_app_access_code_format(kind, value, DSD_APP_ACCESS_CODE_SHORT, out->short_text, sizeof(out->short_text))
        == 0) {
        return;
    }
    (void)dsd_app_access_code_format(kind, value, DSD_APP_ACCESS_CODE_LONG, out->long_text, sizeof(out->long_text));
    out->visible = 1;
}
