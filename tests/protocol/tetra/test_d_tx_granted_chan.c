// SPDX-License-Identifier: GPL-3.0-or-later
/* ETSI EN 300 392-2 Table 14.18 D-TX-GRANTED regression tests. */
#include <dsd-neo/protocol/tetra/tetra_mle.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/opts.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", m); failures++; } } while (0)

static void pack(uint8_t *b, uint32_t v, int off, int n)
{
    for (int i = n - 1; i >= 0; --i)
        b[off++] = (uint8_t)((v >> i) & 1u);
}

static int build(uint8_t *b, uint16_t call_id, uint8_t grant,
                 uint8_t request_allowed, uint8_t encrypted,
                 int party_present, uint32_t ssi)
{
    memset(b, 0, 80);
    pack(b, TETRA_MLE_PD_CMCE, 0, 3);
    pack(b, 11, 3, 5);       /* D-TX-GRANTED */
    pack(b, call_id, 8, 14);
    pack(b, grant, 22, 2);
    pack(b, request_allowed, 24, 1);
    pack(b, encrypted, 25, 1);
    pack(b, 0, 26, 1);       /* reserved */
    pack(b, party_present ? 1u : 0u, 27, 1); /* O-bit */
    if (!party_present)
        return 28;
    pack(b, 0, 28, 1);       /* notification absent */
    pack(b, 1, 29, 1);       /* TPTI present */
    pack(b, 1, 30, 2);       /* TPTI: SSI */
    pack(b, ssi, 32, 24);
    pack(b, 0, 56, 1);       /* terminating M-bit */
    return 57;
}

static void test_standard_layout_and_identity(void)
{
    uint8_t bits[80];
    const uint32_t ssi = 0xABCDEFu;
    int nbits = build(bits, 0x2345u, 3, 1, 1, 1, ssi);
    dsd_state *st = calloc(1, sizeof(*st));
    dsd_opts *opt = calloc(1, sizeof(*opt));
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(st->tetra_tx_granted_valid == 1, "grant-to-another enables receive U-plane");
    CHECK(st->tetra_tx_granted_ssi == ssi, "transmitting SSI decoded");
    CHECK(st->tetra_cmce_tx_granted_perm == 1, "request permission decoded");
    CHECK(st->tetra_enc_mode == 1, "encryption control decoded");
    free(st); free(opt);
}

static void test_non_voice_grants_do_not_enable_audio(void)
{
    uint8_t bits[80];
    dsd_state *st = calloc(1, sizeof(*st));
    dsd_opts *opt = calloc(1, sizeof(*opt));
    int nbits = build(bits, 1, 1, 0, 0, 0, 0);
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(st->tetra_tx_granted_valid == 0, "not-granted keeps receive U-plane off");
    nbits = build(bits, 1, 2, 0, 0, 0, 0);
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(st->tetra_tx_granted_valid == 0, "queued keeps receive U-plane off");
    free(st); free(opt);
}

static void test_cmce_never_overwrites_mac_allocation(void)
{
    uint8_t bits[80];
    int nbits = build(bits, 7, 0, 1, 0, 0, 0);
    dsd_state *st = calloc(1, sizeof(*st));
    dsd_opts *opt = calloc(1, sizeof(*opt));
    st->tetra_vc_assignment_valid = 1;
    st->tetra_vc_carrier = 400;
    st->tetra_vc_timeslot_bitmap = 4;
    st->tetra_vc_freq_hz = 470000000L;
    tetra_mle_dispatch(bits, nbits, 0, opt, st);
    CHECK(st->tetra_vc_assignment_valid == 1, "MAC allocation validity retained");
    CHECK(st->tetra_vc_carrier == 400, "MAC carrier retained");
    CHECK(st->tetra_vc_timeslot_bitmap == 4, "MAC timeslot bitmap retained");
    CHECK(st->tetra_vc_freq_hz == 470000000L, "MAC frequency retained");
    free(st); free(opt);
}

int main(void)
{
    test_standard_layout_and_identity();
    test_non_voice_grants_do_not_enable_audio();
    test_cmce_never_overwrites_mac_allocation();
    if (failures == 0) puts("PASS test_d_tx_granted_chan");
    return failures != 0;
}
