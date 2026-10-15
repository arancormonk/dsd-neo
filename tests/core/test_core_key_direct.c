// SPDX-License-Identifier: GPL-3.0-or-later
#include <dsd-neo/app_control/commands.h>
#include <dsd-neo/app_control/frontend_runtime.h>
#include <dsd-neo/app_control/snapshot.h>
#include <dsd-neo/core/audio.h>
#include <dsd-neo/core/file_io.h>
#include <dsd-neo/core/init.h>
#include <dsd-neo/core/key_set.h>
#include <dsd-neo/core/keyring.h>
#include <dsd-neo/core/opts.h>
#include <dsd-neo/core/opts_fwd.h>
#include <dsd-neo/core/safe_api.h>
#include <dsd-neo/core/state.h>
#include <dsd-neo/core/state_fwd.h>
#include <dsd-neo/core/synctype_ids.h>
#include <dsd-neo/core/vocoder.h>
#include <dsd-neo/crypto/dmr_keystream.h>
#include <dsd-neo/runtime/bootstrap.h>
#include <dsd-neo/runtime/cli.h>
#include <mbelib-neo/mbelib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../src/app_control/commands_internal.h"
#include "test_support.h"

static int failures;

static void
check(int ok, const char* label) {
    if (!ok) {
        DSD_FPRINTF(stderr, "FAIL: %s\n", label);
        ++failures;
    }
}

/* Whether two TETRA air key tables hold the same keys, field by field: dsd_tetra_air_key has padding bytes. */
static int
tetra_air_keys_equal(const dsd_tetra_air_key* a, const dsd_tetra_air_key* b) {
    for (int i = 0; i < DSD_TETRA_AIR_KEY_SLOTS; i++) {
        if (a[i].loaded != b[i].loaded || a[i].kind != b[i].kind || a[i].alg != b[i].alg || a[i].mcc != b[i].mcc
            || a[i].mnc != b[i].mnc || a[i].la != b[i].la || a[i].cc != b[i].cc || a[i].carrier != b[i].carrier
            || memcmp(a[i].key, b[i].key, sizeof a[i].key) != 0) {
            return 0;
        }
    }
    return 1;
}

#ifdef DSD_NEO_TEST_ARGV_FREE_WRAP
static void* watched_argv[8];
static size_t watched_sizes[8];
static int watched_frees;
// GNU linker wrapper observes storage while still allocated; never prints bytes.
// NOLINTBEGIN(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
void __real_free(void* ptr);
void __wrap_free(void* ptr);

void
__wrap_free(void* ptr) {
    for (size_t i = 0; i < 8; ++i) {
        if (ptr && ptr == watched_argv[i]) {
            const unsigned char* bytes = ptr;
            int clear = 1;
            for (size_t j = 0; j < watched_sizes[i]; ++j) {
                clear &= bytes[j] == 0;
            }
            check(clear, "argv allocation securely erased before free");
            watched_argv[i] = NULL;
            ++watched_frees;
        }
    }
    __real_free(ptr);
}

// NOLINTEND(bugprone-reserved-identifier, cert-dcl37-c, cert-dcl51-cpp, misc-use-internal-linkage)
#endif

static void
seed(dsd_opts* opts, dsd_state* state) {
    initOpts(opts);
    initState(state);
    dsd_app_frontend_runtime_start(opts, state);
    state->K = 17;
    state->R = 19;
    state->RR = 23;
    state->rkey_array[7] = 29;
    state->rkey_array_loaded[7] = 1;
    opts->dmr_mute_encL = opts->dmr_mute_encR = 1;
}

static void
expected(const dsd_opts* opts, const dsd_state* state, int type, size_t width) {
    check(state->rkey_array[7] == 29 && state->rkey_array_loaded[7] == 1, "overlay preserves keyring");
    check(state->K == (type == 0 ? 0 : 17), "named basic scalar");
    check(state->R == (type == 2 ? 0xAB : type == 3 ? 32767 : 19), "named R scalar");
    check(state->RR == (type == 2 ? 0xAB : 23), "named RR scalar");
    if (type == 1) {
        check(state->K1 == (width == 10 ? 0x1111111111ULL : 0x1111111111111111ULL), "hex first scalar");
        check(state->K2 == (width == 10 ? 0 : 0x1111111111111111ULL), "hex second scalar");
        check(state->K3 == (width == 64 ? 0x1111111111111111ULL : 0), "hex third scalar");
        check(state->K4 == state->K3 && state->H == state->K1, "hex fourth and H scalar");
        check(state->aes_key_segments[0] == (width == 10 ? 0 : width / 16), "AES width");
        check(state->aes_key_loaded[0] == (width != 10), "AES activation");
    }
    check(state->keyloader == 0 && state->payload_keyid == 0 && state->payload_keyidR == 0, "direct activation");
    // The DMR encrypted-audio mute stays the user's (issue #655): a call the key decrypts plays by the vocoder's
    // verdict, one it cannot stays muted. Undecodable P25 audio is muted.
    check(opts->dmr_mute_encL == 1 && opts->dmr_mute_encR == 1, "every value leaves the DMR mute alone");
    check(opts->unmute_encrypted_p25 == 0, "every value keeps undecodable P25 muted");
}

