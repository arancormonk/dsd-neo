// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>
#include <initializer_list>

extern "C" void rtl_device_test_set_open_fd_failure_hook(int (*hook)(int));
static int result;
static int calls;
static int failures;

static void
check(bool ok, const char* why) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", why);
        ++failures;
    }
}

static int
open_fd_failure(int fd) {
    ++calls;
    check(fd == 31 && rtl_device_preopened_fd_in_use(), "descriptor claimed before driver invocation");
    return result;
}

int
main() {
    input_ring_state ring{};
    rtl_device_test_set_open_fd_failure_hook(open_fd_failure);
    rtl_device_set_preopened_fd(31);
    for (const int code : {-6, -3, -1}) {
        result = code;
        rtl_device_clear_open_error();
        check(rtl_device_create(0, &ring) == nullptr, "descriptor open failure returns no device");
        check(rtl_device_last_open_error() == code, "descriptor error classification retained");
        check(!rtl_device_preopened_fd_in_use(), "failed descriptor claim released for host cleanup");
        check(rtl_device_preopened_fd_is_set(), "host still owns descriptor setting");
    }
    check(calls == 3, "every open reached descriptor driver path");
    rtl_device_set_preopened_fd(-1);
    rtl_device_clear_open_error();
    rtl_device_test_set_open_fd_failure_hook(nullptr);
    return failures ? 1 : 0;
}
