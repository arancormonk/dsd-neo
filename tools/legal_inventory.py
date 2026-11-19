#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check retained legal evidence or supplement a release SPDX 2.3 inventory.

An inventory check is not legal clearance. Unresolved findings remain visible
in installed records and SBOMs instead of becoming an aggregate GPL assertion.
"""

import argparse
import hashlib
import json
import re
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = ("linux", "macos", "windows", "android")
REQUIRED = {"project", "legacy-isc", "bch3", "rockliff-rs", "wallace-golay",
            "mixed-origins", "ezpwd", "tiny-aes", "pffft", "tetra-crypto",
            "mbelib", "android-libusb", "android-rtl", "desktop-rtl", "airspy", "ibm-plex", "recordings",
            "gqrx-scanner", "soapysdr", "soapy-airspy", "soapy-hackrf", "libhackrf", "libusb",
            "pthreads4w", "msvc-runtime", "vcpkg-ports", "soapysdr-util", "codec2", "portaudio", "pdcurses", "ncurses",
            "openssl", "curl", "expat", "zlib", "libsndfile", "libflac", "libogg", "libvorbis", "opus", "mp3lame",
            "mpg123", "qt", "ndk-runtime", "androidx", "kotlin"}
# The notices each package carries for a component, under its documentation folder, and where its staging takes
# each one from: the record's <platform>_release map. A source is the repository or the configured build tree (each
# a relative path under it), the copyright file a vcpkg port installs (share/<port>/copyright) or the package's
# general legal files (legal-component: the legal CMake component on the desktop, the copies of LICENSES/, legal/
# and mbelib's notices the APK staging makes).
RELEASE_MAPS = {"windows": "windows_release", "android": "android_release", "macos": "macos_release"}
# The packages whose staging reads their maps, so every component they carry has one. A platform joins once its
# staging and its package check read the map (cmake/stage_release_notices.cmake, --release-notices).
COMPLETE_RELEASE_MAPS = {"windows", "android", "macos"}
NOTICE_PATH = re.compile(r"[\w.+/-]+")
VCPKG_PORT = re.compile(r"[a-z0-9-]+")
# A file name Windows can load, as tools/windows/release_tree_checks.psm1 (Test-ImportName) accepts an import: no
# folder, none of the characters Windows refuses in a name, not ending in a dot or a space. Records list it in
# lower case, the one spelling of a name Windows compares without regard to case.
RELEASE_FILE = re.compile(r'[^<>:"/\\|?*\x00-\x1f]*[^<>:"/\\|?*\x00-\x1f. ]')
RUNTIME_DLL = re.compile(r"(concrt|msvcp|vccorlib|vcruntime)140(_[a-z0-9_]+)?\.dll")
# Components the packages carry with no notice of their own: code compiled into dsd-neo whose grant is unresolved.
NO_RELEASE_NOTICE = {"wallace-golay"}


def notice_source(source):
    """Whether staging can take a notice from this source: repo: and build: name a path inside their root."""
    if source == "legal-component":
        return True
    kind, _, rest = source.partition(":")
    if kind == "vcpkg":
        return bool(VCPKG_PORT.fullmatch(rest))
    if kind not in ("repo", "build") or not NOTICE_PATH.fullmatch(rest):
        return False
    return all(part not in ("", ".", "..") for part in rest.split("/"))


def read_inventory(root=ROOT):
    return json.loads((root / "legal/components.json").read_text(encoding="utf-8"))


def read_pins(root=ROOT):
    result = {}
    for line in (root / "tools/ci-dependency-pins.env").read_text(encoding="utf-8").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            if key in result:
                raise ValueError(f"Duplicate dependency pin: {key}")
            result[key] = value
    return result


def validate(inventory, root=ROOT):
    if inventory.get("schema_version") != 1 or inventory.get("aggregate_license_concluded") != "NOASSERTION":
        raise ValueError("Expected schema 1 and an unresolved aggregate conclusion")
    version = re.search(r"project\(dsd-neo VERSION (\d+\.\d+\.\d+)",
                        (root / "CMakeLists.txt").read_text(encoding="utf-8"))[1]
    if inventory.get("release") != version:
        raise ValueError("Update the legal review for the project release")
    pins = read_pins(root)
    status = (root / "legal/status.md").read_text(encoding="utf-8")
    ids = set()
    for component in inventory["components"]:
        ident = component["id"]
        if ident in ids or not re.fullmatch(r"[a-z0-9-]+", ident):
            raise ValueError(f"Invalid or duplicate component: {ident}")
        ids.add(ident)
        date.fromisoformat(component["review_date"])
        if not set(component["platforms"]) <= set(PLATFORMS):
            raise ValueError(f"Unknown component platform: {ident}")
        for notice in component["notices"]:
            path = Path(notice)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError("Notice must be a repository-relative path")
            content = (root / path).read_bytes().replace(b"\r\n", b"\n")
            if not content.strip() or hashlib.sha256(content).hexdigest() != component["notice_sha256"].get(notice):
                raise ValueError(f"Missing or altered notice: {notice}")
        if (component["copyright_permission"] == "unresolved"
                and (component["license_concluded"] != "NOASSERTION"
                     or component.get("finding", "MISSING") not in status)):
            raise ValueError(f"Unresolved component lacks a faithful disclosure: {ident}")
        for custom in re.findall(r"LicenseRef-[\w.-]+", component["license_declared"]):
            if not any(Path(path).stem == custom for path in component["notices"]):
                raise ValueError(f"Custom license has no retained text: {custom}")
        if "pin" in component and not re.fullmatch(r"[0-9a-f]{40}", pins.get(component["pin"], "")):
            raise ValueError(f"Missing immutable source revision: {ident}")
        validate_windows_release(component, root)
        for platform in RELEASE_MAPS:
            validate_release_notices(component, platform)
    if ids != REQUIRED:
        raise ValueError("Reviewed component inventory changed; review required coverage")
    return pins


def validate_windows_release(component, root=ROOT):
    """Every component of the Windows ZIP lists the files it is in (its notices: validate_release_notices)."""
    release = component.get("windows_release")
    if release is None or not ships(component, "windows"):
        return
    ident = component["id"]
    files = release.get("binaries")
    if (not isinstance(files, list) or not files or len(set(files)) != len(files)
            or not all(isinstance(name, str) and RELEASE_FILE.fullmatch(name) and name == name.lower()
                       for name in files)):
        raise ValueError(f"Release files must be distinct lower-case Windows file names: {ident}")
    if ident == "msvc-runtime" and not all(RUNTIME_DLL.fullmatch(name) for name in files):
        raise ValueError("The Visual C++ runtime record lists a file outside a toolset's runtime folder")
    if release.get("notices") == {} and files != ["dsd-neo.exe"]:
        raise ValueError(f"The ZIP carries no notice for a library it ships: {ident}")


def ships(component, platform):
    return platform in component["platforms"] and "binary" in component["distribution"]


def validate_release_notices(component, platform):
    """A component a package carries names the notices that package carries for it and where each comes from."""
    ident, key = component["id"], RELEASE_MAPS[platform]
    release = component.get(key)
    if release is None:
        if ships(component, platform) and platform in COMPLETE_RELEASE_MAPS:
            raise ValueError(f"A {platform} binary component lists no release notices ({key}): {ident}")
        return
    if not ships(component, platform):
        raise ValueError(f"Only a component the {platform} package carries has {key}: {ident}")
    if not isinstance(release, dict) or (platform != "windows" and set(release) != {"notices"}):
        raise ValueError(f"{key} must hold only its notices: {ident}")
    notices = release.get("notices")
    if not isinstance(notices, dict):
        raise ValueError(f"Release notices must map a path to its source: {ident}")
    if not notices and not (ident in NO_RELEASE_NOTICE and component["copyright_permission"] == "unresolved"):
        raise ValueError(f"The {platform} package carries no notice for a component it ships: {ident}")
    for notice, source in notices.items():
        path = Path(notice)
        if (path.is_absolute() or ".." in path.parts or "\\" in notice or not notice
                or not isinstance(source, str) or not notice_source(source)):
            raise ValueError(f"Invalid release notice {notice!r} from {source!r}: {ident}")
        if source.startswith("repo:") and source[5:] not in component["notices"]:
            raise ValueError(f"Release notice is not a retained notice of the record: {source} ({ident})")


def release_notices(inventory, platform):
    """The notices a platform's package carries, as paths under its documentation folder."""
    return sorted({notice for component in inventory["components"]
                   for notice in component.get(RELEASE_MAPS[platform], {}).get("notices", {})})


