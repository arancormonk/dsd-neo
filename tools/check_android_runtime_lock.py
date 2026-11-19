#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check an Android app build against legal/android-runtime-lock.json.

The qt, ndk-runtime, androidx, kotlin and project records in legal/components.json were reviewed for one Qt kit, one
NDK, one Gradle release runtime classpath and one set of native libraries in the APK. The lock file holds those facts;
this check fails when the build differs, so a Qt, NDK, AGP or AndroidX update cannot ship under stale records:

- the Qt kit the build was configured with (its Qt6Core_DIR): QT_VERSION in mkspecs/qconfig.pri, and the
  documentNamespace of The Qt Company's build SBOM of each locked module (a kit rebuilt at the same version differs);
- the NDK the build was configured with (ANDROID_NDK_ROOT): Pkg.Revision and Pkg.ReleaseName in source.properties;
- the release runtime classpath, from the output of
  `gradlew dependencies --configuration releaseRuntimeClasspath` in the generated Gradle project;
- the Java libraries build.gradle takes from the generated Gradle project's libs/ (libs/*.jar and libs/*.aar, which
  that report does not list): their names and SHA-256;
- the native libraries the APK carries, all in lib/<abi>/; each Qt one is the file of that name the locked kit's
  build SBOMs record, by SHA-1.

Usage:
  tools/check_android_runtime_lock.py --build-dir build/android-app \\
      --gradle-dependencies release-runtime.txt --apk app.apk \\
      --gradle-libs build/android-app/android/android-build/libs
"""

import argparse
import hashlib
import json
import re
import sys
import zipfile
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[1]
CONFIGURATION = "releaseRuntimeClasspath"
TREE_LINE = re.compile(r"^[ |]*[+\\]--- (?P<dependency>.+)$")
MARKER = re.compile(r" \((?:\*|c|n)\)$")


def read_cache(build_dir):
    """The CMake cache entries of @p build_dir, name to value."""
    entries = {}
    for line in (build_dir / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([A-Za-z_][\w.+-]*):[A-Z_]+=(.*)$", line)
        if match:
            entries[match[1]] = match[2]
    return entries


def read_properties(path, separator):
    """Key/value lines of @p path split at the first @p separator, both sides stripped."""
    values = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if separator in line and not line.lstrip().startswith("#"):
            key, value = line.split(separator, 1)
            values[key.strip()] = value.strip()
    return values


def parse_dependency_report(text):
    """The group:name:version set Gradle resolved for the release runtime classpath, from its dependency report."""
    lines = text.splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith(CONFIGURATION + " ")), None)
    if start is None:
        raise ValueError(f"The dependency report has no {CONFIGURATION} section")
    resolved = set()
    for line in lines[start + 1:]:
        if not line.strip():
            break
        match = TREE_LINE.match(line)
        if not match:
            raise ValueError(f"Unexpected line in the {CONFIGURATION} report: {line.strip()}")
        dependency = match["dependency"]
        if dependency.endswith(" (c)"):
            continue  # a constraint, not a dependency
        if dependency.endswith(" (n)") or dependency.endswith(" FAILED"):
            raise ValueError(f"Gradle did not resolve {dependency}")
        dependency = MARKER.sub("", dependency)
        requested, _, version = dependency.partition(" -> ")
        parts = requested.split(":")
        if len(parts) not in (2, 3) or not all(parts[:2]) or requested.startswith("project "):
            raise ValueError(f"Not a published library: {dependency}")
        if not version:
            if len(parts) != 3:
                raise ValueError(f"No resolved version: {dependency}")
            version = parts[2]
        resolved.add(f"{parts[0]}:{parts[1]}:{version}")
    if not resolved:
        raise ValueError(f"The {CONFIGURATION} report resolves no library")
    return resolved


def check_records(lock, inventory):
    """Every locked record is a reviewed Android binary record whose name carries the locked version."""
    problems = []
    records = {component["id"]: component for component in inventory["components"]}
    for ident in lock["records"]:
        record = records.get(ident)
        if record is None or "android" not in record["platforms"] or "binary" not in record["distribution"]:
            problems.append(f"The lock describes {ident}, which is no reviewed record of an Android binary")
    qt, ndk = lock["records"].get("qt"), lock["records"].get("ndk-runtime")
    if qt is None or ndk is None:
        problems.append("The lock must describe the qt and ndk-runtime records")
        return problems
    if "qt" in records and f"Qt {qt['version']}" not in records["qt"]["name"]:
        problems.append(f"The qt record ({records['qt']['name']}) does not name the locked Qt {qt['version']}")
    if "ndk-runtime" in records and ndk["release"] not in records["ndk-runtime"]["name"]:
        problems.append(f"The ndk-runtime record ({records['ndk-runtime']['name']}) does not name the locked NDK "
                        f"{ndk['release']}")
    return problems


def check_qt(lock, cache):
    """The problems of the build's Qt kit, and the SHA-1 of each file its locked build SBOMs record, by file name."""
    qt = lock["records"]["qt"]
    core_dir = cache.get("Qt6Core_DIR")
    if not core_dir:
        return ["The build's CMake cache names no Qt6Core_DIR"], {}
    kit = Path(core_dir).parents[2]
    version = read_properties(kit / "mkspecs/qconfig.pri", "=").get("QT_VERSION")
    if version != qt["version"]:
        return [f"The build uses Qt {version} from {kit}; the records describe Qt {qt['version']}"], {}
    problems, digests = [], {}
    for module, namespace in sorted(qt["sbom_namespaces"].items()):
        sbom = kit / "sbom" / f"{module}-{version}.spdx.json"
        try:
            document = json.loads(sbom.read_text(encoding="utf-8"))
        except (OSError, ValueError) as error:
            problems.append(f"Cannot read the {module} build SBOM of the Qt kit: {error}")
            continue
        found = document.get("documentNamespace")
        if found != namespace:
            problems.append(f"The Qt kit's {module} is another build than the records describe: {found}")
            continue
        for record in document.get("files", []):
            name = PurePosixPath(str(record.get("fileName", ""))).name
            for checksum in record.get("checksums", []):
                if checksum.get("algorithm") == "SHA1":
                    digests.setdefault(name, set()).add(checksum.get("checksumValue"))
    return problems, digests


