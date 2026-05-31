#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate upstream M17 oracle checkouts and normalize the committed fixture header.

This helper is intentionally opt-in. CI uses the committed header only.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


EXPECTED_GR_M17 = "36267b114b41920b3b62d9545afe7d7c854801bf"
EXPECTED_M17_IMPLEMENTATIONS = "f80793f169751b1fd2de6aaf8de52f862360142c"
EXPECTED_LIBM17 = "07926d08ade8509f1ce55d0289f229ab44cbcf51"


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


def validate_oracles(gr_m17: pathlib.Path, m17_implementations: pathlib.Path, libm17: pathlib.Path) -> None:
    require_commit("gr-m17", gr_m17, EXPECTED_GR_M17)
    require_commit("M17_Implementations", m17_implementations, EXPECTED_M17_IMPLEMENTATIONS)
    require_commit("libm17", libm17, EXPECTED_LIBM17)

    require_text(gr_m17 / "lib" / "m17_coder_impl.cc", "(_fn >> 8) & 0x7F")
    require_text(gr_m17 / "lib" / "m17_decoder_impl.cc", "(_fn >> 8) & 0x7F")
    require_text(m17_implementations / "SP5WWP" / "m17-coder" / "m17-coder-sym.c", "(fn >> 8) & 0x7F")
    require_text(m17_implementations / "SP5WWP" / "m17-decoder" / "m17-decoder-sym.c", "(fn>>8) & 0x7F")
    require_text(libm17 / "decode" / "symbols.c", "{-3, -3, -3, -3, +3, +3, -3, +3}")
    require_text(libm17 / "decode" / "symbols.c", "{+3, -3, +3, +3, -3, -3, -3, -3}")


def normalize_header(output: pathlib.Path) -> None:
    text = output.read_text(encoding="utf-8")
    for required in (
        EXPECTED_GR_M17,
        EXPECTED_M17_IMPLEMENTATIONS,
        EXPECTED_LIBM17,
        "M17_REF_AES_TRANSMITTED_FN = 0xFFFFU",
        "0x0DU, 0x7FU, 0xFFU",
    ):
        if required not in text:
            raise SystemExit(f"{output}: missing committed fixture marker {required!r}")
    output.write_text(text, encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gr-m17", required=True, type=pathlib.Path)
    parser.add_argument("--m17-implementations", required=True, type=pathlib.Path)
    parser.add_argument("--libm17", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    validate_oracles(args.gr_m17, args.m17_implementations, args.libm17)
    normalize_header(args.output)
    print(f"validated and normalized {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
