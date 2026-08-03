/*
 * STM32 fast-alias assembly probe.
 *
 * Compile this translation unit against a selected revision's src/ include
 * root. With no selector macro it exercises fast_fifo/fast_queue. Define
 * H10_EXPLICIT_CA or H10_EXPLICIT_CFA to isolate the counter backend while
 * keeping the payload and public operations identical.
 */

#include "spsc/fifo.hpp"
#include "spsc/queue.hpp"

#include <cstdint>
#include <type_traits>

namespace {

struct probe_item final {
    std::uint32_t value{0u};

    probe_item() noexcept = default;
    explicit probe_item(const std::uint32_t input) noexcept : value(input) {}
    probe_item(const probe_item &) noexcept = default;
    probe_item(probe_item &&) noexcept = default;
    probe_item &operator=(const probe_item &) noexcept = default;
    probe_item &operator=(probe_item &&) noexcept = default;
    ~probe_item() noexcept = default;
};

using ca_fifo_type = ::spsc::fifo<probe_item, 16u, ::spsc::policy::CA<>>;
using cfa_fifo_type = ::spsc::fifo<probe_item, 16u, ::spsc::policy::CFA<>>;
using ca_queue_type = ::spsc::queue<probe_item, 16u, ::spsc::policy::CA<>>;
using cfa_queue_type = ::spsc::queue<probe_item, 16u, ::spsc::policy::CFA<>>;

static_assert(sizeof(ca_fifo_type) == sizeof(cfa_fifo_type));
static_assert(alignof(ca_fifo_type) == alignof(cfa_fifo_type));
static_assert(sizeof(ca_queue_type) == sizeof(cfa_queue_type));
static_assert(alignof(ca_queue_type) == alignof(cfa_queue_type));

#if defined(H10_EXPLICIT_CA)
using fifo_type = ca_fifo_type;
using queue_type = ca_queue_type;
#elif defined(H10_EXPLICIT_CFA)
using fifo_type = cfa_fifo_type;
using queue_type = cfa_queue_type;
#else
using fifo_type = ::spsc::fast_fifo<probe_item, 16u>;
using queue_type = ::spsc::fast_queue<probe_item, 16u>;
#endif

} // namespace

#if defined(__GNUC__)
#  define H10_NOINLINE __attribute__((noinline, used))
#else
#  define H10_NOINLINE
#endif

extern "C" H10_NOINLINE bool
h10_fast_fifo_push(fifo_type &queue, const std::uint32_t value) noexcept {
    return queue.try_push(probe_item{value});
}

extern "C" H10_NOINLINE bool
h10_fast_fifo_pop(fifo_type &queue, std::uint32_t &value) noexcept {
    const probe_item *const item = queue.try_front();
    if (item == nullptr) {
        return false;
    }
    value = item->value;
    return queue.try_pop();
}

extern "C" H10_NOINLINE bool
h10_fast_queue_push(queue_type &queue, const std::uint32_t value) noexcept {
    return queue.try_emplace(value) != nullptr;
}

extern "C" H10_NOINLINE bool
h10_fast_queue_pop(queue_type &queue, std::uint32_t &value) noexcept {
    const probe_item *const item = queue.try_front();
    if (item == nullptr) {
        return false;
    }
    value = item->value;
    return queue.try_pop();
}

#undef H10_NOINLINE
