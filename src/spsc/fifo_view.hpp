/*
 * fifo_view.hpp
 *
 * Non-owning SPSC FIFO (owning-free) with the same public API shape and hot-path logic
 * as spsc::fifo, but operating on user-provided storage.
 *
 * Notes:
 * - This type owns ONLY indices/geometry (SPSCbase) and a raw pointer to storage.
 * - It does NOT allocate and does NOT destroy the underlying buffer.
 * - Copy is deleted: copying would duplicate head/tail state while aliasing the same storage (foot-gun).
 */

#ifndef SPSC_FIFO_VIEW_HPP_
#define SPSC_FIFO_VIEW_HPP_

#include <array>
#include <cstddef>  // std::size_t
#include <cstdint>  // std::uintptr_t      // std::ptrdiff_t
#include <iterator>     // std::reverse_iterator
#include <limits>
#include <type_traits>
#include <utility>      // std::swap, std::move

// Base and utility includes
#include "base/SPSCbase.hpp"        // ::spsc::SPSCbase<Capacity, Policy>, reg
#include "base/spsc_snapshot.hpp"   // ::spsc::snapshot_view, ::spsc::snapshot_traits
#include "base/spsc_tools.hpp"      // RB_FORCEINLINE, RB_UNLIKELY, macros

namespace spsc {


/* =======================================================================
 * fifo_view<T, Capacity, Policy>
 *
 * Non-owning Single-Producer Single-Consumer ring buffer.
 * Can be static (Capacity != 0) or dynamic (Capacity == 0).
 * ======================================================================= */
template<
    class T,
    reg Capacity    = 0,
    typename Policy = ::spsc::policy::default_policy
    >
class fifo_view : private ::spsc::SPSCbase<Capacity, Policy>
{
    static constexpr bool kDynamic = (Capacity == 0);

    using Base         = ::spsc::SPSCbase<Capacity, Policy>;
    using storage_type = T*;

    static constexpr bool kNoThrowMoveOps = true;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type               = T;
    using pointer                  = value_type*;
    using const_pointer            = const value_type*;
    using reference                = value_type&;
    using const_reference          = const value_type&;

    using size_type                = reg;
    using difference_type          = std::ptrdiff_t;

    // Iterator types
    // Battle-mode: iterators are read-only (same as spsc::queue).
    using iterator = ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type, false>;
    using const_iterator =
        ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type, true>;
    using reverse_iterator         = std::reverse_iterator<iterator>;
    using const_reverse_iterator   = std::reverse_iterator<const_iterator>;

private:
    static constexpr std::size_t kValueAlign = alignof(value_type);

    static RB_FORCEINLINE bool is_storage_aligned(const void* p) noexcept {
        if (p == nullptr) {
            return false;
        }
        if constexpr (kValueAlign <= 1u) {
            return true;
        } else {
            static_assert((kValueAlign & (kValueAlign - 1u)) == 0u, "value_type alignment must be power-of-two");
            return (reinterpret_cast<std::uintptr_t>(p) & (kValueAlign - 1u)) == 0u;
        }
    }

public:


    // Snapshot types
    // Battle-mode: snapshots are read-only (same as spsc::queue).
    using snapshot_traits         		= ::spsc::snapshot_traits<std::add_const_t<value_type>, size_type>;
    using snapshot                		= typename snapshot_traits::snapshot;
    using const_snapshot        		= typename snapshot_traits::const_snapshot;
    using snapshot_iterator    			= typename snapshot_traits::iterator;
    using const_snapshot_iterator   	= typename snapshot_traits::const_iterator;

    // Policy types
    using policy_type              = Policy;
    using counter_type             = typename Policy::counter_type;
    using geometry_type            = typename Policy::geometry_type;
    using counter_value            = typename counter_type::value_type;
    using geometry_value           = typename geometry_type::value_type;

    // ------------------------------------------------------------------------------------------
    // Static Assertions
    // ------------------------------------------------------------------------------------------
    static_assert(std::is_default_constructible_v<value_type>,
                  "[spsc::fifo_view]: value_type must be default-constructible.");
    static_assert(!std::is_const_v<value_type>,
                  "[spsc::fifo_view]: const T does not make sense for a writable FIFO.");

#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<value_type>,
                  "[spsc::fifo_view]: no-exceptions mode requires noexcept default constructor.");
    static_assert(std::is_nothrow_destructible_v<value_type>,
                  "[spsc::fifo_view]: no-exceptions mode requires noexcept destructor.");
    static_assert(std::is_nothrow_move_assignable_v<value_type> || std::is_nothrow_copy_assignable_v<value_type>,
                  "[spsc::fifo_view]: no-exceptions mode requires noexcept assignment (move or copy).");
#endif /* (SPSC_ENABLE_EXCEPTIONS == 0) */

