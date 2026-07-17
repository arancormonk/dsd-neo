# DSP, RTL, and Protocol Benchmarking

DSD-neo keeps DSP, RTL, and targeted protocol performance probes opt-in. They
are meant to measure proposed architecture changes before changing production
behavior.

## Build

Use the benchmark preset for local performance work:

```sh
cmake --preset perf-bench
cmake --build --preset perf-bench --target dsd-neo_bench_dsp -j
```

For RTL ingest and live-pipeline support cases, build the RTL benchmark too:

```sh
cmake --build --preset perf-bench --target dsd-neo_bench_rtl -j
```

For the P25 Phase 1 1/2-rate list decoder, build the protocol benchmark:

```sh
cmake --build --preset perf-bench --target dsd-neo_bench_p25_12 -j
```

The preset uses `RelWithDebInfo`, fast math, frame pointers, and tests enabled.
It keeps LTO and native CPU tuning off so results are easier to compare across
machines.

## Run

Run all synthetic benchmark cases:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5
build/perf-bench/tests/dsd-neo_bench_rtl --iters 3000 --repeat 5
build/perf-bench/tests/dsd-neo_bench_p25_12 --iters 3000 --repeat 5
```

Run one case and emit CSV:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp \
  --case full_demod_cqpsk_p25p1 --iters 3000 --repeat 5 --format csv
build/perf-bench/tests/dsd-neo_bench_rtl \
  --case rtl_ingest_u8_combined_wrap --iters 3000 --repeat 5 --format csv
build/perf-bench/tests/dsd-neo_bench_p25_12 \
  --case p25_12_list_marginal --iters 3000 --repeat 5 --format csv
```

List available cases:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp --list
build/perf-bench/tests/dsd-neo_bench_rtl --list
build/perf-bench/tests/dsd-neo_bench_p25_12 --list
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
- FSK discriminator reset/steady-state cases.
- CQPSK stage cases for band-edge FLL, Gardner, differential phasor, Costas, and full demod chains.
- Full demod cases for C4FM audio monitor, FSK discriminator output, and CQPSK P25P1/P25P2.

The RTL benchmark target includes:

- CU8 ingest into the input ring for contiguous and wrap-around reservations.
- Standalone CU8 integer-level reduction at the 16 KiB default block size and
  256 KiB maximum block size.
- Production-equivalent CU8 ingest plus level reduction at both block sizes,
  with benchmark-only `separate` cases retained as the unfused-pass baseline.
- CS16 ingest conversion for Soapy-style signed sample input.
- Post-demod spectrum snapshot updates used by the UI/metrics path.
- 512-sample direct-output batch reads.

The P25 list-decoder benchmark includes clean high-confidence, marginal/noisy,
and equal-metric tie-heavy inputs. It reports each complete 49-symbol decode as
one call and uses the same CSV timing columns as the DSP and RTL benchmarks.

Channel LPF CSV rows include `rate_hz`, `profile`, `tap_count`, and `variant`
metadata so tap-count and profile changes can be compared directly.

## Comparing Runs

Keep benchmark CSV files outside the repository, for example under `/tmp`:

```sh
build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5 --format csv > /tmp/dsd-main.csv
build/perf-bench/tests/dsd-neo_bench_rtl --iters 3000 --repeat 5 --format csv > /tmp/dsd-rtl-main.csv
build/perf-bench/tests/dsd-neo_bench_p25_12 --iters 3000 --repeat 5 --format csv > /tmp/dsd-p25-main.csv
# switch branch or apply a patch
build/perf-bench/tests/dsd-neo_bench_dsp --iters 3000 --repeat 5 --format csv > /tmp/dsd-candidate.csv
build/perf-bench/tests/dsd-neo_bench_rtl --iters 3000 --repeat 5 --format csv > /tmp/dsd-rtl-candidate.csv
build/perf-bench/tests/dsd-neo_bench_p25_12 --iters 3000 --repeat 5 --format csv > /tmp/dsd-p25-candidate.csv
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv
python3 tools/dsp_bench_compare.py /tmp/dsd-rtl-main.csv /tmp/dsd-rtl-candidate.csv
python3 tools/dsp_bench_compare.py /tmp/dsd-p25-main.csv /tmp/dsd-p25-candidate.csv --metric median_ns_per_call
```

Focused comparisons are usually more useful than all-case runs:

```sh
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv --filter cqpsk
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv --filter channel_lpf
```

For CU8 ingest work, capture five-repeat CSV medians for the matching
`rtl_ingest_u8_metrics_separate_*` and `rtl_ingest_u8_metrics_fused_*` cases.
The 16 KiB cases represent the normal live block and the 256 KiB cases exercise
the maximum supported benchmark block. Treat differences below roughly 5% as
inconclusive unless repeated runs and hardware counters agree.

Use `items_per_second` when throughput is easier to read:

```sh
python3 tools/dsp_bench_compare.py /tmp/dsd-main.csv /tmp/dsd-candidate.csv --metric items_per_second
```

On Linux, optional hardware counters can help identify whether a target is math
or memory dominated:

```sh
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  build/perf-bench/tests/dsd-neo_bench_dsp --case op25_costas_loop_cc --iters 3000 --repeat 1 --format csv
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  build/perf-bench/tests/dsd-neo_bench_rtl --case rtl_ingest_u8_combined_contig --iters 3000 --repeat 1 --format csv
```

## Live RTL Perf CSV

Live RTL runs can emit coarse pipeline timing windows without changing the
normal runtime path unless explicitly enabled:

```sh
DSD_NEO_RTL_PERF_CSV=1 \
DSD_NEO_RTL_PERF_INTERVAL_MS=1000 \
build/perf-bench/apps/dsd-cli/dsd-neo -i rtl:0:769.76875M:3:-2:48:0:2:bias -mq -T --enc-lockout
```

`DSD_NEO_RTL_PERF_CSV` enables logging to `dsd-neo-rtl-perf.csv` in the current
working directory. `DSD_NEO_RTL_PERF_INTERVAL_MS` controls the aggregation
window and is clamped to 100-60000 ms. CSV rows include ring fill, cumulative
input drops, ingest timing, `full_demod()` timing, post-demod metrics timing,
output-write timing, consumer-read timing, SNR, CFO, and carrier lock snapshots.
For CU8 sources, `ingest_ns` includes the integer level reduction fused into
widening, rotation, pending-buffer copies, and dropped-tail accounting.

Use live CSV to decide which synthetic cases deserve focused before/after runs.
For example, high `post_metrics_ns` points at `rtl_metrics_spectrum_*`, while
high ingest time points at `rtl_ingest_*` cases.

## Interpreting Results

Prefer median time when comparing branches, and re-run focused cases after any
large system load change. The benchmark prints both per-call and per-item costs;
full demod cases use symbols as the item count, while FIR/ring/kernel cases use
input samples or complex pairs as noted by the case name.

Treat deltas below roughly 5% as noise unless repeated runs and hardware
counters support the result.

Keep benchmark changes separate from DSP or RTL algorithm changes. A good
performance patch should include a before/after run of the affected cases plus
the relevant regression tests.
