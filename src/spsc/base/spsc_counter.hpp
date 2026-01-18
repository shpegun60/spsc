/*
 * spsc_counter.hpp
 *
 * Created on: 30 Nov. 2025
 *   Author: Shpegun60
 *
 *
 * Family of lightweight counter wrappers used by SPSC ring buffers and related
 * primitives. The goal is to provide:
 *
 *   - A uniform API:
 *       * store(value)
 *       * load()
 *       * add(delta)
 *       * inc()
 *
 *   - Multiple backends with different memory and concurrency semantics:
 *       * PlainCounter<T>
 *           - Raw integral variable (no fences, no atomics, constexpr-friendly).
 *           - Suitable for single-core or externally synchronized code paths.
 *
 *       * VolatileCounter<T>
 *           - Volatile integral variable.
 *           - Prevents compiler reordering around loads/stores of this object.
 *           - Does NOT provide cross-core ordering guarantees.
 *
 *       * AtomicCounter<T, Orders>
 *           - std::atomic<U> wrapper (U is unsigned version of T).
 *           - Memory ordering is configurable via Orders:
 *               Orders::load  → std::memory_order for load()
 *               Orders::store → std::memory_order for store()
 *               Orders::rmw   → std::memory_order for add()/inc()
 *           - By default uses acquire/release/acq_rel palette.
 *
 *       * CachelineCounter<Counter, AlignB>
 *           - Wraps any Counter type (Plain/Volatile/Atomic) into a slot that:
 *               • is aligned to AlignB bytes
 *               • has sizeof(...) as a multiple of AlignB
 *           - This is intended to avoid false sharing between producer/consumer
 *             counters and other fields, especially when placed in arrays or
 *             adjacent structures.
 *           - AlignB defaults to ::spsc::hw::cacheline_bytes.
 *
 * Integration hints:
 *   - Use PlainCounter/VolatileCounter for single-core microcontroller-only
 *     builds when you control all access patterns.
 *   - Use AtomicCounter for cross-core or host code, or when you want strict
 *     acquire/release semantics for SPSC queues.
 *   - Use CachelineXxxCounter aliases (CachelineAtomicCounter, etc.) when
 *     producer and consumer counters live in different cores or threads and
 *     you want to avoid cache-line contention.
 *
 * Configuration:
 *   - SPSC_REQUIRE_LOCK_FREE (default 0):
 *       * 0 → Allow std::atomic<U> even if it is not always lock-free. Toolchain
 *             may use libatomic or fallback code paths.
 *       * 1 → static_assert if std::atomic<U>::is_always_lock_free is false.
 */

#ifndef SPSC_COUNTER_HPP_
#define SPSC_COUNTER_HPP_

#include <type_traits>
#include <atomic>

#include "spsc_tools.hpp"      // RB_FORCEINLINE / RB_NOINLINE / etc.
#include "spsc_cacheline.hpp"  // ::spsc::hw::cacheline_bytes
#include "basic_types.h"        // reg

/* --------------------------------------------------------------------
 * Optional: require lock-free atomics or allow fallback toolchains.
 *   - Set SPSC_REQUIRE_LOCK_FREE=1 to hard-fail when std::atomic<U> is not always lock-free.
 *   - Default 0 keeps portability (some MCUs need libatomic or are not LF).
 * -------------------------------------------------------------------- */
#ifndef SPSC_REQUIRE_LOCK_FREE
#  define SPSC_REQUIRE_LOCK_FREE 0
#endif /* SPSC_REQUIRE_LOCK_FREE */

