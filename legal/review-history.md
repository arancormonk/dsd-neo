<!-- SPDX-License-Identifier: GPL-3.0-or-later -->
# Commit and pull request review

Reviewed 2026-10-17. The baseline is `de4212ad1`; the accompanying changes
prepare 2.11.1. The links below identify the evidence, not legal approvals.

| Change | Review result for DSD-neo |
| --- | --- |
| [mbelib #135](https://github.com/arancormonk/mbelib-neo/pull/135), `2b69ade768095ea25e94abe1d4f2bb429fcd2f0c` | 2.6.0 replaces disputed implementation portions, records component provenance, adds full source delivery and conservative SBOMs. Public ABI 2 is retained. Its independent-implementation procedure belongs to mbelib; this review does not claim that procedure for DSD code. Its ETSI and patent questions remain open downstream. |
| [mbelib #136](https://github.com/arancormonk/mbelib-neo/pull/136), `bd11ff5` | Skipped PR classifier propagated through implicit success conditions. Apply explicit status functions and dependency-result checks to DSD's downstream classified jobs. Its signed-source/reviewed-builder recovery design is relevant if DSD adds manual recovery, but should not be copied without DSD-specific source/attestation validation. |
| [mbelib #137](https://github.com/arancormonk/mbelib-neo/pull/137), `e2a7180` | Manual dispatch canceled push CI because both used one workflow group. Add event identity to DSD workflow groups. DSD has no validated recovery input today; tag publishing continues to require a newly created signed tag push. Rerunning the original run is distinct from dispatching a branch. |
| [Initial import](https://github.com/arancormonk/dsd-neo/commit/300644e92) | BCH's restricted notice predates this preparation. The present ISC identifier is not a replacement for that notice. Historical binaries/source remain affected until assessed independently. |
| [GPLv3 and D-STAR rewrite](https://github.com/arancormonk/dsd-neo/commit/3d97b37de99462b5bef8ca5dec9035e513147fa2), 2025-11-20 UTC | Project changed to GPL-3.0-or-later and replaced the GPLv2-only D-STAR header utilities. This addresses that named integration, not every imported file. No associated PR was returned for this direct commit. |
| [Notice follow-up](https://github.com/arancormonk/dsd-neo/commit/c58ca51aac38342e1ab1b3c85565bf8a616405f8) | Retained ezpwd's mixed GPL/LGPL notices. Root-license changes do not supersede those component terms. |
| [DSD #288](https://github.com/arancormonk/dsd-neo/pull/288), [#699](https://github.com/arancormonk/dsd-neo/pull/699), [#707](https://github.com/arancormonk/dsd-neo/pull/707) | Real I/Q fixtures and derivatives improve decoding coverage; technical reproducibility and URLs do not settle recording redistribution. Keep the fixture-permission finding open. |
| [DSD #296](https://github.com/arancormonk/dsd-neo/pull/296), [#505](https://github.com/arancormonk/dsd-neo/pull/505) | Android and Airspy add bundled library/asset obligations. Airspy has retained source-header copyrights and BSD/MIT texts. Android also distributes static mbelib, USB/RTL libraries, fonts and Qt libraries. |
| [DSD #585](https://github.com/arancormonk/dsd-neo/pull/585), [#690](https://github.com/arancormonk/dsd-neo/pull/690), [#711](https://github.com/arancormonk/dsd-neo/pull/711) | TETRA crypto and PFFFT are fetched source dependencies. #711 removes the external speech-codec loader and links mbelib's TETRA decoder on every build. This imports the mbelib table/IPR questions, despite DSD carrying no ACELP decoder source of its own. |
| [DSD #713](https://github.com/arancormonk/dsd-neo/pull/713) | Prepared 2.11.0 and the Semgrep security dependency update. #711's merged tree already pinned mbelib 2.5.1 (its opening PR description still said 2.5.0). Update both CMake/vcpkg project versions and the shared/overlay mbelib pins; Android derives its version from CMake/Git. |
| [DSD #714](https://github.com/arancormonk/dsd-neo/pull/714), [#715](https://github.com/arancormonk/dsd-neo/pull/715) | AUR serialization and the 18-slot CI budget are already present. Preserve those, required check names and classifier fallback behavior while fixing downstream conditions and event cancellation. |

The mbelib PR discussion and review records disclose the absence of separate
human review. No human or legal review is invented for this pass. The raw PR
records and local validation logs are retained outside the tracked source tree.

Historical backfill must use each release's original Git tree and original
dependency revisions. Supplying 2.11.1 source or mbelib 2.6.0 source cannot
substitute for the source of an earlier binary. This preparation does not
change existing releases or contact rights holders.

## Local verification

- Configured and built `dev-debug` against the installed mbelib-neo 2.6.0
  package; all 1,111 CTest cases passed, including I/Q replay and the legal,
  dependency, packaging and CI regression tests.
- Built the Qt frontend and ran `UI_QT_CONTROLLER` successfully. No QML
  behavior or decoder algorithm was changed in this preparation.
- Installed the `legal` CMake component and compared all 15 mbelib notice
  files and the project license texts byte for byte with their originals.
  Android staging regressions also verify missing dependency notices fail
  even when a previous staging directory contains those files.
- Validated supplemental Linux, macOS, Windows and Android inventories
  against the SPDX 2.3 schema. These checks validate metadata shape and
  retained uncertainty, not license compatibility or a complete runtime list.

Native Windows/macOS packages and a complete Android APK/AAB were not built
locally. Their workflow checks remain necessary. No remote CI result or human
review is claimed by the local results above.
