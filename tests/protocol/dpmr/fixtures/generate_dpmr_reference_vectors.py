#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate the committed dPMR CCH reference vectors, and validate their provenance.

The vectors are produced by an independent Python model of the dPMR CCH *encode*
direction -- the direction dsd-neo never implements -- so the committed header is
external ground truth for the decoder rather than a snapshot of it.

The model follows ETSI TS 102 658 (dPMR):

  41 payload bits -> +CRC-7 (X^7 + X^3 + 1) -> 48 bits
                  -> 6 x shortened Hamming(12,8) -> 72 bits
                  -> 6x12 block interleave -> X^9 + X^5 + 1 scrambler -> 72 on-air bits

and is cross-checked against DSDcc, the independent decoder implementation known to
gate real dPMR traffic on this CRC. With --dsdcc pointing at a DSDcc checkout the
helper pins its commit and greps the exact source lines every constant below is
derived from.

This helper is intentionally opt-in. CI consumes only the committed header.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys

EXPECTED_DSDCC = "f27b32d2df131ae3a376fe72d3fb880ae1f9ede1"

# ETSI TS 102 658: CRC-7 over the 41 CCH payload bits, X^7 + X^3 + 1.
CRC7_POLYNOMIAL = 0x09

# Shortened Hamming(12,8) generator, systematic: 8 data bits then 4 parity bits.
# Transposed from DSDcc's Hamming_12_8::m_G (fec.cpp), whose parity columns are the
# rows of the parity-check matrix dsd-neo decodes with.
HAMMING_12_8_PARITY = (
    (0, 2, 4, 5),
    (0, 1, 3, 5, 6),
    (0, 1, 2, 4, 6, 7),
    (1, 3, 4, 7),
)

# The 4-bit syndrome each single-bit error produces, indexed by bit position.
# Syndrome bit 0 is the most significant, matching dsd-neo's (3 - is) shift.
HAMMING_12_8_SYNDROME = (14, 7, 10, 5, 11, 12, 6, 3, 8, 4, 2, 1)

# The scrambler's all-ones seed, and the 72-bit keystream the CCH is scrambled with.
SCRAMBLER_SEED = 0x1FF

# The committed golden in tests/protocol/dpmr/test_dpmr_scrambler.c, which this
# model must reproduce from the polynomial alone.
SCRAMBLER_GOLDEN = (
    "111111111000001111011111000101110011"
    "001000001001010011101101000111100111"
)
SCRAMBLER_GOLDEN_FINAL_STATE = 0x1B3

# dPMR frame sync 2, as dsd-neo's matcher spells it (include/dsd-neo/core/sync_patterns.h).
FS2_SYMBOLS = "113333131331"

# A channel code from the ETSI table, as dsd-neo transcribes it in
# src/protocol/dpmr/dpmr_data.c: colour code 12.
COLOUR_CODE_VALUE = 12
COLOUR_CODE_BITS = 0x5D5D55


def rev_parse(repo: pathlib.Path) -> str:
    return subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()


def require_commit(name: str, repo: pathlib.Path, expected: str) -> None:
    got = rev_parse(repo)
    if got != expected:
        raise SystemExit(f"{name}: got {got}, expected {expected}")


def require_text(path: pathlib.Path, needle: str) -> None:
    text = path.read_text(encoding="utf-8")
    if needle not in text:
        raise SystemExit(f"{path}: missing expected oracle text {needle!r}")


