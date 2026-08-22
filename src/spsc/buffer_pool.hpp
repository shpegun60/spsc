/*
 * buffer_pool.hpp
 *
 * Created on: Apr 13, 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small owning pool of fixed-size buffers.
 *
 * Shapes:
 * - BufferSize > 0, Count > 0 : fully static pool of std::array<T, BufferSize>.
 * - BufferSize > 0, Count == 0: dynamic count of fixed-size buffers.
 * - BufferSize == 0, Count > 0: static count of dynamically-sized buffers (T*).
 * - BufferSize == 0, Count == 0: fully dynamic count + runtime buffer size.
 *
 * Goals:
 * - Keep DMA / cache-sensitive buffers grouped in a dedicated type.
 * - Support both static and dynamic storage.
 * - Reuse SPSC allocator/policy alignment rules.
 * - Expose logical buffer size separately from the policy-rounded storage span.
 *   That span is cache-maintenance-safe when policy alignment matches the
 *   target's real cache line.
 */

#ifndef SPSC_BUFFER_POOL_HPP_
#define SPSC_BUFFER_POOL_HPP_

#include <array>
#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "basic_types.h"
#include "base/spsc_alloc.hpp"
#include "base/spsc_policy.hpp"
#include "base/spsc_slot_wrap.hpp"
#include "base/spsc_tools.hpp"
#include "base/spsc_value_swap.hpp"

namespace spsc {

template <
    class T,
    reg BufferSize = 0,
    reg Count = 0,
    class Policy = policy::default_policy,
    class Alloc = alloc::policy_default_value_alloc_t<
        Policy,
        T,
        alloc::default_alloc
    >
>
class buffer_pool;

namespace detail {

template <class SizeT>
[[nodiscard]] constexpr SizeT ceil_div(const SizeT n, const SizeT d) noexcept
{
    return (d == 0u)
        ? 0u
        : static_cast<SizeT>((n / d) + (((n % d) != 0u) ? 1u : 0u));
}

template <class SizeT, class T>
[[nodiscard]] constexpr SizeT logical_buffer_bytes(const SizeT logical_count) noexcept
{
    if (logical_count == 0u) {
        return 0u;
    }

    constexpr SizeT kElemBytes = static_cast<SizeT>(sizeof(T));
    constexpr SizeT kMax = (std::numeric_limits<SizeT>::max)();
    if (logical_count > (kMax / kElemBytes)) {
        return 0u;
    }

    return static_cast<SizeT>(logical_count * kElemBytes);
}

template <class SizeT, class Policy, class Alloc>
[[nodiscard]] constexpr SizeT effective_buffer_bytes_from_bytes(const SizeT logical_bytes) noexcept
{
    if (logical_bytes == 0u) {
        return 0u;
    }

    const SizeT effective_bytes =
        alloc::round_up_size_for_policy<Policy, Alloc>(logical_bytes);

    return (effective_bytes >= logical_bytes) ? effective_bytes : 0u;
}

template <class SizeT, class T, class Policy, class Alloc>
[[nodiscard]] constexpr SizeT effective_buffer_bytes(const SizeT logical_count) noexcept
{ return effective_buffer_bytes_from_bytes<SizeT, Policy, Alloc>(logical_buffer_bytes<SizeT, T>(logical_count)); }

template <class SizeT, class T, class Policy, class Alloc>
[[nodiscard]] constexpr SizeT effective_buffer_elements(const SizeT logical_count) noexcept
{ return ceil_div(effective_buffer_bytes<SizeT, T, Policy, Alloc>(logical_count), static_cast<SizeT>(sizeof(T))); }

template <class T, class... Args>
T* construct_at_compat(T* ptr, Args&&... args)
{
    return ::new (static_cast<void*>(ptr)) T(std::forward<Args>(args)...);
}

} // namespace detail

/* =======================================================================
 * buffer_pool<T, BufferSize, Count>
 *
 * Static buffer size + static count.
 * Storage is fully embedded: std::array<stored_buffer_type, Count>.
 * ======================================================================= */
template <class T, reg BufferSize, reg Count, class Policy, class Alloc>
class buffer_pool
{
    static_assert(BufferSize > 0u, "[buffer_pool]: BufferSize must be > 0 in the static-buffer specialization.");
    static_assert(Count > 0u, "[buffer_pool]: Count must be > 0 in the static-count specialization.");
    static_assert(!std::is_const_v<T>, "[buffer_pool]: const T does not make sense for writable buffers.");
    static_assert(std::is_default_constructible_v<T>,
                  "[buffer_pool]: static buffers require default-constructible T.");
    static_assert(std::is_copy_constructible_v<T>,
                  "[buffer_pool]: static buffers require copy-constructible T.");
    static_assert(std::is_copy_assignable_v<T>,
                  "[buffer_pool]: static buffers require copy-assignable T.");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "[buffer_pool]: T destructor must be noexcept.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept default-constructible T.");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept copy-assignable T.");
#endif

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type = T;
    using size_type = reg;
    using policy_type = Policy;
    using base_allocator_type = Alloc;
    using buffer_type = std::array<value_type, BufferSize>;
    using stored_buffer_type = detail::cache_aligned_slot_t<buffer_type, policy_type>;

private:
    using storage_type = std::array<stored_buffer_type, Count>;

public:
    using pointer = value_type*;
    using const_pointer = const value_type*;

    static constexpr size_type static_count = Count;
    static constexpr size_type static_buffer_size = BufferSize;

    static_assert(alignof(stored_buffer_type) >= alloc::policy_storage_alignment_v<policy_type, buffer_type>,
                  "[buffer_pool]: static buffer slot alignment must honor the policy storage alignment.");
    static_assert((sizeof(stored_buffer_type) % alignof(stored_buffer_type)) == 0u,
                  "[buffer_pool]: static buffer slot size must be a multiple of its alignment.");
    static_assert(std::is_nothrow_default_constructible_v<base_allocator_type>,
                  "[buffer_pool]: allocator must be nothrow default-constructible.");
    static_assert(std::is_unsigned_v<size_type>,
                  "[buffer_pool]: reg (size_type) must be unsigned.");
    static_assert(sizeof(buffer_type) <= (std::numeric_limits<size_type>::max)(),
                  "[buffer_pool]: logical fixed-buffer byte size must fit in reg.");
    static_assert(sizeof(stored_buffer_type) <= (std::numeric_limits<size_type>::max)(),
                  "[buffer_pool]: effective fixed-buffer byte size must fit in reg.");

    // ------------------------------------------------------------------------------------------
    // Constructors / Assignment
    // ------------------------------------------------------------------------------------------
    buffer_pool() = default;
    ~buffer_pool() noexcept = default;
    buffer_pool(const buffer_pool&) = default;
    buffer_pool& operator=(const buffer_pool&) = default;
    buffer_pool(buffer_pool&&) noexcept(std::is_nothrow_move_constructible_v<stored_buffer_type>) = default;
    buffer_pool& operator=(buffer_pool&&) noexcept(std::is_nothrow_move_assignable_v<stored_buffer_type>) = default;

    void swap(buffer_pool& other) noexcept(::spsc::detail::value_swap_noexcept_v<storage_type>)
    {
        if (this != &other) {
            ::spsc::detail::swap_value(buffers_, other.buffers_);
        }
    }

