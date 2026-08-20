# DMR TG→Key-ID Map: Deferred Review Findings

**Date:** 2026-08-19
**Branch:** `feat/dmr-tg-key-map`
**Status:** design approved, ready for implementation planning

## Problem

A `max`-effort code review of the `--dmr-tg-key-csv` feature produced fifteen findings. Nine were
fixed in the working tree. Six were deferred because each either changes a cross-module interface,
reverses what looked like deliberate intent, or has no structural fix. This design covers those six.

Two of the six do not survive verification as the review stated them, and are re-scoped here:

- The review claimed the `R`→`RR` change "removes recovery for every key source that writes `R`
  without `RR`". Every DMR key source writes both: `-1` (`src/runtime/cli/args.c:1585`) and
  `DSD_APP_CMD_KEY_RC4DES_SET` (`src/app_control/app_command_queue.c:380`). The only `R`-only
  writers are `-R` (`args.c:2055`) and `DSD_APP_CMD_KEY_SCRAMBLER_SET` (`:371`), both documented as
  NXDN/dPMR **scrambler** keys. The slot fix stands; only its test coverage regressed.
- The review claimed the forced-ALGID keystream guard "would need a scratch keystream buffer". It
  does not. The correct semantic on guard failure is "we have no keystream", not "preserve the old
  one".

## Root cause of the central defect

`keyring_dmr_effective_kid()` gates a map row on `keyring_kid_material(state, kid, NULL, NULL)`,
which answers *"does this key id have any bytes at all?"* That question is wrong in two ways.

**ALG-blind.** "Any material" passes even when it is the wrong kind. A talkgroup running ALG `0x21`
(RC4, which needs a scalar) mapped to a key id holding only an AES quartet resolves as mapped.
`keyring_activate_slot_with_kid()` then sets `R = 0`, `dsd_dmr_voice_kid_can_decrypt()` returns 0,
`dmr_flco_slot_can_decrypt()` reports "cannot decrypt", and `dsd_enc_lockout_note()` arms the
session-permanent ledger — forcing a `P_CLEAR` that drops the channel for a call the **signaled**
key would have decrypted. `keyring_dmr_tg_map_note_skipped()` never prints, because from the
resolver's view the row applied.

**Index-aliased.** AES segments live at `kid + {0x000, 0x101, 0x201, 0x301}` in the same flat
`rkey_array`, so segment *N* of key *K* is the same cell as the scalar of key *K + offset*. A key id
with nothing imported reports `aes_loaded` whenever an unrelated key happens to sit at
`kid + 0x101`. The aliasing itself is inherent — `keyring_activate_slot_with_kid()` reads the same
cells, and `keyring.h` documents the mirroring as deliberate. What is fixable is requiring *N* cells
instead of one, so an accidental match needs *N* colliding unrelated keys.

## Design

### 1. A shared "material need" vocabulary

The enum gets its own minimal header, `include/dsd-neo/core/key_material.h` — it is shared
vocabulary, not keyring-specific, and `audio.h` currently pulls only forward-declaration
headers. Routing it through `keyring.h` would drag `call_state.h` into every translation unit
that includes `audio.h`.

```c
typedef enum {
    DSD_KEY_NEED_NONE = 0,   /* alg consumes no keyring material -- a map row cannot help */
    DSD_KEY_NEED_SCALAR,     /* rkey_array[kid] non-zero */
    DSD_KEY_NEED_AES_2,      /* first 2 segments non-zero (DMR AES-128, P25 AES-128) */
    DSD_KEY_NEED_AES_3,      /* first 3 segments non-zero (P25 TDEA) */
    DSD_KEY_NEED_AES_4,      /* all 4 segments non-zero (DMR/P25 AES-256) */
    DSD_KEY_NEED_QUARTET,    /* Kirisun 0x36/0x37 */
} dsd_key_material_need;
```

`keyring.h` includes it and declares the predicate:

```c
int keyring_kid_satisfies_need(const dsd_state* state, int key_id, dsd_key_material_need need);
```

`AES_4` and `QUARTET` are not redundant. `AES_4` asks only that all four segment cells be
non-zero; `QUARTET` additionally requires `keyring_aes_segment_count()` to report four, which
is what `keyring_kid_kirisun_complete()` already predicts about activation. Four non-zero
cells of which only two are flagged `rkey_array_loaded` satisfies the first and not the second.

`DSD_KEY_NEED_NONE` is not a special case in the resolver: `keyring_kid_satisfies_need()` returns 0
for it, so an ALG that cannot be classified simply never maps and the resolver keeps its single-gate
shape:

```c
if (!keyring_kid_satisfies_need(state, kid, need)) {
    return signaled_kid;   /* + skipped notice on the announcing path */
}
```

