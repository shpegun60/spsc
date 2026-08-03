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

#include <cstdint>
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

static_assert(std::is_same_v<
                  ::spsc::fast_fifo<std::uint32_t, 8u>,
                  ::spsc::fifo<std::uint32_t, 8u, ::spsc::policy::CFA<>>>,
              "fast_fifo must select CFA in 2.0");
static_assert(std::is_same_v<
                  ::spsc::fast_queue<std::uint32_t, 8u>,
                  ::spsc::queue<std::uint32_t, 8u, ::spsc::policy::CFA<>>>,
              "fast_queue must select CFA in 2.0");

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

} // namespace

int main() {
    using queue_type = ::spsc::fifo<std::uint32_t, 8u, ::spsc::policy::CFA<>>;
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

    // Also instantiate dynamic storage: its storage member is a pointer, so
    // this catches a policy alignment that would weaken pointer alignment.
    dynamic_queue_type dynamic_queue{8u};
    if (!dynamic_queue.try_push(7u)) {
        return 4;
    }
    return dynamic_queue.try_pop() ? 0 : 5;
}
