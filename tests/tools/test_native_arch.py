#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check when cmake/NativeArch.cmake tunes a build for the host CPU, and when it must not."""

import argparse
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROJECT = Path(__file__).resolve().parent / "native_arch"
NATIVE_FLAG = re.compile(r"(?:^|\s)(-march=native|-mcpu=native|[-/]arch:AVX(?:2|512)?)(?=\s|$)")
MACHINE = platform.machine().lower()
X86_HOST = MACHINE in ("x86_64", "amd64", "i386", "i486", "i586", "i686", "x86")
ARM_HOST = MACHINE.startswith(("aarch64", "arm"))

SKIP_REASON_SCRIPT = r'''
include("${DSD_SOURCE_DIR}/cmake/NativeArch.cmake")
dsd_native_arch_skip_reason(reason)
message("REASON=[${reason}]")
'''


# Build settings a developer or packager may export; each case sets its own.
ISOLATED_ENV = ("CFLAGS", "CXXFLAGS", "LDFLAGS", "CL", "_CL_", "CMAKE_TOOLCHAIN_FILE", "CMAKE_OSX_ARCHITECTURES",
                "CMAKE_BUILD_TYPE", "CMAKE_CONFIGURATION_TYPES", "CMAKE_GENERATOR")
USER_ISA = re.compile(r"(?:^|\s)[\"']?(?:-march=|-mcpu=|[-/]arch:|-arch\s)", re.I)
ENV = {name: value for name, value in os.environ.items() if name not in ISOLATED_ENV}


