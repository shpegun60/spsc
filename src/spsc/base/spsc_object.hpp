/*
 * spsc_object.hpp
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared object-lifetime helpers used by containers that manage T manually.
 */

#ifndef SPSC_OBJECT_HPP_
#define SPSC_OBJECT_HPP_

#include <type_traits>

#include "spsc_tools.hpp" // RB_FORCEINLINE

namespace spsc::detail {

template<class U>
RB_FORCEINLINE void destroy_at(U* p) noexcept
{
    if constexpr (!std::is_trivially_destructible_v<U>) {
        p->~U();
    }
}

} // namespace spsc::detail

#endif /* SPSC_OBJECT_HPP_ */
