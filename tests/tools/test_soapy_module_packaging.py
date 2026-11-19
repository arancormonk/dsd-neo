#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Contracts for the SoapySDR modules the Windows ZIP ships.

SoapyModulePackaging reads the vcpkg overlays, the manifest, the notices, the legal records and the workflows.
SoapyReleaseScripts runs tools/windows/stage_release_tree.ps1 under pwsh over a fake vcpkg tree and
verify_release_tree.ps1 over a fake release tree, and feeds the readers verify_release_tree.ps1 uses (tools/windows/release_tree_checks.psm1) output that Windows builds of
SoapySDRUtil 0.8.1 and dsd-neo printed under Wine. The release jobs run both scripts on the real tree; this test
cannot build it.

Every file and every child's output is read with CRLF turned into LF before it is matched.
"""

import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from test_stage_runtime_dlls import pe_image

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
PWSH = os.environ.get("DSD_NEO_TEST_PWSH") or shutil.which("pwsh")
REQUIRE_TOOLS = os.environ.get("DSD_NEO_REQUIRE_TEST_TOOLS") == "1"
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

# overlay -> (upstream, pin name prefix, library overlay it links)
MODULES = {
    "soapy-airspy": ("pothosware/SoapyAirspy", "SOAPY_AIRSPY", "airspy"),
    "soapy-hackrf": ("pothosware/SoapyHackRF", "SOAPY_HACKRF", "hackrf"),
}
LIBRARIES = {
    "hackrf": ("greatscottgadgets/hackrf", "HACKRF"),
}
MODULE_FILES = ("airspySupport.dll", "HackRFSupport.dll")
# Repository notice -> its name in the ZIP's share/doc/dsd-neo/licenses/.
WINDOWS_NOTICES = {
    "vcpkg-ports/soapy-airspy/copyright": "SoapyAirspy-MIT.txt",
    "vcpkg-ports/soapy-hackrf/copyright": "SoapyHackRF-MIT.txt",
    "vcpkg-ports/hackrf/copyright": "libhackrf-BSD-3-Clause.txt",
    "android/third_party/libusb/COPYING": "libusb-LGPL-2.1-or-later.txt",
    "packaging/windows/PThreads4W-LICENSE.txt": "PThreads4W-Apache-2.0.txt",
    "packaging/windows/PThreads4W-NOTICE.txt": "PThreads4W-NOTICE.txt",
    "packaging/windows/PThreads4W-CONTRIBUTORS.txt": "PThreads4W-CONTRIBUTORS.txt",
    "packaging/windows/libusb-COPYRIGHT.txt": "libusb-COPYRIGHT.txt",
}
# Holders named in the sources compiled into SoapySDR.dll and SoapySDRUtil.exe at SOAPY_SDR_SHA.
SOAPYSDR_HOLDERS = ("Josh Blum", "Bastille Networks", "Coburn Wightman", "Nicholas Corgan")
# Holders and attributions in the libusb 1.0.30 sources the Windows libusb-1.0.dll is built from
# (msvc/libusb_dll.vcxproj), and in its AUTHORS file.
LIBUSB_ATTRIBUTIONS = (
    "Copyright © 2009-2012 Pete Batard <pete@akeo.ie>",
    "Copyright © 2016-2018 Chris Dickens <christopher.a.dickens@gmail.com>",
    "Copyright © 2014 Red Hat, Inc.", "Dmitry Fleytman <dmitry@daynix.com>", "Pavel Gurvich <pavel@daynix.com>",
    "Copyright © 2001 Johannes Erdfelt <johannes@erdfelt.com>", "Copyright © 2007-2009 Daniel Drake <dsd@gentoo.org>",
    "Copyright © 2012-2023 Nathan Hjelm <hjelmn@cs.unm.edu>", "Copyright © 2019-2022 Google LLC. All rights reserved.",
    "Copyright © 2012-2013 Peter Stuge <peter@stuge.se>", "Copyright © 2010 Michael Plante <michael.plante@gmail.com>",
    "Copyright © 2013 Hans de Goede <hdegoede@redhat.com>", "Copyright © 2024 Sylvain Fasel <sylvain@sonatique.net>",
    "Copyright © 2012-2013 RealVNC Ltd.", "With contributions from Michael Plante, Orin Eman et al.",
    "Parts of this code adapted from libusb-win32-v1 by Stephan Meyer",
    "HID Reports IOCTLs inspired from HIDAPI by Alan Ott, Signal 11 Software",
    "Hash table functions adapted from glibc, by Ulrich Drepper et al.", "Major code testing contribution by Xiaofan Chen",
    "Other contributors:")
MIT_GRANT = "The above copyright notice and this permission notice shall be included"
# The overlays copied from vcpkg's builtin ports (MIT, Microsoft) -> where they came from. The others are dsd-neo's own.
VCPKG_PORT_COPIES = {
    "soapysdr": "ports/soapysdr at the manifest baseline",
    "rtlsdr": "ports/rtlsdr at the manifest baseline",
    "portaudio": "ports/portaudio at port-version 8",
    "ncurses": "ports/ncurses at the manifest baseline",
}
BSD_BINARY_CLAUSE = "Redistributions in binary form must reproduce the above copyright notice"
# The libraries vcpkg builds for the Windows ZIP -> {name under licenses/: where staging takes it from}. Each carries
# the copyright file its port installs and the notices of its pinned sources that file lacks.
VCPKG_LIBRARY_NOTICES = {
    "codec2.dll": {"codec2-copyright.txt": "vcpkg:codec2", "codec2-LGPL-2.1.txt": "repo:LICENSES/codec2-LGPL-2.1.txt",
                   "codec2-NOTICES.txt": "repo:LICENSES/codec2-NOTICES.txt",
                   "LicenseRef-codec2-varicode.txt": "repo:LICENSES/LicenseRef-codec2-varicode.txt"},
    "portaudio.dll": {"portaudio-copyright.txt": "vcpkg:portaudio",
                      "portaudio-NOTICES.txt": "repo:packaging/windows/portaudio-NOTICES.txt"},
    "pdcurses.dll": {"LicenseRef-PDCurses-Public-Domain.txt": "repo:LICENSES/LicenseRef-PDCurses-Public-Domain.txt"},
    "libcrypto-3-x64.dll": {"openssl-copyright.txt": "vcpkg:openssl", "Apache-2.0.txt": "repo:LICENSES/Apache-2.0.txt",
                            "OpenSSL-AUTHORS.md": "repo:LICENSES/OpenSSL-AUTHORS.md",
                            "OpenSSL-NOTICES.txt": "repo:LICENSES/OpenSSL-NOTICES.txt",
                            "LicenseRef-OpenSSL-KISA-SEED.txt": "repo:LICENSES/LicenseRef-OpenSSL-KISA-SEED.txt"},
    "libcurl.dll": {"curl-copyright.txt": "vcpkg:curl", "curl-NOTICES.txt": "repo:LICENSES/curl-NOTICES.txt"},
    "libexpat.dll": {"expat-copyright.txt": "vcpkg:expat",
                     "expat-NOTICES.txt": "repo:LICENSES/expat-NOTICES.txt"},
    "z.dll": {"zlib-copyright.txt": "vcpkg:zlib"},
    "sndfile.dll": {"libsndfile-copyright.txt": "vcpkg:libsndfile",
                    "libsndfile-GSM610-COPYRIGHT.txt": "repo:LICENSES/libsndfile-GSM610-COPYRIGHT.txt",
                    "libsndfile-ALAC-Apache-2.0.txt": "repo:LICENSES/libsndfile-ALAC-Apache-2.0.txt",
                    "LicenseRef-Sun-G72x.txt": "repo:LICENSES/LicenseRef-Sun-G72x.txt",
                    "libsndfile-NOTICES.txt": "repo:LICENSES/libsndfile-NOTICES.txt"},
    "flac.dll": {"libflac-copyright.txt": "vcpkg:libflac"},
    "ogg.dll": {"libogg-copyright.txt": "vcpkg:libogg"},
    "vorbis.dll": {"libvorbis-copyright.txt": "vcpkg:libvorbis",
                   "libvorbis-lpc-NOTICE.txt": "repo:LICENSES/libvorbis-lpc-NOTICE.txt"},
    "opus.dll": {"opus-copyright.txt": "vcpkg:opus", "opus-NOTICES.txt": "repo:LICENSES/opus-NOTICES.txt"},
    "libmp3lame.dll": {"mp3lame-copyright.txt": "vcpkg:mp3lame",
                       "mp3lame-LGPL-2.1.txt": "repo:android/third_party/libusb/COPYING",
                       "mp3lame-README.txt": "repo:LICENSES/mp3lame-README.txt",
                       "mp3lame-LICENSE.txt": "repo:LICENSES/mp3lame-LICENSE.txt",
                       "mp3lame-NOTICES.txt": "repo:LICENSES/mp3lame-NOTICES.txt"},
    "mpg123.dll": {"mpg123-copyright.txt": "vcpkg:mpg123",
                   "mpg123-AUTHORS.txt": "repo:LICENSES/mpg123-AUTHORS.txt",
                   "mpg123-NOTICES.txt": "repo:LICENSES/mpg123-NOTICES.txt"},
}
# Words each library's notices must hold: the notices and holders of its compiled sources its port's file lacks.
VCPKG_LIBRARY_WORDS = {
    "codec2.dll": ("Copyright (c) 2003-2010, Mark Borgerding", "Copyright (c) 2017-2018 Mozilla",
                   "Copyright (C) 2006, Matthew C. Valenti", "Copyright (C) 2018 James C. Ahlstrom",
                   "Copyright (C) 2010 Perens LLC", "Copyright (C) 2016 Jeroen Vreeken",
                   "Neither the author nor the names of any contributors", BSD_BINARY_CLAUSE.replace(" notice", ""),
                   "GNU General Public License version 2.1", "GNU LESSER GENERAL PUBLIC LICENSE"),
    "portaudio.dll": ("Andrew Baldwin", "Robert Marsanyi", "David Viens", "Dmitry Kostjuchenko"),
    "pdcurses.dll": ("The files in this directory are released to the public domain.",),
    "libcrypto-3-x64.dll": ("Eric A. Young, Tim J. Hudson", "KISA(Korea Information Security Agency)",
                            "CC0 Public Domain Dedication", "Cryptography Research, Inc.", "Ribose Inc.",
                            "Kungliga Tekniska", "Richard Levitte", "Intel Corporation", "CloudFlare, Inc.",
                            "This is the list of OpenSSL authors for copyright purposes."),
    "libcurl.dll": ("Linus Nielsen Feltzing", "Steve Holme", "Florin Petriuc", "Evgeny Grin", "Markus Moeller",
                    "Marc Hoersken", "Mark Salisbury"),
    "libexpat.dll": ("Thai Open Source Software Center Ltd", "Karl Waclawek", "Sebastian Pipping", "Rhodri James"),
    "sndfile.dll": ("Technische Universitaet Berlin", "Apache License", "Sun Microsystems, Inc.",
                    "Copyright (c) 2011 Apple Inc.", "Xiph.Org Foundation and contributors", "Arthur Taylor"),
    "vorbis.dll": ('Technische Universita"t Berlin', "preserved without modification"),
    "opus.dll": ("Cisco Systems, INC", "Google Inc.", "Arm Limited", "Koen Vos", "Vidyo", "Parrot", "John Ridges"),
    "libmp3lame.dll": ("Mark Taylor", "Takehiro Tominaga", "Robert Hegemann", "David Robinson and Glen Sawyer",
                       "Ron Mayer", "Michael Hipp", "GNU LESSER GENERAL PUBLIC LICENSE"),
    "mpg123.dll": ("Thorsten Glaser", "Mikko Tommila", "Michael Hipp"),
}
# The end-user notice the ZIP carries with the Visual C++ runtime DLLs, its source and the license the record names.
RUNTIME_NOTICE = "licenses/LicenseRef-Microsoft-Visual-CPP-Runtime.txt"
RUNTIME_NOTICE_SOURCE = "packaging/windows/LicenseRef-Microsoft-Visual-CPP-Runtime.txt"
RUNTIME_LICENSE = "LicenseRef-Microsoft-Visual-CPP-Runtime"
# The files the Windows ZIP carries in bin\ and lib\: those of the 2.12.1 ZIP (its file list, in its spellings) and
# the SoapySDR program, modules and library added since. Each must be one a component record lists.
WINDOWS_RELEASE_FILES = (
    "dsd-neo.exe", "airspy.dll", "codec2.dll", "FLAC.dll", "libcrypto-3-x64.dll", "libcurl.dll", "libexpat.dll",
    "libmp3lame.DLL", "libusb-1.0.dll", "mbe-neo.dll", "mpg123.dll", "MSVCP140.dll", "ogg.dll", "opus.dll",
    "pdcurses.dll", "portaudio.dll", "pthreadVC3.dll", "rtlsdr.dll", "sndfile.dll", "SoapySDR.dll", "VCRUNTIME140.dll",
    "VCRUNTIME140_1.dll", "vorbis.dll", "vorbisenc.dll", "z.dll",
    "SoapySDRUtil.exe", "hackrf.dll", "airspySupport.dll", "HackRFSupport.dll")


def read(path):
    return (ROOT / path).read_text(encoding="utf-8").replace("\r\n", "\n")


def read_bytes(path):
    return (ROOT / path).read_bytes().replace(b"\r\n", b"\n")


def normalized(text):
    return ANSI.sub("", text.replace("\r\n", "\n"))


def pins():
    result = {}
    for line in read("tools/ci-dependency-pins.env").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            result[key] = value
    return result


def required_notices():
    """The notices every Windows release tree must hold under share/doc/dsd-neo, as the scripts list them."""
    match = re.search(r"\$script:ReleaseNotices = @\(([^\n]+)\)", read("tools/windows/release_tree_checks.psm1"))
    return re.findall(r"'([^']+)'", match.group(1))


def windows_records(inventory=None):
    """File name (lower case) -> [(component id, {notice under share/doc/dsd-neo: where staging takes it from})], from
    the windows_release of each record in legal/components.json."""
    if inventory is None:
        inventory = json.loads(read("legal/components.json"))
    records = {}
    for component in inventory["components"]:
        release = component.get("windows_release")
        if release:
            for name in release["binaries"]:
                records.setdefault(name.lower(), []).append((component["id"], release["notices"]))
    return records


# The tree's record of the SHA-256 of each notice no record reviews, under share/doc/dsd-neo.
NOTICE_MANIFEST = "notices.sha256"


def notice_hash(data):
    """A notice's SHA-256 with CRLF folded to LF, as tools/legal_inventory.py hashes a reviewed notice."""
    return hashlib.sha256(data.replace(b"\r\n", b"\n")).hexdigest()


def release_notices(root):
    """The notices a release tree must carry under share/doc/dsd-neo -> (where staging takes it from, its reviewed
    hash or None), from the tree's components.json: the notices every tree carries and those of the records of its
    files in bin/ and lib/."""
    inventory = json.loads((root / "share/doc/dsd-neo/legal/components.json").read_text(encoding="utf-8"))
    sources, reviewed = {}, {}
    for component in inventory["components"]:
        for notice, source in component.get("windows_release", {}).get("notices", {}).items():
            sources.setdefault(notice, source)
            if source.startswith("repo:"):
                reviewed[notice] = component["notice_sha256"][source[5:]]
    records = windows_records(inventory)
    names = set(required_notices())
    for path in [*(root / "bin").rglob("*"), *(root / "lib").rglob("*")]:
        if path.is_file() and path != root / "bin/dlls-manifest.txt":
            for _, notices in records.get(path.name.lower(), []):
                names.update(notices)
    return {name: (sources.get(name), reviewed.get(name)) for name in sorted(names)}


