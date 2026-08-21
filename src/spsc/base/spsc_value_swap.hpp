/*
 * spsc_value_swap.hpp
 *
 * Internal value-storage swap helper.
 *
 * Some static SPSC containers require default construction and assignment but
 * do not require a user-provided ADL swap.  std::array::swap is unavailable
 * for such values even though a valid three-assignment exchange exists.  This
 * helper first preserves a real ADL/std swap and otherwise uses that supported
 * fallback, so public swap traits never advertise an operation whose body
 * cannot compile. std::array storage is exchanged element by element so the
 * fallback never materializes the entire container storage on the stack.
 */

#ifndef SPSC_VALUE_SWAP_HPP_
#define SPSC_VALUE_SWAP_HPP_

#include <array>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace spsc::detail {

template<class T>
inline constexpr bool fallback_value_swappable_v =
    std::is_default_constructible_v<T> &&
    (std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>);

template<class T>
inline constexpr bool fallback_value_swap_uses_move_v =
    std::is_move_assignable_v<T> &&
    (std::is_nothrow_move_assignable_v<T> ||
     !std::is_copy_assignable_v<T>);

template<class T>
inline constexpr bool fallback_value_swap_noexcept_v =
    fallback_value_swappable_v<T> &&
    std::is_nothrow_default_constructible_v<T> &&
    (fallback_value_swap_uses_move_v<T>
         ? std::is_nothrow_move_assignable_v<T>
         : std::is_nothrow_copy_assignable_v<T>);

template<class T>
struct value_swap_traits
{
    static constexpr bool available =
        std::is_swappable_v<T> || fallback_value_swappable_v<T>;
    static constexpr bool is_nothrow =
        std::is_swappable_v<T>
            ? std::is_nothrow_swappable_v<T>
            : fallback_value_swap_noexcept_v<T>;
};

template<class T, std::size_t N>
struct value_swap_traits<std::array<T, N>>
{
    static constexpr bool available =
        (N == 0u) || value_swap_traits<T>::available;
    static constexpr bool is_nothrow =
        (N == 0u) || value_swap_traits<T>::is_nothrow;
};

template<class T>
inline constexpr bool value_swappable_v = value_swap_traits<T>::available;

template<class T>
inline constexpr bool value_swap_noexcept_v = value_swap_traits<T>::is_nothrow;

template<class T>
void swap_value(T& lhs, T& rhs) noexcept(value_swap_noexcept_v<T>)
{
    static_assert(value_swappable_v<T>,
                  "[spsc]: value storage requires swap or default construction "
                  "plus move/copy assignment");

    if (std::addressof(lhs) == std::addressof(rhs)) {
        return;
    }

    if constexpr (std::is_swappable_v<T>) {
        using std::swap;
        swap(lhs, rhs);
    } else {
        T temporary{};
        if constexpr (fallback_value_swap_uses_move_v<T>) {
            temporary = std::move(lhs);
            lhs = std::move(rhs);
            rhs = std::move(temporary);
        } else {
            temporary = lhs;
            lhs = rhs;
            rhs = temporary;
        }
    }
}

template<class T, std::size_t N>
void swap_value(std::array<T, N>& lhs,
                std::array<T, N>& rhs) noexcept(value_swap_noexcept_v<std::array<T, N>>)
{
    static_assert(value_swappable_v<std::array<T, N>>,
                  "[spsc]: array storage requires swappable elements or default "
                  "construction plus move/copy assignment");

    if (std::addressof(lhs) == std::addressof(rhs)) {
        return;
    }

    if constexpr (N != 0u) {
        for (std::size_t i = 0u; i < N; ++i) {
            ::spsc::detail::swap_value(lhs[i], rhs[i]);
        }
    }
}

} // namespace spsc::detail

#endif /* SPSC_VALUE_SWAP_HPP_ */
