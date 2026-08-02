/*
 * SPSCbase<C, PolicyT>
 *
 * Created on: 18 Jan. 2026
 *      Author: Shpegun60
 * Copyright (c) 2026 Shpegun60
 * SPDX-License-Identifier: Apache-2.0
 *
 * Core index and capacity logic for single-producer/single-consumer ring buffers.
 *
 * Notes on shadow indices:
 * - Shadows are only useful for atomic-backed policies (MT/SMP).
 * - For plain/volatile policies they waste bytes and can hurt caches.
 * - On 32-bit counters, wrap-around is possible. Shadow logic is wrap-safe by
 *   validating (head - tail) against capacity().
 *
 * Build toggles:
 *   - SPSC_ENABLE_SHADOW_INDICES (default: 1)
 *       0 -> disable shadows entirely
 *       1 -> enable shadows when policy counter backend is atomic-backed (and width allows)
 *
 *   - SPSC_SHADOW_ALLOW_32BIT (default: 0)
 *       0 -> shadows only on reg width >= 64
 *       1 -> allow shadows even on 32-bit reg (useful on some RTOS setups)
 *
 *   - SPSC_SHADOW_REFRESH_HEURISTIC (default: 0)
 *       0 -> no extra refresh in write_size()/read_size()
 *       1 -> refresh once when near boundary (avoid under-estimation)
 *
 *   - SPSC_SHADOW_REFRESH_FRAC_SHIFT (default: 2)
 *       Threshold = capacity() >> shift. Example: shift=2 -> 1/4 capacity.
 *
 * Threading contract (when shadows enabled):
 *   - prod_shadow_tail is updated ONLY by producer-side methods.
 *   - cons_shadow_head is updated ONLY by consumer-side methods.
 *
 * Non-concurrent operations:
 *   - init()/clear() are assumed to be called when the queue is not used concurrently.
 *   - sync_cache() must be called only in non-concurrent restore/adopt/attach(state) paths.
 */

#ifndef SPSC_RING_BASE_HPP_
#define SPSC_RING_BASE_HPP_

#include <cstddef>
#include <limits>
#include <type_traits>

#include "spsc_capacity_ctrl.hpp" // ::spsc::cap::CapacityCtrl<C, PolicyT>
#include "spsc_tools.hpp"         // RB_FORCEINLINE / RB_UNLIKELY (+ core macros)

#ifndef SPSC_ENABLE_SHADOW_INDICES
#  define SPSC_ENABLE_SHADOW_INDICES 1
#endif /* SPSC_ENABLE_SHADOW_INDICES */

#ifndef SPSC_SHADOW_ALLOW_32BIT
#  define SPSC_SHADOW_ALLOW_32BIT 0
#endif /* SPSC_SHADOW_ALLOW_32BIT */

#ifndef SPSC_SHADOW_REFRESH_HEURISTIC
#  define SPSC_SHADOW_REFRESH_HEURISTIC 0
#endif /* SPSC_SHADOW_REFRESH_HEURISTIC */

#ifndef SPSC_SHADOW_REFRESH_FRAC_SHIFT
#  define SPSC_SHADOW_REFRESH_FRAC_SHIFT 2
#endif /* SPSC_SHADOW_REFRESH_FRAC_SHIFT */