def add_spdx(document, inventory, pins, platform):
    if document.get("spdxVersion") != "SPDX-2.3" or "SPDXID" not in document:
        raise ValueError("Expected an SPDX 2.3 document")
    if platform not in PLATFORMS:
        raise ValueError("Unknown release platform")
    aggregate = "SPDXRef-dsd-reviewed-artifact"
    packages = document.setdefault("packages", [])
    if any(p.get("SPDXID", "").startswith("SPDXRef-dsd-reviewed-") for p in packages):
        raise ValueError("SBOM already includes the reviewed component inventory")
    packages.append({
        "name": "dsd-neo reviewed component set", "SPDXID": aggregate,
        "versionInfo": inventory["release"], "downloadLocation": "NOASSERTION",
        "filesAnalyzed": False, "licenseConcluded": "NOASSERTION", "licenseDeclared": "NOASSERTION",
        "copyrightText": "NOASSERTION",
        "comment": "Supplemental inventory, not exhaustive runtime coverage or legal clearance. "
                   "See https://github.com/arancormonk/dsd-neo/blob/v" + inventory["release"] + "/legal/status.md",
    })
    relationships = document.setdefault("relationships", [])
    relationships.append({"spdxElementId": document["SPDXID"], "relationshipType": "DESCRIBES",
                          "relatedSpdxElement": aggregate})
    for component in inventory["components"]:
        if "binary" not in component["distribution"] or platform not in component["platforms"]:
            continue
        ident = "SPDXRef-dsd-reviewed-" + component["id"]
        package = {
            "name": component["name"], "SPDXID": ident, "downloadLocation": "NOASSERTION",
            "filesAnalyzed": False, "licenseDeclared": component["license_declared"],
            "licenseConcluded": component["license_concluded"], "copyrightText": "NOASSERTION",
            "sourceInfo": component["assessment"],
        }
        if "finding" in component:
            package["licenseComments"] = "Unresolved finding: " + component["finding"]
        if "pin" in component:
            package["versionInfo"] = pins[component["pin"]]
        packages.append(package)
        relationships.append({"spdxElementId": aggregate, "relationshipType": "CONTAINS",
                              "relatedSpdxElement": ident})
    return document


