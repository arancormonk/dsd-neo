// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Configure-time probe for cmake/NativeArch.cmake: prints the highest MSVC
 * /arch: level (AVX512, AVX2, AVX) whose instructions the build host can run,
 * or NONE. MSVC has no -march=native, so the host CPU is asked directly.
 *
 * The checks follow src/dsp/simd_x86_cpu.h. A level counts only when the CPU
 * reports every feature that MSVC or clang-cl may emit for it and the OS saves
 * the matching register state (XCR0). GNU-style compilers can build this file
 * too, so the feature logic is testable on any x86 host.
 */

#include <stdio.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#define DSD_NATIVE_PROBE_X86 1
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#else
#define DSD_NATIVE_PROBE_X86 0
#endif

#if DSD_NATIVE_PROBE_X86
static void
probe_cpuid(unsigned int leaf, unsigned int subleaf, unsigned int regs[4]) {
#if defined(_MSC_VER)
    int info[4];
    __cpuidex(info, (int)leaf, (int)subleaf);
    regs[0] = (unsigned int)info[0];
    regs[1] = (unsigned int)info[1];
    regs[2] = (unsigned int)info[2];
    regs[3] = (unsigned int)info[3];
#else
    __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
}

static unsigned long long
probe_xgetbv(void) {
#if defined(_MSC_VER) && !defined(__clang__)
    return _xgetbv(0);
#else
    unsigned int eax = 0;
    unsigned int edx = 0;
    /* XGETBV as bytes: the mnemonic (and clang-cl's _xgetbv) needs the xsave
     * target feature, which this file is not built with. */
    __asm__ volatile(".byte 0x0f, 0x01, 0xd0" : "=a"(eax), "=d"(edx) : "c"(0));
    return ((unsigned long long)edx << 32) | eax;
#endif
}

static int
has_bits(unsigned int value, unsigned int mask) {
    return (value & mask) == mask;
}

static const char*
probe_level(void) {
    unsigned int regs[4] = {0, 0, 0, 0};
    probe_cpuid(0, 0, regs);
    unsigned int max_leaf = regs[0];
    if (max_leaf < 1) {
        return "NONE";
    }
    probe_cpuid(1, 0, regs);
    unsigned int ecx1 = regs[2];
    /* /arch:AVX (clang-cl: Sandy Bridge): SSE3, PCLMUL, SSSE3, SSE4.1, SSE4.2, POPCNT, OSXSAVE, AVX. */
    const unsigned int avx_ecx =
        (1u << 0) | (1u << 1) | (1u << 9) | (1u << 19) | (1u << 20) | (1u << 23) | (1u << 27) | (1u << 28);
    if (!has_bits(ecx1, avx_ecx)) {
        return "NONE";
    }
    unsigned long long xcr0 = probe_xgetbv();
    if ((xcr0 & 0x6u) != 0x6u) {
        return "NONE";
    }

    /* /arch:AVX2 (clang-cl: Haswell, x86-64-v3): FMA, MOVBE, F16C, BMI1, AVX2, BMI2, LZCNT. */
    const unsigned int avx2_ecx = (1u << 12) | (1u << 22) | (1u << 29);
    const unsigned int avx2_ebx7 = (1u << 3) | (1u << 5) | (1u << 8);
    unsigned int ebx7 = 0;
    if (max_leaf >= 7) {
        probe_cpuid(7, 0, regs);
        ebx7 = regs[1];
    }
    probe_cpuid(0x80000000u, 0, regs);
    unsigned int ecx_ext = 0;
    if (regs[0] >= 0x80000001u) {
        probe_cpuid(0x80000001u, 0, regs);
        ecx_ext = regs[2];
    }
    if (!has_bits(ecx1, avx2_ecx) || !has_bits(ebx7, avx2_ebx7) || !has_bits(ecx_ext, 1u << 5)) {
        return "AVX";
    }

    /* /arch:AVX512: AVX-512 F, DQ, CD, BW, VL, with the opmask and ZMM state enabled (XCR0 bits 5-7). */
    const unsigned int avx512_ebx7 = (1u << 16) | (1u << 17) | (1u << 28) | (1u << 30) | (1u << 31);
    if (!has_bits(ebx7, avx512_ebx7) || (xcr0 & 0xE6u) != 0xE6u) {
        return "AVX2";
    }
    return "AVX512";
}
#endif

int
main(void) {
#if DSD_NATIVE_PROBE_X86
    puts(probe_level());
#else
    puts("NONE");
#endif
    return 0;
}
