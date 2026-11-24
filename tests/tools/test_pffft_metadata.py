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
        self.assertEqual(package["licenseDeclared"], "GPL-2.0-or-later AND LicenseRef-PFFFT")
        extracted = sbom["hasExtractedLicensingInfos"]
        self.assertEqual([record["licenseId"] for record in extracted], ["LicenseRef-PFFFT"])
        self.assertEqual(extracted[0]["extractedText"], (ROOT / "LICENSES/LicenseRef-PFFFT.txt").read_text())
        self.assertEqual(package["name"], "pffft-neo")
        self.assertEqual(metadata.REPO, "https://github.com/arancormonk/pffft-neo")
        self.assertIn(metadata.REPO, package["downloadLocation"])
        self.assertIn("pkg:github/arancormonk/pffft-neo@", package["externalRefs"][0]["referenceLocator"])
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
        self.assertIn("arancormonk/pffft-neo/tar.gz/", result)
        self.assertIn('-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT="$srcdir/pffft-neo-$_pffft_sha"', result)
        # A recipe published for the PFFFT fork moves to pffft-neo's archive and unpack directory.
        published = result.replace("pffft-neo-", "pffft-").replace("pffft-neo/", "pffft/")
        self.assertNotIn("pffft-neo", published)
        self.assertEqual(aur.update(published, pins), result)
        braced = published.replace('pffft-$_pffft_sha" \\', 'pffft-${_pffft_sha}" \\')
        self.assertIn('"$srcdir/pffft-${_pffft_sha}"', braced)
        self.assertIn('"$srcdir/pffft-neo-${_pffft_sha}"', aur.update(braced, pins))
        # An override this helper did not write, and cannot move, is refused rather than kept.
        with self.assertRaisesRegex(ValueError, "override"):
            aur.update(result.replace('"$srcdir/pffft-neo-$_pffft_sha"', '"$srcdir/fft"'), pins)
        # A second override after the right one is not hidden by it.
        doubled = result.replace('"$srcdir/pffft-neo-$_pffft_sha" \\\n',
                                 '"$srcdir/pffft-neo-$_pffft_sha" \\\n        -DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT="$srcdir/fft" \\\n')
        self.assertIn('"$srcdir/fft"', doubled)
        with self.assertRaisesRegex(ValueError, "override"):
            aur.update(doubled, pins)

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
                f"https://codeload.github.com/arancormonk/pffft-neo/tar.gz/{sha}", timeout=120)
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
            # Exactly the files cmake/Pffft.cmake requires.
            for path in updater.required_files():
                entry = tarfile.TarInfo(f"pffft-neo-{sha}/{path}")
                body = b"Retained notices v2\r\n" if path == updater.LICENSEREF_MEMBER else b""
                entry.size = len(body)
                archive.addfile(entry, io.BytesIO(body))
        payload = data.getvalue()

        def without(name):
            # The same archive without one file the build requires.
            data = io.BytesIO()
            with tarfile.open(fileobj=io.BytesIO(payload), mode="r:gz") as source, \
                    tarfile.open(fileobj=data, mode="w:gz") as archive:
                for entry in source.getmembers():
                    if entry.name != f"pffft-neo-{sha}/{name}":
                        archive.addfile(entry, source.extractfile(entry))
            return data.getvalue()
        with tempfile.TemporaryDirectory() as directory:
            # A scratch tree with the three files the updater moves together; the repository's stay untouched.
            root = Path(directory)
            pins, licenseref, inventory = (root / "tools/ci-dependency-pins.env",
                                           root / "LICENSES/LicenseRef-PFFFT.txt", root / "legal/components.json")
            for path in (pins, licenseref, inventory):
                path.parent.mkdir(parents=True)
            pins.write_text("OTHER=keep\nPFFFT_SHA=" + "0" * 40 + "\nPFFFT_ARCHIVE_SHA256=" + "0" * 64 + "\n")
            licenseref.write_text("Retained notices v1\n")
            inventory.write_text('{"notice_sha256": {"LICENSES/LicenseRef-PFFFT.txt": "' + "0" * 64 + '"}}\n')
            with patch.object(updater, "download_archive", return_value=payload):
                updater.update_pins(sha, root)
            self.assertEqual(metadata.read_pins(pins), {
                "PFFFT_SHA": sha, "PFFFT_ARCHIVE_SHA256": hashlib.sha256(payload).hexdigest(),
            })
            self.assertEqual(licenseref.read_bytes(), b"Retained notices v2\r\n")
            # The inventory records the hash over LF line endings, as legal_inventory.py checks it.
            self.assertIn(hashlib.sha256(b"Retained notices v2\n").hexdigest(), inventory.read_text())
            before = [path.read_bytes() for path in (pins, licenseref, inventory)]
            self.assertIn(b"OTHER=keep", before[0])
            for name in ("README.md", "src/pffft_impl.h"):
                with patch.object(updater, "download_archive", return_value=without(name)):
                    with self.assertRaisesRegex(KeyError, name):
                        updater.update_pins(sha, root)
                self.assertEqual([path.read_bytes() for path in (pins, licenseref, inventory)], before)
            with patch.object(updater, "download_archive", side_effect=OSError("offline")):
                with self.assertRaises(OSError):
                    updater.update_pins("2" * 40, root)
            self.assertEqual([path.read_bytes() for path in (pins, licenseref, inventory)], before)
            with self.assertRaises(ValueError):
                updater.update_pins("master", root)
            self.assertEqual([path.read_bytes() for path in (pins, licenseref, inventory)], before)
            # A failure moving the last file (the pins) puts back the two already replaced.
            licenseref.write_bytes(b"Retained notices v1\n")
            before = [path.read_bytes() for path in (pins, licenseref, inventory)]
            real_replace = updater.os.replace
            moves = []

            def failing_replace(source, destination):
                moves.append(destination)
                if Path(destination) == pins:
                    raise OSError("read-only tools/")
                real_replace(source, destination)
            with patch.object(updater, "download_archive", return_value=payload), \
                    patch.object(updater.os, "replace", side_effect=failing_replace):
                with self.assertRaisesRegex(OSError, "read-only"):
                    updater.update_pins(sha, root)
            self.assertEqual([Path(path) for path in moves], [licenseref, inventory, pins])
            self.assertEqual([path.read_bytes() for path in (pins, licenseref, inventory)], before)
            self.assertEqual(sorted(path.name for path in root.rglob("*") if path.is_file()),
                             ["LicenseRef-PFFFT.txt", "ci-dependency-pins.env", "components.json"])


if __name__ == "__main__":
    unittest.main()
