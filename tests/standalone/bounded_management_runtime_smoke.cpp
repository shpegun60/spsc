#include "src/spsc/buffer_pool.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/pool.hpp"
#include "src/spsc/typed_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace {

#if (SPSC_ENABLE_EXCEPTIONS != 0)
struct scratch_allocation_failure {};
#endif

struct scratch_allocator_state {
    static inline bool fail_pointer_tables{false};
#if (SPSC_ENABLE_EXCEPTIONS != 0)
    static inline bool throw_from_pointer_tables{false};
#endif
    static inline std::size_t pointer_table_attempts{0u};
    static inline std::size_t payload_attempts{0u};
    static inline std::size_t live_blocks{0u};

    static void reset() noexcept {
        fail_pointer_tables = false;
#if (SPSC_ENABLE_EXCEPTIONS != 0)
        throw_from_pointer_tables = false;
#endif
        pointer_table_attempts = 0u;
        payload_attempts = 0u;
        live_blocks = 0u;
    }
};

template<class T>
struct scratch_probe_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = scratch_probe_allocator<U>; };

    scratch_probe_allocator() noexcept = default;

    template<class U>
    scratch_probe_allocator(const scratch_probe_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(const size_type count) noexcept(
        SPSC_ENABLE_EXCEPTIONS == 0) {
        if constexpr (std::is_pointer_v<value_type>) {
            ++scratch_allocator_state::pointer_table_attempts;
#if (SPSC_ENABLE_EXCEPTIONS != 0)
            if (scratch_allocator_state::throw_from_pointer_tables) {
                throw scratch_allocation_failure{};
            }
#endif
            if (scratch_allocator_state::fail_pointer_tables) {
                return nullptr;
            }
        } else {
            ++scratch_allocator_state::payload_attempts;
        }

        pointer result = static_cast<pointer>(
            ::operator new(count * sizeof(value_type), std::nothrow));
        if (result != nullptr) {
            ++scratch_allocator_state::live_blocks;
        }
        return result;
    }

    void deallocate(pointer ptr, size_type) noexcept {
        if (ptr != nullptr) {
            --scratch_allocator_state::live_blocks;
            ::operator delete(ptr);
        }
    }
};

using probe_allocator = scratch_probe_allocator<std::byte>;
using raw_pool =
    ::spsc::pool<8u, ::spsc::policy::P, probe_allocator>;
using object_pool =
    ::spsc::typed_pool<std::uint32_t, 8u, ::spsc::policy::P,
                       probe_allocator>;
using fixed_count_buffers =
    ::spsc::buffer_pool<std::byte, 0u, 4u, ::spsc::policy::P,
                        probe_allocator>;

[[nodiscard]] bool raw_pool_transaction_is_bounded_and_non_mutating() {
    raw_pool source(sizeof(std::uint32_t));
    raw_pool destination(sizeof(std::uint32_t));
    const std::uint32_t source_value = 0x11223344u;
    const std::uint32_t destination_value = 0x55667788u;

    bool ok = source.is_valid() && destination.is_valid() &&
              source.try_push(source_value) &&
              destination.try_push(destination_value);

    const auto payload_attempts = scratch_allocator_state::payload_attempts;
    const auto live_blocks = scratch_allocator_state::live_blocks;
    scratch_allocator_state::fail_pointer_tables = true;

    destination = source;
    std::uint32_t observed = 0u;
    ok = ok && destination.size() == 1u &&
         destination.buffer_size() == sizeof(destination_value) &&
         destination.try_peek(observed) && observed == destination_value &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;

    ok = ok && !destination.resize(sizeof(destination_value) * 2u) &&
         destination.size() == 1u &&
         destination.buffer_size() == sizeof(destination_value) &&
         destination.try_peek(observed) && observed == destination_value &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;

    raw_pool failed_copy(source);
    ok = ok && !failed_copy.is_valid();

    scratch_allocator_state::fail_pointer_tables = false;
#if (SPSC_ENABLE_EXCEPTIONS != 0)
    scratch_allocator_state::throw_from_pointer_tables = true;
    bool threw = false;
    try {
        destination = source;
    } catch (const scratch_allocation_failure&) {
        threw = true;
    }
    bool resize_threw = false;
    try {
        (void)destination.resize(sizeof(destination_value) * 2u);
    } catch (const scratch_allocation_failure&) {
        resize_threw = true;
    }
    observed = 0u;
    ok = ok && threw && resize_threw && destination.size() == 1u &&
         destination.try_peek(observed) && observed == destination_value &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;
    scratch_allocator_state::throw_from_pointer_tables = false;
#endif
    destination = source;
    observed = 0u;
    return ok && destination.size() == 1u &&
           destination.try_peek(observed) && observed == source_value &&
           destination.resize(sizeof(source_value) * 2u) &&
           destination.buffer_size() == sizeof(source_value) * 2u &&
           destination.try_peek(observed) && observed == source_value;
}

[[nodiscard]] bool typed_pool_transaction_is_bounded_and_non_mutating() {
    object_pool source;
    object_pool destination;
    bool ok = source.is_valid() && destination.is_valid() &&
              source.try_emplace(101u) && destination.try_emplace(202u);

    const auto payload_attempts = scratch_allocator_state::payload_attempts;
    const auto live_blocks = scratch_allocator_state::live_blocks;
    scratch_allocator_state::fail_pointer_tables = true;

    destination = source;
    ok = ok && destination.size() == 1u &&
         destination.try_front() != nullptr &&
         *destination.try_front() == 202u &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;

    object_pool failed_copy(source);
    ok = ok && !failed_copy.is_valid();

    scratch_allocator_state::fail_pointer_tables = false;
#if (SPSC_ENABLE_EXCEPTIONS != 0)
    scratch_allocator_state::throw_from_pointer_tables = true;
    bool threw = false;
    try {
        destination = source;
    } catch (const scratch_allocation_failure&) {
        threw = true;
    }
    ok = ok && threw && destination.size() == 1u &&
         destination.try_front() != nullptr &&
         *destination.try_front() == 202u &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;
    scratch_allocator_state::throw_from_pointer_tables = false;
#endif
    destination = source;
    return ok && destination.size() == 1u &&
           destination.try_front() != nullptr &&
           *destination.try_front() == 101u;
}

