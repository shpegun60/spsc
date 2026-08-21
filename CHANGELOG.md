# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog and the project follows Semantic Versioning.

## [Unreleased]

### Fixed

- `fifo` and `typed_pool` now report copy construction and copy assignment
  accurately through standard type traits. Their deep-copy operations are
  unavailable when the payload cannot support the operation they actually
  perform.
- Static value-storage containers now have a valid assignment-based swap
  fallback for payloads that deliberately omit ADL `swap` but satisfy the
  container's existing default-construction and assignment requirements.
  Array and cache-aligned wrapper storage is exchanged element by element,
  avoiding a whole-storage stack temporary, and the fallback prefers a
  no-throw copy assignment when move assignment may throw.
- Static `latest` no longer advertises move construction, move assignment, or
  swap when its payload cannot support the storage exchange those operations
  perform.
- Object-queue construction and dynamic migration now select global
  placement-new explicitly, while raw-slot migration and destruction use
  pointer arithmetic instead of class-specific address lookup. Payload classes
  with custom allocation or address functions can no longer hide those
  operations.
- `latest`, `chunk`, `typed_pool`, and object-queue guard helpers now remove
  unsupported `push`/`emplace` calls during overload resolution instead of
  accepting a detection expression and failing only in the function body.
- `cache_aligned_slot` no longer advertises construction from `const T&` or
  `T&&` when the wrapped type cannot perform that construction.
- Custom counter backends are validated against the real no-throw
  `value_type`/`store`/`load`/`add`/`inc` contract, including an optional
  `load_relaxed()`. Custom `Policy::allocator_alignment` values must now be
  positive, `std::size_t`-representable integral or enum powers of two.

### Tests

- Added standalone trait/SFINAE runtime smoke coverage, including bounded
  array-swap scratch and class-specific placement-new/address regressions for
  static producer paths and dynamic resize/destruction, plus compile-fail gates
  for invalid custom counter and allocator-alignment extensions.

## [3.0.0] - 2026-08-20

This major release changes the implicit `default_policy` for bare containers.
It deliberately does not alter endpoint algorithms, fully specified policies,
or the semantic aliases introduced in v2.1.

### Changed

- With no `SPSC_DEFAULT_POLICY_ATOMIC` definition, `default_policy` is now
  `FA<>`. Bare policy-based containers such as `fifo<T, N>` therefore use the
  portable single-writer atomic SPSC backend by default.
- `SPSC_DEFAULT_POLICY_ATOMIC` is now a legacy explicit configuration override
  rather than a synthesized library default: explicit `0` remains `P`, and
  explicit `1` remains strict `A<>`.
- `local_*`, `concurrent_*`, and `cache_aligned_*` remain fixed respectively to
  `P`, `FA<>`, and `CFA<>`, independently of the legacy override.
- `policy::CacheAligned<>` has always defaulted its `Base` argument to
  `default_policy`; it therefore now means `CFA<>` in the normal v3
  configuration. Spell `CP`, `CFA<>`, or `CA<>` when that base must be fixed.

### Breaking

- A bare container compiled without the legacy macro is a different C++ type
  from its v2.1 counterpart. Its policy type, metadata layout, `sizeof`,
  `alignof`, generated code, atomic requirements, and ABI may change.
- The P-to-FA switch alone does not change the shipped policy-derived default
  allocator: both policies request allocator alignment `1`. Explicit
  cache-aligned policies still select their aligned allocator path.
- `buffer_pool` also uses `default_policy` when its policy is omitted. It is
  not an SPSC endpoint; its P/FA physical layout and default allocator remain
  the same, but its C++ type changes and the bare form can now inherit the
  lock-free atomic toolchain requirement. Storage-sensitive uses should spell
  `P` or `CP` explicitly as appropriate.

### Migration

- Before upgrading, replace v2 plain bare containers with `local_*` when the
  plain layout and external-synchronization contract must remain unchanged.
- Move real one-producer/one-consumer handoffs to `concurrent_*` (or
  `cache_aligned_*` when justified) before or during the upgrade.
- See [`migration-v3.md`](src/spsc/docs/migration-v3.md) for the exact macro
  compatibility table and source migration examples.

## [2.1.0] - 2026-08-19

This backward-compatible feature release adds semantic aliases for the normal
SPSC policy choices. It does not change container algorithms, memory ordering,
ownership rules, `default_policy`, or the default value of
`SPSC_DEFAULT_POLICY_ATOMIC`.

### Added

- `local_*`, `concurrent_*`, and `cache_aligned_*` type aliases for every
  policy-driven SPSC transport container and view: `fifo`, `queue`,
  `fifo_view`, `pool`, `pool_view`, `typed_pool`, `latest`, `array_fifo`,
  `array_fifo_view`, `carray_fifo_view`, `chunk_fifo`, and `chunk_fifo_view`.
  They bind respectively to `P`, `FA<>`, and `CFA<>` without a runtime wrapper.
- Semantic aliases preserve each owning container's existing policy-derived
  default allocator and still accept an explicit custom allocator.
- Compile-time identity coverage for every semantic alias and runtime smoke
  coverage for the three primary `fifo` spellings.

