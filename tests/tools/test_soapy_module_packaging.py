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
    "soapy-airspyhf": ("pothosware/SoapyAirspyHF", "SOAPY_AIRSPYHF", "airspyhf"),
}
LIBRARIES = {
    "hackrf": ("greatscottgadgets/hackrf", "HACKRF"),
    "airspyhf": ("airspy/airspyhf", "AIRSPYHF"),
}
MODULE_FILES = ("airspySupport.dll", "airspyhfSupport.dll", "HackRFSupport.dll")
# Repository notice -> its name in the ZIP's share/doc/dsd-neo/licenses/.
WINDOWS_NOTICES = {
    "vcpkg-ports/soapy-airspy/copyright": "SoapyAirspy-MIT.txt",
    "vcpkg-ports/soapy-hackrf/copyright": "SoapyHackRF-MIT.txt",
    "vcpkg-ports/soapy-airspyhf/copyright": "SoapyAirspyHF-MIT.txt",
    "vcpkg-ports/hackrf/copyright": "libhackrf-BSD-3-Clause.txt",
    "vcpkg-ports/airspyhf/copyright": "libairspyhf-BSD-3-Clause.txt",
    "android/third_party/libusb/COPYING": "libusb-LGPL-2.1-or-later.txt",
    "packaging/windows/PThreads4W-LICENSE.txt": "PThreads4W-Apache-2.0.txt",
    "packaging/windows/PThreads4W-NOTICE.txt": "PThreads4W-NOTICE.txt",
}
MIT_GRANT = "The above copyright notice and this permission notice shall be included"
BSD_BINARY_CLAUSE = "Redistributions in binary form must reproduce the above copyright notice"


def read(path):
    return (ROOT / path).read_text(encoding="utf-8").replace("\r\n", "\n")


def normalized(text):
    return ANSI.sub("", text.replace("\r\n", "\n"))