def job_block(workflow, job):
    """The text of one job in a workflow: from its key at two spaces to the next job's key."""
    lines = read(f".github/workflows/{workflow}").splitlines()
    start = lines.index(f"  {job}:")
    end = next((i for i in range(start + 1, len(lines)) if re.match(r"  [A-Za-z0-9_-]+:\s*$", lines[i])), len(lines))
    return "\n".join(lines[start:end])


class SoapyModulePackaging(unittest.TestCase):
    def setUp(self):
        self.pins = pins()

    def port(self, name):
        return json.loads(read(f"vcpkg-ports/{name}/vcpkg.json")), read(f"vcpkg-ports/{name}/portfile.cmake")

    def assert_pinned(self, portfile, repo, pin, dynamic_only=True):
        self.assertRegex(self.pins[f"{pin}_SHA"], r"^[0-9a-f]{40}$")
        self.assertRegex(self.pins[f"{pin}_TARBALL_SHA512"], r"^[0-9a-f]{128}$")
        self.assertIn(f"    REPO {repo}\n", portfile)
        self.assertIn(f"    REF {self.pins[pin + '_SHA']}\n", portfile)
        self.assertIn(f"    SHA512 {self.pins[pin + '_TARBALL_SHA512']}\n", portfile)
        self.assertEqual("vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)" in portfile, dynamic_only)
        self.assertNotIn("HEAD_REF", portfile)

    def test_module_overlays(self):
        for name, (repo, pin, library) in MODULES.items():
            with self.subTest(port=name):
                manifest, portfile = self.port(name)
                self.assertEqual(manifest["name"], name)
                self.assertEqual(manifest["license"], "MIT")
                self.assertEqual(manifest["supports"], "windows & !uwp & !static")
                self.assertEqual(manifest["dependencies"][:2], [library, "soapysdr"])
                self.assert_pinned(portfile, repo, pin)
                self.assertIn('"-DCMAKE_DISABLE_FIND_PACKAGE_Git=ON"', portfile)
                # Linked to the overlay's library in each configuration, never to one found elsewhere.
                self.assertRegex(portfile, r'OPTIONS_RELEASE\n\s+"-D\w+=\$\{CURRENT_INSTALLED_DIR\}/lib/' + library)
                self.assertRegex(portfile, r'OPTIONS_DEBUG\n\s+"-D\w+=\$\{CURRENT_INSTALLED_DIR\}/debug/lib/' + library)
                for policy in ("ALLOW_DLLS_IN_LIB", "DLLS_WITHOUT_LIBS", "DLLS_WITHOUT_EXPORTS", "EMPTY_INCLUDE_FOLDER"):
                    self.assertIn(f"set(VCPKG_POLICY_{policy} enabled)", portfile)
                self.assertIn('INSTALL "${CMAKE_CURRENT_LIST_DIR}/copyright"', portfile)

    def test_library_overlays(self):
        for name, (repo, pin) in LIBRARIES.items():
            with self.subTest(port=name):
                manifest, portfile = self.port(name)
                self.assertEqual(manifest["name"], name)
                self.assertEqual(manifest["license"], "BSD-3-Clause")
                self.assertEqual(manifest["dependencies"][:2], ["libusb", "pthreads"])
                self.assert_pinned(portfile, repo, pin)
                self.assertIn('COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt"', portfile)
                cmake = read(f"vcpkg-ports/{name}/CMakeLists.txt")
                self.assertIn("PThreads4W::PThreads4W", cmake)
                self.assertIn('"${LIBUSB_LIBRARY}"', cmake)
                self.assertIn(f"DESTINATION include/lib{name}", cmake)
        hackrf = read("vcpkg-ports/hackrf/CMakeLists.txt")
        self.assertIn("add_library(hackrf host/libhackrf/src/hackrf.c)", hackrf)
        self.assertIn('LIBRARY_VERSION="0.9.2" LIBRARY_RELEASE="2026.01.3"', hackrf)
        self.assertEqual(json.loads(read("vcpkg-ports/hackrf/vcpkg.json"))["version-string"], "2026.01.3")

    def test_soapysdr_overlay_builds_the_tool_from_the_appimage_commit(self):
        manifest, portfile = self.port("soapysdr")
        self.assertEqual((manifest["version"], manifest["port-version"]), ("0.8.1", 1))
        self.assertIn("tools", manifest["features"])
        # A copy of the builtin port, which leaves linkage to its "supports" expression.
        self.assert_pinned(portfile, "pothosware/SoapySDR", "SOAPY_SDR", dynamic_only=False)
        self.assertRegex(portfile, r"FEATURES\n\s+tools\s+ENABLE_APPS\n")
        self.assertRegex(portfile, r"OPTIONS_DEBUG\n\s+-DENABLE_APPS=OFF\n")
        self.assertIn("vcpkg_copy_tools(TOOL_NAMES SoapySDRUtil AUTO_CLEAN)", portfile)
        # The AppImage builds SoapySDR from the same pin.
        self.assertIn('soapy_sdr_sha="$SOAPY_SDR_SHA"', read(".github/workflows/linux-appimage.yaml"))

    def test_manifest_takes_the_modules_and_the_tool_on_windows_only(self):
        entries = {e["name"]: e for e in json.loads(read("vcpkg.json"))["dependencies"] if isinstance(e, dict)}
        # The macOS DMG bundles the library too, but not SoapySDRUtil.
        self.assertEqual(entries["soapysdr"], {"name": "soapysdr", "platform": "windows | osx",
                                               "features": [{"name": "tools", "platform": "windows"}]})
        for name in MODULES:
            self.assertEqual(entries[name], {"name": name, "platform": "windows"})

    def test_notices_name_every_holder(self):
        hackrf = read("vcpkg-ports/hackrf/copyright")
        for line in ("Copyright (c) 2012-2022 Great Scott Gadgets <info@greatscottgadgets.com>",
                     "Copyright (c) 2012, Jared Boone <jared@sharebrained.com>",
                     "Copyright (c) 2013, Benjamin Vernoux <titanmkd@gmail.com>",
                     'Copyright (c) 2022 László Baráth "Uncle Dino" HA7DN <https://github.com/Sasszem>',
                     '"HackRF" is a trademark of Great Scott Gadgets.',
                     "Neither the name of Great Scott Gadgets nor the names of its contributors", BSD_BINARY_CLAUSE):
            self.assertIn(line, hackrf)
        for name, holders in (("soapy-airspy", ["Copyright (c) 2015 Charles J. Cliffe"]),
                              ("soapy-hackrf", ["Copyright (c) 2015-2016 Wei Jiang",
                                                "Copyright (c) 2015-2017 Josh Blum",
                                                "Copyright (c) 2017 Kevin Mehall"])):
            notice = read(f"vcpkg-ports/{name}/copyright")
            with self.subTest(port=name):
                self.assertIn("The MIT License (MIT)", notice)
                self.assertIn(MIT_GRANT, notice)
                for holder in holders:
                    self.assertIn(holder, notice)

    def test_notices_name_every_soapysdr_libusb_and_pthreads4w_holder(self):
        soapysdr = read("LICENSES/SoapySDR-COPYRIGHT.txt")
        for holder in SOAPYSDR_HOLDERS:
            self.assertIn(holder, soapysdr)
        self.assertIn(self.pins["SOAPY_SDR_SHA"], soapysdr)
        self.assertIn("This file has no copyright assigned and is placed in the Public Domain.", soapysdr)
        libusb = read("packaging/windows/libusb-COPYRIGHT.txt")
        for line in LIBUSB_ATTRIBUTIONS:
            self.assertIn(line, libusb)
        # The NOTICE shipped with PThreads4W names its contributors by pointing at this file.
        self.assertIn("listed in the file\nCONTRIBUTORS included with this software", read("packaging/windows/PThreads4W-NOTICE.txt"))
        self.assertIn("Ross Johnson", read("packaging/windows/PThreads4W-CONTRIBUTORS.txt"))
        self.assertIn("licenses/SoapySDR-COPYRIGHT.txt",
                      re.search(r"required=\(([^\n]+)\)", read(".github/workflows/linux-appimage.yaml")).group(1).split())

    def test_legal_records(self):
        inventory = {c["id"]: c for c in json.loads(read("legal/components.json"))["components"]}
        expected = {
            "soapysdr": (["linux", "macos", "windows"], "BSL-1.0", "SOAPY_SDR_SHA",
                         ["LICENSES/BSL-1.0.txt", "LICENSES/SoapySDR-COPYRIGHT.txt"]),
            # SoapySDR's program, which only the Windows ZIP carries (the AppImage builds no programs).
            "soapysdr-util": (["windows"], "BSL-1.0", "SOAPY_SDR_SHA", ["LICENSES/BSL-1.0.txt",
                                                                       "LICENSES/SoapySDR-COPYRIGHT.txt"]),
            "soapy-airspy": (["windows"], "MIT", "SOAPY_AIRSPY_SHA", ["vcpkg-ports/soapy-airspy/copyright"]),
            "soapy-hackrf": (["windows"], "MIT", "SOAPY_HACKRF_SHA", ["vcpkg-ports/soapy-hackrf/copyright"]),
            "libhackrf": (["windows"], "BSD-3-Clause", "HACKRF_SHA", ["vcpkg-ports/hackrf/copyright"]),
            # The vcpkg port's libusb, in the Windows ZIP and the macOS DMG.
            "libusb": (["macos", "windows"], "LGPL-2.1-or-later", None, ["android/third_party/libusb/COPYING",
                                                                       "packaging/windows/libusb-COPYRIGHT.txt",
                                                                       "LICENSES/libusb-macos-NOTICES.txt"]),
            "pthreads4w": (["windows"], "Apache-2.0", None, ["packaging/windows/PThreads4W-LICENSE.txt",
                                                             "packaging/windows/PThreads4W-NOTICE.txt",
                                                             "packaging/windows/PThreads4W-CONTRIBUTORS.txt"]),
        }
        required = read("tools/legal_inventory.py")
        for ident, (platforms, license_id, pin, notices) in expected.items():
            with self.subTest(component=ident):
                component = inventory[ident]
                self.assertEqual(sorted(component["platforms"]), platforms)
                self.assertEqual(component["distribution"], ["binary"])
                self.assertEqual((component["license_declared"], component["license_concluded"]),
                                 (license_id, license_id))
                self.assertEqual(component.get("pin"), pin)
                self.assertEqual(component["notices"], notices)
                for notice in notices:
                    digest = hashlib.sha256((ROOT / notice).read_bytes().replace(b"\r\n", b"\n")).hexdigest()
                    self.assertEqual(component["notice_sha256"][notice], digest)
                self.assertIn(f'"{ident}"', required)
        self.assertEqual(read("LICENSES/BSL-1.0.txt").splitlines()[0], "Boost Software License - Version 1.0 - "
                         "August 17th, 2003")
        self.assertIn("licenses/BSL-1.0.txt", re.search(r"required=\(([^\n]+)\)",
                                                        read(".github/workflows/linux-appimage.yaml")).group(1).split())

    def test_visual_cpp_runtime_record(self):
        # The Windows ZIP carries Microsoft's runtime DLLs: the record names the files it may carry, Microsoft's terms
        # and the end-user notice the ZIP carries with them, and legal/msvc-runtime.md records how the release meets
        # each condition of those terms and the GPL risk it accepts.
        component = next(c for c in json.loads(read("legal/components.json"))["components"]
                         if c["id"] == "msvc-runtime")
        self.assertEqual((component["platforms"], component["distribution"]), (["windows"], ["binary"]))
        self.assertEqual((component["license_declared"], component["license_concluded"]),
                         (RUNTIME_LICENSE, RUNTIME_LICENSE))
        self.assertEqual((component["copyright_permission"], component["gpl_compatibility"]),
                         ("notice-retained", "reviewed-terms"))
        self.assertNotIn("finding", component)
        self.assertEqual(component["notices"], [RUNTIME_NOTICE_SOURCE])
        # legal/msvc-runtime.md comes with the legal component; the release checks require it with the runtime.
        self.assertEqual(component["windows_release"]["notices"],
                         {RUNTIME_NOTICE: "repo:" + RUNTIME_NOTICE_SOURCE, "legal/msvc-runtime.md": "legal-component"})
        self.assertIn("legal/msvc-runtime.md", component["assessment"])
        # The end-user notice: whose the files are, their terms and that they bind those files only, no endorsement,
        # and where Microsoft publishes the runtime.
        notice = " ".join(read(RUNTIME_NOTICE_SOURCE).split())
        for words in ("are Microsoft's Visual C++ runtime", "not licensed under the GNU General Public License",
                      "Do not modify them.", "Do not reverse engineer, decompile or disassemble them.",
                      "Distribute them only unmodified, as part of dsd-neo, and under these same terms.",
                      "These terms do not limit any right the GNU General Public License gives you",
                      "not made, endorsed or supported by Microsoft",
                      "https://visualstudio.microsoft.com/license-terms/",
                      "https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution",
                      "https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist"):
            self.assertIn(words, notice)
        # The terms, quoted from Microsoft, how the release meets each one, the build basis, and the GPL text and the
        # FSF reading that disagree, kept as an accepted risk.
        record = " ".join(" ".join(line.strip().removeprefix(">") for line in read("legal/msvc-runtime.md").splitlines())
                          .split())
        for words in ("add significant primary functionality to it in your applications;",
                      "require distributors and external end users to agree to terms that protect the Distributable "
                      "Code at least as much as these license terms; and",
                      "indemnify, defend, and hold harmless Microsoft from any claims",
                      "use Microsoft's trademarks in your applications' names",
                      "An Excluded License is one that requires, as a condition of use, modification or distribution",
                      "you may copy and distribute with your program any of the files within the following folder and "
                      "its subfolders except as noted below. You may not modify these files.",
                      "(ii) hosted on Microsoft Azure for you",
                      "GitHub hosts Windows runners … on virtual machines in Azure.",
                      "maintainer's own Visual Studio Community licence",
                      "\"System Libraries\" of an executable work include anything",
                      "need not be included in conveying the object code work.",
                      "removes the words ``unless that component itself accompanies the executable.''",
                      "You may not distribute these libraries in compiled DLL form with the program.",
                      "a known, accepted risk", "Not verified.",
                      "https://www.gnu.org/licenses/gpl-faq.html#WindowsRuntimeAndGPL",
                      "https://gplv3.fsf.org/gpl-rationale-2006-01-16.html"):
            self.assertIn(words, record)
        status = read("legal/status.md")
        open_findings = status[status.index("## Findings requiring resolution"):status.index("## Resolved findings")]
        self.assertNotIn("MSVC-RUNTIME", open_findings)
        self.assertIn("| MSVC-RUNTIME |", status[status.index("## Resolved findings"):])
        self.assertIn(RUNTIME_NOTICE, read("docs/release-assets-checklist.md"))
        self.assertIn("legal/msvc-runtime.md", read("THIRD_PARTY.md"))
        # The DLLs the 2.12.1 ZIP carried are on the list, and nothing that a toolset's CRT folder lacks.
        files = component["windows_release"]["binaries"]
        for name in ("msvcp140.dll", "vcruntime140.dll", "vcruntime140_1.dll"):
            self.assertIn(name, files)
        for name in files:
            self.assertRegex(name, r"^(concrt140|msvcp140(_1|_2|_atomic_wait|_codecvt_ids)?|vccorlib140|"
                                   r"vcruntime140(_1|_threads)?)\.dll$")
        for url in ("https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution",
                    "https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution",
                    "https://visualstudio.microsoft.com/license-terms/"):
            self.assertIn(url, component["assessment"])
            self.assertIn(url, read("THIRD_PARTY.md"))
        self.assertIn("| MSVC-RUNTIME |", read("legal/status.md"))
        self.assertIn('"msvc-runtime"', read("tools/legal_inventory.py"))
        self.assertIn("Get-RecordedRuntimeFiles -ComponentsPath (Join-Path $treeRoot "
                      "'share/doc/dsd-neo/legal/components.json')", read("tools/windows/verify_release_tree.ps1"))

    def test_codec2_notices_ship_with_every_package_that_carries_codec2(self):
        # The Windows ZIP, the APK and the AppImage carry codec2 from the pinned commit, and each carries its LGPL 2.1
        # text and its holders' and BSD notices: LICENSES/ is installed whole by the desktop packages' legal component
        # and copied whole into the APK; the AppImage's check requires them, and the APK's every notice the record's
        # android_release map names.
        component = next(c for c in json.loads(read("legal/components.json"))["components"] if c["id"] == "codec2")
        self.assertEqual(sorted(component["platforms"]), ["android", "linux", "macos", "windows"])
        self.assertEqual(component["notices"], ["LICENSES/codec2-LGPL-2.1.txt", "LICENSES/codec2-NOTICES.txt",
                                                "LICENSES/LicenseRef-codec2-varicode.txt"])
        self.assertEqual(component["pin"], "CODEC2_SHA")
        self.assertIn("DIRECTORY LICENSES/", read("CMakeLists.txt"))
        self.assertIn('file(COPY "${REPO_DIR}/LICENSES/" DESTINATION "${_doc_dir}/licenses")',
                      read("cmake/stage_android_package.cmake"))
        appimage = re.search(r"required=\(([^\n]+)\)", read(".github/workflows/linux-appimage.yaml")).group(1).split()
        android = list(component["android_release"]["notices"])
        self.assertIn("tools/legal_inventory.py --release-notices android", read(".github/workflows/android-ci.yaml"))
        checklist = read("docs/release-assets-checklist.md")
        for name in ("licenses/codec2-LGPL-2.1.txt", "licenses/codec2-NOTICES.txt",
                     "licenses/LicenseRef-codec2-varicode.txt"):
            with self.subTest(notice=name):
                self.assertIn(name, appimage)
                self.assertIn(name, android)
                self.assertIn(name, checklist)
        # The varicode files' grant ships unmodified in a note of its own that states how dsd-neo reads it; the reading
        # is the LGPL 2.1, so no LicenseRef enters the license expression.
        varicode = read("LICENSES/LicenseRef-codec2-varicode.txt")
        self.assertIn("it under the terms of the GNU General Public License version 2.1,", varicode)
        self.assertIn("records a reading, not a separate license", varicode)
        self.assertNotIn("LicenseRef", component["license_declared"])
        self.assertNotIn("General Public License version 2.1", read("LICENSES/codec2-NOTICES.txt"))
        # The LGPL text is codec2's own COPYING, unmodified.
        self.assertEqual([line.strip() for line in read("LICENSES/codec2-LGPL-2.1.txt").splitlines()[:2]],
                         ["GNU LESSER GENERAL PUBLIC LICENSE", "Version 2.1, February 1999"])

    def test_portaudio_overlay_keeps_asio_out(self):
        # PortAudio 19.7 compiles the Steinberg ASIO SDK in whenever it finds one, whatever PA_USE_ASIOSDK says.
        _, portfile = self.port("portaudio")
        windows = portfile[portfile.index("if(VCPKG_TARGET_IS_WINDOWS)"):portfile.index("elseif(")]
        self.assertIn("-DPA_USE_ASIO=OFF\n", windows)
        self.assert_pinned(portfile, "PortAudio/portaudio", "PORTAUDIO", dynamic_only=False)
        self.assertEqual(next(c for c in json.loads(read("legal/components.json"))["components"]
                              if c["id"] == "portaudio")["pin"], "PORTAUDIO_SHA")

    def test_vcpkg_port_copies_keep_the_vcpkg_notice(self):
        # vcpkg's MIT license asks that its notice stay with copies of its files: the overlays copied from its ports
        # keep it beside them, and every portfile says whose it is.
        notice = read("vcpkg-ports/LICENSE-vcpkg.txt")
        self.assertTrue(notice.startswith("MIT License\n\nCopyright (c) Microsoft Corporation\n"), notice[:80])
        self.assertIn(MIT_GRANT, notice.replace("\n", " "))
        baseline = json.loads(read("vcpkg.json"))["builtin-baseline"]
        overlays = sorted(p.name for p in (ROOT / "vcpkg-ports").iterdir() if p.is_dir())
        for name in overlays:
            portfile = read(f"vcpkg-ports/{name}/portfile.cmake")
            with self.subTest(port=name):
                if name in VCPKG_PORT_COPIES:
                    self.assertTrue(portfile.startswith("# SPDX-License-Identifier: GPL-3.0-or-later AND MIT\n"),
                                    portfile[:80])
                    header = " ".join(line.lstrip("# ") for line in portfile.split("\n\n", 1)[0].splitlines())
                    for words in ("Copyright (c) Microsoft Corporation", "vcpkg-ports/LICENSE-vcpkg.txt",
                                  VCPKG_PORT_COPIES[name], "dsd-neo's changes"):
                        self.assertIn(words, header)
                    if "manifest baseline" in VCPKG_PORT_COPIES[name]:
                        self.assertIn(baseline, header)
                else:
                    self.assertTrue(portfile.startswith("# SPDX-License-Identifier: GPL-3.0-or-later\n"),
                                    portfile[:80])
        component = next(c for c in json.loads(read("legal/components.json"))["components"] if c["id"] == "vcpkg-ports")
        self.assertEqual((component["license_declared"], component["license_concluded"]), ("MIT", "MIT"))
        self.assertEqual(component["notices"], ["vcpkg-ports/LICENSE-vcpkg.txt"])
        # Build recipes, not compiled into a release: the packages need not carry the notice.
        self.assertEqual(component["distribution"], ["source"])
        self.assertEqual(sorted(component["paths"]),
                         sorted([f"vcpkg-ports/{name}/" for name in VCPKG_PORT_COPIES] + ["vcpkg-ports/LICENSE-vcpkg.txt"]))
        self.assertIn('"vcpkg-ports"', read("tools/legal_inventory.py"))
        self.assertIn("vcpkg-ports/LICENSE-vcpkg.txt", read("THIRD_PARTY.md"))

    def test_every_release_file_has_a_component_record(self):
        # Each file the ZIP carries maps to the components it is built from and the notices the ZIP carries for them;
        # the release scripts refuse a file no record lists, and a tree without one of its notices.
        records = windows_records()
        sources = re.compile(r"^(repo:[^:]+|build:[^:]+|vcpkg:[a-z0-9-]+|legal-component)$")
        for name in WINDOWS_RELEASE_FILES:
            with self.subTest(file=name):
                self.assertIn(name.lower(), records)
                for _, notices in records[name.lower()]:
                    for notice, source in notices.items():
                        self.assertRegex(notice, r"^(licenses/|legal/|[A-Z_.]+$)")
                        self.assertRegex(source, sources)
        # The libraries vcpkg builds from its own ports ship the copyright file the port installs, where it is
        # upstream's, and the notices and holders of their pinned sources that file lacks.
        for name, expected in VCPKG_LIBRARY_NOTICES.items():
            with self.subTest(file=name):
                (component, notices), = records[name]
                self.assertEqual(notices, {"licenses/" + key: value for key, value in expected.items()})
                text = ""
                for source in expected.values():
                    if source.startswith("repo:"):
                        text += read_bytes(source[5:]).decode("utf-8") + "\n"
                for words in VCPKG_LIBRARY_WORDS.get(name, ()):
                    self.assertIn(words, text)
                # On a case-insensitive file system no new notice may replace the port's copyright file.
                self.assertEqual(len({key.lower() for key in notices}), len(notices))
        self.assertEqual(records["vorbisenc.dll"], records["vorbis.dll"])
        # dsd-neo.exe carries every component compiled into it.
        embedded = {component for component, _ in records["dsd-neo.exe"]}
        self.assertEqual(embedded, {"project", "legacy-isc", "bch3", "rockliff-rs", "wallace-golay", "mixed-origins",
                                    "ezpwd", "tiny-aes", "pffft", "tetra-crypto", "gqrx-scanner"})
        stage = read("tools/windows/stage_release_tree.ps1")
        self.assertIn("Assert-ReleaseBinaryRecords -TreeRoot $stage", stage)
        self.assertIn("Assert-ReleaseBinaryRecords -TreeRoot $treeRoot", read("tools/windows/verify_release_tree.ps1"))

    def test_staging_ships_every_notice(self):
        required = re.search(r"\$script:ReleaseNotices = @\(([^\n]+)\)",
                             read("tools/windows/release_tree_checks.psm1")).group(1)
        self.assertIn("Assert-ReleaseNotices -DocDir $docDir", read("tools/windows/stage_release_tree.ps1"))
        self.assertIn("Assert-ReleaseNotices -DocDir (Join-Path $treeRoot 'share/doc/dsd-neo')",
                      read("tools/windows/verify_release_tree.ps1"))
        shipped_from = {}
        for entries in windows_records().values():
            for _, notices in entries:
                shipped_from.update(notices)
        for source, shipped in WINDOWS_NOTICES.items():
            with self.subTest(notice=shipped):
                self.assertTrue((ROOT / source).is_file(), source)
                self.assertEqual(shipped_from[f"licenses/{shipped}"], f"repo:{source}")
                self.assertIn(f"'licenses/{shipped}'", required)
        self.assertIn("'licenses/BSL-1.0.txt'", required)
        self.assertIn("'licenses/SoapySDR-COPYRIGHT.txt'", required)
        self.assertIn("$expectedModules = @(" + ", ".join(f"'{m}'" for m in MODULE_FILES) + ")",
                      read("tools/windows/stage_release_tree.ps1"))

    def test_release_jobs_stage_and_verify(self):
        stage = "./tools/windows/stage_release_tree.ps1"
        verify = "./tools/windows/verify_release_tree.ps1"
        pr = job_block("windows-pr.yaml", "build-native-msvc")
        self.assertEqual(pr.count(stage), 1)
        self.assertEqual(pr.count(verify), 1)
        self.assertLess(pr.index(stage), pr.index(verify))
        # Pull requests read the vcpkg cache and publish nothing.
        self.assertIn("packages: read", pr)
        self.assertRegex(pr, r"index\.json,read\"")
        for word in ("upload-artifact", "attest", "sbom"):
            self.assertNotIn(word, pr)
        release = job_block("windows-ci.yaml", "package-native-msvc")
        self.assertEqual(release.count(stage), 1)
        # Once on the staged tree, once on the ZIP unpacked in a new folder.
        self.assertEqual(release.count(verify), 2)
        self.assertLess(release.index(stage), release.index(verify))
        self.assertLess(release.index("Archive native MSVC ZIP"), release.rindex(verify))
        self.assertIn("ExtractToDirectory", release[release.index("Archive native MSVC ZIP"):release.rindex(verify)])
        # The staging, the librtlsdr probe and the smoke runs live in the scripts now.
        for old in ("Copy-Item", "no transfer left, canceling", "& $exe -h"):
            self.assertNotIn(old, release)
        # The developer shell the build runs in sets VCToolsInstallDir and VCToolsRedistDir for the staging, which
        # takes the Visual C++ runtime from that toolset's folder and checks that the build's compilers are its own.
        for job in (pr, release):
            self.assertEqual(job.count("uses: TheMrMilchmann/setup-msvc-dev@"), 1)
            self.assertLess(job.index("uses: TheMrMilchmann/setup-msvc-dev@"), job.index("cmake --preset win-msvc-release"))
            self.assertLess(job.index("cmake --preset win-msvc-release"), job.index(stage))


