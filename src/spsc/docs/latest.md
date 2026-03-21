# `spsc::latest`

`latest` is the newest-value buffer in the library.

This is **not** a FIFO.

The producer publishes states over time, and the consumer sees only the newest committed slot.

## Mental Model

Think of `latest` as:

- "the newest telemetry frame"
- "the newest controller state"
- "the newest snapshot that the consumer should observe"

Do **not** think of it as a queue of history.

## Variants

There are three main forms:

- `latest<void, 0, Policy, Alloc>`: dynamic raw-byte slots
- `latest<T, 0, Policy, Alloc>`: dynamic typed slots
- `latest<T, Depth, Policy, Alloc>`: static typed slots

## Typed Static Example

```cpp
struct Telemetry {
    float temperature;
    float voltage;
};

spsc::latest<Telemetry, 8> latestState;

Telemetry& slot = latestState.claim();
slot = Telemetry{42.0f, 12.2f};
latestState.publish();

const Telemetry& view = latestState.front();
latestState.pop();
```

## Typed Dynamic Example

```cpp
spsc::latest<Telemetry, 0> latestState;
latestState.resize(8);
```

## Raw Dynamic Example

```cpp
using LatestBytes = spsc::latest<void, 0, spsc::policy::CA<>>;

LatestBytes q;
q.resize(8, 128);

if (void* slot = q.try_claim()) {
    fill_packet(slot, q.bytes_per_slot());
    q.publish();
}
```

## More Example Patterns

### Typed Producer With `try_emplace`

```cpp
if (auto* slot = latestState.try_emplace(42.0f, 12.2f, 1.8f)) {
    use_pointer_immediately(slot);
}
```

### Raw Header-Only Publish

```cpp
struct Header {
    std::uint16_t type;
    std::uint16_t size;
};

q.try_push(Header{3u, 64u});
```

### Explicit Stale-Data Drop

```cpp
latestState.consume_all();
// After a mode switch, ignore any previously published states.
```

## Consumer Semantics

`front()` returns the newest published slot, not the oldest.

`pop()` consumes the snapshot that was last observed by `front()` / `try_front()`.

That means the consumer model is:

1. read current newest slot
2. optionally process it
3. call `pop()` to advance to the snapshot you just observed

## Coalescing

`latest` also offers `coalescing_publish()`.

Use it when:

- the producer updates very frequently
- the consumer only cares about "fresh enough"
- you want to keep slack near full instead of publishing every intermediate state

## API Groups

### Producer

- `claim`, `try_claim`
- `publish`, `try_publish`
- `coalescing_publish`
- typed variants also add `push`, `try_push`, `emplace`, `try_emplace`
- raw `latest<void, 0>` adds `push(U)` / `try_push(U)` for trivially-copyable objects that fit into one slot

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`

### Management

- `resize` / `init` on dynamic variants
- `reserve` on dynamic variants
- `clear`, `swap`, move support

## Good Fits

- latest controller state
- latest UI model or telemetry sample
- newest decoded frame
- newest sensor fusion result

## Less Good Fits

- audit trails
- ordered event history
- every-message-must-be-delivered semantics

## Method Reference

### Construction

- `latest<void, 0>(depth, bytes_per_slot)` for dynamic raw slots
- `latest<T, 0>(depth)` for dynamic typed slots
- `latest<T, Depth>` for static typed slots
- `init(...)` is a convenience alias for `resize(...)`
- `reserve(...)` grows depth or slot bytes on dynamic variants
- `swap(other)` exchanges storage and state

### State And Introspection

- `is_valid()` / `valid()`
- `depth()`
- `capacity()` as a synonym for depth-style introspection
- `size()`, `empty()`, `full()`, `free()`
- `can_write(n)`, `can_read(n)`, `write_size()`, `read_size()`
- `buffer_size()` / `bytes_per_slot()` on raw dynamic `latest<void,0>`
- `data()` on typed variants

### Producer Methods

- `claim()` / `try_claim()`
- `publish()` / `try_publish()`
- `coalescing_publish()` for newest-state flows that tolerate collapsed intermediate updates
- typed variants also provide `push`, `try_push`, `emplace`, `try_emplace`
- raw variant provides `push(U)` / `try_push(U)` for trivially-copyable objects that fit into the slot

The essential rule is:

1. write the next slot
2. publish it
3. consumer sees only the newest committed slot

Typed helper example:

```cpp
latestState.emplace(temperature, voltage, current);
```

Raw helper example:

```cpp
struct PacketHeader {
    std::uint16_t type;
    std::uint16_t size;
};