That failure direction is the safe one: the signaled key id is preserved and the operator is told.

**The gate tests non-zero segment cells, not merely `rkey_array_loaded`.** Reusing
`keyring_aes_segments_complete()` would reintroduce the same defect one layer down — it accepts a
cell that is flagged loaded but zero-valued, which activates as `A_i == 0` and yields
`aes_key_loaded == 0`. `keyring_kid_kirisun_complete()`'s existing comment records exactly this
distinction. Classification must predict activation.

### 2. Table ownership

| Module | Owns | Rationale |
|---|---|---|
| `src/core/vocoder/keyring.c` | `keyring_kid_satisfies_need()` | Built only from the file's existing statics, so `keyring.c` keeps zero project-level undefined symbols and `tests/ui/test_ui_menu_services.c` still links from `menu_services.c` + `keyring.c` alone. |
| `src/core/audio/dsd_audio_gate.c` | `dsd_dmr_alg_key_need(int algid)` | The **single** voice ALG table. `dsd_dmr_voice_alg_can_decrypt()` is rewritten to derive from it rather than carry a second copy — the duplication class this review repeatedly found. |
| `src/protocol/dmr/dmr_block_crypto.c` | a static `dmr_block_alg_key_need(int alg)` | The data path's ALG space is `0/1/2/4/5/7`, a different numbering. Translating it locally avoids a cross-numbering normalization in which data-`2` (DES) and voice-`0x02` (Hytera Enhanced) already collide. |

`dsd_dmr_alg_key_need()` is declared in `include/dsd-neo/core/audio.h`, which includes
`key_material.h` for the enum. Every header involved is `core`, so no layering rule is crossed.

**Voice ALG table** (from the existing `kRKeyAlgs` / `kAesLoadedAlgs` lists plus the Kirisun case):

| ALGID | Need |
|---|---|
| `0x02` Hytera Enhanced, `0x21` DMR RC4, `0x22` DMR DES, `0x81` P25 DES, `0x9F` P25 DES-XL, `0xAA` P25 RC4 | `SCALAR` |
| `0x24` DMR AES-128, `0x89` P25 AES-128 | `AES_2` |
| `0x83` P25 TDEA | `AES_3` |
| `0x25` DMR AES-256, `0x84` P25 AES-256 | `AES_4` |
| `0x36`, `0x37` Kirisun | `QUARTET` |
| anything else | `NONE` |

The `AES_3` row matches the segment counts `src/protocol/p25/p25_crypto.c:305` already uses.

**Data ALG table**, derived from what `dmr_block_crypto_decrypt_payload()` actually consumes:

| `ctx->alg` | Need | Why |
|---|---|---|
| `1` RC4, `2` DES | `SCALAR` | Both branches require `ctx->rkey != 0` |
| `4` AES-128 | `AES_2` | Reads `parts[0..1]` |
| `5` AES-256 | `AES_4` | Reads `parts[0..3]` |
| `0` Moto BP | `NONE` | Uses `state->K` and the `BPK[]` table, never the keyring |
| `7` VTX STD, anything else | `NONE` | No decrypt branch exists; `dmr_block_crypto_apply_bp()` rejects `alg != 0` |

**Deriving `dsd_dmr_voice_alg_can_decrypt()` is behavior-preserving:** `SCALAR` → `r_key != 0`;
`AES_2`/`AES_3`/`AES_4` → `aes_loaded == 1`; `QUARTET` and `NONE` → 0. This keeps every case
`tests/core/test_core_dmr_voice_alg_gate.c` already pins, including `kirisun-generic-needs-slot`
(`0x36` → 0, because only the slot/kid variants can answer that family) and `vertex-unknown`
(`0x07` → 0).

### 3. Threading

Three resolver entry points gain a `dsd_key_material_need` parameter:
`keyring_dmr_effective_kid()`, `keyring_dmr_kid_for_call()`, `keyring_dmr_slot_kid_for_call()`.

| Call site | ALG source | Need computed by |
|---|---|---|
| `src/core/vocoder/dsd_mbe.c:353` (voice) | `payload_algid` / `payload_algidR`, already in scope | `dsd_dmr_alg_key_need()` |
| `src/protocol/dmr/dmr_flco.c:644` (LC classify) | LC `algid` | `dsd_dmr_alg_key_need()` |
| `src/protocol/dmr/dmr_pi.c:247` (PI classify) | `payload_algid` / `payload_algidR` | `dsd_dmr_alg_key_need()` |
| `src/protocol/dmr/dmr_block_crypto.c:127` (data) | `ctx->alg` | `dmr_block_alg_key_need()` |
| `src/core/file/dsd_file.c:1606`, `:1888`, `:1977` (sdrtrunk replay) | `ctx->alg_id` | `dsd_dmr_alg_key_need()` |

