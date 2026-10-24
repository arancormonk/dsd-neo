// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Air interface decryption of TETRA trunked mode downlink signalling (tetra_air_sig.h): the candidate keys, their key
 * streams, the repeat that proves one, the identities it decrypts, and how long a call's key applies.
 */
#include "tetra_air_sig.h"

#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/crypto/tetra_tea.h>
#include <dsd-neo/protocol/tetra/tetra_air.h>
#include <dsd-neo/protocol/tetra/tetra_mac.h>
#include <dsd-neo/runtime/decode_clock.h>
#include <stddef.h>
#include <stdint.h>

#include "tetra_calls.h"
#include "tetra_log.h"
#include "tetra_state.h"

/* A key stream segment covers at most 432 bits on a phase modulation channel (EN 300 392-7 clause 6.3.0). */
#define AIR_SIG_KSS_MAX_BITS 432
/* Encrypted headers in a row with one parity that settle the cell's (a changeover sends both for a while). */
#define AIR_SIG_PARITY_RUN   3

static const char*
air_sig_tea_name(uint8_t ksg) {
    const char* const name = tetra_air_ksg_name((int)ksg);
    return name ? name : "TEA?";
}

/* ---- Candidates ------------------------------------------------------------------------------------------------- */

/* A candidate of loaded key @p key: its key, TEA @p ksg, network and, for a reduced key, channel. */
static void
air_sig_candidate(const dsd_tetra_air_key* key, uint8_t ksg, tetra_air_cand* c) {
    DSD_MEMSET(c, 0, sizeof(*c));
    DSD_MEMCPY(c->key, key->key, sizeof c->key);
    c->valid = 1;
    c->kind = key->kind;
    c->ksg = ksg;
    c->mcc = key->mcc;
    c->mnc = key->mnc;
    if (key->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED) {
        c->la = key->la;
        c->cc = key->cc;
        c->carrier = key->carrier;
    }
}

/* A TEA1 reduced key of exactly the channel being received: its carrier, location area and colour code. */
static int
air_sig_reduced_here(const dsd_tetra_air_key* key, const tetra_state_t* ts, uint16_t cn) {
    return key->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED && key->carrier == cn && key->la == ts->la
           && key->cc == (uint8_t)(ts->colour & 0x3Fu);
}

/* A loaded key of the network being received. */
static int
air_sig_key_of_net(const dsd_tetra_air_key* key, const tetra_state_t* ts) {
    return key->loaded && key->mcc == ts->mcc && key->mnc == ts->mnc;
}

/* The candidates for the network being received on carrier @p cn: its 80-bit key with the TEA it names, or with each
 * of TEA1..TEA3 when it names none (a SwMI may run several, EN 300 392-7 clause 6.3.1, so the network's evidence does
 * not narrow them), then a TEA1 reduced key of exactly this channel. */
static int
air_sig_candidates(const dsd_state* state, const tetra_state_t* ts, uint16_t cn, tetra_air_cand* out) {
    int n = 0;
    /* 80-bit keys first, whatever their slots: a proof takes the first candidate that proves, and a reduced key of the
     * same key stream holds no key for TA61 (tetra_air_sig_identity()). */
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS && n < TETRA_AIR_CANDIDATES; i++) {
        const dsd_tetra_air_key* const key = &state->tetra_air_keys[i];
        if (!air_sig_key_of_net(key, ts) || key->kind != DSD_TETRA_AIR_KEY_CK) {
            continue;
        }
        const int named = key->alg >= 1U && key->alg <= 3U;
        const uint8_t first = named ? (uint8_t)(key->alg - 1U) : 0U;
        const uint8_t last = named ? first : 2U;
        for (uint8_t ksg = first; ksg <= last && n < TETRA_AIR_CANDIDATES; ksg++) {
            air_sig_candidate(key, ksg, &out[n++]);
        }
    }
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS && n < TETRA_AIR_CANDIDATES; i++) {
        const dsd_tetra_air_key* const key = &state->tetra_air_keys[i];
        if (air_sig_key_of_net(key, ts) && air_sig_reduced_here(key, ts, cn)) {
            air_sig_candidate(key, 0U, &out[n++]);
        }
    }
    return n;
}

