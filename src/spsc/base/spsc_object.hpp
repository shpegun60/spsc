/*
 * spsc_object.hpp
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