def pins():
    result = {}
    for line in read("tools/ci-dependency-pins.env").splitlines():
        if line and not line.startswith("#"):
            key, value = line.split("=", 1)
            result[key] = value
    return result


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
        # SoapyAirspyHF's own probe for its gain API cannot link under MSVC; the port sets the result.
        portfile = self.port("soapy-airspyhf")[1]
        self.assertIn('"-DHAVE_GAIN_API=1"', portfile)
        self.assertNotIn("HAVE_GAIN_API", self.port("soapy-airspy")[1] + self.port("soapy-hackrf")[1])

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
        airspyhf = read("vcpkg-ports/airspyhf/CMakeLists.txt")
        self.assertIn("add_library(airspyhf libairspyhf/src/airspyhf.c libairspyhf/src/iqbalancer.c)", airspyhf)
        self.assertEqual(json.loads(read("vcpkg-ports/airspyhf/vcpkg.json"))["version-string"],
                         "1.8.1-" + self.pins["AIRSPYHF_SHA"][:7])

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
        self.assertEqual(entries["soapysdr"], {"name": "soapysdr", "platform": "windows", "features": ["tools"]})
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
        airspyhf = read("vcpkg-ports/airspyhf/copyright")
        for line in ("Copyright (c) 2013-2024, Youssef Touil <youssef@airspy.com>",
                     "Copyright (c) 2013-2023, Youssef Touil <youssef@airspy.com>",
                     "Copyright (c) 2013-2017, Ian Gilmour <ian@sdrsharp.com>",
                     "Copyright (c) 2013-2017, Benjamin Vernoux <bvernoux@airspy.com>",
                     "Copyright (c) 2013, Michael Ossmann <mike@ossmann.com>",
                     "Copyright (c) 2012, Jared Boone <jared@sharebrained.com>",
                     "Copyright (c) 2016-2023, Youssef Touil <youssef@airspy.com>",
                     "Copyright (c) 2018, Leif Asbrink <leif@sm5bsz.com>",
                     "Neither the name of Airspy HF+ nor the names of its contributors",
                     "BSD 3-Clause License\n\nCopyright (c) 2017, Airspy", BSD_BINARY_CLAUSE):
            self.assertIn(line, airspyhf)
        for name, holders in (("soapy-airspy", ["Copyright (c) 2015 Charles J. Cliffe"]),
                              ("soapy-airspyhf", ["Copyright (c) 2015 Charles J. Cliffe",
                                                  "Copyright (c) 2018 Corey Stotts"]),
                              ("soapy-hackrf", ["Copyright (c) 2015-2016 Wei Jiang",
                                                "Copyright (c) 2015-2017 Josh Blum",
                                                "Copyright (c) 2017 Kevin Mehall"])):
            notice = read(f"vcpkg-ports/{name}/copyright")
            with self.subTest(port=name):
                self.assertIn("The MIT License (MIT)", notice)
                self.assertIn(MIT_GRANT, notice)
                for holder in holders:
                    self.assertIn(holder, notice)

    def test_legal_records(self):
        inventory = {c["id"]: c for c in json.loads(read("legal/components.json"))["components"]}
        expected = {
            "soapysdr": (["linux", "windows"], "BSL-1.0", "SOAPY_SDR_SHA", ["LICENSES/BSL-1.0.txt"]),
            "soapy-airspy": (["windows"], "MIT", "SOAPY_AIRSPY_SHA", ["vcpkg-ports/soapy-airspy/copyright"]),
            "soapy-hackrf": (["windows"], "MIT", "SOAPY_HACKRF_SHA", ["vcpkg-ports/soapy-hackrf/copyright"]),
            "soapy-airspyhf": (["windows"], "MIT", "SOAPY_AIRSPYHF_SHA", ["vcpkg-ports/soapy-airspyhf/copyright"]),
            "libhackrf": (["windows"], "BSD-3-Clause", "HACKRF_SHA", ["vcpkg-ports/hackrf/copyright"]),
            "libairspyhf": (["windows"], "BSD-3-Clause", "AIRSPYHF_SHA", ["vcpkg-ports/airspyhf/copyright"]),
            "windows-libusb": (["windows"], "LGPL-2.1-or-later", None, ["android/third_party/libusb/COPYING"]),
            "pthreads4w": (["windows"], "Apache-2.0", None, ["packaging/windows/PThreads4W-LICENSE.txt",
                                                             "packaging/windows/PThreads4W-NOTICE.txt"]),
        }
        required = read("tools/legal_inventory.py")
        for ident, (platforms, license_id, pin, notices) in expected.items():
            with self.subTest(component=ident):
                component = inventory[ident]
                # The DMG bundles Homebrew's SoapySDR, which no record claims.
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

    def test_staging_ships_every_notice(self):
        script = read("tools/windows/stage_release_tree.ps1")
        required = re.search(r"\$required = @\(([^\n]+)\)", script).group(1)
        for source, shipped in WINDOWS_NOTICES.items():
            with self.subTest(notice=shipped):
                self.assertTrue((ROOT / source).is_file(), source)
                self.assertIn(f"    '{source}' = '{shipped}'\n", script)
                self.assertIn(f"'licenses/{shipped}'", required)
        self.assertIn("'licenses/BSL-1.0.txt'", required)
        self.assertIn("$expectedModules = @(" + ", ".join(f"'{m}'" for m in MODULE_FILES) + ")", script)

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
# The same --info as printInfo() (apps/SoapySDRUtil.cpp) prints it for the three modules of a release tree.
INFO_RELEASE = (
    "Lib Version: v0.8.1-unknown\r\nAPI Version: v0.8.0\r\nABI Version: v0.8\r\n"
    "Install root: C:\\Users\\me\\dsd-neo\r\n"
    "Search path:  C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8\r\n"
    "Module found: C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8/airspySupport.dll   (0.2.1)\r\n"
    "Module found: C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8/airspyhfSupport.dll (0.2.1)\r\n"
    "Module found: C:\\Users\\me\\dsd-neo/lib/SoapySDR/modules0.8/HackRFSupport.dll   (0.3.6)\r\n"
    "Available factories... airspy, airspyhf, hackrf\r\nAvailable converters...\r\n")