def check_ndk(lock, cache):
    ndk = lock["records"]["ndk-runtime"]
    root = cache.get("ANDROID_NDK_ROOT") or cache.get("CMAKE_ANDROID_NDK")
    if not root:
        return ["The build's CMake cache names no ANDROID_NDK_ROOT"]
    properties = read_properties(Path(root) / "source.properties", "=")
    found = (properties.get("Pkg.Revision"), properties.get("Pkg.ReleaseName"))
    if found != (ndk["revision"], ndk["release"]):
        return [f"The build uses NDK {found[0]} ({found[1]}); the records describe {ndk['revision']} "
                f"({ndk['release']})"]
    return []


def check_gradle(lock, report):
    expected = {coordinate for facts in lock["records"].values()
                for coordinate in facts.get("release_runtime_classpath", [])}
    resolved = parse_dependency_report(report)
    problems = [f"Gradle resolves {c}, which no record describes" for c in sorted(resolved - expected)]
    problems += [f"Gradle no longer resolves {c}, which the records describe" for c in sorted(expected - resolved)]
    return problems


def check_gradle_libs(lock, libs):
    """The Java libraries in the generated Gradle project's libs/ against the names and SHA-256 the lock gives."""
    expected = {name: digest for facts in lock["records"].values()
                for name, digest in facts.get("gradle_libs", {}).items()}
    found = {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in sorted(libs.iterdir())
             if path.is_file() and path.suffix in (".jar", ".aar")}
    problems = [f"Gradle's libs/ holds {n}, which no record describes" for n in sorted(set(found) - set(expected))]
    problems += [f"Gradle's libs/ lacks {n}, which the records describe" for n in sorted(set(expected) - set(found))]
    problems += [f"Gradle's libs/{n} is another file than the records describe (SHA-256 {found[n]})"
                 for n in sorted(set(found) & set(expected)) if found[n] != expected[n]]
    return problems


def check_apk(lock, apk, qt_digests=None):
    """The APK's native libraries against the lock; with @p qt_digests, each Qt one against the kit's SBOM SHA-1."""
    abi = lock["abi"]
    expected = {name for facts in lock["records"].values() for name in facts.get("libraries", [])}
    qt = set(lock["records"]["qt"].get("libraries", []))
    found, problems = set(), []
    with zipfile.ZipFile(apk) as archive:
        for entry in archive.namelist():
            path = PurePosixPath(entry)
            if path.suffix != ".so":
                continue
            if path.parts[:2] != ("lib", abi) or len(path.parts) != 3:
                problems.append(f"The APK carries {entry}, outside lib/{abi}/")
                continue
            found.add(path.name)
            if qt_digests is not None and path.name in qt:
                digest = hashlib.sha1(archive.read(entry)).hexdigest()
                if digest not in qt_digests.get(path.name, set()):
                    problems.append(f"The APK's {entry} (SHA-1 {digest}) is not the file of that name the locked "
                                    "Qt kit's build SBOMs record")
    problems += [f"The APK carries lib/{abi}/{n}, which no record describes" for n in sorted(found - expected)]
    problems += [f"The APK lacks lib/{abi}/{n}, which the records describe" for n in sorted(expected - found)]
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, required=True, help="the configured Android app build")
    parser.add_argument("--gradle-dependencies", type=Path, required=True,
                        help=f"the output of gradlew dependencies --configuration {CONFIGURATION}")
    parser.add_argument("--apk", type=Path, required=True, help="the release APK")
    parser.add_argument("--gradle-libs", type=Path, required=True,
                        help="the generated Gradle project's libs/ folder, whose *.jar and *.aar build.gradle uses")
    parser.add_argument("--lock", type=Path, default=ROOT / "legal/android-runtime-lock.json")
    parser.add_argument("--components", type=Path, default=ROOT / "legal/components.json")
    args = parser.parse_args(argv)

    lock = json.loads(args.lock.read_text(encoding="utf-8"))
    inventory = json.loads(args.components.read_text(encoding="utf-8"))
    problems = check_records(lock, inventory)
    if not problems:
        cache = read_cache(args.build_dir)
        qt_digests = None
        try:
            qt_problems, qt_digests = check_qt(lock, cache)
            problems += qt_problems
            problems += check_ndk(lock, cache)
            problems += check_gradle(lock, args.gradle_dependencies.read_text(encoding="utf-8"))
            problems += check_gradle_libs(lock, args.gradle_libs)
        except (OSError, ValueError) as error:
            problems.append(str(error))
        problems += check_apk(lock, args.apk, qt_digests)
    for problem in problems:
        print(f"ERROR: {problem}", file=sys.stderr)
    if problems:
        print("The build differs from what legal/android-runtime-lock.json and the records in legal/components.json "
              "describe: review the changed component, update its record, then the lock.", file=sys.stderr)
        return 1
    print("Qt kit, NDK runtime, Gradle release runtime classpath and local Java libraries, and APK native libraries "
          "match the legal records.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
