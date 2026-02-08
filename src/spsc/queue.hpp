/*
 * queue.hpp
 *
 * High-performance SPSC Queue (Ring Buffer) for type T.
 * Interface compatible with spsc::fifo (owning) and queue_view (non-owning).
 *
 * Design goals:
 * - Modern:      constexpr, [[nodiscard]], optional span-based bulk API.
 * - Safe:        Manages object lifetime via placement new / manual
 * destruction.
 * - Fast:        Unchecked hot-paths; bulk regions expose contiguous spans.
 * - Aligned:     Guaranteed memory alignment for T.
 *
 * Concurrency model:
 * - Single Producer / Single Consumer (wait-free / lock-free depends on
 * Policy).
 * - Producer:    push, try_push, emplace, claim, publish.
 * - Consumer:    front, pop, consume, claim_read.
 *
 * MEMORY LAYOUT NOTE:
 * - push()/emplace() constructs elements using placement new.
 * - pop() explicitly destroys elements (destructor call).
 * - resize(), clear(), swap() are NOT thread-safe with push/pop.
 */

#ifndef SPSC_QUEUE_HPP_
#define SPSC_QUEUE_HPP_

#include <algorithm> // std::max
#include <cstddef>   // std::byte, std::ptrdiff_t
#include <iterator>  // std::reverse_iterator
#include <limits>
#include <memory> // std::allocator_traits
#include <new>    // placement new
#include <type_traits>
#include <utility> // std::move, std::swap, std::declval

// Base and utility includes
#include "base/SPSCbase.hpp"      // ::spsc::SPSCbase<Capacity, Policy>
#include "base/spsc_alloc.hpp"    // ::spsc::alloc::align_alloc
#include "base/spsc_object.hpp"   // ::spsc::detail::destroy_at
#include "base/spsc_regions.hpp"  // ::spsc::bulk::region/raw_region + regions
#include "base/spsc_snapshot.hpp" // ::spsc::snapshot_view
#include "base/spsc_tools.hpp"    // RB_FORCEINLINE, RB_UNLIKELY, macros

