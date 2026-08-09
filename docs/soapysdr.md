# DSD-neo SoapySDR Guide

This guide covers using non-RTL SDR hardware with `dsd-neo` through the SoapySDR backend.

## When to use this

Use SoapySDR input when your radio is not being accessed directly via `librtlsdr` (for example Airspy, SDRplay,
HackRF, LimeSDR, USRP, PlutoSDR, and other devices with Soapy modules).

Input syntax:

- `-i soapy`
- `-i soapy:<args>` (example: `-i soapy:driver=sdrplay,serial=123456`)
- `-i soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]`

## 0) Install SoapySDR runtime + tools + your radio module

You need three pieces:

- SoapySDR runtime/development libraries, version 0.8.1 or newer
- `SoapySDRUtil` (device discovery/probe tool)
- A SoapySDR module/plugin for your radio (Airspy/SDRplay/HackRF/LimeSDR/etc.)

Using the prebuilt Windows release zip? The runtime library is already bundled — skip to section 0a for how to add
driver modules.

Sanity-check that the tool is installed and Soapy can see your plugins/devices:

```bash
SoapySDRUtil --info
SoapySDRUtil --find
```

If `--find` shows no devices, you likely do not have the right module installed, or Soapy cannot find it.
If you installed modules into a non-standard location, set `SOAPY_SDR_PLUGIN_PATH` and re-run `SoapySDRUtil --info`.

## 0a) Windows release zip: installing driver modules

