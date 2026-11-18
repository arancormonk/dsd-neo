#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for tools/windows/stage_runtime_dlls.ps1, run under pwsh with fake dumpbin, llvm-readobj and objdump
tools.

The release jobs stage the DLLs a Windows binary needs with this script. An inspection that failed or printed
something other than a dependency report used to leave the binary with no dependencies, so nothing was staged and
the script still succeeded. The fakes print what the real tools print (dumpbin's CRLF report, llvm-readobj's Import
and DelayImport blocks, GNU objdump's pei-x86-64 and LLVM objdump's coff-x86-64 headers and data directory) and are
looked up through PATH as the script looks up the real ones.
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


def dumpbin_report(name, dependencies, file_type="DLL", delay_loaded=()):
    """dumpbin /DEPENDENTS output as MSVC prints it, CRLF line endings included."""
    lines = ["Microsoft (R) COFF/PE Dumper Version 14.44.35217.0",
             "Copyright (C) Microsoft Corporation.  All rights reserved.", "", "",
             f"Dump of file {name}", "", f"File Type: {file_type}", ""]
    if dependencies:
        lines += ["  Image has the following dependencies:", ""] + [f"    {d}" for d in dependencies] + [""]
    if delay_loaded:
        lines += ["  Image has the following delay load dependencies:", ""] + [f"    {d}" for d in delay_loaded] + [""]
    lines += ["  Summary", "", "        1000 .data", "        1000 .pdata", "        3000 .rdata",
              "        1000 .reloc", "        9000 .text", ""]
    return "\r\n".join(lines)


# The 16 entries of a PE image's data directory, as GNU and LLVM objdump -p print them.
DATA_DIRECTORY = ("Export Directory [.edata (or where ever we found it)]", "Import Directory [parts of .idata]",
                  "Resource Directory [.rsrc]", "Exception Directory [.pdata]", "Security Directory",
                  "Base Relocation Directory [.reloc]", "Debug Directory", "Description Directory",
                  "Special Directory", "Thread Storage Directory [.tls]", "Load Configuration Directory",
                  "Bound Import Directory", "Import Address Table Directory", "Delay Import Directory",
                  "CLR Runtime Header", "Reserved")


def data_directory(delay_loaded):
    """The data directory as objdump prints it. Neither GNU nor LLVM objdump names a delay-loaded DLL: an image that
    delay-loads one shows only a non-empty Delay Import Directory (entry d), as lld-link's /delayload output does."""
    lines = ["The Data Directory"]
    for index, name in enumerate(DATA_DIRECTORY):
        rva, size = ("0000000000002008", "00000040") if index == 0xD and delay_loaded else ("0" * 16, "0" * 8)
        lines.append(f"Entry {index:x} {rva} {size} {name}")
    return "\n".join(lines) + "\n"


def gnu_objdump_report(dependencies, delay_loaded=False):
    imports = "".join(f"\n\tDLL Name: {d}\n\tvma:  Hint/Ord Member-Name Bound-To\n\t5338\t   27  ExitProcess\n"
                      for d in dependencies)
    return ("\n{path}:     file format pei-x86-64\n\nCharacteristics 0x2022\n\texecutable\n\tlarge address aware\n"
            "\n" + data_directory(delay_loaded) + "\nThe Import Tables (interpreted .rdata section contents)\n"
            " vma:            Hint    Time      Forward  DLL       First\n" + imports)


def llvm_objdump_report(dependencies, delay_loaded=False):
    imports = "".join(f"\n    DLL Name: {d}\n    vma:  Hint/Ord Member-Name Bound-To\n    0x5338    27  ExitProcess\n"
                      for d in dependencies)
    return ("\n{path}:\tfile format coff-x86-64\nCharacteristics 0x2022\n\texecutable\n\tlarge address aware\n"
            "\n" + data_directory(delay_loaded) + "\nThe Import Tables:\n"
            "  lookup 0000503c time 00000000 fwd 00000000 name 00005990 addr 0000519c\n" + imports)


