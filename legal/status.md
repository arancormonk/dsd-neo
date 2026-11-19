<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Licensing review for DSD-neo 2.12.1

Version 2.12.1 carries forward the 2.11.1 findings below. It pins mbelib-neo
**2.6.11**, commit `ec8f7d1353d2e234daf61790402866d13a5fd706`, and PFFFT
`b7ed3cdb8655426a77aa26f8c0f5154205427f45`, also embedded by that mbelib release.
The updated dependency notices are packaged; the unresolved findings remain open.

Originally reviewed 2026-10-17 from `de4212ad1` for preparation of 2.11.1, with
mbelib-neo **2.6.0**, commit `2b69ade768095ea25e94abe1d4f2bb429fcd2f0c`.
This is an engineering review of licensing evidence, distribution and CI.
It is **not legal clearance**. The project grants GPL-3.0-or-later for its
contributions; that grant does not establish permission for every incorporated
component, recording or patent. Aggregate license conclusions remain
`NOASSERTION`.

The [component inventory](components.json) separates declared licenses,
assessments, distribution and outstanding questions. It supplements the binary
scanner's inventory; it is not a complete inventory of every platform's
transitive runtime dependencies. [Review history](review-history.md) records
the commits and PRs examined. No historical release, tag, asset or Git history
was rewritten, and this review does not authorize publication.

## Findings requiring resolution

