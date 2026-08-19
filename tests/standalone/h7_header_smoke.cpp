#include "src/spsc/array_fifo.hpp"
#include "src/spsc/buffer_pool.hpp"
#include "src/spsc/chunk.hpp"
#include "src/spsc/chunk_fifo.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/fifo_view.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/pool.hpp"
#include "src/spsc/pool_view.hpp"
#include "src/spsc/queue.hpp"
#include "src/spsc/typed_pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace {

template<class T, class = void>
struct exposes_is_allocated : std::false_type {};

template<class T>
struct exposes_is_allocated<
    T,
    std::void_t<decltype(std::declval<T&>().isAllocated_)>> : std::true_type {};

template<class T, class = void>
struct exposes_producer_single_snapshot : std::false_type {};

template<class T>
struct exposes_producer_single_snapshot<
    T,
    std::void_t<decltype(std::declval<T&>().producer_single_snapshot())>>
    : std::true_type {};

template<class T, class = void>
struct exposes_consumer_single_snapshot : std::false_type {};

template<class T>
struct exposes_consumer_single_snapshot<
    T,
    std::void_t<decltype(std::declval<T&>().consumer_single_snapshot())>>
    : std::true_type {};

template<class T, class = void>
struct exposes_producer_full_cached : std::false_type {};

template<class T>
struct exposes_producer_full_cached<
    T,
    std::void_t<decltype(std::declval<T&>().producer_full_cached())>>
    : std::true_type {};

template<class T, class = void>
struct exposes_consumer_empty_cached : std::false_type {};

template<class T>
struct exposes_consumer_empty_cached<
    T,
    std::void_t<decltype(std::declval<T&>().consumer_empty_cached())>>
    : std::true_type {};

template<class T>
inline constexpr bool hides_spscbase_endpoint_api_v =
    !exposes_producer_single_snapshot<T>::value &&
    !exposes_consumer_single_snapshot<T>::value &&
    !exposes_producer_full_cached<T>::value &&
    !exposes_consumer_empty_cached<T>::value;

using static_fifo = ::spsc::fifo<std::uint32_t, 8u, ::spsc::policy::CFA<>>;
using static_fifo_view =
    ::spsc::fifo_view<std::uint32_t, 8u, ::spsc::policy::CFA<>>;
using static_queue = ::spsc::queue<std::uint32_t, 8u, ::spsc::policy::CFA<>>;
using static_pool = ::spsc::pool<8u, ::spsc::policy::CFA<>>;
using static_pool_view = ::spsc::pool_view<8u, ::spsc::policy::CFA<>>;
using static_latest = ::spsc::latest<std::uint32_t, 8u, ::spsc::policy::CFA<>>;
using static_typed_pool =
    ::spsc::typed_pool<std::uint32_t, 8u, ::spsc::policy::CFA<>>;
using endpoint_base = ::spsc::SPSCbase<8u, ::spsc::policy::CFA<>>;
using one_line_counter =
    ::spsc::cnt::CachelineCounter<::spsc::cnt::PlainCounter<std::uint32_t>, 64u>;
using nested_one_line_counter =
    ::spsc::cnt::CachelineCounter<one_line_counter, 64u>;

static_assert(sizeof(one_line_counter) == 64u,
              "a cacheline counter must occupy exactly one requested line");
static_assert(alignof(one_line_counter) == 64u,
              "a cacheline counter must retain the requested alignment");
static_assert(sizeof(nested_one_line_counter) == 64u,
              "zero padding must not double an already line-sized counter");
static_assert(alignof(nested_one_line_counter) == 64u,
              "nested cacheline counters must retain the requested alignment");

static_assert(std::is_same_v<
                  ::spsc::fast_fifo<std::uint32_t, 8u>,
                  ::spsc::fifo<std::uint32_t, 8u, ::spsc::policy::CFA<>>>,
              "fast_fifo must select CFA in 2.0");
static_assert(std::is_same_v<
                  ::spsc::fast_queue<std::uint32_t, 8u>,
                  ::spsc::queue<std::uint32_t, 8u, ::spsc::policy::CFA<>>>,
              "fast_queue must select CFA in 2.0");

using alias_value_type = std::uint32_t;
using alias_value_alloc = std::allocator<alias_value_type>;
using alias_byte_alloc = std::allocator<std::byte>;

