#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Update both TETRA_crypto pins atomically after downloading an immutable archive."""

import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import tarfile
import tempfile
import urllib.request
import ssl

PINS = Path(__file__).resolve().with_name("ci-dependency-pins.env")
# Everything cmake/TetraCrypto.cmake compiles, includes or installs.
REQUIRED = ("tea1.c", "tea1.h", "tea2.c", "tea2.h", "tea3.c", "tea3.h", "LICENSE")


def download_archive(sha):
    # Only HTTPS is supported: no file:// handler or redirect handler is installed.
    opener = urllib.request.OpenerDirector()
    opener.add_handler(urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    url = f"https://codeload.github.com/MidnightBlueLabs/TETRA_crypto/tar.gz/{sha}"
    with opener.open(url, timeout=120) as response:
        if response.status != 200:
            raise OSError(f"TETRA_crypto download returned HTTP {response.status}")
        return response.read()


def update_pins(sha, pins=PINS):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise ValueError("TETRA_crypto revision must be a full 40-character commit SHA")
    sha = sha.lower()
    archive = download_archive(sha)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as source:
        for name in REQUIRED:
            if not source.getmember(f"TETRA_crypto-{sha}/{name}").isfile():
                raise ValueError(f"TETRA_crypto archive is missing {name}")
    digest = hashlib.sha256(archive).hexdigest()
    text = pins.read_text(encoding="utf-8")
    for key, value in (("TETRA_CRYPTO_SHA", sha), ("TETRA_CRYPTO_ARCHIVE_SHA256", digest)):
        pattern = rf"^{key}=.*$"
        if len(re.findall(pattern, text, flags=re.MULTILINE)) != 1:
            raise ValueError(f"Expected exactly one {key} in {pins}")
        text = re.sub(pattern, f"{key}={value}", text, flags=re.MULTILINE)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=pins.parent, delete=False) as output:
            temporary = Path(output.name)
            output.write(text)
        temporary.chmod(pins.stat().st_mode)
        os.replace(temporary, pins)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    print(f"TETRA_CRYPTO_SHA={sha}\nTETRA_CRYPTO_ARCHIVE_SHA256={digest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sha", help="full commit SHA from MidnightBlueLabs/TETRA_crypto")
    args = parser.parse_args()
    try:
        update_pins(args.sha)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(1, f"TETRA_crypto pin update failed: {error}\n")


if __name__ == "__main__":
    main()
