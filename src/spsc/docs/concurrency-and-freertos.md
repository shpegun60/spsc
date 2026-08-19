# Concurrency and FreeRTOS

This page explains how to choose policies and usage patterns for real concurrent execution, especially on embedded systems and FreeRTOS-style firmware.

## 1. The Core Rule

Each queue instance must have:

- exactly one producer context
- exactly one consumer context

Those contexts may be:

- task -> task
- ISR -> task
- task -> ISR

But they still must remain single-producer and single-consumer for that queue instance.

## 2. What Is Concurrent And What Is Not

Hot-path producer and consumer operations are the concurrent API surface:

- `push`
- `try_push`
- `claim`
- `publish`
- `front`
- `try_front`
- `pop`
- `consume`

Management operations are **not** concurrent:

- `resize`
- `clear`
- `destroy`
- `swap`
- copy / move
- `attach` / `adopt` / state restore

Stop both endpoints before doing management work.

### Public Observation Queries

`size()`, `empty()`, `full()`, `free()`, `can_write()`, `can_read()`,
`write_size()`, and `read_size()` are observations: they do not reserve a slot,
publish data, or consume data.

With an atomic-backed policy (`A<>`, `FA<>`, `AA<>`, `CA<>`, `CFA<>`, or
`CAA<>`), a third monitoring context may call those queries while the one
producer and one consumer run. Each result is bounded and data-race-free, but
it is only an approximate, non-linearizable snapshot. It can be stale as soon
as it returns, and a transient snapshot can conservatively report no readable
or writable space.

Do not use a public observation as a reservation or as a substitute for
`try_push()` / `try_claim()` or `try_front()` / `try_pop()`. The endpoint
operations retain their own role-local cache for the hot path; public queries
intentionally do not modify that cache. They can still read both endpoint
index cache lines, so a monitoring thread can create cache traffic even though
it never touches an endpoint cache.

`P`, `V`, `VV`, `CP`, `CV`, and `CVV` do **not** gain portable concurrent
observer support from these methods. For those policies, an independent
monitoring context needs external synchronization (or must run only while the
queue is stopped).

## 3. Policy Selection

### `P`

Use when:

- there is no real cross-context concurrency
- the queue is effectively local or single-threaded

In v3, the normal bare default is `FA<>`, so do not set a macro merely to make
ordinary task/task or ISR/task handoff atomic. `SPSC_DEFAULT_POLICY_ATOMIC` is
a legacy explicit override: `0` selects `P`, `1` selects strict `A<>`, and an
undefined macro selects the modern `FA<>` default. Prefer `local_*`,
`concurrent_*`, or an explicit policy for new code.

### `V` / `VV`

Use only when all of the following hold:

- an external synchronization mechanism or a documented compiler/platform
  contract orders publication and observation of the payload
- the target-specific behavior has been reviewed and tested
- you intentionally accept a non-portable integration contract

Volatile metadata access alone does not create acquire/release synchronization,
does not establish a C++ happens-before edge, and does not order surrounding
non-volatile payload accesses. A single-core target by itself is not enough.

### `A<>`

Use when:

- you want the atomic backend whose increments use RMW operations
- producer and consumer are different FreeRTOS tasks
- you want a straightforward atomic family starting point

### `FA<>`

Use when:

- exactly one role writes each counter, as required by SPSC
- you want the single-writer atomic backend

`FA<>` increments with a relaxed load followed by a release store rather than
an atomic RMW. That is correct under the normal SPSC ownership contract, but
not when multiple writers touch one counter.

### `AA<>`

Use when:

- you also want geometry metadata atomic
- you have a heavier shared-state scenario

### Cache-Aligned Families

- `CP`, `CV`, `CVV`
- `CA<>`, `CFA<>`, `CAA<>`

Use them when:

- false sharing matters
- metadata layout on cacheful hardware matters
- you want cache-aware metadata placement

`CA<>` is cache-aligned `A<>`, so it retains the strict-RMW backend.
`CFA<>` is cache-aligned `FA<>`, so it retains the single-writer backend.
Neither is more correct for a valid SPSC queue: both require exactly one
producer and one consumer; choose based on the target and measurements.

## 4. FreeRTOS Recommendations

### Task -> Task

One strict-RMW configuration:

```cpp
using Policy = spsc::policy::CA<>;
spsc::fifo<Message, 128, Policy> q;
```

