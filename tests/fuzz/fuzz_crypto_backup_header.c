// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Restored backups are untrusted, and dsd_backup_check_header() (with dsd_backup_parse_header(), its whole-buffer
 * form) is the only parser that runs on them before authentication. It does no key derivation, so every input stays
 * cheap; dsd_backup_open() is deliberately not called here, since it would run scrypt on every accepted header.
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

    /* The header-only check reads no further than the header and agrees with the whole-buffer parse, which only
       reports a missing magic as FORMAT. */
    const size_t header_len = size < DSD_BACKUP_HEADER_BYTES ? size : DSD_BACKUP_HEADER_BYTES;
    const int checked = dsd_backup_check_header(data, header_len, (uint64_t)size, NULL, NULL);
    const int magic = dsd_backup_has_magic(data, size);
    if ((checked == DSD_BACKUP_ERR_NOT_BACKUP) == (magic != 0)
        || (checked == DSD_BACKUP_ERR_NOT_BACKUP ? DSD_BACKUP_ERR_FORMAT : checked) != rc) {
        abort();
    }
    return 0;
}