def validate_oracle(dsdcc: pathlib.Path) -> None:
    """Pin DSDcc and grep the source lines these vectors are derived from."""
    require_commit("dsdcc", dsdcc, EXPECTED_DSDCC)

    fec = dsdcc / "fec.cpp"
    dpmr = dsdcc / "dpmr.cpp"

    # Hamming(12,8): the systematic generator and the syndrome->position map.
    require_text(fec, "const unsigned char Hamming_12_8::m_G[12*8]")
    require_text(fec, "1, 0, 0, 0, 0, 0, 0, 0,   1, 1, 1, 0,")
    require_text(fec, "0, 0, 0, 0, 0, 0, 0, 1,   0, 0, 1, 1,")
    require_text(fec, "m_corr[0b1110] = 0;")
    require_text(fec, "m_corr[0b0001] = 11;")

    # CRC-7: the long-division taps for X^7 + X^3 + 1, over 41 bits.
    require_text(dpmr, "divide by X^7+X^3+1 (10001001)")
    require_text(dpmr, "m_bitWork[i+4] ^= 1; // X^3")
    require_text(dpmr, "m_bitWork[i+7] ^= 1; // 1")
    require_text(dpmr, "checkCRC7(m_bitBuffer, 41)")

    # Interleave: the 72-bit scatter, equivalent to a 12x6 transpose.
    require_text(dpmr, "dI72[i] = 12 * (i % 6) + (i / 6);")

    # Scrambler: the all-ones seed and the X^9 + X^5 + 1 feedback.
    require_text(dpmr, "m_sr = 0x3FF; // all ones")
    require_text(dpmr, "unsigned int feedback = ((((m_sr >> 4) & 1) ^ res) << 9);")

    # Hamming block count and CCH field offsets.
    require_text(dpmr, "m_hamming.decode(m_bitBufferRx, m_bitBuffer, 6)")
    require_text(dpmr, "m_frameNumber = (m_bitBuffer[0]<<1) + m_bitBuffer[1];")


def crc7(bits: list[int]) -> int:
    """CRC-7 over `bits`, MSB first, zero-initialised, no final inversion."""
    register = 0
    for bit in bits:
        if ((register >> 6) & 1) ^ (bit & 1):
            register = ((register << 1) ^ CRC7_POLYNOMIAL) & 0x7F
        else:
            register = (register << 1) & 0x7F
    return register


def hamming_12_8_encode(data: list[int]) -> list[int]:
    """Encode 8 data bits into a systematic 12-bit codeword."""
    if len(data) != 8:
        raise ValueError("Hamming(12,8) takes 8 data bits")
    parity = [0, 0, 0, 0]
    for index, taps in enumerate(HAMMING_12_8_PARITY):
        acc = 0
        for tap in taps:
            acc ^= data[tap] & 1
        parity[index] = acc
    return [bit & 1 for bit in data] + parity


def hamming_12_8_syndrome(codeword: list[int]) -> int:
    """The 4-bit syndrome dsd-neo's decoder computes, MSB first."""
    syndrome = 0
    for index, taps in enumerate(HAMMING_12_8_PARITY):
        acc = codeword[8 + index] & 1
        for tap in taps:
            acc ^= codeword[tap] & 1
        syndrome |= acc << (3 - index)
    return syndrome


def interleave_6x12(bits: list[int]) -> list[int]:
    """Inverse of dsd-neo's dpmr_deinterleave_6x12: a 6x12 -> 12x6 transpose."""
    out = [0] * 72
    for i in range(12):
        for j in range(6):
            out[(i * 6) + j] = bits[(j * 12) + i]
    return out


def scrambler_keystream(length: int, seed: int = SCRAMBLER_SEED) -> list[int]:
    """The X^9 + X^5 + 1 keystream, as dsd_scrambled_pmr_bits emits it."""
    shift = [(seed >> i) & 1 for i in range(9)]
    stream = []
    for _ in range(length):
        stream.append(shift[0])
        feedback = shift[4] ^ shift[0]
        shift = shift[1:] + [feedback]
    return stream


def scrambler_final_state(length: int, seed: int = SCRAMBLER_SEED) -> int:
    shift = [(seed >> i) & 1 for i in range(9)]
    for _ in range(length):
        feedback = shift[4] ^ shift[0]
        shift = shift[1:] + [feedback]
    return sum(bit << i for i, bit in enumerate(shift))


def bits_from_int(value: int, width: int) -> list[int]:
    """`width` bits of `value`, most significant first."""
    return [(value >> (width - 1 - i)) & 1 for i in range(width)]


def int_from_bits(bits: list[int]) -> int:
    value = 0
    for bit in bits:
        value = (value << 1) | (bit & 1)
    return value