    friend void swap(buffer_pool& a, buffer_pool& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }
    [[nodiscard]] static constexpr bool is_valid() noexcept { return true; }

    // ------------------------------------------------------------------------------------------
    // Size / Span Introspection
    //
    // count()/size() describe the logical buffer shape.
    // size_bytes()/span_bytes() expose logical byte count vs policy-rounded slot span.
    // For fixed forms configured == usable.
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] static constexpr size_type count() noexcept { return Count; }
    [[nodiscard]] static constexpr size_type size() noexcept { return BufferSize; }
    [[nodiscard]] static constexpr size_type size_bytes() noexcept { return static_cast<size_type>(sizeof(buffer_type)); }
    [[nodiscard]] static constexpr size_type span_bytes() noexcept { return static_cast<size_type>(sizeof(stored_buffer_type)); }
    [[nodiscard]] static constexpr size_type alignment() noexcept { return static_cast<size_type>(alignof(stored_buffer_type)); }
    [[nodiscard]] static constexpr size_type payload_bytes() noexcept { return size_bytes(); }
    [[nodiscard]] static constexpr size_type cache_span_bytes() noexcept { return span_bytes(); }
    [[nodiscard]] static constexpr size_type storage_alignment() noexcept { return alignment(); }
    [[nodiscard]] constexpr base_allocator_type get_allocator() const noexcept { return base_allocator_type{}; }

    // ------------------------------------------------------------------------------------------
    // Element Access
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] pointer data(const size_type index) noexcept { return (index < Count) ? buffers_[index].data() : nullptr; }
    [[nodiscard]] const_pointer data(const size_type index) const noexcept { return (index < Count) ? buffers_[index].data() : nullptr; }
    [[nodiscard]] pointer operator[](const size_type index) noexcept { SPSC_ASSERT(index < Count); return (index < Count) ? buffers_[index].data() : nullptr; }
    [[nodiscard]] const_pointer operator[](const size_type index) const noexcept { SPSC_ASSERT(index < Count); return (index < Count) ? buffers_[index].data() : nullptr; }

private:
    // ------------------------------------------------------------------------------------------
    // Storage
    // ------------------------------------------------------------------------------------------
    storage_type buffers_{};
};

/* =======================================================================
 * buffer_pool<T, BufferSize, 0>
 *
 * Static buffer size + dynamic count.
 * Storage is a dynamic array of promoted fixed-size buffer slots.
 * ======================================================================= */
template <class T, reg BufferSize, class Policy, class Alloc>
class buffer_pool<T, BufferSize, 0u, Policy, Alloc>
{
    static_assert(BufferSize > 0u, "[buffer_pool]: BufferSize must be > 0 in the fixed-buffer dynamic-count specialization.");
    static_assert(!std::is_const_v<T>, "[buffer_pool]: const T does not make sense for writable buffers.");
    static_assert(std::is_default_constructible_v<T>,
                  "[buffer_pool]: fixed-size buffers require default-constructible T.");
    static_assert(std::is_copy_assignable_v<T>,
                  "[buffer_pool]: fixed-size buffers require copy-assignable T.");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "[buffer_pool]: T destructor must be noexcept.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept default-constructible T.");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept copy-assignable T.");
#endif

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type = T;
    using size_type = reg;
    using policy_type = Policy;
    using base_allocator_type = Alloc;
    using buffer_type = std::array<value_type, BufferSize>;
    using stored_buffer_type = detail::cache_aligned_slot_t<buffer_type, policy_type>;
    using allocator_type = typename std::allocator_traits<base_allocator_type>::template rebind_alloc<stored_buffer_type>;
    using alloc_traits = std::allocator_traits<allocator_type>;
    using pointer = value_type*;
    using const_pointer = const value_type*;

    static constexpr size_type static_count = 0u;
    static constexpr size_type static_buffer_size = BufferSize;

    static_assert(std::is_nothrow_default_constructible_v<base_allocator_type>,
                  "[buffer_pool]: base allocator must be nothrow default-constructible.");
    static_assert(alloc_traits::is_always_equal::value,
                  "[buffer_pool]: dynamic buffer count requires always_equal allocator.");
    static_assert(std::is_nothrow_default_constructible_v<allocator_type>,
                  "[buffer_pool]: dynamic buffer count requires nothrow default-constructible allocator.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(
        alloc::detail::allocator_allocate_noexcept_v<allocator_type>,
        "[spsc::buffer_pool]: no-exceptions mode requires "
        "allocator::allocate(size_type) to be noexcept.");
