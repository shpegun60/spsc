# Guard and Bulk Helpers

This page covers the helper objects returned by:

- `scoped_write()`
- `scoped_read()`
- `scoped_write(max_count)`
- `scoped_read(max_count)`
- low-level `claim_write(...)` / `claim_read(...)` patterns

These helpers are where many of the most useful "small API" methods live.

Assume simple helper types like these in the snippets below:

```cpp
struct Header {
    std::uint32_t magic;
    std::uint16_t len;
};

struct Message {
    int id;
    std::string text;
};

struct Frame {
    std::array<std::byte, 256> payload;
};
```

## 1. `fifo` / `fifo_view` Single-Slot Guards

### `write_guard::peek()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* slot = guard.peek();   // does not arm auto-publish
    *slot = make_value();
    guard.publish_on_destroy();
}
```

### `write_guard::get()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* slot = guard.get();    // arms auto-publish
    *slot = make_value();
}
```

### `write_guard::ref()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.ref() = make_value();  // arms auto-publish
}
```

### `write_guard::publish_on_destroy()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* slot = guard.peek();
    *slot = make_value();
    guard.publish_on_destroy();
}
```

### `write_guard::commit()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.ref() = make_value();
    guard.commit();
}
```

### `write_guard::cancel()`

```cpp
auto guard = q.scoped_write();
if (!guard) {
    return;
}

if (!ready_to_publish()) {
    guard.cancel();
    return;
}

guard.ref() = make_value();
```

### `read_guard::get()`, `read_guard::ref()`

```cpp
if (auto guard = q.scoped_read()) {
    auto* ptr = guard.get();
    process(*ptr);
}
```

### `read_guard::commit()`

```cpp
if (auto guard = q.scoped_read()) {
    process(*guard);
    guard.commit();
}
```

### `read_guard::cancel()`

```cpp
auto guard = q.scoped_read();
if (!guard) {
    return;
}

if (!can_consume_yet(*guard)) {
    guard.cancel();
    return;
}

process(*guard);
```

## 2. `fifo` / `fifo_view` Bulk Write Guard

### `claimed()`, `remaining()`

```cpp
auto guard = q.scoped_write(8);

const auto claimed = guard.claimed();
while (guard.remaining() != 0u) {
    guard.write_next(produce_value());
}
```

### `peek_next()`

```cpp
auto guard = q.scoped_write(4);
if (guard) {
    auto* slot = guard.peek_next();
    *slot = make_value();
    guard.mark_written();
    guard.arm_publish();
}
```

### `get_next()`

```cpp
auto guard = q.scoped_write(4);
if (guard) {
    auto* slot = guard.get_next(); // arms auto-publish
    *slot = make_value();
    guard.mark_written();          // advances the publish count
}
```

`get_next()` selects the current slot and arms auto-publish, but it does not
advance the written count. Call `mark_written()` after filling that slot.
`write_next()` and `emplace_next()` advance the count themselves.

### `write_next()`

```cpp
auto guard = q.scoped_write(4);
while (guard.remaining() != 0u) {
    guard.write_next(produce_value());
}
guard.commit();
```

### `emplace_next()`

```cpp
auto guard = q.scoped_write(4);
while (guard.remaining() != 0u) {
    guard.emplace_next(arg1, arg2);
}
guard.commit();
```

### `mark_written()`

```cpp
auto guard = q.scoped_write(4);
auto* slot = guard.peek_next();
fill_slot_manually(*slot);
guard.mark_written();
guard.arm_publish();
```

### `arm_publish()`, `publish_on_destroy()`, `disarm_publish()`

```cpp
auto guard = q.scoped_write(2);
guard.write_next(1);
guard.disarm_publish();   // do not auto-publish on destruction
guard.arm_publish();      // re-arm it
```

### `commit()`, `cancel()`

```cpp
auto guard = q.scoped_write(4);
guard.write_next(1);
guard.write_next(2);
guard.commit();
```

```cpp
auto guard = q.scoped_write(4);
guard.write_next(1);
guard.cancel();
```

## 3. `fifo` / `fifo_view` Bulk Read Guard

### `count()`

```cpp
auto guard = q.scoped_read(8);
const auto n = guard.count();
(void)n;
```

### `regions_view()`

```cpp
auto guard = q.scoped_read(8);
const auto& regs = guard.regions_view();