def air_interface_id_string(ai_id: int) -> str:
    """Render a 24-bit AI ID as its dialled string (dPMR standard A.1.2.1.1.6)."""
    digits = []
    remaining = ai_id
    for divisor in (1464100, 146410, 14641, 1331, 121, 11, 1):
        digit = remaining // divisor
        remaining %= divisor
        digits.append("*" if digit == 10 else chr(ord("0") + digit))
    return "".join(digits)


def build_cch_payload(
    frame_number: int,
    id_half: int,
    communication_mode: int,
    version: int,
    comms_format: int,
    emergency: int,
    reserved: int,
    slow_data: int,
) -> list[int]:
    """The 41 CCH payload bits, in the field order dsd-neo decodes."""
    payload = []
    payload += bits_from_int(frame_number, 2)  # [0:2)
    payload += bits_from_int(id_half, 12)  # [2:14)
    payload += bits_from_int(communication_mode, 3)  # [14:17)
    payload += bits_from_int(version, 2)  # [17:19)
    payload += bits_from_int(comms_format, 2)  # [19:21)
    payload += [emergency & 1]  # [21]
    payload += [reserved & 1]  # [22]
    payload += bits_from_int(slow_data, 18)  # [23:41)
    if len(payload) != 41:
        raise AssertionError(f"CCH payload is {len(payload)} bits, expected 41")
    return payload


def encode_cch(payload41: list[int]) -> dict:
    """Take 41 payload bits all the way to 36 on-air dibits."""
    crc = crc7(payload41)
    decoded48 = payload41 + bits_from_int(crc, 7)

    encoded72 = []
    for block in range(6):
        encoded72 += hamming_12_8_encode(decoded48[block * 8 : (block + 1) * 8])

    interleaved = interleave_6x12(encoded72)
    keystream = scrambler_keystream(72)
    on_air = [interleaved[i] ^ keystream[i] for i in range(72)]
    dibits = [(on_air[2 * i] << 1) | on_air[(2 * i) + 1] for i in range(36)]

    return {
        "crc": crc,
        "decoded48": decoded48,
        "encoded72": encoded72,
        "on_air": on_air,
        "dibits": dibits,
    }


def colour_code_dibits() -> list[int]:
    bits = bits_from_int(COLOUR_CODE_BITS, 24)
    return [(bits[2 * i] << 1) | bits[(2 * i) + 1] for i in range(12)]


def voice_dibits(seed: int) -> list[int]:
    """Deterministic filler for the four AMBE frames of one TCH group.

    The vectors assert CCH content only; the voice payload just has to be stable
    and free of long constant runs that could alias a sync word.
    """
    state = seed & 0xFFFFFFFF
    out = []
    for _ in range(144):
        state = ((state * 1103515245) + 12345) & 0x7FFFFFFF
        out.append((state >> 16) & 3)
    return out


def build_frame(part: dict) -> dict:
    """One FS2 superframe part: its two CCH halves plus the 372 body dibits."""
    first = encode_cch(build_cch_payload(**part["cch"][0]))
    second = encode_cch(build_cch_payload(**part["cch"][1]))

    body = []
    body += first["dibits"]  # CCH #0        (36)
    body += voice_dibits(part["voice_seed"])  # TCH group 0   (144)
    body += colour_code_dibits()  # CC            (12)
    body += second["dibits"]  # CCH #1        (36)
    body += voice_dibits(part["voice_seed"] + 1)  # TCH group 1   (144)
    if len(body) != 372:
        raise AssertionError(f"frame body is {len(body)} dibits, expected 372")

    id_value = ((part["cch"][0]["id_half"] << 12) & 0x00FFF000) | (part["cch"][1]["id_half"] & 0x00000FFF)
    return {
        "name": part["name"],
        "halves": (first, second),
        "body": body,
        "id_value": id_value,
        "id_string": air_interface_id_string(id_value),
    }


