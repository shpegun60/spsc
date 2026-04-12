/*
 * chunk_fifo.hpp
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef SPSC_CHUNK_FIFO_HPP_
#define SPSC_CHUNK_FIFO_HPP_

#include "base/spsc_slot_wrap.hpp"
#include "chunk.hpp"
#include "fifo.hpp"         // ::spsc::fifo, ::spsc::policy::default_policy, reg
#include "fifo_view.hpp"    //::spsc::fifo_view

namespace spsc {

/* ========================================================================
 * chunk_fifo: FIFO over chunks (owning storage)
 *
 * - Uses ::spsc::fifo<chunk<T, ChunkCapacity, Alloc>, FifoCapacity, Policy, Alloc>.
 * - Value-based producer API (push / try_push / emplace / try_emplace)
 * is hard-disabled.
 * - You must use zero-copy claim() / try_claim() / publish() instead.
 * ======================================================================== */
template<
    class T,
    reg   ChunkCapacity = 0,
    reg   FifoCapacity  = 0,
    typename Policy     = ::spsc::policy::default_policy,
    typename Alloc      = ::spsc::alloc::policy_default_value_alloc_t<
        Policy, ::spsc::chunk<T, ChunkCapacity>, ::spsc::alloc::default_alloc>
    >
class chunk_fifo
    : public ::spsc::fifo<
          ::spsc::detail::cache_aligned_slot_t<::spsc::chunk<T, ChunkCapacity, Alloc>, Policy>,
          FifoCapacity,
          Policy,
          Alloc
          >
{
    using ChunkT = ::spsc::chunk<T, ChunkCapacity, Alloc>;
    using SlotT  = ::spsc::detail::cache_aligned_slot_t<ChunkT, Policy>;
    using Base   = ::spsc::fifo<SlotT, FifoCapacity, Policy, Alloc>;

public:
    using chunk_type      = ChunkT;
    using slot_type       = SlotT;
    using value_type      = typename Base::value_type;
    using size_type       = typename Base::size_type;
    using reference       = typename Base::reference;
    using const_reference = typename Base::const_reference;
    using policy_type     = Policy;

    using Base::Base;   // inherit fifo constructors

    static_assert(alignof(value_type) >= alignof(chunk_type),
                  "[spsc::chunk_fifo]: slot alignment must not weaken chunk alignment.");
    static_assert(alignof(value_type) >= ::spsc::alloc::policy_storage_alignment_v<policy_type, chunk_type>,
                  "[spsc::chunk_fifo]: cached policies must lift slot alignment to the policy storage alignment.");
    static_assert((sizeof(value_type) % alignof(value_type)) == 0u,
                  "[spsc::chunk_fifo]: slot size must be a multiple of slot alignment.");
    static_assert(::spsc::detail::allocator_supports_slot_alignment_v<Alloc, value_type>,
                  "[spsc::chunk_fifo]: allocator rebind must preserve the promoted slot alignment.");

    // --------------------------------------------------------------------
    // Hard ban on value-based producers for all ChunkCapacity.
    //
    // Name-hiding rule:
    //   Any declaration of push / try_push / emplace / try_emplace in this
    //   class hides all overloads with the same name from Base, regardless
    //   of signature. So these templates effectively cut off Base's
    //   value-based producer API.
    // --------------------------------------------------------------------
    template<class... Args>
    void push(Args&&...) = delete;

    template<class... Args>
    [[nodiscard]] bool try_push(Args&&...) = delete;

    template<class... Args>
    reference emplace(Args&&...) = delete;

    template<class... Args>
    [[nodiscard]] value_type* try_emplace(Args&&...) = delete;
};


/* ========================================================================
 * chunk_fifo_view: FIFO view over chunks (non-owning)
 *
 * - Uses ::spsc::fifo_view<chunk<T, ChunkCapacity, Alloc>, FifoCapacity, Policy>.
 * - Storage is user-provided.
 * - Same rule: only claim() / try_claim() / publish() on producer side.
 * ======================================================================== */
template<
    class T,
    reg   ChunkCapacity,
    reg   FifoCapacity  = 0,
    typename Policy     = ::spsc::policy::default_policy,
    typename Alloc      = ::spsc::alloc::default_alloc
    >
class chunk_fifo_view
    : public ::spsc::fifo_view<
          ::spsc::detail::cache_aligned_slot_t<::spsc::chunk<T, ChunkCapacity, Alloc>, Policy>,
          FifoCapacity,
          Policy
          >
{
    using ChunkT = ::spsc::chunk<T, ChunkCapacity, Alloc>;
    using SlotT  = ::spsc::detail::cache_aligned_slot_t<ChunkT, Policy>;
    using Base   = ::spsc::fifo_view<SlotT, FifoCapacity, Policy>;

public:
    using chunk_type      = ChunkT;
    using slot_type       = SlotT;
    using value_type      = typename Base::value_type;
    using size_type       = typename Base::size_type;
    using reference       = typename Base::reference;
    using const_reference = typename Base::const_reference;
    using policy_type     = Policy;

    using Base::Base;   // inherit fifo_view constructors

    static_assert(alignof(value_type) >= alignof(chunk_type),
                  "[spsc::chunk_fifo_view]: slot alignment must not weaken chunk alignment.");
    static_assert(alignof(value_type) >= ::spsc::alloc::policy_storage_alignment_v<policy_type, chunk_type>,
                  "[spsc::chunk_fifo_view]: cached policies must lift slot alignment to the policy storage alignment.");
    static_assert((sizeof(value_type) % alignof(value_type)) == 0u,
                  "[spsc::chunk_fifo_view]: slot size must be a multiple of slot alignment.");

    // --------------------------------------------------------------------
    // Hard ban on value-based producers.
    // --------------------------------------------------------------------
    template<class... Args>
    void push(Args&&...) = delete;

    template<class... Args>
    [[nodiscard]] bool try_push(Args&&...) = delete;

    template<class... Args>
    reference emplace(Args&&...) = delete;

    template<class... Args>
    [[nodiscard]] value_type* try_emplace(Args&&...) = delete;
};

} // namespace spsc

#endif /* SPSC_CHUNK_FIFO_HPP_ */