static void
bootstrap_snapshot_secrets(void) {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    char name[] = "test", keyflag[] = "-H", key[65], replay[] = "-r", file[] = "playback-marker.amb";
    DSD_MEMSET(key, '1', 64);
    key[64] = 0;
    char* argv[] = {name, keyflag, key, replay, file, NULL};
    int count = 0, exit_code = -1;
    check(dsd_runtime_bootstrap(5, argv, &opts, &state, &count, &exit_code) == DSD_BOOTSTRAP_CONTINUE,
          "full bootstrap accepted");
    check(state.K1 != 0 && state.K4 != 0, "bootstrap installed key");
    int absent = 1;
    for (int i = 0; i < state.cli_argc_effective; ++i) {
        if (strstr(state.cli_argv[i], key)) {
            absent = 0;
        }
    }
    check(absent, "retained argv contains no startup key");
    check(count == 5 && state.optind == 4 && strcmp(state.cli_argv[state.optind], file) == 0,
          "playback argument index preserved");
    dsd_app_frontend_runtime_start(&opts, &state);
    const dsd_state* snapshot = dsd_app_get_latest_snapshot();
    check(snapshot && !snapshot->cli_argv && snapshot->cli_argc_effective == 0, "snapshot excludes argv ownership");
    dsd_app_frontend_runtime_stop();
#ifdef DSD_NEO_TEST_ARGV_FREE_WRAP
    for (int i = 0; i < state.cli_argc_effective && i < 8; ++i) {
        watched_argv[i] = state.cli_argv[i];
        watched_sizes[i] = strlen(state.cli_argv[i]) + 1;
    }
#endif
    freeState(&state);
#ifdef DSD_NEO_TEST_ARGV_FREE_WRAP
    check(watched_frees == count, "teardown released every retained argv allocation");
#endif
    check(!state.cli_argv && state.cli_argc_effective == 0, "teardown clears argv ownership");
    snapshot = dsd_app_get_latest_snapshot();
    check(snapshot && !snapshot->cli_argv, "retained snapshot has no dangling argv");
    DSD_SECURE_ZERO(key, sizeof key);
    DSD_SECURE_ZERO(&state, sizeof state);
    DSD_SECURE_ZERO(&opts, sizeof opts);
}

/* Known stream bytes: RC4 with a nine-byte zero key after drop 256;
 * AES-128/256 OFB with zero key/IV after the first block (OpenSSL oracle).
 * Record the plaintext emitted by the real voice pipeline, not just mute flags. */