[[nodiscard]] bool buffer_pool_transaction_is_bounded_and_non_mutating() {
    fixed_count_buffers source(3u);
    fixed_count_buffers destination(2u);
    bool ok = source.is_valid() && destination.is_valid();

    for (reg slot = 0u; slot < source.count(); ++slot) {
        for (reg i = 0u; i < source.size(); ++i) {
            source.data(slot)[i] =
                static_cast<std::byte>(100u + slot * 10u + i);
        }
        for (reg i = 0u; i < destination.size(); ++i) {
            destination.data(slot)[i] =
                static_cast<std::byte>(200u + slot * 10u + i);
        }
    }

    const auto payload_attempts = scratch_allocator_state::payload_attempts;
    const auto live_blocks = scratch_allocator_state::live_blocks;
    scratch_allocator_state::fail_pointer_tables = true;

    destination = source;
    ok = ok && destination.size() == 2u &&
         destination.data(0u)[0] == std::byte{200u} &&
         destination.data(3u)[1] == std::byte{231u} &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;

    ok = ok && !destination.resize(5u) && destination.size() == 2u &&
         destination.data(0u)[0] == std::byte{200u} &&
         destination.data(3u)[1] == std::byte{231u} &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;

    fixed_count_buffers failed_copy(source);
    ok = ok && failed_copy.size() == 0u && failed_copy.data(0u) == nullptr;

    scratch_allocator_state::fail_pointer_tables = false;
#if (SPSC_ENABLE_EXCEPTIONS != 0)
    // A throwing pointer-table allocator now propagates from buffer_pool
    // management exactly like it does from pool/typed_pool: the destination
    // is preserved and nothing leaks.
    scratch_allocator_state::throw_from_pointer_tables = true;
    bool threw = false;
    try {
        destination = source;
    } catch (const scratch_allocation_failure&) {
        threw = true;
    }
    bool resize_threw = false;
    try {
        (void)destination.resize(5u);
    } catch (const scratch_allocation_failure&) {
        resize_threw = true;
    }
    ok = ok && threw && resize_threw && destination.size() == 2u &&
         destination.data(0u)[0] == std::byte{200u} &&
         destination.data(3u)[1] == std::byte{231u} &&
         scratch_allocator_state::payload_attempts == payload_attempts &&
         scratch_allocator_state::live_blocks == live_blocks;
    scratch_allocator_state::throw_from_pointer_tables = false;
#endif
    destination = source;
    ok = ok && destination.size() == 3u &&
         destination.data(0u)[0] == std::byte{100u} &&
         destination.data(3u)[2] == std::byte{132u};

    ok = ok && destination.resize(5u) && destination.size() == 5u &&
         destination.data(0u)[0] == std::byte{100u} &&
         destination.data(3u)[2] == std::byte{132u};
    return ok;
}

#if (SPSC_ENABLE_EXCEPTIONS != 0)
struct copy_failure {};

struct throwing_copy_assignment {
    static inline unsigned assignments_before_throw{0u};
    int value{0};

    throwing_copy_assignment() noexcept = default;
    explicit throwing_copy_assignment(const int input) noexcept : value(input) {}
    throwing_copy_assignment(const throwing_copy_assignment&) noexcept = default;
    throwing_copy_assignment(throwing_copy_assignment&&) noexcept = default;
    throwing_copy_assignment& operator=(throwing_copy_assignment&&) noexcept = default;

    throwing_copy_assignment&
    operator=(const throwing_copy_assignment& other) {
        if (assignments_before_throw == 0u) {
            throw copy_failure{};
        }
        --assignments_before_throw;
        value = other.value;
        return *this;
    }
};

[[nodiscard]] bool static_fifo_copy_has_basic_guarantee() {
    using queue_type =
        ::spsc::fifo<throwing_copy_assignment, 8u, ::spsc::policy::P>;

    queue_type source;
    queue_type destination;
    bool ok = source.try_push(throwing_copy_assignment{1}) &&
              source.try_push(throwing_copy_assignment{2}) &&
              destination.try_push(throwing_copy_assignment{9});

    throwing_copy_assignment::assignments_before_throw = 1u;
    bool threw = false;
    try {
        destination = source;
    } catch (const copy_failure&) {
        threw = true;
    }

    return ok && threw && destination.is_valid() && destination.empty() &&
           destination.capacity() == 8u;
}
#endif

} // namespace

int main() {
    scratch_allocator_state::reset();
    bool ok = false;
    {
        ok = raw_pool_transaction_is_bounded_and_non_mutating() &&
             typed_pool_transaction_is_bounded_and_non_mutating() &&
             buffer_pool_transaction_is_bounded_and_non_mutating();
#if (SPSC_ENABLE_EXCEPTIONS != 0)
        ok = ok && static_fifo_copy_has_basic_guarantee();
#endif
    }

    scratch_allocator_state::fail_pointer_tables = false;
#if (SPSC_ENABLE_EXCEPTIONS != 0)
    scratch_allocator_state::throw_from_pointer_tables = false;
#endif
    return ok && scratch_allocator_state::pointer_table_attempts != 0u &&
                   scratch_allocator_state::live_blocks == 0u
               ? 0
               : 1;
}
