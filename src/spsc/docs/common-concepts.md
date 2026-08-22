# Common Concepts

This page explains the ideas shared by almost all `spsc` containers.

## 1. Concurrency Contract

Every container in this library is **strictly SPSC**:

- exactly one producer
- exactly one consumer
- no multi-producer or multi-consumer access to the same instance

This applies even when the API looks "safe". The containers are hardened against corrupted state, but they are not MPSC or MPMC queues.

### Endpoint ownership

| Role while the queue is live | May call | Must not call concurrently |
| --- | --- | --- |
| Producer | producer-side methods: `push`, `emplace`, `claim`, `publish`, write regions, and write guards | consumer-side methods or management |
| Consumer | consumer-side methods: `front`, `pop`, snapshots, read regions, and read guards | producer-side methods or management |
| Third observer | only `size`, `empty`, `full`, `free`, `can_write`, `can_read`, `write_size`, and `read_size`, and only with an atomic-backed policy | endpoint methods, state-management methods, or a reservation decision based on an observation |
| Both endpoints stopped | `resize`, `clear`, `destroy`, `swap`, copy/move, `attach`, `adopt`, and state restore | restart traffic until management is complete |

The observer row is deliberately narrow. Its atomic-policy result is an
approximate snapshot, not a third endpoint or a reservation. See
[Concurrency and FreeRTOS](concurrency-and-freertos.md) for the supported
policy families.

## 2. Static vs Dynamic Geometry

Most containers come in two forms:

- static: the template capacity/depth is non-zero
- dynamic: the template capacity/depth is zero, and runtime `resize()` or constructor parameters are used

Examples:

```cpp
spsc::fifo<int, 1024> static_fifo;
spsc::fifo<int, 0> dynamic_fifo{1024};

spsc::pool<64> static_pool{256};
spsc::pool<0> dynamic_pool{64, 256};
```

The dynamic variants are typically **grow-oriented**. They may normalize the requested depth/capacity internally.

Static capacity fixes ring geometry; it does **not** universally mean that no
heap allocation occurs. The storage and lifetime model belongs to the
container family:

| Family | Static-capacity storage | Lifetime / allocation contract |
| --- | --- | --- |
| `fifo<T>` | in-object `T` slots | slots are already live; producer assigns and `pop()` does not destroy a slot |
| `queue<T>` | fixed geometry, dynamically allocated object storage | placement-new before publish and destruction on `pop()`; static `queue` can allocate |
| `pool` | fixed pointer-ring geometry | configured raw buffers are allocated separately |
| `typed_pool<T>` | fixed pointer-ring geometry | independent owning typed slots are allocated separately and destroyed on consume |
| static typed `latest<T, Depth>` | in-object typed slots | newest-state storage is bounded; dynamic forms allocate as needed |
| `*_view` | caller-owned backing storage | no ownership, allocation, or realignment of the backing storage |

## 3. Owning vs View Containers

There are two major families:

- owning containers own their storage, either inline or through allocation
- `*_view` containers only track head/tail/capacity and operate on memory that you provide

Use a view when:

- storage comes from DMA or a memory-mapped region
- another subsystem owns the backing buffer
- you want to restore state from saved `head/tail`

Use an owning container when you want a self-contained object with allocation handled by the library.

### View recovery metadata

`fifo_view::state_t` and `pool_view::state_t` serialize only `{head, tail}`.
They are index snapshots, not complete storage descriptions. A recovery record
must also preserve and validate the effective capacity/mask and the identity or
layout of the backing storage. `pool_view` additionally requires the same slot
ordering and a compatible `buffer_size`/buffer layout.

`adopt()` validates that the indices are locally legal for the geometry passed
to that call. It cannot prove that this is the geometry or backing layout from
which the indices were saved. Perform restore only while both endpoints are
stopped.

## 4. Producer Styles

Different containers expose different producer models.

### 4.1. Value-Based

Typical on `fifo` and `queue`:

```cpp
q.push(value);
q.emplace(args...);
```

This is the easiest style when you already have a ready-made object.

### 4.2. Zero-Copy `claim()` / `publish()`

Typical on almost every queue-like type:

```cpp
auto* slot = q.try_claim();
if (slot != nullptr) {
    // Fill slot in place.
    q.publish();
}
```

This is the preferred path when:

- the payload is large
- a producer fills a slot directly
- DMA or another subsystem writes into the slot memory

The producer transaction is `claim -> fill/construct -> publish`. Complete it
before another producer-side operation: do not claim a second slot, push, or
start a bulk/guard transaction while the first claim is outstanding.

### 4.3. Sticky-Latest

Specific to `latest`:

- producer publishes snapshots
- consumer reads only the newest committed slot
- consumer does **not** walk history like a FIFO

## 5. Consumer Styles

Most queue-like types support one or more of:

- `front()` / `try_front()`
- `pop()` / `try_pop()`
- `make_snapshot()` + `consume(snapshot)` / `try_consume(snapshot)`
- `claim_read(...)` for bulk region reads

Pick the simplest model that matches your consumer:

- `front/pop` for single-step consumption
- snapshots for read-many-then-consume
- bulk regions for contiguous span processing

`consume(snapshot)` is the fast precondition form: the consumer must not move
between snapshot capture and consume. Use `try_consume(snapshot)` when the
consumer path may branch, delay, or perform another consumer-side operation first.

Snapshots are short-lived consumer transactions. `clear`, reset, `destroy`,
`resize`, `swap`, move, `attach`, `adopt`, and any backing-storage replacement
invalidate every outstanding snapshot. `try_consume(snapshot)` validates the
current pointer, mask, tail, and captured range; it is not a generation or
epoch proof. An old snapshot can become indistinguishable after management
recreates the same state or after one complete modular counter period. Never
retain a snapshot across management or counter wrap.

Likewise, complete `front -> process -> pop` before another consumer-side
operation. A retained `front()` pointer, snapshot, bulk region, or guard is an
active consumer transaction; same-side interleaving invalidates its contract.

For copy-paste patterns covering these shared interface families, see [Method Recipes](method-recipes.md).
For guard- and bulk-helper specific APIs, see [Guard and Bulk Helpers](guard-and-bulk-helpers.md).

## 6. Policies

All main containers use a `Policy` parameter controlling metadata counters and,
where applicable, policy-derived storage alignment.

Ready-made families:

- `default_policy`: `FA<>` when no legacy override macro is defined
- `P`: plain counters for a single context or externally synchronized use
- `V`: volatile counters for externally ordered or platform-specific access
- `VV`: both counters and geometry volatile, with the same restriction
- `A<>`: atomic counters whose increments use atomic RMW operations
- `FA<>`: single-writer atomic counters whose increments use a relaxed load
  followed by a release store
- `AA<>`: atomic counters and atomic geometry

Cache-line aligned aliases:

- `CP`, `CV`, `CVV`
- `CA<>`, `CFA<>`, `CAA<>`

The generic `CacheAligned<>` spelling defaults its base to `default_policy`, so
in the normal v3 configuration it is `CFA<>`. Prefer the named aliases above
when the base policy must not depend on build configuration.

### Semantic aliases

For the three common transport contracts, use semantic aliases instead of
spelling a policy in every declaration:

| Contract | Example | Fixed policy |
| --- | --- | --- |
| one context or external synchronization | `local_fifo<T, N>` | `P` |
| normal portable SPSC | `concurrent_fifo<T, N>` | `FA<>` |
| SPSC with justified cache isolation | `cache_aligned_fifo<T, N>` | `CFA<>` |

The same prefixes are available for `queue`, `fifo_view`, `pool`, `pool_view`,
`typed_pool`, `latest`, `array_fifo` and its views, and `chunk_fifo` and its
view. They are exact type aliases: they do not add state, alter ownership, or
depend on `default_policy`. Owning forms preserve their normal policy-derived
allocator default and leave the final allocator argument available for a custom
allocator.

For new concurrent SPSC code, prefer `concurrent_*`. Choose
`cache_aligned_*` only when cache isolation is appropriate for the target and
measurement. The full `Container<..., Policy, ...>` spelling remains the
advanced API for deliberate use of `A<>`, `CA<>`, `V`, `VV`, `CP`, `AA<>`,
`CAA<>`, and related policies.

In v3, a bare container such as `fifo<T, N>` uses `FA<>` when
`SPSC_DEFAULT_POLICY_ATOMIC` is undefined. The macro is a legacy explicit
override, with deliberately preserved meanings:

| Configuration | `default_policy` |
| --- | --- |
| macro undefined | `FA<>` |
| `SPSC_DEFAULT_POLICY_ATOMIC=0` | `P` |
| `SPSC_DEFAULT_POLICY_ATOMIC=1` | `A<>` |

Do not use this macro for new policy selection. Use the semantic aliases or an
explicit policy instead. The aliases remain explicit and stable in all three
configurations. For the v2 plain-default migration and its ABI/layout impact,
read [Migrating from v2 to v3](migration-v3.md).

`buffer_pool` is intentionally outside this naming scheme. Its policy controls
storage alignment and span, not producer/consumer synchronization; use explicit
`CP` for DMA storage and select the transport alias on the queue or view.

