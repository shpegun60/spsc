# SPSC Hardening Roadmap

Status: active
Audit baseline: `eaea6fbb06c1`
Created: 2026-08-02
Scope: `src/spsc`, its tests, documentation, benchmarks, and build/CI support

## Purpose

This roadmap turns the current audit into small, reviewable slices. Every slice
must leave the repository buildable, documented, and green. Contract and
correctness work comes before hot-path tuning.

The current architecture is sound and does not need a rewrite. The work below
addresses a performance-layout defect, an endpoint-ownership race in cached
introspection, API consistency, missing boundary coverage, and the absence of a
reproducible benchmark/CI matrix.

## Audit Corrections And Locked Decisions

1. The `A<>`/`FA<>` issue is a performance-layout defect, not an SPSC
   correctness failure. Exact offsets are compiler/ABI dependent; tests must
   verify cache-line separation properties on real objects instead of freezing
   one compiler's numeric offsets.
2. Shadow eligibility remains a property of the synchronization backend, not of
   `CacheAligned`. When the global and counter-width gates allow it, every
   atomic-backed policy uses lazy opposite-index shadows. Non-atomic `P`, `V`,
   and `VV` families do not allocate or use shadows; their direct counter reads
   are already the intended fast path. No per-policy shadow opt-in is added.
3. Layout safety is the responsibility of `SPSCbase`. Replace the split
   `rb_shadow_indices` base plus separately declared `_head`/`_tail` members
   with one specialized index-storage object. Its shadow-enabled form places
   producer-written metadata (`_head`, `prod_shadow_tail`) and consumer-written
   metadata (`_tail`, `cons_shadow_head`) in separate cache-line-aligned owner
   blocks whose sizes are multiples of `SPSC_CACHELINE_BYTES`. Same-owner fields
   may share cache lines; writable metadata owned by opposite endpoints may not.
   `CacheAligned<Base, CAlign, ...>` and its `CAlign` do not decide shadow
   eligibility.
4. The intended built-in result is:

   ```text
   P / V / VV / CP / CV / CVV      shadows off
   A<> / FA<> / AA<>                shadows on when global/width gates allow
   CA<> / CFA<> / CAA<>             shadows on when global/width gates allow
   custom atomic-backed policy      shadows on when global/width gates allow
   ```

   The global shadow switch, counter-width gate, and 32-bit opt-in remain in
   force.
5. `size()` and `free()` already avoid shadow mutation. The role-safe split is
   required for `empty()`, `full()`, `can_write()`, `can_read()`,
   `write_size()`, and `read_size()`.
6. Concurrent public observation is portable and data-race-free only for
   atomic-backed policies. `P`, `V`, and `VV` do not become cross-thread-safe
   merely because the observer methods stop writing shadows.
7. Atomic observer results are bounded, approximate, and non-linearizable. They
   do not reserve a slot and may be stale immediately after return.
8. Raw bulk-region APIs always require an explicit `spsc::unsafe` tag. The
   intentional single-slot `claim()`/`try_claim()` APIs are a separate contract
   and are outside that cleanup.
9. `CFA<>` is correct under the one-producer/one-consumer contract. `CA<>` is a
   stricter RMW backend, not a more correct SPSC queue.
10. No `fast_*` alias changes or Rigtorp-equivalence claims are accepted without
    measurements. `queue` is the closest lifetime-model comparison to Rigtorp;
    `fifo` is measured separately.
11. `CHANGELOG.md` calls `v1.0.0` a stable API. Direct removal of the accidental
    view overloads is nevertheless authorized because there are no consumers,
    but it remains a deliberate source-breaking change and must be recorded.
12. A performance baseline is mandatory before any layout, memory-order, or
    hot-path behavior change. It does not block the API-only unsafe-overload
    cleanup.

## Slice Order

| Slice | Deliverable | Depends on | Status |
| --- | --- | --- | --- |
| H1 | Unsafe bulk API consistency | none | complete (2026-08-02) |
| H0 | Reproducible baseline and benchmark harness | none | complete (2026-08-02) |
| H2 | Atomic shadow storage and owner-line layout | H0 | complete (2026-08-02) |
| H3 | Role-safe public introspection | H0, H2 | complete (2026-08-02) |
| H4 | Container, policy, and concurrency documentation | H1-H3 | complete (2026-08-02) |
| H5 | Counter-wrap and invariant tests | H2, H3 | complete (2026-08-02) |
| H6 | Policy, 32-bit, and C++20 matrix | H3, H5 | complete (2026-08-02) |
| H7 | Clean builds, CI, and sanitizers | H5, H6 | complete (2026-08-02) |
| H8 | Fused monolithic single-item operations | H0, H3, H5-H7 | complete (2026-08-02) |
| H0R | Benchmark evidence validity repair | H0, H8 | implemented locally; CI pending |
| H9 | Shadow-aware bulk snapshot path | H8, H0R | pending |
| H10 | Alias and release decision | H8, H9 | pending |

`H1` precedes `H0` only because deleting forwarding overloads does not change
runtime code generation. The critical performance sequence remains
`H0 -> H2 -> H3 -> H8 -> H0R -> H9 -> H10`.

## H1 - Unsafe Bulk API Consistency

Status: complete

### Goal

Make the raw two-region API visibly unsafe and identical across owning,
non-owning, and wrapper containers.

### Scope

- Remove untagged `claim_write(max_count)` and `claim_read(max_count)` from
  `fifo_view` and `pool_view` without a deprecation period.
- Keep only `claim_write(spsc::unsafe, ...)` and
  `claim_read(spsc::unsafe, ...)`.
- Keep wrapper `using Base::claim_write/claim_read` declarations; they will
  expose only tagged overloads after the base cleanup.
