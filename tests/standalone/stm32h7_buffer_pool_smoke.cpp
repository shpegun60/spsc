#ifndef STM32H7xx
#  define STM32H7xx 1
#endif

#include "src/spsc/buffer_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace {

using dma_buffers =
    ::spsc::buffer_pool<std::byte, 100u, 8u, ::spsc::policy::CP>;
using dynamic_count_buffers =
    ::spsc::buffer_pool<std::byte, 100u, 0u, ::spsc::policy::CP>;
using dynamic_size_buffers =
    ::spsc::buffer_pool<std::byte, 0u, 8u, ::spsc::policy::CP>;
using dynamic_shape_buffers =
    ::spsc::buffer_pool<std::byte, 0u, 0u, ::spsc::policy::CP>;

template<class Pool>
inline constexpr bool has_semantic_byte_api_v =
    std::is_same_v<
        decltype(std::declval<const Pool&>().payload_bytes()),
        typename Pool::size_type> &&
    std::is_same_v<
        decltype(std::declval<const Pool&>().cache_span_bytes()),
        typename Pool::size_type> &&
    std::is_same_v<
        decltype(std::declval<const Pool&>().storage_alignment()),
        typename Pool::size_type>;

static_assert(SPSC_CACHELINE_BYTES == 32u,
              "STM32H7 buffer smoke requires a 32-byte cache line");
static_assert(!::spsc::cnt::counter_is_atomic_v<
                  typename ::spsc::policy::CP::counter_type>,
              "buffer_pool alignment does not require atomic counters");
static_assert(dma_buffers::size_bytes() == 100u);
static_assert(dma_buffers::span_bytes() == 128u);
static_assert(dma_buffers::alignment() == 32u);
static_assert(dma_buffers::payload_bytes() == dma_buffers::size_bytes());
static_assert(dma_buffers::cache_span_bytes() == dma_buffers::span_bytes());
static_assert(dma_buffers::storage_alignment() == dma_buffers::alignment());
static_assert(has_semantic_byte_api_v<dma_buffers>);
static_assert(has_semantic_byte_api_v<dynamic_count_buffers>);
static_assert(has_semantic_byte_api_v<dynamic_size_buffers>);
static_assert(has_semantic_byte_api_v<dynamic_shape_buffers>);

} // namespace

int main()
{
    dma_buffers buffers;
    std::uintptr_t previous = 0u;

    for (reg i = 0u; i < buffers.count(); ++i) {
        const auto address = reinterpret_cast<std::uintptr_t>(buffers.data(i));
        if ((address == 0u) || ((address % buffers.storage_alignment()) != 0u)) {
            return 1;
        }
        if ((i != 0u) && ((address - previous) != buffers.cache_span_bytes())) {
            return 2;
        }
        previous = address;
    }

    return 0;
}