for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    process(regs.first.ptr[i]);
}
```

### `commit()`, `cancel()`

```cpp
auto guard = q.scoped_read(8);
consume_regions(guard.regions_view());
guard.commit();
```

```cpp
auto guard = q.scoped_read(8);
if (!ready_for_bulk_consume()) {
    guard.cancel();
}
```

## 4. `queue` Single-Slot And Bulk Helpers

### `write_guard::get()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* raw = guard.get();
    ::new (static_cast<void*>(raw)) Message(1, "manual");
    guard.mark_constructed();
    guard.arm_publish();
}
```

### `write_guard::emplace()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(2, "ready");
}
```

### `write_guard::mark_constructed()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* raw = guard.get();
    ::new (static_cast<void*>(raw)) Message(3, "manual");
    guard.mark_constructed();
    guard.commit();
}
```

### `bulk_write_guard::claimed()`, `constructed()`, `remaining()`

```cpp
auto guard = q.scoped_write(3);
while (guard.remaining() != 0u) {
    guard.emplace_next(next_id(), next_text());
}
guard.commit();
```

### `bulk_read_guard::regions()`

```cpp
auto guard = q.scoped_read(4);
const auto& regs = guard.regions();

for (std::size_t i = 0; i < static_cast<std::size_t>(regs.first.count); ++i) {
    process(regs.first.ptr[i]);
}
guard.commit();
```

## 5. `pool` / `pool_view` Single-Slot Helpers

### `write_guard::peek()`, `write_guard::get()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* slot = guard.peek();
    fill_payload(static_cast<std::byte*>(slot), q.buffer_size());
    guard.publish_on_destroy();
}
```

### `write_guard::as<U>()`

```cpp
if (auto guard = q.scoped_write()) {
    if (auto* hdr = guard.as<Header>()) {
        ::new (static_cast<void*>(hdr)) Header{0x12345678u, 32u};
    }
}
```

### `write_guard::disarm_publish()`

```cpp
auto guard = q.scoped_write();
if (auto* hdr = guard.as<Header>()) {
    ::new (static_cast<void*>(hdr)) Header{0x12345678u, 32u};
    guard.disarm_publish();
}
```

### `read_guard::as<U>()`

```cpp
if (auto guard = q.scoped_read()) {
    if (auto* hdr = guard.as<Header>()) {
        // Only valid if the producer started Header lifetime in the slot.
        handle_header(*hdr);
    }
}
```

## 6. `pool` / `pool_view` Bulk Helpers

### `bulk_write_guard::peek_next()`

```cpp
auto guard = q.scoped_write(4);
if (guard) {
    void* slot = guard.peek_next();
    fill_payload(static_cast<std::byte*>(slot), q.buffer_size());
    guard.mark_written();
    guard.arm_publish();
}
```

### `bulk_write_guard::write_next(data, size)`

```cpp
auto guard = q.scoped_write(4);
std::byte temp[32]{};

while (guard.remaining() != 0u) {
    build_packet(temp, sizeof(temp));
    guard.write_next(temp, sizeof(temp));
}
guard.commit();
```

### `bulk_write_guard::write_next(U)`

```cpp
auto guard = q.scoped_write(2);
guard.write_next(Header{0x12345678u, 32u});
guard.commit();
```

### `bulk_read_guard::first()`, `second()`

```cpp
auto guard = q.scoped_read(8);

for (std::size_t i = 0; i < static_cast<std::size_t>(guard.first().size()); ++i) {
    consume_slot(guard.first()[static_cast<decltype(guard.first().size())>(i)]);
}

for (std::size_t i = 0; i < static_cast<std::size_t>(guard.second().size()); ++i) {
    consume_slot(guard.second()[static_cast<decltype(guard.second().size())>(i)]);
}
```

### `bulk_read_guard::regions_view()`

```cpp
auto guard = q.scoped_read(8);
const auto& regs = guard.regions_view();
(void)regs;
```

## 7. `typed_pool` Helpers

### `write_guard::emplace()`

```cpp
if (auto guard = q.scoped_write()) {
    guard.emplace(args...);
}
```

### `write_guard::mark_constructed()`

```cpp
if (auto guard = q.scoped_write()) {
    auto* raw = guard.get();
    ::new (static_cast<void*>(raw)) Frame{};
    guard.mark_constructed();
    guard.publish_on_destroy();
}
```

### `bulk_read_guard::first()`, `second()`

```cpp
auto guard = q.scoped_read(8);

for (std::size_t i = 0; i < static_cast<std::size_t>(guard.first().size()); ++i) {
    inspect(*guard.first()[static_cast<decltype(guard.first().size())>(i)]);
}
```

### `region_view::ptr(i)`

```cpp
auto guard = q.scoped_read(8);
if (!guard.first().empty()) {
    Frame* frame = guard.first().ptr(0);
    inspect(*frame);
}
```

## 8. Practical Guidance

Prefer these layers in order:

1. `push` / `try_push` or `emplace` / `try_emplace`
2. `scoped_write()` / `scoped_read()`
3. `scoped_write(max_count)` / `scoped_read(max_count)`
4. raw `claim_write(unsafe, ...)` / `claim_read(unsafe, ...)`

That gives you the simplest safe API that still fits the job.
