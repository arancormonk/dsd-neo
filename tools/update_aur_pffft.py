#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep AUR's source acquisition in sync with the compiled PFFFT pin."""

import argparse
from pathlib import Path
import re

from pffft_metadata import read_pins

BEGIN = "# BEGIN pinned PFFFT (tools/update_aur_pffft.py)"
END = "# END pinned PFFFT"


def update(text, pins):
    block = f'''{BEGIN}
_pffft_sha={pins["PFFFT_SHA"]}
source+=("pffft-neo-$_pffft_sha.tar.gz::https://codeload.github.com/arancormonk/pffft-neo/tar.gz/$_pffft_sha")
sha256sums+=('{pins["PFFFT_ARCHIVE_SHA256"]}')
{END}
'''
    pattern = rf"{re.escape(BEGIN)}\n.*?{re.escape(END)}\n"
    if BEGIN in text:
        text, count = re.subn(pattern, lambda _: block, text, flags=re.DOTALL)
        if count != 1:
            raise ValueError("Expected one complete PFFFT source block")
    elif text.count("build() {") == 1:
        text = text.replace("build() {", block + "\nbuild() {", 1)
    else:
        raise ValueError("Unsupported PKGBUILD: expected one build() function")
    # Published recipes still unpack the PFFFT fork's archive name; pffft-neo's unpacks as pffft-neo-<sha>.
    text = re.sub(r'(-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT="\$srcdir/)pffft-(\$_pffft_sha|\$\{_pffft_sha\})"',
                  r'\1pffft-neo-\2"', text)
    if "-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT=" in text and not re.search(
            r'-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT="\$srcdir/pffft-neo-(\$_pffft_sha|\$\{_pffft_sha\})"', text):
        raise ValueError("Unsupported PKGBUILD: the PFFFT source override names another directory")
    if "-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT=" not in text:
        anchor = "        -DCMAKE_INSTALL_PREFIX=/usr \\\n"
        if text.count(anchor) != 1:
            raise ValueError("Unsupported PKGBUILD: expected one CMake install prefix")
        options = '        -DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT="$srcdir/pffft-neo-$_pffft_sha" \\\n'
        # The TETRA_crypto helper may already have disconnected FetchContent.
        if "-DFETCHCONTENT_FULLY_DISCONNECTED=ON" not in text:
            options += '        -DFETCHCONTENT_FULLY_DISCONNECTED=ON \\\n'
        text = text.replace(anchor, anchor + options)
        # A -git recipe must reject a branch whose dependency pin moved before
        # the AUR metadata was refreshed, rather than silently compile stale FFT.
        text = text.replace("build() {\n", '''build() {
    local _dsd_source="$pkgname"
    [[ "$pkgname" == *-git ]] || _dsd_source="$pkgname-$pkgver"
    # Keep the recipe usable before the FetchContent migration reaches main/releases.
    if [[ -f "$_dsd_source/cmake/Pffft.cmake" ]] &&
        ! grep -qx "PFFFT_SHA=$_pffft_sha" "$_dsd_source/tools/ci-dependency-pins.env"; then
        echo "PFFFT pin changed; update the AUR PKGBUILD before building." >&2
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
