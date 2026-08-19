#ifndef SPSC_TEST_POLICY_MATRIX_HPP_
#define SPSC_TEST_POLICY_MATRIX_HPP_

#include <atomic>
#include <type_traits>
#include <utility>

#include "base/spsc_policy.hpp"

namespace spsc::test {

#if defined(SPSC_DEFAULT_POLICY_ATOMIC)
using expected_default_policy = std::conditional_t<
    (SPSC_DEFAULT_POLICY_ATOMIC != 0),
    ::spsc::policy::A<>,
    ::spsc::policy::P>;
#else
using expected_default_policy = ::spsc::policy::FA<>;
#endif

static_assert(SPSC_REQUIRE_LOCK_FREE == 1,
              "test matrix expects lock-free enforcement to stay enabled by default");
static_assert(std::is_same_v<::spsc::policy::default_policy, expected_default_policy>,
              "default_policy must match the v3 modern or legacy-override contract");

template <class... Policies>
struct policy_pack {};

template <class T>
struct type_tag {
    using type = T;
};

template <class Q, class = void>
struct has_publish_count : std::false_type {};

template <class Q>
struct has_publish_count<Q, std::void_t<decltype(std::declval<Q&>().publish(typename Q::size_type{1u}))>>
    : std::true_type {};

template <class Q, class = void>
struct has_try_publish_count : std::false_type {};

template <class Q>
struct has_try_publish_count<Q, std::void_t<decltype(std::declval<Q&>().try_publish(typename Q::size_type{1u}))>>
    : std::true_type {};

template <class Q, class = void>
struct has_untagged_claim_write : std::false_type {};

template <class Q>
struct has_untagged_claim_write<
    Q,
    std::void_t<decltype(std::declval<Q&>().claim_write(typename Q::size_type{1u}))>>
    : std::true_type {};

template <class Q, class = void>
struct has_default_untagged_claim_write : std::false_type {};

template <class Q>
struct has_default_untagged_claim_write<
    Q,
    std::void_t<decltype(std::declval<Q&>().claim_write())>>
    : std::true_type {};

template <class Q, class = void>
struct has_untagged_claim_read : std::false_type {};

template <class Q>
struct has_untagged_claim_read<
    Q,
    std::void_t<decltype(std::declval<Q&>().claim_read(typename Q::size_type{1u}))>>
    : std::true_type {};

template <class Q, class = void>
struct has_default_untagged_claim_read : std::false_type {};

template <class Q>
struct has_default_untagged_claim_read<
    Q,
    std::void_t<decltype(std::declval<Q&>().claim_read())>>
    : std::true_type {};

template <class Q, class N, class = void>
struct has_pop_with : std::false_type {};

template <class Q, class N>
struct has_pop_with<Q, N, std::void_t<decltype(std::declval<Q&>().pop(std::declval<N&>()))>>
    : std::true_type {};

template <class Q, class N, class = void>
struct has_try_pop_with : std::false_type {};

template <class Q, class N>
struct has_try_pop_with<Q, N, std::void_t<decltype(std::declval<Q&>().try_pop(std::declval<N&>()))>>
    : std::true_type {};

template <class Q, class = void>
struct has_destroy_method : std::false_type {};

template <class Q>
struct has_destroy_method<Q, std::void_t<decltype(std::declval<Q&>().destroy())>>
    : std::true_type {};

template <class Q, class = void>
struct has_swap_method : std::false_type {};

template <class Q>
struct has_swap_method<Q, std::void_t<decltype(std::declval<Q&>().swap(std::declval<Q&>()))>>
    : std::true_type {};

template <class Q, class = void>
struct has_resize_one : std::false_type {};

template <class Q>
struct has_resize_one<Q, std::void_t<decltype(std::declval<Q&>().resize(typename Q::size_type{}))>>
    : std::true_type {};

template <class Q, class = void>
struct has_resize_two : std::false_type {};

template <class Q>
struct has_resize_two<Q, std::void_t<decltype(std::declval<Q&>().resize(typename Q::size_type{}, typename Q::size_type{}))>>
    : std::true_type {};

template <class R, class = void>
struct region_has_ptr : std::false_type {};

template <class R>
struct region_has_ptr<R, std::void_t<decltype(std::declval<const R&>().ptr)>>
    : std::true_type {};

template <class R, class = void>
struct region_has_span : std::false_type {};

template <class R>
struct region_has_span<R, std::void_t<decltype(std::declval<const R&>().span())>>
    : std::true_type {};

template <class R, class = void>
struct region_has_data : std::false_type {};

template <class R>
struct region_has_data<R, std::void_t<decltype(std::declval<const R&>().data())>>
    : std::true_type {};

template <class R, class = void>
struct region_has_count : std::false_type {};

template <class R>
struct region_has_count<R, std::void_t<decltype(std::declval<const R&>().count)>>
    : std::true_type {};

template <class Q, class = void>
struct has_valid_method : std::false_type {};

template <class Q>
struct has_valid_method<Q, std::void_t<decltype(std::declval<const Q&>().valid())>>
    : std::is_same<decltype(std::declval<const Q&>().valid()), bool> {};

using extended_nonthreaded_policy_pack =
    policy_pack<
        ::spsc::policy::FA<>,
        ::spsc::policy::AA<>,
        ::spsc::policy::CP,
        ::spsc::policy::CV,
        ::spsc::policy::VV,
        ::spsc::policy::CVV,
        ::spsc::policy::CFA<>,
        ::spsc::policy::CAA<>
    >;

using extended_atomic_like_policy_pack =
    policy_pack<
        ::spsc::policy::FA<>,
        ::spsc::policy::AA<>,
        ::spsc::policy::CFA<>,
        ::spsc::policy::CAA<>
    >;

using extended_cached_policy_pack =
    policy_pack<
        ::spsc::policy::CP,
        ::spsc::policy::CV,
        ::spsc::policy::CVV,
        ::spsc::policy::CFA<>,
        ::spsc::policy::CAA<>
    >;

// H6 deliberately uses named custom order palettes instead of merely
// reusing default_orders. That proves policy users can choose an equivalent
// acquire/release palette or a stronger seq_cst palette without bypassing the
// counter publication checks.
struct explicit_acquire_release_orders {
    static constexpr std::memory_order load  = std::memory_order_acquire;
    static constexpr std::memory_order store = std::memory_order_release;
    static constexpr std::memory_order rmw   = std::memory_order_acq_rel;
};

struct explicit_seq_cst_orders {
    static constexpr std::memory_order load  = std::memory_order_seq_cst;
    static constexpr std::memory_order store = std::memory_order_seq_cst;
    static constexpr std::memory_order rmw   = std::memory_order_seq_cst;
};

using custom_atomic_order_policy_pack =
    policy_pack<
        ::spsc::policy::A<explicit_acquire_release_orders>,
        ::spsc::policy::FA<explicit_acquire_release_orders>,
        ::spsc::policy::CA<explicit_acquire_release_orders>,
        ::spsc::policy::CFA<explicit_acquire_release_orders>,
        ::spsc::policy::A<explicit_seq_cst_orders>,
        ::spsc::policy::FA<explicit_seq_cst_orders>,
        ::spsc::policy::CA<explicit_seq_cst_orders>,
        ::spsc::policy::CFA<explicit_seq_cst_orders>
    >;

template <class... Policies, class Fn>
constexpr void for_each_policy(policy_pack<Policies...>, Fn&& fn) {
    (fn(type_tag<Policies>{}), ...);
}

template <class Fn>
constexpr void for_each_extended_nonthreaded_policy(Fn&& fn) {
    for_each_policy(extended_nonthreaded_policy_pack{}, std::forward<Fn>(fn));
}

template <class Fn>
constexpr void for_each_extended_atomic_like_policy(Fn&& fn) {
    for_each_policy(extended_atomic_like_policy_pack{}, std::forward<Fn>(fn));
}

template <class Fn>
constexpr void for_each_extended_cached_policy(Fn&& fn) {
    for_each_policy(extended_cached_policy_pack{}, std::forward<Fn>(fn));
}

template <class Fn>
constexpr void for_each_custom_atomic_order_policy(Fn&& fn) {
    for_each_policy(custom_atomic_order_policy_pack{}, std::forward<Fn>(fn));
}

} // namespace spsc::test

#endif // SPSC_TEST_POLICY_MATRIX_HPP_