- Convert internal tests to the tagged spelling.
- Do not change the single-slot `claim()`/`try_claim()` contract.
- Record the breaking cleanup in `CHANGELOG.md`.

### Files

- `src/spsc/fifo_view.hpp`
- `src/spsc/pool_view.hpp`
- `src/tests/fifo_view_test.cpp`
- `src/tests/pool_view_test.cpp`
- `src/tests/chunk_test.cpp`
- `src/tests/test_policy_matrix.hpp`
- `src/spsc/docs/fifo_view.md`
- `src/spsc/docs/pool_view.md`
- `CHANGELOG.md`

### Acceptance Criteria

- Tagged zero-count, default-count, normal, and wrapped-region behavior remains
  unchanged.
- SFINAE checks prove that zero-argument and count-only forms are absent for
  `fifo_view`, `pool_view`, `array_fifo_view`, `carray_fifo_view`, and
  `chunk_fifo_view`.
- All tracked examples use the explicit tag.
- The complete Debug/Release shadow matrix passes.

### Risk And Rollback

Low runtime risk; intentional source compatibility break. Rollback is restoring
the four forwarding overloads and their API tests.

### Verification Record

- 27/27 Release and 27/27 Debug suite-runs passed across `shadow_off`,
  `shadow_on`, and `shadow_heur`.
- Strict tagged-only header smoke passed with GCC 13.1 and GCC 15.2 in C++17
  and C++20 using `-Wall -Wextra -Werror -pedantic-errors`.

## H0 - Reproducible Baseline And Benchmark Harness

### Goal

Capture the last pre-layout/pre-hot-path baseline so every optimization has a
measured before/after result.

### Scope

- Add a standalone `benchmarks/` harness and pinned Rigtorp dependency/version.
- Record commit SHA, compiler/version, exact flags, CPU, topology, affinity,
  cache-line configuration, payload size, capacity, batch size, and run count.
- Measure `queue<T, ..., CFA<>>` against Rigtorp under equivalent object-lifetime
  work.
- Measure `fifo<T, ..., CFA<>>` separately.
- Record `sizeof`/`alignof` baselines for `A<>`, `FA<>`, `AA<>`, `CA<>`,
  `CFA<>`, and `CAA<>`, plus like-for-like throughput samples for at least
  `A<>`, `FA<>`, `CA<>`, and `CFA<>`. H2 changes their physical storage while
  intentionally preserving shadow eligibility.
- Record steady-state throughput plus boundary-heavy empty/full behavior.
- Capture generated assembly for producer push/emplace and complete consumer
  `front + pop` paths. Bare `fifo::try_pop()` is not an end-to-end consumer
  comparison.
- Record policy object sizes and alignments for the supported compilers without
  declaring them portable ABI. H2's friend-only layout probe, rather than a
  public API, will capture the private owner-line geometry and offsets after the
  storage change.

### Acceptance Criteria

- A clean checkout can reproduce the benchmark executable and result format.
- CPU affinity and warm-up are explicit; raw samples and summary statistics are
  retained.
- The current 54-suite Debug/Release matrix is green at the recorded baseline.
- No performance claim is added to public documentation in this slice.

### Risk And Rollback

Low. This slice adds measurement infrastructure and does not change the library.

### Verification Record

- Added `benchmarks/spsc_bench.cpp`, the canonical Windows runner at
  `scripts/run_spsc_baseline.ps1`, JSONL/manifest/assembly capture, and the
  pinned `rigtorp/SPSCQueue` v1.1 gitlink
  `565a5149d54930463d58cb0f69b978d439555e66`.
- The initial short capture (2,000,000 transfers, five samples, CPUs `0,1`)
  is retained only as diagnostic history. It used sibling logical CPUs and a
  retry loop which periodically called `std::this_thread::yield()`, so its
  throughput table is not a valid H0 performance reference.
- That retained manifest is now explicitly classified as
  `diagnostic_invalid`. Manifest format 3 requires every new capture to declare
  a diagnostic or release evidence class; release mode rejects dirty library
  or harness revisions, short sample protocols, disabled affinity, and
  producer/consumer placement on the same logical CPU. Clean named H0, H2, and
  H8 artifacts still need to be generated after this closeout is committed.
- The harness was hardened during H2 validation: defaults are now 20,000,000
  transfers, nine samples, and two warm-ups; Windows `auto` affinity chooses
  two distinct physical cores and prefers two non-zero P-cores on a hybrid
  host, retaining CPU 0 only as a fallback for smaller topologies;
  queue/Rigtorp samples are paired in alternating order; raw results record
  median, standard deviation, endpoint CPU time, and
  the resolved affinity; the manifest records the Windows base plan and
  performance overlay; retries use CPU-relax rather than scheduler yield;
  and `-LibraryRoot` supports an H0/H2 build comparison using one harness.
- The H2 -> H0 -> H2 control sequence still showed host-state movement larger
  than a layout-only change. The active Windows Balanced power plan and
  scheduler/thermal state are therefore recorded as an unresolved external
  variable, not misreported as a library gain or regression. A release-quality
  cross-version result needs a controlled performance power plan/idle host and
  paired interleaving of the two revision executables.
- GCC 15.2 strict benchmark and hot-path checks passed in C++17 and C++20 with
  shadows both off and on, using `-Wall -Wextra -Werror -pedantic-errors`.
- Existing SPSC regression executables passed: 27/27 Debug and 27/27 Release
  suite-runs across `shadow_off`, `shadow_on`, and `shadow_heur`.

## H2 - Atomic Shadow Storage And Owner-Line Layout

### Goal

