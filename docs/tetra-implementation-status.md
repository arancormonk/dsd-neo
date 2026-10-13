# TETRA six-stage implementation status

This checklist preserves the six-stage implementation scope. Passing synthetic
tests is evidence for the cases they exercise, not completion of a stage.

| Stage | Existing evidence | Remaining acceptance evidence |
| --- | --- | --- |
| 1. Real sample baseline | **Accepted:** unmodified Apache-2.0 1.5-second SCBS CS16 capture, source commit/blob/SHA-256 provenance, replay metadata, deterministic 54 kHz derivative, and `DECODE_IQ_TETRA_REAL_AIR_SCBS` asserting independently reported MCC 250/MNC 13/CC `0x2C` | Add longer and geographically diverse captures as coverage improvements; they are not required for this baseline gate |
| 2. Physical layer hardening | **Accepted:** soft-symbol history capture, fixed BSCH and SYSINFO type-5 air-chain vectors, direct ETSI scrambler recurrence, captured non-zero-colour BNCH CRC vector, FEC/interleaver tests, deterministic CFO/noise, two-ray and clock-error replay, plus sustained normal- and negative-polarity recovery from a two-second marginal off-air capture reported at about 10 dB in-channel SNR | Naturally inverted captures from other receiver chains remain a coverage improvement rather than a gate |
| 3. IQ automation | **Accepted:** ten reproducible synthetic fixtures plus four hash-verified real-air fixture files, metadata verifier, 33 IQ decode/reject tests, fixed 300 Hz CFO sensitivity bracket (protected control accepted at 6 dB SNR and rejected at 4 dB), and an off-air minimum-yield gate calibrated to 15 matches against 26 observed | Hardware sensitivity measurements may refine thresholds but are not required for deterministic replay acceptance |
| 4. Control plane | MAC/MLE/MM/CMCE focused tests; four frozen hexadecimal security vectors independent of the test message builder for tables A.4/A.26/A.30a/A.31; a published 64-bit SDS-TRANSFER user field plus a separate Apache-2.0 272-bit complete MAC PDU, with the latter exercising MAC length bounding and BL-UDATA LLC decapsulation through MLE/CMCE and recovering message reference 32 plus text `Ahoj`; complete and fragmented basic-link LLC routing, including BL-ACK data and Annex C 32-bit FCS validation before layer-3 publication; Annex E O/P/M validation across the implemented downlink CMCE PDU family; official Annex E.2/E.3 D-FACILITY, E.8 D-CONNECT with SS-AL Facility, E.9-E.11 MM group/LU Accept, E.16-E.18 MLE network broadcast, and E.19-E.24 MLE cell-reselection response vectors; complete extended-PDU table 18.3 D-NWRK-BROADCAST-DA snapshots; complete table 18.4 D-NWRK-BROADCAST EXTENSION channel-class and irregular-channel lists; extended-PDU table 18.5 D-NWRK-BROADCAST REMOVE CA/DA/serving-cell records; table 18.10 D-CHANNEL RESPONSE; EN 300 392-7 tables A.9/A.12/A.15/A.16/A.19/A.20/A.23/A.24/A.27d/A.27f/A.27h/A.30/A.30a D-OTAR CCK/SCK/GCK/GSKO transfer, key association, DM-SCK activation, key deletion, key-status demand, NEWCELL and CMG GTSI, tables A.1-A.4 D-AUTHENTICATION, table A.26 D-CK CHANGE, and tables A.31/A.32 D-DISABLE/D-ENABLE validation; full table 16.54 group address forms; complete fixed-width identity optionals for table 14.15 D-SETUP and tables 14.18/14.19 floor control; SDS ACK/REPORT/TRANSFER headers, short reports, bounded Latin-1/UTF-16BE/GSM text, validated storage/forward addresses, and bounded concatenated SDS reassembly | Obtain a raw off-air SDS capture with reproducible provenance and add independent vectors when published captures expose further security PDUs |
| 5. Trunk tracking | State-machine, hangtime, allocation, call lifecycle and TCH slot-gate tests; v2 IQ retune timeline replay; an automated field-evidence verifier requiring exact aligned data, zero drops, CC-to-VC-to-CC events, source provenance, and ordered realtime BSCH/allocation/TCH/BSCH decode milestones | Run the verifier against a real hardware retune capture proving control-to-traffic acquisition and return under real timing conditions |
| 6. Voice and product UI | **Accepted:** persistent vocoder and failure tests; Qt Quick status integration; complete Qt 6.10.3 MSVC frontend build and 12/12 `UI_QT_*` tests; public GPL-3.0 “Hello Tetra” sample; all 60 recovered ACELP frames matched bit-for-bit against the ETSI reference decoder; independently decoded 8 kHz PCM reference with published expected phrase | Additional real calls and vocoder implementations remain coverage improvements rather than acceptance gates |