    static_assert(std::is_trivially_copyable_v<counter_value>,
                  "[spsc::fifo_view]: counter_value must be trivially copyable (atomic-friendly).");
    static_assert(Capacity == 0 || ::spsc::cap::rb_is_pow2(Capacity),
                  "[spsc::fifo_view]: static Capacity must be power-of-two (mask-based indexing).");
    static_assert(Capacity == 0 || Capacity >= 2,
                  "[spsc::fifo_view]: Capacity must be >= 2 (or 0 for dynamic).");
    static_assert(std::is_move_assignable_v<value_type> || std::is_copy_assignable_v<value_type>,
                  "[spsc::fifo_view]: value_type must be move- or copy-assignable.");
    static_assert(std::numeric_limits<counter_value>::digits >= 2,
                  "[spsc::fifo_view]: counter type is too narrow.");
    static_assert(::spsc::cap::RB_MAX_UNAMBIGUOUS <= (counter_value(1) << (std::numeric_limits<counter_value>::digits - 1)),
                  "[spsc::fifo_view]: RB_MAX_UNAMBIGUOUS exceeds counter unambiguous range.");
    static_assert(std::is_same_v<counter_value, geometry_value>,
                  "[spsc::fifo_view]: policy counter/geometry value types must match.");
    static_assert(std::is_unsigned_v<counter_value>,
                  "[spsc::fifo_view]: policy counter/geometry value type must be unsigned.");
    static_assert(sizeof(counter_value) >= sizeof(size_type),
                  "[spsc::fifo_view]: counter_type::value_type must be at least as wide as reg.");

    // ------------------------------------------------------------------------------------------
    // Region Structs (Bulk Operations)
    // ------------------------------------------------------------------------------------------
    struct region {
        pointer   ptr{nullptr};
        size_type count{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }
#if SPSC_HAS_SPAN
        [[nodiscard]] std::span<value_type> span() const noexcept { return {ptr, count}; }
#endif /* SPSC_HAS_SPAN */
    };

    struct regions {
        region    first{};
        region    second{};
        size_type total{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return total == 0u; }
    };


    // ------------------------------------------------------------------------
    // Serializable head/tail state for IPC / Recovery / Debug.
    // Use with attach(..., state_t{...}) or state() accessor.
    // ------------------------------------------------------------------------
    struct state_t {
        size_type head{0u};
        size_type tail{0u};
    };


    // Constructors / Destructor
    // ------------------------------------------------------------------------------------------

    // Default constructor: invalid until storage is attached.
    fifo_view() = default;

    // [Static] Attach to external buffer (Capacity != 0).
    // Forwarding-ref prevents array-to-pointer decay from hijacking this overload.
    template<
        class Buf,
        size_type C = Capacity,
        typename = std::enable_if_t<
            C != 0 &&
            std::is_convertible_v<Buf, pointer> &&
            !std::is_array_v<std::remove_reference_t<Buf>>
            >
        >
    explicit fifo_view(Buf&& buffer) noexcept
        : Base(), storage_(static_cast<pointer>(buffer))
    {
        if (RB_UNLIKELY(!is_storage_aligned(storage_))) {
            storage_ = nullptr;
            return;
        }
        Base::clear();
    }

    // [Static] Attach to external buffer with Policy tag (for CTAD convenience).
    template<
        class Buf,
        size_type C = Capacity,
        typename = std::enable_if_t<
            C != 0 &&
            std::is_convertible_v<Buf, pointer> &&
            !std::is_array_v<std::remove_reference_t<Buf>>
            >
        >
    explicit fifo_view(Buf&& buffer, Policy) noexcept
        : fifo_view(std::forward<Buf>(buffer))
    {}

    // [Static] Attach via std::array.
    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    explicit fifo_view(std::array<T, Capacity>& arr) noexcept
        : Base(), storage_(arr.data())
    {
        if (RB_UNLIKELY(!is_storage_aligned(storage_))) {
            storage_ = nullptr;
            return;
        }
        Base::clear();
    }

