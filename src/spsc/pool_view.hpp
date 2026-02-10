/*
 * pool_view.hpp
 *
 * Non-owning SPSC pool view with the same API surface as spsc::pool.
 *
 * The ring stores 'void*' pointers.
 * Each pointer refers to a fixed-size buffer of 'buffer_size()' bytes.
 *
 * Notes:
 * - This type owns ONLY indices/geometry (SPSCbase) plus a raw pointer to the slot array.
 * - It does NOT allocate and does NOT free/destroy the underlying buffers.
 * - Copy is deleted: copying would duplicate head/tail state while aliasing the same storage.
 */

#ifndef SPSC_POOL_VIEW_HPP_
#define SPSC_POOL_VIEW_HPP_

#include <array>
#include <cstddef>      // std::ptrdiff_t, std::byte
#include <cstdint>      // std::uintptr_t
#include <cstring>      // std::memcpy
#include <iterator>     // std::reverse_iterator
#include <limits>
#include <type_traits>
#include <utility>      // std::swap, std::move

// Base and utility includes
#include "base/SPSCbase.hpp"        // ::spsc::SPSCbase<Capacity, Policy>, reg
#include "base/spsc_snapshot.hpp"   // ::spsc::snapshot_view, ::spsc::snapshot_traits
#include "base/spsc_regions.hpp"    // ::spsc::bulk::slot_region/slot_regions
#include "base/spsc_tools.hpp"      // RB_FORCEINLINE, RB_UNLIKELY, SPSC_HAS_SPAN

namespace spsc {

/* =======================================================================
 * pool_view<Capacity, Policy>
 *
 * Non-owning Single-Producer Single-Consumer pool of fixed-size raw buffers.
 * Can be static-depth (Capacity != 0) or dynamic-depth (Capacity == 0).
 * ======================================================================= */
template<reg Capacity = 0, typename Policy = ::spsc::policy::default_policy>
class pool_view : private ::spsc::SPSCbase<Capacity, Policy>
{
    static constexpr bool kDynamic = (Capacity == 0);

    using Base = ::spsc::SPSCbase<Capacity, Policy>;

    static constexpr bool kNoThrowMoveOps = true;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type      = void*;
    using pointer         = void*;
    using const_pointer   = const void*;
    using reference       = pointer&;
    using const_reference = pointer const&;

    // Size types
    using size_type       = reg;
    using difference_type = std::ptrdiff_t;

    // Iterator types
    using iterator               	= ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type, false>;
    using const_iterator         	= ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type, true>;
    using reverse_iterator       	= std::reverse_iterator<iterator>;
    using const_reverse_iterator 	= std::reverse_iterator<const_iterator>;

    // Snapshot types
    using snapshot_traits      		= ::spsc::snapshot_traits<std::add_const_t<value_type>, size_type>;
    using snapshot             		= typename snapshot_traits::snapshot;
    using const_snapshot    		= typename snapshot_traits::const_snapshot;
    using snapshot_iterator    		= typename snapshot_traits::iterator;
    using const_snapshot_iterator 	= typename snapshot_traits::const_iterator;

    // Policy types
    using policy_type    = Policy;
    using counter_type   = typename Policy::counter_type;
    using geometry_type  = typename Policy::geometry_type;
    using counter_value  = typename counter_type::value_type;
    using geometry_value = typename geometry_type::value_type;

    // ------------------------------------------------------------------------------------------
    // Static Assertions
    // ------------------------------------------------------------------------------------------
    static_assert(std::numeric_limits<counter_value>::digits >= 2,
                  "[spsc::pool_view]: counter type is too narrow.");
    static_assert(::spsc::cap::RB_MAX_UNAMBIGUOUS <= (counter_value(1) << (std::numeric_limits<counter_value>::digits - 1)),
                  "[spsc::pool_view]: RB_MAX_UNAMBIGUOUS exceeds counter unambiguous range.");
    static_assert(std::is_same_v<counter_value, geometry_value>,
                  "[spsc::pool_view]: policy counter/geometry value types must match.");
    static_assert(std::is_unsigned_v<counter_value>,
                  "[spsc::pool_view]: policy counter/geometry value type must be unsigned.");
    static_assert(sizeof(counter_value) >= sizeof(size_type),
                  "[spsc::pool_view]: counter_type::value_type must be at least as wide as reg.");
    static_assert(Capacity == 0 || ::spsc::cap::rb_is_pow2(Capacity),
                  "[spsc::pool_view]: static Capacity must be power-of-two.");
    static_assert(Capacity == 0 || Capacity >= 2,
                  "[spsc::pool_view]: Capacity must be >= 2 (or 0 for dynamic).");

    // ------------------------------------------------------------------------------------------
    // Region Types (Bulk Operations)
    // ------------------------------------------------------------------------------------------
    using region  = ::spsc::bulk::slot_region<pointer, size_type>;
    using regions = ::spsc::bulk::slot_regions<pointer, size_type>;

    // ------------------------------------------------------------------------------------------
    // Serializable State
    // ------------------------------------------------------------------------------------------
    struct state_t {
        size_type head{0u};
        size_type tail{0u};
    };

    // ------------------------------------------------------------------------------------------
    // Constructors / Destructor
    // ------------------------------------------------------------------------------------------

    // Default constructor: invalid until storage is attached.
    pool_view() = default;

    // [Static] Attach to external slot array (pointer must NOT be a raw C-array argument).
    template<class Slots,
             size_type C = Capacity,
             typename = std::enable_if_t<
                 (C != 0) &&
                 std::is_pointer_v<std::remove_reference_t<Slots>> &&
                 std::is_same_v<std::remove_cv_t<std::remove_reference_t<Slots>>, pointer*>
                 >>
    explicit pool_view(Slots&& slot_array, const size_type buffer_size) noexcept
        : Base(), slots_(slot_array)
    {
        bufferSize_.store(buffer_size);
        Base::clear();
        if (RB_UNLIKELY(slots_ == nullptr) || RB_UNLIKELY(bufferSize_.load() == 0u)) { detach(); }
    }

