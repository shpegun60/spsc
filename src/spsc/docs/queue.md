# `spsc::queue`

`queue<T, Capacity, Policy, Alloc>` is the lifetime-managed owning queue.

Unlike `fifo`, `queue` uses placement new and explicit destruction:

- `push()` / `emplace()` construct elements in raw storage
- `pop()` destroys the object in-place

This makes `queue` the right choice when object lifetime matters.

## When To Use It

Choose `queue` when:

- `T` is not default-constructible
- `T` has non-trivial lifetime and destruction must happen on consume
- you want a real object-lifetime queue, not an assignment-based ring

## Static And Dynamic Variants

```cpp
spsc::queue<std::string, 256> static_q;
spsc::queue<std::string, 0> dynamic_q{256};
```

Even static `queue` allocates its storage so the lifetime model is consistent between static and dynamic variants.

## Basic Example

```cpp
spsc::queue<std::string, 128> q;

q.emplace("hello");
q.push(std::string("world"));

if (auto* front = q.try_front()) {
    consume(*front);
    q.pop(); // destroys the stored std::string
}
```

## Manual Construction Path

This is the right path when you want zero-copy construction.

```cpp
if (auto* slot = q.try_claim()) {
    new (slot) Message(42, "payload");
    q.publish();
}
```

This matters because a claimed write slot is **uninitialized raw storage** until you construct `T` there.

## More Example Patterns

### RAII Producer Path

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(7, "ready");
}
```

### Batch Construction

```cpp
auto guard = q.scoped_write(3);

while (guard.remaining() != 0u) {
    guard.emplace_next(next_id(), next_payload());
}

guard.commit();
```

### Snapshot Processing Of Live Objects

```cpp
auto snap = q.make_snapshot();

for (const auto& message : snap) {
    route_message(message);
}

q.consume(snap);
```

## Snapshots And Regions

`queue` supports snapshots and bulk region helpers, but keep the lifetime model in mind:

- write regions expose raw uninitialized storage
- read regions expose live objects
- bulk guards are useful when several objects are constructed or consumed as a group

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

### Bulk / Lifetime Helpers

- write regions exposing raw storage
- read regions exposing live objects
- scoped write/read guards

### Management

- `reserve`
- `resize`
- `clear`
- `swap`

## Example: Non-Default-Constructible Type

```cpp
struct Message {
    int id;
    std::string text;

    Message(int i, std::string t) : id(i), text(std::move(t)) {}
};

spsc::queue<Message, 256> q;
q.emplace(7, "ready");
```

`fifo<Message,...>` would not be as natural here because `fifo` expects default-constructible slot objects and assignment-based reuse.

## Good Fits

- strings
- smart handles with meaningful destruction
- protocol objects with non-trivial constructors
- types where destructor timing matters

## Less Good Fits

- ultra-simple POD types where `fifo` is enough
- external storage ownership, because there is no `queue_view` here

## Method Reference

### Construction

- `queue()` creates an empty queue.
- `queue(requested_capacity)` exists for the dynamic variant.
- move is supported; copy is intentionally disabled because `queue` owns live object lifetime.
- `swap(other)` exchanges storage and state.
- `destroy()` releases storage and destroys live objects.

Because `queue` manages lifetime, the destructor and clear paths are semantically heavier than `fifo`.

### State And Introspection

- `is_valid()`
- `capacity()`, `size()`, `empty()`, `full()`, `free()`
- `can_write(n)`, `can_read(n)`
- `write_size()`, `read_size()`
- `data()` exposes the raw storage buffer

Important: `data()` points at storage, but not every slot currently holds a live object.

### Producer Methods

- `push(value)` constructs in-place and advances head
- `try_push(value)` returns `false` when full
- `emplace(args...)` constructs `T` directly
- `try_emplace(args...)` returns `nullptr` when full
- `claim()` / `try_claim()` return raw uninitialized storage for manual placement new
- `publish()` / `try_publish()` commit one slot
- `publish(unsafe, n)` / `try_publish(unsafe, n)` commit several prepared slots

Manual construction example:

```cpp
if (auto* slot = q.try_claim()) {
    new (slot) Message(1, "hello");
    q.publish();
}
```

### Consumer Methods

- `front()` / `try_front()` return the current live object
- `pop()` destroys the current object and advances tail
- `try_pop()` returns `false` when empty
- `pop(n)` / `try_pop(n)` destroy and consume several objects
- deleted `pop(U&)` / `try_pop(U&)` trap accidental numeric binding

### Iteration, Snapshots, And Bulk

- `begin/end` and reverse iterators walk the logical queue
- `operator[](i)` indexes logical queue order
- `make_snapshot()`, `consume(snapshot)`, `try_consume(snapshot)`, `consume_all()`
- `claim_write(unsafe, max_count)` exposes raw uninitialized regions
- `claim_read(unsafe, max_count)` exposes readable regions of live objects
- `bulk_write_guard`, `bulk_read_guard`, `write_guard`, `read_guard` support scoped commit/pop behavior
- `scoped_write()`, `scoped_write(max_count)`, `scoped_read()`, `scoped_read(max_count)` build those helpers directly

Use `claim_write` only when you are ready to construct objects into raw storage correctly.

Guard example:

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(42, "payload");
    // commit happens on scope exit
}

if (auto guard = q.scoped_read()) {
    process(*guard);
    // pop() happens on scope exit
}
```