static void
zero_key_voice(void) {
    static const unsigned char stream[3][7] = {
        {0x3b, 0x3f, 0xa8, 0x3f, 0xd3, 0x55, 0xe2},
        {0xf7, 0x95, 0xbd, 0x4a, 0x52, 0xe2, 0x9e},
        {0x08, 0xc3, 0x74, 0x84, 0x8c, 0x22, 0x82},
    };
    char frame[4][24] = {{0}}, cipher[49] = {0};
    mbe_process_result decoded;
    int ready = 0;
    for (int seed_value = 1; seed_value < 96 && !ready; ++seed_value) {
        DSD_MEMSET(frame, 0, sizeof frame);
        frame[(seed_value + 0) % 4][(seed_value * 5 + 3) % 24] = 1;
        frame[(seed_value + 1) % 4][(seed_value * 7 + 11) % 24] = 1;
        frame[(seed_value + 2) % 4][(seed_value * 13 + 17) % 24] = 1;
        frame[(seed_value + 3) % 4][(seed_value * 19 + 23) % 24] = 1;
        ready = mbe_decodeAmbe3600x2450Frame((const char (*)[24])frame, cipher, &decoded) >= 0
                && !dmr_ambe49_should_skip_crypto(cipher);
    }
    check(ready, "encrypted AMBE fixture is active voice");
    for (int algorithm = 0; algorithm < 3; ++algorithm) {
        for (int live = 0; live < 3; ++live) {
            for (int slot = 0; slot < 2; ++slot) {
                static dsd_opts opts;
                static dsd_state state;
                initOpts(&opts);
                initState(&state);
                dsd_app_frontend_runtime_start(&opts, &state);
                const int algid = algorithm == 0 ? 0x21 : algorithm == 1 ? 0x24 : 0x25;
                check(!dsd_dmr_voice_slot_can_decrypt(&state, slot, algid, 0), "absent key cannot decrypt");
                dsd_app_key_direct_payload key = {0};
                key.key_type = algorithm == 0 ? DSD_APP_KEY_TYPE_RC4 : DSD_APP_KEY_TYPE_HEX;
                DSD_MEMSET(key.value, '0', algorithm == 0 ? 1 : algorithm == 1 ? 32 : 64);
                if (live == 2) {
                    if (algorithm == 0) {
                        check(dsd_app_command_set_u64(DSD_APP_CMD_KEY_RC4DES_SET, 0) > 0, "zero legacy scalar queued");
                    } else {
                        dsd_app_aes_key_payload aes = {0};
                        check(dsd_app_command_set_aes_key(&aes) > 0, "zero legacy AES queued");
                    }
                    check(dsd_app_drain_cmds(&opts, &state) == 1, "zero legacy key applied");
                } else if (live) {
                    check(dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &key, sizeof key) > 0,
                          "zero live key queued");
                    check(dsd_app_drain_cmds(&opts, &state) == 1, "zero live key applied");
                } else {
                    char name[] = "zero-key-test", flag[] = "-H";
                    if (algorithm == 0) {
                        flag[1] = '1';
                    }
                    char* args[] = {name, flag, key.value, NULL};
                    check(dsd_parse_args(3, args, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE,
                          "zero startup key parsed");
                }
                check(dsd_dmr_voice_slot_can_decrypt(&state, slot, algid, 0), "supplied zero key is decryptable");
                state.synctype = DSD_SYNC_DMR_BS_VOICE_POS;
                state.currentslot = slot;
                state.payload_algid = state.payload_algidR = algid;
                state.payload_mi = state.payload_miR = 0;
                state.dropL = state.dropR = 256;
                state.DMRvcL = state.DMRvcR = 0;
                DSD_MEMSET(state.aes_iv, 0, sizeof state.aes_iv);
                DSD_MEMSET(state.aes_ivR, 0, sizeof state.aes_ivR);
                opts.floating_point = 1;
                opts.dmr_stereo = 1;
                opts.errorbars = 0;
                FILE* recording = tmpfile();
                check(recording != NULL, "plaintext recording opens");
                if (slot) {
                    opts.mbe_out_fR = recording;
                } else {
                    opts.mbe_out_f = recording;
                }
                processMbeFrame(&opts, &state, DSD_VOICE_PROTOCOL_DMR, NULL, frame, NULL);
                char recovered[49] = {0}, expected_plain[49];
                for (int bit = 0; bit < 49; ++bit) {
                    expected_plain[bit] = cipher[bit] ^ ((stream[algorithm][bit / 8] >> (7 - bit % 8)) & 1);
                }
                if (recording) {
                    check(fseek(recording, 0, SEEK_SET) == 0, "plaintext recording rewinds");
                    opts.mbe_in_f = recording;
                    check(readAmbe2450Data(&opts, &state, recovered) == 0, "decrypted frame was recorded");
                    check(memcmp(recovered, expected_plain, sizeof recovered) == 0,
                          "zero key recovers known plaintext");
                    fclose(recording);
                    opts.mbe_in_f = opts.mbe_out_f = opts.mbe_out_fR = NULL;
                }
                dsd_app_frontend_runtime_stop();
                freeState(&state);
                DSD_SECURE_ZERO(&state, sizeof state);
            }
        }
    }
}

static void
presence_follows_material(void) {
    static dsd_opts opts;
    static dsd_state state;
    initOpts(&opts);
    initState(&state);
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_BASIC, "0", DSD_KEY_APPLY_OVERLAY) == DSD_KEY_DIRECT_OK,
          "zero basic overlay accepted");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_RC4, "0", DSD_KEY_APPLY_OVERLAY) == DSD_KEY_DIRECT_OK,
          "zero scalar overlay accepted");
    state.keyloader = 1;
    check(dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x21, 0), "loader mode alone does not delete left scalar");
    check(dsd_dmr_voice_slot_can_decrypt(&state, 1, 0x21, 0), "loader mode alone does not delete right scalar");
    keyring_activate_slot_with_kid(&state, 0, 77);
    check(!dsd_dmr_voice_slot_can_decrypt(&state, 0, 0x21, 0), "missing keyring entry retires left scalar presence");
    check(dsd_dmr_voice_slot_can_decrypt(&state, 1, 0x21, 0), "keyring replacement preserves companion scalar");
    check(dsd_dmr_missing_alg_key_can_decrypt(&state, 0), "keyring replacement preserves unrelated supplied basic key");
    dsd_key_set empty = {0};
    dsd_key_set_install(&state, &empty);
    check(!dsd_dmr_missing_alg_key_can_decrypt(&state, 0) && !dsd_dmr_missing_alg_key_can_decrypt(&state, 1),
          "empty key set retires all presence");
    freeState(&state);
}

