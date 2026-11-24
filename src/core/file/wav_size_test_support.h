// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_SRC_CORE_FILE_WAV_SIZE_TEST_SUPPORT_H_
#define DSD_NEO_SRC_CORE_FILE_WAV_SIZE_TEST_SUPPORT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Test seam (issue #888); not part of any public header. Size in bytes of the regular file @p filename as a 64-bit
   value (a recording can pass 2 GiB, which a Windows long cannot hold); -1 when it is missing, not a regular file or
   its size cannot be read. */
int64_t dsd_wav_file_size_or_negative(const char* filename);

/* Test seam (issue #888): called with the recording's path when an empty recording (the header alone), its size read,
   is about to be removed; a test makes the removal and any later read of the file fail from it. NULL (the default)
   for none. */
typedef void (*dsd_wav_test_discard_fn)(const char* path);
void dsd_wav_test_set_discard_hook(dsd_wav_test_discard_fn hook);

#ifdef __cplusplus
}
#endif

#endif /* DSD_NEO_SRC_CORE_FILE_WAV_SIZE_TEST_SUPPORT_H_ */