The `<= 0xFF` width guards added by the earlier review fixes stay exactly as they are at every site.

### 4. Notice wording

`keyring_dmr_tg_map_note_skipped()` currently reads "has no imported key; using signaled Key ID".
With an ALG-aware gate the common case it catches becomes "row present, but its key cannot serve
this ALG", so the message names the ALG.

### 5. Satellite fixes

**`K1..K4` precedence becomes an invariant (review finding 9).** With the gate in place, a row can
only apply for data alg `4` when cells `kid+0x000` and `kid+0x101` are non-zero, and for alg `5`
when all four are — exactly the cells `dmr_block_load_aes_key()` reads. Its
`all four zero → substitute state->K1..K4` fallback is therefore unreachable whenever `ctx->mapped`
is set. That is the intended precedence (an explicit per-talkgroup row beats a global manual key)
and it can no longer silently discard a working `-2`/`-H` key, because the row cannot apply without
real AES material behind it. **No behavior change** — a comment stating the invariant, plus tests
pinning both directions.

**Slot-correct scalar fallback keeps its fix, regains its coverage (finding 10).**
`slot_rkey = (id_slot == 0) ? state->R : state->RR` stands. The slot-0 assertion that the original
change converted rather than added is restored, so both slots are pinned. A comment records why the
review's concern does not bite (see "Problem" above).

**Split the notice latch (finding 11).** `dmr_tg_key_note_epoch[2]` in `dsd_state` gains a sibling
`dmr_tg_key_skip_epoch[2]`, and `keyring_dmr_tg_map_note_should_print()` takes which latch to stamp.
Today one latch is shared by two notices reporting opposite outcomes, and it is stamped as a side
effect of the query — so whichever situation holds on an epoch's first mappable frame permanently
silences the other. After the split, each notice prints at most once per epoch and neither can
silence the other: a call that starts with the mapped key unimported prints the skipped notice, and
if the operator imports it mid-call the applied notice prints once too. Bounded, with no flapping
analysis needed (unlike a single latch keyed on an (epoch, kind) pair). Both arrays are cleared by
`keyring_dmr_tg_map_reset()`. This grows `dsd_state` by 16 bytes rather than using `state_ext`,
consistent with the field it sits beside.

**Document the stale-ACTIVE snapshot bound (finding 12).** `keyring_dmr_slot_kid_for_call()`
resolves against whatever epoch is ACTIVE on the slot. When a transmission ends without a decodable
terminator the epoch stays ACTIVE, so the next transmission's first voice frames resolve against the
*previous* talkgroup. No structural fix exists: until the new voice LC opens an epoch, nothing can
signal staleness, and every consumer of the canonical snapshot shares the exposure. What is missing
is the reasoning. `keyring_dmr_tg_map_call_is_mappable()` already explains why an **ENDED** epoch's
stale talkgroup is rejected; it gains the companion note that a stale **ACTIVE** epoch is accepted,
that this can key the first voice frames of the next transmission, and that it self-heals on the
first frame after the new LC because `keyring_activate_slot_with_kid()` runs every frame.
`src/protocol/dmr/dmr_pi.c:243`, whose comment reasons only about the "no snapshot yet" case, gets
the same note. A test pins the self-heal.

**Forced-ALGID keystream guard (finding 13).** In `sdrtrunk_json_apply_forced_algid()`:

```c
if (ctx->alg_id == 0x21) {
    if (state->R != 0 && state->payload_mi != 0) {
        /* build; ctx->ks_key_id = effective_kid; ctx->ks_built = 1; */
    } else {
        ctx->ks_available = 0;   /* no key or no IV yet -- we have no keystream */
    }
}
```

The `alg_id == 0x21` scope is load-bearing: forced AES (`0x22`–`0x25`) never builds in this branch
and its `ks_available` is owned by the MI handler, so an unscoped zeroing would break forced-AES
replay. Like everything else in this file, it self-corrects on the next token once the MI lands.
Section 1 already removes the map-induced trigger (an RC4 row can no longer resolve to a key id with
an empty scalar, so activation cannot zero `R`); this covers the pre-existing "no MI yet" case that
the map merely made reachable more often.

## Testing

Crypto changes fall under the "must add or update tests" policy in `docs/testing.md`.

### Demonstrated red before the fix

1. **Headline.** Talkgroup mapped to a key id holding only an AES quartet, call running ALG `0x21`.
   Before: row applies, `R` activates to 0, `dmr_flco_slot_can_decrypt()` reports "cannot decrypt",
   the session-permanent lockout arms and forces a `P_CLEAR`. After: row does not apply, signaled
   key id preserved, call stays decryptable, lockout never arms, skipped notice names the ALG.
