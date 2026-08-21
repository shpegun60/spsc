# `spsc::typed_pool`

`typed_pool<T, Capacity, Policy, Alloc>` is an independent owning typed-slot
container, not a `queue` alias.

The ring stores `T*` pointers. Each pointer refers to storage for exactly one `T`.

It combines:

- pool-like slot semantics
- typed objects
- optional zero-copy construction

Its static capacity fixes the pointer-ring geometry, but each owned typed slot
is allocated separately. It therefore has pool-style stable slot identity and
queue-style destruction on consume, without implying allocation-free storage.

## When To Use It

Choose `typed_pool` when:

- each slot should hold one `T`
- pointer-like slot identity is useful
- destruction on consume matters
- you want pool semantics instead of queue value semantics

## Basic Example

```cpp
struct Frame {
    std::array<std::byte, 256> payload;
};

spsc::typed_pool<Frame, 32> q;

if (Frame* slot = q.try_claim()) {
    ::new (static_cast<void*>(slot)) Frame{};
    fill_frame(*slot);
    q.publish();
}

if (Frame* frame = q.try_front()) {
    consume_frame(*frame);
    q.pop();
}
```

## `emplace` / `push`

`typed_pool` also supports typed producer helpers:

```cpp
q.emplace(args...);
q.push(frame);
```

These are convenient when you already have a value and do not need manual slot filling.

## Manual Slot Construction

When you want explicit control, use `claim()` and construct into the slot.

```cpp
if (Frame* slot = q.try_claim()) {
    ::new (static_cast<void*>(slot)) Frame{};
    initialize_frame(*slot);
    q.publish();
}
```

The slot memory is persistent across the pool lifetime; the pool rewires pointer order, not object storage layout.

## Dynamic Variant

```cpp
spsc::typed_pool<Frame, 0> q;
q.resize(64);
```

Dynamic `typed_pool` is primarily grow-oriented. On grow, the ring of pointers is migrated in logical order and extra slots are allocated.

## More Example Patterns

### Value-Based Producer

```cpp
Frame frame{};
prepare_frame(frame);
q.try_push(frame);
```

### Batch Construction With A Guard

```cpp
auto guard = q.scoped_write(4);

while (guard.remaining() != 0u) {
    guard.emplace_next(make_frame());
}

guard.commit();
```

### Snapshot Of Stable Slot Pointers

```cpp
auto snap = q.make_snapshot();

for (Frame* frame : snap) {
    analyze(*frame);
}

q.consume(snap);
```

## API Groups

### Producer

- `emplace`, `try_emplace`
- `push`, `try_push`
- `claim`, `try_claim`
- `publish`, `try_publish`

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- snapshots
- bulk read regions

### Management

- `resize`
- `clear`
- `swap`, move, deep copy

## Good Fits

- fixed-size typed frames
- object pools in producer/consumer pipelines
- large typed objects where pointer-style slot handling is natural

## Less Good Fits

- raw byte buffers, where `pool` is simpler
- newest-value semantics, where `latest` is the right model

## Method Reference

### Construction

- `typed_pool()` creates an empty or detached pool depending on static/dynamic form
- `typed_pool(depth)` exists on dynamic variants
- `resize(depth)` grows the dynamic ring of slot pointers
- `resize()` on static variants allocates static storage if needed
- deep copy is available when `T` is copy-constructible; in
  `SPSC_ENABLE_EXCEPTIONS=0` mode that construction must also be `noexcept`
- move is supported
- `swap(other)` exchanges pool state
- `destroy()` frees slot storage

### State And Introspection

- `is_valid()`
- `capacity()`, `size()`, `empty()`, `full()`, `free()`
- `can_write(n)`, `can_read(n)`
- `buffer_size()` is a compile-time convenience equal to `sizeof(T)`
- `data()` returns the logical ring of `T*` slot pointers
- `operator[](i)` indexes logical queue order
- iterators and snapshots expose pointer values

### Producer Methods

- `emplace(args...)` / `try_emplace(args...)`
- `push(const T&)`, `push(T&&)`
- `try_push(...)`
- `claim()` / `try_claim()` for manual construction into slot storage
- `publish()` / `try_publish()`
- `publish(unsafe, n)` / `try_publish(unsafe, n)`