    // [Static] Attach via std::array with Policy tag (for CTAD convenience).
    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    explicit fifo_view(std::array<T, Capacity>& arr, Policy) noexcept
        : fifo_view(arr)
    {}

    // [Static] Attach to raw array reference.
    // N is a function template parameter to avoid forming T(&)[0] when Capacity == 0 under -Wpedantic.
    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    explicit fifo_view(T (&buffer)[N]) noexcept
        : Base(), storage_(buffer)
    {
        Base::clear();
    }

    // [Static] Attach to raw array reference with Policy tag (for CTAD convenience).
    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    explicit fifo_view(T (&buffer)[N], Policy) noexcept
        : fifo_view(buffer)
    {}

    // [Dynamic] Attach to external buffer with runtime capacity (Capacity == 0).
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    fifo_view(pointer buffer, const size_type buffer_capacity) noexcept
        : Base(), storage_(buffer)
    {
        const bool ok = Base::init(buffer_capacity);
        if (RB_UNLIKELY(!ok) || RB_UNLIKELY(!is_storage_aligned(storage_)) || RB_UNLIKELY(Base::capacity() == 0u)) {
            storage_ = nullptr;
            (void)Base::init(0u);
        }
    }

    // [Dynamic] Attach with Policy tag (for CTAD convenience).
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    fifo_view(pointer buffer, const size_type buffer_capacity, Policy) noexcept
        : fifo_view(buffer, buffer_capacity)
    {}

    ~fifo_view() noexcept = default;

    // Copy is deleted: it would duplicate indices while aliasing the same storage.
    fifo_view(const fifo_view&)            = delete;
    fifo_view& operator=(const fifo_view&) = delete;

    // Move semantics
    fifo_view(fifo_view&& other) noexcept(kNoThrowMoveOps) { move_from(std::move(other)); }

    fifo_view& operator=(fifo_view&& other) noexcept(kNoThrowMoveOps) {
        if (this != &other) {
            detach();
            move_from(std::move(other));
        }
        return *this;
    }

    void swap(fifo_view& other) noexcept
    {
        if (this == &other) { return; }

        if constexpr (kDynamic) {
            const size_type a_cap  = Base::capacity();
            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();

            const size_type b_cap  = other.Base::capacity();
            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();

            SPSC_ASSERT((storage_ == nullptr) == (a_cap == 0u));
            SPSC_ASSERT((other.storage_ == nullptr) == (b_cap == 0u));

            std::swap(storage_, other.storage_);

            const bool ok1 = (b_cap != 0u) ? Base::init(b_cap, b_head, b_tail)
                                           : Base::init(0u);

            const bool ok2 = (a_cap != 0u) ? other.Base::init(a_cap, a_head, a_tail)
                                           : other.Base::init(0u);


            if (RB_UNLIKELY(!ok1 || !ok2)) {
                std::swap(storage_, other.storage_);
                const bool rb1 = (a_cap != 0u) ? Base::init(a_cap, a_head, a_tail)
                                               : Base::init(0u);

                const bool rb2 = (b_cap != 0u) ? other.Base::init(b_cap, b_head, b_tail)
                                               : other.Base::init(0u);

                SPSC_ASSERT(rb1 && rb2);
                (void)rb1; (void)rb2;
            }

            // Keep invariant: detached dynamic view has zero geometry.
            if (storage_ == nullptr) { (void)Base::init(0u); }
            if (other.storage_ == nullptr) { (void)other.Base::init(0u); }
        } else {
            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();
            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();

            std::swap(storage_, other.storage_);

            if (storage_ != nullptr) {
                const bool ok = Base::init(b_head, b_tail);
                SPSC_ASSERT(ok);
                (void)ok;
            } else {
                // Keep invariant: detached static view has cleared indices (and shadows).
                Base::clear();
            }

            if (other.storage_ != nullptr) {
                const bool ok = other.Base::init(a_head, a_tail);
                SPSC_ASSERT(ok);
                (void)ok;
            } else {
                other.Base::clear();
            }
        }
    }


    friend void swap(fifo_view& a, fifo_view& b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        if constexpr (kDynamic) {
            // In dynamic mode, capacity 0 means disabled queue.
            return (storage_ != nullptr) && (Base::capacity() != 0u);
        } else {
            // Static view is valid only when a backing buffer is attached.
            return (storage_ != nullptr);
        }
    }