#endif /* SPSC_ENABLE_EXCEPTIONS == 0 */
    static_assert(alloc::detail::allocator_size_covers_reg_v<allocator_type>,
                  "[buffer_pool]: allocator size_type must represent the reg domain.");
    static_assert(std::is_same_v<typename alloc_traits::pointer, stored_buffer_type*>,
                  "[buffer_pool]: dynamic buffer count requires allocator pointer type stored_buffer_type*.");
    static_assert(detail::allocator_supports_slot_alignment_v<base_allocator_type, stored_buffer_type>,
                  "[buffer_pool]: allocator rebind must preserve fixed-buffer slot alignment.");

    static_assert(alignof(stored_buffer_type) >= alloc::policy_storage_alignment_v<policy_type, buffer_type>,
                  "[buffer_pool]: fixed-buffer slot alignment must honor the policy storage alignment.");
    static_assert((sizeof(stored_buffer_type) % alignof(stored_buffer_type)) == 0u,
                  "[buffer_pool]: fixed-buffer slot size must be a multiple of its alignment.");
    static_assert(std::is_unsigned_v<size_type>,
                  "[buffer_pool]: reg (size_type) must be unsigned.");
    static_assert(sizeof(buffer_type) <= (std::numeric_limits<size_type>::max)(),
                  "[buffer_pool]: logical fixed-buffer byte size must fit in reg.");
    static_assert(sizeof(stored_buffer_type) <= (std::numeric_limits<size_type>::max)(),
                  "[buffer_pool]: effective fixed-buffer byte size must fit in reg.");

    static constexpr bool kNoexceptAllocate =
        alloc::detail::allocator_allocate_noexcept_v<allocator_type>;

    // ------------------------------------------------------------------------------------------
    // Constructors / Assignment
    // ------------------------------------------------------------------------------------------
    buffer_pool() noexcept = default;
    explicit buffer_pool(const size_type count) noexcept(kNoexceptAllocate && std::is_nothrow_default_constructible_v<stored_buffer_type>)
    {
        if (count != 0u) {
            (void)init_default_(count);
        }
    }
    ~buffer_pool() noexcept
    {
        destroy();
    }
    buffer_pool(const buffer_pool& other)
    {
        SPSC_ASSERT(other.is_valid());
        if (other.is_valid() && (other.count_ != 0u)) {
            (void)copy_from_(other);
        }
    }
    buffer_pool& operator=(const buffer_pool& other)
    {
        if (this == &other) {
            return *this;
        }

        SPSC_ASSERT(other.is_valid());
        if (!other.is_valid()) {
            destroy();
            return *this;
        }

        buffer_pool tmp;
        if ((other.count_ != 0u) && !tmp.copy_from_(other)) {
            return *this;
        }

        swap(tmp);
        return *this;
    }
    buffer_pool(buffer_pool&& other) noexcept
    {
        move_from_(std::move(other));
    }
    buffer_pool& operator=(buffer_pool&& other) noexcept
    {
        if (this != &other) {
            destroy();
            move_from_(std::move(other));
        }

        return *this;
    }

    void swap(buffer_pool& other) noexcept
    {
        SPSC_ASSERT(state_ok_(buffers_, count_));
        SPSC_ASSERT(state_ok_(other.buffers_, other.count_));

        if (this != &other) {
            std::swap(buffers_, other.buffers_);
            std::swap(count_, other.count_);
        }
    }

    friend void swap(buffer_pool& a, buffer_pool& b) noexcept { a.swap(b); }

    [[nodiscard]] bool resize(const size_type count) noexcept(
        kNoexceptAllocate &&
        std::is_nothrow_default_constructible_v<stored_buffer_type> &&
        std::is_nothrow_copy_assignable_v<stored_buffer_type>)
    {
        if ((count == count_) && is_valid()) {
            return true;
        }

        if (count == 0u) {
            destroy();
            return true;
        }

        stored_buffer_type* new_buffers = nullptr;
        if (!allocate_default_buffers_(count, new_buffers)) {
            return false;
        }

        const size_type copy_count = is_valid() ? ((count_ < count) ? count_ : count) : 0u;
        if (!copy_prefix_(new_buffers, count, buffers_, copy_count)) {
            destroy_buffer_block_(new_buffers, count);
            return false;
        }

        destroy();
        buffers_ = new_buffers;
        count_ = count;
        return true;
    }

    void destroy() noexcept
    {
        if (RB_UNLIKELY(!state_ok_(buffers_, count_))) {
            SPSC_ASSERT(false && "buffer_pool::destroy(): invalid fixed-buffer state");
            buffers_ = nullptr;
            count_ = 0u;
            return;
        }

        if (buffers_ != nullptr) {
            destroy_buffer_block_(buffers_, count_);
            buffers_ = nullptr;
        }

        count_ = 0u;
    }

    [[nodiscard]] bool is_valid() const noexcept { return state_ok_(buffers_, count_); }

    // ------------------------------------------------------------------------------------------
    // Size / Span Introspection
    //
    // count()/size() describe the usable logical shape.
    // span_bytes() exposes the physical policy-rounded span for valid storage.
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] size_type count() const noexcept { return is_valid() ? count_ : 0u; }
    [[nodiscard]] static constexpr size_type size() noexcept { return BufferSize; }
    [[nodiscard]] static constexpr size_type size_bytes() noexcept { return static_cast<size_type>(sizeof(buffer_type)); }
    [[nodiscard]] static constexpr size_type span_bytes() noexcept { return static_cast<size_type>(sizeof(stored_buffer_type)); }
    [[nodiscard]] static constexpr size_type alignment() noexcept { return static_cast<size_type>(alignof(stored_buffer_type)); }
    [[nodiscard]] static constexpr size_type payload_bytes() noexcept { return size_bytes(); }
    [[nodiscard]] static constexpr size_type cache_span_bytes() noexcept { return span_bytes(); }
    [[nodiscard]] static constexpr size_type storage_alignment() noexcept { return alignment(); }
    [[nodiscard]] constexpr base_allocator_type get_allocator() const noexcept { return base_allocator_type{}; }

    // ------------------------------------------------------------------------------------------
    // Element Access
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] pointer data(const size_type index) noexcept { return (is_valid() && (index < count_)) ? buffers_[index].data() : nullptr; }
    [[nodiscard]] const_pointer data(const size_type index) const noexcept { return (is_valid() && (index < count_)) ? buffers_[index].data() : nullptr; }
    [[nodiscard]] pointer operator[](const size_type index) noexcept { SPSC_ASSERT(is_valid()); SPSC_ASSERT(index < count_); return (is_valid() && (index < count_)) ? buffers_[index].data() : nullptr; }
    [[nodiscard]] const_pointer operator[](const size_type index) const noexcept { SPSC_ASSERT(is_valid()); SPSC_ASSERT(index < count_); return (is_valid() && (index < count_)) ? buffers_[index].data() : nullptr; }

private:
    // ------------------------------------------------------------------------------------------
    // Internal Helpers
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] static bool state_ok_(const stored_buffer_type* ptr, const size_type count) noexcept { return (count == 0u) ? (ptr == nullptr) : (ptr != nullptr); }

    static bool allocate_default_buffers_(const size_type count, stored_buffer_type*& out) noexcept(
        kNoexceptAllocate && std::is_nothrow_default_constructible_v<stored_buffer_type>)
    {
        allocator_type alloc{};
        stored_buffer_type* ptr = nullptr;
        size_type built = 0u;

        SPSC_TRY {
            ptr = alloc_traits::allocate(alloc, count);
            if (RB_UNLIKELY(ptr == nullptr)) {
                return false;
            }

            for (; built < count; ++built) {
                detail::construct_at_compat(ptr + built);
            }
        } SPSC_CATCH_ALL {
            if (ptr != nullptr) {
                std::destroy_n(ptr, built);
                alloc_traits::deallocate(alloc, ptr, count);
            }
            return false;
        }

        out = ptr;
        return true;
    }

    static void destroy_buffer_block_(stored_buffer_type* ptr, const size_type count) noexcept
    {
        if (ptr == nullptr) {
            return;
        }

        allocator_type alloc{};
        std::destroy_n(ptr, count);
        alloc_traits::deallocate(alloc, ptr, count);
    }

    static bool copy_prefix_(stored_buffer_type* dst,
                             const size_type dst_count,
                             const stored_buffer_type* src,
                             const size_type copy_count) noexcept(std::is_nothrow_copy_assignable_v<stored_buffer_type>)
    {
        (void)dst_count;
        SPSC_ASSERT(copy_count <= dst_count);
        SPSC_ASSERT((copy_count == 0u) || (dst != nullptr));
        SPSC_ASSERT((copy_count == 0u) || (src != nullptr));

        if (copy_count == 0u) {
            return true;
        }

        if ((dst == nullptr) || (src == nullptr)) {
            return false;
        }

        SPSC_TRY {
            for (size_type i = 0u; i < copy_count; ++i) {
                dst[i] = src[i];
            }
        } SPSC_CATCH_ALL {
            return false;
        }

        return true;
    }

    bool init_default_(const size_type count) noexcept(
        kNoexceptAllocate && std::is_nothrow_default_constructible_v<stored_buffer_type>)
    {
        stored_buffer_type* new_buffers = nullptr;
        if (!allocate_default_buffers_(count, new_buffers)) {
            return false;
        }

        buffers_ = new_buffers;
        count_ = count;
        return true;
    }

    bool copy_from_(const buffer_pool& other)
    {
        if (!other.is_valid()) {
            return false;
        }

        if (other.count_ == 0u) {
            return true;
        }

        stored_buffer_type* new_buffers = nullptr;
        if (!allocate_default_buffers_(other.count_, new_buffers)) {
            return false;
        }

        if (!copy_prefix_(new_buffers, other.count_, other.buffers_, other.count_)) {
            destroy_buffer_block_(new_buffers, other.count_);
            return false;
        }

        buffers_ = new_buffers;
        count_ = other.count_;
        return true;
    }

    void move_from_(buffer_pool&& other) noexcept
    {
        buffers_ = other.buffers_;
        count_ = other.count_;

        other.buffers_ = nullptr;
        other.count_ = 0u;
    }

