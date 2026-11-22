# SPDX-License-Identifier: GPL-3.0-or-later
"""Signal search identifies TETRA on a paced rtl_tcp source, end to end (issue #748).

The scene is tools/bench_scan_skip_silent.py's paced rtl_tcp source (its Scene) with transmitters of this script's
own (tools/bench_signal_search_tcp.py's Transmitter and run_rig()). Each is a committed fixture read in its own format
(cu8 or cs16) and rate, resampled to the capture rate the receiver asks the server for, and placed at its frequency
relative to every tune; a receiver tuned anywhere hears the scene's noise plus every transmitter its capture holds.
Each transmitter is on the air, looped, from the start, and stops ON_S after the search first lands on it.

dsd-neo_test_signal_search_rig runs the search the way the app starts one: the decoder configured from a command line
(-i rtltcp:...:72, the 72 kHz DSP bandwidth the app starts a search with a TETRA range at, nothing else), the
search submitted through the app-control queue, published by the app-control observer to the native history store.
It prints the store's status as it changes and, at the end, the history and the history read back from the journal.
Two runs (the real-air call fixtures share their carrier frequency, 426.7 MHz, which a call needs to be its own):

  run a, ranges in this order:
    nfm       analog only  nfm_tone_synth                      an analog visit first
    dmr       Auto         dmr_voice                           an Auto visit, DMR, before the TETRA ranges
    tmo       TETRA        tetra_call_real_air_72k  426.7 MHz  TETRA, CC 1, main carrier 426.70000 MHz, MCC 228/8889
    cc144     TETRA        tetra_cc_marginal_real_air_144k     the marginal real-air control channel, cs16 at 144 kHz
    cell      TETRA        tetra_bsch_synth_72k twice          a two-carrier cell: its advertised main carrier
                                                               (408.03125 MHz) and a traffic carrier 100 kHz above,
                                                               whose site line names the other frequency
    dmo       TETRA        tetra_dmo_call_synth_72k            TETRA DMO, Direct, no access code
    noise     TETRA        band-limited noise, 24 kHz wide      a visit that confirms nothing
  run b: tmo with tetra_call_awgn_72k (the same call at 3 dB SNR over 72 kHz), then noise again.

Checks: every transmitter is visited and has one history row at its frequency; the TETRA ones are confirmed with
their protocol, access code and site line, and the live status shows them confirmed while their carrier is on; the
noise is not confirmed, and its visit ends "digital unresolved"; the history read back from the journal matches the live
one; the front end ran the source at the 72 kHz capture rate alone; and a confirmed TETRA visit leaves the channel
TETRA_QUIET_S (2.1 s) after its carrier stops, not the 0.5 s an ordinary visit would, within LEAVE_PAD_S.

Usage: signal_search_tetra_rig_check.py RIG FIXTURE_DIR [--keep DIR] [--only a|b]
Exit 0 on success, 1 on a failed check, 77 when numpy is missing. Serves the loopback interface only and never opens
hardware.
"""

import argparse
import json
from pathlib import Path
import re
import sys
import tempfile

try:
    import numpy  # noqa: F401