/* A candidate's fingerprint: which key and TEA it is, to match evidence decrypted under the same one (FNV-1a). It stays
 * in the evidence and is never shown. */
static uint32_t
air_sig_fingerprint(const tetra_air_cand* c) {
    uint32_t h = 2166136261U;
    const uint8_t head[8] = {
        c->kind, c->ksg, (uint8_t)(c->carrier >> 8), (uint8_t)c->carrier, (uint8_t)(c->la >> 8), (uint8_t)c->la,
        c->cc,   0x5Au};
    for (size_t i = 0; i < sizeof head; i++) {
        h = (h ^ head[i]) * 16777619U;
    }
    for (size_t i = 0; i < sizeof c->key; i++) {
        h = (h ^ c->key[i]) * 16777619U;
    }
    /* 0 marks a candidate that decrypted nothing (air_sig_digests()). */
    return h != 0U ? h : 1U;
}

/* XOR @p nbits of @p bits with key stream bits @p offset.. of initial value @p iv on carrier @p cn under @p c. */
static int
air_sig_xor(const tetra_state_t* ts, const tetra_air_cand* c, uint16_t cn, uint32_t iv, int offset, uint8_t* bits,
            int nbits) {
    uint8_t ks[(AIR_SIG_KSS_MAX_BITS + 7) / 8];
    if (iv == 0U || offset < 0 || nbits < 0 || offset + nbits > AIR_SIG_KSS_MAX_BITS) {
        return 0;
    }
    if (nbits == 0) {
        return 1;
    }
    const int ks_bytes = (offset + nbits + 7) / 8;
    DSD_MEMSET(ks, 0, sizeof ks);
    if (c->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED) {
        if (cn != c->carrier || ts->la != c->la || (uint8_t)(ts->colour & 0x3Fu) != c->cc) {
            return 0;
        }
        uint32_t reg = ((uint32_t)c->key[0] << 24) | ((uint32_t)c->key[1] << 16) | ((uint32_t)c->key[2] << 8)
                       | (uint32_t)c->key[3];
        tetra_tea1_keystream_reduced(reg, iv, ks, ks_bytes);
        DSD_SECURE_ZERO(&reg, sizeof reg);
    } else {
        /* TB5 binds the cipher key to this carrier, location area and colour code (EN 300 392-7 clause 6.3.2.2). */
        uint8_t eck[10];
        tetra_tea_derive_eck(cn, ts->la, (uint8_t)(ts->colour & 0x3Fu), c->key, eck);
        tetra_tea_keystream((int)c->ksg, eck, iv, ks, ks_bytes);
        DSD_SECURE_ZERO(eck, sizeof eck);
    }
    for (int i = 0; i < nbits; i++) {
        const int bit = offset + i;
        bits[i] = (uint8_t)((bits[i] & 1u) ^ ((ks[bit / 8] >> (7 - (bit % 8))) & 1u));
    }
    DSD_SECURE_ZERO(ks, sizeof ks);
    return 1;
}

/* Decrypt the pieces of @p ct into @p pt under @p c; 0 when a piece cannot be. */
static int
air_sig_decrypt(const tetra_state_t* ts, const tetra_air_cand* c, const uint8_t* ct, int nbits,
                const tetra_mac_piece* pieces, int npieces, uint8_t* pt) {
    DSD_MEMCPY(pt, ct, (size_t)nbits);
    for (int i = 0; i < npieces; i++) {
        const tetra_mac_piece* const p = &pieces[i];
        if ((int)p->start + (int)p->nbits > nbits
            || !air_sig_xor(ts, c, p->carrier, p->iv, (int)p->offset, pt + p->start, (int)p->nbits)) {
            return 0;
        }
    }
    return 1;
}

