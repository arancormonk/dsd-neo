#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Update both PFFFT pins atomically after downloading an immutable archive."""

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


def download_archive(sha):
    # Only HTTPS is supported: no file:// handler or redirect handler is installed.
    opener = urllib.request.OpenerDirector()
    opener.add_handler(urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    url = f"https://codeload.github.com/arancormonk/pffft/tar.gz/{sha}"
    with opener.open(url, timeout=120) as response:
        if response.status != 200:
            raise OSError(f"PFFFT download returned HTTP {response.status}")
        return response.read()


def update_pins(sha, pins=PINS):
    if not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise ValueError("PFFFT revision must be a full 40-character commit SHA")
    sha = sha.lower()
    archive = download_archive(sha)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as source:
        for name in ("src/pffft.c", "src/pffft_common.c", "include/pffft/pffft.h", "LICENSE.txt"):
            if not source.getmember(f"pffft-{sha}/{name}").isfile():
                raise ValueError(f"PFFFT archive is missing {name}")
    digest = hashlib.sha256(archive).hexdigest()
    text = pins.read_text(encoding="utf-8")
    for key, value in (("PFFFT_SHA", sha), ("PFFFT_ARCHIVE_SHA256", digest)):
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
    print(f"PFFFT_SHA={sha}\nPFFFT_ARCHIVE_SHA256={digest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sha", help="full commit SHA from arancormonk/pffft")
    args = parser.parse_args()
    try:
        update_pins(args.sha)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(1, f"PFFFT pin update failed: {error}\n")


if __name__ == "__main__":
    main()
