#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for truthful component metadata and installed dependency notices."""

import copy
import importlib.util
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
SPEC = importlib.util.spec_from_file_location("legal_inventory", ROOT / "tools/legal_inventory.py")
legal = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(legal)

MBE_NOTICES = (
    "LICENSE", "GPL-2.0-or-later.txt", "ISC.txt", "LicenseRef-PFFFT.txt",
    "LicenseRef-ETSI-EN-300-395-2-data.txt", "upstream-mbelib-attribution.md",
    "pffft/LICENSE.txt", "pffft/PFFFT-NOTICES.txt", "legal/components.json",
    "legal/status.md", "legal/patents.md",
)


def mbelib_notice_fixture(directory):
    for name in MBE_NOTICES:
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("Retained dependency notice fixture: " + name + "\n", encoding="utf-8")
    return directory


class InventoryTests(unittest.TestCase):
    def setUp(self):
        self.inventory = legal.read_inventory(ROOT)
        self.pins = legal.validate(self.inventory, ROOT)

    def test_missing_notice_and_silent_clearance_rejected(self):
        for alteration in ("hash", "conclusion", "finding", "duplicate", "coverage", "release"):
            inventory = copy.deepcopy(self.inventory)
            bch = next(c for c in inventory["components"] if c["id"] == "bch3")
            if alteration == "hash":
                bch["notice_sha256"][bch["notices"][0]] = "0" * 64
            elif alteration == "conclusion":
                bch["license_concluded"] = "ISC"
            elif alteration == "finding":
                bch["finding"] = "nonexistent-finding"
            elif alteration == "duplicate":
                inventory["components"].append(bch)
            elif alteration == "coverage":
                inventory["components"].remove(bch)
            else:
                inventory["release"] = "0.0.0"
            with self.subTest(alteration=alteration), self.assertRaises(ValueError):
                legal.validate(inventory, ROOT)

    def test_binary_sboms_preserve_uncertainty_and_dependency_identity(self):
        for platform in legal.PLATFORMS:
            document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
            legal.add_spdx(document, self.inventory, self.pins, platform)
            legal.extracted_licenses(document, self.inventory, ROOT)
            packages = {p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-"): p for p in document["packages"]}
            for ident in ("artifact", "bch3", "wallace-golay", "mixed-origins", "mbelib"):
                self.assertEqual(packages[ident]["licenseConcluded"], "NOASSERTION")
            self.assertEqual(packages["mbelib"]["versionInfo"], self.pins["MBELIB_NEO_SHA"])
            self.assertNotIn("recordings", packages)
            self.assertEqual("android-libusb" in packages, platform == "android")
            self.assertEqual("ibm-plex" in packages, platform == "android")
            notices = {n["licenseId"]: n["extractedText"] for n in document["hasExtractedLicensingInfos"]}
            self.assertIn("free for non-commercial purposes", notices["LicenseRef-BCH3-Noncommercial"])
            self.assertIn("author's contribution", notices["LicenseRef-Simon-Rockliff"])
            with self.assertRaises(ValueError):
                legal.add_spdx(document, self.inventory, self.pins, platform)

    def test_scanner_records_are_preserved_and_conflicting_licenses_fail(self):
        scanner = {"SPDXID": "SPDXRef-scanned-library", "name": "library"}
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": [scanner.copy()]}
        legal.add_spdx(document, self.inventory, self.pins, "linux")
        self.assertEqual(document["packages"][0], scanner)
        document["hasExtractedLicensingInfos"] = [
            {"licenseId": "LicenseRef-BCH3-Noncommercial", "extractedText": "unrestricted"}]
        with self.assertRaises(ValueError):
            legal.extracted_licenses(document, self.inventory, ROOT)

    def test_version_and_mbelib_overlay_identity(self):
        manifest = json.loads((ROOT / "vcpkg.json").read_text())
        self.assertEqual(manifest["version-string"], self.inventory["release"])
        overlay = (ROOT / "vcpkg-ports/mbe-neo/portfile.cmake").read_text()
        self.assertIn("REF " + self.pins["MBELIB_NEO_SHA"], overlay)
        self.assertIn("SHA512 " + self.pins["MBELIB_NEO_TARBALL_SHA512"], overlay)
        self.assertIn(self.pins["MBELIB_NEO_SHA"], (ROOT / "legal/status.md").read_text())
        manifest = json.loads((ROOT / "vcpkg-ports/mbe-neo/vcpkg.json").read_text())
        self.assertIsNone(manifest["license"])
        for name in ("ISC.txt", "LicenseRef-PFFFT.txt", "LicenseRef-ETSI-EN-300-395-2-data.txt"):
            self.assertIn(name, overlay)


@unittest.skipUnless(shutil.which("cmake"), "cmake is required")
class NoticeStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dsd-legal-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)
        self.mbe = mbelib_notice_fixture(self.work / "prefix/share/doc/mbelib-neo")

    def run_cmake(self, *args):
        return subprocess.run(["cmake", *map(str, args)], cwd=self.work,
                              text=True, capture_output=True, check=False)

    def test_installed_mbelib_layouts_and_missing_notices(self):
        for config in ("lib/cmake/mbe-neo", "lib64/cmake/mbe-neo", "share/mbe-neo",
                       "lib/x86_64-linux-gnu/cmake/mbe-neo"):
            directory = self.work / "prefix" / config
            directory.mkdir(parents=True, exist_ok=True)
            result = self.run_cmake(f"-Dmbe-neo_DIR={directory}", "-P", ROOT / "cmake/MbelibNotices.cmake")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        notice = self.mbe / "LicenseRef-ETSI-EN-300-395-2-data.txt"
        notice.unlink()
        result = self.run_cmake(f"-DDSD_MBELIB_NOTICE_DIR={self.mbe}", "-P", ROOT / "cmake/MbelibNotices.cmake")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(notice.name, result.stderr)

    def test_android_assets_retain_full_notice_tree_and_fail_on_missing_dependency_text(self):
        package = self.work / "package"
        package.mkdir()
        notices = self.work / "notices"
        notices.mkdir()
        for name in ("pffft-LICENSE.txt", "pffft-NOTICES.txt", "tetra_crypto-LICENSE.txt"):
            (notices / name).write_text(name + "\n", encoding="utf-8")
        stage = self.work / "stage"
        args = [f"-DREPO_DIR={ROOT}", f"-DSOURCE_DIR={package}", f"-DSTAGE_DIR={stage}",
                f"-DPFFFT_LICENSE_DIR={notices}", f"-DTETRA_CRYPTO_LICENSE_DIR={notices}",
                f"-DMBELIB_NOTICE_DIR={self.mbe}", "-P", ROOT / "cmake/stage_android_package.cmake"]
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        docs = stage / "assets/doc/dsd-neo"
        for name in MBE_NOTICES:
            self.assertEqual((docs / "licenses/mbelib-neo" / name).read_bytes(), (self.mbe / name).read_bytes())
        for name in ("status.md", "source-delivery.md", "components.json"):
            self.assertEqual((docs / "legal" / name).read_bytes(), (ROOT / "legal" / name).read_bytes())
        self.assertEqual((docs / "licenses/ISC.txt").read_bytes(), (ROOT / "LICENSES/ISC.txt").read_bytes())
        # A stale destination must not hide an incomplete new dependency install.
        (self.mbe / "legal/status.md").unlink()
        self.assertNotEqual(self.run_cmake(*args).returncode, 0)


if __name__ == "__main__":
    unittest.main()