    [[nodiscard]] state_t state() const noexcept
    {
        if (RB_UNLIKELY(!is_valid())) { return {}; }
        return state_t{Base::head(), Base::tail()};
    }


    [[nodiscard]] size_type capacity() const noexcept { return is_valid() ? Base::capacity() : 0u; }
    [[nodiscard]] size_type size()     const noexcept { return is_valid() ? Base::size() : 0u; }
    [[nodiscard]] bool      empty()    const noexcept { return !is_valid() || Base::empty(); }
    [[nodiscard]] bool      full()     const noexcept { return !is_valid() || Base::full(); }
    [[nodiscard]] size_type free()     const noexcept { return is_valid() ? Base::free() : 0u; }

    [[nodiscard]] bool can_write(size_type n = 1u) const noexcept { return is_valid() && Base::can_write(n); }
    [[nodiscard]] bool can_read (size_type n = 1u) const noexcept { return is_valid() && Base::can_read(n); }

    [[nodiscard]] size_type write_size() const noexcept { return is_valid() ? Base::write_size() : 0u; }
    [[nodiscard]] size_type read_size()  const noexcept { return is_valid() ? Base::read_size()  : 0u; }

    void clear() noexcept {
        // Not concurrent with producer/consumer ops.
        Base::clear();
    }

    // Explicit detach: makes the view invalid (does not touch external storage).
    void detach() noexcept {
        storage_ = nullptr;
        if constexpr (kDynamic) {
            (void)Base::init(0u);
        } else {
            Base::clear();
        }
    }

    // Attach / Reset API (no ctor needed)
    // NOTE: These are NOT concurrent with producer/consumer ops.

