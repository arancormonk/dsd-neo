#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Regressions for tools/windows/stage_runtime_dlls.ps1, run under pwsh with fake dumpbin, llvm-readobj and objdump
tools.

The release jobs stage the DLLs a Windows binary needs with this script. An inspection that failed or printed
something other than a dependency report used to leave the binary with no dependencies, so nothing was staged and
the script still succeeded. The fakes print what the real tools print (dumpbin's CRLF report, llvm-readobj's Import
and DelayImport blocks, GNU objdump's pei-x86-64 and LLVM objdump's coff-x86-64 headers, data directory, import and
export tables, as GNU objdump 2.47 and LLVM 23 print them for images that lld-link and mingw-w64 GCC built) and are
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
FAKE_TOOL = """#!{python} -IS
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


def pe_image(tag=b"", dll=True, stub=0, machine=0x8664):
    """The headers Windows reads to load a PE32+ (x64) image, then tag: what the staging checks before it asks a tool
    for the imports. A DLL sets IMAGE_FILE_DLL as well as IMAGE_FILE_EXECUTABLE_IMAGE. stub bytes of DOS stub sit
    between the DOS header and the PE header, which e_lfanew points to. Another machine (0x14C for x86, 0xAA64 for
    ARM64) gets the optional header that machine's linker writes: PE32 for x86."""
    dos = bytearray(64 + stub)
    dos[:2] = b"MZ"
    struct.pack_into("<I", dos, 0x3C, 64 + stub)
    pe32 = machine == 0x14C
    coff = struct.pack("<HHIIIHH", machine, 0, 0, 0, 0, 224 if pe32 else 240,
                       (0x2102 if dll else 0x0102) if pe32 else (0x2022 if dll else 0x0022))
    optional = struct.pack("<H", 0x10B) + bytes(222) if pe32 else struct.pack("<H", 0x20B) + bytes(238)
    return bytes(dos) + b"PE\0\0" + coff + optional + tag


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


def data_directory(delay_loaded, imports=False):
    """The data directory as objdump prints it. Neither GNU nor LLVM objdump names a delay-loaded DLL: an image that
    delay-loads one shows only a non-empty Delay Import Directory (entry d), as lld-link's /delayload output does. An
    image that imports nothing has an empty Import Directory (entry 1)."""
    lines = ["The Data Directory"]
    for index, name in enumerate(DATA_DIRECTORY):
        rva, size = "0" * 16, "0" * 8
        if index == 0xD and delay_loaded:
            rva, size = "0000000000002008", "00000040"
        elif index == 1 and imports:
            rva, size = "000000000000a000", "00000910"
        lines.append(f"Entry {index:x} {rva} {size} {name}")
    return "\n".join(lines) + "\n"


def gnu_import_table(dependencies, null_only=False):
    """GNU objdump's import table: a row per import descriptor, each followed by its DLL Name and members, and the
    all-zero row that ends the table. null_only is a table that holds only that row."""
    lines = ["", "There is an import table in .idata at 0x14000a000", "",
             "The Import Tables (interpreted .idata section contents)",
             " vma:            Hint    Time      Forward  DLL       First",
             "                 Table   Stamp     Chain    Name      Thunk"]
    for index, dependency in enumerate([] if null_only else dependencies):
        lines += [f" {0xa000 + 20 * index:08x}\t{0xa0c8 + 16 * index:08x} 00000000 00000000 "
                  f"{0xa76c + 32 * index:08x} {0xa280 + 16 * index:08x}", "",
                  f"\tDLL Name: {dependency}", "\tvma:     Ordinal  Hint  Member-Name  Bound-To",
                  f"\t{0xa280 + 16 * index:08x}  <none>  0339  GetTickCount", ""]
    count = 0 if null_only else len(dependencies)
    lines += [f" {0xa000 + 20 * count:08x}\t00000000 00000000 00000000 00000000 00000000", ""]
    return "\n".join(lines) + "\n"