def self_check(frames: list[dict]) -> None:
    """Refuse to emit vectors the model itself disagrees with."""
    keystream = scrambler_keystream(72)
    golden = [int(ch) for ch in SCRAMBLER_GOLDEN]
    if keystream != golden:
        raise SystemExit("scrambler keystream does not match the committed golden vector")
    if scrambler_final_state(72) != SCRAMBLER_GOLDEN_FINAL_STATE:
        raise SystemExit("scrambler final state does not match the committed golden vector")

    for frame in frames:
        for half in frame["halves"]:
            # The CRC's defining property: the remainder over message||crc is zero.
            if crc7(half["decoded48"]) != 0:
                raise SystemExit("CRC-7 append property failed")
            # Every Hamming block must be a clean codeword.
            for block in range(6):
                codeword = half["encoded72"][block * 12 : (block + 1) * 12]
                if hamming_12_8_syndrome(codeword) != 0:
                    raise SystemExit("Hamming(12,8) encode produced a non-zero syndrome")
            # And the on-air bits must round-trip back through the decode direction.
            descrambled = [half["on_air"][i] ^ keystream[i] for i in range(72)]
            deinterleaved = [0] * 72
            for i in range(12):
                for j in range(6):
                    deinterleaved[(j * 12) + i] = descrambled[(i * 6) + j]
            if deinterleaved != half["encoded72"]:
                raise SystemExit("interleave does not round-trip")

    # Each syndrome index must name the bit position that produced it.
    for position, syndrome in enumerate(HAMMING_12_8_SYNDROME):
        codeword = hamming_12_8_encode([0] * 8)
        codeword[position] ^= 1
        if hamming_12_8_syndrome(codeword) != syndrome:
            raise SystemExit(f"syndrome table wrong at bit {position}")
    uncorrectable = sorted(set(range(1, 16)) - set(HAMMING_12_8_SYNDROME))
    if uncorrectable != [9, 13, 15]:
        raise SystemExit(f"unexpected uncorrectable syndromes {uncorrectable}")


def format_values(values: list[int]) -> str:
    """A flat initialiser list; clang-format decides where the lines break."""
    return " ".join(f"{value}U," for value in values)