def dumpbin(lines):
    return "\r\n".join(["Microsoft (R) COFF/PE Dumper Version 14.44.35217.0",
                        "Copyright (C) Microsoft Corporation.  All rights reserved.", "", ""] + lines) + "\r\n"


DEPENDENTS = dumpbin(["Dump of file airspyhfSupport.dll", "", "File Type: DLL", "",
                      "  Image has the following dependencies:", "", "    airspyhf.dll", "    SoapySDR.dll",
                      "    MSVCP140.dll", "    KERNEL32.dll", "", "  Image has the following delay load dependencies:",
                      "", "    WS2_32.dll", "", "  Summary", "", "        1000 .data", "        9000 .text", ""])
IMPORTS = dumpbin(["Dump of file airspyhfSupport.dll", "", "File Type: DLL", "",
                   "  Section contains the following imports:", "", "    airspyhf.dll",
                   "             180012000 Import Address Table", "             180016A58 Import Name Table",
                   "                     0 time date stamp", "                     0 Index of first forwarder reference",
                   "", "                           3 airspyhf_close", "                          12 airspyhf_set_hf_att",
                   "", "    SoapySDR.dll", "             180012100 Import Address Table", "",
                   "                         1A7 airspyhf_set_hf_lna", "", "  Summary", "", "        1000 .data", ""])

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
$result['imports'] = [ordered]@{}
foreach ($pair in @(@('airspyhf.dll', 'airspyhf_set_hf_att'), @('airspyhf.dll', 'airspyhf_set_hf_lna'),
                    @('SoapySDR.dll', 'airspyhf_set_hf_lna'), @('airspyhf.dll', 'Import'),
                    @('airspyhf.dll', 'airspyhf_set_hf_agc'))) {
    $result['imports'][$pair[0] + ':' + $pair[1]] = Test-DumpbinImport -Text $text['imports'] -Dll $pair[0] -Function $pair[1]
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
    "dsd-neo.exe": ["rtlsdr.dll", "airspy.dll", "SoapySDR.dll", "VCRUNTIME140.dll", "KERNEL32.dll",
                    "api-ms-win-crt-runtime-l1-1-0.dll"],
    "rtlsdr.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "airspy.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "hackrf.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "airspyhf.dll": ["libusb-1.0.dll", "pthreadVC3.dll", "KERNEL32.dll"],
    "SoapySDR.dll": ["MSVCP140.dll", "VCRUNTIME140.dll", "KERNEL32.dll"],
    "airspySupport.dll": ["airspy.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll"],
    "airspyhfSupport.dll": ["airspyhf.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll"],
    "HackRFSupport.dll": ["hackrf.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll"],
    "SoapySDRUtil.exe": ["SoapySDR.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "KERNEL32.dll"],
}
INSTALLED_BIN = ("rtlsdr.dll", "airspy.dll", "hackrf.dll", "airspyhf.dll", "SoapySDR.dll", "libusb-1.0.dll",
                 "pthreadVC3.dll", "MSVCP140.dll", "VCRUNTIME140.dll", "unused.dll")


# Fakes for running verify_release_tree.ps1 off Windows. dumpbin.exe answers /headers, /DEPENDENTS and /IMPORTS from
# report.json beside it. SoapySDRUtil.exe and dsd-neo.exe model SoapySDR 0.8.1: they load each module in
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
if option == "/headers":
    lines += ["            8664 machine (x64)", "                   High Entropy Virtual Addresses",
              "                   Dynamic base", "                   NX compatible"]
elif option == "/DEPENDENTS":
    lines += ["  Image has the following dependencies:", ""]
    lines += ["    " + dep for dep in report["dependents"].get(name, ["KERNEL32.dll"])]
elif option == "/IMPORTS":
    lines += ["  Section contains the following imports:", ""]
    for dll, functions in report["imports"].get(name, {{}}).items():
        lines += ["    " + dll, "             180012000 Import Address Table", ""]
        lines += ["                          {{:X}} {{}}".format(i, f) for i, f in enumerate(functions)] + [""]
else:
    sys.exit(2)
lines += ["", "  Summary", "", "        1000 .data"]
sys.stdout.write("\r\n".join(lines) + "\r\n")
"""

FAKE_SOAPY_PROGRAM = r"""#!{python} -I
import sys
from pathlib import Path

LIBRARIES = {{"airspySupport.dll": ("airspy", "airspy.dll"), "airspyhfSupport.dll": ("airspyhf", "airspyhf.dll"),
             "HackRFSupport.dll": ("hackrf", "hackrf.dll")}}
MESSAGES = {{"airspy": "Unable to open AirSpy device", "airspyhf": "Unable to open AirspyHF device",
            "hackrf": "no hackrf device matches"}}
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
    "libusb-1.0.dll": ["KERNEL32.dll"], "pthreadVC3.dll": ["KERNEL32.dll"],
    "MSVCP140.dll": ["VCRUNTIME140.dll", "KERNEL32.dll"], "VCRUNTIME140.dll": ["KERNEL32.dll"],
})
RELEASE_IMPORTS = {"airspyhfSupport.dll": {"airspyhf.dll": ["airspyhf_open", "airspyhf_set_hf_att",
                                                            "airspyhf_set_hf_lna", "airspyhf_set_hf_agc"],
                                           "SoapySDR.dll": ["SoapySDR_logf"]}}

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
                           ("imports", IMPORTS)):
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
        self.assertEqual(result["info_release"]["Factories"], ["airspy", "airspyhf", "hackrf"])
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
        self.assertEqual(list(result["probe_messages"]), ["airspy", "airspyhf", "hackrf", ""])
        self.assertEqual(result["dependents"], ["airspyhf.dll", "SoapySDR.dll", "MSVCP140.dll", "KERNEL32.dll",
                                                "WS2_32.dll"])
        self.assertEqual(result["imports"], {
            "airspyhf.dll:airspyhf_set_hf_att": True, "airspyhf.dll:airspyhf_set_hf_lna": False,
            "SoapySDR.dll:airspyhf_set_hf_lna": True, "airspyhf.dll:Import": False,
            "airspyhf.dll:airspyhf_set_hf_agc": False})

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

    def fake_tree(self):
        tools = self.work / "tools"
        tools.mkdir()
        (tools / "dumpbin").write_text(FAKE_DUMPBIN.format(python=sys.executable, deps=DEPENDENCIES),
                                       encoding="utf-8")
        (tools / "cmake").write_text(FAKE_CMAKE.format(python=sys.executable, root=str(ROOT)), encoding="utf-8")
        for tool in ("dumpbin", "cmake"):
            (tools / tool).chmod(0o755)
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
        self.program_files.mkdir()
        return tools, build, installed, windows

    def stage(self, tools, build, installed, windows):
        stage = self.work / "dist/dsd-neo-msvc"
        env = dict(os.environ, PATH=str(tools), SystemRoot=str(windows))
        env["ProgramFiles(x86)"] = str(self.program_files)
        rc, out, err = self.pwsh(STAGE, "-Script", ROOT / "tools/windows/stage_release_tree.ps1", "-Build", build,
                                 "-Stage", stage, "-Installed", installed, env=env)
        return rc, out + err, stage

    def test_stage_release_tree(self):
        tools, build, installed, windows = self.fake_tree()
        rc, output, stage = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 0, output)
        bin_files = sorted(p.name for p in (stage / "bin").iterdir())
        self.assertEqual(bin_files, sorted(["dsd-neo.exe", "SoapySDRUtil.exe", "dlls-manifest.txt"]
                                           + [n for n in INSTALLED_BIN if n != "unused.dll"]))
        self.assertEqual((stage / "bin/SoapySDR.dll").read_bytes(), pe_image(b"SoapySDR.dll"))
        self.assertEqual(sorted(p.relative_to(stage).as_posix() for p in (stage / "lib").rglob("*")),
                         ["lib/SoapySDR", "lib/SoapySDR/modules0.8"]
                         + sorted(f"lib/SoapySDR/modules0.8/{m}" for m in MODULE_FILES))
        licenses = stage / "share/doc/dsd-neo/licenses"
        for source, shipped in WINDOWS_NOTICES.items():
            self.assertEqual((licenses / shipped).read_bytes(), (ROOT / source).read_bytes(), shipped)
        self.assertEqual((licenses / "BSL-1.0.txt").read_bytes(), (ROOT / "LICENSES/BSL-1.0.txt").read_bytes())
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
        # A module DLL the staging cannot find in bin\ or vcpkg's bin\ stops it, as for dsd-neo.exe.
        (installed / "include/SoapySDR/Version.h").write_text('#define SOAPY_SDR_ABI_VERSION "0.8"\n')
        (installed / "bin/hackrf.dll").unlink()
        rc, output, _ = self.stage(tools, build, installed, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("Missing DLL dependencies", output)
        self.assertIn("hackrf.dll", output)


    def release_tree(self):
        """A release tree as stage_release_tree.ps1 leaves it, with fake programs and a fake dumpbin.exe."""
        tools = self.work / "verify-tools"
        tools.mkdir()
        (tools / "dumpbin.exe").write_text(FAKE_VERIFY_DUMPBIN.format(python=sys.executable), encoding="utf-8")
        (tools / "dumpbin.exe").chmod(0o755)
        self.report = {"dependents": json.loads(json.dumps(RELEASE_DEPENDENTS)),
                       "imports": json.loads(json.dumps(RELEASE_IMPORTS))}
        self.write_report(tools)
        root = self.work / "release"
        bin_dir = root / "bin"
        bin_dir.mkdir(parents=True)
        for name in INSTALLED_BIN:
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
        windows = self.work / "verify-windows"
        (windows / "System32").mkdir(parents=True)
        (windows / "System32/KERNEL32.dll").write_bytes(b"MZ system")
        return tools, root, windows

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
        # Every program ran: the drivers, --info, -h, four probes, and both damaged copies.
        self.assertEqual(output.count("SoapySDRUtil.exe --check="), 9)
        self.assertEqual(output.count("dsd-neo.exe --frontend none -i soapy"), 8)

    def test_verify_release_tree_refusals(self):
        tools, root, windows = self.release_tree()
        cases = (
            ("a missing library", root / "bin/hackrf.dll", "Expected one hackrf.dll"),
            ("a missing module", root / "lib/SoapySDR/modules0.8/airspyhfSupport.dll",
             "modules0.8; found airspySupport.dll, HackRFSupport.dll"),
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
        # A dependency report that is not about an image (an object file named like a DLL).
        self.report["types"] = {"hackrf.dll": "COFF OBJECT"}
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("did not report", output)
        self.assertIn("hackrf.dll", output)
        del self.report["types"]
        # SoapyAirspyHF built without its gain controls.
        self.report["imports"]["airspyhfSupport.dll"]["airspyhf.dll"].remove("airspyhf_set_hf_lna")
        self.write_report(tools)
        rc, output = self.verify(tools, root, windows)
        self.assertEqual(rc, 1, output)
        self.assertIn("does not import airspyhf_set_hf_lna", output)

if __name__ == "__main__":
    unittest.main()
