/*
 * array_fifo.hpp
 *
 *  Created on: Nov 16, 2025
 *      Author: admin
 */

#ifndef SPSC_BASE_ARRAY_FIFO_HPP_
#define SPSC_BASE_ARRAY_FIFO_HPP_

#include "base/spsc_alloc.hpp" // ::spsc::alloc::default_alloc
#include "base/spsc_alloc.hpp" // ::spsc::alloc::default_alloc
#include "fifo.hpp"            // ::spsc::fifo
#include "fifo_view.hpp"       // ::spsc::fifo_view
#include <array>

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
          typename Alloc = ::spsc::alloc::default_alloc>
class array_fifo
    : public ::spsc::fifo<std::array<T, N>, FifoCapacity, Policy, Alloc> {
  static_assert(N > 0, "spsc::array_fifo<T,N>: N must be > 0");

  using array_type = std::array<T, N>;
  using Base = ::spsc::fifo<array_type, FifoCapacity, Policy, Alloc>;

public:
  using value_type = typename Base::value_type;
  using size_type = typename Base::size_type;
  using reference = typename Base::reference;
  using const_reference = typename Base::const_reference;

  using Base::Base; // inherit fifo constructors

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
    : public ::spsc::fifo_view<std::array<T, N>, FifoCapacity, Policy> {
  static_assert(N > 0, "spsc::array_fifo_view<T,N>: N must be > 0");

  using array_type = std::array<T, N>;
  using Base = ::spsc::fifo_view<array_type, FifoCapacity, Policy>;

public:
  using value_type = typename Base::value_type;
  using size_type = typename Base::size_type;
  using reference = typename Base::reference;
  using const_reference = typename Base::const_reference;

  using Base::Base; // inherit fifo_view constructors

  // --------------------------------------------------------------------
  // Hard ban on value-based producers for all N.
  // --------------------------------------------------------------------
  template <class... Args> void push(Args &&...) = delete;

  template <class... Args> [[nodiscard]] bool try_push(Args &&...) = delete;

  template <class... Args> reference emplace(Args &&...) = delete;

  template <class... Args>
  [[nodiscard]] value_type *try_emplace(Args &&...) = delete;
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
class carray_fifo_view : public ::spsc::fifo_view<T[N], FifoCapacity, Policy> {
  static_assert(N > 0, "spsc::c_array_fifo_view<T,N>: N must be > 0");

  using array_type = T[N];
  using Base = ::spsc::fifo_view<array_type, FifoCapacity, Policy>;

public:
  using value_type = typename Base::value_type; // T[N]
  using size_type = typename Base::size_type;
  using reference = typename Base::reference;             // T (&)[N]
  using const_reference = typename Base::const_reference; // const T (&)[N]

  using Base::Base; // inherit fifo_view constructors

  // Element meta for convenience
  using element_type = T;
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
};

} // namespace spsc

#endif /* SPSC_BASE_ARRAY_FIFO_HPP_ */
