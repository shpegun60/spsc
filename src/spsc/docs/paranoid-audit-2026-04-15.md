# Paranoid Audit: SPSC Base Layer And Containers

Date: 2026-04-15

Scope:
- Base layer in `src/spsc/base/*`
- Containers in `src/spsc/*.hpp`
- Container test suites in `src/tests/*_test.cpp`

Audit intent:
- Maximum-paranoia manual review of alignment rules, allocator paths, state invariants, failure paths, copy/move semantics, and test coverage shape.
- This document records what was checked and what residual risks still remain.

Bottom line:
- No concrete correctness bug or behavioral regression was found in the reviewed base layer or containers.
- The current design is internally consistent across alignment, allocator rebinding, power-of-two geometry, and fail-closed handling.
- The strongest remaining limitation is test-infrastructure related: some debug death-tests are environment-sensitive on Windows because child process launch can be blocked by `QProcess`.

## Executive Summary

- A full paranoid review was performed across `src/spsc/base/*` and all container headers in `src/spsc/*.hpp`.
- No concrete bug, alignment break, allocator mismatch, or state-invariant regression was found.
- The allocator/alignment model is coherent from policy selection down to rebinds, over-aligned allocation, slot wrapping, and runtime byte rounding for raw-buffer containers.
- Ring geometry and occupancy logic in `SPSCbase` look sound and deliberately fail closed on impossible atomic snapshots.
- Manual-lifetime containers (`queue`, `typed_pool`) intentionally prefer "assert and avoid UB" over unsafe cleanup when state is already corrupted.
- The container test matrix is green in `shadow_off`, `shadow_on`, and `shadow_heur`.
- The main remaining limitation is environmental, not logical: some Windows debug death-tests can be skipped when child process launch is blocked.

## Runtime Verification

Full suite matrix was re-run on 2026-04-15 and recorded in:

- `build/Desktop_Qt_6_10_1_MinGW_64_bit-Debug/bin/paranoid_full_matrix_2026-04-15.txt`

Observed result:

```text
shadow_off PASS buffer_pool
shadow_off PASS chunk
shadow_off PASS fifo
shadow_off PASS fifo_view
shadow_off PASS latest
shadow_off PASS pool
shadow_off PASS pool_view
shadow_off PASS queue
shadow_off PASS typed_pool
shadow_on PASS buffer_pool
shadow_on PASS chunk
shadow_on PASS fifo
shadow_on PASS fifo_view
shadow_on PASS latest
shadow_on PASS pool
shadow_on PASS pool_view
shadow_on PASS queue
shadow_on PASS typed_pool
shadow_heur PASS buffer_pool
shadow_heur PASS chunk
shadow_heur PASS fifo
shadow_heur PASS fifo_view
shadow_heur PASS latest
shadow_heur PASS pool
shadow_heur PASS pool_view
shadow_heur PASS queue
shadow_heur PASS typed_pool
```

Notes:
- `array_fifo` and `chunk_fifo` are covered through `fifo_test.cpp` and `chunk_test.cpp` rather than separate suite names.
- `buffer_pool` also has the extra paranoid tests added earlier for failure copy-constructor and copy/move aliasing paths.

## Base-Layer Audit

### `spsc_config.hpp`

Purpose:
- Central compile-time switches for shadows, exceptions, default policy, aligned-new preference.

What was checked:
- Toggle defaults are consistent with the rest of the library.
- `SPSC_ASSERT` is intentionally weakly defined and may be overridden in test TUs.
- Exception-mode gating is single-source and reused by allocators and `SPSC_TRY` helpers.

Verdict:
- Good.

Residual note:
- Because `SPSC_ASSERT` is intentionally overrideable, include order matters in tests. The current tests already handle that in the suites that need death coverage.

### `spsc_tools.hpp`

Purpose:
- Inline/noinline helpers, branch prediction hints, exception abstraction, optional `<span>` support.