Keep lazy opposite-index shadows for every eligible atomic backend while making
their physical layout safe by construction. Keep the non-atomic path compact
and shadow-free.

### Scope

- Preserve the current `rb_use_shadow_v` eligibility rule: global enable,
  atomic counter backend, and counter-width/32-bit opt-in gates only.
- Replace the split shadow base and naked `_head`/`_tail` members with one
  direct, specialized index-storage member.
- Keep the shadow-disabled specialization compact: it contains only `_head`
  and `_tail`, without shadow fields or shadow-induced cache-line padding.
- In the shadow-enabled specialization, group producer-owned `_head` plus
  `prod_shadow_tail` in one owner block and consumer-owned `_tail` plus
  `cons_shadow_head` in another owner block.
- Give each owner block an effective alignment of at least
  `max(SPSC_CACHELINE_BYTES, alignof(counter_type), alignof(reg))` and make its
  size a multiple of `SPSC_CACHELINE_BYTES`. Counter types smaller than, equal
  to, or larger than the configured cache line must all remain valid.
- Add compile-time checks for owner-block alignment, size, and standard-layout
  properties. Do not rely on derived-class base/member placement or on one
  compiler's incidental tail-padding behavior.
- Ensure the built-in and custom-policy results listed in the locked decisions.
- Update `src/tests/test_build_config.hpp` so every qmake variant explicitly
  checks that `A<>` and `CA<>` have identical shadow eligibility.
- Add a friend-based test layout probe. Do not add a public state/layout API and
  do not use an ODR-changing `SPSC_TESTING` class definition.
- Test actual address ranges/cache-line ownership, not hard-coded GCC offsets.
- Treat `SPSC_CACHELINE_BYTES` as the compile-time layout contract. The guarantee
  assumes normally aligned C++ objects; packed or otherwise misaligned placement
  is outside the supported object-lifetime contract.
- Include an atomic `CacheAligned` policy whose `CAlign` is deliberately smaller
  than the configured cache line. It must still use shadows safely because the
  outer owner blocks, not `CAlign`, provide endpoint isolation.
- Include an over-aligned custom atomic counter to prove that owner-block
  alignment never weakens the counter type's natural alignment.

### Acceptance Criteria

- `A<>`, `FA<>`, `AA<>`, `CA<>`, `CFA<>`, `CAA<>`, and a custom atomic-backed
  policy use shadows whenever the global and width gates allow them.
- `P`, `V`, `VV`, `CP`, `CV`, and `CVV` contain no shadow storage. Disabling
  shadows also selects the compact storage for atomic policies.
- No configured cache line contains writable metadata owned by both producer
  and consumer. Same-owner `_head`/shadow-tail or `_tail`/shadow-head sharing is
  allowed and should be preferred when the counter type fits.
- Owner blocks remain isolated for counter types whose natural alignment is
  below, equal to, or above `SPSC_CACHELINE_BYTES`.
- `SPSC_ENABLE_SHADOW_INDICES=0` disables shadows everywhere.
- Both static and dynamic geometry are covered in all three current qmake
  variants.
- Expected object-size/ABI changes are documented.

### Risk And Rollback

Medium. This intentionally changes atomic object layout, size, and performance,
but does not change which policies are shadow-eligible. Roll back the unified
index-storage representation as one unit if a supported compiler violates the
tested owner-line properties.

### Verification Record

- Replaced the old split shadow base plus naked index members with direct
  `rb_index_storage<Cnt, kUseShadow>` storage. The no-shadow specialization is
  exactly two counters; the shadow specialization stores producer-owned
  `head`/`prod_shadow_tail` and consumer-owned `tail`/`cons_shadow_head` in
  separate cache-line-sized owner blocks.
- Eligibility is unchanged: all atomic built-ins (`A`, `FA`, `AA`, `CA`,
  `CFA`, `CAA`) and custom atomic policies retain shadows when the global and
  width gates allow them; `P`, `V`, `VV`, `CP`, `CV`, and `CVV` never allocate
  shadow storage. `CAlign` does not participate in that decision.
- Added a friend-only layout probe in `src/tests/test_spsc_layout.hpp`; no
  public base-class state or test-only production macro was added. The FIFO
  layout test covers static and dynamic capacity for the six non-atomic and six
  built-in atomic policies, an atomic `CacheAligned` policy with sub-cacheline
  `CAlign`, and an over-aligned custom atomic counter. It checks real field and
  owner-block addresses, configured cache-line separation, block size, and
  counter alignment. `test_build_config.hpp` also keeps the early qmake
  `A<>`/`CA<>` gate invariant explicit without changing test assertion setup.
- Final validation: all six qmake runners rebuilt; 54/54 Debug/Release suite
  runs passed across `shadow_off`, `shadow_on`, and `shadow_heur`. The focused
  owner-line test passed in all six configurations. GCC 15.2 strict syntax
  checks passed 12/12: C++17/C++20 × shadows off/on × benchmark, hot-path, and
  layout-probe translation units with `-Wall -Wextra -Werror -pedantic-errors`.
- Object layout on the benchmark host changed as intended. `queue` with
  `A`/`FA`/`AA` grew from 192 B to 256 B; `queue` with `CA`/`CFA`/`CAA` stayed
  384 B. The measured `fifo` objects stayed 8320 B (`A`/`FA`/`AA`) and 8448 B
  (`CA`/`CFA`/`CAA`), all aligned to 64 B. These are recorded compiler-specific
  observations, not a portable ABI promise.
