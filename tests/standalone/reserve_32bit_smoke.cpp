#include "src/spsc/fifo.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/queue.hpp"
#include "src/tests/test_reserve_allocator.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

namespace {

using allocator = ::spsc::test::reserve_probe_allocator<std::byte>;

constexpr reg kLimit = ::spsc::cap::RB_MAX_UNAMBIGUOUS;
constexpr reg kAboveLimit = static_cast<reg>(kLimit + reg{1u});
constexpr reg kRegMax = std::numeric_limits<reg>::max();
constexpr std::uint32_t kValue = 0xA5C35A3Cu;

static_assert(sizeof(reg) == 4u,
              "reserve_32bit_smoke must run on a genuine 32-bit target");
static_assert(kAboveLimit > kLimit);
static_assert(kRegMax > kLimit);

[[nodiscard]] bool rejected_before_allocation() noexcept
{
    return ::spsc::test::reserve_allocator_stats::allocation_calls == 0u;
}

[[nodiscard]] bool legal_limit_reached_allocator() noexcept
{
    return ::spsc::test::reserve_allocator_stats::allocation_calls == 1u &&
           ::spsc::test::reserve_allocator_stats::last_allocation_count ==
               static_cast<std::size_t>(kLimit);
}

[[nodiscard]] bool fifo_contract()
{
    using queue_type =
        ::spsc::fifo<std::uint32_t, 0u, ::spsc::policy::P, allocator>;

    queue_type queue;
    if (!queue.reserve(8u) || queue.capacity() < 8u ||
        !queue.try_push(kValue)) {
        return false;
    }

    const reg capacity_before = queue.capacity();

    ::spsc::test::reserve_allocator_stats::reset();
    if (queue.reserve(kLimit) || !legal_limit_reached_allocator() ||
        queue.capacity() != capacity_before || queue.size() != 1u ||
        queue.front() != kValue) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    if (queue.reserve(kAboveLimit) || !rejected_before_allocation() ||
        queue.capacity() != capacity_before || queue.size() != 1u) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    return !queue.reserve(kRegMax) && rejected_before_allocation() &&
           queue.capacity() == capacity_before && queue.size() == 1u &&
           queue.front() == kValue;
}

[[nodiscard]] bool object_queue_contract()
{
    using queue_type =
        ::spsc::queue<std::uint32_t, 0u, ::spsc::policy::P, allocator>;

    queue_type queue;
    if (!queue.reserve(8u) || queue.capacity() < 8u ||
        !queue.try_push(kValue)) {
        return false;
    }

    const reg capacity_before = queue.capacity();

    ::spsc::test::reserve_allocator_stats::reset();
    if (queue.reserve(kLimit) || !legal_limit_reached_allocator() ||
        queue.capacity() != capacity_before || queue.size() != 1u ||
        queue.front() != kValue) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    if (queue.reserve(kAboveLimit) || !rejected_before_allocation() ||
        queue.capacity() != capacity_before || queue.size() != 1u) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    return !queue.reserve(kRegMax) && rejected_before_allocation() &&
           queue.capacity() == capacity_before && queue.size() == 1u &&
           queue.front() == kValue;
}

[[nodiscard]] bool typed_latest_contract()
{
    using latest_type =
        ::spsc::latest<std::uint32_t, 0u, ::spsc::policy::P, allocator>;

    latest_type latest;
    if (!latest.reserve(8u) || latest.capacity() < 8u ||
        !latest.try_push(kValue)) {
        return false;
    }

    const reg capacity_before = latest.capacity();

    ::spsc::test::reserve_allocator_stats::reset();
    if (latest.reserve(kLimit) || !legal_limit_reached_allocator() ||
        latest.capacity() != capacity_before || latest.size() != 1u ||
        latest.front() != kValue) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    if (latest.reserve(kAboveLimit) || !rejected_before_allocation() ||
        latest.capacity() != capacity_before || latest.size() != 1u) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    return !latest.reserve(kRegMax) && rejected_before_allocation() &&
           latest.capacity() == capacity_before && latest.size() == 1u &&
           latest.front() == kValue;
}

[[nodiscard]] bool raw_latest_contract()
{
    using latest_type =
        ::spsc::latest<void, 0u, ::spsc::policy::P, allocator>;

    latest_type empty;
    ::spsc::test::reserve_allocator_stats::reset();
    if (empty.reserve(0u, sizeof(kValue)) ||
        !rejected_before_allocation() || empty.is_valid()) {
        return false;
    }

    latest_type latest;
    if (!latest.reserve(8u, sizeof(kValue)) || latest.capacity() < 8u ||
        latest.buffer_size() < sizeof(kValue) || !latest.try_push(kValue)) {
        return false;
    }

    const reg capacity_before = latest.capacity();
    const reg bytes_before = latest.buffer_size();

    ::spsc::test::reserve_allocator_stats::reset();
    if (latest.reserve(kLimit, bytes_before) ||
        !legal_limit_reached_allocator() ||
        latest.capacity() != capacity_before ||
        latest.buffer_size() != bytes_before || latest.size() != 1u) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    if (latest.reserve(kAboveLimit, bytes_before) ||
        !rejected_before_allocation() ||
        latest.capacity() != capacity_before ||
        latest.buffer_size() != bytes_before || latest.size() != 1u) {
        return false;
    }

    ::spsc::test::reserve_allocator_stats::reset();
    if (latest.reserve(kRegMax, bytes_before) ||
        !rejected_before_allocation() ||
        latest.capacity() != capacity_before ||
        latest.buffer_size() != bytes_before || latest.size() != 1u) {
        return false;
    }

    std::uint32_t out{0u};
    std::memcpy(&out, latest.front(), sizeof(out));
    return out == kValue;
}

} // namespace

int main()
{
    if (!fifo_contract()) {
        return 1;
    }
    if (!object_queue_contract()) {
        return 2;
    }
    if (!typed_latest_contract()) {
        return 3;
    }
    return raw_latest_contract() ? 0 : 4;
}