/* FNV-1a over the plaintext bits and their length. */
static uint64_t
air_sig_digest(const uint8_t* bits, int nbits) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < nbits; i++) {
        h = (h ^ (uint64_t)(bits[i] & 1u)) * 1099511628211ULL;
    }
    h = (h ^ (uint64_t)(uint32_t)nbits) * 1099511628211ULL;
    return h;
}

/* ---- Evidence --------------------------------------------------------------------------------------------------- */

static int
air_sig_same_header(const tetra_air_evidence* e, const tetra_air_sig_header* h, const tetra_state_t* ts, uint16_t cn,
                    int nbits) {
    return e->used && e->kind == h->kind && e->addr_type == h->addr_type && e->addr == h->addr && e->label == h->label
           && e->um == h->um && e->enc_mode == h->enc_mode && e->alloc == h->alloc && e->length == (uint16_t)nbits
           && e->carrier == cn && e->la == ts->la && e->cc == (uint8_t)(ts->colour & 0x3Fu);
}

/* The candidate an earlier TM-SDU of the same header, taken under another initial value, proves: the first of
 * @p cand_id whose plaintext digest one of them shares. -1 for none. */
static int
air_sig_find_proof(const tetra_state_t* ts, const tetra_air_sig_header* h, uint16_t cn, int nbits, uint32_t iv,
                   const uint32_t* cand_id, const uint64_t* digest, int ncand, double now_m) {
    for (int k = 0; k < ncand; k++) {
        if (cand_id[k] == 0U) {
            continue; /* no plaintext under this candidate */
        }
        for (int i = 0; i < TETRA_AIR_EVIDENCE_MAX; i++) {
            const tetra_air_evidence* const e = &ts->air_evidence[i];
            if (!air_sig_same_header(e, h, ts, cn, nbits) || e->iv == iv || now_m - e->at_m > e->keep_s) {
                continue;
            }
            for (int j = 0; j < (int)e->ncand; j++) {
                if (e->cand_id[j] == cand_id[k] && e->digest[j] == digest[k]) {
                    return k;
                }
            }
        }
    }
    return -1;
}

/* Keep the TM-SDU as evidence: the oldest of its header goes first once TETRA_AIR_EVIDENCE_PER_SIG are kept, else an
 * expired or the oldest entry. */
static void
air_sig_keep(tetra_state_t* ts, const tetra_air_sig_header* h, uint16_t cn, int nbits, uint32_t iv,
             const uint32_t* cand_id, const uint64_t* digest, int ncand, double now_m, double keep_s) {
    tetra_air_evidence* slot = NULL;
    tetra_air_evidence* oldest_same = NULL;
    tetra_air_evidence* oldest = NULL;
    int same = 0;
    for (int i = 0; i < TETRA_AIR_EVIDENCE_MAX; i++) {
        tetra_air_evidence* const e = &ts->air_evidence[i];
        if (!e->used || now_m - e->at_m > e->keep_s || e->at_m > now_m) {
            if (!slot) {
                slot = e;
            }
            continue;
        }
        if (air_sig_same_header(e, h, ts, cn, nbits)) {
            same++;
            if (!oldest_same || e->at_m < oldest_same->at_m) {
                oldest_same = e;
            }
        }
        if (!oldest || e->at_m < oldest->at_m) {
            oldest = e;
        }
    }
    if (same >= TETRA_AIR_EVIDENCE_PER_SIG) {
        slot = oldest_same;
    } else if (!slot) {
        slot = oldest;
    }
    if (!slot) {
        return;
    }
    DSD_MEMSET(slot, 0, sizeof(*slot));
    slot->used = 1;
    slot->at_m = now_m;
    slot->keep_s = keep_s;
    slot->iv = iv;
    slot->kind = h->kind;
    slot->addr_type = h->addr_type;
    slot->addr = h->addr;
    slot->label = h->label;
    slot->um = h->um;
    slot->enc_mode = h->enc_mode;
    slot->alloc = h->alloc;
    slot->length = (uint16_t)nbits;
    slot->carrier = cn;
    slot->la = ts->la;
    slot->cc = (uint8_t)(ts->colour & 0x3Fu);
    slot->ncand = (uint8_t)ncand;
    for (int k = 0; k < ncand; k++) {
        slot->cand_id[k] = cand_id[k];
        slot->digest[k] = digest[k];
    }
}