- A controlled H0 -> H2 -> H0 -> H2 check used the hardened harness on the
  i9-14900HX/GCC 15.2 host: 30,000,000 transfers, 11 measured pairs, three
  warm-ups, capacity 1024, topology-selected CPUs `0,2`, and Windows Max
  Performance Overlay `ded574b5-45a0-4f42-8737-46345c09c238` over the Balanced
  base scheme. The stable adjacent H0/H2 series was:

  | Workload | H0 `spsc::queue<CFA>` median | H2 median | H2 delta | Rigtorp/SPSC paired ratio H0 / H2 |
  | --- | ---: | ---: | ---: | ---: |
  | steady | 258.8 M/s | 261.3 M/s | +1.0% | 0.916 / 0.962 |
  | forced empty/full boundary | 263.3 M/s | 256.9 M/s | -2.4% | 0.636 / 0.660 |

  Both deltas are within the 4-10% steady and 2-5% boundary sample variation;
  H2 therefore has no measured throughput regression or gain on this host. An
  intervening run in which both SPSC and unchanged Rigtorp slowed by roughly
  6x was excluded as host interference, then immediately followed by the
  stable H0/H2 pair above. The earlier short/yield-based captures remain
  diagnostic history only, not release evidence or a general library ranking.
- The generated GCC 15.2 hot-path probe confirms that H2 changes index-member
  displacements and owner-line geometry, not the producer/consumer control
  flow: the producer still reloads its owner head and the consumer still
  reloads its owner tail. Eliminating those redundant relaxed owner loads is
  explicitly H8's job; it must carry one owner snapshot through availability,
  address calculation, and release publication without weakening opposite-side
  acquire semantics.

## H3 - Role-Safe Public Introspection

### Goal

Prevent a producer, consumer, or monitoring thread from racing on mutable
shadows through public `const` queries while retaining cached endpoint hot paths.

### Scope

- Keep/refactor direct atomic snapshot implementations for `size()` and
  `free()`.
- Add internal producer-only cached helpers for `full`, `can_write`, and
  `write_size`.
- Add internal consumer-only cached helpers for `empty`, `can_read`, and
  `read_size`.
- Make public query methods read head/tail directly and never mutate shadows.
- Migrate every container's producer and consumer operations to the appropriate
  cached helper; public wrappers remain observer methods.
- Audit `fifo`, `fifo_view`, `pool`, `pool_view`, `queue`, `typed_pool`, all
  `latest` variants, and inherited array/chunk wrappers.
- Rewrite existing shadow-regression helpers that currently warm shadows by
  calling public query methods; after this slice only endpoint hot paths may
  warm the caches.
- Document that atomic observations are race-free but approximate and
  non-linearizable; `P`/`V`/`VV` remain outside portable concurrent observation.

### Acceptance Criteria

- A public query cannot write either shadow.
- Producer paths touch only the producer-owned shadow; consumer paths touch only
  the consumer-owned shadow.
- Atomic observer stress always returns bounded values (`size/free <= capacity`)
  and never participates in queue mutation.
- Lazy opposite-index caching is still present in generated hot-path assembly.
- Full tests pass with shadows off, on, and heuristic refresh.
- The later Linux TSan job reports no shadow race in the three-thread
  producer/consumer/observer test.

### Risk And Rollback

High because this changes a shared base and every hot path. Keep the direct and
cached helpers separate so the migration can be reviewed mechanically.

### Verification Record

- Split the base-layer occupancy code into direct observation methods and
  endpoint-local cached helpers. `size`, `free`, `empty`, `full`,
  `can_write`, `can_read`, `write_size`, and `read_size` now read only the
  published indices; they never read or write either shadow.
- Migrated producer/consumer operation paths in `fifo`, `fifo_view`, `pool`,
  `pool_view`, `queue`, `typed_pool`, and all `latest` variants to the
  corresponding producer- or consumer-owned cache helper. Array/chunk FIFO
  wrappers inherit that behavior. A source audit confirms the remaining direct
  base-query calls in those containers are only their public observer wrappers.
- Added a friend-only H3 test subject which establishes nonzero producer and
  consumer shadows, calls every direct observer method, and proves both shadow
  values are unchanged. Existing regression helpers now warm caches only via
  endpoint operations. The new three-thread FIFO regression runs producer,
  consumer, and observer concurrently and verifies bounded
  `size`/`free`/contiguous-region observations.
- Documented the observer contract in `concurrency-and-freertos.md`: atomic
  policies allow bounded, data-race-free but approximate/non-linearizable
  observations; plain and volatile policies still require external
  synchronization for a third observer.
- Final functional validation: 27/27 Debug and 27/27 Release suite-runs
  passed across `shadow_off`, `shadow_on`, and `shadow_heur`. GCC 15.2 strict
  hot-path compilation passed 4/4 for C++17/C++20 and shadows off/on with
  `-Wall -Wextra -Werror -pedantic-errors`; the sole suppressed diagnostic is
  GCC's external Rigtorp `hardware_destructive_interference_size` warning.
- Generated GCC 15.2 C++17/O3 assembly with shadows enabled retains lazy
  endpoint caches: FIFO producer/consumer use shadows at offsets 64/192 and
  queue producer/consumer at 128/256 only on refresh paths. The H3 observer
  probes access only their direct head/tail offsets (FIFO 0/128; queue 64/192)
  and never either shadow. The Linux TSan execution of this regression remains
  the explicitly deferred H7 infrastructure check.

## H4 - Container, Policy, And Concurrency Documentation

Status: complete

### Goal

Make the documented contract match the implementation exactly.

### Scope

- Add a concise producer-only, consumer-only, observer, and non-concurrent
  method ownership table.
- State that atomic observation is approximate/non-linearizable and creates
  cache traffic; plain/volatile policies are not portable observer policies.
- Explain `CFA<>` as the single-writer SPSC backend and `CA<>` as the strict RMW
  backend without calling either one more correct.
