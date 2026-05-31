# `array_fifo`, `array_fifo_view`, `carray_fifo_view`

These wrappers are built on top of `fifo` / `fifo_view`, but specialized for fixed-size arrays.

They are ideal when the payload is a frame with a compile-time fixed width.

With cache-aligned policies such as `spsc::policy::CA<>`, `array_fifo` and
`array_fifo_view` promote each frame slot to cache-line alignment so adjacent
frames do not share a cache line. `carray_fifo_view` keeps its raw `T[Depth][N]`
layout and does not add per-slot wrapper padding.

## Main Types

- `array_fifo<T, N, FifoCapacity, Policy, Alloc>`
- `array_fifo_view<T, N, FifoCapacity, Policy>`
- `carray_fifo_view<T, N, FifoCapacity, Policy>`

## Producer Model

Value-based producers are intentionally disabled.

That means you should use:

- `claim` / `try_claim`
- `publish`

This design keeps the write path explicit and zero-copy-friendly.

## `array_fifo` Example

```cpp
#include "array_fifo.hpp"

using RxFrames = spsc::array_fifo<std::uint8_t, 64, 32>;

RxFrames q;

if (auto* frame = q.try_claim()) {
    frame->fill(0);
    (*frame)[0] = 0xAA;
    (*frame)[1] = 0x55;
    q.publish();
}
```

## `array_fifo_view` Example

```cpp
using View = spsc::array_fifo_view<std::uint8_t, 64, 32>;
std::array<View::value_type, 32> backing{};
View q{backing};
```

## `carray_fifo_view` Example

```cpp
std::uint8_t backing[32][64]{};
spsc::carray_fifo_view<std::uint8_t, 64, 32> q{backing};
```

`carray_fifo_view` is useful when another API or memory map is already expressed as `T[Depth][N]`.

## More Example Patterns

### Snapshot Consumer

```cpp
auto snap = q.make_snapshot();

for (const auto& frame : snap) {
    handle_frame(frame);
}

q.consume(snap);
```

### Restore A View Over Existing Frame Storage

```cpp
spsc::array_fifo_view<std::uint8_t, 64, 0>::state_t st{savedHead, savedTail};
q.attach(backing.data(), backing.size(), st);
```

## API Groups

### Producer

- `claim`, `try_claim`
- `publish`, `try_publish`

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- `consume`, `consume_all`
- `make_snapshot`

### View Management

- constructors from external storage on view variants
- `attach` / `adopt` through the underlying `fifo_view` family

### Disabled By Design

- `push`
- `try_push`
- `emplace`
- `try_emplace`

## Good Fits

- UART / SPI / Modbus frames
- network packets of fixed size
- register snapshots
- fixed binary command blocks

## Less Good Fits

- variable logical length with large unused slack, where `chunk_fifo` may be better
- typed objects with lifetime semantics, where `queue` or `typed_pool` fit better

## Method Reference

These wrappers inherit most of the queue-like API from `fifo` / `fifo_view`.

### Producer

- `claim`, `try_claim`
- `scoped_write`, `scoped_write(max_count)`
- `publish`, `try_publish`
- `publish(unsafe, n)`, `try_publish(unsafe, n)` when using region-based production

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- `consume`, `consume_all`
- `make_snapshot`
- iteration and `operator[]`

### View Variants

- `array_fifo_view` inherits the `fifo_view` attach/adopt/state model
- `carray_fifo_view` is the same idea, but for `T[Depth][N]` storage

### Disabled Intentionally

- `push`
- `try_push`
- `emplace`
- `try_emplace`

That is deliberate: these wrappers are meant to push users toward explicit zero-copy frame filling.

## Snippet Catalog

### `claim()`, `try_claim()`

```cpp
if (auto* frame = q.try_claim()) {
    frame->fill(0);
    (*frame)[0] = 0xAA;
    q.publish();
}
```

```cpp
auto& frame = q.claim();
frame.fill(0x11);
q.publish();
```

### `publish()`, `publish(unsafe, n)`

```cpp
auto regs = q.claim_write(spsc::unsafe, 2);
if (regs.total != 0u) {
    regs.first.ptr[0].fill(0x55);
    q.publish(::spsc::unsafe, 1);
}
```

### `front()`, `try_front()`

```cpp
if (auto* frame = q.try_front()) {
    process_frame(*frame);
}
```

```cpp
if (!q.empty()) {
    process_frame(q.front());
}
```

### `pop()`, `try_pop()`

```cpp
(void)q.try_pop();
```

### `consume_all()`

```cpp
q.consume_all();
```

### `make_snapshot()`, `consume()`

```cpp
auto snap = q.make_snapshot();
for (const auto& frame : snap) {
    process_frame(frame);
}
q.consume(snap);
```

### view `attach()` / `state()`

```cpp
std::array<std::array<std::uint8_t, 64>, 8> backing{};
spsc::array_fifo_view<std::uint8_t, 64, 0> view{backing.data(), backing.size()};

auto st = view.state();
view.attach(backing.data(), backing.size(), st);
```
