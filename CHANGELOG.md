# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog and the project follows Semantic Versioning.

## [Unreleased]

### Changed

- Default public policy selection is now safe-by-default atomic (`A<>`) unless `SPSC_DEFAULT_POLICY_ATOMIC=0` is defined before including the library.
- Atomic counter policies now reject memory orders that cannot publish SPSC payloads safely, including `relaxed_orders` for normal container policies.

### Fixed

- Hardened snapshot consumption against stale lifecycle epochs and mismatched begin/end iterators.
- Hardened release builds for invalid/full/empty non-try pool and typed-pool paths that previously relied on debug assertions.

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

[1.0.0]: https://github.com/shpegun60/spsc/releases/tag/v1.0.0