namespace spsc {

namespace detail {

template <reg Capacity> struct queue_base {
    bool isAllocated_{false};
};

template <> struct queue_base<0> {};

} // namespace detail

/* =======================================================================
 * queue<T, Capacity, Policy, Alloc>
 *
 * Owning Single-Producer Single-Consumer ring buffer.
 * Manages object lifetime manually (Placement New / Explicit Destructor).
 * Compatible with non-default-constructible types.
 *
 * Notes:
 * - Storage is allocated (even for static Capacity) to keep the lifetime model
 *   identical across static/dynamic variants.
 * - Publishing a slot makes it visible to the consumer.
 * - A claimed write slot is UNINITIALIZED memory until you construct T
 * in-place.
 * ======================================================================= */
template <class T, reg Capacity = 0,
         typename Policy = ::spsc::policy::default_policy,
         typename Alloc = ::spsc::alloc::align_alloc<alignof(T)>>
class queue : public detail::queue_base<Capacity>,
              private ::spsc::SPSCbase<Capacity, Policy> {
    static constexpr bool kDynamic = (Capacity == 0);

    using Base = ::spsc::SPSCbase<Capacity, Policy>;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type = T;
    using pointer = value_type *;
    using const_pointer = const value_type *;
    using reference = value_type &;
    using const_reference = const value_type &;

    using size_type = reg;
    using difference_type = std::ptrdiff_t;

    // Allocator types
    using base_allocator_type = Alloc;
    using allocator_type = typename std::allocator_traits<
        base_allocator_type>::template rebind_alloc<value_type>;
    using alloc_traits = std::allocator_traits<allocator_type>;
    using alloc_pointer = typename alloc_traits::pointer;
    using alloc_size_type = typename alloc_traits::size_type;

    // Iterator types
    using iterator = ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type, false>;
    using const_iterator =
        ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type, true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // Snapshot types
    using snapshot_traits = ::spsc::snapshot_traits<std::add_const_t<value_type>, size_type>;
    using snapshot = typename snapshot_traits::snapshot;
    using const_snapshot = typename snapshot_traits::const_snapshot;
    using snapshot_iterator = typename snapshot_traits::iterator;
    using const_snapshot_iterator = typename snapshot_traits::const_iterator;

    // Policy types
    using policy_type = Policy;
    using counter_type = typename Policy::counter_type;
    using geometry_type = typename Policy::geometry_type;
    using counter_value = typename counter_type::value_type;
    using geometry_value = typename geometry_type::value_type;

    // Allocation noexcept detection (important for not lying in signatures).
    static constexpr bool kNoexceptAllocate = noexcept(alloc_traits::allocate(
        std::declval<allocator_type &>(), alloc_size_type{1}));

    // ------------------------------------------------------------------------------------------
    // Static Assertions
    // ------------------------------------------------------------------------------------------
    static_assert(
        !std::is_const_v<value_type>,
        "[spsc::queue]: const T does not make sense for a writable queue.");
    static_assert(std::numeric_limits<counter_value>::digits >= 2,
                  "[spsc::queue]: counter type is too narrow.");
    static_assert(
        ::spsc::cap::RB_MAX_UNAMBIGUOUS <=
            (counter_value(1)
             << (std::numeric_limits<counter_value>::digits - 1)),
        "[spsc::queue]: RB_MAX_UNAMBIGUOUS exceeds counter unambiguous range.");
    static_assert(
        std::is_same_v<counter_value, geometry_value>,
        "[spsc::queue]: policy counter/geometry value types must match.");
    static_assert(
        std::is_unsigned_v<counter_value>,
        "[spsc::queue]: policy counter/geometry value type must be unsigned.");
    static_assert(sizeof(counter_value) >= sizeof(size_type),
                  "[spsc::queue]: counter_type::value_type must be at least as "
                  "wide as reg.");
    static_assert(alloc_traits::is_always_equal::value,
                  "[spsc::queue]: allocator must be stateless (is_always_equal) "
                  "because the queue"
                  " default-constructs allocators for allocate/deallocate.");
    static_assert(std::is_default_constructible_v<allocator_type>,
                  "[spsc::queue]: allocator must be default-constructible.");
    static_assert(std::is_same_v<alloc_pointer, pointer>,
                  "[spsc::queue]: allocator pointer type must be raw T*.");
    static_assert(kDynamic || (Capacity >= 2u),
                  "[spsc::queue]: static Capacity must be >= 2.");
    static_assert(kDynamic || ::spsc::cap::rb_is_pow2(Capacity),
                  "[spsc::queue]: static Capacity must be a power of two.");
    static_assert(kDynamic || (Capacity <= ::spsc::cap::RB_MAX_UNAMBIGUOUS),
                  "[spsc::queue]: static Capacity exceeds RB_MAX_UNAMBIGUOUS.");

    // ------------------------------------------------------------------------------------------
    // Bulk Region Types
    // ------------------------------------------------------------------------------------------
    using write_region  = ::spsc::bulk::uninit_region<value_type, size_type>;
    using write_regions = ::spsc::bulk::region_pair<write_region, size_type>;

    using read_region  = ::spsc::bulk::init_region<value_type, size_type>;
    using read_regions = ::spsc::bulk::region_pair<read_region, size_type>;

    static constexpr size_type npos = static_cast<size_type>(~size_type(0));

    // ------------------------------------------------------------------------------------------
    // Constructors / Destructor
    // ------------------------------------------------------------------------------------------
    queue() noexcept(kDynamic || kNoexceptAllocate) {
        if constexpr (!kDynamic) {
            allocate_static_();
        }
    }

    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    explicit queue(const size_type requested_capacity) {
        (void)resize(requested_capacity);
    }

    ~queue() noexcept { destroy(); }

    queue(const queue &) = delete;
    queue &operator=(const queue &) = delete;

    queue(queue &&other) noexcept { move_from_(std::move(other)); }

    queue &operator=(queue &&other) noexcept {
        if (this != &other) {
            destroy();
            move_from_(std::move(other));
        }
        return *this;
    }

    void swap(queue &other) noexcept {
        if (this == &other) {
            return;
        }

        if constexpr (kDynamic) {
            const size_type a_cap = Base::capacity();
            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();

            const size_type b_cap = other.Base::capacity();
            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();

            std::swap(storage_, other.storage_);

            const bool ok1 =
                (b_cap != 0u) ? Base::init(b_cap, b_head, b_tail) : Base::init(0u);
            const bool ok2 = (a_cap != 0u) ? other.Base::init(a_cap, a_head, a_tail)
                                           : other.Base::init(0u);

            if (RB_UNLIKELY(!ok1 || !ok2)) {
                std::swap(storage_, other.storage_);
                (void)Base::init(a_cap, a_head, a_tail);
                (void)other.Base::init(b_cap, b_head, b_tail);
            }
        } else {
            std::swap(storage_, other.storage_);
            std::swap(this->isAllocated_, other.isAllocated_);

            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();
            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();

            Base::set_head(b_head);
            Base::set_tail(b_tail);
            Base::sync_cache();

            other.Base::set_head(a_head);
            other.Base::set_tail(a_tail);
            other.Base::sync_cache();
        }
    }

    friend void swap(queue &a, queue &b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------------------------------
    // Validity & Introspection
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        if constexpr (kDynamic) {
            return (storage_ != nullptr) && (Base::capacity() != 0u);
        } else {
            return this->isAllocated_ && (storage_ != nullptr);
        }
    }

    [[nodiscard]] size_type capacity() const noexcept {
        return is_valid() ? Base::capacity() : 0u;
    }
    [[nodiscard]] size_type size() const noexcept {
        return is_valid() ? Base::size() : 0u;
    }
    [[nodiscard]] bool empty() const noexcept {
        return !is_valid() || Base::empty();
    }
    [[nodiscard]] bool full() const noexcept {
        return !is_valid() || Base::full();
    }
    [[nodiscard]] size_type free() const noexcept {
        return is_valid() ? Base::free() : 0u;
    }

    [[nodiscard]] bool can_write(size_type n = 1u) const noexcept {
        return is_valid() && Base::can_write(n);
    }
    [[nodiscard]] bool can_read(size_type n = 1u) const noexcept {
        return is_valid() && Base::can_read(n);
    }

    [[nodiscard]] size_type write_size() const noexcept {
        return is_valid() ? Base::write_size() : 0u;
    }
    [[nodiscard]] size_type read_size() const noexcept {
        return is_valid() ? Base::read_size() : 0u;
    }

    [[nodiscard]] allocator_type get_allocator() const noexcept { return {}; }

    // Raw storage pointer (WARNING: slots beyond size() are uninitialized).
    [[nodiscard]] pointer data() noexcept { return storage_; }
    [[nodiscard]] const_pointer data() const noexcept { return storage_; }

    // ------------------------------------------------------------------------------------------
    // Iteration API (Consumer-side only)
    // ------------------------------------------------------------------------------------------
    iterator begin() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return iterator(nullptr, 0u, 0u);
        }
        return iterator(data(), Base::mask(), Base::tail());
    }
    iterator end() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return iterator(nullptr, 0u, 0u);
        }

        // Build end() from a validated used snapshot to avoid impossible
        // head<tail ranges under atomic backends.
        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);

        return iterator(data(), Base::mask(), h);
    }

    const_iterator begin() const noexcept { return cbegin(); }
    const_iterator end() const noexcept { return cend(); }
    const_iterator cbegin() const noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return const_iterator(nullptr, 0u, 0u);
        }
        return const_iterator(data(), Base::mask(), Base::tail());
    }
    const_iterator cend() const noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return const_iterator(nullptr, 0u, 0u);
        }

        // Build cend() from a validated used snapshot to avoid impossible
        // head<tail ranges under atomic backends.
        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);

        return const_iterator(data(), Base::mask(), h);
    }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    const_reverse_iterator crbegin() const noexcept {
        return const_reverse_iterator(cend());
    }
    const_reverse_iterator crend() const noexcept {
        return const_reverse_iterator(cbegin());
    }

    // ------------------------------------------------------------------------------------------
    // Snapshots
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] snapshot make_snapshot() noexcept {
        using it = snapshot_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return snapshot(it(nullptr, 0u, 0u), it(nullptr, 0u, 0u));
        }

        // Use a validated used snapshot to avoid impossible head<tail ranges
        // under atomic backends.
        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);

        const size_type m = static_cast<size_type>(Base::mask());
        return snapshot(it(data(), m, t), it(data(), m, h));
    }
    [[nodiscard]] const_snapshot make_snapshot() const noexcept {
        using it = const_snapshot_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return const_snapshot(it(nullptr, 0u, 0u), it(nullptr, 0u, 0u));
        }

        // Use a validated used snapshot to avoid impossible head<tail ranges
        // under atomic backends.
        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);

        const size_type m = static_cast<size_type>(Base::mask());
        return const_snapshot(it(data(), m, t), it(data(), m, h));
    }
    template <class Snap>
    void consume(const Snap &s) noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(s.begin().data() == data());
        SPSC_ASSERT(s.begin().mask() == Base::mask());

        const size_type cur_tail = static_cast<size_type>(Base::tail());
        SPSC_ASSERT(static_cast<size_type>(s.tail_index()) == cur_tail);

        const size_type new_tail = static_cast<size_type>(s.head_index());
        const size_type n = static_cast<size_type>(new_tail - cur_tail); // wrap-safe
        SPSC_ASSERT(n <= Base::capacity()); // Guards against impossible snapshots

        pop(n);
    }

    template <class Snap> [[nodiscard]] bool try_consume(const Snap &s) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }

        const size_type cap = Base::capacity();

        const size_type snap_tail = static_cast<size_type>(s.tail_index());
        const size_type snap_head = static_cast<size_type>(s.head_index());

        const size_type cur_tail = Base::tail();

        // Snapshot must be from the same storage (cheap identity check)
        const auto *my_data = data();
        const size_type my_mask = Base::mask();
        if (RB_UNLIKELY(s.begin().data() != my_data)) {
            return false;
        }
        if (RB_UNLIKELY(s.begin().mask() != my_mask)) {
            return false;
        }

        // Consumer must not have advanced since snapshot.
        if (RB_UNLIKELY(snap_tail != cur_tail)) {
            return false;
        }

        // Snapshot integrity: used <= capacity (wrap-safe via subtraction)
        const size_type snap_used = static_cast<size_type>(snap_head - snap_tail);
        if (RB_UNLIKELY(snap_used > cap)) {
            return false;
        }

        // Validate that the snapshot range is still available to read.
        // can_read() is allowed to be conservative on transient/invalid observations;
        // do one extra refresh attempt via a direct head reload to reduce spurious failures.
        if (RB_UNLIKELY(!Base::can_read(snap_used))) {
            const size_type h2  = static_cast<size_type>(Base::head());
            const size_type av2 = static_cast<size_type>(h2 - cur_tail);
            if (RB_UNLIKELY(av2 < snap_used) || RB_UNLIKELY(av2 > cap)) {
                return false;
            }
        }

        pop(snap_used);
        return true;
    }


    void consume_all() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }

        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            while (!empty()) {
                pop();
            }
        } else {
            Base::sync_tail_to_head();
        }
    }

    // ------------------------------------------------------------------------------------------
    // Bulk / Regions
    // ------------------------------------------------------------------------------------------
    // WARNING: write regions are raw storage (UNINITIALIZED). You must
    // placement-new into them.
    [[nodiscard]] write_regions
    claim_write(const ::spsc::unsafe_t, const size_type max_count =
                                        static_cast<size_type>(~size_type(0))) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return {};
        }

        const size_type cap = Base::capacity();
        if (RB_UNLIKELY(cap == 0u)) {
            return {};
        }

        size_type head = static_cast<size_type>(Base::head());
        size_type tail = static_cast<size_type>(Base::tail());
        size_type used = static_cast<size_type>(head - tail);

        // Atomic backends can yield an inconsistent snapshot (used > cap).
        if (RB_UNLIKELY(used > cap)) {
            head = static_cast<size_type>(Base::head());
            tail = static_cast<size_type>(Base::tail());
            used = static_cast<size_type>(head - tail);
            if (RB_UNLIKELY(used > cap)) {
                return {}; // conservative
            }
        }

        if (RB_UNLIKELY(used >= cap)) {
            return {};
        }

        size_type total = static_cast<size_type>(cap - used);
        if (max_count < total) {
            total = max_count;
        }
        if (RB_UNLIKELY(total == 0u)) {
            return {};
        }

        const size_type mask  = Base::mask();
        const size_type idx   = static_cast<size_type>(head & mask);
        const size_type to_end = static_cast<size_type>(cap - idx);

        const size_type first_n  = (to_end < total) ? to_end : total;
        const size_type second_n = static_cast<size_type>(total - first_n);

        write_regions r{};
        r.first.raw = reinterpret_cast<std::byte *>(storage_ + idx);
        r.first.count = first_n;

        r.second.count = second_n;
        r.second.raw = (second_n != 0u) ? reinterpret_cast<std::byte *>(storage_) : nullptr;

        r.total = total;
        return r;
    }


    [[nodiscard]] read_regions
    claim_read(const ::spsc::unsafe_t, const size_type max_count =
                                       static_cast<size_type>(~size_type(0))) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return {};
        }

        const size_type cap = Base::capacity();
        if (RB_UNLIKELY(cap == 0u)) {
            return {};
        }

        size_type tail = static_cast<size_type>(Base::tail());
        size_type head = static_cast<size_type>(Base::head());
        size_type av   = static_cast<size_type>(head - tail);

        // Atomic backends can yield an inconsistent snapshot (av > cap).
        if (RB_UNLIKELY(av > cap)) {
            tail = static_cast<size_type>(Base::tail());
            head = static_cast<size_type>(Base::head());
            av   = static_cast<size_type>(head - tail);
            if (RB_UNLIKELY(av > cap)) {
                return {}; // conservative
            }
        }

        if (RB_UNLIKELY(av == 0u)) {
            return {};
        }

        size_type total = av;
        if (max_count < total) {
            total = max_count;
        }
        if (RB_UNLIKELY(total == 0u)) {
            return {};
        }

        const size_type mask   = Base::mask();
        const size_type idx    = static_cast<size_type>(tail & mask);
        const size_type to_end = static_cast<size_type>(cap - idx);

        const size_type first_n  = (to_end < total) ? to_end : total;
        const size_type second_n = static_cast<size_type>(total - first_n);

        read_regions r{};
        r.first.ptr   = slot_ptr(idx);
        r.first.count = first_n;

        r.second.count = second_n;
        r.second.ptr   = (second_n != 0u) ? slot_ptr(0u) : nullptr;

        r.total = total;
        return r;
    }

    // ------------------------------------------------------------------------------------------
    // Producer Operations (Placement New)
    // ------------------------------------------------------------------------------------------
    template <class U, typename = std::enable_if_t<
                          std::is_constructible_v<value_type, U &&>>>
    RB_FORCEINLINE void push(U &&v) {
        SPSC_ASSERT(!full());
        new (&storage_[Base::write_index()]) value_type(std::forward<U>(v));
        Base::increment_head();
    }

    template <class U, typename = std::enable_if_t<
                          std::is_constructible_v<value_type, U &&>>>
    [[nodiscard]] RB_FORCEINLINE bool try_push(U &&v) {
        if (RB_UNLIKELY(full())) {
            return false;
        }
        new (&storage_[Base::write_index()]) value_type(std::forward<U>(v));
        Base::increment_head();
        return true;
    }

    template <class... Args, typename = std::enable_if_t<
                                std::is_constructible_v<value_type, Args &&...>>>
    RB_FORCEINLINE reference emplace(Args &&...args) {
        SPSC_ASSERT(!full());
        pointer slot = &storage_[Base::write_index()];
        slot = ::new (static_cast<void *>(slot)) value_type(std::forward<Args>(args)...);
        slot = std::launder(slot);
        Base::increment_head();
        return *slot;
    }

    template <class... Args, typename = std::enable_if_t<
                                std::is_constructible_v<value_type, Args &&...>>>
    [[nodiscard]] pointer try_emplace(Args &&...args) {
        if (RB_UNLIKELY(full())) {
            return nullptr;
        }
        pointer slot = &storage_[Base::write_index()];
        slot = ::new (static_cast<void *>(slot)) value_type(std::forward<Args>(args)...);
        slot = std::launder(slot);
        Base::increment_head();
        return slot;
    }

    // Returns pointer to raw storage. You MUST construct T in-place before
    // publish().
    [[nodiscard]] RB_FORCEINLINE pointer claim() noexcept {
        SPSC_ASSERT(!full());
        return &storage_[Base::write_index()];
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

    RB_FORCEINLINE void publish(const size_type n) noexcept {
        SPSC_ASSERT(can_write(n));
        Base::advance_head(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_write(n))) {
            return false;
        }
        Base::advance_head(n);
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Consumer Operations (Explicit Destructor)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE reference front() noexcept {
        SPSC_ASSERT(!empty());
        return *slot_ptr(Base::read_index());
    }

    [[nodiscard]] RB_FORCEINLINE const_reference front() const noexcept {
        SPSC_ASSERT(!empty());
        return *slot_ptr(Base::read_index());
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(empty())) {
            return nullptr;
        }
        return slot_ptr(Base::read_index());
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(empty())) {
            return nullptr;
        }
        return slot_ptr(Base::read_index());
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(!empty());
        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            detail::destroy_at(slot_ptr(Base::read_index()));
        }
        Base::increment_tail();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(empty())) {
            return false;
        }
        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            detail::destroy_at(slot_ptr(Base::read_index()));
        }
        Base::increment_tail();
        return true;
    }

    RB_FORCEINLINE void pop(const size_type n) noexcept {
        SPSC_ASSERT(can_read(n));

        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            size_type idx = Base::tail();
            const size_type mask = Base::mask();
            for (size_type i = 0; i < n; ++i) {
                detail::destroy_at(slot_ptr((idx + i) & mask));
            }
        }
        Base::advance_tail(n);
    }
    // Guard against accidental overload selection when passing a value variable
    // that is implicitly convertible to size_type. Without this, a call like:
    //   std::uint32_t out{};
    //   q.pop(out);     // <-- silently treated as pop(N) instead of "pop into out"
    // would compile and do the wrong thing.
    template <class U,
             typename = std::enable_if_t<
                 !std::is_same_v<std::remove_cv_t<U>, size_type> &&
                 std::is_convertible_v<U, size_type>>>
    void pop(U&) noexcept = delete;


    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) {
            return false;
        }
        pop(n);
        return true;
    }
    // Same trap as pop(U&): prevent q.try_pop(out) from accidentally binding to
    // try_pop(size_type n) when 'out' is a numeric lvalue convertible to size_type.
    template <class U,
             typename = std::enable_if_t<
                 !std::is_same_v<std::remove_cv_t<U>, size_type> &&
                 std::is_convertible_v<U, size_type>>>
    [[nodiscard]] bool try_pop(U&) noexcept = delete;


    [[nodiscard]] RB_FORCEINLINE const_reference
    operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx =
            static_cast<size_type>((Base::tail() + i) & Base::mask());
        return *slot_ptr(idx);
    }

    [[nodiscard]] RB_FORCEINLINE reference
    operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx =
            static_cast<size_type>((Base::tail() + i) & Base::mask());
        return *slot_ptr(idx);
    }

