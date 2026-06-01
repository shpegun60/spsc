# `spsc::buffer_pool`

`buffer_pool<T, BufferSize, Count, Policy, Alloc>` owns a collection of fixed-size buffers.

It is not an SPSC queue by itself. Use it when you need stable buffer storage for
DMA, packet payloads, or another queue/pool layer that moves buffer ownership.

## Variants

- `buffer_pool<T, BufferSize, Count>`: static buffer size and static count
- `buffer_pool<T, BufferSize, 0>`: static buffer size, runtime count
- `buffer_pool<T, 0, Count>`: runtime buffer size, static count
- `buffer_pool<T, 0, 0>`: runtime buffer size and runtime count

## Basic Static Example

```cpp
#include "buffer_pool.hpp"

spsc::buffer_pool<std::byte, 1500, 8> buffers;

for (auto i = decltype(buffers)::size_type{0}; i < buffers.count(); ++i) {
    std::byte* payload = buffers.data(i);
    prepare_packet(payload, buffers.size());
}
```

## Dynamic Shape Examples

```cpp
spsc::buffer_pool<std::byte, 1500, 0> runtimeCount;
runtimeCount.resize(8);

spsc::buffer_pool<std::byte, 0, 8> runtimeSize;
runtimeSize.resize(1500);

spsc::buffer_pool<std::byte, 0, 0> dynamicShape;
dynamicShape.resize(8, 1500);
```

## Important Semantics

- `count()` is the number of buffers.
- `size()` is the logical element count per buffer.
- `size_bytes()` is the logical byte size of one buffer.
- `span_bytes()` is the physical per-buffer span after policy/allocator alignment rounding.
- `data(i)` returns `nullptr` when `i` is outside the valid buffer range.
- `operator[](i)` is the assert-style indexed form for valid indices.

Cache-aligned policies can align storage and round the physical span reported by
`span_bytes()`. Runtime-sized variants allocate each payload separately, so treat
this as a per-buffer alignment/span contract rather than a contiguous adjacency
guarantee. On STM32F7/STM32H7-style DMA paths, use `span_bytes()` as the
physical per-buffer span when that is the region handed to DMA. Hardware cache
maintenance still belongs outside the container.

## Good Fits

- fixed-size network or protocol payload buffers
- DMA descriptors where another subsystem owns the producer/consumer protocol
- preallocated storage used together with `pool`, `pool_view`, or `fifo`

## Less Good Fits

- direct SPSC exchange by itself
- variable-length object lifetime management
- newest-value semantics
