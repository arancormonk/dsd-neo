// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The user config file's reads and writes a frontend asks for: load a file or one of its profiles, list its
 * profiles, save the session to a file (issue #672).
 *
 * A menu selects and calls these; it does no file I/O itself. Each runs on the caller's thread (the terminal's UI
 * thread) and hands the decoder what it read as a command, so the decoder's own structs are never touched here. A save
 * writes the config a frontend's published snapshot pair holds.
 */

#ifndef DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CONFIG_FILES_H_
#define DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CONFIG_FILES_H_

#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/state_fwd.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the config file @p path and submit it as a loaded file's apply (DSD_APP_CMD_CONFIG_APPLY with
 * dsd_app_config_load_payload): the session autosaves to @p path once the config stays applied. The decoder's status
 * line says whether it loaded.
 * @return 0 when the file was read and submitted; -1 when it was not read (missing, unreadable, malformed, no memory),
 * and nothing was submitted.
 */
int dsd_app_config_load_submit(const char* path);

/**
 * @brief Read profile @p profile of the config file @p path (the base config with `[profile.NAME]` on top) and submit
 * it as a loaded file's apply, which leaves autosave off.
 * @return 0 when submitted; -1 when the profile was not read, and nothing was submitted.
 */
int dsd_app_config_load_profile_submit(const char* path, const char* profile);

/**
 * @brief The `[profile.NAME]` sections of the config file @p path, as dsd_user_config_list_profiles() lists them:
 * @p names point into @p names_buf.
 * @return the number of names (0 for none), or -1 when the file was not read.
 */
int dsd_app_config_list_profiles(const char* path, const char** names, char* names_buf, size_t names_buf_size,
                                 int max_names);

/**
 * @brief Write the config @p opts and @p state hold (a frontend's published snapshot pair) to @p path, atomically.
 * With @p adopt_autosave set, a written file then becomes the session's autosave (DSD_APP_CMD_CONFIG_METADATA_SET:
 * on, @p path).
 * @return 0 when written; -1 when not (nothing submitted), also without a path or a snapshot.
 */
int dsd_app_config_save(const char* path, const dsd_opts* opts, const dsd_state* state, int adopt_autosave);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_INCLUDE_DSD_NEO_APP_CONTROL_CONFIG_FILES_H_ */
