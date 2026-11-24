# Third-Party Notices

This file summarizes third-party, vendored, and upstream-derived material known
in the source tree. Per-file headers and embedded notices remain authoritative
for file-specific license/provenance details.

The [licensing review](legal/status.md) records unresolved permission,
compatibility, patent and source-delivery findings. This list is attribution,
not a finding that every component is cleared for redistribution.

## Vendored Libraries

- **mbelib-neo** (linked vocoder; pinned release 2.6.12, GPL-2.0-or-later project
  contributions with ISC/pffft-neo components and unresolved ETSI table terms) —
  <https://github.com/arancormonk/mbelib-neo>. Its full installed notices and
  legal records accompany packages in `licenses/mbelib-neo/`. TETRA table
  permission/GPL compatibility and patent questions remain open; do not label
  the entire dependency as cleared GPL code. See its pinned
  [status](https://github.com/arancormonk/mbelib-neo/blob/v2.6.12/legal/status.md).

- **ezpwd Reed-Solomon** (`src/third_party/ezpwd`, target `dsd-neo_ezpwd`; Hard Consulting Corporation; vendored from https://github.com/arancormonk/ezpwd-reed-solomon) - GPL-3.0-or-later; `rs_base` is LGPL-2.1-or-later (per upstream notice). LGPL text: `src/third_party/ezpwd/lesser.txt` (installed as `share/doc/dsd-neo/licenses/ezpwd-LGPL-2.1-or-later.txt`).
- **pffft-neo** (target `dsd-neo_pffft`; arancormonk, derived from PFFFT by Julien Pommier and contributors, based on FFTPACKv4 by Dr Paul Swarztrauber/NCAR) — downloaded from <https://github.com/arancormonk/pffft-neo> at the immutable revision and SHA-256 in `tools/ci-dependency-pins.env`. GPL-2.0-or-later for pffft-neo's contributions, with the inherited BSD-like PFFFT/FFTPACK notices retained. Its README (copyright notice and license grant), GPL text, the inherited PFFFT license and its retained-notice record are installed as `share/doc/dsd-neo/licenses/pffft-neo-README.md`, `pffft-neo-LICENSE.txt`, `pffft-LICENSE.txt` and `pffft-NOTICES.txt`. See `docs/dependencies.md` for offline builds and updates.
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
  Android app; carries three project patches, kept in `android/third_party/patches/`:
  `0001-librtlsdr-add-rtlsdr_open_fd.patch` adds `rtlsdr_open_fd()` for USB-OTG descriptor injection,
  `0002-librtlsdr-check-async-buffer-allocation.patch` makes the async reader check its transfer allocations and
  return `-ENOMEM` instead of submitting NULL transfers, and `0003-librtlsdr-account-for-every-transfer.patch` makes
  it count the transfers libusb owns, return `-1` once none is left, and free nothing until every transfer has been
  called back. The snapshot is not part of any non-Android build, but the Windows, AppImage and macOS release
  packages build librtlsdr from the same pinned upstream revision with `0002` and `0003` applied
  (`tools/usb_dependency_patches.sh`). The license text names no holder, so every package that carries librtlsdr
  also carries `LICENSES/librtlsdr-NOTICES.txt`, the holder lines of every library source and header it compiles
  (Steve Markgraf, Dimitri Stolnikov, Hoernchen, Harald Welte, Sylvain Munaut, Hans-Frieder Vogt, Fitipower
  Integrated Technology Inc and Mauro Carvalho Chehab), as `licenses/librtlsdr-NOTICES.txt`.
- **libusb** (`android/third_party/libusb`; https://github.com/libusb/libusb) - LGPL-2.1-or-later; license text
  `android/third_party/libusb/COPYING`. Trimmed v1.0.30 snapshot (the `linux_usbfs` backend and POSIX shims upstream's
  own Android build uses) built into the Android app. Not part of any non-Android build. The APK also carries
  `LICENSES/libusb-android-NOTICES.txt`, the header of every compiled file up to its license text and the upstream
  AUTHORS file, as `licenses/libusb-android-NOTICES.txt`.

## Bundled Assets

- **IBM Plex Sans / IBM Plex Mono** (`src/ui/qt/fonts/IBMPlexSans-*.ttf`, `src/ui/qt/fonts/IBMPlexMono-*.ttf`;
  (c) IBM Corp., https://github.com/IBM/plex) - SIL Open Font License 1.1. License text
  `src/ui/qt/fonts/IBMPlex-LICENSE.txt` (installed as `share/doc/dsd-neo/licenses/ibm-plex-OFL-1.1.txt`).
  Compiled into the Qt Quick frontend's resource bundle, so it is redistributed by any build with `DSD_ENABLE_QT_UI=ON`
  — today that is the Android app.

## Online Map Data (fetched at run time, not shipped)

- **OpenStreetMap tiles** (Qt Quick frontend's Map screen, issue #800) — map data © OpenStreetMap contributors,
  available under the Open Data Commons Open Database License (ODbL) 1.0 from the OpenStreetMap Foundation; see
  <https://www.openstreetmap.org/copyright>. No map data or tile is in the source tree or in any asset. The app fetches
  tiles from `https://tile.openstreetmap.org/{z}/{x}/{y}.png` only after the user turns map tiles on, under the OSMF
  Tile Usage Policy (<https://operations.osmfoundation.org/policies/tiles/>): a distinct User-Agent
  (`dsd-neo/<version> (+https://github.com/arancormonk/dsd-neo)`), the server's cache headers honoured (at least 7
  days when it sends none), in-view tiles only (no prefetch, no bulk or offline download). The map always shows
  "© OpenStreetMap contributors", linked to the copyright page, while that source is used. Another tile server the
  user enters in Map settings brings its own terms, and the user enters its attribution with it.

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
    script from a seeded tone and noise and contain no recorded material. `speech_nfm_synth`, `speech_wfm_synth` and
    `speech_am_synth` are generated by that script from the project's own synthetic talker
    (`tests/core/speech_parity_tool.c`, `tests/test_support/speech_synth.c`) and seeded noise; they contain no
    recorded material.
  - The `tetra_call_real_air_72k` fixture is a 6.5-second excerpt of the wiki's `TETRA_IQ.zip` I/Q recording of a
    TETRA downlink carrier, resampled to 72 kHz and subject to the same unresolved permission finding as the other wiki-derived
    fixtures. `tools/build_iq_fixtures.py` records its URL and pinned SHA-256 hash.
  - The `tetra_call_gap_noisy_72k`, `tetra_call_adjacent_72k`, `tetra_call_awgn_72k`,
    `tetra_call_neighbour_reject_72k`, `tetra_call_offset_10k_72k` and `tetra_call_halfband_144k` fixtures are the
    `tetra_call_real_air_72k` fixture (or a stretch of it) with seeded noise, a frequency offset or a synthetic TETRA
    neighbour carrier added, or resampled to 144 kHz, by `tools/build_iq_fixtures.py` (no recorded material of their
    own); they carry the `tetra_call_real_air_72k` fixture's terms. `tetra_uplink_bursts_72k`, its sampling-phase
    variants `tetra_uplink_bursts_o1_72k` to `tetra_uplink_bursts_o7_72k`, `tetra_uplink_bursts_short_tail_72k` and the
    `tetra_bsch_*` fixtures are generated by that script from seeded synthetic bursts and noise and contain no recorded
    material; `tetra_uplink_bursts_gap_72k` is a sidecar alone on `tetra_uplink_bursts_72k`'s data.
  - The `nxdn48_after_retune` fixture is the `nxdn48` excerpt of the wiki's `NXDN_IQ.zip`, preceded by a lead-in cut
    from the seeded `noise_floor` fixture (no recorded material) and joined byte for byte by
    `tools/build_iq_fixtures.py`. Its recorded portion has the same unresolved permission finding as `nxdn48`.
  - The `ysf_gap` fixture is a sidecar alone, written by `tools/build_iq_fixtures.py`, that replays the `ysf` data
    file with a capture gap added. It carries no recorded material of its own.
  - The M17 fixture derives from `samples/m17_clear_voice_wav.wav` in [lwvmobile/m17-fme](https://github.com/lwvmobile/m17-fme),
    whose project license is GPL; recording-specific permission still needs confirmation.
  - The `m17_lsf_offair` fixture is cut from the same m17-fme recording (7.5 s on, 1.5 s) the same way as the `m17`
    fixture, and carries its terms.
  - The `m17_after_retune` fixture is the `m17` fixture from 1.5263 s on, preceded by a lead-in cut from the seeded
    `noise_floor` fixture and joined byte for byte by `tools/build_iq_fixtures.py`; its recorded portion carries the
    `m17` fixture's terms.
  - The `nxdn96_after_retune` fixture is the `nxdn96` fixture from 0.5137 s on, preceded the same way by a lead-in cut
    from `noise_floor`; its recorded portion carries the `nxdn96` fixture's terms.
  - The `nxdn96_weak` and `nxdn96_weak_0db` fixtures are the `nxdn96` fixture with seeded complex Gaussian noise added
    by `tools/build_iq_fixtures.py` (no recorded material of its own); they carry the `nxdn96` fixture's terms.
  - The `p25p1_cqpsk_cc_simulcast_noisy` and `p25p2_cc_noisy` fixtures are the `p25p1_cqpsk_cc_simulcast` (the
    `p25p1_cqpsk_cc` fixture with a delayed copy of itself added) and `p25p2_cc` fixtures with seeded complex Gaussian
    noise added by `tools/build_iq_fixtures.py` (no recorded material of their own); they carry their source fixtures'
    terms. The `p25p1_cqpsk_cc_then_noise` fixture is the `p25p1_cqpsk_cc` fixture, a second of the seeded
    `noise_floor` fixture and a second of the `dmr_t3_cc` fixture, joined byte for byte by that script; its recorded
    portions carry their source fixtures' terms. The `p25p1_cqpsk_cc_then_long_noise` fixture is a second of the
    `p25p1_cqpsk_cc` fixture and two seconds of `noise_floor`, joined the same way; its recorded portion carries the
    `p25p1_cqpsk_cc` fixture's terms.
  - The `p25p1_c4fm_adjacent_72k` and `p25p1_cqpsk_adjacent_72k` fixtures are the `p25p1_c4fm_cc` and `p25p1_cqpsk_cc`
    fixtures resampled to 72 kHz with a time-reversed copy of themselves added 12.5 kHz up, by
    `tools/build_iq_fixtures.py` (no recorded material of their own); they carry their source fixtures' terms. The
    `p25p1_c4fm_adjacent_78k`, `p25p1_c4fm_adjacent_94k`, `p25p1_cqpsk_adjacent_78k` and `p25p1_cqpsk_adjacent_94k`
    fixtures are the same mix at 78,125 and 93,750 Hz and carry the same terms.
  - The forced-rate recordings the `DECODE_IQ_*_AIRSPY_*` and `ENGINE_AIRSPY_FORCED_RATE_*` tests use are not committed:
    `tests/io/forced_rate_input.c` makes them at test time from excerpts of the `p25p1_cqpsk_cc`, `p25p1_cqpsk_vc`,
    `p25p1_cqpsk_cc_simulcast`, `p25p2_cc`, `dmr_voice_synth` and `tetra_call_real_air_72k` fixtures, upsampled to an
    Airspy's rates with seeded noise added before and after (no recorded material of their own). They carry their
    source fixtures' terms.
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
  - `p25p1_c4fm_cc_gt_s9` and `p25p1_c4fm_cc_gt_city` derive from two P25 Phase 1 C4FM control-channel captures
    (`mmr-s9-cc.cfile` and `mmr-city-cc.cfile`, 48 kHz float32 I/Q) from
    [MattCheramie/GopherTrunk](https://github.com/MattCheramie/GopherTrunk), Apache License 2.0, pinned at commit
    `f3798e04fa0498a54e969d2d13ec154c0ff0a65e` with their SHA-256 in `tools/build_iq_fixtures.py`
    (`CF32_CAPTURES`), which requantizes each whole capture to cu8 and changes nothing else. Source commits, hashes,
    published capture parameters and the reproduction command are recorded in
    `tests/fixtures/iq/p25p1_c4fm_cc_gt.PROVENANCE.md`; the upstream license text is
    `tests/fixtures/iq/p25p1_c4fm_cc_gt.APACHE-2.0.txt`.
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
license texts currently include the ezpwd, pinned pffft-neo and pinned TETRA_crypto notices under
`share/doc/dsd-neo/licenses/`, plus the IBM Plex font notice for builds that bundle the Qt frontend and the
librtlsdr/libusb notices for the Android package. The Windows, AppImage and macOS release packages, which bundle a
librtlsdr built with `0002` and `0003`, carry its GPL text as `licenses/librtlsdr-GPL-2.0-or-later.txt`,
`packaging/librtlsdr-PATCHES.txt`, naming those patches, as `licenses/librtlsdr-PATCHES.txt`, and its holder lines
as `licenses/librtlsdr-NOTICES.txt`. The Windows release
package also carries the notices of its SoapySDR load path (below): `licenses/SoapyAirspy-MIT.txt`,
`licenses/SoapyHackRF-MIT.txt`, `licenses/libhackrf-BSD-3-Clause.txt`, `licenses/libusb-LGPL-2.1-or-later.txt`,
`licenses/libusb-COPYRIGHT.txt`, `licenses/PThreads4W-Apache-2.0.txt`, `licenses/PThreads4W-NOTICE.txt` and
`licenses/PThreads4W-CONTRIBUTORS.txt`.

Packages also carry `legal/`, the full notices in `LICENSES/` under `licenses/`,
and the selected mbelib installation's documentation under `licenses/mbelib-neo/`.
This does not close the separate complete-runtime-notice or corresponding-source
findings in `legal/status.md`.

Native Airspy support links libairspy (BSD-3-Clause for `airspy.c`, `airspy.h`, and `airspy_commands.h`; MIT for `filters.h` and `iqconverter_*`), from https://github.com/airspy/airspyone_host. Android library sources, Windows overlay builds, AppImage builds and macOS DMG builds are pinned to `bd15be38e91ebaa3e0bebb1e320255bde4ccf059`; the upstream notice, prefixed with the copyright lines from the source headers, is retained in `android/third_party/libairspy/LICENSE` and distributed as `licenses/libairspy-LICENSE.txt`. The Android copy (`android/third_party/libairspy`) carries one project patch, kept in `android/third_party/patches/`: `0001-libairspy-wait-for-cancelled-transfers.patch` makes stop, close, restart and a packing change wait until libusb has called back every cancelled transfer before freeing or resubmitting any. Windows overlay, AppImage and macOS DMG builds apply the same patch (`tools/usb_dependency_patches.sh`); builds against a system libairspy use it unpatched.

## Windows SoapySDR Load Path

The Windows release ZIP runs SoapySDR devices through modules it carries in `lib\SoapySDR\modules0.8\`, with
their libraries in `bin\`. Each is built by a vcpkg overlay in `vcpkg-ports/` from the commit and archive hash pinned
in `tools/ci-dependency-pins.env`; `legal/components.json` records each one.

- **SoapySDR** and **SoapySDRUtil** (Josh Blum, Bastille Networks, Coburn Wightman, Nicholas Corgan,
  https://github.com/pothosware/SoapySDR) - BSL-1.0; license text `LICENSES/BSL-1.0.txt`, installed as
  `licenses/BSL-1.0.txt`. The license text names no holder, so `LICENSES/SoapySDR-COPYRIGHT.txt`, installed as
  `licenses/SoapySDR-COPYRIGHT.txt`, lists every copyright line of the sources the library and SoapySDRUtil are
  compiled from. The Windows ZIP and the AppImage build the 0.8.1 release commit (`SOAPY_SDR_SHA`); the AppImage
  builds the library alone. The ZIP's `SoapySDRUtil.exe` comes from the `soapysdr` overlay's `tools` feature; its MSVC `getopt.h` is mingw-w64's
  public-domain file. The macOS DMG bundles the same overlay's library, built from the same commit without the
  `tools` feature, and carries the same two notices.
- **SoapyAirspy** (Charles J. Cliffe, https://github.com/pothosware/SoapyAirspy) - MIT; release 0.2.1, built against
  the patched libairspy above, so the module and the native Airspy input share one `airspy.dll`. Notice
  `vcpkg-ports/soapy-airspy/copyright`, shipped as `licenses/SoapyAirspy-MIT.txt`.
- **SoapyHackRF** (Wei Jiang, Josh Blum, Kevin Mehall, https://github.com/pothosware/SoapyHackRF) - MIT; release
  0.3.6. Its LICENSE names no holder, so the notice `vcpkg-ports/soapy-hackrf/copyright` adds the source headers'
  holders; shipped as `licenses/SoapyHackRF-MIT.txt`.
- **libhackrf** (Great Scott Gadgets, Jared Boone, Benjamin Vernoux, https://github.com/greatscottgadgets/hackrf) -
  BSD-3-Clause for the library sources; release v2026.01.3, library only (`host/libhackrf/src`). The repository's
  GPL-2.0 firmware and tools are not built. HackRF is a trademark of Great Scott Gadgets. Notice, with every
  source-header holder and the trademark notice, `vcpkg-ports/hackrf/copyright`, shipped as
  `licenses/libhackrf-BSD-3-Clause.txt`.
- **libusb** (https://github.com/libusb/libusb) - LGPL-2.1-or-later; vcpkg's 1.0.30 port at the manifest baseline,
  `libusb-1.0.dll`, which librtlsdr, libairspy and libhackrf load. Its COPYING is the one retained in
  `android/third_party/libusb/COPYING`, shipped as `licenses/libusb-LGPL-2.1-or-later.txt`. The holders and
  attributions in the headers of the sources that DLL is built from (Johannes Erdfelt, Daniel Drake, Nathan Hjelm,
  Peter Stuge, Google LLC, Hans de Goede, Pete Batard, Chris Dickens, Michael Plante, RealVNC Ltd., Red Hat, Inc.,
  Sylvain Fasel and others), and the upstream AUTHORS file, are kept in `packaging/windows/libusb-COPYRIGHT.txt` and
  shipped as `licenses/libusb-COPYRIGHT.txt`. The macOS DMG bundles the same port's `libusb-1.0.0.dylib`, which
  dsd-neo, librtlsdr and libairspy load, built with the Darwin backend; the headers of the sources it is built from
  (Nathan Hjelm and Google LLC for the Darwin backend) and the AUTHORS file are kept in
  `LICENSES/libusb-macos-NOTICES.txt` and shipped as `licenses/libusb-macos-NOTICES.txt`, with the same COPYING.
- **PThreads4W** (John E. Bossom and the Pthreads4w contributors, https://sourceforge.net/projects/pthreads4w/) -
  Apache-2.0; vcpkg's 3.0.0 port at the manifest baseline, `pthreadVC3.dll`, which the same libraries load. Its
  LICENSE, NOTICE and the CONTRIBUTORS file the NOTICE refers to, from that release's archive, are kept in
  `packaging/windows/` and shipped as `licenses/PThreads4W-Apache-2.0.txt`, `licenses/PThreads4W-NOTICE.txt` and
  `licenses/PThreads4W-CONTRIBUTORS.txt`.

## Libraries Built by vcpkg

The Windows release ZIP also carries in `bin\` the libraries vcpkg builds at the manifest baseline in `vcpkg.json`,
or from the project's overlays in `vcpkg-ports/`. For each one the ZIP carries the copyright file its port installs
(`share/<port>/copyright` in vcpkg's tree) as `licenses/<port>-copyright.txt`, except PDCurses, whose port installs
vcpkg's own paraphrase. Each library was reviewed against the sources its DLL is compiled from, in the archive its
port downloads; where the port's file lacks a holder or a license those sources carry, the ZIP also carries the
notices named below, kept in `LICENSES/` (`packaging/windows/` for PortAudio, which only the ZIP carries) and
installed under `licenses/`. The licenses are the ones those sources carry; where a port's `vcpkg.json` declares
another, `legal/components.json` says so.

The Android app links the same baseline's static builds of OpenSSL (libcrypto and libssl, with the legacy provider),
libcurl (with OpenSSL instead of Schannel), zlib, Expat, libsndfile, libFLAC, libogg, libvorbis, Opus, LAME and
mpg123 into `libdsd-neo-app_arm64-v8a.so` (`vcpkg-triplets/arm64-android-static.cmake`). The APK carries the same
notices as the ZIP under the same names in `assets/doc/dsd-neo/licenses/`, each port's copyright file taken from the
Android build's vcpkg tree (`cmake/stage_android_package.cmake`). The shared notice files name the holders of the
sources both builds compile, the arm64 files of OpenSSL and Opus included.

The macOS DMG bundles the same baseline's shared builds of those libraries (OpenSSL's libcrypto and libssl, libcurl
on OpenSSL, zlib, Expat, libsndfile and its codecs), PortAudio, libusb, SoapySDR, ncurses, codec2 and mbelib-neo, in
`lib/` (`vcpkg-triplets/arm64-osx-dynamic-release.cmake`). It carries the notices the ZIP carries for each, under the
same names in `share/doc/dsd-neo/licenses/`, the port's copyright file taken from the DMG build's vcpkg tree, and
the macOS ones named below. `legal/components.json` lists them in each record's `macos_release` map, with the files
in `bin/` and `lib/` the component is in, which drives the DMG's staging (`cmake/stage_release_notices.cmake`) and
its check (`tools/check_release_notices.py`, which also refuses a file in `bin/` or `lib/` that no record lists).

- **codec2** (`codec2.dll`; https://github.com/arancormonk/codec2, a fork of https://github.com/drowe67/codec2, the
  commit `CODEC2_SHA` in `tools/ci-dependency-pins.env`) - LGPL-2.1-only AND LGPL-2.1-or-later AND BSD-3-Clause AND
  BSD-2-Clause. The Android app links the same overlay's static library, and the AppImage and the macOS DMG bundle a
  shared library built from the same commit, so all four carry `licenses/codec2-LGPL-2.1.txt` (codec2's COPYING) and
  `licenses/codec2-NOTICES.txt` (the holders, the kiss_fft and Mozilla BSD notices and the LGPL-2.1-or-later Valenti
  notice), and `licenses/LicenseRef-codec2-varicode.txt` (the varicode files' "GNU General Public License version 2.1"
  grant, unmodified, with a note that dsd-neo reads it as the LGPL 2.1 of codec2's COPYING and README).
- **PortAudio** (`portaudio.dll`; https://www.portaudio.com, release v19.7.0 through the `portaudio` overlay, pinned
  as `PORTAUDIO_SHA`; built without ASIO) - MIT. `licenses/portaudio-NOTICES.txt` adds the holders of the Windows
  host API sources. The macOS DMG's PortAudio, its audio input and output there, uses the Core Audio host API;
  `licenses/portaudio-macos-NOTICES.txt` (`LICENSES/portaudio-macos-NOTICES.txt`) adds the holders of the sources
  that build compiles (also Bjorn Roche) and the authors its Core Audio files name.
- **PDCurses** (`pdcurses.dll`; https://pdcurses.org/, release 3.9) - public domain,
  `licenses/LicenseRef-PDCurses-Public-Domain.txt` (upstream's statements).
- **ncurses** (macOS DMG only: `libncursesw` and `libformw`, and the terminal database in `share/terminfo/`;
  https://invisible-island.net/ncurses/, release 6.5 through the `ncurses` overlay) - X11-distribute-modifications-variant
  AND LicenseRef-ncurses-terminfo. `licenses/X11-distribute-modifications-variant.txt` (ncurses' COPYING, which names
  Thomas E. Dickey and the Free Software Foundation) and `licenses/LicenseRef-ncurses-terminfo.txt` (the terms
  `misc/terminfo.src`, the database's source, states for itself).
- **OpenSSL** (`libcrypto-3-x64.dll`; https://www.openssl.org, release 3.6.5; no libssl, since libcurl uses
  Schannel) - Apache-2.0 AND CC0-1.0 AND LicenseRef-OpenSSL-KISA-SEED. `licenses/OpenSSL-AUTHORS.md`,
  `licenses/OpenSSL-NOTICES.txt` and `licenses/LicenseRef-OpenSSL-KISA-SEED.txt`. `LICENSES/OpenSSL-NOTICES.txt`
  names the holders of the sources every package's OpenSSL 3.6.5 build compiles (Nokia's and Siemens AG's CMP, CRMF
  and HTTP client code and Patrick Powell's `bio_print.c` among them), with their dual-license, CC0 and public-domain
  notes.
- **libcurl** (`libcurl.dll`; https://curl.se/, release 8.23.0, Schannel) - curl AND ISC.
  `licenses/curl-NOTICES.txt`. The macOS DMG's libcurl uses OpenSSL and checks certificates against the macOS
  trust store; `licenses/curl-macos-NOTICES.txt` adds the one holder (Jan Venekamp) of the Apple files only it
  compiles.
- **Expat** (`libexpat.dll`; https://github.com/libexpat/libexpat, release 2.9.0) - MIT AND CC0-1.0.
  `licenses/expat-NOTICES.txt`.
- **zlib** (`z.dll`; https://www.zlib.net/, release 1.3.2) - Zlib.
- **libsndfile** (`sndfile.dll`; https://github.com/libsndfile/libsndfile, release 1.2.2) - LGPL-2.1-or-later AND
  LGPL-2.0-or-later AND Apache-2.0 AND BSD-3-Clause AND TU-Berlin-1.0 AND LicenseRef-Sun-G72x, for its bundled GSM
  06.10, ALAC and G.72x codecs and Xiph.Org code. `licenses/libsndfile-GSM610-COPYRIGHT.txt`,
  `licenses/libsndfile-ALAC-Apache-2.0.txt`, `licenses/LicenseRef-Sun-G72x.txt` and
  `licenses/libsndfile-NOTICES.txt`. It loads:
  - **libFLAC** (`FLAC.dll`, 1.5.0) - BSD-3-Clause.
  - **libogg** (`ogg.dll`, 1.3.6) - BSD-3-Clause.
  - **libvorbis** (`vorbis.dll` and `vorbisenc.dll`, 1.3.7) - BSD-3-Clause AND TU-Berlin-1.0;
    `licenses/libvorbis-lpc-NOTICE.txt`.
  - **Opus** (`opus.dll`, 1.6.1) - BSD-3-Clause AND BSD-2-Clause; `licenses/opus-NOTICES.txt`.
  - **LAME** (`libmp3lame.DLL`, 3.100, with its mpglib decoder) - LGPL-2.0-or-later AND LGPL-2.1-or-later;
    `licenses/mp3lame-LGPL-2.1.txt`, `licenses/mp3lame-README.txt`, `licenses/mp3lame-LICENSE.txt` and
    `licenses/mp3lame-NOTICES.txt`.
  - **mpg123** (`mpg123.dll`, 1.33.7) - LGPL-2.1-only AND MirOS; `licenses/mpg123-AUTHORS.txt` and
    `licenses/mpg123-NOTICES.txt`.

## AppImage OpenSSL

- **OpenSSL 3.5.9** (`usr/lib/libcrypto.so.3` in the Linux AppImage; https://www.openssl.org, the 3.5 LTS commit
  `OPENSSL_SHA` in `tools/ci-dependency-pins.env`, tag `openssl-3.5.9`) - Apache-2.0 AND CC0-1.0 AND
  LicenseRef-OpenSSL-KISA-SEED. The AppImage builds this libcrypto itself, apart from the vcpkg 3.6.5 the other
  packages carry, and carries no libssl from it. `licenses/OpenSSL-3.5-NOTICES.txt`
  (`packaging/appimage/OpenSSL-3.5-NOTICES.txt`) names the holders of the sources that commit compiles for x86_64 and
  aarch64, with their dual-license, CC0 and public-domain notes; `licenses/Apache-2.0.txt`,
  `licenses/OpenSSL-AUTHORS.md` (the same AUTHORS.md as 3.6.5) and `licenses/LicenseRef-OpenSSL-KISA-SEED.txt` (the
  SEED terms, unchanged in 3.5.9) complete it.

## Windows Visual C++ Runtime

- **Microsoft Visual C++ runtime** (Microsoft Corporation) - Microsoft's license terms, not an open-source license. The
  Windows ZIP carries in `bin\` the runtime DLLs its binaries import (`MSVCP140.dll`, `VCRUNTIME140.dll`,
  `VCRUNTIME140_1.dll`, or another file of the `msvc-runtime` record in `legal/components.json`), unmodified, from the
  `VC\Redist\MSVC\<version>\x64\Microsoft.VC<toolset>.CRT` folder of the Visual C++ toolset that built the release.
  The Distributable Code section of the [Visual Studio license terms](https://visualstudio.microsoft.com/license-terms/)
  and its REDIST list ([Visual Studio 2026 and later](https://learn.microsoft.com/en-us/visualstudio/releases/2026/redistribution),
  [Visual Studio 2022](https://learn.microsoft.com/en-us/visualstudio/releases/2022/redistribution)) let a licensed
  Visual Studio user distribute those files. With them the ZIP carries an end-user notice of the terms that apply to
  these files alone, `packaging/windows/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`, as
  `licenses/LicenseRef-Microsoft-Visual-CPP-Runtime.txt`. `legal/msvc-runtime.md` quotes Microsoft's terms and records
  how the release meets them and the GPL question it accepts as a risk.

## Android App Runtime

- **Qt 6.11.3** (The Qt Company Ltd. and other contributors; https://www.qt.io, sources
  https://download.qt.io/official_releases/qt/6.11/6.11.3/submodules/) - the Android app ships Qt's Core, Gui, Network,
  OpenGL, Qml, Quick, Quick Controls, Quick Dialogs, Quick Effects, Quick Layouts, Quick Shapes, Svg and Labs
  FolderListModel libraries, their plugins and QML plugins, and Qt's Android Java code, unmodified from the Qt 6.11.3
  android_arm64_v8a kit. Used under the GNU LGPL version 3 (`licenses/LGPL-3.0-only.txt`; the GPL text is `LICENSE`).
  Qt contains third-party code (PCRE2, FreeType, HarfBuzz, libpng, libjpeg-turbo, the Public Suffix List (MPL-2.0),
  Unicode data and others) whose notices are in `licenses/Qt-NOTICES.txt` (`packaging/android/Qt-NOTICES.txt`). Since
  dsd-neo's source and build scripts are public, the app can be rebuilt against a modified Qt. Qt's own source is in
  Qt's official source archives for the shipped version: every published APK has `<APK name>.qt-source.txt` beside it,
  which names them and the Public Suffix List's source (`legal/source-delivery.md`). Portions of this software are copyright © 2026 The FreeType Project
  (https://freetype.org). All rights reserved. This software is based in part on the work of the Independent JPEG
  Group.
- **LLVM libc++ (Android NDK r28c)** - `libc++_shared.so`, with libc++abi, libunwind and compiler-rt builtins;
  Apache-2.0 with LLVM exceptions (`licenses/Apache-2.0.txt`, `licenses/LLVM-exception.txt`), plus the legacy LLVM
  notices (`licenses/NDK-runtime-NOTICES.txt`, which also holds the notice of Android's bionic startup code linked into
  each native library).
- **AndroidX** (The Android Open Source Project; androidx.core 1.17.0 and its dependencies), Guava's listenablefuture
  1.0 and JSpecify 1.0.0 - Apache-2.0; list in `licenses/AndroidX-NOTICES.txt`.
- **Kotlin standard library 2.2.10, kotlinx.coroutines 1.8.1, JetBrains annotations 23.0.0** (JetBrains s.r.o. and
  contributors) - Apache-2.0, with ThreeTenBp-derived code (BSD-3-Clause) and Boost-derived code (BSL-1.0);
  `licenses/Kotlin-NOTICES.txt`.

The four records (`qt`, `ndk-runtime`, `androidx`, `kotlin`) are in `legal/components.json`; their `android_release`
maps name the files the APK carries. One Apache-2.0 text, `LICENSES/Apache-2.0.txt` (the apache.org file), serves
these records and the OpenSSL record.

## vcpkg Port Recipes

- **vcpkg** (Copyright (c) Microsoft Corporation, https://github.com/microsoft/vcpkg) - MIT. The `soapysdr` and `rtlsdr`
  overlays in `vcpkg-ports/` are copies of vcpkg's builtin ports at the manifest baseline, and `portaudio` of its
  builtin port at port-version 8, each with the dsd-neo changes its `portfile.cmake` names (GPL-3.0-or-later), so each
  copy is under GPL-3.0-or-later AND MIT, as its SPDX line says. vcpkg's license and notice are kept beside them as
  `vcpkg-ports/LICENSE-vcpkg.txt`. The `ncurses` overlay, which only the
  macOS DMG builds, is a copy of vcpkg's builtin port at the manifest baseline too. The recipes only drive the
  builds: no vcpkg code is compiled into a release package, so the packages do not carry this notice.
