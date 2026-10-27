<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Source delivery and release evidence

Source delivery is an open finding for the portable binary distributions.
GitHub's automatic source archive covers this repository's tracked tree; it
does not include dependencies fetched while building. A source link or SBOM
alone does not demonstrate delivery of complete corresponding source.

For each release asset, preserve the following together:

1. The exact signed DSD tag/commit, original tracked bytes and build scripts.
2. Original PFFFT and TETRA_crypto archives matching the release's checksums,
   and the exact mbelib source plus its PFFFT source. mbelib 2.6.5 supplies a
   [source archive](https://github.com/arancormonk/mbelib-neo/releases/tag/v2.6.5);
   that archive covers mbelib, not the whole DSD application.
3. The sources, modifications, notices and build options for libraries actually
   bundled in the asset. Android's USB/RTL/Airspy snapshots and patch are in
   the tracked tree, while its Qt kit and other installed dependencies need
   separate records. Windows uses the selected vcpkg baseline/ports; macOS
   packages Homebrew libraries; AppImage copies its built runtime closure.
4. The actual binary dependency/SBOM inventory, with any claimed system-library
   exclusions justified per platform. Do not assume every shared library is
   excluded merely because it was installed by a package manager.
5. A fresh rebuild from the delivered sources with networking disabled, and
   the relevant install/relink verification. For distributions subject to
   installation-information requirements, determine and supply the necessary
   information. Android signing and store distribution require their own
   assessment; retaining private signing keys is not a substitute for it.

GPLv3's `Corresponding Source` definition and section 6 are in the repository
`LICENSE`. Applicable LGPL, Qt and other component obligations also need to be
evaluated against the actual distribution method. This document records work
needed to close the finding; it does not assert that an archive, written offer,
relink kit or store compliance review has already been supplied.

## Reference recordings

`THIRD_PARTY.md` and `tools/build_iq_fixtures.py` record origins and pinned
hashes. Permission remains unknown for the Signal Identification Wiki-derived
recordings. Existing tracked recordings therefore also occur in automatic Git
source archives. Excluding a future test corpus from a custom archive would
not remove it from Git history or establish rights for earlier distributions.

The GopherTrunk TETRA fixtures have an adjacent Apache notice and provenance
record; telive and M17 samples identify upstream project licenses, which still
need asset-specific confirmation when recordings may have separate rights.
The issue #348 contribution records an intended regression-test use without a
separate, retained general redistribution grant. Synthetic fixtures should be
distinguished from recordings, and missing permission should remain unknown.
Do not add private audit captures, DVSI/ETSI reference archives, generated
listening packs or downloaded speech corpora to release artifacts without
reviewing their own terms.