    // [Static] Attach with Policy tag.
    template<class Slots,
             size_type C = Capacity,
             typename = std::enable_if_t<
                 (C != 0) &&
                 std::is_pointer_v<std::remove_reference_t<Slots>> &&
                 std::is_same_v<std::remove_cv_t<std::remove_reference_t<Slots>>, pointer*>
                 >>
    explicit pool_view(Slots&& slot_array, const size_type buffer_size, Policy) noexcept
        : pool_view(std::forward<Slots>(slot_array), buffer_size)
    {}

    // [Static] Attach via std::array.
    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    explicit pool_view(std::array<pointer, Capacity>& arr, const size_type buffer_size) noexcept
        : pool_view(arr.data(), buffer_size)
    {}

    // [Static] Attach via std::array with Policy tag.
    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    explicit pool_view(std::array<pointer, Capacity>& arr, const size_type buffer_size, Policy) noexcept
        : pool_view(arr.data(), buffer_size)
    {}

    // [Static] Attach via raw C-array reference.
    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    explicit pool_view(pointer (&arr)[N], const size_type buffer_size) noexcept
        : pool_view(&arr[0], buffer_size)
    {}

    // [Static] Attach via raw C-array reference with Policy tag.
    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    explicit pool_view(pointer (&arr)[N], const size_type buffer_size, Policy) noexcept
        : pool_view(&arr[0], buffer_size)
    {}

    // [Dynamic] Attach to external slot array with runtime depth (Capacity == 0).
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    pool_view(pointer* slot_array, const size_type depth, const size_type buffer_size) noexcept
        : Base(), slots_(slot_array)
    {
        bufferSize_.store(buffer_size);
        const bool ok = Base::init(depth);
        if (RB_UNLIKELY(!ok) || RB_UNLIKELY(slots_ == nullptr) || RB_UNLIKELY(bufferSize_.load() == 0u)
            || RB_UNLIKELY(Base::capacity() == 0u))
        {
            detach();
        }
    }

    // [Dynamic] Attach with Policy tag.
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    pool_view(pointer* slot_array, const size_type depth, const size_type buffer_size, Policy) noexcept
        : pool_view(slot_array, depth, buffer_size)
    {}

    ~pool_view() noexcept = default;

    pool_view(const pool_view&)            = delete;
    pool_view& operator=(const pool_view&) = delete;

    pool_view(pool_view&& other) noexcept(kNoThrowMoveOps) { move_from(std::move(other)); }

    pool_view& operator=(pool_view&& other) noexcept(kNoThrowMoveOps) {
        if (this != &other) {
            detach();
            move_from(std::move(other));
        }
        return *this;
    }

    void swap(pool_view& other) noexcept
    {
        if (this == &other) { return; }

        if constexpr (kDynamic) {
            const size_type a_cap  = Base::capacity();
            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();
            pointer* const  a_ptr  = slots_;
            const size_type a_bs   = bufferSize_.load();

            const size_type b_cap  = other.Base::capacity();
            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();
            pointer* const  b_ptr  = other.slots_;
            const size_type b_bs   = other.bufferSize_.load();

            // Invariant for dynamic view: detached == (ptr==nullptr && bs==0 && cap==0)
            SPSC_ASSERT((a_ptr == nullptr) == (a_cap == 0u));
            SPSC_ASSERT((b_ptr == nullptr) == (b_cap == 0u));
            SPSC_ASSERT((a_ptr == nullptr) == (a_bs == 0u));
            SPSC_ASSERT((b_ptr == nullptr) == (b_bs == 0u));

            std::swap(slots_, other.slots_);

            // Manual swap for geometry_type
            bufferSize_.store(b_bs);
            other.bufferSize_.store(a_bs);

            const bool ok1 = (b_ptr != nullptr && b_bs != 0u && b_cap != 0u)
                                 ? Base::init(b_cap, b_head, b_tail)
                                 : Base::init(0u);

            const bool ok2 = (a_ptr != nullptr && a_bs != 0u && a_cap != 0u)
                                 ? other.Base::init(a_cap, a_head, a_tail)
                                 : other.Base::init(0u);

            if (RB_UNLIKELY(!ok1 || !ok2)) {
                // Rollback
                std::swap(slots_, other.slots_);

                // Rollback buffer sizes
                bufferSize_.store(a_bs);
                other.bufferSize_.store(b_bs);

                const bool rb1 = (a_ptr != nullptr && a_bs != 0u && a_cap != 0u)
                                     ? Base::init(a_cap, a_head, a_tail)
                                     : Base::init(0u);

                const bool rb2 = (b_ptr != nullptr && b_bs != 0u && b_cap != 0u)
                                     ? other.Base::init(b_cap, b_head, b_tail)
                                     : other.Base::init(0u);

                SPSC_ASSERT(rb1 && rb2);
                (void)rb1; (void)rb2;
            }

            if (slots_ == nullptr) { (void)Base::init(0u); bufferSize_.store(0u); }
            if (other.slots_ == nullptr) { (void)other.Base::init(0u); other.bufferSize_.store(0u); }
        } else {
            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();
            const size_type a_bs   = bufferSize_.load();

            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();
            const size_type b_bs   = other.bufferSize_.load();

            std::swap(slots_, other.slots_);

            // Manual swap for geometry_type
            bufferSize_.store(b_bs);
            other.bufferSize_.store(a_bs);

            const bool ok_a = Base::init(b_head, b_tail);
            const bool ok_b = other.Base::init(a_head, a_tail);
            SPSC_ASSERT(ok_a && ok_b);
            (void)ok_a; (void)ok_b;

            if (RB_UNLIKELY(slots_ == nullptr || bufferSize_.load() == 0u)) { detach(); }
            if (RB_UNLIKELY(other.slots_ == nullptr || other.bufferSize_.load() == 0u)) { other.detach(); }
        }
    }

    friend void swap(pool_view& a, pool_view& b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        if constexpr (kDynamic) {
            return (slots_ != nullptr) && (bufferSize_.load() != 0u) && (Base::capacity() != 0u);
        } else {
            return (slots_ != nullptr) && (bufferSize_.load() != 0u);
        }
    }

