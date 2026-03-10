// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2026 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

#ifndef DSD_NEO_DSP_SIMD_X86_CPU_H
#define DSD_NEO_DSP_SIMD_X86_CPU_H

#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>

static inline unsigned long long
dsd_neo_xgetbv(unsigned int xcr) {
    unsigned int eax = 0;
    unsigned int edx = 0;
    __asm__ __volatile__("xgetbv" : "=a"(eax), "=d"(edx) : "c"(xcr));
    return ((unsigned long long)edx << 32) | eax;
}
#endif

static inline bool
dsd_neo_cpu_has_avx2_with_os_support(void) {
#if defined(__GNUC__) || defined(__clang__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    bool osxsave = (ecx & (1u << 27)) != 0;
    bool avx = (ecx & (1u << 28)) != 0;
    bool fma = (ecx & (1u << 12)) != 0;
    if (!osxsave || !avx) {
        return false;
    }
    unsigned long long xcr0 = dsd_neo_xgetbv(0);
    bool ymm_enabled = (xcr0 & 0x6) == 0x6;
    if (!ymm_enabled) {
        return false;
    }
    if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
        return false;
    }
    bool has_avx2 = (ebx & (1u << 5)) != 0;
    return has_avx2 && fma;
#elif defined(_MSC_VER)
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    bool osxsave = (cpuInfo[2] & (1 << 27)) != 0;
    bool avx = (cpuInfo[2] & (1 << 28)) != 0;
    bool fma = (cpuInfo[2] & (1 << 12)) != 0;
    if (!osxsave || !avx) {
        return false;
    }
    unsigned long long xcr0 = _xgetbv(0);
    bool ymm_enabled = (xcr0 & 0x6) == 0x6;
    if (!ymm_enabled) {
        return false;
    }
    __cpuidex(cpuInfo, 7, 0);
    bool has_avx2 = (cpuInfo[1] & (1 << 5)) != 0;
    return has_avx2 && fma;
#else
    return false;
#endif
}

#endif /* x86-64 */

#endif /* DSD_NEO_DSP_SIMD_X86_CPU_H */