## Most recent validation

On 2026-09-20 an RTL2832U/R820T receiver was used against a live 862–865 MHz
TETRA network. A 29.13-second CU8 capture at 862.287822 MHz recorded zero IQ or
input-ring drops and continuously decoded BSCH identity MCC 86/MNC 28/CC
`0x19`, SCH-HD, and TCH/FS. The log contains 1,348 BSCH and 266 TCH/FS records.
The field run exposed that the former 48 kHz RTL baseband gives the 18 ksym/s
TETRA timing path a fractional 2.667 samples/symbol. RTL input/configuration now
accepts 72 kHz, providing an integer four samples/symbol and changing live
reception from occasional sync to sustained decoding. No SDS, SYSINFO, or
CHANNEL-ALLOCATION was observed, so the stage-4 raw SDS and stage-5 CC-to-VC-to-CC
hardware gates remain open. See [tetra-field-acceptance.md](tetra-field-acceptance.md).

The Windows Debug `ALL_BUILD` completed after the EN 300 392-7 D-OTAR
CCK/SCK/GCK/GSKO transfer, key-association, DM-SCK activation, key deletion,
key-status demand, NEWCELL and CMG GTSI implementation, the CMCE Annex E envelope migration, the
negative-polarity physical-layer fix, the Annex E.9 MM group-identity implementation,
the Annex E.16-E.18 MLE network-broadcast implementation, complete extended-PDU
table 18.3 DA snapshots, the complete table 18.4 network-broadcast extension
lists, extended-PDU table 18.5 removal records, and
the Annex E.19-E.24 cell-reselection
responses with the corrected outer O-bit before their nested
MM/CMCE SDUs. The current vcpkg manifest resolves the complete required Windows
dependency set, including Expat.
The replay input now converts little-endian CS16 IQ to normalized float samples;
this closes a format gap where metadata accepted CS16 but the replay worker
silently produced no samples. The preserved 50 kHz real-air capture is
deterministically interpolated to 54 kHz (three samples per 18 ksym/s symbol),
and the resulting full-chain replay repeatedly CRC-decodes BSCH identity MCC
250/MNC 13/CC `0x2C`. The receive FEC chain now applies descrambling before deinterleaving, reversing
the normative transmit order of convolutional coding, puncturing,
interleaving, then scrambling. The deterministic IQ generator follows that
same transmit order and its committed fixtures were regenerated; this avoids
using the previous self-consistent but non-air-interface ordering as physical
layer evidence. Separately fixed 120-bit BSCH and 216-bit SYSINFO type-5
vectors prevent the generator and decoder from silently validating the same
future ordering error. They cover both the BSCH seed and K120/a11 path and the
network-derived scrambling seed with the K216/a101 SCH-HD path.
An additional unmodified two-second 144 kHz CS16 control-channel capture was
recorded upstream during marginal-signal reacquisition at about 10 dB
in-channel SNR. Its full-chain replay recovered 26 clean identity-matching BSCH
bursts out of 30 synchronized bursts; the regression requires at least 15 to
avoid reducing this evidence to a single lucky decode.
A deterministic complex conjugate retains that recording's noise, fading,
timing error and ISI while reversing every differential phase transition. Its
negative-polarity replay also recovered 26 identity matches and requires at
least 15.
The combined TETRA suite passed 68/68: 35 TETRA-labelled protocol, audio,
fixture-reproducibility, and field-verifier tests plus 33 IQ decode/reject tests. It includes the D-SETUP, call-response, floor-control,
group-identity, network-broadcast, trunk hangtime, and reassembly cases, and the
IQ replay cases include five complex-conjugated negative-polarity variants. The trunk return integration passed `TETRA_TRUNK_SM`,
`ENGINE_NO_CARRIER_RESET`, and `ENGINE_GENERIC_TRUNK_RETURN_MATRIX`.
A separate Windows MSVC tree configured with `DSD_ENABLE_QT_UI=ON` and Qt
6.10.3 built the complete `dsd-neo_ui_qt` target. All 12 registered
`UI_QT_*` tests passed, including Qt Quick Test loading the real QML screens
through the offscreen software renderer. The public telive “Hello Tetra” sample
also closes the real speech gate: the decoder recovers all 60 ACELP frames
exactly against the ETSI reference output, including the previously missing
Class-0 bit 64, and the independently decoded 14,400-sample PCM reference is
preserved with fixed provenance and hashes. TCH/FS now uses the normative
24x18 speech interleaver, class-specific rate-1/3 puncturing, and 240 PCM
samples per 30 ms frame.
The MM security suite now also decodes frozen MSB-first hexadecimal vectors for
D-AUTHENTICATION RESULT, D-CK CHANGE DEMAND, D-OTAR CMG GTSI PROVIDE, and
D-DISABLE. These vectors do not use the test-side `pack_bits()` constructor,
so an identical offset error in the builder and parser cannot make them pass.
`TETRA_MLE_CMCE` likewise decodes a frozen 88-bit CMCE/MLE envelope around the
64-bit SDS user field published by the independent `tetra-multiframe-sds`
decoder. It recovers PID 130 SDS-TRANSFER message reference 32 and Latin-1 text
`Ahoj`. The upstream README says that real data was removed from its example,
so this strengthens interoperability evidence but does not satisfy the raw
off-air capture and provenance gate.
`TETRA_SDS_REFERENCE_MAC` additionally feeds the separate Apache-2.0
`smarek/kaitai-tetra-sds` 272-bit sample into the MAC parser. The regression
uses the length indicator to exclude trailing fill, removes its BL-UDATA LLC
header, and recovers the same SDS reference and text through the complete
MAC/LLC/MLE/CMCE path. The upstream repository publishes the bytes and decoded
structure but does not identify the sample as an off-air recording, so the
stage-4 field-capture gate remains open.

