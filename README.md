# SPSC Test Project

Main integration project for the `spsc` buffer library and paranoid API tests.

![Qt dashboard with all test suites passed](img/Screenshot%202026-03-21%20141744.png)

Quick links:

- [Changelog](CHANGELOG.md)
- [Reproducible benchmark and validity protocol](benchmarks/README.md)
- [Quick Start](src/spsc/README.md)
- [Documentation Hub](src/spsc/docs/README.md)
- [Common Concepts](src/spsc/docs/common-concepts.md)
- [Concurrency and FreeRTOS](src/spsc/docs/concurrency-and-freertos.md)
- [Method Recipes](src/spsc/docs/method-recipes.md)
- [Guard and Bulk Helpers](src/spsc/docs/guard-and-bulk-helpers.md)

## Comparison With Other SPSC Libraries

This is a practical feature matrix against popular and representative SPSC and SPSC-adjacent queues or FIFO helpers from the wider C/C++ ecosystem.

It is intentionally not a throughput ranking. The goal is to show what this project covers as first-class API surface, and what other libraries do or do not cover.

Important comparison rule:

- A checked cell means the capability exists as a documented, built-in part of that library's primary API.
- An empty cell does not mean the behavior is impossible to build on top, only that it is not a first-class built-in feature of that library.

Official links:

- [`spsc`](src/spsc/README.md) - this project
- [`Boost lockfree::spsc_queue`](https://www.boost.org/doc/libs/latest/doc/html/doxygen/classboost_1_1lockfree_1_1spsc__queue.html)
- [`ETL queue_spsc_atomic`](https://www.etlcpp.com/queue_spsc_atomic.html)
- [`ETL queue_spsc_locked`](https://www.etlcpp.com/queue_spsc_locked.html)
- [`rigtorp/SPSCQueue`](https://github.com/rigtorp/SPSCQueue)
- [`moodycamel::ReaderWriterQueue`](https://github.com/cameron314/readerwriterqueue)
- [`folly::ProducerConsumerQueue`](https://github.com/facebook/folly/blob/main/folly/ProducerConsumerQueue.h)
- [`PortAudio PaUtilRingBuffer`](https://portaudio.com/docs/v19-doxydocs-dev/pa__ringbuffer_8h.html)
- [`JUCE AbstractFifo`](https://docs.juce.com/master/classjuce_1_1AbstractFifo.html)
- [`atomic_queue`](https://github.com/max0x7ba/atomic_queue)

Legend:

- `[x]` built in as a first-class API
- `[ ]` not part of the library's primary API surface

The `ETL` column below refers to the SPSC family formed by `queue_spsc_atomic` and `queue_spsc_locked`.

### API Model Matrix

| API Model | spsc | Boost | ETL | Rigtorp | Moodycamel | Folly | PortAudio | JUCE | atomic_queue |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Simple value enqueue/dequeue API | [x] | [x] | [x] | [x] | [x] | [x] | [ ] | [ ] | [x] |
| In-place producer construction API (`emplace`-style) | [x] | [ ] | [x] | [x] | [x] | [x] | [ ] | [ ] | [ ] |
| True `claim()` / `publish()` producer model | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Staged producer API (`prepare/finished`, `regions/advance`) | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [x] | [x] | [ ] |
| Consumer peek/front without immediate consume | [x] | [x] | [ ] | [x] | [x] | [x] | [ ] | [ ] | [ ] |
| Bulk or region-based read/write helpers | [x] | [x] | [ ] | [ ] | [ ] | [ ] | [x] | [x] | [ ] |
| Snapshot or captured-range consumer API | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Dedicated blocking queue variant / wait methods | [ ] | [ ] | [ ] | [ ] | [x] | [ ] | [ ] | [ ] | [ ] |

### Capability Matrix

| Capability | spsc | Boost | ETL | Rigtorp | Moodycamel | Folly | PortAudio | JUCE | atomic_queue |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Compile-time fixed-capacity form | [x] | [x] | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [x] |
| Runtime-sized queue form | [x] | [x] | [ ] | [x] | [x] | [x] | [x] | [x] | [x] |
| External caller-owned storage | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [x] | [x] | [ ] |
| Typed non-owning view container | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Raw byte / DMA-slot abstraction | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [x] | [ ] | [ ] |
| Newest-only buffer (`latest`) | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Fixed-array / block wrappers (`array_fifo`, `chunk_fifo`) | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Separate lifetime-managed queue type (`queue`) | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |
| Explicit ISR / callback handoff story in official docs | [x] | [ ] | [x] | [ ] | [ ] | [ ] | [x] | [ ] | [ ] |
| Multiple distinct SPSC container models in one library | [x] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] | [ ] |

### Power-of-Two Capacity Behavior

| Library | Power-of-two behavior | Real implication |
| --- | --- | --- |
| `spsc` | Static capacities are required to be power-of-two; dynamic capacities are normalized to an effective power-of-two geometry | The ring is explicitly designed around mask-based wrap and unambiguous index arithmetic |
| Boost `spsc_queue` | No documented power-of-two requirement | Treat it as an arbitrary-capacity typed queue unless you choose a power-of-two size for your own reasons |
| ETL SPSC family | No documented power-of-two requirement | Fixed-capacity embedded queues, but not presented as power-of-two-only queues |
| `rigtorp/SPSCQueue` | Explicitly supports arbitrary non-power-of-two capacities | It intentionally spends one extra slot instead of forcing a power-of-two restriction |
| `moodycamel::ReaderWriterQueue` | No documented power-of-two requirement | Runtime-sized queue/growing queue model, not documented as power-of-two-constrained |
| `folly::ProducerConsumerQueue` | No documented power-of-two requirement | Runtime-sized fixed queue, but not documented as a power-of-two-only design |
| PortAudio `PaUtilRingBuffer` | Element count must be a power-of-two | This is the strongest explicit power-of-two requirement in the compared set |
| JUCE `AbstractFifo` | No documented power-of-two requirement | It is an index manager over your buffer size, not a power-of-two-specialized ring API |
| `atomic_queue` | Any fixed size is supported, but power-of-two size enables extra optimizations | It benefits from power-of-two geometry but does not strictly require it |

### Real Conclusions

- Boost, Rigtorp, Moodycamel, Folly, ETL, and `atomic_queue` are primarily "one queue API" libraries. They differ in performance goals and ergonomics, but they are still fundamentally queue-centric.
- This project is materially different because it is not only a queue. It gives you multiple SPSC transport models under one API family: assignment queue, lifetime-managed queue, typed pool, raw pool, external-storage views, newest-only state handoff, fixed-array wrappers, and chunk/block wrappers.
- If power-of-two geometry is part of your design philosophy, this project and PortAudio are the clearest explicit matches in the compared set. `atomic_queue` also leans into power-of-two optimizations, but does not make them mandatory.
- If you need caller-owned memory, only `spsc`, PortAudio, and JUCE cover that directly in the compared set. But PortAudio is a C raw ring buffer and JUCE is an index manager over your buffer; neither gives you typed non-owning view containers comparable to `fifo_view`, `pool_view`, or the view wrappers here.
- If you need a real `claim()/publish()` workflow, this project is the only one in this comparison that exposes it as a first-class API model. PortAudio and JUCE provide staged write/read mechanics, but not the same queue abstraction shape.
- If you need newest-state semantics, the compared alternatives do not really match `latest`. With the others you would emulate that policy above a normal queue; here it is a dedicated container model.
- If you need raw DMA-style byte slots with policy-aware alignment and external-storage variants, the closest conceptual neighbor is PortAudio's ring buffer, but PortAudio stays much lower-level and C-oriented.
- ETL is the strongest direct alternative when the target is embedded/ISR handoff and you want a simpler fixed-capacity queue with explicit interrupt-oriented documentation.
- Boost is the strongest direct alternative when you mainly want a mature general-purpose typed SPSC queue with wait-free push/pop and some bulk helpers, but you do not need views, raw pools, or newest-only semantics.
- Rigtorp and Moodycamel are strongest when you mainly want a narrow high-performance typed queue API. They are excellent queue implementations, but they do not try to become a full SPSC container toolkit.

Notes:

- This is a representative set, not a claim that these are literally all SPSC queues on the Internet.
- Some entries are pure SPSC queues, while others are lower-level FIFO primitives that solve adjacent SPSC problems.
- Matrix rows mean "the capability exists somewhere in the library's primary API surface", not necessarily that every type in that library exposes it.
- `JUCE AbstractFifo` is fundamentally an index-management helper over user storage, not a typed queue container.
- `PortAudio PaUtilRingBuffer` is a C single-reader / single-writer raw ring buffer over caller-owned memory, not a family of typed C++ containers.
- `rigtorp/SPSCQueue` does provide blocking `push` / `emplace` when full, but not a separate blocking queue type with explicit wait-style dequeue methods like Moodycamel's blocking variant.
- If you only need one typed queue, Boost, ETL, Rigtorp, Moodycamel, Folly, PortAudio, JUCE, or `atomic_queue` may be a better fit simply because they are narrower.
- If you need one library to cover typed values, explicit lifetime control, raw DMA-style buffers, external storage views, newest-value handoff, and block wrappers, this project is deliberately broader.

## What Is Here

- [`SPSC_HARDENING_ROADMAP.md`](SPSC_HARDENING_ROADMAP.md): the active,
  slice-by-slice contract, correctness, test, CI, and performance hardening plan.
- [`benchmarks/`](benchmarks/README.md): reproducible, validity-gated measurement
  harness with a pinned Rigtorp comparator and explicit `inconclusive` output.
- `src/spsc/`: core SPSC library headers (`fifo`, `queue`, `typed_pool`, `fifo_view`, `pool`, `pool_view`, `latest`, `chunk`, etc.).
- `src/tests/*_test.cpp`: paranoid test suites for each buffer type.
- `spsc_test.pro`: Qt/qmake project file.
- `mainwindow.cpp`: Qt dashboard that runs suites, shows `PASS/FAIL`, logs, and timeout status.

Detailed API documentation is in:

- `src/spsc/README.md` for the quick start and top-level overview
- `src/spsc/docs/README.md` for the new split-by-container documentation hub
- `src/spsc/docs/concurrency-and-freertos.md` for task/ISR/FreeRTOS guidance

## Build

Prerequisites:

- Qt 6.x with `QtTest`
- MinGW toolchain (or compatible C++ toolchain configured in Qt)

Typical qmake build flow (Windows/MinGW):

```powershell
mkdir build
cd build
qmake ..\spsc_test.pro
mingw32-make -j8
```

If you use Qt Creator, opening `spsc_test.pro` is enough.

## Run Tests

Launch the Qt dashboard:

```powershell
.\bin\debug\spsc_launcher.exe
# or
.\bin\release\spsc_launcher.exe
```

The dashboard and runner executables are built per configuration:

```powershell
.\bin\debug\spsc_test_shadow_on.exe --run-suite fifo
.\bin\release\spsc_test_shadow_on.exe --run-suite fifo
```

The C++20 runner enables `SPSC_HAS_SPAN=1`; it is separate from the C++17
dashboard matrix so a missing `std::span` library cannot silently disable its
contracts:

```powershell
.\bin\debug\spsc_test_cxx20_span.exe --run-suite fifo span_contract
.\bin\debug\spsc_test_cxx20_span.exe --run-suite pool span_contract
.\bin\debug\spsc_test_cxx20_span.exe --run-suite queue raw_bytes_contract
.\bin\debug\spsc_test_cxx20_span.exe --run-suite chunk span_contract
```

H6 also has two standalone policy targets. The first succeeds only when the
compiler rejects relaxed atomic publication. Run the second from an x86 Visual
Studio Developer PowerShell (or another genuine 32-bit compiler environment):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\test_relaxed_publication_compile_fail.ps1 -Compiler C:\msys64\ucrt64\bin\g++.exe
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_h6_32bit_shadow_matrix.ps1 -Compiler cl
```

## Clean Verification And CI

H7 runs each qmake target from a fresh, uniquely named temporary build
directory. It never reads or removes the repository's `build/`, `bin/`, MOC,
or object artifacts; use `-KeepBuild` only when a failed generated tree needs
to be inspected.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run_h7_matrix.ps1 `
  -Configuration Both -Variant all -BuildLauncher
```

On Linux, the same functional matrix and the standalone sanitizer target are:

```bash
bash scripts/run_h7_matrix.sh --qmake qmake6 --configuration both --variant all
bash scripts/run_h7_sanitizers.sh --sanitizer address,undefined
bash scripts/run_h7_sanitizers.sh --sanitizer thread
bash scripts/run_h6_32bit_shadow_matrix.sh --compiler g++
```

`run_h7_matrix.ps1` accepts `-Qmake`, `-Make`, and `-Compiler` paths when the
Qt kit is not already configured on `PATH`. The GitHub Actions workflow runs
the explicit C++17 shadow Debug/Release matrix, C++20 span Debug/Release
targets, Linux sanitizers, genuine 32-bit execution, AArch64 header smoke, and
Windows MinGW/MSVC header smoke.

The dashboard:

- runs each suite in a separate child process
- shows `PASS`, `FAIL`, `TIMEOUT`, or `CRASH`
- captures the full QtTest log in a text pane
- supports a per-suite timeout from the UI

Main suites include:

- `fifo`
- `fifo_view`
- `pool`
- `pool_view`
- `latest`
- `chunk`
- `queue`
- `typed_pool`
- `buffer_pool`

## Latest Test Report (Integrated Run)

Run sources:

- `build/Desktop_Qt_6_10_1_MinGW_64_bit-Debug/bin/debug/spsc_test_shadow_*.exe`
- `build/Desktop_Qt_6_10_1_MinGW_64_bit-Debug/bin/release/spsc_test_shadow_*.exe`

Environment:

- QtTest 6.10.1 / Qt 6.10.1
- GCC 13.1.0
- Windows 11

Matrix:

- Debug and Release
- `shadow_off`, `shadow_on`, `shadow_heur`
- `fifo`, `fifo_view`, `pool`, `pool_view`, `latest`, `chunk`, `queue`, `typed_pool`, `buffer_pool`

Conclusion:

- 54/54 suite runs passed with 30s per-suite timeout.
- The qmake matrix is a C++17 matrix and forces `SPSC_HAS_SPAN=0`; C++20 `std::span` helpers are intentionally outside this matrix.

## Notes About Death Tests

Debug death tests use child process spawning (`QProcess`).  
In restricted environments they can fail with:

- `QProcess: CreateFile failed. (Access is denied.)`

In a normal local dev environment these tests should pass.

## License

This project is licensed under the [Apache License 2.0](LICENSE).

For the core library sources in [`src/spsc`](src/spsc), the license is also marked directly in file headers using `SPDX-License-Identifier: Apache-2.0`.
