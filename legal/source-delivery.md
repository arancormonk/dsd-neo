<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Source delivery and release evidence

Source delivery is an open finding for the portable binary distributions.
GitHub's automatic source archive covers this repository's tracked tree; it
does not include dependencies fetched while building. A source link or SBOM
alone does not demonstrate delivery of complete corresponding source.

For each release asset, preserve the following together:

1. The exact signed DSD tag/commit, original tracked bytes and build scripts.
2. Original PFFFT and TETRA_crypto archives matching the release's checksums,
   and the exact mbelib source plus its PFFFT source. mbelib 2.6.11 supplies a
   [source archive](https://github.com/arancormonk/mbelib-neo/releases/tag/v2.6.11);
   that archive covers mbelib, not the whole DSD application.
3. The sources, modifications, notices and build options for libraries actually
   bundled in the asset. Android's USB/RTL/Airspy snapshots and patch are in
   the tracked tree; its Qt kit (the qtbase, qtdeclarative and qtsvg 6.11.3
   archives, with their SHA-256 in the `qt` record), its NDK C++ runtime and
   its Maven libraries are recorded in the component inventory. Qt's source
   is provided by directions under GPLv3 section 6(d), the manner LGPL 3
   section 4(d)(0) points to, not by a copy: every APK the release workflow
   publishes has `<APK name>.qt-source.txt` beside it, and a tagged release's
   notes repeat it. The file names the Qt version the build installed, the
   official qtbase, qtdeclarative and qtsvg source archives of that version on
   download.qt.io (dsd-neo applies no patches to Qt, so there is no patch set
   to add) and the source form of the Public Suffix List that Qt Network
   compiles in, at the upstream commit Qt's records name.
   `tools/android_source_directions.py` writes it from the build's own Qt kit
   and fails when the kit is another version. The app's licence screen gives
   the same directions. No copy is kept on dsd-neo's servers, so the source's
   availability depends on Qt's and the Public Suffix List's servers; section
   6(d) leaves the obligation to keep it available with dsd-neo as the
   distributor of the APK, and if those archives went away dsd-neo would have
   to provide the source itself. Windows uses the selected vcpkg baseline/ports.
   The macOS DMG uses Homebrew for build tools only: its libraries come from
   vcpkg at the shared `builtin-baseline` in `vcpkg.json`, built with the
   `arm64-osx-dynamic-release` triplet (`vcpkg-triplets/`) and the overlay
   ports in `vcpkg-ports/`, except librtlsdr and libairspy. Those two are
   built from pinned upstream commits (`RTL_SDR_SHA` and `AIRSPY_SHA` in
   `tools/ci-dependency-pins.env`) with dsd-neo's patch series applied
   (`tools/usb_dependency_patches.sh series librtlsdr|libairspy`, patch files
   in `android/third_party/patches/`). AppImage copies its built runtime closure.
4. The actual binary dependency/SBOM inventory, with any claimed system-library
   exclusions justified per platform. Do not assume every shared library is
   excluded merely because it was installed by a package manager.
5. A fresh rebuild from the delivered sources with networking disabled, and
   the relevant install/relink verification. For distributions subject to
   installation-information requirements, determine and supply the necessary
   information. Android signing and store distribution require their own
   assessment; retaining private signing keys is not a substitute for it.
   For the APK, Installation Information (GPLv3 section 6, LGPL 3 section
   4(e)) is read as not required: it applies to object code conveyed in, with
   or for a User Product as part of a transaction transferring the device,
   and an app download transfers no device. Android also installs
   user-signed APKs, so an app rebuilt against a modified Qt can be installed
   (after the official one is uninstalled).

GPLv3's `Corresponding Source` definition and section 6 are in the repository
`LICENSE`. Applicable LGPL, Qt and other component obligations also need to be
evaluated against the actual distribution method. This document records work
needed to close the finding; apart from the Qt source directions in item 3, it
does not assert that an archive, written offer, relink kit or store compliance
review has already been supplied.

## Reference recordings

`THIRD_PARTY.md` and `tools/build_iq_fixtures.py` record origins and pinned
hashes. Permission remains unknown for the Signal Identification Wiki-derived
recordings. Existing tracked recordings therefore also occur in automatic Git
source archives. Excluding a future test corpus from a custom archive would
not remove it from Git history or establish rights for earlier distributions.

The GopherTrunk TETRA and P25 C4FM fixtures have adjacent Apache notices and
provenance records; telive and M17 samples identify upstream project licenses,
which still need asset-specific confirmation when recordings may have separate
rights.
The issue #348 contribution records an intended regression-test use without a
separate, retained general redistribution grant. Synthetic fixtures should be
distinguished from recordings, and missing permission should remain unknown.
Do not add private audit captures, DVSI/ETSI reference archives, generated
listening packs or downloaded speech corpora to release artifacts without
reviewing their own terms.