def gnu_export_table(name):
    """GNU objdump's export table, which names the image itself."""
    return ("\nThere is an export table in .edata at 0x140009000\n\n"
            "The Export Tables (interpreted .edata section contents)\n\n"
            "Export Flags \t\t\t0\nTime/Date stamp \t\t0\nMajor/Minor \t\t\t0/0\n"
            f"Name \t\t\t\t0000000000009032 {name}\nOrdinal Base \t\t\t1\nNumber in:\n"
            "\tExport Address Table \t\t00000001\n\t[Name Pointer/Ordinal] Table\t00000001\n"
            "Table Addresses\n\tExport Address Table \t\t0000000000009028\n"
            "\tName Pointer Table \t\t000000000000902c\n\tOrdinal Table \t\t\t0000000000009030\n\n"
            "Export Address Table -- Ordinal Base 1\n\t          Ordinal  Address  Type\n"
            "\t[   0] +base[   1] 00001580 Export RVA\n\n"
            "[Ordinal/Name Pointer] Table -- Ordinal Base 1\n\t          Ordinal   Hint Name\n"
            "\t[   0] +base[   1]  0000 demo_fn\n")


def gnu_objdump_report(dependencies, delay_loaded=False, exports=None, imports=None):
    """GNU objdump -p: the import table only for an image with an import directory, then the export table. imports
    replaces the import table's text."""
    has_imports = bool(dependencies) or imports is not None
    if imports is None:
        imports = gnu_import_table(dependencies) if dependencies else ""
    return ("\n{path}:     file format pei-x86-64\n\nCharacteristics 0x2022\n\texecutable\n\tlarge address aware\n"
            "\n" + data_directory(delay_loaded, has_imports) + imports
            + (gnu_export_table(exports) if exports else ""))


def llvm_import_table(dependencies):
    """LLVM objdump's import table: a lookup row per import descriptor, then its DLL Name and members."""
    lines = ["The Import Tables:"]
    for index, dependency in enumerate(dependencies):
        lines += [f"  lookup {0xa0c8 + 16 * index:08x} time 00000000 fwd 00000000 name {0xa76c + 32 * index:08x} "
                  f"addr {0xa280 + 16 * index:08x}", "", f"    DLL Name: {dependency}", "    Hint/Ord  Name",
                  "         825  GetTickCount", ""]
    return "\n".join(lines) + "\n"


def llvm_export_table(name):
    """LLVM objdump's export table, whose 'DLL name' is the image's own."""
    return f"Export Table:\n DLL name: {name}\n Ordinal base: 1\n Ordinal      RVA  Name\n       1   0x1580  demo_fn\n"


def llvm_objdump_report(dependencies, delay_loaded=False, exports=None, imports=None):
    """LLVM objdump -p: the import table only for an image that imports a DLL, then the export table."""
    if imports is None:
        imports = llvm_import_table(dependencies) if dependencies else ""
    return ("\n{path}:\tfile format coff-x86-64\nCharacteristics 0x2022\n\texecutable\n\tlarge address aware\n"
            "\n" + data_directory(delay_loaded, bool(dependencies)) + "\n" + imports
            + (llvm_export_table(exports) if exports else ""))


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