private:
    stored_buffer_type* buffers_{nullptr};
    size_type count_{0u};
};

/* =======================================================================
 * buffer_pool<T, 0, Count>
 *
 * Dynamic buffer size + static count.
 * Storage is a fixed array of T*; each buffer payload is allocated separately.
 * ======================================================================= */
template <class T, reg Count, class Policy, class Alloc>
class buffer_pool<T, 0u, Count, Policy, Alloc>
{
    static_assert(Count > 0u, "[buffer_pool]: Count must be > 0 in the dynamic-buffer static-count specialization.");
    static_assert(!std::is_const_v<T>, "[buffer_pool]: const T does not make sense for writable buffers.");
    static_assert(std::is_default_constructible_v<T>,
                  "[buffer_pool]: dynamic-size buffers require default-constructible T.");
    static_assert(std::is_copy_assignable_v<T>,
                  "[buffer_pool]: dynamic-size buffers require copy-assignable T.");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "[buffer_pool]: T destructor must be noexcept.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept default-constructible T.");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept copy-assignable T.");
#endif

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type = T;
    using size_type = reg;
    using policy_type = Policy;
    using base_allocator_type = Alloc;
    using byte_type = std::byte;
    using byte_allocator_type = typename std::allocator_traits<base_allocator_type>::template rebind_alloc<byte_type>;
    using byte_alloc_traits = std::allocator_traits<byte_allocator_type>;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using byte_pointer = typename byte_alloc_traits::pointer;
    using table_allocator_type = typename std::allocator_traits<base_allocator_type>::template rebind_alloc<pointer>;
    using table_alloc_traits = std::allocator_traits<table_allocator_type>;
    using table_pointer = typename table_alloc_traits::pointer;

    static constexpr size_type static_count = Count;
    static constexpr size_type static_buffer_size = 0u;

    static_assert(std::is_nothrow_default_constructible_v<base_allocator_type>,
                  "[buffer_pool]: base allocator must be nothrow default-constructible.");
    static_assert(byte_alloc_traits::is_always_equal::value,
                  "[buffer_pool]: dynamic buffer size requires always_equal allocator.");
    static_assert(std::is_nothrow_default_constructible_v<byte_allocator_type>,
                  "[buffer_pool]: dynamic buffer size requires nothrow default-constructible allocator.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(
        alloc::detail::allocator_allocate_noexcept_v<byte_allocator_type>,
        "[spsc::buffer_pool]: no-exceptions mode requires byte "
        "allocator::allocate(size_type) to be noexcept.");
#endif /* SPSC_ENABLE_EXCEPTIONS == 0 */
    static_assert(alloc::detail::allocator_size_covers_reg_v<byte_allocator_type>,
                  "[buffer_pool]: byte allocator size_type must represent the reg domain.");
    static_assert(std::is_same_v<byte_pointer, byte_type*>,
                   "[buffer_pool]: dynamic buffer size requires allocator pointer type std::byte*.");
    static_assert(table_alloc_traits::is_always_equal::value,
                  "[buffer_pool]: pointer-table allocator must be always_equal.");
    static_assert(std::is_nothrow_default_constructible_v<table_allocator_type>,
                  "[buffer_pool]: pointer-table allocator must be nothrow default-constructible.");
    static_assert(alloc::detail::allocator_size_covers_reg_v<table_allocator_type>,
                  "[buffer_pool]: pointer-table allocator size_type must represent the reg domain.");
    static_assert(std::is_same_v<table_pointer, pointer*>,
                  "[buffer_pool]: pointer-table allocator must expose T** (raw).");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(
        alloc::detail::allocator_allocate_noexcept_v<table_allocator_type>,
        "[spsc::buffer_pool]: no-exceptions mode requires pointer-table "
        "allocator::allocate(size_type) to be noexcept.");
#endif /* SPSC_ENABLE_EXCEPTIONS == 0 */
    static_assert(alloc::rebind_allocator_min_alignment_v<base_allocator_type, byte_type> >=
                      alloc::policy_storage_alignment_v<policy_type, value_type>,
                  "[buffer_pool]: allocator rebind must preserve runtime-buffer alignment.");
    static_assert(std::is_unsigned_v<size_type>,
                  "[buffer_pool]: reg (size_type) must be unsigned.");

    static constexpr bool kNoexceptByteAllocate =
        alloc::detail::allocator_allocate_noexcept_v<byte_allocator_type>;
    static constexpr bool kNoexceptTableAllocate =
        alloc::detail::allocator_allocate_noexcept_v<table_allocator_type>;
    static constexpr bool kNoexceptAllocate =
        kNoexceptByteAllocate && kNoexceptTableAllocate;

    // ------------------------------------------------------------------------------------------
    // Constructors / Assignment
    // ------------------------------------------------------------------------------------------
    buffer_pool() noexcept = default;
    explicit buffer_pool(const size_type buffer_size) noexcept(
        kNoexceptAllocate && std::is_nothrow_default_constructible_v<value_type>)
    {
        if (buffer_size != 0u) {
            (void)init_size_(buffer_size);
        }
    }
    ~buffer_pool() noexcept
    {
        destroy();
    }
    buffer_pool(const buffer_pool& other)
    {
        SPSC_ASSERT(other.is_valid());
        if (other.is_valid() && (other.buffer_size_ != 0u)) {
            (void)copy_from_(other);
        }
    }
    buffer_pool& operator=(const buffer_pool& other)
    {
        if (this == &other) {
            return *this;
        }

        SPSC_ASSERT(other.is_valid());
        if (!other.is_valid()) {
            destroy();
            return *this;
        }

        if (!copy_from_(other)) {
            return *this;
        }
        return *this;
    }
    buffer_pool(buffer_pool&& other) noexcept
    {
        move_from_(std::move(other));
    }
    buffer_pool& operator=(buffer_pool&& other) noexcept
    {
        if (this != &other) {
            destroy();
            move_from_(std::move(other));
        }

        return *this;
    }

    void swap(buffer_pool& other) noexcept
    {
        SPSC_ASSERT(state_ok_(buffers_, buffer_size_));
        SPSC_ASSERT(state_ok_(other.buffers_, other.buffer_size_));

        if (this != &other) {
            buffers_.swap(other.buffers_);
            std::swap(buffer_size_, other.buffer_size_);
        }
    }

    friend void swap(buffer_pool& a, buffer_pool& b) noexcept { a.swap(b); }

    [[nodiscard]] bool resize(const size_type buffer_size) noexcept(
        kNoexceptAllocate &&
        std::is_nothrow_default_constructible_v<value_type> &&
        std::is_nothrow_copy_assignable_v<value_type>)
    {
        if ((buffer_size == buffer_size_) && is_valid()) {
            return true;
        }

        if (buffer_size == 0u) {
            destroy();
            return true;
        }

        scratch_table_type scratch{};
        if (!allocate_scratch_(scratch) ||
            !allocate_all_buffers_(buffer_size, scratch.data())) {
            return false;
        }

        const size_type copy_size = is_valid() ? ((buffer_size_ < buffer_size) ? buffer_size_ : buffer_size) : 0u;
        if (!copy_prefix_(scratch.data(), buffer_size, buffers_.data(), copy_size)) {
            release_all_buffers_(scratch.data(), buffer_size);
            return false;
        }

        destroy();
        commit_scratch_(scratch.data());
        buffer_size_ = buffer_size;
        return true;
    }

    void destroy() noexcept
    {
        if (RB_UNLIKELY(!state_ok_(buffers_, buffer_size_))) {
            SPSC_ASSERT(false && "buffer_pool::destroy(): invalid runtime-buffer state");
        }

        if (state_ok_(buffers_, buffer_size_)) {
            release_all_buffers_(buffers_.data(), buffer_size_);
        } else {
            clear_slots_();
        }

        buffer_size_ = 0u;
    }

    [[nodiscard]] bool is_valid() const noexcept { return state_ok_(buffers_, buffer_size_); }

    // ------------------------------------------------------------------------------------------
    // Size / Span Introspection
    //
    // size()/size_bytes() expose usable logical size.
    // span_bytes() exposes the physical policy-rounded span for valid storage.
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] static constexpr size_type count() noexcept { return Count; }
    [[nodiscard]] size_type size() const noexcept { return is_valid() ? buffer_size_ : 0u; }
    [[nodiscard]] size_type size_bytes() const noexcept { return is_valid() ? detail::logical_buffer_bytes<size_type, value_type>(buffer_size_) : 0u; }
    [[nodiscard]] size_type span_bytes() const noexcept { return is_valid() ? effective_buffer_size_bytes_(buffer_size_) : 0u; }
    [[nodiscard]] static constexpr size_type alignment() noexcept { return static_cast<size_type>(alloc::policy_storage_alignment_v<policy_type, value_type>); }
    [[nodiscard]] size_type payload_bytes() const noexcept { return size_bytes(); }
    [[nodiscard]] size_type cache_span_bytes() const noexcept { return span_bytes(); }
    [[nodiscard]] static constexpr size_type storage_alignment() noexcept { return alignment(); }
    [[nodiscard]] constexpr base_allocator_type get_allocator() const noexcept { return base_allocator_type{}; }

    // ------------------------------------------------------------------------------------------
    // Element Access
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] pointer data(const size_type index) noexcept { return (is_valid() && (index < Count)) ? buffers_[index] : nullptr; }
    [[nodiscard]] const_pointer data(const size_type index) const noexcept { return (is_valid() && (index < Count)) ? buffers_[index] : nullptr; }
    [[nodiscard]] pointer operator[](const size_type index) noexcept
    {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(buffer_size_ != 0u);
        SPSC_ASSERT(index < Count);
        return (is_valid() && (buffer_size_ != 0u) && (index < Count)) ? buffers_[index] : nullptr;
    }
    [[nodiscard]] const_pointer operator[](const size_type index) const noexcept
    {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(buffer_size_ != 0u);
        SPSC_ASSERT(index < Count);
        return (is_valid() && (buffer_size_ != 0u) && (index < Count)) ? buffers_[index] : nullptr;
    }

