# Changelog

All notable changes to this project will be documented in this file.

The format is based on Keep a Changelog and the project follows Semantic Versioning.

## [Unreleased]

### Fixed

- Unsupported payload categories are now rejected at the contract level
  instead of failing deep inside template instantiation: `queue` rejects
  volatile and raw-array payloads, `typed_pool` rejects const, volatile,
  and raw-array payloads, and owning `fifo` rejects volatile payloads,
  whose manual-lifetime and trivial-copy management paths cannot support
  them (`std::array` remains the supported aggregate payload, and
  array-element views such as `carray_fifo_view` are unaffected). The
  fully static `buffer_pool` no-exceptions gate now also requires nothrow
  copy construction, matching the copy its defaulted copy constructor
  performs.
- Runtime-size `buffer_pool` introspection accessors (`count()`, `size()`,
  `size_bytes()`, `span_bytes()`, `data(i)`, `operator[]`) now use an O(1)
  shape check plus the per-slot null guard instead of re-running the full
  pointer-table scan on every call, so `for (i < count()) data(i)` loops are
  O(N) instead of O(N²). `is_valid()` keeps its deep O(N) integrity-scan
  semantics and still guards the accessors in assert-enabled builds.
- Runtime-shaped `buffer_pool` forms no longer swallow user exceptions in
  exception-enabled builds: a throwing `T` construction or copy assignment
  inside copy construction, copy assignment, or `resize()` now cleans up
  every transient allocation (including already-built sibling buffers) and
  propagates, matching the rest of the owning containers. The destination of
  an assignment or resize is preserved, a copy constructor can no longer
  silently produce an empty pool, and null-returning allocation failure
  still reports `false` without throwing.
- Single RAII write/read guards in `fifo`, `fifo_view`, `pool`, `typed_pool`,
  and `queue` now commit through the checked `try_publish()`/`try_pop()`
  paths instead of the unchecked owner commit, preserving the consumer/
  producer shadow their checked acquisition already proved on explicit
  32-bit shadow-enabled builds. Bulk `unsafe` guards keep their unchecked
  commit-and-poison semantics; `pool_view` single guards already committed
  through validated snapshots.
- Public headers now survive Windows' function-like `min`/`max` macros:
  every `std::numeric_limits<T>::max()`/`min()` call uses the
  `(std::numeric_limits<T>::max)()` idiom (16 call sites across the allocator
  core, `chunk`, `fifo`, `fifo_view`, `pool`, `pool_view`, and `typed_pool`).
  The library never `#undef`s the consumer's macros.
- Raw `latest<void>` `push(U)`/`try_push(U)` no longer decay their argument:
  a C-array payload now publishes its contents instead of the decayed
  pointer's address bytes, volatile arguments are rejected at the contract
  level, and the intermediate `O(sizeof(U))` stack temporary is gone — the
  slot is filled by one direct representation copy, matching `pool::push(U&)`.
- `queue::consume_all()` and `typed_pool::consume_all()` now consume exactly
  the prefix visible in one fresh consumer snapshot, so a concurrently active
  producer cannot make the operation unbounded and later publications survive
  for the next consumer pass.
- Static `fifo`/`latest` move and `chunk` construction traits now include the
  destination operations their bodies actually perform. Lifetime-owning
  containers reject throwing destructors, and allocators constructed from
  `noexcept` paths must have no-throw default constructors.
- Raw typed overlays in `pool`, `pool_view`, and `latest<void>` now bypass
  class-specific address operators, participate only for trivially-copyable
  types, reject cv-qualified outputs, and fail closed on oversized or invalid
  guarded writes without advancing publication state.
- `CachelineCounter` validates its underlying counter directly. Allocator
  `size_type` domains must represent `reg`, and an empty dynamic `queue` can
  resize for an immovable payload while a non-empty one rejects growth without
  changing state.
- `fifo_view` no longer imposes owning-container construction, destruction, or
  assignment requirements on externally owned payloads. Typed `latest` can use
  claim/publish-only payloads that are default-constructible but non-assignable.
- No-exceptions builds now reject throwing custom allocators on every owning
  storage path that actually allocates. `fifo` and `typed_pool` deep-copy
  traits likewise require the copy operation used by their implementation to
  be `noexcept`, and static `fifo` move prefers an available no-throw copy over
  a potentially throwing move assignment. Dynamic `chunk` now owns eager slot
  construction directly instead of delegating lifetime control to a custom
  `Alloc::construct()` hook.
- Explicit sub-64-bit shadow configurations now invalidate endpoint-local
  caches after unchecked single and bulk progress, closing the stale-shadow
  full-period alias that could otherwise overwrite unread data or expose an
  empty slot. Checked commits retain their cached fast path, and 64-bit H8 code
  is unchanged.
