# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog and the project follows Semantic Versioning.

## [Unreleased]

### Changed

- qmake dashboard/test runner builds now target C++17 and keep Debug/Release binaries in separate `bin/debug` and `bin/release` directories.
- qmake C++17 test runners force `SPSC_HAS_SPAN=0`; `std::span` helpers remain available only for C++20-capable builds that enable span support.
- The default SPSC policy remains plain (`SPSC_DEFAULT_POLICY_ATOMIC=0`), while lock-free atomics remain required by default (`SPSC_REQUIRE_LOCK_FREE=1`).

### Fixed

- Restored Qt shadow-build MOC/test runner layout so the dashboard resolves per-config runner executables reliably.

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
