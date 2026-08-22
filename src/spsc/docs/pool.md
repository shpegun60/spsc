# `spsc::pool`

`pool<Capacity, Policy, Alloc>` is an owning SPSC pool of fixed-size raw buffers.

The ring stores `void*` slots. Each slot points to a byte buffer of `buffer_size()` bytes.

## When To Use It

Choose `pool` when:

- the payload is naturally raw bytes
- every slot has the same size
- producers and consumers exchange buffer ownership rather than typed values
- you want a good fit for packet, DMA, or block-buffer workflows

## Static And Dynamic Variants

### Static depth

```cpp
spsc::pool<64> q{256}; // 64 slots, each 256 bytes
```

The static template argument fixes the pointer-ring geometry. The configured
raw buffers are still allocated when the pool is initialized.

### Dynamic depth

```cpp
spsc::pool<0> q{64, 256};
// or:
// spsc::pool<0> q;
// q.resize(64, 256);
```

## Producer Example

```cpp
spsc::pool<64> q{256};

if (void* slot = q.try_claim()) {
    auto* bytes = static_cast<std::byte*>(slot);
    fill_payload(bytes, q.buffer_size());
    q.publish();
}
```

There are also `push(data, size)` / `try_push(data, size)` convenience helpers for memcpy-based write flows.

## Consumer Example

```cpp
if (void* slot = q.try_front()) {
    process_bytes(static_cast<std::byte*>(slot), q.buffer_size());
    q.pop();
}
```

## More Example Patterns

### Memcpy-Style Producer

```cpp
std::byte packet[96]{};
fill_packet(packet, sizeof(packet));

if (!q.try_push(packet, sizeof(packet))) {
    ++droppedPackets;
}
```

### Typed Header Plus Raw Payload

```cpp
struct Header {
    std::uint16_t type;
    std::uint16_t payloadSize;
};

if (auto* slot = q.try_claim()) {
    auto* hdr = ::new (static_cast<void*>(slot)) Header{7u, 48u};

    auto* payload = reinterpret_cast<std::byte*>(hdr + 1);
    fill_payload(payload, hdr->payloadSize);
    q.publish();
}
```

### Batch Buffer Publishing

```cpp
auto guard = q.scoped_write(4);

while (guard.remaining() != 0u) {
    std::byte temp[64]{};
    build_packet(temp, sizeof(temp));
    guard.write_next(temp, sizeof(temp));
}

guard.commit();
```

## API Groups

### Producer

- `claim`, `try_claim`
- `publish`, `try_publish`
- `push(const U&)`
- `push(data, size)` / `try_push(data, size)`

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- `consume`, `consume_all`
- `make_snapshot`

### Bulk / Slot Handling

- slot-region read/write helpers
- pointer-based slot access
- scoped guards for bulk workflows

### Management

- `resize(depth, buffer_size)`
- `resize(buffer_size)` for static depth
- `destroy`, `clear`, `swap`

## Why `pool` Exists

`pool` is often simpler than `fifo<std::array<std::byte, N>>` when:

- the logical payload length varies
- you already think in terms of buffers and slots
- fixed byte capacity per slot is enough

## Cache-Aligned Usage

With `CacheAligned` policies, the default path can help the raw-slot layout:

```cpp
using Q = spsc::pool<0, spsc::policy::CA<>>;
Q q{64, 100};
```

On the cache-aligned default path:

- metadata uses cache-aware layout
- default allocators can become cache-line aligned automatically
- raw slot size can be rounded up internally to the policy alignment

So a requested `100` bytes can become `128` bytes under a 32-byte alignment policy.

## DMA-Friendly Pattern

```cpp
using Q = spsc::pool<0, spsc::policy::CA<>>;
Q q{32, 128};

static void* activeRxSlot = nullptr;

void arm_rx_dma()
{
    if (!activeRxSlot) {
        activeRxSlot = q.try_claim();
    }

    if (activeRxSlot) {
        start_dma_into(activeRxSlot, 128);
    }
}

void on_rx_dma_complete(std::size_t bytesWritten)
{
    if (!activeRxSlot) {
        return;
    }

    platform_invalidate_dma_rx_buffer(activeRxSlot, bytesWritten);
    q.publish();
    activeRxSlot = nullptr;
}
```

Publish only after asynchronous DMA is complete. The container gives you
aligned slots on the default path, but hardware cache maintenance still belongs
outside the container.

## Good Fits

- raw packets
- ADC/UART/SPI frame buffers
- fixed-size binary blocks
- DMA staging buffers

## Less Good Fits

- typed objects with constructors and destructors
- variable-size ownership where slots must resize independently

## Method Reference

### Construction

- `pool(depth, buffer_size)` creates a dynamic pool
- `pool(buffer_size)` creates a static-depth pool and allocates its buffers
- `resize(depth, buffer_size)` exists on dynamic `pool`
- `resize(buffer_size)` exists on static-depth `pool`
- `destroy()` frees the slot array and backing buffers
- `swap(other)` exchanges storage and queue state

### State And Introspection

- `is_valid()`
- `capacity()`, `size()`, `empty()`, `full()`, `free()`
- `buffer_size()` gives the physical slot byte size
- `can_write(n)`, `can_read(n)`, `write_size()`, `read_size()`
- `data()` returns the ring of `void*` slot pointers
- `operator[](i)` indexes logical slot order
- `span()` exposes the current front buffer as bytes when spans are enabled

### Producer Methods

- `claim()` / `try_claim()` return the writable slot pointer
- `claim_as<U>()` returns `U*` only if size and alignment fit; in C++17 it
  does not start `U` lifetime by itself
