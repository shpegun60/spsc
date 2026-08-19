# SPSC Quick Start

This folder contains the `spsc` container family: single-producer / single-consumer queues, pools, newest-value buffers, and block wrappers built on the same base.

This page is the short quick start and overview. The full method-by-method reference, container guides, and concurrency recipes live in [`docs/`](docs/README.md).
The examples below are C++17-safe unless explicitly marked otherwise. Helpers that return `std::span`
(`span()`, `used_span()`, `cap_span()`) require C++20 library support and `SPSC_HAS_SPAN=1`.

## Start Here

- [Documentation Hub](docs/README.md)
- [Migrating from v2 to v3](docs/migration-v3.md)
- [Common Concepts](docs/common-concepts.md)
- [Concurrency and FreeRTOS](docs/concurrency-and-freertos.md)
- [Method Recipes](docs/method-recipes.md)
- [Guard and Bulk Helpers](docs/guard-and-bulk-helpers.md)

## Container Choice

| Need | Recommended type | Notes |
| --- | --- | --- |
| Small values, IDs, commands, POD messages | [`fifo`](docs/fifo.md) | Simplest owning SPSC queue |
| External value storage | [`fifo_view`](docs/fifo_view.md) | Same queue logic, user-owned backing memory |
| Real object lifetime, placement new, destructor on pop | [`queue`](docs/queue.md) | Use when `T` is not a good fit for assignment-based slots |
| Fixed-size raw byte buffers | [`pool`](docs/pool.md) | Great for packets, DMA-style buffers, raw slots |
| External raw buffers / DMA-owned storage | [`pool_view`](docs/pool_view.md) | Non-owning raw-slot view |
| Only the newest committed state matters | [`latest`](docs/latest.md) | Not a FIFO |
| Stable typed object slots addressed by pointers | [`typed_pool`](docs/typed_pool.md) | Independent owning typed-slot pool |
| Fixed-size arrays as queue elements | [`array_fifo` family](docs/array_fifo.md) | Zero-copy frame-style producer flow |
| Variable-used blocks with per-block logical size | [`chunk_fifo` family](docs/chunk_fifo.md) | FIFO of `chunk<T,...>` objects |
| Standalone contiguous block container | [`chunk`](docs/chunk.md) | Payload block, not itself an SPSC queue |
| Owning collection of fixed-size buffers | [`buffer_pool`](docs/buffer_pool.md) | Buffer storage helper, not itself an SPSC queue |

## Concurrency Model

Every queue-like container here follows the same core rule:

- exactly one producer context
- exactly one consumer context
- no multiple producers
- no multiple consumers

Valid deployments include:

- task -> task
- ISR -> task
- task -> ISR
- foreground loop -> worker thread

For FreeRTOS and MCU examples, see [Concurrency and FreeRTOS](docs/concurrency-and-freertos.md).

## Policy Cheat Sheet

The policy controls counters, geometry, atomicity, cacheline padding, and where
applicable policy-derived storage alignment. In v3, a container with no
explicit policy uses `spsc::policy::default_policy`, which is `FA<>` when no
legacy override is defined. That is the normal portable
one-producer/one-consumer default.

`SPSC_DEFAULT_POLICY_ATOMIC` is a legacy explicit build override, not a normal
policy-selection API:

| Configuration before including the headers | `default_policy` |
| --- | --- |
| macro undefined | `FA<>` |
| `SPSC_DEFAULT_POLICY_ATOMIC=0` | `P` |
| `SPSC_DEFAULT_POLICY_ATOMIC=1` | `A<>` |

New code should select `local_*`, `concurrent_*`, `cache_aligned_*`, or an
explicit `Policy` rather than set a build-wide override. `P` still requires one
execution context or external synchronization; merely running on one core is
not sufficient when a task and an ISR can preempt each other.

Common ready-made policies from [`base/spsc_policy.hpp`](base/spsc_policy.hpp):

- `spsc::policy::P`: plain counters for one context or external synchronization
- `spsc::policy::V`: volatile counters with plain geometry
- `spsc::policy::VV`: volatile counters and volatile geometry
- `spsc::policy::A<>`: atomic RMW-counter backend
- `spsc::policy::FA<>`: single-writer atomic-counter backend
- `spsc::policy::AA<>`: atomic counters and atomic geometry
- `spsc::policy::CP`, `CV`, `CVV`, `CA<>`, `CFA<>`, `CAA<>`: cacheline-aligned variants

`V`, `VV`, `CV`, and `CVV` do not provide portable acquire/release
synchronization and do not order surrounding non-volatile payload accesses.
Use them only when external synchronization or a documented compiler/platform
contract provides publication ordering. For ordinary task/task, ISR/task, or
task/ISR handoff, start with `A<>` / `FA<>` or their cache-aligned `CA<>` /
`CFA<>` variants.

`CacheAligned` policies pad and align policy-owned metadata. For raw-slot
owning containers such as `pool` and `latest<void>`, the default allocator path
can also derive payload alignment from the policy. For typed contiguous
containers such as `fifo<T,...>` or `queue<T,...>`, payload alignment still
primarily comes from `T` itself. A `*_view` policy cannot realign or resize the
caller-owned backing buffer.

