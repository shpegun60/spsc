/*
 * spsc_alloc.hpp
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPSC_ALLOC_HPP_
#define SPSC_ALLOC_HPP_

#include <cstddef>     // std::size_t, std::byte, std::max_align_t, std::ptrdiff_t
#include <cstring>     // std::memcpy
#include <limits>      // std::numeric_limits
#include <memory>      // std::allocator_traits
#include <new>         // std::nothrow, std::align_val_t
#include <type_traits> // std::true_type, std::false_type
#include <utility>     // std::declval

#include "basic_types.h"        // reg
#include "spsc_tools.hpp"

namespace spsc::alloc {

enum class fail_mode : unsigned {
    throws,        // allocate() throws std::bad_alloc on failure (requires SPSC_ENABLE_EXCEPTIONS != 0)
    returns_null   // allocate() returns nullptr on failure (intended for spsc containers)
};

namespace detail {

#if defined(__cpp_aligned_new) && defined(__STDCPP_DEFAULT_NEW_ALIGNMENT__)
inline constexpr std::size_t kDefaultNewAlign = __STDCPP_DEFAULT_NEW_ALIGNMENT__;
#else
inline constexpr std::size_t kDefaultNewAlign = alignof(std::max_align_t);
#endif

// Raw header stores the original pointer returned by ::operator new.
inline constexpr std::size_t kRawHeaderSize = sizeof(void*);

// Prefer native aligned new/delete when available and requested by config.
inline constexpr bool kPreferAlignedNewConfig =
#if defined(__cpp_aligned_new)
    (SPSC_ALLOC_PREFER_ALIGNED_NEW != 0);
#else
    false;
#endif

constexpr bool is_pow2(const std::size_t x) noexcept {
    return (x != 0u) && ((x & (x - 1u)) == 0u);
}

constexpr std::size_t max_sz(const std::size_t a, const std::size_t b) noexcept {
    return (a > b) ? a : b;
}

constexpr bool add_overflow(const std::size_t a, const std::size_t b) noexcept {
    return a > (std::numeric_limits<std::size_t>::max() - b);
}

template<fail_mode Mode>
[[nodiscard]] inline void* fail_ptr() noexcept(Mode == fail_mode::returns_null)
{
    static_assert((Mode != fail_mode::throws) || (SPSC_ENABLE_EXCEPTIONS != 0),
                  "fail_mode::throws requires SPSC_ENABLE_EXCEPTIONS != 0");

    if constexpr (Mode == fail_mode::throws) {
#if (SPSC_ENABLE_EXCEPTIONS != 0)
        throw std::bad_alloc{};
#else
        return nullptr;
#endif
    } else {
        return nullptr;
    }
}

template<fail_mode Mode>
[[nodiscard]] inline void* fail_ptr_after_raw(void* raw) noexcept(Mode == fail_mode::returns_null)
{
    if (raw) {
        ::operator delete(raw);
    }
    return fail_ptr<Mode>();
}

/*
 * Portable over-aligned allocation using plain ::operator new/delete.
 * - Allocates extra bytes, aligns the returned pointer manually.
 * - Stores the original raw pointer right before the payload.
 */
