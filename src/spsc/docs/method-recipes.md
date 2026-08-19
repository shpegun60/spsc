# Method Recipes

This page is the practical cheat sheet for the shared interface families used across `spsc` containers.

Use it together with the per-container pages:

- the container page tells you whether the method exists and what its semantics are
- this page shows the most typical usage pattern
- [Guard and Bulk Helpers](guard-and-bulk-helpers.md) covers the advanced helper objects in detail

## Which Containers Support Which Style

| Interface family | Typical containers |
| --- | --- |
| `push` / `try_push` / `emplace` | `fifo`, `fifo_view`, `queue`, `latest<T>`, `typed_pool`, `pool`, `pool_view` |
| `claim` / `try_claim` + `publish` | almost all queue-like containers |
| `front` / `try_front` + `pop` | almost all queue-like containers |
| snapshots | `fifo`, `fifo_view`, `queue`, `pool`, `pool_view`, `typed_pool`, wrappers built on them |
| bulk `claim_write` / `claim_read` | `fifo`, `fifo_view`, `queue`, `pool`, `pool_view`, `typed_pool` |
| `attach` / `adopt` / `state` | `fifo_view`, `pool_view`, wrappers built on `fifo_view` |
| typed raw-slot overlays | `pool`, `pool_view` via `claim_as`, `front_as`, `try_peek` |
| newest-only sticky read | `latest` |

## 1. State Checks Before You Touch The Queue

This is the smallest safe mental template:

```cpp
if (!q.is_valid()) {
    return;
}

if (q.empty()) {
    return;
}

if (q.full()) {
    return;
}
```

Useful shared introspection methods:

- `capacity()`, `size()`, `free()`
- `can_write(n)`, `can_read(n)`
- `write_size()`, `read_size()` on the queue-like types that expose contiguous windows

Example:

```cpp
if (q.can_write(8)) {
    // producer can publish up to 8 prepared elements
}
```

## 2. Value-Based Producer: `push` / `try_push`

Use this when you already have a finished value.

```cpp
if (!q.try_push(value)) {
    // queue full
}
```

This is the simplest path for:

- `fifo`
- `fifo_view`
- `queue`
- `typed_pool`
- typed `latest`

`pool` and `pool_view` also have typed `push(const U&)` / `try_push(const U&)`, but only for trivially-copyable `U`.

## 3. In-Place Construction: `emplace` / `try_emplace`

Use this when constructing the object directly is clearer than creating a temporary.

```cpp
q.emplace(arg1, arg2, arg3);

if (auto* slot = q.try_emplace(arg1, arg2, arg3)) {
    use_immediately(*slot);
}
```

Best fit:

- `queue`
- `typed_pool`
- typed `latest`
- also `fifo` / `fifo_view` when assignment-based slot reuse is fine

## 4. Zero-Copy Producer: `claim` / `try_claim` + `publish`

This is the main pattern when the producer wants direct slot access.

```cpp
if (auto* slot = q.try_claim()) {
    fill_slot(*slot);
    q.publish();
}
```

Use this for:

- large payloads
- DMA-prepared buffers
- block-oriented writes
- `array_fifo` / `chunk_fifo`, where value-based producer methods are intentionally deleted

Treat `claim -> fill/construct -> publish` as one producer transaction. Do not
interleave it with another producer-side claim, push, bulk region, or guard.

Bulk publish is also available:

```cpp
auto regs = q.claim_write(spsc::unsafe, 8);

for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    fill(regs.first.ptr[i]);
}
for (std::size_t i = 0; i < static_cast<std::size_t>(regs.second.count); ++i) {
    fill(regs.second.ptr[i]);
}

q.publish(::spsc::unsafe, regs.total);
```

## 5. Raw-Storage Producer: `queue` / `typed_pool` Construction Path

`queue` returns uninitialized raw storage from `claim()`.  
`typed_pool` returns persistent object storage intended for placement new or guarded construction.

`queue` manual construction:

```cpp
if (auto* slot = q.try_claim()) {
    new (slot) Message(42, "payload");
    q.publish();
}
```

`typed_pool` manual construction:

```cpp
if (auto* slot = q.try_claim()) {
    new (slot) Frame{};
    prepare(*slot);
    q.publish();
}
```

