// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief Pick a generated CSV from the imports directory, or fall back to typing a path.
 *
 * The "Import Channel Map CSV" and "Import Group List CSV" menu items used to
 * ask only for a path. RadioReference-generated files carry spaces in their
 * names and are tedious to type, so these items now offer a chooser of the
 * imports directory's files of the requested kind, with an "Enter a path..." row
 * that falls back to the old prompt. When the directory holds no such file the
 * prompt opens directly, so a user with no imports sees no change.
 *
 * This header is internal to src/ui/terminal/ and should NOT be installed.
 */
#ifndef DSD_NEO_SRC_UI_TERMINAL_CSV_PICKER_H_
#define DSD_NEO_SRC_UI_TERMINAL_CSV_PICKER_H_

#include <stddef.h>
#include "menu_prompts.h"

/** @brief Longest path a picked file can carry. */
#define CSV_PICKER_PATH_MAX  1024
/** @brief Longest chooser label a picked file can carry. */
#define CSV_PICKER_LABEL_MAX 160
/** @brief Files a single collect can return. */
#define CSV_PICKER_MAX       128

/**
 * @brief Collect the imports directory's CSVs of one kind, newest name order.
 *
 * A file is included when it has a readable provenance sidecar whose `kind`
 * matches @p kind; sidecar-less files are skipped, as their kind is unknown.
 * Paths and labels are filled in lockstep and sorted by label.
 *
 * @param dir    Imports directory, or NULL/"" for none.
 * @param kind   "chan" or "group".
 * @param paths  [out] Absolute paths, one per file.
 * @param labels [out] Display labels, one per file.
 * @param max    Capacity of both arrays.
 * @return The number of files collected (0..max), or 0 for a NULL/empty dir.
 */
int ui_csv_picker_collect(const char* dir, const char* kind, char paths[][CSV_PICKER_PATH_MAX],
                          char labels[][CSV_PICKER_LABEL_MAX], int max);

/**
 * @brief Open the picker for one CSV kind.
 *
 * When the imports directory holds matching files, a chooser lists them with a
 * final "Enter a path..." row; otherwise the path prompt opens immediately.
 * Either way @p on_done is invoked once with the chosen path, "" on an explicit
 * empty submit, or NULL on cancel - exactly as ui_prompt_open_string_async().
 *
 * @param kind         "chan" or "group".
 * @param prompt_title Title for the fall-back path prompt.
 * @param cap          Buffer capacity for a typed path.
 * @param on_done      Callback invoked with the result.
 * @param user_ctx     Context passed to @p on_done.
 */
void ui_csv_import_picker_open(const char* kind, const char* prompt_title, size_t cap, ui_prompt_string_done_fn on_done,
                               void* user_ctx);

#endif /* DSD_NEO_SRC_UI_TERMINAL_CSV_PICKER_H_ */