template<fail_mode Mode>
[[nodiscard]] inline void* aligned_alloc_raw(const std::size_t alignmentIn, const std::size_t size)
    noexcept(Mode == fail_mode::returns_null)
{
    if (RB_UNLIKELY(size == 0u)) {
        return nullptr;
    }

    std::size_t alignment = alignmentIn;
    if (alignment < alignof(void*)) {
        alignment = alignof(void*);
    }

    const std::uintptr_t upMax = std::numeric_limits<std::uintptr_t>::max();
    const std::size_t upMaxSz  = static_cast<std::size_t>(upMax);

    if (RB_UNLIKELY(!is_pow2(alignment) || ((alignment - 1u) > upMaxSz))) {
        return fail_ptr<Mode>();
    }

    const std::size_t padMax = alignment - 1u;

    if (RB_UNLIKELY(add_overflow(size, padMax))) {
        return fail_ptr<Mode>();
    }
    const std::size_t tmp = size + padMax;

    if (RB_UNLIKELY(add_overflow(tmp, kRawHeaderSize))) {
        return fail_ptr<Mode>();
    }
    const std::size_t totalSize = tmp + kRawHeaderSize;

    if (RB_UNLIKELY(totalSize > upMaxSz)) {
        return fail_ptr<Mode>();
    }

    void* raw = nullptr;
    if constexpr (Mode == fail_mode::throws) {
        raw = ::operator new(totalSize);
    } else {
        raw = ::operator new(totalSize, std::nothrow);
        if (RB_UNLIKELY(!raw)) {
            return nullptr;
        }
    }

    const std::uintptr_t rawUp = reinterpret_cast<std::uintptr_t>(raw);

    if (RB_UNLIKELY(rawUp > (upMax - static_cast<std::uintptr_t>(kRawHeaderSize)))) {
        return fail_ptr_after_raw<Mode>(raw);
    }

    auto* const rawBytes = static_cast<std::byte*>(raw);
    auto* const basePtr  = rawBytes + kRawHeaderSize;

    const std::uintptr_t baseUp = reinterpret_cast<std::uintptr_t>(basePtr);
    const std::uintptr_t add    = static_cast<std::uintptr_t>(alignment - 1u);

    if (RB_UNLIKELY(baseUp > (upMax - add))) {
        return fail_ptr_after_raw<Mode>(raw);
    }

    const std::uintptr_t alignedUp = (baseUp + add) & ~add;
    const std::uintptr_t offset    = alignedUp - rawUp;

    if (RB_UNLIKELY(offset < static_cast<std::uintptr_t>(kRawHeaderSize))) {
        return fail_ptr_after_raw<Mode>(raw);
    }

    const std::uintptr_t totalUp = static_cast<std::uintptr_t>(totalSize);
    const std::uintptr_t endUp   = offset + static_cast<std::uintptr_t>(size);

    if (RB_UNLIKELY(endUp > totalUp)) {
        return fail_ptr_after_raw<Mode>(raw);
    }

    auto* const alignedPtr = rawBytes + static_cast<std::size_t>(offset);
    auto* const header     = alignedPtr - kRawHeaderSize;

    std::memcpy(header, &raw, kRawHeaderSize);
    return static_cast<void*>(alignedPtr);
}

inline void aligned_free_raw(void* ptr) noexcept
{
    if (RB_UNLIKELY(!ptr)) {
        return;
    }

    auto* const payload = static_cast<std::byte*>(ptr);
    auto* const header  = payload - kRawHeaderSize;

    void* raw = nullptr;
    std::memcpy(&raw, header, kRawHeaderSize);

    ::operator delete(raw);
}

template<class T>
inline constexpr bool needs_overaligned_alloc = (alignof(T) > kDefaultNewAlign);

#if defined(__cpp_aligned_new)
// Detect presence of ::operator new(size_t, align_val_t, nothrow).
// Some toolchains define __cpp_aligned_new but still miss this overload.
template<class Dummy = void>
struct has_aligned_nothrow_new {
    template<class U>
    static auto test(int) -> decltype(
        ::operator new(std::size_t{sizeof(U)}, std::align_val_t{alignof(U)}, std::nothrow),
        std::true_type{}
    );

    template<class>
    static auto test(...) -> std::false_type;

    static constexpr bool value = decltype(test<std::max_align_t>(0))::value;
};

// Detect presence of ::operator delete(void*, align_val_t).
// If this is missing, using aligned-new is a trap (mismatched delete).
template<class Dummy = void>
struct has_aligned_delete {
    template<class U>
    static auto test(int) -> decltype(
        ::operator delete(static_cast<void*>(nullptr), std::align_val_t{alignof(U)}),
        std::true_type{}
    );

    template<class>
    static auto test(...) -> std::false_type;

    static constexpr bool value = decltype(test<std::max_align_t>(0))::value;
};

inline constexpr bool kHasAlignedNothrowNew = has_aligned_nothrow_new<>::value;
inline constexpr bool kHasAlignedDelete     = has_aligned_delete<>::value;
#else
inline constexpr bool kHasAlignedNothrowNew = false;
inline constexpr bool kHasAlignedDelete     = false;
#endif

// Final gating for "native aligned new/delete" usage.
inline constexpr bool kAlignedThrowOk =
#if defined(__cpp_aligned_new)
    kPreferAlignedNewConfig && kHasAlignedDelete;
#else
    false;
#endif

inline constexpr bool kAlignedNoThrowOk =
#if defined(__cpp_aligned_new)
    kPreferAlignedNewConfig && kHasAlignedDelete && kHasAlignedNothrowNew;
#else
    false;
#endif

} // namespace detail

// ============================================================================
// aligned_allocator<T, Alignment, Mode>
// ============================================================================

template<class T, std::size_t Alignment, fail_mode Mode>
class aligned_allocator
{
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal                        = std::true_type;

