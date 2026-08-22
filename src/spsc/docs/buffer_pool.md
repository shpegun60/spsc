# `spsc::buffer_pool`

`buffer_pool<T, BufferSize, Count, Policy, Alloc>` owns a collection of fixed-size buffers.

It is not an SPSC queue by itself. Use it when you need stable buffer storage for
DMA, packet payloads, or another queue/pool layer that moves buffer ownership.

`buffer_pool` intentionally has no `local_*`, `concurrent_*`, or
`cache_aligned_*` aliases. Its `Policy` controls storage alignment and physical
span rather than producer/consumer synchronization. Keep storage policy
selection explicit, especially `CP` for cache-aligned DMA storage.

In v3, omitting its policy follows the global `FA<>` default even though
`buffer_pool` is not an SPSC endpoint. For a storage layout that must remain
plain or DMA-cache-aligned across the v2-to-v3 boundary, spell `P` or `CP`
explicitly. See [Migrating from v2 to v3](migration-v3.md).

## Variants

- `buffer_pool<T, BufferSize, Count>`: static buffer size and static count
- `buffer_pool<T, BufferSize, 0>`: static buffer size, runtime count
- `buffer_pool<T, 0, Count>`: runtime buffer size, static count
- `buffer_pool<T, 0, 0>`: runtime buffer size and runtime count

Readable aliases preserve the same four shapes and the same default-policy
mapping: `static_buffer_pool`, `fixed_buffer_pool`,
`fixed_count_buffer_pool`, and `dynamic_buffer_pool`.

## Basic Static Example

```cpp
#include "buffer_pool.hpp"

spsc::buffer_pool<std::byte, 1500, 8, spsc::policy::P> buffers;

for (auto i = decltype(buffers)::size_type{0}; i < buffers.count(); ++i) {
    std::byte* payload = buffers.data(i);
    prepare_packet(payload, buffers.size());
}
```

## Dynamic Shape Examples

```cpp
spsc::buffer_pool<std::byte, 1500, 0, spsc::policy::P> runtimeCount;
runtimeCount.resize(8);

spsc::buffer_pool<std::byte, 0, 8, spsc::policy::P> runtimeSize;
runtimeSize.resize(1500);

spsc::buffer_pool<std::byte, 0, 0, spsc::policy::P> dynamicShape;
dynamicShape.resize(8, 1500);
```

## Important Semantics

- `count()` is the number of buffers.
- `size()` is the logical element count per buffer.
- `size_bytes()` / `payload_bytes()` are the logical byte size of one buffer.
- `span_bytes()` / `cache_span_bytes()` are the physical per-buffer span after
  policy alignment rounding.
- `alignment()` / `storage_alignment()` report the alignment guaranteed by the
  value type and policy. They do not report a stronger alignment that a custom
  allocator may happen to provide.
- `data(i)` returns `nullptr` when `i` is outside the valid buffer range.
- `operator[](i)` is the assert-style indexed form for valid indices.
- A zero count or zero buffer size is a coherent empty shape for runtime-shaped
  variants. Consequently, `is_valid()` alone cannot distinguish deliberate
  emptiness from a constructor allocation failure. If a non-empty shape is
  required, call `resize()`, check its boolean result, and verify the exact
  `count()` and `size()` requested.
- `buffer_pool<T, 0, Count>` uses an allocator-backed transient pointer table
  for copy and resize. Stack use is independent of `Count`, container layout is
  unchanged, and a failed temporary allocation leaves the previous assignment
  or resize destination unchanged.

Cache-aligned policies can align storage and round the physical span reported by
`cache_span_bytes()`. Runtime-sized variants allocate each payload separately,
so treat this as a per-buffer alignment/span contract rather than a contiguous
adjacency guarantee. The cache-span name is a valid cache-maintenance contract
only when the selected policy alignment matches the target's real cache line.
Hardware cache maintenance still belongs outside the container.

## STM32H7 DMA Storage

`buffer_pool` owns storage but has no producer/consumer indices, so it needs
alignment from its policy, not atomic counters. Use `CP` for the storage and an
atomic policy such as `CFA<>` on the queue or view that transfers ownership:

```cpp
using DmaBuffers =
    spsc::buffer_pool<std::byte, 100, 8, spsc::policy::CP>;
using Transport = spsc::pool_view<8, spsc::policy::CFA<>>;

static_assert(DmaBuffers::payload_bytes() == 100);
static_assert(DmaBuffers::cache_span_bytes() == 128);
static_assert(DmaBuffers::storage_alignment() == 32);
```

For STM32H7, ensure `SPSC_CACHELINE_BYTES == 32` through the STM32 target macro
or `-DSPSC_FORCE_CACHELINE=32`. The allocator/linker still decides whether
dynamic storage resides in DMA-accessible SRAM. `buffer_pool` deliberately does
not perform `SCB_CleanDCache_by_Addr`, `SCB_InvalidateDCache_by_Addr`, DMA
ownership transfer, or memory-region selection.

## Good Fits

- fixed-size network or protocol payload buffers
- DMA descriptors where another subsystem owns the producer/consumer protocol
- preallocated storage used together with `pool`, `pool_view`, or `fifo`

## Less Good Fits

- direct SPSC exchange by itself
- variable-length object lifetime management
- newest-value semantics
