/*
 * spsc_slot_wrap.hpp
 *
 * Created on: 12 Apr. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPSC_SLOT_WRAP_HPP_
#define SPSC_SLOT_WRAP_HPP_

#include "spsc_alloc.hpp"
#include "spsc_value_swap.hpp"

#include <type_traits>
#include <utility>

namespace spsc::detail {

template <class T, std::size_t Align>
struct alignas(Align) cache_aligned_slot : T {
    using T::T;
    using T::operator=;

    cache_aligned_slot() = default;
    cache_aligned_slot(const cache_aligned_slot&) = default;
    cache_aligned_slot(cache_aligned_slot&&) noexcept(std::is_nothrow_move_constructible_v<T>) = default;
    cache_aligned_slot& operator=(const cache_aligned_slot&) = default;
    cache_aligned_slot& operator=(cache_aligned_slot&&) noexcept(std::is_nothrow_move_assignable_v<T>) = default;

    template<class U = T,
             std::enable_if_t<std::is_same_v<U, T> &&
                              std::is_constructible_v<T, const T&>, int> = 0>
    cache_aligned_slot(const T& other) : T(other) {}

    template<class U = T,
             std::enable_if_t<std::is_same_v<U, T> &&
                              std::is_constructible_v<T, T&&>, int> = 0>
    cache_aligned_slot(T&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : T(std::move(other)) {}

    template<bool Enabled = ::spsc::detail::value_swappable_v<T>,
             std::enable_if_t<Enabled, int> = 0>
    void swap(cache_aligned_slot& other) noexcept(
        ::spsc::detail::value_swap_noexcept_v<T>)
    {
        ::spsc::detail::swap_value(static_cast<T&>(*this),
                                   static_cast<T&>(other));
    }

    template<bool Enabled = ::spsc::detail::value_swappable_v<T>,
             std::enable_if_t<Enabled, int> = 0>
    friend void swap(cache_aligned_slot& lhs,
                     cache_aligned_slot& rhs) noexcept(
        ::spsc::detail::value_swap_noexcept_v<T>)
    {
        lhs.template swap<Enabled>(rhs);
    }
};

template <class T, class Policy>
using cache_aligned_slot_t = std::conditional_t<
    (::spsc::alloc::policy_storage_alignment_v<Policy, T> > alignof(T)),
    cache_aligned_slot<T, ::spsc::alloc::policy_storage_alignment_v<Policy, T>>,
    T>;

template <class Alloc, class T>
inline constexpr bool allocator_supports_slot_alignment_v =
    (::spsc::alloc::rebind_allocator_min_alignment_v<Alloc, T> >= alignof(T));

} // namespace spsc::detail

#endif /* SPSC_SLOT_WRAP_HPP_ */
