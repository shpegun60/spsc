# SPSC Reproducible Baseline Harness

This directory implements H0 from the hardening roadmap. It captures a
machine-specific baseline; it does **not** claim that one queue is generally
faster than another.

## Pinned Comparator

The harness uses the MIT-licensed `rigtorp/SPSCQueue` submodule at:

```text
v1.1
565a5149d54930463d58cb0f69b978d439555e66
```

Initialize it in a fresh checkout before compiling:

```powershell
git submodule update --init --recursive
```

## Windows Baseline Run

From the repository root:

```powershell
.\scripts\run_spsc_baseline.ps1
```

If the local PowerShell execution policy blocks project scripts, use a
process-local override (it does not change the machine policy):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_spsc_baseline.ps1
```

The runner uses the selected `g++` compiler and emits a release-oriented
binary. On Windows it asks the benchmark to choose two distinct physical cores,
preferring P-cores on a hybrid host (rather than sibling logical CPUs); an
explicit `-ProducerCpu/-ConsumerCpu` pair remains available. The manifest
records the requested and resolved pair plus the active base power scheme and
Windows performance overlay. The default capture
uses 20,000,000 transfers, nine measured samples, and two warm-ups per case.
It writes:

- `benchmarks/results/<timestamp>-<sha>.jsonl` — raw samples and summaries;
- `benchmarks/results/<timestamp>-<sha>.manifest.json` — compiler, flags, CPU,
  topology, affinity request, cache-line configuration, submodule pin, and git
  state;
- `benchmarks/results/<timestamp>-<sha>.hotpath.s` — generated assembly for
  the selected producer and complete consumer paths.

Useful overrides:

```powershell
.\scripts\run_spsc_baseline.ps1 -Items 50000000 -Samples 9 -Warmup 3 -Capacity 1024
.\scripts\run_spsc_baseline.ps1 -Suite queue -ProducerCpu 2 -ConsumerCpu 3
.\scripts\run_spsc_baseline.ps1 -NoAffinity
.\scripts\run_spsc_baseline.ps1 -RequireClean
.\scripts\run_spsc_baseline.ps1 -Suite queue -LibraryRoot C:\tmp\spsc-h0
```

Supported static capacities are `64`, `256`, `1024`, and `4096`. The runner
records the effective value in every output file.

## What Is Compared

`queue` versus Rigtorp uses the same lifecycle-shaped path on both sides:

```text
producer: try_emplace(sequence)
consumer: try_front() -> read sequence -> pop()
```

The payload has a non-trivial destructor with a thread-local volatile sink, so
both implementations execute an observable destruction path after consumption.
This is the appropriate comparison for `spsc::queue`, which owns constructed
objects. It is not a comparison of bare `try_pop()` calls.

`fifo` is measured separately with live `uint64_t` slots and the complete:

```text
producer: try_push(value)
consumer: try_front() -> read value -> pop()
```

The policy suite runs `A<>`, `FA<>`, `CA<>`, and `CFA<>` through that same
`fifo` workload. It records container `sizeof` and `alignof` for the main
atomic policy families in the metadata record.

## Workloads

- `steady`: producer and consumer transfer a continuous sequence while failed
  full/empty attempts are counted.
- `boundary`: an out-of-band phase variable holds the consumer until the
  producer has filled the queue, performs one failed full probe, drains the
  complete batch, and performs one empty probe. It measures boundary behavior,
  not independent steady-state throughput.

Every raw sample verifies delivered sequence order, item count, and checksum.
A failed verification marks the sample and its summary as invalid.

Transient full/empty retries use a CPU-relax instruction (`PAUSE` on x86), not
`std::this_thread::yield()`. The queues can naturally observe different retry
counts; periodically yielding would turn that difference into an uncontrolled
Windows scheduler measurement rather than a queue measurement.

For the direct `queue<CFA>` versus Rigtorp comparison, each measured pair is
run in alternating order. The JSONL samples carry `pair_index` and
`order_in_pair`, followed by a `paired_summary` whose rate ratio is
`Rigtorp / spsc::queue<CFA>`. This prevents the first implementation from
always inheriting a different thermal or scheduler state.

## Result Format

The JSONL file contains one `metadata` record, one `sample` record per measured
run, a `summary` record for each implementation/policy/workload tuple, and a
`paired_summary` for queue/Rigtorp cases. Summaries include median, mean,
sample standard deviation, minimum, and maximum; use the median and paired
ratio for comparison, not a short-run arithmetic mean. Throughput is named
`transfers_per_second`: one successfully delivered item is one transfer, not
two endpoint operations.

Windows samples also record endpoint CPU time. It is diagnostic telemetry (its
clock is quantized on some Windows installations), useful for detecting a run
that became a scheduling experiment rather than proof of an exact CPU-time
ratio.

The manifest deliberately records dirty-worktree state. A retained baseline
should normally be generated after committing the relevant source changes; a
dirty manifest is diagnostic evidence, not a release-quality reference.
`-RequireClean` turns that recommendation into an enforced capture precondition.

`-LibraryRoot` keeps the current checkout's harness but compiles it against a
different checked-out library revision. It is intended for controlled H0/H2
comparisons; the manifest records both the library and harness worktrees.

H0 records container sizes and alignments. Private index offsets are not made
public merely for benchmarking; H2's friend-only layout probe will record the
owner-line geometry after the layout change.

## Manual GNU Make Build

On a POSIX/MSYS shell with GNU Make:

```sh
cd benchmarks
make
./.build/spsc_bench --suite queue --output results/manual.jsonl
make assembly
```

The PowerShell runner is the canonical Windows capture path because it adds the
machine manifest and affinity selection.