Manual path:

```cpp
if (auto* slot = q.try_claim()) {
    ::new (static_cast<void*>(slot)) Frame{};
    prepare(*slot);
    q.publish();
}
```

### Consumer Methods

- `front()` / `try_front()`
- `pop()` / `try_pop()` destroy the object and consume the slot
- `pop(n)` / `try_pop(n)` destroy several objects
- `consume(snapshot)`, `try_consume(snapshot)`, `consume_all()`

### Bulk And Guards

- `claim_read(unsafe, max_count)`
- bulk slot regions
- `bulk_write_guard`, `bulk_read_guard`
- `write_guard`, `read_guard`
- `scoped_write()`, `scoped_write(max_count)`, `scoped_read()`, `scoped_read(max_count)`

These helpers are useful when several objects are produced or consumed per cycle.

Example:

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(args...);
    // publish() happens on scope exit
}

if (auto guard = q.scoped_read()) {
    use(*guard);
    // pop() happens on scope exit
}
```

### Management

- `clear()` destroys all currently live objects
- `resize(depth)` grows dynamic pools
- `swap`, move, copy, destroy

Use these only when producer and consumer are stopped.

## Snippet Catalog

### `buffer_size()`

```cpp
static_assert(spsc::typed_pool<Frame, 8>::buffer_size() == sizeof(Frame));
```

### `capacity()`, `size()`, `free()`

```cpp
const auto cap = q.capacity();
const auto used = q.size();
const auto freeSlots = q.free();
```

### `is_valid()`, `can_write()`, `can_read()`

```cpp
if (!q.is_valid()) {
    return;
}

(void)q.can_write(2);
(void)q.can_read(2);
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

### `data()`

```cpp
Frame* const* slots = q.data();
inspect_slots(slots, q.capacity());
```

### `begin()`, `end()`

```cpp
for (Frame* frame : q) {
    inspect(*frame);
}
```

### `operator[]`

```cpp
if (!q.empty()) {
    Frame* first = q[0];
    inspect(*first);
}
```

### `push()`, `try_push()`

```cpp
Frame frame{};
prepare_frame(frame);
q.push(frame);
(void)q.try_push(frame);
```

### `emplace()`, `try_emplace()`

```cpp
q.emplace(args...);
(void)q.try_emplace(args...);
```

### `claim()`, `try_claim()`

```cpp
if (Frame* slot = q.try_claim()) {
    ::new (static_cast<void*>(slot)) Frame{};
    prepare(*slot);
    q.publish();
}
```

### `publish()`, `publish(unsafe, n)`

```cpp
auto guard = q.scoped_write(2);
guard.emplace_next(make_frame());
guard.emplace_next(make_frame());
guard.commit();
```

### `front()`, `try_front()`

```cpp
if (Frame* frame = q.try_front()) {
    inspect(*frame);
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
for (Frame* frame : snap) {
    inspect(*frame);
}
q.consume(snap);
```

### `try_consume()`, `consume_all()`

```cpp
auto snap = q.make_snapshot();
(void)q.try_consume(snap);
q.consume_all();
```

### `claim_write()`

```cpp
auto regs = q.claim_write(spsc::unsafe, 4);
(void)regs;
```

### `claim_read()`

```cpp
auto regs = q.claim_read(spsc::unsafe, 4);
for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    inspect(*regs.first.ptr[i]);
}
q.pop(regs.total);
```

### `scoped_write(max_count)`, `scoped_read(max_count)`

```cpp
auto w = q.scoped_write(4);
auto r = q.scoped_read(4);
(void)w;
(void)r;
```

### `scoped_write()`, `scoped_read()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(args...);
}

if (auto guard = q.scoped_read()) {
    use(*guard);
}
```

### `resize()`, `clear()`, `destroy()`

```cpp
spsc::typed_pool<Frame, 0> dyn;
dyn.resize(32);
dyn.clear();
dyn.destroy();
```

### `swap()`

```cpp
spsc::typed_pool<Frame, 0> a{16};
spsc::typed_pool<Frame, 0> b{32};
swap(a, b);
```
