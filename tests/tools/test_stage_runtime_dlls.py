#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for tools/windows/stage_runtime_dlls.ps1, run under pwsh with fake dumpbin and objdump tools.

The release jobs stage the DLLs a Windows binary needs with this script. An inspection that failed or printed
something other than a dependency report used to leave the binary with no dependencies, so nothing was staged and
the script still succeeded. The fakes print what the real tools print (dumpbin's CRLF report, GNU objdump's
pei-x86-64 and LLVM objdump's coff-x86-64 headers) and are looked up through PATH as the script looks up the real
ones.
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
SCRIPT = ROOT / "tools/windows/stage_runtime_dlls.ps1"
PWSH = os.environ.get("DSD_NEO_TEST_PWSH") or shutil.which("pwsh")
REQUIRE_TOOLS = os.environ.get("DSD_NEO_REQUIRE_TEST_TOOLS") == "1"
ANSI = re.compile(r"\x1b\[[0-9;?]*[A-Za-z]")

# Answers every call from files beside it: responses/<tool>/<name of the inspected file>.{out,err,rc}. "{path}" in a
# .out file becomes the path the tool was given, as objdump prints it.
FAKE_TOOL = """#!{python} -I
import sys
from pathlib import Path

tool = Path(sys.argv[0])
here = tool.resolve().parent
with open(here / "calls.log", "a", encoding="utf-8") as log:
    log.write(tool.name + " " + " ".join(sys.argv[1:]) + "\\n")
inspected = sys.argv[-1]
base = here / "responses" / tool.name / Path(inspected.replace("\\\\", "/")).name
out = base.with_name(base.name + ".out")
err = base.with_name(base.name + ".err")
rc = base.with_name(base.name + ".rc")
if out.exists():
    sys.stdout.buffer.write(out.read_bytes().replace(b"{{path}}", inspected.encode()))
if err.exists():
    sys.stderr.buffer.write(err.read_bytes())
sys.exit(int(rc.read_text()) if rc.exists() else 0)
"""


# Runs the script as the workflows do and prints a failure's message whole: pwsh's own error view wraps it at the
# console width, which would split the words the tests look for.
RUNNER = """param([string]$Script, [string]$Target, [string]$Dest, [string]$Search)
try {
    & $Script -TargetBinary $Target -DestinationDir $Dest -SearchDirs $Search
} catch {
    [Console]::Error.WriteLine('FAILED: ' + $_.Exception.Message)
    exit 1
}
"""


def pe_image(tag=b"", dll=True, stub=0):
    """The headers Windows reads to load a PE32+ (x64) image, then tag: what the staging checks before it asks a tool
    for the imports. A DLL sets IMAGE_FILE_DLL as well as IMAGE_FILE_EXECUTABLE_IMAGE. stub bytes of DOS stub sit
    between the DOS header and the PE header, which e_lfanew points to."""
    dos = bytearray(64 + stub)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64 + stub)
    coff = struct.pack("<HHIIIHH", 0x8664, 0, 0, 0, 0, 240, 0x2022 if dll else 0x0022)
    return bytes(dos) + b"PE\0\0" + coff + struct.pack("<H", 0x20B) + bytes(238) + tag


def coff_object(tag=b""):
    """An x64 COFF object file as a compiler writes it: no MZ header, no optional header. Not a loadable image."""
    return struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0) + tag


def dumpbin_report(name, dependencies, file_type="DLL"):
    """dumpbin /DEPENDENTS output as MSVC prints it, CRLF line endings included."""
    lines = ["Microsoft (R) COFF/PE Dumper Version 14.44.35217.0",
             "Copyright (C) Microsoft Corporation.  All rights reserved.", "", "",
             f"Dump of file {name}", "", f"File Type: {file_type}", ""]
    if dependencies:
        lines += ["  Image has the following dependencies:", ""] + [f"    {d}" for d in dependencies] + [""]
    lines += ["  Summary", "", "        1000 .data", "        1000 .pdata", "        3000 .rdata",
              "        1000 .reloc", "        9000 .text", ""]
    return "\r\n".join(lines)


def gnu_objdump_report(dependencies):
    imports = "".join(f"\n\tDLL Name: {d}\n\tvma:  Hint/Ord Member-Name Bound-To\n\t5338\t   27  ExitProcess\n"
                      for d in dependencies)
    return ("\n{path}:     file format pei-x86-64\n\nCharacteristics 0x2022\n\texecutable\n\tlarge address aware\n"
            "\nThe Import Tables (interpreted .rdata section contents)\n"
            " vma:            Hint    Time      Forward  DLL       First\n" + imports)


def llvm_objdump_report(dependencies):
    imports = "".join(f"\n    DLL Name: {d}\n    vma:  Hint/Ord Member-Name Bound-To\n    0x5338    27  ExitProcess\n"
                      for d in dependencies)
    return ("\n{path}:\tfile format coff-x86-64\nCharacteristics 0x2022\n\texecutable\n\tlarge address aware\n"
            "\nThe Import Tables:\n  lookup 0000503c time 00000000 fwd 00000000 name 00005990 addr 0000519c\n"
            + imports)


