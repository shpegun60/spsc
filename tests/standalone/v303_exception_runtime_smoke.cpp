#include "src/spsc/chunk.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/pool.hpp"
#include "src/spsc/queue.hpp"

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

#if (SPSC_ENABLE_EXCEPTIONS == 0)
#  error "v303_exception_runtime_smoke.cpp requires SPSC_ENABLE_EXCEPTIONS=1"
#endif

namespace {

struct default_error {};
struct guard_error {};
struct allocation_error {};

struct allocation_tracker {
    static inline std::size_t calls{0u};
    static inline std::size_t live_blocks{0u};
    static inline std::size_t throw_on_call{0u};

    static void reset(const std::size_t throw_on) noexcept {
        calls = 0u;
        live_blocks = 0u;
        throw_on_call = throw_on;
    }
};

template<class T>
struct throwing_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = throwing_allocator<U>; };

    throwing_allocator() noexcept = default;

    template<class U>
    throwing_allocator(const throwing_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(const size_type count) {
        ++allocation_tracker::calls;
        if (allocation_tracker::calls == allocation_tracker::throw_on_call) {
            throw allocation_error{};
        }
        pointer result = static_cast<pointer>(
            ::operator new(count * sizeof(value_type)));
        ++allocation_tracker::live_blocks;
        return result;
    }

    void deallocate(pointer ptr, size_type) noexcept {
        if (ptr != nullptr) {
            --allocation_tracker::live_blocks;
            ::operator delete(ptr);
        }
    }
};

struct throwing_default_noexcept_ops {
    static inline bool throw_on_default{false};

    int value{0};

    throwing_default_noexcept_ops() noexcept(false) {
        if (throw_on_default) {
            throw default_error{};
        }
    }

    throwing_default_noexcept_ops(
        const throwing_default_noexcept_ops&) noexcept = default;
    throwing_default_noexcept_ops(
        throwing_default_noexcept_ops&&) noexcept = default;
    throwing_default_noexcept_ops& operator=(
        const throwing_default_noexcept_ops&) noexcept = default;
    throwing_default_noexcept_ops& operator=(
        throwing_default_noexcept_ops&&) noexcept = default;
    ~throwing_default_noexcept_ops() noexcept = default;
};

using static_fifo =
    ::spsc::fifo<throwing_default_noexcept_ops, 8u, ::spsc::policy::P>;
using static_latest =
    ::spsc::latest<throwing_default_noexcept_ops, 8u, ::spsc::policy::P>;
using static_chunk = ::spsc::chunk<throwing_default_noexcept_ops, 8u>;

static_assert(!std::is_nothrow_move_constructible_v<static_fifo>);
static_assert(!std::is_nothrow_move_constructible_v<static_latest>);
static_assert(!std::is_nothrow_default_constructible_v<static_chunk>);

[[nodiscard]] bool fifo_move_propagates_default_failure() {
    throwing_default_noexcept_ops::throw_on_default = false;
    static_fifo source;
    source.claim().value = 17;
    source.publish();

    throwing_default_noexcept_ops::throw_on_default = true;
    bool caught = false;
    try {
        static_fifo destination(std::move(source));
        (void)destination;
    } catch (const default_error&) {
        caught = true;
    }
    throwing_default_noexcept_ops::throw_on_default = false;

    return caught && source.size() == 1u && source.front().value == 17;
}

[[nodiscard]] bool latest_move_propagates_default_failure() {
    throwing_default_noexcept_ops::throw_on_default = false;
    static_latest source;
    source.claim().value = 23;
    source.publish();

    throwing_default_noexcept_ops::throw_on_default = true;
    bool caught = false;
    try {
        static_latest destination(std::move(source));
        (void)destination;
    } catch (const default_error&) {
        caught = true;
    }
    throwing_default_noexcept_ops::throw_on_default = false;

    const auto* remaining = source.try_front();
    return caught && source.size() == 1u && remaining != nullptr &&
           remaining->value == 23;
}

[[nodiscard]] bool chunk_default_propagates_failure() {
    throwing_default_noexcept_ops::throw_on_default = true;
    bool caught = false;
    try {
        static_chunk value;
        (void)value;
    } catch (const default_error&) {
        caught = true;
    }
    throwing_default_noexcept_ops::throw_on_default = false;
    return caught;
}

[[nodiscard]] bool throwing_allocator_cleanup_is_complete() {
    using dynamic_pool =
        ::spsc::pool<0u, ::spsc::policy::P,
                     throwing_allocator<std::byte>>;

    allocation_tracker::reset(4u);
    dynamic_pool pool;
    bool caught = false;
    try {
        (void)pool.resize(4u, 16u);
    } catch (const allocation_error&) {
        caught = true;
    }

    return caught && allocation_tracker::calls == 4u &&
           allocation_tracker::live_blocks == 0u &&
           pool.capacity() == 0u && !pool.is_valid();
}

[[nodiscard]] bool guards_have_documented_unwind_semantics() {
    ::spsc::queue<int, 8u, ::spsc::policy::P> object_queue;
    bool caught = false;
    try {
        auto guard = object_queue.scoped_write(3u);
        if (guard.emplace_next(31) == nullptr ||
            guard.emplace_next(37) == nullptr) {
            return false;
        }
        throw guard_error{};
    } catch (const guard_error&) {
        caught = true;
    }
    if (!caught || object_queue.size() != 2u ||
        object_queue.try_front() == nullptr ||
        *object_queue.try_front() != 31 || !object_queue.try_pop() ||
        object_queue.try_front() == nullptr ||
        *object_queue.try_front() != 37 || !object_queue.try_pop()) {
        return false;
    }

    ::spsc::fifo<int, 8u, ::spsc::policy::P> fifo;
    caught = false;
    try {
        auto guard = fifo.scoped_write();
        int* slot = guard.get();
        if (slot == nullptr) {
            return false;
        }
        *slot = 41;
        throw guard_error{};
    } catch (const guard_error&) {
        caught = true;
    }
    if (!caught || fifo.try_front() == nullptr || *fifo.try_front() != 41) {
        return false;
    }

    caught = false;
    try {
        auto guard = fifo.scoped_read();
        if (!guard || *guard != 41) {
            return false;
        }
        throw guard_error{};
    } catch (const guard_error&) {
        caught = true;
    }
    if (!caught || !fifo.empty() || !fifo.try_push(43)) {
        return false;
    }

    caught = false;
    try {
        auto guard = fifo.scoped_read();
        try {
            if (!guard || *guard != 43) {
                return false;
            }
            throw guard_error{};
        } catch (...) {
            guard.cancel();
            throw;
        }
    } catch (const guard_error&) {
        caught = true;
    }
    return caught && fifo.try_front() != nullptr && *fifo.try_front() == 43 &&
           fifo.try_pop();
}

} // namespace

int main() {
    return fifo_move_propagates_default_failure() &&
                   latest_move_propagates_default_failure() &&
                   chunk_default_propagates_failure() &&
                   throwing_allocator_cleanup_is_complete() &&
                   guards_have_documented_unwind_semantics()
               ? 0
               : 1;
}