    [[nodiscard]] state_t state() const noexcept {
        if (RB_UNLIKELY(!is_valid())) { return {}; }
        return state_t{Base::head(), Base::tail()};
    }

    [[nodiscard]] size_type capacity() const noexcept { return is_valid() ? Base::capacity() : 0u; }
    [[nodiscard]] size_type size()     const noexcept { return is_valid() ? Base::size() : 0u; }
    [[nodiscard]] bool      empty()    const noexcept { return !is_valid() || Base::empty(); }
    [[nodiscard]] bool      full()     const noexcept { return !is_valid() || Base::full(); }
    [[nodiscard]] size_type free()     const noexcept { return is_valid() ? Base::free() : 0u; }

    [[nodiscard]] size_type buffer_size() const noexcept { return is_valid() ? bufferSize_.load() : 0u; }

    [[nodiscard]] bool can_write(size_type n = 1u) const noexcept { return is_valid() && Base::can_write(n); }
    [[nodiscard]] bool can_read (size_type n = 1u) const noexcept { return is_valid() && Base::can_read(n); }

    [[nodiscard]] size_type write_size() const noexcept { return is_valid() ? Base::write_size() : 0u; }
    [[nodiscard]] size_type read_size()  const noexcept { return is_valid() ? Base::read_size()  : 0u; }

    void clear() noexcept { Base::clear(); }

    // Explicit detach: makes the view invalid (does not touch external storage).
    void detach() noexcept {
        slots_ = nullptr;
        bufferSize_.store(0u);
        if constexpr (kDynamic) { (void)Base::init(0u); }
        else { Base::clear(); }
    }

    // Attach / adopt API (NOT concurrent with producer/consumer ops)

    template<class Slots,
             size_type C = Capacity,
             typename = std::enable_if_t<
                 (C != 0) &&
                 std::is_pointer_v<std::remove_reference_t<Slots>> &&
                 std::is_same_v<std::remove_cv_t<std::remove_reference_t<Slots>>, pointer*>
                 >>
    [[nodiscard]] bool attach(Slots&& slot_array, const size_type buffer_size) noexcept
    {
        if (RB_UNLIKELY(slot_array == nullptr) || RB_UNLIKELY(buffer_size == 0u)) {
            detach();
            return false;
        }
        slots_ = slot_array;
        bufferSize_.store(buffer_size);
        Base::clear();
        return true;
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool attach(std::array<pointer, Capacity>& arr, const size_type buffer_size) noexcept
    {
        return attach(arr.data(), buffer_size);
    }

    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    [[nodiscard]] bool attach(pointer (&arr)[N], const size_type buffer_size) noexcept
    {
        return attach(&arr[0], buffer_size);
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool attach(pointer* slot_array, const size_type depth, const size_type buffer_size) noexcept
    {
        slots_ = slot_array;
        bufferSize_.store(buffer_size);

        const bool ok = Base::init(depth);
        if (RB_UNLIKELY(!ok) || RB_UNLIKELY(slots_ == nullptr) || RB_UNLIKELY(bufferSize_.load() == 0u)
            || RB_UNLIKELY(Base::capacity() == 0u))
        {
            detach();
            return false;
        }
        return true;
    }

    // Adopt state (restore head/tail). Validates invariants.
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool adopt(pointer* slot_array, const size_type depth, const size_type buffer_size,
                             const size_type initial_head, const size_type initial_tail) noexcept
    {
        slots_ = slot_array;
        bufferSize_.store(buffer_size);

        const bool ok = Base::init(depth, initial_head, initial_tail);
        if (RB_UNLIKELY(!ok) || RB_UNLIKELY(slots_ == nullptr) || RB_UNLIKELY(bufferSize_.load() == 0u)
            || RB_UNLIKELY(Base::capacity() == 0u))
        {
            detach();
            return false;
        }

        const size_type cap  = Base::capacity();
        const size_type used = static_cast<size_type>(Base::head() - Base::tail());
        if (RB_UNLIKELY(used > cap)) {
            detach();
            return false;
        }
        return true;
    }

    template<class Slots,
             size_type C = Capacity,
             typename = std::enable_if_t<
                 (C != 0) &&
                 std::is_pointer_v<std::remove_reference_t<Slots>> &&
                 std::is_same_v<std::remove_cv_t<std::remove_reference_t<Slots>>, pointer*>
                 >>
    [[nodiscard]] bool adopt(Slots&& slot_array, const size_type buffer_size,
                             const size_type initial_head, const size_type initial_tail) noexcept
    {
        if (RB_UNLIKELY(slot_array == nullptr) || RB_UNLIKELY(buffer_size == 0u)) {
            detach();
            return false;
        }

        const size_type cap  = Base::capacity();
        const size_type used = static_cast<size_type>(initial_head - initial_tail);

        if (RB_UNLIKELY(used > cap)) {
            detach();
            return false;
        }

        slots_ = slot_array;
        bufferSize_.store(buffer_size);

        if (RB_UNLIKELY(!Base::init(initial_head, initial_tail))) {
            detach();
            return false;
        }
        return true;
    }

    // Sugar: attach + restore state in one call.
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool attach(pointer* slot_array, const size_type depth, const size_type buffer_size, const state_t st) noexcept
    {
        return adopt(slot_array, depth, buffer_size, st.head, st.tail);
    }

    template<class Slots,
             size_type C = Capacity,
             typename = std::enable_if_t<
                 (C != 0) &&
                 std::is_pointer_v<std::remove_reference_t<Slots>> &&
                 std::is_same_v<std::remove_cv_t<std::remove_reference_t<Slots>>, pointer*>
                 >>
    [[nodiscard]] bool attach(Slots&& slot_array, const size_type buffer_size, const state_t st) noexcept
    {
        return adopt(std::forward<Slots>(slot_array), buffer_size, st.head, st.tail);
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool attach(std::array<pointer, Capacity>& arr, const size_type buffer_size, const state_t st) noexcept
    {
        return attach(arr.data(), buffer_size, st);
    }

    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    [[nodiscard]] bool attach(pointer (&arr)[N], const size_type buffer_size, const state_t st) noexcept
    {
        return attach(&arr[0], buffer_size, st);
    }

    // Reset indices to empty while keeping attachment (if any).
    void reset() noexcept { Base::clear(); }

    // ------------------------------------------------------------------------------------------
    // Access to the ring of pointers (void**)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer const* data() noexcept { return slots_; }
    [[nodiscard]] RB_FORCEINLINE pointer const* data() const noexcept { return slots_; }

    // ------------------------------------------------------------------------------------------
    // Iteration API (Consumer Side Only)
    // ------------------------------------------------------------------------------------------
    iterator begin() noexcept {
        if (RB_UNLIKELY(!is_valid())) { return iterator(nullptr, 0u, 0u); }
        return iterator(data(), Base::mask(), Base::tail());
    }

    iterator end() noexcept {
        if (RB_UNLIKELY(!is_valid())) { return iterator(nullptr, 0u, 0u); }

        // Build end() from a validated used snapshot to avoid impossible head<tail ranges under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

        return iterator(data(), Base::mask(), h);
    }

    const_iterator begin() const noexcept { return cbegin(); }
    const_iterator end()   const noexcept { return cend(); }

    const_iterator cbegin() const noexcept {
        if (RB_UNLIKELY(!is_valid())) { return const_iterator(nullptr, 0u, 0u); }
        return const_iterator(data(), Base::mask(), Base::tail());
    }

    const_iterator cend() const noexcept {
        if (RB_UNLIKELY(!is_valid())) { return const_iterator(nullptr, 0u, 0u); }

        // Build cend() from a validated used snapshot to avoid impossible head<tail ranges under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

        return const_iterator(data(), Base::mask(), h);
    }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    reverse_iterator rend()   noexcept { return reverse_iterator(begin()); }

    const_reverse_iterator rbegin()  const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()    const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    const_reverse_iterator crend()   const noexcept { return const_reverse_iterator(cbegin()); }

    // ------------------------------------------------------------------------------------------
    // Snapshots
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] snapshot make_snapshot() noexcept {
        using it = snapshot_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return snapshot(it(nullptr, 0u, 0u), it(nullptr, 0u, 0u));
        }
        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

        const size_type m = Base::mask();
        return snapshot(it(data(), m, t), it(data(), m, h));
    }

    [[nodiscard]] const_snapshot make_snapshot() const noexcept {
        using it = const_snapshot_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return const_snapshot(it(nullptr, 0u, 0u), it(nullptr, 0u, 0u));
        }
        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

        const size_type m = Base::mask();
        return const_snapshot(it(data(), m, t), it(data(), m, h));
    }

