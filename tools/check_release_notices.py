#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a package's documentation folder carries every notice its component records name.

Reads the <platform>_release map of each record in legal/components.json (the same maps
cmake/stage_release_notices.cmake stages from) and checks the folder of a staged tree or a
mounted package: each notice is there and not empty, and a notice from the repository, the
build tree or the vcpkg tree has the content of its source (line endings aside). A
legal-component notice is installed by the package's general legal files and only has to be
there. --require names further files the folder must hold.
"""

import argparse
import json
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
RELEASE_MAPS = {"windows": "windows_release", "android": "android_release", "macos": "macos_release"}


def inside(path):
    """Whether a map path names a file inside its folder."""
    parts = PurePosixPath(path).parts
    return bool(parts) and not PurePosixPath(path).is_absolute() and "\\" not in path and all(
        part not in ("", ".", "..") for part in parts)


def source_file(source, repo, build, share):
    """The file a notice is staged from, or None when it is not known here."""
    kind, _, rest = source.partition(":")
    if kind == "repo":
        return repo / rest
    if kind == "build" and build is not None:
        return build / rest
    if kind == "vcpkg" and share is not None:
        return share / rest / "copyright"
    return None


def normalized(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def check(records, platform, doc, repo, build=None, share=None, required=()):
    """Return the problems found and the number of notices checked."""
    problems = []
    notices = {}
    for component in records["components"]:
        for notice, source in component.get(RELEASE_MAPS[platform], {}).get("notices", {}).items():
            if not inside(notice):
                problems.append(f"{notice!r} is not a path inside the package ({component['id']})")
                continue
            notices.setdefault(notice, []).append((component["id"], source))
    if not notices:
        problems.append(f"legal/components.json names no notice for {platform}")
    for notice, owners in sorted(notices.items()):
        path = doc / notice
        if not path.is_file():
            ident, source = owners[0]
            problems.append(f"missing {notice} ({ident}, {source})")
            continue
        if not normalized(path).strip():
            problems.append(f"empty {notice}")
            continue
        for ident, source in owners:
            origin = source_file(source, repo, build, share)
            if origin is not None and (not origin.is_file() or normalized(origin) != normalized(path)):
                problems.append(f"{notice} differs from {source} ({ident})")
    for name in required:
        if not inside(name) or not (doc / name).is_file():
            problems.append(f"missing {name} (required)")
    return problems, len(notices)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=sorted(RELEASE_MAPS), required=True)
    parser.add_argument("--doc-dir", type=Path, required=True, help="the package's share/doc/dsd-neo folder")
    parser.add_argument("--components", type=Path, default=ROOT / "legal/components.json")
    parser.add_argument("--repo", type=Path, default=ROOT, help="the repository the repo: notices come from")
    parser.add_argument("--build-dir", type=Path, help="the configured build tree (build: notices)")
    parser.add_argument("--vcpkg-share", type=Path, help="the vcpkg tree's share folder (vcpkg: notices)")
    parser.add_argument("--require", nargs="*", default=[], help="further files the folder must hold")
    args = parser.parse_args()
    records = json.loads(args.components.read_text(encoding="utf-8"))
    problems, count = check(records, args.platform, args.doc_dir, args.repo, args.build_dir, args.vcpkg_share,
                            args.require)
    for problem in problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        print(f"{args.doc_dir} lacks notices its records name ({len(problems)} problem(s)).", file=sys.stderr)
        return 1
    print(f"{args.doc_dir} carries the {count} notices legal/components.json names for {args.platform}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