static_assert(std::is_same_v<
                  ::spsc::local_fifo<alias_value_type, 8u>,
                  ::spsc::fifo<alias_value_type, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_fifo<alias_value_type, 8u>,
                  ::spsc::fifo<alias_value_type, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_fifo<alias_value_type, 8u>,
                  ::spsc::fifo<alias_value_type, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_fifo<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::fifo<alias_value_type, 0u, ::spsc::policy::P, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_fifo<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::fifo<alias_value_type, 0u, ::spsc::policy::FA<>, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_fifo<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::fifo<alias_value_type, 0u, ::spsc::policy::CFA<>, alias_value_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_queue<alias_value_type, 8u>,
                  ::spsc::queue<alias_value_type, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_queue<alias_value_type, 8u>,
                  ::spsc::queue<alias_value_type, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_queue<alias_value_type, 8u>,
                  ::spsc::queue<alias_value_type, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_queue<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::queue<alias_value_type, 0u, ::spsc::policy::P, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_queue<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::queue<alias_value_type, 0u, ::spsc::policy::FA<>, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_queue<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::queue<alias_value_type, 0u, ::spsc::policy::CFA<>, alias_value_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_fifo_view<alias_value_type, 8u>,
                  ::spsc::fifo_view<alias_value_type, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_fifo_view<alias_value_type, 8u>,
                  ::spsc::fifo_view<alias_value_type, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_fifo_view<alias_value_type, 8u>,
                  ::spsc::fifo_view<alias_value_type, 8u, ::spsc::policy::CFA<>>>);

static_assert(std::is_same_v<
                  ::spsc::local_pool<8u>,
                  ::spsc::pool<8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_pool<8u>,
                  ::spsc::pool<8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_pool<8u>,
                  ::spsc::pool<8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_pool<0u, alias_byte_alloc>,
                  ::spsc::pool<0u, ::spsc::policy::P, alias_byte_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_pool<0u, alias_byte_alloc>,
                  ::spsc::pool<0u, ::spsc::policy::FA<>, alias_byte_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_pool<0u, alias_byte_alloc>,
                  ::spsc::pool<0u, ::spsc::policy::CFA<>, alias_byte_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_pool_view<8u>,
                  ::spsc::pool_view<8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_pool_view<8u>,
                  ::spsc::pool_view<8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_pool_view<8u>,
                  ::spsc::pool_view<8u, ::spsc::policy::CFA<>>>);

static_assert(std::is_same_v<
                  ::spsc::local_typed_pool<alias_value_type, 8u>,
                  ::spsc::typed_pool<alias_value_type, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_typed_pool<alias_value_type, 8u>,
                  ::spsc::typed_pool<alias_value_type, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_typed_pool<alias_value_type, 8u>,
                  ::spsc::typed_pool<alias_value_type, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_typed_pool<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::typed_pool<alias_value_type, 0u, ::spsc::policy::P, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_typed_pool<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::typed_pool<alias_value_type, 0u, ::spsc::policy::FA<>, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_typed_pool<alias_value_type, 0u, alias_value_alloc>,
                  ::spsc::typed_pool<alias_value_type, 0u, ::spsc::policy::CFA<>, alias_value_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_latest<alias_value_type, 8u>,
                  ::spsc::latest<alias_value_type, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_latest<alias_value_type, 8u>,
                  ::spsc::latest<alias_value_type, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_latest<alias_value_type, 8u>,
                  ::spsc::latest<alias_value_type, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_latest<void, 0u, alias_byte_alloc>,
                  ::spsc::latest<void, 0u, ::spsc::policy::P, alias_byte_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_latest<void, 0u, alias_byte_alloc>,
                  ::spsc::latest<void, 0u, ::spsc::policy::FA<>, alias_byte_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_latest<void, 0u, alias_byte_alloc>,
                  ::spsc::latest<void, 0u, ::spsc::policy::CFA<>, alias_byte_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_array_fifo<alias_value_type, 4u, 8u>,
                  ::spsc::array_fifo<alias_value_type, 4u, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_array_fifo<alias_value_type, 4u, 8u>,
                  ::spsc::array_fifo<alias_value_type, 4u, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_array_fifo<alias_value_type, 4u, 8u>,
                  ::spsc::array_fifo<alias_value_type, 4u, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_array_fifo<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::array_fifo<alias_value_type, 4u, 0u, ::spsc::policy::P, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_array_fifo<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::array_fifo<alias_value_type, 4u, 0u, ::spsc::policy::FA<>, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_array_fifo<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::array_fifo<alias_value_type, 4u, 0u, ::spsc::policy::CFA<>, alias_value_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_array_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::array_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_array_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::array_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_array_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::array_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::CFA<>>>);

static_assert(std::is_same_v<
                  ::spsc::local_carray_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::carray_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_carray_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::carray_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_carray_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::carray_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::CFA<>>>);

static_assert(std::is_same_v<
                  ::spsc::local_chunk_fifo<alias_value_type, 4u, 8u>,
                  ::spsc::chunk_fifo<alias_value_type, 4u, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_chunk_fifo<alias_value_type, 4u, 8u>,
                  ::spsc::chunk_fifo<alias_value_type, 4u, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_chunk_fifo<alias_value_type, 4u, 8u>,
                  ::spsc::chunk_fifo<alias_value_type, 4u, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_chunk_fifo<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::chunk_fifo<alias_value_type, 4u, 0u, ::spsc::policy::P, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_chunk_fifo<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::chunk_fifo<alias_value_type, 4u, 0u, ::spsc::policy::FA<>, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_chunk_fifo<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::chunk_fifo<alias_value_type, 4u, 0u, ::spsc::policy::CFA<>, alias_value_alloc>>);