# Output of Windows builds run under Wine, CRLF kept: a mingw-w64 SoapySDRUtil 0.8.1 with one module (SoapyAirspy)
# installed, with that module's airspy.dll removed from bin\, and with no module folder; the 2.12.1 release ZIP's
# dsd-neo.exe probing the Airspy driver with the module present and a bare -i soapy without modules. The release
# jobs check the same text from the MSVC builds on Windows.
INFO_ONE_MODULE = (
    "######################################################\r\n"
    "##     Soapy SDR -- the SDR abstraction library     ##\r\n"
    "######################################################\r\n\r\n"
    "Lib Version: v0.8.1-unknown\r\nAPI Version: v0.8.0\r\nABI Version: v0.8\r\n"
    "Install root: D:\\t\\mingw\r\n"
    "Search path:  D:\\t\\mingw/lib/SoapySDR/modules0.8\r\n"
    "Module found: D:\\t\\mingw/lib/SoapySDR/modules0.8/libairspySupport.dll (0.2.1)\r\n"
    "Available factories... airspy\r\n"
    "Available converters...\r\n -  CF32 -> [CF32, CS16, CS8, CU16, CU8]\r\n -    U8 -> [F32, S16, S8]\r\n")
INFO_LOAD_FAILED = (
    "Lib Version: v0.8.1-unknown\r\nAPI Version: v0.8.0\r\nABI Version: v0.8\r\n"
    "Install root: D:\\t\\mingw-nodll\r\n"
    "Search path:  D:\\t\\mingw-nodll/lib/SoapySDR/modules0.8\r\n"
    "Module found: D:\\t\\mingw-nodll/lib/SoapySDR/modules0.8/libairspySupport.dll\r\n"
    "  LoadLibrary() failed: Module not found.\r\n\r\n"
    "Available factories... No factories found!\r\nAvailable converters...\r\n")
INFO_NO_MODULES = (
    "Lib Version: v0.8.1-unknown\r\nAPI Version: v0.8.0\r\nABI Version: v0.8\r\n"
    "Install root: D:\\t\\mingw-nolib\r\n"
    "Search path:  D:\\t\\mingw-nolib/lib/SoapySDR/modules0.8 (missing)\r\n"
    "No modules found!\r\nAvailable factories... No factories found!\r\nAvailable converters...\r\n")
CHECK_LOAD_FAILED_STDERR = (
    "\x1b[1m\x1b[31m[ERROR] SoapySDR::loadModule(D:\\t\\mingw-nodll/lib/SoapySDR/modules0.8/libairspySupport.dll)\r\n"
    "  LoadLibrary() failed: Module not found.\r\r\n\x1b[0m\r\n")
