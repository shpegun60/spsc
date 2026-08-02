/* Assembly-only hot-path probe used by run_spsc_baseline.ps1. */

#include <cstdint>

#include "basic_types.h"
#include "spsc/fifo.hpp"
#include "spsc/queue.hpp"

#if defined(_MSC_VER)
#  define SPSC_BENCH_NOINLINE __declspec(noinline)
#else
#  define SPSC_BENCH_NOINLINE __attribute__((noinline))
#endif

using fifo_type = spsc::fifo<std::uint64_t, 1024u, spsc::policy::CFA<>>;
using queue_type = spsc::queue<std::uint64_t, 1024u, spsc::policy::CFA<>>;

extern "C" SPSC_BENCH_NOINLINE bool spsc_fifo_producer(fifo_type &queue,
                                                        const std::uint64_t value) {
    return queue.try_push(value);
}

extern "C" SPSC_BENCH_NOINLINE bool spsc_fifo_consumer(fifo_type &queue,
                                                        std::uint64_t &sink) {
    const auto *value = queue.try_front();
    if (value == nullptr) {
        return false;
    }
    sink += *value;
    queue.pop();
    return true;
}

extern "C" SPSC_BENCH_NOINLINE bool spsc_queue_producer(queue_type &queue,
                                                         const std::uint64_t value) {
    return queue.try_emplace(value) != nullptr;
}

extern "C" SPSC_BENCH_NOINLINE bool spsc_queue_consumer(queue_type &queue,
                                                         std::uint64_t &sink) {
    const auto *value = queue.try_front();
    if (value == nullptr) {
        return false;
    }
    sink += *value;
    queue.pop();
    return true;
}