    static_assert(Alignment != 0u, "aligned_allocator: Alignment must be non-zero");
    static_assert(detail::is_pow2(Alignment), "aligned_allocator: Alignment must be pow2");
    static_assert(detail::is_pow2(alignof(T)), "aligned_allocator: alignof(T) must be pow2");
    static_assert((Mode != fail_mode::throws) || (SPSC_ENABLE_EXCEPTIONS != 0),
                  "aligned_allocator: fail_mode::throws requires exceptions");

    aligned_allocator() noexcept = default;

    template<class U>
    aligned_allocator(const aligned_allocator<U, Alignment, Mode>&) noexcept {}

    [[nodiscard]] T* allocate(size_type n) noexcept(Mode == fail_mode::returns_null)
    {
        if (RB_UNLIKELY(n == 0u)) {
            return nullptr;
        }

        if (RB_UNLIKELY(n > (std::numeric_limits<size_type>::max() / sizeof(T)))) {
            return static_cast<T*>(detail::fail_ptr<Mode>());
        }

        const size_type bytes = n * sizeof(T);
        constexpr size_type kEffAlign = detail::max_sz(Alignment, alignof(T));

        if constexpr (kEffAlign <= detail::kDefaultNewAlign) {
            if constexpr (Mode == fail_mode::throws) {
                return static_cast<T*>(::operator new(bytes));
            } else {
                return static_cast<T*>(::operator new(bytes, std::nothrow));
            }
        } else {
#if defined(__cpp_aligned_new)
            if constexpr (Mode == fail_mode::throws) {
                if constexpr (detail::kAlignedThrowOk) {
                    return static_cast<T*>(::operator new(bytes, std::align_val_t(kEffAlign)));
                } else {
                    return static_cast<T*>(detail::aligned_alloc_raw<Mode>(kEffAlign, bytes));
                }
            } else {
                if constexpr (detail::kAlignedNoThrowOk) {
                    return static_cast<T*>(::operator new(bytes, std::align_val_t(kEffAlign), std::nothrow));
                } else {
                    return static_cast<T*>(detail::aligned_alloc_raw<Mode>(kEffAlign, bytes));
                }
            }
#else
            return static_cast<T*>(detail::aligned_alloc_raw<Mode>(kEffAlign, bytes));
#endif
        }
    }

    void deallocate(T* p, size_type /*n*/) noexcept
    {
        if (RB_UNLIKELY(!p)) {
            return;
        }

        constexpr size_type kEffAlign = detail::max_sz(Alignment, alignof(T));

        if constexpr (kEffAlign <= detail::kDefaultNewAlign) {
            ::operator delete(p);
        } else {
#if defined(__cpp_aligned_new)
            if constexpr (Mode == fail_mode::throws) {
                if constexpr (detail::kAlignedThrowOk) {
                    ::operator delete(p, std::align_val_t(kEffAlign));
                } else {
                    detail::aligned_free_raw(p);
                }
            } else {
                if constexpr (detail::kAlignedNoThrowOk) {
                    ::operator delete(p, std::align_val_t(kEffAlign));
                } else {
                    detail::aligned_free_raw(p);
                }
            }
#else
            detail::aligned_free_raw(p);
#endif
        }
    }

    template<class U>
    struct rebind {
        using other = aligned_allocator<U, Alignment, Mode>;
    };
};

template<class T1, std::size_t A1, fail_mode M1, class T2, std::size_t A2, fail_mode M2>
inline bool operator==(const aligned_allocator<T1, A1, M1>&,
                       const aligned_allocator<T2, A2, M2>&) noexcept
{
    return (A1 == A2) && (M1 == M2);
}

template<class T1, std::size_t A1, fail_mode M1, class T2, std::size_t A2, fail_mode M2>
inline bool operator!=(const aligned_allocator<T1, A1, M1>& a,
                       const aligned_allocator<T2, A2, M2>& b) noexcept
{
    return !(a == b);
}

// ============================================================================
// basic_allocator<T, Mode>
// ============================================================================

template<class T, fail_mode Mode>
class basic_allocator
{
public:
    using value_type      = T;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;

    using propagate_on_container_move_assignment = std::true_type;
    using is_always_equal                        = std::true_type;

    static_assert((Mode != fail_mode::throws) || (SPSC_ENABLE_EXCEPTIONS != 0),
                  "basic_allocator: fail_mode::throws requires exceptions");

    basic_allocator() noexcept = default;

    template<class U>
    basic_allocator(const basic_allocator<U, Mode>&) noexcept {}

