// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Restored backups are untrusted, and dsd_backup_parse_header() is the only parser that runs on them before
 * authentication. It does no key derivation, so every input stays cheap; dsd_backup_open() is deliberately not
 * called here, since it would run scrypt on every accepted header.
 */

#include <dsd-neo/crypto/backup_seal.h>

#include <stdlib.h>

#include "fuzz_support.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == NULL) {
        return 0;
    }

    size = dsd_fuzz_bounded_size(size);

    dsd_backup_kdf_params kdf;
    DSD_MEMSET(&kdf, 0, sizeof(kdf));
    size_t payload_len = 0U;
    const int rc = dsd_backup_parse_header(data, size, &kdf, &payload_len);

    /* An accepted header must describe exactly this input, within the open-side policy. */
    if (rc == DSD_BACKUP_OK) {
        if (payload_len > DSD_BACKUP_MAX_PAYLOAD_BYTES || dsd_backup_sealed_size(payload_len) != size
            || kdf.log2_n < 14U || kdf.log2_n > 17U || kdf.r != 8U || kdf.p < 1U || kdf.p > 4U) {
            abort();
        }
    } else if (rc != DSD_BACKUP_ERR_FORMAT && rc != DSD_BACKUP_ERR_VERSION && rc != DSD_BACKUP_ERR_PARAMS
               && rc != DSD_BACKUP_ERR_TOO_LARGE) {
        abort();
    }
    return 0;
}