What was checked:
- `RB_FORCEINLINE`/`RB_NOINLINE` fallbacks degrade safely per compiler family.
- `SPSC_TRY`/`SPSC_CATCH_ALL` collapse safely in no-exception mode.
- Span detection is feature-tested and harmless when unavailable.

Verdict:
- Good.

Residual note:
- This file is foundational for test death-path behavior because it re-exposes `SPSC_ASSERT`.

### `spsc_object.hpp`

Purpose:
- Shared `destroy_at()` helper for manual lifetime containers.

What was checked:
- Destruction is guarded by `!is_trivially_destructible_v<U>`.
- No hidden allocator or ownership assumptions.

Verdict:
- Good and intentionally minimal.

### `spsc_slot_wrap.hpp`

Purpose:
- Promote slot alignment for cache-aligned payload storage.

What was checked:
- `cache_aligned_slot<T, Align>` preserves constructors and assignment of `T`.
- `cache_aligned_slot_t` only wraps when policy storage alignment exceeds `alignof(T)`.
- `allocator_supports_slot_alignment_v` correctly checks allocator rebind minimum alignment against promoted slot type.

Verdict:
- Good.

Residual note:
- This is a key link between policy alignment and wrapper containers such as `array_fifo`, `chunk_fifo`, and the static `buffer_pool` forms.

### `spsc_regions.hpp`

Purpose:
- Shared POD region types for bulk APIs.

What was checked:
- `region_pair`, `init_region`, `uninit_region`, `region`, and `slot_region` are shape-only and do not smuggle ownership.
- `uninit_region` correctly exposes bytes and explicit `ptr_uninit()` rather than pretending lifetime exists.
- `slot_region` models spans over pointer rings, not spans over pointees.

Verdict:
- Good.

### `spsc_snapshot.hpp`

Purpose:
- Ring iterators and snapshot views.

What was checked:
- `ring_iterator` indexes by logical index and masks into storage lazily.
- `std::launder` is used when dereferencing storage that may be reused via placement new.
- `snapshot_view::size()` and `const_snapshot_view::size()` fail closed to `0` if the captured shape is impossible.

Verdict:
- Good.

Residual note:
- Snapshot correctness relies on the containers constructing begin/end from validated head/tail snapshots. The containers do this consistently.

### `spsc_cacheline.hpp`

Purpose:
- Cacheline-size detection and portable alignment macro.

What was checked:
- Explicit override wins.
- Detection order is sensible: exact platform, Qt-assisted host detection, MCU-family heuristics, generic fallback.
- `SPSC_CACHELINE_BYTES` is clamped to a minimum and enforced to be power-of-two.
- `SPSC_ALIGNED(n)` is consistently defined.

Verdict:
- Good.

Residual note:
- The file contains mojibake in comments due to encoding, but logic is unaffected.
- The chosen 32-byte floor for some cache-less MCU cases is conservative rather than exact. That is a design choice, not a bug.

### `spsc_capacity_ctrl.hpp`

Purpose:
- Power-of-two capacity control for static and dynamic ring geometry.

What was checked:
- Shared constants `RB_REG_BITS` and `RB_MAX_UNAMBIGUOUS` are correct for unsigned-difference ring math.
- `rb_is_pow2`, `rb_next_power2`, and `rb_floor_power2` are sanity-checked with static assertions.
- Static `CapacityCtrl<C>` hard-enforces compile-time power-of-two capacity.
- Dynamic `CapacityCtrl<0, Policy>` stores geometry in `Policy::geometry_type` and floors runtime capacity to a safe power-of-two.

Verdict:
- Good.

Residual note:
- Dynamic geometry uses floor-to-power-of-two rather than ceil; containers that want ceil semantics already call `rb_next_power2()` before `init()`.

### `spsc_counter.hpp`

Purpose:
- Counter backend family: plain, volatile, atomic, fast-atomic, cacheline-wrapped.