- `publish()` / `try_publish()` commit one slot
- `publish(unsafe, n)` / `try_publish(unsafe, n)` commit several prepared slots
- `push(const U&)` / `try_push(const U&)` memcpy a trivially-copyable object into the slot;
  unchecked `push` requires `sizeof(U) <= buffer_size()`, while `try_push`
  checks this at runtime and returns `false`
- `push(data, size)` / `try_push(data, size)` copy raw bytes, truncating to `buffer_size()`
- `try_write(v)` is a convenience alias for typed `try_push`

Typed write example:

```cpp
struct Header {
    std::uint32_t magic;
    std::uint16_t len;
};

if (auto* hdr = q.claim_as<Header>()) {
    ::new (static_cast<void*>(hdr)) Header{0x12345678u, 42};
    q.publish();
}
```

### Consumer Methods

- `front()` / `try_front()` return the current slot pointer
- `front_as<U>()` returns `U*` when the slot is large enough and aligned; only
  dereference it when the producer started `U` lifetime in that slot
- `try_peek(out)` memcpy-reads the current slot into a trivially-copyable object
- `pop()` / `try_pop()` consume one slot
- `pop(n)` / `try_pop(n)` consume several slots

Safe peek example:

```cpp
Header hdr{};
if (q.try_peek(hdr)) {
    handle_header(hdr);
}
```

### Snapshots, Bulk, And Guards

- `make_snapshot()`, `consume(snapshot)`, `try_consume(snapshot)`, `consume_all()`
- `claim_write(unsafe, max_count)` and `claim_read(unsafe, max_count)`
- `bulk_write_guard`, `bulk_read_guard`
- `write_guard`, `read_guard`
- `scoped_write()`, `scoped_write(max_count)`, `scoped_read()`, `scoped_read(max_count)`

These are especially useful when you process several buffers per scheduler wakeup.

Guard example:

```cpp
if (auto guard = q.scoped_write()) {
    if (auto* hdr = guard.as<Header>()) {
        ::new (static_cast<void*>(hdr)) Header{0x12345678u, 42};
    }
    // publish() happens on scope exit after as<U>() arms it
}

if (auto guard = q.scoped_read()) {
    if (auto* hdr = guard.as<Header>()) {
        // Only valid if the producer started Header lifetime in the slot.
        handle_header(*hdr);
    }
    // pop() happens on scope exit
}
```

### Management Notes

`CacheAligned` policies can influence default allocation and raw slot rounding.  
That is helpful for aligned raw-buffer use, but cache maintenance still remains outside the container.

## Snippet Catalog

### `buffer_size()`

```cpp
const auto bytesPerSlot = q.buffer_size();
```

### `claim()`, `try_claim()`

```cpp
if (void* slot = q.try_claim()) {
    fill_payload(static_cast<std::byte*>(slot), q.buffer_size());
    q.publish();
}
```

### `claim_as<U>()`

```cpp
if (auto* hdr = q.claim_as<Header>()) {
    ::new (static_cast<void*>(hdr)) Header{0x12345678u, 64u};
    q.publish();
}
```

### `push(const U&)`

Precondition: `q` is valid and not full, and `sizeof(U) <= q.buffer_size()`.
This unchecked hot path enforces the size condition only through `SPSC_ASSERT`;
use `try_push` when the payload type may not fit.

```cpp
Header hdr{0x12345678u, 32u};
q.push(hdr);
```

### `try_push(const U&)`

```cpp
if (!q.try_push(Header{0x12345678u, 32u})) {
    ++dropCount;
}
```

### `push(data, size)`

```cpp
std::byte bytes[48]{};
q.push(bytes, sizeof(bytes));
```

### `try_push(data, size)`

```cpp
std::byte bytes[48]{};
(void)q.try_push(bytes, sizeof(bytes));
```

### `try_write(v)`

```cpp
(void)q.try_write(Header{0x12345678u, 32u});
```

### `publish()`, `publish(unsafe, n)`

```cpp
auto guard = q.scoped_write(2);
std::byte a[16]{};
std::byte b[16]{};
guard.write_next(a, sizeof(a));
guard.write_next(b, sizeof(b));
guard.commit();
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
if (q.can_read(4)) {
    q.pop(4);
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

### `claim_write()`, `claim_read()`

```cpp
auto w = q.claim_write(spsc::unsafe, 4);
auto r = q.claim_read(spsc::unsafe, 4);
(void)w;
(void)r;
```

### `scoped_write()`, `scoped_read()`

```cpp
if (auto guard = q.scoped_write()) {
    if (auto* hdr = guard.as<Header>()) {
        ::new (static_cast<void*>(hdr)) Header{0x12345678u, 16u};
    }
}

if (auto guard = q.scoped_read()) {
    if (auto* hdr = guard.as<Header>()) {
        // Only valid if the producer started Header lifetime in the slot.
        handle_header(*hdr);
    }
}
```

### `resize()`

```cpp
spsc::pool<0> dyn;
dyn.resize(32, 128);
```

Every non-zero resize axis is grow-only. For example, growing depth with
`resize(64, 16)` on an existing `32 x 128` dynamic pool produces at least
`64 x 128`; it does not truncate existing 128-byte payloads to 16 bytes.
Passing zero for either axis remains the explicit destroy operation.

### `clear()`, `destroy()`

```cpp
q.clear();
q.destroy();
```

### `swap()`

```cpp
spsc::pool<0> a{16, 64};
spsc::pool<0> b{16, 128};
swap(a, b);
```
