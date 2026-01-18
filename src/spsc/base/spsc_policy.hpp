/*
 * policy.hpp
 *
 * Created on: 30 Nov. 2025
 *      Author: Shpegun60
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
 *        - P   : fastest single-core (plain counters and geometry)
 *        - V   : volatile counters (ISR <-> task), plain geometry
 *        - VV  : everything volatile (strict volatile propagation)
 *        - A<O>: atomic counters with configurable orders, plain geometry
 *        - AA<O>: atomic counters and atomic geometry (heavy shared setups)
 *
 *   3) default_policy:
 *        - Controlled via SPSC_DEFAULT_POLICY_ATOMIC:
 *            0 → P   (PlainCounter)
 *            1 → A<> (AtomicCounter with default_orders)
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
 *   using PPolicy   = spsc::policy::P;            // plain, fast, single-core
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

/* Helper trait: detect "counter-like" counter backend.
 * Requirements:
 *   - T has:  void store(reg)
 *   - T has:  reg-compatible load()   (convertible to reg)
 *   - T has:  void add(reg)
 *   - T has:  void inc()
 */
template <typename T, typename = void>
struct is_counter_like : std::false_type {};

template <typename T>
struct is_counter_like<
    T, std::void_t<decltype(std::declval<T &>().store(std::declval<reg>())),
                decltype(std::declval<const T &>().load()),
                decltype(std::declval<T &>().add(std::declval<reg>())),
                decltype(std::declval<T &>().inc())>>
    : std::bool_constant<std::is_convertible_v<
          decltype(std::declval<const T &>().load()), reg>> {};

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
                  "[Policy]: counter_type must implement store/load/add/inc with "
                  "reg-compatible value");
    static_assert(detail::is_counter_like_v<Geo>,
                  "[Policy]: geometry_type must implement store/load/add/inc "
                  "with reg-compatible value");

    using counter_type = Cnt;
    using geometry_type = Geo;
};

/* --------------------------- Ready-made aliases ---------------------------
 * P   : fastest single-core (plain counters and geometry)
 * V   : ISR <-> task, single-core (volatile counters; plain geometry)
 * VV  : everything volatile (rare; mainly for strict volatile propagation)
 * A<O>: RTOS/tasks (atomic counters with acq/rel; plain geometry)
 * AA<O>: both counters and geometry atomic (shared memory / SMP)
 * ------------------------------------------------------------------------- */
using P = Policy<>;

using V = Policy<VolatileCounter<reg>, PlainCounter<reg>>;
using VV = Policy<VolatileCounter<reg>, VolatileCounter<reg>>;

template <class O = default_orders>
using A = Policy<FastAtomicCounter<reg, O>, PlainCounter<reg>>;

template <class O = default_orders>
using FA = Policy<FastAtomicCounter<reg, O>, PlainCounter<reg>>;

template <class O = default_orders>
using AA = Policy<FastAtomicCounter<reg, O>, FastAtomicCounter<reg, O>>;

// RMW variants (slower): keep only if you really need atomic RMW semantics.
template <class O = default_orders>
using ARMW = Policy<AtomicCounter<reg, O>, PlainCounter<reg>>;

template <class O = default_orders>
using AARMW = Policy<AtomicCounter<reg, O>, AtomicCounter<reg, O>>;

/* Default policy: compile-time switchable without editing callers. */
#ifndef SPSC_DEFAULT_POLICY_ATOMIC
#define SPSC_DEFAULT_POLICY_ATOMIC 1
#endif /* SPSC_DEFAULT_POLICY_ATOMIC */

static_assert(SPSC_DEFAULT_POLICY_ATOMIC == 0 ||
                  SPSC_DEFAULT_POLICY_ATOMIC == 1,
              "SPSC_DEFAULT_POLICY_ATOMIC must be 0 or 1");

using default_policy = std::conditional_t<SPSC_DEFAULT_POLICY_ATOMIC, A<>, P>;

/* ------------------------ Cache-line wrapper ------------------------
 * CacheAligned<Base, CAlign, GAlign>
 *
 * Produces a new policy from Base where:
 *   counter_type  = CachelineCounter<Base::counter_type,  CAlign>
 *   geometry_type = CachelineCounter<Base::geometry_type, GAlign>
 *
 * Defaults:
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
                  "[CacheAligned]: Base::counter_type must be counter-like "
                  "(store/load/add/inc)");
    static_assert(detail::is_counter_like_v<base_geometry_type>,
                  "[CacheAligned]: Base::geometry_type must be counter-like "
                  "(store/load/add/inc)");

    static_assert(CAlign != 0, "[CacheAligned]: CAlign must be non-zero");
    static_assert((CAlign & (CAlign - 1u)) == 0u,
                  "[CacheAligned]: CAlign must be power-of-two");

    static_assert(GAlign != 0, "[CacheAligned]: GAlign must be non-zero");
    static_assert((GAlign & (GAlign - 1u)) == 0u,
                  "[CacheAligned]: GAlign must be power-of-two");

public:
    using counter_type = CachelineCounter<base_counter_type, CAlign>;
    using geometry_type = CachelineCounter<base_geometry_type, GAlign>;

    static_assert(
        detail::is_counter_like_v<counter_type>,
        "[CacheAligned]: resulting counter_type must remain counter-like");
    static_assert(
        detail::is_counter_like_v<geometry_type>,
        "[CacheAligned]: resulting geometry_type must remain counter-like");
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
