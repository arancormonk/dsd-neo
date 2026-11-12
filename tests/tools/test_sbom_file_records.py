#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Release SBOM file records name the shipped files and carry their real digests."""

import builtins
import errno
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import warnings
import zipfile

ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
TOOL = ROOT / "tools" / "sbom_file_records.py"
RUNNER_ROOT = "C:/Users/runneradmin/AppData/Local/Temp/syft-archive-contents-123/"
ZERO_SHA1 = [{"algorithm": "SHA1", "checksumValue": "0" * 40}]
NO_FILES_KEY = object()


def tool_module():
    spec = importlib.util.spec_from_file_location("sbom_file_records", TOOL)
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


class HalfWriter:
    """A text stream that writes half of what it is given, then fails as a full disk would."""

    def __init__(self, stream):
        self.stream = stream

    def write(self, text):
        self.stream.write(text[:len(text) // 2])
        self.stream.flush()
        raise OSError(errno.ENOSPC, "No space left on device (test)")

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.stream.close()

    def __getattr__(self, name):
        return getattr(self.stream, name)


def digests(data):
    # SPDX 2.3 requires a SHA1 for every file record; it identifies the file alongside SHA256.
    return [{"algorithm": name, "checksumValue": hashlib.new(name.lower(), data).hexdigest()}
            for name in ("SHA1", "SHA256")]


def record(name, checksums=ZERO_SHA1, ident="SPDXRef-File-a"):
    return {"fileName": name, "SPDXID": ident, "checksums": [dict(entry) for entry in checksums],
            "licenseConcluded": "NOASSERTION", "copyrightText": "NOASSERTION"}


class FileRecordTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix="dsd-sbom-files-")
        self.addCleanup(directory.cleanup)
        self.work = Path(directory.name)

    def make_zip(self, members, name="pkg.zip", directories=()):
        path = self.work / name
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")  # duplicate member names are a deliberate fixture
            with zipfile.ZipFile(path, "w") as archive:
                for directory in directories:
                    archive.writestr(directory, b"")
                for member, data in members:
                    archive.writestr(member, data)
        return path

    def make_file(self, data, name="dsd.AppImage"):
        path = self.work / name
        path.write_bytes(data)
        return path

    def write_document(self, files=NO_FILES_KEY, compact=False):
        document = {"spdxVersion": "SPDX-2.3", "SPDXID": "SPDXRef-DOCUMENT", "name": "pkg",
                    "packages": [{"SPDXID": "SPDXRef-Package-x", "name": "x"}],
                    "relationships": [{"spdxElementId": "SPDXRef-DOCUMENT", "relationshipType": "DESCRIBES",
                                       "relatedSpdxElement": "SPDXRef-Package-x"}]}
        if files is not NO_FILES_KEY:
            document["files"] = files
        path = self.work / "doc.spdx.json"
        text = json.dumps(document, separators=(",", ":")) if compact else json.dumps(document, indent=2)
        path.write_text(text + "\n", encoding="utf-8")
        return path

    def run_tool(self, sbom, asset, interpreter_flags=()):
        return subprocess.run([sys.executable, *interpreter_flags, str(TOOL), "--spdx", str(sbom),
                               "--asset", str(asset)], capture_output=True, text=True)

    def bind(self, files, asset):
        sbom = self.write_document(files)
        result = self.run_tool(sbom, asset)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return json.loads(sbom.read_text(encoding="utf-8"))

    def assert_rejected(self, files, asset, reason):
        sbom = self.write_document(files)
        before = sbom.read_bytes()
        result = self.run_tool(sbom, asset)
        self.assertNotEqual(result.returncode, 0, "unbindable records were accepted")
        self.assertRegex(result.stderr, reason)
        self.assertEqual(sbom.read_bytes(), before, "a rejected document was rewritten")

    def test_windows_runner_unpack_path_becomes_member_path_with_digests(self):
        data = b"dll bytes\n"
        asset = self.make_zip([("bin/a.dll", data), ("bin/b.dll", b"other\n")], directories=("bin/",))
        for separator in ("/", "\\"):
            with self.subTest(separator=separator):
                names = [RUNNER_ROOT + "bin/a.dll", RUNNER_ROOT + "bin/b.dll"]
                files = [record(name.replace("/", separator), ident=f"SPDXRef-File-{index}")
                         for index, name in enumerate(names)]
                document = self.bind(files, asset)
                self.assertEqual([entry["fileName"] for entry in document["files"]], ["bin/a.dll", "bin/b.dll"])
                self.assertEqual(document["files"][0]["checksums"], digests(data))
                self.assertEqual(document["files"][1]["checksums"], digests(b"other\n"))
                self.assertEqual(document["files"][0]["SPDXID"], "SPDXRef-File-0")
                self.assertEqual(document["files"][0]["licenseConcluded"], "NOASSERTION")
                original = json.loads(self.write_document().read_text(encoding="utf-8"))
                for key in ("packages", "relationships", "SPDXID", "name"):
                    self.assertEqual(document[key], original[key])

    def test_correct_records_are_unchanged_and_runs_are_idempotent(self):
        data = b"dll bytes\n"
        asset = self.make_zip([("bin/a.dll", data)])
        sbom = self.write_document([record("bin/a.dll", digests(data))], compact=True)
        before = sbom.read_bytes()
        result = self.run_tool(sbom, asset)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sbom.read_bytes(), before)
        sbom = self.write_document([record(RUNNER_ROOT + "bin/a.dll")])
        self.assertEqual(self.run_tool(sbom, asset).returncode, 0)
        first = sbom.read_bytes()
        result = self.run_tool(sbom, asset)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sbom.read_bytes(), first)

    def test_wrong_real_digest_fails(self):
        data = b"dll bytes\n"
        asset = self.make_zip([("bin/a.dll", data)])
        good_sha1, good_sha256 = digests(data)
        self.assert_rejected([record("bin/a.dll", [{"algorithm": "SHA1", "checksumValue": "1" * 40}])],
                             asset, r"SPDXRef-File-a.*SHA1")
        self.assert_rejected([record("bin/a.dll", [good_sha1, {"algorithm": "SHA256", "checksumValue": "2" * 64}])],
                             asset, r"SPDXRef-File-a.*SHA256")
        self.assert_rejected([record("bin/a.dll", [good_sha256, {"algorithm": "SHA1", "checksumValue": ""}])],
                             asset, r"SPDXRef-File-a.*SHA1")

    def test_single_file_asset_record_gets_asset_digests(self):
        data = b"\x7fELF not a zip\n"
        asset = self.make_file(data)
        for name in ("dsd.AppImage", "/github/workspace/dist/dsd.AppImage", "D:\\a\\dist\\dsd.AppImage"):
            with self.subTest(name=name):
                document = self.bind([record(name)], asset)
                self.assertEqual(document["files"][0]["fileName"], "dsd.AppImage")
                self.assertEqual(document["files"][0]["checksums"], digests(data))

    def test_zip_record_naming_the_asset_binds_to_the_asset(self):
        asset = self.make_zip([("bin/a.dll", b"dll\n")])
        document = self.bind([record(RUNNER_ROOT + "bin/a.dll", ident="SPDXRef-File-m"),
                              record("D:/a/dsd-neo/dist/pkg.zip", ident="SPDXRef-File-z")], asset)
        self.assertEqual(document["files"][1]["fileName"], "pkg.zip")
        self.assertEqual(document["files"][1]["checksums"], digests(asset.read_bytes()))
        self.assertEqual(document["files"][0]["fileName"], "bin/a.dll")

    def test_unbindable_records_fail(self):
        asset = self.make_zip([("a.dll", b"top\n"), ("bin/a.dll", b"dll\n"), ("lib/b.dll", b"b\n")],
                              directories=("bin/", "lib/"))
        self.assert_rejected([record(RUNNER_ROOT + "bin/missing.dll")], asset, r"SPDXRef-File-a.*shipped")
        self.assert_rejected([record("")], asset, r"SPDXRef-File-a.*shipped")
        self.assert_rejected([record(RUNNER_ROOT + "bin")], asset, r"SPDXRef-File-a.*shipped")
        self.assert_rejected([record(RUNNER_ROOT + "bin/")], asset, r"SPDXRef-File-a.*shipped")
        self.assert_rejected([record(RUNNER_ROOT + "ta.dll")], asset, r"SPDXRef-File-a.*shipped")
        self.assert_rejected([record(RUNNER_ROOT + "bin/a.dll", ident="SPDXRef-File-1"),
                              record(RUNNER_ROOT + "bin/a.dll", ident="SPDXRef-File-2")], asset,
                             r"SPDXRef-File-2.*bin/a\.dll.*already")
        self.assert_rejected([record("/work/one/bin/a.dll", ident="SPDXRef-File-1"),
                              record("/work/two/lib/b.dll", ident="SPDXRef-File-2")], asset,
                             r"SPDXRef-File-2.*root")
        self.assert_rejected([record(RUNNER_ROOT + "bin/a.dll", [{"algorithm": "MD5", "checksumValue": "0" * 32}])],
                             asset, r"SPDXRef-File-a.*MD5")
        self.assert_rejected([record(RUNNER_ROOT + "pkg.zip", ident="SPDXRef-File-1"),
                              record("pkg.zip", ident="SPDXRef-File-2")], asset, r"SPDXRef-File-2.*pkg\.zip.*already")
        single = self.make_file(b"single\n")
        self.assert_rejected([record("other.AppImage")], single, r"SPDXRef-File-a.*shipped")

    def test_zip_with_duplicate_members_is_rejected(self):
        asset = self.make_zip([("bin/a.dll", b"one\n"), ("bin\\a.dll", b"two\n")])
        self.assert_rejected([record("bin/a.dll")], asset, r"more than one member named bin/a\.dll")

    def test_zip_with_a_file_and_a_link_of_one_name_is_rejected(self):
        asset = self.make_zip([("bin/a.dll", b"dll\n")])
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")  # the duplicate name is the fixture
            with zipfile.ZipFile(asset, "a") as archive:
                link = zipfile.ZipInfo("bin/a.dll")
                link.external_attr = (stat.S_IFLNK | 0o777) << 16
                archive.writestr(link, b"elsewhere.dll")
        self.assert_rejected([record(RUNNER_ROOT + "bin/a.dll")], asset, r"more than one member named bin/a\.dll")

    def test_only_regular_file_entries_are_shipped_files(self):
        unix, dos = 3, 0  # ZipInfo.create_system values
        kinds = (
            # (entry kind, entry name, create_system, external_attr, is a shipped file)
            ("regular file with Unix mode bits", "entry.dll", unix, (stat.S_IFREG | 0o644) << 16, True),
            ("regular file, DOS archive attribute", "entry.dll", dos, 0x20, True),
            ("regular file, no attributes (release ZIP shape)", "entry.dll", dos, 0, True),
            ("directory by trailing slash", "entry/", dos, 0, False),
            ("directory by DOS attribute", "entry", dos, 0x10, False),
            ("directory by Unix type bits", "entry", unix, (stat.S_IFDIR | 0o755) << 16, False),
            ("symlink", "entry.dll", unix, (stat.S_IFLNK | 0o777) << 16, False),
            ("FIFO", "entry.dll", unix, (stat.S_IFIFO | 0o644) << 16, False),
            ("character device", "entry.dll", unix, (stat.S_IFCHR | 0o644) << 16, False),
            ("block device", "entry.dll", unix, (stat.S_IFBLK | 0o644) << 16, False),
            ("socket", "entry.dll", unix, (stat.S_IFSOCK | 0o644) << 16, False),
            ("volume label", "entry", dos, 0x08, False),
        )
        data = b"entry bytes\n"
        for kind, name, system, attributes, shipped in kinds:
            with self.subTest(kind=kind):
                asset = self.work / "kinds.zip"
                with zipfile.ZipFile(asset, "w") as archive:
                    info = zipfile.ZipInfo(name)
                    info.create_system = system
                    info.external_attr = attributes
                    archive.writestr(info, data)
                    # writestr gives a slash-named entry directory attributes itself; keep the ones under test.
                    archive.infolist()[-1].external_attr = attributes
                files = [record(RUNNER_ROOT + name)]
                if shipped:
                    document = self.bind(files, asset)
                    self.assertEqual(document["files"][0]["fileName"], name)
                    self.assertEqual(document["files"][0]["checksums"], digests(data))
                else:
                    self.assert_rejected(files, asset, r"SPDXRef-File-a.*does not name a shipped file")

    def test_longest_member_suffix_wins(self):
        asset = self.make_zip([("a.dll", b"top\n"), ("bin/a.dll", b"nested\n")])
        document = self.bind([record(RUNNER_ROOT + "bin/a.dll")], asset)
        self.assertEqual(document["files"][0]["fileName"], "bin/a.dll")
        self.assertEqual(document["files"][0]["checksums"], digests(b"nested\n"))

    def test_documents_without_file_records_are_not_rewritten(self):
        asset = self.make_file(b"dmg\n", "pkg.dmg")
        for files in (NO_FILES_KEY, None, []):
            with self.subTest(files=files):
                sbom = self.write_document(files, compact=True)
                before = sbom.read_bytes()
                result = self.run_tool(sbom, asset)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(sbom.read_bytes(), before)

    def test_files_that_are_not_a_list_fail(self):
        asset = self.make_file(b"dmg\n", "pkg.dmg")
        for files in ({}, "", 0, False, {"fileName": "pkg.dmg"}, "pkg.dmg"):
            with self.subTest(files=files):
                self.assert_rejected(files, asset, r"files is not a list")

    def test_rewrite_has_the_form_the_other_spdx_tools_write(self):
        data = b"dll bytes\n"
        asset = self.make_zip([("bin/a.dll", data)])
        sbom = self.write_document([record(RUNNER_ROOT + "bin/a.dll"), record("pkg.zip", ident="SPDXRef-File-z")])
        expected = json.loads(sbom.read_text(encoding="utf-8"))
        expected["files"][0].update(fileName="bin/a.dll", checksums=digests(data))
        expected["files"][1].update(checksums=digests(asset.read_bytes()))
        result = self.run_tool(sbom, asset)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        reference = self.work / "reference.json"
        reference.write_text(json.dumps(expected, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(sbom.read_bytes(), reference.read_bytes())

    def test_successful_rewrite_keeps_permissions_and_leaves_no_temporary_file(self):
        asset = self.make_zip([("bin/a.dll", b"dll\n")])
        sbom = self.write_document([record(RUNNER_ROOT + "bin/a.dll")])
        if os.name == "posix":
            sbom.chmod(0o640)
        before = stat.S_IMODE(sbom.stat().st_mode)
        result = self.run_tool(sbom, asset)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(json.loads(sbom.read_text(encoding="utf-8"))["files"][0]["fileName"], "bin/a.dll")
        self.assertEqual(sorted(path.name for path in self.work.iterdir()), sorted([asset.name, sbom.name]))
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE(sbom.stat().st_mode), before)

    def run_with_half_writes(self, sbom, asset):
        """Run the tool in-process with every write stream failing part-way, as a full disk would."""
        module = tool_module()
        real_open = io.open

        def failing_open(file, mode="r", *args, **kwargs):
            stream = real_open(file, mode, *args, **kwargs)
            return HalfWriter(stream) if "w" in mode else stream

        argv = ["sbom_file_records.py", "--spdx", str(sbom), "--asset", str(asset)]
        with patch.object(sys, "argv", argv), patch.object(builtins, "open", failing_open), \
                patch.object(io, "open", failing_open):
            with self.assertRaises(OSError):
                module.main()

    def test_failed_write_leaves_the_document_and_no_temporary_file(self):
        # Runs everywhere, Windows included.
        asset = self.make_zip([("bin/a.dll", b"dll\n")])
        sbom = self.write_document([record(RUNNER_ROOT + "bin/a.dll")])
        before = sbom.read_bytes()
        self.run_with_half_writes(sbom, asset)
        self.assertEqual(sbom.read_bytes(), before)
        self.assertEqual(sorted(path.name for path in self.work.iterdir()), sorted([asset.name, sbom.name]))

    def linked_document(self, files):
        """Return (real document, symlink to it in another directory), or skip where links cannot be made."""
        real = self.work / "real"
        links = self.work / "links"
        real.mkdir()
        links.mkdir()
        document = self.write_document(files).replace(real / "doc.spdx.json")
        link = links / "doc.spdx.json"
        try:
            link.symlink_to(document)
        except (OSError, NotImplementedError) as error:
            self.skipTest(f"this platform or account cannot create symbolic links ({error})")
        return document, link

    def test_rewrite_through_a_symlink_updates_the_real_file(self):
        data = b"dll bytes\n"
        asset = self.make_zip([("bin/a.dll", data)])
        document, link = self.linked_document([record(RUNNER_ROOT + "bin/a.dll")])
        if os.name == "posix":
            document.chmod(0o640)
        result = self.run_tool(link, asset)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertTrue(link.is_symlink(), "the link was replaced by a regular file")
        self.assertEqual(link.resolve(), document.resolve())
        self.assertEqual(json.loads(document.read_text(encoding="utf-8"))["files"][0]["checksums"], digests(data))
        self.assertEqual([path.name for path in document.parent.iterdir()], [document.name])
        self.assertEqual([path.name for path in link.parent.iterdir()], [link.name])
        if os.name == "posix":
            self.assertEqual(stat.S_IMODE(document.stat().st_mode), 0o640)

    def test_failed_write_through_a_symlink_leaves_the_real_file(self):
        asset = self.make_zip([("bin/a.dll", b"dll\n")])
        document, link = self.linked_document([record(RUNNER_ROOT + "bin/a.dll")])
        before = document.read_bytes()
        self.run_with_half_writes(link, asset)
        self.assertTrue(link.is_symlink())
        self.assertEqual(document.read_bytes(), before)
        self.assertEqual([path.name for path in document.parent.iterdir()], [document.name])
        self.assertEqual([path.name for path in link.parent.iterdir()], [link.name])

    @unittest.skipUnless(os.name == "posix", "file-size limits (RLIMIT_FSIZE) exist only on POSIX systems")
    def test_file_size_limit_leaves_the_document_and_no_temporary_file(self):
        import resource
        import signal

        asset = self.make_zip([("bin/a.dll", b"dll\n")])
        sbom = self.write_document([record(RUNNER_ROOT + "bin/a.dll")])
        before = sbom.read_bytes()
        limit = 64  # far below any rewritten document, so the write fails part-way

        def limit_file_size():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)  # fail the write with EFBIG instead of killing
            resource.setrlimit(resource.RLIMIT_FSIZE, (limit, limit))

        result = subprocess.run([sys.executable, str(TOOL), "--spdx", str(sbom), "--asset", str(asset)],
                                capture_output=True, text=True, preexec_fn=limit_file_size)
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(sbom.read_bytes(), before)
        self.assertEqual(sorted(path.name for path in self.work.iterdir()), sorted([asset.name, sbom.name]))

    def test_runs_with_docstrings_stripped(self):
        data = b"dll bytes\n"
        asset = self.make_zip([("bin/a.dll", data)])
        sbom = self.write_document([record(RUNNER_ROOT + "bin/a.dll")])
        result = self.run_tool(sbom, asset, ("-OO",))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        document = json.loads(sbom.read_text(encoding="utf-8"))
        self.assertEqual(document["files"][0]["fileName"], "bin/a.dll")
        self.assertEqual(document["files"][0]["checksums"], digests(data))


