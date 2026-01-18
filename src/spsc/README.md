
# SPSC FIFO Library

Single-producer / single-consumer (SPSC) ring-buffer library with several layers:

- `spsc::fifo<T, Capacity, Policy>`: generic SPSC ring over `T`.
- `spsc::chunk<T, ChunkCapacity>`: logical "chunk" (block) of `T`.
- `spsc::chunk_fifo<T, ChunkCapacity, FifoCapacity, Policy>`: FIFO of `chunk<T,...>`.
- `spsc::chunk_fifo_view<T, ChunkCapacity, FifoCapacity, Policy>`: FIFO view over user-provided chunk storage.
- `spsc::array_fifo<T, N, FifoCapacity, Policy>`: FIFO of `std::array<T, N>`.
- `spsc::array_fifo_view<T, N, FifoCapacity, Policy>`: FIFO view over user-provided `std::array` storage.

The library is designed for:

- lock-free SPSC queues,
- ISR / DMA producers and thread consumers,
- fixed-size packets (arrays),
- block-based processing (chunks of samples, frames, etc.).

`reg` is an integer type used for capacities/indices (for example `using reg = std::size_t;`).

> **Important:**  
> The entire model is strictly SPSC: exactly one producer thread and exactly one consumer thread accessing a given FIFO instance.

---

## 0. Quick start

Minimal example: plain scalar FIFO between two threads.

```cpp
#include "fifo.hpp"

using reg = std::size_t;

spsc::fifo<int, 1024> q;

// Producer thread
void producer()
{
    int value = 0;
    for (;;) {
        while (!q.try_push(value)) {
            // queue is full, spin / sleep / drop
        }
        ++value;
    }
}

// Consumer thread
void consumer()
{
    for (;;) {
        auto snap = q.make_snapshot();

        for (int& v : snap) {
            handle_value(v);
        }

        q.consume(snap);
    }
}
````

---

## 1. Choosing the right FIFO

Typical mapping from use case to type:

| Use case                                    | Recommended type                                           |
| ------------------------------------------- | ---------------------------------------------------------- |
| Scalar messages / commands                  | `spsc::fifo<T, Capacity, Policy>`                          |
| Fixed-size protocol frames                  | `spsc::array_fifo<std::uint8_t, N, FifoCapacity, Policy>`  |
| Blocks of samples (ADC, audio, FFT windows) | `spsc::chunk_fifo<T, ChunkCapacity, FifoCapacity, Policy>` |
| Using existing external storage (SRAM, DMA) | `*_view` variants (`chunk_fifo_view` / `array_fifo_view`)  |
| Shared memory / mmap regions                | `*_view` with user-provided backing storage                |
| ISR / DMA → worker thread                   | `chunk_fifo` or `chunk_fifo_view`                          |
| Thread → logging thread                     | `fifo<std::string,...>` or `fifo<BufferHandle,...>`        |

---

## 2. Concurrency model

The library assumes a strict SPSC model:

* Exactly **1 producer** and **1 consumer** per FIFO instance.
* Producer and consumer may run on different threads or contexts (e.g. ISR vs thread).
* FIFO **does not** support:

  * multiple producers,
  * multiple consumers,
  * arbitrary concurrent random access to internal storage.

For MPSC or MPMC scenarios, typical patterns are:

* Fan-in: multiple SPSC queues (one per producer) feeding a single "arbiter" thread.
* Fan-out: one producer writing to N SPSC queues, one per consumer.

---

## 3. Core type: `spsc::fifo<T, Capacity, Policy>`

### 3.1. Static vs dynamic capacity

```cpp
#include "fifo.hpp"

// Static geometry: Capacity != 0
spsc::fifo<int, 1024> q_static;

// Dynamic geometry: Capacity == 0
spsc::fifo<int, 0> q_dynamic{1024};   // capacity requested at runtime
```

* `Capacity != 0`

  * Storage is `std::array<T, Capacity>`.
  * No `resize()`. Geometry is compile-time.

* `Capacity == 0`

  * Storage is `std::unique_ptr<T[]>` (or equivalent).
  * Geometry is runtime:

    * via constructor taking `size_type`,
    * or via `bool resize(size_type requested_capacity)`.

Actual capacity is the power-of-two ring size derived from the requested value (implementation-specific).

### 3.2. Producer API (value-based)

For `fifo<T,...>` the producer API is **value-based**:

```cpp
spsc::fifo<int, 1024> q;

