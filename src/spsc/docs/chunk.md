# `spsc::chunk`

`chunk<T, ChunkCapacity, Alloc>` is not an SPSC queue by itself.

It is a contiguous block container designed to be used:

- on its own as a block buffer
- as the payload type inside `fifo`
- most naturally inside `chunk_fifo`

## Variants

- static: `chunk<T, N>` backed by `std::array<T, N>`
- dynamic: `chunk<T, 0>` backed by allocator-managed storage

## Static Example

```cpp
#include "chunk.hpp"

spsc::chunk<std::uint16_t, 256> block;
block.push(1);
block.push(2);
block.push(3);
```

## Dynamic Example

```cpp
spsc::chunk<std::uint16_t, 0> block;
block.reserve(512);
block.push(10);
```

## Important Semantic Detail

For dynamic chunks:

- elements in `[0..capacity)` are eagerly constructed
- `push()` / `emplace()` use assignment or construction into the next logical slot
- `resize()` moves the logical length cursor, not the allocation itself

That makes `chunk` very practical for block workflows.

## DMA-Like Pattern

When a peripheral writes into the chunk memory directly, you can publish the logical size afterwards.

```cpp
spsc::chunk<std::uint16_t, 256> block;

start_dma(block.data(), block.capacity());
block.commit_size(actualSamplesWritten);
```

## More Example Patterns

### Dynamic Reserve Then Fill

```cpp
spsc::chunk<std::uint16_t, 0> block;
block.reserve(512);

for (std::uint16_t i = 0; i < 128; ++i) {
    block.push(i);
}
```

### Clamp Or Reject Resize

```cpp
spsc::chunk<int, 16> block;

block.resize_clamp(100); // logical size becomes 16
const bool exact = block.try_resize(100); // false
```

### Trim A Partially Built Block

```cpp
block.pop_back_n(4);
```

## Useful Operations

- `data()`
- `size()`, `capacity()`, `free()`
- `push`, `try_push`
- `emplace`, `try_emplace`
- `resize`, `try_resize`, `commit_size`
- `clear`
- `used_span()` / `cap_span()` when spans are enabled

## API Groups

### Capacity / State

- `size`, `capacity`, `free`
- `empty`, `full`

### Data Access

- `data`
- `operator[]`
- `front`, `back`

### Mutating Operations

- `push`, `try_push`
- `emplace`, `try_emplace`
- `resize`, `try_resize`, `commit_size`
- `clear`

## Good Fits

- ADC sample windows
- FFT blocks
- packet assembly before publish
- reusable temporary buffers

## Less Good Fits

- direct thread-safe exchange by itself
- unordered ownership transfer without a surrounding queue

## Method Reference

### Construction

- `chunk<T, N>` is fixed-size and allocation-free
- `chunk<T, 0>` starts empty and usually needs `reserve(...)`
- copy, move, and `swap(...)` are supported

### State And Capacity

- `size()`, `capacity()`, `free()`
- `empty()`, `full()`
- `clear()`
- dynamic `chunk<T, 0>` adds `reserve(new_cap)`
- `resize(n)` clamps or grows depending on variant
- `try_resize(n)` exists on the static variant and refuses overflow instead of clamping
- `resize_clamp(n)` exists on the static variant as an explicit naming variant

### Data Access

- `data()`
- `operator[](i)`
- `front()`, `back()`
- `try_front()`, `try_back()`
- `used_span()`, `cap_span()` when spans are enabled
- iterators over the currently used range

### Mutating Methods

- `push(...)` / `try_push(...)`
- `emplace(...)` / `try_emplace(...)`
- `pop_back()` / `try_pop_back()` / `pop_back_n(n)` if you use the chunk as a local block builder
- `commit_size(n)` after external writes such as DMA

The important semantic rule is that dynamic chunks are assignment-based over an already allocated storage area.

## Snippet Catalog

### `size()`, `capacity()`, `free()`

```cpp
const auto used = block.size();
const auto cap = block.capacity();
const auto freeSlots = block.free();
```

### `empty()`, `full()`

```cpp
if (block.empty()) {
    return;
}

if (block.full()) {
    flush_block(block);
}
```

### `data()`

```cpp
auto* ptr = block.data();
```

### `operator[]`

```cpp
block[0] = 42;
```

### `front()`, `back()`

```cpp
auto& first = block.front();
auto& last = block.back();
```

### `try_front()`, `try_back()`

```cpp
if (auto* first = block.try_front()) {
    process(*first);
}
```

### `used_span()`, `cap_span()`

```cpp
for (auto value : block.used_span()) {
    process(value);
}

auto allSlots = block.cap_span();
(void)allSlots;
```

### `push()`, `try_push()`

```cpp
block.push(1);
(void)block.try_push(2);
```

### `emplace()`, `try_emplace()`

```cpp
block.emplace(args...);
(void)block.try_emplace(args...);
```

### `resize()`, `try_resize()`, `resize_clamp()`

```cpp
block.resize(8);
(void)block.try_resize(8);
block.resize_clamp(64);
```

### `reserve()`

```cpp
spsc::chunk<int, 0> dyn;
dyn.reserve(128);
```

### `commit_size()`

```cpp
write_external_data(block.data(), block.capacity());
block.commit_size(actualCount);
```

### `pop_back()`, `try_pop_back()`, `pop_back_n()`

```cpp
block.pop_back();
(void)block.try_pop_back();
block.pop_back_n(3);
```

### `clear()`

```cpp
block.clear();
```

### `swap()`

```cpp
spsc::chunk<int, 16> a;
spsc::chunk<int, 16> b;
swap(a, b);
```