- Validated `queue::try_pop(n)` and `queue::try_consume(snapshot)` operations
  now share prefix destruction with their unchecked counterparts but advance
  through the checked consumer path, preserving a proven consumer shadow.
- `try_consume(snapshot)` in `fifo`, `fifo_view`, `pool`, `pool_view`, and
  `typed_pool` (inherited by the `array_fifo` and `chunk_fifo` wrappers) now
  commits through the checked consumer path instead of the unchecked `pop(n)`
  commit, matching `queue` and preserving the proven consumer shadow on
  explicit 32-bit shadow-enabled configurations. `typed_pool` object
  destruction for `pop(n)`/`try_pop(n)`/`try_consume` now shares a single
  prefix-destruction helper.
- In no-exceptions mode, a non-empty dynamic `queue` rejects growth before
  allocation unless its live values have a no-throw move or copy construction
  path. Empty queues can still grow for immovable or throwing-relocation types.
- Container construction now explicitly normalizes custom counter and geometry
  backends to zero, including the auxiliary `buffer_size` state in `pool` and
  `pool_view`; it no longer relies on a custom backend's default value.
- Dynamic pointer tables in `pool`, `typed_pool`, raw `latest<void>`, and the
  fully dynamic `buffer_pool` now explicitly begin every pointer element's
  lifetime before assignment, preserving the C++17 raw-storage contract while
  bypassing custom allocator construction hooks.
- Static `fifo` copy assignment no longer materializes a second embedded
  payload array on the stack. Static `pool`/`typed_pool` and runtime-sized
  fixed-count `buffer_pool` copy/resize transactions now keep their temporary
  pointer tables in allocator-backed storage, bounding management stack use
  independently of compile-time `Capacity`/`Count` without changing persistent
  container layout. Custom allocators for those forms must therefore support
  the documented raw pointer-table rebind, including no-throw allocation in
  no-exceptions builds.

### Tests

- Added active-producer bounded-consume regressions, hostile exception and raw
  overlay smokes, immovable-queue coverage, and compile-fail checks for invalid
  counter, allocator-domain, allocator-construction, and destructor contracts.
- Added mode-0 compile-fail coverage for throwing allocators across all owning
  allocation shapes, copy-trait regressions, static-fifo move-selection checks,
  exception-mode multi-allocation rollback verification, and a hostile
  allocator regression proving that `chunk` never calls `Alloc::construct()`.
- Added deterministic static/dynamic genuine-32-bit stale-shadow wrap probes
  for strict-RMW and single-writer atomic policies, mode-0 queue
  relocation/state-preservation coverage, hostile non-zero counter/geometry
  construction checks, hostile allocator regressions for all four dynamic
  pointer-table allocation paths, and checked queue-path shadow-preservation
  coverage. Hostile stale-shadow setup is kept separate from synchronized
  checked-path setup.
- Extended the genuine-32-bit H6 shadow matrix with checked `try_consume`
  shadow-preservation regressions for `fifo`, `fifo_view`, `pool`, `pool_view`,
  and `typed_pool` under both strict-RMW and single-writer counting policies.
- Added raw `latest` C-array publish and oversized-payload rejection
  regressions, plus a bounded-stack probe proving raw `latest` publish of a
  4096-byte payload keeps an O(1) frame.
- Extended the genuine-32-bit H6 shadow matrix with single write/read guard
  shadow-preservation regressions across `fifo`, `fifo_view`, `pool`,
  `typed_pool`, and `queue` under both counting policies.
- Added unique-diagnostic compile-fail coverage for const/volatile/raw-array
  payload rejection in `queue`, `typed_pool`, and `fifo`, and for the fully
  static `buffer_pool` mode-0 copy-construction gate.
- Extended the exception runtime smoke with `buffer_pool` regressions for
  all three runtime shapes: throwing default construction and copy
  assignment propagate from copy construction, copy assignment, and
  `resize()` with zero leaks and preserved destinations, while
  null-returning allocation failure still reports `false`.
- Added allocation-failure/state-preservation regressions for transient static
  management tables and an exception-mode static-`fifo` basic-guarantee test.
- Compile-fail verification now requires exactly one unique library contract
  diagnostic per negative scenario, and bounded-stack wrapper probes cover
  both large outer FIFO capacity and large inner array/chunk capacity.

### CI

- Added exception-enabled runtime and ASan+UBSan coverage plus Cortex-M7 object
  code generation with symbol/disassembly checks for unwanted runtime helpers.
- Added host (both exception modes) and Cortex-M7 `-fstack-usage` gates that
  reject static management frames above 512 bytes, using
  `Capacity`/`Count == 4096` probes plus `array_fifo`/`chunk_fifo` inner
  dimensions of 4096.
- Added a hostile Windows macro smoke to the MSVC header job: `<windows.h>`
  is included without `NOMINMAX` before every public header, so an unguarded
  `numeric_limits` call fails the build.

### Documentation