// Push by value / move
q.push(1);
q.push(std::move(x));

// Non-blocking push
if (!q.try_push(2)) {
    // queue full
}

// Emplace in-place
q.emplace(10);  // constructs T from Args...

auto opt_ref = q.try_emplace(20);
if (opt_ref) {
    int& ref = opt_ref->get();
    // ref refers to stored element
}
```

Notes:

* `push` / `try_push` accept any `U` such that `T&` is assignable from `U&&`.
* `emplace` / `try_emplace` construct `value_type` and assign it into the slot.
* `T` does not have to be copyable, only assignable/movable as required.

### 3.3. Consumer API

```cpp
// Front access
int&       front_ref  = q.front();
const int& front_cref = std::as_const(q).front();

auto opt_front = q.try_front();
if (opt_front) {
    int& v = opt_front->get();
}

// Pop
q.pop();  // undefined if empty; caller must check !empty()

// Size & capacity
auto n          = q.size();
auto cap        = q.capacity();
auto free_slots = q.free();
```

### 3.4. Iterators and snapshots

`fifo` supports direct iteration over the current logical range:

```cpp
for (int& v : q) {
    process(v);
}
```

There is also a snapshot mechanism:

```cpp
auto snap = q.make_snapshot();

for (int& v : snap) {
    process(v);
}

q.consume(snap);      // advances tail by exactly snap.size()
q.consume_all();      // consume everything
```

Snapshot is a logical view `[tail, head)` captured at some moment:

* Producer may continue to push while the consumer iterates over the snapshot.
* Only `consume(snapshot)` or `consume_all()` advance the consumer index.

---

## 4. Chunks: `spsc::chunk<T, ChunkCapacity>`

A `chunk<T, ChunkCapacity>` is a small buffer of `T` with its own length and capacity.

* `ChunkCapacity != 0`

  * Storage: `std::array<T, ChunkCapacity>`.
  * Capacity is compile-time.

* `ChunkCapacity == 0`

  * Storage: dynamically allocated (`std::unique_ptr<T[]>` or equivalent).
  * Capacity must be set via `reserve()` before use.

Typical API (simplified):

```cpp
#include "chunk.hpp"

spsc::chunk<int, 256> ch;

// Capacity / size
reg cap = ch.capacity();  // 256
reg sz  = ch.size();

// Push
ch.emplace_back(1);
ch.push_back(2);

// Indexing
int x = ch[0];

// Reset
ch.clear();  // size = 0

// Dynamic chunk: ChunkCapacity == 0
spsc::chunk<int, 0> ch_dyn;
ch_dyn.reserve(512);  // allocate backing storage
```

Chunks are intended to be transported as values in `fifo`, `chunk_fifo`, etc.

---

## 5. `chunk_fifo`: FIFO of chunks

```cpp
#include "chunk_fifo.hpp"

template<
    class T,
    reg   ChunkCapacity = 0,
    reg   FifoCapacity  = 0,
    typename Policy     = ::spsc::policy::default_policy
>
class spsc::chunk_fifo;
```

Conceptually:

```cpp
using ChunkT = spsc::chunk<T, ChunkCapacity>;
using Base   = spsc::fifo<ChunkT, FifoCapacity, Policy>;
```

with **value-based producer API disabled**.

### 5.1. Why value-based producers are disabled

For `chunk_fifo` and `chunk_fifo_view` the following are deleted:

```cpp
template<class... Args>
void push(Args&&...) = delete;

template<class... Args>
bool try_push(Args&&...) = delete;

template<class... Args>
reference emplace(Args&&...) = delete;

template<class... Args>
std::optional<std::reference_wrapper<value_type>>
try_emplace(Args&&...) = delete;
```

This hides all `push/try_push/emplace/try_emplace` overloads from the base `fifo`.
The only allowed producer API is **zero-copy**:

```cpp
ChunkT& claim() noexcept;
std::optional<std::reference_wrapper<ChunkT>> try_claim() noexcept;
void publish() noexcept;
```

Pattern:

1. Producer `claim()`s a slot.
2. Fills the chunk in-place.
3. Calls `publish()` to make it visible to the consumer.

### 5.2. Example: static chunk + static fifo

```cpp
using Fifo = spsc::chunk_fifo<int, 256, 1024>;
Fifo q;