What was checked:
- Backends share a uniform API: `store/load/add/inc`.
- All counter value types are normalized to unsigned.
- Atomic order palettes are validated at compile time.
- `FastAtomicCounter` is clearly documented as single-writer only.
- `CacheSlot` and `CachelineCounter` enforce power-of-two alignment and slot-size multiple-of-alignment.
- Lock-free enforcement is optional and correctly gated by `SPSC_REQUIRE_LOCK_FREE`.

Verdict:
- Good.

Residual note:
- `FastAtomicCounter` is only safe because this is an SPSC library and each counter has a single mutator role. Using it outside that contract would be wrong, but the library’s use of it is consistent with the design.

### `spsc_policy.hpp`

Purpose:
- Compile-time policy layer selecting counter and geometry storage types.

What was checked:
- `Policy<Cnt, Geo>` validates both backends as counter-like.
- Ready-made aliases (`P`, `V`, `VV`, `A`, `FA`, `AA`) are coherent.
- `CacheAligned<Base, CAlign, GAlign>` preserves counter-like semantics while promoting alignment and exports `allocator_alignment`.
- `default_policy` is selected by a compile-time switch.

Verdict:
- Good.

Residual note:
- Container payload alignment does not automatically become cache-aligned for typed contiguous containers unless the allocator path or wrapper type participates. This is deliberate and consistent with the design.

### `spsc_alloc.hpp`

Purpose:
- Allocator layer, aligned allocation, fail-mode handling, policy-aware default allocator selection.

What was checked:
- Manual aligned allocation path stores the raw pointer header safely and checks all overflow conditions before arithmetic.
- `aligned_allocator` uses the strongest of requested alignment and `alignof(T)`.
- Native aligned new/delete is used only when the compiler and runtime actually support the whole pair safely.
- `basic_allocator` upgrades to over-aligned allocation only when needed.
- `policy_default_alloc_t`, `policy_default_value_alloc_t`, `policy_storage_alignment_v`, `rebind_allocator_min_alignment_v`, `policy_slot_round_alignment_v`, and `round_up_size_for_policy()` form a coherent model.

Verdict:
- Good.

Residual note:
- The allocator layer is one of the strongest parts of the design. I did not find a broken alignment path or a mismatched deallocation path.

### `SPSCbase.hpp`

Purpose:
- Core ring math, head/tail ownership, optional shadow indices, and geometry-safe occupancy calculations.

What was checked:
- Static vs dynamic geometry init paths are distinct and sane.
- `sync_cache()` is used only for non-concurrent restore/swap/adopt-like paths.
- `size/empty/full/free/can_write/can_read/write_size/read_size` all fail closed on impossible atomic snapshots.
- Shadow indices are enabled only for atomic backends and can be heuristically refreshed near boundaries.
- `sync_tail_to_head()` only touches the consumer-owned shadow.
- `swap_base()` preserves sanity checks and keeps geometry/index/shadow state coherent.

Verdict:
- Good.

Residual note:
- This is the single most important correctness file in the library, and it currently looks solid.
- The design intentionally prefers conservative false negatives over unsafe false positives when atomic snapshots look impossible.

## Container Audit Table

