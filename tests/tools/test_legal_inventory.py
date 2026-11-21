#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for truthful component metadata and installed dependency notices."""

import copy
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
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

# LICENSES/Apache-2.0.txt is the apache.org text; every record that names Apache-2.0 retains this one file.
APACHE_SHA256 = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"


def apache_sha256(path):
    """The digest of a retained licence text, as the inventory check takes it."""
    return legal.notice_sha256(path)


# The libraries vcpkg links statically into the Android app.
VCPKG_ANDROID_PORTS = ("openssl", "curl", "zlib", "expat", "libsndfile", "libflac", "libogg", "libvorbis", "opus",
                       "mp3lame", "mpg123")


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

    def test_android_release_records_are_checked(self):
        # Every component the APK carries names the notices the APK carries for it and where staging takes each one
        # from (android_release), checked as windows_release is; the macOS map has the same shape.
        def edit(ident, change):
            inventory = copy.deepcopy(self.inventory)
            change(next(c for c in inventory["components"] if c["id"] == ident))
            return inventory

        for component in self.inventory["components"]:
            shipped = "android" in component["platforms"] and "binary" in component["distribution"]
            with self.subTest(component=component["id"]):
                self.assertEqual("android_release" in component, shipped)
        cases = {
            "no release record": edit("opus", lambda c: c.pop("android_release")),
            "no notice": edit("opus", lambda c: c["android_release"].update(notices={})),
            "notices that are not a map": edit("opus", lambda c: c["android_release"].update(notices=["x"])),
            "an unknown source": edit(
                "opus", lambda c: c["android_release"]["notices"].update({"licenses/p.txt": "web:opus"})),
            "a notice outside the documentation": edit(
                "opus", lambda c: c["android_release"]["notices"].update({"../opus.txt": "vcpkg:opus"})),
            "a repository notice the record does not retain": edit(
                "opus", lambda c: c["android_release"]["notices"].update({"licenses/x.txt": "repo:LICENSES/ISC.txt"})),
            "a repository notice above the repository": edit(
                "opus", lambda c: c["android_release"]["notices"].update({"licenses/x.txt": "repo:../COPYRIGHT"})),
            "a component the APK does not carry": edit(
                "portaudio", lambda c: c.update(android_release={"notices": {"licenses/p.txt": "vcpkg:portaudio"}})),
            "a source-only component": edit(
                "recordings", lambda c: c.update(android_release={"notices": {"LICENSE": "repo:LICENSE"}})),
            "binaries in a map that has none": edit(
                "opus", lambda c: c["android_release"].update(binaries=["libopus.so"])),
            "a macOS map for a component the DMG does not carry": edit(
                "pdcurses", lambda c: c.update(macos_release={"notices": {"licenses/p.txt": "vcpkg:pdcurses"}})),
            "an empty macOS map": edit("airspy", lambda c: c.update(macos_release={"notices": {}})),
            "a macOS component without a map": edit("ncurses", lambda c: c.pop("macos_release")),
            "a macOS map without its binaries": edit("ncurses", lambda c: c["macos_release"].pop("binaries")),
            "a macOS binary named with its suffix": edit(
                "ncurses", lambda c: c["macos_release"].update(binaries=["libncursesw.6.dylib"])),
            "a macOS binary in a folder": edit(
                "ncurses", lambda c: c["macos_release"].update(binaries=["lib/libformw"])),
            "a macOS binary listed twice": edit(
                "openssl", lambda c: c["macos_release"].update(binaries=["libssl", "libssl"])),
        }
        for label, inventory in cases.items():
            with self.subTest(case=label), self.assertRaises(ValueError):
                legal.validate(inventory, ROOT)
        # The macOS map has the shape of the others; the DMG's staging and its check read them, so every component the
        # DMG carries has one (COMPLETE_RELEASE_MAPS).
        self.assertEqual(legal.COMPLETE_RELEASE_MAPS, {"windows", "android", "macos"})

    def test_release_notice_listing_follows_the_maps(self):
        # The package checks read the notices each package must carry from the records.
        result = subprocess.run([sys.executable, str(ROOT / "tools/legal_inventory.py"), "--release-notices", "android"],
                                text=True, capture_output=True, check=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        listed = result.stdout.splitlines()
        expected = sorted({notice for c in self.inventory["components"]
                           for notice in c.get("android_release", {}).get("notices", {})})
        self.assertEqual(listed, expected)
        self.assertIn("licenses/OpenSSL-NOTICES.txt", listed)
        self.assertIn("licenses/mbelib-neo/legal/status.md", listed)
        android = (ROOT / ".github/workflows/android-ci.yaml").read_text(encoding="utf-8")
        self.assertIn("tools/check_release_notices.py --platform android", android)
        self.assertNotIn("licenses/opus-NOTICES.txt", android)
        for platform in legal.RELEASE_MAPS:
            result = subprocess.run([sys.executable, str(ROOT / "tools/legal_inventory.py"), "--release-notices",
                                     platform], text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stderr)

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
            self.assertEqual("ncurses" in packages, platform == "macos")
            self.assertEqual("libusb" in packages, platform in ("macos", "windows"))
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
            # The custom texts of the vcpkg libraries come from their retained files: PDCurses ships in the ZIP only,
            # OpenSSL and libsndfile in the ZIP, the APK and the DMG (OpenSSL's SEED terms also in the AppImage, whose
            # OpenSSL 3.5 carries them unchanged), the ncurses terminal database in the DMG only. codec2's varicode
            # grant is read as the LGPL 2.1: its note file (LICENSES/LicenseRef-codec2-varicode.txt) records that
            # reading and adds no license to any SBOM.
            for custom, platforms in (("LicenseRef-PDCurses-Public-Domain", ("windows",)),
                                      ("LicenseRef-OpenSSL-KISA-SEED", ("windows", "android", "macos", "linux")),
                                      ("LicenseRef-Sun-G72x", ("windows", "android", "macos")),
                                      ("LicenseRef-ncurses-terminfo", ("macos",))):
                self.assertEqual(custom in notices, platform in platforms)
                if platform in platforms:
                    self.assertEqual(notices[custom], (ROOT / "LICENSES" / (custom + ".txt")).read_text(encoding="utf-8"))
            self.assertFalse(any("codec2" in name for name in notices))
            with self.assertRaises(ValueError):
                legal.add_spdx(document, self.inventory, self.pins, platform)

    def test_sboms_carry_codec2_where_it_ships(self):
        # The Windows ZIP (overlay DLL), the Android app (the same overlay, statically linked), the AppImage and the DMG
        # (shared libraries built from the same commit) carry codec2.
        expression = "LGPL-2.1-only AND LGPL-2.1-or-later AND BSD-3-Clause AND BSD-2-Clause"
        for platform in legal.PLATFORMS:
            document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
            legal.add_spdx(document, self.inventory, self.pins, platform)
            packages = {p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-"): p for p in document["packages"]}
            with self.subTest(platform=platform):
                codec2 = packages["codec2"]
                self.assertEqual((codec2["licenseDeclared"], codec2["licenseConcluded"]), (expression, expression))
                self.assertEqual(codec2["versionInfo"], self.pins["CODEC2_SHA"])
                self.assertNotIn("licenseComments", codec2)
                self.assertIn("macOS DMG", codec2["sourceInfo"])
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
                # The DMG adds the notices of the files only its builds compile (curl's and PortAudio's macOS files).
                shipped = {source.removeprefix("repo:") for key in ("windows_release", "macos_release")
                           for source in component.get(key, {}).get("notices", {}).values() if source.startswith("repo:")}
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
        for ident, name in (("android-rtl", "licenses/librtlsdr-NOTICES.txt"),
                            ("android-libusb", "licenses/libusb-android-NOTICES.txt")):
            self.assertEqual(components[ident]["android_release"]["notices"][name], "repo:LICENSES/" + name[9:])

    def test_openssl_notice_names_the_holders_of_every_build(self):
        # One notice serves every package that carries vcpkg's OpenSSL 3.6.5: it names the holders of the CMP, CRMF and
        # HTTP client code and of bio_print.c, which the Windows libcrypto compiles too, and of the libssl and arm64
        # sources the Android build adds.
        notice = "LICENSES/OpenSSL-NOTICES.txt"
        text = (ROOT / notice).read_text(encoding="utf-8")
        listed, holder = {}, None
        for line in text[text.index("besides The OpenSSL Project"):text.index("\n-----")].split("\n")[3:]:
            if line.startswith("    "):
                listed[holder].add(line.strip())
            elif line:
                holder = line
                listed[holder] = set()
        for holder, path in (("Copyright Nokia 2007-2019", "crypto/cmp/cmp_ctx.c"),
                             ("Copyright Nokia 2007-2018", "crypto/crmf/crmf_lib.c"),
                             ("Copyright Nokia 2007-2020", "crypto/cmp/cmp_vfy.c"),
                             ("Copyright Siemens AG 2015-2019", "crypto/crmf/crmf_pbm.c"),
                             ("Copyright Siemens AG 2015-2020", "crypto/cmp/cmp_vfy.c"),
                             ("Copyright Siemens AG 2018-2020", "crypto/http/http_client.c"),
                             ("Copyright Siemens AG 2022", "crypto/cmp/cmp_genm.c"),
                             ("Copyright Patrick Powell 1995", "crypto/bio/bio_print.c"),
                             ("Copyright 2005 Nokia. All rights reserved.", "ssl/t1_enc.c"),
                             ("Copyright 2011 Google Inc.", "crypto/ec/ecp_nistp256.c"),
                             ("Copyright 2023 IBM Corp.", "crypto/ec/ecp_nistp384.c"),
                             ("Copyright (C) Cavium networks Ltd. 2016.", "crypto/aes/asm/aes-sha1-armv8.pl"),
                             ("Copyright 2006 NTT (Nippon Telegraph and Telephone Corporation) .",
                              "crypto/camellia/camellia.c"),
                             ("Copyright (c) 2004, 2018, Richard Levitte <richard@levitte.org>", "crypto/LPdir_unix.c"),
                             ("Copyright (C) 2006, Network Resonance, Inc. Copyright (C) 2011, RTFM, Inc.",
                              "ssl/d1_srtp.c")):
            with self.subTest(holder=holder):
                self.assertIn(path, listed.get(holder, ()))
        self.assertIn("It may be used for any purpose as long as this notice remains intact", text)
        self.assertNotIn("Windows libcrypto-3-x64.dll: release", text)
        openssl = next(c for c in self.inventory["components"] if c["id"] == "openssl")
        self.assertIn(notice, openssl["notices"])
        self.assertEqual(openssl["windows_release"]["notices"]["licenses/OpenSSL-NOTICES.txt"], "repo:" + notice)
        self.assertFalse((ROOT / "packaging/windows/OpenSSL-NOTICES.txt").exists())

    def test_appimage_openssl_is_recorded_from_its_own_pin(self):
        # The AppImage builds libcrypto.so.3 from the OpenSSL 3.5 LTS commit OPENSSL_SHA, not from the vcpkg baseline's
        # 3.6.5 the openssl record covers: it has a record of its own, with the version, commit and holders of that
        # source, and the AppImage job requires its notices.
        components = {c["id"]: c for c in self.inventory["components"]}
        self.assertIn("appimage-openssl", legal.REQUIRED)
        record = components["appimage-openssl"]
        self.assertEqual((record["platforms"], record["distribution"], record["pin"]), (["linux"], ["binary"],
                                                                                         "OPENSSL_SHA"))
        self.assertNotIn("linux", components["openssl"]["platforms"])
        self.assertEqual(record["license_concluded"], "Apache-2.0 AND CC0-1.0 AND LicenseRef-OpenSSL-KISA-SEED")
        notice = "packaging/appimage/OpenSSL-3.5-NOTICES.txt"
        self.assertEqual(record["notices"], ["LICENSES/Apache-2.0.txt", "LICENSES/OpenSSL-AUTHORS.md", notice,
                                             "LICENSES/LicenseRef-OpenSSL-KISA-SEED.txt"])
        text = (ROOT / notice).read_text(encoding="utf-8")
        self.assertIn(f"release 3.5.9 (tag openssl-3.5.9, commit\n{self.pins['OPENSSL_SHA']},", text)
        self.assertIn("3.5.9", record["name"])
        self.assertNotIn("3.6.5", text)
        listed, holder = {}, None
        for line in text[text.index("besides The OpenSSL Project"):text.index("\n-----")].split("\n")[2:]:
            if line.startswith("    "):
                listed[holder].add(line.strip())
            elif line:
                holder = line
                listed[holder] = set()
        for holder, path in (("Copyright Nokia 2007-2019", "crypto/cmp/cmp_ctx.c"),
                             ("Copyright Siemens AG 2018-2020", "crypto/http/http_client.c"),
                             ("Copyright Patrick Powell 1995", "crypto/bio/bio_print.c"),
                             ("Copyright (c) 2008 Andy Polyakov <appro@openssl.org>",
                              "crypto/camellia/asm/cmll-x86_64.pl"),
                             ("Copyright 2006 NTT (Nippon Telegraph and Telephone Corporation) .",
                              "crypto/camellia/camellia.c")):
            with self.subTest(holder=holder):
                self.assertIn(path, listed.get(holder, ()))
        # The default build leaves out the NIST P-224/256/384/521 code, and 3.5.9 has no Cavium files; libssl's own
        # sources are not in the AppImage.
        for absent in ("Google Inc.", "IBM Corp.", "Cavium", "ssl/s3_lib.c"):
            self.assertNotIn(absent, text)
        # The Linux SBOM names the AppImage's libcrypto by its pinned commit.
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
        legal.add_spdx(document, self.inventory, self.pins, "linux")
        packages = {p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-"): p for p in document["packages"]}
        self.assertEqual(packages["appimage-openssl"]["versionInfo"], self.pins["OPENSSL_SHA"])
        self.assertNotIn("openssl", packages)
        # The AppImage carries the notice under licenses/ and its job requires the four files.
        workflow = (ROOT / ".github/workflows/linux-appimage.yaml").read_text(encoding="utf-8")
        self.assertIn('cp -f packaging/appimage/OpenSSL-3.5-NOTICES.txt "$docdir/licenses/OpenSSL-3.5-NOTICES.txt"',
                      workflow)
        required = re.search(r"(?m)^ +required=\((.*)\)$", workflow)[1].split()
        for name in ("licenses/OpenSSL-3.5-NOTICES.txt", "licenses/Apache-2.0.txt", "licenses/OpenSSL-AUTHORS.md",
                     "licenses/LicenseRef-OpenSSL-KISA-SEED.txt"):
            self.assertIn(name, required)

    def test_android_records_name_the_vcpkg_libraries_the_app_links(self):
        # The APK links vcpkg's static builds of these libraries into libdsd-neo-app_arm64-v8a.so, from the same
        # baseline as the Windows ZIP: its SBOM names each with the expression and version of the reviewed sources, and
        # the notices the records retain are shared files every package can carry.
        ports = ("openssl", "curl", "zlib", "expat", "libsndfile", "libflac", "libogg", "libvorbis", "opus", "mp3lame",
                 "mpg123")
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
        legal.add_spdx(document, self.inventory, self.pins, "android")
        packages = {p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-"): p for p in document["packages"]}
        components = {c["id"]: c for c in self.inventory["components"]}
        for ident in ports:
            component = components[ident]
            with self.subTest(component=ident):
                self.assertIn(ident, packages)
                self.assertEqual(packages[ident]["licenseConcluded"], component["license_concluded"])
                self.assertIn("Android app", component["assessment"])
                self.assertIn("vcpkg-triplets/arm64-android-static.cmake", component["paths"])
                for notice in component["notices"]:
                    self.assertFalse(notice.startswith("packaging/windows/"), notice)
        self.assertEqual(packages["openssl"]["name"], "OpenSSL")
        self.assertIn("libssl", components["openssl"]["assessment"])
        self.assertIn("CURL_USE_OPENSSL=ON", components["curl"]["assessment"])
        # The APK carries the notices the ZIP carries for these libraries, each port's copyright file from the Android
        # build's vcpkg tree.
        for port in ports:
            with self.subTest(port=port):
                self.assertEqual(components[port]["android_release"], {"notices": components[port]["windows_release"]
                                                                       ["notices"]})
                self.assertEqual(components[port]["android_release"]["notices"][f"licenses/{port}-copyright.txt"],
                                 "vcpkg:" + port)

    def test_android_runtime_records_cover_qt_the_ndk_runtime_and_the_java_libraries(self):
        # The APK's Qt libraries and Java code, the NDK's libc++_shared.so and the AndroidX and Kotlin libraries R8
        # merges into classes.dex each have a record: Qt is used under the LGPL 3 (never GPL-2.0-only, which dsd-neo's
        # GPL 3 cannot take), and the APK carries every licence text and notice the records name.
        expected = {
            "qt": "LGPL-3.0-only AND ",
            "ndk-runtime": "Apache-2.0 WITH LLVM-exception AND (NCSA OR MIT) AND BSL-1.0 AND BSD-2-Clause",
            "androidx": "Apache-2.0",
            "kotlin": "Apache-2.0 AND BSD-3-Clause AND BSL-1.0",
        }
        self.assertLessEqual(set(expected), legal.REQUIRED)
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
        legal.add_spdx(document, self.inventory, self.pins, "android")
        legal.extracted_licenses(document, self.inventory, ROOT)
        packages = {p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-"): p for p in document["packages"]}
        components = {c["id"]: c for c in self.inventory["components"]}
        for ident, expression in expected.items():
            component = components[ident]
            with self.subTest(component=ident):
                self.assertEqual(component["platforms"], ["android"])
                self.assertTrue(packages[ident]["licenseConcluded"].startswith(expression), packages[ident])
                self.assertNotIn("GPL-2.0-only", component["license_concluded"])
                self.assertNotIn("finding", component)
                shipped = dict(component["android_release"]["notices"])
                self.assertEqual(shipped["licenses/Apache-2.0.txt"], "repo:LICENSES/Apache-2.0.txt")
                if ident == "qt":
                    # Besides its retained notices, the APK carries the Qt source directions the build writes.
                    self.assertEqual(shipped.pop("licenses/Qt-SOURCE.txt"), "build:qt-source/qt-source.txt")
                self.assertEqual({source.removeprefix("repo:") for source in shipped.values()}, set(component["notices"]))
        for platform in ("windows", "linux", "macos"):
            document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
            legal.add_spdx(document, self.inventory, self.pins, platform)
            self.assertFalse({"qt", "ndk-runtime", "androidx", "kotlin"} & {
                p["SPDXID"].removeprefix("SPDXRef-dsd-reviewed-") for p in document["packages"]})
        # Qt's two custom texts reach the APK's SBOM from their retained files.
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
        legal.add_spdx(document, self.inventory, self.pins, "android")
        legal.extracted_licenses(document, self.inventory, ROOT)
        extracted = {n["licenseId"]: n["extractedText"] for n in document["hasExtractedLicensingInfos"]}
        for custom in ("LicenseRef-SHA1-Public-Domain", "LicenseRef-ICC-License"):
            self.assertEqual(extracted[custom], (ROOT / "LICENSES" / (custom + ".txt")).read_text(encoding="utf-8"))
        # One Apache-2.0 text for every record that names it: the apache.org file.
        self.assertEqual(apache_sha256(ROOT / "LICENSES/Apache-2.0.txt"), APACHE_SHA256)
        self.assertIn("LICENSES/Apache-2.0.txt", components["openssl"]["notices"])
        qt = (ROOT / "packaging/android/Qt-NOTICES.txt").read_text(encoding="utf-8")
        for words in ("This software is based in part on the work of the Independent JPEG Group.",
                      "This software is based in part on the work of the FreeType Team.",
                      "46de0710b594945a5d446ae44354daa386141c6903c53781b2ca547bc62f8f8f",
                      "https://publicsuffix.org/list/public_suffix_list.dat",
                      # Qt 6.11.3's qunicodetables_p.h: generated from the Unicode 17.0 database.
                      "Unicode Character Database (Unicode 17.0) and Unicode CLDR v48.2"):
            self.assertIn(words, qt)
        self.assertIn("Unicode Character Database (Unicode 17.0) and CLDR 48.2 data", components["qt"]["assessment"])
        self.assertIn("kotlinx.coroutines library.", (ROOT / "packaging/android/Kotlin-NOTICES.txt").read_text(
            encoding="utf-8"))
        # What the records do not settle stays visible in the findings.
        status = (ROOT / "legal/status.md").read_text(encoding="utf-8")
        # The licences screen, the release APK's plugin set and the drift check are delivered in this change set; the
        # macOS ncurses overlay and Android's vcpkg requirement are settled. None of them is left as an open item.
        for words in ("the app's licences screen lists every notice the APK carries",
                      "release APKs no longer bundle Qt's QML debugger and profiler plugins",
                      "android-ci fails when the Qt kit, the Gradle dependency set or the NDK runtime differs",
                      "the DMG keeps the vcpkg ncurses overlay",
                      "Android configure keeps requiring vcpkg"):
            self.assertIn(words, status)
        for words in ("licences screen reads a fixed list", "Nothing guards these records",
                      "For the APK: show every notice", "stay open under RUNTIME-NOTICES"):
            self.assertNotIn(words, status)
        # Qt's source is offered by directions beside every APK (GPL 3 section 6(d)); the records say so, and that the
        # obligation stays with dsd-neo while the archives sit on Qt's servers.
        self.assertIn("These are now provided by directions under GPL section 6(d)", status)
        self.assertNotIn("not yet offered with the APK", status)
        delivery = (ROOT / "legal/source-delivery.md").read_text(encoding="utf-8")
        for words in ("`<APK name>.qt-source.txt`", "`tools/android_source_directions.py`",
                      "availability depends on Qt's and the Public Suffix List's servers",
                      "leaves the obligation to keep it available with dsd-neo as the\n   distributor"):
            self.assertIn(words, delivery)
        self.assertIn("tools/android_source_directions.py", components["qt"]["paths"])

    def test_sboms_name_only_the_soapysdr_programs_each_package_carries(self):
        # The AppImage builds SoapySDR without its programs (ENABLE_APPS=OFF) and the DMG without the overlay's tools
        # feature; only the Windows ZIP carries SoapySDRUtil.exe, so only its SBOM names it.
        appimage = (ROOT / ".github/workflows/linux-appimage.yaml").read_text(encoding="utf-8")
        self.assertIn("-DENABLE_APPS=OFF", appimage)
        for platform in legal.PLATFORMS:
            document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": []}
            legal.add_spdx(document, self.inventory, self.pins, platform)
            names = [p["name"] for p in document["packages"]]
            with self.subTest(platform=platform):
                self.assertEqual("SoapySDR" in names, platform in ("linux", "macos", "windows"), names)
                self.assertEqual([n for n in names if "SoapySDRUtil" in n],
                                 ["SoapySDRUtil"] if platform == "windows" else [])

    def test_apache_text_digest_ignores_crlf_checkouts(self):
        # A checkout with CRLF line endings (Git's autocrlf on Windows) holds the same licence text: the digest is
        # taken over LF line endings, as the inventory check takes every retained notice.
        text = (ROOT / "LICENSES/Apache-2.0.txt").read_bytes().replace(b"\r\n", b"\n")
        with tempfile.TemporaryDirectory() as directory:
            for name, content in (("lf.txt", text), ("crlf.txt", text.replace(b"\n", b"\r\n"))):
                path = Path(directory) / name
                path.write_bytes(content)
                with self.subTest(line_endings=name):
                    self.assertEqual(apache_sha256(path), APACHE_SHA256)
            altered = Path(directory) / "altered.txt"
            altered.write_bytes(text.replace(b"Apache License", b"Apache Licence", 1))
            self.assertNotEqual(apache_sha256(altered), APACHE_SHA256)

    def test_scanner_records_are_preserved_and_conflicting_licenses_fail(self):
        scanner = {"SPDXID": "SPDXRef-scanned-library", "name": "library"}
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "packages": [scanner.copy()]}
        legal.add_spdx(document, self.inventory, self.pins, "linux")
        self.assertEqual(document["packages"][0], scanner)
        document["hasExtractedLicensingInfos"] = [
            {"licenseId": "LicenseRef-BCH3-Noncommercial", "extractedText": "unrestricted"}]
        with self.assertRaises(ValueError):
            legal.extracted_licenses(document, self.inventory, ROOT)

    def test_ncurses_overlay_declares_and_installs_the_reviewed_terms(self):
        # The library is under the upstream COPYING (X11-distribute-modifications-variant) and the terminal database
        # under the statements of misc/terminfo.src: the overlay's manifest names both, and the copyright file the
        # port installs carries both texts, as the record has them reviewed.
        record = next(c for c in self.inventory["components"] if c["id"] == "ncurses")
        manifest = json.loads((ROOT / "vcpkg-ports/ncurses/vcpkg.json").read_text(encoding="utf-8"))
        self.assertEqual(manifest["license"], record["license_concluded"])
        notice = "LicenseRef-ncurses-terminfo.txt"
        self.assertIn("LICENSES/" + notice, record["notices"])
        self.assertEqual((ROOT / "vcpkg-ports/ncurses" / notice).read_bytes(), (ROOT / "LICENSES" / notice).read_bytes())
        portfile = (ROOT / "vcpkg-ports/ncurses/portfile.cmake").read_text(encoding="utf-8")
        self.assertIn('vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/COPYING" "${CMAKE_CURRENT_LIST_DIR}/'
                      + notice + '")', portfile)

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

    def test_standalone_scripts_set_the_project_policies(self):
        # `cmake -P` leaves every policy unset, so on CMake 3.x IN_LIST and the newer if() rules are not available
        # unless the script asks for the project's minimum itself.
        minimum = re.search(r"(?m)^cmake_minimum_required\(VERSION ([0-9.]+)\)", (ROOT / "CMakeLists.txt").read_text())
        for script in ("cmake/stage_android_package.cmake", "cmake/stage_release_notices.cmake"):
            with self.subTest(script=script):
                text = (ROOT / script).read_text(encoding="utf-8")
                commands = [line for line in text.splitlines() if line.strip() and not line.startswith("#")]
                self.assertEqual(commands[0], f"cmake_minimum_required(VERSION {minimum[1]})")

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

    def android_fixture(self):
        package = self.work / "package"
        package.mkdir()
        build = self.work / "build"
        for name in ("pffft-licenses/pffft-LICENSE.txt", "pffft-licenses/pffft-NOTICES.txt",
                     "tetra_crypto-licenses/tetra_crypto-LICENSE.txt", "qt-source/qt-source.txt"):
            (build / name).parent.mkdir(parents=True, exist_ok=True)
            (build / name).write_text(name + "\n", encoding="utf-8")
        share = self.work / "vcpkg_installed/arm64-android-static/share"
        for port in VCPKG_ANDROID_PORTS:
            (share / port).mkdir(parents=True)
            (share / port / "copyright").write_text("Port copyright fixture: " + port + "\n", encoding="utf-8")
        stage = self.work / "stage"
        args = [f"-DREPO_DIR={ROOT}", f"-DSOURCE_DIR={package}", f"-DSTAGE_DIR={stage}", f"-DBUILD_DIR={build}",
                f"-DMBELIB_NOTICE_DIR={self.mbe}", f"-DVCPKG_SHARE_DIR={share}", "-P",
                ROOT / "cmake/stage_android_package.cmake"]
        return build, share, stage / "assets/doc/dsd-neo", args

    def test_android_assets_retain_full_notice_tree_and_fail_on_missing_dependency_text(self):
        build, share, docs, args = self.android_fixture()
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        # Every notice an android_release map names, from where the map says.
        inventory = legal.read_inventory(ROOT)
        staged = 0
        for component in inventory["components"]:
            for notice, source in component.get("android_release", {}).get("notices", {}).items():
                kind, _, rest = source.partition(":")
                expected = {"repo": lambda: ROOT / rest, "build": lambda: build / rest,
                            "vcpkg": lambda: share / rest / "copyright"}.get(kind)
                with self.subTest(notice=notice):
                    self.assertTrue((docs / notice).is_file())
                    if expected is not None:
                        self.assertEqual((docs / notice).read_bytes(), expected().read_bytes())
                    staged += 1
        self.assertGreater(staged, 40)
        for name in MBE_NOTICES:
            self.assertEqual((docs / "licenses/mbelib-neo" / name).read_bytes(), (self.mbe / name).read_bytes())
        for name in ("status.md", "source-delivery.md", "components.json"):
            self.assertEqual((docs / "legal" / name).read_bytes(), (ROOT / "legal" / name).read_bytes())
        self.assertEqual((docs / "licenses/ISC.txt").read_bytes(), (ROOT / "LICENSES/ISC.txt").read_bytes())
        # A stale destination must not hide an incomplete new dependency install.
        (share / "mpg123/copyright").unlink()
        self.assertNotEqual(self.run_cmake(*args).returncode, 0)
        (share / "mpg123/copyright").write_text("restored\n", encoding="utf-8")
        self.assertEqual(self.run_cmake(*args).returncode, 0)
        (build / "pffft-licenses/pffft-NOTICES.txt").unlink()
        self.assertNotEqual(self.run_cmake(*args).returncode, 0)
        (build / "pffft-licenses/pffft-NOTICES.txt").write_text("restored\n", encoding="utf-8")
        (self.mbe / "legal/status.md").unlink()
        self.assertNotEqual(self.run_cmake(*args).returncode, 0)

    def test_android_notice_index_lists_every_staged_file(self):
        # The app's licences screen reads the files this index names, so it lists exactly what the APK carries:
        # the project's own files first, then every other staged file in a stable order.
        build, share, docs, args = self.android_fixture()
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
        staged = sorted(path.relative_to(docs).as_posix() for path in docs.rglob("*")
                        if path.is_file() and path.name != "notice-index.txt")
        self.assertEqual(index[:3], ["LICENSE", "COPYRIGHT", "THIRD_PARTY.md"])
        self.assertEqual(index[3:], sorted(set(staged) - {"LICENSE", "COPYRIGHT", "THIRD_PARTY.md"}))
        for notice in legal.release_notices(legal.read_inventory(ROOT), "android"):
            self.assertIn(notice, index)
        self.assertIn("licenses/Qt-NOTICES.txt", index)
        self.assertIn("licenses/LGPL-3.0-only.txt", index)

        # A notice added to a record's map reaches the index with no other change.
        inventory = legal.read_inventory(ROOT)
        qt = next(c for c in inventory["components"] if c["id"] == "qt")
        qt["android_release"]["notices"]["licenses/Added-NOTICES.txt"] = "repo:LICENSES/ISC.txt"
        components = self.work / "components.json"
        components.write_text(json.dumps(inventory), encoding="utf-8")
        result = self.run_cmake(f"-DCOMPONENTS={components}", *args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
        self.assertIn("licenses/Added-NOTICES.txt", index)
        self.assertEqual(index.count("licenses/Added-NOTICES.txt"), 1)

        # Dropped from the map again, an incremental build removes it from the folder and the index, as a clean
        # build would; a file the staging never wrote goes as well.
        (docs / "licenses/stray.txt").write_text("stray\n", encoding="utf-8")
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
        for gone in ("licenses/Added-NOTICES.txt", "licenses/stray.txt"):
            self.assertFalse((docs / gone).exists(), gone)
            self.assertNotIn(gone, index)
        staged = sorted(path.relative_to(docs).as_posix() for path in docs.rglob("*")
                        if path.is_file() and path.name != "notice-index.txt")
        self.assertEqual(sorted(index), staged)

    def test_android_staging_rerun_rewrites_nothing(self):
        # A no-op build leaves every staged file as it was, so Gradle has nothing to repackage.
        build, share, docs, args = self.android_fixture()
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        before = {path: path.stat().st_mtime_ns for path in docs.rglob("*") if path.is_file()}
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        after = {path: path.stat().st_mtime_ns for path in docs.rglob("*") if path.is_file()}
        self.assertEqual(after, before)

    def test_android_staging_owns_only_its_output(self):
        # A folder beside the stage is not the staging's: it is left as it was, and no scratch folder stays behind,
        # also when the staging fails.
        build, share, docs, args = self.android_fixture()
        stage = docs.parents[2]
        beside = self.work / "stage-doc"
        (beside / "keep").mkdir(parents=True)
        (beside / "keep/mine.txt").write_text("not the staging's\n", encoding="utf-8")
        before = sorted(path.name for path in self.work.iterdir())
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((beside / "keep/mine.txt").read_text(encoding="utf-8"), "not the staging's\n")
        self.assertEqual(sorted(path.name for path in self.work.iterdir()), sorted(before + [stage.name]))
        (share / "mpg123/copyright").unlink()
        self.assertNotEqual(self.run_cmake(*args).returncode, 0)
        self.assertEqual(sorted(path.name for path in self.work.iterdir()), sorted(before + [stage.name]))
        self.assertTrue((beside / "keep/mine.txt").is_file())

    def scratch_folders(self):
        """The folders a staging run made beside the stage for its fresh documentation and left behind."""
        return sorted(path.name for path in self.work.iterdir() if path.name.startswith(".stage-doc-"))

    @unittest.skipIf(os.name == "nt" or os.geteuid() == 0, "needs a folder the run cannot write")
    def test_android_staging_leaves_no_scratch_folder_whatever_step_fails(self):
        # The run removes the folder it staged the documentation in whichever of its steps fails, not only the notice
        # script: here, publishing to the package's documentation folder fails.
        build, share, docs, args = self.android_fixture()
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (docs / "LICENSE").unlink()
        docs.chmod(0o555)
        self.addCleanup(docs.chmod, 0o755)
        result = self.run_cmake(*args)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.scratch_folders(), [], result.stdout + result.stderr)
        docs.chmod(0o755)
        # A package document where the licence texts' folder goes.
        source = self.work / "package/assets/doc/dsd-neo"
        source.mkdir(parents=True)
        (source / "licenses").write_text("a file where a folder goes\n", encoding="utf-8")
        result = self.run_cmake(*args)
        self.assertEqual(self.scratch_folders(), [], result.stdout + result.stderr)

    @unittest.skipIf(os.name == "nt", "needs symbolic links")
    def test_android_staging_refuses_symbolic_links_in_the_documentation_path(self):
        # A link in android/package/'s documentation, on the way to it, or in the staged package's documentation
        # path is refused before anything is written, so nothing is written through it to where it points.
        build, share, docs, args = self.android_fixture()
        package = self.work / "package"
        stage = docs.parents[2]
        outside = self.work / "outside"

        def reset():
            for path in (package / "assets", stage, outside):
                if path.is_symlink() or path.is_file():
                    path.unlink()
                elif path.exists():
                    shutil.rmtree(path)
            outside.mkdir()
            (outside / "target.txt").write_text("not the staging's\n", encoding="utf-8")

        def folder_link(path):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(outside, target_is_directory=True)

        def file_link(path):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.symlink_to(outside / "target.txt")

        def staged_link(relative):
            def make():
                result = self.run_cmake(*args)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                path = stage / relative
                shutil.rmtree(path)
                folder_link(path)
            return make

        for label, link in (
                ("a folder in the package documentation",
                 lambda: folder_link(package / "assets/doc/dsd-neo/licenses")),
                ("a file of a notice's name", lambda: file_link(package / "assets/doc/dsd-neo/LICENSE")),
                ("another file", lambda: file_link(package / "assets/doc/dsd-neo/extra.txt")),
                ("the package documentation folder", lambda: folder_link(package / "assets/doc/dsd-neo")),
                ("a folder on the way to it", lambda: folder_link(package / "assets")),
                ("a folder on the staged path", staged_link("assets/doc")),
                ("a folder in the staged documentation", staged_link("assets/doc/dsd-neo/licenses"))):
            with self.subTest(case=label):
                reset()
                link()
                result = self.run_cmake(*args)
                self.assertEqual(sorted(path.name for path in outside.iterdir()), ["target.txt"])
                self.assertEqual((outside / "target.txt").read_text(encoding="utf-8"), "not the staging's\n")
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                # CMake wraps a long message: compare it as one line.
                self.assertIn("is a symbolic link", " ".join((result.stdout + result.stderr).split()))
                self.assertEqual(self.scratch_folders(), [])

    @unittest.skipIf(os.name == "nt", "needs symbolic links")
    def test_android_staging_refuses_a_linked_package_or_stage_folder(self):
        # The source and stage folders themselves are checked too: a stage folder that links to another package tree
        # would have its notices overwritten and its stale files removed there.
        build, share, docs, args = self.android_fixture()
        package = self.work / "package"
        stage = docs.parents[2]
        other = self.work / "other-package"
        (other / "assets/doc/dsd-neo").mkdir(parents=True)
        (other / "assets/doc/dsd-neo/stale.txt").write_text("another tree's file\n", encoding="utf-8")
        (other / "keep.txt").write_text("another tree's file\n", encoding="utf-8")

        def tree(root):
            return sorted((p.relative_to(root).as_posix(), p.read_bytes()) for p in root.rglob("*") if p.is_file())

        before = tree(other)
        real_package = self.work / "real-package"
        for label, link in (("the stage folder", lambda: stage.symlink_to(other, target_is_directory=True)),
                            ("the package folder",
                             lambda: (package.rename(real_package),
                                      package.symlink_to(real_package, target_is_directory=True)))):
            with self.subTest(case=label):
                link()
                try:
                    result = self.run_cmake(*args)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("is a symbolic link", " ".join((result.stdout + result.stderr).split()))
                    self.assertEqual(tree(other), before)
                    self.assertEqual(self.scratch_folders(), [])
                finally:
                    for path in (stage, package):
                        if path.is_symlink():
                            path.unlink()
                    if real_package.exists():
                        real_package.rename(package)

    @unittest.skipIf(os.name == "nt", "needs symbolic links")
    def test_android_staging_refuses_symbolic_links_among_the_notices(self):
        # A link the notices bring with them is refused before a package document is merged beneath it, so nothing is
        # written through it into mbelib's installation or wherever else it points.
        build, share, docs, args = self.android_fixture()
        outside = self.work / "outside-legal"
        (self.mbe / "legal").rename(outside)
        (self.mbe / "legal").symlink_to(outside, target_is_directory=True)
        before = sorted(path.name for path in outside.iterdir())
        source = self.work / "package/assets/doc/dsd-neo/licenses/mbelib-neo/legal"
        source.mkdir(parents=True)
        (source / "extra.md").write_text("from the package source\n", encoding="utf-8")
        result = self.run_cmake(*args)
        self.assertEqual(sorted(path.name for path in outside.iterdir()), before)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"{self.mbe / 'legal'} is a symbolic link", " ".join((result.stdout + result.stderr).split()))
        self.assertEqual(self.scratch_folders(), [])

    @unittest.skipIf(os.name == "nt" or os.geteuid() == 0, "needs a folder the run cannot write")
    def test_android_staging_merges_package_documents_into_read_only_notice_folders(self):
        # An installed notice folder may be read-only; its copy is the staging's own, so a package document that takes
        # no notice's path is still merged into it, and the installed folders keep their modes.
        build, share, docs, args = self.android_fixture()
        source = self.work / "package/assets/doc/dsd-neo/licenses/mbelib-neo/legal"
        source.mkdir(parents=True)
        (source / "extra.md").write_text("from the package source\n", encoding="utf-8")
        for folder in (self.mbe / "legal", self.mbe):
            folder.chmod(0o555)
            self.addCleanup(folder.chmod, 0o755)
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((docs / "licenses/mbelib-neo/legal/extra.md").read_text(encoding="utf-8"),
                         "from the package source\n")
        self.assertIn("licenses/mbelib-neo/legal/extra.md",
                      (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines())
        for folder in (self.mbe / "legal", self.mbe):
            self.assertEqual(folder.stat().st_mode & 0o777, 0o555)
        self.assertEqual(self.scratch_folders(), [])

    def test_android_staging_keeps_the_notice_index_path_for_the_index(self):
        # The index's path is the staging's: a package document at it or under it is left out, and a folder an older
        # staging left there gives way to the index.
        build, share, docs, args = self.android_fixture()
        source = self.work / "package/assets/doc/dsd-neo"
        (source / "notice-index.txt").mkdir(parents=True)
        (source / "notice-index.txt/extra.txt").write_text("from the package source\n", encoding="utf-8")
        (source / "extra.txt").write_text("from the package source\n", encoding="utf-8")

        def check(result):
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue((docs / "notice-index.txt").is_file())
            index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
            self.assertIn("extra.txt", index)
            self.assertFalse([name for name in index if name.startswith("notice-index.txt")], index)
            self.assertEqual(self.scratch_folders(), [])

        check(self.run_cmake(*args))
        shutil.rmtree(source / "notice-index.txt")
        (docs / "notice-index.txt").unlink()
        (docs / "notice-index.txt").mkdir()
        (docs / "notice-index.txt/stale.txt").write_text("an older staging's file\n", encoding="utf-8")
        check(self.run_cmake(*args))

    def test_android_staging_reads_folder_paths_literally(self):
        # A package or stage folder whose path holds pattern characters, or a package folder given relative to the
        # working folder, keeps the package's documents in the APK and its index, and a link there is still refused.
        build, share, docs, args = self.android_fixture()
        package = self.work / "package"
        (package / "assets/doc/dsd-neo/extra.txt").parent.mkdir(parents=True)
        (package / "assets/doc/dsd-neo/extra.txt").write_text("from the package source\n", encoding="utf-8")
        # Windows forbids * and ? in a file name, so there the folders take the bracket pattern characters alone.
        pattern = " [qa]" if os.name == "nt" else " [qa]*?"
        for label, package_name, source, stage_name in (
                ("a package folder with pattern characters", "package" + pattern, None, "stage"),
                ("a stage folder with pattern characters", "package", None, "stage" + pattern),
                ("a relative package folder", "package", "package", "stage")):
            with self.subTest(case=label):
                if package.name != package_name:
                    package = package.rename(self.work / package_name)
                stage = self.work / stage_name
                replaced = {"-DSOURCE_DIR": f"-DSOURCE_DIR={source or package}", "-DSTAGE_DIR": f"-DSTAGE_DIR={stage}"}
                case_args = [replaced.get(str(arg).partition("=")[0], arg) for arg in args]
                case_docs = stage / "assets/doc/dsd-neo"
                result = self.run_cmake(*case_args)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((case_docs / "extra.txt").read_text(encoding="utf-8"), "from the package source\n")
                self.assertIn("extra.txt", (case_docs / "notice-index.txt").read_text(encoding="utf-8").splitlines())
                if os.name != "nt":
                    link = package / "assets/doc/dsd-neo/linked.txt"
                    link.symlink_to(package / "assets/doc/dsd-neo/extra.txt")
                    result = self.run_cmake(*case_args)
                    link.unlink()
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertIn("is a symbolic link", " ".join((result.stdout + result.stderr).split()))
                self.assertEqual([path.name for path in self.work.iterdir() if "-doc-" in path.name], [])
                shutil.rmtree(stage)
                if package.name != "package":
                    package = package.rename(self.work / "package")

    def test_android_staging_keeps_the_package_source_documentation(self):
        # A file android/package/ puts in the documentation folder reaches the APK and its index, in a clean build and
        # in an incremental one, and a generated notice of the same name takes its place.
        build, share, docs, args = self.android_fixture()
        source = self.work / "package/assets/doc/dsd-neo"
        (source / "extra").mkdir(parents=True)
        (source / "extra/README.txt").write_text("from the package source\n", encoding="utf-8")
        (source / "LICENSE").write_text("an outdated copy\n", encoding="utf-8")
        for _ in range(2):
            result = self.run_cmake(*args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual((docs / "extra/README.txt").read_text(encoding="utf-8"), "from the package source\n")
            self.assertEqual((docs / "LICENSE").read_bytes(), (ROOT / "LICENSE").read_bytes())
            index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
            self.assertIn("extra/README.txt", index)
            self.assertEqual(index.count("LICENSE"), 1)
        before = {path: path.stat().st_mtime_ns for path in docs.rglob("*") if path.is_file()}
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual({path: path.stat().st_mtime_ns for path in docs.rglob("*") if path.is_file()}, before)

    def test_android_staging_prefers_the_notices_to_package_documents_at_their_paths(self):
        # A package document at a notice's path is left out whatever its timestamp, and one under a notice's path is
        # left out before anything is written, so the notice is staged in its place; other package documents stay,
        # also in the notices' folders.
        build, share, docs, args = self.android_fixture()
        source = self.work / "package/assets/doc/dsd-neo"
        canonical = ("COPYRIGHT", "THIRD_PARTY.md", "legal/components.json")
        for name in canonical:
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("a stale copy\n", encoding="utf-8")
            times = (ROOT / name).stat()
            os.utime(path, ns=(times.st_atime_ns, times.st_mtime_ns))
        (source / "licenses/Qt-SOURCE.txt").mkdir(parents=True)
        (source / "licenses/Qt-SOURCE.txt/note.txt").write_text("a folder where a notice goes\n", encoding="utf-8")
        (source / "licenses/extra.txt").write_text("from the package source\n", encoding="utf-8")

        def staged_files():
            return sorted(path.relative_to(docs).as_posix() for path in docs.rglob("*")
                          if path.is_file() and path.name != "notice-index.txt")

        for _ in range(2):
            result = self.run_cmake(*args)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for name in canonical:
                self.assertEqual((docs / name).read_bytes(), (ROOT / name).read_bytes(), name)
            self.assertTrue((docs / "licenses/Qt-SOURCE.txt").is_file())
            self.assertEqual((docs / "licenses/Qt-SOURCE.txt").read_bytes(),
                             (build / "qt-source/qt-source.txt").read_bytes())
            self.assertEqual((docs / "licenses/extra.txt").read_text(encoding="utf-8"), "from the package source\n")
            index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
            self.assertEqual(sorted(index), staged_files())
            self.assertIn("licenses/extra.txt", index)

        # A package file where the licence texts' folder goes is left out as well.
        shutil.rmtree(source)
        source.mkdir()
        (source / "licenses").write_text("a file where a folder goes\n", encoding="utf-8")
        result = self.run_cmake(*args)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((docs / "licenses/ISC.txt").read_bytes(), (ROOT / "LICENSES/ISC.txt").read_bytes())
        index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
        self.assertEqual(sorted(index), staged_files())
        self.assertEqual(self.scratch_folders(), [])

    def test_android_staging_replaces_a_notice_folder_with_a_notice_file(self):
        # A notice that was a folder's file and becomes a file of the folder's name, and back again.
        build, share, docs, args = self.android_fixture()
        inventory = legal.read_inventory(ROOT)
        qt = next(c for c in inventory["components"] if c["id"] == "qt")
        components = self.work / "components.json"
        for notice in ("licenses/acme/NOTICE", "licenses/acme", "licenses/acme/NOTICE"):
            with self.subTest(notice=notice):
                notices = {name: source for name, source in qt["android_release"]["notices"].items()
                           if not name.startswith("licenses/acme")}
                qt["android_release"]["notices"] = {**notices, notice: "repo:LICENSES/ISC.txt"}
                components.write_text(json.dumps(inventory), encoding="utf-8")
                result = self.run_cmake(f"-DCOMPONENTS={components}", *args)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertTrue((docs / notice).is_file())
                self.assertEqual((docs / notice).read_bytes(), (ROOT / "LICENSES/ISC.txt").read_bytes())
                index = (docs / "notice-index.txt").read_text(encoding="utf-8").splitlines()
                self.assertEqual([name for name in index if name.startswith("licenses/acme")], [notice])

    def test_release_notice_staging_follows_any_platform_map(self):
        # The staging the APK uses takes any platform's map, so the DMG can use it as well; it refuses a source it does
        # not know, a path outside its root and a legal-component notice the package lacks.
        build = self.work / "build"
        (build / "notices").mkdir(parents=True)
        (build / "notices/b.txt").write_text("build notice\n", encoding="utf-8")
        share = self.work / "share"
        (share / "port").mkdir(parents=True)
        (share / "port/copyright").write_text("port notice\n", encoding="utf-8")
        docs = self.work / "docs"
        (docs / "licenses").mkdir(parents=True)
        (docs / "licenses/installed.txt").write_text("installed\n", encoding="utf-8")

        def stage(notices, platform="macos"):
            components = self.work / "components.json"
            components.write_text(json.dumps({"components": [
                {"id": "a", "macos_release": {"notices": notices}},
                {"id": "b", "android_release": {"notices": {"licenses/other.txt": "repo:LICENSE"}}}]}),
                encoding="utf-8")
            return self.run_cmake(f"-DPLATFORM={platform}", f"-DREPO_DIR={ROOT}", f"-DBUILD_DIR={build}",
                                  f"-DVCPKG_SHARE_DIR={share}", f"-DDOC_DIR={docs}", f"-DCOMPONENTS={components}",
                                  "-P", ROOT / "cmake/stage_release_notices.cmake")

        result = stage({"licenses/r.txt": "repo:LICENSES/ISC.txt", "licenses/b.txt": "build:notices/b.txt",
                        "licenses/p-copyright.txt": "vcpkg:port", "licenses/installed.txt": "legal-component"})
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((docs / "licenses/r.txt").read_bytes(), (ROOT / "LICENSES/ISC.txt").read_bytes())
        self.assertEqual((docs / "licenses/b.txt").read_text(encoding="utf-8"), "build notice\n")
        self.assertEqual((docs / "licenses/p-copyright.txt").read_text(encoding="utf-8"), "port notice\n")
        self.assertFalse((docs / "licenses/other.txt").exists())
        for label, notices in {
            "an unknown source": {"licenses/x.txt": "web:x"},
            "a repository path above the repository": {"licenses/x.txt": "repo:../x"},
            "a build path through a parent folder": {"licenses/x.txt": "build:notices/../../x"},
            "an absolute build path": {"licenses/x.txt": "build:/etc/hostname"},
            "a destination outside the documentation": {"../x.txt": "repo:LICENSE"},
            "a missing source": {"licenses/x.txt": "build:notices/missing.txt"},
            "a missing port": {"licenses/x.txt": "vcpkg:missing"},
            "a legal-component notice the package lacks": {"licenses/missing.txt": "legal-component"},
        }.items():
            with self.subTest(case=label):
                self.assertNotEqual(stage(notices).returncode, 0)
        self.assertNotEqual(stage({"licenses/r.txt": "repo:LICENSE"}, platform="solaris").returncode, 0)


if __name__ == "__main__":
    unittest.main()
