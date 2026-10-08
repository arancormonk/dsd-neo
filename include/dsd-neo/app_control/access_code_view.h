// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Frontend-neutral text for the access code a call was heard with (issue #575).
 *
 * A call history row records its access code as a kind and a value (`Event_History::access_code_kind` and
 * `access_code`, `<dsd-neo/core/access_code.h>`). This view spells them the one way every frontend shows them: a
 * short form for a list row ("CC 1", "NAC 293", "RAN 5", "CAN 0") and a long one for a detail sheet ("Color code 1",
 * "Network access code 293"). A NAC is three uppercase hex digits, as P25 names it; every other code is decimal. A
 * kind this build does not know, or a value outside its protocol's range, shows nothing, so a corrupt or newer store
 * never reads as a code.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_ACCESS_CODE_VIEW_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_ACCESS_CODE_VIEW_H_

#include <dsd-neo/core/access_code.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Display-ready access code. Both texts are ASCII and always terminated; empty when not visible. */
typedef struct {
    int visible;         /**< 1 when kind and value name a real code. */
    char short_text[16]; /**< "CC 1", "NAC 293", "RAN 5", "CAN 0", or "". */
    char long_text[40];  /**< "Color code 1", "Network access code 293", "Radio access number 5",
                              "Channel access number 0", or "". */
} dsd_app_access_code;

/**
 * @brief Fill @p out with the text for an access code.
 *
 * Zeroes @p out first. The ranges: colour code 0..63 (dPMR's; DMR's 0..15 is a subset), NAC 0x001..0xFFE, RAN
 * 0..63, CAN 0..15. Anything else, `DSD_ACCESS_CODE_NONE` and an unknown kind included, leaves @p out not visible
 * with empty texts.
 *
 * @param kind  A `dsd_access_code_kind`.
 * @param value The code.
 * @param out   Receives the text; NULL is ignored.
 */
void dsd_app_access_code_view(uint8_t kind, uint32_t value, dsd_app_access_code* out);

/** @brief Which text dsd_app_access_code_format() writes. */
enum {
    DSD_APP_ACCESS_CODE_SHORT = 0, /**< The list row's text, `dsd_app_access_code::short_text`. */
    DSD_APP_ACCESS_CODE_LONG = 1,  /**< The detail sheet's text, `dsd_app_access_code::long_text`. */
};

/**
 * @brief Write one of the two texts dsd_app_access_code_view() fills, for a caller that shows one at a time.
 *
 * The text is the one the view's field holds, or "" where the view is not visible. A @p form other than
 * `DSD_APP_ACCESS_CODE_SHORT` or `DSD_APP_ACCESS_CODE_LONG` writes "". A text longer than @p out_size is cut short
 * and terminated; `sizeof(dsd_app_access_code::long_text)` holds either.
 *
 * @param kind     A `dsd_access_code_kind`.
 * @param value    The code.
 * @param form     `DSD_APP_ACCESS_CODE_SHORT` or `DSD_APP_ACCESS_CODE_LONG`.
 * @param out      Receives the text; NULL writes nothing.
 * @param out_size Capacity of @p out; 0 writes nothing.
 * @return 1 when kind and value name a real code and @p form is valid (the text was written), 0 otherwise.
 */
int dsd_app_access_code_format(uint8_t kind, uint32_t value, int form, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_ACCESS_CODE_VIEW_H_ */
