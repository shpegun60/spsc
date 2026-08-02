#ifndef SPSC_TEST_POLICY_MATRIX_HPP_
#define SPSC_TEST_POLICY_MATRIX_HPP_

#include <type_traits>
#include <utility>

#include "base/spsc_policy.hpp"

namespace spsc::test {

static_assert(SPSC_DEFAULT_POLICY_ATOMIC == 0,
              "test matrix expects the public default policy to be plain unless explicitly overridden");
static_assert(SPSC_REQUIRE_LOCK_FREE == 1,
              "test matrix expects lock-free enforcement to stay enabled by default");
static_assert(std::is_same_v<::spsc::policy::default_policy, ::spsc::policy::P>,
              "default_policy must match the configured plain-by-default contract");

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

} // namespace spsc::test

#endif // SPSC_TEST_POLICY_MATRIX_HPP_