def llvm_readobj_report(dependencies, delay_loaded=()):
    """llvm-readobj --coff-imports output: an Import block per DLL, a DelayImport block per delay-loaded one."""
    imports = "".join(f"Import {{\n  Name: {d}\n  ImportLookupTableRVA: 0x7710\n  ImportAddressTableRVA: 0x6100\n"
                      f"  Symbol: ExitProcess (27)\n}}\n" for d in dependencies)
    delayed = "".join(f"DelayImport {{\n  Name: {d}\n  Attributes: 0x1\n  ModuleHandle: 0x3000\n"
                      f"  ImportAddressTable: 0x3008\n  ImportNameTable: 0x2048\n  BoundDelayImportTable: 0x0\n"
                      f"  UnloadDelayImportTable: 0x0\n  Import {{\n    Symbol: dep_fn (0)\n"
                      f"    Address: 0x180001024\n  }}\n}}\n" for d in delay_loaded)
    return "\nFile: {path}\nFormat: COFF-x86-64\nArch: x86_64\nAddressSize: 64bit\n" + imports + delayed


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

    def visual_studio(self, version="14.51.36231", toolset="VC143"):
        """A Visual Studio installation that vswhere finds; returns its redistributable runtime folder."""
        install = self.work / "vs"
        installer = self.program_files / "Microsoft Visual Studio/Installer"
        installer.mkdir(parents=True, exist_ok=True)
        vswhere = installer / "vswhere.exe"
        vswhere.write_text(f"#!/bin/sh\necho '{install}'\n", encoding="utf-8")
        vswhere.chmod(0o755)
        crt = install / f"VC/Redist/MSVC/{version}/x64/Microsoft.{toolset}.CRT"
        crt.mkdir(parents=True)
        return crt

    def stage(self, search=None):
        # Only the fakes are on PATH, so no real dumpbin, objdump or llvm-objdump on the host can answer instead.
        env = dict(os.environ, PATH=str(self.tools), SystemRoot=str(self.work / "windows"))
        env["ProgramFiles(x86)"] = str(self.program_files)
        runner = self.work / "run.ps1"
        runner.write_text(RUNNER, encoding="utf-8")
        search = self.search if search is None else search
        result = subprocess.run([PWSH, "-NoProfile", "-NonInteractive", "-File", str(runner), "-Script", str(SCRIPT),
                                 "-Target", str(self.target), "-Dest", str(self.dest), "-Search", str(search)],
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

    def test_runtime_is_found_when_the_destination_is_the_only_search_folder(self):
        # The release workflows name one search folder, and a caller may name only the destination: the folders to
        # search then reduce to one, and the Visual C++ runtime folders must still be searched after it.
        crt = self.visual_studio()
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["VCRUNTIME140_1.dll", "KERNEL32.dll"]))
        (crt / "VCRUNTIME140_1.dll").write_bytes(pe_image(b"runtime"))
        self.respond("dumpbin", "VCRUNTIME140_1.dll", out=dumpbin_report("VCRUNTIME140_1.dll", ["KERNEL32.dll"]))
        rc, output = self.stage(search=self.dest)
        self.assertEqual(rc, 0, output)
        self.assertEqual(self.staged(), ["VCRUNTIME140_1.dll", "dlls-manifest.txt"])

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

    def reset_tools(self):
        shutil.rmtree(self.tools)
        self.tools.mkdir()
        for staged in self.dest.iterdir():
            if staged != self.target:
                staged.unlink()

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

    def test_debug_runtime_dlls_are_refused(self):
        # A dependency built with /MDd imports Microsoft's debug runtime, which may not be redistributed: neither the
        # copy in the search folders nor the one in System32 may be staged.
        (self.system32 / "KERNEL32.dll").write_bytes(pe_image(b"system"))
        for name, where in (("VCRUNTIME140D.dll", self.search), ("MSVCP140D.dll", self.search),
                            ("ucrtbased.dll", self.system32), ("VCRUNTIME140_1D.dll", self.system32)):
            with self.subTest(dll=name):
                self.reset_tools()
                self.tool("dumpbin")
                self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["a.dll", "KERNEL32.dll"]))
                (self.search / "a.dll").write_bytes(pe_image(b"a"))
                self.respond("dumpbin", "a.dll", out=dumpbin_report("a.dll", [name, "KERNEL32.dll"]))
                (where / name).write_bytes(pe_image(b"debug runtime"))
                self.respond("dumpbin", name, out=dumpbin_report(name, ["KERNEL32.dll"]))
                output = self.assert_refused([name, "debug Visual C++ runtime"])
                self.assertIn(f"{self.dest / 'a.dll'} imports {name}", output)
                self.assertFalse((self.dest / name).exists(), output)
                (where / name).unlink()

    def test_only_listed_windows_dlls_stay_out_of_bin(self):
        # A DLL that a driver package put in the build machine's System32 is not part of Windows: a clean machine lacks
        # it, so the release must carry it, and staging stops when it cannot.
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["libusbK.dll", "KERNEL32.dll"]))
        (self.system32 / "libusbK.dll").write_bytes(pe_image(b"driver package"))
        (self.system32 / "KERNEL32.dll").write_bytes(pe_image(b"system"))
        self.assert_refused(["Missing DLL dependencies", "libusbK.dll"])
        self.assertFalse((self.dest / "libusbK.dll").exists())
        # A Windows DLL on the list is never looked for or copied, wherever a copy happens to be.
        self.reset_tools()
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["WS2_32.dll", "IPHLPAPI.DLL"]))
        (self.search / "WS2_32.dll").write_bytes(pe_image(b"stray copy"))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        self.assertEqual([name for name in self.staged() if name.endswith(".dll")], [])

    def test_delay_loaded_dependencies_are_staged(self):
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        (self.search / "dep.dll").write_bytes(pe_image(b"dep"))
        for tool, report in (
                ("dumpbin", dumpbin_report("dsd-neo.exe", ["a.dll"], delay_loaded=["dep.dll"])),
                # llvm-readobj on Windows ends its lines with CRLF.
                ("llvm-readobj", llvm_readobj_report(["a.dll"], delay_loaded=["dep.dll"]).replace("\n", "\r\n"))):
            with self.subTest(tool=tool):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report)
                empty = dumpbin_report("x.dll", []) if tool == "dumpbin" else llvm_readobj_report([])
                for name in ("a.dll", "dep.dll"):
                    self.respond(tool, name, out=empty)
                rc, output = self.stage()
                self.assertEqual(rc, 0, output)
                self.assertEqual(self.staged(), ["a.dll", "dep.dll", "dlls-manifest.txt"])
                if tool == "llvm-readobj":
                    calls = normalized((self.tools / "calls.log").read_text(encoding="utf-8")).splitlines()
                    self.assertIn(f"llvm-readobj --coff-imports {self.target}", calls)

    def test_every_dependency_name_is_staged(self):
        # A DLL name may hold spaces and need not end in .dll; each tool's report names it as it names any other.
        names = ["Vendor Support.dll", "vendor.drv"]
        for name in names + ["Late Loader.dll"]:
            (self.search / name).write_bytes(pe_image(name.encode()))
        for tool, report, delayed in (
                ("dumpbin", lambda: dumpbin_report("dsd-neo.exe", names, delay_loaded=["Late Loader.dll"]), True),
                ("llvm-readobj", lambda: llvm_readobj_report(names, delay_loaded=["Late Loader.dll"]), True),
                ("objdump", lambda: gnu_objdump_report(names), False),
                ("llvm-objdump", lambda: llvm_objdump_report(names), False)):
            with self.subTest(tool=tool):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report())
                empty = {"dumpbin": dumpbin_report("x.dll", []), "llvm-readobj": llvm_readobj_report([]),
                         "objdump": gnu_objdump_report([]), "llvm-objdump": llvm_objdump_report([])}[tool]
                for name in names + ["Late Loader.dll"]:
                    self.respond(tool, name, out=empty)
                rc, output = self.stage()
                self.assertEqual(rc, 0, output)
                expected = names + (["Late Loader.dll"] if delayed else [])
                self.assertEqual([name for name in self.staged() if name != "dlls-manifest.txt"], sorted(expected))

    def test_unread_dependency_lines_stop_staging(self):
        # A line of a dependency list that names no DLL stops staging: skipping it could leave a DLL the binary loads
        # out of the release.
        bad = "C:\\vendor\\late.dll"
        for tool, report, words in (
                ("dumpbin", dumpbin_report("dsd-neo.exe", ["KERNEL32.dll"], delay_loaded=[bad]), [bad]),
                ("llvm-readobj", llvm_readobj_report(["KERNEL32.dll"]).replace("  Name: KERNEL32.dll\n", ""),
                 ["Import block without a DLL name"]),
                ("llvm-readobj", llvm_readobj_report(["KERNEL32.dll"]).replace("KERNEL32.dll", bad), [bad]),
                ("llvm-readobj", llvm_readobj_report(["KERNEL32.dll"]) + "Unexpected: line\n", ["Unexpected: line"]),
                ("objdump", gnu_objdump_report(["KERNEL32.dll"]).replace("KERNEL32.dll", bad), [bad]),
                ("llvm-objdump", llvm_objdump_report(["KERNEL32.dll"]).replace("DLL Name: KERNEL32.dll", "DLL Name:"),
                 ["DLL Name:"])):
            with self.subTest(tool=tool, words=words):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report)
                output = self.assert_refused([tool, str(self.target)] + words)
                self.assertNotIn("Traceback", output)

    def test_objdump_cannot_list_delay_loaded_dependencies(self):
        # GNU and LLVM objdump -p list only the import table: for an image whose one dependency is delay-loaded they
        # exit 0, name no DLL and show a non-empty Delay Import Directory. Staging stops rather than stage nothing.
        (self.search / "dep.dll").write_bytes(pe_image(b"dep"))
        for tool, report in (("objdump", gnu_objdump_report), ("llvm-objdump", llvm_objdump_report)):
            with self.subTest(tool=tool):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report([], delay_loaded=True))
                self.assert_refused([tool, str(self.target), "delay-loaded"])
            with self.subTest(tool=tool, report="no data directory"):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report([]).replace(data_directory(False), ""))
                self.assert_refused([tool, str(self.target), "Delay Import Directory"])

    def test_llvm_readobj_report_for_another_file_stops_staging(self):
        self.tool("llvm-readobj")
        self.respond("llvm-readobj", "dsd-neo.exe",
                     out=llvm_readobj_report(["a.dll"]).replace("{path}", str(self.work / "other.exe")))
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        self.assert_refused(["llvm-readobj", str(self.target)])


if __name__ == "__main__":
    unittest.main()
