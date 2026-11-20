#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Write the directions to the Qt source code that go next to every APK.

The APK carries The Qt Company's prebuilt Qt for Android, used under the LGPL 3. Its source is
offered by directions to Qt's official source archives for the exact Qt version the build used
(GPL 3 section 6(d), as LGPL 3 section 4(d)(0) asks), not by a copy on dsd-neo's servers.

  directions  read the Qt version from the kit the build used (mkspecs/qconfig.pri), check it is
              the version given, and write the directions: the qtbase, qtdeclarative and qtsvg
              archives of that version on download.qt.io, and the source form of the Public Suffix
              List that Qt Network compiles in, as the kit's qtbase SBOM records it.
  notes       put the directions into a release's notes, replacing an earlier copy.
"""

import argparse
import json
import re
import sys
from pathlib import Path

# The Qt repositories whose libraries, plugins and QML modules the APK ships (legal/components.json: qt).
QT_MODULES = ("qtbase", "qtdeclarative", "qtsvg")
VERSION = re.compile(r"(\d+)\.(\d+)\.(\d+)")
PSL_PACKAGE = "Network_Attribution_psl-data"
PSL_PURL = re.compile(r"pkg:generic/psl-data@([0-9a-f]{40})(?:\?|$)")
BEGIN = "<!-- qt-source-directions:begin -->"
END = "<!-- qt-source-directions:end -->"


class DirectionsError(Exception):
    pass


def kit_version(kit):
    """The Qt version the kit reports in mkspecs/qconfig.pri."""
    path = kit / "mkspecs/qconfig.pri"
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as error:
        raise DirectionsError(f"cannot read {path}: {error}") from error
    found = re.findall(r"(?m)^QT_VERSION\s*=\s*(\S+)\s*$", text)
    if len(found) != 1:
        raise DirectionsError(f"{path} does not name one QT_VERSION")
    return found[0]


def public_suffix_list(kit, version):
    """The Public Suffix List version and upstream commit the kit's qtbase SBOM records."""
    path = kit / "sbom" / f"qtbase-{version}.spdx.json"
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise DirectionsError(f"cannot read {path}: {error}") from error
    packages = [p for p in document.get("packages", []) if p.get("name") == PSL_PACKAGE]
    if len(packages) != 1:
        raise DirectionsError(f"{path} does not record the Public Suffix List ({PSL_PACKAGE}) once")
    package = packages[0]
    commits = {match[1] for ref in package.get("externalRefs", [])
               if (match := PSL_PURL.match(str(ref.get("referenceLocator", ""))))}
    psl_version = package.get("versionInfo", "")
    if len(commits) != 1 or not re.fullmatch(r"[\w.:-]+", psl_version):
        raise DirectionsError(f"{path} gives no single version and commit for the Public Suffix List")
    return psl_version, commits.pop()


def directions(version, kit, apk, dsd_source):
    match = VERSION.fullmatch(version)
    if not match:
        raise DirectionsError(f"not a Qt release version: {version!r}")
    reported = kit_version(kit)
    if reported != version:
        raise DirectionsError(f"the Qt kit {kit} is Qt {reported}, not Qt {version}")
    for module in QT_MODULES:
        if not (kit / "sbom" / f"{module}-{version}.spdx.json").is_file():
            raise DirectionsError(f"the Qt kit {kit} has no {module} {version} SBOM")
    psl_version, psl_commit = public_suffix_list(kit, version)
    base = f"https://download.qt.io/archive/qt/{match[1]}.{match[2]}/{version}/submodules"
    archives = "\n".join(f"- {module}: {base}/{module}-everywhere-src-{version}.tar.xz" for module in QT_MODULES)
    return f"""## Qt source code for {apk}

This APK contains Qt {version} for Android (android_arm64_v8a): The Qt Company's prebuilt
libraries, plugins and QML modules, unmodified, and Qt's Java code, which the build compiles
into the app's classes.dex. dsd-neo applies no patches to Qt's source. dsd-neo uses Qt under the
GNU Lesser General Public License version 3; the APK carries its text and Qt's notices
(assets/doc/dsd-neo/licenses/LGPL-3.0-only.txt and Qt-NOTICES.txt).

The source code of the Qt {version} modules in this APK is in Qt's official source archives:

{archives}

Each archive's SHA-256 checksum is in the file of the same name with .sha256 added, in the same
folder.

Qt Network in this APK contains the Public Suffix List (Mozilla Public License 2.0), version
{psl_version}, in a compiled form. Its source form is public_suffix_list.dat at commit
{psl_commit} of https://github.com/publicsuffix/list:
https://github.com/publicsuffix/list/blob/{psl_commit}/public_suffix_list.dat

These files are on The Qt Company's and the Public Suffix List's servers, not dsd-neo's.
dsd-neo's own source code and the build scripts that combine it with Qt are at
{dsd_source}
"""


def merge_notes(body, section):
    """The release notes with one copy of the directions, replacing an earlier one."""
    body = body.replace("\r\n", "\n")
    start, end = body.find(BEGIN), body.find(END)
    if start >= 0 and end > start:
        body = body[:start] + body[end + len(END):]
    elif start >= 0 or end >= 0:
        raise DirectionsError("the release notes hold an unmatched Qt source directions marker")
    body = body.rstrip("\n")
    block = f"{BEGIN}\n{section.strip()}\n{END}\n"
    return f"{body}\n\n{block}" if body else block


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    write = commands.add_parser("directions", help="write the directions for one APK")
    write.add_argument("--qt-version", required=True, help="the Qt version the build installed")
    write.add_argument("--qt-kit", type=Path, required=True, help="the android_arm64_v8a kit the build used")
    write.add_argument("--apk", required=True, help="the APK's file name")
    write.add_argument("--dsd-source", required=True, help="where dsd-neo's source for this APK is")
    write.add_argument("--output", type=Path, required=True)
    notes = commands.add_parser("notes", help="put the directions into release notes")
    notes.add_argument("--body", type=Path, required=True, help="the release notes as they are")
    notes.add_argument("--directions", type=Path, required=True, help="the file the directions command wrote")
    notes.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.command == "directions":
            text = directions(args.qt_version, args.qt_kit, args.apk, args.dsd_source)
        else:
            text = merge_notes(args.body.read_text(encoding="utf-8"), args.directions.read_text(encoding="utf-8"))
    except DirectionsError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    args.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