class NativeArch(unittest.TestCase):
    def setUp(self):
        # The compile-command cases look for host-tuning flags, so a forwarded
        # compiler command that carries an ISA option of its own (CC="gcc
        # -march=native") would be indistinguishable; the rules themselves are
        # covered by test_skip_rules.
        if self._testMethodName != "test_skip_rules" and \
                any(USER_ISA.search(arg or "") for arg in (ARGS.cc_arg1, ARGS.cxx_arg1)):
            self.skipTest("the forwarded compiler command already selects a target ISA")

    def run_command(self, *command, env=None):
        result = subprocess.run(command, capture_output=True, text=True, timeout=300, env=env or ENV)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def compiler_args(self):
        args = []
        if ARGS.cc:
            args.append(f"-DCMAKE_C_COMPILER={ARGS.cc}")
        if ARGS.cxx:
            args.append(f"-DCMAKE_CXX_COMPILER={ARGS.cxx}")
        # CC/CXX can be wrappers, e.g. "ccache clang"; CMake keeps the real
        # compiler in ARG1 when it splits that command.
        if ARGS.cc_arg1:
            args.append(f"-DCMAKE_C_COMPILER_ARG1={ARGS.cc_arg1}")
        if ARGS.cxx_arg1:
            args.append(f"-DCMAKE_CXX_COMPILER_ARG1={ARGS.cxx_arg1}")
        return args

    def configure(self, directory, *extra, generator="Ninja", build_type="Release"):
        build = Path(directory) / "build"
        command = [ARGS.cmake, "-S", str(PROJECT), "-B", str(build), "-G", generator,
                   f"-DDSD_SOURCE_DIR={ROOT.as_posix()}", *self.compiler_args(), *extra]
        if generator == "Ninja" and build_type:
            command.append(f"-DCMAKE_BUILD_TYPE={build_type}")
        log = self.run_command(*command)
        result = dict(line.split("=", 1) for line in
                      (build / "native_arch_result.txt").read_text(encoding="utf-8").splitlines())
        entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
        return build, log, result, entries

    def commands_for(self, entries, generator, configuration):
        commands = []
        for entry in entries:
            command = entry.get("command") or " ".join(entry.get("arguments", []))
            # CMake 3.20 omits the optional compilation database output field.
            object_path = entry.get("output", command).replace("\\", "/")
            if generator == "Ninja Multi-Config" and f".dir/{configuration}/" not in object_path:
                continue
            commands.append(command)
        self.assertEqual(len(commands), 2, entries)
        return commands

    def assert_no_native_flag(self, commands):
        for command in commands:
            self.assertIsNone(NATIVE_FLAG.search(command), command)

    def assert_skipped(self, result, log, reason_pattern):
        self.assertEqual(result["flags"], "", log)
        self.assertRegex(result["reason"], reason_pattern, log)
        self.assertIn("Native CPU tuning: off (", log)

    def expect_host_tuning(self, result, log):
        """A GNU-style compiler on an x86 or ARM host tunes unless it rejects the flag."""
        if re.search(r"compiler rejects|probe found no AVX", result["reason"]):
            return False
        # The forwarded compiler command may itself pick an ISA (CC="gcc -march=x86-64").
        if re.search(r"^CMAKE_(C|CXX)_COMPILER_ARG1 already selects", result["reason"]):
            return False
        if re.search(r"MSVC host tuning covers|no host tuning rule for processor", result["reason"]):
            return False
        self.assertTrue(X86_HOST or ARM_HOST or result["flags"], log)
        if X86_HOST or ARM_HOST:
            self.assertEqual(result["reason"], "", log)
            self.assertRegex(result["flags"], r"^(-march=native|-mcpu=native|/arch:AVX(2|512)?)$", log)
        return bool(result["flags"])

    def user_isa_flag(self, msvc):
        if msvc:
            return "/arch:AVX"
        if X86_HOST:
            return "-march=x86-64"
        if MACHINE.startswith(("aarch64", "arm64")):
            return "-march=armv8-a"
        if MACHINE.startswith("armv7"):
            return "-march=armv7-a"
        return None

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_release_and_debug(self):
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            _, log, result, entries = self.configure(directory)
            commands = self.commands_for(entries, "Ninja", "Release")
            if self.expect_host_tuning(result, log):
                for command in commands:
                    self.assertIn(result["flags"], NATIVE_FLAG.findall(command), command)
                self.assertIn(f"Native CPU tuning: {result['flags']} (Release", log)
            else:
                self.assert_no_native_flag(commands)
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            _, log, result, entries = self.configure(directory, build_type="Debug")
            self.assert_no_native_flag(self.commands_for(entries, "Ninja", "Debug"))
            if result["flags"]:
                self.assertIn("not this Debug build", log)

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_multi_config(self):
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            _, log, result, entries = self.configure(
                directory, "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo", generator="Ninja Multi-Config")
            tuned = self.expect_host_tuning(result, log)
            for configuration in ("Release", "RelWithDebInfo"):
                commands = self.commands_for(entries, "Ninja Multi-Config", configuration)
                for command in commands:
                    found = NATIVE_FLAG.findall(command)
                    self.assertEqual(found, [result["flags"]] if tuned else [], command)
            self.assert_no_native_flag(self.commands_for(entries, "Ninja Multi-Config", "Debug"))

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_option_off(self):
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            _, log, result, entries = self.configure(directory, "-DDSD_ENABLE_NATIVE=OFF")
            self.assert_skipped(result, log, r"^DSD_ENABLE_NATIVE=OFF$")
            self.assert_no_native_flag(self.commands_for(entries, "Ninja", "Release"))

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_native_probe_diagnostics(self):
        for language in ("C", "CXX"):
            with self.subTest(language=language), tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
                _, log, result, _ = self.configure(directory)
                flag = result["flags"]
                if not flag.startswith("-m"):
                    self.skipTest("no accepted GNU-style native flag on this host")
                # Reconfigure the same tree: a cached successful probe must
                # not hide a diagnostic from either compiler on the next run.
                macro = f"DSD_NATIVE_ARCH_{language}_{re.sub(r'[^a-zA-Z0-9_]', '_', flag)}"
                header = Path(directory) / "native warning.h"
                header.write_text(f"#ifdef {macro}\n#warning native tuning diagnostic\n#endif\n", encoding="utf-8")
                _, log, result, entries = self.configure(
                    directory, f'-DCMAKE_REQUIRED_FLAGS=-include "{header.as_posix()}"')
                self.assert_skipped(result, log, rf"^the {language} compiler rejects {re.escape(flag)}$")
                self.assert_no_native_flag(self.commands_for(entries, "Ninja", "Release"))

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_user_isa_flags(self):
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            _, log, result, _ = self.configure(directory)
            msvc = result["flags"].startswith("/arch:") or "MSVC" in result["reason"] or \
                "probe found no AVX" in result["reason"]
        flag = self.user_isa_flag(msvc)
        if flag is None:
            self.skipTest(f"no known baseline ISA flag for {MACHINE}")
        for variable in ("CMAKE_C_FLAGS", "CMAKE_CXX_FLAGS_RELEASE"):
            with self.subTest(variable=variable), tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
                value = flag if variable == "CMAKE_C_FLAGS" else f"-O2 {flag}"
                if msvc and variable == "CMAKE_CXX_FLAGS_RELEASE":
                    value = f"/O2 {flag}"
                _, log, result, entries = self.configure(directory, f"-D{variable}={value}")
                self.assert_skipped(result, log, rf"^{variable} already selects a target ISA$")
                for command in self.commands_for(entries, "Ninja", "Release"):
                    self.assertNotRegex(command, r"(-march=native|-mcpu=native)")

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_cross_compiling(self):
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            toolchain = Path(directory) / "toolchain.cmake"
            toolchain.write_text(
                "set(CMAKE_SYSTEM_NAME ${CMAKE_HOST_SYSTEM_NAME})\n"
                "set(CMAKE_SYSTEM_PROCESSOR ${CMAKE_HOST_SYSTEM_PROCESSOR})\n",
                encoding="utf-8")
            _, log, result, entries = self.configure(directory, f"-DCMAKE_TOOLCHAIN_FILE={toolchain.as_posix()}")
            self.assert_skipped(result, log, r"^cross-compiling for ")
            self.assert_no_native_flag(self.commands_for(entries, "Ninja", "Release"))

    def skip_reason(self, environment=None, **variables):
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            script = Path(directory) / "skip_reason.cmake"
            script.write_text(SKIP_REASON_SCRIPT, encoding="utf-8")
            command = [ARGS.cmake, f"-DDSD_SOURCE_DIR={ROOT.as_posix()}"]
            command += [f"-D{name}={value}" for name, value in variables.items()]
            command += ["-P", str(script)]
            log = self.run_command(*command, env={**ENV, **(environment or {})})
            match = re.search(r"REASON=\[(.*)\]", log, re.S)
            self.assertIsNotNone(match, log)
            return match[1]

    def test_skip_rules(self):
        cases = [
            ({}, r"^$"),
            ({"CMAKE_CROSSCOMPILING": "TRUE", "CMAKE_SYSTEM_NAME": "Linux"}, r"^cross-compiling for Linux$"),
            ({"CMAKE_SYSTEM_NAME": "Android"}, r"^target system Android$"),
            ({"CMAKE_SYSTEM_NAME": "iOS"}, r"^target system iOS$"),
            ({"CMAKE_SYSTEM_NAME": "Emscripten"}, r"^target system Emscripten$"),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_HOST_SYSTEM_PROCESSOR": "arm64",
              "CMAKE_OSX_ARCHITECTURES": "x86_64;arm64"}, r"several architectures"),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_HOST_SYSTEM_PROCESSOR": "arm64",
              "CMAKE_OSX_ARCHITECTURES": "x86_64"}, r"is not the host architecture \(arm64\)$"),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_HOST_SYSTEM_PROCESSOR": "x86_64",
              "CMAKE_OSX_ARCHITECTURES": "x86_64"}, r"^$"),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_HOST_SYSTEM_PROCESSOR": "arm64"}, r"^$"),
            # CMake ignores CMAKE_OSX_ARCHITECTURES off Apple platforms.
            ({"CMAKE_SYSTEM_NAME": "Linux", "CMAKE_HOST_SYSTEM_PROCESSOR": "x86_64",
              "CMAKE_OSX_ARCHITECTURES": "x86_64;arm64"}, r"^$"),
            ({"CMAKE_C_COMPILER_TARGET": "aarch64-linux-gnu"}, r"^CMAKE_C_COMPILER_TARGET selects aarch64-linux-gnu$"),
            ({"CMAKE_C_FLAGS": "-O2 -march=x86-64-v2"}, r"^CMAKE_C_FLAGS already selects a target ISA$"),
            ({"CMAKE_CXX_FLAGS": "-pipe -mcpu=cortex-a72"}, r"^CMAKE_CXX_FLAGS already selects"),
            ({"CMAKE_CXX_FLAGS_RELWITHDEBINFO": "-O2 -g -march=znver3"}, r"^CMAKE_CXX_FLAGS_RELWITHDEBINFO already"),
            ({"CMAKE_C_FLAGS_MINSIZEREL": "/O1 /arch:AVX2"}, r"^CMAKE_C_FLAGS_MINSIZEREL already"),
            ({"CMAKE_CXX_FLAGS_RELEASE": "-O2 -arch arm64"}, r"^CMAKE_CXX_FLAGS_RELEASE already"),
            ({"CMAKE_C_FLAGS": "-O2\t-march=x86-64"}, r"^CMAKE_C_FLAGS already selects"),
            ({"CMAKE_C_FLAGS": '-O2 "-march=x86-64"'}, r"^CMAKE_C_FLAGS already selects"),
            ({"CMAKE_CXX_FLAGS": "-O2 '-mcpu=cortex-a72'"}, r"^CMAKE_CXX_FLAGS already selects"),
            ({"CMAKE_C_COMPILER_ARG1": " -march=x86-64"}, r"^CMAKE_C_COMPILER_ARG1 already selects"),
            ({"CMAKE_CXX_COMPILER_ARG1": " clang++"}, r"^$"),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_HOST_SYSTEM_PROCESSOR": "x86_64",
              "CMAKE_OSX_ARCHITECTURES": "x86_64", "CMAKE_OSX_SYSROOT": "iphonesimulator"},
             r"^CMAKE_OSX_SYSROOT selects iphonesimulator$"),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_OSX_SYSROOT":
              "/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/"
              "iPhoneSimulator.sdk"}, r"^CMAKE_OSX_SYSROOT selects "),
            ({"CMAKE_SYSTEM_NAME": "Darwin", "CMAKE_OSX_SYSROOT": "macosx"}, r"^$"),
            # Only Release-like configs are tuned, so Debug flags do not count;
            # -mtune alone keeps the default target ISA.
            ({"CMAKE_C_FLAGS_DEBUG": "-march=x86-64"}, r"^$"),
            ({"CMAKE_C_FLAGS": "-O2 -mtune=generic"}, r"^$"),
        ]
        for variables, expected in cases:
            with self.subTest(variables=variables):
                self.assertRegex(self.skip_reason(**variables), expected)
        # MSVC-style compilers read options from CL and _CL_ as well.
        env_cases = [
            ({"CL": "/arch:AVX"}, {"MSVC": "TRUE"}, r"^the CL environment variable already selects"),
            ({"_CL_": "/O2 /arch:AVX2"}, {"CMAKE_C_COMPILER_FRONTEND_VARIANT": "MSVC"},
             r"^the _CL_ environment variable already selects"),
            ({"CL": "/arch:AVX"}, {}, r"^$"),
        ]
        for environment, variables, expected in env_cases:
            with self.subTest(environment=environment, variables=variables):
                self.assertRegex(self.skip_reason(environment, **variables), expected)

    @unittest.skipUnless(sys.platform.startswith("linux") and X86_HOST, "compares the probe with /proc/cpuinfo")
    def test_msvc_probe_matches_cpuinfo(self):
        # ARG1 holds the compiler behind a launcher ("ccache gcc") or extra
        # arguments ("gcc -pipe"); either way it follows the executable.
        compiler = [ARGS.cc or "cc", *shlex.split(ARGS.cc_arg1 or "")]
        if any(Path(part).stem.lower() in ("cl", "clang-cl") for part in compiler):
            self.skipTest("needs a GNU-style C compiler")
        flags = set()
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.startswith("flags"):
                flags = set(line.split(":", 1)[1].split())
                break
        avx = {"pni", "pclmulqdq", "ssse3", "sse4_1", "sse4_2", "popcnt", "avx"}
        avx2 = avx | {"fma", "movbe", "f16c", "bmi1", "avx2", "bmi2", "abm"}
        avx512 = avx2 | {"avx512f", "avx512dq", "avx512cd", "avx512bw", "avx512vl"}
        expected = "NONE"
        for level, needed in (("AVX", avx), ("AVX2", avx2), ("AVX512", avx512)):
            if needed <= flags:
                expected = level
        with tempfile.TemporaryDirectory(prefix="dsd-native-") as directory:
            program = Path(directory) / "probe"
            self.run_command(*compiler, "-O2", "-o", str(program), str(ROOT / "cmake" / "native_arch_probe.c"))
            self.assertEqual(self.run_command(str(program)).strip(), expected, sorted(flags))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    parser.add_argument("--cc-arg1")
    parser.add_argument("--cxx-arg1")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