This is a conventional strict-RMW starting point. `CFA<>` is also correct when
the normal one-producer/one-consumer ownership rule holds; it is not a weaker
correctness mode.

Typical fits:

- command FIFOs
- message passing between worker tasks
- control task -> IO task handoff
- decoder task -> UI task state transfer

### ISR -> Task

Two portable atomic choices under the SPSC ownership contract are:

```cpp
using StrictRmw = spsc::policy::CA<>;
using SingleWriter = spsc::policy::CFA<>;
```

Use `CA<>` when you want the strict-RMW atomic backend. `CFA<>` is also correct
when exactly one producer and one consumer own their respective counters.
`V` / `CV` are not standalone ISR/task synchronization policies; use them only
under the explicit external or platform-specific ordering contract described
above.

Typical fits:

- UART RX ISR -> parser task
- ADC / timer ISR -> processing task
- peripheral completion ISR -> worker task

### Task -> ISR

The same SPSC contract still applies:

- one producer task
- one consumer ISR

Again, `A<>` / `CA<>` is a straightforward strict-RMW starting point.
`FA<>` / `CFA<>` remain valid when the exact SPSC single-writer rule holds.

Typical fits:

- task queues prepared TX frames for a peripheral ISR
- task fills DMA descriptors, ISR consumes and arms hardware
- control task hands short commands to a timer ISR

## 5. FreeRTOS Integration Pattern

The queue does not block by itself.  
Use RTOS primitives around it.

```cpp
// Producer task or ISR:
if (q.try_push(msg)) {
    xTaskNotifyGive(consumerTaskHandle);
}

// Consumer task:
for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    while (auto* item = q.try_front()) {
        process(*item);
        q.pop();
    }
}
```

The SPSC container transports the data. FreeRTOS handles wakeup and scheduling.

## 6. Real Task -> Task Examples

### Command FIFO Between Two Tasks

```cpp
#include "fifo.hpp"

struct Command {
    std::uint8_t id;
    std::uint32_t value;
};

using CommandQ = spsc::fifo<Command, 64, spsc::policy::CA<>>;

static CommandQ commandQ;
static TaskHandle_t workerTaskHandle = nullptr;

void ControlTask(void*)
{
    for (;;) {
        Command cmd{7u, read_new_value()};

        if (commandQ.try_push(cmd)) {
            xTaskNotifyGive(workerTaskHandle);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void WorkerTask(void*)
{
    workerTaskHandle = xTaskGetCurrentTaskHandle();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (auto* cmd = commandQ.try_front()) {
            execute_command(*cmd);
            commandQ.pop();
        }
    }
}
```

### Object-Lifetime Queue Between Tasks

Use `queue` when destruction timing matters:

```cpp
#include "queue.hpp"

struct Message {
    std::string text;
    std::uint32_t seq;
};

using MessageQ = spsc::queue<Message, 32, spsc::policy::CA<>>;

static MessageQ messageQ;
static TaskHandle_t loggerTaskHandle = nullptr;

void ProducerTask(void*)
{
    std::uint32_t seq = 0;

    for (;;) {
        if (messageQ.try_emplace(Message{"ready", seq})) {
            xTaskNotifyGive(loggerTaskHandle);
            ++seq;
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void LoggerTask(void*)
{
    loggerTaskHandle = xTaskGetCurrentTaskHandle();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (auto* msg = messageQ.try_front()) {
            log_line(msg->text, msg->seq);
            messageQ.pop(); // destroys Message in-place
        }
    }
}
```

### Newest-State Handoff Between Tasks

This is a better fit for `latest` than for a FIFO when the consumer only needs the freshest state.

```cpp
#include "latest.hpp"

struct Telemetry {
    float voltage;
    float current;
    float temperature;
};

using TelemetryQ = spsc::latest<Telemetry, 8, spsc::policy::CA<>>;

static TelemetryQ telemetryQ;
static TaskHandle_t uiTaskHandle = nullptr;

void SamplerTask(void*)
{
    for (;;) {
        if (Telemetry* slot = telemetryQ.try_claim()) {
            *slot = Telemetry{
                read_voltage(),
                read_current(),
                read_temperature()
            };
            if (telemetryQ.coalescing_publish()) {
                // Notify only when head advanced: false leaves the written
                // slot producer-private and no new value is visible yet.
                xTaskNotifyGive(uiTaskHandle);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void UiTask(void*)
{
    uiTaskHandle = xTaskGetCurrentTaskHandle();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (const Telemetry* view = telemetryQ.try_front()) {
            draw_telemetry(*view);
            telemetryQ.pop();
        }
    }
}
```