    template<class Snap>
    void consume(const Snap& s) noexcept {
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


    template<class Snap>
    [[nodiscard]] bool try_consume(const Snap& s) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return false; }

        const size_type cap = Base::capacity();

        const size_type snap_tail = static_cast<size_type>(s.tail_index());
        const size_type snap_head = static_cast<size_type>(s.head_index());

        const size_type cur_tail = Base::tail();

        // Snapshot must be from the same buffer (cheap identity check)
        const auto* my_data     = data();
        const size_type my_mask = Base::mask();
        if (RB_UNLIKELY(s.begin().data() != my_data)) { return false; }
        if (RB_UNLIKELY(s.begin().mask() != my_mask)) { return false; }

        if (RB_UNLIKELY(snap_tail != cur_tail)) { return false; }

        const size_type snap_used = static_cast<size_type>(snap_head - snap_tail);
        if (RB_UNLIKELY(snap_used > cap)) { return false; }

        // Validate that the snapshot range is still available to read.
        // can_read() may be conservative under transient observations; do one extra refresh attempt.
        if (RB_UNLIKELY(!Base::can_read(snap_used))) {
            const size_type h2  = static_cast<size_type>(Base::head());
            const size_type av2 = static_cast<size_type>(h2 - cur_tail);
            if (RB_UNLIKELY(av2 < snap_used) || RB_UNLIKELY(av2 > cap)) { return false; }
        }

        pop(snap_used);
        return true;
    }

    void consume_all() noexcept {
        if (RB_UNLIKELY(!is_valid())) { return; }
        Base::sync_tail_to_head();
    }