2. **Alias.** Key id `0x0C` with nothing imported but an unrelated scalar at `0x10D`. Before:
   `keyring_kid_material(0x0C)` reports material and the row applies. After:
   `keyring_kid_satisfies_need(0x0C, DSD_KEY_NEED_AES_2)` is false.
3. **Muted notice.** Call opens on a mapped talkgroup whose key is not imported → skipped notice
   prints. Key imported mid-epoch → applied notice must also print.

### Coverage by target

| CTest target | Adds |
|---|---|
| `CORE_DMR_TG_KEY_MAP` | `keyring_kid_satisfies_need()` table: each need × satisfied / unsatisfied / alias-only. Red case 2. Red case 3 via the existing `capture_notice_hits()` harness. Both latches cleared by `keyring_dmr_tg_map_reset()`. |
| `CORE_DMR_VOICE_ALG_GATE` | `dsd_dmr_alg_key_need()` for every ALGID in both lists plus unknowns → `NONE`. **Existing assertions are not edited** — they are the regression guard proving the `dsd_dmr_voice_alg_can_decrypt()` rewrite is behavior-preserving. |
| `DMR_BLOCK_CRYPTO_KEY_MAP` | Data need mapping: alg `4` row backed only by a scalar → not applied; alg `1` row backed only by AES segments → not applied; algs `0`/`7` → never applied. Finding 9 both directions. Finding 10 slot-0 **and** slot-1 fallback. |
| `DMR_FLCO_PRIVACY_MODES` | Red case 1 end-to-end through the LC path — classification **and** the lockout gate, since that pair diverging is the bug class the resolver exists to prevent. |
| `DMR_PI_KIRISUN` | The same need reaches the PI publish path; PI and LC agree on one call. |
| `CORE_MBE_FILE_IO` | Finding 13 both ways: forced `0x21` with the guard failing → `ks_available == 0`; forced `0x24` → `ks_available` untouched by that branch. Finding 12 self-heal: a stale-ACTIVE TG-100 snapshot keys the first frame, a TG-200 epoch opens, the next frame resolves against TG-200. |

### Invariants to re-verify rather than assume

- `UI_MENU_SERVICES` still links from `menu_services.c` + `keyring.c` alone. This constraint is what
  rules out moving resolution into `dsd_audio_gate.c`, so `keyring.c` must gain no project symbols.
- `CORE_AUDIO2_HELPERS` stubs `dsd_dmr_voice_slot_can_decrypt` at
  `tests/core/test_core_audio2_helpers.c:294`, i.e. it links `dsd_audio2.c` without
  `dsd_audio_gate.c`. Confirm `dsd_audio2.c` never needs `dsd_dmr_alg_key_need()`, or grow the stub
  list.
- `ARCH_RULES` and `CANONICAL_CALL_STATE_AUDIT` — `audio.h` including `keyring.h` is core→core, but
  run them rather than reason about them.
- Full `ctest --preset dev-debug`, `tools/format.sh`, `tools/cmake_format_check.sh`,
  `tools/semgrep.sh --strict`, `tools/clang_tidy.sh`, then `tools/preflight_ci.sh`.
  `tools/quality_preflight.sh` is warranted for a change of this breadth.

## Alternatives rejected

**Pass the raw `algid` and let `keyring.c` own one table.** Smaller call-site churn, but `keyring.c`
would duplicate ALG knowledge `dsd_audio_gate.c` already owns — the drift class this review kept
finding — and the data path would need normalization into voice numbering, where data-`2` and
voice-`0x02` already collide.

**Move resolution wholesale into `dsd_audio_gate.c`** as one
`dsd_dmr_resolve_key() -> {kid, mapped, material, can_decrypt}`. Conceptually cleanest and would
permanently retire the FLCO/PI duplication, but it breaks the `UI_MENU_SERVICES` link constraint,
does not fit the data path (which has no slot-material notion), and is by far the largest diff.

**Shape-only gate with no ALGID plumbing** — require a non-zero scalar or a complete non-zero
quartet. Cheap and contained, and it does fix the aliasing half, but an RC4 talkgroup mapped to an
AES-only key id still maps, still zeroes `R`, and still arms the lockout. That is the failure that
matters most, because the lockout is session-permanent and cannot self-heal.

## Known residuals

- AES-128's two required cells are `kid` and `kid + 0x101`, and `kid` doubles as the scalar slot, so
  an RC4 scalar at `kid` plus an unrelated key at `kid + 0x101` still satisfies `AES_2`. A two-cell
  collision rather than one, and identical to what `p25_crypto.c:305` already accepts. Eliminating
  it would need import-intent tracking in `rkey_array`, which is a separate change.
- The stale-ACTIVE snapshot window (finding 12) is documented and tested, not closed.
