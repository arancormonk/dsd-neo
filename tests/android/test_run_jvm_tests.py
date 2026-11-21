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
from pathlib import Path, PureWindowsPath
import subprocess
import sys
import tempfile
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


class CollectorChoice(unittest.TestCase):
    """The serial collector is a default only. HotSpot refuses to start with two collectors selected ("Multiple garbage
    collectors selected"), so a collector the caller selected, in JAVA_OPTS for kotlinc or in the options every JVM or
    every java launch reads (JAVA_TOOL_OPTIONS, HotSpot's _JAVA_OPTIONS, JDK_JAVA_OPTIONS), is the one used, and no
    other is added."""

    SERIAL = "-XX:+UseSerialGC"

    def kotlinc_options(self, environ):
        _argv, env, _runtime = runner.compile_command(JAVA_EXE, KOTLINC_BAT, None, FILES, OUTPUT, environ)
        return env["JAVA_OPTS"].split()

    def cached_argv(self, environ):
        cached = (PureWindowsPath(r"C:\gradle\kotlin-compiler-embeddable.jar"),)
        argv, _env, _runtime = runner.compile_command(JAVA_EXE, None, cached, FILES, OUTPUT, environ)
        return argv

    def test_java_opts_collector_is_kept_alone(self):
        options = self.kotlinc_options({"JAVA_OPTS": "-XX:+UseG1GC"})
        self.assertIn("-XX:+UseG1GC", options)
        self.assertNotIn(self.SERIAL, options)
        self.assertIn("-XX:TieredStopAtLevel=1", options)

    def test_jvm_wide_collector_is_kept_alone(self):
        for name in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS"):
            with self.subTest(name=name):
                environ = {name: "-Xss1M -XX:+UseZGC"}
                self.assertNotIn(self.SERIAL, self.kotlinc_options(environ))
                self.assertNotIn(self.SERIAL, self.cached_argv(environ))
                self.assertNotIn(self.SERIAL, runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", environ))

    def test_quoted_collector_is_kept_alone(self):
        # The JVM takes JAVA_TOOL_OPTIONS='"-XX:+UseG1GC"' (its options may be quoted), and two collectors stop it.
        for quoted in ('"-XX:+UseG1GC"', "'-XX:+UseG1GC'", '-Xss1M "-XX:+UseZGC" -Xmx1G', '-XX:+"UseG1GC"'):
            with self.subTest(quoted=quoted):
                environ = {"JAVA_TOOL_OPTIONS": quoted}
                self.assertNotIn(self.SERIAL, self.cached_argv(environ))
                self.assertNotIn(self.SERIAL, runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", environ))

    def test_java_opts_quotes_are_literal_to_the_launcher(self):
        # kotlinc's launcher passes JAVA_OPTS's words on as they are: an apostrophe there is part of a property.
        options = self.kotlinc_options({"JAVA_OPTS": "-Downer=O'Brien -XX:+UseG1GC"})
        self.assertIn("-XX:+UseG1GC", options)
        self.assertNotIn(self.SERIAL, options)
        # bin/kotlinc: the shell splits the unquoted expansion at whitespace, quotes and all.
        for java_opts, selected in (("-Downer=O'Brien -XX:+UseG1GC", True), ('"-XX:+UseG1GC"', False)):
            with self.subTest(java_opts=java_opts):
                _argv, env, _runtime = runner.compile_command(
                    JAVA_EXE, "/opt/kotlinc/bin/kotlinc", None, FILES, OUTPUT, {"JAVA_OPTS": java_opts}
                )
                self.assertEqual(self.SERIAL not in env["JAVA_OPTS"].split(), selected)

    def test_a_quoted_property_value_selects_no_collector(self):
        # One system property whose value happens to name a collector: the JVM selects none by it.
        environ = {"JAVA_TOOL_OPTIONS": '-Dorg.gradle.jvmargs="-XX:+UseG1GC"'}
        self.assertIn(self.SERIAL, self.cached_argv(environ))

    def test_serial_by_default(self):
        self.assertIn(self.SERIAL, self.kotlinc_options({}))
        self.assertIn(self.SERIAL, self.cached_argv({"JAVA_OPTS": "-XX:+UseG1GC"}))  # java itself never reads JAVA_OPTS
        argv = runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", {"JAVA_TOOL_OPTIONS": "-Xss1M"})
        self.assertEqual(argv, [JAVA_EXE, *runner.JVM_OPTIONS, "-cp", "tests.jar", f"{runner.PACKAGE}.UsbBrokerTestKt"])

    def test_the_test_jvm_gets_its_own_temp_folder(self):
        # The JVM ignores TMPDIR, so without the property the tests' temp folders pile up in /tmp.
        argv = runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", {}, Path("scratch") / "tmp")
        temp = f"-Djava.io.tmpdir={Path('scratch') / 'tmp'}"
        self.assertEqual(argv.count(temp), 1)
        self.assertLess(argv.index(temp), argv.index("-cp"))
        self.assertFalse(any(word.startswith("-Djava.io.tmpdir") for word in
                             runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", {})))

    def test_the_test_jvm_writes_no_shared_performance_data(self):
        # HotSpot's hsperfdata_<user> folder would stay in the system or native temp folder after every JVM.
        self.assertIn("-XX:-UsePerfData", runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", {}))
        self.assertIn("-XX:-UsePerfData", self.cached_argv({}))
        self.assertIn("-XX:-UsePerfData", self.kotlinc_options({}))

    def test_the_native_temp_folder_follows_the_test(self):
        env = runner.temp_environment({"PATH": "/bin", "TMP": "/elsewhere"}, Path("scratch"))
        self.assertEqual((env["TMPDIR"], env["TEMP"], env["TMP"]), (str(Path("scratch")),) * 3)
        self.assertEqual(env["PATH"], "/bin")

    def test_the_compiler_jvm_gets_the_build_temp_folder(self):
        cached = (PureWindowsPath(r"C:\gradle\kotlin-compiler-embeddable.jar"),)
        argv, _env, _runtime = runner.compile_command(JAVA_EXE, None, cached, FILES, OUTPUT, {}, "C:\\b d")
        self.assertEqual(argv.count("-Djava.io.tmpdir=C:\\b d"), 1)
        self.assertLess(argv.index("-Djava.io.tmpdir=C:\\b d"), argv.index("-cp"))
        # kotlinc's POSIX launcher splits JAVA_OPTS at every space, so the folder goes in JAVA_TOOL_OPTIONS, quoted.
        _argv, env, _runtime = runner.compile_command(JAVA_EXE, KOTLINC_BAT, None, FILES, OUTPUT,
                                                      {"JAVA_TOOL_OPTIONS": "-Xss1M"}, "/b d")
        self.assertEqual(runner.jvm_option_words(env["JAVA_TOOL_OPTIONS"]),
                         ["-Xss1M", "-XX:-UsePerfData", "-Djava.io.tmpdir=/b d"])
        _argv, env, _runtime = runner.compile_command(JAVA_EXE, KOTLINC_BAT, None, FILES, OUTPUT, {})
        self.assertNotIn("JAVA_TOOL_OPTIONS", env)

    def test_kotlinc_runs_the_java_its_launcher_picks(self):
        self.assertEqual(runner.kotlinc_java({"JAVACMD": "/jdk21/bin/java", "JAVA_HOME": "/jdk17"}, "java"), "/jdk21/bin/java")
        self.assertEqual(Path(runner.kotlinc_java({"JAVA_HOME": "/jdk17"}, "java")).parent, Path("/jdk17") / "bin")
        self.assertEqual(runner.kotlinc_java({}, "/usr/bin/java"), "/usr/bin/java")

    def test_the_kotlinc_jvms_get_no_perf_data_through_java_tool_options(self):
        # The POSIX launcher's own `java -version` reads JAVA_TOOL_OPTIONS but not JAVA_OPTS.
        _argv, env, _runtime = runner.compile_command(JAVA_EXE, KOTLINC_BAT, None, FILES, OUTPUT, {}, "/b")
        self.assertIn("-XX:-UsePerfData", runner.jvm_option_words(env["JAVA_TOOL_OPTIONS"]))

    def test_isolation_settings_come_from_the_jvm_output(self):
        flags = "     bool UseParallelGC = false {product} {default}\n     bool UsePerfData   = false {product} {command line}\n"
        properties = "Property settings:\n    java.io.tmpdir = /b d/t1\n"
        self.assertEqual(runner.isolation_settings(flags + properties), ("/b d/t1", False))
        # Either stream: -XX:+DisplayVMOutputToStderr puts the flags after the properties, on stderr.
        self.assertEqual(runner.isolation_settings(properties + flags.replace("= false {product} {command",
                                                                              "= true {product} {command")),
                         ("/b d/t1", True))
        self.assertIsNone(runner.isolation_settings(properties))
        self.assertIsNone(runner.isolation_settings(flags))

    def test_launch_errors_keep_no_option_values(self):
        self.assertEqual(runner.launch_error("Error: Could not find or load main class -Dservice.apiKey=s3cr3t"),
                         "Error: Could not find or load main class -Dservice.apiKey=...")
        self.assertIsNone(runner.LAUNCH_ERROR.match("    javax.net.ssl.keyStorePassword = s3cr3t"))

    def test_a_relative_option_file_keeps_the_callers_directory(self):
        # FileListingTest runs in its temp folder, where a relative file the caller's options name would not be found.
        tmp = Path("scratch")
        self.assertEqual(runner.test_directory(True, {}, tmp), tmp)
        self.assertIsNone(runner.test_directory(False, {}, tmp))
        for name, words in (("JDK_JAVA_OPTIONS", "@vm.args"), ("_JAVA_OPTIONS", "-XX:VMOptionsFile=vm.options"),
                            ("JAVA_TOOL_OPTIONS", "-XX:Flags=.hotspotrc")):
            with self.subTest(name=name):
                self.assertIsNone(runner.test_directory(True, {name: words}, tmp))
        absolute = str(Path.cwd() / "vm.args")
        self.assertEqual(runner.test_directory(True, {"JDK_JAVA_OPTIONS": f'"@{absolute}" @@x'}, tmp), tmp)

    def test_only_named_tests_run_in_their_temp_folder(self):
        # The others keep the caller's working directory, so relative files the caller's JVM options name resolve.
        builds = [build for suite in runner.SUITES.values() for build in suite.builds]
        self.assertEqual(sorted(test for build in builds for test in build.in_folder), ["FileListingTest.kt"])
        for build in builds:
            self.assertTrue(set(build.in_folder) <= set(build.tests), build)

    def test_the_effective_temp_folder_is_read_from_the_jvm_settings(self):
        settings = "Property settings:\n    file.encoding = UTF-8\n    java.io.tmpdir = /b d/t1\n    os.name = Linux\n"
        self.assertEqual(runner.tmpdir_setting(settings), "/b d/t1")
        self.assertIsNone(runner.tmpdir_setting("openjdk version 21\n"))

    def test_leftovers_are_named(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertEqual(runner.leftovers(folder), [])
            (Path(folder) / "b").mkdir()
            (Path(folder) / ".a").write_text("")
            self.assertEqual(runner.leftovers(folder), [".a", "b"])

    def test_a_deselected_collector_selects_none(self):
        self.assertIn(self.SERIAL, self.kotlinc_options({"JAVA_OPTS": "-XX:-UseG1GC"}))

    def test_an_argument_file_is_left_its_collector(self):
        # The launcher expands JDK_JAVA_OPTIONS=@file, and a G1 there beside the serial collector stops the JVM.
        environ = {"JDK_JAVA_OPTIONS": "-Xss1M @/home/tester/jvm.args"}
        self.assertNotIn(self.SERIAL, self.kotlinc_options(environ))
        self.assertNotIn(self.SERIAL, self.cached_argv(environ))
        argv = runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", environ)
        self.assertNotIn(self.SERIAL, argv)
        self.assertIn("-XX:TieredStopAtLevel=1", argv)
        # kotlinc puts JAVA_OPTS's words on java's command line, where the launcher expands one too.
        self.assertNotIn(self.SERIAL, self.kotlinc_options({"JAVA_OPTS": "@jvm.args"}))

    def test_a_hotspot_options_file_is_left_its_collector(self):
        # HotSpot reads -XX:Flags= and -XX:VMOptionsFile= wherever it takes options, JAVA_TOOL_OPTIONS included, and a
        # G1 in either file beside the serial collector stops the JVM.
        for option in ("-XX:Flags=/home/tester/.hotspotrc", "-XX:VMOptionsFile=/home/tester/vm.options"):
            for name in ("JAVA_TOOL_OPTIONS", "JDK_JAVA_OPTIONS", "_JAVA_OPTIONS"):
                with self.subTest(option=option, name=name):
                    environ = {name: f"-Xss1M {option}"}
                    self.assertNotIn(self.SERIAL, self.kotlinc_options(environ))
                    self.assertNotIn(self.SERIAL, self.cached_argv(environ))
                    argv = runner.test_command(JAVA_EXE, "tests.jar", "UsbBrokerTest.kt", environ)
                    self.assertNotIn(self.SERIAL, argv)
            with self.subTest(option=option, name="JAVA_OPTS"):
                self.assertNotIn(self.SERIAL, self.kotlinc_options({"JAVA_OPTS": option}))

    def test_an_argument_file_only_where_the_launcher_expands_it(self):
        # The JVM-wide options expand no argument file, and "@@" is a literal "@" to the launcher.
        literal = ({"JAVA_TOOL_OPTIONS": "@jvm.args"}, {"_JAVA_OPTIONS": "@jvm.args"}, {"JDK_JAVA_OPTIONS": "@@x"})
        for environ in literal:
            with self.subTest(environ=environ):
                self.assertIn(self.SERIAL, self.cached_argv(environ))


if __name__ == "__main__":
    unittest.main()