## 7. ISR -> Task Examples

### Small POD Messages From ISR

```cpp
#include "fifo.hpp"

using EventQ = spsc::fifo<std::uint32_t, 64, spsc::policy::CA<>>;

static EventQ eventQ;
static TaskHandle_t eventTaskHandle = nullptr;

extern "C" void EXTI15_10_IRQHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    const std::uint32_t stamp = read_timer_counter();
    if (eventQ.try_push(stamp)) {
        vTaskNotifyGiveFromISR(eventTaskHandle, &higherPriorityTaskWoken);
    }

    clear_exti_flag();
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void EventTask(void*)
{
    eventTaskHandle = xTaskGetCurrentTaskHandle();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (auto* stamp = eventQ.try_front()) {
            consume_timestamp(*stamp);
            eventQ.pop();
        }
    }
}
```

### Raw Buffers From ISR With `pool`

```cpp
#include "pool.hpp"

using RxPool = spsc::pool<16, spsc::policy::CA<>>;

static RxPool rxPool{128};
static TaskHandle_t parserTaskHandle = nullptr;

extern "C" void ADC_IRQHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    if (void* slot = rxPool.try_claim()) {
        std::memcpy(slot, adc_dma_shadow_buffer, rxPool.buffer_size());
        rxPool.publish();
        vTaskNotifyGiveFromISR(parserTaskHandle, &higherPriorityTaskWoken);
    }

    clear_adc_irq();
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void ParserTask(void*)
{
    parserTaskHandle = xTaskGetCurrentTaskHandle();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (void* slot = rxPool.try_front()) {
            parse_frame(static_cast<std::byte*>(slot), rxPool.buffer_size());
            rxPool.pop();
        }
    }
}
```

### Platform-Specific Volatile Variant (Expert Opt-In)

If an external synchronization mechanism or the compiler/platform contract
explicitly guarantees payload ordering:

```cpp
using EmbeddedPolicy = spsc::policy::CV;
using SampleQ = spsc::fifo<std::uint16_t, 128, EmbeddedPolicy>;
```

This is not portable C++ synchronization. Do not select `CV` merely because the
MCU has one core; keep `CA<>` or `CFA<>` unless that external contract is both
documented and tested.

## 8. Task -> ISR Examples

### Task Queues TX Frames, ISR Drains Them

```cpp
#include "array_fifo.hpp"

using TxFrames = spsc::array_fifo<std::uint8_t, 64, 16, spsc::policy::CA<>>;

static TxFrames txFrames;

void TxTask(void*)
{
    for (;;) {
        if (auto* frame = txFrames.try_claim()) {
            build_tx_frame(*frame);
            txFrames.publish();
            enable_uart_txe_interrupt();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

extern "C" void USART1_IRQHandler(void)
{
    static std::size_t pos = 0;

    if (auto* frame = txFrames.try_front()) {
        write_uart_byte((*frame)[pos++]);

        if (pos == frame->size()) {
            pos = 0;
            txFrames.pop();
        }
    } else {
        disable_uart_txe_interrupt();
    }
}
```

### Task Prepares Blocks, Timer ISR Consumes One Sample At A Time

```cpp
#include "chunk_fifo.hpp"

using AudioBlocks = spsc::chunk_fifo<std::int16_t, 128, 4, spsc::policy::CA<>>;

static AudioBlocks audioBlocks;
static std::size_t audioIndex = 0;

void AudioFillTask(void*)
{
    for (;;) {
        if (auto* block = audioBlocks.try_claim()) {
            block->clear();
            fill_audio_block(*block);
            audioBlocks.publish();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

extern "C" void TIM6_DAC_IRQHandler(void)
{
    if (auto* block = audioBlocks.try_front()) {
        output_sample((*block)[audioIndex++]);

        if (audioIndex == block->size()) {
            audioIndex = 0;
            audioBlocks.pop();
        }
    } else {
        output_sample(0);
    }

    clear_tim6_irq();
}
```

## 9. ISR-Side Rules

When one side is an ISR:

- prefer `try_*` APIs
- keep the ISR hot path short
- do not call management APIs from the ISR
- publish only fully prepared data