| Container | Alignment model | Allocator model | Core invariants | Review verdict |
| --- | --- | --- | --- | --- |
| `array_fifo` / `array_fifo_view` | Slot promoted via `cache_aligned_slot_t<std::array<...>, Policy>` when needed | Uses `policy_default_value_alloc_t` for owning form; view has no allocator | Value producer API deleted, zero-copy only | Good |
| `chunk_fifo` / `chunk_fifo_view` | Slot promoted via `cache_aligned_slot_t<chunk<...>, Policy>` | Owning form uses policy-aware allocator | Value producer API deleted, zero-copy only | Good |
| `chunk` static | `std::array<T, N>` alignment comes from `T` | No runtime allocator | `len_ <= capacity()` | Good |
| `chunk` dynamic | Alignment comes from `T` and allocator | `basic_allocator` rebound to `T` | `storage_ == nullptr` iff `cap_ == 0`; `size() <= cap_` | Good |
| `fifo` | Typed contiguous storage, no policy payload alignment promotion by default | Owning form allocates `T` via rebound allocator | Dynamic valid iff `storage_ != nullptr && cap != 0`; static always valid | Good |
| `fifo_view` | External typed storage alignment must already be correct | No allocator | Valid iff attached, plus non-zero cap for dynamic | Good |
| `pool` | Raw buffers round through policy-aware byte alignment | Slot array + per-buffer byte allocation | Valid iff slot table present and `bufferSize_ != 0` | Good |
| `pool_view` | External slot pointers must already point to valid aligned buffers for user types | No allocator | Valid iff slot table present and `bufferSize_ != 0`, plus cap for dynamic | Good |
| `latest<void>` | Raw slot bytes rounded through policy-aware allocator path | Slot table + per-slot byte allocation | Valid iff slots present, `bufferSize_ != 0`, cap != 0 | Good |
| `latest<T>` dynamic | Typed contiguous storage aligned by `T` / allocator | Rebound typed allocator | Valid iff `storage_ != nullptr && cap != 0` | Good |
| `latest<T, Depth>` static | Static storage aligned to max of policy storage alignment and `alignof(T)` | No runtime allocator | Always valid | Good |
| `queue` | Typed placement-new storage aligned by `T` / allocator | Rebound typed allocator | Valid iff storage attached; lifetime tracked by queue discipline | Good |
| `typed_pool` | One separately allocated `T` per slot; each slot naturally aligned for `T` | Pointer ring + per-object typed allocation | Valid iff slot table exists and slot ownership set is coherent | Good |
| `buffer_pool` static | Per-slot `cache_aligned_slot_t<std::array<T,N>, Policy>` | No runtime allocator | Always valid | Good |
| `buffer_pool` dynamic forms | Runtime-buffer forms use `allocator<std::byte>` payload and policy-aware span rounding | Dynamic slot table and/or byte-buffer allocation depending on specialization | `empty-or-valid`, no half-live state | Good |

## Per-Container Quick Verdicts

### `array_fifo`

Checked:
- Wrapper shape over `fifo<std::array<T, N>, ...>`.
- Slot promotion and cache-aligned wrapper interaction.
- Deleted value-push API to keep usage zero-copy only.

Potentially risky:
- No independent state machine of its own; correctness depends on `fifo`.

Verdict:
- Good.

### `array_fifo_view`

Checked:
- Wrapper shape over `fifo_view<std::array<T, N>, ...>`.
- External-storage attach semantics inherited from `fifo_view`.
- Deleted value-push API and preserved zero-copy discipline.

Potentially risky:
- External storage must already satisfy the typed alignment and capacity contract.

Verdict:
- Good.

### `chunk_fifo`

Checked:
- Wrapper shape over `fifo<chunk<T, N>, ...>`.
- Chunk slot promotion under cache-aligned policies.
- Deleted value-push API to prevent accidental copy-style use.

Potentially risky:
- Correctness is downstream of `fifo` plus `chunk`.

Verdict:
- Good.

### `chunk_fifo_view`

Checked:
- Wrapper shape over `fifo_view<chunk<T, N>, ...>`.
- Non-owning attachment behavior inherited from `fifo_view`.
- Zero-copy-only API surface preserved.

Potentially risky:
- External storage and attachment lifetime remain the caller's responsibility.

Verdict:
- Good.

### `chunk`

Checked:
- Static logical-size discipline over fixed storage.
- Dynamic reserve/resize/move-only behavior.
- `commit_size()` and DMA-style logical-size workflows.

Potentially risky:
- Dynamic form uses eager default construction after reserve, so its cost model is important for large `T`.

