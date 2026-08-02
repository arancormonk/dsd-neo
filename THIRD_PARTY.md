# Third-Party Notices

This file summarizes third-party, vendored, and upstream-derived material known
in the source tree. Per-file headers and embedded notices remain authoritative
for file-specific license/provenance details.

## Vendored Libraries

- **ezpwd Reed-Solomon** (`src/third_party/ezpwd`, target `dsd-neo_ezpwd`; Hard Consulting Corporation; vendored from https://github.com/arancormonk/ezpwd-reed-solomon) - GPL-3.0-or-later; `rs_base` is LGPL-2.1-or-later (per upstream notice). LGPL text: `src/third_party/ezpwd/lesser.txt` (installed as `share/doc/dsd-neo/licenses/ezpwd-LGPL-2.1-or-later.txt`).
- **pffft** (`src/third_party/pffft`, target `dsd-neo_pffft`; Julien Pommier, based on FFTPACKv4 by Dr Paul Swarztrauber/NCAR) - BSD-like FFTPACK license; `src/third_party/pffft/COPYING` (installed as `share/doc/dsd-neo/licenses/pffft-FFTPACK.txt`).
- **Tiny AES** (`src/crypto/crypt-aes.c`) - Unlicense/public domain; original upstream https://github.com/kokke/tiny-AES-c.
- **librtlsdr** (`android/third_party/librtlsdr`; osmocom, https://gitea.osmocom.org/sdr/rtl-sdr) - GPL-2.0-or-later;
  license text `android/third_party/librtlsdr/COPYING`. Trimmed v2.0.2 snapshot (library sources only) built into the
  Android app; carries one project patch adding `rtlsdr_open_fd()` for USB-OTG descriptor injection, kept as
  `android/third_party/patches/0001-librtlsdr-add-rtlsdr_open_fd.patch`. Not part of any non-Android build.
- **libusb** (`android/third_party/libusb`; https://github.com/libusb/libusb) - LGPL-2.1-or-later; license text
  `android/third_party/libusb/COPYING`. Trimmed v1.0.30 snapshot (the `linux_usbfs` backend and POSIX shims upstream's
  own Android build uses) built into the Android app. Not part of any non-Android build.

## Embedded Provenance-Bearing Code

- **EDACS-FME / EDACS helpers** (`src/protocol/edacs/edacs-fme.c`) - source comments credit EDACS-FM, sp5wwp/ledacs,
  XTAL Labs, Robert Morelos-Zaragoza, LWVMOBILE, and ilyacodes.
- **Binary BCH v3.1** (`src/protocol/edacs/edacs-bch3.c`) - source comments credit Robert Morelos-Zaragoza and note
  portions from Simon Rockliff's Reed-Solomon code. The file also contains a non-commercial/commercial-permission notice
  that should be reviewed before changing its license characterization.
- **NXDN OP25-derived tables and helpers** (`include/dsd-neo/protocol/nxdn/nxdn_const.h`,
  `src/protocol/nxdn/nxdn_frame.c`, `src/protocol/nxdn/nxdn_deperm.c`) - source comments credit Osmocom OP25, Max H.
  Parke, and Mathias Weyland under GPL notices.
- **NXDN convolution helper** (`src/protocol/nxdn/nxdn_convolution.c`) - source comments credit Jonathan Naylor,
  Edouard Griffiths, and Louis HERVE under a GPL-2.0-or-later notice.
- **FEC helpers outside `src/third_party`** (`include/dsd-neo/fec/ReedSolomon.hpp`,
  `include/dsd-neo/fec/Golay24.hpp`, `src/fec/fec.c`) - source comments credit Simon Rockliff, Hank Wallace, and
  Edouard Griffiths respectively.
- **OP25/GNU Radio/Yair Linn-derived DSP and protocol snippets** (`src/dsp/costas.cpp`, `src/dsp/ted.cpp`,
  `src/protocol/p25/phase2/p25p2_frame.c`, `src/protocol/p25/p25_crc.c`) - source comments identify OP25, GNU Radio,
  KA1RBI, Free Software Foundation, and Yair Linn provenance for selected algorithms/tables.
- **Miscellaneous embedded provenance** (`src/core/util/dsd_misc.c`, `src/io/control/dsd_rigctl.c`,
  `src/protocol/ysf/ysf.c`) - source comments identify OP25, libM17, NedSimao/FilteringLibrary, gqrx-scanner, DSDcc,
  gr-ysf, and Munaut-derived code or ideas.

## Test Fixtures

- **I/Q decode fixtures** (`tests/fixtures/iq/*.iq`) - derived from public sample recordings, converted to cu8
  baseband by `tools/build_iq_fixtures.py`, which records the upstream URL for every fixture.
  - Most fixtures derive from user-contributed off-air sample recordings hosted on the
    [Signal Identification Wiki](https://www.sigidwiki.com/). The wiki does not publish an explicit content license
    (its MediaWiki rights metadata is empty), so no CC compatibility claim is made; the fixtures are short excerpts
    of off-air radio transmissions redistributed for testing with attribution to Signal Identification Wiki
    contributors, with per-fixture upstream URLs recorded in `tools/build_iq_fixtures.py`. If upstream licensing is
    clarified, update this entry.
  - The M17 fixture derives from `samples/m17_clear_voice_wav.wav` in [lwvmobile/m17-fme](https://github.com/lwvmobile/m17-fme),
    licensed GPL.

The project `LICENSE`, `COPYRIGHT`, and this notice file are installed with binary packages. Installed third-party
license texts currently include the vendored ezpwd and pffft notices under `share/doc/dsd-neo/licenses/`.