Prefer `emplace(...)` or the RAII guards unless you specifically need this lower-level path.

## 6. Consumer Pattern: `front` / `try_front` + `pop`

This is the standard consume-one-item flow:

```cpp
if (auto* item = q.try_front()) {
    process(*item);
    q.pop();
}
```

That same shape works for:

- `fifo`
- `fifo_view`
- `queue`
- `pool`
- `pool_view`
- `typed_pool`
- wrappers built on them

Treat `front -> process -> pop` as one consumer transaction. Do not obtain a
second front, snapshot, bulk region, or read guard while the first retained
view is active.

For `latest`, remember that `front()` is the newest committed slot, not the oldest queued element.

## 7. Snapshot Pattern

Use snapshots when the consumer wants to inspect a stable logical range and then consume exactly that range.

```cpp
auto snap = q.make_snapshot();

for (const auto& item : snap) {
    process(item);
}

q.consume(snap);
```

Use `try_consume(snapshot)` when the consumer path may branch or delay:

```cpp
auto snap = q.make_snapshot();

if (validate(snap)) {
    (void)q.try_consume(snap);
}
```

This pattern exists on the FIFO-like families, but not on `latest`.

## 8. View-State Pattern: `attach` / `adopt` / `state`

Use this when storage lives outside the container.

Fresh attach:

```cpp
spsc::fifo_view<int, 0> q;
q.attach(storage.data(), storage.size());
```

Restore from saved state:

```cpp
spsc::fifo_view<int, 0>::state_t st{savedHead, savedTail};
q.attach(storage.data(), storage.size(), st);
```

Same idea applies to `pool_view`:

```cpp
spsc::pool_view<0>::state_t st{savedHead, savedTail};
q.attach(slots.data(), depth, bufferSize, st);
```

Use:

- `attach(...)` when you want to bind storage and reset queue state
- `adopt(...)` when you already have head/tail and want to restore them
- `state()` when you want to serialize the current head/tail
- `reset()` when you want to clear indices but keep storage attached
- `detach()` when you want the view to forget storage entirely

## 9. Raw Buffer Overlay: `claim_as`, `front_as`, `try_peek`

These are specific to `pool` and `pool_view`.

Producer typed overlay:

```cpp
if (auto* hdr = q.claim_as<Header>()) {
    ::new (static_cast<void*>(hdr)) Header{0x12345678u, 64};
    q.publish();
}
```

`claim_as<U>()` checks size and alignment only. In C++17, placement-new is what
starts `U` lifetime in the raw slot.

Consumer typed overlay:

```cpp
if (auto* hdr = q.front_as<Header>()) {
    // Only valid if the producer started Header lifetime in the slot.
    handle_header(*hdr);
    q.pop();
}
```

Copy-out peek:

```cpp
Header hdr{};
if (q.try_peek(hdr)) {
    handle_header(hdr);
}
```

Use `try_peek` when you want a safe copy and do not want to depend on slot lifetime directly.

## 10. Scoped Single-Slot Guards

These are the easiest RAII helpers for one element at a time.

`fifo`:

```cpp
if (auto guard = q.scoped_write()) {
    guard.ref() = produce_value();
}

if (auto guard = q.scoped_read()) {
    consume(*guard);
}
```

`queue`:

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(42, "payload");
}

if (auto guard = q.scoped_read()) {
    consume(*guard);
}
```

`pool`:

```cpp
if (auto guard = q.scoped_write()) {
    if (auto* hdr = guard.as<Header>()) {
        ::new (static_cast<void*>(hdr)) Header{0x12345678u, 64};
    }
}
```

`typed_pool`:

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(args...);
}
```

Default rule:

- write guards publish on scope exit only when properly armed
- read guards pop on scope exit unless you call `cancel()`

## 11. Scoped Bulk Guards

Bulk guards help when you expect to process several slots per wakeup.

`fifo` / `fifo_view`:

```cpp
auto guard = q.scoped_write(8);
while (guard.remaining() != 0u) {
    guard.write_next(produce_value());
}
guard.commit();
```

`queue` / `typed_pool`:

```cpp
auto guard = q.scoped_write(4);
while (guard.remaining() != 0u) {
    guard.emplace_next(args...);
}
guard.commit();
```

`pool` / `pool_view`:

```cpp
auto guard = q.scoped_write(4);
while (guard.remaining() != 0u) {
    guard.write_next(packet_data, packet_size);
}
guard.commit();
```

When you use the guard path, prefer `remaining()`, `commit()`, and `cancel()` over manual index arithmetic.

## 12. Bulk Read Regions

`claim_read(unsafe, max_count)` is the low-level batch-consumer API.

For `fifo` / `fifo_view`:

```cpp
auto regs = q.claim_read(spsc::unsafe, 16);

for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    process(regs.first.ptr[i]);
}
for (std::size_t i = 0; i < static_cast<std::size_t>(regs.second.count); ++i) {
    process(regs.second.ptr[i]);
}

q.pop(regs.total);
```

For `queue`, the read side is the same shape, but the elements are live objects:

```cpp
auto regs = q.claim_read(spsc::unsafe, 8);

for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    process(regs.first.ptr[i]);
}

q.pop(regs.total);
```

For `pool` / `pool_view` / `typed_pool`, the region is a slot list:

```cpp
auto regs = q.claim_read(spsc::unsafe, 4);

for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    consume_slot(regs.first.ptr[i]);
}

q.pop(regs.total);
```

Use this style when one scheduler wakeup should process many elements with minimal branch overhead.

## 13. Queue Raw Write Regions

`queue::claim_write(unsafe, max_count)` returns raw uninitialized storage.  
That means you must construct the objects manually.

```cpp
auto regs = q.claim_write(spsc::unsafe, 2);

if (regs.first.count != 0u) {
    auto* raw = regs.first.raw;
    auto* slots = reinterpret_cast<Message*>(raw);
    new (&slots[0]) Message(1, "hello");
}

q.publish(::spsc::unsafe, 1);
```

This path is powerful, but easier to misuse than `emplace()` or `scoped_write()`.

## 14. Cancel And Deferred Commit

The guard APIs are intentionally designed for "inspect first, commit later" workflows.

Producer cancel:

```cpp
auto guard = q.scoped_write();
if (!guard) {
    return;
}

if (!producer_ready()) {
    guard.cancel();
    return;
}

guard.ref() = produce_value();
```

Consumer cancel:

```cpp
auto guard = q.scoped_read();
if (!guard) {
    return;
}

if (!can_process_now(*guard)) {
    guard.cancel(); // do not pop yet
    return;
}

process(*guard);
guard.commit();
```

This is especially useful when the queue is only one part of a larger state machine.

## 15. `latest`: Newest-State Pattern

`latest` is not a FIFO. Use it like this:

```cpp
if (Telemetry* slot = latestState.try_claim()) {
    *slot = read_telemetry();
    latestState.publish();
}

if (const Telemetry* view = latestState.try_front()) {
    consume(*view);
    latestState.pop();
}
```

If the producer updates very frequently and the consumer only cares about freshness:

```cpp
if (Telemetry* slot = latestState.try_claim()) {
    *slot = read_telemetry();

    const bool published = latestState.coalescing_publish();
    if (published) {
        notify_consumer();
    }
}
```

Here `false` means **not published**, not "queued for eventual publication".
It is appropriate when a later periodic update may replace the current
producer-private slot. Use `publish()`, `try_publish()`, or an explicit retry
protocol for a one-shot command that must arrive after the producer stops.

Dynamic raw `latest<void, 0>` setup:

```cpp
spsc::latest<void, 0, spsc::policy::CA<>> q;
q.resize(8, 128);
```

Dynamic typed `latest<T, 0>` setup:

```cpp
spsc::latest<Telemetry, 0> q;
q.resize(8);
```

Consumer-side stale-drop pattern:

```cpp
while (latestState.try_front()) {
    auto* view = latestState.try_front();
    if (!view) {
        break;
    }

    render(*view);
    latestState.pop();
}
```

## 16. `chunk`: Build A Block Then Hand It Off

Use `chunk` as a local block builder:

```cpp
spsc::chunk<std::uint16_t, 256> block;

block.clear();
block.push(10);
block.push(20);
block.push(30);
```