The Windows release zip ships `SoapySDR.dll` (version 0.8.1, the core library only) in `bin\`. It does **not**
include any radio driver modules or the `SoapySDRUtil.exe` tool, so out of the box `-i soapy` always reports
`SoapySDR: enumerate found no devices`. To use a Soapy radio you must install a module for it yourself.

**Where modules are loaded from.** `SoapySDR.dll` resolves its install root from its own location (one level above
`bin\`), so with the zip extracted to `C:\dsd-neo` it searches:

- `C:\dsd-neo\lib\SoapySDR\modules0.8\` — create this folder and drop module DLLs into it, or
- every directory listed in the `SOAPY_SDR_PLUGIN_PATH` environment variable (semicolon-separated), or
- `%SOAPY_SDR_ROOT%\lib\SoapySDR\modules0.8\` if you set `SOAPY_SDR_ROOT` (it overrides the DLL-relative root).

**The `modules0.8` name is the module ABI version, and it must match.** DSD-neo bundles the SoapySDR 0.8.1
*release*, whose ABI string is plain `0.8`. Module DLLs built against a different ABI are skipped at startup with
an ABI-mismatch warning. In particular, bundles built from SoapySDR git master (for example PothosSDR installers)
use suffixed ABIs such as `0.8-3` — modules taken from those bundles will not load against the shipped
`SoapySDR.dll`. Use modules built against a SoapySDR 0.8.x release, or build the module yourself against the
0.8.1 headers/library from the zip's ecosystem.

**Module dependencies still apply.** A module DLL loads its own backend libraries, which must be findable next to
the module or on `PATH` — for example SDRplay's `sdrplay_api.dll` (install the official SDRplay API/service first)
or `airspy.dll` for Airspy.

**Verifying without `SoapySDRUtil.exe`.** Watch dsd-neo's startup log: a wrong or missing module shows
`SoapySDR: enumerate found no devices`, and ABI mismatches are logged when modules are scanned. If you want the
full `--find`/`--probe` workflow from section 2, install a matching-ABI SoapySDR build separately and point its
`SOAPY_SDR_PLUGIN_PATH` at the same module directory.

## 1) Build with Soapy enabled

```bash
cmake --preset dev-debug -DDSD_ENABLE_SOAPYSDR=ON -DDSD_REQUIRE_SOAPYSDR=ON
cmake --build --preset dev-debug -j
```

During configure, confirm Soapy availability from the status line:

- `SoapySDR backend enabled: ON (available: ON)`

If availability is `OFF`, install SoapySDR 0.8.1 or newer development packages and the module for your radio, then
reconfigure.

## 2) Discover device arguments

Use Soapy tools to find valid argument strings:

```bash
SoapySDRUtil --find
SoapySDRUtil --probe="driver=<driver_name>"
```

Take the key/value pairs you want (for example `driver=...`, `serial=...`) and use them after `soapy:`.

## 3) Configure tuning (important)

In Soapy mode, you can either:

- Use `-i soapy[:args]` for backend/device selection only, then tune through config keys.
- Use `-i soapy[:args]:freq[:gain[:ppm[:bw[:sql[:vol]]]]]` for one-shot CLI startup tuning.

Trailing Soapy tuning fields map to the same shared controls used by RTL/RTL-TCP (`rtl_*` keys).
If your Soapy args string itself contains `:`, prefer config (`soapy_args` + `rtl_*`) to avoid ambiguity.
`--print-config` normalizes this shorthand before rendering, so effective output shows `soapy_args` plus `rtl_*` fields.
For digital decode, SoapySDR uses the same FSK discriminator and CQPSK symbol contracts as RTL USB, RTL-TCP, and IQ
replay.

Minimal config (recommended):

```ini
[input]
source = "soapy"
soapy_args = "driver=sdrplay,serial=123456"
rtl_freq = "851.375M"
```

Optional tuning keys (also shared with RTL/RTL-TCP):

- `rtl_freq`
- `rtl_gain`
- `rtl_ppm`
- `rtl_bw_khz`
- `rtl_sql`
- `rtl_volume` (monitor/non-symbol gain)

Optional Soapy-specific keys:

- `soapy_profile = "auto|generic|airspy|sdrplay|hackrf|lime|pluto|rtlsdr|uhd|sddc"` selects a capability profile.
  `auto` detects from the Soapy driver/hardware strings.
- `soapy_stream_format = "auto|cf32|cs16"` controls RX stream format selection. `auto` prefers the device native
  format when it is `CF32` or `CS16`, then falls back to supported formats.
- `soapy_antenna = "<name>"` selects a listed RX antenna.
- `soapy_clock = "<source>"` selects a listed clock source.
- `soapy_settings = "key=value[,key=value...]"` writes generic Soapy device settings before stream setup. Items may be
  separated with commas or semicolons. Use
  `rx:key=value` or `rx0:key=value` for RX channel 0 settings.
- `soapy_gains = "NAME:dB[,NAME:dB...]"` applies named Soapy gain stages and suppresses aggregate gain changes. Items
  may be separated with commas, semicolons, or spaces.
- `soapy_bandwidth_hz = -1|0|<Hz>` uses profile/default behavior for `-1`, driver automatic/no explicit request for
  `0`, or validates and applies an explicit hardware bandwidth in Hz.
- `digital_resample = "auto|on|off"` controls whether the digital FSK stream is resampled to the resampler target
  (48000 Hz by default). `auto` engages only when the device forces a sample rate that yields a non-integer
  samples-per-symbol; `off` always keeps the raw demod rate; `on` resamples whenever it would help, that is
  whenever the target rate differs from the demod rate and is a multiple of the symbol rate (a target that cannot
  produce an integer samples-per-symbol is bypassed even in `on` mode). CQPSK symbol output is never resampled
  because its timing loop already tracks a fractional SPS.

`soapy_settings` is a strict passthrough to the installed Soapy driver. DSD-neo checks reported setting keys and
option lists when the driver provides metadata, then calls Soapy `writeSetting`. Startup fails for malformed items,
unknown scopes, missing settings, invalid option values, or write errors.

Common SDRplay module examples include:

- `rfnotch_ctrl=true`
- `dabnotch_ctrl=true`
- `biasT_ctrl=false`
- `agc_setpoint=-30`
- `rfgain_sel=4`

Full example:

```ini
[input]
source = "soapy"
soapy_args = "driver=sdrplay,serial=123456"
soapy_profile = "sdrplay"
soapy_stream_format = "auto"
soapy_settings = "rfnotch_ctrl=true,dabnotch_ctrl=true,biasT_ctrl=false,agc_setpoint=-30,rfgain_sel=4"
soapy_gains = "IFGR:35"
soapy_bandwidth_hz = 200000
rtl_freq = "851.375M"
rtl_gain = 22
rtl_ppm = -2
rtl_bw_khz = 24
rtl_sql = 0
rtl_volume = 2
```

Set `rtl_freq` explicitly for predictable startup frequency. `rtl_volume` is a monitor/non-symbol gain field and does not
scale SoapySDR or other RTL-family digital symbols.

## 3a) RX-888 and other SDDC devices

The RX-888 family (RX-888, RX-888 mk2, RX-999, RX-666, BBRF103) is used through the **SoapySDDC** module from
[`ik1xpv/ExtIO_sddc`](https://github.com/ik1xpv/ExtIO_sddc). DSD-neo has no native FX3 driver: the SDDC library is
what uploads the FX3 firmware, reads the raw ADC stream, and downconverts it on the host.

Build the module (Linux needs `libfftw3-dev`), then confirm Soapy sees the radio:

```bash
SoapySDRUtil --find
SoapySDRUtil --probe="driver=SDDC"
```

The device enumerates only when your user can claim the Cypress FX3 USB interface, so install a udev rule for it
if `--find` comes up empty while `lsusb` shows the device.

Working configuration:

```ini
[input]
source = "soapy"
soapy_args = "driver=SDDC"
soapy_profile = "sddc"          # or auto; detected from the SDDC driver/RX888 hardware key
soapy_antenna = "VHF"           # required above 32 MHz (see below)
soapy_gains = "RF:0,IF:24"
soapy_settings = "adc_frequency=98304000"
soapy_bandwidth_hz = 0
rtl_freq = "851.375M"
rtl_bw_khz = 48
```

**Antenna.** SDDC exposes two RX ports: `HF` (direct sampling) and `VHF` (the R828D tuner). It starts on `HF`, and
every DSD-neo protocol lives above 32 MHz, so the tuner port must be selected or the receiver hears nothing. The
`sddc` profile selects `VHF` automatically when no `soapy_antenna` is configured and the startup frequency is above
32 MHz, and logs the choice. An explicit `soapy_antenna` always wins.

**`adc_frequency` and the sample-rate grid.** SoapySDDC derives its rate list from the ADC clock, offering
`{2, 4, 8, 16, 32, 64} MSPS` at the stock 128 MHz clock. DSD-neo asks for 1,536,000 Hz with the default 48 kHz DSP
bandwidth, so the driver would supply 2 MSPS instead, and no power-of-two decimation of 2 MSPS is divisible by
4800 or 6000. Setting `adc_frequency = 98304000` moves the grid to `{1.536, 3.072, 6.144, ...} MSPS`, so the device
delivers exactly the requested rate. `soapy_settings` is applied when the device is opened, before the rate is
programmed, so it takes effect for the first tune.

Without that setting DSD-neo still works: it re-picks decimation for the rate the device actually delivers, and
`digital_resample = "auto"` normalizes the resulting 62,500 Hz stream to 48,000 Hz for an integer SPS. That path
costs extra CPU, so prefer the ADC clock when your SoapySDDC build exposes `adc_frequency` (check
`SoapySDRUtil --probe`).

**Throughput.** SDDC streams raw ADC samples over USB 3.0 and downconverts them on the host, so USB and CPU load
follow `adc_frequency` (roughly 196 MB/s at 98.304 MHz) no matter which output rate DSD-neo requests. A USB 3.0
port and a modern multi-core CPU are required; a USB 2.0 port will not work.

**Limits.** The driver reports no frequency correction, so `rtl_ppm` and auto-PPM are unavailable and DSD-neo
disables auto-PPM with a notice at startup. It has no AGC/gain mode and no hardware bandwidth control: set the `RF`
(attenuator) and `IF` (VGA) stages through `soapy_gains`, and leave `soapy_bandwidth_hz = 0`. The RX stream is
CF32-only, so `--iq-capture` requires `--iq-capture-format cf32`.

## 4) Run

Examples:

```bash
# Use saved config
dsd-neo --config ~/.config/dsd-neo/config.ini --frontend terminal