    [[nodiscard]] T* allocate(size_type n) noexcept(Mode == fail_mode::returns_null)
    {
        if (RB_UNLIKELY(n == 0u)) {
            return nullptr;
        }

        if (RB_UNLIKELY(n > (std::numeric_limits<size_type>::max() / sizeof(T)))) {
            return static_cast<T*>(detail::fail_ptr<Mode>());
        }

        const size_type bytes = n * sizeof(T);

        if constexpr (!detail::needs_overaligned_alloc<T>) {
            if constexpr (Mode == fail_mode::throws) {
                return static_cast<T*>(::operator new(bytes));
            } else {
                return static_cast<T*>(::operator new(bytes, std::nothrow));
            }
        } else {
            using impl = aligned_allocator<T, alignof(T), Mode>;
            impl a{};
            return a.allocate(n);
        }
    }

    void deallocate(T* p, size_type n) noexcept
    {
        if (RB_UNLIKELY(!p)) {
            return;
        }

        if constexpr (!detail::needs_overaligned_alloc<T>) {
            ::operator delete(p);
        } else {
            using impl = aligned_allocator<T, alignof(T), Mode>;
            impl a{};
            a.deallocate(p, n);
        }
    }

    template<class U>
    struct rebind {
        using other = basic_allocator<U, Mode>;
    };
};

template<class T1, fail_mode M1, class T2, fail_mode M2>
inline bool operator==(const basic_allocator<T1, M1>&,
                       const basic_allocator<T2, M2>&) noexcept
{
    return M1 == M2;
}

template<class T1, fail_mode M1, class T2, fail_mode M2>
inline bool operator!=(const basic_allocator<T1, M1>& a,
                       const basic_allocator<T2, M2>& b) noexcept
{
    return !(a == b);
}

// ============================================================================
// Default allocator aliases
// ============================================================================

static_assert(SPSC_ENABLE_EXCEPTIONS == 0 || SPSC_ENABLE_EXCEPTIONS == 1,
              "SPSC_ENABLE_EXCEPTIONS must be 0 or 1");

using default_alloc = basic_allocator<std::byte,
    (SPSC_ENABLE_EXCEPTIONS != 0) ? fail_mode::throws : fail_mode::returns_null
>;

template<std::size_t Alignment>
using align_alloc = aligned_allocator<std::byte, Alignment,
    (SPSC_ENABLE_EXCEPTIONS != 0) ? fail_mode::throws : fail_mode::returns_null
>;

