# SPSC Quick Start

This folder contains the `spsc` container family: single-producer / single-consumer queues, pools, newest-value buffers, and block wrappers built on the same base.

This page is the short quick start and overview. The full method-by-method reference, container guides, and concurrency recipes live in [`docs/`](docs/README.md).

## Start Here

- [Documentation Hub](docs/README.md)
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
| Stable typed object slots addressed by pointers | [`typed_pool`](docs/typed_pool.md) | Pool semantics with typed slots |
| Fixed-size arrays as queue elements | [`array_fifo` family](docs/array_fifo.md) | Zero-copy frame-style producer flow |
| Variable-used blocks with per-block logical size | [`chunk_fifo` family](docs/chunk_fifo.md) | FIFO of `chunk<T,...>` objects |
| Standalone contiguous block container | [`chunk`](docs/chunk.md) | Payload block, not itself an SPSC queue |

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

The policy controls only the queue metadata behavior: counters, geometry, atomicity, and cacheline padding.
By default, containers use `spsc::policy::default_policy`, which is `A<>` unless
`SPSC_DEFAULT_POLICY_ATOMIC` is explicitly set to `0` before including the library.
If you opt into `P`, treat that instance as single-thread/single-core only; it is
not a task/task or thread/thread synchronization policy.

Common ready-made policies from [`base/spsc_policy.hpp`](base/spsc_policy.hpp):

- `spsc::policy::P`: plain counters, fastest single-core path
- `spsc::policy::V`: volatile producer counters, good fit for ISR -> task on one core
- `spsc::policy::VV`: everything volatile
- `spsc::policy::A<>`: atomic counters, general threaded path
- `spsc::policy::FA<>`: fast single-writer atomic counters
- `spsc::policy::AA<>`: atomic counters and atomic geometry
- `spsc::policy::CP`, `CV`, `CVV`, `CA<>`, `CFA<>`, `CAA<>`: cacheline-aligned variants

`CacheAligned` policies pad and align queue metadata. For raw-slot containers such as `pool` and `latest<void>`, the default allocator path can also derive payload alignment from the policy. For typed contiguous containers such as `fifo<T,...>` or `queue<T,...>`, payload alignment still primarily comes from `T` itself.

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

struct Frame {
    std::array<std::byte, 256> payload;
};

spsc::typed_pool<Frame, 16> q;

if (Frame* slot = q.try_claim()) {
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
    block->push_back(10);
    block->push_back(20);
    block->push_back(30);
    q.publish();
}

if (auto* block = q.try_front()) {
    for (std::uint16_t sample : block->used_span()) {
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

If you target MCUs such as STM32H7:

- `CacheAligned` policies pad queue metadata to cacheline boundaries
- raw-slot containers such as `pool` and `latest<void>` can derive default payload alignment from the policy
- typed payload alignment still comes from `T` or from an explicit allocator
- D-cache clean/invalidate is a platform concern, not the queue's job

Typical STM32H7-oriented raw-slot configuration:

```cpp
using Policy = spsc::policy::CA<>;
using RxPool = spsc::pool<0, Policy>;

RxPool q{8, 100}; // CacheAligned path may round raw slot bytes upward
```

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