def normalized(text):
    """Child output with CRLF line endings and pwsh's terminal styling removed."""
    return ANSI.sub("", text.replace("\r\n", "\n"))


@unittest.skipUnless(PWSH or REQUIRE_TOOLS, "pwsh is required")
class StageRuntimeDlls(unittest.TestCase):
    def setUp(self):
        if not PWSH:
            self.fail("pwsh is required (DSD_NEO_REQUIRE_TEST_TOOLS=1)")
        self.temp = tempfile.TemporaryDirectory(prefix="dsd-stage-dlls-")
        self.addCleanup(self.temp.cleanup)
        work = Path(self.temp.name)
        self.tools = work / "tools"
        self.tools.mkdir()
        self.system32 = work / "windows/System32"
        self.system32.mkdir(parents=True)
        self.program_files = work / "pf86"
        self.program_files.mkdir()
        self.search = work / "vcpkg/bin"
        self.search.mkdir(parents=True)
        self.dest = work / "stage/bin"
        self.dest.mkdir(parents=True)
        self.target = self.dest / "dsd-neo.exe"
        self.target.write_bytes(pe_image(b"target", dll=False))
        self.work = work

    def tool(self, name):
        path = self.tools / name
        path.write_text(FAKE_TOOL.format(python=sys.executable), encoding="utf-8")
        path.chmod(0o755)
        (self.tools / "responses" / name).mkdir(parents=True, exist_ok=True)

    def respond(self, tool, inspected, out="", err="", rc=0):
        base = self.tools / "responses" / tool / inspected
        base.with_name(inspected + ".out").write_bytes(out.encode())
        base.with_name(inspected + ".err").write_bytes(err.encode())
        base.with_name(inspected + ".rc").write_text(str(rc))

    def stage(self):
        # Only the fakes are on PATH, so no real dumpbin, objdump or llvm-objdump on the host can answer instead.
        env = dict(os.environ, PATH=str(self.tools), SystemRoot=str(self.work / "windows"))
        env["ProgramFiles(x86)"] = str(self.program_files)
        runner = self.work / "run.ps1"
        runner.write_text(RUNNER, encoding="utf-8")
        result = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-File", str(runner), "-Script", str(SCRIPT),
                                 "-Target", str(self.target), "-Dest", str(self.dest), "-Search", str(self.search)],
                                env=env, cwd=self.work, check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        return result.returncode, normalized(result.stdout.decode("utf-8", "replace"))

    def staged(self):
        return sorted(p.name for p in self.dest.iterdir() if p.name != "dsd-neo.exe")

    def assert_refused(self, words):
        rc, output = self.stage()
        self.assertNotEqual(rc, 0, output)
        for word in words:
            self.assertIn(word, output)
        self.assertFalse((self.dest / "dlls-manifest.txt").exists(), output)
        return output

    def test_failed_dumpbin_stops_staging(self):
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out="Microsoft (R) COFF/PE Dumper Version 14.44.35217.0\r\n",
                     err="LINK : fatal error LNK1181: cannot open input file 'dsd-neo.exe'\r\n", rc=1)
        output = self.assert_refused(["dumpbin", str(self.target), "LNK1181"])
        self.assertIn("exit 1", output)

    def test_dumpbin_without_a_report_stops_staging(self):
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out="Microsoft (R) COFF/PE Dumper Version 14.44.35217.0\r\n"
                     "Copyright (C) Microsoft Corporation.  All rights reserved.\r\n\r\n")
        self.assert_refused(["dumpbin", str(self.target)])

    def test_dumpbin_report_stages_dependencies_and_the_runtime(self):
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", [
            "a.dll", "VCRUNTIME140_1.dll", "KERNEL32.dll", "api-ms-win-crt-runtime-l1-1-0.dll"]))
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        self.respond("dumpbin", "a.dll", out=dumpbin_report("a.dll", ["KERNEL32.dll"]))
        for name in ("VCRUNTIME140_1.dll", "KERNEL32.dll"):
            (self.system32 / name).write_bytes(pe_image(b"system"))
        self.respond("dumpbin", "VCRUNTIME140_1.dll", out=dumpbin_report("VCRUNTIME140_1.dll", ["KERNEL32.dll"]))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        # The runtime DLL found only in System32 is copied; the OS DLL and the API set are not.
        self.assertEqual(self.staged(), ["VCRUNTIME140_1.dll", "a.dll", "dlls-manifest.txt"])
        manifest = normalized((self.dest / "dlls-manifest.txt").read_text(encoding="utf-8-sig")).split()
        self.assertEqual(sorted(manifest), ["VCRUNTIME140_1.dll", "a.dll"])
        calls = normalized((self.tools / "calls.log").read_text(encoding="utf-8")).splitlines()
        self.assertIn(f"dumpbin /DEPENDENTS {self.target}", calls)

    def test_every_visual_cpp_runtime_dll_is_staged(self):
        # The runtime DLLs whose names carry a suffix come with the Visual C++ redistributable, not with Windows: a
        # build machine has them in System32, a clean Windows does not.
        runtime = ["MSVCP140_ATOMIC_WAIT.dll", "MSVCP140_CODECVT_IDS.dll", "VCRUNTIME140_THREADS.dll"]
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", runtime + ["KERNEL32.dll"]))
        for name in runtime + ["KERNEL32.dll"]:
            (self.system32 / name).write_bytes(pe_image(b"system"))
            self.respond("dumpbin", name, out=dumpbin_report(name, ["KERNEL32.dll"]))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        self.assertEqual([name for name in self.staged() if name.endswith(".dll")], sorted(runtime))

    def test_dumpbin_report_without_dependencies_is_valid(self):
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", []))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        self.assertEqual([name for name in self.staged() if name.endswith(".dll")], [])

    def test_missing_dependency_still_fails(self):
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["missing.dll"]))
        self.assert_refused(["Missing DLL dependencies", "missing.dll"])

    def test_gnu_and_llvm_objdump_reports_stage_dependencies(self):
        for tool, report in (("objdump", gnu_objdump_report), ("llvm-objdump", llvm_objdump_report)):
            with self.subTest(tool=tool):
                shutil.rmtree(self.tools)
                self.tools.mkdir()
                for staged in self.dest.iterdir():
                    if staged != self.target:
                        staged.unlink()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report(["a.dll", "KERNEL32.dll"]))
                (self.search / "a.dll").write_bytes(pe_image(b"a"))
                (self.system32 / "KERNEL32.dll").write_bytes(pe_image(b"system"))
                self.respond(tool, "a.dll", out=report([]))
                rc, output = self.stage()
                self.assertEqual(rc, 0, output)
                self.assertEqual(self.staged(), ["a.dll", "dlls-manifest.txt"])
                calls = normalized((self.tools / "calls.log").read_text(encoding="utf-8")).splitlines()
                self.assertIn(f"{tool} -p {self.target}", calls)

    def test_failed_objdump_stops_staging(self):
        self.tool("llvm-objdump")
        self.respond("llvm-objdump", "dsd-neo.exe", rc=1, err=f"llvm-objdump: error: '{self.target}': The file was "
                     "not recognized as a valid object file\n")
        self.assert_refused(["llvm-objdump", str(self.target), "not recognized"])

    def test_objdump_report_for_another_file_stops_staging(self):
        self.tool("objdump")
        self.respond("objdump", "dsd-neo.exe",
                     out=gnu_objdump_report(["a.dll"]).replace("{path}", str(self.work / "other.exe")))
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        self.assert_refused(["objdump", str(self.target)])

    def test_objdump_header_without_a_pe_format_stops_staging(self):
        self.tool("objdump")
        self.respond("objdump", "dsd-neo.exe", out=textwrap.dedent("""
            {path}:     file format elf64-x86-64

            Program Header:
            """))
        self.assert_refused(["objdump", str(self.target)])

    def test_object_files_are_refused(self):
        # An x64 object file named like a DLL: llvm-objdump calls it coff-x86-64 like an image and lists no imports,
        # so only the file itself shows it is not one.
        self.target = self.dest / "foo.dll"
        self.target.write_bytes(coff_object(b"object"))
        self.tool("llvm-objdump")
        self.respond("llvm-objdump", "foo.dll", out=llvm_objdump_report([]))
        self.assert_refused(["not a PE image", str(self.target)])

    def test_tool_reports_for_object_files_are_refused(self):
        # The file is an image, but the tool reports an object file: the report is not about what will be loaded.
        for tool, report in (("dumpbin", dumpbin_report("dsd-neo.exe", [], file_type="COFF OBJECT")),
                             ("objdump", gnu_objdump_report([]).replace("pei-x86-64", "pe-x86-64"))):
            with self.subTest(tool=tool):
                shutil.rmtree(self.tools)
                self.tools.mkdir()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report)
                self.assert_refused([tool, str(self.target)])

    def test_pe_header_past_the_first_4096_bytes(self):
        # e_lfanew may point anywhere in the file: a linker places the PE header after a DOS stub of any size.
        self.target.write_bytes(pe_image(b"target", dll=False, stub=8192))
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", []))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        # An e_lfanew past the end of the file, or negative, names no PE header.
        for label, lfanew in (("past the end", 1 << 20), ("negative", -64)):
            with self.subTest(e_lfanew=label):
                image = bytearray(pe_image(b"target", dll=False))
                struct.pack_into("<i", image, 0x3C, lfanew)
                self.target.write_bytes(bytes(image))
                self.assert_refused(["not a PE image", str(self.target)])



if __name__ == "__main__":
    unittest.main()
