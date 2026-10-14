// SPDX-License-Identifier: GPL-3.0-or-later
// Verify the production FFT wrapper against an independent complex DFT, run its
// largest transform on a small thread stack, and exercise each of its allocations,
// including retry after a failed size change.
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <dsd-neo/runtime/signal_search.h>
#ifdef _WIN32
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

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

// The signal search's largest transform. Without a work buffer, pffft takes 8 bytes per point of scratch from the
// caller's stack: 512 KiB here, which overflows a macOS secondary thread (512 KiB) and most of a Windows one (1 MiB).
constexpr int kLargestN = DSD_SEARCH_MAX_FFT;
constexpr size_t kSmallStack = size_t{256} * 1024;
// Static: 512 KiB of samples would not fit the small stack either.
alignas(64) float largest[2 * kLargestN];

void
largest_transform() {
    float* data = largest;
    fill(data, kLargestN);
    assert(cache.forward(kLargestN, data));
    for (int k = 0; k < kLargestN; ++k) {
        const double real = k == 3 ? 0.3 * kLargestN : 0.0;
        const double imag = k == 7 ? -0.1 * kLargestN : 0.0;
        assert(fabs(data[static_cast<size_t>(k) * 2] - real) < 0.01);
        assert(fabs(data[static_cast<size_t>(k) * 2 + 1] - imag) < 0.01);
    }
}

#ifdef _WIN32
unsigned __stdcall
largest_transform_entry(void*) {
    largest_transform();
    return 0;
}
#else
void*
largest_transform_entry(void*) {
    largest_transform();
    return nullptr;
}
#endif

void
test_small_stack() {
#ifdef _WIN32
    const uintptr_t handle = _beginthreadex(nullptr, static_cast<unsigned>(kSmallStack), largest_transform_entry,
                                            nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    assert(handle != 0);
    HANDLE thread = reinterpret_cast<HANDLE>(handle);
    assert(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0);
    CloseHandle(thread);
#else
    pthread_attr_t attr;
    assert(pthread_attr_init(&attr) == 0);
    assert(pthread_attr_setstacksize(&attr, kSmallStack) == 0);
    pthread_t thread;
    assert(pthread_create(&thread, &attr, largest_transform_entry, nullptr) == 0);
    assert(pthread_join(thread, nullptr) == 0);
    pthread_attr_destroy(&attr);
#endif
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
    test_small_stack();
#ifdef DSD_TEST_FFT_ALLOC_WRAP
    test_allocation_retry(1);
    // Force a different cached size before exercising the twiddle and work buffer allocations.
    test_dft();
    test_allocation_retry(2);
    test_dft();
    test_allocation_retry(3);
#endif
    return 0;
}

// NOLINTEND(cert-dcl03-c,misc-static-assert)
