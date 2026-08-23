# `spsc::pool_view`

`pool_view<Capacity, Policy>` is the non-owning version of `pool`.

It stores and orders `void*` slots, but the actual slot array and the backing buffers are supplied by you.

## When To Use It

Choose `pool_view` when:

- the buffers already exist
- DMA or a low-level subsystem owns the payload memory
- the slot array must live in a specific memory region
- you want to restore head/tail state over externally owned raw buffers

## Static-Depth Example

```cpp
#include "pool_view.hpp"
#include <array>

constexpr std::size_t kDepth = 16;
constexpr std::size_t kBytes = 128;

std::array<void*, kDepth> slots{};
std::array<std::array<std::byte, kBytes>, kDepth> buffers{};

for (std::size_t i = 0; i < kDepth; ++i) {
    slots[i] = buffers[i].data();
}

spsc::pool_view<kDepth> q{slots, kBytes};
```

## Dynamic-Depth Example

```cpp
std::vector<void*> slots(depth);
std::vector<std::vector<std::byte>> buffers(depth, std::vector<std::byte>(bufferSize));

for (std::size_t i = 0; i < depth; ++i) {
    slots[i] = buffers[i].data();
}

spsc::pool_view<0> q;
q.attach(slots.data(), slots.size(), bufferSize);
```

## State Restore

`pool_view` supports attach/adopt with saved state.

```cpp
spsc::pool_view<0>::state_t st{savedHead, savedTail};

q.attach(slots.data(), slots.size(), bufferSize, st);
```

This is useful when the data memory survives longer than the process or object
that wraps it, but `state_t` is only `{head, tail}`. It does not serialize the
effective capacity/mask, `buffer_size`, slot ordering, or backing-buffer
layout. Save and validate all of that metadata separately. `adopt()` can reject
locally impossible indices, but it cannot detect a compatible-looking restore
into the wrong geometry or slot table.

`state_t` uses the library's platform-sized `size_type`. It is an index
snapshot for a matching build, not a portable disk, shared-protocol, or wire
format. Cross-build persistence must encode fixed-width fields explicitly,
choose a byte order and format version, range-check on decode, and still
validate capacity, slot width, slot ordering, and backing layout before
`adopt()`.

## More Example Patterns

### External DMA Buffers In Static RAM

```cpp
constexpr std::size_t kDepth = 8;
constexpr std::size_t kBytes = 128;

alignas(32) static std::byte dmaBuffers[kDepth][kBytes];
static void* slots[kDepth];

for (std::size_t i = 0; i < kDepth; ++i) {
    slots[i] = dmaBuffers[i];
}

spsc::pool_view<kDepth, spsc::policy::CA<>> q{slots, kBytes};
```

`pool_view` does not allocate, round, or validate the external payload buffers.
`CacheAligned` affects only the view's metadata; it cannot realign a slot
pointer or resize a caller-owned buffer.
For STM32F7/STM32H7-style DMA, make each slot start cache-line aligned and
make the maintained byte span cover whole cache lines.

### Typed Overlay On External Storage

```cpp
struct Header {
    std::uint32_t magic;
    std::uint16_t len;
};

if (auto* hdr = q.claim_as<Header>()) {
    ::new (static_cast<void*>(hdr)) Header{0x12345678u, 32u};
    q.publish();
}
```

`claim_as<U>()` only checks size and alignment. In C++17 it does not start
`U` lifetime by itself; use placement-new before dereferencing as `U`.

### Reset Indices Without Losing External Buffers

```cpp
q.reset();
// The external slot table and buffers stay owned by the caller.
```

## API Shape

The API intentionally mirrors `pool`:

- `claim`, `try_claim`, `publish`
- `front`, `try_front`, `pop`
- `push(data, size)` for memcpy-style writes
- snapshots
- bulk slot-region access
- `attach` / `adopt` / `detach`

## API Groups

### Producer

- `claim`, `try_claim`
- `publish`, `try_publish`
- `push(data, size)` / `try_push(data, size)`

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- `consume`, `consume_all`
- `make_snapshot`

### External-Storage Management

- constructors from slot arrays
- `attach`
- `adopt`
- `detach`
- index-only `state_t` snapshot

