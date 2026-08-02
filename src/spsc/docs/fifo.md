# `spsc::fifo`

`fifo<T, Capacity, Policy, Alloc>` is the default owning SPSC ring buffer for value-like data.

It is assignment-based:

- slots are default-constructed
- `push()` / `emplace()` assign into the slot
- `pop()` advances the tail, but does not destroy the slot object

## When To Use It

Choose `fifo` when:

- `T` is default-constructible
- assignment or move-assignment is a good fit
- you want the simplest queue API
- you do not need per-element destructor calls on `pop()`

Choose `queue` instead when lifetime and explicit construction/destruction matter.

## Static And Dynamic Variants

### Static

```cpp
#include "fifo.hpp"

spsc::fifo<int, 1024> q;
```

Static `fifo` keeps its default-constructed `T` slots inside the queue object.
Its fixed capacity therefore does not use a separate payload allocation.

### Dynamic

```cpp
#include "fifo.hpp"

spsc::fifo<int, 0> q{1024};
// or:
// spsc::fifo<int, 0> q;
// q.resize(1024);
```

## Basic Producer / Consumer Flow

```cpp
spsc::fifo<int, 1024> q;

// Producer
q.push(1);
q.emplace(2);

// Consumer
if (!q.empty()) {
    int value = q.front();
    q.pop();
}
```

## Non-Blocking Style

```cpp
if (!q.try_push(value)) {
    // full
}

if (auto front = q.try_front()) {
    consume(*front);
    q.pop();
}
```

## Zero-Copy Producer Path

This is useful when a producer wants to fill the slot directly.

```cpp
auto* slot = q.try_claim();
if (slot != nullptr) {
    *slot = make_value();
    q.publish();
}
```

Complete `claim -> fill -> publish` as one producer transaction. Do not start
another producer operation while that claim remains outstanding.

Bulk publish is also available when you claim or write several elements via region APIs.

## Snapshots

Snapshots are a convenient consumer pattern when you want to read a stable logical range and then consume it.

```cpp
auto snap = q.make_snapshot();

for (const auto& item : snap) {
    process(item);
}

q.consume(snap);
```

`consume(snapshot)` is a precondition API: the consumer must not move between
`make_snapshot()` and `consume()`. Use `try_consume(snapshot)` when consumer
logic may branch, delay, or observe another consumer-side operation first.

Use snapshots when:

- you want simple read-only iteration
- the producer may continue pushing while the consumer walks the captured range

## More Example Patterns

### Drop-New On Overflow

```cpp
if (!q.try_push(sample)) {
    ++droppedSamples;
}
```

### Keep Only Newest Data

Do not call `pop()` from the producer to make room in a live SPSC FIFO. The
producer owns head movement; the consumer owns tail movement. Use `latest`
for newest-state semantics, or have the consumer explicitly discard stale
entries on the consumer side.

### Batch Producer With A Bulk Guard

```cpp
auto guard = q.scoped_write(4);

while (guard.remaining() != 0u) {
    guard.write_next(read_sensor_sample());
}

guard.commit();
```

### Snapshot Then Conditional Consume

```cpp
auto snap = q.make_snapshot();

if (is_frame_complete(snap)) {
    decode_frame(snap);
    q.consume(snap);
}
```

## API Groups

### Producer

- `push`, `try_push`
- `emplace`, `try_emplace`
- `claim`, `try_claim`
- `publish`, `try_publish`

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- `consume`, `consume_all`
- `make_snapshot`

### Bulk / Iteration

- iterators over the current logical range
- `claim_read(...)`
- write/read region helpers
- scoped guards

### Management

- `reserve`
- `resize`
- `clear`
- `swap`

## Bulk Regions

`fifo` also supports contiguous read/write region access. This is helpful when you want to process several elements with minimal wrap-around handling.

Typical fit:

- batching sensor samples
- copying from one queue into another buffer
- integration with APIs that accept spans or pointer+length pairs

## Policies And Alignment

Examples:

```cpp
using Fast = spsc::fifo<int, 1024, spsc::policy::CA<>>;
using Plain = spsc::fifo<int, 1024, spsc::policy::P>;
```

For cache-sensitive payloads:

```cpp
struct alignas(32) Frame {
    std::uint8_t bytes[128];
};

using Q = spsc::fifo<Frame, 128, spsc::policy::CA<>>;
```

This gives a better default for metadata and starting storage alignment. For DMA-style payloads, also make sure `sizeof(Frame)` fits your cache-line strategy.