- Correct `fast_*` wording but defer aliases themselves to `H10`.
- State explicitly:
  - static capacity fixes geometry but does not always imply no heap;
  - `fifo` uses assignment into live slots;
  - `queue` manages object lifetime and may allocate in its static-capacity form;
  - `latest` provides newest-state semantics but still has bounded capacity and
    can become full;
  - `typed_pool` is an independent owning typed-slot container;
  - DMA cache clean/invalidate remains the platform's responsibility;
  - cache-aligned policies align metadata types and provide allocator hints,
    but external views cannot realign caller-owned backing storage;
  - `claim -> fill/construct -> publish` and `front -> process -> pop` forbid
    same-side interleaving.
- Mark the older paranoid audit as historical/superseded where it conflicts with
  the current findings.

### Acceptance Criteria

- Public docs contain no unmeasured Rigtorp-performance equivalence.
- Every public concurrency claim is valid for the named policy family.
- Container allocation, lifetime, newest-state, and DMA contracts match code and
  tests.

### Risk And Rollback

Low code risk. Documentation must land with the behavior it describes.

### Verification Record

- Added the role-ownership and same-side transaction contract to the common
  documentation: producer `claim -> fill/construct -> publish`, consumer
  `front -> process -> pop`, the atomic-only observer surface, and the stopped
  management boundary.
- Documented `CA<>` as the cache-aligned RMW backend and `CFA<>` as the
  cache-aligned single-writer backend, without ranking either as more correct.
  Public observer reads are now explicitly described as approximate and as
  possible cross-core cache traffic.
- Reconciled container guides with their actual allocation/lifetime behavior:
  static capacity is not universally allocation-free; `fifo` uses live
  assignment slots; `queue` manages lifetime and allocates even at static
  capacity; `pool` and `typed_pool` allocate their slots; and `latest` remains
  bounded even when coalescing.
- Documented the external-storage boundary: `CacheAligned` affects queue
  metadata and owning allocator hints, but views cannot realign caller memory;
  DMA cache maintenance remains platform code. Marked the older paranoid audit
  as historical where it conflicts with H1-H3 findings.
- `git diff --check` passed, and a local-link scan over every changed Markdown
  document resolved all relative links. Header changes are comments only; no
  runtime code or generated hot path changed in this slice.

## H5 - Counter-Wrap And Invariant Tests

Status: complete

### Goal

Exercise monotonic counter overflow rather than only physical ring wrapping.

### Scope

- Use the existing view restore/adopt state and a test-only derived base probe
  where possible to initialize
  `head/tail` near `SIZE_MAX`.
- Reserve a narrow friend layout probe for private-address inspection only;
  avoid a production `SPSC_TESTING` switch.
- Cross counter wrap through zero while testing empty, full, push/pop,
  `can_read/can_write`, physical mask wrap, and shadow refresh.
- Cover static/dynamic capacity, shadow on/off, and corrupted-state fail-closed
  behavior.

### Acceptance Criteria

- Producer and consumer both cross the unsigned counter boundary without loss,
  overwrite, duplicate delivery, or an out-of-range query.
- Capacity remains at most half the counter domain.
- Tests complete quickly and do not simulate billions of operations.

### Risk And Rollback

Medium. Test access must not expand the production API or alter production
object layout.

### Verification Record

- Added a test-only derived `SPSCbase` subject; it exposes protected state only
  to tests, without a production test macro, raw-index API, or layout change.
- Started static and dynamic-capacity `P` and `CFA<>` subjects at
  `SIZE_MAX - 3`, then crossed both `head` and `tail` through `max -> 0`.
  The checks cover logical occupancy, full/empty, `can_read/can_write`,
  physical indices and contiguous regions, plus producer/consumer shadow-cache
  refresh whenever shadows are enabled.
- Added real static and dynamic `fifo_view::adopt()` coverage across the same
  boundary. It verifies exact FIFO delivery through physical ring reuse and
  rejects a restored state whose logical distance is `capacity + 1`.
- Atomic direct observation queries fail closed on a stable impossible raw
  state; normal `init`/`adopt` rejects that state and returns to a valid empty
  state. Cached endpoint observations are deliberately not asserted after an
  artificially injected state because their lazy snapshots may legitimately be
  stale.
- Built Debug and Release test runners for `shadow_off`, `shadow_on`, and
  `shadow_heur`; the complete `fifo` and `fifo_view` suites passed in all six
  configurations. `git diff --check` also passed.

## H6 - Policy, 32-bit, And C++20 Matrix

Status: complete

### Goal

Cover the configurations currently inferred rather than executed.

### Scope

- Add direct `VV` coverage.
- Add valid acquire/release and `seq_cst` custom-order policies.
- Add an expected compile-fail target proving relaxed publication is rejected.
- Run a genuine 32-bit target with `SPSC_SHADOW_ALLOW_32BIT=0` and `1`; changing
  only a typedef is not sufficient.
- Add C++20 runtime tests according to each API's actual contract:
  `fifo::span()` covers capacity storage, `pool::span()` covers the current
  front buffer or is empty, `queue::raw_bytes()` exposes allocated raw storage,
  and chunk uses `used_span()`/`cap_span()`. Include applicable const/non-const,
  empty/full, wrapped-region, and alignment cases.

### Acceptance Criteria

- Valid policies compile and run; the invalid relaxed-publication target fails
  for the intended static assertion.
- `sizeof(reg) == 4` is checked in the 32-bit jobs.
- C++20 tests run with `SPSC_HAS_SPAN=1`, not only as header compile smoke.

### Risk And Rollback

Medium. Keep expected-failure targets isolated from the normal successful build.

### Verification Record