## Correct Usage Pattern

The container does not allocate and does not free:

- you allocate buffers
- you build the slot pointer array
- you attach that array to `pool_view`
- `pool_view` only manages the SPSC ordering

## Good Fits

- externally allocated DMA buffers
- shared memory layouts
- startup-time static memory maps
- restart/recovery flows where payload memory already exists

## Less Good Fits

- cases where the container itself should allocate and own memory
- cases where you do not want to manage the external slot array lifetime

## Method Reference

### Construction And External Ownership

- `pool_view()` creates a detached view.
- static constructors accept slot arrays plus `buffer_size`.
- dynamic constructors accept `(pointer* slots, depth, buffer_size)`.
- `attach(...)` binds the slot array and resets queue state.
- `adopt(...)` binds the slot array and restores supplied head/tail indices
  after validating them against the supplied geometry.
- `reset()` clears head/tail while keeping the current slot array attached.
- `detach()` clears the view.
- `state()` returns a serializable `{head, tail}` pair only; geometry,
  `buffer_size`, slot ordering, and backing-layout metadata remain external.

Remember that the view never allocates buffers and never frees them.

### State And Introspection

- `is_valid()`
- `capacity()`, `size()`, `empty()`, `full()`, `free()`
- `buffer_size()`
- `can_write(n)`, `can_read(n)`, `write_size()`, `read_size()`
- `data()` returns the external slot pointer ring
- `operator[](i)` indexes logical slot order
- `span()` exposes the current front slot as bytes when spans are enabled

### Producer Methods

- `claim()` / `try_claim()`
- `claim_as<U>()`
- `publish()` / `try_publish()`
- `publish(unsafe, n)` / `try_publish(unsafe, n)`
- `push(const U&)` / `try_push(const U&)` for trivially-copyable payloads
- `push(data, size)` / `try_push(data, size)` for raw byte copies
- `try_write(v)` convenience wrapper

### Consumer Methods

- `front()` / `try_front()`
- `front_as<U>()`
- `try_peek(out)`
- `pop()` / `try_pop()`
- `pop(n)` / `try_pop(n)`

### Snapshots, Bulk, And Guards

- `make_snapshot()`, `consume(snapshot)`, `try_consume(snapshot)`, `consume_all()`
- `claim_write(unsafe, max_count)`, `claim_read(unsafe, max_count)`
- `bulk_write_guard`, `bulk_read_guard`
- `write_guard`, `read_guard`
- `scoped_write()`, `scoped_write(max_count)`, `scoped_read()`, `scoped_read(max_count)`

### Restore Pattern

```cpp
struct saved_pool_state {
    spsc::pool_view<0>::state_t indices;
    reg effective_capacity;
    reg buffer_size;
    std::uint32_t layout_version;
};

spsc::pool_view<0> q{slots.data(), depth, bufferSize};
if (!q.is_valid() ||
    q.capacity() != saved.effective_capacity ||
    q.buffer_size() != saved.buffer_size ||
    !slot_layout_matches(saved.layout_version)) {
    q.detach();
    return false;
}

return q.adopt(slots.data(), depth, bufferSize,
               saved.indices.head, saved.indices.tail);
```

The application-defined layout check must cover slot order and the compatibility
of every referenced backing buffer.

## Snippet Catalog

### `attach(slots, depth, buffer_size)`

```cpp
spsc::pool_view<0> q;
q.attach(slots.data(), slots.size(), bufferSize);
```

### `attach(slots, buffer_size, state)`

```cpp
spsc::pool_view<0>::state_t st{savedHead, savedTail};
q.attach(slots.data(), slots.size(), bufferSize, st);
```

### `adopt()`

```cpp
(void)q.adopt(slots.data(), slots.size(), bufferSize, savedHead, savedTail);
```

### `state()`

```cpp
const auto st = q.state();
persist(st.head, st.tail);
```

### `reset()`

```cpp
q.reset();
```

### `is_valid()`, `capacity()`, `size()`, `free()`

```cpp
if (!q.is_valid()) {
    return;
}

const auto cap = q.capacity();
const auto used = q.size();
const auto freeSlots = q.free();
```