| Finding | Evidence and consequence | Required next evidence or remedy |
| --- | --- | --- |
| BCH-NONCOMMERCIAL | `src/protocol/edacs/edacs-bch3.c` includes Robert Morelos-Zaragoza's noncommercial restriction and requires written permission for commercial use. Its former sole ISC identifier did not describe those terms. The original notice is retained and the identifier now includes `LicenseRef-BCH3-Noncommercial`. | Obtain a compatible grant from the relevant rights holders or replace the implementation using an independently documented basis. Relabeling the file does not resolve the restriction. |
| GOLAY-PERMISSION | `include/dsd-neo/fec/Golay24.hpp` names Hank Wallace's article and incorporates its implementation. The reviewed article identifies copyright but supplies no explicit redistribution grant. The repository's ISC identifier is not evidence of a grant by that author. | Establish the applicable grant and retain it, or replace the implementation. Assessment stays `NOASSERTION`; no public-domain inference is made from publication on a website. |
| MIXED-PROVENANCE | `src/fec/ez.cpp`, `src/protocol/p25/phase2/p25p2_frame.c`, `src/protocol/p25/p25_crc.c`, `src/protocol/ysf/ysf.c` have ISC identifiers but identify other origins. `src/core/util/dsd_misc.c` contains an explicit GPL-3.0-or-later OP25 notice; its identifier now preserves both licenses. | Trace the remaining fragments to exact upstream revisions, retain their notices and reconcile file identifiers. GPLv3 OP25 material can be combined with this project's GPLv3 code; the unsupported ISC-only characterization still needs correction. |
| MBELIB-ETSI | mbelib-neo 2.6.11 retains TETRA ACELP table data with unresolved copyright permission and GPL compatibility under its `ETSI-RETAIN-1` disclosure. Its GPLv2-or-later project grant and conformance tests do not resolve those data rights. | Follow the pinned dependency's [status](https://github.com/arancormonk/mbelib-neo/blob/v2.6.11/legal/status.md). Carry its notices and `NOASSERTION` conclusions downstream. No new exception or permission is inferred here. |
| FIXTURE-PERMISSION | Signal Identification Wiki recordings and derivatives are tracked under `tests/fixtures/iq`; no explicit redistribution license has been established. Attribution, short duration and off-air origin alone do not establish permission. Other contributed audio needs asset-specific evidence as well. | Obtain permission for the particular recordings, replace them with synthetic or expressly licensed fixtures, or remove them from public distribution. Keep acquisition URLs and hashes, but do not treat those as authorization. |
| CORRESPONDING-SOURCE | Current GitHub source snapshots omit downloaded PFFFT and TETRA_crypto source. Portable binaries also bundle mbelib and platform-dependent libraries. No verified, complete corresponding-source delivery for each binary is established by the current workflow. | Assemble the exact source, modifications, build scripts and applicable installation information for each distributed binary; verify a fresh offline rebuild and provide the source through an appropriate distribution method. See [source delivery](source-delivery.md). |
| RUNTIME-NOTICES | The existing license-file checklist covers only selected dependencies. Copying mbelib's notices fixes that omission, but does not establish notice/source coverage for every Homebrew, vcpkg, AppImage or Qt runtime library. The Windows ZIP's SoapySDR load path is now recorded with its notices: SoapySDR and SoapySDRUtil (also SoapySDR for the AppImage), the SoapyAirspy and SoapyHackRF modules, libhackrf, and the libusb and PThreads4W the radio libraries load. Every file the Windows ZIP carries in `bin\` and `lib\` now has a component record (`windows_release`) naming the notices the ZIP carries for it, and the release job refuses a file without one or a tree without those notices. The libraries vcpkg builds for the ZIP are reviewed against their sources (see Resolved findings), and codec2 is recorded for the APK and the AppImage as well. The APK's statically linked vcpkg libraries (OpenSSL's libcrypto and libssl, libcurl, zlib, Expat, libsndfile, libFLAC, libogg, libvorbis, Opus, LAME and mpg123, from the same baseline) are recorded for Android with the notices the APK carries. Every package that carries librtlsdr (the ZIP, the AppImage, the DMG and the APK) carries `licenses/librtlsdr-NOTICES.txt`, the holder lines of its compiled sources, and the APK carries the same for its libusb (`licenses/libusb-android-NOTICES.txt`). Not recorded yet: the AppImage's other bundled libraries (Ubuntu 20.04 packages), the macOS DMG's Homebrew libraries, the APK's Qt 6.11.3 libraries, plugins and QML imports (and the third-party code inside them), the NDK's `libc++_shared.so`, and the AndroidX, Kotlin and Qt Java code in the APK's `classes*.dex`. | Compare each other final binary's actual dependency closure and SBOM with its notice and source inventory, including Qt modules/plugins and their selected license terms. |

These findings remain open. The audit checks enforce faithful records and
notice delivery, not a release waiver. In particular, BCH-NONCOMMERCIAL is an
express restriction, not merely a missing attribution. Resolve the relevant
rights and distribution issues before claiming GPL-compliant redistribution.

## Resolved findings

| Finding | Resolution | Accepted risk |
| --- | --- | --- |
| MSVC-RUNTIME | Resolved 2026-11-18 by policy. The Windows ZIP keeps carrying Microsoft's Visual C++ runtime DLLs in `bin\`, copied unmodified only from the `VC\Redist` runtime folder of the toolset that built the release, never from System32. [msvc-runtime.md](msvc-runtime.md) quotes the Distributable Code section of the Visual Studio license terms and its REDIST list, and records how the release meets each condition: an end-user notice of protective terms for these files alone (`licenses/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`, which the release job requires whenever the ZIP carries one of them), no Microsoft trademark in the program's name, and the indemnity of the Visual Studio 2026 terms, which the maintainer accepts. The release relies on the maintainer's Visual Studio Community licence, with the hosted runner as a Build Device hosted on Microsoft Azure. | The FSF GPL FAQ (`#WindowsRuntimeAndGPL`) says these DLLs may not be distributed with a GPL program; the GPLv3 text (sections 1 and 6) and the FSF's 2006 drafting rationale support doing so. Other copyright holders whose GPL code dsd-neo contains could rely on the FAQ. Not verified: the licensing of the runner's preinstalled Visual Studio Enterprise, that its runtime files match a Community installation's, and what makes end users of a ZIP "agree to terms". The fallback is to stop carrying the DLLs and require Microsoft's Visual C++ Redistributable. |
| RUNTIME-NOTICES (Windows vcpkg libraries) | Resolved 2026-11-18 for the libraries vcpkg builds for the Windows ZIP: codec2, PortAudio, PDCurses, OpenSSL's libcrypto, libcurl, Expat, zlib, libsndfile and the libFLAC, libogg, libvorbis, Opus, LAME and mpg123 libraries it loads. Each record now gives the license expression of the sources its DLL is compiled from, in the archive its port downloads (checked against the port's SHA512), rather than the port's metadata, and the ZIP carries the notices and holders those sources name that the port's copyright file lacks: Mark Borgerding's and Mozilla's BSD notices in codec2, the GSM 06.10, ALAC (Apache-2.0) and Sun G.72x terms and the Xiph.Org notices in libsndfile, the Cisco, Google, Arm and other holders of Opus, OpenSSL's AUTHORS.md, its holders (Nokia's and Siemens AG's CMP, CRMF and HTTP client code and Patrick Powell's `bio_print.c` among them, added 2026-11-18 to `LICENSES/OpenSSL-NOTICES.txt`, which every package that carries OpenSSL 3.6.5 shares) and KISA's SEED terms, and the like. codec2's varicode files grant "the GNU General Public License version 2.1", which does not exist; they are recorded as the LGPL 2.1 of codec2's COPYING and README, and `LICENSES/LicenseRef-codec2-varicode.txt` reproduces the grant unmodified with a note stating that reading (a note, not a separate license: no LicenseRef enters the expression). The APK and the AppImage, which carry codec2 from the same commit, carry its notices too. The PortAudio overlay builds without ASIO. | The compiled-file lists come from the ports' options and the upstream build files, not from a Windows build log; the same holds for LAME linking its mpglib decoder. The varicode reading has no statement from upstream. PDCurses' public-domain dedication may not take effect everywhere, and upstream gives no fallback license. libmp3lame's `fft.c` names no license of its own and is taken as covered by LAME's LGPL. No patent statement is cleared. |