- Added direct `VV` execution to the extended non-threaded policy pack. Its
  smoke test passed for `buffer_pool`, `fifo`, `fifo_view`, `pool`,
  `pool_view`, `latest`, `queue`, and `typed_pool` in the Debug
  `shadow_on` runner.
- Added named acquire/release and `seq_cst` custom-order palettes. Static and
  dynamic `fifo` coverage passed for all eight resulting atomic policies, with
  threaded coverage for the `FA` and `CFA` variants, in each Debug
  `shadow_off`, `shadow_on`, and `shadow_heur` runner. The three C++17
  runners were rebuilt from the changed sources first.
- `scripts/test_relaxed_publication_compile_fail.ps1` passed with the local
  UCRT64 GCC: it succeeds only when the intended
  `AtomicCounter: SPSC payload publication requires acquire/seq_cst loads`
  static assertion rejects the relaxed-order policy.
- `scripts/run_h6_32bit_shadow_matrix.ps1` passed from the Visual Studio x86
  developer environment with `cl` for both
  `SPSC_SHADOW_ALLOW_32BIT=0` and `=1`. Its source asserts `sizeof(reg) == 4`
  and verifies that the shadow gate follows the selected value before running
  FIFO wrap/order checks.
- Added a separate C++20 qmake runner that sets `SPSC_HAS_SPAN=1`. Its Debug
  and Release builds passed the `fifo::span`, `pool::span`,
  `queue::raw_bytes`, and `chunk::{used_span,cap_span}` contract cases,
  including the applicable const/non-const, empty/full, wrapped-storage, and
  alignment checks. The ordinary C++17 runners continue to set
  `SPSC_HAS_SPAN=0` by default.
- `git diff --check` passed for the completed H6 change set.

## H7 - Clean Builds, CI, And Sanitizers

Status: complete

### Goal

Make clean, cross-toolchain verification routine and prevent stale generated
artifacts from affecting results.

### Scope

- Add safe out-of-source build/matrix scripts under `scripts/`.
- Scripts own narrowly named build directories and validate paths before any
  cleanup; they do not recursively delete an ambiguous workspace path.
- Add Linux GCC and Clang jobs, Windows MinGW/MSVC header smoke where available,
  ASan/UBSan, and Linux TSan.
- Add genuine 32-bit execution and ARM cross-compile smoke; use runtime/QEMU
  separately if available rather than describing cross-compile as runtime proof.
- Run Debug/Release, shadow variants, C++17, and C++20 in explicit jobs.

### Acceptance Criteria

- A clean clone builds without pre-existing `.moc`, object, or qmake state.
- The complete functional matrix is green.
- TSan is clean for atomic producer/consumer/observer tests.
- CI logs identify compiler, flags, policy/shadow configuration, and test suite.

### Risk And Rollback

Medium infrastructure risk. Keep toolchain-specific exclusions documented and
do not silently treat a skipped sanitizer or runtime job as a pass.

### Verification

- A fresh temporary qmake build passed for C++17 Debug `shadow_on` and Release
  `shadow_heur`, each running the `fifo` suite. A separate fresh C++20 span
  Debug runner passed `fifo`, and the clean dashboard launcher built from the
  same isolated tree. This exposed and fixed a real qmake MOC dependency bug:
  absolute generated paths could cause a fresh source-MOC dependency to be
  skipped.
- Strict standalone public-header smoke passed locally with GCC in C++17 and
  C++20 (`SPSC_HAS_SPAN=1`) and with MSVC in C++17/C++20 under `/W4 /WX`.
  The MSVC check also exposed and fixed a width-discarded shift warning in the
  capacity helper; the shift is now template-dependent and only instantiated
  when valid for `reg`.
- The standalone producer/consumer/observer stress passed locally, and the
  existing genuine MSVC x86 shadow-gate execution passed again for
  `SPSC_SHADOW_ALLOW_32BIT=0` and `=1`.
- GitHub Actions run `30766194472` passed the clean GCC/Clang qmake matrix,
  C++20 span targets, ASan/UBSan, TSan, genuine 32-bit shadow gates, AArch64
  public-header smoke, Windows MinGW/MSVC header smoke, and the clean
  out-of-source dashboard build. The run also caught and validated fixes for
  Qt 6.4 source-MOC discovery in the large `fifo_view` test unit, dynamic FIFO
  storage alignment on Clang, and padding-sensitive `pool` stress comparison.
- Local STM32 GNU Tools 14.3.1 strict C++17 public-header/API smoke passes for
  Cortex-M4 and Cortex-M7 with `-mthumb -fno-exceptions -fno-rtti` and a forced
  32-byte cache line. Cortex-M0 is rejected by the default lock-free atomic gate
  and passes when `SPSC_REQUIRE_LOCK_FREE=0`; that is a pre-existing toolchain/
  policy limitation, not an H8 regression. CI still automates AArch64 rather
  than `arm-none-eabi`, so Cortex-M automation remains an explicit follow-up.

## H8 - Fused Monolithic Single-Item Operations

Status: complete

### Goal

Remove redundant owner-index loads without weakening publication semantics.

### Scope

- Carry one owner snapshot through availability, index calculation, payload
  access, and commit in one public operation.
- Split owner-side relaxed loads from opposite-side acquire loads where the
  counter interface requires it.
- Preserve release publication and SPSC single-writer rules.
- Optimize producer and complete consumer paths across the core containers.
- Keep split-phase calls such as `claim -> publish` and `front -> pop` as
  separate transactions; carrying a snapshot between them would require a new
  persistent reservation/token contract.

### Acceptance Criteria

- Compiler-specific assembly checks confirm the intended owner-load count for
  the canonical FIFO and lifetime-managed queue probes on GCC, Clang, and MSVC
  where supported.
