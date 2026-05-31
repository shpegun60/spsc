#ifndef SPSC_TEST_POLICY_MATRIX_HPP_
#define SPSC_TEST_POLICY_MATRIX_HPP_

#include <type_traits>
#include <utility>

#include "base/spsc_policy.hpp"

namespace spsc::test {

static_assert(SPSC_DEFAULT_POLICY_ATOMIC == 1,
              "test matrix expects the public default policy to be atomic/thread-capable");
static_assert(SPSC_REQUIRE_LOCK_FREE == 1,
              "test matrix expects atomic policies to require lock-free counters by default");
static_assert(std::is_same_v<::spsc::policy::default_policy, ::spsc::policy::A<>>,
              "default_policy must match the configured safe-by-default atomic contract");

template <class... Policies>
struct policy_pack {};

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
    (fn.template operator()<Policies>(), ...);
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