namespace spsc {


namespace detail {

/* Detect whether a counter backend is atomic-based.
 * Supports:
 *   - AtomicCounter<T, Orders>
 *   - FastAtomicCounter<T, Orders>
 *   - CachelineCounter<Inner, AlignB> wrapping the above
 *   - Any counter-like backend that exposes static constexpr bool is_atomic
 *     (defaults to false when absent)
 */
template <class T>
inline constexpr bool is_atomic_counter_backend_v =
    ::spsc::cnt::counter_is_atomic_v<typename T::counter_type>;

/* Friend-only test hook. It is defined in src/tests/test_spsc_layout.hpp and
 * intentionally does not become a public SPSCbase API.
 */
template<reg C, class PolicyT>
struct spscbase_layout_probe;

/* All data writable by one endpoint belongs to the same owner block.
 *
 * For the shadow-enabled representation:
 *   producer block: _head + prod_shadow_tail
 *   consumer block: _tail + cons_shadow_head
 *
 * Blocks are independently cache-line aligned and sized in full cache-line
 * multiples. This avoids relying on fragile base/member placement or tail
 * padding while allowing same-owner fields to remain close together.
 */
inline constexpr std::size_t rb_cacheline_bytes =
    static_cast<std::size_t>(SPSC_CACHELINE_BYTES);

static_assert(rb_cacheline_bytes != 0u,
              "SPSC cache-line size must be non-zero");
static_assert((rb_cacheline_bytes & (rb_cacheline_bytes - 1u)) == 0u,
              "SPSC cache-line size must be a power of two");

[[nodiscard]] constexpr std::size_t rb_max_size_(const std::size_t a,
                                                  const std::size_t b) noexcept {
    return (a > b) ? a : b;
}

[[nodiscard]] constexpr std::size_t rb_align_up_(const std::size_t value,
                                                  const std::size_t alignment) noexcept {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

template<class Cnt>
struct rb_owner_block_layout {
    static constexpr std::size_t kAlignment =
        rb_max_size_(rb_cacheline_bytes,
                     rb_max_size_(alignof(Cnt), alignof(reg)));
    static constexpr std::size_t kShadowOffset =
        rb_align_up_(sizeof(Cnt), alignof(reg));
    static constexpr std::size_t kContentBytes =
        kShadowOffset + sizeof(reg);
    static constexpr std::size_t kBlockBytes =
        rb_align_up_(kContentBytes, kAlignment);
    static constexpr std::size_t kTrailingBytes =
        kBlockBytes - kContentBytes;

    static_assert((kAlignment & (kAlignment - 1u)) == 0u,
                  "Owner-block alignment must be a power of two");
    static_assert(kAlignment >= rb_cacheline_bytes,
                  "Owner-block alignment must cover one cache line");
    static_assert((kBlockBytes % rb_cacheline_bytes) == 0u,
                  "Owner block must occupy a whole number of cache lines");
};

template<class Cnt, bool HasExplicitPadding =
                       (rb_owner_block_layout<Cnt>::kTrailingBytes != 0u)>
struct rb_producer_index_block_impl;

template<class Cnt>
struct alignas(rb_owner_block_layout<Cnt>::kAlignment)
    rb_producer_index_block_impl<Cnt, true> {
    using layout_type = rb_owner_block_layout<Cnt>;

    Cnt head{};
    mutable reg prod_shadow_tail{0u};
    unsigned char padding[layout_type::kTrailingBytes]{};
};

template<class Cnt>
struct alignas(rb_owner_block_layout<Cnt>::kAlignment)
    rb_producer_index_block_impl<Cnt, false> {
    Cnt head{};
    mutable reg prod_shadow_tail{0u};
};

template<class Cnt>
using rb_producer_index_block = rb_producer_index_block_impl<Cnt>;

template<class Cnt, bool HasExplicitPadding =
                       (rb_owner_block_layout<Cnt>::kTrailingBytes != 0u)>
struct rb_consumer_index_block_impl;

template<class Cnt>
struct alignas(rb_owner_block_layout<Cnt>::kAlignment)
    rb_consumer_index_block_impl<Cnt, true> {
    using layout_type = rb_owner_block_layout<Cnt>;

    Cnt tail{};
    mutable reg cons_shadow_head{0u};
    unsigned char padding[layout_type::kTrailingBytes]{};
};

template<class Cnt>
struct alignas(rb_owner_block_layout<Cnt>::kAlignment)
    rb_consumer_index_block_impl<Cnt, false> {
    Cnt tail{};
    mutable reg cons_shadow_head{0u};
};

template<class Cnt>
using rb_consumer_index_block = rb_consumer_index_block_impl<Cnt>;

template<class Cnt, bool Enabled>
struct rb_index_storage;

template<class Cnt>
struct rb_index_storage<Cnt, false> {
    static constexpr bool kHasShadows = false;

    Cnt head{};
    Cnt tail{};

    [[nodiscard]] RB_FORCEINLINE Cnt& head_counter() noexcept { return head; }
    [[nodiscard]] RB_FORCEINLINE const Cnt& head_counter() const noexcept { return head; }
    [[nodiscard]] RB_FORCEINLINE Cnt& tail_counter() noexcept { return tail; }
    [[nodiscard]] RB_FORCEINLINE const Cnt& tail_counter() const noexcept { return tail; }
};

template<class Cnt>
struct rb_index_storage<Cnt, true> {
    using layout_type = rb_owner_block_layout<Cnt>;
    using producer_block_type = rb_producer_index_block<Cnt>;
    using consumer_block_type = rb_consumer_index_block<Cnt>;

    static constexpr bool kHasShadows = true;

    static_assert(alignof(producer_block_type) >= layout_type::kAlignment,
                  "Producer owner block lost required alignment");
    static_assert(alignof(consumer_block_type) >= layout_type::kAlignment,
                  "Consumer owner block lost required alignment");
    static_assert(sizeof(producer_block_type) == layout_type::kBlockBytes,
                  "Producer owner block must match its calculated layout");
    static_assert(sizeof(consumer_block_type) == layout_type::kBlockBytes,
                  "Consumer owner block must match its calculated layout");
    static_assert((sizeof(producer_block_type) % rb_cacheline_bytes) == 0u,
                  "Producer owner block must end at a cache-line boundary");
    static_assert((sizeof(consumer_block_type) % rb_cacheline_bytes) == 0u,
                  "Consumer owner block must end at a cache-line boundary");
    static_assert(!std::is_standard_layout<Cnt>::value ||
                      std::is_standard_layout<producer_block_type>::value,
                  "A standard-layout counter requires a standard-layout producer block");
    static_assert(!std::is_standard_layout<Cnt>::value ||
                      std::is_standard_layout<consumer_block_type>::value,
                  "A standard-layout counter requires a standard-layout consumer block");

    producer_block_type producer{};
    consumer_block_type consumer{};

    [[nodiscard]] RB_FORCEINLINE Cnt& head_counter() noexcept { return producer.head; }
    [[nodiscard]] RB_FORCEINLINE const Cnt& head_counter() const noexcept { return producer.head; }
    [[nodiscard]] RB_FORCEINLINE Cnt& tail_counter() noexcept { return consumer.tail; }
    [[nodiscard]] RB_FORCEINLINE const Cnt& tail_counter() const noexcept { return consumer.tail; }

    [[nodiscard]] RB_FORCEINLINE reg& prod_shadow_tail() noexcept {
        return producer.prod_shadow_tail;
    }
    [[nodiscard]] RB_FORCEINLINE reg& prod_shadow_tail() const noexcept {
        return producer.prod_shadow_tail;
    }
    [[nodiscard]] RB_FORCEINLINE reg& cons_shadow_head() noexcept {
        return consumer.cons_shadow_head;
    }
    [[nodiscard]] RB_FORCEINLINE reg& cons_shadow_head() const noexcept {
        return consumer.cons_shadow_head;
    }
};

template <class PolicyT>
inline constexpr bool rb_use_shadow_v =
    (SPSC_ENABLE_SHADOW_INDICES != 0) &&
    is_atomic_counter_backend_v<PolicyT> &&
    ((std::numeric_limits<reg>::digits >= 64) || (SPSC_SHADOW_ALLOW_32BIT != 0));

} // namespace detail

template<reg C, typename PolicyT = ::spsc::policy::default_policy>
class SPSCbase
    : private ::spsc::cap::CapacityCtrl<C, PolicyT>
{
    static_assert((C == 0u) || cap::rb_is_pow2(C),
                  "[SPSCbase]: Capacity must be power of 2 or 0");
    static_assert(std::is_unsigned<reg>::value,
                  "[SPSCbase]: 'reg' must be unsigned");

    static constexpr unsigned kBits = std::numeric_limits<reg>::digits;
    static_assert(kBits >= 2, "[SPSCbase]: 'reg' must have at least 2 bits");
    static_assert((C == 0u) || (C <= (reg(1) << (kBits - 1))),
                  "[SPSCbase]: Capacity must be <= 2^(bits(reg)-1)");

#if SPSC_SHADOW_REFRESH_HEURISTIC
    static_assert(SPSC_SHADOW_REFRESH_FRAC_SHIFT >= 0,
                  "[SPSCbase]: SPSC_SHADOW_REFRESH_FRAC_SHIFT must be non-negative");
    static_assert(static_cast<unsigned>(SPSC_SHADOW_REFRESH_FRAC_SHIFT) < kBits,
                  "[SPSCbase]: SPSC_SHADOW_REFRESH_FRAC_SHIFT must be < bits(reg) to avoid UB shift");
#endif /* SPSC_SHADOW_REFRESH_HEURISTIC */

    using Base = ::spsc::cap::CapacityCtrl<C, PolicyT>;
    using Cnt  = typename PolicyT::counter_type;

    static_assert(::spsc::policy::detail::is_counter_like_v<Cnt>,
                  "[SPSCbase]: PolicyT::counter_type must implement store/load/add/inc with reg-compatible value");

    // Atomic backend property is independent of shadows.
    static constexpr bool kAtomicBackend =
        ::spsc::detail::is_atomic_counter_backend_v<PolicyT>;

    static constexpr bool kUseShadow =
        ::spsc::detail::rb_use_shadow_v<PolicyT>;

    using IndexStorage = ::spsc::detail::rb_index_storage<Cnt, kUseShadow>;

    static_assert(IndexStorage::kHasShadows == kUseShadow,
                  "Index storage must exactly match shadow eligibility");
    static_assert(kUseShadow || sizeof(IndexStorage) == (sizeof(Cnt) * 2u),
                  "Shadow-disabled index storage must remain compact");
    static_assert(!kUseShadow || !std::is_standard_layout<Cnt>::value ||
                      std::is_standard_layout<IndexStorage>::value,
                  "Standard-layout counters require standard-layout index storage");

    friend struct ::spsc::detail::spscbase_layout_probe<C, PolicyT>;

private:
    [[nodiscard]] static RB_FORCEINLINE reg rb_min_(const reg a, const reg b) noexcept {
        return (a < b) ? a : b;
    }

    [[nodiscard]] static RB_FORCEINLINE reg rb_mask_(const reg cap) noexcept {
        return static_cast<reg>(cap - 1u);
    }


public:
    using Base::capacity;
    using Base::mask;

protected:
    SPSCbase() noexcept = default;

    // Dynamic-capacity constructor (C == 0 only).
    template<reg C_ = C, typename = std::enable_if_t<C_ == 0>>
    explicit SPSCbase(const reg cap) noexcept {
        init(cap);
    }

    // Dynamic constructor with state restoration (C == 0 only).
    template<reg C_ = C, typename = std::enable_if_t<C_ == 0>>
    explicit SPSCbase(const reg cap, const reg initial_head, const reg initial_tail) noexcept {
        init(cap, initial_head, initial_tail);
    }

    ~SPSCbase() noexcept = default;

    // Dynamic-capacity re-init (C == 0 only).
    template<reg C_ = C, typename = std::enable_if_t<C_ == 0>>
    bool init(const reg cap) noexcept {
        const bool ok = Base::init(cap);
        clear(); // non-concurrent hard sync
        sync_cache();
        return ok;
    }

    // Re-init with explicit state (C == 0 only).
    // Returns false if geometry is bad OR if state is corrupted (size > capacity).
    template<reg C_ = C, typename = std::enable_if_t<C_ == 0>>
    bool init(const reg cap, const reg initial_head, const reg initial_tail) noexcept {
        if (!Base::init(cap)) {
            clear();
            sync_cache();
            return false;
        }

        if (capacity() == 0u) {
            clear();
            sync_cache();
            return initial_head == 0u && initial_tail == 0u;
        }

        const reg used = static_cast<reg>(initial_head - initial_tail);
        if (RB_UNLIKELY(used > capacity())) {
            clear();
            sync_cache();
            return false;
        }

        set_head(initial_head);
        set_tail(initial_tail);
        sync_cache();
        return true;
    }

    // Re-init with explicit state (C == 0 only).
    // Returns false if geometry is bad OR if state is corrupted (size > capacity).
    template<reg C_ = C, typename = std::enable_if_t<C_ != 0>>
    bool init(const reg initial_head, const reg initial_tail) noexcept {

        const reg used = static_cast<reg>(initial_head - initial_tail);
        if (RB_UNLIKELY(used > capacity())) {
            clear();
            sync_cache();
            return false;
        }

        set_head(initial_head);
        set_tail(initial_tail);
        sync_cache();
        return true;
    }

protected:
    // Non-concurrent shadow synchronization for restore/adopt/attach(state).
    // Call this ONLY when the queue is not used concurrently.
    RB_FORCEINLINE void sync_cache() noexcept {
        if constexpr (kUseShadow) {
            const reg t = _indices.tail_counter().load();
            const reg h = _indices.head_counter().load();
            _indices.prod_shadow_tail() = t;
            _indices.cons_shadow_head() = h;
        }
    }

    // Non-concurrent swap of base state (indices + geometry for dynamic).
    // Call this ONLY when both instances are not used concurrently.
    RB_FORCEINLINE void swap_base(SPSCbase &other) noexcept;

protected:
    // Core occupancy helpers.
    [[nodiscard]] RB_FORCEINLINE reg  size()  const noexcept;
    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept;
    [[nodiscard]] RB_FORCEINLINE bool full()  const noexcept;

    [[nodiscard]] RB_FORCEINLINE reg  free() const noexcept;
    [[nodiscard]] RB_FORCEINLINE bool can_write(const reg n = 1u) const noexcept;
    [[nodiscard]] RB_FORCEINLINE bool can_read (const reg n = 1u) const noexcept;

    RB_FORCEINLINE void clear() noexcept;

protected:
    // Raw head/tail accessors.
    [[nodiscard]] RB_FORCEINLINE reg head() const noexcept;
    [[nodiscard]] RB_FORCEINLINE reg tail() const noexcept;

    // Modular indices into the storage (head/tail modulo capacity()).
    [[nodiscard]] RB_FORCEINLINE reg write_index() const noexcept;
    [[nodiscard]] RB_FORCEINLINE reg read_index () const noexcept;

    // Contiguous sizes from current head/tail.
    [[nodiscard]] RB_FORCEINLINE reg write_size() const noexcept;
    [[nodiscard]] RB_FORCEINLINE reg read_size () const noexcept;

    [[nodiscard]] RB_FORCEINLINE reg write_to_end_capacity() const noexcept;
    [[nodiscard]] RB_FORCEINLINE reg read_to_end_capacity () const noexcept;

    // Sync helper (force queue to empty state from the consumer side).
    // Consumer-owned "consume all" (advances tail to head). Can be used concurrently.
    RB_FORCEINLINE void sync_tail_to_head() noexcept;

    // Advancement helpers (producer/consumer responsibilities).
    RB_FORCEINLINE void advance_head(const reg) noexcept;
    RB_FORCEINLINE void advance_tail(const reg) noexcept;

    RB_FORCEINLINE void increment_head() noexcept;
    RB_FORCEINLINE void increment_tail() noexcept;

    RB_FORCEINLINE void set_head(const reg) noexcept;
    RB_FORCEINLINE void set_tail(const reg) noexcept;

private:
    IndexStorage _indices{};
};

/* ----------------------------- definitions ----------------------------- */

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::clear() noexcept {
    _indices.tail_counter().store(0u);
    _indices.head_counter().store(0u);

    if constexpr (kUseShadow) {
        // clear() is non-concurrent by contract: safe to reset both.
        _indices.prod_shadow_tail() = 0u;
        _indices.cons_shadow_head() = 0u;
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::sync_tail_to_head() noexcept {
    // Consumer-owned operation (consume all).
    const reg h = _indices.head_counter().load();
    _indices.tail_counter().store(h);

    if constexpr (kUseShadow) {
        // Touch ONLY consumer-owned shadow to avoid a data race.
        _indices.cons_shadow_head() = h;
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::size() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    if constexpr (!kAtomicBackend) {
        const reg t = _indices.tail_counter().load();
        const reg h = _indices.head_counter().load();
        return static_cast<reg>(h - t);
    } else {
        // Consumer-safe size:
        // - Retry once on impossible snapshots (used > cap).
        // - If still impossible, report empty (0) to avoid any over-read of typed storage.
        reg t = _indices.tail_counter().load();
        reg h = _indices.head_counter().load();
        reg used = static_cast<reg>(h - t);

        if (RB_UNLIKELY(used > cap)) {
            t = _indices.tail_counter().load();
            h = _indices.head_counter().load();
            used = static_cast<reg>(h - t);

            if (RB_UNLIKELY(used > cap)) {
                return 0u;
            }
        }

        return used;
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE bool SPSCbase<C, PolicyT>::empty() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return true;
        }
    }

    const reg t = _indices.tail_counter().load();

    if constexpr (!kUseShadow) {
        const reg h = _indices.head_counter().load();

        if constexpr (!kAtomicBackend) {
            return h == t;
        } else {
            const reg av = static_cast<reg>(h - t);
            // Conservative on impossible snapshots.
            return (av == 0u) || RB_UNLIKELY(av > cap);
        }
    } else {
        // Consumer-side hot-path using shadow head.
        reg h  = _indices.cons_shadow_head();
        reg av = static_cast<reg>(h - t);

        if ((av == 0u) || (av > cap)) {
            h = _indices.head_counter().load();
            _indices.cons_shadow_head() = h;
            av = static_cast<reg>(h - t);
        }

        return (av == 0u) || RB_UNLIKELY(av > cap);
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE bool SPSCbase<C, PolicyT>::full() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return true;
        }
    }

    const reg h = _indices.head_counter().load();

    if constexpr (!kUseShadow) {
        const reg t    = _indices.tail_counter().load();
        const reg used = static_cast<reg>(h - t);

        if constexpr (kAtomicBackend) {
            return (used == cap) || RB_UNLIKELY(used > cap);
        } else {
            return used == cap;
        }
    } else {
        // Producer-side hot-path using shadow tail.
        reg t    = _indices.prod_shadow_tail();
        reg used = static_cast<reg>(h - t);

        if (used < cap) {
            return false;
        }

        t = _indices.tail_counter().load();
        _indices.prod_shadow_tail() = t;
        used = static_cast<reg>(h - t);

        return (used == cap) || RB_UNLIKELY(used > cap);
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::free() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    if constexpr (!kAtomicBackend) {
        const reg t = _indices.tail_counter().load();
        const reg h = _indices.head_counter().load();
        const reg used = static_cast<reg>(h - t);
        return (used >= cap) ? 0u : static_cast<reg>(cap - used);
    } else {
        // Producer-safe free space:
        // - Retry once on impossible snapshots (used > cap).
        // - If still impossible, report no space (0) to prevent overwrite.
        reg t = _indices.tail_counter().load();
        reg h = _indices.head_counter().load();
        reg used = static_cast<reg>(h - t);

        if (RB_UNLIKELY(used > cap)) {
            t = _indices.tail_counter().load();
            h = _indices.head_counter().load();
            used = static_cast<reg>(h - t);

            if (RB_UNLIKELY(used > cap)) {
                return 0u;
            }
        }

        return (used >= cap) ? 0u : static_cast<reg>(cap - used);
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE bool SPSCbase<C, PolicyT>::can_write(const reg n) const noexcept {
    if (n == 0u) {
        return true;
    }

    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return false;
        }
    }

    if (RB_UNLIKELY(n > cap)) {
        return false;
    }

    const reg h     = _indices.head_counter().load();
    const reg limit = static_cast<reg>(cap - n); // safe because n <= cap

    if constexpr (!kUseShadow) {
        const reg t    = _indices.tail_counter().load();
        const reg used = static_cast<reg>(h - t);

        if constexpr (kAtomicBackend) {
            if (RB_UNLIKELY(used > cap)) {
                return false;
            }
        }

        return used <= limit;
    } else {
        // Producer-side hot-path using shadow tail.
        reg t    = _indices.prod_shadow_tail();
        reg used = static_cast<reg>(h - t);

        if (used <= limit) {
            return true;
        }

        t = _indices.tail_counter().load();
        _indices.prod_shadow_tail() = t;
        used = static_cast<reg>(h - t);

        if (RB_UNLIKELY(used > cap)) {
            return false;
        }

        return used <= limit;
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE bool SPSCbase<C, PolicyT>::can_read(const reg n) const noexcept {
    if (n == 0u) {
        return true;
    }

    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return false;
        }
    }

    if (RB_UNLIKELY(n > cap)) {
        return false;
    }

    const reg t = _indices.tail_counter().load();

    if constexpr (!kUseShadow) {
        const reg h  = _indices.head_counter().load();
        const reg av = static_cast<reg>(h - t);

        if constexpr (kAtomicBackend) {
            if (RB_UNLIKELY(av > cap)) {
                return false;
            }
        }

        return av >= n;
    } else {
        // Consumer-side hot-path using shadow head.
        reg h  = _indices.cons_shadow_head();
        reg av = static_cast<reg>(h - t);

        if ((av < n) || (av > cap)) {
            h = _indices.head_counter().load();
            _indices.cons_shadow_head() = h;
            av = static_cast<reg>(h - t);

            if (RB_UNLIKELY(av > cap)) {
                return false;
            }
        }

        return av >= n;
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::head() const noexcept {
    return _indices.head_counter().load();
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::tail() const noexcept {
    return _indices.tail_counter().load();
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::write_index() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    return static_cast<reg>(_indices.head_counter().load() & rb_mask_(cap));
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::read_index() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    return static_cast<reg>(_indices.tail_counter().load() & rb_mask_(cap));
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::write_size() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    const reg h = _indices.head_counter().load();
    const reg m = rb_mask_(cap);

    if constexpr (!kUseShadow) {
        const reg t    = _indices.tail_counter().load();
        const reg used = static_cast<reg>(h - t);

        if (used >= cap) {
            return 0u;
        }

        const reg fr  = static_cast<reg>(cap - used);
        const reg w2e = static_cast<reg>(cap - (h & m));
        return rb_min_(w2e, fr);
    } else {
        // Producer-side hot-path using shadow tail.
        reg t    = _indices.prod_shadow_tail();
        reg used = static_cast<reg>(h - t);

        // Compute free space conservatively (0 on full/invalid).
        reg fr = (used < cap) ? static_cast<reg>(cap - used) : 0u;

#if SPSC_SHADOW_REFRESH_HEURISTIC
        const reg thr = static_cast<reg>(cap >> SPSC_SHADOW_REFRESH_FRAC_SHIFT);
        if ((fr == 0u) || ((thr != 0u) && (fr < thr))) {
#else
        if (fr == 0u) {
#endif /* SPSC_SHADOW_REFRESH_HEURISTIC */
            t = _indices.tail_counter().load();
            _indices.prod_shadow_tail() = t;
            used = static_cast<reg>(h - t);
            fr = (used < cap) ? static_cast<reg>(cap - used) : 0u;
        }

        if (fr == 0u) {
            return 0u;
        }

        const reg w2e = static_cast<reg>(cap - (h & m));
        return rb_min_(w2e, fr);
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::read_size() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    const reg t = _indices.tail_counter().load();
    const reg m = rb_mask_(cap);

    if constexpr (!kUseShadow) {
        reg h  = _indices.head_counter().load();
        reg av = static_cast<reg>(h - t);

        if constexpr (kAtomicBackend) {
            // Confirm once on empty or impossible snapshots.
            if ((av == 0u) || (av > cap)) {
                h  = _indices.head_counter().load();
                av = static_cast<reg>(h - t);
                if ((av == 0u) || (av > cap)) {
                    return 0u;
                }
            }
        } else {
            if (av == 0u) {
                return 0u;
            }
        }

        const reg r2e = static_cast<reg>(cap - (t & m));
        return rb_min_(r2e, av);
    } else {
        // Consumer-side hot-path using shadow head.
        reg h  = _indices.cons_shadow_head();
        reg av = static_cast<reg>(h - t);

        // Clamp availability to 0 on empty/invalid; used only to decide refresh.
        reg av_ok = ((av != 0u) && (av <= cap)) ? av : 0u;

#if SPSC_SHADOW_REFRESH_HEURISTIC
        const reg thr = static_cast<reg>(cap >> SPSC_SHADOW_REFRESH_FRAC_SHIFT);
        if ((av_ok == 0u) || ((thr != 0u) && (av_ok < thr))) {
#else
        if (av_ok == 0u) {
#endif /* SPSC_SHADOW_REFRESH_HEURISTIC */
            h = _indices.head_counter().load();
            _indices.cons_shadow_head() = h;
            av = static_cast<reg>(h - t);

            if ((av == 0u) || (av > cap)) {
                return 0u;
            }
            av_ok = av;
        }

        const reg r2e = static_cast<reg>(cap - (t & m));
        return rb_min_(r2e, av_ok);
    }
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::write_to_end_capacity() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    const reg hix = static_cast<reg>(_indices.head_counter().load() & rb_mask_(cap));
    return static_cast<reg>(cap - hix);
}

template<reg C, typename PolicyT>
RB_FORCEINLINE reg SPSCbase<C, PolicyT>::read_to_end_capacity() const noexcept {
    const reg cap = capacity();

    if constexpr (C == 0) {
        if (RB_UNLIKELY(cap == 0u)) {
            return 0u;
        }
    }

    const reg tix = static_cast<reg>(_indices.tail_counter().load() & rb_mask_(cap));
    return static_cast<reg>(cap - tix);
}

/* head ops */
template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::set_head(const reg new_head) noexcept {
    _indices.head_counter().store(new_head);
}

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::increment_head() noexcept {
    _indices.head_counter().inc();
}

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::advance_head(const reg n) noexcept {
    _indices.head_counter().add(n);
}

/* tail ops */
template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::set_tail(const reg new_tail) noexcept {
    _indices.tail_counter().store(new_tail);
}

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::increment_tail() noexcept {
    _indices.tail_counter().inc();
}

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::advance_tail(const reg n) noexcept {
    _indices.tail_counter().add(n);
}

template<reg C, typename PolicyT>
RB_FORCEINLINE void SPSCbase<C, PolicyT>::swap_base(SPSCbase &other) noexcept {
    if (this == &other) {
        return;
    }

    const reg a_head = head();
    const reg a_tail = tail();
    const reg b_head = other.head();
    const reg b_tail = other.tail();

    if constexpr (C == 0) {
        const reg a_cap = capacity();
        const reg b_cap = other.capacity();

        // Non-concurrent contract: states must already be sane.
        SPSC_ASSERT((a_cap == 0u) ? (a_head == 0u && a_tail == 0u)
                                  : (static_cast<reg>(a_head - a_tail) <= a_cap));
        SPSC_ASSERT((b_cap == 0u) ? (b_head == 0u && b_tail == 0u)
                                  : (static_cast<reg>(b_head - b_tail) <= b_cap));

        // Swap only geometry (capacity/mask) without touching head/tail.
        auto &geo_a = static_cast<Base &>(*this);   // Base == CapacityCtrl<C, PolicyT>
        auto &geo_b = static_cast<Base &>(other);

        (void)geo_a.init(b_cap);
        (void)geo_b.init(a_cap);

        // Now swap indices.
        if (RB_UNLIKELY(b_cap == 0u)) {
            set_head(0u);
            set_tail(0u);
        } else {
            set_head(b_head);
            set_tail(b_tail);
        }

        if (RB_UNLIKELY(a_cap == 0u)) {
            other.set_head(0u);
            other.set_tail(0u);
        } else {
            other.set_head(a_head);
            other.set_tail(a_tail);
        }

        sync_cache();
        other.sync_cache();
        return;
    }

    // Static geometry: just swap indices, then keep caches coherent.
    set_head(b_head);
    set_tail(b_tail);
    other.set_head(a_head);
    other.set_tail(a_tail);

    sync_cache();
    other.sync_cache();
}


} // namespace spsc

#endif /* SPSC_RING_BASE_HPP_ */