## Next implementation work

1. Obtain a raw off-air SDS capture with reproducible provenance and continue adding published
   independent security/control-plane vectors; all downlink D-OTAR PDUs with
   normative PDU tables in EN 300 392-7 V3.5.1 are implemented. Four frozen
   security vectors now break the local builder/parser feedback loop. The
   official ETS 300 394-5-3 abstract test suite was audited,
   but it targets the 1999 protocol edition and supplies RAND/key values through
   external PIXIT parameters, so it cannot serve as a fixed V3.5.1 bit-vector
   oracle.
2. Add geographically and receiver-diverse captures as coverage permits; the
   physical-layer and deterministic IQ automation gates are accepted.
3. Complete hardware tracking with a control-to-traffic-to-control retune
   capture under real receiver timing. The acceptance command and hash report
   are implemented in `tools/tetra/verify_field_capture.py`; the real capture,
   provenance file, and realtime replay log remain required. Voice and Qt
   acceptance are complete.

Stages 1 through 3 and stage 6 are accepted. Stages 4 and 5 retain the evidence
gates in the table.
See [testing.md](testing.md) for test details and fixture limitations.

## Tracking integration audit

`tetra_sm_tick()` is called from TETRA frame handling and refreshes its timer
while `tetra_call_active` is set. The generic `noCarrier()` path independently
decides whether to return using `last_vc_sync_time` and `trunk_is_tuned`.
Integration now treats all TETRA sync types as generic trunk traffic.
On an accepted, failed, or impossible generic CC return it closes the persistent
vocoder and clears TETRA call, floor-control, and traffic-allocation state without
issuing a second tune. Duplicate-grant suppression requires the shared tuned
flag, and tests cover failed same-frequency reacquisition followed by a successful
retry. TETRA-owned asynchronous VC and CC-return requests now retain their
correlated request ID: completion commits the staged state and starts hangtime
from the hardware completion timestamp, while failure restores the prior state,
VC identity, generic frequency caches, activity timestamps, and shared tuned
flag and retires the failed frame gate. An
overlapping grant or release cannot overtake the in-flight transition. Disabling
trunking after a successful cleanup publishes `IDLE`, consistent with the state
definition. The engine polls TETRA completion before applying its frame gate and
again in the unsynchronized loop, preventing a failed tune from blocking the
protocol tick that would otherwise be needed to retire that same gate. Accepted,
slot-gated TCH data refreshes `last_vc_sync_time` even when the
vocoder is unavailable; rejected slots and short frames do not. The generic
trunk-return matrix now exercises positive and negative TETRA sync families. The
state machine mirrors IDLE/ON_CC/TUNED into `dsd_state`, allowing Qt snapshots
to display the live trunk state without reading the decoder-thread singleton.
Real receiver timing remains the stage-5 acceptance requirement.

