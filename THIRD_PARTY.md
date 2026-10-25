# Third-Party Notices

This file summarizes third-party, vendored, and upstream-derived material known
in the source tree. Per-file headers and embedded notices remain authoritative
for file-specific license/provenance details.

The [licensing review](legal/status.md) records unresolved permission,
compatibility, patent and source-delivery findings. This list is attribution,
not a finding that every component is cleared for redistribution.

## Vendored Libraries

- **mbelib-neo** (linked vocoder; pinned release 2.6.2, GPL-2.0-or-later project
  contributions with ISC/PFFFT components and unresolved ETSI table terms) —
  <https://github.com/arancormonk/mbelib-neo>. Its full installed notices and
  legal records accompany packages in `licenses/mbelib-neo/`. TETRA table
  permission/GPL compatibility and patent questions remain open; do not label
  the entire dependency as cleared GPL code. See its pinned
  [status](https://github.com/arancormonk/mbelib-neo/blob/v2.6.2/legal/status.md).

- **ezpwd Reed-Solomon** (`src/third_party/ezpwd`, target `dsd-neo_ezpwd`; Hard Consulting Corporation; vendored from https://github.com/arancormonk/ezpwd-reed-solomon) - GPL-3.0-or-later; `rs_base` is LGPL-2.1-or-later (per upstream notice). LGPL text: `src/third_party/ezpwd/lesser.txt` (installed as `share/doc/dsd-neo/licenses/ezpwd-LGPL-2.1-or-later.txt`).
- **PFFFT** (target `dsd-neo_pffft`; Julien Pommier and contributors, based on FFTPACKv4 by Dr Paul Swarztrauber/NCAR) — downloaded from <https://github.com/arancormonk/pffft> at the immutable revision and SHA-256 in `tools/ci-dependency-pins.env`. The BSD-like license and compiled backend notices are installed as `share/doc/dsd-neo/licenses/pffft-LICENSE.txt` and `pffft-NOTICES.txt`. See `docs/dependencies.md` for offline builds and updates.
- **TETRA_crypto** (target `dsd-neo_tetra_crypto`; Copyright 2023 Midnight Blue) — the TEA1, TEA2 and TEA3 keystream
  generators (`tea1.c`, `tea2.c`, `tea3.c`), downloaded from <https://github.com/MidnightBlueLabs/TETRA_crypto> at the
  immutable revision and SHA-256 in `tools/ci-dependency-pins.env`. Apache-2.0; the license text is installed as
  `share/doc/dsd-neo/licenses/tetra_crypto-LICENSE.txt`. See `docs/dependencies.md` for offline builds and updates.
  `src/crypto/tetra_ta61.c` is a port of the same revision's TA61 identity encryption and the HURDLE block encryption
  it runs on (`taa1.c`, `hurdle.c`), rewritten byte by byte for portability; it is under the same Apache-2.0 license
  as well as GPL-3.0-or-later, and keeps the Midnight Blue notice.
- **Tiny AES** (`src/crypto/crypt-aes.c`) - Unlicense/public domain; original upstream https://github.com/kokke/tiny-AES-c.
- **librtlsdr** (`android/third_party/librtlsdr`; osmocom, https://gitea.osmocom.org/sdr/rtl-sdr) - GPL-2.0-or-later;
  license text `android/third_party/librtlsdr/COPYING`. Trimmed v2.0.3 snapshot (library sources only) built into the
  Android app; carries two project patches, kept in `android/third_party/patches/`:
  `0001-librtlsdr-add-rtlsdr_open_fd.patch` adds `rtlsdr_open_fd()` for USB-OTG descriptor injection, and
  `0002-librtlsdr-check-async-buffer-allocation.patch` makes the async reader check its transfer allocations and
  return `-ENOMEM` instead of submitting NULL transfers. Not part of any non-Android build.
- **libusb** (`android/third_party/libusb`; https://github.com/libusb/libusb) - LGPL-2.1-or-later; license text
  `android/third_party/libusb/COPYING`. Trimmed v1.0.30 snapshot (the `linux_usbfs` backend and POSIX shims upstream's
  own Android build uses) built into the Android app. Not part of any non-Android build.

## Bundled Assets

- **IBM Plex Sans / IBM Plex Mono** (`src/ui/qt/fonts/IBMPlexSans-*.ttf`, `src/ui/qt/fonts/IBMPlexMono-*.ttf`;
  (c) IBM Corp., https://github.com/IBM/plex) - SIL Open Font License 1.1. License text
  `src/ui/qt/fonts/IBMPlex-LICENSE.txt` (installed as `share/doc/dsd-neo/licenses/ibm-plex-OFL-1.1.txt`).
  Compiled into the Qt Quick frontend's resource bundle, so it is redistributed by any build with `DSD_ENABLE_QT_UI=ON`
  — today that is the Android app.

## Embedded Provenance-Bearing Code

- **EDACS-FME / EDACS helpers** (`src/protocol/edacs/edacs-fme.c`) - source comments credit EDACS-FM, sp5wwp/ledacs,
  XTAL Labs, Robert Morelos-Zaragoza, LWVMOBILE, and ilyacodes.
- **Binary BCH v3.1** (`src/protocol/edacs/edacs-bch3.c`) - source comments credit Robert Morelos-Zaragoza and note
  portions from Simon Rockliff's Reed-Solomon code. The file also contains a non-commercial/commercial-permission notice
  incompatible with an unrestricted ISC characterization. Its identifier now
  includes `LicenseRef-BCH3-Noncommercial`; the full notice is retained in
  `LICENSES/LicenseRef-BCH3-Noncommercial.txt`. Permission/GPL compatibility
  remains unresolved (BCH-NONCOMMERCIAL in the review).
- **NXDN OP25-derived tables and helpers** (`include/dsd-neo/protocol/nxdn/nxdn_const.h`,
  `src/protocol/nxdn/nxdn_frame.c`, `src/protocol/nxdn/nxdn_deperm.c`) - source comments credit Osmocom OP25, Max H.
  Parke, and Mathias Weyland under GPL notices.
- **NXDN convolution helper** (`src/protocol/nxdn/nxdn_convolution.c`) - source comments credit Jonathan Naylor,
  Edouard Griffiths, and Louis HERVE under a GPL-2.0-or-later notice.
- **FEC helpers outside `src/third_party`** (`include/dsd-neo/fec/ReedSolomon.hpp`,
  `include/dsd-neo/fec/Golay24.hpp`, `src/fec/fec.c`) - source comments credit Simon Rockliff, Hank Wallace, and
  Edouard Griffiths respectively. Rockliff's custom permission notice is retained
  as `LICENSES/LicenseRef-Simon-Rockliff.txt`, and the RS identifier includes it.
  Redistribution permission for Wallace's Golay implementation is unresolved;
  its ISC header alone does not establish that permission.
- **OP25/GNU Radio/Yair Linn-derived DSP and protocol snippets** (`src/dsp/costas.cpp`, `src/dsp/ted.cpp`,
  `src/protocol/p25/phase2/p25p2_frame.c`, `src/protocol/p25/p25_crc.c`) - source comments identify OP25, GNU Radio,
  KA1RBI, Free Software Foundation, and Yair Linn provenance for selected algorithms/tables.
- **Miscellaneous embedded provenance** (`src/core/util/dsd_misc.c`, `src/io/control/dsd_rigctl.c`,
  `src/protocol/ysf/ysf.c`) - source comments identify OP25, libM17, NedSimao/FilteringLibrary, gqrx-scanner, DSDcc,
  gr-ysf, and Munaut-derived code or ideas.
  The rigctl portions credited to neural75's gqrx-scanner retain its MIT license
  in `LICENSES/gqrx-scanner-MIT.txt`; the rigctl identifier includes both ISC and MIT.

## Test Fixtures

- **I/Q decode fixtures** (`tests/fixtures/iq/*.iq`) - derived from public sample recordings, converted to cu8
  baseband by `tools/build_iq_fixtures.py`, which records the upstream URL for every fixture.
  - Most fixtures derive from user-contributed off-air sample recordings hosted on the
    [Signal Identification Wiki](https://www.sigidwiki.com/). The wiki does not publish an explicit content license
    (its MediaWiki rights metadata is empty), so no CC compatibility claim is made; the fixtures are short excerpts
    of off-air radio transmissions redistributed for testing with attribution to Signal Identification Wiki
    contributors, with per-fixture upstream URLs recorded in `tools/build_iq_fixtures.py`. If upstream licensing is
    clarified, update this entry.
    **Review finding:** these facts do not establish permission to redistribute
    the recordings. Permission remains unknown; see `legal/source-delivery.md`.
  - The `provoice` fixture is the wiki's `Provoice.mp3` receiver-audio recording of one ProVoice transmission,
    with the same unresolved permission finding. `tools/build_iq_fixtures.py` records its URL and pinned SHA-256, and undoes the
    receiver's de-emphasis before converting it to baseband.
  - The analog fixtures `am_airband_real`, `nfm_ctcss_real`, `nfm_squelch_real_a` and `nfm_squelch_real_b` are short
    excerpts of the wiki's `AM_IQ.zip`, `IQ_CTCSS_example_482768kHz_IQ.zip` and `Unknown_NFM_squelch_IQ.zip` I/Q
    recordings, with the same unresolved permission finding as the other wiki-derived fixtures. `tools/build_iq_fixtures.py`
    records their URLs and pinned SHA-256 hashes. `nfm_tone_synth` and `nfm_adjacent_synth` are generated by that
    script from a seeded tone and noise and contain no recorded material.
  - The `tetra_call_real_air_72k` fixture is a 6.5-second excerpt of the wiki's `TETRA_IQ.zip` I/Q recording of a
    TETRA downlink carrier, resampled to 72 kHz and subject to the same unresolved permission finding as the other wiki-derived
    fixtures. `tools/build_iq_fixtures.py` records its URL and pinned SHA-256 hash.
  - The `nxdn48_after_retune` fixture is the `nxdn48` excerpt of the wiki's `NXDN_IQ.zip`, preceded by a lead-in cut
    from the seeded `noise_floor` fixture (no recorded material) and joined byte for byte by
    `tools/build_iq_fixtures.py`. Its recorded portion has the same unresolved permission finding as `nxdn48`.
  - The `ysf_gap` fixture is a sidecar alone, written by `tools/build_iq_fixtures.py`, that replays the `ysf` data
    file with a capture gap added. It carries no recorded material of its own.
  - The M17 fixture derives from `samples/m17_clear_voice_wav.wav` in [lwvmobile/m17-fme](https://github.com/lwvmobile/m17-fme),
    whose project license is GPL; recording-specific permission still needs confirmation.
  - The `m17_after_retune` fixture is the `m17` fixture from 1.5263 s on, preceded by a lead-in cut from the seeded
    `noise_floor` fixture and joined byte for byte by `tools/build_iq_fixtures.py`; its recorded portion carries the
    `m17` fixture's terms.
  - The `nxdn96_after_retune` fixture is the `nxdn96` fixture from 0.5137 s on, preceded the same way by a lead-in cut
    from `noise_floor`; its recorded portion carries the `nxdn96` fixture's terms.
  - The `nxdn96_weak` and `nxdn96_weak_0db` fixtures are the `nxdn96` fixture with seeded complex Gaussian noise added
    by `tools/build_iq_fixtures.py` (no recorded material of its own); they carry the `nxdn96` fixture's terms.
  - The `dmr_t3_ras_cc` fixture derives from a DSDPlus raw discriminator capture of a DMR Tier III control
    channel contributed by the reporter of [issue #348](https://github.com/arancormonk/dsd-neo/issues/348)
    for regression testing (short excerpt of off-air control-channel signalling, no voice content; the source
    file has no stable URL, so `tools/build_iq_fixtures.py` pins its SHA-256 under `LOCAL_SOURCES`).
  - `tetra_scbs_real_air_50k.cs16` and `tetra_cc_marginal_real_air_144k.iq` are unmodified TETRA SCBS
    control-channel captures from
    [MattCheramie/GopherTrunk](https://github.com/MattCheramie/GopherTrunk), Apache License 2.0. The paired
    `tetra_scbs_real_air_54k.iq` is this project's deterministic integer-arithmetic resample for the 18 ksym/s
    decoder path, and `tetra_cc_marginal_real_air_144k_inverted.iq` is the deterministic complex conjugate used
    to verify negative-polarity recovery with the real capture's impairments. `tetra_scbs_real_air_50k.cs16.json` is
    this project's hand-written sidecar that replays the unmodified 50 kHz capture (the replay resamples it). Source
    commit, hashes, published capture parameters, expected cell identity, and reproduction
    command are recorded in `tests/fixtures/iq/tetra_scbs_real_air.PROVENANCE.md`; the upstream license text is
    `tests/fixtures/iq/tetra_scbs_real_air.APACHE-2.0.txt`.
  - `tests/fixtures/tetra/acelp/hello_tetra.acelp` and its decoder-input reference
    `hello_tetra.codec` come from the `sq5bpf/telive` test sample. Exact source,
    decoder, upstream GPL-3.0 project license, and hashes are recorded in the adjacent `PROVENANCE.md`;
    recording-specific permission still needs confirmation.
  - The frozen `Ahoj` SDS-TRANSFER vector in `tests/protocol/tetra/test_mle_cmce.c` derives its 64-bit SDS user
    field and expected decoded values from the example output published by
    [itds-consulting/tetra-multiframe-sds](https://github.com/itds-consulting/tetra-multiframe-sds), GPL-3.0.
    This project supplies the minimal CMCE/MLE envelope used by the regression test.
  - The complete 272-bit MAC-to-SDS `Ahoj` vector in
    `tests/protocol/tetra/test_sds_reference_mac.c` is `sample2.text` from
    [smarek/kaitai-tetra-sds](https://github.com/smarek/kaitai-tetra-sds), Apache License 2.0, pinned at
    commit `33f9fe3647e23e5a344cd96bfc6555531815e6bd` and blob
    `70e397d01cfc6e95eef97afac6961214ad9037c9`.

The project `LICENSE`, `COPYRIGHT`, and this notice file are installed with binary packages. Installed third-party
license texts currently include the ezpwd, pinned PFFFT and pinned TETRA_crypto notices under
`share/doc/dsd-neo/licenses/`, plus the IBM Plex font notice for builds that bundle the Qt frontend and the
librtlsdr/libusb notices for the Android package.

Packages also carry `legal/`, the full notices in `LICENSES/` under `licenses/`,
and the selected mbelib installation's documentation under `licenses/mbelib-neo/`.
This does not close the separate complete-runtime-notice or corresponding-source
findings in `legal/status.md`.

Native Airspy support links libairspy (BSD-3-Clause for `airspy.c`, `airspy.h`, and `airspy_commands.h`; MIT for `filters.h` and `iqconverter_*`), from https://github.com/airspy/airspyone_host. Android library sources, Windows overlay builds, and AppImage builds are pinned to `bd15be38e91ebaa3e0bebb1e320255bde4ccf059`; the upstream notice, prefixed with the copyright lines from the source headers, is retained in `android/third_party/libairspy/LICENSE` and distributed as `licenses/libairspy-LICENSE.txt`.
