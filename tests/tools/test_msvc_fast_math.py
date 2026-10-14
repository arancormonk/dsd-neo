#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check generated MSVC /fp choices from any host; also compile them on Windows."""

import argparse
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
PROJECT = r'''
cmake_minimum_required(VERSION 3.20)
project(msvc_fast_math_test LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "" FORCE)
include("${DSD_SOURCE_DIR}/cmake/MsvcFastMath.cmake")

add_compile_definitions(KEEP_DIRECTORY_OPTION)
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/shared.c" "int c_value(void) { return 1; }\n")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/shared.cpp" "int cpp_value() { return 2; }\n")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/precise.c" "int precise_value(void) { return 3; }\n")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/strict.c" "int strict_value(void) { return 4; }\n")
set_source_files_properties(precise.c PROPERTIES COMPILE_OPTIONS "/fp:precise;/DKEEP_SOURCE_OPTION")
set_source_files_properties(strict.c PROPERTIES COMPILE_OPTIONS "/fp:strict")

add_library(fast OBJECT shared.c shared.cpp precise.c strict.c)
target_compile_definitions(fast PRIVATE KEEP_TARGET_OPTION)
add_library(precise OBJECT shared.c shared.cpp)
target_compile_options(precise PRIVATE /fp:precise)
add_library(fft OBJECT shared.c shared.cpp)
target_compile_options(fft PRIVATE "$<$<CONFIG:Debug>:/fp:strict>")

# A source's properties are directory-scoped, including when its absolute
# path is compiled again from a test directory without the IEEE exception.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/tests")
file(WRITE "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt" [=[
add_library(reused OBJECT ../precise.c)
]=])
add_subdirectory(tests)
add_library(ignored INTERFACE)
add_custom_target(also_ignored)

if(DSD_ENABLE_FAST_MATH)
    cmake_language(DEFER CALL dsd_msvc_fast_math "${CMAKE_CURRENT_SOURCE_DIR}")
endif()
if(MSVC AND CMAKE_C_COMPILER_ID STREQUAL "MSVC")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native-msvc" "yes")
endif()
'''


class MsvcFastMath(unittest.TestCase):
    def run_command(self, *command):
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def check_configuration(self, generator, fast_math, configuration):
        with tempfile.TemporaryDirectory(prefix="dsd-msvc-fp-") as directory:
            # Preserve Windows runners' short temporary paths. Expanding them
            # during source-property lookup must not lose the selected mode.
            source = Path(directory) / "source"
            build = Path(directory) / "build"
            source.mkdir()
            (source / "CMakeLists.txt").write_text(PROJECT, encoding="utf-8")
            command = [ARGS.cmake, "-S", str(source), "-B", str(build), "-G", generator,
                       f"-DDSD_SOURCE_DIR={ROOT.as_posix()}",
                       f"-DDSD_ENABLE_FAST_MATH={'ON' if fast_math else 'OFF'}"]
            if generator == "Ninja":
                command.append(f"-DCMAKE_BUILD_TYPE={configuration}")
            if ARGS.cc:
                command.append(f"-DCMAKE_C_COMPILER={ARGS.cc}")
            if ARGS.cxx:
                command.append(f"-DCMAKE_CXX_COMPILER={ARGS.cxx}")
            # CC/CXX can be wrappers, e.g. macOS CI's "ccache clang". CMake
            # records the real compiler in ARG1 when it splits that command.
            if ARGS.cc_arg1:
                command.append(f"-DCMAKE_C_COMPILER_ARG1={ARGS.cc_arg1}")
            if ARGS.cxx_arg1:
                command.append(f"-DCMAKE_CXX_COMPILER_ARG1={ARGS.cxx_arg1}")
            self.run_command(*command)
            entries = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
            count = 0
            for entry in entries:
                command = entry["command"]
                # CMake 3.20 omits the optional compilation database output field.
                object_path = entry.get("output", command).replace("\\", "/")
                if generator == "Ninja Multi-Config" and f".dir/{configuration}/" not in object_path:
                    continue
                count += 1
                target = re.search(r"CMakeFiles/(\w+)\.dir", object_path)[1]
                name = Path(entry["file"]).name
                if target == "precise" or (target == "fast" and name == "precise.c"):
                    expected = ["precise"]
                elif (target == "fft" and configuration == "Debug") or name == "strict.c":
                    expected = ["strict"]
                else:
                    expected = ["fast"] if fast_math else []
                modes = re.findall(r"(?:^|\s)[-/]fp:(fast|precise|strict)(?=\s|$)", command)
                self.assertEqual(modes, expected, command)
                self.assertIn("KEEP_DIRECTORY_OPTION", command)
                if target == "fast":
                    self.assertIn("KEEP_TARGET_OPTION", command)
                if target == "fast" and name == "precise.c":
                    self.assertIn("KEEP_SOURCE_OPTION", command)
            self.assertEqual(count, 9, entries)
            if (build / "native-msvc").exists():
                log = self.run_command(ARGS.cmake, "--build", str(build), "--config", configuration)
                self.assertNotIn("D9025", log)

    @unittest.skipUnless(shutil.which("ninja"), "Ninja is needed to inspect compile commands")
    def test_fp_modes(self):
        for generator in ("Ninja", "Ninja Multi-Config"):
            for fast_math in (False, True):
                for configuration in ("Debug", "Release"):
                    with self.subTest(generator=generator, fast_math=fast_math, configuration=configuration):
                        self.check_configuration(generator, fast_math, configuration)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", default="cmake")
    parser.add_argument("--cc")
    parser.add_argument("--cxx")
    parser.add_argument("--cc-arg1")
    parser.add_argument("--cxx-arg1")
    ARGS, remaining = parser.parse_known_args()
    unittest.main(argv=[__file__, *remaining])
