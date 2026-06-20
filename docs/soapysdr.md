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

Sanity-check that the tool is installed and Soapy can see your plugins/devices:

```bash
SoapySDRUtil --info
SoapySDRUtil --find
```

If `--find` shows no devices, you likely do not have the right module installed, or Soapy cannot find it.
If you installed modules into a non-standard location, set `SOAPY_SDR_PLUGIN_PATH` and re-run `SoapySDRUtil --info`.

## 1) Build with Soapy enabled

```bash
cmake --preset dev-debug -DDSD_ENABLE_SOAPYSDR=ON -DDSD_REQUIRE_SOAPYSDR=ON
cmake --build --preset dev-debug -j
```

During configure, confirm Soapy availability from the status line:

- `SoapySDR backend enabled: ON (available: ON)`

If availability is `OFF`, install SoapySDR 0.8.1 or newer development packages and the module for your radio, then
reconfigure. Older 0.7.x CMake package compatibility is intentionally not supported.

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

- `soapy_profile = "auto|generic|airspy|sdrplay|hackrf|lime|pluto|rtlsdr|uhd"` selects a capability profile. `auto`
  detects from the Soapy driver/hardware strings.
- `soapy_stream_format = "auto|cf32|cs16"` controls RX stream format selection. `auto` prefers the device native
  format when it is `CF32` or `CS16`, then falls back to supported formats.
- `soapy_antenna = "<name>"` selects a listed RX antenna.
- `soapy_clock = "<source>"` selects a listed clock source.
- `soapy_settings = "key=value[,key=value...]"` writes generic Soapy device settings before stream setup. Use
  `rx:key=value` or `rx0:key=value` for RX channel 0 settings.
- `soapy_gains = "NAME:dB[,NAME:dB...]"` applies named Soapy gain stages and suppresses aggregate gain changes.
- `soapy_bandwidth_hz = -1|0|<Hz>` uses profile/default behavior for `-1`, driver automatic/no explicit request for
  `0`, or validates and applies an explicit hardware bandwidth in Hz.

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

## 4) Run

Examples:

```bash
# Use saved config
dsd-neo --config ~/.config/dsd-neo/config.ini -N

# One-shot trunking with explicit Soapy args
dsd-neo -fs -i soapy:driver=airspy -T -C connect_plus_chan.csv -G group.csv -N

# One-shot Soapy args + startup tuning
dsd-neo -fs -i soapy:driver=airspy:851.375M:22:-2:24:0:2 -T -C connect_plus_chan.csv -G group.csv -N
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
- IQ capture from Soapy requires an active `CF32` stream. Devices that only expose `CS16` can still be used for live
  decode, but `--iq-capture` is rejected for that stream format.

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
  Confirm `SOAPY_SDR_PLUGIN_PATH` includes the module directory for your Soapy drivers.

Manual driver-setting check:

```bash
SoapySDRUtil --probe="driver=sdrplay"
dsd-neo --config ~/.config/dsd-neo/config.ini -N
```