private:
    // ------------------------------------------------------------------------------------------
    // Internal Helpers
    // ------------------------------------------------------------------------------------------
    using scratch_table_type =
        alloc::detail::pointer_table_scratch<pointer, base_allocator_type>;

    [[nodiscard]] static bool state_ok_(const std::array<pointer, Count>& slot_ptrs,
                                        const size_type buffer_size) noexcept
    {
        bool all_null = true;
        bool all_present = true;

        for (const auto ptr : slot_ptrs) {
            all_null &= (ptr == nullptr);
            all_present &= (ptr != nullptr);
        }

        return (buffer_size == 0u) ? all_null : all_present;
    }

    static bool allocate_buffer_(const size_type buffer_size, pointer& out) noexcept(
        kNoexceptByteAllocate && std::is_nothrow_default_constructible_v<value_type>)
    {
        const size_type effective_bytes = effective_buffer_size_bytes_(buffer_size);
        if (RB_UNLIKELY(effective_bytes == 0u)) {
            return false;
        }

        byte_allocator_type alloc{};
        byte_pointer raw = nullptr;
        pointer ptr = nullptr;
        size_type built = 0u;

        SPSC_TRY {
            raw = byte_alloc_traits::allocate(alloc, effective_bytes);
            if (RB_UNLIKELY(raw == nullptr)) {
                return false;
            }

            ptr = reinterpret_cast<pointer>(raw);
            for (; built < buffer_size; ++built) {
                detail::construct_at_compat(ptr + built);
            }
        } SPSC_CATCH_ALL {
            if (ptr != nullptr) {
                std::destroy_n(ptr, built);
            }
            if (raw != nullptr) {
                byte_alloc_traits::deallocate(alloc, raw, effective_bytes);
            }
            return false;
        }

        out = ptr;
        return true;
    }

    static void release_buffer_(pointer ptr, const size_type buffer_size) noexcept
    {
        if (ptr == nullptr) {
            return;
        }

        const size_type effective_bytes = effective_buffer_size_bytes_(buffer_size);
        byte_allocator_type alloc{};
        std::destroy_n(ptr, buffer_size);
        byte_alloc_traits::deallocate(alloc,
                                      reinterpret_cast<byte_pointer>(ptr),
                                      effective_bytes);
    }

    static bool allocate_all_buffers_(const size_type buffer_size, pointer* const out) noexcept(
        kNoexceptByteAllocate && std::is_nothrow_default_constructible_v<value_type>)
    {
        SPSC_ASSERT(out != nullptr);
        if (RB_UNLIKELY(out == nullptr)) {
            return false;
        }

        for (size_type i = 0u; i < Count; ++i) {
            if (!allocate_buffer_(buffer_size, out[i])) {
                for (size_type j = 0u; j < i; ++j) {
                    release_buffer_(out[j], buffer_size);
                    out[j] = nullptr;
                }
                return false;
            }
        }

        return true;
    }

    static void release_all_buffers_(pointer* const slot_ptrs,
                                     const size_type buffer_size) noexcept
    {
        if (slot_ptrs == nullptr) {
            return;
        }
        for (size_type i = 0u; i < Count; ++i) {
            release_buffer_(slot_ptrs[i], buffer_size);
            slot_ptrs[i] = nullptr;
        }
    }

    static bool copy_prefix_(pointer* const dst,
                             const size_type dst_buffer_size,
                             pointer const* const src,
                             const size_type copy_size) noexcept(std::is_nothrow_copy_assignable_v<value_type>)
    {
        (void)dst_buffer_size;
        if (copy_size == 0u) {
            return true;
        }

        SPSC_ASSERT(copy_size <= dst_buffer_size);

        SPSC_TRY {
            for (size_type slot = 0u; slot < Count; ++slot) {
                SPSC_ASSERT(dst[slot] != nullptr);
                SPSC_ASSERT(src[slot] != nullptr);
                if ((dst[slot] == nullptr) || (src[slot] == nullptr)) {
                    return false;
                }
                for (size_type i = 0u; i < copy_size; ++i) {
                    dst[slot][i] = src[slot][i];
                }
            }
        } SPSC_CATCH_ALL {
            return false;
        }

        return true;
    }

    static bool allocate_scratch_(scratch_table_type& scratch) noexcept(
        kNoexceptTableAllocate)
    {
        SPSC_TRY {
            return scratch.allocate(Count);
        } SPSC_CATCH_ALL {
            return false;
        }
    }

    void commit_scratch_(pointer const* const fresh) noexcept
    {
        SPSC_ASSERT(fresh != nullptr);
        for (size_type i = 0u; i < Count; ++i) {
            buffers_[i] = fresh[i];
        }
    }

    bool init_size_(const size_type buffer_size) noexcept(
        kNoexceptAllocate && std::is_nothrow_default_constructible_v<value_type>)
    {
        scratch_table_type scratch{};
        if (!allocate_scratch_(scratch) ||
            !allocate_all_buffers_(buffer_size, scratch.data())) {
            return false;
        }

        commit_scratch_(scratch.data());
        buffer_size_ = buffer_size;
        return true;
    }

    bool copy_from_(const buffer_pool& other)
    {
        if (!other.is_valid()) {
            return false;
        }

        if (other.buffer_size_ == 0u) {
            destroy();
            return true;
        }

        scratch_table_type scratch{};
        if (!allocate_scratch_(scratch) ||
            !allocate_all_buffers_(other.buffer_size_, scratch.data())) {
            return false;
        }

        if (!copy_prefix_(scratch.data(), other.buffer_size_,
                          other.buffers_.data(), other.buffer_size_)) {
            release_all_buffers_(scratch.data(), other.buffer_size_);
            return false;
        }

        destroy();
        commit_scratch_(scratch.data());
        buffer_size_ = other.buffer_size_;
        return true;
    }

    void move_from_(buffer_pool&& other) noexcept
    {
        buffers_ = other.buffers_;
        buffer_size_ = other.buffer_size_;

        other.clear_slots_();
        other.buffer_size_ = 0u;
    }

    void clear_slots_() noexcept
    {
        for (auto& ptr : buffers_) {
            ptr = nullptr;
        }
    }

    [[nodiscard]] static constexpr size_type effective_buffer_size_bytes_(const size_type logical_size) noexcept
    {
        return detail::effective_buffer_bytes<size_type, value_type, policy_type, base_allocator_type>(
            logical_size);
    }