    template<size_type C = Capacity, typename = std::enable_if_t<(C != 0u)>>
    [[nodiscard]] bool attach(pointer buffer) noexcept
    {
        if (RB_UNLIKELY(!is_storage_aligned(buffer))) { detach(); return false; }
        storage_ = buffer;
        Base::clear();
        return true;
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool attach(std::array<value_type, Capacity>& arr) noexcept
    {
        return attach(arr.data());
    }

    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    [[nodiscard]] bool attach(value_type (&arr)[N]) noexcept
    {
        return attach(&arr[0]);
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool attach(pointer buffer, const size_type buffer_capacity) noexcept
    {
        storage_ = buffer;
        const bool ok = Base::init(buffer_capacity);
        if (RB_UNLIKELY(!ok) || RB_UNLIKELY(!is_storage_aligned(storage_)) || RB_UNLIKELY(Base::capacity() == 0u)) {
            detach();
            return false;
        }
        return true;
    }

    // Restore head/tail from external source (IPC / recovery). Validates invariants.
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool adopt(pointer buffer, const size_type buffer_capacity,
                             const size_type initial_head, const size_type initial_tail) noexcept
    {
        storage_ = buffer;
        const bool ok = Base::init(buffer_capacity, initial_head, initial_tail);
        if (RB_UNLIKELY(!ok) || RB_UNLIKELY(!is_storage_aligned(storage_)) || RB_UNLIKELY(Base::capacity() == 0u)) {
            detach();
            return false;
        }

        // Extra paranoia: ensure "used <= capacity" after adoption.
        const size_type cap  = Base::capacity();
        const size_type used = static_cast<size_type>(Base::head() - Base::tail());
        if (RB_UNLIKELY(used > cap)) {
            detach();
            return false;
        }
        return true;
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool adopt(pointer buffer,
                             const size_type initial_head, const size_type initial_tail) noexcept
    {
        if (RB_UNLIKELY(!is_storage_aligned(buffer))) { detach(); return false; }
        storage_ = buffer;

        const bool ok = Base::init(initial_head, initial_tail);
        if (RB_UNLIKELY(!ok)) {
            detach();
            return false;
        }
        return true;
    }


    // Convenience: attach + restore state in one call (sugar over adopt()).
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool attach(pointer buffer, const size_type buffer_capacity, const state_t st) noexcept
    {
        return adopt(buffer, buffer_capacity, st.head, st.tail);
    }

    [[nodiscard]] bool attach(pointer buffer, const state_t st) noexcept
    {
        if constexpr (Capacity == 0u) {
            // Dynamic view requires an explicit capacity.
            (void)buffer;
            (void)st;
            detach();
            return false;
        } else {
            return adopt(buffer, st.head, st.tail);
        }
    }

    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool attach(std::array<value_type, Capacity>& arr, const state_t st) noexcept
    {
        return attach(arr.data(), st);
    }

    template<reg N, size_type C = Capacity, typename = std::enable_if_t<(C != 0u) && (N == C)>>
    [[nodiscard]] bool attach(value_type (&arr)[N], const state_t st) noexcept
    {
        return attach(&arr[0], st);
    }

    // Reset indices to empty while keeping the attachment (if any).
    void reset() noexcept
    {
        Base::clear();
    }

    // ------------------------------------------------------------------------------------------
    // Iteration API (Consumer Side Only)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] pointer data() noexcept { return storage_; }
    [[nodiscard]] const_pointer data() const noexcept { return storage_; }

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

    const_iterator begin()  const noexcept { return cbegin(); }
    const_iterator end()    const noexcept { return cend(); }

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

    reverse_iterator rbegin()              noexcept { return reverse_iterator(end()); }
    reverse_iterator rend()                noexcept { return reverse_iterator(begin()); }
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

    // Consume exactly s.size() elements from snapshot
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

    // Try to consume s.size(), ensuring consumer hasn't moved since snapshot
    template<class Snap>
    [[nodiscard]] bool try_consume(const Snap& s) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return false; }

        const size_type cap = Base::capacity();

        const size_type snap_tail = static_cast<size_type>(s.tail_index());
        const size_type snap_head = static_cast<size_type>(s.head_index());

        const size_type cur_tail  = static_cast<size_type>(Base::tail());

        // Snapshot must be from the same buffer (cheap identity check)
        const auto* my_data     = data();
        const size_type my_mask = Base::mask();
        if (RB_UNLIKELY(s.begin().data() != my_data)) { return false; }
        if (RB_UNLIKELY(s.begin().mask() != my_mask)) { return false; }

        // Consumer must not have advanced since snapshot.
        if (RB_UNLIKELY(snap_tail != cur_tail)) { return false; }

        // Snapshot integrity: used <= capacity (wrap-safe via subtraction)
        const size_type snap_used = static_cast<size_type>(snap_head - snap_tail);
        if (RB_UNLIKELY(snap_used > cap)) { return false; }

        // Validate that the snapshot range is still available to read.
        // can_read() is allowed to be conservative on transient/invalid observations;
        // do one extra refresh attempt via a direct head reload to reduce spurious failures.
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
    [[nodiscard]] regions claim_write(const size_type max_count = std::numeric_limits<size_type>::max()) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return {}; }

        const size_type cap = Base::capacity();

        size_type total = static_cast<size_type>(Base::free());
        if (max_count < total) { total = max_count; }
        if (RB_UNLIKELY(total == 0u)) { return {}; }

        const size_type wi      = static_cast<size_type>(Base::write_index()); // masked
        const size_type w2e     = static_cast<size_type>(cap - wi);
        const size_type first_n = (w2e < total) ? w2e : total;

        regions r{};
        auto* const buf = data();

        r.first.ptr   = buf + wi;
        r.first.count = first_n;

        r.second.count = static_cast<size_type>(total - first_n);
        r.second.ptr   = (r.second.count != 0u) ? buf : nullptr;

        r.total = total;
        return r;
    }