namespace spsc::cnt {

/* --------------------------------------------------------------------
 * remove_cvref_t for pre-C++20 portability
 * -------------------------------------------------------------------- */
template<class T>
using rb_remove_cvref_t = std::remove_cv_t<std::remove_reference_t<T>>;

/* Helper: integral but not bool */
template<class T>
inline constexpr bool is_integral_not_bool_v =
    std::is_integral_v<rb_remove_cvref_t<T>> &&
    !std::is_same_v<rb_remove_cvref_t<T>, bool>;

/* ------------------------------ PlainCounter ------------------------------
 * Raw loads/stores, no fences, constexpr-friendly.
 * Suitable for single-core or when external synchronization is guaranteed.
 * -------------------------------------------------------------------------- */
template<typename T>
class PlainCounter {
    static_assert(is_integral_not_bool_v<T>, "PlainCounter<T>: T must be an integral type (not bool)");
    using U = std::make_unsigned_t<rb_remove_cvref_t<T>>;

public:
    static constexpr bool is_atomic = false;
    using value_type = U;

private:
    U v{0};

public:
    RB_FORCEINLINE constexpr void store(const U x) noexcept { v = x; }
    [[nodiscard]] RB_FORCEINLINE constexpr U load() const noexcept { return v; }
    RB_FORCEINLINE constexpr void add(const U n) noexcept { v += n; }
    RB_FORCEINLINE constexpr void inc() noexcept { ++v; }
};

/* ----------------------------- VolatileCounter -----------------------------
 * Single-core publish/observe; prevents compiler reordering around this object.
 * NOT a memory barrier; not safe for multi-core without external sync.
 * --------------------------------------------------------------------------- */
template<typename T>
class VolatileCounter {
    static_assert(is_integral_not_bool_v<T>, "VolatileCounter<T>: T must be an integral type (not bool)");
    using U = std::make_unsigned_t<rb_remove_cvref_t<T>>;

public:
    static constexpr bool is_atomic = false;
    using value_type = U;

private:
    volatile U v{0};

public:
    RB_FORCEINLINE void store(const U x) noexcept { v = x; }
    [[nodiscard]] RB_FORCEINLINE U load() const noexcept { return v; }
    RB_FORCEINLINE void add(const U n) noexcept {
        // Single load + single store (no double-reads of volatile).
        const T cur = v;
        v = static_cast<T>(cur + n);
    }
    RB_FORCEINLINE void inc() noexcept {
        // Avoid deprecated ++ on volatile: explicit load-modify-store.
        const T cur = v;
        v = static_cast<T>(cur + 1u);
    }
};

/* ------------------------------- Orders palette --------------------------- */
struct default_orders {
    static constexpr std::memory_order load  = std::memory_order_acquire;
    static constexpr std::memory_order store = std::memory_order_release;
    static constexpr std::memory_order rmw   = std::memory_order_acq_rel;
};

struct relaxed_orders {
    static constexpr std::memory_order load  = std::memory_order_relaxed;
    static constexpr std::memory_order store = std::memory_order_relaxed;
    static constexpr std::memory_order rmw   = std::memory_order_relaxed;
};

/* Internal constexpr checks to catch nonsense orders at compile-time. */
namespace detail {

    constexpr bool valid_load_order(std::memory_order mo) {
        switch (mo) {
            case std::memory_order_relaxed:
            case std::memory_order_consume:
            case std::memory_order_acquire:
            case std::memory_order_seq_cst:
                return true;
            default:
                return false; // release / acq_rel are invalid for load
        }
    }

    constexpr bool valid_store_order(std::memory_order mo) {
        switch (mo) {
            case std::memory_order_relaxed:
            case std::memory_order_release:
            case std::memory_order_seq_cst:
                return true;
            default:
                return false; // acquire / consume / acq_rel invalid for store
        }
    }

    constexpr bool valid_rmw_order(std::memory_order mo) {
        switch (mo) {
            case std::memory_order_relaxed:
            case std::memory_order_acquire:
            case std::memory_order_release:
            case std::memory_order_acq_rel:
            case std::memory_order_seq_cst:
                return true;
            default:
                return false; // consume invalid for RMW
        }
    }