PROBE_AIRSPY = (
    "NOTICE: SoapySDR InputNOTICE: : driver=airspy\r\nAudio In Device: soapy:driver=airspy\r\n"
    "SoapySDR: enumerate found no devices for args 'driver=airspy'.\r\n"
    "SoapySDR: exception in Device::make: Unable to open AirSpy device\r\n"
    "SoapySDR: failed to create device for args 'driver=airspy'.\r\n"
    "Failed to open SoapySDR device with args: driver=airspy.\r\nsoft stopping...\r\n"
    "Failed to open radio stream.\r\nNOTICE: Exiting.\r\n")
PROBE_BARE_NO_MODULES = (
    "NOTICE: SoapySDR InputNOTICE: : default device args\r\nAudio In Device: soapy\r\n"
    "SoapySDR: enumerate found no devices for args ''.\r\n"
    "SoapySDR: exception in Device::make: SoapySDR::Device::make() no match\r\n"
    "SoapySDR: failed to create device for args ''.\r\nFailed to open SoapySDR device.\r\nsoft stopping...\r\n"
    "Failed to open radio stream.\r\nNOTICE: Exiting.\r\n")
# The same --info as printInfo() (apps/SoapySDRUtil.cpp) prints it for the two modules of a release tree.
INFO_RELEASE = (
    "Lib Version: v0.8.1-unknown\r\nAPI Version: v0.8.0\r\nABI Version: v0.8\r\n"
    "Install root: C:\\Users\\me\\dsd-neo\r\n"
    "Search path:  C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8\r\n"
    "Module found: C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8/airspySupport.dll (0.2.1)\r\n"
    "Module found: C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8/HackRFSupport.dll (0.3.6)\r\n"
    "Available factories... airspy, hackrf\r\nAvailable converters...\r\n")


def dumpbin(lines):
    return "\r\n".join(["Microsoft (R) COFF/PE Dumper Version 14.44.35217.0",
                        "Copyright (C) Microsoft Corporation.  All rights reserved.", "", ""] + lines) + "\r\n"


DEPENDENTS = dumpbin(["Dump of file HackRFSupport.dll", "", "File Type: DLL", "",
                      "  Image has the following dependencies:", "", "    hackrf.dll", "    SoapySDR.dll",
                      "    MSVCP140.dll", "    KERNEL32.dll", "", "  Image has the following delay load dependencies:",
                      "", "    WS2_32.dll", "", "  Summary", "", "        1000 .data", "        9000 .text", ""])

# A DLL whose name holds a space, in the delay-load list.
DEPENDENTS_SPACED = DEPENDENTS.replace("    WS2_32.dll", "    WS2_32.dll\r\n    Vendor Support.dll")
# The report of a DLL that imports nothing: no dependency list, only the summary.
DEPENDENTS_NONE = dumpbin(["Dump of file forwarder.dll", "", "File Type: DLL", "", "  Summary", "",
                           "        1000 .rdata", "        1000 .reloc", ""])
# dumpbin /HEADERS of an x64 DLL, the parts the readers and the hardening check read.
HEADERS = dumpbin(["Dump of file HackRFSupport.dll", "", "PE signature found", "", "File Type: DLL", "",
                   "FILE HEADER VALUES", "            8664 machine (x64)", "               6 number of sections", "",
                   "OPTIONAL HEADER VALUES", "             20B magic # (PE32+)",
                   "                   High Entropy Virtual Addresses", "                   Dynamic base",
                   "                   NX compatible", "", "  Summary", "", "        1000 .data",
                   "        9000 .text", ""])
# A diagnostic MSVC prints about the file it read, named as the tool was given it: "<file> : warning LNK....".
FILE_WARNING = "HackRFSupport.dll : warning LNK4048: Invalid format file; ignored"
# Reads one dumpbin report with one reader (Get-DumpbinDependents or Get-DumpbinMachine); prints the result as JSON,
# or the reader's refusal.
STRICT_READER = r"""param([string]$Module, [string]$File, [string]$Reader)
Import-Module $Module -Force
$text = [System.IO.File]::ReadAllText($File)
try {
    $result = @(& $Reader $text)
    ConvertTo-Json -Compress -InputObject @{ result = $result }
} catch {
    [Console]::Out.WriteLine('FAILED: ' + $_.Exception.Message)
    exit 1
}
"""

# How the release scripts treat a DLL a binary imports (Get-DllKind).
DLL_KINDS = {
    # The Windows DLLs the release imports (the 2.12.1 ZIP and a later nightly), in the spellings the import tables
    # use, and nothing else from System32: a driver package's DLL there, or one that comes with Windows but that no
    # release binary imports yet, must be in bin\ until it is added to the list.
    **{name: "Windows" for name in ("ADVAPI32.dll", "bcrypt.dll", "CRYPT32.dll", "GDI32.dll", "IPHLPAPI.DLL",
                                    "KERNEL32.dll", "ole32.dll", "Secur32.dll", "SETUPAPI.dll", "SHLWAPI.dll",
                                    "USER32.dll", "WINMM.dll", "WS2_32.dll")},
    **{name: "Other" for name in ("ucrtbase.dll", "mfc42.dll", "msvcp60.dll", "msvcrt.dll", "libusbK.dll",
                                  "winusb.dll", "SHELL32.dll", "airspy.dll")},
    "api-ms-win-crt-heap-l1-1-0.dll": "ApiSet", "ext-ms-win-ntuser-uicontext-ext-l1-1-0.dll": "ApiSet",
    **{name: "Redistributable" for name in ("VCRUNTIME140.dll", "vcruntime140_1.dll", "VCRUNTIME140_THREADS.dll",
                                            "MSVCP140.dll", "msvcp140_2.dll", "MSVCP140_ATOMIC_WAIT.dll",
                                            "MSVCP140_CODECVT_IDS.dll", "concrt140.dll", "vcomp140.dll",
                                            "mfc140u.dll", "mfc140enu.dll", "mfc140deu.dll")},
    # Microsoft's debug runtime, which a /MDd build imports and which may not be redistributed.
    **{name: "DebugRuntime" for name in ("msvcp140d.dll", "ucrtbased.dll", "VCRUNTIME140D.dll", "vcruntime140_1d.dll",
                                         "VCRUNTIME140_THREADSD.dll", "MSVCP140D_ATOMIC_WAIT.dll",
                                         "msvcp140d_codecvt_ids.dll", "msvcp140_2d.dll", "concrt140d.dll",
                                         "vccorlib140d.dll", "vcomp140d.dll", "vcamp140d.dll", "mfc140d.dll",
                                         "mfc140ud.dll", "mfcm140ud.dll", "msvcr120d.dll")},
}

# Runs the release scripts and the readers; prints the readers' results as JSON on its last line.
READERS = r"""param([string]$Module, [string]$Fixtures)
$ErrorActionPreference = 'Stop'
Import-Module $Module -Force
$text = @{}
foreach ($file in Get-ChildItem -LiteralPath $Fixtures -File) {
    $text[$file.BaseName] = [System.IO.File]::ReadAllText($file.FullName)
}
$result = [ordered]@{}
foreach ($name in 'info_one_module', 'info_load_failed', 'info_no_modules', 'info_release') {
    $result[$name] = Get-SoapyUtilInfo $text[$name]
}
foreach ($name in 'info_one_module', 'info_load_failed', 'check_load_failed_stderr', 'probe_airspy',
                  'probe_bare_no_modules') {
    $result['error_' + $name] = Find-SoapyLoadError $text[$name]
}
$result['probe_messages'] = Get-SoapyProbeMessages
$result['no_match'] = Get-SoapyNoMatchMessage
$result['check_text'] = ConvertTo-CheckText $text['check_load_failed_stderr']
$result['path'] = ConvertTo-ComparablePath 'D:\t\mingw/lib/SoapySDR/modules0.8/'
$result['dependents'] = @(Get-DumpbinDependents $text['dependents'])
$result['kinds'] = [ordered]@{}
foreach ($name in $text['kinds'].Split("`n", [System.StringSplitOptions]::RemoveEmptyEntries)) {
    $result['kinds'][$name.Trim()] = Get-DllKind -Name $name.Trim()
}
$result | ConvertTo-Json -Depth 5 -Compress
"""

RUNNER = r"""param([string]$Module, [string]$Program, [string]$Mode)
$ErrorActionPreference = 'Stop'
Import-Module $Module -Force
try {
    $run = Invoke-IsolatedProcess -FilePath $Program -Arguments @('-I', '-c', $Mode, 'a=b c') -PathValue 'only-this' `
        -TimeoutSeconds 5
    $run | ConvertTo-Json -Compress
} catch {
    [Console]::Out.WriteLine('FAILED: ' + $_.Exception.Message)
    exit 1
}
"""

# Runs stage_release_tree.ps1 and prints a failure's message whole (pwsh's own error view wraps it).
STAGE = r"""param([string]$Script, [string]$Build, [string]$Stage, [string]$Installed)
try {
    & $Script -BuildDir $Build -StageDir $Stage -InstalledDir $Installed
} catch {
    [Console]::Error.WriteLine('FAILED: ' + $_.Exception.Message)
    exit 1
}
"""

# dumpbin for the staging run: each binary's dependencies come from DEPS (name -> list), as dumpbin prints them.
FAKE_DUMPBIN = r"""#!{python} -I
import sys
from pathlib import Path

DEPS = {deps!r}
name = Path(sys.argv[-1].replace("\\", "/")).name
lines = ["Dump of file " + name, "", "File Type: DLL", "", "  Image has the following dependencies:", ""]
lines += ["    " + dep for dep in DEPS.get(name, ["KERNEL32.dll"])] + ["", "  Summary", ""]
sys.stdout.write("\r\n".join(lines))
"""

# cmake --install <build> --prefix <stage> --component legal, as the legal component installs the repository's
# LICENSES/ and legal/ and the mbelib notices.
FAKE_CMAKE = r"""#!{python} -I
import shutil
import sys
from pathlib import Path

args = sys.argv[1:]
with open(Path(sys.argv[0]).resolve().parent / "cmake.log", "a", encoding="utf-8") as log:
    log.write(" ".join(args) + "\n")
if args[0] != "--install" or args[2:] != ["--prefix", args[3], "--component", "legal"]:
    sys.exit(2)
doc = Path(args[3]) / "share/doc/dsd-neo"
shutil.copytree({root!r} + "/LICENSES", doc / "licenses", dirs_exist_ok=True)
shutil.copytree({root!r} + "/legal", doc / "legal", dirs_exist_ok=True)
for name in ("ISC.txt", "LicenseRef-ETSI-EN-300-395-2-data.txt", "legal/status.md", "legal/patents.md"):
    path = doc / "licenses/mbelib-neo" / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("mbelib notice fixture\n")
"""

