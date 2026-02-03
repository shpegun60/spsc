/*
 * latest.hpp
 *
 * Latest-value SPSC buffer ("latest queue"):
 * - Single producer, single consumer.
 * - Producer publishes state snapshots.
 * - Consumer observes ONLY the latest published snapshot.
 * - pop() marks the last snapshot observed by front()/try_front() as consumed (tail := that head snapshot).
 *
 * This is not a FIFO.
 * If the consumer stops calling pop(), the buffer can become full and the
 * producer must stop publishing (or coalesce by not publishing).
 *
 * Template variants:
 * - latest<void, 0, Policy, Alloc>  : dynamic raw bytes (depth + bytes/slot runtime)
 * - latest<T,    0, Policy, Alloc>  : dynamic typed (depth runtime, sizeof(T) fixed)
 * - latest<T, Depth, Policy, Alloc> : static typed (depth compile-time)
 *
 * Producer-side contract:
 * - claim():        returns the next write slot (does not advance indices)
 *                  Precondition: is_valid() && !full().
 * - publish():      commits the claimed slot (advances head)
 *                  Precondition: is_valid() && !full().
 * - try_claim():    returns nullptr if invalid or full()
 * - try_publish():  returns false if invalid or full()
 * - coalescing_publish(): optional helper that keeps slack near full by sometimes NOT
 *                  advancing head (multiple producer updates coalesce into one).
 *
 * Consumer-side contract:
 * - front():        returns reference/pointer to the latest committed slot
 *                  Precondition: is_valid() && !empty().
 * - try_front():    returns nullptr if invalid or empty().
 * - pop():          consumes up to the head snapshot used by the last front()/try_front() (sticky snapshot)
 * - try_pop():      returns false if invalid or empty().
 *
 * Notes:
 * - Dynamic variants (Depth == 0) require resize() before using claim()/front().
 *   try_* helpers are safe to call at any time.
 */

#ifndef SPSC_LATEST_HPP_
#define SPSC_LATEST_HPP_

#include <array>
#include <cstddef>                 // std::byte, std::ptrdiff_t
#include <cstring>                 // std::memcpy
#include <memory>                  // std::allocator_traits, uninitialized_default_construct_n, destroy_n
#include <type_traits>
#include <utility>                 // std::swap, std::move, std::forward

#include "base/SPSCbase.hpp"           // ::spsc::SPSCbase
#include "base/spsc_alloc.hpp"         // ::spsc::alloc::default_alloc
#include "base/spsc_capacity_ctrl.hpp" // ::spsc::cap helpers
#include "base/spsc_policy.hpp"        // ::spsc::policy::default_policy
#include "base/spsc_tools.hpp"         // RB_FORCEINLINE, RB_UNLIKELY, SPSC_TRY...

namespace spsc {

/* Forward declaration */
template<class T = void, reg Depth = 0u, class Policy = ::spsc::policy::default_policy, class Alloc = ::spsc::alloc::default_alloc>
class latest;

/* ============================================================================
 * 1) Dynamic raw variant: latest<void, 0, Policy, Alloc>
 * ============================================================================
 */
template<class Policy, class Alloc>
class latest<void, 0u, Policy, Alloc> : private ::spsc::SPSCbase<0u, Policy>
{
    using Base = ::spsc::SPSCbase<0u, Policy>;

    static constexpr bool kDynamic = true;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using policy_type           = Policy;
    using counter_type          = typename Policy::counter_type;
    using geometry_type         = typename Policy::geometry_type;
    using counter_value         = typename counter_type::value_type;

    using value_type            = void;
    using size_type             = reg;
    using difference_type       = std::ptrdiff_t;

    using pointer               = void*;
    using const_pointer         = const void*;

    // Raw byte view types (internal convenience)
    using byte_type             = std::byte;
    using byte_pointer          = byte_type*;
    using const_byte_pointer    = const byte_type*;

    // Allocator types
    using base_allocator_type   = Alloc;

    // Allocator for raw bytes (per-slot payload)
    using byte_allocator_type   = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<byte_type>;
    using byte_alloc_traits     = std::allocator_traits<byte_allocator_type>;
    using byte_alloc_pointer    = typename byte_alloc_traits::pointer;

    // Allocator for the ring of pointers (void*)
    using slot_value_type       = pointer;
    using slot_allocator_type   = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<slot_value_type>;
    using slot_alloc_traits     = std::allocator_traits<slot_allocator_type>;
    using slot_pointer          = typename slot_alloc_traits::pointer;

    // ------------------------------------------------------------------------------------------
    // static asserts
    // ------------------------------------------------------------------------------------------
    static_assert(std::is_same_v<byte_alloc_pointer, byte_pointer>,
                  "[spsc::latest<void,0>]: allocator pointer type must be std::byte*.");
    static_assert(std::is_same_v<slot_pointer, slot_value_type*>,
                  "[spsc::latest<void,0>]: allocator pointer type must be void**.");
    static_assert(byte_alloc_traits::is_always_equal::value,
                  "[spsc::latest<void,0>]: requires always_equal allocator (stateless).");
    static_assert(slot_alloc_traits::is_always_equal::value,
                  "[spsc::latest<void,0>]: requires always_equal allocator (stateless).");
    static_assert(std::is_default_constructible_v<byte_allocator_type>,
                  "[spsc::latest<void,0>]: requires default-constructible allocator.");
    static_assert(std::is_default_constructible_v<slot_allocator_type>,
                  "[spsc::latest<void,0>]: requires default-constructible allocator.");
    static_assert(std::is_trivially_copyable_v<counter_value>,
                  "policy::counter_type::value_type must be trivially copyable (atomic-friendly)");
    static_assert(std::is_same_v<typename counter_type::value_type, typename geometry_type::value_type>,
                  "policy::counter_type::value_type and policy::geometry_type::value_type must match");
    static_assert(std::is_unsigned_v<counter_value>,
                  "policy::counter_type::value_type must be unsigned");

public:
    // ============================================================================
    // Interface
    // ============================================================================

