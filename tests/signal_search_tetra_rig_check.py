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
  run b: tmo with tetra_call_awgn_72k (the same call at 3 dB SNR over 72 kHz), then noise again, then
    dmo_ph0..6  TETRA      a DMO call's traffic (one burst a frame in slot 1, DM-OCCUPIED every 340 ms), its bursts
                           at a fixed phase against each tune, an eighth of a frame (7.1 ms) on at the next, the four
                           calls starting a quarter frame apart: each found and confirmed TETRA DMO, Direct, on some
                           pass (by DMO_MAX_PASS), whichever phases the span measurements and the landing meet it at
    pres_on     TETRA      a repeater's presence signal train (DPRES-SYNC in slots 1 and 3): TETRA DMO, presence signal
    pres_end    TETRA      the same train, off from the search's landing tune on: confirms nothing, ends unresolved

Checks: every transmitter is visited and has one history row at its frequency; the TETRA ones are confirmed with
their protocol, access code and site line, and the live status shows them confirmed while their carrier is on; the
noise is not confirmed, and its visit ends "digital unresolved"; the history read back from the journal matches the live
one; the front end ran the source at the 72 kHz capture rate alone; and a confirmed TETRA visit leaves the channel
TETRA_QUIET_S (2.1 s) after its carrier stops, not the 0.5 s an ordinary visit would, within LEAVE_PAD_S. A DMO visit's
activity refreshes only on checked call content, so a DMO visit is timed from the last such burst the scene sent before
the stop (the call fixture's loop has no call content for 0.64 s around its set-up). A presence signal never
refreshes the visit's activity, so pres_on leaves TETRA_QUIET_S after its first confirmation, with its train still on,
not 2.1 s after it stops.

Usage: signal_search_tetra_rig_check.py RIG FIXTURE_DIR [--keep DIR] [--only a|b]
Exit 0 on success, 1 on a failed check, 77 when numpy is missing. Serves the loopback interface only and never opens
hardware.
"""

import argparse
import importlib
import importlib.util
import json
import math
from pathlib import Path
import re
import sys
import tempfile

if importlib.util.find_spec("numpy") is None:
    print(f"SKIP: the rig needs numpy, which {sys.executable} does not have")
    sys.exit(77)

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

tcp_bench = importlib.import_module("bench_signal_search_tcp")
bif = importlib.import_module("build_iq_fixtures")
# The scene's block: it plays a transmitter a whole block at a time.
BLOCK = importlib.import_module("bench_scan_skip_silent").BLOCK

DSP_KHZ = 72
# The capture rate the RTL front end asks the source for at a 72 kHz DSP bandwidth (16 times it).
CAPTURE_HZ = 1152000
assert tcp_bench.capture_rate(DSP_KHZ) == CAPTURE_HZ
SNR_DB = 30.0
ON_S = 4.0
NOISE_ON_S = 6.0
TETRA_QUIET_S = 2.1
# The decoder reads a little behind the paced stream, and its quiet timing runs on its own clock: this much past the
# quiet time still passes. Under it on the other side: a TMO downlink is continuous, so the last block that refreshes
# the visit can come up to a slot (14 ms) before the stop, and the retune is stamped when the source hears it. A DMO
# visit is timed from the last refreshing burst the scene sent, not from the stop (dmo_refresh_ends()). Tight enough
# either way to catch a quiet time that is wrong: an ordinary visit's 0.5 s, or one twice as long.
LEAVE_PAD_S = 1.5
LEAVE_EARLY_S = 0.3
RUN_SECONDS = 180
# A DMO frame (four timeslots, 56.7 ms), and the step a phase-stepped DMO call's bursts move by against the tune at
# each tune that holds it (once a pass): an eighth of a frame, 7.1 ms, finer than the RF detector's two 10 ms windows.
DMO_FRAME_S = 4 * bif.TETRA_SLOT_SYMBOLS / bif.TETRA_SYMBOL_RATE
DMO_PHASE_STEP_S = DMO_FRAME_S / 8
# A call must land by this pass: eight times round its eight phases. Only about one phase in eight puts a burst across
# both of the RF detector's 10 ms windows, and where the windows fall after a tune moves by a block or so from tune to
# tune, so a call can miss its phase's turn: twenty calls over five runs landed on passes 1 to 15.
DMO_MAX_PASS = 64
# More than a run reaches: the run ends once every transmitter has been visited and left.
PASSES = 1000
DOT = " · "
SANITIZER = re.compile(r"AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:")


def tetra_site(main, mcc, mnc):
    """The site line of a TMO cell: its main carrier first, an LA after the MNC when SYSINFO carried one."""
    head = re.escape(f"Main carrier {main}{DOT}MCC {mcc}{DOT}MNC {mnc}")
    return re.compile("^" + head + f"({re.escape(DOT)}LA \\d+)?$")


# id, range digital (0 analog, 1 Auto, 10 TETRA), fixture (None: noise; "dmo traffic" and "dmo presence" the
# synthetic bursts of build_iq_fixtures.py), frequency Hz, range half-width Hz, step Hz, what the history row must show
# (protocol, access, site regex or "" for none, confirmed), how long it stays on after the first landing, and how it
# plays: looped from the scene's start (None), its bursts at a phase against each tune ("phase", the phase at the first
# tune in eighths of a frame), or off from the search's landing tune on ("until landing").
TMO_SITE = tetra_site("426.70000 MHz", 228, 8889)
DIRECT = re.compile("^Direct$")
CELL_SITE = tetra_site("408.03125 MHz", 460, 4242)
RUN_A = (
    ("nfm", 0, "nfm_tone_synth", 446100000, 25000, 12500, None, ON_S),
    ("dmr", 1, "dmr_voice", 448100000, 25000, 12500, ("DMR", None, None, 1), ON_S),
    ("tmo", 10, "tetra_call_real_air_72k", 426700000, 50000, 25000, ("TETRA", "CC 1", TMO_SITE, 1), ON_S),
    ("cc144", 10, "tetra_cc_marginal_real_air_144k", 467912500, 50000, 12500,
     ("TETRA", "CC 44", tetra_site("467.91250 MHz", 250, 13), 1), ON_S),
    ("cell", 10, "tetra_bsch_synth_72k", 408031250, 25000, 25000, ("TETRA", "CC 17", CELL_SITE, 1), ON_S),
    ("traffic", 10, "tetra_bsch_synth_72k", 408131250, 25000, 25000, ("TETRA", "CC 17", CELL_SITE, 1), ON_S),
    ("dmo", 10, "tetra_dmo_call_synth_72k", 433000000, 25000, 25000, ("TETRA DMO", "", DIRECT, 1), ON_S),
    ("noise", 10, None, 440000000, 25000, 25000, (None, None, None, 0), NOISE_ON_S),
)
RUN_B = (
    ("awgn", 10, "tetra_call_awgn_72k", 426700000, 50000, 25000, ("TETRA", "CC 1", TMO_SITE, 1), ON_S),
    ("noise", 10, None, 440000000, 25000, 25000, (None, None, None, 0), NOISE_ON_S),
    # A DMO call's traffic (about 29 % of the time on air) whose bursts sit at a fixed phase against the tune, an
    # eighth of a frame on at the next tune: four calls starting a quarter frame apart, so the span measurements and
    # the landing meet each call at its own phases.
    ("dmo_ph0", 10, "dmo traffic", 416000000, 25000, 25000, ("TETRA DMO", "", DIRECT, 1), ON_S, ("phase", 0)),
    ("dmo_ph2", 10, "dmo traffic", 418000000, 25000, 25000, ("TETRA DMO", "", DIRECT, 1), ON_S, ("phase", 2)),
    ("dmo_ph4", 10, "dmo traffic", 420000000, 25000, 25000, ("TETRA DMO", "", DIRECT, 1), ON_S, ("phase", 4)),
    ("dmo_ph6", 10, "dmo traffic", 422000000, 25000, 25000, ("TETRA DMO", "", DIRECT, 1), ON_S, ("phase", 6)),
    # A repeater's presence signal train: heard, it confirms as presence only (off before its 2.1 s quiet time, so
    # one visit), and one that ends at the landing tune confirms nothing and ends unresolved.
    ("pres_on", 10, "dmo presence", 412000000, 25000, 25000, ("TETRA DMO", "", re.compile("^presence signal$"), 1),
     2.0),
    ("pres_end", 10, "dmo presence", 414000000, 25000, 25000, (None, None, None, 0), 0.0, ("until landing", 0)),
)
# The two carriers of the synthetic cell sit in one range.
SHARED_RANGE = {"traffic": "cell"}


def dmo_samples(kind):
    """One multiframe of a synthetic DMO call's traffic, or of a presence signal train, at 72 kHz (it loops
    seamlessly)."""
    slots = bif.tetra_dmo_traffic_slots(1) if kind == "dmo traffic" else bif.tetra_dmo_pres_slots(1)
    return bif.tetra_dmo_samples(slots, 0.0, 0.0), bif.TETRA_DMO_RATE_HZ


def dmo_refresh_ends(kind):
    """(end times, in seconds into one loop, of the bursts of a DMO transmitter that refresh its visit's activity; the
    loop's length), or None for one this script does not time so (a presence signal never refreshes it; see
    presence_only()).

    A direct mode visit's activity refreshes only on checked call content (tetra_search.h): direct mode speech and a
    DMAC-SYNC the call records take as a call, DM-SETUP and DM-OCCUPIED here, never DM-TX CEASED. The call fixture
    loops with DM-TX CEASED and 0.25 s of noise each side of its set-up, so its last refresh can come up to about
    0.64 s before the transmitter stops; the traffic train has a gap of at most a frame. The layouts are
    tools/build_iq_fixtures.py's own, laid out again here (tetra_dmo_samples()) and checked against the fixture's
    length in Transmitter."""
    if kind == "dmo traffic":
        slots, lead_s, tail_s = bif.tetra_dmo_traffic_slots(1), 0.0, 0.0
    elif kind == bif.TETRA_DMO_NAME:
        slots, lead_s, tail_s = bif.tetra_dmo_slots(), bif.TETRA_DMO_LEAD_S, bif.TETRA_DMO_TAIL_S
    else:
        return None
    ceased = {tuple(bif.tetra_dmo_dsb(slot, frame, bif.TETRA_DMO_MSG_TX_CEASED)) for frame in (1, 2) for slot in (1, 3)}
    rate = bif.TETRA_DMO_RATE_HZ
    sps = rate // bif.TETRA_SYMBOL_RATE
    lead = int(round(lead_s * rate))
    ends = []
    for index, dibits in enumerate(slots):
        if dibits is None or tuple(dibits) in ceased:
            continue
        start = lead + index * bif.TETRA_SLOT_SYMBOLS * sps + bif.TETRA_DMO_BURST_DELAY * sps
        ends.append((start + (len(dibits) + 1) * sps) / rate)
    count = lead + len(slots) * bif.TETRA_SLOT_SYMBOLS * sps + int(round(tail_s * rate))
    return ends, count / rate


class Transmitter(tcp_bench.Transmitter):
    """A scene transmitter with what its range searches for and what its detection must show."""

    def __init__(self, spec, fixture_dir):
        name, self.digital, fixture, frequency, self.half_width, step, self.expect, on_s = spec[:8]
        self.play, self.start_phase = spec[8] if len(spec) > 8 else (None, 0)
        samples = dmo_samples(fixture) if fixture and fixture.startswith("dmo ") else None
        super().__init__(name, frequency, None if samples else fixture, snr_db=SNR_DB, on_s=on_s, step=step,
                         fixture_dir=fixture_dir, samples=samples)
        layout = dmo_refresh_ends(fixture)
        self.refresh_ends = None
        if layout is not None:
            self.refresh_ends, loop_s = layout
            # The layout must be the one the scene plays: a fixture rebuilt with another one fails here, not by a
            # departure measured from the wrong burst.
            if abs(loop_s - self.seconds) > 0.5 / bif.TETRA_DMO_RATE_HZ:
                raise ValueError(f"{name}: {fixture} lasts {self.seconds:.6f} s, its layout {loop_s:.6f} s")
        # How long the run goes on after its stop: a visit that heard nothing waits out its 4 s probe.
        self.linger_s = 4.0 + LEAVE_PAD_S + 1.0 if self.play == "until landing" else TETRA_QUIET_S + LEAVE_PAD_S + 1.0

    def covering_tunes(self, t):
        """Scene times of the tunes up to @p t whose capture holds this transmitter (span measurements and visits)."""
        return [tt for tt, _w, centre, rate in list(self.scene.tunes)
                if tt <= t and abs(centre - self.frequency) < 0.45 * rate]

    def phase_at(self, t):
        """A phase-stepped call's phase against the tune on air at @p t: its start phase, an eighth of a frame on per
        tune."""
        count = len(self.covering_tunes(t))
        return (self.start_phase + max(count - 1, 0)) * DMO_PHASE_STEP_S % DMO_FRAME_S

    def landing_tunes(self):
        """Scene times of the search's visit tunes: a visit tunes the channel a quarter of the rate below the centre
        (the receiver's Fs/4 offset), which no span measurement does."""
        return [tt for tt, _w, centre, rate in list(self.scene.tunes) if abs(centre - rate / 4 - self.frequency) < 1000]

    def landing_tune(self, t):
        """Whether the search's visit tune has come by @p t."""
        return any(tt <= t for tt in self.landing_tunes())

    def call_at(self, t):
        if self.play == "until landing":
            return None if self.landing_tune(t) else super().call_at(t)
        if self.play != "phase" or (self.stop_at is not None and t >= self.stop_at):
            return super().call_at(t)
        tunes = self.covering_tunes(t)
        origin = (tunes[-1] if tunes else 0.0) - self.phase_at(t)
        return origin + math.floor((t - origin) / self.seconds) * self.seconds


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
        return None not in stops and all(scene.sample_time > tx.stop_at + tx.linger_s for tx in transmitters)

    folder = work / name
    folder.mkdir(parents=True, exist_ok=True)
    record = tcp_bench.run_rig(args.rig, transmitters, ranges_of(transmitters), DSP_KHZ, folder, RUN_SECONDS,
                               done=done, passes=PASSES)
    return dict(record, name=name, transmitters=transmitters)


def landing_pass(record, tx):
    """The pass the search first landed on @p tx in (tcp_bench.landed_hit()), or None."""
    for _t, _wall, m in tcp_bench.rig_messages(record, "status"):
        hit = tcp_bench.landed_hit(m)
        if hit is not None and tx.owns(hit["frequency_hz"]):
            return m["pass"]
    return None


def presence_only(tx):
    """Whether @p tx confirms as a presence signal, which never holds a visit."""
    return tx.expect is not None and tx.expect[2] is not None and "presence" in tx.expect[2].pattern


def last_refresh(tx):
    """Scene time of the end of the last burst that refreshes @p tx's visit (dmo_refresh_ends()) the scene sent whole
    before the transmitter went off, or None. The scene plays whole blocks: the transmitter goes off at the first block
    that starts at or after its stop."""
    if tx.stop_at is None or tx.refresh_ends is None:
        return None
    block_s = BLOCK / CAPTURE_HZ
    off = math.ceil(tx.stop_at / block_s - 1e-9) * block_s
    start = tx.call_at(tx.stop_at - 1e-9)
    if start is None:
        return None
    sent = [start + end for end in tx.refresh_ends if start + end <= off]
    # Off before this loop's first refresh (in its lead or its set-up): the last one came in the loop before.
    return max(sent) if sent else start - tx.seconds + max(tx.refresh_ends)


def departure_from(record, tx):
    """(the scene time a departure is measured from, its name): for a DMO transmitter whose bursts this script times,
    the last refreshing burst it sent; for a presence signal, which never refreshes the visit's activity, its first
    confirmation; for the rest, whose downlink refreshes the visit every slot, the stop. (None, name) when there is
    none."""
    if presence_only(tx):
        confirmed = tcp_bench.confirm_time(record, tx)
        return (None if confirmed is None else tx.landed + confirmed), "its first confirmation"
    if tx.refresh_ends is not None:
        return last_refresh(tx), "its last refreshing burst"
    return tx.stop_at, "its carrier stopped"


def departure(record, tx):
    """Seconds from departure_from() to the first retune after it, or None."""
    since, _name = departure_from(record, tx)
    if since is None:
        return None
    later = [t for t, _wall, _centre, _rate in record["tunes"] if t > since]
    return later[0] - since if later else None


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
        if tx.play == "phase":
            landed = landing_pass(record, tx)
            if landed is None or landed > DMO_MAX_PASS:
                failures.append(f"{tx.id}: landed on pass {landed}, not by pass {DMO_MAX_PASS}")
        if tx.expect is None or not tx.expect[0] or not tx.expect[0].startswith("TETRA"):
            continue
        # A presence signal never holds a visit: it leaves 2.1 s after its first confirmation, while its train is
        # still on (one that refreshed the visit would stay until 2.1 s after the stop, about 4 s after it).
        left = departure(record, tx)
        _since, since_name = departure_from(record, tx)
        if left is None:
            failures.append(f"{tx.id}: no retune after {since_name}")
        elif not TETRA_QUIET_S - LEAVE_EARLY_S <= left <= TETRA_QUIET_S + LEAVE_PAD_S:
            failures.append(f"{tx.id}: left {left:.2f} s after {since_name}, not about {TETRA_QUIET_S} s")
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
        since = "confirmation" if presence_only(tx) else "last refresh" if tx.refresh_ends is not None else "stop"
        confirm = tcp_bench.confirm_time(record, tx)
        confirm_text = "-" if confirm is None else f"{confirm:.2f} s"
        phase = ""
        if tx.play == "phase" and tx.landed is not None:
            landing = tx.landing_tunes()[0]
            found = [t for t in tx.covering_tunes(landing) if t < landing][-1]
            phase = (f" (pass {landing_pass(record, tx)}, found at phase {tx.phase_at(found) * 1000:.1f} ms, landed at "
                     f"{tx.phase_at(landing) * 1000:.1f} ms)")
        print(f"  {tx.id:8s} {tx.frequency / 1e6:10.5f} MHz landed {landed}{phase} confirmed {confirm_text} later, "
              f"left {left_text} after {since}: {shown}")


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
            try:
                record = run(args, name, specs, work)
            except ValueError as refused:
                print(f"error: {refused}", file=sys.stderr)
                return 2
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
