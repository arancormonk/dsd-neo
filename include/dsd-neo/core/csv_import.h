// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief CSV import helpers for runtime/UI one-shot actions.
 *
 * Declares CSV import entrypoints implemented in core.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_IMPORT_H_H
#define DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_IMPORT_H_H

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>

#ifdef __cplusplus
extern "C" {
#endif

int csvGroupImport(const dsd_opts* opts, dsd_state* state);
int csvGroupImportPath(const char* group_file_path, dsd_state* state);
int csvLCNImport(const dsd_opts* opts, dsd_state* state);
int csvChanImport(const dsd_opts* opts, dsd_state* state);
int csvKeyImportDec(const dsd_opts* opts, dsd_state* state);
int csvKeyImportHex(const dsd_opts* opts, dsd_state* state);
int csvVertexKsImport(dsd_state* state, const char* path);

#ifdef __cplusplus
}
#endif
#endif /* DSD_NEO_INCLUDE_DSD_NEO_CORE_CSV_IMPORT_H_H */
