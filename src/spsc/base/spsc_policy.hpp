/*
 * policy.hpp
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 *
 *
 * Zero-runtime policy traits for SPSC ring internals.
 *
 * Purpose:
 *   - Provide a single place where SPSC internals learn:
 *       * which counter type to use for indices (head/tail)
 *       * which type to use for geometry (capacity, mask, etc.)
 *       * whether these should be plain, volatile, atomic, or cacheline-padded
 *
 * Design:
 *   - Purely compile-time, zero data, zero runtime.
 *   - Types encode their own alignment:
 *       * PlainCounter<T> / VolatileCounter<T> / AtomicCounter<T, O>
 *       * CachelineCounter<Counter, AlignB> for false-sharing avoidance
 *   - Policy no longer exposes numeric alignment constants; alignment is
 *     implicit in alignof(counter_type) / alignof(geometry_type).
 *
 * Layers:
 *   1) Base Policy<Cnt, Geo>
 *        - Just picks the two storage types.
 *        - No alignment numbers, no logic.
 *
 *   2) Ready-made aliases:
 *        - P   : plain counters and geometry; single context or external sync
 *        - V   : volatile counters, externally ordered/platform-specific
 *        - VV  : volatile counters and geometry, with the same restriction
 *        - A<O>: atomic counters with configurable orders, plain geometry
 *        - FA<O>: single-writer atomic counters, plain geometry
 *        - AA<O>: atomic counters and atomic geometry (heavy shared setups)
 *
 *   3) default_policy:
 *        - Modern v3 default when SPSC_DEFAULT_POLICY_ATOMIC is undefined:
 *            FA<> (single-writer atomic counters)
 *        - Legacy explicit SPSC_DEFAULT_POLICY_ATOMIC override:
 *            0 → P   (PlainCounter)
 *            1 → A<> (strict AtomicCounter with default_orders)
 *
 *   4) CacheAligned<Base, CAlign, GAlign>:
 *        - Derives a new policy from Base but wraps:
 *            counter_type   → CachelineCounter<Base::counter_type,   CAlign>
 *            geometry_type  → CachelineCounter<Base::geometry_type,  GAlign>
 *        - Defaults use detection constants from spsc_cacheline.hpp:
 *            ::spsc::hw::cacheline_bytes
 *
 * Usage examples:
 *
 *   using PPolicy   = spsc::policy::P;            // plain, single-context
 *   using APolicy   = spsc::policy::A<>;          // atomic counters
 *   using CAPolicy  = spsc::policy::CacheAligned<APolicy>; // atomic +
 * cacheline-padded
 *
 *   template<class Policy = spsc::policy::default_policy>
 *   class SPSCbase { ... };
 */

#ifndef SPSC_POLICY_HPP_
#define SPSC_POLICY_HPP_

#include <type_traits>
#include <utility>

#include "basic_types.h"      // reg
#include "spsc_cacheline.hpp" // spsc::hw::cacheline_bytes
#include "spsc_counter.hpp"   // Plain/Volatile/Atomic/Cacheline counters
#include "spsc_tools.hpp"