### Management

- `reserve(min_capacity)` and `resize(requested_capacity)` exist on dynamic `queue`
- `clear()` destroys all live objects and resets the queue
- `destroy()` frees storage

These methods are non-concurrent.

## Snippet Catalog

### `capacity()`, `size()`, `free()`

```cpp
const auto cap = q.capacity();
const auto used = q.size();
const auto freeSlots = q.free();
```

### `is_valid()`, `empty()`, `full()`

```cpp
if (!q.is_valid() || q.empty()) {
    return;
}

if (q.full()) {
    ++fullCount;
}
```

### `can_write()`, `can_read()`, `write_size()`, `read_size()`

```cpp
if (q.can_write(4)) {
    const auto contiguousWritable = q.write_size();
    (void)contiguousWritable;
}

if (q.can_read(2)) {
    const auto contiguousReadable = q.read_size();
    (void)contiguousReadable;
}
```

### `data()`

```cpp
auto* rawStorage = q.data();
(void)rawStorage;
```

### `operator[]`, `begin()`, `end()`

```cpp
for (const auto& item : q) {
    handle(item);
}

if (!q.empty()) {
    handle(q[0]);
}
```

### `push()`

```cpp
q.push(Message{1, "hello"});
```

### `try_push()`

```cpp
if (!q.try_push(Message{2, "world"})) {
    ++dropCount;
}
```

### `emplace()`

```cpp
q.emplace(3, "ready");
```

### `try_emplace()`

```cpp
if (auto* msg = q.try_emplace(4, "queued")) {
    inspect(*msg);
}
```

### `claim()`

```cpp
auto* slot = q.claim();
new (slot) Message(5, "manual");
q.publish();
```

### `try_claim()`

```cpp
if (auto* slot = q.try_claim()) {
    new (slot) Message(6, "manual");
    q.publish();
}
```

### `publish()`, `try_publish()`

```cpp
if (auto* slot = q.try_claim()) {
    new (slot) Message(7, "done");
    q.publish();
}
```

### `front()`, `try_front()`

```cpp
if (auto* msg = q.try_front()) {
    handle(*msg);
}
```

### `pop()`, `try_pop()`

```cpp
if (!q.empty()) {
    q.pop();
}

(void)q.try_pop();
```

### `pop(n)`

```cpp
if (q.can_read(2)) {
    q.pop(2);
}
```

### `make_snapshot()`

```cpp
auto snap = q.make_snapshot();
for (const auto& msg : snap) {
    handle(msg);
}
```

### `try_consume()`, `consume_all()`

```cpp
auto snap = q.make_snapshot();
if (ready()) {
    (void)q.try_consume(snap);
}

q.consume_all();
```

### `consume_all()`

```cpp
q.consume_all();
```

### `claim_write()`

```cpp
auto regs = q.claim_write(spsc::unsafe, 2);
if (regs.first.count != 0u) {
    auto* slots = regs.first.ptr_uninit();
    new (&slots[0]) Message(10, "bulk");
    q.publish(::spsc::unsafe, 1);
}
```

### `claim_read()`

```cpp
auto regs = q.claim_read(spsc::unsafe, 2);
for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    handle(regs.first.ptr[i]);
}
q.pop(regs.total);
```

### `scoped_write()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(11, "guard");
}
```

### `scoped_read()`

```cpp
if (auto guard = q.scoped_read()) {
    handle(*guard);
}
```

### `scoped_write(max_count)`, `scoped_read(max_count)`

```cpp
auto w = q.scoped_write(4);
auto r = q.scoped_read(4);
(void)w;
(void)r;
```

### `reserve()`, `resize()`

```cpp
spsc::queue<Message, 0> dyn;
dyn.reserve(64);
dyn.resize(128);
```

### `clear()`, `destroy()`

```cpp
q.clear();
q.destroy();
```

### `swap()`

```cpp
spsc::queue<Message, 0> a{32};
spsc::queue<Message, 0> b{64};
swap(a, b);
```
