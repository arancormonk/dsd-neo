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

    def test_runtime_file_list_is_checked(self):
        # The Windows release scripts carry only the Visual C++ runtime DLLs this list names.
        for files in ([], ["msvcp140.dll", "msvcp140.dll"], ["MSVCP140.dll"], ["bin/msvcp140.dll"], "msvcp140.dll",
                      ["msvcp140.dll", "portaudio.dll"]):
            inventory = copy.deepcopy(self.inventory)
            runtime = next(c for c in inventory["components"] if c["id"] == "msvc-runtime")
            runtime["windows_release"]["binaries"] = files
            with self.subTest(files=files), self.assertRaises(ValueError):
                legal.validate(inventory, ROOT)

    def test_windows_release_records_are_checked(self):
        # Each component of the Windows ZIP names the files it is in and the notices the ZIP carries for it, and where
        # the staging takes each one from; a notice from the repository must be one the record retains.
        def edit(ident, change):
            inventory = copy.deepcopy(self.inventory)
            change(next(c for c in inventory["components"] if c["id"] == ident))
            return inventory

        cases = {
            "no release record": edit("portaudio", lambda c: c.pop("windows_release")),
            "no file": edit("portaudio", lambda c: c["windows_release"].update(binaries=[])),
            "a path for a file": edit("portaudio", lambda c: c["windows_release"].update(binaries=["bin/portaudio.dll"])),
            "an upper-case file": edit("portaudio", lambda c: c["windows_release"].update(binaries=["PortAudio.dll"])),
            "a notice outside the documentation": edit(
                "portaudio", lambda c: c["windows_release"].update(notices={"../portaudio.txt": "vcpkg:portaudio"})),
            "an unknown source": edit(
                "portaudio", lambda c: c["windows_release"].update(notices={"licenses/p.txt": "web:portaudio"})),
            "a repository notice the record does not retain": edit(
                "soapy-airspy", lambda c: c["windows_release"]["notices"].update(
                    {"licenses/extra.txt": "repo:LICENSES/ISC.txt"})),
            "a missing repository notice": edit(
                "soapy-airspy", lambda c: c["windows_release"]["notices"].update(
                    {"licenses/extra.txt": "repo:LICENSES/missing.txt"})),
            "a component the ZIP does not carry": edit(
                "ibm-plex", lambda c: c.update(windows_release={"binaries": ["dsd-neo.exe"], "notices": {}})),
            # A notice source must stay inside the root its prefix names, whatever follows the prefix.
            "a build notice above the build tree": edit(
                "portaudio", lambda c: c["windows_release"]["notices"].update({"licenses/x.txt": "build:../COPYRIGHT"})),
            "a repository notice above the repository": edit(
                "portaudio", lambda c: c["windows_release"]["notices"].update({"licenses/x.txt": "repo:../COPYRIGHT"})),
            "a repository notice through a parent folder": edit(
                "portaudio", lambda c: c["windows_release"]["notices"].update(
                    {"licenses/x.txt": "repo:LICENSES/../COPYRIGHT"})),
            "an absolute build notice": edit(
                "portaudio", lambda c: c["windows_release"]["notices"].update({"licenses/x.txt": "build:/COPYRIGHT"})),
            "a build notice with an empty part": edit(
                "portaudio", lambda c: c["windows_release"]["notices"].update(
                    {"licenses/x.txt": "build:pffft-licenses//pffft-LICENSE.txt"})),
            "a build notice through the current folder": edit(
                "portaudio", lambda c: c["windows_release"]["notices"].update(
                    {"licenses/x.txt": "build:./pffft-licenses/pffft-LICENSE.txt"})),
            # Every standalone library the ZIP carries ships a notice; only the recorded exception compiled into
            # dsd-neo.exe has none.
            "a standalone library without notices": edit("mpg123", lambda c: c["windows_release"].update(notices={})),
            "the runtime without notices": edit("msvc-runtime", lambda c: c["windows_release"].update(notices={})),
            "embedded code without notices": edit("tiny-aes", lambda c: c["windows_release"].update(notices={})),
            "the exception in a standalone file": edit(
                "wallace-golay", lambda c: c["windows_release"].update(binaries=["golay.dll"])),
        }
        for label, inventory in cases.items():
            with self.subTest(case=label), self.assertRaises(ValueError):
                legal.validate(inventory, ROOT)

    def test_release_files_may_be_any_name_windows_loads(self):
        # The release scripts follow imports whatever their suffix and stage names with spaces (Test-ImportName in
        # tools/windows/release_tree_checks.psm1), so a record can name every such file, in lower case.
        inventory = copy.deepcopy(self.inventory)
        portaudio = next(c for c in inventory["components"] if c["id"] == "portaudio")
        portaudio["windows_release"]["binaries"] = ["portaudio.dll", "vendor.drv", "vendor support.dll", "a.b-c+d_e"]
        legal.validate(inventory, ROOT)
        for name in ("Vendor Support.dll", "VENDOR.DRV", "vendor.dll.", "vendor.dll ", "bin/vendor.dll",
                     "bin\\vendor.dll", "c:vendor.dll", "vendor?.dll", "vendor*.dll", "vendor<.dll", "..", ".", "",
                     "vendor\x01.dll"):
            portaudio["windows_release"]["binaries"] = ["portaudio.dll", name]
            with self.subTest(name=name), self.assertRaises(ValueError):
                legal.validate(inventory, ROOT)
        # The Visual C++ runtime record keeps its own list of the files a toolset's runtime folder holds.
        runtime = next(c for c in inventory["components"] if c["id"] == "msvc-runtime")
        runtime["windows_release"]["binaries"] = ["msvcp140.dll", "vendor.drv"]
        portaudio["windows_release"]["binaries"] = ["portaudio.dll"]
        with self.assertRaises(ValueError):
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
            # The Windows SBOM names Microsoft's terms for the Visual C++ runtime and carries the ZIP's notice of them.
            runtime = "LicenseRef-Microsoft-Visual-CPP-Runtime"
            self.assertEqual(runtime in notices, platform == "windows")
            if platform == "windows":
                self.assertEqual((packages["msvc-runtime"]["licenseDeclared"],
                                  packages["msvc-runtime"]["licenseConcluded"]), (runtime, runtime))
                self.assertNotIn("licenseComments", packages["msvc-runtime"])
                self.assertIn("Do not modify them.", notices[runtime])
            # The custom texts of the Windows libraries come from their retained files. codec2's varicode grant is
            # read as the LGPL 2.1: its note file (LICENSES/LicenseRef-codec2-varicode.txt) records that reading and
            # adds no license to any SBOM.
            for custom in ("LicenseRef-PDCurses-Public-Domain", "LicenseRef-OpenSSL-KISA-SEED", "LicenseRef-Sun-G72x"):
                self.assertEqual(custom in notices, platform == "windows")
                if platform == "windows":
                    self.assertEqual(notices[custom], (ROOT / "LICENSES" / (custom + ".txt")).read_text(encoding="utf-8"))
            self.assertFalse(any("codec2" in name for name in notices))
            with self.assertRaises(ValueError):
                legal.add_spdx(document, self.inventory, self.pins, platform)

    def test_sboms_carry_codec2_where_it_ships(self):
        # The Windows ZIP (overlay DLL), the Android app (the same overlay, statically linked) and the AppImage (a
        # shared library built from the same commit) carry codec2; the DMG's comes from Homebrew and is not recorded.
        expression = "LGPL-2.1-only AND LGPL-2.1-or-later AND BSD-3-Clause AND BSD-2-Clause"
        for platform in legal.PLATFORMS:
            document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
            legal.add_spdx(document, self.inventory, self.pins, platform)
            packages = {p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-"): p for p in document["packages"]}
            with self.subTest(platform=platform):
                self.assertEqual("codec2" in packages, platform != "macos")
                if platform != "macos":
                    codec2 = packages["codec2"]
                    self.assertEqual((codec2["licenseDeclared"], codec2["licenseConcluded"]), (expression, expression))
                    self.assertEqual(codec2["versionInfo"], self.pins["CODEC2_SHA"])
                    self.assertNotIn("licenseComments", codec2)
        android = (ROOT / ".github/workflows/android-ci.yaml").read_text(encoding="utf-8")
        self.assertIn("grep -qE '(^| )codec2_create$'", android)
        self.assertIn('build_dep https://github.com/arancormonk/codec2 "$codec2_sha"',
                      (ROOT / ".github/workflows/linux-appimage.yaml").read_text(encoding="utf-8"))

    def test_windows_library_records_follow_their_sources(self):
        # The libraries vcpkg builds for the Windows ZIP: the license expression their pinned sources support, every
        # notice the ZIP needs for them retained, and no open finding.
        expected = {
            "codec2": "LGPL-2.1-only AND LGPL-2.1-or-later AND BSD-3-Clause AND BSD-2-Clause",
            "portaudio": "MIT",
            "pdcurses": "LicenseRef-PDCurses-Public-Domain",
            "openssl": "Apache-2.0 AND CC0-1.0 AND LicenseRef-OpenSSL-KISA-SEED",
            "curl": "curl AND ISC",
            "expat": "MIT AND CC0-1.0",
            "zlib": "Zlib",
            "libsndfile": "LGPL-2.1-or-later AND LGPL-2.0-or-later AND Apache-2.0 AND BSD-3-Clause AND TU-Berlin-1.0 "
                          "AND LicenseRef-Sun-G72x",
            "libflac": "BSD-3-Clause",
            "libogg": "BSD-3-Clause",
            "libvorbis": "BSD-3-Clause AND TU-Berlin-1.0",
            "opus": "BSD-3-Clause AND BSD-2-Clause",
            "mp3lame": "LGPL-2.0-or-later AND LGPL-2.1-or-later",
            "mpg123": "LGPL-2.1-only AND MirOS",
        }
        components = {c["id"]: c for c in self.inventory["components"]}
        for ident, expression in expected.items():
            component = components[ident]
            with self.subTest(component=ident):
                self.assertEqual((component["license_declared"], component["license_concluded"]),
                                 (expression, expression))
                self.assertEqual((component["copyright_permission"], component["gpl_compatibility"]),
                                 ("notice-retained", "reviewed-terms"))
                self.assertEqual(component["patent_status"], "not-cleared")
                self.assertNotIn("finding", component)
                self.assertNotIn("RUNTIME-NOTICES", component["assessment"])
                shipped = {source.removeprefix("repo:") for source in component["windows_release"]["notices"].values()
                           if source.startswith("repo:")}
                self.assertEqual(shipped, set(component["notices"]))
        self.assertEqual(json.loads((ROOT / "vcpkg-ports/codec2/vcpkg.json").read_text())["license"],
                         expected["codec2"])
        status = (ROOT / "legal/status.md").read_text(encoding="utf-8")
        open_findings = status[status.index("## Findings requiring resolution"):status.index("## Resolved findings")]
        runtime = next(line for line in open_findings.splitlines() if line.startswith("| RUNTIME-NOTICES |"))
        self.assertNotIn("is not reviewed", runtime)

    def test_usb_library_notices_name_every_holder_of_the_compiled_sources(self):
        # librtlsdr's and the Android libusb's license texts name no holder, so each package carries a notice with the
        # holder lines of every source and header the library compiles. The Android snapshot of librtlsdr has the
        # file headers of the pinned desktop release, so one notice serves every package.
        def holder_lines(paths):
            lines = set()
            for path in paths:
                for line in path.read_text(encoding="utf-8").splitlines()[:40]:
                    text = line.lstrip("/* ").strip()
                    if text.startswith(("Copyright", "(C)", "(c)")):
                        lines.add(text)
            return lines

        rtl = ROOT / "android/third_party/librtlsdr"
        libusb = ROOT / "android/third_party/libusb"
        cases = {
            "LICENSES/librtlsdr-NOTICES.txt": (
                ("desktop-rtl", "android-rtl"),
                sorted((rtl / "src").glob("*.c")) + sorted((rtl / "include").glob("*.h"))),
            "LICENSES/libusb-android-NOTICES.txt": (
                ("android-libusb",),
                sorted((libusb / "libusb").glob("*.[ch]")) + sorted((libusb / "libusb/os").glob("*.[ch]"))
                + [libusb / "config.h"]),
        }
        components = {c["id"]: c for c in self.inventory["components"]}
        for notice, (records, sources) in cases.items():
            text = (ROOT / notice).read_text(encoding="utf-8")
            with self.subTest(notice=notice):
                self.assertTrue(sources)
                for line in sorted(holder_lines(sources)):
                    self.assertIn(line, text)
                for ident in records:
                    self.assertIn(notice, components[ident]["notices"])
        self.assertEqual(components["desktop-rtl"]["windows_release"]["notices"].get("licenses/librtlsdr-NOTICES.txt"),
                         "repo:LICENSES/librtlsdr-NOTICES.txt")
        self.assertIn("licenses/librtlsdr-NOTICES.txt",
                      (ROOT / ".github/workflows/linux-appimage.yaml").read_text(encoding="utf-8"))
        android = (ROOT / ".github/workflows/android-ci.yaml").read_text(encoding="utf-8")
        for name in ("licenses/librtlsdr-NOTICES.txt", "licenses/libusb-android-NOTICES.txt"):
            self.assertIn(name, android)

    def test_sboms_name_only_the_soapysdr_programs_each_package_carries(self):
        # The AppImage builds SoapySDR without its programs (ENABLE_APPS=OFF); only the Windows ZIP carries
        # SoapySDRUtil.exe, so only its SBOM names it.
        appimage = (ROOT / ".github/workflows/linux-appimage.yaml").read_text(encoding="utf-8")
        self.assertIn("-DENABLE_APPS=OFF", appimage)
        for platform in legal.PLATFORMS:
            document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
            legal.add_spdx(document, self.inventory, self.pins, platform)
            names = [p["name"] for p in document["packages"]]
            with self.subTest(platform=platform):
                self.assertEqual("SoapySDR" in names, platform in ("linux", "windows"), names)
                self.assertEqual([n for n in names if "SoapySDRUtil" in n],
                                 ["SoapySDRUtil"] if platform == "windows" else [])

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
