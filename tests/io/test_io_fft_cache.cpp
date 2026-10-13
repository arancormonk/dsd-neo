// SPDX-License-Identifier: GPL-3.0-or-later
// Verify the production FFT wrapper against an independent complex DFT and
// exercise both setup allocations, including retry after a failed size change.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "rtl_fft_cache.h"

// LLVM 22+ misclassifies these runtime FFT oracles as compile-time assertions.
// NOLINTBEGIN(cert-dcl03-c,misc-static-assert)

namespace {
// Production caches deliberately outlive their users; keep this one reachable too.
dsd_io::FftSetupCache cache;
constexpr double pi = 3.14159265358979323846;
#ifdef DSD_TEST_FFT_ALLOC_WRAP
int fail_at = 0;
int allocations = 0;
#endif

void
fill(float* data, int n) {
    for (int i = 0; i < n; ++i) {
        const double phase = 2.0 * pi * static_cast<double>(i) / n;
        data[static_cast<size_t>(i) * 2] = static_cast<float>(0.3 * cos(3.0 * phase) + 0.1 * sin(7.0 * phase));
        data[static_cast<size_t>(i) * 2 + 1] = static_cast<float>(0.3 * sin(3.0 * phase) - 0.1 * cos(7.0 * phase));
    }
}

bool
same_bits(const float* first, const float* second, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint32_t a = 0;
        uint32_t b = 0;
        std::memcpy(&a, first + i, sizeof(a));
        std::memcpy(&b, second + i, sizeof(b));
        if (a != b) {
            return false;
        }
    }
    return true;
}

void
test_dft() {
    constexpr int n = 256;
    alignas(64) float original[2 * n];
    alignas(64) float actual[2 * n];
    fill(original, n);
    std::memcpy(actual, original, sizeof(actual));
    assert(cache.forward(n, actual));
    for (int k = 0; k < n; ++k) {
        double real = 0.0;
        double imag = 0.0;
        for (int i = 0; i < n; ++i) {
            const double angle = 2.0 * pi * k * i / n;
            real += original[static_cast<size_t>(i) * 2] * cos(angle)
                    + original[static_cast<size_t>(i) * 2 + 1] * sin(angle);
            imag += original[static_cast<size_t>(i) * 2 + 1] * cos(angle)
                    - original[static_cast<size_t>(i) * 2] * sin(angle);
        }
        assert(fabs(actual[static_cast<size_t>(k) * 2] - real) < 0.0001);
        assert(fabs(actual[static_cast<size_t>(k) * 2 + 1] - imag) < 0.0001);
    }
    std::memcpy(original, actual, sizeof(actual));
    fill(actual, n);
    assert(cache.forward(n, actual));
    assert(same_bits(original, actual, static_cast<size_t>(n) * 2));
}

void
test_sizes() {
    alignas(64) float data[8192];
    for (int n = 32; n <= 4096; n *= 2) {
        fill(data, n);
        assert(cache.forward(n, data));
        for (int k = 0; k < n; ++k) {
            const double real = k == 3 ? 0.3 * n : 0.0;
            const double imag = k == 7 ? -0.1 * n : 0.0;
            assert(fabs(data[static_cast<size_t>(k) * 2] - real) < 0.001);
            assert(fabs(data[static_cast<size_t>(k) * 2 + 1] - imag) < 0.001);
        }
    }
}
} // namespace

#ifdef DSD_TEST_FFT_ALLOC_WRAP
// GNU ld's --wrap option requires these exact symbol names.
// NOLINTBEGIN(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" void* __real_malloc(size_t size);

extern "C" void*
__wrap_malloc(size_t size) {
    if (fail_at > 0 && ++allocations == fail_at) {
        return nullptr;
    }
    return __real_malloc(size);
}

// NOLINTEND(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)

namespace {
void
test_allocation_retry(int failure) {
    alignas(64) float data[1024];
    alignas(64) float original[1024];
    fill(data, 512);
    std::memcpy(original, data, sizeof(data));
    fail_at = failure;
    allocations = 0;
    assert(!cache.forward(512, data));
    assert(allocations == failure);
    assert(same_bits(original, data, 1024));
    fail_at = 0;
    assert(cache.forward(512, data));
    assert(fabs(data[6] - 0.3 * 512) < 0.001);
}
} // namespace
#endif

int
main() {
    test_dft();
    test_sizes();
#ifdef DSD_TEST_FFT_ALLOC_WRAP
    test_allocation_retry(1);
    // Force a different cached size before exercising the twiddle allocation.
    test_dft();
    test_allocation_retry(2);
#endif
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