// Producer
void producer()
{
    if (auto opt = q.try_claim()) {
        auto& ch = opt->get();   // chunk<int,256>&

        ch.clear();              // reset logical size

        while (ch.free() > 0) {
            ch.emplace_back(next_sample());
        }

        q.publish();
    }
}

// Consumer
void consumer()
{
    auto snap = q.make_snapshot();

    for (auto& ch : snap) {
        for (reg i = 0; i < ch.size(); ++i) {
            handle(ch[i]);
        }
    }

    q.consume(snap);
}
```

---

## 6. Initializing dynamic chunks in `chunk_fifo`

### 6.1. Case A: `chunk_fifo<int, 0, 1024>`

* `ChunkCapacity = 0` → dynamic chunks.
* `FifoCapacity = 1024` → static ring of 1024 slots.

```cpp
using Fifo = spsc::chunk_fifo<int, 0, 1024>;
Fifo q;

constexpr reg kChunkCapacity = 512;

// Single-threaded initialization via data()
auto* chunks = q.data();            // pointer to underlying ChunkT array
const reg cap = q.capacity();       // 1024, or pow2-processed version

for (reg i = 0; i < cap; ++i) {
    auto& ch = chunks[i];
    ch.reserve(kChunkCapacity);     // allocate internal buffer
    ch.clear();                     // logical size = 0
}
```

After this, the producer reuses already allocated chunks:

```cpp
void producer(Fifo& q)
{
    if (auto opt = q.try_claim()) {
        auto& ch = opt->get();

        ch.clear();

        while (ch.free() > 0) {
            ch.emplace_back(next_sample());
        }

        q.publish();
    }
}
```

> **Note:** `data()` + bulk initialization is intended to be used **before** the queue enters SPSC mode (before threads start).

### 6.2. Case B: `chunk_fifo<int, 0, 0>` (fully dynamic)

* `ChunkCapacity = 0` → dynamic chunks.
* `FifoCapacity = 0` → dynamic fifo geometry.

```cpp
using Fifo = spsc::chunk_fifo<int, 0, 0>;

// Construct with requested capacity
constexpr reg kSlots         = 1024;
constexpr reg kChunkCapacity = 512;

Fifo q{kSlots};     // or q.resize(kSlots);

// Initialize all chunks
auto* chunks = q.data();
const reg cap = q.capacity();

for (reg i = 0; i < cap; ++i) {
    auto& ch = chunks[i];
    ch.reserve(kChunkCapacity);
    ch.clear();
}
```

Then usage is identical to Case A.

---

## 7. `chunk_fifo_view`: FIFO view over external chunk storage

```cpp
#include "chunk_fifo.hpp"

template<
    class T,
    reg   ChunkCapacity,
    reg   FifoCapacity  = 0,
    typename Policy     = ::spsc::policy::default_policy
>
class spsc::chunk_fifo_view;
```

`chunk_fifo_view` wraps **user-provided** storage:

```cpp
using Chunk    = spsc::chunk<int, 0>;
using FifoView = spsc::chunk_fifo_view<int, 0, 0>;

constexpr reg kRequestedSlots = 1024;

// Backing array (must outlive the view)
static Chunk backing[kRequestedSlots];

// Dynamic fifo view: buffer + buffer_capacity
FifoView q{backing, kRequestedSlots};
```

For `FifoCapacity == 0`, the effective capacity is a power-of-two derived from `buffer_capacity`.

### 7.1. Initializing dynamic chunks via view

```cpp
constexpr reg kChunkCapacity = 512;

Chunk* chunks = q.data();
const reg cap = q.capacity();

for (reg i = 0; i < cap; ++i) {
    auto& ch = chunks[i];
    ch.reserve(kChunkCapacity);
    ch.clear();
}
```

Producer/consumer code is the same as for `chunk_fifo`.

### 7.2. Example: `chunk_fifo_view<int, 1000, 0>`

Here:

* `ChunkCapacity = 1000` → `chunk<int,1000>` with fixed storage.
* `FifoCapacity = 0` → dynamic geometry.

```cpp
using Chunk    = spsc::chunk<int, 1000>;
using FifoView = spsc::chunk_fifo_view<int, 1000, 0>;

constexpr reg kRequestedSlots = 1024;
static Chunk backing[kRequestedSlots];

FifoView q{backing, kRequestedSlots};

