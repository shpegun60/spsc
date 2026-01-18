/*
 * capacity_ctrl.hpp
 *
 * Created on: 30 Nov. 2025
 *      Author: Shpegun60
 *
 * Capacity control for SPSC buffers:
 *   - Static model   : CapacityCtrl<C != 0>  → compile-time capacity/mask
 *   - Dynamic model  : CapacityCtrl<0, P>    → runtime capacity/mask with
 *                       policy-driven geometry backend.
 *
 * Design:
 *   - For C != 0:
 *       * Header-only, no storage, no runtime.
 *       * Enforces power-of-two capacity within the "unambiguous" range
 *         (<= 2^(bits(reg)-1)).
 *
 *   - For C == 0:
 *       * Stores capacity and mask in Policy::geometry_type.
 *       * geometry_type is assumed to be "counter-like":
 *           - has:  void store(reg)
 *           - has:  load() returning something convertible to reg
 *           - typically one of:
 *               PlainCounter<reg>
 *               VolatileCounter<reg>
 *               AtomicCounter<reg, Orders>
 *               CachelineCounter<Counter, AlignB> wrapping any of the above.
 *       * No fallback for plain integral types: geometry must be a real
 *         counter backend, not a naked reg.
 */

#ifndef SPSC_CAPACITY_CTRL_HPP_
#define SPSC_CAPACITY_CTRL_HPP_

#include <limits>
#include <type_traits>
#include <utility>              // std::declval

#include "spsc_policy.hpp"      // spsc::policy::default_policy
#include "spsc_tools.hpp"       // RB_FORCEINLINE

/* -------------------- Feature tests --------------------
 * Detect availability of <bit> and int_pow2 facility (C++20).
 * Falls back to custom helpers when not available.
 */
#if defined(__has_include)
#  if __has_include(<bit>)
#    include <bit>
#    define SPSC_HAS_BIT_HEADER 1
#  else
#    define SPSC_HAS_BIT_HEADER 0
#  endif
#else
#  define SPSC_HAS_BIT_HEADER 0
#endif /* SPSC_HAS_BIT_HEADER */

#if SPSC_HAS_BIT_HEADER && defined(__cpp_lib_int_pow2) && (__cpp_lib_int_pow2 >= 201902L)
#  define SPSC_HAS_INT_POW2 1
#else
#  define SPSC_HAS_INT_POW2 0
#endif /* SPSC_HAS_INT_POW2 */

namespace spsc::cap {

using ::spsc::policy::default_policy;

/* ---------- Shared constants ----------
 * RB_REG_BITS:         bit-width of 'reg'
 * RB_MAX_UNAMBIGUOUS:  maximum capacity that keeps index differences unambiguous
 *                      (i.e., <= 2^(bits(reg)-1)).
 */
constexpr unsigned RB_REG_BITS        = std::numeric_limits<reg>::digits;
constexpr reg      RB_MAX_UNAMBIGUOUS = reg(1) << (RB_REG_BITS - 1);

/* ---------- Bit helpers ---------- */

/* True if x is a non-zero power of two. */
static RB_FORCEINLINE constexpr bool rb_is_pow2(const reg x) noexcept {
    return x && ((x & (x - 1u)) == 0u);
}

/* Propagate highest set bit to all lower bits (unrolled, width-aware). */
static RB_FORCEINLINE constexpr reg rb_fold_ones(reg v) noexcept {
    if constexpr (RB_REG_BITS > 1)  v |= (v >> 1);
    if constexpr (RB_REG_BITS > 2)  v |= (v >> 2);
    if constexpr (RB_REG_BITS > 4)  v |= (v >> 4);
    if constexpr (RB_REG_BITS > 8)  v |= (v >> 8);
    if constexpr (RB_REG_BITS > 16) v |= (v >> 16);
    if constexpr (RB_REG_BITS > 32) v |= (v >> 32);   // 64+ bit regs
    if constexpr (RB_REG_BITS > 64) v |= (v >> 64);   // 128-bit exotics
    return v;
}

/* Next power-of-two (used under RB_MAX_UNAMBIGUOUS clamp in callers).
 * n == 0                     -> 1
 * Otherwise ceil to the next power-of-two (keeps n when already pow2).
 */
#if SPSC_HAS_INT_POW2
static RB_FORCEINLINE constexpr reg rb_next_power2(reg n) noexcept {
    if (n == 0) return 1;
    return std::bit_ceil(n);
}

/* Largest power-of-two <= n (0 when n == 0). */
static RB_FORCEINLINE constexpr reg rb_floor_power2(reg n) noexcept {
    return n ? std::bit_floor(n) : 0;
}
#else
static RB_FORCEINLINE constexpr reg rb_next_power2(reg n) noexcept {
    if (n == 0) return 1;
    return rb_fold_ones(n - 1) + 1;
}

/* Largest power-of-two <= n (0 when n == 0). */
static RB_FORCEINLINE constexpr reg rb_floor_power2(reg n) noexcept {
    if (n == 0) return 0;
    const reg v = rb_fold_ones(n);
    return v - (v >> 1);
}
#endif /* SPSC_HAS_INT_POW2 */

/* Sanity checks (compile-time only) to guard regressions in helpers. */
static_assert(rb_is_pow2(reg{1}) && rb_is_pow2(reg{8}) && !rb_is_pow2(reg{6}), "rb_is_pow2 sanity");
static_assert(rb_floor_power2(reg{0}) == 0, "floor_pow2(0)");
static_assert(rb_floor_power2(reg{1}) == 1, "floor_pow2(1)");
static_assert(rb_floor_power2(reg{3}) == 2, "floor_pow2(3)");
static_assert(rb_next_power2(reg{0}) == 1,  "next_pow2(0)");
static_assert(rb_next_power2(reg{3}) == 4,  "next_pow2(3)");
static_assert(rb_is_pow2(RB_MAX_UNAMBIGUOUS), "RB_MAX_UNAMBIGUOUS must be power of two");
static_assert(rb_floor_power2(RB_MAX_UNAMBIGUOUS) == RB_MAX_UNAMBIGUOUS, "floor_pow2 at limit");
static_assert(rb_next_power2 (RB_MAX_UNAMBIGUOUS) == RB_MAX_UNAMBIGUOUS, "next_pow2 at limit");

/* ====================== Static capacity model: C != 0 ======================
 * Compile-time capacity/mask. No runtime initialization, no storage.
 */
template<reg C, typename Policy = default_policy>
class CapacityCtrl
{
    static_assert(C > 0,                   "[CapacityCtrl]: capacity must be > 0");
    static_assert(std::is_unsigned_v<reg>, "[CapacityCtrl]: 'reg' must be unsigned");
    static_assert(rb_is_pow2(C),           "[CapacityCtrl]: capacity must be power of two");
    static_assert(C <= RB_MAX_UNAMBIGUOUS, "[CapacityCtrl]: capacity must be <= 2^(bits(reg)-1)");

