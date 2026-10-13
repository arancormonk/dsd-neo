#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep pin updates atomic and the released/scanned revision tied to the pin."""

import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import tarfile
import tempfile
import subprocess
import shutil
import sys
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    sys.modules[name] = result
    spec.loader.exec_module(result)
    return result


metadata = module("pffft_metadata")
updater = module("update_pffft")
aur = module("update_aur_pffft")


class PinTests(unittest.TestCase):
    @unittest.skipUnless(sys.platform.startswith("linux") and shutil.which("bash"), "Linux analyzer job")
    def test_cppcheck_source_override_needs_no_dsd_configure(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            binaries = work / "bin"
            binaries.mkdir()
            args_log = work / "cppcheck-args"
            for name, body in (("cppcheck", 'printf "%s\\n" "$@" > "$PFFFT_TEST_CPP_ARGS"\n'),
                               ("cmake", "exit 91\n")):
                program = binaries / name
                program.write_text("#!/usr/bin/env bash\n" + body)
                program.chmod(0o755)
            source = work / "fft"
            (source / "include/pffft").mkdir(parents=True)
            (source / "include/pffft/pffft.h").touch()
            # cppcheck.sh resolves both fetched sources the same way.
            tea = work / "tea"
            tea.mkdir()
            (tea / "tea1.h").touch()
            (work / "src").mkdir()
            (work / "src/example.c").write_text("int example(void) { return 0; }\n")
            script = work / "cppcheck.sh"
            shutil.copyfile(ROOT / "tools/cppcheck.sh", script)
            result = subprocess.run(["bash", str(script), "--strict", "--", "src/example.c"], cwd=work,
                                    env={**os.environ, "PATH": str(binaries) + os.pathsep + os.environ["PATH"],
                                         "DSD_PFFFT_SOURCE_DIR": str(source),
                                         "DSD_TETRA_CRYPTO_SOURCE_DIR": str(tea),
                                         "DSD_ANALYSIS_BUILD_DIR": str(work / "unconfigured"),
                                         "PFFFT_TEST_CPP_ARGS": str(args_log)}, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            arguments = args_log.read_text().splitlines()
            self.assertIn(str(source / "include/pffft"), arguments)
            self.assertIn(str(tea), arguments)
            self.assertIn(f"--suppress=*:{tea}/*", arguments)

    def test_sbom_records_exact_source(self):
        pins = metadata.read_pins()
        sbom = metadata.add_spdx({"SPDXID": "SPDXRef-DOCUMENT", "packages": []}, pins)
        package = sbom["packages"][0]
        self.assertEqual(package["versionInfo"], pins["PFFFT_SHA"])
        self.assertIn(metadata.REPO, package["downloadLocation"])
        self.assertEqual(package["checksums"][0]["checksumValue"], pins["PFFFT_ARCHIVE_SHA256"])
        self.assertEqual(sbom["relationships"][0]["relatedSpdxElement"], package["SPDXID"])
        with self.assertRaises(ValueError):
            metadata.add_spdx(sbom, pins)
        json.dumps(sbom)

    def test_aur_sources_and_offline_build(self):
        recipe = ("source=(\"dsd.tar.gz\")\nsha256sums=('original')\n"
                  'build() {\n    cmake -B build -S "$pkgname" \\\n'
                  '        -DCMAKE_INSTALL_PREFIX=/usr \\\n'
                  '        -DBUILD_TESTING=OFF\n}\n')
        pins = metadata.read_pins()
        result = aur.update(recipe, pins)
        self.assertEqual(aur.update(result, pins), result)
        self.assertIn("sha256sums=('original')", result)
        self.assertIn(pins["PFFFT_ARCHIVE_SHA256"], result)
        self.assertIn(pins["PFFFT_SHA"], result)
        self.assertIn('-DFETCHCONTENT_FULLY_DISCONNECTED=ON', result)
        self.assertIn('grep -qx "PFFFT_SHA=$_pffft_sha"', result)

    @unittest.skipUnless(sys.platform != "win32" and shutil.which("bash"), "AUR requires a Unix shell")
    def test_aur_transition_and_stale_pin(self):
        pins = metadata.read_pins()
        recipe = ('pkgname=dsd-neo-git\nsource=()\nsha256sums=()\n'
                  'build() {\n    cmake -B build -S "$pkgname" \\\n'
                  '        -DCMAKE_INSTALL_PREFIX=/usr \\\n'
                  '        -DBUILD_TESTING=OFF\n}\n')
        script = 'cmake() { return 0; }\nsrcdir="$PWD"\n' + aur.update(recipe, pins) + '\nbuild\n'
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'dsd-neo-git'
            (source / 'cmake').mkdir(parents=True)
            (source / 'tools').mkdir()
            # Published pre-migration sources remain buildable with the new recipe.
            result = subprocess.run(['bash', '-c', script], cwd=directory, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            (source / 'cmake' / 'Pffft.cmake').touch()
            pinfile = source / 'tools' / 'ci-dependency-pins.env'
            pinfile.write_text('PFFFT_SHA=' + pins['PFFFT_SHA'] + '\n')
            result = subprocess.run(['bash', '-c', script], cwd=directory, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            pinfile.write_text('PFFFT_SHA=' + '0' * 40 + '\n')
            result = subprocess.run(['bash', '-c', script], cwd=directory, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'PFFFT pin changed', result.stderr)

    def test_https_download_rejects_redirects(self):
        with patch.object(updater.urllib.request, "OpenerDirector") as factory:
            opener = factory.return_value
            response = opener.open.return_value.__enter__.return_value
            response.status = 200
            response.read.return_value = b"archive"
            sha = "1" * 40
            self.assertEqual(updater.download_archive(sha), b"archive")
            opener.open.assert_called_with(
                f"https://codeload.github.com/arancormonk/pffft/tar.gz/{sha}", timeout=120)
            opener.add_handler.assert_called_once()
            handler = opener.add_handler.call_args.args[0]
            self.assertIsInstance(handler, updater.urllib.request.HTTPSHandler)
            response.status = 302
            with self.assertRaisesRegex(OSError, "HTTP 302"):
                updater.download_archive(sha)

    def test_update_and_download_failure(self):
        sha = "1" * 40
        data = io.BytesIO()
        with tarfile.open(fileobj=data, mode="w:gz") as archive:
            for path in ("src/pffft.c", "src/pffft_common.c", "include/pffft/pffft.h", "LICENSE.txt"):
                entry = tarfile.TarInfo(f"pffft-{sha}/{path}")
                archive.addfile(entry, io.BytesIO())
        payload = data.getvalue()
        with tempfile.TemporaryDirectory() as directory:
            pins = Path(directory) / "pins.env"
            pins.write_text("OTHER=keep\nPFFFT_SHA=" + "0" * 40 + "\nPFFFT_ARCHIVE_SHA256=" + "0" * 64 + "\n")
            with patch.object(updater, "download_archive", return_value=payload):
                updater.update_pins(sha, pins)
            self.assertEqual(metadata.read_pins(pins), {
                "PFFFT_SHA": sha, "PFFFT_ARCHIVE_SHA256": hashlib.sha256(payload).hexdigest(),
            })
            before = pins.read_bytes()
            self.assertIn(b"OTHER=keep", before)
            with patch.object(updater, "download_archive", side_effect=OSError("offline")):
                with self.assertRaises(OSError):
                    updater.update_pins("2" * 40, pins)
            self.assertEqual(pins.read_bytes(), before)
            with self.assertRaises(ValueError):
                updater.update_pins("master", pins)
            self.assertEqual(pins.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
