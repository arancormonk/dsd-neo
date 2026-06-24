// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Regression test: serial radio control must map configured baud rates,
 * preserve the opened descriptor, and send the documented resume commands.
 */

#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/io/control.h>
#include <dsd-neo/platform/file_compat.h>
#include <dsd-neo/platform/platform.h>
#include <dsd-neo/platform/posix_compat.h>
#include <dsd-neo/runtime/log.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#if !DSD_PLATFORM_WIN_NATIVE
#include <termios.h>
#include <unistd.h>
#endif
#include "dsd-neo/core/opts_fwd.h"
#include "dsd-neo/core/safe_api.h"
#include "dsd-neo/core/state_fwd.h"

#if !DSD_PLATFORM_WIN_NATIVE
static int g_open_result;
static char g_open_path[128];
static int g_tcsetattr_calls;
static int g_tcsetattr_fd;
static int g_tcsetattr_action;
static struct termios g_last_tty;
static int g_ospeed_calls;
static int g_ispeed_calls;
static speed_t g_last_ospeed;
static speed_t g_last_ispeed;
static int g_dsd_write_fd;
static char g_dsd_write_buf[16];
static size_t g_dsd_write_len;
static ssize_t g_dsd_write_result;
static int g_write_fd;
static unsigned char g_write_buf[16];
static size_t g_write_len;
static ssize_t g_write_result;

static void
reset_stubs(void) {
    g_open_result = -1;
    DSD_MEMSET(g_open_path, 0, sizeof(g_open_path));
    g_tcsetattr_calls = 0;
    g_tcsetattr_fd = -1;
    g_tcsetattr_action = -1;
    DSD_MEMSET(&g_last_tty, 0, sizeof(g_last_tty));
    g_ospeed_calls = 0;
    g_ispeed_calls = 0;
    g_last_ospeed = 0;
    g_last_ispeed = 0;
    g_dsd_write_fd = -1;
    DSD_MEMSET(g_dsd_write_buf, 0, sizeof(g_dsd_write_buf));
    g_dsd_write_len = 0;
    g_dsd_write_result = 7;
    g_write_fd = -1;
    DSD_MEMSET(g_write_buf, 0, sizeof(g_write_buf));
    g_write_len = 0;
    g_write_result = 5;
}

int
test_dsd_open_serial_write(const char* path) {
    if (path) {
        DSD_SNPRINTF(g_open_path, sizeof(g_open_path), "%s", path);
    }
    return g_open_result;
}

int
test_tcsetattr(int fd, int optional_actions, const struct termios* termios_p) {
    g_tcsetattr_calls++;
    g_tcsetattr_fd = fd;
    g_tcsetattr_action = optional_actions;
    if (termios_p) {
        g_last_tty = *termios_p;
    }
    return 0;
}

int
test_cfsetospeed(struct termios* termios_p, speed_t speed) {
    (void)termios_p;
    g_ospeed_calls++;
    g_last_ospeed = speed;
    return 0;
}

int
test_cfsetispeed(struct termios* termios_p, speed_t speed) {
    (void)termios_p;
    g_ispeed_calls++;
    g_last_ispeed = speed;
    return 0;
}

ssize_t
test_dsd_write(int fd, const void* buf, size_t count) {
    g_dsd_write_fd = fd;
    g_dsd_write_len = count;
    if (count <= sizeof(g_dsd_write_buf)) {
        DSD_MEMCPY(g_dsd_write_buf, buf, count);
    }
    return g_dsd_write_result;
}

ssize_t
test_write(int fd, const void* buf, size_t count) {
    g_write_fd = fd;
    g_write_len = count;
    if (count <= sizeof(g_write_buf)) {
        DSD_MEMCPY(g_write_buf, buf, count);
    }
    return g_write_result;
}
#endif

void
dsd_neo_log_write(dsd_neo_log_level_t level, const char* format, ...) {
    (void)level;
    (void)format;
}

#if !DSD_PLATFORM_WIN_NATIVE
static int
expect_open_failure_sets_fd_negative(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_SNPRINTF(opts.serial_dev, sizeof(opts.serial_dev), "%s", "/dev/test-radio");
    opts.serial_baud = 9600;
    opts.serial_fd = 77;

    openSerial(&opts, &state);
    if (opts.serial_fd != -1 || strcmp(g_open_path, "/dev/test-radio") != 0 || g_tcsetattr_calls != 0) {
        DSD_FPRINTF(stderr, "open failure did not leave serial fd negative without termios\n");
        return 1;
    }
    return 0;
}