DMA-style logical-size commit:

```cpp
start_dma(block.data(), block.capacity());

// Later, after DMA completion and platform cache maintenance:
block.commit_size(actualCount);
```

Dynamic `chunk` setup:

```cpp
spsc::chunk<std::uint16_t, 0> block;
block.reserve(512);
```

C++17-safe processing:

```cpp
for (std::uint16_t sample : block) {
    process(sample);
}
```

When C++20 `std::span` support is enabled (`SPSC_HAS_SPAN=1`), the same used range is also
available through `block.used_span()`.

## 17. Wrapper Families: `array_fifo*` And `chunk_fifo*`

`array_fifo*` producer:

```cpp
if (auto* frame = rxFrames.try_claim()) {
    frame->fill(0);
    (*frame)[0] = 0xAA;
    (*frame)[1] = 0x55;
    rxFrames.publish();
}
```

`chunk_fifo*` producer:

```cpp
if (auto* block = audioBlocks.try_claim()) {
    block->clear();
    block->push(left);
    block->push(right);
    audioBlocks.publish();
}
```

These wrappers intentionally push you toward zero-copy production.

## 18. Dynamic Reconfiguration

These methods are management operations, not hot-path concurrent operations.

Examples:

```cpp
spsc::fifo<int, 0> q;
q.resize(256);

spsc::queue<Message, 0> q2;
q2.reserve(512);

spsc::pool<0, spsc::policy::CA<>> q3;
q3.resize(64, 100); // under CacheAligned defaults this can round internally

spsc::latest<void, 0, spsc::policy::CA<>> q4;
q4.reserve(8, 100);
```

Stop both producer and consumer before:

- `resize`
- `reserve`
- `clear`
- `destroy`
- `swap`
- `attach` / `adopt`

Safe stop/reconfigure sketch:

```cpp
stop_requested = true;
wait_until_producer_stopped();
wait_until_consumer_stopped();

q.clear();
q.resize(newCapacity);

stop_requested = false;
restart_endpoints();
```

## 19. Single-Thread Local Queue Use

Sometimes the container is just a local ring or scratch pipeline inside one loop.

```cpp
spsc::fifo<int, 16> q;

for (int i = 0; i < 8; ++i) {
    q.push(i);
}

while (auto* item = q.try_front()) {
    process(*item);
    q.pop();
}
```

This is a valid use of the containers. You do not need real concurrency to benefit from the API.

## 20. Deterministic Interleaving In One Thread

This is useful in tests, simulations, and host-side verification.

```cpp
spsc::fifo<int, 8> q;

q.push(1);
q.push(2);

if (auto* first = q.try_front()) {
    process(*first);
    q.pop();
}

q.push(3);

auto snap = q.make_snapshot();
for (const auto& item : snap) {
    process(item);
}
q.consume(snap);
```

You can model producer and consumer actions explicitly without threads.

## 21. Pre-Initialization Before Concurrency Starts

Use this pattern when dynamic storage or external views need setup before producer/consumer go live.

```cpp
spsc::chunk_fifo<std::uint16_t, 0, 8> q;

for (std::size_t i = 0; i < static_cast<std::size_t>(q.capacity()); ++i) {
    q.data()[i].reserve(256);
    q.data()[i].clear();
}

// Only after this do the producer and consumer start running.
```

The same idea applies to:

- `fifo_view.attach(...)`
- `pool_view.attach(...)`
- dynamic `resize()` / `reserve()`
- restoring `state_t` into view containers

## 22. Save / Restore State Across Restart

```cpp
spsc::fifo_view<int, 0>::state_t st = q.state();
persist_state(st.head, st.tail);
```

Later:

```cpp
spsc::fifo_view<int, 0>::state_t restored{savedHead, savedTail};
q.attach(storage.data(), storage.size(), restored);
```

This is one of the main reasons the `*_view` family exists.

## 23. Drain One Container Into Another

```cpp
while (auto* item = src.try_front()) {
    if (!dst.try_push(*item)) {
        break;
    }
    src.pop();
}
```

This is useful for:

- staging from ISR-facing queue to task-facing queue
- changing policy/container boundary between pipeline stages
- copying from raw ingress to typed decoded queue
