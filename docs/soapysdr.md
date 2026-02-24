# DSD-neo SoapySDR Guide

This guide covers using non-RTL SDR hardware with `dsd-neo` through the SoapySDR backend.

## When to use this

Use SoapySDR input when your radio is not being accessed directly via `librtlsdr` (for example Airspy, SDRplay,
HackRF, LimeSDR, USRP, PlutoSDR, and other devices with Soapy modules).

Input syntax:

- `-i soapy`
- `-i soapy:<args>` (example: `-i soapy:driver=sdrplay,serial=123456`)

## 0) Install SoapySDR runtime + tools + your radio module

You need three pieces:

- SoapySDR runtime libraries
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

If availability is `OFF`, install SoapySDR development packages and the module for your radio, then reconfigure.

## 2) Discover device arguments

Use Soapy tools to find valid argument strings:

```bash
SoapySDRUtil --find
SoapySDRUtil --probe="driver=<driver_name>"
```

Take the key/value pairs you want (for example `driver=...`, `serial=...`) and use them after `soapy:`.

## 3) Configure tuning (important)

In Soapy mode, the `-i soapy[:args]` string only selects the backend/device. It does **not** parse
`freq:gain:ppm:bw:sql:vol` like `rtl:` and `rtltcp:`.

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
- `rtl_volume`

Full example:

```ini
[input]
source = "soapy"
soapy_args = "driver=sdrplay,serial=123456"
rtl_freq = "851.375M"
rtl_gain = 22
rtl_ppm = -2
rtl_bw_khz = 24
rtl_sql = 0
rtl_volume = 2
```

Set `rtl_freq` explicitly for predictable startup frequency.

## 4) Run

Examples:

```bash
# Use saved config
dsd-neo --config ~/.config/dsd-neo/config.ini -N

# One-shot trunking with explicit Soapy args
dsd-neo -fs -i soapy:driver=airspy -T -C connect_plus_chan.csv -G group.csv -N
```

## Behavior and limits vs RTL/RTL-TCP

- `rtl_device` index selection is ignored in Soapy mode.
- Some RTL-specific controls are not available in Soapy mode (bias tee, direct sampling, offset tuning, xtal/IF-gain
  controls, test mode, RTL-TCP autotune).
- Driver capability support varies. Frequency correction (PPM), manual gain mode/range, and bandwidth control may be
  unavailable on some hardware.
- Requested sample rate/gain may be quantized or clamped by the driver.
- The current backend expects Soapy RX stream format support for `CF32` or `CS16`.

## Troubleshooting

- `SoapySDR backend unavailable in this build.`:
  Rebuild with Soapy enabled and installed.
- `SoapySDR: enumerate found no devices ...`:
  Your args likely do not match any available device; verify with `SoapySDRUtil --find`.
- `SoapySDR: invalid args string ...`:
  Fix formatting in `soapy_args` or `-i soapy:<args>`.
- `RX stream formats do not include CF32 or CS16.`:
  The current backend cannot consume that driver stream format.
- `SoapySDR: RX overflow count=...`:
  Host or USB bus is falling behind. Try reducing throughput by lowering `rtl_bw_khz` (config key; for example 48 -> 16)
  and/or overriding tuner bandwidth (env `DSD_NEO_TUNER_BW_HZ=<Hz|auto>`), then reduce system load or adjust driver settings.
- Discovery/plugin issues:
  Confirm `SOAPY_SDR_PLUGIN_PATH` includes the module directory for your Soapy drivers.
