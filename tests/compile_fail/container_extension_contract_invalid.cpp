#include "buffer_pool.hpp"
#include "chunk.hpp"
#include "fifo.hpp"
#include "latest.hpp"
#include "pool.hpp"
#include "queue.hpp"
#include "typed_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

template<class T>
struct narrow_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::uint16_t;
    using difference_type = std::int16_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = narrow_allocator<U>; };

    narrow_allocator() noexcept = default;

    template<class U>
    narrow_allocator(const narrow_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type) noexcept { return nullptr; }
    void deallocate(pointer, size_type) noexcept {}
};

template<class T>
struct throwing_default_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = throwing_default_allocator<U>; };

    throwing_default_allocator() noexcept(false) {}

    template<class U>
    throwing_default_allocator(const throwing_default_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type) noexcept { return nullptr; }
    void deallocate(pointer, size_type) noexcept {}
};

template<class T>
struct throwing_allocate_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = throwing_allocate_allocator<U>; };

    throwing_allocate_allocator() noexcept = default;

    template<class U>
    throwing_allocate_allocator(
        const throwing_allocate_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type) noexcept(false) {
        return nullptr;
    }

    void deallocate(pointer, size_type) noexcept {}
};

// Multi-rebind containers use role-specific allocators so each negative test
// violates exactly one allocation contract.
template<class T, class ThrowingValue>
struct throwing_value_allocate_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind {
        using other = throwing_value_allocate_allocator<U, ThrowingValue>;
    };

    throwing_value_allocate_allocator() noexcept = default;

    template<class U>
    throwing_value_allocate_allocator(
        const throwing_value_allocate_allocator<U, ThrowingValue>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type) noexcept(
        !std::is_same_v<value_type, ThrowingValue>) {
        return nullptr;
    }

    void deallocate(pointer, size_type) noexcept {}
};

template<class T>
struct throwing_pointer_table_allocator {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    template<class U>
    struct rebind { using other = throwing_pointer_table_allocator<U>; };

    throwing_pointer_table_allocator() noexcept = default;

    template<class U>
    throwing_pointer_table_allocator(
        const throwing_pointer_table_allocator<U>&) noexcept {}

    [[nodiscard]] pointer allocate(size_type) noexcept(
        !std::is_pointer_v<value_type>) {
        return nullptr;
    }

    void deallocate(pointer, size_type) noexcept {}
};

struct throwing_destructor_value {
    ~throwing_destructor_value() noexcept(false) {}
};

// Mode-0 gate for the fully static buffer_pool copy constructor: nothrow
// default construction and copy assignment, but a throwing copy constructor.
struct throwing_copy_ctor_value {
    throwing_copy_ctor_value() noexcept = default;
    throwing_copy_ctor_value(const throwing_copy_ctor_value&) noexcept(false) {}
    throwing_copy_ctor_value&
    operator=(const throwing_copy_ctor_value&) noexcept = default;
    ~throwing_copy_ctor_value() noexcept = default;
};

#if defined(SPSC_TEST_THROWING_ALLOCATOR_CHUNK)
using rejected_container =
    spsc::chunk<int, 0u, throwing_allocate_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_FIFO)
using rejected_container =
    spsc::fifo<int, 0u, spsc::policy::P,
               throwing_allocate_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_QUEUE)
using rejected_container =
    spsc::queue<int, 8u, spsc::policy::P,
                throwing_allocate_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_STATIC_POOL)
using rejected_container =
    spsc::pool<8u, spsc::policy::P,
               throwing_value_allocate_allocator<std::byte, std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_DYNAMIC_POOL)
using rejected_container =
    spsc::pool<0u, spsc::policy::P,
               throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_STATIC_TYPED_POOL)
using rejected_container =
    spsc::typed_pool<int, 8u, spsc::policy::P,
                     throwing_value_allocate_allocator<std::byte, int>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_DYNAMIC_TYPED_POOL)
using rejected_container =
    spsc::typed_pool<int, 0u, spsc::policy::P,
                     throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_RAW_LATEST)
using rejected_container =
    spsc::latest<void, 0u, spsc::policy::P,
                 throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_TYPED_LATEST)
using rejected_container =
    spsc::latest<int, 0u, spsc::policy::P,
                 throwing_allocate_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_BUFFER_COUNT)
using rejected_container =
    spsc::buffer_pool<int, 4u, 0u, spsc::policy::P,
                      throwing_allocate_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_BUFFER_SIZE)
using rejected_container =
    spsc::buffer_pool<std::byte, 0u, 4u, spsc::policy::P,
                      throwing_value_allocate_allocator<std::byte, std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_BUFFER_SHAPE)
using rejected_container =
    spsc::buffer_pool<std::byte, 0u, 0u, spsc::policy::P,
                      throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_POINTER_TABLE_STATIC_POOL)
using rejected_container =
    spsc::pool<8u, spsc::policy::P,
               throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_POINTER_TABLE_STATIC_TYPED_POOL)
using rejected_container =
    spsc::typed_pool<int, 8u, spsc::policy::P,
                     throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_POINTER_TABLE_FIXED_COUNT_BUFFER_POOL)
using rejected_container =
    spsc::buffer_pool<std::byte, 0u, 4u, spsc::policy::P,
                      throwing_pointer_table_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_ALLOCATOR_DEFAULT)
using rejected_container =
    spsc::queue<int, 0u, spsc::policy::P,
                throwing_default_allocator<std::byte>>;
#elif defined(SPSC_TEST_THROWING_DESTRUCTOR)
using rejected_container =
    spsc::queue<throwing_destructor_value, 0u, spsc::policy::P>;
#elif defined(SPSC_TEST_QUEUE_VOLATILE_PAYLOAD)
using rejected_container = spsc::queue<volatile int, 8u, spsc::policy::P>;
#elif defined(SPSC_TEST_QUEUE_ARRAY_PAYLOAD)
using rejected_container = spsc::queue<int[4], 8u, spsc::policy::P>;
#elif defined(SPSC_TEST_TYPED_POOL_CONST_PAYLOAD)
using rejected_container = spsc::typed_pool<const int, 8u, spsc::policy::P>;
#elif defined(SPSC_TEST_TYPED_POOL_VOLATILE_PAYLOAD)
using rejected_container = spsc::typed_pool<volatile int, 8u, spsc::policy::P>;
#elif defined(SPSC_TEST_TYPED_POOL_ARRAY_PAYLOAD)
using rejected_container = spsc::typed_pool<int[4], 8u, spsc::policy::P>;
#elif defined(SPSC_TEST_FIFO_VOLATILE_PAYLOAD)
using rejected_container = spsc::fifo<volatile int, 8u, spsc::policy::P>;
#elif defined(SPSC_TEST_BUFFER_POOL_MODE0_COPY_CTOR)
using rejected_container =
    spsc::buffer_pool<throwing_copy_ctor_value, 4u, 2u, spsc::policy::P>;
#else
using rejected_container =
    spsc::queue<int, 0u, spsc::policy::P, narrow_allocator<std::byte>>;
#endif

int main()
{
    return sizeof(rejected_container);
}
