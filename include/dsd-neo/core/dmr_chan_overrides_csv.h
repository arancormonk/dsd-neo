// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The DMR Tier III channels file of --dmr-chan-overrides (discussion #900 F3).
 *
 * One row per channel, `lpcn,frequency_hz`, both decimal: the channel number dsd-neo shows (1..4094) and its frequency
 * in Hz (100000..2147483647), or 0 to hide the channel. A first line that does not start with a number is a header.
 * Blank lines are skipped. A row with another shape or a number out of range is reported with a warning and skipped,
 * as -C reports its rows; a channel given again is reported and its later row is used.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_DMR_CHAN_OVERRIDES_CSV_H_
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_DMR_CHAN_OVERRIDES_CSV_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One accepted row: return 0 when it was taken, nonzero to report it refused. */
typedef int (*dsd_dmr_chan_override_row_fn)(uint16_t lpcn, long hz, void* ctx);

/**
 * Read @p path and pass each accepted row, in file order, to @p row with @p ctx. Returns how many rows @p row took, or
 * -1 when the file cannot be opened or read (an error is logged).
 */
int dsd_csv_dmr_chan_overrides_read(const char* path, dsd_dmr_chan_override_row_fn row, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_DMR_CHAN_OVERRIDES_CSV_H_ */
