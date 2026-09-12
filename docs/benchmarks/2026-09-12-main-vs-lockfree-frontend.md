# Data Tamer: `origin/main` vs `lockfree-frontend` (PR #75) — 2026-09-12

Same machine, same compiler, same session, runs interleaved. Nothing here is committed.

## Setup

- `main`: `e761f4f` (`origin/main`), worktree `.worktrees/bench-main`
- `pr`: `be73097` (`lockfree-frontend`, PR #75), worktree `.worktrees/bench-pr` (detached)
- Build: CMake Release, `-DDATA_TAMER_BUILD_BENCHMARKS=ON -DDATA_TAMER_BUILD_TESTS=ON -DDATA_TAMER_BUILD_ROS=OFF -DDATA_TAMER_BUILD_EXAMPLES=OFF`, `-j 8`, static `libdata_tamer.a`.
  `main` additionally needed `-DCMAKE_CXX_FLAGS="-include cstdint"` because its vendored `3rdparty/mcap/include/mcap/types.hpp` does not include `<cstdint>` and GCC 15 rejects it (the PR fixed this in-tree). No source was modified.
- Machine: 13th Gen Intel Core i7-13700H (14 cores / 20 threads, hybrid P+E), 31 GiB RAM, Linux 7.0.0-31-generic, `intel_pstate`, governor `performance`, `energy_performance_preference=performance`, min/max 400/5000 MHz.
- Toolchain: g++ (Ubuntu 15.2.0-16ubuntu1) 15.2.0, CMake 4.2.3, `-std=c++17 -O2 -DNDEBUG` for the probe (library built with each tree's own Release flags).
- All runs pinned with `taskset -c 2-7`.

### What each side does per `takeSnapshot()` call

**main:** under the channel mutex it serializes every registered series into a member `Snapshot` (resizing its `payload` vector), then under `sinks_mutex` calls `pushSnapshot()` on each sink, which copies the whole `Snapshot` into an unbounded `moodycamel::ConcurrentQueue<Snapshot>`. The sink's own thread dequeues copies and calls `storeSnapshot()`; there is no back-pressure, and the per-sink copy is where the caller-thread allocations come from.

**pr:** it acquires a preallocated slot from a bounded `SnapshotPool` (`tryAcquire`, returns `false` and bumps `pool_exhausted` when none is free), serializes into that slot under a `WriteMutex`, and `try_enqueue`s a reference-counted `SnapshotRef` into each sink's bounded `BlockingConcurrentQueue` (default capacity 1024) via a per-sink producer token. The sink thread `wait_dequeue`s the ref and calls `storeSnapshot()`; the slot returns to the pool when the last ref drops, so the caller thread allocates nothing in steady state.

## 1. Google Benchmark (`--benchmark_repetitions=5 --benchmark_report_aggregates_only=true --benchmark_min_time=0.2s`)

Medians of 5 repetitions, real time, ns/iteration. Both binaries use a `NullSink` (`storeSnapshot` returns `true`) and one channel; the loop body is `takeSnapshot()` on both sides (the PR wraps it in a helper that also counts allocations, which adds a thread-local counter read per iteration).

| case | main (ns) | pr (ns) | pr/main |
|---|---:|---:|---:|
| DT_Doubles/125 | 715 | 282 | 0.39 |
| DT_Doubles/250 | 837 | 327 | 0.39 |
| DT_Doubles/500 | 1284 | 471 | 0.37 |
| DT_Doubles/1000 | 2388 | 774 | 0.32 |
| DT_Doubles/2000 | 4068 | 1421 | 0.35 |
| DT_PoseType/125 | 2187 | 725 | 0.33 |
| DT_PoseType/250 | 3898 | 1217 | 0.31 |
| DT_PoseType/500 | 5794 | 2734 | 0.47 |
| DT_PoseType/1000 | 9382 | 5059 | 0.54 |

Cases only on `main`: none (main's binary is named `dt_benchmark`, the PR renamed it `data_tamer_benchmark`).

Cases only on `pr` (median ns): DT_MultiSink/1 783, /2 925, /4 1384; DT_LoggedValueSet/10 7, /100 63, /1000 1029; DT_SnapshotWithWriter 1906; DT_SnapshotWithTransactionWriter 3109; DT_WriteMutexTryLock 15; DT_StdMutexTryLock 12; DT_WriteMutexSpinThenLock/hold_us:0 174, :1 1075, :5 5919; DT_SnapshotPoolTryAcquire/busy_slots:0 4, :32 5, :63 30; DT_SnapshotRefCloneDestroy 15; DT_SnapshotPoolRoundTrip2Consumers 289.

## 2. Standalone 1 kHz probe (`probe.cpp`, common API only)

One `LogChannel`, `registerValue` on a `std::vector<double>` of 500, 500 `createLoggedValue<double>`, N `DummySink`s (each tree's own `dummy_sink.hpp`, default constructor). 10 warm-up calls, then 8000 `takeSnapshot()` calls paced at 1 kHz with `clock_nanosleep(TIMER_ABSTIME)`; per-call duration via `steady_clock`; heap allocations on the calling thread counted by linking the PR's `tests/alloc_counter.cpp` (global `operator new` replacement) into both binaries. Writer threads call `set()` on their half of the LoggedValues every 200 µs. `main` and `pr` alternate rep by rep. `takeSnapshot()` returned `true` on every call in every run (no pool exhaustion on `pr`).

Times in µs. `allocs` = heap allocations per `takeSnapshot()` call (min = median = max in every run).

### Config A: 0 writers, 1 sink

| rep | side | p50 | p99 | p99.9 | max | allocs |
|---|---|---:|---:|---:|---:|---:|
| 1 | main | 20.1 | 49.2 | 68.6 | 145.2 | 2 |
| 1 | pr | 22.6 | 31.9 | 45.4 | 78.9 | 0 |
| 2 | main | 18.6 | 43.7 | 73.3 | 113.1 | 2 |
| 2 | pr | 6.1 | 36.0 | 56.5 | 185.4 | 0 |
| 3 | main | 21.5 | 72.8 | 88.2 | 159.4 | 2 |
| 3 | pr | 4.2 | 32.5 | 45.9 | 54.9 | 0 |

### Config B: 2 writers, 1 sink

| rep | side | p50 | p99 | p99.9 | max | allocs |
|---|---|---:|---:|---:|---:|---:|
| 1 | main | 19.0 | 59.1 | 84.0 | 144.5 | 2 |
| 1 | pr | 16.9 | 49.8 | 61.0 | 72.1 | 0 |
| 2 | main | 17.6 | 54.4 | 78.1 | 187.4 | 2 |
| 2 | pr | 23.0 | 49.6 | 59.8 | 103.4 | 0 |
| 3 | main | 18.3 | 61.9 | 80.6 | 137.5 | 2 |
| 3 | pr | 6.2 | 46.5 | 60.4 | 162.0 | 0 |

### Config C: 2 writers, 4 sinks

| rep | side | p50 | p99 | p99.9 | max | allocs |
|---|---|---:|---:|---:|---:|---:|
| 1 | main | 18.4 | 63.8 | 116.2 | 346.1 | 8 |
| 1 | pr | 20.4 | 62.9 | 109.6 | 366.1 | 0 |
| 2 | main | 18.0 | 61.9 | 131.6 | 157.3 | 8 |
| 2 | pr | 18.9 | 56.8 | 70.9 | 190.1 | 0 |
| 3 | main | 17.8 | 51.6 | 109.6 | 156.8 | 8 |
| 3 | pr | 18.5 | 60.3 | 73.7 | 999.5 | 0 |

## Caveats

- **Load.** When the session started another job (a DuckDB build in a different repo) had all 20 CPUs at 100% (load avg 21-23). Measurements were started only after the 1-minute load average fell below 3 and no `cc1plus` was running; load was 2.2 at the start of the probe runs, 1.0 at the end, and 1.6-1.8 during the Google Benchmark runs. Some residual background activity from that session (e.g. page cache, other user processes) cannot be excluded.
- **CPU frequency.** Governor `performance` on `intel_pstate`, but frequency/C-state transitions are not disabled. The 1 kHz probe sleeps ~980 µs between calls, so the core is idle most of the time; `pr`'s p50 is bimodal across reps (4-6 µs in 3 runs, 17-23 µs in 6 runs) and `main`'s p50 is 18-21 µs in all 9 runs. This report does not determine why; it may be wake-up/frequency behaviour rather than library behaviour, and the same effect may inflate `main`'s p50 too. Google Benchmark reported `cpu_scaling_enabled: false` but measured different MHz for the two runs (717 vs 478 MHz), which is a snapshot of the current frequency, not a controlled setting.
- **CPU set.** `taskset -c 2-7` on a hybrid CPU; those logical CPUs are P-core hyperthreads, so writer threads, sink threads and the measured thread may share physical cores.
- **Tight loop vs paced.** The Google Benchmark numbers are a hot loop (caches and branch predictors warm, no sleeping); the probe is paced at 1 kHz from a cold-ish core. They measure different regimes and should not be compared to each other.
- **Probe sink.** `DummySink::storeSnapshot` copies the full snapshot under a mutex on the sink thread on both sides; this is sink-thread work and is not in the measured interval, but on `main` the copy into the queue is on the calling thread (the 2 allocations/sink/call), whereas on `pr` the calling thread only enqueues a ref.
- **Statistical weight.** 3 repetitions x 8000 samples per configuration; the max column is a single sample per run and varies by 2-6x between reps of the same configuration on both sides. p99.9 is 8 samples.
- Repetitions are not enough to call p50 differences in configs B/C significant; the consistent differences across all 9 runs are: allocations per call (2 or 8 on `main`, 0 on `pr`), and p99.9 in config C (110-132 µs on `main`, 71-110 µs on `pr`).

## Reproduction

Worktrees were removed after the run. Build dirs and the probe live in the session scratchpad (`bench-main-build`, `bench-pr-build`, `probe/probe.cpp`, `probe/alloc_counter.{cpp,hpp}`, `run_all.sh`, `probe_results.txt`, `gb-main.json`, `gb-pr.json`). Probe compile line per side:

```
g++ -std=c++17 -O2 -DNDEBUG [-include cstdint on main] \
  -I<tree>/data_tamer_cpp/include -I<tree>/data_tamer_cpp/3rdparty \
  probe.cpp alloc_counter.cpp <build>/libdata_tamer.a -lpthread -o probe-<side>
taskset -c 2-7 ./probe-<side> <writers> <sinks>
```
