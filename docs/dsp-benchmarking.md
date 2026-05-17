# DSP Benchmarking

DSD-neo keeps DSP benchmarks opt-in. They are meant to measure proposed DSP
architecture changes before changing production behavior.

## Build

Use the benchmark preset for local performance work:

```sh
cmake --preset perf-bench
cmake --build --preset perf-bench --target dsd-neo_bench_dsp -j
```

The preset uses `RelWithDebInfo`, fast math, frame pointers, and tests enabled.
It keeps LTO and native CPU tuning off so results are easier to compare across
machines.

## Run

Run all synthetic benchmark cases:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5
```

Run one case and emit CSV:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp \
  --case full_demod_cqpsk_p25p1 --iters 3000 --repeat 5 --format csv
```

List available cases:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp --list
```

Useful options:

- `--iters N`: measured iterations per repeat.
- `--warmup N`: warmup iterations before each repeat.
- `--repeat N`: independent measured repeats.
- `--case NAME`: run one benchmark case.
- `--format text|csv`: human-readable or machine-readable output.

## Benchmark Groups

The benchmark target includes:

- Input/output rings and RTL u8 widening/rotation.
- SIMD FIR kernels, half-band decimators, and generated channel LPF plans.
- FSK modem acquisition/steady-state cases.
- CQPSK stage cases for band-edge FLL, Gardner, CMA equalizer, differential phasor, Costas, and full demod chains.
- Full demod cases for C4FM audio monitor, FSK symbol output, and CQPSK P25P1/P25P2.

Channel LPF CSV rows include `rate_hz`, `profile`, `tap_count`, and `variant`
metadata so tap-count and profile changes can be compared directly.

## Comparing Runs

Keep benchmark CSV files outside the repository, for example under `/tmp`:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5 --format csv > /tmp/dsd-main.csv
# switch branch or apply a patch
build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5 --format csv > /tmp/dsd-candidate.csv
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv
```

Focused comparisons are usually more useful than all-case runs:

```sh
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv --filter cqpsk
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv --filter channel_lpf
```

Use `items_per_second` when throughput is easier to read:

```sh
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv --metric items_per_second
```

On Linux, optional hardware counters can help identify whether a target is math
or memory dominated:

```sh
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  build/perf-bench/tests/dsd-neo_bench_dsp --case op25_costas_loop_cc --iters 3000 --repeat 1 --format csv
```

## Interpreting Results

Prefer median time when comparing branches, and re-run focused cases after any
large system load change. The benchmark prints both per-call and per-item costs;
full demod cases use symbols as the item count, while FIR/ring/kernel cases use
input samples or complex pairs as noted by the case name.

Treat deltas below roughly 5% as noise unless repeated runs and hardware
counters support the result.

Keep benchmark changes separate from DSP algorithm changes. A good performance
patch should include a before/after run of the affected cases plus the relevant
DSP regression tests.
