#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""run_jvm_tests.py's compile command reaches the Kotlin compiler's JVM intact on Windows as well as elsewhere.

Windows runs kotlinc through kotlinc.bat, and cmd.exe hands a batch file its arguments split at spaces, commas,
semicolons and "=" outside double quotes. A JVM option such as -J-XX:TieredStopAtLevel=1 came apart there, and every
ANDROID_*_JVM test failed with "Improperly specified VM option". The options now go through JAVA_OPTS, which both
launchers put on the java command line. These run in milliseconds and start no JVM.
"""

import importlib
import os
from pathlib import PureWindowsPath
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
runner = importlib.import_module("run_jvm_tests")

# What cmd.exe splits a batch file's arguments at, outside double quotes.
BATCH_DELIMITERS = " \t,;="


def batch_parameters(command_line):
    """%~1, %~2, ... as cmd.exe gives them to a batch file run with @p command_line after its name: split at the
    delimiters outside double quotes, runs of them counting once, each without its surrounding quotes."""
    params = []
    current = ""
    quoted = False
    for ch in command_line:
        if ch == '"':
            quoted = not quoted
            current += ch
        elif ch in BATCH_DELIMITERS and not quoted:
            if current:
                params.append(current)
            current = ""
        else:
            current += ch
    if current:
        params.append(current)
    return [p[1:-1] if len(p) >= 2 and p.startswith('"') and p.endswith('"') else p for p in params]


KOTLINC_BAT = r"C:\hostedtoolcache\windows\kotlinc\bin\kotlinc.BAT"
JAVA_EXE = r"C:\hostedtoolcache\windows\Java\bin\java.EXE"
FILES = (
    PureWindowsPath(r"D:\a\dsd-neo\dsd-neo\android\package\src\io\github\arancormonk\dsdneo\UsbBroker.kt"),
    PureWindowsPath(r"D:\a\dsd-neo\dsd-neo\tests\android\UsbBrokerTest.kt"),
)
OUTPUT = PureWindowsPath(r"C:\Users\RUNNER~1\AppData\Local\Temp\dsd-usb-tests-x1\tests.jar")


class BatchSplit(unittest.TestCase):
    """The emulated split, on what broke: a -J option with "=" comes apart, a quoted path with spaces does not."""

    def test_splits_at_equals(self):
        self.assertEqual(batch_parameters("-J-XX:TieredStopAtLevel=1 -include-runtime"),
                         ["-J-XX:TieredStopAtLevel", "1", "-include-runtime"])

    def test_keeps_a_quoted_path(self):
        self.assertEqual(batch_parameters('"C:\\Program Files\\a.kt" -d out'), ["C:\\Program Files\\a.kt", "-d", "out"])


class KotlincCommand(unittest.TestCase):
    def command(self, environ):
        return runner.compile_command(JAVA_EXE, KOTLINC_BAT, None, FILES, OUTPUT, environ)

    def test_batch_file_gets_every_argument_whole(self):
        argv, _env, _runtime = self.command({})
        line = subprocess.list2cmdline(argv[1:])
        self.assertEqual(batch_parameters(line), argv[1:])

    def test_no_argument_carries_a_jvm_option(self):
        argv, _env, _runtime = self.command({})
        self.assertFalse([arg for arg in argv[1:] if arg.startswith("-J")])
        self.assertFalse([arg for arg in argv[1:] if any(ch in arg for ch in ",;=")])

    def test_java_opts_carry_the_options_after_the_launcher_default(self):
        _argv, env, runtime = self.command({"PATH": "x"})
        self.assertEqual(env["JAVA_OPTS"].split(), [*runner.KOTLINC_DEFAULT_JAVA_OPTS.split(), *runner.JVM_OPTIONS])
        self.assertEqual(env["PATH"], "x")
        self.assertEqual(runtime, str(OUTPUT))

    def test_java_opts_already_set_are_kept(self):
        _argv, env, _runtime = self.command({"JAVA_OPTS": " -Xmx1G "})
        self.assertEqual(env["JAVA_OPTS"].split(), ["-Xmx1G", *runner.JVM_OPTIONS])

    def test_caller_environment_is_not_changed(self):
        environ = {"JAVA_OPTS": "-Xmx1G"}
        self.command(environ)
        self.assertEqual(environ, {"JAVA_OPTS": "-Xmx1G"})


class CachedCompilerCommand(unittest.TestCase):
    """Without kotlinc, java.exe itself compiles: a program, not a batch file, so it takes the options as arguments."""

    def test_java_takes_the_options(self):
        cached = (PureWindowsPath(r"C:\gradle\kotlin-compiler-embeddable.jar"),)
        argv, env, runtime = runner.compile_command(JAVA_EXE, None, cached, FILES, OUTPUT, {"JAVA_OPTS": "-Xmx1G"})
        self.assertEqual(argv[: 1 + len(runner.JVM_OPTIONS)], [JAVA_EXE, *runner.JVM_OPTIONS])
        self.assertEqual(env, {"JAVA_OPTS": "-Xmx1G"})
        self.assertEqual(argv[-2:], ["-d", str(OUTPUT)])
        self.assertEqual(runtime, os.pathsep.join((str(OUTPUT), str(cached[0]))))


if __name__ == "__main__":
    unittest.main()
