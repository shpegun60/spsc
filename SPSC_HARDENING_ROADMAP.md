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
   A<> / FA<> / AA<>                shadows on
   CA<> / CFA<> / CAA<>             shadows on
   custom atomic-backed policy      shadows on
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
| H4 | Container, policy, and concurrency documentation | H1-H3 | pending |
| H5 | Counter-wrap and invariant tests | H2, H3 | pending |
| H6 | Policy, 32-bit, and C++20 matrix | H3, H5 | pending |
| H7 | Clean builds, CI, and sanitizers | H5, H6 | pending |
| H8 | Fused single-item hot path | H0, H3, H5-H7 | pending |
| H9 | Shadow-aware bulk snapshot path | H8 | pending |
| H10 | Alias and release decision | H8, H9 | pending |

`H1` precedes `H0` only because deleting forwarding overloads does not change
runtime code generation. The critical performance sequence remains
`H0 -> H2 -> H3 -> H8 -> H9 -> H10`.

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
- The harness was hardened during H2 validation: defaults are now 20,000,000
  transfers, nine samples, and two warm-ups; Windows `auto` affinity chooses
  two distinct physical cores and prefers P-cores on a hybrid host; queue/Rigtorp samples are paired in alternating
  order; raw results record median, standard deviation, endpoint CPU time, and
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

## H5 - Counter-Wrap And Invariant Tests

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

## H6 - Policy, 32-bit, And C++20 Matrix

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

## H7 - Clean Builds, CI, And Sanitizers

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

## H8 - Fused Single-Item Hot Path

### Goal

Remove redundant owner-index loads without weakening publication semantics.

### Scope

- Carry one owner snapshot through availability, index calculation, payload
  access, and commit.
- Split owner-side relaxed loads from opposite-side acquire loads where the
  counter interface requires it.
- Preserve release publication and SPSC single-writer rules.
- Optimize producer and complete consumer paths across the core containers.

### Acceptance Criteria

- Compiler-specific assembly checks confirm the intended owner-load count for
  GCC, Clang, and MSVC where supported.
- No correctness, sanitizer, or wrap test regresses.
- Benchmarks show no regression across payload sizes and boundary-heavy cases.

### Risk And Rollback

High. Review memory-order changes independently from mechanical call-site
fusion, even if they land in the same release phase.

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

- Re-run the `H0` benchmark suite after `H8` and `H9`.
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
