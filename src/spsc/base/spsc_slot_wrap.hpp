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

    cache_aligned_slot(const T& other) : T(other) {}
    cache_aligned_slot(T&& other) noexcept(std::is_nothrow_move_constructible_v<T>)
        : T(std::move(other)) {}
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