int
tetra_air_sig_slot(const dsd_opts* opts, const dsd_state* state, int offset, tetra_mac_piece* piece) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    uint16_t cn = 0;
    if (!ts || !piece || ts->dmo || ts->uplink || !ts->net_known || !ts->sysinfo_known || !ts->tdma_valid
        || !ts->hyperframe_valid || !tetra_calls_carrier(opts, state, &cn)) {
        return 0;
    }
    tetra_air_cand cand[TETRA_AIR_CANDIDATES];
    const int n = air_sig_candidates(state, ts, cn, cand);
    DSD_SECURE_ZERO(cand, sizeof cand);
    if (n == 0) {
        return 0;
    }
    DSD_MEMSET(piece, 0, sizeof(*piece));
    piece->iv = tetra_air_build_iv(ts->tn, ts->fn, ts->mn, ts->hyperframe, 0U);
    piece->carrier = cn;
    piece->offset = (uint16_t)offset;
    return piece->iv != 0U;
}

/* The candidate an earlier copy proves for a TM-SDU long enough to count, which is then kept as evidence itself, for as
 * much longer as it took to arrive; -1 for none. */
static int
air_sig_prove(tetra_state_t* ts, const tetra_air_sig_header* h, uint16_t cn, const tetra_air_sig_input* in,
              const uint32_t* cand_id, const uint64_t* digest, int ncand) {
    if (in->nbits < TETRA_AIR_PROOF_MIN_BITS) {
        return -1;
    }
    const double now_m = dsd_decode_now_mono_s();
    double span_s = in->span_s > 0.0 ? in->span_s : 0.0;
    span_s = span_s < TETRA_AIR_EVIDENCE_SPAN_S ? span_s : TETRA_AIR_EVIDENCE_SPAN_S;
    const uint32_t iv = in->pieces[0].iv;
    const int proved = air_sig_find_proof(ts, h, cn, in->nbits, iv, cand_id, digest, ncand, now_m);
    air_sig_keep(ts, h, cn, in->nbits, iv, cand_id, digest, ncand, now_m, TETRA_AIR_EVIDENCE_S + span_s);
    return proved;
}

/* The digest of plaintext @p pt as evidence: a rebuilt TM-SDU's MAC-END allocation moved to its front, so two copies
 * fragmented apart compare alike. -1 when the allocation does not read there. */
static int
air_sig_canonical_digest(const tetra_air_sig_input* in, const uint8_t* pt, uint64_t* digest) {
    if (in->alloc_at < 0 || !in->alloc_bits) {
        *digest = air_sig_digest(pt, in->nbits);
        return 1;
    }
    const int n = in->alloc_bits(pt, in->nbits, in->alloc_at, in->alloc_ctx);
    if (n < 0 || in->alloc_at + n > in->nbits) {
        return 0;
    }
    uint8_t canonical[TETRA_MAC_FRAG_BITS];
    DSD_MEMCPY(canonical, pt + in->alloc_at, (size_t)n);
    DSD_MEMCPY(canonical + n, pt, (size_t)in->alloc_at);
    DSD_MEMCPY(canonical + n + in->alloc_at, pt + in->alloc_at + n, (size_t)(in->nbits - in->alloc_at - n));
    *digest = air_sig_digest(canonical, in->nbits);
    DSD_SECURE_ZERO(canonical, (size_t)in->nbits);
    return 1;
}

/* Decrypt under every candidate: each one's fingerprint and plaintext digest, the fingerprint 0 for one that could not
 * decrypt (it matches nothing). Returns 1 when one could. @p pt is scratch, zeroed after. */