static void
test_m17_key_widths(void) {
    static dsd_state state;
    static dsd_opts opts;
    const char* aes192 = "000000000000000100000000000000020000000000000003";
    for (int path = 0; path < 3; ++path) {
        seed(&opts, &state);
        if (path == 0) {
            check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_M17_SCRAMBLER, "ABCDEF", DSD_KEY_APPLY_OVERLAY)
                      == DSD_KEY_DIRECT_OK,
                  "M17 typed 24-bit seed");
            check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_M17_AES, aes192, DSD_KEY_APPLY_OVERLAY)
                      == DSD_KEY_DIRECT_OK,
                  "M17 typed AES192");
        } else if (path == 1) {
            char name[] = "test", seed_flag[] = "--m17-scrambler-key", seed_value[] = "ABCDEF";
            char aes_flag[] = "--m17-aes-key";
            char aes_value[65];
            DSD_SNPRINTF(aes_value, sizeof(aes_value), "%s", aes192);
            char* argv[] = {name, seed_flag, seed_value, aes_flag, aes_value, NULL};
            check(dsd_parse_args(5, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "M17 CLI typed widths");
        } else {
            dsd_app_key_direct_payload payload = {0};
            payload.key_type = DSD_APP_KEY_TYPE_M17_SCRAMBLER;
            DSD_SNPRINTF(payload.value, sizeof(payload.value), "%s", "ABCDEF");
            check(dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof(payload)) > 0, "M17 seed queued");
            payload.key_type = DSD_APP_KEY_TYPE_M17_AES;
            DSD_SNPRINTF(payload.value, sizeof(payload.value), "%s", aes192);
            check(dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof(payload)) > 0, "M17 AES queued");
            dsd_app_drain_cmds(&opts, &state);
            DSD_SECURE_ZERO(&payload, sizeof(payload));
        }
        check(state.R == 0xABCDEF, "M17 seed retains all 24 bits");
        check(state.aes_key_loaded[0] && state.aes_key_segments[0] == 3 && state.A3[0] == 3,
              "M17 AES192 retains three segments");
        check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_M17_SCRAMBLER, "000000", DSD_KEY_APPLY_OVERLAY)
                  != DSD_KEY_DIRECT_OK,
              "M17 zero seed follows resolver availability");
        check(state.R == 0xABCDEF, "invalid M17 input preserves previous seed");
        check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_HEX, aes192, DSD_KEY_APPLY_OVERLAY) != DSD_KEY_DIRECT_OK,
              "generic hex does not acquire unsupported width");
        freeState(&state);
    }
}

/* TETRA air keys (--tetra-key mcc,mnc,key[,teaN] or mcc,mnc,<reduced key>,la=,cc=,carrier=): parsed strictly, an
 * 80-bit key filed one per network and a TEA1 reduced key one per channel, kept by other direct keys, carried by the key
 * set and wiped with the other keys. */