Verdict:
- Good.

### `fifo`

Checked:
- Ring invariants, occupancy math, copy/move/resize/destroy paths.
- Alignment path for typed contiguous storage.
- Dynamic failure behavior and conservative snapshot handling.

Potentially risky:
- Typed payload alignment is allocator-driven, not auto-promoted to cacheline alignment unless the slot type or allocator requests it.

Verdict:
- Good.

### `fifo_view`

Checked:
- `attach`, `adopt`, `detach`, restore, move, and swap flows.
- Dynamic validity coupling of attachment and non-zero capacity.
- Conservative read/write region behavior on impossible snapshots.

Potentially risky:
- Caller-owned external storage must remain valid and correctly aligned for the full attachment lifetime.

Verdict:
- Good.

### `pool`

Checked:
- Raw-buffer slot-table plus per-buffer allocation model.
- Typed exposure gates in `claim_as()` / `front_as()`.
- Reallocation and migration order.

Potentially risky:
- Typed access correctness depends on caller respecting size/alignment fit of `U` into the raw buffer contract.

Verdict:
- Good.

### `pool_view`

Checked:
- Non-owning raw-slot attachment model.
- Defensive rejection of `nullptr` slots.
- Coupled invariants among slot table, capacity, and buffer size.

Potentially risky:
- External slot pointers and their backing buffers are entirely caller-owned.

Verdict:
- Good.

### `latest`

Checked:
- Newest-value semantics rather than FIFO semantics.
- Snapshot bridging through `cons_head_snapshot_`.
- Both raw-byte and typed forms, including dynamic normalization and resize.

Potentially risky:
- Behavior is intentionally different from FIFO, so misuse risk is conceptual rather than structural.

Verdict:
- Good.

### `queue`

Checked:
- Placement-new / explicit-destroy lifetime discipline.
- Copy/move/resize of live object ranges.
- Fail-closed cleanup behavior on impossible corrupted snapshots.

Potentially risky:
- In impossible corrupted states, the implementation intentionally prefers assert-and-leak over UB.

Verdict:
- Good.

### `typed_pool`

Checked:
- Pointer-ring model with one typed backing allocation per slot.
- Placement-new / explicit-destroy API.
- Reallocation, copy, move, and inactive-slot preservation.

Potentially risky:
- Like `queue`, the implementation deliberately fails closed if a corrupted state prevents safe cleanup.

Verdict:
- Good.

### `buffer_pool`

Checked:
- All four specialization families.
- Logical payload bytes vs physical DMA/cache-safe span bytes.
- Runtime byte-allocation path, alignment rounding, and failure-path behavior.
- `empty-or-valid` invariants for dynamic forms.

Potentially risky:
- The design has many specialization branches, so long-term safety depends on keeping tests equally exhaustive.

Verdict:
- Good.

## Container-By-Container Notes

### `fifo`

What was checked:
- Assignment-based ring, not manual lifetime.
- Dynamic `resize()` linearizes old live range into new buffer before releasing old storage.
- Dynamic copy failure leaves the destination unchanged in copy assignment.
- `claim_read` and `claim_write` return conservative empty regions on impossible atomic snapshots.

Verdict:
- Good.

### `fifo_view`

What was checked:
- `attach`, `adopt`, `detach`, and `state_t` restore paths.
- Static view validity depends only on attachment; dynamic view also depends on non-zero capacity.
- `move_from()` preserves detached-invalid normalization.

Verdict:
- Good.

### `pool`

What was checked:
- Raw slot bytes are copied via `memcpy` only.
- `claim_as()` and `front_as()` enforce both size fit and runtime alignment before exposing typed pointers.
- `reallocate_impl()` allocates all buffers first, migrates, then destroys old storage.

Verdict:
- Good.

### `pool_view`

What was checked:
- External slot arrays are never owned.
- Publish/front reject `nullptr` slots defensively.
- Dynamic move/swap paths preserve the invariant coupling slot table, capacity, and `bufferSize_`.