static int
air_sig_digests(const tetra_state_t* ts, const tetra_air_cand* cand, int ncand, const tetra_air_sig_input* in,
                uint8_t* pt, uint32_t* cand_id, uint64_t* digest) {
    int decrypted = 0;
    for (int k = 0; k < ncand; k++) {
        cand_id[k] = 0U;
        if (air_sig_decrypt(ts, &cand[k], in->ct, in->nbits, in->pieces, in->npieces, pt)
            && air_sig_canonical_digest(in, pt, &digest[k])) {
            cand_id[k] = air_sig_fingerprint(&cand[k]);
            decrypted = 1;
        }
    }
    DSD_SECURE_ZERO(pt, (size_t)in->nbits);
    return decrypted;
}

static int
air_sig_input_ok(const tetra_air_sig_input* in) {
    return in && in->ct && in->pieces && in->npieces >= 1 && in->nbits >= 1 && in->nbits <= TETRA_MAC_FRAG_BITS;
}

static void
air_sig_epoch_moves(tetra_state_t* ts, const char* why) {
    ts->air_epoch++;
    TETRA_LOG_VERBOSE(ts, 1, "[TETRA AIR] the cell's key may have changed (%s); calls decrypt again once proved\n",
                      why);
}

/* The cell signals under key version parity @p parity: another than the settled one moves the cell key epoch. */
static void
air_sig_settle_parity(tetra_state_t* ts, uint8_t parity) {
    if (ts->air_parity_known && ts->air_parity != parity) {
        air_sig_epoch_moves(ts, "the encryption mode's key version parity changed");
    }
    ts->air_parity = parity;
    ts->air_parity_known = 1;
    ts->air_parity_next = parity;
    ts->air_parity_run = AIR_SIG_PARITY_RUN;
}

/* A proved TM-SDU settles the parity it was sent under at once, ahead of the run of headers: its key is that version's,
 * and the epoch it is stamped with is the one the headers still to come leave in place. */
int
tetra_air_sig_open(dsd_state* state, const tetra_air_sig_header* h, const tetra_air_sig_input* in, uint8_t* pt,
                   tetra_air_cand* used) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !h || !pt || !used || !air_sig_input_ok(in)) {
        return 0;
    }
    const uint16_t cn = in->pieces[0].carrier;
    tetra_air_cand cand[TETRA_AIR_CANDIDATES];
    const int ncand = air_sig_candidates(state, ts, cn, cand);
    uint32_t cand_id[TETRA_AIR_CANDIDATES] = {0};
    uint64_t digest[TETRA_AIR_CANDIDATES] = {0};
    const int decrypted = ncand > 0 && air_sig_digests(ts, cand, ncand, in, pt, cand_id, digest);
    const int proved = decrypted ? air_sig_prove(ts, h, cn, in, cand_id, digest, ncand) : -1;
    if (proved < 0 || !air_sig_decrypt(ts, &cand[proved], in->ct, in->nbits, in->pieces, in->npieces, pt)) {
        DSD_SECURE_ZERO(cand, sizeof cand);
        return 0;
    }
    *used = cand[proved];
    air_sig_settle_parity(ts, (uint8_t)(h->enc_mode & 1u));
    used->epoch = ts->air_epoch;
    used->parity = (uint8_t)(h->enc_mode & 1u);
    DSD_SECURE_ZERO(cand, sizeof cand);
    /* A plaintext only its own key stream gives again names the network's algorithm. */
    (void)tetra_air_ksg_remember(state, used->mcc, used->mnc, used->ksg);
    TETRA_LOG_VERBOSE(ts, 2, "[TETRA AIR] %d-bit TM-SDU proved by a repeat under %s%s\n", in->nbits,
                      air_sig_tea_name(used->ksg),
                      used->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED ? " (reduced key)" : "");
    return 1;
}

int
tetra_air_sig_identity(const tetra_air_cand* used, tetra_mac_address* addr) {
    if (!used || !addr || !used->valid || used->kind != DSD_TETRA_AIR_KEY_CK || !addr->esi) {
        return 0;
    }
    uint32_t ssi = 0;
    tetra_ta61_decrypt(used->key, addr->ssi, &ssi);
    addr->esi_value = addr->ssi;
    addr->ssi = ssi;
    addr->esi = 0;
    addr->esi_decrypted = 1;
    return 1;
}

