// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_IO_RADIO_RTL_TUNER_GAINS_H
#define DSD_NEO_IO_RADIO_RTL_TUNER_GAINS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * librtlsdr's manual gain steps (rtlsdr_get_tuner_gains()), for a tuner known only by the rtl_tcp header: its tuner
 * type (librtlsdr's enum rtlsdr_tuner) and its gain count. Portable: no sockets, no librtlsdr.
 *
 * Returns 1 and stores in @p out_selected_tenth_db the step nearest to @p request_tenth_db (on a tie the lower step,
 * as the USB path picks) when @p tuner_type is one of librtlsdr's tuners (E4000=1, FC0012=2, FC0013=3, FC2580=4,
 * R820T=5, R828D=6) and @p header_gain_count is the size of that tuner's table. Otherwise returns 0 and leaves
 * @p out_selected_tenth_db alone: the caller sends the request as it is.
 */
int dsd_rtl_tuner_gain_nearest(uint32_t tuner_type, uint32_t header_gain_count, int request_tenth_db,
                               int* out_selected_tenth_db);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_IO_RADIO_RTL_TUNER_GAINS_H */