static void
test_tetra_air_keys(void) {
    static dsd_state state;
    static dsd_opts opts;
    static const uint8_t want[10] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A};
    seed(&opts, &state);
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,0102030405060708090A", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "TETRA key accepted");
    const dsd_tetra_air_key* k = &state.tetra_air_keys[0];
    check(k->loaded && k->mcc == 262 && k->mnc == 1001 && memcmp(k->key, want, sizeof want) == 0,
          "TETRA key filed under its network");
    check(k->kind == DSD_TETRA_AIR_KEY_CK && k->alg == 0U, "an 80-bit key that names no algorithm");

    static const char* const bad[] = {
        "1024,1,00000000000000000000",
        "1,16384,00000000000000000000",
        "1,1,000000000000000000000",
        "1,1,0000000000000000000",
        "1,1,0000000000000000000G",
        "-1,1,00000000000000000000",
        "1,1",
        "1,1,00000000000000000000,1",
        "",
        /* The algorithm an 80-bit key may name: TEA1 to TEA3, nothing else, and only there. */
        "1,1,00000000000000000000,tea4",
        "1,1,00000000000000000000,tea0",
        "1,1,00000000000000000000,tea",
        "1,1,00000000000000000000,tea12",
        "1,1,00000000000000000000,tea1,",
        "1,1,00000000000000000000,",
        "1,1,00000000,tea1",
        /* A reduced key is 8 hex digits and needs its whole channel, in order. */
        "1,1,00000000",
        "1,1,0000000,la=1,cc=1,carrier=1",
        "1,1,000000000,la=1,cc=1,carrier=1",
        "1,1,0000000G,la=1,cc=1,carrier=1",
        "1,1,00000000,la=1,cc=1",
        "1,1,00000000,cc=1,la=1,carrier=1",
        "1,1,00000000,la=16384,cc=1,carrier=1",
        "1,1,00000000,la=1,cc=64,carrier=1",
        "1,1,00000000,la=1,cc=1,carrier=4096",
        "1,1,00000000,la=-1,cc=1,carrier=1",
        "1,1,00000000,la=,cc=1,carrier=1",
        "1,1,00000000,LA=1,cc=1,carrier=1",
        "1,1,00000000,la=1,cc=1,carrier=1,tea1",
        "1,1,00000000000000000000,la=1,cc=1,carrier=1",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, bad[i], DSD_KEY_APPLY_OVERLAY) != DSD_KEY_DIRECT_OK,
              "malformed TETRA key refused");
    }
    check(state.tetra_air_keys[0].loaded && state.tetra_air_keys[1].loaded == 0, "refusals leave the table alone");

    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,FFFFFFFFFFFFFFFFFFFF", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "same network replaced");
    check(state.tetra_air_keys[0].key[0] == 0xFF && state.tetra_air_keys[1].loaded == 0,
          "replacement, not a duplicate");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1002,00000000000000000001", DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1003,00000000000000000002",
                                      DSD_KEY_APPLY_OVERLAY)
                     == DSD_KEY_DIRECT_OK
              && dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1004,00000000000000000003",
                                      DSD_KEY_APPLY_OVERLAY)
                     == DSD_KEY_DIRECT_OK,
          "four networks fit");

    /* An 80-bit key that names its algorithm, in either case; naming another replaces the network's key. */
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1002,00000000000000000011,TEA2",
                               DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && state.tetra_air_keys[1].mnc == 1002 && state.tetra_air_keys[1].alg == 2U
              && state.tetra_air_keys[1].kind == DSD_TETRA_AIR_KEY_CK && state.tetra_air_keys[1].key[9] == 0x11,
          "an 80-bit key names TEA2 and replaces the network's key");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1002,00000000000000000011,tea3",
                               DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && state.tetra_air_keys[1].alg == 3U && state.tetra_air_keys[4].loaded == 0,
          "an 80-bit key names TEA3");

    /* A TEA1 reduced key: its register big-endian in the first four bytes, filed per channel beside the network's
     * 80-bit key; all-zero is a register like any other. */
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,1A2b3C4d,la=4097,cc=25,carrier=654",
                               DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_OK,
          "a reduced key loads");
    const dsd_tetra_air_key* r = &state.tetra_air_keys[4];
    static const uint8_t want_reduced[10] = {0x1A, 0x2B, 0x3C, 0x4D, 0, 0, 0, 0, 0, 0};
    check(r->loaded && r->kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED && r->alg == 1U && r->mcc == 262 && r->mnc == 1001
              && r->la == 4097 && r->cc == 25 && r->carrier == 654 && memcmp(r->key, want_reduced, 10) == 0,
          "the reduced key is TEA1 for its channel, register big-endian");
    check(state.tetra_air_keys[0].loaded && state.tetra_air_keys[0].kind == DSD_TETRA_AIR_KEY_CK
              && state.tetra_air_keys[0].mnc == 1001,
          "the network's 80-bit key stays beside it");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,00000000,la=4097,cc=25,carrier=654",
                               DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && r->key[0] == 0 && r->key[3] == 0 && state.tetra_air_keys[5].loaded == 0,
          "the same channel's reduced key is replaced, all-zero accepted");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,00000001,la=4097,cc=25,carrier=655",
                               DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && state.tetra_air_keys[5].carrier == 655 && state.tetra_air_keys[4].carrier == 654,
          "another carrier is another reduced key");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,00000002,la=4098,cc=25,carrier=654",
                               DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,00000003,la=4097,cc=26,carrier=654",
                                      DSD_KEY_APPLY_OVERLAY)
                     == DSD_KEY_DIRECT_OK
              && state.tetra_air_keys[6].la == 4098 && state.tetra_air_keys[7].cc == 26,
          "another location area or colour code is another reduced key");

    /* Sixteen keys in all. */
    char spec[64];
    for (unsigned mnc = 2000U; mnc < 2008U; mnc++) {
        DSD_SNPRINTF(spec, sizeof spec, "262,%u,00000000000000000004", mnc);
        check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, spec, DSD_KEY_APPLY_OVERLAY) == DSD_KEY_DIRECT_OK,
              "keys fill the table");
    }
    check(state.tetra_air_keys[DSD_TETRA_AIR_KEY_SLOTS - 1].loaded && DSD_TETRA_AIR_KEY_SLOTS == 16,
          "sixteen keys fit");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1005,00000000000000000004", DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_INVALID_ARGUMENT,
          "a seventeenth key is refused");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,00000004,la=1,cc=1,carrier=1",
                               DSD_KEY_APPLY_OVERLAY)
              == DSD_KEY_DIRECT_INVALID_ARGUMENT,
          "and so is a seventeenth reduced key");
    check(state.tetra_air_keys[0].mnc == 1001 && state.tetra_air_keys[0].key[0] == 0xFF,
          "the refused key evicts nothing");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_TETRA_AIR, "262,1001,00000005,la=4097,cc=25,carrier=654",
                               DSD_KEY_APPLY_OVERLAY)
                  == DSD_KEY_DIRECT_OK
              && r->key[3] == 0x05,
          "a full table still replaces a key it holds");

    char hex10[11] = "0123456789";
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_HEX, hex10, DSD_KEY_APPLY_OVERLAY) == DSD_KEY_DIRECT_OK,
          "a hex key applies after TETRA keys");
    check(state.tetra_air_keys[0].loaded && state.tetra_air_keys[3].mnc == 1004, "a hex key keeps the TETRA keys");

    dsd_key_set captured = {0};
    dsd_key_set again = {0};
    check(dsd_key_set_capture(&captured, &state) == 0, "key set captures TETRA keys");
    static dsd_state other;
    static dsd_opts other_opts;
    seed(&other_opts, &other);
    dsd_key_set_install(&other, &captured);
    check(tetra_air_keys_equal(other.tetra_air_keys, state.tetra_air_keys), "install carries TETRA keys");
    check(dsd_key_set_capture(&again, &other) == 0 && dsd_key_set_equal(&captured, &again), "TETRA keys compare equal");
    other.tetra_air_keys[2].key[9] ^= 1U;
    dsd_key_set_free(&again);
    check(dsd_key_set_capture(&again, &other) == 0 && !dsd_key_set_equal(&captured, &again),
          "a different TETRA key compares unequal");
    dsd_key_set_free(&again);
    dsd_key_set_free(&captured);
    freeState(&other);

    char name[] = "test";
    char flag[] = "--tetra-key";
    char value[] = "1,2,00000000000000000005";
    char* argv[] = {name, flag, value, NULL};
    freeState(&state);
    seed(&opts, &state);
    check(state.tetra_air_keys[0].loaded == 0, "initState clears TETRA keys");
    check(dsd_parse_args(3, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE && state.tetra_air_keys[0].loaded
              && state.tetra_air_keys[0].mcc == 1 && state.tetra_air_keys[0].key[9] == 5,
          "CLI --tetra-key loads the key");
    char bad_value[] = "1,2,5";
    char* bad_argv[] = {name, flag, bad_value, NULL};
    static dsd_state bad_state;
    static dsd_opts bad_opts;
    seed(&bad_opts, &bad_state);
    check(dsd_parse_args(3, bad_argv, &bad_opts, &bad_state, NULL, NULL) == DSD_PARSE_ERROR,
          "CLI --tetra-key refuses a short key");
    freeState(&bad_state);
    freeState(&state);

    /* Both new forms through the command line, the = spelling too. */
    char named[] = "1,2,00000000000000000005,tea2";
    char reduced[] = "--tetra-key=1,2,DEADBEEF,la=3,cc=4,carrier=5";
    char* new_argv[] = {name, flag, named, reduced, NULL};
    seed(&opts, &state);
    check(dsd_parse_args(4, new_argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE
              && state.tetra_air_keys[0].alg == 2U && state.tetra_air_keys[1].kind == DSD_TETRA_AIR_KEY_TEA1_REDUCED
              && state.tetra_air_keys[1].key[0] == 0xDE && state.tetra_air_keys[1].carrier == 5,
          "CLI --tetra-key takes a named algorithm and a reduced key");
    freeState(&state);
}

/* What --tetra-key prints for each argument it takes, refused or loaded: @p argc arguments after the program name. */
static int
tetra_key_cli_output(char** args, int argc, char* out, size_t out_size) {
    static dsd_state state;
    static dsd_opts opts;
    char name[] = "test";
    char* argv[8] = {name};
    for (int i = 0; i < argc && i < 7; i++) {
        argv[i + 1] = args[i];
    }
    seed(&opts, &state);
    dsd_test_capture_stderr cap;
    if (dsd_test_capture_stderr_begin(&cap, "tetra_key_cli") != 0) {
        freeState(&state);
        return -1;
    }
    const int rc = dsd_parse_args(argc + 1, argv, &opts, &state, NULL, NULL);
    (void)dsd_test_capture_stderr_end(&cap);
    freeState(&state);
    return dsd_test_capture_stderr_read(&cap, out, out_size) == 0 ? rc : -1;
}

/* A refused --tetra-key names the grammar and its limits, never the key it was given; a loaded one prints nothing of
 * it either. */
static void
test_tetra_air_key_output(void) {
    static char out[4096];
    char flag[] = "--tetra-key";
    char bad_reduced[] = "1,2,DEADBEEF,la=3,cc=64,carrier=5";
    char* refused[] = {flag, bad_reduced};
    check(tetra_key_cli_output(refused, 2, out, sizeof out) == DSD_PARSE_ERROR, "a bad reduced key is refused");
    check(strstr(out, "--tetra-key requires") != NULL && strstr(out, "la=") != NULL && strstr(out, "16 keys") != NULL,
          "the refusal names both forms and the table size");
    check(strstr(out, "DEADBEEF") == NULL && strstr(out, "deadbeef") == NULL, "the refusal never echoes the key");

    char bad_ck[] = "1,2,0123456789ABCDEF0123,tea4";
    char* refused_ck[] = {flag, bad_ck};
    check(tetra_key_cli_output(refused_ck, 2, out, sizeof out) == DSD_PARSE_ERROR && strstr(out, "0123456789") == NULL,
          "a refused 80-bit key is not echoed");

    char good_reduced[] = "1,2,CAFEF00D,la=3,cc=4,carrier=5";
    char good_ck[] = "1,2,0123456789ABCDEF0123,tea2";
    char* loaded[] = {flag, good_reduced, flag, good_ck};
    check(tetra_key_cli_output(loaded, 4, out, sizeof out) == DSD_PARSE_CONTINUE, "both forms load");
    check(strstr(out, "CAFEF00D") == NULL && strstr(out, "cafef00d") == NULL && strstr(out, "0123456789") == NULL,
          "loading prints no key");
}

static void
test_retained_decryption_request(void) {
    static dsd_state state;
    static dsd_opts opts;
    seed(&opts, &state);
    state.K = 19;
    state.M = 0x24;
    state.keyloader = 1;
    DSD_SNPRINTF(state.key_profile_ref, sizeof(state.key_profile_ref), "%s", "existing-profile");
    dsd_app_decryption_payload p = {0};
    p.request_id = 1001;
    p.session_generation = dsd_app_command_session_generation();
    check(p.session_generation != 0, "live session has a command generation");
    check(dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof(p)) > 0, "unchanged request queued");
    dsd_app_drain_cmds(&opts, &state);
    dsd_app_decryption_result result = {0};
    check(dsd_app_decryption_result_get(&result) && result.request_id == p.request_id
              && result.status == DSD_APP_KEY_APPLIED,
          "retained no-op result");
    check(state.K == 19 && state.M == 0x24 && state.keyloader == 1
              && strcmp(state.key_profile_ref, "existing-profile") == 0,
          "unchanged request preserves keys, loader and force");
    p.request_id++;
    p.fields = DSD_APP_DECRYPTION_MATERIAL | DSD_APP_DECRYPTION_MAP | DSD_APP_DECRYPTION_FORCE;
    p.source = DSD_APP_KEY_SOURCE_DIRECT_OVERLAY;
    p.key_type = DSD_APP_KEY_TYPE_BASIC;
    p.force = 0x21;
    DSD_SNPRINTF(p.value, sizeof(p.value), "%s", "73");
    DSD_SNPRINTF(p.map_file, sizeof(p.map_file), "%s", "dsd-neo-missing-map-fixture.csv");
    check(dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof(p)) > 0, "invalid map request queued");
    dsd_app_drain_cmds(&opts, &state);
    dsd_app_decryption_result_get(&result);
    check(result.status == DSD_APP_KEY_FILE_ERROR && state.K == 19 && state.M == 0x24 && state.keyloader == 1,
          "failed preparation preserves all prior fields");
    p.request_id++;
    p.fields &= ~DSD_APP_DECRYPTION_MAP;
    p.map_file[0] = '\0';
    check(dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof(p)) > 0, "valid defaults request queued");
    dsd_app_drain_cmds(&opts, &state);
    dsd_app_decryption_result_get(&result);
    check(result.status == DSD_APP_KEY_APPLIED && state.K == 73 && state.M == 0x21 && state.keyloader == 0,
          "decoder-owned completion applies changed fields");
    dsd_app_command_session_set_open(0);
    dsd_app_command_session_set_open(1);
    p.request_id++;
    DSD_SNPRINTF(p.value, sizeof(p.value), "%s", "92");
    check(dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof(p)) > 0,
          "old session request admitted for validation");
    dsd_app_drain_cmds(&opts, &state);
    dsd_app_decryption_result_get(&result);
    check(result.status == DSD_APP_KEY_STALE && state.K == 73, "old session cannot mutate the replacement session");
    p.session_generation = dsd_app_command_session_generation();
    p.request_id++;
    check(dsd_app_command_submit(DSD_APP_CMD_DECRYPTION_APPLY, &p, sizeof(p)) > 0, "pending request queued");
    dsd_app_command_session_set_open(0);
    dsd_app_decryption_result_get(&result);
    check(result.request_id == p.request_id && result.status == DSD_APP_KEY_CANCELLED && state.K == 73,
          "session close retains cancellation without mutation");
    DSD_SECURE_ZERO(&p, sizeof(p));
    freeState(&state);
}