Shadow-index isolation is independent of `CacheAligned`. When the global
shadow switch and counter-width gate allow it, every atomic-backed policy gets
producer- and consumer-owned metadata blocks from the internal `SPSCbase`.
`CacheAligned` policies additionally pad their policy counters and geometry and
propagate allocator-alignment hints. On a 32-bit `reg` domain, shadows remain
off by default unless `SPSC_SHADOW_ALLOW_32BIT=1` is selected explicitly.

`SPSCbase` is an internal implementation base, not a supported public extension
API. Applications should use the concrete containers and views rather than
derive from `SPSCbase` or expose its protected endpoint helpers.

## Semantic Aliases

For new code, choose the concurrency contract from the type name instead of
starting with an explicit policy:

| Use case | Recommended type | Fixed policy |
| --- | --- | --- |
| One execution context or external synchronization | `local_fifo<T, N>` | `P` |
| Normal portable producer/consumer SPSC | `concurrent_fifo<T, N>` | `FA<>` |
| Concurrent SPSC where cache isolation is justified | `cache_aligned_fifo<T, N>` | `CFA<>` |

The same `local_*`, `concurrent_*`, and `cache_aligned_*` scheme exists for
the policy-driven transport types: `fifo`, `queue`, `fifo_view`, `pool`,
`pool_view`, `typed_pool`, `latest`, the `array_fifo` family, and the
`chunk_fifo` family. They are exact compile-time aliases, not runtime wrapper
classes. Owning aliases retain the matching policy-derived default allocator
and still accept a custom allocator in the natural final template position.

For new concurrent SPSC code, prefer `concurrent_*` aliases. Use
`cache_aligned_*` when cache-line isolation is useful for the target and has
been justified by measurement; it is not a universal throughput claim. Use the
full `Container<..., Policy, ...>` form when you intentionally need advanced
policies such as `A<>`, `CA<>`, `V`, `VV`, `CP`, `AA<>`, or `CAA<>`.

In v3, the bare form follows the modern `FA<>` default:
`fifo<T, N>` is equivalent to `concurrent_fifo<T, N>` under the normal
undefined-macro configuration. The semantic aliases never depend on the
default. To preserve a v2 plain bare container's layout and synchronization
contract, migrate it to `local_*` before upgrading. See
[Migrating from v2 to v3](docs/migration-v3.md) for the exact compatibility
table, including explicit legacy macro builds.

`buffer_pool` intentionally has no `local_*`, `concurrent_*`, or
`cache_aligned_*` aliases. It owns storage but has no producer/consumer index;
its policy expresses storage layout. Keep using explicit `policy::CP` for
cache-aligned DMA storage and apply `concurrent_*` or `cache_aligned_*` to the
queue or view that transfers ownership. Legacy `fast_fifo` and `fast_queue`
remain available and still select `CFA<>`.

## Quick Examples

The examples below use the current pointer-based `try_*` API style used by the library.

### `fifo`: simplest owning queue

```cpp
#include "spsc/fifo.hpp"

spsc::fifo<int, 1024> q;

// Producer
(void)q.try_push(42);

// Consumer
if (const int* value = q.try_front()) {
    consume(*value);
    q.pop();
}
```

### `fifo_view`: queue logic over external storage

```cpp
#include "spsc/fifo_view.hpp"
#include <array>

std::array<int, 256> storage{};
spsc::fifo_view<int, 256> q{storage};

q.push(7);

if (int* value = q.try_front()) {
    handle(*value);
    q.pop();
}
```

### `queue`: lifetime-managed objects

```cpp
#include "spsc/queue.hpp"
#include <string>

spsc::queue<std::string, 64> q;

q.emplace("hello");

if (std::string* msg = q.try_front()) {
    log_line(*msg);
    q.pop(); // destroys the stored object
}
```

### `pool`: raw fixed-size buffers

```cpp
#include "spsc/pool.hpp"

using RxPool = spsc::pool<32, spsc::policy::CA<>>;

RxPool q{128}; // 32 slots, each 128 bytes

if (void* slot = q.try_claim()) {
    fill_packet(slot, q.buffer_size());
    q.publish();
}

if (void* slot = q.try_front()) {
    process_packet(slot, q.buffer_size());
    q.pop();
}
```

### `pool_view`: external DMA-style buffers

```cpp
#include "spsc/pool_view.hpp"
#include <cstddef>

constexpr std::size_t kDepth = 8;
constexpr std::size_t kBytes = 128;

alignas(32) static std::byte dmaBuffers[kDepth][kBytes];
static void* slots[kDepth];

for (std::size_t i = 0; i < kDepth; ++i) {
    slots[i] = dmaBuffers[i];
}

spsc::pool_view<kDepth, spsc::policy::CA<>> q{slots, kBytes};
```

### `latest`: newest-state handoff

