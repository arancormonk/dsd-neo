#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep TETRA_crypto pin updates atomic and the released/scanned revision tied to the pin."""

import hashlib
import importlib.util
import io
import json
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


metadata = module("tetra_crypto_metadata")
updater = module("update_tetra_crypto")
aur = module("update_aur_tetra_crypto")
pffft_metadata = module("pffft_metadata")
pffft_aur = module("update_aur_pffft")

RECIPE = ('pkgname=dsd-neo-git\nsource=("dsd.tar.gz")\nsha256sums=(\'original\')\n'
          'build() {\n    cmake -B build -S "$pkgname" \\\n'
          '        -DCMAKE_INSTALL_PREFIX=/usr \\\n'
          '        -DBUILD_TESTING=OFF\n}\n')


class PinTests(unittest.TestCase):
    def test_sbom_records_exact_source(self):
        pins = metadata.read_pins()
        sbom = metadata.add_spdx({"SPDXID": "SPDXRef-DOCUMENT", "packages": []}, pins)
        package = sbom["packages"][0]
        self.assertEqual(package["versionInfo"], pins["TETRA_CRYPTO_SHA"])
        self.assertIn(metadata.REPO, package["downloadLocation"])
        self.assertEqual(package["checksums"][0]["checksumValue"], pins["TETRA_CRYPTO_ARCHIVE_SHA256"])
        self.assertEqual(package["licenseDeclared"], "Apache-2.0")
        self.assertEqual(sbom["relationships"][0]["relatedSpdxElement"], package["SPDXID"])
        with self.assertRaises(ValueError):
            metadata.add_spdx(sbom, pins)
        # Both embedded sources can enrich the same SBOM.
        pffft_metadata.add_spdx(sbom, pffft_metadata.read_pins())
        self.assertEqual(len({package["SPDXID"] for package in sbom["packages"]}), 2)
        json.dumps(sbom)

    def test_pins_reject_the_old_tarball_form(self):
        with tempfile.TemporaryDirectory() as directory:
            pins = Path(directory) / "pins.env"
            pins.write_text("TETRA_CRYPTO_SHA=" + "1" * 40 + "\nTETRA_CRYPTO_TARBALL_SHA512=" + "2" * 128 + "\n")
            with self.assertRaises(ValueError):
                metadata.read_pins(pins)

    def test_aur_sources_and_offline_build(self):
        pins = metadata.read_pins()
        result = aur.update(RECIPE, pins)
        self.assertEqual(aur.update(result, pins), result)
        self.assertIn("sha256sums=('original')", result)
        self.assertIn(pins["TETRA_CRYPTO_ARCHIVE_SHA256"], result)
        self.assertIn(pins["TETRA_CRYPTO_SHA"], result)
        self.assertIn('-DFETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO="$srcdir/TETRA_crypto-$_tetra_crypto_sha"', result)
        self.assertEqual(result.count("-DFETCHCONTENT_FULLY_DISCONNECTED=ON"), 1)
        self.assertIn('grep -qx "TETRA_CRYPTO_SHA=$_tetra_crypto_sha"', result)

    def test_aur_composes_with_the_pffft_helper(self):
        pins = metadata.read_pins()
        fft_pins = pffft_metadata.read_pins()
        for first, second in ((lambda text: pffft_aur.update(text, fft_pins), lambda text: aur.update(text, pins)),
                              (lambda text: aur.update(text, pins), lambda text: pffft_aur.update(text, fft_pins))):
            result = second(first(RECIPE))
            self.assertEqual(second(first(result)), result)
            self.assertEqual(result.count("-DFETCHCONTENT_FULLY_DISCONNECTED=ON"), 1)
            self.assertIn("-DFETCHCONTENT_SOURCE_DIR_DSD_PFFFT=", result)
            self.assertIn("-DFETCHCONTENT_SOURCE_DIR_DSD_TETRA_CRYPTO=", result)
            self.assertEqual(result.count("sha256sums+=("), 2)
            self.assertEqual(result.count("source+=("), 2)

    @unittest.skipUnless(sys.platform != "win32" and shutil.which("bash"), "AUR requires a Unix shell")
    def test_aur_transition_and_stale_pin(self):
        pins = metadata.read_pins()
        recipe = RECIPE.replace('source=("dsd.tar.gz")\nsha256sums=(\'original\')', 'source=()\nsha256sums=()')
        script = 'cmake() { return 0; }\nsrcdir="$PWD"\n' + aur.update(recipe, pins) + '\nbuild\n'
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'dsd-neo-git'
            (source / 'cmake').mkdir(parents=True)
            (source / 'tools').mkdir()
            # Sources from before the embedded TETRA_crypto remain buildable with the new recipe.
            result = subprocess.run(['bash', '-c', script], cwd=directory, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            (source / 'cmake' / 'TetraCrypto.cmake').touch()
            pinfile = source / 'tools' / 'ci-dependency-pins.env'
            pinfile.write_text('TETRA_CRYPTO_SHA=' + pins['TETRA_CRYPTO_SHA'] + '\n')
            result = subprocess.run(['bash', '-c', script], cwd=directory, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            pinfile.write_text('TETRA_CRYPTO_SHA=' + '0' * 40 + '\n')
            result = subprocess.run(['bash', '-c', script], cwd=directory, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn(b'TETRA_crypto pin changed', result.stderr)

    def test_https_download_rejects_redirects(self):
        with patch.object(updater.urllib.request, "OpenerDirector") as factory:
            opener = factory.return_value
            response = opener.open.return_value.__enter__.return_value
            response.status = 200
            response.read.return_value = b"archive"
            sha = "1" * 40
            self.assertEqual(updater.download_archive(sha), b"archive")
            opener.open.assert_called_with(
                f"https://codeload.github.com/MidnightBlueLabs/TETRA_crypto/tar.gz/{sha}", timeout=120)
            opener.add_handler.assert_called_once()
            handler = opener.add_handler.call_args.args[0]
            self.assertIsInstance(handler, updater.urllib.request.HTTPSHandler)
            response.status = 302
            with self.assertRaisesRegex(OSError, "HTTP 302"):
                updater.download_archive(sha)

    def test_update_and_download_failure(self):
        sha = "1" * 40

        def archive_with(revision, paths):
            data = io.BytesIO()
            with tarfile.open(fileobj=data, mode="w:gz") as archive:
                for path in paths:
                    entry = tarfile.TarInfo(f"TETRA_crypto-{revision}/{path}")
                    archive.addfile(entry, io.BytesIO())
            return data.getvalue()

        payload = archive_with(sha, updater.REQUIRED)
        with tempfile.TemporaryDirectory() as directory:
            pins = Path(directory) / "pins.env"
            pins.write_text("OTHER=keep\nTETRA_CRYPTO_SHA=" + "0" * 40 + "\nTETRA_CRYPTO_ARCHIVE_SHA256=" + "0" * 64
                            + "\n")
            with patch.object(updater, "download_archive", return_value=payload):
                updater.update_pins(sha, pins)
            self.assertEqual(metadata.read_pins(pins), {
                "TETRA_CRYPTO_SHA": sha, "TETRA_CRYPTO_ARCHIVE_SHA256": hashlib.sha256(payload).hexdigest(),
            })
            before = pins.read_bytes()
            self.assertIn(b"OTHER=keep", before)
            with patch.object(updater, "download_archive", side_effect=OSError("offline")):
                with self.assertRaises(OSError):
                    updater.update_pins("2" * 40, pins)
            self.assertEqual(pins.read_bytes(), before)
            incomplete = archive_with("3" * 40, updater.REQUIRED[1:])
            with patch.object(updater, "download_archive", return_value=incomplete):
                with self.assertRaises(KeyError):
                    updater.update_pins("3" * 40, pins)
            self.assertEqual(pins.read_bytes(), before)
            with self.assertRaises(ValueError):
                updater.update_pins("master", pins)
            self.assertEqual(pins.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
