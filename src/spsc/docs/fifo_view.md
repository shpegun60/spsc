# `spsc::fifo_view`

`fifo_view<T, Capacity, Policy>` is the non-owning sibling of `fifo`.

It keeps the SPSC indices and logic, but the storage is supplied by you.

## When To Use It

Choose `fifo_view` when:

- you already have a backing buffer
- storage must live in a specific memory region
- you want to restore or adopt saved head/tail state
- DMA, shared memory, or fixed SRAM layout owns the actual payload memory

## Static Storage Example

```cpp
#include "fifo_view.hpp"
#include <array>

std::array<int, 1024> storage{};
spsc::fifo_view<int, 1024> q{storage};
```

You can also attach later:

```cpp
spsc::fifo_view<int, 1024> q;
q.attach(storage);
```

## Dynamic Storage Example

```cpp
std::vector<int> storage(2048);

spsc::fifo_view<int, 0> q;
q.attach(storage.data(), storage.size());
```

## State Restore / Adopt

This is useful for shared memory or restart/recovery flows.

```cpp
spsc::fifo_view<int, 0>::state_t st{
    .head = savedHead,
    .tail = savedTail,
};

q.attach(storage.data(), storage.size(), st);
```

## More Example Patterns

### Cache-Aligned External Storage

```cpp
struct alignas(32) Frame {
    std::uint8_t bytes[128];
};

alignas(32) static Frame storage[16];

spsc::fifo_view<Frame, 16, spsc::policy::CA<>> q{storage};
```

`CA<>` aligns the queue's metadata, not `storage`. A view never reallocates or
realigns caller-owned memory, so `Frame` and the backing array must already
meet the alignment and DMA/cache requirements of the target.

### Reset While Keeping The Same Buffer

```cpp
q.reset();
// storage is still attached, but head/tail are cleared
```

### Detach And Reattach Later

```cpp
q.detach();

if (reconfigure_storage()) {
    q.attach(storage.data(), storage.size());
}
```

## Producer / Consumer API

The API shape is intentionally close to `fifo`:

- `push`, `try_push`
- `emplace`, `try_emplace`
- `claim`, `publish`
- `front`, `pop`
- snapshots and bulk regions

So the migration path between `fifo` and `fifo_view` is usually straightforward.

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

### External-Storage Management

- constructors from array/pointer
- `attach`
- `adopt`
- `detach`
- serialized `state_t`

## Alignment Rules

`fifo_view` validates the alignment of the external storage against `alignof(T)`.

That means:

- if `T` is naturally aligned, a normal buffer is enough
- if `T` is `alignas(32)`, the backing storage must also be 32-byte aligned

For DMA or manually aligned payloads, make the external storage contract explicit.

## Good Fits

- external static arrays
- memory regions owned by another subsystem
- integration with a DMA-prepared buffer
- recovery from saved `head/tail`

## Less Good Fits

- scenarios where you want the queue to own allocation
- cases where the backing buffer lifetime is unclear

## Method Reference

### Construction And Attachment

- `fifo_view()` creates a detached view.
- static constructors accept `std::array<T, Capacity>&` or `T (&buffer)[Capacity]`.
- dynamic constructors accept `(pointer buffer, size_type capacity)`.
- `attach(...)` binds external storage and clears queue state.
- `adopt(...)` binds storage and restores a supplied head/tail state.
- `reset()` clears head/tail while keeping the current storage attached.
- `detach()` forgets the storage and resets the queue state.
- `state()` returns a serializable `{head, tail}` snapshot.

Typical restore flow:

```cpp
spsc::fifo_view<int, 0>::state_t st{savedHead, savedTail};
q.attach(buffer, capacity, st);
```

### State And Introspection

- `is_valid()` tells you whether storage is attached correctly.
- `capacity()`, `size()`, `empty()`, `full()`, `free()` expose queue state.
- `can_write(n)`, `can_read(n)`, `write_size()`, `read_size()` help with batch logic.
- `clear()` resets indices while keeping attached storage.
- `data()` returns the external storage pointer.

### Iteration And Snapshots

- iterators work the same way as in `fifo`
- `operator[](i)` indexes logical queue order
- `make_snapshot()`, `consume(snapshot)`, `try_consume(snapshot)`, `consume_all()` mirror `fifo`
- `span()` exposes the whole attached storage when spans are enabled

### Producer Methods

- `push`, `try_push`
- `emplace`, `try_emplace`
- `claim`, `try_claim`
- `publish`, `try_publish`
- `publish(unsafe, n)`, `try_publish(unsafe, n)`

Use the same producer patterns as `fifo`, but remember that the storage lifetime is external.

### Consumer Methods

- `front`, `try_front`
- `pop`, `try_pop`
- `pop(n)`, `try_pop(n)`
- deleted `pop(U&)` / `try_pop(U&)` overload traps protect against accidental numeric binding

### Bulk And RAII Helpers

- `claim_write(unsafe, max_count)`
- `claim_read(unsafe, max_count)`
- `bulk_write_guard`, `bulk_read_guard`
- `write_guard`, `read_guard`
- `scoped_write()`, `scoped_write(max_count)`, `scoped_read()`, `scoped_read(max_count)`

These helper types behave like the `fifo` versions, but operate on external storage.

## Snippet Catalog

### `attach(buffer, capacity)`

```cpp
spsc::fifo_view<int, 0> q;
q.attach(storage.data(), storage.size());
```

### `attach(array)`

```cpp
std::array<int, 64> storage{};
spsc::fifo_view<int, 64> q;
q.attach(storage);
```

### `attach(buffer, state)`

```cpp
spsc::fifo_view<int, 0>::state_t st{savedHead, savedTail};
q.attach(storage.data(), storage.size(), st);
```

### `adopt()`

```cpp
(void)q.adopt(storage.data(), storage.size(), savedHead, savedTail);
```

### `state()`

```cpp
const auto st = q.state();
save_state(st.head, st.tail);
```

### `reset()`

```cpp
q.reset();
```

### `detach()`

```cpp
q.detach();
```

### `is_valid()`

```cpp
if (!q.is_valid()) {
    return;
}
```

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

### `push()`, `try_push()`

```cpp
q.push(1);
(void)q.try_push(2);
```

### `emplace()`, `try_emplace()`

```cpp
q.emplace(arg1, arg2);
(void)q.try_emplace(arg1, arg2);
```

### `claim()`, `try_claim()`, `publish()`

```cpp
if (auto* slot = q.try_claim()) {
    *slot = make_value();
    q.publish();
}
```

### `front()`, `try_front()`, `pop()`

```cpp
if (auto* front = q.try_front()) {
    consume(*front);
    q.pop();
}
```

### `make_snapshot()`, `consume()`

```cpp
auto snap = q.make_snapshot();
for (const auto& item : snap) {
    process(item);
}
q.consume(snap);
```

### `claim_write(unsafe, ...)`, `claim_read(unsafe, ...)`

Raw bulk regions always require the explicit `spsc::unsafe` tag.

```cpp
auto w = q.claim_write(spsc::unsafe, 4);
auto r = q.claim_read(spsc::unsafe, 4);
(void)w;
(void)r;
```

### `scoped_write()`, `scoped_read()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.ref() = make_value();
}

if (auto guard = q.scoped_read()) {
    process(*guard);
}
```

### `clear()`

```cpp
q.clear();
```