```cpp
#include "spsc/latest.hpp"

struct Telemetry {
    float temperature;
    float voltage;
};

spsc::latest<Telemetry, 8> latestState;

Telemetry& slot = latestState.claim();
slot = Telemetry{42.0f, 12.1f};
latestState.publish();

if (const Telemetry* view = latestState.try_front()) {
    update_ui(*view);
    latestState.pop();
}
```

### `typed_pool`: one typed object per slot

```cpp
#include "spsc/typed_pool.hpp"
#include <array>
#include <cstddef>
#include <new>

struct Frame {
    std::array<std::byte, 256> payload;
};

spsc::typed_pool<Frame, 16> q;

if (Frame* slot = q.try_claim()) {
    new (slot) Frame{};
    prepare_frame(*slot);
    q.publish();
}

if (Frame* frame = q.try_front()) {
    consume_frame(*frame);
    q.pop();
}
```

### `array_fifo`: fixed-width frames

```cpp
#include "spsc/array_fifo.hpp"
#include <cstdint>

using Frames = spsc::array_fifo<std::uint8_t, 64, 16>;

Frames q;

if (auto* frame = q.try_claim()) {
    frame->fill(0);
    (*frame)[0] = 0xAA;
    (*frame)[1] = 0x55;
    q.publish();
}

if (const auto* frame = q.try_front()) {
    decode_frame(*frame);
    q.pop();
}
```

### `chunk_fifo`: variable-used blocks

```cpp
#include "spsc/chunk_fifo.hpp"
#include <cstdint>

using Blocks = spsc::chunk_fifo<std::uint16_t, 256, 8>;

Blocks q;

if (auto* block = q.try_claim()) {
    block->clear();
    block->push(10);
    block->push(20);
    block->push(30);
    q.publish();
}

if (auto* block = q.try_front()) {
    for (std::uint16_t sample : *block) {
        process_sample(sample);
    }
    q.pop();
}
```

## Snapshots And Bulk Processing

Most queue-like containers support snapshot-style reads:

```cpp
auto snap = q.make_snapshot();

for (const auto& item : snap) {
    process(item);
}

q.consume(snap);
```

This is the easiest read-side pattern when you want:

- stable logical iteration over `[tail, head)`
- batch processing
- producer progress to continue independently while the consumer walks the captured view

For bulk guards, read/write regions, RAII helpers, and more zero-copy flows, see [Guard and Bulk Helpers](docs/guard-and-bulk-helpers.md) and [Method Recipes](docs/method-recipes.md).

## Cache-Aligned And MCU-Oriented Usage

If you target cacheful Cortex-M MCUs such as STM32F7/STM32H7:

- `CacheAligned` policies pad queue metadata to cacheline boundaries
- raw-slot containers such as `pool` and `latest<void>` can derive default payload alignment from the policy
- typed payload alignment still comes from `T` or from an explicit allocator
- D-cache clean/invalidate is a platform concern, not the queue's job
- for STM32F7/STM32H7-style DMA, make sure `SPSC_CACHELINE_BYTES` is 32;
  pass `-DSPSC_FORCE_CACHELINE=32` when your build does not expose a suitable
  Cortex-M or STM32 family macro

Typical STM32F7/STM32H7-oriented raw-slot configuration:

```cpp
using Policy = spsc::policy::CA<>;
using RxPool = spsc::pool<0, Policy>;

RxPool q{8, 100}; // CacheAligned path may round raw slot bytes upward
```

For standalone DMA buffer storage, prefer the non-atomic cache-aligned policy;
the separate transport object owns the concurrent SPSC contract:

```cpp
using DmaBuffers =
    spsc::buffer_pool<std::byte, 100, 8, spsc::policy::CP>;
using Transport = spsc::pool_view<8, spsc::policy::CFA<>>;
```

For `buffer_pool`, `payload_bytes()` is the logical payload and
`cache_span_bytes()` is the policy-rounded physical span. The latter is a cache
maintenance span only when the configured policy alignment matches the target
cache line.

For complete task/ISR/DMA patterns, see [Concurrency and FreeRTOS](docs/concurrency-and-freertos.md).

## Reading Order

If you are new to the library, this order works well:

1. [Common Concepts](docs/common-concepts.md)
2. [Concurrency and FreeRTOS](docs/concurrency-and-freertos.md)
3. [Method Recipes](docs/method-recipes.md)
4. [Guard and Bulk Helpers](docs/guard-and-bulk-helpers.md)
5. The specific container guide you plan to use

## Container Guides

- [fifo](docs/fifo.md)
- [fifo_view](docs/fifo_view.md)
- [queue](docs/queue.md)
- [pool](docs/pool.md)
- [pool_view](docs/pool_view.md)
- [latest](docs/latest.md)
- [typed_pool](docs/typed_pool.md)
- [chunk](docs/chunk.md)
- [array_fifo family](docs/array_fifo.md)
- [chunk_fifo family](docs/chunk_fifo.md)

This overview intentionally avoids duplicating the full reference manual. The split docs are the source of truth for API details and extended recipes.