def jobs(source):
    # Read only the workflow's consistently indented job and step blocks; no YAML dependency.
    body = re.search(r"(?ms)^jobs:\n(.*?)(?=^\S|\Z)", source)[1]
    return re.split(r"(?m)^  (?=[\w-]+:\n)", body)[1:]


def steps(job):
    return re.split(r"(?m)^      - ", job)[1:]


def field(step, name):
    match = re.search(rf"(?m)^ +{re.escape(name)}: (.+)$", step)
    return match[1].strip() if match else None


def step_env(step):
    block = re.search(r"(?m)^        env:\n((?: {10}[^\n]*\n)+)", step)
    return dict(re.findall(r"(?m)^ {10}([A-Z_][A-Z0-9_]*): (.+)$", block[1])) if block else {}


def run_lines(step):
    block = re.search(r"(?m)^        run: \|\n((?: {10}[^\n]*\n|\n)+)", step)
    return [line.strip() for line in block[1].splitlines() if line.strip()] if block else []


def resolve(word, env):
    match = re.fullmatch(r"\$\{?([A-Z_][A-Z0-9_]*)\}?", word)
    return env.get(match[1]) if match else word


def check_wiring(source):
    """Return the problems with how a workflow's SBOM steps bind their file records."""
    problems = []
    found = 0
    for job in jobs(source):
        pipeline = steps(job)
        for index, step in enumerate(pipeline):
            if "uses: anchore/sbom-action@" not in step:
                continue
            found += 1
            name, output, asset = step.split("\n", 1)[0], field(step, "output-file"), field(step, "file")
            bound = None
            for later_index, later in enumerate(pipeline[index + 1:], index + 1):
                commands = [shlex.split(line) for line in run_lines(later) if line.startswith("python")]
                calls = [words for words in commands if "tools/sbom_file_records.py" in words]
                if calls:
                    env = step_env(later)
                    by_interpreter = {}
                    for words in commands:
                        by_interpreter[words[0]] = words
                    ok = all(words == by_interpreter[words[0]] for words in calls) and all(
                        "tools/sbom_file_records.py" in words for words in by_interpreter.values())
                    for words in calls:
                        arguments = dict(zip(words[2::2], words[3::2]))
                        ok = ok and resolve(arguments.get("--spdx", ""), env) == output
                        ok = ok and resolve(arguments.get("--asset", ""), env) == asset
                    if not ok:
                        problems.append(f"{name}: file-record binding does not run last on its output and asset")
                    bound = later_index
                    break
                if (("uses: actions/upload-artifact@" in later and field(later, "path") == output)
                        or ("uses: actions/attest@" in later and field(later, "sbom-path") == output)):
                    break
            if bound is None:
                problems.append(f"{name}: SBOM is uploaded or attested without binding its file records")
    return found, problems


class WorkflowWiringTests(unittest.TestCase):
    def test_every_sbom_step_binds_file_records_before_upload(self):
        total = 0
        for workflow in sorted((ROOT / ".github/workflows").glob("*.y*ml")):
            found, problems = check_wiring(workflow.read_text(encoding="utf-8"))
            total += found
            self.assertEqual(problems, [], workflow.name)
        self.assertGreaterEqual(total, 5, "expected the release SBOM steps")

    def test_unwired_sbom_step_is_reported(self):
        workflow = (ROOT / ".github/workflows/windows-ci.yaml").read_text(encoding="utf-8")
        self.assertTrue("tools/sbom_file_records.py" in workflow, "windows-ci.yaml does not bind file records")
        unwired = "\n".join(line for line in workflow.splitlines() if "tools/sbom_file_records.py" not in line)
        found, problems = check_wiring(unwired + "\n")
        self.assertEqual(found, 1)
        self.assertEqual(len(problems), 1, problems)


if __name__ == "__main__":
    unittest.main()