/* ---- The cell's key ---------------------------------------------------------------------------------------------- */

void
tetra_air_sig_note_parity(dsd_state* state, uint8_t enc_mode) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts || !tetra_enc_mode_encrypted(enc_mode)) {
        return;
    }
    const uint8_t parity = (uint8_t)(enc_mode & 1u);
    if (ts->air_parity_run == 0U || ts->air_parity_next != parity) {
        ts->air_parity_next = parity;
        ts->air_parity_run = 1;
    } else if (ts->air_parity_run < UINT8_MAX) {
        ts->air_parity_run++;
    }
    if (ts->air_parity_run >= AIR_SIG_PARITY_RUN) {
        air_sig_settle_parity(ts, parity);
    }
}

void
tetra_air_sig_note_sysinfo(dsd_state* state, int cck_flag, uint16_t value, const tetra_cell_security* sec) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    if (cck_flag) {
        if (ts->air_version_known && ts->air_version != value) {
            air_sig_epoch_moves(ts, "another SCK-VN or CCK-id");
        }
        ts->air_version = value;
        ts->air_version_known = 1;
    }
    if (sec) {
        if (ts->air_sec_known && (ts->air_sckn != sec->sckn || ts->air_gck != sec->gck_supported)) {
            air_sig_epoch_moves(ts, "another SCKN or GCK support");
        }
        ts->air_sckn = sec->sckn;
        ts->air_gck = sec->gck_supported;
        ts->air_sec_known = 1;
    }
}

void
tetra_air_sig_forget_carrier(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (ts) {
        DSD_MEMSET(ts->air_evidence, 0, sizeof ts->air_evidence);
    }
}

void
tetra_air_sig_forget_cell(dsd_state* state) {
    tetra_state_t* const ts = tetra_st_existing(state);
    if (!ts) {
        return;
    }
    tetra_air_sig_forget_carrier(state);
    ts->air_epoch++;
    ts->air_version_known = 0;
    ts->air_sec_known = 0;
    ts->air_parity_known = 0;
    ts->air_parity_run = 0;
}

/* The loaded key @p key stands for, with its bytes and named TEA unchanged. */
static int
air_sig_key_loaded(const dsd_state* state, const tetra_air_cand* key) {
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS; i++) {
        const dsd_tetra_air_key* const k = &state->tetra_air_keys[i];
        if (!k->loaded || k->kind != key->kind || k->mcc != key->mcc || k->mnc != key->mnc) {
            continue;
        }
        if (key->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED
            && (k->carrier != key->carrier || k->la != key->la || k->cc != key->cc)) {
            continue;
        }
        int same = 1;
        for (size_t j = 0; j < sizeof k->key; j++) {
            same &= k->key[j] == key->key[j];
        }
        const int tea_ok = key->kind != DSD_TETRA_AIR_KEY_CK || k->alg == 0U || k->alg == (uint8_t)(key->ksg + 1U);
        return same && tea_ok;
    }
    return 0;
}

int
tetra_air_sig_key_current(const dsd_state* state, const tetra_air_cand* key) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    return ts && key && key->valid && key->epoch == ts->air_epoch
           && (!ts->air_parity_known || ts->air_parity == key->parity) && air_sig_key_loaded(state, key);
}

int
tetra_air_sig_decrypt_voice(const dsd_opts* opts, const dsd_state* state, const tetra_air_cand* key, uint8_t* bits,
                            int nbits, int offset) {
    const tetra_state_t* const ts = tetra_st_peek(state);
    uint16_t cn = 0;
    if (!ts || !bits || ts->dmo || !ts->hyperframe_valid || !ts->tdma_valid || !tetra_calls_carrier(opts, state, &cn)
        || !tetra_air_sig_key_current(state, key)) {
        return 0;
    }
    const uint32_t iv = tetra_air_build_iv(ts->tn, ts->fn, ts->mn, ts->hyperframe, ts->uplink ? 1u : 0u);
    return air_sig_xor(ts, key, cn, iv, offset, bits, nbits);
}