### `buffer_size()`, `write_size()`, `read_size()`

```cpp
const auto bytes = q.buffer_size();
const auto contiguousWritable = q.write_size();
const auto contiguousReadable = q.read_size();
```

### `empty()`, `full()`, `can_write()`, `can_read()`

```cpp
if (q.empty()) {
    return;
}

if (q.full()) {
    ++fullCount;
}

(void)q.can_write(2);
(void)q.can_read(2);
```

### `data()`, `operator[]`

```cpp
void* const* slotArray = q.data();
(void)slotArray;

if (!q.empty()) {
    void* first = q[0];
    (void)first;
}
```

### `detach()`

```cpp
q.detach();
```

### `claim()`, `try_claim()`, `publish()`

```cpp
if (void* slot = q.try_claim()) {
    fill_payload(static_cast<std::byte*>(slot), q.buffer_size());
    q.publish();
}
```

### `claim_as<U>()`

```cpp
if (auto* hdr = q.claim_as<Header>()) {
    ::new (static_cast<void*>(hdr)) Header{0x12345678u, 24u};
    q.publish();
}
```

### `push(data, size)`, `try_push(data, size)`

```cpp
std::byte payload[32]{};
q.push(payload, sizeof(payload));
(void)q.try_push(payload, sizeof(payload));
```

Both overloads clamp the copy to `buffer_size()`: a `true` result means
"one slot was published", not "the whole input fit". Use the typed
`try_push(const U&)` when an oversized input must be rejected instead of
truncated.

### `try_publish()`, `publish(unsafe, n)`

```cpp
if (void* slot = q.try_claim()) {
    fill_payload(static_cast<std::byte*>(slot), q.buffer_size());
    (void)q.try_publish();
}
```

```cpp
auto guard = q.scoped_write(2);
std::byte a[16]{};
std::byte b[16]{};
guard.write_next(a, sizeof(a));
guard.write_next(b, sizeof(b));
guard.commit();
```

### `try_write(v)`

```cpp
(void)q.try_write(Header{0x12345678u, 20u});
```

### `front()`, `try_front()`

```cpp
if (void* slot = q.try_front()) {
    inspect(static_cast<std::byte*>(slot), q.buffer_size());
}
```

### `front_as<U>()`

```cpp
if (auto* hdr = q.front_as<Header>()) {
    // Only valid if the producer started Header lifetime in the slot.
    handle_header(*hdr);
}
```

### `try_peek(out)`

```cpp
Header hdr{};
if (q.try_peek(hdr)) {
    handle_header(hdr);
}
```

### `pop()`, `try_pop()`, `pop(n)`

```cpp
(void)q.try_pop();
if (q.can_read(2)) {
    q.pop(2);
}
```

### `make_snapshot()`, `consume()`

```cpp
auto snap = q.make_snapshot();
for (void* slot : snap) {
    inspect(static_cast<std::byte*>(slot), q.buffer_size());
}
q.consume(snap);
```

### `try_consume()`, `consume_all()`

```cpp
auto snap = q.make_snapshot();
(void)q.try_consume(snap);
q.consume_all();
```

### `claim_write(unsafe, ...)`, `claim_read(unsafe, ...)`

Raw bulk regions always require the explicit `spsc::unsafe` tag.

```cpp
auto w = q.claim_write(spsc::unsafe, 4);
auto r = q.claim_read(spsc::unsafe, 4);
(void)w;
(void)r;
```

### `clear()`

```cpp
q.clear();
```

### `scoped_write()`, `scoped_read()`

```cpp
if (auto guard = q.scoped_write()) {
    if (auto* hdr = guard.as<Header>()) {
        ::new (static_cast<void*>(hdr)) Header{0x12345678u, 12u};
    }
}

if (auto guard = q.scoped_read()) {
    if (auto* hdr = guard.as<Header>()) {
        // Only valid if the producer started Header lifetime in the slot.
        handle_header(*hdr);
    }
}
```

### `scoped_write(max_count)`, `scoped_read(max_count)`

```cpp
auto w = q.scoped_write(4);
auto r = q.scoped_read(4);
(void)w;
(void)r;
```
