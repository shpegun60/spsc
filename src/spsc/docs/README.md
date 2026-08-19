# SPSC Documentation Hub

This folder contains the split documentation for the `spsc` containers.

If you are new to the library, read in this order:

1. [Common Concepts](common-concepts.md)
2. [Concurrency and FreeRTOS](concurrency-and-freertos.md)
3. [Method Recipes](method-recipes.md)
4. [Guard and Bulk Helpers](guard-and-bulk-helpers.md)
5. One container guide from the list below
6. The short [Quick Start](../README.md) when you want a fast overview before diving into container-specific docs

## Quick Container Choice

| Need | Recommended type |
| --- | --- |
| Small values, commands, IDs, POD messages | [`fifo`](fifo.md) |
| External value storage supplied by user | [`fifo_view`](fifo_view.md) |
| Non-default-constructible objects, full lifetime control | [`queue`](queue.md) |
| Fixed-size raw byte buffers | [`pool`](pool.md) |
| External raw buffers / DMA-owned memory | [`pool_view`](pool_view.md) |
| Only the newest value matters | [`latest`](latest.md) |
| Stable typed object slots addressed by pointers | [`typed_pool`](typed_pool.md) |
| A contiguous block container used as payload | [`chunk`](chunk.md) |
| FIFO of fixed-size arrays | [`array_fifo` family](array_fifo.md) |
| FIFO of `chunk<T,...>` blocks | [`chunk_fifo` family](chunk_fifo.md) |
| Owning collection of fixed-size buffers | [`buffer_pool`](buffer_pool.md) |

## Choose the Concurrency Spelling

For policy-driven SPSC transport containers, the v2.1 aliases make the common
contract visible in the type name:

| Need | Prefer | Policy |
| --- | --- | --- |
| one context or external synchronization | `local_*` | `P` |
| ordinary portable one-producer/one-consumer transport | `concurrent_*` | `FA<>` |
| concurrent transport with justified cache isolation | `cache_aligned_*` | `CFA<>` |

This applies to `fifo`, `queue`, their views, pools, `latest`, and the array
and chunk FIFO families. The aliases are zero-cost type aliases; the explicit
`Container<..., Policy, ...>` form remains available for advanced policy
selection. `buffer_pool` is deliberately excluded because it is storage, not
an SPSC transport endpoint; keep its DMA alignment policy explicit (normally
`CP`). See [Common Concepts](common-concepts.md#6-policies) for the full
policy contract.

## Container Guides

- [Common Concepts](common-concepts.md)
- [Concurrency and FreeRTOS](concurrency-and-freertos.md)
- [Method Recipes](method-recipes.md)
- [Guard and Bulk Helpers](guard-and-bulk-helpers.md)
- [fifo](fifo.md)
- [fifo_view](fifo_view.md)
- [queue](queue.md)
- [pool](pool.md)
- [pool_view](pool_view.md)
- [latest](latest.md)
- [typed_pool](typed_pool.md)
- [chunk](chunk.md)
- [array_fifo family](array_fifo.md)
- [chunk_fifo family](chunk_fifo.md)
- [buffer_pool](buffer_pool.md)

## Practical Reading Strategy

- Start with `fifo` if you want the most typical SPSC queue.
- Move to `queue` when explicit object lifetime control is required.
- Move to `pool` / `typed_pool` when the producer and consumer exchange ownership of slots or buffers.
- Move to `latest` when the consumer should only observe the newest state.
- Use `*_view` variants when storage is owned by DMA, shared memory, static RAM, or another subsystem.
