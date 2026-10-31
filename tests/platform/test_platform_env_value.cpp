// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The C++ reader of the environment override store (#782): a short value, a value longer than its stack
 * buffer, and a variable that is unset or hidden.
 */

#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/platform/env.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

/* Fails the test with @p what when @p ok is false. */
static void
require(bool ok, const char* what) {
    if (!ok) {
        DSD_FPRINTF(stderr, "PLATFORM_ENV_VALUE: %s\n", what);
        abort();
    }
}

int
main() {
    static const char* const kName = "DSD_NEO_TEST_ENV_VALUE";
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    {
        const dsd_env_value unset(kName);
        require(unset.c_str() == nullptr, "an unset variable reads as NULL");
    }

    require(dsd_env_set(DSD_ENV_LAYER_SESSION, kName, "short") == 0, "set a short value");
    {
        const dsd_env_value value(kName);
        require(value.c_str() != nullptr && strcmp(value.c_str(), "short") == 0, "read a short value");
    }

    // Longer than the reader's 128-byte stack buffer: sized and copied from the store.
    static const size_t kLengths[] = {127U, 128U, 129U, 1000U};
    for (size_t length : kLengths) {
        const std::string text(length, 'v');
        require(dsd_env_set(DSD_ENV_LAYER_SESSION, kName, text.c_str()) == 0, "set a long value");
        const dsd_env_value value(kName);
        require(value.c_str() != nullptr && text == value.c_str(), "read a long value whole");
    }

    require(dsd_env_set(DSD_ENV_LAYER_SESSION, kName, nullptr) == 0, "hide the variable");
    {
        const dsd_env_value hidden(kName);
        require(hidden.c_str() == nullptr, "a hidden variable reads as NULL");
    }
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);

    printf("PLATFORM_ENV_VALUE: OK\n");
    return 0;
}