# One-shot trunking with explicit Soapy args
dsd-neo -fs -i soapy:driver=airspy -T -C connect_plus_chan.csv -G group.csv --frontend terminal

# One-shot Soapy args + startup tuning
dsd-neo -fs -i soapy:driver=airspy:851.375M:22:-2:24:0:2 -T -C connect_plus_chan.csv -G group.csv --frontend terminal
```

## Behavior and limits vs RTL/RTL-TCP

- `rtl_device` index selection is ignored in Soapy mode.
- Some RTL-specific shortcuts are not available in Soapy mode (RTL bias-tee UI/CLI shortcut, direct sampling, offset
  tuning, xtal/IF-gain controls, test mode, RTL-TCP autotune). Use `soapy_settings` for driver-specific controls when
  the Soapy module exposes them, such as SDRplay `biasT_ctrl`.
- Driver capability support varies. Frequency correction (PPM), manual gain mode/range, and bandwidth control may be
  unavailable on some hardware.
- Native SDRplay/Airspy APIs are intentionally out of scope for now; DSD-neo controls non-RTL radios through SoapySDR.
- Requested sample rate/gain may be quantized or clamped by the driver.
- The current backend expects Soapy RX stream format support for `CF32` or `CS16`; `auto` chooses the native format
  first when it is supported.
- IQ capture from Soapy requires an active `CF32` stream and `--iq-capture-format cf32`; the CLI default is `cu8`, so a
  Soapy CF32 capture must request the matching format explicitly. Devices that only expose `CS16` can still be used for
  live decode, but `--iq-capture` is rejected for that stream format.

## Troubleshooting

- `SoapySDR backend unavailable in this build.`:
  Rebuild with Soapy enabled and SoapySDR 0.8.1 or newer installed.
- `SoapySDR: enumerate found no devices ...`:
  Your args likely do not match any available device; verify with `SoapySDRUtil --find`.
- `SoapySDR: invalid args string ...`:
  Fix formatting in `soapy_args` or `-i soapy:<args>`.
- `SoapySDR: invalid soapy_settings ...`, `setting ... is unavailable`, or `failed to write setting`:
  Compare the configured setting keys and values with `SoapySDRUtil --probe="<args>"`. If a setting appears under
  channel settings in the probe output, use the `rx:` or `rx0:` prefix.
- `RX stream formats do not include CF32 or CS16.`:
  The current backend cannot consume that driver stream format.
- `SoapySDR: RX overflow count=...`:
  Host or USB bus is falling behind. Try reducing throughput by lowering `rtl_bw_khz` (config key; for example 48 -> 16)
  and/or overriding tuner bandwidth (env `DSD_NEO_TUNER_BW_HZ=<Hz|auto>`), then reduce system load or adjust driver settings.
- Discovery/plugin issues:
  Confirm `SOAPY_SDR_PLUGIN_PATH` includes the module directory for your Soapy drivers. On Windows release-zip
  installs no modules ship at all — see section 0a for where to put them and the ABI-match requirement.

Manual driver-setting check:

```bash
SoapySDRUtil --probe="driver=sdrplay"
dsd-neo --config ~/.config/dsd-neo/config.ini --frontend terminal
```
