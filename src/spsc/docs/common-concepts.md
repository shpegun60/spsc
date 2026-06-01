# Common Concepts

This page explains the ideas shared by almost all `spsc` containers.

## 1. Concurrency Contract

Every container in this library is **strictly SPSC**:

- exactly one producer
- exactly one consumer
- no multi-producer or multi-consumer access to the same instance

This applies even when the API looks "safe". The containers are hardened against corrupted state, but they are not MPSC or MPMC queues.

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

## 3. Owning vs View Containers

There are two major families:

- owning containers allocate and own their storage
- `*_view` containers only track head/tail/capacity and operate on memory that you provide

Use a view when:

- storage comes from DMA or a memory-mapped region
- another subsystem owns the backing buffer
- you want to restore state from saved `head/tail`

Use an owning container when you want a self-contained object with allocation handled by the library.

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

For copy-paste patterns covering these shared interface families, see [Method Recipes](method-recipes.md).
For guard- and bulk-helper specific APIs, see [Guard and Bulk Helpers](guard-and-bulk-helpers.md).

## 6. Policies

All main containers use a `Policy` parameter controlling the metadata counters.

Ready-made families:

- `default_policy`: `P` by default; define `SPSC_DEFAULT_POLICY_ATOMIC=1` only when you deliberately want `A<>` as the default
- `P`: plain counters, fastest on simple single-core paths
- `V`: volatile counters for ISR/task style communication
- `VV`: both counters and geometry volatile
- `A<>`: strict atomic counters
- `FA<>`: fast single-writer atomic counters
- `AA<>`: atomic counters and atomic geometry

Cache-line aligned aliases:

- `CP`, `CV`, `CVV`
- `CA<>`, `CFA<>`, `CAA<>`

Example:

```cpp
using Policy = spsc::policy::CA<>;
spsc::fifo<int, 1024, Policy> q;
```

Do not use `P` for normal thread/thread or task/task handoff. It has plain
non-atomic counters and relies on external synchronization or a genuinely
single-context execution model.

For task/ISR guidance and policy selection under RTOS-style concurrency, read [Concurrency and FreeRTOS](concurrency-and-freertos.md).

## 7. Alignment and Cache-Line Notes

`CacheAligned` policies now influence more than just metadata:

- metadata counters and geometry are cache-line padded
- default allocators derived from the policy can pick aligned allocation automatically
- raw-slot containers such as `pool` and `latest<void>` can round slot size upward when the policy requires it

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
- `typed_pool<T>`: stable slots holding one `T` each
- `pool`: raw byte buffers
- `latest<T>` / `latest<void>`: newest-state only
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