- No correctness, sanitizer, or wrap test regresses.
- The canonical 8-byte trivial FIFO and lifetime-managed queue workloads show
  no boundary-heavy regression. A literal multi-size sweep is deferred to
  H9/H10 and is required before broader payload-size claims.

### Risk And Rollback

High. Review memory-order changes independently from mechanical call-site
fusion, even if they land in the same release phase.

### Implementation And Verification Record

- `SPSCbase` now has immediate-use checked snapshots for `try_*` endpoints and
  owner-only snapshots for contract-precondition endpoints.  The checked form
  carries one owner value through availability, slot index, payload access, and
  publication/retirement; the owner-only form deliberately avoids a redundant
  opposite-endpoint validation in Release where the public contract already
  requires a readable/writable slot.
- Built-in counters expose an optional `load_relaxed()` for the endpoint-owned
  index.  The opposite endpoint continues to use its configured acquire or
  `seq_cst` load. `FastAtomicCounter`, plain, and volatile counters publish
  from the captured owner value; strict `AtomicCounter` remains a RMW backend
  and deliberately continues to use `inc()`/`add()` for commit.
- `fifo`, `fifo_view`, `queue`, `pool`, and `typed_pool` use the owner-only
  path for precondition operations and the checked path for `try_*` operations.
  `latest` retains its fresh-producer-head path because it selects the newest
  element rather than FIFO's oldest. No persistent index metadata was added;
  H2 owner-line and shadow layout therefore remains unchanged.
- The public consumer probe is intentionally `try_front()` followed by
  `pop()`. It now has two consumer-tail read/commit accesses - one for each
  public operation. Clang may fold the final access into an in-place increment;
  collapsing the two public-operation accesses further would require a new
  reservation/token contract and is intentionally out of scope.
- Added compiler-specific H8 assembly gates. The closeout gate covers both
  `fifo` and lifetime-managed `queue`: local GCC 15.2 (UCRT64) and MSVC 19.50
  report one producer owner load and two consumer read/commit accesses for each
  probe. GitHub Actions runs the same script for GCC, Clang, and MSVC.
- `queue` and `typed_pool` now inherit their allocation-state implementation
  bases privately. Cross-toolchain public-header smoke uses access-detection and
  pointer-conversion assertions to prevent allocation state, implementation
  bases, H8 snapshots, or cached endpoint helpers from leaking into object APIs.
- Closeout verification passed all nine suites in the four Debug variants
  (`shadow_off`, `shadow_on`, `shadow_heur`, and C++20 `cxx20_span`): 36 suite
  executions. Strict public-header/API smoke passed in C++17/C++20 with GCC and
  MSVC and on Cortex-M4/M7 with STM32 GNU Tools 14.3.1. The genuine MSVC x86
  shadow gate passed for both 32-bit opt-in states; local GCC/MSVC FIFO and queue
  assembly gates and the intended relaxed-publication compile-fail check passed.
- Local final verification passed all 9 suites in Debug and Release for
  `shadow_on`, `shadow_off`, and C++20 `cxx20_span` (54 executions). Strict
  C++17/C++20 public-header smoke passed with GCC and MSVC. Both 32-bit shadow
  gate states passed strict `-m32 -fsyntax-only`; the local host lacks the
  32-bit runtime needed for execution, which remains covered by CI.
- Controlled `c5079c4 -> f074fd5` boundary captures used GCC 15.2, `CFA`,
  capacity 1024, CPUs `0,2`, 100,000,000 transfers, and four alternating
  before/after pairs under high process priority. All samples verified. Median
  boundary throughput was `+5.7%` for trivial 8-byte `fifo` and `-0.2%` for
  non-trivial-lifetime 8-byte `queue`, i.e. no measurable boundary regression.
  Steady-state samples were strongly multi-modal on this host even with
  affinity and priority, so they are retained as diagnostic evidence only and
  are not presented as a library-wide throughput claim. The canonical H0
  harness currently has these two payload models but not a literal multi-size
  sweep; H9/H10 must extend that before any general alias or performance claim.
- The earlier practical-parity position is withdrawn as ranking evidence.
  Repeated format-version-1 runs of the same `386b691` binary, workload,
  capacity, affinity pair, and sample count reversed the winner by far more
  than the within-capture variation. `High performance` did not remove the
  reversal. Those captures remain correctness/regression diagnostics, but they
  cannot support an SPSC-versus-Rigtorp winner claim. H0R defines the repaired
  evidence protocol below.

## H0R - Benchmark Evidence Validity Repair

Status: implemented locally; CI pending

### Goal

Make unstable host behavior an explicit `inconclusive` result instead of
allowing a machine-specific capture to force a queue ranking.

### Scope

- Reuse one pinned producer/consumer worker pair across warm-up and measured
  runs for each workload and endpoint assignment.
- Pre-touch each queue before the timed region.
- Measure both endpoint assignments (`P -> C` and `C -> P`) and alternate both
  direction order and implementation order.
- Record per-endpoint CPU time, Windows thread cycles, retry rate, affinity
  success, and direction-specific paired statistics.
- Emit a final `comparison_summary` with `spsc_faster`, `rigtorp_faster`,
  `parity`, or `inconclusive`; never manufacture a winner when a gate fails.
- Record power scheme/overlay before and after the canonical runner.
- Keep legacy format-version-1 captures as diagnostic data only.

### Acceptance Criteria

- Every measured sample retains sequence, checksum, count, and lifetime
  verification.
- Release evidence uses at least nine pairs in both affinity directions on
  distinct pinned CPUs.
- A host-wide winner claim requires agreement from additional same-class
  physical-core pairs; a passing single-pair classification remains local to
  that pair.
