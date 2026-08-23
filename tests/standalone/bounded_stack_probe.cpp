#include "src/spsc/array_fifo.hpp"
#include "src/spsc/buffer_pool.hpp"
#include "src/spsc/chunk_fifo.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/pool.hpp"
#include "src/spsc/typed_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

#if defined(__GNUC__) || defined(__clang__)
#  define SPSC_STACK_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#  define SPSC_STACK_NOINLINE __declspec(noinline)
#else
#  define SPSC_STACK_NOINLINE
#endif

namespace {

// The destination objects are supplied by the caller.  The stack gate thus
// measures transient management frames, not the containers' intentional
// embedded storage.
constexpr reg kLargeCount = 4096u;

using large_fifo =
    ::spsc::fifo<std::uint32_t, kLargeCount, ::spsc::policy::P>;
using wrapped_array_fifo =
    ::spsc::array_fifo<std::uint16_t, 32u, 128u, ::spsc::policy::P>;
using wrapped_chunk_fifo =
    ::spsc::chunk_fifo<std::uint16_t, 32u, 128u, ::spsc::policy::P>;
using large_inner_array_fifo =
    ::spsc::array_fifo<std::byte, kLargeCount, 2u, ::spsc::policy::P>;
using large_inner_chunk_fifo =
    ::spsc::chunk_fifo<std::byte, kLargeCount, 2u, ::spsc::policy::P>;
using large_pool =
    ::spsc::pool<kLargeCount, ::spsc::policy::P>;
using large_typed_pool =
    ::spsc::typed_pool<std::uint32_t, kLargeCount, ::spsc::policy::P>;
using large_fixed_count_buffers =
    ::spsc::buffer_pool<std::byte, 0u, kLargeCount,
                        ::spsc::policy::P>;
using raw_latest = ::spsc::latest<void, 0u, ::spsc::policy::P>;

static_assert(std::is_copy_constructible_v<large_fifo>);
static_assert(std::is_copy_assignable_v<large_fifo>);
static_assert(std::is_copy_assignable_v<large_inner_array_fifo>);
static_assert(std::is_copy_assignable_v<large_inner_chunk_fifo>);
static_assert(std::is_copy_constructible_v<large_pool>);
static_assert(std::is_copy_assignable_v<large_pool>);
static_assert(std::is_copy_constructible_v<large_typed_pool>);
static_assert(std::is_copy_assignable_v<large_typed_pool>);
static_assert(std::is_copy_constructible_v<large_fixed_count_buffers>);
static_assert(std::is_copy_assignable_v<large_fixed_count_buffers>);

} // namespace

// Raw latest publish must copy the caller's object representation directly
// into the slot; the frame must stay O(1), not O(sizeof(payload)). Defined
// outside the anonymous namespace so the probe keeps external linkage.
struct spsc_stack_large_pod {
    std::byte bytes[4096u];
};

SPSC_STACK_NOINLINE void
spsc_stack_fifo_assign(large_fifo& destination, const large_fifo& source) {
    destination = source;
}

SPSC_STACK_NOINLINE void
spsc_stack_array_fifo_assign(wrapped_array_fifo& destination,
                             const wrapped_array_fifo& source) {
    destination = source;
}

SPSC_STACK_NOINLINE void
spsc_stack_chunk_fifo_assign(wrapped_chunk_fifo& destination,
                             const wrapped_chunk_fifo& source) {
    destination = source;
}

SPSC_STACK_NOINLINE void
spsc_stack_large_inner_array_fifo_assign(
    large_inner_array_fifo& destination,
    const large_inner_array_fifo& source) {
    destination = source;
}

SPSC_STACK_NOINLINE void
spsc_stack_large_inner_chunk_fifo_assign(
    large_inner_chunk_fifo& destination,
    const large_inner_chunk_fifo& source) {
    destination = source;
}

SPSC_STACK_NOINLINE void
spsc_stack_pool_copy_construct(void* destination,
                               const large_pool& source) {
    (void)::new (destination) large_pool(source);
}

SPSC_STACK_NOINLINE void
spsc_stack_pool_assign(large_pool& destination, const large_pool& source) {
    destination = source;
}

SPSC_STACK_NOINLINE bool
spsc_stack_pool_resize(large_pool& value) {
    return value.resize(128u);
}

SPSC_STACK_NOINLINE void
spsc_stack_typed_pool_copy_construct(void* destination,
                                     const large_typed_pool& source) {
    (void)::new (destination) large_typed_pool(source);
}

SPSC_STACK_NOINLINE void
spsc_stack_typed_pool_assign(large_typed_pool& destination,
                             const large_typed_pool& source) {
    destination = source;
}

SPSC_STACK_NOINLINE void
spsc_stack_buffer_pool_copy_construct(
    void* destination, const large_fixed_count_buffers& source) {
    (void)::new (destination) large_fixed_count_buffers(source);
}

SPSC_STACK_NOINLINE void
spsc_stack_buffer_pool_assign(large_fixed_count_buffers& destination,
                              const large_fixed_count_buffers& source) {
    destination = source;
}

SPSC_STACK_NOINLINE bool
spsc_stack_buffer_pool_resize(large_fixed_count_buffers& value) {
    return value.resize(128u);
}

SPSC_STACK_NOINLINE bool
spsc_stack_raw_latest_push(raw_latest& value,
                           const spsc_stack_large_pod& payload) {
    return value.try_push(payload);
}

#undef SPSC_STACK_NOINLINE
