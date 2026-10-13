#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate OSV commit input or add pinned TETRA_crypto provenance to an SPDX SBOM."""

import argparse
import json
from pathlib import Path
import re

PINS = Path(__file__).resolve().with_name("ci-dependency-pins.env")
REPO = "https://github.com/MidnightBlueLabs/TETRA_crypto"
PACKAGE_ID = "SPDXRef-Package-dsd-tetra-crypto"


def read_pins(path=PINS):
    text = path.read_text(encoding="utf-8")
    result = {}
    for name, size in (("TETRA_CRYPTO_SHA", 40), ("TETRA_CRYPTO_ARCHIVE_SHA256", 64)):
        values = re.findall(rf"^{name}=(.*)$", text, re.MULTILINE)
        if len(values) != 1 or not re.fullmatch(rf"[0-9a-f]{{{size}}}", values[0]):
            raise ValueError(f"Expected one valid {name} in {path}")
        result[name] = values[0]
    return result


def add_spdx(document, pins):
    sha = pins["TETRA_CRYPTO_SHA"]
    packages = document.setdefault("packages", [])
    if any(package.get("SPDXID") == PACKAGE_ID for package in packages):
        raise ValueError("SBOM already contains the pinned TETRA_crypto package")
    packages.append({
        "name": "TETRA_crypto", "SPDXID": PACKAGE_ID, "versionInfo": sha,
        "downloadLocation": f"{REPO}/archive/{sha}.tar.gz",
        "homepage": REPO, "filesAnalyzed": False,
        "licenseConcluded": "NOASSERTION", "licenseDeclared": "Apache-2.0",
        "copyrightText": "NOASSERTION",
        "sourceInfo": f"TEA1/TEA2/TEA3 sources compiled into dsd-neo from {REPO} at commit {sha}",
        "checksums": [{"algorithm": "SHA256", "checksumValue": pins["TETRA_CRYPTO_ARCHIVE_SHA256"]}],
        "externalRefs": [{"referenceCategory": "PACKAGE-MANAGER", "referenceType": "purl",
                          "referenceLocator": f"pkg:github/MidnightBlueLabs/TETRA_crypto@{sha}"}],
    })
    document.setdefault("relationships", []).append({
        "spdxElementId": document["SPDXID"], "relationshipType": "DESCRIBES",
        "relatedSpdxElement": PACKAGE_ID,
    })
    return document


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument("--osv", type=Path, help="write OSV custom lockfile")
    action.add_argument("--spdx", type=Path, help="enrich an SPDX JSON SBOM in place before attestation")
    args = parser.parse_args()
    pins = read_pins()
    if args.osv:
        document = {"results": [{"packages": [{"package": {
            "name": "github.com/MidnightBlueLabs/TETRA_crypto", "commit": pins["TETRA_CRYPTO_SHA"],
        }}]}]}
        output = args.osv
    else:
        output = args.spdx
        document = add_spdx(json.loads(output.read_text(encoding="utf-8")), pins)
    output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