Typical ISR producer pattern:

```cpp
void adc_isr()
{
    if (auto* slot = q.try_claim()) {
        fill_from_peripheral(slot);
        q.publish();
        notify_consumer_task();
    }
}
```

## 10. DMA, Cache, And Embedded Examples

On cacheful Cortex-M MCUs such as `STM32F7` / `STM32H7`:

- `CacheAligned` improves metadata layout
- typed payload alignment still depends on `T` and allocator choice
- raw-slot containers can benefit from policy-derived aligned allocation
- cache maintenance is still outside the queue
- make sure `SPSC_CACHELINE_BYTES` is 32; pass `-DSPSC_FORCE_CACHELINE=32`
  when your build does not expose a suitable Cortex-M or STM32 family macro

Practical advice:

- `pool` / `latest<void>` are strong fits for aligned raw DMA buffers
- `fifo<T>` / `queue<T>` need explicit payload alignment when the payload itself must be line-aware
- `*_view` containers require already-correct external storage; a
  `CacheAligned` policy cannot realign or resize caller-owned backing memory
- avoid sharing a maintained cache line with unrelated dirty data

CMSIS by-address cache maintenance must cover whole cache lines. Keep that in
your platform layer:

```cpp
static_assert((SPSC_CACHELINE_BYTES & (SPSC_CACHELINE_BYTES - 1u)) == 0u);

inline std::size_t dma_cache_span(const void* p,
                                  std::size_t bytes,
                                  std::uintptr_t& alignedBegin)
{
    constexpr std::uintptr_t line = SPSC_CACHELINE_BYTES;
    const auto begin = reinterpret_cast<std::uintptr_t>(p);
    const auto end = begin + bytes;
    alignedBegin = begin & ~(line - 1u);
    const auto alignedEnd = (end + line - 1u) & ~(line - 1u);
    return static_cast<std::size_t>(alignedEnd - alignedBegin);
}

inline void clean_dma_tx_buffer(const void* p, std::size_t bytes)
{
    std::uintptr_t begin = 0u;
    const auto span = dma_cache_span(p, bytes, begin);
    SCB_CleanDCache_by_Addr(reinterpret_cast<std::uint32_t*>(begin),
                            static_cast<std::int32_t>(span));
}

inline void invalidate_dma_rx_buffer(void* p, std::size_t bytes)
{
    std::uintptr_t begin = 0u;
    const auto span = dma_cache_span(p, bytes, begin);
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<std::uint32_t*>(begin),
                                 static_cast<std::int32_t>(span));
}
```

### External DMA Slot Table With `pool_view`

```cpp
#include "pool_view.hpp"

constexpr std::size_t kDepth = 8;
constexpr std::size_t kBytes = 128;

alignas(32) static std::byte rxBuffers[kDepth][kBytes];
static void* rxSlots[kDepth];

using DmaPool = spsc::pool_view<kDepth, spsc::policy::CA<>>;
static DmaPool dmaPool;

void init_dma_pool()
{
    for (std::size_t i = 0; i < kDepth; ++i) {
        rxSlots[i] = rxBuffers[i];
    }

    dmaPool.attach(rxSlots, kBytes);
}
```

### DMA Completion Callback Feeding A Task

```cpp
static TaskHandle_t dmaTaskHandle = nullptr;
static void* activeRxSlot = nullptr;

void arm_next_rx_dma()
{
    if (!activeRxSlot) {
        activeRxSlot = dmaPool.try_claim();
    }

    if (activeRxSlot) {
        invalidate_dma_rx_buffer(activeRxSlot, kBytes);
        start_dma_into(activeRxSlot, kBytes);
    }
}

extern "C" void DMA1_Stream0_IRQHandler(void)
{
    BaseType_t higherPriorityTaskWoken = pdFALSE;

    if (activeRxSlot) {
        invalidate_dma_rx_buffer(activeRxSlot, kBytes);
        finish_dma_rx(activeRxSlot, kBytes);
        dmaPool.publish();
        activeRxSlot = nullptr;
        vTaskNotifyGiveFromISR(dmaTaskHandle, &higherPriorityTaskWoken);
    }

    clear_dma_irq();
    portYIELD_FROM_ISR(higherPriorityTaskWoken);
}

void DmaConsumerTask(void*)
{
    dmaTaskHandle = xTaskGetCurrentTaskHandle();
    arm_next_rx_dma();

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (void* slot = dmaPool.try_front()) {
            process_dma_block(static_cast<std::byte*>(slot), dmaPool.buffer_size());
            dmaPool.pop();
        }

        arm_next_rx_dma();
    }
}
```

