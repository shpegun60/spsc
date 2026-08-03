# SPSC Reproducible Measurement Harness

This directory implements H0 and the H0R validity repair from the hardening
roadmap. It captures machine-specific measurements and can explicitly report
`inconclusive`; it does **not** force a winner or claim that one queue is
generally faster than another.

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
preferring two non-zero P-cores on a hybrid host (rather than CPU 0 or sibling
logical CPUs). CPU 0 remains a fallback for hosts without two other suitable
physical cores; an explicit `-ProducerCpu/-ConsumerCpu` pair remains available.
By default each case is measured in both endpoint assignments (`P -> C` and
`C -> P`) on persistent pinned worker threads. The manifest records both
assignments and the base power scheme/Windows performance overlay before and
after execution.
The default capture
uses 20,000,000 transfers, nine measured samples, and two warm-ups per case.

`auto` selects one deterministic physical-core pair; it does not assume that
all same-class cores on a hybrid or turbo-controlled host are performance
equivalent. A host-wide ranking must be repeated on additional explicit
same-class core pairs and treated as inconclusive if those pair-level
classifications disagree.

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
.\scripts\run_spsc_baseline.ps1 -Suite queue -Directions forward
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

## STM32 Fast-Alias Assembly Probe

`stm32_fast_alias_probe.cpp` instantiates the complete `fast_fifo` and
`fast_queue` producer/consumer paths used by H10. With no selector macro it
tests the public aliases. Define `H10_EXPLICIT_CA=1` or
`H10_EXPLICIT_CFA=1` to isolate the counter backend while keeping the payload
and operations identical.

Example Cortex-M7 compilation from the repository root:

```powershell
arm-none-eabi-g++.exe -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Werror `
  -pedantic-errors -mthumb -mcpu=cortex-m7 -mfloat-abi=soft `
  -fno-exceptions -fno-rtti -DSPSC_FORCE_CACHELINE=32 `
  -I. -Isrc -c benchmarks/stm32_fast_alias_probe.cpp -o fast_alias_m7.o
arm-none-eabi-objdump.exe -d fast_alias_m7.o
```

Use `-mcpu=cortex-m4` for the M4 probe. This is static code-generation
evidence, not a hardware cycle benchmark. The source also verifies that
`CA<>` and `CFA<>` keep identical `sizeof` and `alignof` for the probed FIFO
and queue types on the selected toolchain.

## Workloads

- `steady`: producer and consumer transfer a continuous sequence while failed
  full/empty attempts are counted. This is a contention-sensitive system
  measurement: retry rate and endpoint/core balance are part of the result.
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
run in alternating implementation order. Direction order also alternates, and
the same persistent OS threads execute both implementations on a given
endpoint assignment. The JSONL samples carry `pair_index`, `order_in_pair`, and
`direction`. A `paired_summary` is emitted for each direction; its rate ratio
is `Rigtorp / spsc::queue<CFA>`.

The final `comparison_summary` is one of:

- `spsc_faster` or `rigtorp_faster`: both directions pass the sample gate and
  agree outside the parity band;
- `parity`: both directions pass and remain inside the configured parity band;
- `inconclusive`: insufficient samples, excessive paired variation, low
  endpoint CPU occupancy, failed affinity, direction sensitivity, or a
  classification disagreement.

The default ranking gate requires at least nine measured pairs per direction,
paired-ratio CV no greater than 10%, minimum endpoint CPU occupancy of 80%,
direction-ratio spread no greater than 15%, and a parity band of `0.95..1.05`.
These thresholds make unstable evidence explicit; they do not transform one
capture into a platform-independent claim.

## Result Format

Format version 2 contains one `metadata` record, one `sample` record per
measured run/direction, a `summary` record for each
implementation/policy/workload/direction tuple, per-direction `paired_summary`
records, and a final `comparison_summary` for queue/Rigtorp cases. Summaries
include median, mean, sample standard deviation, minimum, maximum, retry rate,
and CPU-occupancy telemetry; use the classification rather than selecting a
convenient raw median. Throughput is named
`transfers_per_second`: one successfully delivered item is one transfer, not
two endpoint operations.

Windows samples also record endpoint CPU time and `QueryThreadCycleTime`
cycles. CPU time is quantized on some Windows installations; both fields are
diagnostic telemetry, not a replacement throughput metric.

Legacy format-version-1 captures created a fresh thread pair for every
measurement and tested only one endpoint assignment. Repeated identical runs
on the canonical Windows host reversed the queue/Rigtorp ranking, so those
captures remain regression diagnostics only and must not support a winner
claim. Even a ranking-eligible version-2 capture should be reproduced in a
second process before it is retained as release evidence.

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