// Optional: clear all chunks before use
Chunk* chunks = q.data();
const reg cap = q.capacity();

for (reg i = 0; i < cap; ++i) {
    chunks[i].clear();
}
```

No `reserve()` is needed because chunk capacity is static.

---

## 8. `array_fifo`: FIFO of `std::array<T, N>`

```cpp
#include "array_fifo.hpp"

template<
    class T,
    reg   N,
    reg   FifoCapacity = 0,
    typename Policy    = ::spsc::policy::default_policy
>
class spsc::array_fifo;
```

Conceptually:

```cpp
using array_type = std::array<T, N>;
using Base       = spsc::fifo<array_type, FifoCapacity, Policy>;
```

with **value-based producers disabled**, similar to `chunk_fifo`:

* No `push/try_push/emplace/try_emplace`.
* Only `claim()/try_claim()/publish()`.

Example:

```cpp
using Fifo = spsc::array_fifo<std::uint8_t, 256, 1024>;
Fifo q;

// Producer: prepare fixed-size frame
void producer()
{
    if (auto opt = q.try_claim()) {
        auto& frame = opt->get();   // std::array<std::uint8_t,256>&

        // Fill frame
        frame[0] = header;
        // ...

        q.publish();
    }
}

// Consumer
void consumer()
{
    auto snap = q.make_snapshot();

    for (auto& frame : snap) {
        process_frame(frame);
    }

    q.consume(snap);
}
```

---

## 9. `array_fifo_view`: FIFO view over `std::array<T, N>`

```cpp
#include "array_fifo.hpp"

template<
    class T,
    reg   N,
    reg   FifoCapacity = 0,
    typename Policy    = ::spsc::policy::default_policy
>
class spsc::array_fifo_view;
```

Usage is similar to `chunk_fifo_view`, but backing storage is `std::array<T, N>` provided by the user:

```cpp
using Frame    = std::array<std::uint8_t, 256>;
using FifoView = spsc::array_fifo_view<std::uint8_t, 256, 0>;

constexpr reg kRequestedSlots = 512;
static Frame backing[kRequestedSlots];

FifoView q{backing, kRequestedSlots};

// Producer / consumer code identical to array_fifo.
```

---


## 10. Policies

All FIFO types are parameterized by a `Policy` that controls the low-level representation of indices and geometry:

- `counter_type` – type used for `head` / `tail` indices.
- `geometry_type` – type used for capacity / mask / other static geometry.

Both types are "counter-like" wrappers defined in the library:

- `PlainCounter<reg>`
- `VolatileCounter<reg>`
- `AtomicCounter<reg, Orders>`
- `CachelineCounter<Counter, AlignB>`

The main policy template is:

```cpp
template<class Cnt = PlainCounter<reg>, class Geo = Cnt>
struct Policy {
    using counter_type  = Cnt;
    using geometry_type = Geo;
};
````

### 10.1. Ready-made policies

The library provides several aliases in `spsc::policy`:

```cpp
using P  = Policy<>;
// Plain counters for both indices and geometry.
// Fastest option for single-core or relaxed environments.

using V  = Policy<VolatileCounter<reg>, PlainCounter<reg>>;
// Volatile head/tail, plain geometry.
// Useful for ISR <-> task setups on MCUs.

using VV = Policy<VolatileCounter<reg>, VolatileCounter<reg>>;
// Everything volatile. Strict propagation of updates.

template<class O = default_orders>
using A  = Policy<AtomicCounter<reg, O>, PlainCounter<reg>>;
// Atomic counters with configurable memory orders, plain geometry.

template<class O = default_orders>
using AA = Policy<AtomicCounter<reg, O>, AtomicCounter<reg, O>>;
// Atomic counters for both indices and geometry.
```

Typical usage:

```cpp
#include "spsc_policy.hpp"
#include "fifo.hpp"

using FifoPlain    = spsc::fifo<int, 1024, spsc::policy::P>;
using FifoVolatile = spsc::fifo<int, 1024, spsc::policy::V>;
using FifoAtomic   = spsc::fifo<int, 1024, spsc::policy::A<>>;
```

### 10.2. `default_policy`

`default_policy` is a compile-time alias controlled by the macro `SPSC_DEFAULT_POLICY_ATOMIC`:

```cpp
using default_policy =
    std::conditional_t<SPSC_DEFAULT_POLICY_ATOMIC, A<>, P>;
```

