# Native Airspy R2 and Mini

DSD-neo can receive directly from Airspy R2 and Airspy Mini devices using
libairspy. This backend does not require SoapySDR or an SDR application running
alongside the decoder. Airspy HF+ devices use a different driver and are not
supported by this backend.

## Build and install

On Debian/Ubuntu install `libairspy-dev`; on macOS use `brew install airspy`.
Windows builds use the pinned `airspy` vcpkg overlay. Android packages build the
pinned native driver against the application's shared libusb library.

```sh
cmake --preset dev-debug -DDSD_REQUIRE_AIRSPY=ON
cmake --build --preset dev-debug -j
```

`DSD_ENABLE_AIRSPY` defaults to `ON` and detects the library when available.
`DSD_REQUIRE_AIRSPY=ON` makes a missing or disabled dependency an error.
Airspy works with both `DSD_ENABLE_RTLSDR=OFF` and `DSD_ENABLE_SOAPYSDR=OFF`.

Linux users need permission to access the USB device. Install the Airspy udev
rules supplied by the driver package. Android obtains USB permission through its
normal system dialog; the application's native code uses the descriptor owned by
the USB manager.

On Windows dsd-neo talks to the Airspy through the WinUSB driver. You normally
install nothing: the Airspy's firmware reports it as a WinUSB device, and Windows
sets up the driver itself when you plug it in (Airspy's Quick Start guide says
the same for Windows 8, 8.1, 10 and 11). Some PCs are set up to block that,
the guide says. Then install Airspy's WinUSB Compatibility Driver from the Quick
Start guide on airspy.com: in Device Manager pick the Airspy, choose Update
Driver, then Browse my computer, and point it at the `.inf` file from that
download. Zadig can bind WinUSB to it too.

Only one program can open an Airspy at a time, so close SDR# or any other SDR
program first. `dsd-neo --airspy-list` prints the serial of every Airspy it can
open. `No Airspy devices found.` there, or `Airspy open: AIRSPY_ERROR_NOT_FOUND
(-5)` at start, means the Airspy is not plugged in, has no driver, or another
program has it open.

The release packages (the Windows ZIP, the Linux AppImage, the macOS DMG and the
Android app) build libairspy from the pinned source with one dsd-neo patch:
stopping, closing or restarting a stream waits until the USB stack has handed
back every transfer before the library frees or reuses it. Upstream libairspy
can free a transfer that is still in flight, which can crash the decoder or stop
the stream from coming back after a retune or a settings change. A build from
source against a distribution's or Homebrew's libairspy uses that library as it
is packaged, without the patch. To use the patched library there, build it from
the pinned source with `tools/usb_dependency_patches.sh fetch libairspy <dir>`
and point CMake at its install prefix. The same applies to librtlsdr; see
[Dependencies](dependencies.md).

## Select and tune

```sh
dsd-neo --airspy-list
dsd-neo -i airspy:851.375M -ft --frontend terminal
dsd-neo -i airspy:serial=0123456789ABCDEF:851.375M -ft --frontend terminal
```