q.try_push(PacketHeader{7u, 96u});
```

`coalescing_publish()` is a policy-free helper for "newest state wins" traffic:

```cpp
if (Telemetry* slot = latestState.try_claim()) {
    *slot = read_telemetry();
    latestState.coalescing_publish();
}
```

### Consumer Methods

- `front()` / `try_front()` return the newest committed slot
- `pop()` / `try_pop()` consume the sticky snapshot previously observed by `front` / `try_front`
- `consume_all()` drops all currently readable snapshots
- `clear()` resets the structure

This is the key difference from FIFO containers: `front()` is newest, not oldest.

There are intentionally no snapshot iterators here. `latest` is a sticky newest-value view, not a range container.

### Management

- raw dynamic: `init(depth, bytes_per_slot)`, `resize(depth, bytes_per_slot)`, `reserve(depth, bytes_per_slot)`
- typed dynamic: `init(depth)`, `resize(depth)`, `reserve(depth)`
- typed static: no resize path
- `destroy()`
- `clear()`
- `swap(...)`

These are non-concurrent management operations.

## Snippet Catalog

### `depth()`, `capacity()`, `size()`, `free()`

```cpp
const auto depth = latestState.depth();
const auto used = latestState.size();
const auto freeSlots = latestState.free();
```

### `valid()`, `is_valid()`

```cpp
if (!latestState.valid()) {
    return;
}
```

### `empty()`, `full()`, `can_write()`, `can_read()`

```cpp
if (latestState.empty()) {
    return;
}

if (latestState.full()) {
    ++fullCount;
}

(void)latestState.can_write(1);
(void)latestState.can_read(1);
```

### `write_size()`, `read_size()`

```cpp
const auto writable = latestState.write_size();
const auto readable = latestState.read_size();
```

### `claim()`, `try_claim()`

```cpp
if (auto* slot = latestState.try_claim()) {
    *slot = read_telemetry();
    latestState.publish();
}
```

### `publish()`, `try_publish()`

```cpp
if (auto* slot = latestState.try_claim()) {
    *slot = read_telemetry();
    (void)latestState.try_publish();
}
```

### `coalescing_publish()`

```cpp
if (auto* slot = latestState.try_claim()) {
    *slot = read_telemetry();
    latestState.coalescing_publish();
}
```

### `push()`, `try_push()`

```cpp
latestState.push(Telemetry{});
(void)latestState.try_push(Telemetry{});
```

### `emplace()`, `try_emplace()`

```cpp
latestState.emplace(v, i, t);
(void)latestState.try_emplace(v, i, t);
```

### raw `push(U)`

```cpp
spsc::latest<void, 0> bytesLatest;
bytesLatest.resize(4, 64);
bytesLatest.try_push(Header{7u, 32u});
```

### `front()`, `try_front()`

```cpp
if (const auto* view = latestState.try_front()) {
    render(*view);
}
```

```cpp
if (!latestState.empty()) {
    render(latestState.front());
}
```

### `pop()`, `try_pop()`

```cpp
if (latestState.try_front()) {
    latestState.pop();
}

(void)latestState.try_pop();
```

### `consume_all()`

```cpp
latestState.consume_all();
```

### `clear()`

```cpp
latestState.clear();
```

### raw `buffer_size()`, `bytes_per_slot()`

```cpp
const auto bytes = bytesLatest.buffer_size();
const auto slotBytes = bytesLatest.bytes_per_slot();
```

### `data()` on typed variants

```cpp
auto* backing = latestState.data();
inspect_backing_slots(backing, latestState.depth());
```

### `init()`, `resize()`, `reserve()`

```cpp
spsc::latest<Telemetry, 0> dynTyped;
dynTyped.init(8);
dynTyped.reserve(16);
dynTyped.resize(32);
```

```cpp
spsc::latest<void, 0> dynRaw;
dynRaw.init(8, 64);
dynRaw.reserve(16, 128);
dynRaw.resize(32, 128);
```

### `destroy()`

```cpp
dynTyped.destroy();
dynRaw.destroy();
```

### `swap()`

```cpp
spsc::latest<Telemetry, 0> a{8};
spsc::latest<Telemetry, 0> b{16};
swap(a, b);
```