* `SPSC_DEFAULT_POLICY_ATOMIC == 0` → `default_policy` is `P` (plain counters).
* `SPSC_DEFAULT_POLICY_ATOMIC != 0` → `default_policy` is `A<>` (atomic counters with `default_orders`).

All FIFO types use `default_policy` if no explicit policy is provided:

```cpp
using FifoDefault = spsc::fifo<int, 1024>;  // uses default_policy
```

### 10.3. Cache-line aligned policies

For multi-core or heavy shared-memory scenarios, the library provides `CacheAligned`:

```cpp
template<
    class Base  = default_policy,
    reg   CAlign = ::spsc::hw::cacheline_bytes,
    reg   GAlign = CAlign
>
struct CacheAligned;
```

It wraps an existing policy and pads its counters and geometry to cache-line boundaries:

```cpp
using AtomicPolicy      = spsc::policy::A<>;
using AtomicAligned     = spsc::policy::CacheAligned<AtomicPolicy>;
using AtomicAlignedFifo = spsc::fifo<int, 1024, AtomicAligned>;
```

This reduces false sharing when producer and consumer run on different cores.

---

## 11. Usage patterns and recipes

This section contains ready-to-use patterns built on top of `fifo`, `chunk_fifo`, and friends.

### 11.1. DMA / ADC data pipeline (`chunk_fifo`)

* ISR/DMA callback acts as producer.
* Worker thread acts as consumer.

```cpp
using Fifo = spsc::chunk_fifo<int16_t, 256, 1024>;
Fifo q;

// DMA callback (producer)
void dma_callback()
{
    if (auto opt = q.try_claim()) {
        auto& ch = opt->get();
        ch.clear();

        // Copy 256 samples from DMA buffer
        for (reg i = 0; i < ch.capacity(); ++i) {
            ch[i] = dma_buffer[i];
        }
        // Optionally set logical size explicitly:
        // ch.set_size(ch.capacity());

        q.publish();
    }
}

// Processing thread (consumer)
void processing_thread()
{
    while (running) {
        auto snap = q.make_snapshot();

        for (auto& ch : snap) {
            run_fft(ch.data(), ch.size());
        }

        q.consume(snap);
    }
}
```

---

### 11.2. Modbus / SPI / UART frames (`array_fifo`)

`array_fifo<uint8_t, N>` is ideal for fixed-size protocol frames:

```cpp
using RxFifo = spsc::array_fifo<std::uint8_t, 64, 256>;
RxFifo rx_frames;

// Protocol stack calls this when one complete frame is received
void on_rx_frame(const std::uint8_t* buf, std::size_t len)
{
    if (len != 64) {
        return; // unexpected size
    }

    if (auto opt = rx_frames.try_claim()) {
        auto& frame = opt->get();
        std::memcpy(frame.data(), buf, 64);
        rx_frames.publish();
    }
}

// Higher-level protocol thread
void protocol_thread()
{
    auto snap = rx_frames.make_snapshot();

    for (auto& frame : snap) {
        handle_frame(frame);
    }

    rx_frames.consume(snap);
}
```

---

### 11.3. Logging queue (`fifo<std::string>`)

Producer threads enqueue log messages, a dedicated logger thread flushes them to disk or console.

```cpp
using LogFifo = spsc::fifo<std::string, 0>;  // dynamic capacity

LogFifo log_q{1024}; // 1024 slots

// Producer (any thread)
void log_info(const char* msg)
{
    std::string s = msg;

    if (!log_q.try_push(std::move(s))) {
        // Optional: count dropped logs
    }
}

// Consumer (logger thread)
void logger_thread()
{
    for (;;) {
        auto snap = log_q.make_snapshot();

        for (auto& entry : snap) {
            write_to_file(entry);
        }

        log_q.consume(snap);
        flush_file();
    }
}
```

`fifo` works with move-only types as long as they are assignable/movable into the slot.

---

### 11.4. Drop-old strategy (latest-oriented)

Sometimes only the most recent data matters (for example telemetry). Older data can be dropped.

```cpp
using Fifo = spsc::fifo<Stats, 64>;
Fifo q;

// Producer: drop oldest if full
void produce_stats(const Stats& s)
{
    if (!q.try_push(s)) {
        q.pop();      // drop oldest
        q.push(s);    // now guaranteed to succeed
    }
}

// Consumer: process all accumulated stats
void consume_stats()
{
    auto snap = q.make_snapshot();

    for (auto& s : snap) {
        apply_stats(s);
    }

    q.consume(snap);
}
```