Input syntax is `airspy[:serial=<16 hexadecimal digits>][:frequency[:bw[:sql[:vol]]]]`.
The optional trailing fields are DSP bandwidth in kHz, squelch in dB (0 disables,
`auto[+N]` for the [auto squelch](cli.md#auto-squelch---squelch-auto), or `noise[+N]` for the
[noise squelch](cli.md#noise-squelch---squelch-noise)),
and monitor volume (0–3); gain uses the separate native controls. Bandwidth must
be one of 4, 6, 8, 12, 16, 24, 48 or 72 kHz, the same list as an RTL-SDR; other
values warn (`Airspy bandwidth 96 kHz is unsupported; using 48 kHz.`) and fall
back to 48. See [DSP bandwidth and rates](#dsp-bandwidth-and-rates) for how an
Airspy reaches it.
Numeric volume values are clamped to 0–3. Invalid squelch or volume text warns
and keeps the previous/default value; invalid frequencies still prevent startup.
An explicit serial must match exactly. Without one, the first enumerated device
is selected and its serial is logged. Frequencies use the usual Hz/K/M/G syntax;
the native tuning range is 24–1700 MHz.

Terminal users can also select Airspy under **Input → Switch source** and adjust
it under **Input → Airspy receiver controls**. Qt and Android expose an Airspy
source in saved systems, exploration, and scan lists, with native receiver
controls in setup and the live radio panel.

## DSP bandwidth and rates

An Airspy runs at its own sample rates: 10 or 2.5 MS/s on an R2, 6 or 3 MS/s on
a Mini. Halving those never lands on a DSP bandwidth. dsd-neo halves the rate as
far as it can while staying at or above the DSP bandwidth, then resamples what
is left to the bandwidth exactly. So every decoder and the analog monitor run at
the same rate as on an RTL-SDR at that bandwidth.

| DSP bandwidth | R2 (2.5 or 10 MS/s) halves to | Mini (3 or 6 MS/s) halves to |
| --- | --- | --- |
| 72 and 48 kHz | 78,125 Hz | 93,750 Hz |
| 24 kHz | 39,062.5 Hz | 46,875 Hz |
| 16 and 12 kHz | 19,531.25 Hz | 23,437.5 Hz |
| 8 and 6 kHz | 9,765.625 Hz | 11,718.75 Hz |
| 4 kHz | 4,882.8125 Hz (9,765.625 Hz at 10 MS/s) | 5,859.375 Hz |

The resampler then takes that rate to the bandwidth with an exact ratio, for
example 384/625 from 78,125 Hz to 48 kHz. It costs well under 1 % of one CPU
core. What this means in practice:

- **Every mode runs**, TETRA included. `-fT` sets the 72 kHz bandwidth TETRA
  needs, on an Airspy as on an RTL-SDR (`-fT -i airspy:851.375M`). Switching to
  TETRA while running at another bandwidth keeps TETRA selected but decodes
  nothing until you set 72 kHz; dsd-neo does not change the bandwidth on its
  own, and the toast says so (`Decoding TETRA: needs a 72 kHz DSP bandwidth; it
  stays 48 kHz until you set it`). TETRA is not part of Auto and is not a
  `--trunk-scan` target, as on any other input.
- **Analog widths** follow the RTL-SDR table in [the CLI guide](cli.md) (Analog
  reception): an NFM width of 25 kHz needs a 48 or 72 kHz DSP bandwidth, and AM
  does not run at 4 or 6 kHz. A width the bandwidth cannot filter is refused at
  startup, before the Airspy opens, with the fix (`NFM bandwidth 25 kHz does not
  fit the 24 kHz DSP rate (the largest width it fits is 20.4 kHz); set the DSP
  bandwidth to 48 or 72 kHz`). The terminal, a loaded config and
  `--validate-config` hold an Airspy to the same rule.
- **FSK modes** (P25 Phase 1 C4FM, DMR, NXDN, dPMR, YSF, M17, EDACS and the
  like) still count an Airspy's rate as one the device chose. So
  `digital_resample = "auto"` resamples the decoder's input to 48 kHz where the
  bandwidth gives no whole number of samples per symbol (12 kHz for 4,800 symbols
  per second), where an RTL-SDR at 12 kHz is not resampled. At 48 kHz every FSK
  symbol rate is whole, so nothing is resampled.
- **Qt and Android** offer the TETRA chip on an Airspy, and the add-system wizard
  shows the bandwidth field for an Airspy (gain and PPM stay hidden, since the
  Airspy has its own receiver controls). Leave it empty for TETRA, which then
  runs at 72 kHz. A saved Airspy bandwidth snaps to the nearest one in the list
  above, and one above 72 kHz runs at 48 kHz.

## Receiver settings

Example configuration:

```ini
[input]
source = "airspy"
airspy_serial = ""
rtl_freq = "851.375M"
rtl_bw_khz = 48
rtl_sql = 0
airspy_sample_rate = 0
airspy_gain_mode = "sensitivity"
airspy_sensitivity_gain = 10
airspy_linearity_gain = 10
airspy_lna_gain = 1
airspy_mixer_gain = 5
airspy_vga_gain = 5
airspy_lna_agc = false
airspy_mixer_agc = false
airspy_bias_tee = false
```

Every `airspy_*` setting also has a CLI option with hyphens, for example
`--airspy-gain-mode manual --airspy-lna-gain 12 --airspy-bias-tee 0`.
Boolean CLI values accept `0`/`1` or `false`/`true`.
Invalid non-serial `airspy_*` INI values warn with the key and value and keep the
previous/default setting. An invalid `airspy_serial` prevents Airspy startup until
corrected or overridden with `--airspy-serial` or `-i airspy:serial=...`;
overriding a different setting does not clear a serial error. `--validate-config`
continues to report invalid values.

- **Sample rate:** `0` or `auto` selects the highest supported rate up to
  10 MS/s on desktop and the lowest usable rate on Android. An explicit rate in
  samples/second must appear in the device's reported list. Rate availability
  depends on the device and firmware; unsupported requests fail.
- **Gain:** sensitivity and linearity modes use indices from 0 to 21. Manual
  mode exposes LNA, mixer, and VGA indices from 0 to 15. These indices are not
  dB values. LNA and mixer AGC apply only in manual mode; their corresponding
  manual gain values take effect when AGC is disabled.
- **Bias tee:** off by default, controlled by the Airspy setting. The shared
  RTL bias preference does not enable antenna power on an Airspy.
- **Shared controls:** frequency, DSP bandwidth, squelch, and monitor volume
  retain the existing radio configuration fields. Hardware sample rate is
  separate from DSP bandwidth: the decoder halves and resamples whatever rate the
  Airspy runs at to the DSP bandwidth (see
  [DSP bandwidth and rates](#dsp-bandwidth-and-rates)).
  Applying a config during a live Airspy session retunes frequency and updates
  squelch immediately. Bandwidth or monitor-volume changes restart the stream,
  combined with any serial or sample-rate change in the same restart. A failed
  reopen restores the previous receiver settings; other config changes, including
  audio output, still finish applying and the command reports failure. While a
  `-Y` row or `--trunk-scan` target carries `--squelch-db` or `--squelch`, an applied squelch
  (from a config or the squelch control) changes the configured default, which
  takes effect when the scanner leaves that row; see
  [CSV Formats](csv-formats.md). Airspy setting edits leave the squelch alone, so
  a stream they reopen starts on the row's threshold.
- **Unsupported controls:** libairspy has no PPM-correction API. Auto-PPM and
  RTL-specific direct sampling, oscillator, and tuner controls do not apply.
- **Overload:** an Airspy's overload cannot be seen in the I/Q samples libairspy
  delivers. libairspy turns the Airspy's real samples into I/Q itself, and that
  conversion halves their amplitude and filters one of the two parts, so samples at
  the Airspy's limits arrive near half scale and never reach a clipping threshold.
  Neither [Signal search](signal-search.md) nor the input level meter shows Airspy
  clipping. If strong signals are near, lower the gain.

Device and rate changes reopen the receiver. Gain, AGC, and bias changes are
serialized through the decoder's command path. Frequency changes flush the
driver's queued samples before receiving on the new frequency.

## Capture and replay

Airspy delivers normalized complex float IQ. Capture it as CF32:

```sh
dsd-neo -i airspy:851.375M --iq-capture capture --iq-capture-format cf32
dsd-neo --iq-replay capture.iq.json
```

Metadata records the native backend, selected serial, actual rate, tuning,
and stream discontinuities, and the rate chain the stream ran: the halved rate,
the resampling ratio and the DSP rate after it. Replay runs the same chain the
stream ran live. Replay uses the shared IQ pipeline and does not
require Airspy hardware or libairspy. CU8 capture is not supported for Airspy.
See [IQ capture and replay](iq-capture-replay.md) (Rate Chain And Resampling).

Higher rates increase USB, CPU, memory, and capture-storage usage. Check decode
results and drop metrics on the target host, particularly Android. Hardware
reception and trunk-follow timing still require validation on an R2 and Mini;
the automated tests cover the adapter with a fake SDK, live decoding at the
Airspy's rates through it, and the shared IQ pipeline.