private:
    std::array<pointer, Count> buffers_{};
    size_type buffer_size_{0u};
};

/* =======================================================================
 * buffer_pool<T, 0, 0>
 *
 * Dynamic buffer size + dynamic count.
 * Storage is a dynamic T** table; each buffer payload is allocated separately.
 * ======================================================================= */
template <class T, class Policy, class Alloc>
class buffer_pool<T, 0u, 0u, Policy, Alloc>
{
    static_assert(!std::is_const_v<T>, "[buffer_pool]: const T does not make sense for writable buffers.");
    static_assert(std::is_default_constructible_v<T>,
                  "[buffer_pool]: dynamic buffers require default-constructible T.");
    static_assert(std::is_copy_assignable_v<T>,
                  "[buffer_pool]: dynamic buffers require copy-assignable T.");
    static_assert(std::is_nothrow_destructible_v<T>,
                  "[buffer_pool]: T destructor must be noexcept.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept default-constructible T.");
    static_assert(std::is_nothrow_copy_assignable_v<T>,
                  "[buffer_pool]: no-exceptions mode requires noexcept copy-assignable T.");
#endif

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type = T;
    using size_type = reg;
    using policy_type = Policy;
    using base_allocator_type = Alloc;
    using slot_allocator_type = typename std::allocator_traits<base_allocator_type>::template rebind_alloc<value_type*>;
    using slot_alloc_traits = std::allocator_traits<slot_allocator_type>;
    using byte_type = std::byte;
    using byte_allocator_type = typename std::allocator_traits<base_allocator_type>::template rebind_alloc<byte_type>;
    using byte_alloc_traits = std::allocator_traits<byte_allocator_type>;
    using pointer = value_type*;
    using const_pointer = const value_type*;
    using byte_pointer = typename byte_alloc_traits::pointer;

    static constexpr size_type static_count = 0u;
    static constexpr size_type static_buffer_size = 0u;

    static_assert(std::is_nothrow_default_constructible_v<base_allocator_type>,
                  "[buffer_pool]: base allocator must be nothrow default-constructible.");
    static_assert(slot_alloc_traits::is_always_equal::value,
                  "[buffer_pool]: dynamic count requires always_equal slot allocator.");
    static_assert(byte_alloc_traits::is_always_equal::value,
                  "[buffer_pool]: dynamic buffer size requires always_equal object allocator.");
    static_assert(std::is_nothrow_default_constructible_v<slot_allocator_type>,
                  "[buffer_pool]: dynamic count requires nothrow default-constructible slot allocator.");
    static_assert(std::is_nothrow_default_constructible_v<byte_allocator_type>,
                  "[buffer_pool]: dynamic buffer size requires nothrow default-constructible object allocator.");
#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(
        alloc::detail::allocator_allocate_noexcept_v<slot_allocator_type>,
        "[spsc::buffer_pool]: no-exceptions mode requires slot "
        "allocator::allocate(size_type) to be noexcept.");
    static_assert(
        alloc::detail::allocator_allocate_noexcept_v<byte_allocator_type>,
        "[spsc::buffer_pool]: no-exceptions mode requires byte "
        "allocator::allocate(size_type) to be noexcept.");