- A direction fails ranking eligibility when paired-ratio CV exceeds 10%,
  minimum endpoint CPU occupancy is below 80%, affinity is not applied, or the
  sample count is insufficient.
- The final ranking is `inconclusive` when direction spread exceeds 15% or the
  two directions disagree; `0.95..1.05` is the explicit parity band.
- GCC and Clang CI compile the harness and validate version-2 JSON semantics.

### Risk And Rollback

Low for library behavior: H0R changes only benchmark execution and evidence
classification. Legacy JSONL readers must continue treating format version 1
as a separate diagnostic format.

### Local Implementation And Validation

- JSONL format version 2 now uses persistent pinned workers, queue pre-touch,
  alternating implementation/direction order, forward and reverse affinity,
  per-endpoint CPU-time/cycle telemetry, retry rates, per-direction paired
  gates, and a final comparison classification. The canonical manifest is
  format version 4 and records affinity and power state before/after capture.
- Strict GCC 15.2 C++17 and C++20 builds passed with
  `-Wall -Wextra -Werror -pedantic-errors`; MSVC C++17 passed `/W4 /WX`, and its
  runtime smoke recorded non-zero `QueryThreadCycleTime` values in every sample.
- Queue, policy, forward-only, bidirectional, and 100-sample persistent-worker
  smokes completed without an unverified sample. The PowerShell runner emitted
  two resolved directions, unchanged pre/post power state, two comparison
  summaries, and a valid format-version-4 manifest.
- The default-scale local validation used GCC 15.2, `queue<CFA>`, Rigtorp v1.1,
  capacity 1024, 20,000,000 transfers, nine measured pairs plus two warm-ups,
  and CPUs `0,2` in both endpoint assignments. All 72 measured samples were
  verified. Steady paired medians were `Rigtorp / SPSC = 0.713` forward and
  `1.307` reverse; both direction sample gates passed, but their `83.4%` spread
  produced `inconclusive: direction_sensitive_result` (geometric mean `0.965`).
  Boundary medians were `0.975` and `0.976`; their minimum CPU occupancy was
  below the conservative 80% gate, so boundary was also `inconclusive` rather
  than an unsupported parity claim.
- A follow-up non-zero-core diagnostic repeated the same default-scale protocol
  on P-core pairs `2,4`, `4,6`, and `6,8` under one unchanged Balanced power
  plan. All 216 measured samples were verified. Steady forward/reverse ratios
  were `0.734/1.261`, `1.077/1.131`, and `0.888/0.572`, respectively. Only
  `4,6` passed both direction gates (`Rigtorp / SPSC` geometric ratio `1.104`);
  the reverse runs on `2,4` and `6,8` failed the paired-variation gate. Avoiding
  CPU 0 therefore removes a housekeeping concern but does not create a
  core-pair-independent ranking. `auto` now prefers the first two non-zero
  P/SMT physical cores, while any host-wide claim must remain inconclusive
  unless additional same-class pairs agree.
- GCC/Clang and MSVC protocol smoke steps are present in GitHub Actions. H0R
  remains CI-pending until those checks run on the committed revision.

## H9 - Shadow-Aware Bulk Snapshot Path

### Goal

Reuse one producer/consumer snapshot for availability, index, and region
calculation while preserving the explicit unsafe raw API.

### Scope

- Add internal producer and consumer bulk snapshot helpers using the correct
  endpoint-owned shadow.
- Reuse them in `claim_write/read` and existing RAII bulk guards.
- Do not expose a new public token in the first implementation.
- If a public token is proposed later, require it to be move-only, queue-bound,
  one-shot, origin/count preserving, prefix-commit-only, and release-safe without
  relying solely on `SPSC_ASSERT`.
- Keep same-side interleaving explicitly unsafe/forbidden for raw tagged regions.

### Acceptance Criteria

- Region totals and split boundaries match the model across wrap and counter
  overflow.
- The opposite atomic index is refreshed only at the appropriate boundary.
- Batch benchmarks cover multiple requested counts and payload sizes.
- No public API silently upgrades an unsafe raw region into a falsely safe token.

### Risk And Rollback

High. Land the internal snapshot representation before any optional public RAII
surface.

## H10 - Alias And Release Decision

### Goal

Make names, release versioning, and performance claims follow measured behavior.

### Scope

- Re-run the repaired `H0R` benchmark suite after `H9`.
- Decide whether to change `fast_fifo`/`fast_queue`, add explicit
  `single_writer_fast_*` and `strict_atomic_*` aliases, or retain existing aliases
  with corrected documentation.
- Treat an alias target change as a concrete type/layout and source/ABI change.
- Plan the next SemVer release as `2.0.0` for the authorized breaking cleanup,
  unless the project explicitly and publicly resets the pre-adoption versioning
  contract before release.

### Acceptance Criteria

- Alias names and recommendations are backed by retained benchmark data.
- `CA<>` is not advertised as making an SPSC queue more correct.
- Rigtorp comparisons state workload, toolchain, hardware, and uncertainty.
- Changelog, migration note, and release version agree about breaking changes.

### Risk And Rollback

Medium. Adding explicit aliases is the compatibility-friendly fallback if the
measured winner varies by platform.

## Final Production Gate

The hardening program is complete only when:

- all slices above are complete or explicitly rejected with evidence;
- the clean Debug/Release functional matrix is green;
- C++17/C++20, GCC/Clang, sanitizers, real 32-bit, and ARM smoke results are
  recorded;
- public observer, endpoint ownership, memory-order, allocation, lifetime, and
  DMA contracts are documented;
- layout and counter-overflow regressions are executable tests;
- every performance claim and alias decision points to a reproducible benchmark.
