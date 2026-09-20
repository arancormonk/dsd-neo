// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/safe_api.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/app_control/command_dispatch.h"
#include "../../src/crypto/vendor_ap_key_parse.h"
#include "dsd-neo/app_control/commands.h"
#include "dsd-neo/core/opts.h"
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/state.h"
#include "dsd-neo/core/state_fwd.h"

static unsigned char needle[4];
static unsigned int erased[2048];
static int watching;
static int failures;

// Observe actual owned copies while still alive. Never inspect expired stack
// addresses or print secret bytes. The production volatile wipe still runs.
static void*
observed_zero(void* storage, size_t dst_size, size_t count) {
    unsigned char* bytes = storage;
    int matched = 0;
    if (watching && storage && count <= dst_size) {
        for (size_t i = 0; i + sizeof needle <= count; ++i) {
            matched |= memcmp(bytes + i, needle, sizeof needle) == 0;
        }
    }
    void* result = dsd_safe_secure_zero_impl(storage, dst_size, count);
    if (matched) {
        if (count < sizeof erased / sizeof erased[0]) {
            ++erased[count];
        }
        for (size_t i = 0; i < count; ++i) {
            if (bytes[i] != 0) {
                ++failures;
            }
        }
    }
    return result;
}

#undef DSD_SECURE_ZERO
#define DSD_SECURE_ZERO(dst, size) observed_zero((dst), DSD_NEO_OBJECT_SIZE(dst), (size))

// Instrument only these production translation units; library code stays unchanged.
// NOLINTBEGIN(bugprone-suspicious-include) -- Deliberate private-source instrumentation.
#include "../../src/app_control/app_command_queue.c"
#include "../../src/crypto/crypt-tyt.c"

// NOLINTEND(bugprone-suspicious-include)

static void
check_cleanup(int ok, const char* label) {
    if (!ok) {
        ++failures;
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
    }
}

static void
watch(unsigned char byte) {
    DSD_MEMSET(needle, byte, sizeof needle);
    DSD_MEMSET(erased, 0, sizeof erased);
    watching = 1;
}

int
main(void) {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    const int commands[] = {DSD_APP_CMD_KEY_TYT_AP_SET,  DSD_APP_CMD_KEY_RETEVIS_RC2_SET, DSD_APP_CMD_KEY_TYT_EP_SET,
                            DSD_APP_CMD_KEY_KEN_SCR_SET, DSD_APP_CMD_KEY_ANYTONE_BP_SET,  DSD_APP_CMD_KEY_XOR_SET};
    const char* values[] = {"11111111111111111111111111111111",
                            "11111111111111111111111111111111",
                            "1111111111111111 2222222222222222",
                            "1111",
                            "1111",
                            "49:11111111111180"};
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; ++i) {
        struct dsd_app_command command = {0};
        command.id = commands[i];
        command.n = strlen(values[i]) + 1;
        DSD_MEMCPY(command.data, values[i], command.n);
        watch('1');
        check_cleanup(apply_cmd_key_management_stream_keys(&opts, &state, &command), "stream command handled");
        watching = 0;
        check_cleanup(erased[256] > 0, "stream handler erases its key text");
        if (i == 3 || i == 4) {
            check_cleanup(erased[128] > 0, "stream adapter erases its key text");
        }
    }
    struct dsd_app_command command = {0};
    command.n = 5 * sizeof(uint64_t);
    DSD_MEMSET(command.data, 0x11, command.n);
    watch(0x11);
    check_cleanup(apply_cmd_key_hytera_set(&opts, &state, &command), "Hytera command handled");
    check_cleanup(erased[40] > 0, "Hytera handler erases parsed payload");
    check_cleanup(apply_cmd_key_aes_set(&opts, &state, &command), "AES command handled");
    check_cleanup(erased[32] > 0, "AES handler erases parsed payload");
    watch('1');
    tyt_ep_aes_keystream_creation(&state, values[2], 0);
    check_cleanup(erased[1024] > 0, "TYT EP erases copied input");
    watch(0x11);
    tyt_ep_aes_keystream_creation(&state, values[2], 0);
    check_cleanup(erased[16] >= 2, "TYT EP erases derived user and reversed keys");
    watch('1');
    tyt_ap_pc4_keystream_creation(&state, values[0], 0);
    check_cleanup(erased[sizeof(dsd_vendor_ap_key)] > 0, "TYT AP erases parsed key");
    watch('1');
    tyt_ap_pc4_keystream_creation(&state, "11111111invalid", 0);
    check_cleanup(erased[sizeof(dsd_vendor_ap_key)] > 0, "TYT AP erases valid prefix on parse failure");
    watch(0x11);
    tyt_ap_pc4_keystream_creation(&state, values[0], 0);
    check_cleanup(erased[16] >= 2, "TYT AP erases both derived byte arrays");
    watching = 0;
    freeState(&state);
    return failures ? 1 : 0;
}