    /* --------------------------------------------------------------------
     * cacheline_pad<PadBytes>:
     *   - Helper to avoid zero-sized arrays in standard C++.
     *   - Specialization for PadBytes == 0 becomes an empty type.
     * -------------------------------------------------------------------- */
    template<reg PadBytes>
    struct cacheline_pad {
        unsigned char padding[PadBytes];
    };

    template<>
    struct cacheline_pad<0> {
        // No padding needed
    };

    /* --------------------------------------------------------------------
     * CacheSlot<T, L>:
     *   - Aligns T to L.
     *   - Adds padding so that sizeof(CacheSlot) is a multiple of L.
     *   - L must be non-zero power-of-two and >= alignof(T).
     *
     * Typical use:
     *   CacheSlot<MyType, 64> slot;
     *   static_assert(sizeof(slot) % 64 == 0, "");
     * -------------------------------------------------------------------- */
    template<class T, reg L>
    struct CacheSlot {
        static_assert(L != 0, "CacheSlot: alignment L must be non-zero");
        static_assert((L & (L - 1u)) == 0u, "CacheSlot: alignment L must be power-of-two");
        static_assert(L >= alignof(T), "CacheSlot: L must be >= alignof(T)");

        static constexpr reg kAlign   = L;
        static constexpr reg kRawSize = sizeof(T);
        static constexpr reg kRem     = kRawSize % kAlign;
        static constexpr reg kPad     = (kRem == 0u) ? 0u : (kAlign - kRem);

        alignas(L) T value{};
        cacheline_pad<kPad> pad;
    };

} // namespace detail

/* ------------------------------ AtomicCounter ------------------------------
 * Acquire for loads, release for stores, acq_rel for RMW by default.
 * Turn on SPSC_REQUIRE_LOCK_FREE=1 if you want to hard-enforce lock-free.
 * --------------------------------------------------------------------------- */
template<typename T, typename Orders = default_orders>
class AtomicCounter {
    static_assert(is_integral_not_bool_v<T>, "AtomicCounter<T>: T must be integral (not bool)");
    using U = std::make_unsigned_t<rb_remove_cvref_t<T>>;

public:
    static constexpr bool is_atomic = true;
    using value_type = U;

private:
    /* Compile-time sanity on memory orders */
    static_assert(detail::valid_load_order(Orders::load),   "AtomicCounter: invalid load memory_order");
    static_assert(detail::valid_store_order(Orders::store), "AtomicCounter: invalid store memory_order");
    static_assert(detail::valid_rmw_order(Orders::rmw),     "AtomicCounter: invalid RMW memory_order");

    std::atomic<U> v{0};

#if SPSC_REQUIRE_LOCK_FREE
    static_assert(std::atomic<U>::is_always_lock_free, "AtomicCounter<U>: not always lock-free on this target");
#endif /* SPSC_REQUIRE_LOCK_FREE */

public:
    RB_FORCEINLINE void store(const U x) noexcept { v.store(x, Orders::store); }
    [[nodiscard]] RB_FORCEINLINE U load() const noexcept { return v.load(Orders::load); }
    RB_FORCEINLINE void add(const U n) noexcept { (void)v.fetch_add(n, Orders::rmw); }
    RB_FORCEINLINE void inc() noexcept { (void)v.fetch_add(U{1}, Orders::rmw); }
};


/* ------------------------------ FastAtomicCounter ------------------------------
 * Fast atomic counter
 * --------------------------------------------------------------------------- */
template<typename T, typename Orders = default_orders>
class FastAtomicCounter {
    static_assert(is_integral_not_bool_v<T>, "FastAtomicCounter<T>: T must be integral (not bool)");
    using U = std::make_unsigned_t<rb_remove_cvref_t<T>>;

public:
    static constexpr bool is_atomic = true;
    using value_type = U;

private:
    /* Compile-time sanity on memory orders */
    static_assert(detail::valid_load_order(Orders::load),   "FastAtomicCounter: invalid load memory_order");
    static_assert(detail::valid_store_order(Orders::store), "FastAtomicCounter: invalid store memory_order");
    static_assert(detail::valid_rmw_order(Orders::rmw),     "FastAtomicCounter: invalid RMW memory_order");