static_assert(std::is_same_v<
                  ::spsc::local_chunk_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::chunk_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::P>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_chunk_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::chunk_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::FA<>>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_chunk_fifo_view<alias_value_type, 4u, 8u>,
                  ::spsc::chunk_fifo_view<alias_value_type, 4u, 8u, ::spsc::policy::CFA<>>>);
static_assert(std::is_same_v<
                  ::spsc::local_chunk_fifo_view<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::chunk_fifo_view<alias_value_type, 4u, 0u, ::spsc::policy::P, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::concurrent_chunk_fifo_view<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::chunk_fifo_view<alias_value_type, 4u, 0u, ::spsc::policy::FA<>, alias_value_alloc>>);
static_assert(std::is_same_v<
                  ::spsc::cache_aligned_chunk_fifo_view<alias_value_type, 4u, 0u, alias_value_alloc>,
                  ::spsc::chunk_fifo_view<alias_value_type, 4u, 0u, ::spsc::policy::CFA<>, alias_value_alloc>>);

static_assert(!exposes_is_allocated<static_queue>::value,
              "queue must not expose internal allocation state");
static_assert(!std::is_convertible_v<
                  static_queue*, ::spsc::detail::queue_base<8u>*>,
              "queue_base must remain an implementation detail");

static_assert(!exposes_is_allocated<static_typed_pool>::value,
              "typed_pool must not expose internal allocation state");
static_assert(!std::is_convertible_v<
                  static_typed_pool*, ::spsc::detail::typed_pool_base<8u>*>,
              "typed_pool_base must remain an implementation detail");

static_assert(hides_spscbase_endpoint_api_v<static_fifo>);
static_assert(hides_spscbase_endpoint_api_v<static_fifo_view>);
static_assert(hides_spscbase_endpoint_api_v<static_queue>);
static_assert(hides_spscbase_endpoint_api_v<static_pool>);
static_assert(hides_spscbase_endpoint_api_v<static_pool_view>);
static_assert(hides_spscbase_endpoint_api_v<static_latest>);
static_assert(hides_spscbase_endpoint_api_v<static_typed_pool>);

static_assert(!std::is_convertible_v<static_fifo*, endpoint_base*>);
static_assert(!std::is_convertible_v<static_fifo_view*, endpoint_base*>);
static_assert(!std::is_convertible_v<static_queue*, endpoint_base*>);
static_assert(!std::is_convertible_v<static_pool*, endpoint_base*>);
static_assert(!std::is_convertible_v<static_pool_view*, endpoint_base*>);
static_assert(!std::is_convertible_v<static_latest*, endpoint_base*>);
static_assert(!std::is_convertible_v<static_typed_pool*, endpoint_base*>);

template<class Q>
int fifo_alias_runtime_smoke(Q& queue, const std::uint32_t value, const int base) {
    if (!queue.try_push(value)) {
        return base;
    }
    const auto* observed = queue.try_front();
    if (observed == nullptr || *observed != value) {
        return base + 1;
    }
    return queue.try_pop() ? 0 : base + 2;
}

} // namespace

int main() {
    using queue_type = ::spsc::cache_aligned_fifo<std::uint32_t, 8u>;
    using dynamic_queue_type = ::spsc::fifo<std::uint32_t>;

    queue_type queue;
    if (!queue.try_push(42u)) {
        return 1;
    }
    const auto* value = queue.try_front();
    if (value == nullptr || *value != 42u) {
        return 2;
    }
    if (!queue.try_pop()) {
        return 3;
    }

    ::spsc::local_fifo<std::uint32_t, 8u> local_queue;
    const int local_result = fifo_alias_runtime_smoke(local_queue, 11u, 10);
    if (local_result != 0) {
        return local_result;
    }

    ::spsc::concurrent_fifo<std::uint32_t, 8u> concurrent_queue;
    const int concurrent_result = fifo_alias_runtime_smoke(concurrent_queue, 12u, 20);
    if (concurrent_result != 0) {
        return concurrent_result;
    }

    ::spsc::cache_aligned_fifo<std::uint32_t, 8u> cache_aligned_queue;
    const int cache_aligned_result = fifo_alias_runtime_smoke(cache_aligned_queue, 13u, 30);
    if (cache_aligned_result != 0) {
        return cache_aligned_result;
    }

    // Also instantiate dynamic storage: its storage member is a pointer, so
    // this catches a policy alignment that would weaken pointer alignment.
    dynamic_queue_type dynamic_queue{8u};
    if (!dynamic_queue.try_push(7u)) {
        return 4;
    }
    return dynamic_queue.try_pop() ? 0 : 5;
}
