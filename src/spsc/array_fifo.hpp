/*
 * array_fifo.hpp
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPSC_BASE_ARRAY_FIFO_HPP_
#define SPSC_BASE_ARRAY_FIFO_HPP_

#include "base/spsc_slot_wrap.hpp" // ::spsc::detail::cache_aligned_slot_t
#include "base/spsc_alloc.hpp" // ::spsc::alloc::default_alloc
#include "fifo.hpp"            // ::spsc::fifo
#include "fifo_view.hpp"       // ::spsc::fifo_view
#include <array>
#include <utility>

namespace spsc {

/* ========================================================================
 * array_fifo: FIFO over std::array<T, N> (owning storage)
 *
 *  - Uses ::spsc::fifo<std::array<T, N>, FifoCapacity, Policy, Alloc>.
 *  - Value-based producer API (push / try_push / emplace / try_emplace)
 *    is hard-disabled.
 *  - You must use zero-copy claim() / try_claim() / publish() instead.
 * ======================================================================== */
template <class T, reg N, reg FifoCapacity = 0,
         typename Policy = ::spsc::policy::default_policy,
         typename Alloc = ::spsc::alloc::policy_default_value_alloc_t<
             Policy, std::array<T, N>, ::spsc::alloc::default_alloc>>
class array_fifo
    : private ::spsc::fifo<
          ::spsc::detail::cache_aligned_slot_t<std::array<T, N>, Policy>,
          FifoCapacity,
          Policy,
          Alloc> {
    static_assert(N > 0, "spsc::array_fifo<T,N>: N must be > 0");

    using ArrayT = std::array<T, N>;
    using SlotT = ::spsc::detail::cache_aligned_slot_t<ArrayT, Policy>;
    using Base = ::spsc::fifo<SlotT, FifoCapacity, Policy, Alloc>;

public:
    using array_type = ArrayT;
    using slot_type = SlotT;
    using value_type = typename Base::value_type;
    using pointer = typename Base::pointer;
    using const_pointer = typename Base::const_pointer;
    using size_type = typename Base::size_type;
    using reference = typename Base::reference;
    using const_reference = typename Base::const_reference;
    using difference_type = typename Base::difference_type;
    using region = typename Base::region;
    using regions = typename Base::regions;
    using iterator = typename Base::iterator;
    using const_iterator = typename Base::const_iterator;
    using reverse_iterator = typename Base::reverse_iterator;
    using const_reverse_iterator = typename Base::const_reverse_iterator;
    using snapshot = typename Base::snapshot;
    using const_snapshot = typename Base::const_snapshot;
    using base_allocator_type = typename Base::base_allocator_type;
    using allocator_type = typename Base::allocator_type;
    using policy_type = Policy;

    using Base::Base; // inherit fifo constructors

    using Base::begin;
    using Base::can_read;
    using Base::can_write;
    using Base::capacity;
    using Base::cbegin;
    using Base::cend;
    using Base::claim;
    using Base::claim_read;
    using Base::claim_write;
    using Base::clear;
    using Base::consume;
    using Base::consume_all;
    using Base::crbegin;
    using Base::crend;
    using Base::data;
    using Base::destroy;
    using Base::empty;
    using Base::end;
    using Base::free;
    using Base::front;
    using Base::full;
    using Base::get_allocator;
    using Base::is_valid;
    using Base::make_snapshot;
    using Base::operator[];
    using Base::pop;
    using Base::publish;
    using Base::rbegin;
    using Base::read_size;
    using Base::rend;
    using Base::reserve;
    using Base::resize;
    using Base::scoped_read;
    using Base::size;
#if SPSC_HAS_SPAN
    using Base::span;
#endif
    using Base::try_claim;
    using Base::try_consume;
    using Base::try_front;
    using Base::try_pop;
    using Base::try_publish;
    using Base::write_size;

    void swap(array_fifo& other) noexcept(noexcept(std::declval<Base&>().swap(std::declval<Base&>()))) {
        Base::swap(static_cast<Base&>(other));
    }

    friend void swap(array_fifo& a, array_fifo& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

    static_assert(alignof(value_type) >= alignof(array_type),
                  "[spsc::array_fifo]: slot alignment must not weaken std::array alignment.");
    static_assert(alignof(value_type) >= ::spsc::alloc::policy_storage_alignment_v<Policy, array_type>,
                  "[spsc::array_fifo]: cached policies must lift slot alignment to the policy storage alignment.");
    static_assert((sizeof(value_type) % alignof(value_type)) == 0u,
                  "[spsc::array_fifo]: slot size must be a multiple of slot alignment.");
    static_assert(::spsc::detail::allocator_supports_slot_alignment_v<Alloc, value_type>,
                  "[spsc::array_fifo]: allocator rebind must preserve the promoted slot alignment.");

    // --------------------------------------------------------------------
    // Hard ban on value-based producers for all N.
    //
    // Name-hiding rule:
    //   Any declaration of push / try_push / emplace / try_emplace in this
    //   class hides all overloads with the same name from Base, regardless
    //   of signature. These templates therefore cut off Base's value-based
    //   producer API, forcing zero-copy claim()/try_claim()/publish().
    // --------------------------------------------------------------------
    template <class... Args> void push(Args &&...) = delete;

    template <class... Args> [[nodiscard]] bool try_push(Args &&...) = delete;

    template <class... Args> reference emplace(Args &&...) = delete;

    template <class... Args>
    [[nodiscard]] value_type *try_emplace(Args &&...) = delete;

    void scoped_write() = delete;
    void scoped_write(size_type) = delete;
};

/* ========================================================================
 * array_fifo_view: FIFO view over std::array<T, N> (non-owning)
 *
 *  - Uses ::spsc::fifo_view<std::array<T, N>, FifoCapacity, Policy>.
 *  - Storage is user-provided.
 *  - Same rule: only claim() / try_claim() / publish() on producer side.
 * ======================================================================== */
template <class T, reg N, reg FifoCapacity = 0,
         typename Policy = ::spsc::policy::default_policy>
class array_fifo_view
    : private ::spsc::fifo_view<
          ::spsc::detail::cache_aligned_slot_t<std::array<T, N>, Policy>,
          FifoCapacity,
          Policy> {
    static_assert(N > 0, "spsc::array_fifo_view<T,N>: N must be > 0");

    using ArrayT = std::array<T, N>;
    using SlotT = ::spsc::detail::cache_aligned_slot_t<ArrayT, Policy>;
    using Base = ::spsc::fifo_view<SlotT, FifoCapacity, Policy>;

public:
    using array_type = ArrayT;
    using slot_type = SlotT;
    using value_type = typename Base::value_type;
    using pointer = typename Base::pointer;
    using const_pointer = typename Base::const_pointer;
    using size_type = typename Base::size_type;
    using reference = typename Base::reference;
    using const_reference = typename Base::const_reference;
    using difference_type = typename Base::difference_type;
    using state_t = typename Base::state_t;
    using region = typename Base::region;
    using regions = typename Base::regions;
    using iterator = typename Base::iterator;
    using const_iterator = typename Base::const_iterator;
    using reverse_iterator = typename Base::reverse_iterator;
    using const_reverse_iterator = typename Base::const_reverse_iterator;
    using snapshot = typename Base::snapshot;
    using const_snapshot = typename Base::const_snapshot;
    using policy_type = Policy;

    using Base::Base; // inherit fifo_view constructors

    using Base::adopt;
    using Base::attach;
    using Base::begin;
    using Base::can_read;
    using Base::can_write;
    using Base::capacity;
    using Base::cbegin;
    using Base::cend;
    using Base::claim;
    using Base::claim_read;
    using Base::claim_write;
    using Base::clear;
    using Base::consume;
    using Base::consume_all;
    using Base::crbegin;
    using Base::crend;
    using Base::data;
    using Base::detach;
    using Base::empty;
    using Base::end;
    using Base::free;
    using Base::front;
    using Base::full;
    using Base::is_valid;
    using Base::make_snapshot;
    using Base::operator[];
    using Base::pop;
    using Base::publish;
    using Base::rbegin;
    using Base::read_size;
    using Base::rend;
    using Base::reset;
    using Base::scoped_read;
    using Base::size;
#if SPSC_HAS_SPAN
    using Base::span;
#endif
    using Base::state;
    using Base::try_claim;
    using Base::try_consume;
    using Base::try_front;
    using Base::try_pop;
    using Base::try_publish;
    using Base::write_size;

    void swap(array_fifo_view& other) noexcept(noexcept(std::declval<Base&>().swap(std::declval<Base&>()))) {
        Base::swap(static_cast<Base&>(other));
    }

    friend void swap(array_fifo_view& a, array_fifo_view& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

    static_assert(alignof(value_type) >= alignof(array_type),
                  "[spsc::array_fifo_view]: slot alignment must not weaken std::array alignment.");
    static_assert(alignof(value_type) >= ::spsc::alloc::policy_storage_alignment_v<Policy, array_type>,
                  "[spsc::array_fifo_view]: cached policies must lift slot alignment to the policy storage alignment.");
    static_assert((sizeof(value_type) % alignof(value_type)) == 0u,
                  "[spsc::array_fifo_view]: slot size must be a multiple of slot alignment.");

    // --------------------------------------------------------------------
    // Hard ban on value-based producers for all N.
    // --------------------------------------------------------------------
    template <class... Args> void push(Args &&...) = delete;

    template <class... Args> [[nodiscard]] bool try_push(Args &&...) = delete;

    template <class... Args> reference emplace(Args &&...) = delete;

    template <class... Args>
    [[nodiscard]] value_type *try_emplace(Args &&...) = delete;

    void scoped_write() = delete;
    void scoped_write(size_type) = delete;
};

/* ========================================================================
 * carray_fifo_view: FIFO view over C-style T[N] arrays (non-owning)
 *
 *  - Uses ::spsc::fifo_view<T[N], FifoCapacity, Policy>.
 *  - Storage is user-provided: T buf[FifoCapacity][N] or T buf[maxCap][N].
 *  - Same rule: only claim() / try_claim() / publish() on producer side.
 * ======================================================================== */
template <class T, reg N, reg FifoCapacity = 0,
          typename Policy = ::spsc::policy::default_policy>
class carray_fifo_view : private ::spsc::fifo_view<T[N], FifoCapacity, Policy> {
    static_assert(N > 0, "spsc::c_array_fifo_view<T,N>: N must be > 0");

    using array_type = T[N];
    using Base = ::spsc::fifo_view<array_type, FifoCapacity, Policy>;

public:
    using value_type = typename Base::value_type; // T[N]
    using pointer = typename Base::pointer;
    using const_pointer = typename Base::const_pointer;
    using size_type = typename Base::size_type;
    using reference = typename Base::reference;             // T (&)[N]
    using const_reference = typename Base::const_reference; // const T (&)[N]
    using difference_type = typename Base::difference_type;
    using state_t = typename Base::state_t;
    using region = typename Base::region;
    using regions = typename Base::regions;
    using iterator = typename Base::iterator;
    using const_iterator = typename Base::const_iterator;
    using reverse_iterator = typename Base::reverse_iterator;
    using const_reverse_iterator = typename Base::const_reverse_iterator;
    using snapshot = typename Base::snapshot;
    using const_snapshot = typename Base::const_snapshot;

    using Base::Base; // inherit fifo_view constructors

    using Base::adopt;
    using Base::attach;
    using Base::begin;
    using Base::can_read;
    using Base::can_write;
    using Base::capacity;
    using Base::cbegin;
    using Base::cend;
    using Base::claim;
    using Base::claim_read;
    using Base::claim_write;
    using Base::clear;
    using Base::consume;
    using Base::consume_all;
    using Base::crbegin;
    using Base::crend;
    using Base::data;
    using Base::detach;
    using Base::empty;
    using Base::end;
    using Base::free;
    using Base::front;
    using Base::full;
    using Base::is_valid;
    using Base::make_snapshot;
    using Base::operator[];
    using Base::pop;
    using Base::publish;
    using Base::rbegin;
    using Base::read_size;
    using Base::rend;
    using Base::reset;
    using Base::scoped_read;
    using Base::size;
#if SPSC_HAS_SPAN
    using Base::span;
#endif
    using Base::state;
    using Base::try_claim;
    using Base::try_consume;
    using Base::try_front;
    using Base::try_pop;
    using Base::try_publish;
    using Base::write_size;

    void swap(carray_fifo_view& other) noexcept(noexcept(std::declval<Base&>().swap(std::declval<Base&>()))) {
        Base::swap(static_cast<Base&>(other));
    }

    friend void swap(carray_fifo_view& a, carray_fifo_view& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

    // Element meta for convenience
    using element_type = T;
    using policy_type = Policy;
    static constexpr size_type array_size = static_cast<size_type>(N);

    // --------------------------------------------------------------------
    // Hard ban on value-based producers (same idea as array_fifo_view).
    // Producer must use claim()/try_claim() + publish().
    // --------------------------------------------------------------------
    template <class... Args> void push(Args &&...) = delete;

    template <class... Args> [[nodiscard]] bool try_push(Args &&...) = delete;

    template <class... Args> reference emplace(Args &&...) = delete;

    template <class... Args>
    [[nodiscard]] value_type *try_emplace(Args &&...) = delete;

    void scoped_write() = delete;
    void scoped_write(size_type) = delete;
};

} // namespace spsc

#endif /* SPSC_BASE_ARRAY_FIFO_HPP_ */
