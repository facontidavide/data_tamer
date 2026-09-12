# Performance baseline — before the lock-free front end

Recorded on the unmodified library at commit `a851887`, built with benchmark
and allocation-hook sources copied in from `data_tamer_cpp` commit `a832e2d`
(HEAD of `lockfree-frontend` at the time of this fix wave — items 1 and 2 of
the final review; item 3, "make `AllocCounter` see `malloc`", is BLOCKED, see
the plan's final fix report (not kept in the repository)).
Because item 3 did not land, `tests/alloc_counter.cpp` is still the original
`operator new`/`operator delete` interposition hook, **not** a malloc-level
one: `allocs/op` below counts C++ allocations only (as in the previous
baseline), and does **not** include allocations the vendored moodycamel
queue makes directly through `std::malloc`.

Machine: `Model name:                              Intel(R) Core(TM) Ultra 7 265H`, `16` cores, kernel `6.17.0-1032-oem`,
GCC `g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0`, build preset `release` (`-O3`, no sanitizers).

Recording conditions (corrected from the previous baseline, which was taken
under load average 4.1 with CPU scaling on and no core pinning):
- All runs pinned to the P-cores with `taskset -c 0-5` (this Core Ultra 7
  265H exposes CPUs 0-5 as P-cores).
- The machine was **not** fully idle: background load average was ~1.1-1.4
  (desktop session with a browser and other agent processes running), lower
  than the 4.1 load of the previous baseline but not zero. This is noted as
  a residual limitation, see "Known limitations" below.
- Micro-benchmarks use `--benchmark_repetitions=5
  --benchmark_report_aggregates_only=true`, `--benchmark_min_time=0.5s` per
  repetition; the table below reports the `_mean`/`_median` aggregate rows.
- The `--fifo` `rt_latency` variant was intentionally not re-run (excluded
  from this recording pass, as it was in the reproduction commands used
  here); the previous "not run: no CAP_SYS_NICE" result for it is left as
  historical context further down and was not refreshed.
- CPU scaling was still enabled (not disabled at the governor level); the
  `***WARNING*** CPU scaling is enabled` benchmark message is expected.

How to reproduce: see the commands in
the implementation plan, Task 5, Step 5,
adjusted per item 4 of
the plan's final fix report (not kept in the repository)
(worktree at `a851887`, benchmark+hook sources copied from commit `a832e2d`,
pinned to CPUs 0-5, `--benchmark_repetitions=5`).

## Micro-benchmarks (`dt_benchmark`)

```
2026-09-10T16:23:24+02:00
Running ./build/release/benchmarks/dt_benchmark
Run on (16 X 5300 MHz CPU s)
CPU Caches:
  L1 Data 48 KiB (x16)
  L1 Instruction 64 KiB (x16)
  L2 Unified 3072 KiB (x16)
  L3 Unified 24576 KiB (x1)
Load Average: 1.18, 0.86, 0.75
***WARNING*** CPU scaling is enabled, the benchmark real time measurements may be noisy and will incur extra overhead.
***WARNING*** Library was built as DEBUG. Timings may be affected.
----------------------------------------------------------------------------------------
Benchmark                              Time             CPU   Iterations UserCounters...
----------------------------------------------------------------------------------------
DT_Doubles/125_mean                  525 ns          505 ns            5 allocs/op=2
DT_Doubles/125_median                526 ns          509 ns            5 allocs/op=2
DT_Doubles/125_stddev               24.3 ns         25.7 ns            5 allocs/op=0
DT_Doubles/125_cv                   4.64 %          5.09 %             5 allocs/op=0.00%
DT_Doubles/250_mean                  609 ns          582 ns            5 allocs/op=2
DT_Doubles/250_median                604 ns          583 ns            5 allocs/op=2
DT_Doubles/250_stddev               33.2 ns         24.8 ns            5 allocs/op=0
DT_Doubles/250_cv                   5.45 %          4.27 %             5 allocs/op=0.00%
DT_Doubles/500_mean                 1001 ns          884 ns            5 allocs/op=2
DT_Doubles/500_median                971 ns          867 ns            5 allocs/op=2
DT_Doubles/500_stddev                120 ns         83.6 ns            5 allocs/op=0
DT_Doubles/500_cv                  12.02 %          9.46 %             5 allocs/op=0.00%
DT_Doubles/1000_mean                2048 ns         1744 ns            5 allocs/op=2
DT_Doubles/1000_median              2054 ns         1746 ns            5 allocs/op=2
DT_Doubles/1000_stddev              43.3 ns         29.7 ns            5 allocs/op=0
DT_Doubles/1000_cv                  2.11 %          1.70 %             5 allocs/op=0.00%
DT_Doubles/2000_mean                3879 ns         3356 ns            5 allocs/op=2
DT_Doubles/2000_median              3887 ns         3342 ns            5 allocs/op=2
DT_Doubles/2000_stddev              82.1 ns         51.4 ns            5 allocs/op=0
DT_Doubles/2000_cv                  2.12 %          1.53 %             5 allocs/op=0.00%
DT_PoseType/125_mean                1853 ns         1572 ns            5 allocs/op=2
DT_PoseType/125_median              1855 ns         1570 ns            5 allocs/op=2
DT_PoseType/125_stddev              13.2 ns         29.5 ns            5 allocs/op=0
DT_PoseType/125_cv                  0.71 %          1.88 %             5 allocs/op=0.00%
DT_PoseType/250_mean                3385 ns         2959 ns            5 allocs/op=2
DT_PoseType/250_median              3379 ns         2943 ns            5 allocs/op=2
DT_PoseType/250_stddev              81.5 ns         46.5 ns            5 allocs/op=0
DT_PoseType/250_cv                  2.41 %          1.57 %             5 allocs/op=0.00%
DT_PoseType/500_mean                6179 ns         5533 ns            5 allocs/op=2
DT_PoseType/500_median              6263 ns         5568 ns            5 allocs/op=2
DT_PoseType/500_stddev               223 ns          111 ns            5 allocs/op=0
DT_PoseType/500_cv                  3.61 %          2.01 %             5 allocs/op=0.00%
DT_PoseType/1000_mean               8155 ns         7769 ns            5 allocs/op=2
DT_PoseType/1000_median             8195 ns         7846 ns            5 allocs/op=2
DT_PoseType/1000_stddev              110 ns          149 ns            5 allocs/op=0
DT_PoseType/1000_cv                 1.35 %          1.92 %             5 allocs/op=0.00%
DT_MultiSink/1_mean                 2081 ns         1767 ns            5 allocs/op=2
DT_MultiSink/1_median               2095 ns         1767 ns            5 allocs/op=2
DT_MultiSink/1_stddev               41.0 ns         25.4 ns            5 allocs/op=0
DT_MultiSink/1_cv                   1.97 %          1.44 %             5 allocs/op=0.00%
DT_MultiSink/2_mean                 1771 ns         1629 ns            5 allocs/op=4
DT_MultiSink/2_median               1774 ns         1639 ns            5 allocs/op=4
DT_MultiSink/2_stddev               18.2 ns         19.8 ns            5 allocs/op=0
DT_MultiSink/2_cv                   1.03 %          1.21 %             5 allocs/op=0.00%
DT_MultiSink/4_mean                 3603 ns         3128 ns            5 allocs/op=8
DT_MultiSink/4_median               3466 ns         3010 ns            5 allocs/op=8
DT_MultiSink/4_stddev                239 ns          182 ns            5 allocs/op=0
DT_MultiSink/4_cv                   6.62 %          5.82 %             5 allocs/op=0.00%
DT_LoggedValueSet/10_mean            567 ns          567 ns            5 items_per_second=18.0336M/s
DT_LoggedValueSet/10_median          604 ns          604 ns            5 items_per_second=16.5519M/s
DT_LoggedValueSet/10_stddev         83.5 ns         83.5 ns            5 items_per_second=3.31247M/s
DT_LoggedValueSet/10_cv            14.73 %         14.74 %             5 items_per_second=18.37%
DT_LoggedValueSet/100_mean          5415 ns         5413 ns            5 items_per_second=19.528M/s
DT_LoggedValueSet/100_median        6045 ns         6042 ns            5 items_per_second=16.5507M/s
DT_LoggedValueSet/100_stddev        1193 ns         1193 ns            5 items_per_second=5.99337M/s
DT_LoggedValueSet/100_cv           22.04 %         22.03 %             5 items_per_second=30.69%
DT_LoggedValueSet/1000_mean        60650 ns        60629 ns            5 items_per_second=16.4936M/s
DT_LoggedValueSet/1000_median      60670 ns        60649 ns            5 items_per_second=16.4883M/s
DT_LoggedValueSet/1000_stddev       42.1 ns         42.3 ns            5 items_per_second=11.501k/s
DT_LoggedValueSet/1000_cv           0.07 %          0.07 %             5 items_per_second=0.07%
DT_SnapshotWithWriter_mean          9753 ns         9727 ns            5 allocs/op=2
DT_SnapshotWithWriter_median        9749 ns         9726 ns            5 allocs/op=2
DT_SnapshotWithWriter_stddev         191 ns          189 ns            5 allocs/op=0
DT_SnapshotWithWriter_cv            1.96 %          1.94 %             5 allocs/op=0.00%
```

Note: the "Library was built as DEBUG" warning above refers to the system
`libbenchmark` package (`libbenchmark-dev` 1.8.3-3), which was built without
`NDEBUG`; `data_tamer` itself and this benchmark binary were built with the
`release` preset (`CMAKE_BUILD_TYPE=Release`, `-O3`).

`allocs/op` is 2 for every single-sink case (`DT_Doubles`, `DT_PoseType`,
`DT_MultiSink/1`, `DT_SnapshotWithWriter`) and scales as `2 × sinks` for
`DT_MultiSink` (2, 4, 8 for 1, 2, 4 sinks), matching the expected `Snapshot`
copy into the queue per sink. As above, this hook only sees `operator
new`/`operator delete`, not `std::malloc`, so it does not include the
moodycamel queue's own block allocations.

### Known limitations of this recording

- `DT_MultiSink` medians are **not** strictly monotonically non-decreasing
  in sink count: `/1` median is 2095 ns vs. `/2` median at 1774 ns (repeated
  twice, both times `/2` came in slightly below `/1`). This was checked
  against the required sanity condition, re-run once as instructed, and
  remained inconsistent both times; per the recording instructions this is
  committed anyway with the discrepancy called out here rather than
  iterating further. The residual background load (~1.1-1.4, see above) and
  only 5 repetitions per case are the most likely causes — `/1` and `/2` are
  close enough (within ~15%, both well under the `/4` value) that this
  reads as measurement noise at low sink counts rather than a genuine
  non-monotonicity, but it does not meet the letter of the sanity check.
- The 1-sink `NullSink` and 1-sink MCAP harness p50s (42960 ns vs. 47406 ns,
  see below) are within ~10%, comfortably inside the ~20% sanity bound.

## Latency harness (`rt_latency`)

### 1000 values, 1 sink
```
rt_latency values=1000 sinks=1 writers=0 seconds=10 rate=1000Hz mcap=- fifo=0
samples=10000 p50=42960 ns p99=53672 ns p99.9=61377 ns max=120970 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 2 sinks
```
rt_latency values=1000 sinks=2 writers=0 seconds=10 rate=1000Hz mcap=- fifo=0
samples=10000 p50=44114 ns p99=57923 ns p99.9=79553 ns max=97482 ns
allocations per call after warm-up: 4.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 1 sink, 2 writer threads
```
rt_latency values=1000 sinks=1 writers=2 seconds=10 rate=1000Hz mcap=- fifo=0
samples=10000 p50=46742 ns p99=58297 ns p99.9=68501 ns max=106931 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 1 compressed MCAP sink
```
rt_latency values=1000 sinks=1 writers=0 seconds=10 rate=1000Hz mcap=/tmp/baseline.mcap fifo=0
samples=10000 p50=47406 ns p99=75007 ns p99.9=89928 ns max=454433 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```

### 1000 values, 1 sink, 2 writers, SCHED_FIFO (if available)

Not re-run in this recording pass (the `--fifo` variant is intentionally
excluded from the item-4 reproduction commands). Historical result from the
previous baseline, left here for context only and not refreshed under the
new pinning/idle conditions:

```
rt_latency values=1000 sinks=1 writers=2 seconds=10 rate=1000Hz mcap=- fifo=1
SCHED_FIFO not available (need CAP_SYS_NICE); running SCHED_OTHER
samples=10000 p50=53878 ns p99=130422 ns p99.9=167372 ns max=375072 ns
allocations per call after warm-up: 2.0000
takeSnapshot returned false: 0 / 10000
fifo=0
```