namespace spsc::policy {

/* Bring counters and default memory orders into scope */
using ::spsc::cnt::AtomicCounter;
using ::spsc::cnt::CachelineCounter;
using ::spsc::cnt::default_orders;
using ::spsc::cnt::FastAtomicCounter;
using ::spsc::cnt::PlainCounter;
using ::spsc::cnt::VolatileCounter;

/* 'reg' must be an unsigned integral (used as size/counter domain). */
static_assert(std::is_integral_v<reg> && std::is_unsigned_v<reg>,
              "[policy]: 'reg' must be an unsigned integral type");

namespace detail {

/* Optional owner-side relaxed load is valid only when it preserves the same
 * value domain and cannot throw.  Its absence remains supported: SPSCbase
 * falls back to load() for third-party counters.
 */
template<typename T, typename Value, typename = void>
struct relaxed_load_contract : std::true_type {};

template<typename T, typename Value>
struct relaxed_load_contract<
    T, Value,
    std::void_t<decltype(std::declval<const T&>().load_relaxed())>>
    : std::bool_constant<
          std::is_convertible_v<
              decltype(std::declval<const T&>().load_relaxed()), Value> &&
          noexcept(std::declval<const T&>().load_relaxed())> {};

/* Full contract for a custom counter backend.
 *
 * SPSCbase is noexcept and every owning container later names value_type, so
 * merely detecting store/load/add/inc is not enough.  Keep this trait equal
 * to the actual extension contract: an unsigned integral, reg-compatible
 * value domain; no-throw construction and counter operations; and a correctly
 * typed optional load_relaxed().
 */
template <typename T, typename = void>
struct is_counter_like : std::false_type {};

template <typename T>
struct is_counter_like<
    T, std::void_t<typename T::value_type,
                   decltype(std::declval<T&>().store(
                       std::declval<typename T::value_type>())),
                   decltype(std::declval<const T&>().load()),
                   decltype(std::declval<T&>().add(
                       std::declval<typename T::value_type>())),
                   decltype(std::declval<T&>().inc())>>
    : std::bool_constant<
          std::is_integral_v<std::remove_cv_t<typename T::value_type>> &&
          std::is_unsigned_v<std::remove_cv_t<typename T::value_type>> &&
          !std::is_same_v<std::remove_cv_t<typename T::value_type>, bool> &&
          std::is_convertible_v<typename T::value_type, reg> &&
          std::is_convertible_v<reg, typename T::value_type> &&
          std::is_nothrow_default_constructible_v<T> &&
          std::is_convertible_v<
              decltype(std::declval<const T&>().load()),
              typename T::value_type> &&
          noexcept(std::declval<T&>().store(
              std::declval<typename T::value_type>())) &&
          noexcept(std::declval<const T&>().load()) &&
          noexcept(std::declval<T&>().add(
              std::declval<typename T::value_type>())) &&
          noexcept(std::declval<T&>().inc()) &&
          relaxed_load_contract<T, typename T::value_type>::value> {};

template <typename T>
inline constexpr bool is_counter_like_v = is_counter_like<T>::value;
} // namespace detail

/* ------------------------------ Policy --------------------------------
 * Base policy:
 *   - Cnt: counter storage type (head/tail indices)
 *   - Geo: geometry storage type (capacity, mask, etc.)
 *
 * Alignment is defined by the types themselves:
 *   - alignof(counter_type)
 *   - alignof(geometry_type)
 *
 * No numeric alignment constants are exposed here anymore; if you need
 * cache-line alignment, use CacheAligned<> or cacheline-aware counter types.
 * --------------------------------------------------------------------- */

template <
    class Cnt = PlainCounter<reg>,
    class Geo = Cnt
>
struct Policy {
    static_assert(detail::is_counter_like_v<Cnt>,
                  "[Policy]: counter_type must satisfy the custom counter contract");
    static_assert(detail::is_counter_like_v<Geo>,
                  "[Policy]: geometry_type must satisfy the custom counter contract");

    using counter_type = Cnt;
    using geometry_type = Geo;

