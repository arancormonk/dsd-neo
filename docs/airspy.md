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
rules supplied by the driver package. On Windows the device must be accessible
through WinUSB. Android obtains USB permission through its normal system dialog;
the application's native code uses the descriptor owned by the USB manager.

## Select and tune

```sh
dsd-neo --airspy-list
dsd-neo -i airspy:851.375M -ft --frontend terminal
dsd-neo -i airspy:serial=0123456789ABCDEF:851.375M -ft --frontend terminal
```

Input syntax is `airspy[:serial=<16 hexadecimal digits>][:frequency[:bw[:sql[:vol]]]]`.
The optional trailing fields are DSP bandwidth in kHz, squelch in dB (0 disables),
and monitor volume (0–3); gain uses the separate native controls. Bandwidth must
be one of 4, 6, 8, 12, 16, 24, or 48 kHz; other values warn and fall back to 48.
Numeric volume values are clamped to 0–3. Invalid squelch or volume text warns
and keeps the previous/default value; invalid frequencies still prevent startup.
An explicit serial must match exactly. Without one, the first enumerated device
is selected and its serial is logged. Frequencies use the usual Hz/K/M/G syntax;
the native tuning range is 24–1700 MHz.

Terminal users can also select Airspy under **Input → Switch source** and adjust
it under **Input → Airspy receiver controls**. Qt and Android expose an Airspy
source in saved systems, exploration, and scan lists, with native receiver
controls in setup and the live radio panel.

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
  separate from DSP bandwidth. The decoder adapts its rate chain to the actual
  hardware rate and normalizes fractional FSK samples-per-symbol when needed.
  Applying a config during a live Airspy session retunes frequency and updates
  squelch immediately. Bandwidth or monitor-volume changes restart the stream,
  combined with any serial or sample-rate change in the same restart. A failed
  reopen restores the previous receiver settings; other config changes, including
  audio output, still finish applying and the command reports failure.
- **Unsupported controls:** libairspy has no PPM-correction API. Auto-PPM and
  RTL-specific direct sampling, oscillator, and tuner controls do not apply.

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
and stream discontinuities. Replay uses the shared IQ pipeline and does not
require Airspy hardware or libairspy. CU8 capture is not supported for Airspy.

Higher rates increase USB, CPU, memory, and capture-storage usage. Check decode
results and drop metrics on the target host, particularly Android. Hardware
reception and trunk-follow timing still require validation on an R2 and Mini;
the automated tests cover the adapter with a fake SDK and the shared IQ pipeline.