## Remediation in this preparation

- Pin the signed mbelib-neo v2.6.11 source and its independently downloaded
  archive checksum in CI and the Windows overlay. Require 2.6 or later so
  builds use the implementation remediation and installed legal records.
- Retain full ISC, Unlicense, gqrx-scanner MIT and custom Rockliff/BCH notices, and correct the
  four directly evidenced SPDX mismatches without removing inherited notices.
- Install the component findings and the selected mbelib installation's
  complete notice directory. Carry these through desktop packages, Android
  assets and the Qt notices reader. The overlay's copyright file includes
  mbelib's ISC, PFFFT and ETSI notices and its unresolved disclosures.
- Add curated component records to binary SBOMs before upload/attestation,
  preserving `NOASSERTION` for unresolved components and the combined artifact.
- Record the Windows ZIP's SoapySDR load path (SoapySDR, SoapySDRUtil, two
  device modules, libhackrf, libusb and PThreads4W) in the component
  inventory, and ship each one's notice in `licenses/`; the Windows
  release job refuses a tree that lacks one. SoapySDR's BSL-1.0 text is
  `LICENSES/BSL-1.0.txt`. Corresponding source for the LGPL libusb build stays
  under CORRESPONDING-SOURCE.
- Give every file the Windows ZIP carries in `bin\` and `lib\` a component
  record that names the notices the ZIP carries for it and where staging
  takes each one from (`windows_release` in the component inventory). The
  staging copies the notices of every file it stages, and the release job
  refuses a file no record lists or a tree without its notices.
- Review each library vcpkg builds for the Windows ZIP against the sources in
  the archive its port pins, record the license expression those sources
  carry, and ship, beside the copyright file the port installs, the notices
  and holders that file lacks (`LICENSES/` and `packaging/windows/`). Record
  codec2 for the APK and the AppImage too, and ship its notices there. Record
  the eleven vcpkg libraries the APK links statically (OpenSSL with libssl,
  libcurl, zlib, Expat, libsndfile, libFLAC, libogg, libvorbis, Opus, LAME
  and mpg123) for Android as well, and ship their notices in the APK. Build
  PortAudio without ASIO, and pin its overlay's source in
  `tools/ci-dependency-pins.env` like the other overlays.
- Record the Visual C++ runtime DLLs the Windows ZIP carries (`msvc-runtime`):
  the files it may carry and Microsoft's REDIST lists. The release job copies
  them only from the toolset's `VC\Redist` runtime folder, never from the
  build machine's System32, and refuses a runtime DLL the record does not
  list. [msvc-runtime.md](msvc-runtime.md) records Microsoft's terms for them,
  how the release meets each condition, its licence basis and the accepted
  GPL risk; the ZIP carries an end-user notice of those terms with the DLLs.
- Keep vcpkg's MIT license and Microsoft's notice beside the overlays copied
  from its ports (`soapysdr`, `rtlsdr`, `portaudio`) as
  `vcpkg-ports/LICENSE-vcpkg.txt`; each copied portfile names its source port
  and the dsd-neo changes. No vcpkg code reaches a release package.
- Separate workflow concurrency by event and explicitly gate downstream
  Android, nightly and AUR jobs on successful prerequisites plus
  `!cancelled()`, so a skipped PR-only classifier does not suppress publication.
  This keeps the existing runner budget and signed-tag validation.

## Patent assessment

The mbelib [patent research](https://github.com/arancormonk/mbelib-neo/blob/v2.6.11/legal/patents.md)
flags US 8,359,197 (including decoder/tone claims), US 8,595,002, regenerated
phase and voicing-smoothing references, and ETSI TETRA declarations. This
decoder invokes the reviewed AMBE+2 and TETRA paths. Its separate FEC, framing
and parameter transport code also needs claim-specific review; being a decoder
does not establish non-infringement. No claim construction, official-register
status verification, exhaustive search or jurisdiction-specific clearance was
performed here. No educational, research, local-use or patent-age exemption is
asserted. A predicted expiration date does not close a finding.

ETSI distinguishes copyright authorization from patent licensing in its
[IPR guidance](https://www.etsi.org/resources/intellectual-property-rights/).
Keep both questions separate. Rights-holder correspondence or qualified legal
advice is needed for conclusions this engineering evidence cannot establish.

## Sources and limits

- Repository file notices and the commit/PR record listed in
  [review-history.md](review-history.md).
- [Morelos-Zaragoza's ECC page](https://www.eccpage.com/) and the retained
  BCH program notice. No separate commercial permission was found in this repo.
- [Hank Wallace's Golay article](https://aqdi.com/articles/using-the-golay-error-detection-and-correction-code-3/),
  reviewed 2026-10-17; current publication does not establish historical terms.
- [gqrx-scanner's MIT license](https://github.com/neural75/gqrx-scanner/blob/88c8999d5559211dc655d580940b0e917488c334/LICENSE),
  present since the initial 2017 commit; retained for the credited rigctl portions.
- [Signal Identification Wiki rights metadata](https://www.sigidwiki.com/api.php?action=query&meta=siteinfo&siprop=rightsinfo&format=json),
  retrieved 2026-10-17: both rights URL and rights text are empty.
- Microsoft's [Visual Studio Redistribution](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution)
  (2026 and later) and [Visual Studio 2022 Redistribution](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)
  REDIST lists and its [license terms directory](https://visualstudio.microsoft.com/license-terms/),
  retrieved 2026-11-18, with the license documents those pages embed; the
  sources [msvc-runtime.md](msvc-runtime.md) lists.
- The repository's `LICENSE`, especially GPLv3 sections 1, 5, 6 and 10,
  for corresponding source, notices and additional restrictions.
- The [mbelib release](https://github.com/arancormonk/mbelib-neo/releases/tag/v2.6.11)
  and its pinned component, reference-data and patent records.

This review checked current-tree notices, selected provenance history and
release paths. It did not certify authorship of all 2,528 baseline tracked
files or every prior release. DCO sign-offs, tests and code review are useful
process evidence, not independent grants from upstream rights holders.