    // ------------------------------------------------------------------------------------------
    // Bulk / Regions
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] regions
    claim_write(const ::spsc::unsafe_t, const size_type max_count =
                                        std::numeric_limits<size_type>::max()) noexcept {
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

        regions r{};
        auto *const buf = data();

        r.first.ptr = buf + idx;
        r.first.count = first_n;

        r.second.count = second_n;
        r.second.ptr = (second_n != 0u) ? buf : nullptr;

        r.total = total;
        return r;
    }

    [[nodiscard]] regions
    claim_read(const ::spsc::unsafe_t, const size_type max_count =
                                       std::numeric_limits<size_type>::max()) noexcept {
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

        const size_type mask  = Base::mask();
        const size_type idx   = static_cast<size_type>(tail & mask);
        const size_type to_end = static_cast<size_type>(cap - idx);

        const size_type first_n  = (to_end < total) ? to_end : total;
        const size_type second_n = static_cast<size_type>(total - first_n);

        regions r{};
        auto *const buf = data();

        r.first.ptr = buf + idx;
        r.first.count = first_n;

        r.second.count = second_n;
        r.second.ptr = (second_n != 0u) ? buf : nullptr;

        r.total = total;
        return r;
    }

    [[nodiscard]] regions
    claim_write(const size_type max_count = std::numeric_limits<size_type>::max()) noexcept {
        return claim_write(::spsc::unsafe, max_count);
    }

    [[nodiscard]] regions
    claim_read(const size_type max_count = std::numeric_limits<size_type>::max()) noexcept {
        return claim_read(::spsc::unsafe, max_count);
    }

    // ------------------------------------------------------------------------------------------
    // Producer Operations
    // ------------------------------------------------------------------------------------------
    template<class U>
    RB_FORCEINLINE void push(const U& v) noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool_view]: U must be trivially copyable");
        if (RB_UNLIKELY(full())) { SPSC_ASSERT(!full()); return; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { SPSC_ASSERT(sizeof(U) <= bufferSize_.load()); return; }

        pointer dst = slots_[Base::write_index()];
        if (RB_UNLIKELY(dst == nullptr)) { SPSC_ASSERT(dst != nullptr); return; }
        std::memcpy(dst, &v, sizeof(U));
        Base::increment_head();
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE bool try_push(const U& v) noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool_view]: U must be trivially copyable");
        if (RB_UNLIKELY(full())) { return false; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { return false; }

        pointer dst = slots_[Base::write_index()];
        if (RB_UNLIKELY(dst == nullptr)) { return false; }
        std::memcpy(dst, &v, sizeof(U));
        Base::increment_head();
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE pointer claim() noexcept {
        if (RB_UNLIKELY(full())) { SPSC_ASSERT(!full()); return nullptr; }
        pointer p = slots_[Base::write_index()];
        if (RB_UNLIKELY(p == nullptr)) { SPSC_ASSERT(p != nullptr); return nullptr; }
        return p;
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(full())) { return nullptr; }
        pointer p = slots_[Base::write_index()];
        if (RB_UNLIKELY(p == nullptr)) { return nullptr; }
        return p;
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE U* claim_as() noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool_view]: U must be trivially copyable");
        pointer p = try_claim();
        if (RB_UNLIKELY(!p)) { return nullptr; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { return nullptr; }
        if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) != 0u)) { return nullptr; }
        return static_cast<U*>(p);
    }

    template<class U>
    [[nodiscard]] bool try_peek(U& out) const noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool_view]: U must be trivially copyable");
        const_pointer p = try_front();
        if (!p) return false;
        if (sizeof(U) > buffer_size()) return false;

        // memcpy is the only universal "no UB" bridge for raw storage
        std::memcpy(&out, p, sizeof(U));
        return true;
    }

    template<class U>
    [[nodiscard]] bool try_write(const U& v) noexcept {
        return try_push(v);
    }

    RB_FORCEINLINE void publish() noexcept {
        if (RB_UNLIKELY(full())) { SPSC_ASSERT(!full()); return; }
        if (RB_UNLIKELY(slots_[Base::write_index()] == nullptr)) {
            SPSC_ASSERT(slots_[Base::write_index()] != nullptr);
            return;
        }
        Base::increment_head();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish() noexcept {
        if (RB_UNLIKELY(full())) { return false; }
        if (RB_UNLIKELY(slots_[Base::write_index()] == nullptr)) { return false; }
        Base::increment_head();
        return true;
    }

    RB_FORCEINLINE void publish(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_write(n))) { SPSC_ASSERT(can_write(n)); return; }
        const size_type h = static_cast<size_type>(Base::head());
        const size_type m = Base::mask();
        for (size_type i = 0u; i < n; ++i) {
            if (RB_UNLIKELY(slots_[(h + i) & m] == nullptr)) {
                SPSC_ASSERT(slots_[(h + i) & m] != nullptr);
                return;
            }
        }
        Base::advance_head(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_write(n))) { return false; }
        const size_type h = static_cast<size_type>(Base::head());
        const size_type m = Base::mask();
        for (size_type i = 0u; i < n; ++i) {
            if (RB_UNLIKELY(slots_[(h + i) & m] == nullptr)) { return false; }
        }
        Base::advance_head(n);
        return true;
    }

    // --------------------------------------------------------------------------
    // Raw Buffer Push API
    // --------------------------------------------------------------------------
    RB_FORCEINLINE void push(const void* data, const size_type size) noexcept {
        if (RB_UNLIKELY(full())) { SPSC_ASSERT(!full()); return; }

        const size_type bufferSize = bufferSize_.load();
        const size_type copy_n = (size < bufferSize) ? size : bufferSize;
        if (RB_UNLIKELY((copy_n != 0u) && (data == nullptr))) { SPSC_ASSERT((copy_n == 0u) || (data != nullptr)); return; }

        pointer dst = slots_[Base::write_index()];
        if (RB_UNLIKELY(dst == nullptr)) { SPSC_ASSERT(dst != nullptr); return; }
        if (copy_n != 0u) {
            std::memcpy(dst, data, copy_n);
        }
        Base::increment_head();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_push(const void* data, const size_type size) noexcept {
        if (RB_UNLIKELY(full())) { return false; }

        const size_type bufferSize = bufferSize_.load();
        const size_type copy_n = (size < bufferSize) ? size : bufferSize;
        if (RB_UNLIKELY((copy_n != 0u) && (data == nullptr))) { return false; }

        pointer dst = slots_[Base::write_index()];
        if (RB_UNLIKELY(dst == nullptr)) { return false; }
        if (copy_n != 0u) {
            std::memcpy(dst, data, copy_n);
        }
        Base::increment_head();
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Consumer Operations
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer front() noexcept {
        if (RB_UNLIKELY(empty())) { SPSC_ASSERT(!empty()); return nullptr; }
        pointer p = slots_[Base::read_index()];
        if (RB_UNLIKELY(p == nullptr)) { SPSC_ASSERT(p != nullptr); return nullptr; }
        return p;
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer front() const noexcept {
        if (RB_UNLIKELY(empty())) { SPSC_ASSERT(!empty()); return nullptr; }
        const_pointer p = slots_[Base::read_index()];
        if (RB_UNLIKELY(p == nullptr)) { SPSC_ASSERT(p != nullptr); return nullptr; }
        return p;
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(empty())) { return nullptr; }
        pointer p = slots_[Base::read_index()];
        if (RB_UNLIKELY(p == nullptr)) { return nullptr; }
        return p;
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(empty())) { return nullptr; }
        const_pointer p = slots_[Base::read_index()];
        if (RB_UNLIKELY(p == nullptr)) { return nullptr; }
        return p;
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE U* front_as() noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool_view]: U must be trivially copyable");
        pointer p = try_front();
        if (RB_UNLIKELY(!p)) { return nullptr; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { return nullptr; }
        if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) != 0u)) { return nullptr; }
        return static_cast<U*>(p);
    }

    RB_FORCEINLINE void pop() noexcept {
        if (RB_UNLIKELY(empty())) { SPSC_ASSERT(!empty()); return; }
        Base::increment_tail();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(empty())) { return false; }
        Base::increment_tail();
        return true;
    }

    RB_FORCEINLINE void pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) { SPSC_ASSERT(can_read(n)); return; }
        Base::advance_tail(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) { return false; }
        Base::advance_tail(n);
        return true;
    }

    // Prevent accidental overload selection when passing a numeric lvalue:
    //   std::uint32_t n = 3;
    //   p.pop(n);   // would bind to pop(size_type) and pop 3 buffers.
    template<class U,
             typename = std::enable_if_t<
                 !std::is_same_v<std::remove_cv_t<U>, size_type> &&
                 std::is_convertible_v<U, size_type>>>
    void pop(U&) noexcept = delete;

    // Same trap for try_pop(n).
    template<class U,
             typename = std::enable_if_t<
                 !std::is_same_v<std::remove_cv_t<U>, size_type> &&
                 std::is_convertible_v<U, size_type>>>
    [[nodiscard]] bool try_pop(U&) noexcept = delete;

    [[nodiscard]] RB_FORCEINLINE pointer operator[](const size_type i) noexcept {
        if (RB_UNLIKELY(i >= size())) { SPSC_ASSERT(i < size()); return nullptr; }
        const size_type idx = static_cast<size_type>((Base::tail() + i) & Base::mask());
        return slots_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer operator[](const size_type i) const noexcept {
        if (RB_UNLIKELY(i >= size())) { SPSC_ASSERT(i < size()); return nullptr; }
        const size_type idx = static_cast<size_type>((Base::tail() + i) & Base::mask());
        return slots_[idx];
    }

#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<std::byte> span() noexcept {
        pointer p = try_front();
        if (!p) { return {}; }
        return std::span<std::byte>(static_cast<std::byte*>(p), bufferSize_.load());
    }

    [[nodiscard]] std::span<const std::byte> span() const noexcept {
        const_pointer p = try_front();
        if (!p) { return {}; }
        return std::span<const std::byte>(static_cast<const std::byte*>(p), bufferSize_.load());
    }