#endif /* SPSC_ENABLE_EXCEPTIONS == 0 */
    static_assert(alloc::detail::allocator_size_covers_reg_v<slot_allocator_type>,
                  "[buffer_pool]: slot allocator size_type must represent the reg domain.");
    static_assert(alloc::detail::allocator_size_covers_reg_v<byte_allocator_type>,
                  "[buffer_pool]: byte allocator size_type must represent the reg domain.");
    static_assert(std::is_same_v<typename slot_alloc_traits::pointer, pointer*>,
                  "[buffer_pool]: dynamic count requires slot allocator pointer type T**.");
    static_assert(std::is_same_v<byte_pointer, byte_type*>,
                  "[buffer_pool]: dynamic buffer size requires object allocator pointer type std::byte*.");
    static_assert(alloc::rebind_allocator_min_alignment_v<base_allocator_type, byte_type> >=
                      alloc::policy_storage_alignment_v<policy_type, value_type>,
                  "[buffer_pool]: allocator rebind must preserve runtime-buffer alignment.");
    static_assert(std::is_unsigned_v<size_type>,
                  "[buffer_pool]: reg (size_type) must be unsigned.");

    static constexpr bool kNoexceptAllocateSlots =
        alloc::detail::allocator_allocate_noexcept_v<slot_allocator_type>;
    static constexpr bool kNoexceptAllocateObjects =
        alloc::detail::allocator_allocate_noexcept_v<byte_allocator_type>;

    // ------------------------------------------------------------------------------------------
    // Constructors / Assignment
    // ------------------------------------------------------------------------------------------
    buffer_pool() noexcept = default;
    buffer_pool(const size_type count, const size_type buffer_size) noexcept(
        kNoexceptAllocateSlots &&
        kNoexceptAllocateObjects &&
        std::is_nothrow_default_constructible_v<value_type>)
    {
        if ((count != 0u) && (buffer_size != 0u)) {
            (void)init_shape_(count, buffer_size);
        }
    }
    ~buffer_pool() noexcept
    {
        destroy();
    }
    buffer_pool(const buffer_pool& other)
    {
        SPSC_ASSERT(other.is_valid());
        if (other.is_valid() && (other.count_ != 0u) && (other.buffer_size_ != 0u)) {
            (void)copy_from_(other);
        }
    }
    buffer_pool& operator=(const buffer_pool& other)
    {
        if (this == &other) {
            return *this;
        }

        SPSC_ASSERT(other.is_valid());
        if (!other.is_valid()) {
            destroy();
            return *this;
        }

        buffer_pool tmp;
        if ((other.count_ != 0u) && (other.buffer_size_ != 0u) && !tmp.copy_from_(other)) {
            return *this;
        }

        swap(tmp);
        return *this;
    }
    buffer_pool(buffer_pool&& other) noexcept
    {
        move_from_(std::move(other));
    }
    buffer_pool& operator=(buffer_pool&& other) noexcept
    {
        if (this != &other) {
            destroy();
            move_from_(std::move(other));
        }

        return *this;
    }

    void swap(buffer_pool& other) noexcept
    {
        SPSC_ASSERT(state_ok_(buffers_, count_, buffer_size_));
        SPSC_ASSERT(state_ok_(other.buffers_, other.count_, other.buffer_size_));

        if (this != &other) {
            std::swap(buffers_, other.buffers_);
            std::swap(count_, other.count_);
            std::swap(buffer_size_, other.buffer_size_);
        }
    }

    friend void swap(buffer_pool& a, buffer_pool& b) noexcept { a.swap(b); }

    [[nodiscard]] bool resize(const size_type count, const size_type buffer_size) noexcept(
        kNoexceptAllocateSlots &&
        kNoexceptAllocateObjects &&
        std::is_nothrow_default_constructible_v<value_type> &&
        std::is_nothrow_copy_assignable_v<value_type>)
    {
        if ((count == 0u) || (buffer_size == 0u)) {
            destroy();
            return true;
        }

        if ((count == count_) && (buffer_size == buffer_size_) && is_valid()) {
            return true;
        }

        pointer* new_buffers = nullptr;
        if (!allocate_shape_(count, buffer_size, new_buffers)) {
            return false;
        }

        const size_type copy_count = is_valid() ? ((count_ < count) ? count_ : count) : 0u;
        const size_type copy_size = is_valid() ? ((buffer_size_ < buffer_size) ? buffer_size_ : buffer_size) : 0u;
        if (!copy_prefix_(new_buffers, count, buffer_size, buffers_, copy_count, copy_size)) {
            destroy_shape_(new_buffers, count, buffer_size);
            return false;
        }

        destroy();
        buffers_ = new_buffers;
        count_ = count;
        buffer_size_ = buffer_size;
        return true;
    }

    void destroy() noexcept
    {
        if (RB_UNLIKELY(!state_ok_(buffers_, count_, buffer_size_))) {
            SPSC_ASSERT(false && "buffer_pool::destroy(): invalid fully-dynamic state");
        }

        if (state_ok_(buffers_, count_, buffer_size_)) {
            destroy_shape_(buffers_, count_, buffer_size_);
        }

        buffers_ = nullptr;
        count_ = 0u;
        buffer_size_ = 0u;
    }

    [[nodiscard]] bool is_valid() const noexcept { return state_ok_(buffers_, count_, buffer_size_); }

    // ------------------------------------------------------------------------------------------
    // Size / Span Introspection
    //
    // count()/size() expose usable logical shape.
    // span_bytes() exposes the physical policy-rounded span for valid storage.
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] size_type count() const noexcept { return is_valid() ? count_ : 0u; }
    [[nodiscard]] size_type size() const noexcept { return is_valid() ? buffer_size_ : 0u; }
    [[nodiscard]] size_type size_bytes() const noexcept { return is_valid() ? detail::logical_buffer_bytes<size_type, value_type>(buffer_size_) : 0u; }
    [[nodiscard]] size_type span_bytes() const noexcept { return is_valid() ? effective_buffer_size_bytes_(buffer_size_) : 0u; }
    [[nodiscard]] static constexpr size_type alignment() noexcept { return static_cast<size_type>(alloc::policy_storage_alignment_v<policy_type, value_type>); }
    [[nodiscard]] size_type payload_bytes() const noexcept { return size_bytes(); }
    [[nodiscard]] size_type cache_span_bytes() const noexcept { return span_bytes(); }
    [[nodiscard]] static constexpr size_type storage_alignment() noexcept { return alignment(); }
    [[nodiscard]] constexpr base_allocator_type get_allocator() const noexcept { return base_allocator_type{}; }

    // ------------------------------------------------------------------------------------------
    // Element Access
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] pointer data(const size_type index) noexcept { return (is_valid() && (index < count_)) ? buffers_[index] : nullptr; }
    [[nodiscard]] const_pointer data(const size_type index) const noexcept { return (is_valid() && (index < count_)) ? buffers_[index] : nullptr; }
    [[nodiscard]] pointer operator[](const size_type index) noexcept { SPSC_ASSERT(is_valid()); SPSC_ASSERT(index < count_); return (is_valid() && (index < count_)) ? buffers_[index] : nullptr; }
    [[nodiscard]] const_pointer operator[](const size_type index) const noexcept { SPSC_ASSERT(is_valid()); SPSC_ASSERT(index < count_); return (is_valid() && (index < count_)) ? buffers_[index] : nullptr; }

