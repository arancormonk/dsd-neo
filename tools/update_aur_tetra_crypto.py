#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep AUR's source acquisition in sync with the compiled TETRA_crypto pin."""

import argparse
from pathlib import Path
import re

from tetra_crypto_metadata import read_pins

BEGIN = "# BEGIN pinned TETRA_crypto (tools/update_aur_tetra_crypto.py)"
END = "# END pinned TETRA_crypto"
DISCONNECTED = "-DFETCHCONTENT_FULLY_DISCONNECTED=ON"


def update(text, pins):
    block = f'''{BEGIN}
_tetra_crypto_sha={pins["TETRA_CRYPTO_SHA"]}
source+=("TETRA_crypto-$_tetra_crypto_sha.tar.gz::https://codeload.github.com/MidnightBlueLabs/TETRA_crypto/tar.gz/$_tetra_crypto_sha")
sha256sums+=('{pins["TETRA_CRYPTO_ARCHIVE_SHA256"]}')
{END}
'''
    pattern = rf"{re.escape(BEGIN)}\n.*?{re.escape(END)}\n"
    if BEGIN in text:
        text, count = re.subn(pattern, lambda _: block, text, flags=re.DOTALL)
        if count != 1:
            raise ValueError("Expected one complete TETRA_crypto source block")
    elif text.count("build() {") == 1:
        text = text.replace("build() {", block + "\nbuild() {", 1)
    else:
        raise ValueError("Unsupported PKGBUILD: expected one build() function")
    if "-DFETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO=" not in text:
        anchor = "        -DCMAKE_INSTALL_PREFIX=/usr \\\n"
        if text.count(anchor) != 1:
            raise ValueError("Unsupported PKGBUILD: expected one CMake install prefix")
        options = '        -DFETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO="$srcdir/TETRA_crypto-$_tetra_crypto_sha" \\\n'
        # The PFFFT helper may already have disconnected FetchContent.
        if DISCONNECTED not in text:
            options += f"        {DISCONNECTED} \\\n"
        text = text.replace(anchor, anchor + options)
        # A -git recipe must reject a branch whose dependency pin moved before
        # the AUR metadata was refreshed, rather than silently compile stale TEA code.
        text = text.replace("build() {\n", '''build() {
    local _tetra_crypto_source="$pkgname"
    [[ "$pkgname" == *-git ]] || _tetra_crypto_source="$pkgname-$pkgver"
    # Sources that predate the embedded TETRA_crypto have no pin to compare.
    if [[ -f "$_tetra_crypto_source/cmake/TetraCrypto.cmake" ]] &&
        ! grep -qx "TETRA_CRYPTO_SHA=$_tetra_crypto_sha" "$_tetra_crypto_source/tools/ci-dependency-pins.env"; then
        echo "TETRA_crypto pin changed; update the AUR PKGBUILD before building." >&2
        return 1
    fi
''', 1)
    return text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pkgbuild", type=Path)
    parser.add_argument("--pins", type=Path, help="pin file from the source revision being packaged")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    pins = read_pins(args.pins) if args.pins else read_pins()
    before = args.pkgbuild.read_text(encoding="utf-8")
    after = update(before, pins)
    if args.check:
        raise SystemExit(0 if before == after else 1)
    args.pkgbuild.write_text(after, encoding="utf-8")


if __name__ == "__main__":
    main()
