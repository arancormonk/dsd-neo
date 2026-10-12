#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Opt-in CMake acquisition checks (network required); also accepts CMake 3.20."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def configure(cmake, source, build, *options, error=None):
    command = [cmake, "-S", str(source), "-B", str(build),
               "-DBUILD_TESTING=OFF", "-DDSD_ENABLE_QT_UI=OFF", *options]
    result = subprocess.run(command, capture_output=True, text=True, timeout=240)
    log = result.stdout + result.stderr
    (build.parent / f"{build.name}.log").write_text(log, encoding="utf-8")
    if error:
        assert result.returncode != 0 and error in " ".join(log.split()), log
    else:
        assert result.returncode == 0, log
    print(f"PASS {build.name}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", default="cmake")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="dsd-pffft-acquisition-") as directory:
        work = Path(directory)
        source = work / "source"
        # Copy the working source distribution without build outputs or Git metadata.
        paths = subprocess.check_output([
            "git", "ls-files", "-z", "--cached", "--others", "--exclude-standard",
        ], cwd=ROOT).decode().split("\0")
        for name in paths:
            path = ROOT / name
            if name and path.is_file():
                target = source / name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, target)
        online = work / "download"
        configure(args.cmake, source, online)
        configure(args.cmake, source, online, "-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        local = work / "pffft source without git"
        shutil.copytree(online / "_deps/dsd_pffft-src", local)
        offline = work / "offline"
        configure(args.cmake, source, offline, f"-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT={local}",
                  "-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        subprocess.run([args.cmake, "--build", str(offline), "--target", "dsd-neo_pffft", "-j", "4"], check=True,
                       stdout=subprocess.DEVNULL, timeout=240)
        configure(args.cmake, source, work / "missing", "-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT=/no/pffft/here",
                  error="existing absolute PFFFT source directory")
        configure(args.cmake, source, work / "layout", f"-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT={source}",
                  error="Incompatible PFFFT source directory")
        pins = source / "tools/ci-dependency-pins.env"
        text = pins.read_text()
        checksum = next(line for line in text.splitlines() if line.startswith("PFFFT_ARCHIVE_SHA256="))
        pins.write_text(text.replace(checksum, "PFFFT_ARCHIVE_SHA256=" + "0" * 64))
        configure(args.cmake, source, work / "checksum", error="SHA256")


if __name__ == "__main__":
    main()
