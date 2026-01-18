/*
 * chunk_fifo.hpp
 *
 * Created on: Nov 16, 2025
 * Author: Shpegun60
 */

#ifndef SPSC_CHUNK_FIFO_HPP_
#define SPSC_CHUNK_FIFO_HPP_

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
    typename Alloc      = ::spsc::alloc::default_alloc
>
class chunk_fifo
    : public ::spsc::fifo<
          ::spsc::chunk<T, ChunkCapacity, Alloc>,
          FifoCapacity,
          Policy,
          Alloc
      >
{
    using ChunkT = ::spsc::chunk<T, ChunkCapacity, Alloc>;
    using Base   = ::spsc::fifo<ChunkT, FifoCapacity, Policy, Alloc>;

public:
    using value_type      = ChunkT;
    using size_type       = typename Base::size_type;
    using reference       = typename Base::reference;
    using const_reference = typename Base::const_reference;

    using Base::Base;   // inherit fifo constructors

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
          ::spsc::chunk<T, ChunkCapacity, Alloc>,
          FifoCapacity,
          Policy
      >
{
    using ChunkT = ::spsc::chunk<T, ChunkCapacity, Alloc>;
    using Base   = ::spsc::fifo_view<ChunkT, FifoCapacity, Policy>;

public:
    using value_type      = ChunkT;
    using size_type       = typename Base::size_type;
    using reference       = typename Base::reference;
    using const_reference = typename Base::const_reference;

    using Base::Base;   // inherit fifo_view constructors

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