private:
    // ------------------------------------------------------------------------------------------
    // Internal Helpers
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] static bool state_ok_(pointer* slot_ptrs,
                                        const size_type count,
                                        const size_type buffer_size) noexcept
    {
        if ((count == 0u) || (buffer_size == 0u)) {
            return (count == 0u) && (buffer_size == 0u) && (slot_ptrs == nullptr);
        }

        if (slot_ptrs == nullptr) {
            return false;
        }

        for (size_type i = 0u; i < count; ++i) {
            if (slot_ptrs[i] == nullptr) {
                return false;
            }
        }

        return true;
    }

    static bool allocate_buffer_(const size_type buffer_size, pointer& out) noexcept(
        kNoexceptAllocateObjects && std::is_nothrow_default_constructible_v<value_type>)
    {
        const size_type effective_bytes = effective_buffer_size_bytes_(buffer_size);
        if (RB_UNLIKELY(effective_bytes == 0u)) {
            return false;
        }

        byte_allocator_type alloc{};
        byte_pointer raw = nullptr;
        pointer ptr = nullptr;
        size_type built = 0u;

        SPSC_TRY {
            raw = byte_alloc_traits::allocate(alloc, effective_bytes);
            if (RB_UNLIKELY(raw == nullptr)) {
                return false;
            }

            ptr = reinterpret_cast<pointer>(raw);
            for (; built < buffer_size; ++built) {
                detail::construct_at_compat(ptr + built);
            }
        } SPSC_CATCH_ALL {
            if (ptr != nullptr) {
                std::destroy_n(ptr, built);
            }
            if (raw != nullptr) {
                byte_alloc_traits::deallocate(alloc, raw, effective_bytes);
            }
            return false;
        }

        out = ptr;
        return true;
    }

    static void release_buffer_(pointer ptr, const size_type buffer_size) noexcept
    {
        if (ptr == nullptr) {
            return;
        }

        const size_type effective_bytes = effective_buffer_size_bytes_(buffer_size);
        byte_allocator_type alloc{};
        std::destroy_n(ptr, buffer_size);
        byte_alloc_traits::deallocate(alloc,
                                      reinterpret_cast<byte_pointer>(ptr),
                                      effective_bytes);
    }

    static bool allocate_shape_(const size_type count,
                                const size_type buffer_size,
                                pointer*& out) noexcept(
        kNoexceptAllocateSlots &&
        kNoexceptAllocateObjects &&
        std::is_nothrow_default_constructible_v<value_type>)
    {
        slot_allocator_type slot_alloc{};
        pointer* slot_ptrs = nullptr;
        size_type built = 0u;

        SPSC_TRY {
            slot_ptrs = slot_alloc_traits::allocate(slot_alloc, count);
            if (RB_UNLIKELY(slot_ptrs == nullptr)) {
                return false;
            }

            for (size_type i = 0u; i < count; ++i) {
                (void)::new (static_cast<void*>(slot_ptrs + i))
                    pointer(nullptr);
            }

            for (; built < count; ++built) {
                if (!allocate_buffer_(buffer_size, slot_ptrs[built])) {
                    for (size_type i = 0u; i < built; ++i) {
                        release_buffer_(slot_ptrs[i], buffer_size);
                        slot_ptrs[i] = nullptr;
                    }
                    slot_alloc_traits::deallocate(slot_alloc, slot_ptrs, count);
                    return false;
                }
            }
        } SPSC_CATCH_ALL {
            if (slot_ptrs != nullptr) {
                for (size_type i = 0u; i < built; ++i) {
                    release_buffer_(slot_ptrs[i], buffer_size);
                    slot_ptrs[i] = nullptr;
                }
                slot_alloc_traits::deallocate(slot_alloc, slot_ptrs, count);
            }
            return false;
        }

        out = slot_ptrs;
        return true;
    }

    static void destroy_shape_(pointer* slot_ptrs,
                               const size_type count,
                               const size_type buffer_size) noexcept
    {
        if (slot_ptrs == nullptr) {
            return;
        }

        for (size_type i = 0u; i < count; ++i) {
            release_buffer_(slot_ptrs[i], buffer_size);
            slot_ptrs[i] = nullptr;
        }

        slot_allocator_type slot_alloc{};
        slot_alloc_traits::deallocate(slot_alloc, slot_ptrs, count);
    }

    static bool copy_prefix_(pointer* dst,
                             const size_type dst_count,
                             const size_type dst_buffer_size,
                             const pointer* src,
                             const size_type copy_count,
                             const size_type copy_size) noexcept(std::is_nothrow_copy_assignable_v<value_type>)
    {
        (void)dst_count;
        (void)dst_buffer_size;
        SPSC_ASSERT(copy_count <= dst_count);
        SPSC_ASSERT(copy_size <= dst_buffer_size);
        SPSC_ASSERT(((copy_count == 0u) || (copy_size == 0u)) || (dst != nullptr));
        SPSC_ASSERT(((copy_count == 0u) || (copy_size == 0u)) || (src != nullptr));

        if ((copy_count == 0u) || (copy_size == 0u)) {
            return true;
        }

        if ((dst == nullptr) || (src == nullptr)) {
            return false;
        }

        SPSC_TRY {
            for (size_type slot = 0u; slot < copy_count; ++slot) {
                SPSC_ASSERT(dst[slot] != nullptr);
                SPSC_ASSERT(src[slot] != nullptr);
                if ((dst[slot] == nullptr) || (src[slot] == nullptr)) {
                    return false;
                }
                for (size_type i = 0u; i < copy_size; ++i) {
                    dst[slot][i] = src[slot][i];
                }
            }
        } SPSC_CATCH_ALL {
            return false;
        }

        return true;
    }

    bool init_shape_(const size_type count, const size_type buffer_size) noexcept(
        kNoexceptAllocateSlots &&
        kNoexceptAllocateObjects &&
        std::is_nothrow_default_constructible_v<value_type>)
    {
        pointer* new_buffers = nullptr;
        if (!allocate_shape_(count, buffer_size, new_buffers)) {
            return false;
        }

        buffers_ = new_buffers;
        count_ = count;
        buffer_size_ = buffer_size;
        return true;
    }

    bool copy_from_(const buffer_pool& other)
    {
        if (!other.is_valid()) {
            return false;
        }

        if ((other.count_ == 0u) || (other.buffer_size_ == 0u)) {
            return true;
        }

        pointer* new_buffers = nullptr;
        if (!allocate_shape_(other.count_, other.buffer_size_, new_buffers)) {
            return false;
        }

        if (!copy_prefix_(new_buffers, other.count_, other.buffer_size_, other.buffers_, other.count_, other.buffer_size_)) {
            destroy_shape_(new_buffers, other.count_, other.buffer_size_);
            return false;
        }

        buffers_ = new_buffers;
        count_ = other.count_;
        buffer_size_ = other.buffer_size_;
        return true;
    }

    void move_from_(buffer_pool&& other) noexcept
    {
        buffers_ = other.buffers_;
        count_ = other.count_;
        buffer_size_ = other.buffer_size_;

        other.buffers_ = nullptr;
        other.count_ = 0u;
        other.buffer_size_ = 0u;
    }

    [[nodiscard]] static constexpr size_type effective_buffer_size_bytes_(const size_type logical_size) noexcept
    {
        return detail::effective_buffer_bytes<size_type, value_type, policy_type, base_allocator_type>(
            logical_size);
    }

private:
    pointer* buffers_{nullptr};
    size_type count_{0u};
    size_type buffer_size_{0u};
};


/* =======================================================================
 * Public aliases
 *
 * Keep the original buffer_pool<T, BufferSize, Count, ...> interface for
 * users who want the explicit 0-sentinel shapes, but also expose readable
 * aliases for the four common forms.
 * ======================================================================= */

template <
    class T,
    reg BufferSize,
    reg Count,
    class Policy = policy::default_policy,
    class Alloc = alloc::policy_default_value_alloc_t<
        Policy,
        T,
        alloc::default_alloc
    >
>
using static_buffer_pool = buffer_pool<T, BufferSize, Count, Policy, Alloc>;

template <
    class T,
    reg BufferSize,
    class Policy = policy::default_policy,
    class Alloc = alloc::policy_default_value_alloc_t<
        Policy,
        T,
        alloc::default_alloc
    >
>
using fixed_buffer_pool = buffer_pool<T, BufferSize, 0u, Policy, Alloc>;

template <
    class T,
    reg Count,
    class Policy = policy::default_policy,
    class Alloc = alloc::policy_default_value_alloc_t<
        Policy,
        T,
        alloc::default_alloc
    >
>
using fixed_count_buffer_pool = buffer_pool<T, 0u, Count, Policy, Alloc>;

template <
    class T,
    class Policy = policy::default_policy,
    class Alloc = alloc::policy_default_value_alloc_t<
        Policy,
        T,
        alloc::default_alloc
    >
>
using dynamic_buffer_pool = buffer_pool<T, 0u, 0u, Policy, Alloc>;

} // namespace spsc

#endif /* SPSC_BUFFER_POOL_HPP_ */