Verdict:
- Good.

### `chunk`

What was checked:
- Static specialization is just logical-size management over already-constructed storage.
- Dynamic specialization is move-only and uses eager default construction after reserve.
- `commit_size()` is a controlled DMA-style logical size acceptor.

Verdict:
- Good.

### `latest`

What was checked:
- This is not FIFO: `front()` resolves newest published element, and `pop()` advances tail to the latest published head snapshot.
- Snapshot bridging through `cons_head_snapshot_` is reset correctly on `pop`, `clear`, `destroy`, and `consume_all`.
- Dynamic forms normalize to empty/disabled state cleanly.

Verdict:
- Good.

### `queue`

What was checked:
- Producer constructs with placement new; consumer destroys explicitly.
- `clear()` and `destroy()` prefer fail-closed behavior on corrupted atomic snapshots.
- Dynamic `resize()` migrates live objects before destroying old storage.

Verdict:
- Good.

Residual note:
- If internal state is already corrupted, the implementation intentionally prefers “assert and leak” over UB. That is defensive and deliberate.

### `typed_pool`

What was checked:
- One typed object backing allocation per slot.
- `claim()` returns raw storage for placement new; `front()` returns live object pointer.
- Dynamic reallocation preserves active objects first in logical order, then reattaches inactive spare slots.
- Copy builds a temporary object set and swaps only on success.

Verdict:
- Good.

Residual note:
- Like `queue`, the destroy/clear logic is intentionally fail-closed when live-object sets cannot be determined safely.

### `buffer_pool`

What was checked:
- All four specialization families.
- Static forms separate logical payload bytes from physical slot span bytes.
- Runtime-buffer forms round physical bytes through policy-aware byte allocators.
- Dynamic specializations now hold real `empty-or-valid` invariants.
- Copy/move/resize/failure paths match the documented state model.

Verdict:
- Good.

## Test Coverage Notes

The current tests are strong and broad:
- Static and dynamic forms
- Alignment contracts
- Allocator accounting
- Failure constructor, resize, copy assignment, and copy constructor paths
- Copy/move aliasing corner cases
- Policy matrix smoke coverage

Notable coverage shape:
- `fifo_test.cpp` also covers `array_fifo` and `array_fifo_view`
- `chunk_test.cpp` also covers `chunk_fifo` and `chunk_fifo_view`
- `buffer_pool_test.cpp` is currently one of the most paranoid suites in the tree

Residual test limitation:
- Several debug death-tests can `QSKIP` if Windows blocks child-process launch via `QProcess`.
- This is a test-environment limitation, not a container logic finding.

## Residual Risks

1. Debug death-test reliability on Windows

- Assert-path coverage exists, but full local execution depends on child-process launch behaving.
- This affects confidence in negative-path instrumentation, not the mainline container logic.

2. Fail-closed leak paths in manual-lifetime containers

- `queue` and `typed_pool` intentionally avoid UB by refusing unsafe cleanup when atomic snapshot state is already corrupted.
- That means “assert and leak” is a documented defensive behavior in impossible states.

3. Conservative false negatives under impossible atomic snapshots

- `SPSCbase` and dependent containers intentionally return empty/no-space/no-read-region when a snapshot looks impossible.
- This is the correct safety bias for this library, but it is worth remembering when debugging hostile races or external misuse.

## Final Review Position

After:
- manual review of `src/spsc/base/*`
- manual review of container headers in `src/spsc/*.hpp`
- cross-check against existing test design
- full runtime matrix re-run on 2026-04-15

the current conclusion is:

- No concrete bug found.
- Alignment, allocator, and geometry models are internally consistent.
- The base layer is strong enough that the container-level correctness story currently looks credible.
- The main remaining work, if desired, is not container redesign but stronger test-infrastructure for death/assert paths on Windows.
