#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replay publisher shell steps against the basenames preserved by artifact uploads."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest


ROOT = Path(os.environ.get("DSD_TEST_SOURCE_ROOT", Path(__file__).resolve().parents[2]))
CASES = (
    ("windows-ci.yaml", "publish-native-msvc", "dsd-neo-msvc-x86_64-native", ".zip"),
    ("macos-ci.yaml", "publish-portable-dmg", "dsd-neo-macos-arm64-portable", ".dmg"),
    ("linux-appimage.yaml", "publish-appimage", "dsd-neo-linux-x86_64-portable", ".AppImage"),
    ("linux-appimage.yaml", "publish-appimage", "dsd-neo-linux-aarch64-portable", ".AppImage"),
    ("android-ci.yaml", "publish-apk", "dsd-neo-android-arm64-app", ".apk"),
)


def steps(source):
    # Read only the workflow's consistently indented step blocks; no YAML dependency.
    return re.split(r"(?m)^      - ", source)[1:]


def field(step, name):
    return re.search(rf"(?m)^ +{re.escape(name)}: (.+)$", step).group(1)


def expand(text, values):
    return re.sub(r"\$\{\{\s*(.*?)\s*\}\}", lambda match: values[match[1]], text)


def script(step):
    block = re.search(r"(?m)^        run: \|\n((?: {10}[^\n]*\n|\n)+)", step)
    return textwrap.dedent(block[1]) if block else ""


@unittest.skipUnless(os.name == "posix" and shutil.which("bash"), "publishers run in Bash")
class ReleaseSbomFilenames(unittest.TestCase):
    def check_publication(self, pipeline, values, work, package, version):
        if version == "nightly":
            upload = next(step for step in pipeline if "uses: softprops/action-gh-release@" in step)
            files = re.search(r"(?m)^          files: \|\n((?: {12}[^\n]*\n)+)", upload)[1]
            self.assertEqual(expand(textwrap.dedent(files), values).splitlines(),
                             [package, package + ".spdx.json"])
            return
        binaries = work / "bin"
        binaries.mkdir()
        calls = work / "calls"
        calls.mkdir()
        gh = binaries / "gh"
        gh.write_text('#!/usr/bin/env bash\nset -eu\nprintf "%s\\n" "$@" > "$DSD_TEST_GH_LOG/$2"\n',
                      encoding="utf-8")
        gh.chmod(0o755)
        env = {**os.environ, "PATH": str(binaries) + os.pathsep + os.environ["PATH"],
               "DSD_TEST_GH_LOG": str(calls), "GH_TOKEN": "test-fixture",
               "RELEASE_TAG": version, "RELEASE_REPOSITORY": "arancormonk/dsd-neo",
               "APK_PATH": package, "ZIP_PATH": package, "DMG_PATH": package, "APPIMAGE_PATH": package,
               "SBOM_PATH": package + ".spdx.json"}
        wait = next(step for step in pipeline if step.startswith("name: Wait for release metadata\n"))
        result = subprocess.run(["bash", str(ROOT / field(wait, "run"))], cwd=work, env=env,
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((calls / "view").read_text().splitlines(),
                         ["release", "view", version, "--repo", "arancormonk/dsd-neo"])
        upload = next(step for step in pipeline if "gh release upload" in step)
        result = subprocess.run(["bash", "-eu", "-o", "pipefail", "-c", expand(script(upload), values)],
                                cwd=work, env=env, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual((calls / "upload").read_text().splitlines(),
                         ["release", "upload", version, package, package + ".spdx.json",
                          "--repo", "arancormonk/dsd-neo", "--clobber"])

    def replay(self, case, version, missing=None):
        workflow, job, stem, extension = case
        source = (ROOT / ".github/workflows" / workflow).read_text(encoding="utf-8")
        publisher = re.search(rf"(?ms)^  {job}:\n(.*?)(?=^  [\w-]+:|\Z)", source)[1]
        artifact = f"{stem}-{version}"
        package = f"dist/{artifact}{extension}"
        values = {}
        for prefix in ("steps.names.outputs", "steps.zip.outputs", "steps.dmg.outputs"):
            for key in ("ART", "ART_NAME"):
                values[f"{prefix}.{key}"] = artifact
            for key in ("OUT", "ZIP_PATH", "DMG_PATH", "APK_PATH"):
                values[f"{prefix}.{key}"] = package
        values.update({"needs.build-apk.outputs.art_name": artifact,
                       "needs.build-apk.outputs.apk_path": package})
        uploaded = next(step for step in steps(source) if step.startswith("name: Upload enriched SBOM\n"))
        attested = next(step for step in steps(source)
                        if "uses: actions/attest@" in step and "sbom-path:" in step)
        self.assertEqual(expand(field(attested, "subject-path"), values), package)
        self.assertEqual(expand(field(attested, "sbom-path"), values),
                         expand(field(uploaded, "path"), values))
        pipeline = steps(publisher)
        download = next(index for index, step in enumerate(pipeline)
                        if step.startswith(("name: Download package SBOM artifact\n",
                                            "name: Download APK SBOM artifact\n")))
        verify = next(index for index, step in enumerate(pipeline)
                      if step.startswith("name: Verify downloaded release files\n"))
        self.assertEqual(expand(field(uploaded, "name"), values),
                         expand(field(pipeline[download], "name"), values))
        # upload-artifact's label identifies the artifact, not the file it contains.
        basename = Path(expand(field(uploaded, "path"), values)).name
        if missing == "package-basename":
            basename = artifact + ".spdx.json"
        with tempfile.TemporaryDirectory(prefix="dsd-release-sbom-") as directory:
            work = Path(directory)
            destination = work / field(pipeline[download], "path")
            destination.mkdir(parents=True)
            contents = b'{"spdxVersion":"SPDX-2.3","packages":[]}\n'
            if missing != "sbom":
                (destination / basename).write_bytes(contents)
            if missing != "package":
                (work / package).write_bytes(b"package fixture\n")
            commands = [expand(script(step), values) for step in pipeline[download + 1:verify + 1]]
            self.assertTrue(commands[-1], "Publisher must verify its downloaded files")
            result = subprocess.run(["bash", "-eu", "-o", "pipefail", "-c", "\n".join(commands)], cwd=work,
                                    env={**os.environ, "APK_PATH": package, "ART_NAME": artifact},
                                    capture_output=True, text=True)
            if missing:
                self.assertNotEqual(result.returncode, 0, "Publisher accepted missing or misnamed assets")
            else:
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual((work / (package + ".spdx.json")).read_bytes(), contents)
                self.check_publication(pipeline, values, work, package, version)

    def test_uploaded_basename_reaches_nightly_and_tag_publishers(self):
        for case in CASES:
            for version in ("nightly", "v1.2.3"):
                with self.subTest(workflow=case[0], package=case[2], version=version):
                    self.replay(case, version)

    def test_missing_or_misnamed_files_block_publication(self):
        for case in CASES:
            for missing in ("package", "sbom", "package-basename"):
                with self.subTest(workflow=case[0], package=case[2], missing=missing):
                    self.replay(case, "v1.2.3", missing)


if __name__ == "__main__":
    unittest.main()
