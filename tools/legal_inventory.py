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
            "mbelib", "android-libusb", "android-rtl", "airspy", "ibm-plex", "recordings", "gqrx-scanner"}


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
    if ids != REQUIRED:
        raise ValueError("Reviewed component inventory changed; review required coverage")
    return pins


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
    args = parser.parse_args()
    if bool(args.spdx) != bool(args.platform):
        parser.error("--spdx and --platform must be supplied together")
    inventory = read_inventory()
    pins = validate(inventory)
    if args.spdx:
        document = add_spdx(json.loads(args.spdx.read_text(encoding="utf-8")), inventory, pins, args.platform)
        extracted_licenses(document, inventory)
        args.spdx.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    print("Legal records checked; unresolved findings remain open (not legal clearance).")


if __name__ == "__main__":
    main()