The volatile families (`V`, `VV`, `CV`, and `CVV`) are not standalone portable
synchronization policies. Volatile access applies to their metadata objects but
does not create a C++ happens-before edge or order the surrounding payload. Use
them only when an external synchronization mechanism or a documented
compiler/platform contract supplies those guarantees.

Example:

```cpp
using Policy = spsc::policy::CA<>;
spsc::fifo<int, 1024, Policy> q;
```

`CA<>` is `CacheAligned<A<>>`: the cache-aligned strict-RMW counter backend.
`CFA<>` is `CacheAligned<FA<>>`: the cache-aligned single-writer backend. Both
are correct under the exact one-producer/one-consumer contract; `CA<>` is not
"more correct", it simply uses RMW increments. Choose between them for the
target and measurement, not from a correctness ranking.

In 2.0, the legacy `fast_fifo` and `fast_queue` convenience aliases select
`CFA<>`, the single-writer atomic backend that matches the exact SPSC ownership
rule. The word `fast` is not a cross-platform throughput claim. When concrete
type identity or the counter backend is an important design choice, spell the
container policy explicitly as `CA<>`, `CFA<>`, or another suitable policy.

Do not use `P` for normal thread/thread or task/task handoff. It has plain
non-atomic counters and relies on external synchronization or a genuinely
single-context execution model.

For task/ISR guidance and policy selection under RTOS-style concurrency, read [Concurrency and FreeRTOS](concurrency-and-freertos.md).

### Custom policy and counter backends

The full `Policy<Cnt, Geo>` spelling is an advanced extension point. A custom
counter backend is checked at the policy boundary and must provide:

- an unsigned integral `value_type` compatible with `reg`;
- a non-throwing default constructor;
- non-throwing `store(value_type)`, `load()`, `add(value_type)`, and `inc()`;
- an optional `load_relaxed()` only when it is non-throwing and returns a value
  convertible to the same `value_type`.

The concrete container can impose additional width requirements for its index
domain. For example, its `counter_type::value_type` must be at least as wide
as `reg`. Invalid backends are rejected when `Policy` is instantiated instead
of failing later in an unrelated container template.

The library does not require a custom counter or geometry backend's default
constructor to produce zero. Every container constructor explicitly stores the
canonical zero state before exposing the object. A backend must therefore
honor its non-throwing `store(0)` operation; its own default value is ignored.

`Policy::allocator_alignment` is optional; omitting it means `1`. When a
custom policy supplies it, the value must be a positive integral or enum
constant, fit in `std::size_t`, and be a power of two. The allocator and
cache-span helpers deliberately use power-of-two rounding, so a
value such as `24` is rejected rather than producing a silently incorrect
stride. Use `CP`, `CFA<>`, `CA<>`, or another explicit `CacheAligned<>` form
for the normal cache-line cases.

## 7. Alignment and Cache-Line Notes

When shadows are enabled for an eligible atomic-backed policy, `SPSCbase`
places producer-owned (`head` plus cached `tail`) and consumer-owned (`tail`
plus cached `head`) metadata in separate owner blocks. This endpoint isolation
does not depend on `CacheAligned`. The global shadow switch, counter-width gate,
and explicit 32-bit opt-in still decide whether those shadow-enabled blocks are
present.

Shadow delta validation is safe across ordinary counter wrap. It cannot carry
generation information through an entire counter-value period. For enabled
sub-64-bit shadows, unchecked producer/consumer progress invalidates the local
shadow so the next cached check reloads the real opposite counter. The 64-bit
hot path intentionally has no extra invalidation store; retaining a stale
shadow across all `2^64` counter values is treated as practically unreachable,
not claimed to be mathematically impossible.

`CacheAligned` policies influence policy-owned metadata and owning allocator
paths:

- metadata counters and geometry are cache-line padded
- default allocators derived from the policy can pick aligned allocation automatically
- raw-slot containers such as `pool` and `latest<void>` can round slot size upward when the policy requires it

Thus `A<>`/`FA<>` and `CA<>`/`CFA<>` have the same shadow eligibility. The
cache-aligned variants additionally pad their policy counters and geometry and
propagate allocator-alignment hints.

They cannot change an external buffer's address, alignment, or size. A
`fifo_view` or `pool_view` caller must align and size its own backing storage
for `T`, DMA, and cache-maintenance requirements.

Important boundary:

- `CacheAligned` helps with **software-visible layout**
- it does **not** perform hardware cache maintenance like `SCB_CleanDCache*` / `SCB_InvalidateDCache*`

For `STM32F7` / `STM32H7` style DMA usage:

- align metadata with `CA<>` / related aliases
- align the payload itself
- make raw slot sizes a multiple of the cache-line size when possible
- keep DMA cache maintenance or MPU policy outside the container
- make sure `SPSC_CACHELINE_BYTES` is 32; pass `-DSPSC_FORCE_CACHELINE=32`
  when your build does not expose a suitable Cortex-M or STM32 family macro
- clean before memory-to-peripheral DMA, invalidate after peripheral-to-memory
  DMA completes and before the CPU reads, using platform code that covers whole
  cache lines

## 8. Typed vs Raw Payload Models

Choose the payload model first, then choose the container:

- `fifo<T>`: assignment-based typed ring
- `queue<T>`: lifetime-managed typed ring
- `typed_pool<T>`: independent owning typed-slot container with pool-style pointers
- `pool`: raw byte buffers
- `latest<T>` / `latest<void>`: newest-state only, but still finite and able to become full
- `chunk<T>`: contiguous block used as a payload type

## 9. Which Container Fits Which Problem

- Use `fifo` for small messages and generic ring semantics.
- Use `queue` when `T` is not default-constructible or destruction timing matters.
- Use `pool` when the payload is raw bytes and fixed-size slots are natural.
- Use `typed_pool` when slots should hold one `T` but you still want pool-like pointer semantics.
- Use `latest` when state replacement matters more than history.
- Use `array_fifo` for fixed-size protocol frames.
- Use `chunk_fifo` for variable logical block length inside a fixed-capacity block type.

## 10. Non-Concurrent Operations

These are generally **not** safe to call concurrently with producer/consumer traffic:

- `resize()`
- `clear()`
- `destroy()`
- `swap()`
- copy / move / state restore helpers

Treat them as management operations performed while the queue is stopped.

Management stack usage is bounded independently of compile-time
`Capacity`/`Count`. Static `fifo` copy assignment works in place instead of
materializing a second full container. If a payload copy assignment throws, the
destination remains valid and logically empty (basic guarantee).

Static `pool` and `typed_pool`, plus `buffer_pool<T, 0, Count>`, use an
allocator-backed transient pointer table for transactional copy/resize work.
This does not add persistent fields or change container `sizeof`/`alignof`, but
the management operation performs one additional temporary allocation. A null
allocation result leaves the old destination unchanged; boolean `resize()`
reports `false`. These rules apply only while endpoints are stopped, as above.

RAII guard destructors still run during exception unwinding. Depending on the
guard state, scope exit can publish an armed write prefix or consume an active
read region; it is not an automatic rollback boundary. If guarded work may
throw, catch while the guard is still alive and call `cancel()` (or disarm
publication where that is the intended contract) before rethrowing. See
[Guard And Bulk Helpers](guard-and-bulk-helpers.md#exception-unwinding-and-raii-side-effects).

## 11. Assertions And Allocation Failure

`SPSC_ASSERT` is an opt-in diagnostic hook. Unless the application defines it
before including any `spsc` header, its default expansion is a no-op; the
library does not automatically map it to the standard `assert()` macro.

```cpp
#include <cassert>
#define SPSC_ASSERT(expr) assert(expr)
#include "fifo.hpp"
```

Define it consistently for the whole build. Assertions diagnose violated
preconditions; they are not a replacement for `try_*` methods or `is_valid()`.

The default `SPSC_ENABLE_EXCEPTIONS=0` configuration makes the shipped
allocators return `nullptr` on allocation failure. Because constructors cannot
return a status, most allocation-backed constructors may produce an invalid
object. This applies to runtime-sized containers and to allocation-backed
static forms such as `queue`. Check `is_valid()` before publishing the object
to producer/consumer endpoints. Prefer the boolean result of `resize()` or
`reserve()` when initialization must report failure explicitly.

A custom allocator used by an allocation-backed form in this mode must expose
`allocate(size_type) noexcept`. Throwing allocators, including
`std::allocator`, are rejected at compile time because mode `0` deliberately
does not build exception rollback handlers. This requirement applies to every
rebound allocator that actually allocates (for example, both slot and payload
allocators in a dynamic pool). It does not constrain an allocator template
argument that remains unused by wholly embedded storage, such as static
`fifo`, static typed `latest`, static `chunk`, or fully static `buffer_pool`.
Static `pool`, static `typed_pool`, and runtime-sized fixed-count `buffer_pool`
do use rebound allocators for payloads and transient management tables. Define
`SPSC_ENABLE_EXCEPTIONS=1` build-wide when a throwing allocator is required.

Runtime-shaped `buffer_pool` specializations are the deliberate exception: a
zero count or zero buffer size is a coherent empty shape, so `is_valid()` can
remain `true` after constructor allocation failure. When a non-empty pool is
required, use the boolean `resize()` result and verify the resulting
`count()`/`size()` against the requested shape.