    [[nodiscard]] regions claim_read(const size_type max_count = std::numeric_limits<size_type>::max()) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return {}; }

        const size_type cap = Base::capacity();

        size_type total = static_cast<size_type>(Base::size());
        if (max_count < total) { total = max_count; }
        if (RB_UNLIKELY(total == 0u)) { return {}; }

        const size_type ri      = static_cast<size_type>(Base::read_index()); // masked
        const size_type r2e     = static_cast<size_type>(cap - ri);
        const size_type first_n = (r2e < total) ? r2e : total;

        regions r{};
        auto* const buf = data();

        r.first.ptr   = buf + ri;
        r.first.count = first_n;

        r.second.count = static_cast<size_type>(total - first_n);
        r.second.ptr   = (r.second.count != 0u) ? buf : nullptr;

        r.total = total;
        return r;
    }

    // ------------------------------------------------------------------------------------------
    // Producer Operations
    // ------------------------------------------------------------------------------------------
    template<
        class U,
        typename = std::enable_if_t<std::is_assignable_v<reference, U&&>>
        >
    RB_FORCEINLINE void push(U&& v) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        SPSC_ASSERT(!full());
        storage_[Base::write_index()] = std::forward<U>(v);
        Base::increment_head();
    }

    template<
        class U,
        typename = std::enable_if_t<std::is_assignable_v<reference, U&&>>
        >
    [[nodiscard]] RB_FORCEINLINE bool try_push(U&& v) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        if (RB_UNLIKELY(full())) { return false; }
        storage_[Base::write_index()] = std::forward<U>(v);
        Base::increment_head();
        return true;
    }

    template<class... Args, typename = std::enable_if_t<
                                std::is_constructible_v<value_type, Args&&...> && std::is_assignable_v<reference, value_type>
                                >>
    RB_FORCEINLINE reference emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        SPSC_ASSERT(!full());
        const size_type wi = Base::write_index();
        reference slot = storage_[wi];
        slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return slot;
    }

    template<class... Args, typename = std::enable_if_t<
                                std::is_constructible_v<value_type, Args&&...> && std::is_assignable_v<reference, value_type>
                                >>
    [[nodiscard]] pointer try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        if (RB_UNLIKELY(full())) { return nullptr; }
        const size_type wi = Base::write_index();
        reference slot = storage_[wi];
        slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return &slot;
    }

    [[nodiscard]] RB_FORCEINLINE reference claim() noexcept {
        SPSC_ASSERT(!full());
        return storage_[Base::write_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(full())) { return nullptr; }
        return &storage_[Base::write_index()];
    }

    RB_FORCEINLINE void publish() noexcept {
        SPSC_ASSERT(!full());
        Base::increment_head();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish() noexcept {
        if (RB_UNLIKELY(full())) { return false; }
        Base::increment_head();
        return true;
    }

    RB_FORCEINLINE void publish(const size_type n) noexcept {
        SPSC_ASSERT(can_write(n));
        Base::advance_head(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_write(n))) { return false; }
        Base::advance_head(n);
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Consumer Operations
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE reference front() noexcept {
        SPSC_ASSERT(!empty());
        return storage_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE const_reference front() const noexcept {
        SPSC_ASSERT(!empty());
        return storage_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(empty())) { return nullptr; }
        return &storage_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(empty())) { return nullptr; }
        return &storage_[Base::read_index()];
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(!empty());
        Base::increment_tail();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(empty())) { return false; }
        Base::increment_tail();
        return true;
    }

    RB_FORCEINLINE void pop(const size_type n) noexcept {
        SPSC_ASSERT(can_read(n));
        Base::advance_tail(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) { return false; }
        Base::advance_tail(n);
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE const_reference operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx = static_cast<size_type>((Base::tail() + i) & Base::mask());
        return storage_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE reference operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx = static_cast<size_type>((Base::tail() + i) & Base::mask());
        return storage_[idx];
    }

#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<value_type> span() noexcept {
        return std::span<value_type>(data(), capacity());
    }
    [[nodiscard]] std::span<const value_type> span() const noexcept {
        return std::span<const value_type>(data(), capacity());
    }