def clang_format(text: str, repo_root: pathlib.Path) -> str:
    """Run the project's clang-format so regeneration is idempotent."""
    try:
        return subprocess.run(
            ["clang-format", f"-assume-filename={repo_root / 'x.h'}"],
            input=text,
            text=True,
            check=True,
            capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit(f"clang-format failed: {exc}") from exc


def render_header(frames: list[dict]) -> str:
    keystream = scrambler_keystream(72)
    fs2_dibits = [int(ch) for ch in FS2_SYMBOLS]

    out = []
    out.append("// SPDX-License-Identifier: GPL-3.0-or-later")
    out.append("/*")
    out.append(" * dPMR CCH reference vectors -- generated, do not edit by hand.")
    out.append(" *")
    out.append(" * Produced by tests/protocol/dpmr/fixtures/generate_dpmr_reference_vectors.py from an")
    out.append(" * independent model of the ETSI TS 102 658 CCH encode direction, cross-checked against")
    out.append(" * DSDcc. See the README in this directory for provenance and regeneration.")
    out.append(" */")
    out.append("")
    out.append("#ifndef DSD_NEO_TESTS_PROTOCOL_DPMR_FIXTURES_DPMR_REFERENCE_VECTORS_H_")
    out.append("#define DSD_NEO_TESTS_PROTOCOL_DPMR_FIXTURES_DPMR_REFERENCE_VECTORS_H_")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f'static const char DPMR_REF_DSDCC_COMMIT[] = "{EXPECTED_DSDCC}";')
    out.append("")

    out.append("/* Scrambler: X^9 + X^5 + 1, all-ones seed, the 72 bits one CCH half is masked with. */")
    out.append(f"static const uint32_t DPMR_REF_SCRAMBLER_SEED = 0x{SCRAMBLER_SEED:03X}U;")
    out.append(f"static const uint32_t DPMR_REF_SCRAMBLER_FINAL_STATE = 0x{scrambler_final_state(72):03X}U;")
    out.append("static const uint8_t DPMR_REF_SCRAMBLER_KEYSTREAM[72] = {")
    out.append(format_values(keystream))
    out.append("};")
    out.append("")

    out.append("/* Block interleave: the position each of the 72 coded bits is transmitted in. */")
    out.append("static const uint8_t DPMR_REF_INTERLEAVE_INDEX[72] = {")
    index = [0] * 72
    for i in range(12):
        for j in range(6):
            index[(j * 12) + i] = (i * 6) + j
    out.append(format_values(index))
    out.append("};")
    out.append("")

    out.append("/* Shortened Hamming(12,8): 8 data bits, then 4 parity bits. */")
    out.append("typedef struct {")
    out.append("    uint8_t data[8];")
    out.append("    uint8_t codeword[12];")
    out.append("} dpmr_ref_hamming_vector;")
    out.append("")
    hamming_inputs = [0x00, 0xFF, 0x01, 0x80, 0x5A, 0xA5, 0x0F, 0xF0, 0x37, 0xC8]
    out.append(f"#define DPMR_REF_HAMMING_VECTOR_COUNT {len(hamming_inputs)}")
    out.append(f"static const dpmr_ref_hamming_vector DPMR_REF_HAMMING_VECTORS[{len(hamming_inputs)}] = {{")
    for value in hamming_inputs:
        data = bits_from_int(value, 8)
        codeword = hamming_12_8_encode(data)
        data_text = ", ".join(f"{bit}U" for bit in data)
        code_text = ", ".join(f"{bit}U" for bit in codeword)
        out.append(f"    {{{{{data_text}}}, {{{code_text}}}}},")
    out.append("};")
    out.append("")
    out.append("/* The syndrome a single-bit error in each position produces, most significant bit first. */")
    out.append("static const uint8_t DPMR_REF_HAMMING_SYNDROME_BY_POSITION[12] = {")
    out.append(format_values(list(HAMMING_12_8_SYNDROME)))
    out.append("};")
    out.append("/* The syndromes the code cannot place, and so must report uncorrectable. */")
    out.append("static const uint8_t DPMR_REF_HAMMING_UNCORRECTABLE_SYNDROMES[3] = {9U, 13U, 15U};")
    out.append("")

    out.append("/* CRC-7: X^7 + X^3 + 1 over the 41 payload bits, zero seed, no final inversion. */")
    out.append("typedef struct {")
    out.append("    uint8_t payload[41];")
    out.append("    uint8_t crc;")
    out.append("} dpmr_ref_crc_vector;")
    out.append("")

    crc_payloads = []
    for frame in frames:
        for half in frame["halves"]:
            crc_payloads.append(half["decoded48"][:41])
    crc_payloads.append([0] * 41)
    crc_payloads.append([1] * 41)
    out.append(f"#define DPMR_REF_CRC_VECTOR_COUNT {len(crc_payloads)}")
    out.append(f"static const dpmr_ref_crc_vector DPMR_REF_CRC_VECTORS[{len(crc_payloads)}] = {{")
    for payload in crc_payloads:
        payload_text = ", ".join(f"{bit}U" for bit in payload)
        out.append(f"    {{{{{payload_text}}}, 0x{crc7(payload):02X}U}},")
    out.append("};")
    out.append("")

    out.append("/* dPMR frame sync 2, as dibits: the 12 symbols that precede each frame below. */")
    out.append("static const uint8_t DPMR_REF_FS2_DIBITS[12] = {")
    out.append(format_values(fs2_dibits))
    out.append("};")
    out.append("")
    out.append("/* The channel code carried between the two TCH groups. */")
    out.append(f"static const int32_t DPMR_REF_COLOUR_CODE = {COLOUR_CODE_VALUE};")
    out.append("")

    out.append("/* A complete FS2 superframe part: 372 body dibits, and what decoding them must yield. */")
    out.append("typedef struct {")
    out.append("    const char* name;")
    out.append("    uint8_t body_dibits[372];")
    out.append("    uint8_t cch_decoded[2][48];")
    out.append("    uint8_t crc[2];")
    out.append("    uint32_t frame_number[2];")
    out.append("    uint32_t communication_mode[2];")
    out.append("    uint32_t version[2];")
    out.append("    uint32_t comms_format[2];")
    out.append("    uint32_t emergency[2];")
    out.append("    uint32_t id_value;")
    out.append("    const char* id_string;")
    out.append("} dpmr_ref_frame;")
    out.append("")
    out.append(f"#define DPMR_REF_FRAME_COUNT {len(frames)}")
    out.append(f"static const dpmr_ref_frame DPMR_REF_FRAMES[{len(frames)}] = {{")
    for frame in frames:
        first, second = frame["halves"]
        out.append("    {")
        out.append(f'        .name = "{frame["name"]}",')
        out.append("        .body_dibits = {")
        out.append(format_values(frame["body"]))
        out.append("},")
        out.append("        .cch_decoded = {")
        for half in (first, second):
            out.append("{")
            out.append(format_values(half["decoded48"]))
            out.append("},")
        out.append("},")
        out.append(f'        .crc = {{0x{first["crc"]:02X}U, 0x{second["crc"]:02X}U}},')
        out.append(
            "        .frame_number = {"
            f'{int_from_bits(first["decoded48"][0:2])}U, {int_from_bits(second["decoded48"][0:2])}U}},'
        )
        out.append(
            "        .communication_mode = {"
            f'{int_from_bits(first["decoded48"][14:17])}U, {int_from_bits(second["decoded48"][14:17])}U}},'
        )
        out.append(
            "        .version = {"
            f'{int_from_bits(first["decoded48"][17:19])}U, {int_from_bits(second["decoded48"][17:19])}U}},'
        )
        out.append(
            "        .comms_format = {"
            f'{int_from_bits(first["decoded48"][19:21])}U, {int_from_bits(second["decoded48"][19:21])}U}},'
        )
        out.append(
            "        .emergency = {" f'{first["decoded48"][21]}U, {second["decoded48"][21]}U}},'
        )
        out.append(f'        .id_value = {frame["id_value"]}U,')
        out.append(f'        .id_string = "{frame["id_string"]}",')
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("#endif /* DSD_NEO_TESTS_PROTOCOL_DPMR_FIXTURES_DPMR_REFERENCE_VECTORS_H_ */")
    out.append("")
    return "\n".join(out)


