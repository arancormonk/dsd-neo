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
PFFFT_CMAKE = Path(__file__).resolve().parents[1] / "cmake" / "Pffft.cmake"
ROOT = Path(__file__).resolve().parents[1]
LICENSEREF_MEMBER = "third_party/pffft/LicenseRef-PFFFT.txt"


def required_files(cmake=PFFFT_CMAKE):
    """The sources and notices cmake/Pffft.cmake requires of a PFFFT source tree."""
    text = cmake.read_text(encoding="utf-8")
    names = []
    for group in ("sources", "notices"):
        block = re.search(rf"^set\(_pffft_{group}\n(.*?)^\)", text, re.MULTILINE | re.DOTALL)
        if block is None:
            raise ValueError(f"{cmake} has no _pffft_{group} list")
        names += block.group(1).split()
    return names


def download_archive(sha):
    # Only HTTPS is supported: no file:// handler or redirect handler is installed.
    opener = urllib.request.OpenerDirector()
    opener.add_handler(urllib.request.HTTPSHandler(context=ssl.create_default_context()))
    url = f"https://codeload.github.com/arancormonk/pffft-neo/tar.gz/{sha}"
    with opener.open(url, timeout=120) as response:
        if response.status != 200:
            raise OSError(f"PFFFT download returned HTTP {response.status}")
        return response.read()


def replace_files(contents):
    """Replace each path with its bytes, all or none: every file is written beside its destination first, and a
    failed move puts back the files already replaced."""
    staged = []
    replaced = []
    try:
        for path, data in contents:
            with tempfile.NamedTemporaryFile(mode="wb", dir=path.parent, delete=False) as output:
                staged.append((path, Path(output.name), path.read_bytes()))
                output.write(data)
            staged[-1][1].chmod(path.stat().st_mode)
        for path, temporary, original in staged:
            os.replace(temporary, path)
            replaced.append((path, original))
    except BaseException:
        for path, original in reversed(replaced):
            path.write_bytes(original)
        raise
    finally:
        for _, temporary, _ in staged:
            temporary.unlink(missing_ok=True)


def update_pins(sha, root=ROOT):
    """Move the pins in root's tools/ci-dependency-pins.env, with the tracked LICENSES/LicenseRef-PFFFT.txt
    (pffft-neo's retained-notice record, the inventory's LicenseRef-PFFFT) and its hash in legal/components.json."""
    pins = root / "tools" / "ci-dependency-pins.env"
    licenseref = root / "LICENSES" / "LicenseRef-PFFFT.txt"
    inventory = root / "legal" / "components.json"
    if not re.fullmatch(r"[0-9a-fA-F]{40}", sha):
        raise ValueError("PFFFT revision must be a full 40-character commit SHA")
    sha = sha.lower()
    archive = download_archive(sha)
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as source:
        for name in required_files():
            if not source.getmember(f"pffft-neo-{sha}/{name}").isfile():
                raise ValueError(f"PFFFT archive is missing {name}")
        notice = source.extractfile(f"pffft-neo-{sha}/{LICENSEREF_MEMBER}").read()
    digest = hashlib.sha256(archive).hexdigest()
    # The tracked LicenseRef text and its inventory hash (over LF line endings) follow the pin.
    records = inventory.read_text(encoding="utf-8")
    key = r'("LICENSES/LicenseRef-PFFFT\.txt": ")[0-9a-f]{64}(")'
    if len(re.findall(key, records)) != 1:
        raise ValueError(f"Expected exactly one LicenseRef-PFFFT notice hash in {inventory}")
    notice_digest = hashlib.sha256(notice.replace(b"\r\n", b"\n")).hexdigest()
    records = re.sub(key, rf"\g<1>{notice_digest}\g<2>", records)
    text = pins.read_text(encoding="utf-8")
    for key, value in (("PFFFT_SHA", sha), ("PFFFT_ARCHIVE_SHA256", digest)):
        pattern = rf"^{key}=.*$"
        if len(re.findall(pattern, text, flags=re.MULTILINE)) != 1:
            raise ValueError(f"Expected exactly one {key} in {pins}")
        text = re.sub(pattern, f"{key}={value}", text, flags=re.MULTILINE)
    replace_files(((licenseref, notice), (inventory, records.encode("utf-8")), (pins, text.encode("utf-8"))))
    print(f"PFFFT_SHA={sha}\nPFFFT_ARCHIVE_SHA256={digest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sha", help="full commit SHA from arancormonk/pffft-neo")
    args = parser.parse_args()
    try:
        update_pins(args.sha)
    except (OSError, ValueError, KeyError, tarfile.TarError) as error:
        parser.exit(1, f"PFFFT pin update failed: {error}\n")


if __name__ == "__main__":
    main()
