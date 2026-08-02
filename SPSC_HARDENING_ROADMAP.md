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
2. Shadows are allowed only when the policy explicitly opts in and its counter
   storage is structurally at least one configured cache line wide and aligned.
   A missing opt-in on a custom policy means `false`.
3. `CacheAligned<Base, CAlign, ...>` is not automatically shadow-safe:
   user-supplied `CAlign` may be smaller than `SPSC_CACHELINE_BYTES`. The gate
   must check `CAlign`, `alignof(counter_type)`, and `sizeof(counter_type)`.
4. The intended built-in result is:

   ```text
   A<> / FA<> / AA<> shadows off
   CA<> / CFA<>     shadows on
   CAA<>            shadows on
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
| H0 | Reproducible baseline and benchmark harness | none | pending |
| H2 | Shadow eligibility and layout contract | H0 | pending |
| H3 | Role-safe public introspection | H0, H2 | pending |
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
- Record steady-state throughput plus boundary-heavy empty/full behavior.
- Capture generated assembly for producer push/emplace and complete consumer
  `front + pop` paths. Bare `fifo::try_pop()` is not an end-to-end consumer
  comparison.
- Record policy object sizes and observed metadata offsets for the supported
  compilers without declaring those offsets portable ABI.

### Acceptance Criteria

- A clean checkout can reproduce the benchmark executable and result format.
- CPU affinity and warm-up are explicit; raw samples and summary statistics are
  retained.
- The current 54-suite Debug/Release matrix is green at the recorded baseline.
- No performance claim is added to public documentation in this slice.

### Risk And Rollback

Low. This slice adds measurement infrastructure and does not change the library.

## H2 - Shadow Eligibility And Layout Contract

### Goal

Enable lazy opposite-index shadows only when their metadata can be physically
isolated from the owner counters.

### Scope

- Add a detected policy property whose absence means `false`.
- A `CacheAligned` policy may opt in only when its requested counter alignment
  and resulting counter type are at least `SPSC_CACHELINE_BYTES` aligned and
  sized.
- Extend `rb_use_shadow_v` with the policy/layout safety property while keeping
  the atomic-backend, global-enable, and counter-width gates.
- Ensure the built-in results listed in the locked decisions.
- Update `src/tests/test_build_config.hpp` so each qmake variant checks the new
  eligibility truth table instead of expecting every atomic policy to cache.
- Add a friend-based test layout probe. Do not add a public state/layout API and
  do not use an ODR-changing `SPSC_TESTING` class definition.
- Test actual addresses/cache-line indices, not hard-coded GCC offsets.
- Include a custom `CacheAligned` policy whose `CAlign` is deliberately smaller
  than the configured cache line; shadows must remain off.

### Acceptance Criteria

- `A<>` and `FA<>` have no shadow storage.
- Default `CA<>`, `CFA<>`, and `CAA<>` use shadows when the global and width
  gates allow them.
- For every shadow-enabled policy, producer shadow, consumer shadow, `_head`,
  and `_tail` satisfy the declared cache-line isolation properties.
- `SPSC_ENABLE_SHADOW_INDICES=0` disables shadows everywhere.
- Both static and dynamic geometry are covered in all three current qmake
  variants.
- Expected object-size/ABI changes are documented.

### Risk And Rollback

Medium. This intentionally changes `A<>`/`FA<>` layout and performance. Revert
the eligibility predicate and trait together if a supported compiler violates
the tested layout properties.

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
