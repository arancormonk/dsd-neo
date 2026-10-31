// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/**
 * @file
 * @brief The C++ reader of the environment override store (#782): a short value, a value longer than its stack
 * buffer, and a variable that is unset or hidden.
 */

#include <assert.h>
#include <dsd-neo/platform/env.h>
#include <stdio.h>
#include <string.h>
#include <string>

int
main() {
    static const char* const kName = "DSD_NEO_TEST_ENV_VALUE";
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);
    {
        const dsd_env_value unset(kName);
        assert(unset.c_str() == nullptr);
    }

    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, kName, "short") == 0);
    {
        const dsd_env_value value(kName);
        assert(value.c_str() != nullptr && strcmp(value.c_str(), "short") == 0);
    }

    // Longer than the reader's 128-byte stack buffer: sized and copied from the store.
    static const size_t kLengths[] = {127U, 128U, 129U, 1000U};
    for (size_t length : kLengths) {
        const std::string text(length, 'v');
        assert(dsd_env_set(DSD_ENV_LAYER_SESSION, kName, text.c_str()) == 0);
        const dsd_env_value value(kName);
        assert(value.c_str() != nullptr && text == value.c_str());
    }

    assert(dsd_env_set(DSD_ENV_LAYER_SESSION, kName, nullptr) == 0);
    {
        const dsd_env_value hidden(kName);
        assert(hidden.c_str() == nullptr);
    }
    dsd_env_clear_layer(DSD_ENV_LAYER_SESSION);

    printf("PLATFORM_ENV_VALUE: OK\n");
    return 0;
}