    std::atomic<U> v{0};

#if SPSC_REQUIRE_LOCK_FREE
    static_assert(std::atomic<U>::is_always_lock_free, "AtomicCounter<U>: not always lock-free on this target");
#endif /* SPSC_REQUIRE_LOCK_FREE */

public:
    RB_FORCEINLINE void store(const U x) noexcept { v.store(x, Orders::store); }
    [[nodiscard]] RB_FORCEINLINE U load() const noexcept { return v.load(Orders::load); }
    RB_FORCEINLINE void add(const U n) noexcept {
        const U x = v.load(std::memory_order_relaxed) + n;
        v.store(x, Orders::store);
    }
    RB_FORCEINLINE void inc() noexcept {
        const U x = v.load(std::memory_order_relaxed) + U{1};
        v.store(x, Orders::store);
    }
};

/* -------------------------- CachelineCounter -------------------------------
 * Generic cacheline-aligned counter wrapper.
 *
 * Template parameters:
 *   - Counter : already instantiated counter type
 *               (PlainCounter<reg>, VolatileCounter<reg>, AtomicCounter<reg, ...>, etc.)
 *   - AlignB  : alignment and slot size granularity (must be power-of-two).
 *               Defaults to ::spsc::hw::cacheline_bytes.
 *
 * Layout:
 *   - Uses detail::CacheSlot<Counter, AlignB>, so the object:
 *       * is aligned to AlignB
 *       * has sizeof(...) as a multiple of AlignB (guaranteed by CacheSlot)
 *   - The wrapper forwards the standard counter API to the underlying object.
 * --------------------------------------------------------------------------- */
template<
    class Counter,
    reg AlignB = ::spsc::hw::cacheline_bytes
>
class alignas(AlignB) CachelineCounter {
public:
    static constexpr bool is_atomic = Counter::is_atomic;
    using underlying_type = Counter;
    using value_type      = typename Counter::value_type;

private:
    static_assert(AlignB != 0, "CachelineCounter: AlignB must be non-zero");
    static_assert((AlignB & (AlignB - 1u)) == 0u,
                  "CachelineCounter: AlignB must be power-of-two");

    using Slot = detail::CacheSlot<Counter, AlignB>;

    Slot slot_{};

    static_assert(sizeof(Slot) % AlignB == 0,
                  "CachelineCounter: Slot size must be multiple of AlignB");

public:
    CachelineCounter() = default;

    RB_FORCEINLINE void store(const value_type x) noexcept { slot_.value.store(x); }
    [[nodiscard]] RB_FORCEINLINE value_type load() const noexcept { return slot_.value.load(); }
    RB_FORCEINLINE void add(const value_type n) noexcept { slot_.value.add(n); }
    RB_FORCEINLINE void inc() noexcept { slot_.value.inc(); }

    // Access to underlying counter when needed for advanced control.
    [[nodiscard]] RB_FORCEINLINE underlying_type&       underlying()       noexcept { return slot_.value; }
    [[nodiscard]] RB_FORCEINLINE const underlying_type& underlying() const noexcept { return slot_.value; }
};

/* Optional convenience aliases for common cases */
template<typename T, reg AlignB = ::spsc::hw::cacheline_bytes>
using CachelinePlainCounter = CachelineCounter<PlainCounter<T>, AlignB>;

template<typename T, reg AlignB = ::spsc::hw::cacheline_bytes>
using CachelineVolatileCounter = CachelineCounter<VolatileCounter<T>, AlignB>;

template<typename T, typename Orders = default_orders, reg AlignB = ::spsc::hw::cacheline_bytes>
using CachelineAtomicCounter = CachelineCounter<AtomicCounter<T, Orders>, AlignB>;

} // namespace spsc::cnt

#endif /* SPSC_COUNTER_HPP_ */