    /* Header-only and ODR-safe constants. */
    static inline constexpr reg _cap  = C;
    static inline constexpr reg _mask = C - 1u;

public:
    [[nodiscard]] RB_FORCEINLINE static constexpr reg capacity() noexcept { return _cap; }
    [[nodiscard]] RB_FORCEINLINE static constexpr reg mask()     noexcept { return _mask; }
};

/* ====================== Dynamic capacity model: C == 0 ======================
 * Runtime capacity/mask stored with policy-driven backend.
 *
 * geometry_type requirements:
 *   - Must satisfy detail::is_counter_like<geometry_type>.
 *   - Typically one of:
 *       PlainCounter<reg>
 *       VolatileCounter<reg>
 *       AtomicCounter<reg, Orders>
 *       CachelineCounter<Counter, AlignB> wrapping any of the above.
 */
template<typename Policy>
class CapacityCtrl<0, Policy>
{
    static_assert(std::is_unsigned_v<reg>, "[CapacityCtrl<0>]: 'reg' must be unsigned");

    using geometry_type = typename Policy::geometry_type;

    static_assert(::spsc::policy::detail::is_counter_like_v<geometry_type>,
        "[CapacityCtrl<0>]: Policy::geometry_type must provide store/load/add/inc");

    geometry_type _cap{};   /* runtime capacity (pow2 or 0) */
    geometry_type _mask{};  /* cap - 1 */

public:
    /* Initialize geometry:
     * c == 0       -> cap=0, mask=0 (disabled)
     * c  > 0       -> clamp to RB_MAX_UNAMBIGUOUS and floor to power-of-two
     * Always returns true (overflow is handled by clamping).
     */
    [[nodiscard]] RB_FORCEINLINE bool init(const reg c) noexcept {
        if (c == 0u) {
            _cap.store(0u);
            _mask.store(0u);
            return true;
        }

        const reg req = (c > RB_MAX_UNAMBIGUOUS) ? RB_MAX_UNAMBIGUOUS : c;
        const reg cap = rb_is_pow2(req) ? req : rb_floor_power2(req);

        _cap.store(cap);
        _mask.store(cap - 1u);
        return true;
    }

    /* Accessors delegate to geometry_type API. */
    [[nodiscard]] RB_FORCEINLINE reg capacity() const noexcept {
        return static_cast<reg>(_cap.load());
    }

    [[nodiscard]] RB_FORCEINLINE reg mask() const noexcept {
        return static_cast<reg>(_mask.load());
    }
};

} // namespace spsc::cap

#endif /* SPSC_CAPACITY_CTRL_HPP_ */