# stage()'s default: the developer shell of the first toolset visual_studio() made, if any.
DEVELOPER_SHELL = object()


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
        self.redist = None

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
        """A Visual Studio installation that vswhere finds; returns its redistributable runtime folder. The first one
        is the toolset of the developer shell stage() runs in."""
        install = self.work / "vs"
        installer = self.program_files / "Microsoft Visual Studio/Installer"
        installer.mkdir(parents=True, exist_ok=True)
        vswhere = installer / "vswhere.exe"
        vswhere.write_text(f"#!/bin/sh\necho '{install}'\n", encoding="utf-8")
        vswhere.chmod(0o755)
        crt = install / f"VC/Redist/MSVC/{version}/x64/Microsoft.{toolset}.CRT"
        crt.mkdir(parents=True)
        if self.redist is None:
            self.redist = self.developer_shell(install, "14.51.36500", version)
        return crt

    def developer_shell(self, install, tools_version, redist_version, name="default"):
        """The variables a developer shell of a Visual Studio installation sets: its toolset's folder and the runtime
        folder its version files in VC/Auxiliary/Build pair with that toolset, as the shell reads them."""
        (install / "VC/Tools/MSVC" / tools_version / "bin/Hostx64/x64").mkdir(parents=True, exist_ok=True)
        (install / "VC/Redist/MSVC" / redist_version).mkdir(parents=True, exist_ok=True)
        versions = install / "VC/Auxiliary/Build"
        versions.mkdir(parents=True, exist_ok=True)
        (versions / f"Microsoft.VCToolsVersion.{name}.txt").write_text(tools_version + "\r\n")
        (versions / f"Microsoft.VCRedistVersion.{name}.txt").write_text(redist_version + "\r\n")
        return {"VCToolsInstallDir": f"{install}/VC/Tools/MSVC/{tools_version}/",
                "VCToolsRedistDir": f"{install}/VC/Redist/MSVC/{redist_version}/"}

    def stage(self, search=None, redist=DEVELOPER_SHELL):
        # Only the fakes are on PATH, so no real dumpbin, objdump or llvm-objdump on the host can answer instead.
        env = dict(os.environ, PATH=str(self.tools), SystemRoot=str(self.work / "windows"))
        env["ProgramFiles(x86)"] = str(self.program_files)
        # A Visual Studio developer shell names its toolset's folder and that toolset's redistributable folder here
        # (developer_shell()); None is no developer shell.
        for name in ("VCToolsInstallDir", "VCToolsRedistDir"):
            env.pop(name, None)
        if redist is DEVELOPER_SHELL:
            redist = self.redist
        if redist is not None:
            env.update(redist)
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

    def assert_refused(self, words, **stage):
        rc, output = self.stage(**stage)
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
        (self.visual_studio() / "VCRUNTIME140_1.dll").write_bytes(pe_image(b"redistributable"))
        self.respond("dumpbin", "VCRUNTIME140_1.dll", out=dumpbin_report("VCRUNTIME140_1.dll", ["KERNEL32.dll"]))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        # The runtime DLL is copied from Visual Studio's redistributable folder, not from System32; the OS DLL and the
        # API set are not copied.
        self.assertEqual(self.staged(), ["VCRUNTIME140_1.dll", "a.dll", "dlls-manifest.txt"])
        self.assertEqual((self.dest / "VCRUNTIME140_1.dll").read_bytes(), pe_image(b"redistributable"))
        manifest = normalized((self.dest / "dlls-manifest.txt").read_text(encoding="utf-8-sig")).split()
        self.assertEqual(sorted(manifest), ["VCRUNTIME140_1.dll", "a.dll"])
        calls = normalized((self.tools / "calls.log").read_text(encoding="utf-8")).splitlines()
        self.assertIn(f"dumpbin /DEPENDENTS {self.target}", calls)

    def test_every_visual_cpp_runtime_dll_is_staged(self):
        # The runtime DLLs whose names carry a suffix come with the Visual C++ redistributable, not with Windows: a
        # build machine has them in System32, a clean Windows does not.
        runtime = ["MSVCP140_ATOMIC_WAIT.dll", "MSVCP140_CODECVT_IDS.dll", "VCRUNTIME140_THREADS.dll"]
        crt = self.visual_studio()
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", runtime + ["KERNEL32.dll"]))
        for name in runtime + ["KERNEL32.dll"]:
            (self.system32 / name).write_bytes(pe_image(b"system"))
            self.respond("dumpbin", name, out=dumpbin_report(name, ["KERNEL32.dll"]))
        for name in runtime:
            (crt / name).write_bytes(pe_image(b"redistributable"))
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

    def runtime_import(self, name="VCRUNTIME140.dll"):
        self.reset_tools()
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", [name, "KERNEL32.dll"]))
        self.respond("dumpbin", name, out=dumpbin_report(name, ["KERNEL32.dll"]))

    def test_runtime_comes_only_from_the_toolsets_redistributable_folder(self):
        # Microsoft lets a licensed Visual Studio user distribute the files of VC\Redist (legal/components.json,
        # msvc-runtime). The build machine's System32 copy may be another version, and a search folder's copy may come
        # from anywhere.
        (self.system32 / "VCRUNTIME140.dll").write_bytes(pe_image(b"system"))
        (self.search / "VCRUNTIME140.dll").write_bytes(pe_image(b"vcpkg"))
        # The developer shell's toolset: Visual Studio 2026 names its folder Microsoft.VC145.CRT.
        shell_vars = self.developer_shell(self.work / "vs18", "14.51.36500", "14.51.36231")
        shell = self.work / "vs18/VC/Redist/MSVC/14.51.36231"
        (shell / "x64/Microsoft.VC145.CRT").mkdir(parents=True)
        (shell / "x64/Microsoft.VC145.CRT/VCRUNTIME140.dll").write_bytes(pe_image(b"developer shell"))
        # Another toolset the latest Visual Studio holds, newer than the one that built the binaries.
        newer = self.visual_studio(version="14.52.36400", toolset="VC145")
        (newer / "VCRUNTIME140.dll").write_bytes(pe_image(b"newest toolset"))
        self.runtime_import()
        rc, output = self.stage(redist=shell_vars)
        self.assertEqual(rc, 0, output)
        self.assertEqual((self.dest / "VCRUNTIME140.dll").read_bytes(), pe_image(b"developer shell"))
        # Without a developer shell the toolset that built the binaries is unknown: the newest one installed may not
        # be it, so nothing is taken from it.
        self.runtime_import()
        output = self.assert_refused(["VCRUNTIME140.dll", "no Visual C++ runtime folder", "VCToolsRedistDir"],
                                     redist=None)
        self.assertFalse((self.dest / "VCRUNTIME140.dll").exists(), output)
        # A runtime DLL that the toolset's folder lacks is missing, whatever System32 or the search folders hold.
        self.runtime_import("MSVCP140.dll")
        for where in (self.system32, self.search):
            (where / "MSVCP140.dll").write_bytes(pe_image(b"elsewhere"))
        output = self.assert_refused(["MSVCP140.dll",
                                      f"not in the Visual C++ runtime folder {shell / 'x64/Microsoft.VC145.CRT'}"],
                                     redist=shell_vars)
        self.assertFalse((self.dest / "MSVCP140.dll").exists(), output)

    def test_runtime_comes_from_the_installation_of_the_toolset(self):
        # VCToolsRedistDir must be the runtime folder that the installation of the shell's toolset (VCToolsInstallDir)
        # pairs with that toolset in its version files: another installation's runtime, or another toolset's, would
        # be copied, hashed and recorded as this toolset's.
        crt = self.visual_studio()
        (crt / "VCRUNTIME140.dll").write_bytes(pe_image(b"developer shell"))
        shell = dict(self.redist)
        other = self.developer_shell(self.work / "vs-other", "14.51.36500", "14.51.36231")
        older = self.work / "vs/VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT"
        for folder in (self.work / "vs-other/VC/Redist/MSVC/14.51.36231/x64/Microsoft.VC143.CRT", older):
            folder.mkdir(parents=True)
            (folder / "VCRUNTIME140.dll").write_bytes(pe_image(b"another runtime"))
        for label, variables, words in (
                ("another installation's runtime", dict(shell, VCToolsRedistDir=other["VCToolsRedistDir"]),
                 ["is not in the Visual Studio installation of VCToolsInstallDir", other["VCToolsRedistDir"]]),
                ("another toolset's runtime", dict(shell, VCToolsRedistDir=f"{older.parent.parent}/"),
                 ["is runtime 14.44.35112, which no Microsoft.VCRedistVersion.*.txt", "toolset 14.51.36500"]),
                ("no toolset", {"VCToolsRedistDir": shell["VCToolsRedistDir"]},
                 ["VCToolsRedistDir is set without VCToolsInstallDir"])):
            with self.subTest(case=label):
                self.runtime_import()
                output = self.assert_refused(words, redist=variables)
                self.assertFalse((self.dest / "VCRUNTIME140.dll").exists(), output)
        # An older toolset installed beside the default one has its own pair of version files, in a folder of its own.
        side = self.developer_shell(self.work / "vs", "14.44.35207", "14.44.35112", name="14.44.35207")
        (self.work / "vs/VC/Auxiliary/Build/14.44.35207").mkdir()
        for kind in ("Tools", "Redist"):
            (self.work / f"vs/VC/Auxiliary/Build/Microsoft.VC{kind}Version.14.44.35207.txt").rename(
                self.work / f"vs/VC/Auxiliary/Build/14.44.35207/Microsoft.VC{kind}Version.14.44.35207.txt")
        self.runtime_import()
        rc, output = self.stage(redist=side)
        self.assertEqual(rc, 0, output)
        self.assertEqual((self.dest / "VCRUNTIME140.dll").read_bytes(), pe_image(b"another runtime"))
        self.runtime_import()
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        self.assertEqual((self.dest / "VCRUNTIME140.dll").read_bytes(), pe_image(b"developer shell"))

    def test_runtime_already_in_the_destination_is_the_toolsets_copy(self):
        # Staging keeps a DLL already in the destination rather than copy it again (the release jobs stage three
        # binaries into one bin\). A runtime DLL kept that way must still be the developer shell toolset's own copy.
        crt = self.visual_studio()
        (crt / "VCRUNTIME140.dll").write_bytes(pe_image(b"developer shell"))
        self.runtime_import()
        (self.dest / "VCRUNTIME140.dll").write_bytes(pe_image(b"developer shell"))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        for label, copy, stage, words in (
                ("another toolset's copy", pe_image(b"another toolset"), {},
                 ["differs from", str(crt / "VCRUNTIME140.dll")]),
                ("no developer shell", pe_image(b"developer shell"), {"redist": None},
                 ["no Visual C++ runtime folder", "VCToolsRedistDir"])):
            with self.subTest(case=label):
                self.runtime_import()
                (self.dest / "VCRUNTIME140.dll").write_bytes(copy)
                output = self.assert_refused([str(self.dest / "VCRUNTIME140.dll")] + words, **stage)
                self.assertEqual((self.dest / "VCRUNTIME140.dll").read_bytes(), copy, output)
        # The toolset's folder lacks the file kept in the destination.
        self.runtime_import("MSVCP140.dll")
        (self.dest / "MSVCP140.dll").write_bytes(pe_image(b"kept"))
        self.assert_refused([str(self.dest / "MSVCP140.dll"), f"not in the Visual C++ runtime folder {crt}"])

    def test_unrecorded_runtime_dlls_are_refused(self):
        # A Visual C++ runtime DLL that the msvc-runtime record does not list has no recorded terms: staging stops,
        # even with the file in the toolset's folder.
        crt = self.visual_studio()
        for name in ("vcomp140.dll", "mfc140u.dll", "VCAMP140.dll"):
            with self.subTest(dll=name):
                (crt / name).write_bytes(pe_image(b"redistributable"))
                self.runtime_import(name)
                output = self.assert_refused([name, "msvc-runtime record", str(ROOT / "legal/components.json")])
                self.assertFalse((self.dest / name).exists(), output)

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
                # dumpbin exits 0 after a warning about the file it read, which it names before the severity: the
                # report is partial wherever the line stands, even with a list or a summary that looks complete.
                ("dumpbin", "dsd-neo.exe : warning LNK4048: Invalid format file; ignored\r\n"
                 + dumpbin_report("dsd-neo.exe", ["KERNEL32.dll"]), ["LNK4048"]),
                ("dumpbin", dumpbin_report("dsd-neo.exe", []) + "\r\ndsd-neo.exe : warning LNK4048: Invalid format\r\n",
                 ["LNK4048"]),
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

    def test_export_table_names_no_dependency(self):
        # An image that exports a function names itself in its export table, as LLVM objdump prints it: 'DLL name:',
        # which differs from the import table's 'DLL Name:' only in case. Only the import table names dependencies.
        self.target = self.work / "lib/demo.dll"
        self.target.parent.mkdir()
        self.target.write_bytes(pe_image(b"demo"))
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        for tool, report in (("objdump", gnu_objdump_report), ("llvm-objdump", llvm_objdump_report)):
            for imports in ([], ["a.dll", "KERNEL32.dll"]):
                with self.subTest(tool=tool, imports=imports):
                    self.reset_tools()
                    self.tool(tool)
                    self.respond(tool, "demo.dll", out=report(imports, exports="demo.dll"))
                    self.respond(tool, "a.dll", out=report([], exports="a.dll"))
                    rc, output = self.stage()
                    self.assertEqual(rc, 0, output)
                    self.assertEqual([name for name in self.staged() if name.endswith(".dll")],
                                     [name for name in imports if name == "a.dll"])

    def test_empty_import_tables_are_valid(self):
        # An image with no import directory imports no DLL, and GNU objdump prints no import table for it; an import
        # directory that holds only the descriptor that ends it is empty too, and GNU objdump prints that one row.
        # LLVM objdump prints no import table for either.
        for tool, report in (("objdump", gnu_objdump_report([])),
                             ("objdump", gnu_objdump_report([], imports=gnu_import_table([], null_only=True))),
                             ("llvm-objdump", llvm_objdump_report([])),
                             ("llvm-readobj", llvm_readobj_report([]))):
            with self.subTest(tool=tool, report=report):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report)
                rc, output = self.stage()
                self.assertEqual(rc, 0, output)
                self.assertEqual([name for name in self.staged() if name.endswith(".dll")], [])

    def test_import_tables_the_tool_could_not_read_stop_staging(self):
        # GNU and LLVM objdump exit 0 when they cannot read an import table, or stop reading it part way: GNU objdump
        # says so in its report, or prints a descriptor's row without its DLL Name, or no row that ends the table;
        # LLVM objdump prints the table's heading without a descriptor, or a descriptor without its DLL Name.
        # llvm-readobj and both objdumps print a warning when they read only part of the image.
        gnu_table = gnu_import_table(["a.dll", "KERNEL32.dll"])
        a_rows = gnu_table.split("\tDLL Name: a.dll\n")
        last_row = gnu_table.rstrip("\n").rsplit("\n", 1)[0]
        llvm_table = llvm_import_table(["a.dll", "KERNEL32.dll"])
        cases = (
            ("objdump", "no section", gnu_objdump_report([], imports=(
                "\nThere is an import table, but the section containing it could not be found\n")),
             ["There is an import table, but the section containing it could not be found"]),
            ("objdump", "no contents", gnu_objdump_report([], imports=(
                "\nThere is an import table in .bss, but that section has no contents\n")),
             ["that section has no contents"]),
            ("objdump", "no table", gnu_objdump_report([], imports=""), ["printed no import table"]),
            ("objdump", "descriptor without a name", gnu_objdump_report(
                [], imports=a_rows[0] + "\tDLL Name: a.dll\n".join(a_rows[1:]).replace(
                    "\tDLL Name: KERNEL32.dll\n", "", 1)), ["import descriptor without a DLL name"]),
            ("objdump", "no end of the table", gnu_objdump_report([], imports=last_row + "\n"),
             ["did not print the end of the import table"]),
            ("objdump", "unread members", gnu_objdump_report(
                [], imports=gnu_table.replace("<none>  0339  GetTickCount", "<corrupt: 0x0000beef>", 1)),
             ["<corrupt: 0x0000beef>"]),
            ("objdump", "unread first thunk", gnu_objdump_report([], imports=gnu_table.replace(
                "\tDLL Name: a.dll\n", "\tDLL Name: a.dll\n\nThere is a first thunk, but the section containing it "
                "could not be found\n", 1)), ["There is a first thunk"]),
            ("objdump", "warning", gnu_objdump_report([], imports=gnu_table.split(" 0000a014")[0]) +
             "objdump: warning: private headers incomplete: file truncated\n", ["private headers incomplete"]),
            # BFD names the file it was reading between the program and the severity.
            ("objdump", "file's warning", gnu_objdump_report([], imports=gnu_table.split(" 0000a014")[0]) +
             "objdump: {path}: warning: private headers incomplete: file truncated\n",
             ["could not read all of", "private headers incomplete"]),
            ("llvm-objdump", "heading only", llvm_objdump_report(["a.dll"], imports="The Import Tables:\n"),
             ["did not finish the import table"]),
            ("llvm-objdump", "descriptor without a name", llvm_objdump_report(["a.dll"], imports=llvm_table.replace(
                "    DLL Name: KERNEL32.dll\n", "", 1), exports="dsd-neo.exe"),
             ["import descriptor without a DLL name"]),
            ("llvm-objdump", "warning", llvm_objdump_report(["a.dll"]) +
             "llvm-objdump: warning: '{path}': the import table could not be read\n", ["could not be read"]),
            ("llvm-readobj", "warning in a block", llvm_readobj_report(["a.dll"]).replace(
                "  Symbol: ExitProcess (27)\n", "  Symbol: ExitProcess (27)\n"
                "llvm-readobj: warning: '{path}': unexpected end of the import table\n", 1),
             ["unexpected end of the import table"]),
        )
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        for tool, label, report, words in cases:
            with self.subTest(tool=tool, case=label):
                self.reset_tools()
                self.tool(tool)
                self.respond(tool, "dsd-neo.exe", out=report)
                self.respond(tool, "a.dll", out={"objdump": gnu_objdump_report, "llvm-objdump": llvm_objdump_report,
                                                 "llvm-readobj": llvm_readobj_report}[tool]([]))
                output = self.assert_refused([tool, str(self.target)] + words)
                self.assertNotIn("Traceback", output)

    def test_images_of_another_machine_are_refused(self):
        # The release is x64: Windows refuses to load an x86 or ARM64 DLL into its processes (ERROR_BAD_EXE_FORMAT),
        # though dumpbin reports it as a DLL like any other. Every image staging reads must be x64.
        for label, machine in (("x86", 0x14C), ("ARM64", 0xAA64)):
            for where in ("target", "dependency"):
                with self.subTest(machine=label, image=where):
                    self.reset_tools()
                    self.tool("dumpbin")
                    self.target.write_bytes(pe_image(b"target", dll=False,
                                                     machine=machine if where == "target" else 0x8664))
                    (self.search / "a.dll").write_bytes(pe_image(b"a", machine=machine if where != "target"
                                                                 else 0x8664))
                    self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["a.dll"]))
                    self.respond("dumpbin", "a.dll", out=dumpbin_report("a.dll", ["KERNEL32.dll"]))
                    image = self.target if where == "target" else self.dest / "a.dll"
                    self.assert_refused([str(image), f"0x{machine:04X}", "not x64"])
        self.target.write_bytes(pe_image(b"target", dll=False))

    def test_manifest_lists_hidden_dlls(self):
        # The ZIP carries hidden files like any other, so the DLL list must name them too.
        self.tool("dumpbin")
        self.respond("dumpbin", "dsd-neo.exe", out=dumpbin_report("dsd-neo.exe", ["KERNEL32.dll"]))
        (self.dest / ".hidden.dll").write_bytes(pe_image(b"hidden"))
        rc, output = self.stage()
        self.assertEqual(rc, 0, output)
        manifest = self.dest / "dlls-manifest.txt"
        self.assertTrue(manifest.exists(), "staging wrote no DLL list: it saw no DLL in the destination")
        self.assertEqual(normalized(manifest.read_text(encoding="utf-8-sig")).split(), [".hidden.dll"])

    def test_llvm_readobj_report_for_another_file_stops_staging(self):
        self.tool("llvm-readobj")
        self.respond("llvm-readobj", "dsd-neo.exe",
                     out=llvm_readobj_report(["a.dll"]).replace("{path}", str(self.work / "other.exe")))
        (self.search / "a.dll").write_bytes(pe_image(b"a"))
        self.assert_refused(["llvm-readobj", str(self.target)])


if __name__ == "__main__":
    unittest.main()
