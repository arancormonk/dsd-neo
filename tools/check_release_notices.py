#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check that a package's documentation folder carries every notice its component records name.

Reads the <platform>_release map of each record in legal/components.json (the same maps
cmake/stage_release_notices.cmake stages from) and checks the folder of a staged tree or a
mounted package: each notice is there and not empty, and a notice from the repository, the
build tree or the vcpkg tree has the content of its source (line endings aside). A
legal-component notice, and each further file --require names, comes with the package's
general legal files: one under licenses/mbelib-neo/ has the content of the mbelib-neo notice
folder the build selected (DSD_MBELIB_SELECTED_NOTICE_DIR in --build-dir's CMake cache), any
other the content of the repository's file (licenses/ is the repository's LICENSES/).

--index names the folder's index of its files (the APK's notice-index.txt, which the licences
screen reads): it must be there, be UTF-8 text, and name each other file of the folder, and
only those.

--binary-dir names the package's binary folders (the DMG's bin/ and lib/): every file in them
must be one that a record's map lists under "binaries", so a library no record covers, whose
notices nothing stages, cannot ship. A macOS name stands for the file of that name and for the
library <name>.dylib with any version, <name>.<n>[.<n>...].dylib.
"""

import argparse
import json
import re
import sys
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
RELEASE_MAPS = {"windows": "windows_release", "android": "android_release", "macos": "macos_release"}
# What QString::trimmed() removes from an index line (QChar::isSpace()): the Unicode space, line and paragraph
# separators, and TAB, LF, VT, FF, CR and NEL.
QT_SPACE = ("\t\n\v\f\r\x85 \xa0\u1680\u2000\u2001\u2002\u2003\u2004\u2005\u2006\u2007\u2008\u2009\u200a"
            "\u2028\u2029\u202f\u205f\u3000")


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


def selected_mbelib(build):
    """The mbelib-neo notice folder the configured build at @p build packages, or None when the cache names none."""
    for line in (build / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        name, _, value = line.partition("=")
        if name.partition(":")[0] == "DSD_MBELIB_SELECTED_NOTICE_DIR" and value:
            return Path(value)
    return None


def general_file(name, repo, mbelib):
    """The file a general legal file of the package is installed from; None for mbelib's when no folder is known."""
    if name.startswith("licenses/mbelib-neo/"):
        return None if mbelib is None else mbelib / name.removeprefix("licenses/mbelib-neo/")
    if name.startswith("licenses/"):
        return repo / "LICENSES" / name.removeprefix("licenses/")
    return repo / name


def normalized(path):
    return path.read_bytes().replace(b"\r\n", b"\n")


def check(records, platform, doc, repo, build=None, share=None, required=(), mbelib=None):
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
            if source == "legal-component":
                origin = general_file(notice, repo, mbelib)
            else:
                origin = source_file(source, repo, build, share)
            if origin is not None and (not origin.is_file() or normalized(origin) != normalized(path)):
                problems.append(f"{notice} differs from {source} ({ident})")
    for name in required:
        if name in notices:
            continue  # checked above, against the source its record maps it to
        if not inside(name) or not (doc / name).is_file():
            problems.append(f"missing {name} (required)")
            continue
        origin = general_file(name, repo, mbelib)
        if origin is not None and (not origin.is_file() or normalized(origin) != normalized(doc / name)):
            problems.append(f"{name} differs from {origin} (required)")
    return problems, len(notices)


def check_index(doc, index):
    """Return the problems of the folder's index of its files: missing, or not naming exactly the other files."""
    if not inside(index) or not (doc / index).is_file():
        return [f"missing {index}"]
    # As DecoderHost::licenseFiles() reads it: the raw bytes as UTF-8 (QString::fromUtf8() takes one leading byte
    # order mark as no text), split on LF alone, each line trimmed. Bytes that are not UTF-8 would reach the screen as
    # replacement characters, so they are an error of the index.
    data = (doc / index).read_bytes()
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as error:
        return [f"{index} is not valid UTF-8 (byte {error.start}: {error.reason})"]
    lines = text.removeprefix("\ufeff").split("\n")
    listed = {line.strip(QT_SPACE) for line in lines if line.strip(QT_SPACE)}
    present = {path.relative_to(doc).as_posix() for path in doc.rglob("*") if path.is_file()} - {index}
    problems = [f"{name} is in the folder but not in {index}" for name in sorted(present - listed)]
    problems += [f"{index} names {name}, which the folder lacks" for name in sorted(listed - present)]
    return problems


def recorded(name, names, platform):
    """Whether a file name is one the records list for a package."""
    if name in names:
        return True
    return platform == "macos" and any(re.fullmatch(re.escape(n) + r"(\.[0-9]+)*\.dylib", name) for n in names)


def check_binaries(records, platform, folders):
    """Return the problems found and the number of files checked in a package's binary folders."""
    key = RELEASE_MAPS[platform]
    names = {name for component in records["components"] for name in component.get(key, {}).get("binaries", [])}
    problems = []
    count = 0
    for folder in folders:
        if not folder.is_dir():
            problems.append(f"missing binary folder {folder}")
            continue
        for entry in sorted(folder.iterdir()):
            count += 1
            if not recorded(entry.name, names, platform):
                problems.append(f"{entry.name} in {folder} is in no record's {key} binaries")
    return problems, count


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--platform", choices=sorted(RELEASE_MAPS), required=True)
    parser.add_argument("--doc-dir", type=Path, required=True, help="the package's share/doc/dsd-neo folder")
    parser.add_argument("--components", type=Path, default=ROOT / "legal/components.json")
    parser.add_argument("--repo", type=Path, default=ROOT, help="the repository the repo: notices come from")
    parser.add_argument("--build-dir", type=Path, help="the configured build tree (build: notices)")
    parser.add_argument("--vcpkg-share", type=Path, help="the vcpkg tree's share folder (vcpkg: notices)")
    parser.add_argument("--require", nargs="*", default=[], help="further files the folder must hold")
    parser.add_argument("--index", help="the folder's index of its other files, which must name each of them")
    parser.add_argument("--binary-dir", type=Path, nargs="*", default=[],
                        help="the package's binary folders, whose every file a record must list")
    args = parser.parse_args()
    records = json.loads(args.components.read_text(encoding="utf-8"))
    problems, mbelib = [], None
    if args.build_dir is not None:
        mbelib = selected_mbelib(args.build_dir)
        if mbelib is None:
            problems.append(f"The build at {args.build_dir} names no DSD_MBELIB_SELECTED_NOTICE_DIR, so mbelib-neo's "
                            "notices cannot be compared with the ones it packages")
    found, count = check(records, args.platform, args.doc_dir, args.repo, args.build_dir, args.vcpkg_share,
                         args.require, mbelib)
    problems += found
    if args.index is not None:
        problems += check_index(args.doc_dir, args.index)
    binary_problems, binaries = check_binaries(records, args.platform, args.binary_dir)
    for problem in problems + binary_problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        print(f"{args.doc_dir} lacks notices its records name ({len(problems)} problem(s)).", file=sys.stderr)
    if binary_problems:
        print(f"The package carries binaries no record names ({len(binary_problems)} problem(s)).", file=sys.stderr)
    if problems or binary_problems:
        return 1
    print(f"{args.doc_dir} carries the {count} notices legal/components.json names for {args.platform}.")
    if args.binary_dir:
        print(f"The {binaries} binaries in {', '.join(map(str, args.binary_dir))} are each named by a record.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