This pattern keeps the queue from stalling when the consumer is slower than the producer, while always retaining the latest data.

---

### 11.5. Multi-stage pipeline with snapshots

Snapshots make it easy to build multi-stage pipelines where one stage is a consumer for one FIFO and a producer for another.

```cpp
using RawChunkFifo     = spsc::chunk_fifo<float, 256, 1024>;
using SpectraChunkFifo = spsc::chunk_fifo<std::complex<float>, 256, 1024>;

RawChunkFifo     raw_q;
SpectraChunkFifo spectra_q;

// Stage 1: ADC DMA → raw_q (producer as in previous example)

// Stage 2: FFT worker: raw_q -> spectra_q
void fft_thread()
{
    for (;;) {
        auto snap = raw_q.make_snapshot();

        for (auto& ch_in : snap) {
            auto opt = spectra_q.try_claim();
            if (!opt) {
                // Optional: drop or wait; here we drop
                continue;
            }

            auto& ch_out = opt->get();
            ch_out.clear();

            run_fft(ch_in.data(), ch_in.size(), ch_out.data());
            // ch_out.set_size(ch_in.size());

            spectra_q.publish();
        }

        raw_q.consume(snap);
    }
}

// Stage 3: UI / logger consumes spectra_q
```

Each stage is a separate SPSC queue; snapshots provide stable views of producer data while the producer continues to work.

---

### 11.6. `array_fifo_view` over DMA circular rings

`array_fifo_view` can wrap an existing ring buffer that is already used by DMA in circular mode.

```cpp
using Frame = std::array<std::uint8_t, 32>;

// DMA writes into this ring in circular mode
static Frame dma_rx_ring[128];

using RxView = spsc::array_fifo_view<std::uint8_t, 32, 0>;
RxView rx_q{dma_rx_ring, 128};  // capacity derived from 128

// Initialization: set up internal head/tail indices
// (implementation-specific, usually done once before DMA start)

// DMA ISR: called when a full frame has been written into next slot
void dma_rx_irq_handler()
{
    // Implementation may use preclaimed slots; conceptual example:
    rx_q.publish();
}

// Consumer thread
void protocol_thread()
{
    for (;;) {
        auto snap = rx_q.make_snapshot();

        for (auto& frame : snap) {
            handle_frame(frame);
        }

        rx_q.consume(snap);
    }
}
```

`*_view` types do not own the storage and assume the backing array outlives the view.


---

### 11.7. Shared memory / mmap with `chunk_fifo_view<std::byte, 0, 0>`

Advanced use case: use `chunk_fifo_view` to describe a layout in shared memory or an mmap region.

```cpp
using ByteChunk = spsc::chunk<std::byte, 0>;
using FifoView  = spsc::chunk_fifo_view<std::byte, 0, 0>;

// mmap returns raw pointer to shared region
void* shm_ptr = mmap(...);
auto* chunks  = static_cast<ByteChunk*>(shm_ptr);

constexpr reg kRequestedSlots = 1024;

// Construct view over shared memory
FifoView q{chunks, kRequestedSlots};

// Single-threaded initialization before sharing
for (reg i = 0; i < q.capacity(); ++i) {
    chunks[i].reserve(4096);
    chunks[i].clear();
}
```

The FIFO types only define the in-memory layout and access pattern. Cross-process memory ordering and synchronization are the responsibility of the caller.

---

### 11.8. Move-only types (`std::unique_ptr` handles)

`fifo` can transport move-only handles such as `std::unique_ptr`.

```cpp
struct BufferHandle {
    std::unique_ptr<std::uint8_t[]> data;
    reg size{};
};

using BufFifo = spsc::fifo<BufferHandle, 256>;
BufFifo buf_q;

// Producer
void produce_buffer()
{
    BufferHandle h;
    h.size = 1024;
    h.data = std::make_unique<std::uint8_t[]>(h.size);

    fill_buffer(h.data.get(), h.size);

    if (!buf_q.try_push(std::move(h))) {
        // Drop or reuse handle as needed
    }
}

// Consumer
void consume_buffer()
{
    auto snap = buf_q.make_snapshot();

    for (auto& h : snap) {
        handle_buffer(h.data.get(), h.size);
    }

    buf_q.consume(snap);
}
```