#endif /* SPSC_HAS_SPAN */

    // ------------------------------------------------------------------------------------------
    // RAII Based API
    // ------------------------------------------------------------------------------------------
    class write_guard {
    public:
        write_guard() noexcept = default;

        explicit write_guard(fifo_view& q) noexcept
            : q_(&q), ptr_(q.try_claim()), active_(ptr_ != nullptr) {}

        write_guard(const write_guard&)            = delete;
        write_guard& operator=(const write_guard&) = delete;

        write_guard(write_guard&& other) noexcept
            : q_(other.q_), ptr_(other.ptr_), active_(other.active_),
            publish_on_destroy_(other.publish_on_destroy_) {
            other.q_ = nullptr;
            other.ptr_ = nullptr;
            other.active_ = false;
            other.publish_on_destroy_ = false;
        }

        write_guard& operator=(write_guard&&) = delete;

        ~write_guard() noexcept {
            if (active_ && q_ && publish_on_destroy_) { q_->publish(); }
        }

        void publish_on_destroy() const noexcept {
            if (active_) { publish_on_destroy_ = true; }
        }

        [[nodiscard]] pointer peek() const noexcept { return ptr_; }

        [[nodiscard]] pointer get() const noexcept {
            publish_on_destroy();
            return ptr_;
        }

        [[nodiscard]] reference ref() const noexcept {
            SPSC_ASSERT(ptr_ != nullptr);
            publish_on_destroy();
            return *ptr_;
        }

        [[nodiscard]] reference operator*() const noexcept { return ref(); }
        [[nodiscard]] pointer   operator->() const noexcept { return get(); }
        explicit operator bool() const noexcept { return active_; }

        void commit() noexcept {
            if (active_ && q_) { q_->publish(); }
            cancel();
        }

        void cancel() noexcept {
            active_ = false;
            publish_on_destroy_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        fifo_view* q_{nullptr};
        pointer    ptr_{nullptr};
        bool       active_{false};
        mutable bool publish_on_destroy_{false};
    };
    class read_guard {
    public:
        read_guard() noexcept = default;

        explicit read_guard(fifo_view& q) noexcept
            : q_(&q), ptr_(q.try_front()), active_(ptr_ != nullptr) {}

        read_guard(const read_guard&)            = delete;
        read_guard& operator=(const read_guard&) = delete;

        read_guard(read_guard&& other) noexcept
            : q_(other.q_), ptr_(other.ptr_), active_(other.active_)
        {
            other.q_ = nullptr;
            other.ptr_ = nullptr;
            other.active_ = false;
        }

        read_guard& operator=(read_guard&&) = delete;

        ~read_guard() noexcept {
            if (active_ && q_) { q_->pop(); }
        }

        [[nodiscard]] pointer   get()        const noexcept { return ptr_; }
        [[nodiscard]] reference ref()        const noexcept { SPSC_ASSERT(ptr_ != nullptr); return *ptr_; }
        [[nodiscard]] reference operator*()  const noexcept { return ref(); }
        [[nodiscard]] pointer   operator->() const noexcept { return ptr_; }
        explicit operator bool()             const noexcept { return active_; }

        void commit() noexcept {
            if (active_ && q_) { q_->pop(); }

            active_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }

        void cancel() noexcept {
            active_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        fifo_view* q_{nullptr};
        pointer    ptr_{nullptr};
        bool       active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept { return write_guard(*this); }
    [[nodiscard]] read_guard  scoped_read()  noexcept { return read_guard(*this); }

private:
    void move_from(fifo_view&& other) noexcept(kNoThrowMoveOps)
    {
        if constexpr (kDynamic) {
            const size_type cap  = other.Base::capacity();
            const size_type head = other.Base::head();
            const size_type tail = other.Base::tail();
            pointer ptr          = other.storage_;

            if (RB_UNLIKELY((ptr == nullptr) != (cap == 0u))) {
                storage_ = nullptr;
                (void)Base::init(0u);
                return;
            }

            if (ptr != nullptr) {
                const bool ok = Base::init(cap, head, tail);
                if (RB_UNLIKELY(!ok)) {
                    storage_ = nullptr;
                    (void)Base::init(0u);
                    return;
                }
                storage_ = ptr;
            } else {
                storage_ = nullptr;
                (void)Base::init(0u);
            }

            other.storage_ = nullptr;
            (void)other.Base::init(0u);
        } else {
            storage_ = other.storage_;
            if (storage_ != nullptr) {
                const size_type h = other.Base::head();
                const size_type t = other.Base::tail();
                const bool ok = Base::init(h, t);
                SPSC_ASSERT(ok);
                (void)ok;
            } else {
                Base::clear();
            }
            other.storage_ = nullptr;
            other.Base::clear();
        }
    }

private:
    storage_type storage_{nullptr};
};


// ------------------------------------------------------------------------
// Deduction Guides
// ------------------------------------------------------------------------
template<class T>
fifo_view(T*, reg) -> fifo_view<T, 0u>;

template<class T, reg N>
fifo_view(T (&)[N]) -> fifo_view<T, N>;

template<class T, reg N>
fifo_view(std::array<T, N>&) -> fifo_view<T, N>;

template<class T, class Policy>
fifo_view(T*, reg, Policy) -> fifo_view<T, 0u, Policy>;

template<class T, reg N, class Policy>
fifo_view(T (&)[N], Policy) -> fifo_view<T, N, Policy>;

template<class T, reg N, class Policy>
fifo_view(std::array<T, N>&, Policy) -> fifo_view<T, N, Policy>;


} // namespace spsc

#endif /* SPSC_FIFO_VIEW_HPP_ */
