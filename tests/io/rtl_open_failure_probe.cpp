// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/io/rtl_device.h>
#include <dsd-neo/runtime/input_ring.h>
#include <memory>
#include <stdint.h>

struct rtl_device;

// input_ring_state contains C++ atomics. Keep the existing C API test in C,
// and supply a real ring here so the open reaches the wrapped driver call.
extern "C" int dsd_test_rtl_open_failure(void);

// The open first identifies the receiver's USB device; with no device listed it
// stops there, so the probe never enumerates (or opens) the host's dongles.
// GNU ld --wrap requires external linkage and the reserved symbol name.
// NOLINTBEGIN(bugprone-reserved-identifier, misc-use-internal-linkage)
extern "C" uint32_t __wrap_rtlsdr_get_device_count(void);

uint32_t
__wrap_rtlsdr_get_device_count(void) {
    return 0U;
}

// NOLINTEND(bugprone-reserved-identifier, misc-use-internal-linkage)

int
dsd_test_rtl_open_failure(void) {
    auto ring = std::make_unique<input_ring_state>();
    std::unique_ptr<rtl_device, decltype(&rtl_device_destroy)> dev(rtl_device_create(0, ring.get()),
                                                                   rtl_device_destroy);
    // Even an unexpected successful open must release its borrowed ring before this probe returns.
    return dev == nullptr;
}