### Typed Payload Alignment For Cache-Line Aware FIFOs

```cpp
struct alignas(32) Frame {
    std::uint8_t bytes[128];
};

using FrameQ = spsc::fifo<Frame, 16, spsc::policy::CA<>>;
```

This aligns each frame slot itself. `CA<>` aligns metadata and allocator defaults; `alignas(32)` aligns the payload type.

### Raw Dynamic Slots With Automatic Round-Up

```cpp
using RxPool = spsc::pool<0, spsc::policy::CA<>>;

RxPool q{16, 100};

// Under a 32-byte cacheline policy the physical slot size can become 128.
const auto slotBytes = q.buffer_size();
```

That makes DMA/cache-line management easier around the queue, but the clean/invalidate calls are still platform code.

## 11. FreeRTOS Synchronization Patterns Around The Queue

### Direct-To-Task Notifications

This is the lightest default for one producer waking one consumer:

```cpp
if (q.try_push(item)) {
    xTaskNotifyGive(consumerTaskHandle);
}
```

### Binary Semaphore Wrapper

Use this when multiple producer-side events must share one wakeup primitive:

```cpp
if (q.try_push(item)) {
    xSemaphoreGive(consumerSemaphore);
}

for (;;) {
    xSemaphoreTake(consumerSemaphore, portMAX_DELAY);

    while (auto* item = q.try_front()) {
        process(*item);
        q.pop();
    }
}
```

### Polling Loop For Soft Real-Time Tasks

Sometimes a task already has a periodic loop and does not need blocking waits:

```cpp
for (;;) {
    while (auto* item = q.try_front()) {
        process(*item);
        q.pop();
    }

    vTaskDelay(pdMS_TO_TICKS(1));
}
```

## 12. Stop / Reconfigure / Restart Pattern

Management APIs are not concurrent. A simple practical pattern is:

```cpp
#include <atomic>

static std::atomic<bool> stopRequested{false};
static std::atomic<bool> producerStopped{false};
static std::atomic<bool> consumerStopped{false};

void ProducerTask(void*)
{
    for (;;) {
        if (stopRequested.load(std::memory_order_acquire)) {
            producerStopped.store(true, std::memory_order_release);
            while (stopRequested.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            producerStopped.store(false, std::memory_order_release);
        }

        produce_into_queue();
    }
}

void ConsumerTask(void*)
{
    for (;;) {
        if (stopRequested.load(std::memory_order_acquire)) {
            consumerStopped.store(true, std::memory_order_release);
            while (stopRequested.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
            consumerStopped.store(false, std::memory_order_release);
        }

        consume_from_queue();
    }
}

void ReconfigureQueue()
{
    stopRequested.store(true, std::memory_order_release);

    while (!producerStopped.load(std::memory_order_acquire) ||
           !consumerStopped.load(std::memory_order_acquire)) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    q.clear();
    q.resize(newCapacity);

    stopRequested.store(false, std::memory_order_release);
}
```

The synchronization primitive around the stop request is up to your system; the important part is that `resize`, `clear`, `swap`, `attach`, and `adopt` happen only while both endpoints are quiescent.

## 13. What Not To Do

- do not let two tasks both use producer-side APIs on the same queue
- do not call `resize()` while producer and consumer are active
- do not use `volatile` metadata as standalone task/ISR synchronization
- do not assume the queue performs blocking or cache maintenance for you

Bad pattern:

```cpp
// Wrong: two producer tasks touching the same instance.
void ProducerA(void*) { q.push(1); }
void ProducerB(void*) { q.push(2); }
```

Correct pattern:

```cpp
spsc::fifo<int, 64> qA;
spsc::fifo<int, 64> qB;

// one producer per queue, then merge in a dedicated task if needed
```

## 14. Short Practical Rules

If you want one compact rule set:

- task/task: start with `CA<>`
- ISR/task on an embedded target: start with `CA<>` or `CFA<>`; use `CV` only with documented external/platform ordering
- management operations: only while the queue is stopped
- DMA payloads: solve payload alignment separately from metadata policy