### Documentation

- New code is guided to use `concurrent_*` for portable one-producer/
  one-consumer transport and `cache_aligned_*` only when cache isolation is
  appropriate for the measured target.
- The full `Container<..., Policy, ...>` form remains the advanced API for
  policies such as `A<>`, `CA<>`, `V`, `VV`, `CP`, `AA<>`, and `CAA<>`.
- `buffer_pool` deliberately has no concurrency-named aliases: its policy
  describes storage layout, so DMA storage continues to use explicit `CP`.

## [2.0.3] - 2026-08-19

This CI-only maintenance release makes hosted Linux verification resilient to
transient Ubuntu mirror stalls. It does not change public SPSC headers, runtime
behavior, policies, aliases, or container ownership semantics.

### Fixed

- GitHub Actions Linux setup now prefers the canonical Ubuntu archive when a
  hosted runner exposes a stalled Azure mirror endpoint.
- All Linux `apt-get` setup steps use bounded retries and HTTP(S) timeouts.
- The AArch64 header smoke and Qt functional matrix have enough time to reach
  their actual compile/test steps after dependency installation.

## [2.0.2] - 2026-08-19

This is a narrow bug-fix and contract-documentation release. It does not
change SPSC endpoint algorithms, policies, aliases, or container ownership
semantics.

### Fixed

- Dynamic `chunk<T, 0>::reserve()` now prefers noexcept copy assignment when
  move assignment may throw.
- Static `chunk<T, N>::swap()` now exposes the correct conditional `noexcept`.
- Zero-valued cacheline configuration inputs are rejected before cacheline
  normalization.

### Documentation and Tests

- Clarified `latest::coalescing_publish()` true/false semantics: `false` is
  not eventual publication and a pending producer slot may be overwritten.
- Updated newest-state and FreeRTOS examples to observe the publication result
  before notifying a consumer.
- Added regressions for chunk assignment selection, conditional swap noexcept,
  pending coalesced slots, and zero cacheline configurations.

## [2.0.1] - 2026-08-05

This maintenance release hardens packaging, portability, metadata layout, and
STM32H7 buffer-storage contracts without changing the SPSC endpoint mechanics.

### Added

- Standalone C++17 checks that compile every public header independently.
- A standalone qmake consumer that validates both short and `spsc/...` include
  forms using only `spsc.pri`.
- An STM32H7 buffer-pool smoke test covering the 100-byte payload, 128-byte
  physical stride, and 32-byte alignment contract.
- Explicit `buffer_pool` aliases: `payload_bytes()`, `cache_span_bytes()`, and
  `storage_alignment()`.

### Changed

- Clarified that `V`, `VV`, `CV`, and `CVV` rely on external platform/toolchain
  ordering and do not establish portable C++ acquire/release synchronization;
  `CA`/`CFA` remain the recommended task/task and ISR/task policies.
- Documented the STM32H7 split between `CP` buffer storage and `CFA<>` SPSC
  transport metadata, including payload versus cache-maintenance span.

### Fixed

- Completed `spsc.pri` include roots and header manifest, including
  `buffer_pool.hpp`, `spsc_slot_wrap.hpp`, and the root `basic_types.h`.
- Added direct `<utility>` dependencies to headers that use utility facilities.
- Removed zero-length `CacheSlot` padding storage, preventing exact-cache-line
  nested/custom counter layouts from being inflated by an extra cache line.

## [2.0.0] - 2026-08-03

This major release contains intentional source and object-layout/ABI changes
relative to `v1.0.0`.

### Added

- A reproducible STM32 Cortex-M4/M7 assembly probe for the `fast_fifo` and
  `fast_queue` alias targets, with explicit `CA<>`/`CFA<>` selectors.
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

- `fast_fifo` and `fast_queue` now select the SPSC-specialized `CFA<>`
  single-writer atomic backend instead of strict-RMW `CA<>`. This intentional
  concrete-type/source/ABI change is confined to 2.0; callers requiring the old
  backend can spell `CA<>` explicitly.
- Release documentation makes no general SPSC-versus-Rigtorp winner or parity
  claim. H0R measurements changed classification with endpoint direction and
  physical-core pair, so retained results are explicitly scoped or
  `inconclusive`.
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
- Removed the accidentally tracked Qt Creator per-user project file. Qt Creator
  user files remain ignored and local to each checkout.

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

[Unreleased]: https://github.com/shpegun60/spsc/compare/v3.0.0...HEAD
[3.0.0]: https://github.com/shpegun60/spsc/compare/v2.1.0...v3.0.0
[2.1.0]: https://github.com/shpegun60/spsc/compare/v2.0.3...v2.1.0
[2.0.3]: https://github.com/shpegun60/spsc/compare/v2.0.2...v2.0.3
[2.0.2]: https://github.com/shpegun60/spsc/compare/v2.0.1...v2.0.2
[2.0.1]: https://github.com/shpegun60/spsc/compare/v2.0.0...v2.0.1
[2.0.0]: https://github.com/shpegun60/spsc/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/shpegun60/spsc/releases/tag/v1.0.0