# The DLLs of the fake vcpkg tree and what each imports.
DEPENDENCIES = {
    "dsd-neo.exe": ["rtlsdr.dll", "airspy.dll", "SoapySDR.dll", "portaudio.dll", "codec2.dll", "sndfile.dll",
                    "libcrypto-3-x64.dll", "VCRUNTIME140.dll", "KERNEL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll"],
    "sndfile.dll": ["opus.dll", "KERNEL32.dll"],
    "rtlsdr.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "airspy.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "hackrf.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "SoapySDR.dll": ["MSVCP140.dll", "VCRUNTIME140.dll", "KERNEL32.dll"],
    "airspySupport.dll": ["airspy.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll"],
    "HackRFSupport.dll": ["hackrf.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll"],
    "SoapySDRUtil.exe": ["SoapySDR.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "KERNEL32.dll"],
}
INSTALLED_BIN = ("rtlsdr.dll", "airspy.dll", "hackrf.dll", "SoapySDR.dll", "libusb-1.0.dll", "pthreadVC3.dll",
                 "portaudio.dll", "codec2.dll", "sndfile.dll", "opus.dll", "libcrypto-3-x64.dll", "unused.dll")
# The copyright file vcpkg's portaudio port installs, which the ZIP carries as licenses/portaudio-copyright.txt.
PORTAUDIO_COPYRIGHT = b"PortAudio Portable Real-Time Audio Library\r\nCopyright (c) 1999-2011 Ross Bencina, Phil Burk\r\n"
# The copyright files the other ports of the fake tree install.
PORT_COPYRIGHTS = {port: f"{port} copyright file\r\n".encode() for port in ("codec2", "libsndfile", "opus", "openssl")}
# The Visual C++ runtime the binaries import, which staging takes from the toolset's redistributable folder of the
# Visual Studio that vswhere finds, never from vcpkg's bin\.
RUNTIME_DLLS = ("MSVCP140.dll", "VCRUNTIME140.dll")


# Fakes for running verify_release_tree.ps1 off Windows. dumpbin.exe answers /HEADERS and /DEPENDENTS, in any case,
# from report.json beside it (its "unhardened" files lack NX compatibility, its "machines" name a file's machine line). SoapySDRUtil.exe and dsd-neo.exe model SoapySDR 0.8.1: they load each module in
# <their folder>/../lib/SoapySDR/modules0.8, which fails when the module's library is not in their own folder, and
# print what the real programs print (the probe messages are the drivers' own).
FAKE_VERIFY_DUMPBIN = r"""#!{python} -I
import json
import sys
from pathlib import Path

report = json.loads((Path(sys.argv[0]).resolve().parent / "report.json").read_text())
option, path = sys.argv[1], sys.argv[2]
name = Path(path).name
kind = report.get("types", {{}}).get(name, "EXECUTABLE IMAGE" if name.endswith(".exe") else "DLL")
lines = ["Microsoft (R) COFF/PE Dumper Version 14.44.35217.0", "", "Dump of file " + path, "", "File Type: " + kind, ""]
if option.upper() == "/HEADERS":
    lines += ["PE signature found", "", "File Type: " + kind, "", "FILE HEADER VALUES"]
    machine = report.get("machines", {{}}).get(name, "8664 machine (x64)")
    lines += ["            " + machine] if machine else []
    lines += ["                   High Entropy Virtual Addresses", "                   Dynamic base"]
    if name not in report.get("unhardened", []):
        lines += ["                   NX compatible"]
elif option.upper() == "/DEPENDENTS":
    lines += ["  Image has the following dependencies:", ""]
    lines += ["    " + dep for dep in report["dependents"].get(name, ["KERNEL32.dll"])]
else:
    sys.exit(2)
lines += ["", "  Summary", "", "        1000 .data"]
sys.stdout.write("\r\n".join(lines) + "\r\n")
"""

FAKE_SOAPY_PROGRAM = r"""#!{python} -I
import sys
from pathlib import Path

LIBRARIES = {{"airspySupport.dll": ("airspy", "airspy.dll"), "HackRFSupport.dll": ("hackrf", "hackrf.dll")}}
MESSAGES = {{"airspy": "Unable to open AirSpy device", "hackrf": "no hackrf device matches"}}
program = Path(sys.argv[0])
bin_dir = program.parent
root = bin_dir.parent
module_dir = root / "lib/SoapySDR/modules0.8"
modules = sorted(module_dir.glob("*.dll")) if module_dir.is_dir() else []
drivers = []
for module in modules:
    driver, library = LIBRARIES[module.name]
    if (bin_dir / library).exists():
        drivers.append(driver)
    else:
        sys.stderr.write("[ERROR] SoapySDR::loadModule(%s/lib/SoapySDR/modules0.8/%s)\r\n"
                         "  LoadLibrary() failed: The specified module could not be found.\r\n" % (root, module.name))
args = sys.argv[1:]
if program.name == "SoapySDRUtil.exe":
    if args == ["--info"]:
        print("Lib Version: v0.8.1-unknown\r\nABI Version: v0.8\r\nInstall root: %s\r" % root)
        for module in modules:
            print("Module found: %s/lib/SoapySDR/modules0.8/%s (0.2.1)\r" % (root, module.name))
        print("Available factories... %s\r" % (", ".join(sorted(drivers)) or "No factories found!"))
        sys.exit(0)
    driver = args[0].split("=", 1)[1]
    print("Loading modules... done\r\nChecking driver '%s'... %s\r" % (driver, "PRESENT" if driver in drivers else
                                                                      "MISSING!"))
    sys.exit(0 if driver in drivers else 1)
if args == ["-h"]:
    print("Usage: dsd-neo [options]\r")
    sys.exit(0)
source = args[args.index("-i") + 1]
driver = source.split("driver=", 1)[1] if "driver=" in source else ""
if driver in drivers:
    message = MESSAGES[driver]
elif not driver and len(drivers) > 1:
    message = "SoapySDR::Device::make() no driver specified and no enumeration results"
elif not driver and drivers:
    message = MESSAGES[drivers[0]]
else:
    message = "SoapySDR::Device::make() no match"
sys.stderr.write("SoapySDR: exception in Device::make: %s\r\nFailed to open radio stream.\r\n" % message)
sys.exit(1)
"""

# Runs verify_release_tree.ps1 and prints a failure's message whole.
VERIFY = r"""param([string]$Script, [string]$Root)
try {
    & $Script -Root $Root -TimeoutSeconds 60
} catch {
    [Console]::Error.WriteLine('FAILED: ' + $_.Exception.Message)
    exit 1
}
"""

# A release tree's binaries and what each imports (dumpbin /DEPENDENTS).
RELEASE_DEPENDENTS = dict(DEPENDENCIES, **{
    "libusb-1.0.dll": ["KERNEL32.dll"], "pthreadVC3.dll": ["KERNEL32.dll"], "portaudio.dll": ["KERNEL32.dll"],
    "MSVCP140.dll": ["VCRUNTIME140.dll", "KERNEL32.dll"], "VCRUNTIME140.dll": ["KERNEL32.dll"],
})

@unittest.skipUnless(PWSH or REQUIRE_TOOLS, "pwsh is required")
class SoapyReleaseScripts(unittest.TestCase):
    def setUp(self):
        if not PWSH:
            self.fail("pwsh is required (DSD_NEO_REQUIRE_TEST_TOOLS=1)")
        self.temp = tempfile.TemporaryDirectory(prefix="dsd-soapy-release-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def pwsh(self, script_text, *args, env=None):
        script = self.work / f"run-{len(list(self.work.glob('run-*.ps1')))}.ps1"
        script.write_text(script_text, encoding="utf-8")
        result = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-File", str(script), *map(str, args)],
                                env=env, cwd=self.work, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        return (result.returncode, normalized(result.stdout.decode("utf-8", "replace")),
                normalized(result.stderr.decode("utf-8", "replace")))

    def test_scripts_parse(self):
        names = ("stage_runtime_dlls.ps1", "stage_release_tree.ps1", "verify_release_tree.ps1",
                 "release_tree_checks.psm1")
        check = "\n".join(
            f"$e = $null; [void][System.Management.Automation.Language.Parser]::ParseFile('{ROOT / 'tools/windows' / n}',"
            f" [ref]$null, [ref]$e); '{n} ' + $e.Count" for n in names)
        rc, out, err = self.pwsh(check)
        self.assertEqual(rc, 0, err)
        self.assertEqual(out.split(), [word for n in names for word in (n, "0")])

    def test_readers_on_windows_output(self):
        fixtures = self.work / "fixtures"
        fixtures.mkdir()
        for name, text in (("info_one_module", INFO_ONE_MODULE), ("info_load_failed", INFO_LOAD_FAILED),
                           ("info_no_modules", INFO_NO_MODULES), ("info_release", INFO_RELEASE),
                           ("check_load_failed_stderr", CHECK_LOAD_FAILED_STDERR), ("probe_airspy", PROBE_AIRSPY),
                           ("probe_bare_no_modules", PROBE_BARE_NO_MODULES), ("dependents", DEPENDENTS),
                           ("kinds", "\r\n".join(DLL_KINDS))):
            (fixtures / f"{name}.txt").write_bytes(text.encode("utf-8"))
        rc, out, err = self.pwsh(READERS, "-Module", ROOT / "tools/windows/release_tree_checks.psm1",
                                 "-Fixtures", fixtures)
        self.assertEqual(rc, 0, err)
        result = json.loads(out.strip().splitlines()[-1])
        self.assertEqual(result["info_one_module"], {
            "InstallRoot": "D:\\t\\mingw", "Factories": ["airspy"],
            "Modules": ["D:\\t\\mingw/lib/SoapySDR/modules0.8/libairspySupport.dll"]})
        self.assertEqual(result["info_load_failed"]["Factories"], [])
        self.assertEqual(len(result["info_load_failed"]["Modules"]), 1)
        self.assertEqual(result["info_no_modules"], {"InstallRoot": "D:\\t\\mingw-nolib", "Modules": [],
                                                     "Factories": []})
        self.assertEqual(result["info_release"]["Factories"], ["airspy", "hackrf"])
        self.assertEqual([m.rsplit("/", 1)[1] for m in result["info_release"]["Modules"]], list(MODULE_FILES))
        self.assertIsNone(result["error_info_one_module"])
        self.assertEqual(result["error_info_load_failed"], "LoadLibrary() failed")
        self.assertEqual(result["error_check_load_failed_stderr"], "LoadLibrary() failed")
        self.assertIsNone(result["error_probe_airspy"])
        self.assertNotIn("\r", result["check_text"])
        self.assertNotIn("\x1b", result["check_text"])
        self.assertEqual(result["path"], "D:\\t\\mingw\\lib\\SoapySDR\\modules0.8")
        # The probe expectations are what dsd-neo printed.
        self.assertIn(result["probe_messages"]["airspy"], normalized(PROBE_AIRSPY))
        self.assertIn(result["no_match"], normalized(PROBE_BARE_NO_MODULES))
        self.assertNotIn(result["no_match"], normalized(PROBE_AIRSPY))
        self.assertEqual(list(result["probe_messages"]), ["airspy", "hackrf", ""])
        self.assertEqual(result["dependents"], ["hackrf.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll",
                                                "WS2_32.dll"])
        self.assertEqual(result["kinds"], DLL_KINDS)

    def read_dumpbin(self, text, reader="Get-DumpbinDependents"):
        report = self.work / f"report-{len(list(self.work.glob('report-*.txt')))}.txt"
        report.write_bytes(text.encode("utf-8"))
        rc, out, err = self.pwsh(STRICT_READER, "-Module", ROOT / "tools/windows/release_tree_checks.psm1",
                                 "-File", report, "-Reader", reader)
        if rc != 0:
            return None, out + err
        return json.loads(out.strip().splitlines()[-1])["result"], out + err

    def test_dumpbin_readers_read_every_name(self):
        result, output = self.read_dumpbin(DEPENDENTS_SPACED)
        self.assertEqual(result, ["hackrf.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll", "WS2_32.dll",
                                  "Vendor Support.dll"], output)
        # A DLL that imports nothing: a clean report with only the summary is an empty list, not a refusal.
        result, output = self.read_dumpbin(DEPENDENTS_NONE)
        self.assertEqual(result, [], output)
        result, output = self.read_dumpbin(HEADERS, reader="Get-DumpbinMachine")
        self.assertEqual(result, [0x8664], output)

    def test_dumpbin_readers_refuse_lines_they_cannot_read(self):
        # A line of a dependency list that the reader cannot place stops it: skipped, it could hide a DLL.
        cases = (
            ("a path in the dependency list", DEPENDENTS.replace("    WS2_32.dll", "    C:\\vendor\\late.dll"),
             "C:\\vendor\\late.dll"),
            ("a warning in the dependency list",
             DEPENDENTS.replace("    WS2_32.dll", "    LINK : warning LNK4048: Invalid format file; ignored"),
             "LNK4048"),
            # dumpbin exits 0 after a warning, and MSVC names the file it read, not the tool, before the severity.
            # Such a line before the dependency list or after the summary means it read the image only in part,
            # however empty or complete the list looks.
            ("a file's warning before the dependency list",
             DEPENDENTS.replace("  Image has the following dependencies:",
                                FILE_WARNING + "\r\n  Image has the following dependencies:"), FILE_WARNING),
            ("a file's warning after the summary", DEPENDENTS + FILE_WARNING + "\r\n", FILE_WARNING),
            ("a file's warning in a report with no imports", DEPENDENTS_NONE + FILE_WARNING + "\r\n", FILE_WARNING),
            ("a full path's error before the dependency list",
             DEPENDENTS.replace("  Image has the following dependencies:",
                                "C:\\Program Files\\x\\HackRFSupport.dll : fatal error LNK1107: invalid or corrupt file"
                                "\r\n  Image has the following dependencies:"), "LNK1107"),
            ("a lower-case tool name", DEPENDENTS + "dumpbin : error LNK1104: cannot open file\r\n", "LNK1104"),
        )
        # The header reader refuses the same: a machine line read from part of the headers says nothing.
        header_cases = (
            ("a file's warning before the headers",
             HEADERS.replace("PE signature found", FILE_WARNING + "\r\nPE signature found"), FILE_WARNING),
            ("a file's warning after the summary", HEADERS + FILE_WARNING + "\r\n", FILE_WARNING),
            ("a tool's error", HEADERS + "DUMPBIN : fatal error LNK1106: invalid file or disk full\r\n", "LNK1106"),
        )
        readers = [(case, "Get-DumpbinDependents") for case in cases]
        readers += [(case, "Get-DumpbinMachine") for case in header_cases]
        for (label, text, words), reader in readers:
            with self.subTest(case=label, reader=reader):
                result, output = self.read_dumpbin(text, reader=reader)
                self.assertIsNone(result, output)
                self.assertIn("FAILED: ", output)
                self.assertIn(words, output)

    def test_isolated_process_runner(self):
        module = ROOT / "tools/windows/release_tree_checks.psm1"
        child = ("import os, sys; sys.stdout.write('out one\\r\\nout two\\r\\n'); sys.stderr.write('err\\r\\n');"
                 "print(repr([os.environ.get('SOAPY_SDR_ROOT'), os.environ.get('SOAPY_SDR_PLUGIN_PATH'),"
                 "os.environ.get('DSD_NEO_CONFIG'), os.environ.get('PATH'), os.listdir('.'), sys.argv[1:]]));"
                 "sys.exit(3)")
        env = dict(os.environ, SOAPY_SDR_ROOT="elsewhere", SOAPY_SDR_PLUGIN_PATH="elsewhere", DSD_NEO_CONFIG="x.ini")
        rc, out, err = self.pwsh(RUNNER, "-Module", module, "-Program", sys.executable, "-Mode", child, env=env)
        self.assertEqual(rc, 0, out + err)
        run = json.loads(out.strip().splitlines()[-1])
        self.assertEqual(run["ExitCode"], 3)
        self.assertEqual(run["StdErr"], "err\n")
        self.assertTrue(run["StdOut"].startswith("out one\nout two\n"), run["StdOut"])
        self.assertIn("[None, None, None, 'only-this', [], ['a=b c']]", run["StdOut"])
        rc, out, err = self.pwsh(RUNNER, "-Module", module, "-Program", sys.executable,
                                 "-Mode", "import time; time.sleep(60)")
        self.assertEqual(rc, 1, out + err)
        self.assertIn("did not finish within 5 s", out)

    def write_dumpbin(self, tools, deps):
        (tools / "dumpbin").write_text(FAKE_DUMPBIN.format(python=sys.executable, deps=deps), encoding="utf-8")
        (tools / "dumpbin").chmod(0o755)

    def fake_tree(self):
        tools = self.work / "tools"
        tools.mkdir()
        self.write_dumpbin(tools, DEPENDENCIES)
        (tools / "cmake").write_text(FAKE_CMAKE.format(python=sys.executable, root=str(ROOT)), encoding="utf-8")
        (tools / "cmake").chmod(0o755)
        build = self.work / "build"
        (build / "apps/dsd-cli/Release").mkdir(parents=True)
        (build / "apps/dsd-cli/Release/dsd-neo.exe").write_bytes(pe_image(b"dsd-neo", dll=False))
        for directory, names in (("pffft-licenses", ("pffft-LICENSE.txt", "pffft-NOTICES.txt")),
                                 ("tetra_crypto-licenses", ("tetra_crypto-LICENSE.txt",))):
            (build / directory).mkdir()
            for name in names:
                (build / directory / name).write_text(name + "\n")
        installed = self.work / "vcpkg_installed/x64-windows-dynamic-release"
        (installed / "bin").mkdir(parents=True)
        for name in INSTALLED_BIN:
            (installed / "bin" / name).write_bytes(pe_image(name.encode()))
        (installed / "share/portaudio").mkdir(parents=True)
        (installed / "share/portaudio/copyright").write_bytes(PORTAUDIO_COPYRIGHT)
        for port, text in PORT_COPYRIGHTS.items():
            (installed / "share" / port).mkdir(parents=True)
            (installed / "share" / port / "copyright").write_bytes(text)
        (installed / "include/SoapySDR").mkdir(parents=True)
        (installed / "include/SoapySDR/Version.h").write_text(
            '#pragma once\r\n#define SOAPY_SDR_API_VERSION 0x00080000\r\n#define SOAPY_SDR_ABI_VERSION "0.8"\r\n')
        modules = installed / "lib/SoapySDR/modules0.8"
        modules.mkdir(parents=True)
        for name in MODULE_FILES:
            (modules / name).write_bytes(pe_image(name.encode()))
        # vcpkg_copy_tools deploys the tool's DLLs beside it; the ZIP takes them from bin\ instead.
        (installed / "tools/soapysdr").mkdir(parents=True)
        (installed / "tools/soapysdr/SoapySDRUtil.exe").write_bytes(pe_image(b"SoapySDRUtil", dll=False))
        (installed / "tools/soapysdr/SoapySDR.dll").write_bytes(pe_image(b"tool-folder copy"))
        windows = self.work / "windows"
        (windows / "System32").mkdir(parents=True)
        (windows / "System32/KERNEL32.dll").write_bytes(pe_image(b"system"))
        self.program_files = self.work / "pf86"
        installer = self.program_files / "Microsoft Visual Studio/Installer"
        installer.mkdir(parents=True)
        (installer / "vswhere.exe").write_text(f"#!/bin/sh\necho '{self.work / 'vs'}'\n", encoding="utf-8")
        (installer / "vswhere.exe").chmod(0o755)
        self.crt = self.work / "vs/VC/Redist/MSVC/14.50.35719/x64/Microsoft.VC145.CRT"
        self.crt.mkdir(parents=True)
        for name in RUNTIME_DLLS:
            (self.crt / name).write_bytes(pe_image(name.encode()))
        # The developer shell of the toolset that built the tree: its compilers, as CMake records them, and the
        # variables it sets. Its runtime folder has a version of its own, which the installation's version files pair
        # with the toolset's, as the developer shell reads them.
        self.toolset = self.toolset_folder(self.work / "vs", "14.51.36231")
        self.version_files(self.work / "vs", "14.51.36231", "14.50.35719")
        self.shell = {"VCToolsInstallDir": f"{self.toolset}/", "VCToolsRedistDir": f"{self.crt.parent.parent}/"}
        self.installed = installed
        self.write_cache(build, self.toolset)
        return tools, build, installed, windows

    def toolset_folder(self, install, version):
        """A Visual Studio installation's toolset folder VC/Tools/MSVC/<version> with its compilers."""
        toolset = install / "VC/Tools/MSVC" / version
        (toolset / "bin/Hostx64/x64").mkdir(parents=True, exist_ok=True)
        for name in ("cl.exe", "link.exe"):
            (toolset / "bin/Hostx64/x64" / name).write_bytes(pe_image(name.encode(), dll=False))
        return toolset

    def version_files(self, install, tools_version, redist_version, name="default"):
        """The version files in VC/Auxiliary/Build that pair a toolset with its runtime folder."""
        folder = install / "VC/Auxiliary/Build"
        folder.mkdir(parents=True, exist_ok=True)
        (folder / f"Microsoft.VCToolsVersion.{name}.txt").write_text(tools_version + "\r\n")
        (folder / f"Microsoft.VCRedistVersion.{name}.txt").write_text(redist_version + "\r\n")

    def write_cache(self, build, toolset, installed=None, compiler=None):
        """The CMakeCache.txt of a build whose compilers are toolset's cl.exe, as CMake records the preset's cl, and
        whose vcpkg tree is installed: its VCPKG_INSTALLED_DIR and its triplet."""
        compiler = compiler or f"{toolset}/bin/Hostx64/x64/cl.exe"
        installed = installed or self.installed
        (build / "CMakeCache.txt").write_text(
            "# This is the CMakeCache file.\r\n"
            f"CMAKE_CXX_COMPILER:STRING={compiler}\r\nCMAKE_CXX_COMPILER_LAUNCHER:STRING=sccache\r\n"
            f"CMAKE_C_COMPILER:STRING={compiler}\r\nCMAKE_LINKER:FILEPATH={toolset}/bin/Hostx64/x64/link.exe\r\n"
            f"VCPKG_INSTALLED_DIR:PATH={installed.parent}\r\nVCPKG_TARGET_TRIPLET:STRING={installed.name}\r\n"
            "VCPKG_TARGET_TRIPLET-ADVANCED:INTERNAL=1\r\n")

    def stage(self, tools, build, installed, windows, repo=ROOT):
        stage = self.work / "dist/dsd-neo-msvc"
        env = dict(os.environ, PATH=str(tools), SystemRoot=str(windows))
        env["ProgramFiles(x86)"] = str(self.program_files)
        for name in ("VCToolsInstallDir", "VCToolsRedistDir"):
            env.pop(name, None)
        env.update(self.shell)
        rc, out, err = self.pwsh(STAGE, "-Script", repo / "tools/windows/stage_release_tree.ps1", "-Build", build,
                                 "-Stage", stage, "-Installed", installed, env=env)
        return rc, out + err, stage

    def test_stage_release_tree(self):
        tools, build, installed, windows = self.fake_tree()
        rc, output, stage = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)
        bin_files = sorted(p.name for p in (stage / "bin").iterdir())
        self.assertEqual(bin_files, sorted(["dsd-neo.exe", "SoapySDRUtil.exe", "dlls-manifest.txt"]
                                           + [n for n in INSTALLED_BIN if n != "unused.dll"] + list(RUNTIME_DLLS)))
        for name in RUNTIME_DLLS:
            self.assertEqual((stage / "bin" / name).read_bytes(), (self.crt / name).read_bytes())
        self.assertEqual((stage / "bin/SoapySDR.dll").read_bytes(), pe_image(b"SoapySDR.dll"))
        self.assertEqual(sorted(p.relative_to(stage).as_posix() for p in (stage / "lib").rglob("*")),
                         ["lib/SoapySDR", "lib/SoapySDR/modules0.8"]
                         + sorted(f"lib/SoapySDR/modules0.8/{m}" for m in MODULE_FILES))
        licenses = stage / "share/doc/dsd-neo/licenses"
        for source, shipped in WINDOWS_NOTICES.items():
            self.assertEqual((licenses / shipped).read_bytes(), (ROOT / source).read_bytes(), shipped)
        self.assertEqual((licenses / "BSL-1.0.txt").read_bytes(), (ROOT / "LICENSES/BSL-1.0.txt").read_bytes())
        # A library vcpkg builds from its own port: the ZIP carries the copyright file the port installs, and the
        # notices of its pinned sources from the repository (codec2's kiss_fft and Mozilla notices, libsndfile's GSM
        # 06.10, ALAC and G.72x terms, the holders of Opus's x86 code, OpenSSL's AUTHORS.md and KISA's terms).
        self.assertEqual((licenses / "portaudio-copyright.txt").read_bytes(), PORTAUDIO_COPYRIGHT)
        for port, text in PORT_COPYRIGHTS.items():
            self.assertEqual((licenses / f"{port}-copyright.txt").read_bytes(), text)
        for name in ("portaudio.dll", "codec2.dll", "sndfile.dll", "opus.dll", "libcrypto-3-x64.dll"):
            shipped = ""
            for notice, source in VCPKG_LIBRARY_NOTICES[name].items():
                with self.subTest(file=name, notice=notice):
                    if source.startswith("repo:"):
                        self.assertEqual((licenses / notice).read_bytes(), (ROOT / source[5:]).read_bytes())
                        shipped += (licenses / notice).read_text(encoding="utf-8") + "\n"
            for words in VCPKG_LIBRARY_WORDS[name]:
                with self.subTest(file=name, words=words):
                    self.assertIn(words, shipped)
        # The Visual C++ runtime the tree carries comes with Microsoft's terms for it.
        self.assertEqual((stage / "share/doc/dsd-neo" / RUNTIME_NOTICE).read_bytes(),
                         (ROOT / RUNTIME_NOTICE_SOURCE).read_bytes())
        # The tree records the SHA-256 of each notice it carries that no record reviews: those from the build tree,
        # vcpkg and the legal component, and the notices every tree carries that no record lists.
        docs = stage / "share/doc/dsd-neo"
        expected = [f"{notice_hash((docs / name).read_bytes())}  {name}"
                    for name, (_, reviewed) in release_notices(stage).items() if reviewed is None]
        self.assertEqual((docs / NOTICE_MANIFEST).read_text(encoding="utf-8").splitlines(), expected)
        for name in ("licenses/portaudio-copyright.txt", "licenses/pffft-LICENSE.txt", "licenses/mbelib-neo/ISC.txt",
                     "COPYRIGHT", "legal/components.json"):
            self.assertTrue(any(line.endswith("  " + name) for line in expected), name)
        self.assertFalse(any(line.endswith("  licenses/portaudio-NOTICES.txt") for line in expected))
        log = normalized((tools / "cmake.log").read_text(encoding="utf-8"))
        self.assertEqual(log, f"--install {build} --prefix {stage} --component legal\n")

    def test_stage_release_tree_needs_an_empty_folder(self):
        tools, build, installed, windows = self.fake_tree()
        stage = self.work / "dist/dsd-neo-msvc"
        stage.mkdir(parents=True)
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)
        # A second staging into the same folder would keep the first run's DLLs: the runtime staging keeps a DLL
        # already in bin\, so a module from the new run would ship beside the old hackrf.dll.
        (installed / "bin/hackrf.dll").write_bytes(pe_image(b"a newer hackrf.dll"))
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn(f"is not empty: {stage}", output)
        self.assertEqual((stage / "bin/hackrf.dll").read_bytes(), pe_image(b"hackrf.dll"))

    def test_stage_release_tree_refusals(self):
        tools, build, installed, windows = self.fake_tree()
        modules = installed / "lib/SoapySDR/modules0.8"
        cases = (
            ("an unexpected module", lambda: (modules / "SoapyRemoteSupport.dll").write_bytes(b"MZ"),
             lambda: (modules / "SoapyRemoteSupport.dll").unlink(), "SoapyRemoteSupport.dll"),
            ("a missing module", lambda: (modules / "HackRFSupport.dll").rename(self.work / "held.dll"),
             lambda: (self.work / "held.dll").rename(modules / "HackRFSupport.dll"), "HackRFSupport.dll"),
            ("no SoapySDRUtil", lambda: (installed / "tools/soapysdr/SoapySDRUtil.exe").rename(self.work / "u.exe"),
             lambda: (self.work / "u.exe").rename(installed / "tools/soapysdr/SoapySDRUtil.exe"),
             "SoapySDRUtil.exe not found"),
            ("no ABI version", lambda: (installed / "include/SoapySDR/Version.h").write_text("#pragma once\n"),
             lambda: None, "No SOAPY_SDR_ABI_VERSION"),
        )
        for label, break_tree, repair, message in cases:
            with self.subTest(case=label):
                break_tree()
                rc, output, stage = self.stage(tools, build, installed, windows)
                self.assertEqual(rc, 1, output)
                self.assertIn(message, output)
                repair()
                shutil.rmtree(stage, ignore_errors=True)
        (installed / "include/SoapySDR/Version.h").write_text('#define SOAPY_SDR_ABI_VERSION "0.8"\n')
        # A staged library whose notice is not where its record says, and a staged DLL no record lists.
        copyright = installed / "share/portaudio/copyright"
        copyright.unlink()
        rc, output, stage = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn(f"licenses/portaudio-copyright.txt of portaudio comes from {copyright}", output)
        copyright.write_bytes(PORTAUDIO_COPYRIGHT)
        shutil.rmtree(stage)
        (installed / "bin/vendor.dll").write_bytes(pe_image(b"vendor"))
        self.write_dumpbin(tools, dict(DEPENDENCIES, **{"dsd-neo.exe": DEPENDENCIES["dsd-neo.exe"] + ["vendor.dll"]}))
        rc, output, stage = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("No component record", output)
        self.assertIn(str(stage / "bin/vendor.dll"), output)
        self.write_dumpbin(tools, DEPENDENCIES)
        shutil.rmtree(stage)
        # A module DLL the staging cannot find in bin\ or vcpkg's bin\ stops it, as for dsd-neo.exe.
        (installed / "bin/hackrf.dll").unlink()
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("Missing DLL dependencies", output)
        self.assertIn("hackrf.dll", output)

    def test_stage_release_tree_keeps_notice_sources_in_their_roots(self):
        # A record's repo: or build: source names a file inside the repository or the build tree: staging refuses one
        # that climbs out of it, even to a file that exists. The staging reads the inventory of the repository its
        # script is in, so the script runs from a copy whose other folders are the repository's.
        tools, build, installed, windows = self.fake_tree()
        repo = self.work / "repo"
        (repo / "tools").mkdir(parents=True)
        shutil.copytree(ROOT / "tools/windows", repo / "tools/windows")
        shutil.copytree(ROOT / "legal", repo / "legal")
        for entry in ROOT.iterdir():
            if entry.name not in ("tools", "legal", ".git"):
                (repo / entry.name).symlink_to(entry)
        (self.work / "outside.txt").write_text("not a notice of the release\n")
        components = repo / "legal/components.json"
        original = components.read_text(encoding="utf-8")
        for source in ("build:../outside.txt", "repo:../outside.txt", "repo:legal/../../outside.txt",
                       "build:./../outside.txt", "repo:legal//components.json"):
            with self.subTest(source=source):
                shutil.rmtree(self.work / "dist", ignore_errors=True)
                inventory = json.loads(original)
                portaudio = next(c for c in inventory["components"] if c["id"] == "portaudio")
                portaudio["windows_release"]["notices"]["licenses/outside.txt"] = source
                components.write_text(json.dumps(inventory), encoding="utf-8")
                rc, output, stage = self.stage(tools, build, installed, windows, repo=repo)
                self.assertEqual(rc, 1, output)
                self.assertIn(f"names a source outside its root: '{source}'", output)
                self.assertFalse((stage / "share/doc/dsd-neo/licenses/outside.txt").exists())
        components.write_text(original, encoding="utf-8")
        shutil.rmtree(self.work / "dist", ignore_errors=True)
        rc, output, _ = self.stage(tools, build, installed, windows, repo=repo)
        self.assertEqual(rc, 0, output)

    def test_stage_release_tree_checks_the_text_of_repository_notices(self):
        # A notice staging takes from the repository must hold the text its record reviewed (notice_sha256, CRLF
        # folded to LF): a checkout whose notice changed since the review does not ship. The staging reads the
        # repository its script is in, so the script runs from a copy whose notice files can change.
        tools, build, installed, windows = self.fake_tree()
        repo = self.work / "repo"
        (repo / "tools").mkdir(parents=True)
        shutil.copytree(ROOT / "tools/windows", repo / "tools/windows")
        for name in ("legal", "packaging"):
            shutil.copytree(ROOT / name, repo / name)
        for entry in ROOT.iterdir():
            if entry.name not in ("tools", "legal", "packaging", ".git"):
                (repo / entry.name).symlink_to(entry)
        notice = repo / "packaging/windows/portaudio-NOTICES.txt"
        original = notice.read_bytes()
        # Windows line endings in the checkout hold the same text.
        notice.write_bytes(original.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        rc, output, stage = self.stage(tools, build, installed, windows, repo=repo)
        self.assertEqual(rc, 0, output)
        shutil.rmtree(self.work / "dist")
        notice.write_bytes(original + b"A line added after the review.\n")
        rc, output, stage = self.stage(tools, build, installed, windows, repo=repo)
        self.assertEqual(rc, 1, output)
        self.assertIn(f"{stage / 'share/doc/dsd-neo/licenses/portaudio-NOTICES.txt'}: SHA-256", output)
        self.assertIn("not the reviewed", output)

    def test_stage_release_tree_takes_the_runtime_from_the_toolset(self):
        # The Visual C++ runtime comes from the toolset's redistributable folder, the files Microsoft's terms cover
        # (legal/components.json, msvc-runtime), not from a copy vcpkg's bin\ or the build machine's System32 holds.
        tools, build, installed, windows = self.fake_tree()
        for name in RUNTIME_DLLS:
            (installed / "bin" / name).write_bytes(pe_image(b"vcpkg copy"))
            (windows / "System32" / name).write_bytes(pe_image(b"system copy"))
        rc, output, stage = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)
        for name in RUNTIME_DLLS:
            self.assertEqual((stage / "bin" / name).read_bytes(), pe_image(name.encode()), name)
        shutil.rmtree(stage)
        (self.crt / "MSVCP140.dll").unlink()
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn(f"MSVCP140.dll (imported by", output)
        self.assertIn(f"not in the Visual C++ runtime folder {self.crt}", output)


    def test_stage_release_tree_needs_the_developer_shell_of_the_build(self):
        # The runtime the release carries is that of the toolset that built it: staging takes it from the developer
        # shell's VCToolsRedistDir, so the build's compilers must be that shell's, under its VCToolsInstallDir, and
        # that runtime folder must be the one the same installation pairs with that toolset.
        tools, build, installed, windows = self.fake_tree()
        shell = dict(self.shell)
        older = self.toolset_folder(self.work / "vs", "14.44.35207")
        # A folder whose name starts like the toolset's, and a folder in the toolset that links out of it.
        lookalike = self.toolset_folder(self.work / "vs", "14.51.36231-old")
        (self.toolset / "bin/Hostx64/arm64").symlink_to(older / "bin/Hostx64/x64", target_is_directory=True)
        # Another Visual Studio installation with the same toolset and its runtime, and another runtime folder of this
        # installation that its version files do not pair with the toolset.
        other_install = self.work / "vs-other"
        self.toolset_folder(other_install, "14.51.36231")
        self.version_files(other_install, "14.51.36231", "14.50.35719")
        other_crt = other_install / "VC/Redist/MSVC/14.50.35719/x64/Microsoft.VC145.CRT"
        older_crt = self.work / "vs/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT"
        for crt in (other_crt, older_crt):
            crt.mkdir(parents=True)
            for name in RUNTIME_DLLS:
                (crt / name).write_bytes(pe_image(b"another runtime"))
        versions = self.work / "vs/VC/Auxiliary/Build"
        cases = (
            ("built with another toolset", lambda: self.write_cache(build, older),
             [f"was built with {older}/bin/Hostx64/x64/cl.exe", self.shell["VCToolsInstallDir"]]),
            ("built with a toolset whose name starts like the shell's", lambda: self.write_cache(build, lookalike),
             [f"was built with {lookalike}/bin/Hostx64/x64/cl.exe", self.shell["VCToolsInstallDir"]]),
            ("built with another toolset's compiler linked into the shell's",
             lambda: self.write_cache(build, self.toolset, compiler=f"{self.toolset}/bin/Hostx64/arm64/cl.exe"),
             [f"was built with {self.toolset}/bin/Hostx64/arm64/cl.exe "
              f"({os.path.realpath(older / 'bin/Hostx64/x64/cl.exe')})"]),
            ("built with a compiler not on this machine",
             lambda: self.write_cache(build, self.work / "vs/VC/Tools/MSVC/14.29.30133"),
             ["14.29.30133/bin/Hostx64/x64/cl.exe, which is not on this machine"]),
            ("no VCToolsRedistDir", lambda: self.shell.pop("VCToolsRedistDir"), ["VCToolsRedistDir is not set"]),
            ("no VCToolsInstallDir", lambda: self.shell.pop("VCToolsInstallDir"), ["VCToolsInstallDir is not set"]),
            ("no CMake cache", lambda: (build / "CMakeCache.txt").unlink(), ["No CMakeCache.txt in"]),
            ("the runtime of another installation",
             lambda: self.shell.update(VCToolsRedistDir=f"{other_crt.parent.parent}/"),
             ["is not in the Visual Studio installation of VCToolsInstallDir", str(other_crt.parent.parent)]),
            ("the runtime of another toolset of the installation",
             lambda: self.shell.update(VCToolsRedistDir=f"{older_crt.parent.parent}/"),
             ["is runtime 14.44.35112, which no Microsoft.VCRedistVersion.*.txt", "toolset 14.51.36231"]),
            ("version files that pair the toolset with another runtime",
             lambda: self.version_files(self.work / "vs", "14.51.36231", "14.44.35112"),
             ["is runtime 14.50.35719, which no Microsoft.VCRedistVersion.*.txt"]),
            ("no version files", lambda: shutil.rmtree(versions),
             ["No Microsoft.VCToolsVersion.*.txt", "names the toolset 14.51.36231"]),
        )
        for label, break_shell, words in cases:
            with self.subTest(case=label):
                break_shell()
                try:
                    rc, output, stage = self.stage(tools, build, installed, windows)
                    self.assertEqual(rc, 1, output)
                    for word in words:
                        self.assertIn(word, output)
                    self.assertFalse((stage / "bin/VCRUNTIME140.dll").exists(), output)
                finally:
                    self.shell = dict(shell)
                    self.write_cache(build, self.toolset)
                    self.version_files(self.work / "vs", "14.51.36231", "14.50.35719")
                    shutil.rmtree(self.work / "dist/dsd-neo-msvc", ignore_errors=True)
        # A cache that spells the toolset's folder with backslashes, as on Windows, names the same compilers.
        cache = build / "CMakeCache.txt"
        cache.write_text(cache.read_text().replace(f"{self.toolset}/bin/Hostx64/x64/",
                                                   f"{self.toolset}\\bin\\Hostx64\\x64\\"))
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)
        shutil.rmtree(self.work / "dist/dsd-neo-msvc")
        # So does a cache that reaches the toolset through another path to the same folder: on Windows a short 8.3
        # name (C:/PROGRA~1/Microsoft Visual Studio/...) or a junction, here a symbolic link standing in for them.
        alias = self.work / "PROGRA~1"
        alias.symlink_to(self.work / "vs", target_is_directory=True)
        self.write_cache(build, alias / "VC/Tools/MSVC/14.51.36231")
        rc, output, stage = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)
        self.assertEqual((stage / "bin/VCRUNTIME140.dll").read_bytes(), (self.crt / "VCRUNTIME140.dll").read_bytes())
        shutil.rmtree(self.work / "dist/dsd-neo-msvc")
        # And a developer shell that names its folders through that other path.
        self.write_cache(build, self.toolset)
        self.shell = {"VCToolsInstallDir": f"{alias}/VC/Tools/MSVC/14.51.36231/",
                      "VCToolsRedistDir": f"{alias}/VC/Redist/MSVC/14.50.35719/"}
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)

    def test_stage_release_tree_takes_the_vcpkg_tree_of_the_build(self):
        # The vcpkg tree must be the one the build was configured with (VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET in
        # its CMakeCache.txt): another workspace's tree of the same triplet, or another triplet's, would pair this
        # build's dsd-neo.exe with another build's modules and DLLs. Nothing is staged then.
        tools, build, installed, windows = self.fake_tree()
        other_workspace = self.work / "other-workspace/vcpkg_installed" / installed.name
        other_triplet = installed.parent / "x64-windows-dynamic"
        for copy in (other_workspace, other_triplet):
            shutil.copytree(installed, copy, symlinks=True)
        cache = build / "CMakeCache.txt"
        cases = (
            ("another workspace's tree", other_workspace, lambda: None,
             [f"{other_workspace} is not the vcpkg tree {build} was built against ({installed}, from VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET"]),
            ("another triplet's tree", other_triplet, lambda: None,
             [f"{other_triplet} is not the vcpkg tree {build} was built against ({installed}, from VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET"]),
            ("a build of another triplet", installed, lambda: self.write_cache(build, self.toolset, other_triplet),
             [f"{installed} is not the vcpkg tree {build} was built against ({other_triplet}, from VCPKG_INSTALLED_DIR and VCPKG_TARGET_TRIPLET"]),
            ("no VCPKG_INSTALLED_DIR", installed,
             lambda: cache.write_text(re.sub(r"VCPKG_INSTALLED_DIR:PATH=[^\n]*\n", "", cache.read_text())),
             ["names 0 VCPKG_INSTALLED_DIR entries"]),
            ("no VCPKG_TARGET_TRIPLET", installed,
             lambda: cache.write_text(re.sub(r"VCPKG_TARGET_TRIPLET:STRING=[^\n]*\n", "", cache.read_text())),
             ["names 0 VCPKG_TARGET_TRIPLET entries"]),
        )
        for label, tree, break_cache, words in cases:
            with self.subTest(case=label):
                break_cache()
                try:
                    rc, output, stage = self.stage(tools, build, tree, windows)
                    self.assertEqual(rc, 1, output)
                    for word in words:
                        self.assertIn(word, output)
                    self.assertFalse(stage.exists(), output)
                finally:
                    self.write_cache(build, self.toolset)
                    shutil.rmtree(self.work / "dist", ignore_errors=True)
        # The build's own tree through another path to it, and a cache that spells its folder with backslashes.
        alias = self.work / "vcpkg-alias"
        alias.symlink_to(installed.parent, target_is_directory=True)
        cache.write_text(cache.read_text().replace(f"VCPKG_INSTALLED_DIR:PATH={installed.parent}",
                                                   f"VCPKG_INSTALLED_DIR:PATH={installed.parent}\\".replace("/", "\\")))
        rc, output, _ = self.stage(tools, build, alias / installed.name, windows)
        self.assertEqual(rc, 0, output)

    def test_stage_release_tree_sees_hidden_modules(self):
        # A hidden file in vcpkg's module folder is a module SoapySDR loads like any other: staging must see it.
        tools, build, installed, windows = self.fake_tree()
        (installed / "lib/SoapySDR/modules0.8/.SoapyRemoteSupport.dll").write_bytes(pe_image(b"hidden module"))
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn(".SoapyRemoteSupport.dll", output)

    def release_tree(self):
        """A release tree as stage_release_tree.ps1 leaves it, with fake programs and a fake dumpbin.exe."""
        tools = self.work / "verify-tools"
        tools.mkdir()
        (tools / "dumpbin.exe").write_text(FAKE_VERIFY_DUMPBIN.format(python=sys.executable), encoding="utf-8")
        (tools / "dumpbin.exe").chmod(0o755)
        self.report = {"dependents": json.loads(json.dumps(RELEASE_DEPENDENTS))}
        self.write_report(tools)
        root = self.work / "release"
        bin_dir = root / "bin"
        bin_dir.mkdir(parents=True)
        for name in INSTALLED_BIN + RUNTIME_DLLS:
            if name != "unused.dll":
                (bin_dir / name).write_bytes(b"MZ " + name.encode())
        (bin_dir / "rtlsdr.dll").write_bytes(b"MZ ... no transfer left, canceling ...")
        for name in ("SoapySDRUtil.exe", "dsd-neo.exe"):
            (bin_dir / name).write_text(FAKE_SOAPY_PROGRAM.format(python=sys.executable), encoding="utf-8")
            (bin_dir / name).chmod(0o755)
        modules = root / "lib/SoapySDR/modules0.8"
        modules.mkdir(parents=True)
        for name in MODULE_FILES:
            (modules / name).write_bytes(b"MZ " + name.encode())
        self.write_notices(root)
        windows = self.work / "verify-windows"
        (windows / "System32").mkdir(parents=True)
        (windows / "System32/KERNEL32.dll").write_bytes(b"MZ system")
        return tools, root, windows

    def write_notices(self, root):
        # The legal component's components.json, which holds the records of the tree's files and the runtime DLLs a
        # tree may carry, then the notices every tree carries and those the records of its files name, as staging
        # leaves them: a notice from the repository is the repository's file, which its record reviewed; the others
        # (from the build, vcpkg or the legal component) are fixtures whose hashes the tree's manifest records.
        docs = root / "share/doc/dsd-neo"
        (docs / "legal").mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / "legal/components.json", docs / "legal/components.json")
        for name, (source, _) in release_notices(root).items():
            if name == "legal/components.json":
                continue
            (docs / name).parent.mkdir(parents=True, exist_ok=True)
            if source and source.startswith("repo:"):
                shutil.copyfile(ROOT / source[5:], docs / name)
            elif name in ("COPYRIGHT", "THIRD_PARTY.md") or (name.startswith("legal/") and (ROOT / name).is_file()):
                shutil.copyfile(ROOT / name, docs / name)
            else:
                (docs / name).write_text(f"{name} from {source}\n")
        self.write_manifest(root)

    def write_manifest(self, root):
        """The notice manifest staging writes: the SHA-256 of each notice the tree carries that no record reviews."""
        docs = root / "share/doc/dsd-neo"
        lines = [f"{notice_hash((docs / name).read_bytes())}  {name}\n"
                 for name, (_, reviewed) in release_notices(root).items() if reviewed is None]
        (docs / NOTICE_MANIFEST).write_text("".join(lines), encoding="utf-8")

    def record_files(self, root, *names):
        """Lists more files in the tree's components.json, in a record with no notices, as staging would ship it."""
        components = root / "share/doc/dsd-neo/legal/components.json"
        inventory = json.loads(components.read_text(encoding="utf-8"))
        inventory["components"].append({"id": "test-files", "windows_release": {"binaries": list(names), "notices": {}}})
        components.write_text(json.dumps(inventory), encoding="utf-8")
        self.write_manifest(root)

    def write_report(self, tools):
        (tools / "report.json").write_text(json.dumps(self.report), encoding="utf-8")

    def verify(self, tools, root, windows):
        env = dict(os.environ, PATH=str(tools), SystemRoot=str(windows), RUNNER_TEMP=str(self.work))
        rc, out, err = self.pwsh(VERIFY, "-Script", ROOT / "tools/windows/verify_release_tree.ps1", "-Root", root,
                                 env=env)
        return rc, out + err

    def test_verify_release_tree(self):
        tools, root, windows = self.release_tree()
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)
        self.assertIn(f"Release tree verified: {root}", output)
        for line in ("OK: rtlsdr.dll is built with the librtlsdr transfer patches",
                     "OK: the SoapySDR modules, their libraries and every DLL they import are in place"):
            self.assertIn(line, output)
        # Every program ran: the drivers, --info, -h, three probes, and both damaged copies.
        self.assertEqual(output.count("SoapySDRUtil.exe --check="), 6)
        self.assertEqual(output.count("dsd-neo.exe --frontend none -i soapy"), 6)

    def test_verify_release_tree_refusals(self):
        tools, root, windows = self.release_tree()
        cases = (
            ("a missing library", root / "bin/hackrf.dll", "Expected one hackrf.dll"),
            ("a missing module", root / "lib/SoapySDR/modules0.8/HackRFSupport.dll",
             "modules0.8; found airspySupport.dll"),
            ("an unpatched librtlsdr", root / "bin/rtlsdr.dll", "lacks the reader's 'no transfer left' message"),
        )
        for label, path, message in cases:
            with self.subTest(case=label):
                saved = path.read_bytes()
                if label == "an unpatched librtlsdr":
                    path.write_bytes(b"MZ rtlsdr")
                else:
                    path.unlink()
                try:
                    rc, output = self.verify(tools, root, windows)
                finally:
                    path.write_bytes(saved)
                self.assertEqual(rc, 1, output)
                self.assertIn(message, output)
        # A ZIP that lost a notice, or every notice.
        notice = root / "share/doc/dsd-neo/licenses/PThreads4W-NOTICE.txt"
        notice.unlink()
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn(f"Missing or empty required license file: {notice}", output)
        shutil.rmtree(root / "share")
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("Missing or empty required license file", output)
        self.write_notices(root)
        # A Visual C++ runtime DLL the tree lacks, which the build machine's System32 happens to hold: a clean Windows
        # does not have it.
        (windows / "System32/MSVCP140_ATOMIC_WAIT.dll").write_bytes(b"MZ redistributable")
        self.report["dependents"]["SoapySDR.dll"].append("MSVCP140_ATOMIC_WAIT.dll")
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("SoapySDR.dll -> MSVCP140_ATOMIC_WAIT.dll", output)
        self.report["dependents"]["SoapySDR.dll"].remove("MSVCP140_ATOMIC_WAIT.dll")
        # A dependency report that is not about an image (an object file named like a DLL).
        self.report["types"] = {"hackrf.dll": "COFF OBJECT"}
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("did not report", output)
        self.assertIn("hackrf.dll", output)
        del self.report["types"]

    def assert_verify_refuses(self, tools, root, windows, *messages):
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        for message in messages:
            self.assertIn(message, output)

    def test_verify_release_tree_refuses_the_debug_runtime(self):
        tools, root, windows = self.release_tree()
        # A library built with /MDd imports Microsoft's debug runtime, which may not be redistributed, even when the
        # tree carries a copy.
        for name in ("MSVCP140D.dll", "ucrtbased.dll"):
            with self.subTest(dll=name):
                (root / "bin" / name).write_bytes(b"MZ debug runtime")
                self.report["dependents"][name] = ["KERNEL32.dll"]
                self.report["dependents"]["SoapySDR.dll"].append(name)
                try:
                    self.assert_verify_refuses(tools, root, windows, f"SoapySDR.dll -> {name}",
                                               "debug Visual C++ runtime")
                finally:
                    self.report["dependents"]["SoapySDR.dll"].remove(name)
                    (root / "bin" / name).unlink()

    def test_verify_release_tree_refuses_dlls_only_the_build_machine_has(self):
        tools, root, windows = self.release_tree()
        # A DLL a driver package put in the build machine's System32 is not part of Windows: a clean machine lacks it.
        (windows / "System32/libusbK.dll").write_bytes(b"MZ driver package")
        self.report["dependents"]["libusb-1.0.dll"] = ["libusbK.dll", "KERNEL32.dll"]
        self.assert_verify_refuses(tools, root, windows, "libusb-1.0.dll -> libusbK.dll")
        # The listed Windows DLLs need no copy anywhere.
        (windows / "System32/KERNEL32.dll").unlink()
        self.report["dependents"]["libusb-1.0.dll"] = ["KERNEL32.dll", "ADVAPI32.dll"]
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)

    def test_verify_release_tree_refuses_unrecorded_runtime_dlls(self):
        # Every Visual C++ runtime DLL the tree carries, imported or not, must be one the msvc-runtime record the tree
        # ships lists with Microsoft's terms for it.
        tools, root, windows = self.release_tree()
        bin_dir = root / "bin"
        (bin_dir / "vcomp140.dll").write_bytes(b"MZ OpenMP runtime")
        self.report["dependents"]["vcomp140.dll"] = ["KERNEL32.dll"]
        self.report["dependents"]["SoapySDR.dll"].append("vcomp140.dll")
        self.assert_verify_refuses(tools, root, windows, "SoapySDR.dll -> vcomp140.dll", "msvc-runtime record")
        self.report["dependents"]["SoapySDR.dll"].remove("vcomp140.dll")
        self.assert_verify_refuses(tools, root, windows, "\nvcomp140.dll", "msvc-runtime record")
        (bin_dir / "vcomp140.dll").unlink()
        # A tree whose inventory has no such record names no runtime it may carry.
        components = root / "share/doc/dsd-neo/legal/components.json"
        inventory = json.loads(components.read_text(encoding="utf-8"))
        inventory["components"] = [c for c in inventory["components"] if c["id"] != "msvc-runtime"]
        components.write_text(json.dumps(inventory), encoding="utf-8")
        self.assert_verify_refuses(tools, root, windows, f"{components} has no msvc-runtime record")
        self.write_notices(root)
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)

    def test_verify_release_tree_reads_every_dependency_name(self):
        # A DLL whose name holds a space or does not end in .dll is a dependency like any other: the tree must carry it.
        tools, root, windows = self.release_tree()
        for name in ("Vendor Support.dll", "vendor.drv"):
            with self.subTest(dll=name):
                self.report["dependents"]["SoapySDR.dll"].append(name)
                try:
                    self.assert_verify_refuses(tools, root, windows, f"SoapySDR.dll -> {name}")
                finally:
                    self.report["dependents"]["SoapySDR.dll"].remove(name)
        # A dependency list line that is no DLL name stops the check.
        self.report["dependents"]["SoapySDR.dll"].append("C:\\vendor\\late.dll")
        self.assert_verify_refuses(tools, root, windows, "C:\\vendor\\late.dll", str(root / "bin/SoapySDR.dll"))

    def test_verify_release_tree_follows_imports_whatever_the_suffix(self):
        # Windows loads the file an import names whatever its suffix: a vendor.drv in bin\ that SoapySDR.dll imports
        # is a binary of the release like a DLL. The DLLs it imports must be in bin\ as files, and it must be hardened.
        tools, root, windows = self.release_tree()
        bin_dir = root / "bin"
        (bin_dir / "vendor.drv").write_bytes(b"MZ vendor driver")
        self.report["dependents"]["SoapySDR.dll"].append("vendor.drv")
        self.report["dependents"]["vendor.drv"] = ["late.dll", "KERNEL32.dll"]
        self.assert_verify_refuses(tools, root, windows, "vendor.drv -> late.dll")
        # A folder of that name is no DLL.
        (bin_dir / "late.dll").mkdir()
        self.assert_verify_refuses(tools, root, windows, "vendor.drv -> late.dll")
        (bin_dir / "late.dll").rmdir()
        (bin_dir / "late.dll").write_bytes(b"MZ late")
        self.report["dependents"]["late.dll"] = ["KERNEL32.dll"]
        self.record_files(root, "vendor.drv", "late.dll")
        for name in ("vendor.drv", "late.dll"):
            with self.subTest(unhardened=name):
                self.report["unhardened"] = [name]
                self.assert_verify_refuses(tools, root, windows, "missing PE NX compatible", str(bin_dir / name))
        del self.report["unhardened"]
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)
        self.assertIn(f"Windows PE hardening OK: {bin_dir / 'vendor.drv'}", output)

    def test_verify_release_tree_needs_a_record_and_notices_for_every_file(self):
        # Every file of bin\ and lib\ must be one a component record lists, and the tree must carry the notices of
        # the records of its files.
        tools, root, windows = self.release_tree()
        for path in (root / "bin/vendor.dll", root / "lib/SoapySDR/vendor.txt"):
            with self.subTest(file=path.name):
                path.write_bytes(b"MZ vendor")
                try:
                    self.assert_verify_refuses(tools, root, windows, "No component record", str(path))
                finally:
                    path.unlink()
        notice = root / "share/doc/dsd-neo/licenses/portaudio-copyright.txt"
        notice.unlink()
        self.assert_verify_refuses(tools, root, windows, f"portaudio.dll (portaudio): {notice}")
        notice.write_text("")
        self.assert_verify_refuses(tools, root, windows, f"portaudio.dll (portaudio): {notice}")
        self.write_notices(root)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)
        self.assertIn("OK: every file in bin\\ and lib\\ has a component record, and the tree carries its notices",
                      output)

    def test_verify_release_tree_checks_the_text_of_every_notice(self):
        # A file at a notice's path is not the notice: a placeholder, a truncated copy or another component's text
        # passes every check of presence. A notice from the repository must hold the text its record reviewed
        # (notice_sha256 in the tree's components.json, CRLF folded to LF); any other the text staging recorded in the
        # tree's notices.sha256.
        tools, root, windows = self.release_tree()
        docs = root / "share/doc/dsd-neo"
        notices = release_notices(root)
        reviewed = ("licenses/portaudio-NOTICES.txt", "LICENSE", RUNTIME_NOTICE, "licenses/ISC.txt")
        staged = ("licenses/portaudio-copyright.txt", "licenses/pffft-LICENSE.txt", "licenses/mbelib-neo/ISC.txt",
                  "THIRD_PARTY.md", "legal/components.json")
        for name in reviewed:
            self.assertIsNotNone(notices[name][1], name)
        for name in staged:
            self.assertIsNone(notices[name][1], name)
        # A reviewed notice with Windows line endings holds the same text.
        crlf = docs / "licenses/portaudio-NOTICES.txt"
        crlf.write_bytes(crlf.read_bytes().replace(b"\r\n", b"\n").replace(b"\n", b"\r\n"))
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)
        # Every wrong notice is named, so one run shows each kind refused.
        for name in reviewed:
            (docs / name).write_text(name + "\n")
        for name in staged[:-1]:
            with open(docs / name, "a", encoding="utf-8") as notice:
                notice.write("one more line\n")
        self.assert_verify_refuses(tools, root, windows, "Notices whose text is not the one the release must carry",
                                   *[f"{docs / name}: SHA-256" for name in reviewed + staged[:-1]],
                                   "not the reviewed", "not the staged")
        self.write_notices(root)
        # A notice the manifest does not list, and a tree without the manifest.
        manifest = docs / NOTICE_MANIFEST
        manifest.write_text("".join(line + "\n" for line in manifest.read_text().splitlines()
                                    if not line.endswith("  THIRD_PARTY.md")))
        self.assert_verify_refuses(tools, root, windows, f"{docs / 'THIRD_PARTY.md'}: no reviewed hash",
                                   f"no line in {manifest}")
        manifest.unlink()
        self.assert_verify_refuses(tools, root, windows, f"No notice manifest at {manifest}")

    def test_verify_release_tree_needs_the_runtime_notice_with_the_runtime(self):
        # A tree that carries a Visual C++ runtime DLL carries the notice of Microsoft's terms for it; a tree without
        # the runtime does not need it.
        tools, root, windows = self.release_tree()
        notice = root / "share/doc/dsd-neo" / RUNTIME_NOTICE
        notice.unlink(missing_ok=True)
        self.assert_verify_refuses(tools, root, windows, f"MSVCP140.dll (msvc-runtime): {notice}",
                                   f"VCRUNTIME140.dll (msvc-runtime): {notice}")
        for name in RUNTIME_DLLS:
            (root / "bin" / name).unlink()
            del self.report["dependents"][name]
        for dependents in self.report["dependents"].values():
            dependents[:] = [name for name in dependents if name not in RUNTIME_DLLS]
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)

    def test_verify_release_tree_refuses_images_of_another_machine(self):
        # An x64 process cannot load a DLL of another machine: a DLL delay-loaded only when a receiver is opened would
        # pass the hardware-free runs below and fail on the user's machine. Every image the checks walk must be x64.
        tools, root, windows = self.release_tree()
        bin_dir = root / "bin"
        (bin_dir / "late.dll").write_bytes(b"MZ late")
        self.record_files(root, "late.dll")
        self.report["dependents"]["late.dll"] = ["KERNEL32.dll"]
        self.report["dependents"]["airspy.dll"].append("late.dll")
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)
        for image, machine, label in (("late.dll", "14C machine (x86)", "0x014C"),
                                      ("hackrf.dll", "AA64 machine (ARM64)", "0xAA64"),
                                      ("airspySupport.dll", "14C machine (x86)", "0x014C"),
                                      ("dsd-neo.exe", "AA64 machine (ARM64)", "0xAA64")):
            with self.subTest(image=image, machine=machine):
                self.report["machines"] = {image: machine}
                self.assert_verify_refuses(tools, root, windows, f"{image} is built for COFF machine {label}",
                                           "not x64")
        # A header report that names no machine says nothing of the image.
        self.report["machines"] = {"late.dll": ""}
        self.assert_verify_refuses(tools, root, windows, "named 0 machine types", str(bin_dir / "late.dll"))
        del self.report["machines"]

    def test_verify_release_tree_sees_hidden_files(self):
        # The ZIP carries hidden files and folders, and Windows loads them: every check of the tree must see them.
        tools, root, windows = self.release_tree()
        bin_dir = root / "bin"
        modules = root / "lib/SoapySDR/modules0.8"
        cases = (
            ("an unrecorded file", bin_dir / ".vendor.txt", None, ["No component record", str(bin_dir / ".vendor.txt")]),
            ("an unexpected module", modules / ".SoapyRemoteSupport.dll", None,
             [f"Expected exactly airspySupport.dll, HackRFSupport.dll in {modules}", ".SoapyRemoteSupport.dll"]),
            ("a second copy", root / "lib/.cache/airspy.dll", None,
             ["Expected one airspy.dll", str(root / "lib/.cache/airspy.dll")]),
            ("a binary with a missing import", bin_dir / ".vendor.dll", ["missing.dll", "KERNEL32.dll"],
             [".vendor.dll -> missing.dll"]),
        )
        self.record_files(root, ".vendor.dll")
        for label, path, imports, words in cases:
            with self.subTest(case=label):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"MZ hidden")
                if imports:
                    self.report["dependents"][path.name] = imports
                try:
                    self.assert_verify_refuses(tools, root, windows, *words)
                finally:
                    path.unlink()
                    if imports:
                        del self.report["dependents"][path.name]
        shutil.rmtree(root / "lib/.cache")
        self.write_report(tools)

    def test_verify_release_tree_needs_the_runtime_record_with_the_runtime(self):
        # legal/msvc-runtime.md, which records Microsoft's terms for the runtime DLLs and how the release meets them,
        # ships with those DLLs: a tree that lost it fails, like one that lost the end-user notice.
        tools, root, windows = self.release_tree()
        record = root / "share/doc/dsd-neo/legal/msvc-runtime.md"
        self.assertEqual(record.read_bytes(), (ROOT / "legal/msvc-runtime.md").read_bytes())
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)
        # Another text than the one staged.
        record.write_text("Visual C++ runtime terms\n")
        self.assert_verify_refuses(tools, root, windows, f"{record}: SHA-256", f"not the staged")
        record.unlink()
        self.assert_verify_refuses(tools, root, windows, f"MSVCP140.dll (msvc-runtime): {record}",
                                   f"VCRUNTIME140.dll (msvc-runtime): {record}")
        record.write_text("")
        self.assert_verify_refuses(tools, root, windows, f"MSVCP140.dll (msvc-runtime): {record}")
        self.write_notices(root)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 0, output)

    def test_verify_release_tree_needs_one_copy_of_each_shared_library(self):
        # dsd-neo.exe's native Airspy input and librtlsdr must load the airspy.dll and libusb-1.0.dll the modules and
        # their libraries load, not copies of them under another name.
        tools, root, windows = self.release_tree()
        bin_dir = root / "bin"
        for importer, shared, copy in (("dsd-neo.exe", "airspy.dll", "libairspy.dll"),
                                       ("rtlsdr.dll", "libusb-1.0.dll", "usb-1.0.dll"),
                                       # Unpatched, and named so that the patch check looks only at rtlsdr.dll.
                                       ("dsd-neo.exe", "rtlsdr.dll", "rtl-sdr.dll"),
                                       ("dsd-neo.exe", "SoapySDR.dll", "libSoapySDR.dll"),
                                       ("SoapySDRUtil.exe", "SoapySDR.dll", "libSoapySDR.dll")):
            with self.subTest(importer=importer, copy=copy):
                saved = list(self.report["dependents"][importer])
                (bin_dir / copy).write_bytes(b"MZ " + copy.encode())
                self.report["dependents"][copy] = list(self.report["dependents"].get(shared, ["KERNEL32.dll"]))
                self.report["dependents"][importer] = [copy if name == shared else name for name in saved]
                try:
                    self.assert_verify_refuses(tools, root, windows, f"{bin_dir / importer} does not import {shared}")
                finally:
                    self.report["dependents"][importer] = saved
                    del self.report["dependents"][copy]
                    (bin_dir / copy).unlink()


if __name__ == "__main__":
    unittest.main()
