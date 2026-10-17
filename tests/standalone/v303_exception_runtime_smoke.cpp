#include "src/spsc/buffer_pool.hpp"
#include "src/spsc/chunk.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/pool.hpp"
#include "src/spsc/queue.hpp"
#include "src/spsc/typed_pool.hpp"

#include <array>
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

// Fail at every position of a live prefix, including after partial migration.
// Check both object lifetimes and raw allocation ownership; all checks remain
// active even when compiled with NDEBUG.
struct payload_error {};

struct relocation_probe {
    static inline int live{0};
    static inline unsigned copies{0u};
    static inline unsigned assignments{0u};
    static inline unsigned fail_copy{0u};
    static inline unsigned fail_assignment{0u};

    int value{0};

    relocation_probe() noexcept { ++live; }
    explicit relocation_probe(const int input) noexcept : value(input) { ++live; }
    relocation_probe(const relocation_probe& other) : value(other.value) {
        if (++copies == fail_copy) {
            throw payload_error{};
        }
        ++live;
    }
    relocation_probe(relocation_probe&& other) noexcept(false)
        : value(other.value) {
        ++live;
        other.value = -1;
    }
    relocation_probe& operator=(const relocation_probe& other) {
        if (++assignments == fail_assignment) {
            throw payload_error{};
        }
        value = other.value;
        return *this;
    }
    relocation_probe& operator=(relocation_probe&& other) noexcept(false) {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~relocation_probe() noexcept { --live; }

    static void fail_on(const unsigned copy, const unsigned assignment) noexcept {
        copies = assignments = 0u;
        fail_copy = copy;
        fail_assignment = assignment;
    }
};

using relocation_allocator = throwing_allocator<relocation_probe>;

[[nodiscard]] int payload_value(const relocation_probe& value) noexcept {
    return value.value;
}
[[nodiscard]] int payload_value(const relocation_probe* const value) noexcept {
    return value != nullptr ? value->value : -1;
}

template<class Container>
[[nodiscard]] bool has_original_prefix(const Container& container) {
    return container.size() == 3u && payload_value(container[0u]) == 1 &&
           payload_value(container[1u]) == 2 &&
           payload_value(container[2u]) == 3;
}

template<class Action>
[[nodiscard]] bool catches_payload_failure(Action action) {
    bool caught = false;
    try {
        action();
    } catch (const payload_error&) {
        caught = true;
    }
    relocation_probe::fail_on(0u, 0u);
    return caught;
}

template<class Container>
[[nodiscard]] bool advance_empty_ring(Container& container,
                                      const unsigned initial_cursor) {
    for (unsigned i = 0u; i < initial_cursor; ++i) {
        if (!container.try_emplace(0) || !container.try_pop()) {
            return false;
        }
    }
    return true;
}

template<class Container, class Resize, class Prepare>
[[nodiscard]] bool relocation_failure_cases(Resize resize,
                                            const bool copy_construction,
                                            Prepare prepare) {
    for (unsigned position = 1u; position <= 3u; ++position) {
        Container container;
        if (!resize(container, 4u) || !prepare(container) ||
            container.try_emplace(1) == nullptr ||
            container.try_emplace(2) == nullptr ||
            container.try_emplace(3) == nullptr) {
            return false;
        }

        const auto* const previous_data = container.data();
        const int previous_live = relocation_probe::live;
        const auto previous_blocks = allocation_tracker::live_blocks;
        relocation_probe::fail_on(copy_construction ? position : 0u,
                                  copy_construction ? 0u : position);
        const bool caught = catches_payload_failure(
            [&] { (void)resize(container, 8u); });

        if (!caught || relocation_probe::live != previous_live ||
            allocation_tracker::live_blocks != previous_blocks ||
            container.data() != previous_data || container.capacity() != 4u ||
            !has_original_prefix(container)) {
            return false;
        }
    }
    return relocation_probe::live == 0 && allocation_tracker::live_blocks == 0u;
}

template<reg Capacity>
[[nodiscard]] bool typed_pool_copy_failure_cases(const unsigned initial_cursor) {
    using pool_type = ::spsc::typed_pool<relocation_probe, Capacity,
                                       ::spsc::policy::P, relocation_allocator>;

    for (unsigned position = 1u; position <= 3u; ++position) {
        pool_type source;
        pool_type destination;
        if constexpr (Capacity == 0u) {
            if (!source.resize(4u) || !destination.resize(4u)) {
                return false;
            }
        }
        if (!advance_empty_ring(source, initial_cursor) ||
            !source.try_emplace(1) || !source.try_emplace(2) ||
            !source.try_emplace(3) || !destination.try_emplace(9)) {
            return false;
        }

        const auto* const previous_data = destination.data();
        const int previous_live = relocation_probe::live;
        const auto previous_blocks = allocation_tracker::live_blocks;
        relocation_probe::fail_on(position, 0u);
        const bool assignment_caught = catches_payload_failure(
            [&] { destination = source; });

        if (!assignment_caught || relocation_probe::live != previous_live ||
            allocation_tracker::live_blocks != previous_blocks ||
            destination.data() != previous_data || destination.size() != 1u ||
            destination.front()->value != 9 || !has_original_prefix(source)) {
            return false;
        }

        relocation_probe::fail_on(position, 0u);
        const bool construction_caught = catches_payload_failure([&] {
            pool_type copy(source);
            (void)copy;
        });
        if (!construction_caught || relocation_probe::live != previous_live ||
            allocation_tracker::live_blocks != previous_blocks ||
            !has_original_prefix(source)) {
            return false;
        }
    }
    return relocation_probe::live == 0 && allocation_tracker::live_blocks == 0u;
}

[[nodiscard]] bool typed_pool_grow_allocation_failures() {
    using pool_type = ::spsc::typed_pool<relocation_probe, 0u,
                                       ::spsc::policy::P, relocation_allocator>;

    // Growing 4 -> 8 allocates one pointer table and four additional payloads.
    for (unsigned position = 1u; position <= 5u; ++position) {
        if (relocation_probe::live != 0 || allocation_tracker::live_blocks != 0u) {
            return false;
        }
        allocation_tracker::reset(0u);
        pool_type pool;
        if (!pool.resize(4u) || !advance_empty_ring(pool, 3u) ||
            !pool.try_emplace(1) || !pool.try_emplace(2) ||
            !pool.try_emplace(3)) {
            return false;
        }

        const auto* const previous_data = pool.data();
        const std::array<relocation_probe*, 4u> previous_slots{
            pool.data()[0u], pool.data()[1u],
            pool.data()[2u], pool.data()[3u]};
        const int previous_live = relocation_probe::live;
        const auto previous_blocks = allocation_tracker::live_blocks;
        allocation_tracker::throw_on_call = allocation_tracker::calls + position;
        bool caught = false;
        try {
            (void)pool.resize(8u);
        } catch (const allocation_error&) {
            caught = true;
        }
        allocation_tracker::throw_on_call = 0u;

        if (!caught || relocation_probe::live != previous_live ||
            allocation_tracker::live_blocks != previous_blocks ||
            pool.data() != previous_data || pool.capacity() != 4u ||
            !has_original_prefix(pool)) {
            return false;
        }
        for (reg i = 0u; i < previous_slots.size(); ++i) {
            if (pool.data()[i] != previous_slots[i]) {
                return false;
            }
        }
        if (!pool.resize(8u) || !has_original_prefix(pool)) {
            return false;
        }
    }
    return relocation_probe::live == 0 && allocation_tracker::live_blocks == 0u;
}

[[nodiscard]] bool payload_rollback_is_complete() {
    using object_queue = ::spsc::queue<relocation_probe, 0u,
                                     ::spsc::policy::P, relocation_allocator>;
    using value_fifo = ::spsc::fifo<relocation_probe, 0u,
                                  ::spsc::policy::P, relocation_allocator>;
    using dynamic_chunk = ::spsc::chunk<relocation_probe, 0u,
                                       relocation_allocator>;

    allocation_tracker::reset(0u);
    relocation_probe::fail_on(0u, 0u);
    const auto resize = [](auto& container, const reg count) {
        return container.resize(count);
    };
    const auto reserve = [](auto& container, const reg count) {
        return container.reserve(count);
    };

    // Cursor 3 places the prefix at physical slots [3,0,1], exercising both
    // sides of a wrapped ring. There are 36 ring payload failures in total.
    for (unsigned cursor = 0u; cursor <= 3u; cursor += 3u) {
        const auto prepare = [cursor](auto& container) {
            return advance_empty_ring(container, cursor);
        };
        if (!relocation_failure_cases<object_queue>(resize, true, prepare) ||
            !typed_pool_copy_failure_cases<4u>(cursor) ||
            !typed_pool_copy_failure_cases<0u>(cursor) ||
            !relocation_failure_cases<value_fifo>(resize, false, prepare)) {
            return false;
        }
    }

    // Plus three contiguous chunk failures and five typed_pool allocation
    // failures: 44 injected failures, each with lifetime/ownership checks.
    return relocation_failure_cases<dynamic_chunk>(
               reserve, false, [](auto&) { return true; }) &&
           typed_pool_grow_allocation_failures();
}

} // namespace

int main() {
    return fifo_move_propagates_default_failure() &&
                   latest_move_propagates_default_failure() &&
                   chunk_default_propagates_failure() &&
                   throwing_allocator_cleanup_is_complete() &&
                   guards_have_documented_unwind_semantics() &&
                   buffer_pool_exception_contract_holds() &&
                   buffer_pool_null_allocation_still_reports_false() &&
                   payload_rollback_is_complete()
               ? 0
               : 1;
}