# The vector set. Two superframe parts, carrying the two halves of each identity:
# frame numbers 0/1 publish the called party (talkgroup), 2/3 the calling party.
# The two AI IDs are chosen to dial as 1234567 and 7654321 -- all digits, so an
# I/Q decode test can match them with a plain regex, and unmistakable in a log.
CALLED_ID = 1806845
CALLING_ID = 11206075

FRAME_SPECS = [
    {
        "name": "called-id",
        "voice_seed": 0x1234,
        "cch": [
            {
                "frame_number": 0,
                "id_half": (CALLED_ID >> 12) & 0xFFF,
                "communication_mode": 1,
                "version": 0,
                "comms_format": 1,
                "emergency": 0,
                "reserved": 0,
                "slow_data": 0x1A2B3,
            },
            {
                "frame_number": 1,
                "id_half": CALLED_ID & 0xFFF,
                "communication_mode": 1,
                "version": 0,
                "comms_format": 1,
                "emergency": 0,
                "reserved": 0,
                "slow_data": 0x0C4D5,
            },
        ],
    },
    {
        "name": "calling-id",
        "voice_seed": 0x5678,
        "cch": [
            {
                "frame_number": 2,
                "id_half": (CALLING_ID >> 12) & 0xFFF,
                "communication_mode": 1,
                "version": 0,
                "comms_format": 1,
                "emergency": 0,
                "reserved": 0,
                "slow_data": 0x3E6F7,
            },
            {
                "frame_number": 3,
                "id_half": CALLING_ID & 0xFFF,
                "communication_mode": 1,
                "version": 0,
                "comms_format": 1,
                "emergency": 0,
                "reserved": 0,
                "slow_data": 0x28901,
            },
        ],
    },
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dsdcc", type=pathlib.Path, help="DSDcc checkout to pin and grep")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    if args.dsdcc is not None:
        validate_oracle(args.dsdcc)
        print(f"validated DSDcc oracle at {EXPECTED_DSDCC}")
    else:
        print("note: --dsdcc not given, skipping oracle provenance checks")

    frames = [build_frame(spec) for spec in FRAME_SPECS]
    self_check(frames)
    text = clang_format(render_header(frames), args.output.resolve().parent)
    args.output.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {args.output}")
    for frame in frames:
        print(f"  {frame['name']}: id={frame['id_value']:#08x} string={frame['id_string']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