except ImportError:
    print(f"SKIP: the rig needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import bench_signal_search_tcp as tcp_bench  # noqa: E402

DSP_KHZ = 72
# The capture rate the RTL front end asks the source for at a 72 kHz DSP bandwidth (16 times it).
CAPTURE_HZ = 1152000
assert tcp_bench.capture_rate(DSP_KHZ) == CAPTURE_HZ
SNR_DB = 30.0
ON_S = 4.0
NOISE_ON_S = 6.0
TETRA_QUIET_S = 2.1
# The decoder reads a little behind the paced stream, and its quiet timing runs on its own clock: this much past the
# quiet time still passes. Under it on the other side: the last burst before the stop can be up to a slot early.
LEAVE_PAD_S = 1.5
LEAVE_EARLY_S = 0.3
RUN_SECONDS = 150
# More than a run reaches: the run ends once every transmitter has been visited and left.
PASSES = 1000
DOT = " · "
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:")


def tetra_site(main, mcc, mnc):
    """The site line of a TMO cell: its main carrier first, an LA after the MNC when SYSINFO carried one."""
    return re.compile("^" + re.escape(f"Main carrier {main}{DOT}MCC {mcc}{DOT}MNC {mnc}") + f"({re.escape(DOT)}LA \\d+)?$")


# id, range digital (0 analog, 1 Auto, 10 TETRA), fixture (None: noise), frequency Hz, range half-width Hz, step Hz,
# what the history row must show (protocol, access, site regex or "" for none, confirmed), how long it stays on after
# the first landing.
TMO_SITE = tetra_site("426.70000 MHz", 228, 8889)
CELL_SITE = tetra_site("408.03125 MHz", 460, 4242)
RUN_A = (
    ("nfm", 0, "nfm_tone_synth", 446100000, 25000, 12500, None, ON_S),
    ("dmr", 1, "dmr_voice", 448100000, 25000, 12500, ("DMR", None, None, 1), ON_S),
    ("tmo", 10, "tetra_call_real_air_72k", 426700000, 50000, 25000, ("TETRA", "CC 1", TMO_SITE, 1), ON_S),
    ("cc144", 10, "tetra_cc_marginal_real_air_144k", 467912500, 50000, 12500,
     ("TETRA", "CC 44", tetra_site("467.91250 MHz", 250, 13), 1), ON_S),
    ("cell", 10, "tetra_bsch_synth_72k", 408031250, 25000, 25000, ("TETRA", "CC 17", CELL_SITE, 1), ON_S),
    ("traffic", 10, "tetra_bsch_synth_72k", 408131250, 25000, 25000, ("TETRA", "CC 17", CELL_SITE, 1), ON_S),
    ("dmo", 10, "tetra_dmo_call_synth_72k", 433000000, 25000, 25000, ("TETRA DMO", "", re.compile("^Direct$"), 1), ON_S),
    ("noise", 10, None, 440000000, 25000, 25000, (None, None, None, 0), NOISE_ON_S),
)
RUN_B = (
    ("awgn", 10, "tetra_call_awgn_72k", 426700000, 50000, 25000, ("TETRA", "CC 1", TMO_SITE, 1), ON_S),
    ("noise", 10, None, 440000000, 25000, 25000, (None, None, None, 0), NOISE_ON_S),
)
# The two carriers of the synthetic cell sit in one range.
SHARED_RANGE = {"traffic": "cell"}


class Transmitter(tcp_bench.Transmitter):
    """A scene transmitter with what its range searches for and what its detection must show."""

    def __init__(self, spec, fixture_dir):
        name, self.digital, fixture, frequency, self.half_width, step, self.expect, on_s = spec
        super().__init__(name, frequency, fixture, snr_db=SNR_DB, on_s=on_s, step=step, fixture_dir=fixture_dir)


def ranges_of(transmitters):
    out = []
    for tx in transmitters:
        if tx.id in SHARED_RANGE:
            continue
        top = max(t.frequency for t in transmitters if t is tx or SHARED_RANGE.get(t.id) == tx.id)
        probe = 4000 if tx.digital == 10 else 3000
        width = 25000 if tx.digital == 10 else 16000
        out.append((tx.id, tx.frequency - tx.half_width, top + tx.half_width, tx.step, width, tx.digital, probe,
                    12000))
    return out


def run(args, name, specs, work):
    transmitters = [Transmitter(spec, args.fixture_dir) for spec in specs]

    def done(scene):
        # Every transmitter visited, stopped and given time to be left (the search keeps going round meanwhile: a
        # bursty DMO call passes the RF detector only at some burst phases).
        stops = [tx.stop_at for tx in transmitters]
        return None not in stops and scene.sample_time > max(stops) + TETRA_QUIET_S + LEAVE_PAD_S + 1.0

    folder = work / name
    folder.mkdir(parents=True, exist_ok=True)
    record = tcp_bench.run_rig(args.rig, transmitters, ranges_of(transmitters), DSP_KHZ, folder, RUN_SECONDS,
                               done=done, passes=PASSES)
    return dict(record, name=name, transmitters=transmitters)


def departure(record, tx):
    """Seconds from the transmitter's stop to the first retune after it, or None."""
    if tx.stop_at is None:
        return None
    later = [t for t, _wall, _centre, _rate in record["tunes"] if t > tx.stop_at]
    return later[0] - tx.stop_at if later else None


def check_row(tx, rows, failures):
    mine = [r for r in rows if tx.owns(r["frequency_hz"])]
    if tx.landed is None:
        failures.append(f"{tx.id}: never visited")
        return
    if len(mine) != 1:
        failures.append(f"{tx.id}: {len(mine)} history rows at {tx.frequency} Hz, not one: {mine}")
        return
    row = mine[0]
    if tx.expect is None:
        return
    protocol, access, site, confirmed = tx.expect
    if row["confirmed"] != confirmed:
        failures.append(f"{tx.id}: confirmed {row['confirmed']}, not {confirmed}: {row}")
    if protocol is None:
        # The TETRA probe heard nothing it could check, so the visit fell back to NFM: "digital unresolved".
        if row["protocol"] or row["site"] or row["access"] or not row["unresolved"]:
            failures.append(f"{tx.id}: noise showed {row}")
        return
    if row["protocol"] != protocol:
        failures.append(f"{tx.id}: protocol {row['protocol']!r}, not {protocol!r}")
    if access is not None and row["access"] != access:
        failures.append(f"{tx.id}: access {row['access']!r}, not {access!r}")
    if site is not None and not site.match(row["site"]):
        failures.append(f"{tx.id}: site {row['site']!r} does not match {site.pattern!r}")
    if not row["ended"]:
        failures.append(f"{tx.id}: the history row never ended")


def check(record):
    failures = []
    text = "\n".join(line for _t, _wall, line, _m in record["lines"])
    if SANITIZER.search(text):
        failures.append("sanitizer report in the rig's output")
    if record["exit"] != 0:
        failures.append(f"the rig exited with {record['exit']}")
    rates = {rate for _t, _w, _c, rate in record["tunes"][1:]}
    if rates != {CAPTURE_HZ}:
        failures.append(f"the front end ran the source at {sorted(rates)} Hz, not {CAPTURE_HZ} Hz alone")
    history, reload = tcp_bench.rig_rows(record, "history"), tcp_bench.rig_rows(record, "reload")
    if [(r["frequency_hz"], r["protocol"], r["access"], r["site"], r["confirmed"]) for r in history] != [
        (r["frequency_hz"], r["protocol"], r["access"], r["site"], r["confirmed"]) for r in reload
    ]:
        failures.append(f"the journal read back {reload}, not {history}")
    for tx in record["transmitters"]:
        check_row(tx, history, failures)
        # Shown confirmed while the visit runs (the Now panel's "TETRA · confirmed"), not only once it ends.
        confirmed = tcp_bench.confirm_time(record, tx)
        if tx.expect and tx.expect[3] and (confirmed is None or confirmed >= tx.on_s):
            failures.append(f"{tx.id}: the live status never showed it confirmed while its carrier was on")
        if tx.expect is None or not tx.expect[0] or not tx.expect[0].startswith("TETRA"):
            continue
        left = departure(record, tx)
        if left is None:
            failures.append(f"{tx.id}: no retune after its carrier stopped")
        elif not TETRA_QUIET_S - LEAVE_EARLY_S <= left <= TETRA_QUIET_S + LEAVE_PAD_S:
            failures.append(f"{tx.id}: left {left:.2f} s after its carrier stopped, not about {TETRA_QUIET_S} s")
    return failures


def report(record):
    print(f"run {record['name']}:")
    rows = tcp_bench.rig_rows(record, "history")
    for tx in record["transmitters"]:
        left = departure(record, tx)
        mine = [r for r in rows if tx.owns(r["frequency_hz"])]
        shown = "; ".join(f"{r['protocol'] or '-'} confirmed={r['confirmed']} access={r['access']!r} "
                          f"site={r['site']!r}" for r in mine)
        landed = "never" if tx.landed is None else f"{tx.landed:6.2f} s"
        left_text = "-" if left is None else f"{left:.2f} s"
        confirm = tcp_bench.confirm_time(record, tx)
        confirm_text = "-" if confirm is None else f"{confirm:.2f} s"
        print(f"  {tx.id:8s} {tx.frequency / 1e6:10.5f} MHz landed {landed} confirmed {confirm_text} later, left "
              f"{left_text} after stop: {shown}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("rig")
    parser.add_argument("fixture_dir", type=Path)
    parser.add_argument("--keep", type=Path, help="keep each run's journal and a record.json in this folder")
    parser.add_argument("--only", choices=("a", "b"), help="run only one of the two scenes")
    args = parser.parse_args()
    runs = [("a", RUN_A), ("b", RUN_B)]
    if args.only:
        runs = [r for r in runs if r[0] == args.only]
    failures = []
    with tempfile.TemporaryDirectory(prefix="search-rig-") as scratch:
        work = args.keep if args.keep else Path(scratch)
        work.mkdir(parents=True, exist_ok=True)
        for name, specs in runs:
            record = run(args, name, specs, work)
            report(record)
            failed = check(record)
            if args.keep:
                kept = dict(record, transmitters=[(t.id, t.frequency, t.landed) for t in record["transmitters"]])
                (work / f"record-{name}.json").write_text(json.dumps(kept, default=str), encoding="utf-8")
            if failed:
                for _t, _wall, line, _m in record["lines"][-40:]:
                    print(line)
            failures += [f"run {name}: {f}" for f in failed]
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    print("OK: every TETRA transmitter confirmed with its access code and site line, the noise confirmed nothing, the "
          "journal read back the same rows, and each TETRA visit left 2.1 s after its carrier stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
