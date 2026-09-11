#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Hardware-free regressions for Airspy dependency and packaging contracts."""

import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
NOTICE = "libairspy-LICENSE.txt"
# SHA256 of the unmodified upstream bd15be3 libairspy/src files and LICENSE.md.
UPSTREAM_HASHES = {
    "airspy.c": "f6fd2fe296819dc1e2b6655de32f53b7dde6d60d17ef3902297b96974b3109fa",
    "airspy.h": "b2ad0b944263ed91f947e111195da3dd906cd51c5b4455c603499d6741283e44",
    "airspy_commands.h": "36de20a93d07c593608acbd230880dd0468602f28592d36d8ccfe1247f79155e",
    "filters.h": "0e4414b46e53fa68d6388009b025f7d5dc93b89cdbc1a91633c9e2e8fe73f9cb",
    "iqconverter_float.c": "146489491b5c2d3003f3d0c74a943ff97aa7b40b3afde163d009e4fdfc54bd79",
    "iqconverter_float.h": "b4d3db2a9f76c79d2888ea743bd6d1ebf13a99717f3be9edf2941338a76704ec",
    "iqconverter_int16.c": "2c8b60fee70c5817338971ad6ffdb2a4eb61a2f272816dfc55459d4da6f4195a",
    "iqconverter_int16.h": "7fabb42ce4a40c2cf6df589a146029fd591c5de97ba595d45c54e909546296a5",
    "LICENSE": "dc49b4210fb03af14b881f8f6fdea053446c28181854ed3712e2e5e73fb0f10c",
}


def run(args, **kwargs):
    return subprocess.run(args, text=True, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, **kwargs)


