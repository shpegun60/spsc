#ifndef SPSC_TEST_RESERVE_ALLOCATOR_HPP_
#define SPSC_TEST_RESERVE_ALLOCATOR_HPP_

#include <cstddef>
#include <memory>
#include <type_traits>

#include "base/spsc_alloc.hpp"

namespace spsc::test {

struct reserve_allocator_stats {
    static inline std::size_t allocation_calls{0u};
    static inline std::size_t last_allocation_count{0u};

    static void reset() noexcept {
        allocation_calls = 0u;
        last_allocation_count = 0u;
    }
};

template <class T>
class reserve_probe_allocator {
public:
    using value_type = T;
    using is_always_equal = std::true_type;

    template <class U>
    struct rebind {
        using other = reserve_probe_allocator<U>;
    };

    reserve_probe_allocator() noexcept = default;

    template <class U>
    reserve_probe_allocator(const reserve_probe_allocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(const std::size_t count) noexcept {
        ++reserve_allocator_stats::allocation_calls;
        reserve_allocator_stats::last_allocation_count = count;

        // Keep boundary tests deterministic: ordinary requests receive real
        // storage, while RB_MAX-sized probes fail without asking the host for
        // an impossibly large allocation.
        if (count == 0u || count > kMaxSuccessfulElements) {
            return nullptr;
        }
        return spsc::alloc::basic_allocator<
            T, spsc::alloc::fail_mode::returns_null>{}.allocate(count);
    }

    void deallocate(T* const ptr, const std::size_t count) noexcept {
        if (ptr != nullptr) {
            spsc::alloc::basic_allocator<
                T, spsc::alloc::fail_mode::returns_null>{}.deallocate(
                    ptr, count);
        }
    }

    template <class U>
    [[nodiscard]] bool operator==(const reserve_probe_allocator<U>&) const noexcept {
        return true;
    }

    template <class U>
    [[nodiscard]] bool operator!=(const reserve_probe_allocator<U>&) const noexcept {
        return false;
    }

private:
    static constexpr std::size_t kMaxSuccessfulElements = 64u;
};

} // namespace spsc::test

#endif // SPSC_TEST_RESERVE_ALLOCATOR_HPP_