## SDS-TL storage/forward audit

The parser now validates and publishes the conditional storage/forward block
from SDS-TRANSFER and SDS-REPORT. It records the validity period and supports
SNA, SSI, TSI, external subscriber number, and the explicit no-address value.
External numbers accept only the digit alphabet defined by ETSI table 14.59,
and an odd digit count requires a zero dummy digit. Reserved address types,
reserved digit values, non-zero padding, and truncated fields reject the whole
replacement without modifying the last complete SDS state. The compact TETRA
channel summary includes the decoded address and validity period.

## Concatenated SDS audit

Protocol identifiers 0x0C and 0x8C now use a bounded four-context reassembler.
Each context accepts the standardized range of 2 to 255 parts and the full
11-bit per-PDU SDS payload bound. The key contains the calling-party SSI, the
short or extended 12-bit concatenation reference, and the with/without-SDS-TL
form. Parts may arrive in any order. Identical repeats are suppressed; a repeat
whose content or length differs discards the conflicting context. A five-minute
idle timeout and oldest-context replacement bound lifetime and memory use.
The first part supplies the original protocol identifier, and text reaches
`dsd_state` only after all parts are present and the reassembled coding payload
passes the same GSM 7-bit, Latin-1, or UTF-16BE validation as an unfragmented
message. The compact channel summary exposes reference and receive progress.
The current CMCE interface does not expose a distinct recipient identity, so the
cache key cannot include the recipient component named by the SDS specification;
independent off-air vectors remain necessary to validate interoperability.

## D-SETUP optional-field audit

The CMCE parser now retains all fixed-width identity fields from ETSI EN
300 392-2 table 14.15: notification indicator, 24-bit temporary address,
calling-party type, calling SSI, and the 24-bit calling-party extension used by
CPTI value 2. Each value has an explicit validity flag. A complete later setup
that omits an element clears its old validity and calling identity, while a
truncated optional chain still leaves the last complete call state unchanged.
The table defines the remainder as type-3 elements owned by their respective
call-control or supplementary-service protocols. Unknown elements are skipped
only after their identifier and 11-bit length are complete and in bounds.

## Floor-control identity audit

The fixed-width optional chains from tables 14.18 and 14.19 are now decoded for
both D-TX-GRANTED and D-TX-INTERRUPT. State records notification validity,
transmitting-party type, SSI, and the CPTI/TPTI value 2 extension separately.
A complete message without those elements clears prior identity metadata; a
truncated conditional SSI or extension publishes nothing. Matching call release
and external control-channel return clear the floor identity and event validity
flags. The compact channel summary displays the current granted transmitter as
`PTT` and the transmitter attached to another floor event as `TXSRC`.