## Good Fits

- integer messages
- small structs
- handles and IDs
- logging records when default construction is acceptable

## Less Good Fits

- non-default-constructible objects
- objects where `pop()` must run the destructor immediately
- external storage ownership, where `fifo_view` is a better fit

## Method Reference

### Construction

- `fifo()` creates an empty queue.
- `fifo(requested_capacity)` exists only for dynamic `fifo<T, 0, ...>`.
- copy and move are supported.
- `swap(other)` exchanges storage and queue state. Use only when the queue is stopped.

Example:

```cpp
spsc::fifo<int, 0> q;
q.resize(256);
```

### State And Introspection

- `is_valid()` matters mostly for dynamic queues. Static `fifo` is always valid.
- `capacity()`, `size()`, `empty()`, `full()`, `free()` describe queue state.
- `can_write(n)` and `can_read(n)` are useful before bulk operations.
- `write_size()` and `read_size()` expose the current producer/consumer window.
- `get_allocator()` returns a default-constructed allocator object.

Typical guard:

```cpp
if (q.can_write(4)) {
    // Safe to publish 4 elements.
}
```

### Iteration And Data Access

- `data()` returns the underlying storage pointer.
- `begin/end`, `cbegin/cend`, `rbegin/rend` iterate the current logical range.
- `operator[](i)` indexes logical queue order, not raw ring order.
- `span()` is available when `std::span` support is enabled and exposes the whole backing storage.

Use iterators and `operator[]` on the consumer side or in non-concurrent inspection code.

### Snapshots

- `make_snapshot()` captures the current logical `[tail, head)` range.
- `consume(snapshot)` consumes exactly the captured range.
- `try_consume(snapshot)` succeeds only if the consumer has not moved since the snapshot was taken.
- `consume_all()` advances tail to head.

Use `try_consume()` when snapshots may outlive intermediate consumer logic.

### Producer Methods

- `push(value)` writes one element and advances head. Precondition: not full.
- `try_push(value)` returns `false` when full.
- `emplace(args...)` constructs a temporary `T` and assigns it into the slot.
- `try_emplace(args...)` returns `nullptr` when full.
- `claim()` returns a writable slot reference without advancing head.
- `try_claim()` returns `nullptr` when full.
- `publish()` commits one claimed slot.
- `try_publish()` returns `false` when full.
- `publish(unsafe, n)` and `try_publish(unsafe, n)` commit several previously prepared slots.

Claim/publish example:

```cpp
auto* slot = q.try_claim();
if (slot) {
    *slot = produce_value();
    q.publish();
}
```

### Consumer Methods

- `front()` returns the current readable element. Precondition: not empty.
- `try_front()` returns `nullptr` when empty.
- `pop()` consumes one element.
- `try_pop()` returns `false` when empty.
- `pop(n)` and `try_pop(n)` consume several elements.

The deleted `pop(U&)` and `try_pop(U&)` overload traps are intentional. They prevent accidental binding of numeric lvalues to `pop(size_type)`.

### Bulk And RAII Helpers

- `claim_write(unsafe, max_count)` returns up to two contiguous writable regions.
- `claim_read(unsafe, max_count)` returns up to two contiguous readable regions.
- `bulk_write_guard` batches several writes and publishes them automatically or manually.
- `bulk_read_guard` batches reads and pops on scope exit or explicit `commit()`.
- `write_guard` and `read_guard` are single-slot RAII helpers.
- `scoped_write()`, `scoped_write(max_count)`, `scoped_read()`, `scoped_read(max_count)` construct those helpers directly.

Use these when you want fewer branch points or a cleaner commit/cancel workflow.

Example:

```cpp
if (auto guard = q.scoped_write()) {
    guard.ref() = produce_value();
    // publish() happens on scope exit because ref() arms publish_on_destroy().
}

if (auto guard = q.scoped_read()) {
    consume(*guard);
    // pop() happens on scope exit unless you call cancel().
}
```

### Management Methods

- `reserve(min_capacity)` exists on dynamic `fifo`.
- `resize(requested_capacity)` exists on dynamic `fifo`.
- `clear()` resets indices but keeps storage.
- `destroy()` releases storage in the dynamic variant.

These methods are non-concurrent management operations.

## Snippet Catalog

### `capacity()`, `size()`, `free()`

```cpp
const auto cap = q.capacity();
const auto used = q.size();
const auto freeSlots = q.free();
```

