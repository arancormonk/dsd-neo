// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_TESTS_FUZZ_SUPPORT_H_
#define DSD_NEO_TESTS_FUZZ_SUPPORT_H_

#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#define DSD_NEO_FUZZ_MAX_INPUT 65536U

static inline size_t
dsd_fuzz_bounded_size(size_t size) {
    return size > DSD_NEO_FUZZ_MAX_INPUT ? DSD_NEO_FUZZ_MAX_INPUT : size;
}

static inline int
dsd_fuzz_write_file(const char* path, const uint8_t* data, size_t size) {
    FILE* fp = fopen(path, "wb");
    if (!fp) {
        return -1;
    }
    if (size > 0U && fwrite(data, 1U, size, fp) != size) {
        fclose(fp);
        return -1;
    }
    return fclose(fp);
}

static inline int
dsd_fuzz_make_temp_path(char* out_path, size_t out_path_size, const char* prefix) {
    int fd = dsd_test_mkstemp(out_path, out_path_size, prefix);
    if (fd < 0) {
        return -1;
    }
    return dsd_close(fd);
}

static inline int
dsd_fuzz_make_temp_json_path(char* out_path, size_t out_path_size, const char* prefix) {
    char base_path[DSD_TEST_PATH_MAX];
    if (dsd_fuzz_make_temp_path(base_path, sizeof(base_path), prefix) != 0) {
        return -1;
    }
    (void)remove(base_path);
    int n = DSD_SNPRINTF(out_path, out_path_size, "%s.json", base_path);
    if (n < 0 || (size_t)n >= out_path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

#endif /* DSD_NEO_TESTS_FUZZ_SUPPORT_H_ */