#endif /* SPSC_HAS_SPAN */
    // ------------------------------------------------------------------------------------------
    // RAII Based API
    // ------------------------------------------------------------------------------------------

    class bulk_write_guard {
    public:
        bulk_write_guard() noexcept = default;

        explicit bulk_write_guard(pool_view& p,
                                  const size_type max_count = static_cast<size_type>(~size_type(0))) noexcept
            : p_(&p), regs_(p.claim_write(::spsc::unsafe, max_count)) {
            if (regs_.total == 0u) {
                p_ = nullptr;
            }
        }

        bulk_write_guard(const bulk_write_guard&) = delete;
        bulk_write_guard& operator=(const bulk_write_guard&) = delete;

        bulk_write_guard(bulk_write_guard&& other) noexcept
            : p_(other.p_), regs_(other.regs_), written_(other.written_),
              publish_on_destroy_(other.publish_on_destroy_) {
            other.p_ = nullptr;
            other.regs_ = {};
            other.written_ = 0u;
            other.publish_on_destroy_ = false;
        }

        bulk_write_guard& operator=(bulk_write_guard&&) = delete;

        ~bulk_write_guard() noexcept {
            if (p_ != nullptr && written_ != 0u && publish_on_destroy_) {
                p_->publish(written_);
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return (p_ != nullptr) && (regs_.total != 0u);
        }

        [[nodiscard]] size_type claimed() const noexcept { return regs_.total; }
        [[nodiscard]] size_type constructed() const noexcept { return written_; }
        [[nodiscard]] size_type remaining() const noexcept {
            return static_cast<size_type>(regs_.total - written_);
        }

        [[nodiscard]] pointer peek_next() const noexcept {
            if (RB_UNLIKELY(p_ == nullptr)) { SPSC_ASSERT(p_ != nullptr); return nullptr; }
            if (RB_UNLIKELY(written_ >= regs_.total)) { SPSC_ASSERT(written_ < regs_.total); return nullptr; }
            return slot_ptr_at_(written_);
        }

        [[nodiscard]] pointer get_next() const noexcept { return peek_next(); }

        template<class U>
        [[nodiscard]] pointer emplace_next(const U& v) noexcept {
            return write_next(v);
        }

        template<class U>
        [[nodiscard]] pointer write_next(const U& v) noexcept {
            static_assert(std::is_trivially_copyable_v<U>, "[pool_view::bulk_write_guard]: U must be trivially copyable");

            if (RB_UNLIKELY(p_ == nullptr)) { SPSC_ASSERT(p_ != nullptr); return nullptr; }
            if (RB_UNLIKELY(written_ >= regs_.total)) { SPSC_ASSERT(written_ < regs_.total); return nullptr; }
            SPSC_ASSERT(sizeof(U) <= p_->buffer_size());
            if (RB_UNLIKELY(sizeof(U) > p_->buffer_size())) { return nullptr; }

            pointer dst = slot_ptr_at_(written_);
            if (RB_UNLIKELY(dst == nullptr)) { return nullptr; }
            std::memcpy(dst, &v, sizeof(U));
            ++written_;
            publish_on_destroy_ = true;
            return dst;
        }

        [[nodiscard]] pointer write_next(const void* src, const size_type size) noexcept {
            if (RB_UNLIKELY(p_ == nullptr)) { SPSC_ASSERT(p_ != nullptr); return nullptr; }
            if (RB_UNLIKELY(written_ >= regs_.total)) { SPSC_ASSERT(written_ < regs_.total); return nullptr; }

            pointer dst = slot_ptr_at_(written_);
            if (RB_UNLIKELY(dst == nullptr)) { return nullptr; }
            const size_type bs = p_->buffer_size();
            const size_type n = (size < bs) ? size : bs;
            SPSC_ASSERT((n == 0u) || (src != nullptr));
            if (RB_UNLIKELY((n != 0u) && (src == nullptr))) { return nullptr; }
            if (n != 0u) {
                std::memcpy(dst, src, n);
            }
            ++written_;
            publish_on_destroy_ = true;
            return dst;
        }

        void mark_written() noexcept {
            if (RB_UNLIKELY(p_ == nullptr)) { SPSC_ASSERT(p_ != nullptr); return; }
            if (RB_UNLIKELY(written_ >= regs_.total)) { SPSC_ASSERT(written_ < regs_.total); return; }
            ++written_;
        }

        void arm_publish() noexcept {
            if (RB_UNLIKELY(p_ == nullptr)) { SPSC_ASSERT(p_ != nullptr); return; }
            SPSC_ASSERT(written_ != 0u && "arm_publish() requires at least one written element");
            if (RB_UNLIKELY(written_ == 0u)) { return; }
            publish_on_destroy_ = true;
        }

        void publish_on_destroy() noexcept { arm_publish(); }

        void disarm_publish() noexcept { publish_on_destroy_ = false; }

        void commit() noexcept {
            if (p_ != nullptr && written_ != 0u) {
                p_->publish(written_);
            }
            reset_();
        }

        void cancel() noexcept { reset_(); }

    private:
        [[nodiscard]] pointer slot_ptr_at_(const size_type i) const noexcept {
            SPSC_ASSERT(i < regs_.total);
            if (RB_UNLIKELY(i >= regs_.total)) { return nullptr; }
            if (i < regs_.first.count) {
                return (regs_.first.ptr != nullptr) ? regs_.first.ptr[i] : nullptr;
            }
            return (regs_.second.ptr != nullptr) ? regs_.second.ptr[i - regs_.first.count] : nullptr;
        }

        void reset_() noexcept {
            p_ = nullptr;
            regs_ = {};
            written_ = 0u;
            publish_on_destroy_ = false;
        }

        pool_view* p_{nullptr};
        regions regs_{};
        size_type written_{0u};
        bool publish_on_destroy_{false};
    };

    class bulk_read_guard {
    public:
        class region_view {
        public:
            region_view() noexcept = default;
            constexpr region_view(pointer const* slts, const size_type count) noexcept
                : slts_(slts), count_(count) {}

            [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0u; }
            [[nodiscard]] constexpr size_type size() const noexcept { return count_; }

            [[nodiscard]] pointer ptr(const size_type i) const noexcept {
                SPSC_ASSERT(i < count_);
                if (RB_UNLIKELY(i >= count_) || RB_UNLIKELY(slts_ == nullptr)) { return nullptr; }
                return slts_[i];
            }

            [[nodiscard]] pointer operator[](const size_type i) const noexcept { return ptr(i); }

#if SPSC_HAS_SPAN
            [[nodiscard]] std::span<pointer const> raw_span() const noexcept {
                return {slts_, static_cast<std::size_t>(count_)};
            }
#endif /* SPSC_HAS_SPAN */

        private:
            pointer const* slts_{nullptr};
            size_type count_{0u};
        };

        bulk_read_guard() noexcept = default;

        explicit bulk_read_guard(pool_view& p,
                                 const size_type max_count = static_cast<size_type>(~size_type(0))) noexcept
            : p_(&p), regs_(p.claim_read(::spsc::unsafe, max_count)), active_(regs_.total != 0u) {
            if (!active_) {
                p_ = nullptr;
            }
        }

        bulk_read_guard(const bulk_read_guard&) = delete;
        bulk_read_guard& operator=(const bulk_read_guard&) = delete;

        bulk_read_guard(bulk_read_guard&& other) noexcept
            : p_(other.p_), regs_(other.regs_), active_(other.active_) {
            other.p_ = nullptr;
            other.regs_ = {};
            other.active_ = false;
        }

        bulk_read_guard& operator=(bulk_read_guard&&) = delete;

        ~bulk_read_guard() noexcept {
            if (active_ && p_) {
                p_->pop(regs_.total);
            }
        }

        [[nodiscard]] explicit operator bool() const noexcept { return active_; }

        [[nodiscard]] size_type count() const noexcept { return regs_.total; }
        [[nodiscard]] const regions& regions_view() const noexcept { return regs_; }

        [[nodiscard]] region_view first() const noexcept {
            return region_view(regs_.first.ptr, regs_.first.count);
        }
        [[nodiscard]] region_view second() const noexcept {
            return region_view(regs_.second.ptr, regs_.second.count);
        }

        void commit() noexcept {
            if (active_ && p_) {
                p_->pop(regs_.total);
            }
            cancel();
        }

        void cancel() noexcept {
            active_ = false;
            p_ = nullptr;
            regs_ = {};
        }

    private:
        pool_view* p_{nullptr};
        regions regs_{};
        bool active_{false};
    };

    class write_guard {
    public:
        write_guard() noexcept = default;

        explicit write_guard(pool_view& p) noexcept
            : p_(&p), ptr_(p.try_claim()) {}

        write_guard(const write_guard&)            = delete;
        write_guard& operator=(const write_guard&) = delete;

        write_guard(write_guard&& other) noexcept
            : p_(other.p_), ptr_(other.ptr_), publish_on_destroy_(other.publish_on_destroy_) {
            other.p_ = nullptr;
            other.ptr_ = nullptr;
            other.publish_on_destroy_ = false;
        }

        write_guard& operator=(write_guard&&) = delete;

        ~write_guard() noexcept {
            if (p_ && ptr_ && publish_on_destroy_) {
                p_->publish();
            }
        }

        // Raw access does not arm publishing.
        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        [[nodiscard]] pointer peek() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return (p_ != nullptr) && (ptr_ != nullptr); }

        template<class U>
        [[nodiscard]] U* as() const noexcept {
            if (RB_UNLIKELY(!ptr_ || !p_)) { return nullptr; }
            static_assert(std::is_trivially_copyable_v<U>, "[pool_view::guard]: U must be trivially copyable");
            if (RB_UNLIKELY(sizeof(U) > p_->buffer_size())) { return nullptr; }
            if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(ptr_) % alignof(U)) != 0u)) { return nullptr; }
            // If typed view is usable, assume caller intends to write.
            publish_on_destroy_ = true;
            return static_cast<U*>(ptr_);
        }

        // Call this only after you fully populated the claimed slot.
        void publish_on_destroy() noexcept {
            SPSC_ASSERT(p_ && ptr_);
            publish_on_destroy_ = true;
        }

        void arm_publish() noexcept { publish_on_destroy(); }

        void disarm_publish() noexcept { publish_on_destroy_ = false; }

        void commit() noexcept {
            if (p_ && ptr_) {
                p_->publish();
            }
            cancel();
        }

        void cancel() noexcept {
            publish_on_destroy_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        pool_view* p_{nullptr};
        pointer    ptr_{nullptr};
        mutable bool publish_on_destroy_{false};
    };

    class read_guard {
    public:
        read_guard() noexcept = default;

        explicit read_guard(pool_view& p) noexcept
            : p_(&p), ptr_(p.try_front()), active_(ptr_ != nullptr) {}

        read_guard(const read_guard&)            = delete;
        read_guard& operator=(const read_guard&) = delete;

        read_guard(read_guard&& other) noexcept
            : p_(other.p_), ptr_(other.ptr_), active_(other.active_)
        {
            other.p_ = nullptr;
            other.ptr_ = nullptr;
            other.active_ = false;
        }

        read_guard& operator=(read_guard&&) = delete;

        ~read_guard() noexcept {
            if (active_ && p_) { p_->pop(); }
        }

        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return active_; }

        template<class U>
        [[nodiscard]] U* as() const noexcept {
            if (RB_UNLIKELY(!ptr_ || !p_)) { return nullptr; }
            static_assert(std::is_trivially_copyable_v<U>, "[pool_view::guard]: U must be trivially copyable");
            if (RB_UNLIKELY(sizeof(U) > p_->buffer_size())) { return nullptr; }
            if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(ptr_) % alignof(U)) != 0u)) { return nullptr; }
            return static_cast<U*>(ptr_);
        }

        void commit() noexcept {
            if (active_ && p_) { p_->pop(); }
            active_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

        void cancel() noexcept {
            active_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        pool_view* p_{nullptr};
        pointer    ptr_{nullptr};
        bool       active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept { return write_guard(*this); }
    [[nodiscard]] bulk_write_guard scoped_write(const size_type max_count) noexcept {
        return bulk_write_guard(*this, max_count);
    }
    [[nodiscard]] read_guard  scoped_read()  noexcept { return read_guard(*this); }
    [[nodiscard]] bulk_read_guard scoped_read(const size_type max_count) noexcept {
        return bulk_read_guard(*this, max_count);
    }

private:
    void move_from(pool_view&& other) noexcept(kNoThrowMoveOps)
    {
        if constexpr (kDynamic) {
            const size_type cap  = other.Base::capacity();
            const size_type head = other.Base::head();
            const size_type tail = other.Base::tail();
            pointer* ptr         = other.slots_;
            const size_type bs   = other.bufferSize_.load();

            const bool detached = (ptr == nullptr) || (bs == 0u) || (cap == 0u);

            if (RB_UNLIKELY(detached)) {
                slots_ = nullptr;
                bufferSize_.store(0u);
                (void)Base::init(0u);
            } else {
                // Defensive: moved-from source must satisfy dynamic invariant
                // (ptr, buffer_size, capacity) are either all zero/empty or all present.
                if (RB_UNLIKELY(((ptr == nullptr) != (cap == 0u)) ||
                                ((ptr == nullptr) != (bs  == 0u)))) {
                    slots_ = nullptr;
                    bufferSize_.store(0u);
                    (void)Base::init(0u);
                    other.detach();
                    return;
                }

                const bool ok = Base::init(cap, head, tail);
                if (RB_UNLIKELY(!ok)) {
                    slots_ = nullptr;
                    bufferSize_.store(0u);
                    (void)Base::init(0u);
                    other.detach();
                    return;
                }

                slots_ = ptr;
                bufferSize_.store(bs);
            }

            other.detach();
        } else {
            const size_type obs = other.bufferSize_.load();
            slots_ = other.slots_;
            bufferSize_.store(obs);

            // Defensive normalization: static view is valid only with both slot table and non-zero buffer_size.
            if (RB_UNLIKELY(slots_ == nullptr || obs == 0u)) {
                slots_ = nullptr;
                bufferSize_.store(0u);
                Base::clear();
            } else {
                // Non-concurrent state transfer: must synchronize shadows too (if enabled).
                const bool ok = Base::init(other.Base::head(), other.Base::tail());
                if (RB_UNLIKELY(!ok)) {
                    // If source counters are corrupted, degrade safely to detached invalid.
                    slots_ = nullptr;
                    bufferSize_.store(0u);
                    Base::clear();
                }
                SPSC_ASSERT(ok);
            }

            other.slots_ = nullptr;
            other.bufferSize_.store(0u);
            other.Base::clear();
        }
    }

private:
    pointer* slots_{nullptr};
    geometry_type  bufferSize_{};
};

// ------------------------------------------------------------------------
// Deduction Guides
// ------------------------------------------------------------------------
pool_view(void**, reg, reg) -> pool_view<0u>;

template<class P>
pool_view(void**, reg, reg, P) -> pool_view<0u, P>;

template<reg N>
pool_view(void* (&)[N], reg) -> pool_view<N>;

template<reg N, class P>
pool_view(void* (&)[N], reg, P) -> pool_view<N, P>;

template<reg N>
pool_view(std::array<void*, N>&, reg) -> pool_view<N>;

template<reg N, class P>
pool_view(std::array<void*, N>&, reg, P) -> pool_view<N, P>;

} // namespace spsc

#endif /* SPSC_POOL_VIEW_HPP_ */