### `empty()`, `full()`

```cpp
if (q.empty()) {
    return;
}

if (q.full()) {
    ++fullCount;
}
```

### `write_size()`, `read_size()`

```cpp
const auto contiguousWritable = q.write_size();
const auto contiguousReadable = q.read_size();
```

### `data()`

```cpp
auto* storage = q.data();
inspect_ring_storage(storage, q.capacity());
```

### `operator[]`

```cpp
for (std::size_t i = 0; i < static_cast<std::size_t>(q.size()); ++i) {
    process(q[static_cast<decltype(q.size())>(i)]);
}
```

### `begin()`, `end()`

```cpp
for (const auto& item : q) {
    process(item);
}
```

### `rbegin()`, `rend()`

```cpp
for (auto it = q.rbegin(); it != q.rend(); ++it) {
    process_reverse(*it);
}
```

### `span()`

Available only when C++20 `std::span` support is enabled (`SPSC_HAS_SPAN=1`).

```cpp
auto ring = q.span();
inspect_storage_bytes(ring.data(), ring.size());
```

### `push()`

```cpp
q.push(42);
```

### `try_push()`

```cpp
if (!q.try_push(value)) {
    ++dropCount;
}
```

### `emplace()`

```cpp
q.emplace(arg1, arg2, arg3);
```

### `try_emplace()`

```cpp
if (auto* slot = q.try_emplace(arg1, arg2, arg3)) {
    touch(*slot);
}
```

### `claim()`

```cpp
auto& slot = q.claim();
slot = produce_value();
q.publish();
```

### `try_claim()`

```cpp
if (auto* slot = q.try_claim()) {
    *slot = produce_value();
    q.publish();
}
```

### `publish()`

```cpp
auto* slot = q.try_claim();
if (slot) {
    *slot = 7;
    q.publish();
}
```

### `try_publish()`

```cpp
auto* slot = q.try_claim();
if (slot) {
    *slot = 7;
    (void)q.try_publish();
}
```

### `publish(unsafe, n)`

```cpp
auto guard = q.scoped_write(3);
guard.write_next(1);
guard.write_next(2);
guard.write_next(3);
guard.commit(); // equivalent to publish(3)
```

### `front()`

```cpp
if (!q.empty()) {
    auto& front = q.front();
    process(front);
}
```

### `try_front()`

```cpp
if (auto* front = q.try_front()) {
    process(*front);
}
```

### `pop()`

```cpp
if (!q.empty()) {
    q.pop();
}
```

### `try_pop()`

```cpp
if (!q.try_pop()) {
    ++emptyHits;
}
```

### `pop(n)`

```cpp
if (q.can_read(4)) {
    q.pop(4);
}
```

### `make_snapshot()`

```cpp
auto snap = q.make_snapshot();
for (const auto& item : snap) {
    process(item);
}
```

### `consume(snapshot)`

```cpp
auto snap = q.make_snapshot();
consume_snapshot_items(snap);
q.consume(snap);
```

Use `try_consume(snapshot)` instead when the snapshot is not consumed
immediately after the captured range is processed.

### `try_consume(snapshot)`

```cpp
auto snap = q.make_snapshot();
if (validate(snap)) {
    (void)q.try_consume(snap);
}
```

### `consume_all()`

```cpp
q.consume_all();
```

### `claim_write()`

```cpp
auto regs = q.claim_write(spsc::unsafe, 8);
for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    regs.first.ptr[i] = make_value();
}
q.publish(::spsc::unsafe, regs.total);
```

### `claim_read()`

```cpp
auto regs = q.claim_read(spsc::unsafe, 8);
for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    process(regs.first.ptr[i]);
}
q.pop(regs.total);
```

### `scoped_write()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.ref() = make_value();
}
```

### `scoped_read()`

```cpp
if (auto guard = q.scoped_read()) {
    process(*guard);
}
```

### `clear()`

```cpp
q.clear();
```

### `reserve()`

```cpp
spsc::fifo<int, 0> dyn;
dyn.reserve(256);
```

### `resize()`

```cpp
spsc::fifo<int, 0> dyn;
dyn.resize(512);
```

### `destroy()`

```cpp
spsc::fifo<int, 0> dyn{128};
dyn.destroy();
```

### `swap()`

```cpp
spsc::fifo<int, 0> a{128};
spsc::fifo<int, 0> b{256};
swap(a, b);
```
