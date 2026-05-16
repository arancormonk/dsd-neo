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

Useful options:

- `--iters N`: measured iterations per repeat.
- `--warmup N`: warmup iterations before each repeat.
- `--repeat N`: independent measured repeats.
- `--case NAME`: run one benchmark case.
- `--format text|csv`: human-readable or machine-readable output.

## Interpreting Results

Prefer median time when comparing branches, and re-run focused cases after any
large system load change. The benchmark prints both per-call and per-item costs;
full demod cases use symbols as the item count, while FIR/ring/kernel cases use
input samples or complex pairs as noted by the case name.

Keep benchmark changes separate from DSP algorithm changes. A good performance
patch should include a before/after run of the affected cases plus the relevant
DSP regression tests.