def extracted_licenses(document, inventory, root=ROOT):
    extracted = document.setdefault("hasExtractedLicensingInfos", [])
    known = {record["licenseId"]: record for record in extracted}
    used = " ".join(p.get("licenseDeclared", "") for p in document["packages"])
    for component in inventory["components"]:
        for custom in re.findall(r"LicenseRef-[\w.-]+", component["license_declared"]):
            if custom not in used:
                continue
            notice = next(path for path in component["notices"] if Path(path).stem == custom)
            content = (root / notice).read_text(encoding="utf-8")
            if custom in known:
                if known[custom]["extractedText"] != content:
                    raise ValueError(f"Conflicting extracted license: {custom}")
                continue
            record = {"licenseId": custom, "extractedText": content, "name": custom}
            extracted.append(record)
            known[custom] = record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spdx", type=Path, help="supplement an SPDX JSON before attestation")
    parser.add_argument("--platform", choices=PLATFORMS)
    parser.add_argument("--release-notices", choices=sorted(RELEASE_MAPS), metavar="PLATFORM",
                        help="print the notices the platform's package carries, one path per line")
    args = parser.parse_args()
    if bool(args.spdx) != bool(args.platform):
        parser.error("--spdx and --platform must be supplied together")
    if args.release_notices and args.spdx:
        parser.error("--release-notices stands alone")
    inventory = read_inventory()
    pins = validate(inventory)
    if args.release_notices:
        for notice in release_notices(inventory, args.release_notices):
            print(notice)
        return
    if args.spdx:
        document = add_spdx(json.loads(args.spdx.read_text(encoding="utf-8")), inventory, pins, args.platform)
        extracted_licenses(document, inventory)
        args.spdx.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print("Legal records checked; unresolved findings remain open (not legal clearance).")


if __name__ == "__main__":
    main()