---

## 12. Error handling & overflow strategies

The FIFO API is non-blocking and does not enforce any specific overflow behavior. Common strategies:

1. **Drop-new** (default):

   * If `try_push()` fails, simply skip the write.
   * Suitable for lossy telemetry or optional data.

2. **Drop-old**:

   * If `try_push()` fails, call `pop()` and then `push()`.
   * Ensures the queue always contains the most recent values.

3. **Backpressure wrapper**:

   * Wrap `try_push()` in a helper that waits/retries until space is available (spin, sleep, wait on event).
   * Do not build blocking behavior into interrupt handlers.

4. **Statistics and monitoring**:

   * Count failed `try_push()` calls for diagnostics.
   * Optionally expose queue `free()` and `size()` to monitor load.

---

## 13. Best practices & pitfalls

1. **SPSC only**

   * Exactly 1 producer and 1 consumer per FIFO instance.
   * No multiple producers, no multiple consumers.

2. **Initialization**

   * For dynamic chunks (`ChunkCapacity == 0`), call `reserve()` on each chunk **before** starting concurrent use.
   * For dynamic FIFOs (`FifoCapacity == 0`), set capacity once (`ctor` or `resize`) **before** SPSC starts.

3. **Direct `data()` access**

   * Allowed and useful for initial bulk setup (for example pre-reserving chunks).
   * Do not modify storage via `data()` concurrently with producer/consumer operations.

4. **`resize()`**

   * Do not call `resize()` while producer or consumer are running.
   * Changing geometry is a single-threaded operation.

5. **Value-based producers**

   * In `fifo<T,...>`:

     * `push/try_push/emplace/try_emplace` are allowed and intended.
   * In `chunk_fifo` / `chunk_fifo_view` / `array_fifo` / `array_fifo_view`:

     * Value-based producers are intentionally disabled.
     * Use `claim()/try_claim()/publish()` to avoid accidental copies and enforce zero-copy semantics.

6. **Snapshots**

   * A snapshot is a logical view; producer can continue to push while the consumer processes the snapshot.
   * Only `consume(snapshot)` / `consume_all()` advance the tail.
   * A snapshot does not prevent overwriting of elements that are not yet consumed; follow the SPSC contract.

7. **Type choice**

   * Use `chunk_fifo` for variable processing length over fixed-size blocks (such as FFT windows).
   * Use `array_fifo` for fixed-size protocol frames or messages.
   * Use `fifo` for scalar messages, move-only handles, and general-purpose SPSC communication.

---

## 14. Quick reference

### 14.1. FIFO types

```cpp
spsc::fifo<T, Capacity, Policy>                  // generic SPSC fifo
spsc::chunk<T, ChunkCapacity>                    // logical chunk of T
spsc::chunk_fifo<T, ChunkCapacity, FifoCapacity, Policy>
spsc::chunk_fifo_view<T, ChunkCapacity, FifoCapacity, Policy>
spsc::array_fifo<T, N, FifoCapacity, Policy>
spsc::array_fifo_view<T, N, FifoCapacity, Policy>
```

### 14.2. Producer API

* `fifo<T,...>`:

  * `push(U&&)`
  * `try_push(U&&)`
  * `emplace(Args&&...)`
  * `try_emplace(Args&&...)`

* `chunk_fifo` / `chunk_fifo_view` / `array_fifo` / `array_fifo_view`:

  * `ChunkT& claim()`
  * `std::optional<std::reference_wrapper<ChunkT>> try_claim()`
  * `void publish()`

### 14.3. Consumer API (all fifo-like types)

* `bool empty()`
* `bool full()`
* `size_type size()`
* `size_type capacity()`
* `size_type free()`
* `reference front()`
* `std::optional<std::reference_wrapper<value_type>> try_front()`
* `void pop()`
* Iteration: `begin()/end()`, `rbegin()/rend()`
* Snapshots: `make_snapshot()`, `consume(snapshot)`, `consume_all()`

---

This README covers:

* Core scalar FIFO usage.
* Chunk and chunk-based FIFOs for block processing.
* Dynamic vs static geometry.
* Views over external storage.
* Policies and mapping to MCU vs desktop environments.
* Practical patterns (DMA, protocols, logging, pipelines, shared memory).

Drop this file in the SPSC library root as `README.md` and pretend the system was always this well documented.

```
```
