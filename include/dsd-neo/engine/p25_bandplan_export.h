// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Export of the learned P25 band plan (IDEN tables) to the band-plan CSV.
 *
 * Engine-level because, under trunk scan, the rows come from every target's
 * parked snapshot as well as the live tables; the writer itself is
 * csvP25BandplanExportRows() in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_ENGINE_P25_BANDPLAN_EXPORT_H_
#define DSD_NEO_INCLUDE_DSD_NEO_ENGINE_P25_BANDPLAN_EXPORT_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Write every ready IDEN entry as band-plan rows to @p path.
 *
 * Collects the live tables and, when trunk scan is active, each parked target's
 * snapshot tables (dsd_p25_bandplan_append_tables() de-duplicates by identifier,
 * table and WACN/SYS), then writes them with csvP25BandplanExportRows().
 * Serves the --p25-bandplan-export flag at clean shutdown and the
 * DSD_APP_CMD_EXPORT_P25_BANDPLAN command on the decoder thread.
 *
 * @return The number of rows written (>= 1), or -1 when there was nothing to
 *         write or the file could not be written (already logged).
 */
int dsd_engine_p25_bandplan_export(const dsd_opts* opts, const dsd_state* state, const char* path);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_ENGINE_P25_BANDPLAN_EXPORT_H_ */
