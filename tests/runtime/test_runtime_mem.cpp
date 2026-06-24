// SPDX-License-Identifier: GPL-3.0-or-later
// Coverage fixtures intentionally use private-source inclusion, synthetic sentinels,
// invalid-value negative vectors, or wrapper symbols to exercise guarded behavior.
// NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result)
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dsd-neo/runtime/mem.h>

static void
test_aligned_malloc_rejects_zero_size(void) {
    assert(dsd_neo_aligned_malloc(0) == nullptr);
}

static void
test_aligned_malloc_returns_aligned_writable_memory(void) {
    void* ptr = dsd_neo_aligned_malloc(DSD_NEO_ALIGN * 2U);
    assert(ptr != nullptr);
    assert((reinterpret_cast<std::uintptr_t>(ptr) % DSD_NEO_ALIGN) == 0U);

    std::memset(ptr, 0x5A, DSD_NEO_ALIGN * 2U);
    const unsigned char* bytes = static_cast<const unsigned char*>(ptr);
    assert(bytes[0] == 0x5AU);
    assert(bytes[DSD_NEO_ALIGN * 2U - 1U] == 0x5AU);

    dsd_neo_aligned_free(ptr);
}

static void
test_aligned_free_accepts_null(void) {
    dsd_neo_aligned_free(nullptr);
}

int
main(void) {
    test_aligned_malloc_rejects_zero_size();
    test_aligned_malloc_returns_aligned_writable_memory();
    test_aligned_free_accepts_null();

    std::puts("RUNTIME_MEM: OK");
    return 0;
}

// NOLINTEND(bugprone-implicit-widening-of-multiplication-result)