namespace detail {

template<class T>
struct object_alignment : std::integral_constant<std::size_t, alignof(T)> {};

template<>
struct object_alignment<void> : std::integral_constant<std::size_t, 1u> {};

template<class Policy, class = void>
struct policy_allocator_alignment : std::integral_constant<std::size_t, 1u> {};

template<class Policy>
struct checked_policy_allocator_alignment
    : std::integral_constant<
          std::size_t,
          static_cast<std::size_t>(Policy::allocator_alignment)> {
private:
    using raw_type = std::remove_cv_t<
        std::remove_reference_t<decltype(Policy::allocator_alignment)>>;
    static constexpr std::size_t alignment =
        static_cast<std::size_t>(Policy::allocator_alignment);

    static_assert((std::is_integral_v<raw_type> &&
                   !std::is_same_v<raw_type, bool>) ||
                      std::is_enum_v<raw_type>,
                  "[spsc::alloc]: Policy::allocator_alignment must be an "
                  "integral or enum constant");
    static_assert(static_cast<long double>(Policy::allocator_alignment) > 0.0L &&
                      static_cast<long double>(Policy::allocator_alignment) <=
                          static_cast<long double>(
                              std::numeric_limits<std::size_t>::max()),
                  "[spsc::alloc]: Policy::allocator_alignment must be positive "
                  "and representable as size_t");
    static_assert(alignment != 0u && is_pow2(alignment),
                  "[spsc::alloc]: Policy::allocator_alignment must be a "
                  "non-zero power of two");
};

template<class Policy>
struct policy_allocator_alignment<Policy, std::void_t<decltype(Policy::allocator_alignment)>>
    : checked_policy_allocator_alignment<Policy> {};

template<class Alloc, class T>
using rebind_alloc_t = typename std::allocator_traits<Alloc>::template rebind_alloc<T>;

template<class Alloc, class = void>
struct allocator_size_covers_reg : std::false_type {};

template<class Alloc>
struct allocator_size_covers_reg<
    Alloc,
    std::void_t<typename std::allocator_traits<Alloc>::size_type>>
    : std::bool_constant<
          std::is_integral_v<
              typename std::allocator_traits<Alloc>::size_type> &&
          std::is_unsigned_v<
              typename std::allocator_traits<Alloc>::size_type> &&
          (std::numeric_limits<
               typename std::allocator_traits<Alloc>::size_type>::digits >=
           std::numeric_limits<reg>::digits)> {};

template<class Alloc>
inline constexpr bool allocator_size_covers_reg_v =
    allocator_size_covers_reg<Alloc>::value;

// std::allocator_traits<Alloc>::allocate() is not conditionally noexcept in
// C++17, even when Alloc::allocate() is.  Inspect the allocator operation that
// the standard allocator contract actually requires so the no-exceptions
// configuration accepts the shipped null-returning allocators while rejecting
// throwing custom allocators such as std::allocator.
template<class Alloc, class = void>
struct allocator_allocate_noexcept : std::false_type {};

template<class Alloc>
struct allocator_allocate_noexcept<
    Alloc,
    std::void_t<
        typename std::allocator_traits<Alloc>::size_type,
        decltype(std::declval<Alloc&>().allocate(
            std::declval<
                typename std::allocator_traits<Alloc>::size_type>()))>>
    : std::bool_constant<noexcept(std::declval<Alloc&>().allocate(
          std::declval<
              typename std::allocator_traits<Alloc>::size_type>()))> {};

template<class Alloc>
inline constexpr bool allocator_allocate_noexcept_v =
    allocator_allocate_noexcept<Alloc>::value;

template<class Alloc, class T, class = void>
struct allocator_min_alignment : std::integral_constant<std::size_t, object_alignment<T>::value> {};

template<class T, fail_mode Mode>
struct allocator_min_alignment<basic_allocator<T, Mode>, T, void>
    : std::integral_constant<std::size_t, max_sz(kDefaultNewAlign, alignof(T))> {};

template<class T, std::size_t Alignment, fail_mode Mode>
struct allocator_min_alignment<aligned_allocator<T, Alignment, Mode>, T, void>
    : std::integral_constant<std::size_t, max_sz(Alignment, alignof(T))> {};

template<class SizeT>
constexpr SizeT round_up_pow2_multiple(const SizeT n, const std::size_t alignment) noexcept
{
    if (alignment <= 1u) {
        return n;
    }

    const auto mask = static_cast<SizeT>(alignment - 1u);
    const auto maxv = (std::numeric_limits<SizeT>::max)();
    if (n > static_cast<SizeT>(maxv - mask)) {
        return SizeT{0};
    }

    return static_cast<SizeT>((n + mask) & ~mask);
}

} // namespace detail

template<class T>
inline constexpr std::size_t object_alignment_v = detail::object_alignment<T>::value;

template<class Policy>
inline constexpr std::size_t policy_allocator_alignment_v =
    detail::policy_allocator_alignment<Policy>::value;

template<class Policy, std::size_t ValueAlignment, class FallbackAlloc = default_alloc>
using policy_default_alloc_t = std::conditional_t<
    (policy_allocator_alignment_v<Policy> > 1u),
    align_alloc<detail::max_sz(ValueAlignment, policy_allocator_alignment_v<Policy>)>,
    FallbackAlloc
>;

template<class Policy, class T, class FallbackAlloc = default_alloc>
using policy_default_value_alloc_t =
    policy_default_alloc_t<Policy, object_alignment_v<T>, FallbackAlloc>;

template<class Policy, class T>
inline constexpr std::size_t policy_storage_alignment_v =
    detail::max_sz(object_alignment_v<T>, policy_allocator_alignment_v<Policy>);

template<class Alloc, class T>
inline constexpr std::size_t rebind_allocator_min_alignment_v =
    detail::allocator_min_alignment<typename detail::rebind_alloc_t<Alloc, T>, T>::value;

template<class Policy, class Alloc>
inline constexpr std::size_t policy_slot_round_alignment_v =
    ((policy_allocator_alignment_v<Policy> > 1u) &&
     (rebind_allocator_min_alignment_v<Alloc, std::byte> >= policy_allocator_alignment_v<Policy>))
        ? policy_allocator_alignment_v<Policy>
        : 1u;

template<class Policy, class Alloc, class SizeT>
constexpr SizeT round_up_size_for_policy(const SizeT n) noexcept
{
    return detail::round_up_pow2_multiple(n, policy_slot_round_alignment_v<Policy, Alloc>);
}

} // namespace spsc::alloc

#endif /* SPSC_ALLOC_HPP_ */
