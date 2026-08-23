#include "src/spsc/buffer_pool.hpp"
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

// ---------------------------------------------------------------------------
// buffer_pool exception contract: runtime-shaped forms must propagate user
// exceptions after complete cleanup (no leak, previous state preserved),
// while null-returning allocation failure keeps reporting false.
// ---------------------------------------------------------------------------

struct copy_error {};

struct bp_probe {
    static inline bool throw_on_default{false};
    static inline bool throw_on_copy_assign{false};
    static inline int live{0};

    int value{0};

    bp_probe() {
        if (throw_on_default) {
            throw default_error{};
        }
        ++live;
    }
    bp_probe(const bp_probe& other) : value(other.value) { ++live; }
    bp_probe& operator=(const bp_probe& other) {
        if (throw_on_copy_assign) {
            throw copy_error{};
        }
        value = other.value;
        return *this;
    }
    ~bp_probe() noexcept { --live; }
};

struct bp_probe_flags_reset {
    ~bp_probe_flags_reset() noexcept {
        bp_probe::throw_on_default = false;
        bp_probe::throw_on_copy_assign = false;
    }
};

template<class Pool, class Resize, class GrowResize>
[[nodiscard]] bool buffer_pool_exceptions_for_shape(Resize resize,
                                                    GrowResize grow) {
    bp_probe_flags_reset reset_flags{};
    bp_probe::throw_on_default = false;
    bp_probe::throw_on_copy_assign = false;

    const int live_before = bp_probe::live;

    // Baseline shape with recognizable payload values.
    Pool source;
    if (!resize(source) || !source.is_valid()) {
        return false;
    }
    source.data(0u)[0].value = 71;

    // 1. Throwing default construction during a growing resize propagates,
    //    leaks nothing, and preserves the previous shape and payload.
    {
        const int live_valid = bp_probe::live;
        bp_probe::throw_on_default = true;
        bool caught = false;
        try {
            (void)grow(source);
        } catch (const default_error&) {
            caught = true;
        }
        bp_probe::throw_on_default = false;
        if (!caught || bp_probe::live != live_valid || !source.is_valid() ||
            source.data(0u)[0].value != 71) {
            return false;
        }
    }

    // 2. A throwing copy assignment inside the copy constructor propagates
    //    instead of producing a silent empty pool, and leaks nothing.
    {
        const int live_valid = bp_probe::live;
        bp_probe::throw_on_copy_assign = true;
        bool caught = false;
        try {
            Pool copy(source);
            (void)copy;
        } catch (const copy_error&) {
            caught = true;
        }
        bp_probe::throw_on_copy_assign = false;
        if (!caught || bp_probe::live != live_valid ||
            source.data(0u)[0].value != 71) {
            return false;
        }
    }

    // 3. Copy assignment propagates and preserves the destination.
    {
        Pool destination;
        if (!resize(destination)) {
            return false;
        }
        destination.data(0u)[0].value = 88;

        const int live_valid = bp_probe::live;
        bp_probe::throw_on_copy_assign = true;
        bool caught = false;
        try {
            destination = source;
        } catch (const copy_error&) {
            caught = true;
        }
        bp_probe::throw_on_copy_assign = false;
        if (!caught || bp_probe::live != live_valid ||
            !destination.is_valid() || destination.data(0u)[0].value != 88) {
            return false;
        }
    }

    source.destroy();
    return bp_probe::live == live_before;
}

[[nodiscard]] bool buffer_pool_exception_contract_holds() {
    using fixed_size_pool = ::spsc::buffer_pool<bp_probe, 4u, 0u,
                                                ::spsc::policy::P>;
    using fixed_count_pool = ::spsc::buffer_pool<bp_probe, 0u, 4u,
                                                 ::spsc::policy::P>;
    using dynamic_pool = ::spsc::buffer_pool<bp_probe, 0u, 0u,
                                             ::spsc::policy::P>;

    const bool fixed_size_ok =
        buffer_pool_exceptions_for_shape<fixed_size_pool>(
            [](fixed_size_pool& p) { return p.resize(2u); },
            [](fixed_size_pool& p) { return p.resize(3u); });
    const bool fixed_count_ok =
        buffer_pool_exceptions_for_shape<fixed_count_pool>(
            [](fixed_count_pool& p) { return p.resize(2u); },
            [](fixed_count_pool& p) { return p.resize(3u); });
    const bool dynamic_ok =
        buffer_pool_exceptions_for_shape<dynamic_pool>(
            [](dynamic_pool& p) { return p.resize(2u, 2u); },
            [](dynamic_pool& p) { return p.resize(3u, 3u); });

    return fixed_size_ok && fixed_count_ok && dynamic_ok;
}

// Null-returning allocation failure (as opposed to a thrown exception) must
// keep reporting false without throwing and without touching current state.
struct null_alloc_gate {
    static inline bool fail{false};
};

template<class T>
struct null_returning_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = null_returning_allocator<U>; };

    null_returning_allocator() noexcept = default;

    template<class U>
    null_returning_allocator(const null_returning_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(const size_type count) noexcept {
        if (null_alloc_gate::fail) {
            return nullptr;
        }
        return static_cast<pointer>(
            ::operator new(count * sizeof(value_type), std::nothrow));
    }

    void deallocate(pointer ptr, size_type) noexcept {
        ::operator delete(ptr);
    }
};

[[nodiscard]] bool buffer_pool_null_allocation_still_reports_false() {
    using pool_type = ::spsc::buffer_pool<int, 4u, 0u, ::spsc::policy::P,
                                          null_returning_allocator<int>>;

    null_alloc_gate::fail = false;
    pool_type pool;
    if (!pool.resize(2u) || !pool.is_valid()) {
        return false;
    }
    pool.data(0u)[0] = 5;

    null_alloc_gate::fail = true;
    bool grew = true;
    try {
        grew = pool.resize(3u);
    } catch (...) {
        null_alloc_gate::fail = false;
        return false;
    }
    null_alloc_gate::fail = false;

    return !grew && pool.is_valid() && pool.count() == 2u &&
           pool.data(0u)[0] == 5;
}

} // namespace

int main() {
    return fifo_move_propagates_default_failure() &&
                   latest_move_propagates_default_failure() &&
                   chunk_default_propagates_failure() &&
                   throwing_allocator_cleanup_is_complete() &&
                   guards_have_documented_unwind_semantics() &&
                   buffer_pool_exception_contract_holds() &&
                   buffer_pool_null_allocation_still_reports_false()
               ? 0
               : 1;
}
