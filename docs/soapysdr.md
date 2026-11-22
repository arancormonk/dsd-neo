# DSD-neo SoapySDR Guide

This guide covers using non-RTL SDR hardware with `dsd-neo` through the SoapySDR backend.

## When to use this

Airspy R2 and Mini also have a [native libairspy backend](airspy.md).

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

Using the prebuilt Windows release zip? It already has all three for Airspy and HackRF radios; see section 0a.

Sanity-check that the tool is installed and Soapy can see your plugins/devices:

```bash
SoapySDRUtil --info
SoapySDRUtil --find
```

If `--find` shows no devices, you likely do not have the right module installed, or Soapy cannot find it.
If you installed modules into a non-standard location, set `SOAPY_SDR_PLUGIN_PATH` and re-run `SoapySDRUtil --info`.

## 0a) Windows release zip: bundled modules, and adding others

The Windows release zip ships SoapySDR 0.8.1 with two radio modules and the `SoapySDRUtil.exe` tool:

| Radio | Module (in `lib\SoapySDR\modules0.8\`) | Its library (in `bin\`) | Args |
| --- | --- | --- | --- |
| Airspy R2, Airspy Mini | `airspySupport.dll` (SoapyAirspy 0.2.1) | `airspy.dll`, the one the native Airspy input uses | `driver=airspy` |
| HackRF One | `HackRFSupport.dll` (SoapyHackRF 0.3.6) | `hackrf.dll` | `driver=hackrf` |

`bin\SoapySDR.dll`, `bin\libusb-1.0.dll` and `bin\pthreadVC3.dll` serve both. Other radios need a module you add
yourself (see "Adding another module" below); for an Airspy HF+ or HF+ Discovery that is SoapyAirspyHF. From the
unzipped folder, `bin\SoapySDRUtil.exe --info` lists the modules it found and `bin\SoapySDRUtil.exe --find` lists the
radios plugged in. Windows reaches each radio through the WinUSB driver. An Airspy normally sets it up by itself
([airspy.md](airspy.md)); a radio that `--find` does not list may need WinUSB installed once with
[Zadig](https://zadig.akeo.ie/), as an RTL-SDR does.

What dsd-neo does with each (none of this is tested on real hardware yet):

- **Airspy R2 and Mini** get the Airspy profile and the device's own rate nearest what dsd-neo asks for (2.5 MS/s on
  an R2, 3 MS/s on a Mini). Gains: `LNA`, `MIX`, `VGA`. They also have a native input, `-i airspy`
  ([airspy.md](airspy.md)). Skip silent channels checks them on the native input only, not through SoapySDR (see
  [trunk-scan.md](trunk-scan.md)).
- **HackRF One** gets the HackRF profile and runs at 10 MS/s, whatever DSP bandwidth you pick, except where that does
  not fit (see Sample rates below): its makers list 2 MS/s as its lowest rate and recommend 8 MS/s or more. dsd-neo halves 10 MS/s down to the DSP bandwidth, as it does for an
  Airspy. Its filter opens to 5.5 MHz (see Tuner filter below). That is 20 MB/s over USB, 80 MB/s for a cf32
  `--iq-capture`, and a 32 MiB input buffer (about 419 ms at 10 MS/s). `soapy_profile = "generic"` keeps the old
  behaviour, the listed rate nearest the request: 1 or 2 MS/s, depending on the DSP bandwidth. Gains: `LNA`, `VGA` and the
  14 dB `AMP`; set them with `soapy_gains` (for example `soapy_gains = "LNA:24,VGA:20,AMP:0"`), and keep `AMP` off
  near strong transmitters. Skip silent channels checks it at 10 MS/s.

**Picking a radio.** A bare `-i soapy` opens the first radio found, in driver order: Airspy, then HackRF. With more than one radio plugged in, name it: `-i soapy:driver=hackrf` or
`-i soapy:driver=airspy,serial=...`. To find a radio, a module opens each free radio of its kind for a moment: the
HackRF module opens every free HackRF, reads its serial and turns its transceiver off, even when it then picks another
radio. A radio another program is using is skipped; whether a HackRF streaming in another program is left alone is
untested.

**What the errors mean.**

- `SoapySDR::Device::make() no match`: no loaded module handles that driver (a typo in `driver=`, or a module that did
  not load; run `bin\SoapySDRUtil.exe --info`).
- `Unable to open AirSpy device`, `no hackrf device matches`: the module loaded but found no free radio of that kind
  (unplugged, in use by another program, or without the WinUSB driver).
- `no driver specified and no enumeration results`: a bare `-i soapy` found no radio at all.

**Adding another module.** `SoapySDR.dll` finds its install root from its own location (one level above `bin\`), so
with the zip extracted to `C:\dsd-neo` it loads modules from:

- `C:\dsd-neo\lib\SoapySDR\modules0.8\`, or
- every directory listed in the `SOAPY_SDR_PLUGIN_PATH` environment variable (semicolon-separated), or
- `%SOAPY_SDR_ROOT%\lib\SoapySDR\modules0.8\` if you set `SOAPY_SDR_ROOT` (it overrides the DLL-relative root).

A module you add must be:

- 64-bit, built with the Microsoft compiler (the bundled `MSVCP140.dll` and `VCRUNTIME140.dll` runtime).
- Built for module ABI `0.8` exactly, that is against a SoapySDR 0.8.x release. Modules from SoapySDR development
  builds report ABIs such as `0.8-3` (PothosSDR installers ship these), and SoapySDR 0.9 modules report `0.9`; both
  are skipped with `failed ABI check`. Only one module per driver loads: the first to register wins, and any later
  one is skipped with `duplicate entry`. SoapySDR loads `lib\SoapySDR\modules0.8\` before the
  `SOAPY_SDR_PLUGIN_PATH` folders, so a module there for a bundled driver is skipped and the bundled one stays in use.
  Within one folder it loads files in the order Windows lists them, so which of two modules for the same driver in
  the same folder wins is not defined. To replace a bundled driver, remove the bundled module rather than add a
  second one beside it.
- Able to find its own DLLs: Windows looks for them next to `dsd-neo.exe` (in `bin\`) and on `PATH`, not next to the
  module. SDRplay's module, for example, needs `sdrplay_api.dll` from the official SDRplay API installer on `PATH`.
  Do not put any DLL other than a module in the module folder: SoapySDR loads every DLL there as a module.

**An Airspy HF+ through a module you added.** dsd-neo gives an Airspy HF+ or HF+ Discovery the generic profile (it
does not take it for an Airspy) and the device's rate nearest the request, its highest (768 kS/s on a Discovery). It
has no filter control. The hardware covers 0.5 kHz to 31 MHz and 64 to 260 MHz; the `-i soapy:...:freq` shorthand
starts at 1 MHz, so tune lower through `[input] rtl_freq` (untested). Skip silent channels cannot check it at these
rates.

Long folder names (around 260 characters) or folder names with characters outside your Windows language's code page
can stop SoapySDR from finding its modules; extract the zip to a short path such as `C:\dsd-neo`.

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

**Device rates.** DSD-neo asks the device for the DSP bandwidth times a power of two (1,536,000 Hz at the default
48 kHz). Many devices run only at rates of their own, for example 2 MS/s on an SDRplay or an RX-888, or 2.5 or 3 MS/s
on an Airspy. DSD-neo halves such a rate as far as it can while staying at or above the DSP bandwidth, then resamples
what is left to the DSP bandwidth exactly: 2 MS/s halves to 62,500 Hz, which is resampled to 48,000 Hz. So the
decoders, the symbol timing and the analog channel widths run at the DSP bandwidth, as on an RTL-SDR. The rate the
device reports back decides this (a report within 100 ppm of the rate written counts as that rate), and a retune that
changes it plans again. A device slower than the DSP bandwidth does not start (`the device runs at 62500 Hz, slower
than the 72000 Hz DSP bandwidth; choose a smaller DSP bandwidth`), nor does
one whose rate cannot be resampled to the bandwidth within 5 ppm (`dsd-neo cannot convert the device's ... Hz to the
48000 Hz DSP bandwidth closely enough ...; choose another DSP bandwidth`); every rate grid listed here converts
exactly. A profile can prefer a rate of its own: the HackRF profile asks for 10 MS/s when the radio offers that rate,
and dsd-neo halves it down the same way. It is a preference, not a promise. Near the top of the tuning range, where 10
MS/s would put the capture (a quarter of the rate above the channel) past the highest frequency dsd-neo can tune, the
HackRF is asked instead for the rate it offers nearest the plain request among those that fit (2 MS/s at the default
DSP bandwidth). When no rate it offers fits there, dsd-neo still asks for one and lets the rate the radio reports
decide: a radio that delivers a slower rate than it offers can still tune the channel. And the radio's read-back decides
in the end: the rate it reports is the rate dsd-neo runs, if the chain
can use it. Up to 10 MS/s is accepted, checked on the rate the radio reports back;
a radio that reports more does not start (`the device runs ... Hz, above the 10000000 Hz capture ceiling`), and dsd-neo
sizes its input buffer for the rate the radio delivers. A retune keeps to what that buffer holds: a HackRF that started
at 2 MS/s near the top of the range stays at 2 MS/s when you tune further down, where 10 MS/s would fit the range but
not the buffer. A retune whose reported rate still moves above what the buffer holds is refused and the radio stays where
it was; restart the stream to use the new rate.

**Tuner filter.** A SoapySDR capture is tuned a quarter of the sample rate above the channel (dsd-neo moves it back in
software, which keeps the channel away from the radio's centre spike), so the radio's filter has to keep a channel
that far off its centre: at least half the sample rate plus 72 kHz, the widest DSP bandwidth. With `soapy_bandwidth_hz`
at its default (`-1`), a profile's filter narrower than that opens to the narrowest filter the radio offers at or
above it, and the start says so once:

- HackRF at 10 MS/s: 5.5 MHz (was 1.75 MHz).
- SDRplay at 2 MS/s: 1.536 MHz; at 1 MS/s: 600 kHz (was 200 kHz).
- ADALM-Pluto, which runs the rate dsd-neo asks for (1.536 MS/s, say): 1 MHz (was 200 kHz).
- LimeSDR: unchanged; its narrowest filter, 1.4 MHz, is already wide enough at the rates dsd-neo runs it.

```text
SoapySDR: opened the profile's 1750000 Hz bandwidth to 5500000 Hz for the channel 2500000 Hz off centre.
```

The SDRplay and ADALM-Pluto figures come from their drivers' filter lists; they are not tested on those radios. An
explicit `soapy_bandwidth_hz` (or `DSD_NEO_TUNER_BW_HZ`) is applied as the radio takes it. dsd-neo judges the filter the
radio reports after the write (where its driver reports one): if that is narrower, whether you set it, the radio kept
its old filter, or even its widest filter is too narrow, dsd-neo warns once for that filter and rate:

```text
SoapySDR: bandwidth 5000000 Hz is narrower than 5072000 Hz, which keeps the channel (2500000 Hz off centre, a quarter of the 10000000 Hz sample rate) at every DSP bandwidth.
```

A filter a little narrower than that figure can still keep a narrower DSP bandwidth's channel whole; the figure is for
the widest. `soapy_bandwidth_hz = 0` leaves the filter to the driver. With `DSD_NEO_DISABLE_FS4_SHIFT=1` the capture is
tuned on the channel itself, so none of this applies: profile filters stay as they are and nothing is warned about.

**Retunes.** A radio can still hand over samples from before a retune; dsd-neo drops the first 120 ms after one. A
HackRF's driver queues 15 buffers of samples by default (about 197 ms at 10 MS/s, on top of about 52 ms the USB
transfers hold), so dsd-neo asks it for 4 (`buffers=4`): with the transfers and the one buffer dsd-neo may be holding
when the retune comes, that is about 118 ms at 10 MS/s, inside the 120 ms. At a lower rate (`soapy_profile =
"generic"`) the same samples take longer, and dsd-neo drops as many as they hold (about 590 ms at 2 MS/s). The cost is
a smaller cushion when the computer falls behind: past about 52 ms the HackRF driver drops samples and dsd-neo logs an
overflow. That 52 ms figure comes from the driver's sources and has not been measured on a HackRF.

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
- `rtl_bw_khz` (4, 6, 8, 12, 16, 24, 48 or 72 kHz; 72 is TETRA's, for which DSD-neo asks the device for 1,152,000 Hz.
  A device whose rate grid has no such rate is resampled to 72 kHz as above, so TETRA decodes on it too)
- `rtl_sql`
- `rtl_volume` (monitor/non-symbol gain)

Optional Soapy-specific keys:

- `soapy_profile = "auto|generic|airspy|sdrplay|hackrf|lime|pluto|rtlsdr|uhd|sddc"` selects a capability profile.
  `auto` detects from the Soapy driver/hardware strings; an Airspy HF+ gets `generic`. `hackrf` runs a HackRF at
  10 MS/s; `generic` on a HackRF keeps the rate nearest the request.
- `soapy_stream_format = "auto|cf32|cs16"` controls RX stream format selection. `auto` prefers the device native
  format when it is `CF32` or `CS16`, then falls back to supported formats. See [Signal levels on a CS16
  stream](#signal-levels-on-a-cs16-stream) for how `CS16` samples are scaled.
- `soapy_antenna = "<name>"` selects a listed RX antenna.
- `soapy_clock = "<source>"` selects a listed clock source.
- `soapy_settings = "key=value[,key=value...]"` writes generic Soapy device settings before stream setup. Items may be
  separated with commas or semicolons. Use
  `rx:key=value` or `rx0:key=value` for RX channel 0 settings.
- `soapy_gains = "NAME:dB[,NAME:dB...]"` applies named Soapy gain stages and suppresses aggregate gain changes. Items
  may be separated with commas, semicolons, or spaces.
- `soapy_bandwidth_hz = -1|0|<Hz>` uses the profile's filter for `-1` (opened for the quarter-rate offset, see Tuner
  filter above), driver automatic/no explicit request for `0`, or validates and applies an explicit hardware bandwidth
  in Hz (with a warning when it is narrower than the offset channel needs).
- `digital_resample = "auto|on|off"` controls whether the digital FSK stream is resampled to the resampler target
  (48000 Hz by default). The stream runs at the DSP bandwidth (see Device rates above), so `auto` engages only when
  the device picked its own rate and the DSP bandwidth yields a non-integer samples-per-symbol (12 kHz for
  4800 sym/s); `off` always keeps the raw demod rate; `on` resamples whenever it would help, that is
  whenever the target rate differs from the demod rate and is a multiple of the symbol rate (a target that cannot
  produce an integer samples-per-symbol is bypassed even in `on` mode). CQPSK symbol output is never resampled: its
  symbol timing runs on the complex stream before any output resampling, and needs a whole number of samples per
  symbol there. At a DSP bandwidth that gives none (16 or 12 kHz for P25 Phase 1) the stream warns at start:
  `WARNING: CQPSK cannot time symbols at 16000 Hz (3.333 samples per symbol for 4800 sym/s); use a DSP bandwidth of
  24, 48 or 72 kHz.`

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
rtl_freq = "851.375M"
rtl_gain = 22
rtl_ppm = -2
rtl_bw_khz = 24
rtl_sql = 0
rtl_volume = 2
```

Set `rtl_freq` explicitly for predictable startup frequency. `rtl_volume` is a monitor/non-symbol gain field and does not
scale SoapySDR or other RTL-family digital symbols.

### Signal levels on a CS16 stream

A `CS16` stream hands DSD-neo whole numbers, and the driver says how large they can get. `SoapySDRUtil --probe`
shows it as `Native format: CS16 [full-scale=2048]`, and DSD-neo logs it at start as
`native=CS16 [full-scale=2048]`.

- When `CS16` is the radio's native format, DSD-neo divides each sample by that full scale. A sample at the radio's
  limit then reads as 0 dBFS, as it does on a `CF32` stream.
- When `CS16` is not native (for example an RTL-SDR forced to `cs16`), or the driver reports no usable full scale,
  DSD-neo divides by 32768, the scale SoapySDR's own converters use.
- The input level meter counts a `CS16` sample as clipping once it reaches 99% of full scale. That catches both
  limits of every radio, also the ones whose top value sits a little below the bottom one.

PlutoSDR and bladeRF send 12-bit samples with a full scale of 2048. DSD-neo used to divide them by 32768, so on
`CS16` (what `auto` picks for them) every level read about 24 dB too low, and the meter never showed HOT or
CLIPPING even when the radio was overloaded. They are now scaled by 2048:

- On a PlutoSDR or bladeRF with a `CS16` stream, every level reads about 24 dB higher than before: the input level
  meter, RF level and the low-level advice.
- A level squelch set on one of these radios needs raising by about 24 dB, for example from -80 to -56. That covers
  `rtl_sql` (or `sql` in the `-i soapy:` spec) and a scan row's `--squelch-db`. Auto and noise squelch learn their
  level from the signal and need no change.
- Decoding and audio do not change.
- If you set `soapy_stream_format = "cf32"` to work around the low levels, nothing changes for you: the driver
  already divided by 2048 on `CF32`. You can keep it, or go back to `auto`.

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

Without that setting DSD-neo still works: it re-picks decimation for the rate the device actually delivers and
resamples the resulting 62,500 Hz stream to 48,000 Hz (see Device rates above). Halving 2 MSPS and resampling it costs
a little more CPU than halving 1.536 MSPS, so prefer the ADC clock when your SoapySDDC build exposes `adc_frequency`
(check `SoapySDRUtil --probe`).

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
- DSD-neo has no native SDRplay input; SDRplay radios run through SoapySDR. Airspy R2 and Mini can run through
  SoapySDR too, or through DSD-neo's native Airspy input (see [Native Airspy R2 and Mini](airspy.md)).
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
  The computer or the USB bus is falling behind the radio. For most radios a lower `rtl_bw_khz` (config key; for
  example 48 -> 16) asks for a lower sample rate. A HackRF runs 10 MS/s whatever `rtl_bw_khz` is; to lower its rate,
  set `soapy_profile = "generic"` (1 or 2 MS/s). The tuner filter (`DSD_NEO_TUNER_BW_HZ`) does not change the USB
  traffic, and a narrow one can cut out the channel. Otherwise plug the radio into a USB port of its own (not a hub),
  close other programs that load the computer, or adjust the driver's settings.
- Discovery/plugin issues:
  Confirm `SOAPY_SDR_PLUGIN_PATH` includes the module directory for your Soapy drivers. The Windows release zip ships
  the Airspy and HackRF modules and `bin\SoapySDRUtil.exe`; see section 0a for adding others and the ABI-match
  requirement.

Manual driver-setting check:

```bash
SoapySDRUtil --probe="driver=sdrplay"
dsd-neo --config ~/.config/dsd-neo/config.ini --frontend terminal
```
