# `chunk_fifo` and `chunk_fifo_view`

These wrappers combine `chunk<T,...>` with `fifo` / `fifo_view`.

The result is a FIFO of logical blocks:

- each queue slot is one `chunk`
- each chunk has its own logical size
- blocks move through the pipeline as queue entries

## Main Types

- `chunk_fifo<T, ChunkCapacity, FifoCapacity, Policy, Alloc>`
- `chunk_fifo_view<T, ChunkCapacity, FifoCapacity, Policy, Alloc>`

## Why It Exists

Use `chunk_fifo` when:

- blocks have a maximum size, but each published block may use fewer elements
- the consumer naturally works per-block
- the producer wants to fill a block and then publish it as one queue element

This is common for:

- ADC sample windows
- audio blocks
- frame decoders
- packet assembly

## Producer Model

Value-based producers are intentionally disabled here too.

You are expected to use zero-copy block filling:

```cpp
using Blocks = spsc::chunk_fifo<std::uint16_t, 256, 8>;

Blocks q;

if (auto* block = q.try_claim()) {
    block->clear();
    block->push_back(10);
    block->push_back(20);
    block->push_back(30);
    q.publish();
}
```

## Consumer Example

```cpp
if (auto* block = q.try_front()) {
    for (std::uint16_t sample : block->used_span()) {
        process(sample);
    }
    q.pop();
}
```

## Dynamic Variants

There are two moving parts:

- the chunk capacity
- the fifo depth

That means a dynamic configuration may involve:

- fixed chunk capacity, dynamic fifo depth
- dynamic chunk capacity and dynamic fifo depth

Read the long-form [overview](../README.md) for the detailed initialization recipes, especially for fully dynamic block pipelines.

## `chunk_fifo_view`

Use the view version when the chunk storage is external:

```cpp
std::array<spsc::chunk<std::uint16_t, 256>, 8> backing{};
spsc::chunk_fifo_view<std::uint16_t, 256, 8> q{backing};
```

## More Example Patterns

### Pre-Reserve Dynamic Chunks Before SPSC Start

```cpp
using Blocks = spsc::chunk_fifo<std::uint16_t, 0, 8>;

Blocks q;

for (std::size_t i = 0; i < static_cast<std::size_t>(q.capacity()); ++i) {
    q.data()[i].reserve(256);
    q.data()[i].clear();
}
```

### Snapshot Consumer Of Whole Blocks

```cpp
auto snap = q.make_snapshot();

for (const auto& block : snap) {
    process_block(block.used_span());
}

q.consume(snap);
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

- constructors from external chunk storage on `chunk_fifo_view`
- `attach` / `adopt` through the underlying `fifo_view`

### Disabled By Design

- `push`
- `try_push`
- `emplace`
- `try_emplace`

## Good Fits

- windowed DSP
- framed sensor acquisition
- block-oriented protocol parsing
- pipelines where a block remains contiguous until consumption

## Less Good Fits

- fixed-size frames with no logical length variation, where `array_fifo` is simpler
- pure newest-value semantics, where `latest` is the right tool

## Method Reference

These wrappers inherit most of their API from `fifo` / `fifo_view`, but the payload is `chunk<T,...>`.

### Producer

- `claim`, `try_claim`
- `publish`, `try_publish`
- `publish(n)`, `try_publish(n)` for region-style production

The producer usually:

1. claims one `chunk`
2. fills it
3. sets its logical size
4. publishes it

### Consumer

- `front`, `try_front`
- `pop`, `try_pop`
- `consume`, `consume_all`
- `make_snapshot`
- iteration over queue entries

### View Variant

`chunk_fifo_view` inherits the `fifo_view`-style attach/adopt/state model, but over externally owned chunk storage.

### Disabled Intentionally

- `push`
- `try_push`
- `emplace`
- `try_emplace`

This is by design, because block production is expected to be zero-copy and explicit.

## Snippet Catalog

### `claim()`, `try_claim()`

```cpp
if (auto* block = q.try_claim()) {
    block->clear();
    block->push_back(1);
    block->push_back(2);
    q.publish();
}
```

```cpp
auto& block = q.claim();
block.clear();
fill_block(block);
q.publish();
```

### `publish()`

```cpp
if (auto* block = q.try_claim()) {
    fill_block(*block);
    q.publish();
}
```

### `front()`, `try_front()`

```cpp
if (auto* block = q.try_front()) {
    process_block(block->used_span());
}
```

```cpp
if (!q.empty()) {
    process_block(q.front().used_span());
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
for (const auto& block : snap) {
    process_block(block.used_span());
}
q.consume(snap);
```

### view `attach()` / `state()`

```cpp
std::array<spsc::chunk<std::uint16_t, 64>, 8> backing{};
spsc::chunk_fifo_view<std::uint16_t, 64, 0> view{backing.data(), backing.size()};

auto st = view.state();
view.attach(backing.data(), backing.size(), st);
```