#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<std::byte> raw_bytes() noexcept {
        return {reinterpret_cast<std::byte *>(storage_),
                static_cast<size_t>(capacity()) * sizeof(value_type)};
    }
    [[nodiscard]] std::span<const std::byte> raw_bytes() const noexcept {
        return {reinterpret_cast<const std::byte *>(storage_),
                static_cast<size_t>(capacity()) * sizeof(value_type)};
    }
#endif /* SPSC_HAS_SPAN */

    // ------------------------------------------------------------------------------------------
    // Clear / Destroy
    // ------------------------------------------------------------------------------------------
    void clear() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }
        if constexpr (!std::is_trivially_destructible_v<value_type>) {
            const size_type cap = Base::capacity();
            const size_type head = Base::head();
            const size_type tail = Base::tail();
            const size_type used = static_cast<size_type>(head - tail);

            if (used <= cap) {
                const size_type mask = Base::mask();
                for (size_type i = 0; i < used; ++i) {
                    detail::destroy_at(slot_ptr((tail + i) & mask));
                }
            }
        }
        Base::clear();
    }

    void destroy() noexcept {
        if constexpr (kDynamic) {
            pointer p = storage_;
            size_type cap = Base::capacity();
            size_type head = Base::head();
            size_type tail = Base::tail();

            storage_ = nullptr;
            (void)Base::init(0u);

            if (p == nullptr || cap == 0u) {
                return;
            }

            // Best-effort destroy live elements if state looks sane.
            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                const size_type used = static_cast<size_type>(head - tail);
                if (used <= cap) {
                    const size_type mask = cap - 1u;
                    for (size_type i = 0; i < used; ++i) {
                        detail::destroy_at(std::launder(&p[(tail + i) & mask]));
                    }
                }
            }

            allocator_type alloc{};
            alloc_traits::deallocate(alloc, p, cap);
        } else {
            if (!this->isAllocated_ || storage_ == nullptr) {
                this->isAllocated_ = false;
                storage_ = nullptr;
                Base::clear();
                return;
            }

            clear();
            allocator_type alloc{};
            alloc_traits::deallocate(alloc, storage_, Capacity);
            storage_ = nullptr;
            this->isAllocated_ = false;
            Base::clear();
        }
    }

    // ------------------------------------------------------------------------------------------
    // Dynamic-only API (Resize)
    // ------------------------------------------------------------------------------------------
    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool reserve(size_type min_capacity) {
        if (is_valid() && capacity() >= min_capacity) {
            return true;
        }
        return resize(min_capacity);
    }

    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool resize(const size_type requested_capacity) {
        static_assert(::spsc::cap::rb_is_pow2(::spsc::cap::RB_MAX_UNAMBIGUOUS),
                      "[queue]: RB_MAX_UNAMBIGUOUS must be power of two");

        if (requested_capacity == 0u) {
            destroy();
            return true;
        }

        size_type req = requested_capacity;
        if (req > ::spsc::cap::RB_MAX_UNAMBIGUOUS) {
            req = ::spsc::cap::RB_MAX_UNAMBIGUOUS;
        }

        size_type target_cap = ::spsc::cap::rb_next_power2(req);
        if (target_cap < 2u) {
            target_cap = 2u;
        }

        if (is_valid() && target_cap <= Base::capacity()) {
            return true;
        }

        allocator_type alloc{};
        pointer new_buf = alloc_traits::allocate(alloc, target_cap);
        if (RB_UNLIKELY(new_buf == nullptr)) {
            return false;
        }

        size_type migrated = 0u;

        // Snapshot old state for migration (keep old queue intact until success).
        const bool had_old = is_valid();
        const size_type old_cap = had_old ? Base::capacity() : 0u;
        const size_type old_mask = had_old ? Base::mask() : 0u;
        const size_type old_tail = had_old ? Base::tail() : 0u;
        const size_type old_size = had_old ? Base::size() : 0u;

        SPSC_TRY {
            for (size_type i = 0; i < old_size; ++i) {
                pointer src = slot_ptr((old_tail + i) & old_mask);
                pointer dst = &new_buf[i];

                if constexpr (std::is_nothrow_move_constructible_v<value_type> ||
                              !std::is_copy_constructible_v<value_type>) {
                    new (dst) value_type(std::move(*src));
                } else {
                    new (dst) value_type(*src);
                }

                ++migrated;
            }
        }
        SPSC_CATCH_ALL {
            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                for (size_type i = 0; i < migrated; ++i) {
                    detail::destroy_at(&new_buf[i]);
                }
            }
            alloc_traits::deallocate(alloc, new_buf, target_cap);
            SPSC_RETHROW;
        }

        // Destroy and release old storage only after successful migration.
        if (had_old && storage_ != nullptr && old_cap != 0u) {
            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                // Destroy all live objects in old ring (they are moved-from but valid).
                for (size_type i = 0; i < old_size; ++i) {
                    detail::destroy_at(slot_ptr((old_tail + i) & old_mask));
                }
            }
            alloc_traits::deallocate(alloc, storage_, old_cap);
        }

        storage_ = new_buf;
        (void)Base::init(target_cap);
        if (migrated != 0u) {
            Base::set_head(migrated);
        }
        Base::sync_cache();
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // RAII Based API
    // ------------------------------------------------------------------------------------------

    class bulk_write_guard {
    public:
        bulk_write_guard() noexcept = default;

        explicit bulk_write_guard(queue &q,
                                  const size_type max_count = static_cast<size_type>(~size_type(0))) noexcept
            : q_(&q), regs_(q.claim_write(::spsc::unsafe, max_count)) {
            if (regs_.total == 0u) {
                q_ = nullptr;
            }
        }

        bulk_write_guard(const bulk_write_guard &) = delete;
        bulk_write_guard &operator=(const bulk_write_guard &) = delete;

        bulk_write_guard(bulk_write_guard &&other) noexcept
            : q_(other.q_), regs_(other.regs_), constructed_(other.constructed_),
            publish_on_destroy_(other.publish_on_destroy_) {
            other.q_ = nullptr;
            other.regs_ = {};
            other.constructed_ = 0u;
            other.publish_on_destroy_ = false;
        }

        bulk_write_guard &operator=(bulk_write_guard &&) = delete;

        ~bulk_write_guard() noexcept {
            if (q_ == nullptr || regs_.total == 0u) {
                return;
            }

            if (constructed_ == 0u) {
                return;
            }

            if (publish_on_destroy_) {
                q_->publish(constructed_);
                return;
            }

            destroy_constructed_();
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return (q_ != nullptr) && (regs_.total != 0u);
        }

        [[nodiscard]] size_type claimed() const noexcept { return regs_.total; }
        [[nodiscard]] size_type constructed() const noexcept { return constructed_; }
        [[nodiscard]] size_type remaining() const noexcept { return regs_.total - constructed_; }

        template <class... Args>
        [[nodiscard]] pointer emplace_next(Args &&...args) noexcept(
            std::is_nothrow_constructible_v<value_type, Args &&...>) {
            SPSC_ASSERT(q_ != nullptr);
            SPSC_ASSERT(constructed_ < regs_.total);

            pointer p = slot_ptr_at_(constructed_);
            p = ::new (static_cast<void *>(p)) value_type(std::forward<Args>(args)...);
            p = std::launder(p);

            ++constructed_;
            publish_on_destroy_ = true;
            return p;
        }

        void arm_publish() noexcept {
            SPSC_ASSERT(q_ != nullptr);
            SPSC_ASSERT(constructed_ != 0u && "arm_publish() requires at least one constructed element");
            publish_on_destroy_ = true;
        }

        void disarm_publish() noexcept { publish_on_destroy_ = false; }

        void commit() noexcept {
            if (q_ && constructed_ != 0u) {
                q_->publish(constructed_);
            }
            // After publish, consumer owns destruction.
            reset_();
        }

        void cancel() noexcept {
            if (q_ && constructed_ != 0u) {
                destroy_constructed_();
            }
            reset_();
        }

    private:
        [[nodiscard]] pointer slot_ptr_at_(const size_type i) const noexcept {
            SPSC_ASSERT(i < regs_.total);
            if (i < regs_.first.count) {
                return regs_.first.ptr_uninit() + i;
            }
            return regs_.second.ptr_uninit() + (i - regs_.first.count);
        }

        void destroy_constructed_() noexcept {
            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                for (size_type i = 0; i < constructed_; ++i) {
                    ::spsc::detail::destroy_at(std::launder(slot_ptr_at_(i)));
                }
            }
        }

        void reset_() noexcept {
            q_ = nullptr;
            regs_ = {};
            constructed_ = 0u;
            publish_on_destroy_ = false;
        }

        queue *q_{nullptr};
        write_regions regs_{};
        size_type constructed_{0u};
        bool publish_on_destroy_{false};
    };

    class bulk_read_guard {
    public:
        bulk_read_guard() noexcept = default;

        explicit bulk_read_guard(queue &q,
                                 const size_type max_count = static_cast<size_type>(~size_type(0))) noexcept
            : q_(&q), regs_(q.claim_read(::spsc::unsafe, max_count)), active_(regs_.total != 0u) {
            if (!active_) {
                q_ = nullptr;
            }
        }

        bulk_read_guard(const bulk_read_guard &) = delete;
        bulk_read_guard &operator=(const bulk_read_guard &) = delete;

        bulk_read_guard(bulk_read_guard &&other) noexcept
            : q_(other.q_), regs_(other.regs_), active_(other.active_) {
            other.q_ = nullptr;
            other.regs_ = {};
            other.active_ = false;
        }

        bulk_read_guard &operator=(bulk_read_guard &&) = delete;

        ~bulk_read_guard() noexcept {
            if (active_ && q_) {
                q_->pop(regs_.total);
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept { return active_; }

        [[nodiscard]] size_type count() const noexcept { return regs_.total; }
        [[nodiscard]] const read_regions &regions() const noexcept { return regs_; }

        void commit() noexcept {
            if (active_ && q_) {
                q_->pop(regs_.total);
            }
            active_ = false;
            q_ = nullptr;
            regs_ = {};
        }

        void cancel() noexcept {
            active_ = false;
            q_ = nullptr;
            regs_ = {};
        }

    private:
        queue *q_{nullptr};
        read_regions regs_{};
        bool active_{false};
    };

    class write_guard {
    public:
        write_guard() noexcept = default;

        explicit write_guard(queue &q) noexcept : q_(&q), ptr_(q.try_claim()) {}

        write_guard(const write_guard &) = delete;
        write_guard &operator=(const write_guard &) = delete;

        write_guard(write_guard &&other) noexcept
            : q_(other.q_), ptr_(other.ptr_), constructed_(other.constructed_),
            publish_on_destroy_(other.publish_on_destroy_) {
            other.q_ = nullptr;
            other.ptr_ = nullptr;
            other.constructed_ = false;
            other.publish_on_destroy_ = false;
        }

        write_guard &operator=(write_guard &&) = delete;

        ~write_guard() noexcept {
            if (q_ == nullptr || ptr_ == nullptr) {
                return;
            }

            if (publish_on_destroy_ && constructed_) {
                q_->publish();
                return;
            }

            // Constructed but not published: destroy to avoid leaking a live object
            // in an unclaimed slot.
            if (constructed_) {
                detail::destroy_at(std::launder(ptr_));
            }
        }

        // Raw storage pointer (UNINITIALIZED until you construct T in-place).
        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return (q_ != nullptr) && (ptr_ != nullptr); }

        // Safe path: construct and arm publishing.
        template <class... Args>
        [[nodiscard]] pointer emplace(Args &&...args) noexcept(
            std::is_nothrow_constructible_v<value_type, Args &&...>) {
            SPSC_ASSERT(q_ && ptr_);
            SPSC_ASSERT(!constructed_);
            ptr_ = ::new (static_cast<void *>(ptr_)) value_type(std::forward<Args>(args)...);
            ptr_ = std::launder(ptr_);
            constructed_ = true;
            publish_on_destroy_ = true;
            return ptr_;
        }

        // Manual path: call this AFTER placement-new to arm publishing on scope
        // exit.
        void mark_constructed() noexcept {
            SPSC_ASSERT(q_ && ptr_);
            SPSC_ASSERT(!constructed_);
            ptr_ = std::launder(ptr_);
            constructed_ = true;
        }

        void arm_publish() noexcept {
            SPSC_ASSERT(q_ && ptr_);
            SPSC_ASSERT(constructed_ && "arm_publish() requires a constructed object; call mark_constructed() after placement-new");
            publish_on_destroy_ = true;
        }

        void commit() noexcept {
            if (q_ && ptr_) {
                SPSC_ASSERT(constructed_ && "write_guard::commit() publishing an unconstructed slot");
                if (constructed_) {
                    q_->publish();
                }
            }

            // After publish, consumer owns destruction.
            publish_on_destroy_ = false;
            constructed_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }
        void cancel() noexcept {
            if (q_ && ptr_ && constructed_) {
                detail::destroy_at(std::launder(ptr_));
            }

            publish_on_destroy_ = false;
            constructed_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        queue *q_{nullptr};
        pointer ptr_{nullptr};
        bool constructed_{false};
        bool publish_on_destroy_{false};
    };

    class read_guard {
    public:
        read_guard() noexcept = default;

        explicit read_guard(queue &q) noexcept
            : q_(&q), ptr_(q.try_front()), active_(ptr_ != nullptr) {}

        read_guard(const read_guard &) = delete;
        read_guard &operator=(const read_guard &) = delete;

        read_guard(read_guard &&other) noexcept
            : q_(other.q_), ptr_(other.ptr_), active_(other.active_) {
            other.q_ = nullptr;
            other.ptr_ = nullptr;
            other.active_ = false;
        }

        read_guard &operator=(read_guard &&) = delete;

        ~read_guard() noexcept {
            if (active_ && q_) {
                q_->pop();
            }
        }

        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        [[nodiscard]] reference ref() const noexcept {
            SPSC_ASSERT(ptr_ != nullptr);
            return *ptr_;
        }
        [[nodiscard]] reference operator*() const noexcept { return ref(); }
        [[nodiscard]] pointer operator->() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return active_; }
        void commit() noexcept {
            if (active_ && q_) {
                q_->pop();
            }
            cancel();
        }

        void cancel() noexcept {
            active_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        queue *q_{nullptr};
        pointer ptr_{nullptr};
        bool active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept {
        return write_guard(*this);
    }
    [[nodiscard]] bulk_write_guard scoped_write(const size_type max_count) noexcept {
        return bulk_write_guard(*this, max_count);
    }
    [[nodiscard]] read_guard scoped_read() noexcept { return read_guard(*this); }
    [[nodiscard]] bulk_read_guard scoped_read(const size_type max_count) noexcept {
        return bulk_read_guard(*this, max_count);
    }

private:
    void allocate_static_() noexcept(kNoexceptAllocate) {
        if constexpr (!kDynamic) {
            allocator_type alloc{};
            storage_ = alloc_traits::allocate(alloc, Capacity);
            this->isAllocated_ = (storage_ != nullptr);
            Base::clear();
        }
    }

    void move_from_(queue &&other) noexcept {
        storage_ = other.storage_;

        if constexpr (kDynamic) {
            (void)Base::init(other.Base::capacity(), other.Base::head(),
                              other.Base::tail());
            other.storage_ = nullptr;
            (void)other.Base::init(0u);
        } else {
            this->isAllocated_ = other.isAllocated_;
            Base::set_head(other.Base::head());
            Base::set_tail(other.Base::tail());
            Base::sync_cache();

            other.storage_ = nullptr;
            other.isAllocated_ = false;
            other.Base::clear();
        }
    }

    /*
   * Helper to obtain a pointer to a live object within the storage.
   */
    [[nodiscard]] RB_FORCEINLINE pointer
    slot_ptr(size_type index) const noexcept {
        // &storage_[index] gives T*. std::launder is needed when storage is reused via placement-new;
        // it does not provide synchronization or memory ordering.
        return std::launder(&storage_[index]);
    }

private:
    pointer storage_{nullptr};
};

// ---------------------------------------------------------------------------
// Convenience Aliases
// ---------------------------------------------------------------------------

/**
 * fast_queue<T, Capacity>:
 * A pre-configured queue using Atomic counters and Cache-line padding.
 */
template <class T, reg Capacity = 0,
         typename Alloc = ::spsc::alloc::align_alloc<alignof(T)>>
using fast_queue = queue<T, Capacity, ::spsc::policy::CA<>, Alloc>;

} // namespace spsc

#endif /* SPSC_QUEUE_HPP_ */