    // ------------------------------------------------------------------------------------------
    // Construction / Assignment
    // ------------------------------------------------------------------------------------------
    latest() = default;

    explicit latest(const size_type depth, const size_type bytes_per_slot) {
        (void)resize(depth, bytes_per_slot);
    }

    latest(const latest&) = delete;
    latest& operator=(const latest&) = delete;

    latest(latest&& other) noexcept { move_from(std::move(other)); }

    latest& operator=(latest&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    ~latest() noexcept { destroy(); }

    void swap(latest& other) noexcept {
        using std::swap;

        swap(slots_, other.slots_);
        swap(bufferSize_, other.bufferSize_);
        swap(cons_head_snapshot_, other.cons_head_snapshot_);
        swap(cons_has_snapshot_, other.cons_has_snapshot_);

        const size_type cap_a  = Base::capacity();
        const size_type head_a = Base::head();
        const size_type tail_a = Base::tail();

        const size_type cap_b  = other.Base::capacity();
        const size_type head_b = other.Base::head();
        const size_type tail_b = other.Base::tail();

        (void)Base::init(cap_b, head_b, tail_b);
        (void)other.Base::init(cap_a, head_a, tail_a);
    }

    friend void swap(latest& a, latest& b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------------------------------
    // State / Geometry
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        return (slots_ != nullptr) && (bufferSize_ != 0u) && (Base::capacity() != 0u);
    }

    [[nodiscard]] RB_FORCEINLINE bool valid() const noexcept { return is_valid(); }

    [[nodiscard]] RB_FORCEINLINE size_type depth() const noexcept {
        return static_cast<size_type>(Base::capacity());
    }

    [[nodiscard]] RB_FORCEINLINE size_type buffer_size() const noexcept {
        return bufferSize_;
    }

    [[nodiscard]] RB_FORCEINLINE size_type bytes_per_slot() const noexcept {
        return is_valid() ? buffer_size() : 0u;
    }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection (fifo/pool-style)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE size_type capacity() const noexcept {
        return is_valid() ? depth() : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE size_type free() const noexcept {
        return is_valid() ? static_cast<size_type>(Base::free()) : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE bool can_write(const size_type n = 1u) const noexcept {
        return is_valid() && Base::can_write(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool can_read(const size_type n = 1u) const noexcept {
        return is_valid() && Base::can_read(n);
    }

    [[nodiscard]] RB_FORCEINLINE size_type write_size() const noexcept {
        return is_valid() ? static_cast<size_type>(Base::write_size()) : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE size_type read_size() const noexcept {
        return is_valid() ? static_cast<size_type>(Base::read_size()) : 0u;
    }

    [[nodiscard]] base_allocator_type get_allocator() const noexcept { return {}; }

    // Dynamic-only: explicit resource release.
    RB_FORCEINLINE void destroy() noexcept { destroy_impl(); }


    // Convenience: init == resize().
    [[nodiscard]] RB_FORCEINLINE bool init(const size_type depth_req, const size_type bytes_per_slot_req) {
        return resize(depth_req, bytes_per_slot_req);
    }

    // ------------------------------------------------------------------------------------------
    // Capacity management
    // ------------------------------------------------------------------------------------------
    /* Dynamic-only: reserve/grow (no shrink). */
    [[nodiscard]] bool reserve(const size_type min_depth, const size_type min_bytes_per_slot) {
        if (is_valid()) {
            if (depth() >= min_depth && buffer_size() >= min_bytes_per_slot) {
                return true;
            }
        }
        return resize(min_depth, min_bytes_per_slot);
    }

    /* Dynamic-only: resize/grow.
     * depth == 0 -> destroy (release memory, disable container).
     */
    [[nodiscard]] bool resize(size_type depth_req, const size_type bytes_per_slot) {
        static_assert(::spsc::cap::rb_is_pow2(::spsc::cap::RB_MAX_UNAMBIGUOUS),
                      "[latest]: RB_MAX_UNAMBIGUOUS must be power of two");

        if (depth_req == 0u) {
            destroy();
            return true;
        }

        if (bytes_per_slot == 0u) {
            return false;
        }

        if (depth_req < 2u) {
            depth_req = 2u;
        }

        if (depth_req > ::spsc::cap::RB_MAX_UNAMBIGUOUS) {
            depth_req = ::spsc::cap::RB_MAX_UNAMBIGUOUS;
        }

        const size_type depth_pow2 = static_cast<size_type>(::spsc::cap::rb_next_power2(depth_req));
        if (!::spsc::cap::rb_is_pow2(depth_pow2)) {
            return false;
        }

        if (is_valid()) {
            if (depth_pow2 <= depth() && bytes_per_slot <= buffer_size()) {
                return true;
            }
        }

        slot_allocator_type slot_alloc{};
        byte_allocator_type buf_alloc{};

        slot_pointer new_pool = slot_alloc_traits::allocate(slot_alloc, depth_pow2);
        if (RB_UNLIKELY(!new_pool)) {
            return false;
        }

        size_type i = 0u;

        SPSC_TRY {
            for (; i < depth_pow2; ++i) {
                byte_pointer buf = byte_alloc_traits::allocate(buf_alloc, bytes_per_slot);
                if (RB_UNLIKELY(!buf)) {
                    break;
                }
                new_pool[i] = static_cast<pointer>(static_cast<void*>(buf));
            }

            if (i != depth_pow2) {
                const size_type allocated = i;
                for (size_type j = 0u; j < allocated; ++j) {
                    auto* bptr = static_cast<byte_pointer>(new_pool[j]);
                    byte_alloc_traits::deallocate(buf_alloc, bptr, bytes_per_slot);
                }
                slot_alloc_traits::deallocate(slot_alloc, new_pool, depth_pow2);
                return false;
            }
        } SPSC_CATCH_ALL {
            const size_type allocated = i;
            for (size_type j = 0u; j < allocated; ++j) {
                auto* bptr = static_cast<byte_pointer>(new_pool[j]);
                byte_alloc_traits::deallocate(buf_alloc, bptr, bytes_per_slot);
            }
            slot_alloc_traits::deallocate(slot_alloc, new_pool, depth_pow2);
            SPSC_RETHROW;
        }

        destroy();

        slots_       = new_pool;
        bufferSize_  = bytes_per_slot;
        (void)Base::init(depth_pow2);
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept { return Base::empty(); }
    [[nodiscard]] RB_FORCEINLINE bool full()  const noexcept { return Base::full(); }
    [[nodiscard]] RB_FORCEINLINE size_type size() const noexcept { return static_cast<size_type>(Base::size()); }

    // ------------------------------------------------------------------------------------------
    // Producer API
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer claim() noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(!full());
        return slots_[Base::write_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(!is_valid() || full())) {
            return nullptr;
        }
        return slots_[Base::write_index()];
    }

    RB_FORCEINLINE void publish() noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(!full());
        Base::increment_head();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish() noexcept {
        if (RB_UNLIKELY(!is_valid() || full())) {
            return false;
        }
        Base::increment_head();
        return true;
    }

    /* Coalescing publish: keep slack near full by sometimes NOT advancing head.
     * Returns true only when head advanced.
     */
    [[nodiscard]] RB_FORCEINLINE bool coalescing_publish() noexcept {
        const size_type cap = depth();
        if (RB_UNLIKELY(!is_valid() || cap == 0u || full())) {
            return false;
        }
        if (cap < 4u) {
            Base::increment_head();
            return true;
        }
        const size_type used = size();
        if (used <= (cap - 3u)) {
            Base::increment_head();
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------
    // Consumer API
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE const_pointer front() const noexcept {
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            // size() can conservatively return 0 on an "impossible" snapshot (used > cap). Refresh head once.
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            SPSC_ASSERT(used2 != 0u);
            SPSC_ASSERT(used2 <= depth());
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return slots_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE pointer front() noexcept {
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            // size() can conservatively return 0 on an "impossible" snapshot (used > cap). Refresh head once.
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            SPSC_ASSERT(used2 != 0u);
            SPSC_ASSERT(used2 <= depth());
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return slots_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return nullptr;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return nullptr;
            }
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return slots_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return nullptr;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return nullptr;
            }
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return slots_[idx];
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        size_type h = 0u;
        if (cons_has_snapshot_) {
            h = cons_head_snapshot_;
        } else {
            size_type used = static_cast<size_type>(Base::size());
            if (RB_UNLIKELY(used == 0u)) {
                const size_type h2    = static_cast<size_type>(Base::head());
                const size_type used2 = static_cast<size_type>(h2 - t);
                SPSC_ASSERT(used2 != 0u);
                SPSC_ASSERT(used2 <= depth());
                used = used2;
            }
            h = static_cast<size_type>(t + used);
        }

        const size_type delta = static_cast<size_type>(h - t);
        SPSC_ASSERT(delta != 0u);
        SPSC_ASSERT(delta <= depth());
        Base::advance_tail(delta);

        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        if (cons_has_snapshot_) {
            const size_type h = cons_head_snapshot_;
            const size_type delta = static_cast<size_type>(h - t);
            if (RB_UNLIKELY(delta == 0u) || RB_UNLIKELY(delta > depth())) {
                cons_head_snapshot_ = 0u;
                cons_has_snapshot_  = false;
                return false;
            }
            Base::advance_tail(delta);
            cons_head_snapshot_ = 0u;
            cons_has_snapshot_  = false;
            return true;
        }

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return false;
            }
            used = used2;
        }

        Base::advance_tail(used);
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
        return true;
    }

    RB_FORCEINLINE void consume_all() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }
        Base::sync_tail_to_head();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    RB_FORCEINLINE void clear() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }
        Base::clear();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }


    template<class U>
    RB_FORCEINLINE void push(U&& src) noexcept {
        using V = std::decay_t<U>;
        static_assert(std::is_trivially_copyable_v<V>,
                      "latest<void,0>::push(U): U must be trivially copyable");

        SPSC_ASSERT(buffer_size() >= static_cast<size_type>(sizeof(V)));
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(!full());

        if (RB_UNLIKELY(!is_valid() || full())) {
            return;
        }
        if (RB_UNLIKELY(buffer_size() < static_cast<size_type>(sizeof(V)))) {
            return;
        }

        V tmp(std::forward<U>(src));
        pointer dst = slots_[Base::write_index()];
        std::memcpy(dst, &tmp, sizeof(V));
        Base::increment_head();
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE bool try_push(U&& src) noexcept {
        using V = std::decay_t<U>;
        static_assert(std::is_trivially_copyable_v<V>,
                      "latest<void,0>::try_push(U): U must be trivially copyable");

        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }
        if (RB_UNLIKELY(buffer_size() < static_cast<size_type>(sizeof(V)))) {
            return false;
        }

        pointer dst = try_claim();
        if (RB_UNLIKELY(!dst)) {
            return false;
        }

        V tmp(std::forward<U>(src));
        std::memcpy(dst, &tmp, sizeof(V));
        Base::increment_head();
        return true;
    }

private:
    // ============================================================================
    // Implementation details
    // ============================================================================

    void destroy_impl() noexcept {
        byte_allocator_type buf_alloc{};
        slot_allocator_type slot_alloc{};

        if (slots_ != nullptr) {
            const size_type d  = depth();
            const size_type bs = buffer_size();

            if (d != 0u && bs != 0u) {
                for (size_type i = 0u; i < d; ++i) {
                    if (slots_[i] != nullptr) {
                        auto* bptr = static_cast<byte_pointer>(slots_[i]);
                        byte_alloc_traits::deallocate(buf_alloc, bptr, bs);
                    }
                }
            }

            if (d != 0u) {
                slot_alloc_traits::deallocate(slot_alloc, slots_, d);
            }
        }

        slots_       = nullptr;
        bufferSize_  = 0u;
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_ = false;
        (void)Base::init(0u);
    }

    void move_from(latest&& other) noexcept {
        slots_              = other.slots_;
        bufferSize_         = other.bufferSize_;
        cons_head_snapshot_ = other.cons_head_snapshot_;
        cons_has_snapshot_  = other.cons_has_snapshot_;

        (void)Base::init(other.Base::capacity(), other.Base::head(), other.Base::tail());

        other.slots_              = nullptr;
        other.bufferSize_         = 0u;
        other.cons_head_snapshot_ = 0u;
        other.cons_has_snapshot_  = false;
        (void)other.Base::init(0u);
    }


private:
    slot_pointer slots_ = nullptr;
    size_type    bufferSize_ = 0u;
    mutable size_type cons_head_snapshot_ = 0u;
    mutable bool      cons_has_snapshot_ = false;
};


/* ============================================================================
 * 2) Dynamic typed variant: latest<T, 0, Policy, Alloc>
 * ============================================================================
 */
template<class T, class Policy, class Alloc>
class latest<T, 0u, Policy, Alloc> : private ::spsc::SPSCbase<0u, Policy>
{
    static_assert(!std::is_void_v<T>, "spsc::latest<T,0,Policy,Alloc>: T must not be void; use latest<void,0,...>");
    using Base = ::spsc::SPSCbase<0u, Policy>;

    static constexpr bool kDynamic = true;

public:
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type            = T;
    using pointer               = value_type*;
    using const_pointer         = const value_type*;
    using reference             = value_type&;
    using const_reference       = const value_type&;

    using size_type             = reg;
    using difference_type       = std::ptrdiff_t;

    // Allocator types
    using base_allocator_type   = Alloc;
    using allocator_type        = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<value_type>;
    using alloc_traits          = std::allocator_traits<allocator_type>;
    using alloc_pointer         = typename alloc_traits::pointer;

    // Policy types
    using policy_type           = Policy;
    using counter_type          = typename Policy::counter_type;
    using geometry_type         = typename Policy::geometry_type;
    using counter_value         = typename counter_type::value_type;

    // ------------------------------------------------------------------------------------------
    // static asserts
    // ------------------------------------------------------------------------------------------
    static_assert(std::is_same_v<alloc_pointer, pointer>,
                  "[spsc::latest<T,0>]: allocator pointer type must be T*.");
    static_assert(alloc_traits::is_always_equal::value,
                  "[spsc::latest<T,0>]: requires always_equal allocator (stateless).");
    static_assert(std::is_default_constructible_v<allocator_type>,
                  "[spsc::latest<T,0>]: requires default-constructible allocator.");
    static_assert(std::is_trivially_copyable_v<counter_value>,
                  "policy::counter_type::value_type must be trivially copyable (atomic-friendly)");
    static_assert(std::is_default_constructible_v<value_type>,
                  "[spsc::latest<T,0>]: value_type must be default-constructible.");
    static_assert(!std::is_const_v<value_type>,
                  "[spsc::latest<T,0>]: const T does not make sense for a writable container.");

#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<value_type>,
                  "[spsc::latest<T,0>]: no-exceptions mode requires noexcept default constructor.");
    static_assert(std::is_nothrow_destructible_v<value_type>,
                  "[spsc::latest<T,0>]: no-exceptions mode requires noexcept destructor.");
    static_assert(std::is_nothrow_move_assignable_v<value_type> || std::is_nothrow_copy_assignable_v<value_type>,
                  "[spsc::latest<T,0>]: no-exceptions mode requires noexcept assignment (move or copy).");
#endif /* SPSC_ENABLE_EXCEPTIONS */

    static_assert(std::is_same_v<typename counter_type::value_type, typename geometry_type::value_type>,
                  "policy::counter_type::value_type and policy::geometry_type::value_type must match");
    static_assert(std::is_unsigned_v<counter_value>,
                  "policy::counter_type::value_type must be unsigned");

public:
    // ============================================================================
    // Interface
    // ============================================================================

    // ------------------------------------------------------------------------------------------
    // Construction / Assignment
    // ------------------------------------------------------------------------------------------
    latest() = default;

    explicit latest(const size_type depth) {
        (void)resize(depth);
    }

    latest(const latest&) = delete;
    latest& operator=(const latest&) = delete;

    latest(latest&& other) noexcept { move_from(std::move(other)); }

    latest& operator=(latest&& other) noexcept(std::is_nothrow_destructible_v<value_type>) {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    ~latest() noexcept(std::is_nothrow_destructible_v<value_type>) { destroy(); }

    void swap(latest& other) noexcept {
        using std::swap;

        swap(storage_, other.storage_);
        swap(cons_head_snapshot_, other.cons_head_snapshot_);
        swap(cons_has_snapshot_, other.cons_has_snapshot_);

        const size_type cap_a  = Base::capacity();
        const size_type head_a = Base::head();
        const size_type tail_a = Base::tail();

        const size_type cap_b  = other.Base::capacity();
        const size_type head_b = other.Base::head();
        const size_type tail_b = other.Base::tail();

        (void)Base::init(cap_b, head_b, tail_b);
        (void)other.Base::init(cap_a, head_a, tail_a);
    }

    friend void swap(latest& a, latest& b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------------------------------
    // State / Geometry
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        return (storage_ != nullptr) && (Base::capacity() != 0u);
    }

    [[nodiscard]] RB_FORCEINLINE bool valid() const noexcept { return is_valid(); }

    [[nodiscard]] RB_FORCEINLINE size_type depth() const noexcept {
        return static_cast<size_type>(Base::capacity());
    }

    [[nodiscard]] RB_FORCEINLINE pointer data() noexcept { return storage_; }
    [[nodiscard]] RB_FORCEINLINE const_pointer data() const noexcept { return storage_; }

    [[nodiscard]] base_allocator_type get_allocator() const noexcept { return {}; }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection (fifo/pool-style)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE size_type capacity() const noexcept {
        return is_valid() ? depth() : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE size_type free() const noexcept {
        return is_valid() ? static_cast<size_type>(Base::free()) : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE bool can_write(const size_type n = 1u) const noexcept {
        return is_valid() && Base::can_write(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool can_read(const size_type n = 1u) const noexcept {
        return is_valid() && Base::can_read(n);
    }

    [[nodiscard]] RB_FORCEINLINE size_type write_size() const noexcept {
        return is_valid() ? static_cast<size_type>(Base::write_size()) : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE size_type read_size() const noexcept {
        return is_valid() ? static_cast<size_type>(Base::read_size()) : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept { return Base::empty(); }
    [[nodiscard]] RB_FORCEINLINE bool full()  const noexcept { return Base::full(); }
    [[nodiscard]] RB_FORCEINLINE size_type size() const noexcept { return static_cast<size_type>(Base::size()); }

    // Dynamic-only: explicit resource release.
    RB_FORCEINLINE void destroy() noexcept(std::is_nothrow_destructible_v<value_type>) { destroy_impl(); }

    // Convenience: init == resize().
    [[nodiscard]] RB_FORCEINLINE bool init(const size_type depth_req) { return resize(depth_req); }

    [[nodiscard]] bool reserve(const size_type min_depth) {
        if (depth() >= min_depth) {
            return true;
        }
        return resize(min_depth);
    }

    // ------------------------------------------------------------------------------------------
    // Capacity management
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] bool resize(size_type depth_req) {
        static_assert(::spsc::cap::rb_is_pow2(::spsc::cap::RB_MAX_UNAMBIGUOUS),
                      "[latest]: RB_MAX_UNAMBIGUOUS must be power of two");

        allocator_type alloc{};

        if (depth_req == 0u) {
            destroy();
            return true;
        }

        if (depth_req < 2u) {
            depth_req = 2u;
        }

        if (depth_req > ::spsc::cap::RB_MAX_UNAMBIGUOUS) {
            depth_req = ::spsc::cap::RB_MAX_UNAMBIGUOUS;
        }

        const size_type target_cap = static_cast<size_type>(::spsc::cap::rb_next_power2(depth_req));
        if (!::spsc::cap::rb_is_pow2(target_cap)) {
            return false;
        }

        // No growth needed.
        if (storage_ && Base::capacity() && target_cap <= depth()) {
            return true;
        }

        pointer new_buf = alloc_traits::allocate(alloc, target_cap);
        if (RB_UNLIKELY(!new_buf)) {
            return false;
        }

        SPSC_TRY {
            std::uninitialized_default_construct_n(new_buf, target_cap);
        } SPSC_CATCH_ALL {
            alloc_traits::deallocate(alloc, new_buf, target_cap);
            SPSC_RETHROW;
        }

        // Drop old state (latest is not a FIFO; resize is not a concurrent operation).
        destroy();

        storage_ = new_buf;
        (void)Base::init(target_cap);
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Producer API
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE reference claim() noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(!full());
        return storage_[Base::write_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(!is_valid() || full())) {
            return nullptr;
        }
        return &storage_[Base::write_index()];
    }

    RB_FORCEINLINE void publish() noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(!full());
        Base::increment_head();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish() noexcept {
        if (RB_UNLIKELY(!is_valid() || full())) {
            return false;
        }
        Base::increment_head();
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE bool coalescing_publish() noexcept {
        const size_type cap = depth();
        if (RB_UNLIKELY(!is_valid() || cap == 0u || full())) {
            return false;
        }
        if (cap < 4u) {
            Base::increment_head();
            return true;
        }
        const size_type used = size();
        if (used <= (cap - 3u)) {
            Base::increment_head();
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------------------------------
    // Consumer API
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE reference front() noexcept {
        // "latest" is not FIFO: front() returns the newest published element.
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            // size() can conservatively return 0 on an "impossible" snapshot (used > cap). Refresh head once.
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            SPSC_ASSERT(used2 != 0u);
            SPSC_ASSERT(used2 <= depth());
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE const_reference front() const noexcept {
        // "latest" is not FIFO: front() returns the newest published element.
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            // size() can conservatively return 0 on an "impossible" snapshot (used > cap). Refresh head once.
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            SPSC_ASSERT(used2 != 0u);
            SPSC_ASSERT(used2 <= depth());
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return nullptr;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return nullptr;
            }
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_ + idx;
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return nullptr;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return nullptr;
            }
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_ + idx;
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        size_type h = 0u;
        if (cons_has_snapshot_) {
            h = cons_head_snapshot_;
        } else {
            size_type used = static_cast<size_type>(Base::size());
            if (RB_UNLIKELY(used == 0u)) {
                const size_type h2    = static_cast<size_type>(Base::head());
                const size_type used2 = static_cast<size_type>(h2 - t);
                SPSC_ASSERT(used2 != 0u);
                SPSC_ASSERT(used2 <= depth());
                used = used2;
            }
            h = static_cast<size_type>(t + used);
        }

        const size_type delta = static_cast<size_type>(h - t);
        SPSC_ASSERT(delta != 0u);
        SPSC_ASSERT(delta <= depth());
        Base::advance_tail(delta);

        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        if (cons_has_snapshot_) {
            const size_type h = cons_head_snapshot_;
            const size_type delta = static_cast<size_type>(h - t);
            if (RB_UNLIKELY(delta == 0u) || RB_UNLIKELY(delta > depth())) {
                cons_head_snapshot_ = 0u;
                cons_has_snapshot_  = false;
                return false;
            }
            Base::advance_tail(delta);
            cons_head_snapshot_ = 0u;
            cons_has_snapshot_  = false;
            return true;
        }

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return false;
            }
            used = used2;
        }

        Base::advance_tail(used);
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
        return true;
    }

    RB_FORCEINLINE void consume_all() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }
        Base::sync_tail_to_head();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    RB_FORCEINLINE void clear() noexcept {
        Base::clear();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }


    template<class U>
    RB_FORCEINLINE void push(U&& value) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(!full());
        if (RB_UNLIKELY(!is_valid() || full())) {
            return;
        }
        reference slot = storage_[Base::write_index()];
        slot = std::forward<U>(value);
        Base::increment_head();
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE bool try_push(U&& value) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        pointer slot = try_claim();
        if (RB_UNLIKELY(!slot)) {
            return false;
        }
        *slot = std::forward<U>(value);
        Base::increment_head();
        return true;
    }

    template<class... Args>
    RB_FORCEINLINE reference emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        reference slot = claim();
        slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return slot;
    }

    template<class... Args>
    [[nodiscard]] RB_FORCEINLINE pointer try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        pointer slot = try_claim();
        if (RB_UNLIKELY(!slot)) {
            return nullptr;
        }
        *slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return slot;
    }

private:
    // ============================================================================
    // Implementation details
    // ============================================================================

    void destroy_impl() noexcept(std::is_nothrow_destructible_v<value_type>) {
        allocator_type alloc{};

        if (storage_ != nullptr) {
            const size_type cap = depth();
            if (cap != 0u) {
                if constexpr (!std::is_trivially_destructible_v<value_type>) {
                    std::destroy_n(storage_, cap);
                }
                alloc_traits::deallocate(alloc, storage_, cap);
            }
        }

        storage_ = nullptr;
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_ = false;
        (void)Base::init(0u);
    }

    void move_from(latest&& other) noexcept {
        storage_            = other.storage_;
        cons_head_snapshot_ = other.cons_head_snapshot_;
        cons_has_snapshot_  = other.cons_has_snapshot_;

        (void)Base::init(other.Base::capacity(), other.Base::head(), other.Base::tail());

        other.storage_            = nullptr;
        other.cons_head_snapshot_ = 0u;
        other.cons_has_snapshot_  = false;
        (void)other.Base::init(0u);
    }


private:
    pointer storage_ = nullptr;
    mutable size_type cons_head_snapshot_ = 0u;
    mutable bool      cons_has_snapshot_ = false;
};


/* ============================================================================
 * 3) Static typed variant: latest<T, Depth, Policy, Alloc>
 * ============================================================================
 */
template<class T, reg Depth, class Policy, class Alloc>
class latest : private ::spsc::SPSCbase<Depth, Policy>
{
    using Base = ::spsc::SPSCbase<Depth, Policy>;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type         = T;
    using size_type          = reg;
    using difference_type    = std::ptrdiff_t;

    using pointer            = value_type*;
    using const_pointer      = const value_type*;
    using reference          = value_type&;
    using const_reference    = const value_type&;

    using base_allocator_type = Alloc;
    using allocator_type      = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<value_type>;
    using alloc_traits        = std::allocator_traits<allocator_type>;
    using alloc_pointer       = typename alloc_traits::pointer;

    // Policy types
    using policy_type        = Policy;
    using counter_type       = typename Policy::counter_type;
    using geometry_type      = typename Policy::geometry_type;
    using counter_value      = typename counter_type::value_type;

    static_assert(std::is_default_constructible_v<value_type>,
                  "[spsc::latest<T,Depth>]: value_type must be default-constructible.");
    static_assert(!std::is_const_v<value_type>,
                  "[spsc::latest<T,Depth>]: const T does not make sense for a writable container.");
    static_assert(std::is_default_constructible_v<allocator_type>,
                  "[spsc::latest<T,Depth>]: allocator must be default-constructible (used by get_allocator()).");
    static_assert(std::is_same_v<alloc_pointer, pointer>,
                  "[spsc::latest<T,Depth>]: allocator pointer type must be T*." );

#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<value_type>,
                  "[spsc::latest<T,Depth>]: no-exceptions mode requires noexcept default constructor.");
    static_assert(std::is_nothrow_destructible_v<value_type>,
                  "[spsc::latest<T,Depth>]: no-exceptions mode requires noexcept destructor.");
    static_assert(std::is_nothrow_move_assignable_v<value_type> || std::is_nothrow_copy_assignable_v<value_type>,
                  "[spsc::latest<T,Depth>]: no-exceptions mode requires noexcept assignment (move or copy).");
#endif /* SPSC_ENABLE_EXCEPTIONS */

    static_assert(std::is_same_v<typename counter_type::value_type, typename geometry_type::value_type>,
                  "policy::counter_type::value_type and policy::geometry_type::value_type must match");
    static_assert(std::is_unsigned_v<counter_value>,
                  "policy::counter_type::value_type must be unsigned");
    static_assert(std::is_trivially_copyable_v<counter_value>,
                  "policy::counter_type::value_type must be trivially copyable (atomic-friendly)");
    static_assert(!std::is_void_v<T>,
                  "spsc::latest<T,Depth,Policy,Alloc>: T must not be void");
    static_assert(Depth >= 2u,
                  "spsc::latest<T,Depth,Policy,Alloc>: Depth must be >= 2");
    static_assert(::spsc::cap::rb_is_pow2(Depth),
                  "spsc::latest<T,Depth,Policy,Alloc>: Depth must be power-of-two");
    static_assert(Depth <= ::spsc::cap::RB_MAX_UNAMBIGUOUS,
                  "spsc::latest<T,Depth,Policy,Alloc>: Depth must be <= RB_MAX_UNAMBIGUOUS");

public:
    latest() = default;

    latest(const latest&) = delete;
    latest& operator=(const latest&) = delete;

    latest(latest&& other) noexcept(std::is_nothrow_swappable_v<value_type>) { move_from(std::move(other)); }

    latest& operator=(latest&& other) noexcept(std::is_nothrow_swappable_v<value_type>) {
        if (this != &other) {
            move_from(std::move(other));
        }
        return *this;
    }

    ~latest() noexcept = default;

    void swap(latest& other) noexcept(std::is_nothrow_swappable_v<value_type>) {
        using std::swap;
        swap(storage_, other.storage_);
        swap(cons_head_snapshot_, other.cons_head_snapshot_);
        swap(cons_has_snapshot_, other.cons_has_snapshot_);

        const size_type head_a = Base::head();
        const size_type tail_a = Base::tail();

        const size_type head_b = other.Base::head();
        const size_type tail_b = other.Base::tail();

        // IMPORTANT: Base has optional shadow caches (producer/consumer) used by atomic backends.
        // Any non-concurrent modification of head/tail must re-sync shadows, otherwise stale
        // shadows can under-report "used" and allow overwrites.
        {
            const bool ok_a = Base::init(head_b, tail_b);
            const bool ok_b = other.Base::init(head_a, tail_a);
            SPSC_ASSERT(ok_a);
            SPSC_ASSERT(ok_b);
            (void)ok_a;
            (void)ok_b;
        }
    }

    friend void swap(latest& a, latest& b) noexcept(noexcept(a.swap(b))) { a.swap(b); }

    [[nodiscard]] static constexpr size_type depth() noexcept { return static_cast<size_type>(Depth); }

    [[nodiscard]] RB_FORCEINLINE pointer data() noexcept { return storage_.data(); }
    [[nodiscard]] RB_FORCEINLINE const_pointer data() const noexcept { return storage_.data(); }

    [[nodiscard]] base_allocator_type get_allocator() const noexcept { return {}; }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection (fifo/pool-style)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept { return true; }

    [[nodiscard]] RB_FORCEINLINE bool valid() const noexcept { return true; }

    [[nodiscard]] static constexpr size_type capacity() noexcept { return depth(); }

    [[nodiscard]] RB_FORCEINLINE size_type free() const noexcept {
        return static_cast<size_type>(Base::free());
    }

    [[nodiscard]] RB_FORCEINLINE bool can_write(const size_type n = 1u) const noexcept {
        return Base::can_write(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool can_read(const size_type n = 1u) const noexcept {
        return Base::can_read(n);
    }

    [[nodiscard]] RB_FORCEINLINE size_type write_size() const noexcept {
        return static_cast<size_type>(Base::write_size());
    }

    [[nodiscard]] RB_FORCEINLINE size_type read_size() const noexcept {
        return static_cast<size_type>(Base::read_size());
    }

    // Static: destroy == clear indices (no allocations).

    // Static: destroy == clear indices (no allocations).
    RB_FORCEINLINE void destroy() noexcept {
        Base::clear();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept { return Base::empty(); }
    [[nodiscard]] RB_FORCEINLINE bool full()  const noexcept { return Base::full(); }
    [[nodiscard]] RB_FORCEINLINE size_type size() const noexcept { return static_cast<size_type>(Base::size()); }

    [[nodiscard]] RB_FORCEINLINE reference claim() noexcept {
        SPSC_ASSERT(!full());
        return storage_[Base::write_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(full())) {
            return nullptr;
        }
        return &storage_[Base::write_index()];
    }

    RB_FORCEINLINE void publish() noexcept {
        SPSC_ASSERT(!full());
        Base::increment_head();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish() noexcept {
        if (RB_UNLIKELY(full())) {
            return false;
        }
        Base::increment_head();
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE bool coalescing_publish() noexcept {
        if (RB_UNLIKELY(full())) {
            return false;
        }
        if (Depth < 4u) {
            Base::increment_head();
            return true;
        }
        const size_type used = size();
        if (used <= (depth() - 3u)) {
            Base::increment_head();
            return true;
        }
        return false;
    }

    [[nodiscard]] RB_FORCEINLINE reference front() noexcept {
        // "latest" is not FIFO: front() returns the newest published element.
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            // size() can conservatively return 0 on an "impossible" snapshot (used > cap). Refresh head once.
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            SPSC_ASSERT(used2 != 0u);
            SPSC_ASSERT(used2 <= depth());
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE const_reference front() const noexcept {
        // "latest" is not FIFO: front() returns the newest published element.
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            // size() can conservatively return 0 on an "impossible" snapshot (used > cap). Refresh head once.
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            SPSC_ASSERT(used2 != 0u);
            SPSC_ASSERT(used2 <= depth());
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        const size_type t = static_cast<size_type>(Base::tail());

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return nullptr;
            }
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_.data() + idx;
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        const size_type t = static_cast<size_type>(Base::tail());

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return nullptr;
            }
            used = used2;
        }

        const size_type h = static_cast<size_type>(t + used);
        cons_head_snapshot_ = h;
        cons_has_snapshot_  = true;

        const size_type idx = (h - 1u) & Base::mask();
        return storage_.data() + idx;
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(is_valid());

        const size_type t = static_cast<size_type>(Base::tail());

        size_type h = 0u;
        if (cons_has_snapshot_) {
            h = cons_head_snapshot_;
        } else {
            size_type used = static_cast<size_type>(Base::size());
            if (RB_UNLIKELY(used == 0u)) {
                const size_type h2    = static_cast<size_type>(Base::head());
                const size_type used2 = static_cast<size_type>(h2 - t);
                SPSC_ASSERT(used2 != 0u);
                SPSC_ASSERT(used2 <= depth());
                used = used2;
            }
            h = static_cast<size_type>(t + used);
        }

        const size_type delta = static_cast<size_type>(h - t);
        SPSC_ASSERT(delta != 0u);
        SPSC_ASSERT(delta <= depth());
        Base::advance_tail(delta);

        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }

        const size_type t = static_cast<size_type>(Base::tail());

        if (cons_has_snapshot_) {
            const size_type h = cons_head_snapshot_;
            const size_type delta = static_cast<size_type>(h - t);
            if (RB_UNLIKELY(delta == 0u) || RB_UNLIKELY(delta > depth())) {
                cons_head_snapshot_ = 0u;
                cons_has_snapshot_  = false;
                return false;
            }
            Base::advance_tail(delta);
            cons_head_snapshot_ = 0u;
            cons_has_snapshot_  = false;
            return true;
        }

        size_type used = static_cast<size_type>(Base::size());
        if (RB_UNLIKELY(used == 0u)) {
            const size_type h2    = static_cast<size_type>(Base::head());
            const size_type used2 = static_cast<size_type>(h2 - t);
            if (RB_UNLIKELY(used2 == 0u) || RB_UNLIKELY(used2 > depth())) {
                return false;
            }
            used = used2;
        }

        Base::advance_tail(used);
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
        return true;
    }

    RB_FORCEINLINE void consume_all() noexcept {
        Base::sync_tail_to_head();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    RB_FORCEINLINE void clear() noexcept {
        Base::clear();
        cons_head_snapshot_ = 0u;
        cons_has_snapshot_  = false;
    }

    template<class U>
    RB_FORCEINLINE void push(U&& value) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        reference slot = claim();
        slot = std::forward<U>(value);
        Base::increment_head();
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE bool try_push(U&& value) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        pointer slot = try_claim();
        if (RB_UNLIKELY(!slot)) {
            return false;
        }
        *slot = std::forward<U>(value);
        Base::increment_head();
        return true;
    }

    template<class... Args>
    RB_FORCEINLINE reference emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        reference slot = claim();
        slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return slot;
    }

    template<class... Args>
    [[nodiscard]] RB_FORCEINLINE pointer try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        pointer slot = try_claim();
        if (RB_UNLIKELY(!slot)) {
            return nullptr;
        }
        *slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return slot;
    }

private:

    void move_from(latest&& other) noexcept(std::is_nothrow_swappable_v<value_type>) {
        storage_.swap(other.storage_);

        // Keep shadow caches consistent with externally set head/tail (move is non-concurrent).
        {
            const bool ok = Base::init(other.Base::head(), other.Base::tail());
            SPSC_ASSERT(ok);
            (void)ok;
        }

        cons_head_snapshot_       = other.cons_head_snapshot_;
        cons_has_snapshot_        = other.cons_has_snapshot_;
        other.cons_head_snapshot_ = 0u;
        other.cons_has_snapshot_  = false;
        other.Base::clear();
    }


private:
    std::array<value_type, Depth> storage_{};
    mutable size_type cons_head_snapshot_ = 0u;
    mutable bool      cons_has_snapshot_ = false;
};

} // namespace spsc

#endif /* SPSC_LATEST_HPP_ */
