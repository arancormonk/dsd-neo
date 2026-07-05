// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Copyright (C) 2025 by arancormonk <180709949+arancormonk@users.noreply.github.com>
 */

/*
 * Verify MAC VPDU length inference from MCO for unknown opcode and capacity capping.
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dsd-neo/core/safe_api.h"
#include "test_support.h"

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#endif

#define setenv dsd_test_setenv

// Forward declare config init to keep test dependencies narrow.
struct dsd_opts;

void dsd_neo_config_init(const struct dsd_opts* opts);

// Test shim entrypoint (provided by dsd-neo_proto_p25)
void p25_test_process_mac_vpdu(int type, const unsigned char* mac_bytes, int mac_len);

// Minimal stubs to satisfy linked objects from the P25 proto library
typedef struct dsd_opts dsd_opts;
typedef struct dsd_state dsd_state;

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
unpack_byte_array_into_bit_array(const uint8_t* input, uint8_t* output, int len) {
    (void)input;
    (void)output;
    (void)len;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_header_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
apx_embedded_alias_blocks_phase2(dsd_opts* opts, dsd_state* state, uint8_t slot, uint8_t* lc_bits) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)lc_bits;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
l3h_embedded_alias_decode(dsd_opts* opts, dsd_state* state, uint8_t slot, int16_t len, uint8_t* input) {
    (void)opts;
    (void)state;
    (void)slot;
    (void)len;
    (void)input;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
nmea_harris(dsd_opts* opts, dsd_state* state, uint8_t* input, uint32_t src, int slot) {
    (void)opts;
    (void)state;
    (void)input;
    (void)src;
    (void)slot;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetFreq(int sockfd, long int freq) {
    (void)sockfd;
    (void)freq;
    return false;
}

bool
// NOLINTNEXTLINE(misc-use-internal-linkage)
SetModulation(int sockfd, int bw) {
    (void)sockfd;
    (void)bw;
    return false;
}

void
// NOLINTNEXTLINE(misc-use-internal-linkage)
return_to_cc(dsd_opts* opts, dsd_state* state) {
    (void)opts;
    (void)state;
}
// NOLINTNEXTLINE(misc-use-internal-linkage)
struct RtlSdrContext* g_rtl_ctx = 0;

int
// NOLINTNEXTLINE(misc-use-internal-linkage)
rtl_stream_tune(struct RtlSdrContext* ctx, uint32_t center_freq_hz) {
    (void)ctx;
    (void)center_freq_hz;
    return 0;
}

static int
parse_len_fields(const char* s, int* lenB, int* lenC) {
    const char* p = strstr(s, "\"lenB\":");
    const char* q = strstr(s, "\"lenC\":");
    if (!p || !q) {
        return -1;
    }
    errno = 0;
    char* end_b = NULL;
    char* end_c = NULL;
    long vb = strtol(p + 7, &end_b, 10);
    long vc = strtol(q + 7, &end_c, 10);
    if (end_b == (p + 7) || end_c == (q + 7) || errno == ERANGE || vb < INT_MIN || vb > INT_MAX || vc < INT_MIN
        || vc > INT_MAX) {
        return -1;
    }
    *lenB = (int)vb;
    *lenC = (int)vc;
    return 0;
}

static int
run_case(int type, uint8_t opcode, int expectB, int expectC) {
    // Ensure JSON output is enabled
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init(NULL);

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "p25_mac_segment") != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 100;
    }

    unsigned char mac[24];
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[0] = 1;      // mark header present so MCO heuristic applies on FACCH
    mac[1] = opcode; // opcode with low 6 bits interpreted as MCO
    p25_test_process_mac_vpdu(type, mac, 24);

    dsd_test_capture_stderr_end(&cap);

    // Read file back
    FILE* f = fopen(cap.path, "r");
    if (!f) {
        DSD_FPRINTF(stderr, "Failed to open %s\n", cap.path);
        return 101;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    int lenB = -1, lenC = -1;
    if (parse_len_fields(buf, &lenB, &lenC) != 0) {
        DSD_FPRINTF(stderr, "JSON parse failed: %s\n", buf);
        return 102;
    }
    if (lenB != expectB) {
        DSD_FPRINTF(stderr, "lenB mismatch type=%d op=%02X got B=%d want B=%d (C=%d)\n", type, opcode, lenB, expectB,
                    lenC);
        return 103;
    }
    if (lenC != expectC) {
        DSD_FPRINTF(stderr, "lenC mismatch type=%d op=%02X got C=%d want C=%d\n", type, opcode, lenC, expectC);
        return 104;
    }
    (void)remove(cap.path);
    return 0;
}

static int
run_payload_len_case(const char* tag, int type, uint8_t opcode, uint8_t mfid, uint8_t payload_len, int expectB,
                     int expectC) {
    setenv("DSD_NEO_PDU_JSON", "1", 1);
    dsd_neo_config_init(NULL);

    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, tag) != 0) {
        DSD_FPRINTF(stderr, "Failed to capture stderr: %s\n", strerror(errno));
        return 200;
    }

    unsigned char mac[24];
    DSD_MEMSET(mac, 0, sizeof(mac));
    mac[0] = 1;
    mac[1] = opcode;
    mac[2] = mfid;
    mac[3] = payload_len;
    p25_test_process_mac_vpdu(type, mac, 24);

    dsd_test_capture_stderr_end(&cap);

    FILE* f = fopen(cap.path, "r");
    if (!f) {
        DSD_FPRINTF(stderr, "Failed to open %s\n", cap.path);
        return 201;
    }
    char buf[512];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    int lenB = -1, lenC = -1;
    if (parse_len_fields(buf, &lenB, &lenC) != 0) {
        DSD_FPRINTF(stderr, "%s JSON parse failed: %s\n", tag, buf);
        return 202;
    }
    if (lenB != expectB) {
        DSD_FPRINTF(stderr, "%s lenB mismatch got B=%d want B=%d (C=%d)\n", tag, lenB, expectB, lenC);
        return 203;
    }
    if (lenC != expectC) {
        DSD_FPRINTF(stderr, "%s lenC mismatch got C=%d want C=%d\n", tag, lenC, expectC);
        return 204;
    }
    (void)remove(cap.path);
    return 0;
}

int
main(void) {
    // FACCH capacity = 16 octets (after opcode). Choose opcode 0x23 (base table 0), MCO=35 → infer 34 → cap 16.
    int rc = run_case(/*FACCH*/ 0, 0x23, /*B*/ 16, /*C*/ 0);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_81_len", /*FACCH*/ 0, 0x81, 0x90, 0x06, /*B*/ 6, /*C*/ 10);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_8f_len", /*FACCH*/ 0, 0x8F, 0x90, 0x0B, /*B*/ 11, /*C*/ 5);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_moto_bf_len", /*FACCH*/ 0, 0xBF, 0x90, 0x00, /*B*/ 3, /*C*/ 13);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_multifrag_cont_len", /*FACCH*/ 0, 0x10, 0x0A, 0x00, /*B*/ 10, /*C*/ 6);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_harris_a8_len", /*FACCH*/ 0, 0xA8, 0xA4, 0x0A, /*B*/ 10, /*C*/ 6);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_harris_b0_len", /*SACCH*/ 1, 0xB0, 0xA4, 0x11, /*B*/ 17, /*C*/ 2);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_tait_b5_fixed_len", /*FACCH*/ 0, 0xB5, 0xD8, 0x08, /*B*/ 5, /*C*/ 11);
    if (rc != 0) {
        return rc;
    }
    rc = run_payload_len_case("p25_tait_variable_len", /*FACCH*/ 0, 0xB4, 0xD8, 0x08, /*B*/ 8, /*C*/ 8);
    if (rc != 0) {
        return rc;
    }
    DSD_FPRINTF(stderr, "P25p2 MAC MCO->length inference (FACCH) passed\n");
    return 0;
}

#if defined(__GNUC__) && !defined(__cplusplus)
#pragma GCC diagnostic pop
#endif