- Clarified C++17 view-state recovery syntax, empty `buffer_pool` validity, and
  RAII guard side effects during exception unwinding; added the missing SPDX
  header to the internal value-swap helper.
- Documented snapshot epoch/ABA limits, the owning `pool::push(U)` size
  precondition, finite-period shadow behavior, custom-backend zero
  normalization, and the bounded-stack management/failure contracts for
  static owning containers. Clarified that actual dynamic `latest` growth
  rebuilds storage and clears published state, while no-op management calls
  preserve it.

## [3.0.2] - 2026-08-21

This patch release closes the remaining multi-axis management and generic C++
edge cases from the post-v3 audit, and adds genuine 32-bit reserve plus
Cortex-M7 toolchain gates. It does not change the v3 default-policy model or
public state layouts.

### Fixed

- Dynamic raw `latest` now treats depth and slot bytes as independent
  grow-only axes in both `reserve()` and non-zero `resize()` calls. A
  successful reserve cannot shrink the other axis, and `reserve(0, bytes)` no
  longer reports success on an invalid object that cannot provide storage.
- Dynamic `pool::resize(depth, buffer_size)` now retains the existing slot
  width while depth grows, preventing silent payload-suffix truncation.
- Payload address lookup in `fifo`, `fifo_view`, typed `latest`, `chunk`, and
  snapshot iterators now bypasses class-specific `operator&` consistently.
- Custom counter validation now rejects a `noexcept` load whose conversion to
  its declared value type or the library index type can throw, including the
  optional relaxed-load path.

### Tests

- Added full four-direction depth/slot-width resize matrices and hostile
  payload address regressions.
- The genuine 32-bit job now executes reserve boundaries for dynamic `fifo`,
  `queue`, and typed/raw `latest`, including allocator and state-preservation
  checks at `RB_MAX_UNAMBIGUOUS` and above it.

### CI

- Added a bare-metal `arm-none-eabi-g++` Cortex-M7/Thumb syntax gate using the
  actual newlib C++ headers, with both normal and explicitly enabled 32-bit
  shadow configurations.

### Documentation

- Clarified grow-only multi-axis management, exception-time prefix publication
  by object bulk guards, and the fact that view `state_t` is not a portable
  persistence or wire format.

## [3.0.1] - 2026-08-21

This patch release closes the post-v3 trait, raw-storage, reserve, and recovery
contract findings without changing the v3 default-policy model or public state
layouts.

### Fixed

- Dynamic `fifo`, `queue`, and typed/raw `latest` reserve operations now reject
  requests above `RB_MAX_UNAMBIGUOUS` instead of silently forwarding them to a
  clamping resize path. Whenever `reserve(n)` succeeds, the resulting ring
  capacity is now guaranteed to be at least `n`.
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

- Added deterministic reserve-boundary coverage for `RB_MAX_UNAMBIGUOUS`, the
  first value above it, and `std::numeric_limits<reg>::max()`, including
  allocator-call and state-preservation checks.
- Added illustrative `fifo_view` and `pool_view` recovery coverage showing why
  head/tail indices alone cannot validate a different capacity/mask, slot
  order, or raw-buffer layout.
- Added standalone trait/SFINAE runtime smoke coverage, including bounded
  array-swap scratch and class-specific placement-new/address regressions for
  static producer paths and dynamic resize/destruction, plus compile-fail gates
  for invalid custom counter and allocator-alignment extensions.

### Documentation

- Clarified that view `state_t` values contain indices only and require
  separately validated recovery geometry and backing layout.
- Documented the opt-in/no-op default `SPSC_ASSERT` hook, no-exception
  allocation-failure validity checks, static `queue::destroy()` lifetime, and
  the required `mark_written()` step after FIFO bulk-guard `get_next()`.
- Corrected manual placement-new examples to select global placement new via
  `void*`, and corrected the raw `pool::try_push()` failure comment.

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

[Unreleased]: https://github.com/shpegun60/spsc/compare/v3.0.2...HEAD
[3.0.2]: https://github.com/shpegun60/spsc/compare/v3.0.1...v3.0.2
[3.0.1]: https://github.com/shpegun60/spsc/compare/v3.0.0...v3.0.1
[3.0.0]: https://github.com/shpegun60/spsc/compare/v2.1.0...v3.0.0
[2.1.0]: https://github.com/shpegun60/spsc/compare/v2.0.3...v2.1.0
[2.0.3]: https://github.com/shpegun60/spsc/compare/v2.0.2...v2.0.3
[2.0.2]: https://github.com/shpegun60/spsc/compare/v2.0.1...v2.0.2
[2.0.1]: https://github.com/shpegun60/spsc/compare/v2.0.0...v2.0.1
[2.0.0]: https://github.com/shpegun60/spsc/compare/v1.0.0...v2.0.0
[1.0.0]: https://github.com/shpegun60/spsc/releases/tag/v1.0.0
