<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Licensing review for DSD-neo 2.11.4

Version 2.11.4 carries forward the 2.11.1 findings below. It pins mbelib-neo
**2.6.5**, commit `77a9a33b93eff910bc2bcade18c1e76fce1b7105`, and PFFFT
`316f0b01e3bdd327e89933891ea2c0be2d10c75e`, also embedded by that mbelib release.
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
| MBELIB-ETSI | mbelib-neo 2.6.5 retains TETRA ACELP table data with unresolved copyright permission and GPL compatibility under its `ETSI-RETAIN-1` disclosure. Its GPLv2-or-later project grant and conformance tests do not resolve those data rights. | Follow the pinned dependency's [status](https://github.com/arancormonk/mbelib-neo/blob/v2.6.5/legal/status.md). Carry its notices and `NOASSERTION` conclusions downstream. No new exception or permission is inferred here. |
| FIXTURE-PERMISSION | Signal Identification Wiki recordings and derivatives are tracked under `tests/fixtures/iq`; no explicit redistribution license has been established. Attribution, short duration and off-air origin alone do not establish permission. Other contributed audio needs asset-specific evidence as well. | Obtain permission for the particular recordings, replace them with synthetic or expressly licensed fixtures, or remove them from public distribution. Keep acquisition URLs and hashes, but do not treat those as authorization. |
| CORRESPONDING-SOURCE | Current GitHub source snapshots omit downloaded PFFFT and TETRA_crypto source. Portable binaries also bundle mbelib and platform-dependent libraries. No verified, complete corresponding-source delivery for each binary is established by the current workflow. | Assemble the exact source, modifications, build scripts and applicable installation information for each distributed binary; verify a fresh offline rebuild and provide the source through an appropriate distribution method. See [source delivery](source-delivery.md). |
| RUNTIME-NOTICES | The existing license-file checklist covers only selected dependencies. Copying mbelib's notices fixes that omission, but does not establish notice/source coverage for every Homebrew, vcpkg, AppImage or Qt runtime library. | Compare each final binary's actual dependency closure and SBOM with its notice and source inventory, including Qt modules/plugins and their selected license terms. |

These findings remain open. The audit checks enforce faithful records and
notice delivery, not a release waiver. In particular, BCH-NONCOMMERCIAL is an
express restriction, not merely a missing attribution. Resolve the relevant
rights and distribution issues before claiming GPL-compliant redistribution.

## Remediation in this preparation

- Pin the signed mbelib-neo v2.6.5 source and its independently downloaded
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
- Separate workflow concurrency by event and explicitly gate downstream
  Android, nightly and AUR jobs on successful prerequisites plus
  `!cancelled()`, so a skipped PR-only classifier does not suppress publication.
  This keeps the existing runner budget and signed-tag validation.

## Patent assessment

The mbelib [patent research](https://github.com/arancormonk/mbelib-neo/blob/v2.6.5/legal/patents.md)
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
- The repository's `LICENSE`, especially GPLv3 sections 1, 5, 6 and 10,
  for corresponding source, notices and additional restrictions.
- The [mbelib release](https://github.com/arancormonk/mbelib-neo/releases/tag/v2.6.5)
  and its pinned component, reference-data and patent records.

This review checked current-tree notices, selected provenance history and
release paths. It did not certify authorship of all 2,528 baseline tracked
files or every prior release. DCO sign-offs, tests and code review are useful
process evidence, not independent grants from upstream rights holders.