static int
expect_open_success_maps_baud(int baud, speed_t expected_speed, int expect_speed_calls) {
    reset_stubs();
    g_open_result = 42;
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    DSD_SNPRINTF(opts.serial_dev, sizeof(opts.serial_dev), "%s", "/dev/test-radio");
    opts.serial_baud = baud;

    openSerial(&opts, &state);
    if (opts.serial_fd != 42 || g_tcsetattr_calls != 1 || g_tcsetattr_fd != 42 || g_tcsetattr_action != TCSANOW) {
        DSD_FPRINTF(stderr, "open success did not configure expected descriptor\n");
        return 1;
    }
    if (g_ospeed_calls != expect_speed_calls || g_ispeed_calls != expect_speed_calls) {
        DSD_FPRINTF(stderr, "unexpected speed call counts for baud %d\n", baud);
        return 1;
    }
    if (expect_speed_calls && (g_last_ospeed != expected_speed || g_last_ispeed != expected_speed)) {
        DSD_FPRINTF(stderr, "unexpected speed selected for baud %d\n", baud);
        return 1;
    }
    if ((g_last_tty.c_cflag & CSIZE) != CS8 || (g_last_tty.c_cflag & (PARENB | PARODD | CSTOPB | CRTSCTS)) != 0
        || (g_last_tty.c_iflag & (IXON | IXOFF | IXANY)) != 0 || g_last_tty.c_lflag != 0 || g_last_tty.c_oflag != 0
        || g_last_tty.c_cc[VMIN] != 1 || g_last_tty.c_cc[VTIME] != 5) {
        DSD_FPRINTF(stderr, "termios flags were not configured for 8N1 raw write\n");
        return 1;
    }
    return 0;
}

static int
expect_resume_scan_commands_and_reset(void) {
    reset_stubs();
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));

    opts.serial_fd = -1;
    state.numtdulc = 9;
    resumeScan(&opts, &state);
    if (state.numtdulc != 9 || g_dsd_write_len != 0 || g_write_len != 0) {
        DSD_FPRINTF(stderr, "invalid serial fd should not send resume commands\n");
        return 1;
    }

    opts.serial_fd = 55;
    state.numtdulc = 9;
    resumeScan(&opts, &state);
    const unsigned char expected_binary[] = {2, 75, 15, 3, 93};
    if (state.numtdulc != 0 || g_dsd_write_fd != 55 || g_dsd_write_len != 7
        || memcmp(g_dsd_write_buf, "\rKEY00\r", 7) != 0 || g_write_fd != 55 || g_write_len != sizeof(expected_binary)
        || memcmp(g_write_buf, expected_binary, sizeof(expected_binary)) != 0) {
        DSD_FPRINTF(stderr, "resumeScan did not send expected command sequence\n");
        return 1;
    }

    reset_stubs();
    g_dsd_write_result = 3;
    g_write_result = 2;
    opts.serial_fd = 56;
    state.numtdulc = 7;
    resumeScan(&opts, &state);
    if (state.numtdulc != 0 || g_dsd_write_len != 7 || g_write_len != 5) {
        DSD_FPRINTF(stderr, "partial write path did not attempt both commands and reset state\n");
        return 1;
    }
    return 0;
}
#endif

int
main(void) {
#if DSD_PLATFORM_WIN_NATIVE
    static dsd_opts opts;
    static dsd_state state;
    DSD_MEMSET(&opts, 0, sizeof(opts));
    DSD_MEMSET(&state, 0, sizeof(state));
    opts.serial_fd = 77;
    state.numtdulc = 5;
    openSerial(&opts, &state);
    if (opts.serial_fd != -1) {
        return 1;
    }
    resumeScan(&opts, &state);
    return state.numtdulc == 0 ? 0 : 1;
#else
    int rc = 0;
    rc |= expect_open_failure_sets_fd_negative();
    rc |= expect_open_success_maps_baud(9600, B9600, 1);
    rc |= expect_open_success_maps_baud(230400, B230400, 1);
    rc |= expect_open_success_maps_baud(12345, B115200, 1);
    rc |= expect_open_success_maps_baud(0, 0, 0);
    rc |= expect_resume_scan_commands_and_reset();
    return rc;
#endif
}
