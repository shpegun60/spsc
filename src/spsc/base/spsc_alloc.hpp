/*
 * spsc_alloc.hpp
 *
 * Created on: 30 Nov. 2025
 * Author: Shpegun60
 */

#ifndef SPSC_ALLOC_HPP_
#define SPSC_ALLOC_HPP_

#include <cstddef>     // std::size_t, std::byte, std::max_align_t, std::ptrdiff_t
#include <cstring>     // std::memcpy
#include <limits>      // std::numeric_limits
#include <new>         // std::nothrow, std::align_val_t
#include <type_traits> // std::true_type, std::false_type

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

} // namespace spsc::alloc

#endif /* SPSC_ALLOC_HPP_ */