class AirspyPackaging(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="dsd-airspy-packaging-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout)

    def test_b1_upstream_snapshot_and_notices(self):
        vendor = ROOT / "android/third_party/libairspy"
        for name, digest in UPSTREAM_HASHES.items():
            with self.subTest(file=name):
                self.assertEqual(hashlib.sha256((vendor / name).read_bytes()).hexdigest(), digest)
        self.assertEqual((vendor / "LICENSE").read_bytes(),
                         (ROOT / "vcpkg-ports/airspy/copyright").read_bytes())
        port = json.loads((ROOT / "vcpkg-ports/airspy/vcpkg.json").read_text())
        self.assertEqual(port["license"], "BSD-3-Clause AND MIT")

    @unittest.skipUnless(shutil.which("cmake"), "cmake is required")
    def test_b1_android_stages_complete_notice(self):
        source = self.work / "package"
        source.mkdir()
        stage = self.work / "stage"
        result = run(["cmake", f"-DREPO_DIR={ROOT}", f"-DSOURCE_DIR={source}",
                      f"-DSTAGE_DIR={stage}", "-P", str(ROOT / "cmake/stage_android_package.cmake")])
        self.assert_success(result)
        notice = stage / "assets/doc/dsd-neo/licenses" / NOTICE
        self.assertTrue(notice.is_file(), str(notice))
        self.assertEqual(hashlib.sha256(notice.read_bytes()).hexdigest(), UPSTREAM_HASHES["LICENSE"])

    def test_b1_release_notice_requirements(self):
        appimage = (ROOT / ".github/workflows/linux-appimage.yaml").read_text()
        required = re.search(r"required=\(([^\n]+)\)", appimage).group(1)
        self.assertIn("licenses/" + NOTICE, required.split())
        build_dep = next(line for line in appimage.splitlines()
                         if "build_dep https://github.com/airspy/airspyone_host " in line)
        self.assertIn("-DCMAKE_POLICY_VERSION_MINIMUM=3.5", build_dep)
        windows = (ROOT / ".github/workflows/windows-ci.yaml").read_text()
        self.assertIn("'licenses/" + NOTICE + "'", windows)
        self.assertIn("Copy-Item -Force 'android/third_party/libairspy/LICENSE' "
                      "(Join-Path $licenseDir '" + NOTICE + "') -ErrorAction Stop", windows)

    @unittest.skipUnless(shutil.which("clang-format"), "clang-format is required")
    def test_b2_upstream_format_is_ignored(self):
        # Use a scratch copy: this also verifies format-on-save cannot rewrite it.
        vendor = self.work / "android/third_party/libairspy"
        vendor.mkdir(parents=True)
        for name in (".clang-format", ".clang-format-ignore"):
            shutil.copyfile(ROOT / name, self.work / name)
        source = vendor / "airspy.c"
        shutil.copyfile(ROOT / "android/third_party/libairspy/airspy.c", source)
        before = source.read_bytes()
        self.assert_success(run(["clang-format", "-n", "--Werror", str(source)]))
        self.assert_success(run(["clang-format", "-i", str(source)]))
        self.assertEqual(source.read_bytes(), before)
        self.assertIn("android/third_party/libairspy/** linguist-vendored",
                      (ROOT / ".gitattributes").read_text().splitlines())

    def installer_output(self, manager, mode):
        # Isolate PATH so host package managers cannot mask the selected fixture.
        bindir = self.work / f"{manager}-{mode}"
        bindir.mkdir()
        for name in ("dirname", "cat"):
            (bindir / name).symlink_to(shutil.which(name))
        for name, body in ((manager, "exit 99"), ("id", "echo 0")):
            script = bindir / name
            script.write_text("#!/bin/sh\n" + body + "\n")
            script.chmod(0o755)
        env = dict(os.environ, PATH=str(bindir), DSD_NEO_BUILD_JOBS="1")
        result = run(["/bin/sh", str(ROOT / "tools/install_linux.sh"), "--dry-run", "--yes",
                      "--radio", mode, "--codec2", "off", "--prefix", str(self.work / "prefix"),
                      "--build-dir", str(self.work / "build")], env=env)
        self.assert_success(result)
        self.assertIn("DSD-neo install complete.", result.stdout)
        return result.stdout

    def test_m5_installer_radio_modes(self):
        for mode, value in (("off", "OFF"), ("required", "ON")):
            with self.subTest(mode=mode):
                output = self.installer_output("apt-get", mode)
                configure = next(line for line in output.splitlines()
                                 if line.startswith(f"+ cmake -S {ROOT} -B "))
                for backend in ("AIRSPY", "RTLSDR", "SOAPYSDR"):
                    for flag in ("ENABLE", "REQUIRE"):
                        self.assertIn(f"-DDSD_{flag}_{backend}={value}", configure.split())

    def test_m6_distro_package_commands(self):
        for manager, expected, command in (("apk", "airspyone-host-dev", "+ apk add "),
                                           ("dnf", "airspyone_host-devel", "+ dnf -y install ")):
            with self.subTest(manager=manager):
                output = self.installer_output(manager, "required")
                packages = [word for line in output.splitlines() if line.startswith(command)
                            for word in line.split()]
                self.assertIn(expected, packages)
                self.assertNotIn("airspy-dev", packages)

    def test_m8_overlay_drift_is_rejected_and_repaired(self):
        for name in ("tools/ci-dependency-pins.env", "tools/check_vcpkg_overlay_pins.sh"):
            dest = self.work / name
            dest.parent.mkdir(exist_ok=True)
            shutil.copyfile(ROOT / name, dest)
        shutil.copytree(ROOT / "vcpkg-ports", self.work / "vcpkg-ports")
        command = ["bash", str(self.work / "tools/check_vcpkg_overlay_pins.sh")]
        self.assert_success(run(command, cwd=self.work))
        port = self.work / "vcpkg-ports/airspy/portfile.cmake"
        original = port.read_text()
        for key, length in (("REF", 40), ("SHA512", 128)):
            with self.subTest(key=key):
                port.write_text(re.sub(rf"(?m)^(\s*{key} )\S+", rf"\g<1>{'0' * length}", original))
                rejected = run(command, cwd=self.work)
                self.assertEqual(rejected.returncode, 1, rejected.stdout)
                self.assertIn(f"airspy/portfile.cmake: {key}", rejected.stdout)
                self.assert_success(run(command + ["--fix"], cwd=self.work))
                self.assertEqual(port.read_text(), original)
                self.assert_success(run(command, cwd=self.work))

    def test_m9_aur_declares_and_requires_airspy(self):
        workflow = (ROOT / ".github/workflows/linux-ci.yaml").read_text()
        template = workflow.split("cat > PKGBUILD <<'PKGBUILD'\n", 1)[1].split("          PKGBUILD\n", 1)[0]
        pkgbuild = self.work / "PKGBUILD"
        pkgbuild.write_text(textwrap.dedent(template))
        # Execute the generated build function; the shell function records CMake args.
        script = '''
set -eu
source "$1"
printf 'DEPENDENCY=%s\n' "${depends[@]}"
cmake() { printf 'CMAKE_ARG=%s\n' "$@"; }
build
'''
        result = run(["bash", "-c", script, "test", str(pkgbuild)], cwd=self.work)
        self.assert_success(result)
        self.assertIn("CMAKE_ARG=--build", result.stdout.splitlines())
        self.assertIn("DEPENDENCY=airspy", result.stdout.splitlines())
        self.assertIn("CMAKE_ARG=-DDSD_REQUIRE_AIRSPY=ON", result.stdout.splitlines())


if __name__ == "__main__":
    unittest.main()