    // Payload/storage alignment hint used by default allocator traits.
    static constexpr reg allocator_alignment = 1u;
};

/* --------------------------- Ready-made aliases ---------------------------
 * P   : plain counters and geometry; single context or external sync
 * V   : volatile counters; requires external or platform-specific ordering
 * VV  : volatile counters and geometry; same synchronization restriction
 * A<O>: RTOS/tasks (strict atomic counters with RMW add/inc; plain geometry)
 * FA<O>: single-writer atomic counters (load+store add/inc; plain geometry)
 * AA<O>: strict atomic counters and geometry atomic (shared memory / SMP)
 * ------------------------------------------------------------------------- */
using P = Policy<>;

using V = Policy<VolatileCounter<reg>, PlainCounter<reg>>;
using VV = Policy<VolatileCounter<reg>, VolatileCounter<reg>>;

template <class O = default_orders>
using A = Policy<AtomicCounter<reg, O>, PlainCounter<reg>>;

template <class O = default_orders>
using FA = Policy<FastAtomicCounter<reg, O>, PlainCounter<reg>>;

template <class O = default_orders>
using AA = Policy<AtomicCounter<reg, O>, AtomicCounter<reg, O>>;

// Backward-compatible aliases with explicit RMW naming.
template <class O = default_orders>
using ARMW = A<O>;

template <class O = default_orders>
using AARMW = AA<O>;

/*
 * v3 default-policy contract:
 *
 *   no SPSC_DEFAULT_POLICY_ATOMIC definition -> FA<>
 *   SPSC_DEFAULT_POLICY_ATOMIC=0            -> P   (legacy explicit override)
 *   SPSC_DEFAULT_POLICY_ATOMIC=1            -> A<> (legacy explicit override)
 *
 * The macro is deliberately not synthesized when absent. Its definedness is
 * part of the compatibility contract, so callers can distinguish the modern
 * default from an explicit legacy plain override.
 */
#if defined(SPSC_DEFAULT_POLICY_ATOMIC)
static_assert(SPSC_DEFAULT_POLICY_ATOMIC == 0 ||
                  SPSC_DEFAULT_POLICY_ATOMIC == 1,
              "SPSC_DEFAULT_POLICY_ATOMIC must be 0 or 1");

using default_policy = std::conditional_t<(SPSC_DEFAULT_POLICY_ATOMIC != 0), A<>, P>;
#else
using default_policy = FA<>;
#endif

/* ------------------------ Cache-line wrapper ------------------------
 * CacheAligned<Base, CAlign, GAlign>
 *
 * Produces a new policy from Base where:
 *   counter_type  = CachelineCounter<Base::counter_type,  CAlign>
 *   geometry_type = CachelineCounter<Base::geometry_type, GAlign>
 *
 * Defaults:
 *   - Base   -> default_policy (and therefore follows the v3/legacy mapping)
 *   - CAlign → ::spsc::hw::cacheline_bytes
 *   - GAlign → CAlign (::spsc::hw::cacheline_bytes)
 *
 * All alignment invariants are enforced inside CachelineCounter:
 *   - AlignB != 0
 *   - AlignB is power-of-two
 *   - AlignB >= alignof(T)
 *   - sizeof(CachelineCounter<...>) is a multiple of AlignB
 * ------------------------------------------------------------------- */

template <
    class Base = default_policy,
    reg CAlign = ::spsc::hw::cacheline_bytes,
    reg GAlign = CAlign
>
struct CacheAligned {
private:
    using base_counter_type = typename Base::counter_type;
    using base_geometry_type = typename Base::geometry_type;

    static_assert(detail::is_counter_like_v<base_counter_type>,
                  "[CacheAligned]: Base::counter_type must satisfy the custom counter contract");
    static_assert(detail::is_counter_like_v<base_geometry_type>,
                  "[CacheAligned]: Base::geometry_type must satisfy the custom counter contract");

    static_assert(CAlign != 0, "[CacheAligned]: CAlign must be non-zero");
    static_assert((CAlign & (CAlign - 1u)) == 0u,
                  "[CacheAligned]: CAlign must be power-of-two");

    static_assert(GAlign != 0, "[CacheAligned]: GAlign must be non-zero");
    static_assert((GAlign & (GAlign - 1u)) == 0u,
                  "[CacheAligned]: GAlign must be power-of-two");

public:
    using counter_type = CachelineCounter<base_counter_type, CAlign>;
    using geometry_type = CachelineCounter<base_geometry_type, GAlign>;

    // Propagate the strongest cache-line request to payload allocators/helpers.
    static constexpr reg allocator_alignment = (CAlign > GAlign) ? CAlign : GAlign;

    static_assert(
        detail::is_counter_like_v<counter_type>,
        "[CacheAligned]: resulting counter_type must satisfy the custom counter contract");
    static_assert(
        detail::is_counter_like_v<geometry_type>,
        "[CacheAligned]: resulting geometry_type must satisfy the custom counter contract");
};

/* --------------------------- Cache-line aliases --------------------------- */
using CP = CacheAligned<P>;

using CV = CacheAligned<V>;
using CVV = CacheAligned<VV>;

template <class O = default_orders> using CA = CacheAligned<A<O>>;

template <class O = default_orders> using CFA = CacheAligned<FA<O>>;

template <class O = default_orders> using CAA = CacheAligned<AA<O>>;

} // namespace spsc::policy

#endif /* SPSC_POLICY_HPP_ */