int
main(void) {
    bootstrap_snapshot_secrets();
    zero_key_voice();
    presence_follows_material();
    test_tetra_air_keys();
    test_tetra_air_key_output();
    static dsd_state state;
    static dsd_opts opts;
    const int types[] = {0, 1, 1, 1, 2, 3};
    const size_t widths[] = {1, 10, 32, 64, 2, 5};
    const char* flags[] = {"-b", "-H", "-H", "-H", "-1", "-R"};
    for (size_t i = 0; i < 6; ++i) {
        char value[72] = {0};
        if (types[i] == 1) {
            DSD_MEMSET(value, '1', widths[i]);
        } else {
            DSD_SNPRINTF(value, sizeof value, "%s", types[i] == 0 ? "0" : types[i] == 2 ? "AB" : "32767");
        }
        for (int path = 0; path < 3; ++path) {
            seed(&opts, &state);
            if (path == 0) {
                check(dsd_key_apply_direct(&state, (dsd_key_type)types[i], value, DSD_KEY_APPLY_OVERLAY)
                          == DSD_KEY_DIRECT_OK,
                      "helper accepts type");
                dsd_key_apply_mute_policy(&opts, &state);
            } else if (path == 1) {
                char name[] = "test";
                char flag[3];
                DSD_SNPRINTF(flag, sizeof flag, "%s", flags[i]);
                char* argv[] = {name, flag, value, NULL};
                check(dsd_parse_args(3, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "CLI accepts type");
            } else {
                dsd_app_key_direct_payload payload = {0};
                payload.key_type = types[i];
                DSD_SNPRINTF(payload.value, sizeof payload.value, "%s", value);
                check(dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload)
                          == DSD_APP_COMMAND_SUBMIT_QUEUED,
                      "queue accepts type");
                DSD_SECURE_ZERO(&payload, sizeof payload);
                check(dsd_app_drain_cmds(&opts, &state) == 1, "queue drains type");
            }
            expected(&opts, &state, types[i], widths[i]);
            check(dsd_key_apply_direct(&state, (dsd_key_type)types[i], value, DSD_KEY_APPLY_REPLACE)
                      == DSD_KEY_DIRECT_OK,
                  "replace accepts each type");
            check(state.rkey_array[7] == 0 && state.rkey_array_loaded[7] == 0, "each replacement clears keyring");
            check(state.K == 0 && (types[i] == 2 || types[i] == 3 || state.R == 0),
                  "replacement clears unrelated scalars");
            freeState(&state);
        }
        DSD_SECURE_ZERO(value, sizeof value);
    }
    // Every width accepts zero through startup and live entry, with the same mute rule.
    for (size_t i = 0; i < 6; ++i) {
        for (int path = 0; path < 2; ++path) {
            seed(&opts, &state);
            dsd_app_key_direct_payload zero = {0};
            zero.key_type = types[i];
            DSD_MEMSET(zero.value, '0', types[i] == 1 ? widths[i] : 1);
            if (path == 0) {
                char name[] = "test";
                char flag[3];
                DSD_SNPRINTF(flag, sizeof flag, "%s", flags[i]);
                char* argv[] = {name, flag, zero.value, NULL};
                check(dsd_parse_args(3, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "zero CLI accepted");
            } else {
                dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &zero, sizeof zero);
                dsd_app_drain_cmds(&opts, &state);
            }
            check(opts.dmr_mute_encL == 1 && opts.dmr_mute_encR == 1, "all zero types share mute policy");
            check((types[i] != 0 || state.K == 0) && (types[i] != 1 || state.K1 == 0) && (types[i] < 2 || state.R == 0),
                  "zero scalar installed");
            DSD_SECURE_ZERO(&zero, sizeof zero);
            freeState(&state);
        }
    }
    seed(&opts, &state);
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_BASIC, "256", DSD_KEY_APPLY_REPLACE) == DSD_KEY_DIRECT_INVALID_DEC,
          "bad decimal reason");
    check(state.K == 17 && state.rkey_array[7] == 29, "failure is atomic");
    check(dsd_key_apply_direct(&state, DSD_KEY_TYPE_BASIC, "0", DSD_KEY_APPLY_REPLACE) == DSD_KEY_DIRECT_OK,
          "replace accepted");
    check(state.K == 0 && state.R == 0 && state.RR == 0 && state.rkey_array_loaded[7] == 0 && state.rkey_array[7] == 0,
          "replace clears other keys");
    dsd_key_set row = {0};
    check(dsd_key_set_load_direct(&row, NULL, "51") == DSD_KEY_DIRECT_OK, "row prepared");
    row.keyloader = 1;
    check(dsd_scan_keys_enter(&state, &row) == 1, "keyed row entered");
    dsd_app_key_direct_payload payload = {DSD_APP_KEY_TYPE_BASIC, "73"};
    dsd_app_command_submit(DSD_APP_CMD_KEY_DIRECT_SET, &payload, sizeof payload);
    DSD_SECURE_ZERO(&payload, sizeof payload);
    dsd_app_drain_cmds(&opts, &state);
    check(state.K == 51 && state.scan_keys_baseline.scalars.K == 73, "live change updates baseline under row");
    check(state.keyloader == 1, "live scalar edit preserves active keyloader");
    dsd_scan_keys_leave(&state);
    check(state.K == 73, "unkeyed hop restores updated baseline");
    dsd_scan_keys_enter(&state, &row);
    check(state.K == 51, "keyed hop restores row");
    dsd_scan_keys_leave(&state);
    check(state.K == 73, "second unkeyed hop retains change");
    dsd_key_set_free(&row);
    freeState(&state);
    for (int mode = 0; mode < 3; ++mode) {
        for (int path = 0; path < 3; ++path) {
            seed(&opts, &state);
            if (path == 0) {
                check(dsd_key_apply_force(&state, mode) == DSD_KEY_DIRECT_OK, "force helper");
            } else if (path == 1) {
                char name[] = "test";
                char flag[] = "-4";
                if (mode == 2) {
                    flag[1] = '0';
                }
                char* argv[] = {name, mode ? flag : NULL, NULL};
                check(dsd_parse_args(mode ? 2 : 1, argv, &opts, &state, NULL, NULL) == DSD_PARSE_CONTINUE, "force CLI");
            } else {
                dsd_app_command_set_i32(DSD_APP_CMD_FORCE_KEY_SET, mode);
                dsd_app_drain_cmds(&opts, &state);
            }
            check(state.M == (mode == 2 ? 0x21 : mode), "force expected state");
            freeState(&state);
        }
    }
    test_retained_decryption_request();
    test_m17_key_widths();
    return failures ? 1 : 0;
}
