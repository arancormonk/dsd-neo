#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reproduce deterministic derivatives of the preserved TETRA real-air captures."""

from __future__ import annotations

import argparse
from array import array
import hashlib
from pathlib import Path
import sys
from typing import Optional


SOURCE_SHA256 = "dcd4f3dbeceb09243e20f3632d2543462a13db05fa3815354e4a597a34263fd0"
DERIVED_SHA256 = "601c843b35fdaa3a31e07ed402519af96b3fdf2f5433f67f6a8502385f450ea6"
MARGINAL_SHA256 = "61dd6b917a390c49552e6564679e588bc047dae3dd9ec89a3a1ea4201f5d9399"
MARGINAL_INVERTED_SHA256 = "4e8c4ffcc3a93946857c32c888cc1e67fd7347152729a4a5c3c440806c0e890e"
INPUT_RATE = 50_000
OUTPUT_RATE = 54_000


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def rounded_div(value: int, divisor: int) -> int:
    """Round an integer quotient symmetrically, with halves away from zero."""
    if value >= 0:
        return (value + divisor // 2) // divisor
    return -((-value + divisor // 2) // divisor)


def resample_cs16_50k_to_54k(raw: bytes) -> bytes:
    """Linearly interpolate interleaved little-endian IQ using exact integer arithmetic."""
    if len(raw) == 0 or len(raw) % 4:
        raise ValueError("source must contain complete little-endian CS16 IQ pairs")
    samples = array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()

    complex_count = len(samples) // 2
    output_count = complex_count * OUTPUT_RATE // INPUT_RATE
    # 50/54 reduces to 25/27. Keeping the rational position in integers makes
    # the committed derivative byte-identical across Python and OS versions.
    denominator = 27
    numerator_step = 25
    output = array("h")
    for output_index in range(output_count):
        source_index, remainder = divmod(output_index * numerator_step, denominator)
        next_index = min(source_index + 1, complex_count - 1)
        for component in (0, 1):
            left = samples[source_index * 2 + component]
            right = samples[next_index * 2 + component]
            mixed = left * (denominator - remainder) + right * remainder
            output.append(rounded_div(mixed, denominator))

    if sys.byteorder != "little":
        output.byteswap()
    return output.tobytes()


def conjugate_cs16(raw: bytes) -> bytes:
    """Complex-conjugate interleaved little-endian CS16 with saturating negation."""
    if len(raw) == 0 or len(raw) % 4:
        raise ValueError("source must contain complete little-endian CS16 IQ pairs")
    samples = array("h")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    for index in range(1, len(samples), 2):
        # -INT16_MIN is not representable. Saturation matches the quantizer used
        # by the synthetic fixture generator and keeps the transform portable.
        samples[index] = 32767 if samples[index] == -32768 else -samples[index]
    if sys.byteorder != "little":
        samples.byteswap()
    return samples.tobytes()


def verify(source: Path, derived: Path) -> None:
    raw = source.read_bytes()
    if sha256(raw) != SOURCE_SHA256:
        raise ValueError(f"unexpected source SHA-256: {sha256(raw)}")
    expected = resample_cs16_50k_to_54k(raw)
    if sha256(expected) != DERIVED_SHA256:
        raise ValueError(f"unexpected generated SHA-256: {sha256(expected)}")
    if derived.read_bytes() != expected:
        raise ValueError(f"stale derived fixture: {derived}")


def verify_marginal(path: Path, inverted: Optional[Path] = None) -> None:
    actual = sha256(path.read_bytes())
    if actual != MARGINAL_SHA256:
        raise ValueError(f"unexpected marginal-capture SHA-256: {actual}")
    if inverted is not None:
        expected = conjugate_cs16(path.read_bytes())
        if sha256(expected) != MARGINAL_INVERTED_SHA256:
            raise ValueError(f"unexpected generated inverted SHA-256: {sha256(expected)}")
        if inverted.read_bytes() != expected:
            raise ValueError(f"stale inverted marginal fixture: {inverted}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--conjugate-source", type=Path)
    parser.add_argument("--conjugate-output", type=Path)
    parser.add_argument("--verify", action="store_true")
    args = parser.parse_args()
    try:
        resample_requested = args.source is not None or args.output is not None
        conjugate_requested = args.conjugate_source is not None or args.conjugate_output is not None
        if not resample_requested and not conjugate_requested:
            parser.error("provide a source/output pair")
        if resample_requested and (args.source is None or args.output is None):
            parser.error("--source and --output must be used together")
        if conjugate_requested and (args.conjugate_source is None or args.conjugate_output is None):
            parser.error("--conjugate-source and --conjugate-output must be used together")
        if resample_requested:
            if args.verify:
                verify(args.source, args.output)
                print("verified raw and resampled TETRA real-air fixtures")
            else:
                raw = args.source.read_bytes()
                if sha256(raw) != SOURCE_SHA256:
                    raise ValueError(f"unexpected source SHA-256: {sha256(raw)}")
                args.output.write_bytes(resample_cs16_50k_to_54k(raw))
                print(f"wrote {args.output}")
        if conjugate_requested:
            raw = args.conjugate_source.read_bytes()
            if sha256(raw) != MARGINAL_SHA256:
                raise ValueError(f"unexpected marginal-capture SHA-256: {sha256(raw)}")
            expected = conjugate_cs16(raw)
            if sha256(expected) != MARGINAL_INVERTED_SHA256:
                raise ValueError(f"unexpected generated inverted SHA-256: {sha256(expected)}")
            if args.verify:
                if args.conjugate_output.read_bytes() != expected:
                    raise ValueError(f"stale inverted marginal fixture: {args.conjugate_output}")
                print("verified marginal and inverted TETRA real-air fixtures")
            else:
                args.conjugate_output.write_bytes(expected)
                print(f"wrote {args.conjugate_output}")
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
