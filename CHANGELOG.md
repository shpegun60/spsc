# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog and the project follows Semantic Versioning.

## [Unreleased]

### Added

- Reproducible SPSC baseline harness with raw JSONL samples, host/compiler
  manifest, hot-path assembly capture, CPU-affinity controls, and a pinned
  `rigtorp/SPSCQueue` v1.1 comparator.
- Three-thread atomic observer regression coverage: producer, consumer, and a
  simultaneous public-query observer.
- Counter-wrap regressions that cross the actual unsigned index boundary in
  short runs, cover static/dynamic `fifo_view::adopt()`, shadow-cache refresh,
  and fail-closed handling of invalid restored state.
- A policy/configuration test matrix covering direct `VV` execution, explicit
  acquire/release and `seq_cst` atomic order palettes, expected rejection of
  relaxed publication, and genuine 32-bit shadow gating.
- A separate C++20 `SPSC_HAS_SPAN=1` qmake runner with runtime contracts for
  `fifo::span`, `pool::span`, `queue::raw_bytes`, and chunk span views.
- A clean-build verification path: isolated qmake matrix runners, standalone
  public-header and atomic-observer sanitizer targets, and GitHub Actions jobs
  for Linux GCC/Clang, sanitizers, genuine 32-bit execution, ARM smoke, and
  Windows MinGW/MSVC header smoke.
- Compiler-specific H8 hot-path assembly checks for FIFO and lifetime-managed
  queue probes on GCC, Clang, and MSVC.
- Cross-toolchain public-header assertions that keep allocation-state bases,
  H8 snapshots, and cached endpoint helpers out of container object APIs.

### Changed

- Documented exact SPSC endpoint ownership, same-side claim/front transaction
  rules, atomic-observer cache traffic, `CA<>` versus `CFA<>`, allocation and
  lifetime boundaries, bounded `latest` semantics, and the fact that external
  views cannot realign caller-owned storage. `fast_*` names are now explicitly
  documented as legacy aliases rather than throughput claims.
- The SPSC benchmark now uses topology-aware physical-core affinity,
  prefers two non-zero physical P/SMT cores when Windows topology permits,
  uses persistent pinned workers, both producer/consumer CPU assignments,
  paired alternating samples, pre-touched queues, CPU-relax retries, endpoint
  CPU-time and thread-cycle telemetry, and optional cross-revision header
  selection.
- Benchmark format version 2 emits per-direction paired summaries and a gated
  `spsc_faster`/`rigtorp_faster`/`parity`/`inconclusive` comparison result.
  Legacy single-direction captures are diagnostic-only after identical Windows
  runs were observed to reverse the apparent winner.
- Benchmark manifests now distinguish release evidence from diagnostic history;
  release captures require clean library/harness revisions, a minimum sample
  protocol, unchanged pre/post power state, and distinct bidirectional
  producer/consumer affinity.
- qmake dashboard/test runner builds now target C++17 and keep Debug/Release binaries in separate `bin/debug` and `bin/release` directories.
- qmake C++17 test runners force `SPSC_HAS_SPAN=0`; `std::span` helpers remain available only for C++20-capable builds that enable span support.
- qmake test runners can now select their language standard and span setting;
  the dashboard remains on its existing C++17 matrix while C++20 span coverage
  is an explicit target.
- qmake-generated MOC, object, RCC, and UI paths are relative to each build
  directory, so a fresh out-of-source build cannot mistake an absolute MOC
  dependency for an already generated file.
- The default SPSC policy remains plain (`SPSC_DEFAULT_POLICY_ATOMIC=0`), while lock-free atomics remain required by default (`SPSC_REQUIRE_LOCK_FREE=1`).
- Single-item endpoint operations now reuse one owner-index snapshot. Checked
  `try_*` operations retain the opposite-endpoint acquire check; contract-
  precondition operations use an owner-only Release path. Strict `CA<>`
  publication remains RMW, while single-writer counter backends publish from
  the captured owner value.

### Fixed

- Atomic SPSC index metadata and eligible shadows now live in separate
  cache-line-aligned producer/consumer owner blocks. Non-atomic and
  shadow-disabled storage stays compact, while atomic shadow eligibility is
  unchanged.
- Public occupancy queries now use direct index snapshots and never mutate
  producer/consumer shadow caches. Endpoint operations retain their role-local
  lazy cache, so a third atomic observer does not race a shadow.
- Restored Qt shadow-build MOC/test runner layout so the dashboard resolves per-config runner executables reliably.
- Clean qmake verification now forces source-MOC generation for the large
  `fifo_view` test translation unit, preserving Qt 6.4 clean-build coverage.
- Dynamic `fifo` storage now preserves its actual member alignment, including
  pointer alignment on 64-bit Clang targets.
- The threaded `pool` stress test now compares semantic `Blob` contents rather
  than unspecified tail padding.
- `queue` and `typed_pool` now inherit their allocation-state implementation
  bases privately, so `isAllocated_` and base conversions do not leak through
  the public container API.
- GitHub Actions now verifies that relaxed atomic publication policies fail for
  the intended memory-order assertion.
- The relaxed-publication compile-fail contract now exits successfully after
  observing the expected compiler rejection, instead of leaking that
  intentional non-zero compiler status through Linux `pwsh`.

### Removed

- Removed the untagged `fifo_view` and `pool_view` bulk-region overloads. Raw
  regions now consistently require `claim_write(spsc::unsafe, ...)` or
  `claim_read(spsc::unsafe, ...)` across all containers. This is an intentional
  source-breaking cleanup made before those convenience overloads had consumers.

## [1.0.0] - 2026-03-21

First stable public release of the `spsc` container library.

### Added

- Core SPSC container family under `src/spsc`: `fifo`, `fifo_view`, `queue`, `pool`, `pool_view`, `latest`, `typed_pool`, `array_fifo`, `chunk_fifo`, and `chunk`.
- Shared SPSC base layer with policy-driven metadata, cacheline-aware policy variants, power-of-two geometry, snapshots, region helpers, and allocator support.
- Paranoid test suites for the main containers and a Qt dashboard runner with suite status, logs, and timeout handling.
- Full split documentation set under `src/spsc/docs/`, including Quick Start, per-container guides, method recipes, guard/bulk helper notes, concurrency guidance, FreeRTOS examples, and STM32H7 cache-alignment guidance.
- Root README with project overview, quick links, dashboard screenshot, and feature comparison against other representative SPSC libraries.
- Apache-2.0 licensing with SPDX markers in library source files.

### Changed

- Hardened queue, fifo, fifo_view, pool, pool_view, latest, typed_pool, and chunk contracts through repeated API review and paranoid test expansion.
- Refined cache-aligned allocator defaults so cache-aware policies automatically improve alignment behavior where the container model can safely derive it.
- Removed the unsafe `sync_head_to_tail()` base API and kept cache/shadow synchronization paths explicit and one-directional.
- Normalized library file headers and authorship metadata to `Shpegun60`.

### Notes

- `1.0.0` marks the first intended stable API line for the library.
- Future bug-fix-only updates should use `1.0.x`.
- New backward-compatible features should use `1.1.0`, `1.2.0`, and so on.

[Unreleased]: https://github.com/shpegun60/spsc/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/shpegun60/spsc/releases/tag/v1.0.0
