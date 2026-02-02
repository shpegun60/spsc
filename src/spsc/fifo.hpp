/*
 * fifo.hpp
 *
 * Modernized SPSC FIFO (owning).
 *
 * Design goals:
 * - Modern:      constexpr, [[nodiscard]], optional span-based bulk API.
 * - Safe:        Dynamic fifo can be "invalid"; safe wrappers guard that.
 * - Fast:        Unchecked hot-paths; bulk regions expose contiguous spans.
 * - Optimized:   Uses memcpy for trivial types during resize/copy operations.
 *
 * Concurrency model:
 * - Single Producer / Single Consumer (wait-free / lock-free depends on
 * Policy).
 * - Producer:    push, try_push, emplace, claim, publish.
 * - Consumer:    front, pop, consume, claim_read.
 *
 * MEMORY LAYOUT NOTE:
 * - pop() does NOT destroy elements (assignment-based ring).
 * - Elements are overwritten on the next push.
 * - resize(), clear(), swap() are NOT thread-safe with push/pop.
 */

#ifndef SPSC_FIFO_HPP_
#define SPSC_FIFO_HPP_

#include <algorithm> // std::max, std::copy, std::move
#include <array>
#include <cstddef>  // std::ptrdiff_t, std::size_t
#include <cstring>  // std::memcpy
#include <iterator> // std::reverse_iterator
#include <limits>
#include <memory> // std::allocator_traits, std::uninitialized_...
#include <type_traits>
#include <utility> // std::move, std::swap

// Base and utility includes
#include "base/SPSCbase.hpp"      // ::spsc::SPSCbase<Capacity, Policy>, reg
#include "base/spsc_alloc.hpp"    // ::spsc::alloc::default_alloc
#include "base/spsc_snapshot.hpp" // ::spsc::snapshot_view, ::spsc::snapshot_traits
#include "base/spsc_tools.hpp"    // RB_FORCEINLINE, RB_UNLIKELY, macros

namespace spsc {

/* =======================================================================
 * fifo<T, Capacity, Policy, Alloc>
 *
 * Owning Single-Producer Single-Consumer ring buffer.
 * Can be static (Capacity != 0) or dynamic (Capacity == 0).
 * ======================================================================= */
template <class T, reg Capacity = 0,
         typename Policy = ::spsc::policy::default_policy,
         typename Alloc = ::spsc::alloc::default_alloc>
class fifo : private ::spsc::SPSCbase<Capacity, Policy> {
    static constexpr bool kDynamic = (Capacity == 0);

    using Base = ::spsc::SPSCbase<Capacity, Policy>;
    using StaticBuf = std::array<T, Capacity>;
    using DynamicBuf = T *;
    using storage_type = std::conditional_t<kDynamic, DynamicBuf, StaticBuf>;

    static constexpr bool kNoThrowMoveOps =
        kDynamic ? true
                 : (std::is_move_assignable_v<storage_type>
                        ? std::is_nothrow_move_assignable_v<storage_type>
                        : std::is_nothrow_copy_assignable_v<storage_type>);

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

    // Iterator types
    using iterator = ::spsc::detail::ring_iterator<value_type, size_type, false>;
    using const_iterator =
        ::spsc::detail::ring_iterator<value_type, size_type, true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // Snapshot types
    using snapshot_traits = ::spsc::snapshot_traits<value_type, size_type>;
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

    // ------------------------------------------------------------------------------------------
    // Static Assertions
    // ------------------------------------------------------------------------------------------
    static_assert(!kDynamic || alloc_traits::is_always_equal::value,
                  "[spsc::fifo]: dynamic fifo requires always_equal allocator "
                  "(stateless).");
    static_assert(
        !kDynamic || std::is_default_constructible_v<allocator_type>,
        "[spsc::fifo]: dynamic fifo requires default-constructible allocator.");
    static_assert(
        !kDynamic || std::is_same_v<alloc_pointer, pointer>,
        "[spsc::fifo]: dynamic fifo requires allocator pointer type T*.");
    static_assert(std::is_default_constructible_v<value_type>,
                  "[spsc::fifo]: value_type must be default-constructible.");
    static_assert(
        !std::is_const_v<value_type>,
        "[spsc::fifo]: const T does not make sense for a writable FIFO.");

#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<value_type>,
                  "[spsc::fifo]: no-exceptions mode requires noexcept default "
                  "constructor.");
    static_assert(
        std::is_nothrow_destructible_v<value_type>,
        "[spsc::fifo]: no-exceptions mode requires noexcept destructor.");
    static_assert(std::is_nothrow_move_assignable_v<value_type> ||
                      std::is_nothrow_copy_assignable_v<value_type>,
                  "[spsc::fifo]: no-exceptions mode requires noexcept assignment "
                  "(move or copy).");
#endif /* SPSC_ENABLE_EXCEPTIONS == 0 */

    static_assert(std::is_trivially_copyable_v<counter_value>,
                  "[spsc::fifo]: counter_value must be trivially copyable "
                  "(atomic-friendly).");
    static_assert(Capacity == 0 || ::spsc::cap::rb_is_pow2(Capacity),
                  "[spsc::fifo]: static Capacity must be power-of-two "
                  "(mask-based indexing).");
    static_assert(Capacity == 0 || Capacity >= 2,
                  "[spsc::fifo]: Capacity must be >= 2 (or 0 for dynamic).");
    static_assert(std::is_move_assignable_v<value_type> ||
                      std::is_copy_assignable_v<value_type>,
                  "[spsc::fifo]: value_type must be move- or copy-assignable.");
    static_assert(std::numeric_limits<counter_value>::digits >= 2,
                  "[spsc::fifo]: counter type is too narrow.");
    static_assert(::spsc::cap::RB_MAX_UNAMBIGUOUS <= (counter_value(1) << (std::numeric_limits<counter_value>::digits - 1)),
                  "[spsc::fifo]: RB_MAX_UNAMBIGUOUS exceeds counter unambiguous range.");
    static_assert(std::is_same_v<counter_value, geometry_value>,
                  "[spsc::fifo]: policy counter/geometry value types must match.");
    static_assert(std::is_unsigned_v<counter_value>,
                  "[spsc::fifo]: policy counter/geometry value type must be unsigned.");
    static_assert(sizeof(counter_value) >= sizeof(size_type),
                  "[spsc::fifo]: counter_type::value_type must be at least as wide as reg.");
    static_assert(std::is_default_constructible_v<allocator_type>,
                  "[spsc::fifo]: allocator must be default-constructible (used by get_allocator()).");
    // Extra compile-time hardening (keeps parity with pool)
    static_assert(std::is_unsigned_v<size_type>,
                  "[spsc::fifo]: reg (size_type) must be unsigned.");
    static_assert(Capacity == 0 || (Capacity <= ::spsc::cap::RB_MAX_UNAMBIGUOUS),
                  "[spsc::fifo]: static Capacity exceeds RB_MAX_UNAMBIGUOUS.");

    // ------------------------------------------------------------------------------------------
    // Region Structs (Bulk Operations)
    // ------------------------------------------------------------------------------------------
    struct region {
        pointer ptr{nullptr};
        size_type count{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }
#if SPSC_HAS_SPAN
        [[nodiscard]] std::span<value_type> span() const noexcept {
            return {ptr, count};
        }
#endif /* SPSC_HAS_SPAN */
    };

    struct regions {
        region first{};
        region second{};
        size_type total{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return total == 0u; }
    };

    // ------------------------------------------------------------------------------------------
    // Constructors / Destructor
    // ------------------------------------------------------------------------------------------

    // Default constructor: empty queue, capacity() depends on model.
    fifo() = default;

    // Dynamic-only constructor: request initial capacity via resize().
    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    explicit fifo(const size_type requested_capacity) {
        (void)resize(requested_capacity);
    }

    ~fifo() noexcept { destroy(); }

    // Copy semantics
    fifo(const fifo &other) : Base() { copy_from(other); }

    fifo &operator=(const fifo &other) {
        if (this == &other) {
            return *this;
        }

        if constexpr (kDynamic) {
            fifo tmp(other);

            // If source is valid but tmp couldn't become valid (alloc fail path),
            // keep *this unchanged.
            if (other.is_valid() && !tmp.is_valid()) {
                return *this;
            }

            swap(tmp);
            return *this;
        } else {
            if constexpr (std::is_nothrow_swappable_v<storage_type>) {
                fifo tmp(other);
                swap(tmp);
                return *this;
            } else {
                // copy_from() already does Base::clear() in static branch.
                copy_from(other);
                return *this;
            }
        }
    }

    // Move semantics
    fifo(fifo &&other) noexcept(kNoThrowMoveOps) : Base() { move_from(std::move(other)); }

    fifo &operator=(fifo &&other) noexcept(kNoThrowMoveOps) {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    void swap(fifo &other) noexcept(kDynamic ||
                                    std::is_nothrow_swappable_v<storage_type>) {
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

            SPSC_ASSERT((storage_ == nullptr) == (a_cap == 0u));
            SPSC_ASSERT((other.storage_ == nullptr) == (b_cap == 0u));

            std::swap(storage_, other.storage_);

            const bool ok1 =
                (b_cap != 0u) ? Base::init(b_cap, b_head, b_tail) : Base::init(0u);

            const bool ok2 = (a_cap != 0u) ? other.Base::init(a_cap, a_head, a_tail)
                                           : other.Base::init(0u);

            if (RB_UNLIKELY(!ok1 || !ok2)) {
                std::swap(storage_, other.storage_);

                const bool rb1 =
                    (a_cap != 0u) ? Base::init(a_cap, a_head, a_tail) : Base::init(0u);

                const bool rb2 = (b_cap != 0u) ? other.Base::init(b_cap, b_head, b_tail)
                                               : other.Base::init(0u);

                SPSC_ASSERT(rb1 && rb2);
                (void)rb1;
                (void)rb2;
            }

            // Keep invariant: detached dynamic fifo has zero geometry.
            if (storage_ == nullptr) {
                (void)Base::init(0u);
            }
            if (other.storage_ == nullptr) {
                (void)other.Base::init(0u);
            }
        } else {
            const size_type a_head = Base::head();
            const size_type a_tail = Base::tail();
            const size_type b_head = other.Base::head();
            const size_type b_tail = other.Base::tail();

            storage_.swap(other.storage_);
            Base::set_head(b_head);
            Base::set_tail(b_tail);
            Base::sync_cache();

            other.Base::set_head(a_head);
            other.Base::set_tail(a_tail);
            other.Base::sync_cache();
        }
    }

    friend void swap(fifo &a, fifo &b) noexcept(noexcept(a.swap(b))) {
        a.swap(b);
    }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        if constexpr (kDynamic) {
            return (storage_ != nullptr) && (Base::capacity() != 0u);
        } else {
            return true;
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

    void clear() noexcept { Base::clear(); }

    [[nodiscard]] allocator_type get_allocator() const noexcept { return {}; }

    // ------------------------------------------------------------------------------------------
    // Iteration API (Consumer Side Only)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] pointer data() noexcept {
        if constexpr (kDynamic) {
            return storage_;
        } else {
            return storage_.data();
        }
    }

    [[nodiscard]] const_pointer data() const noexcept {
        if constexpr (kDynamic) {
            return storage_;
        } else {
            return storage_.data();
        }
    }

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

        // Use a validated "used" snapshot to avoid impossible head<tail ranges
        // under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

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

        // Use a validated "used" snapshot to avoid impossible head<tail ranges
        // under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

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

        // Use a validated "used" snapshot to avoid impossible head<tail ranges
        // under atomic backends.
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

        // Use a validated "used" snapshot to avoid impossible head<tail ranges
        // under atomic backends.
        const size_type t    = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h    = static_cast<size_type>(t + used);

        const size_type m = Base::mask();
        return const_snapshot(it(data(), m, t), it(data(), m, h));
    }

    template <class Snap> void consume(const Snap &s) noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(s.begin().data() == data());
        SPSC_ASSERT(s.begin().mask() == Base::mask());

        const size_type cur_tail = static_cast<size_type>(Base::tail());
        SPSC_ASSERT(static_cast<size_type>(s.tail_index()) == cur_tail);

        const size_type new_tail = static_cast<size_type>(s.head_index());
        SPSC_ASSERT(new_tail >= cur_tail); // Guards against impossible snapshots

        pop(static_cast<size_type>(new_tail - cur_tail));
    }

    template <class Snap> [[nodiscard]] bool try_consume(const Snap &s) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }

        const size_type cap = Base::capacity();

        const size_type snap_tail = static_cast<size_type>(s.tail_index());
        const size_type snap_head = static_cast<size_type>(s.head_index());

        const size_type cur_tail = Base::tail();

        // Snapshot must be from the same buffer (cheap identity check)
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

        Base::sync_tail_to_head();
    }

    // ------------------------------------------------------------------------------------------
    // Bulk / Regions
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] regions
    claim_write(const size_type max_count =
                std::numeric_limits<size_type>::max()) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return {};
        }

        const size_type cap = Base::capacity();

        size_type total = static_cast<size_type>(Base::free());
        if (max_count < total) {
            total = max_count;
        }
        if (RB_UNLIKELY(total == 0u)) {
            return {};
        }

        const size_type wi = static_cast<size_type>(Base::write_index()); // masked
        const size_type w2e = static_cast<size_type>(cap - wi);
        const size_type first_n = (w2e < total) ? w2e : total;

        regions r{};
        auto *const buf = data();
        r.first.ptr = buf + wi;
        r.first.count = first_n;

        r.second.count = static_cast<size_type>(total - first_n);
        r.second.ptr = (r.second.count != 0u) ? buf : nullptr;

        r.total = total;
        return r;
    }

    [[nodiscard]] regions
    claim_read(const size_type max_count =
               std::numeric_limits<size_type>::max()) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return {};
        }

        const size_type cap = Base::capacity();

        size_type total = static_cast<size_type>(Base::size());
        if (max_count < total) {
            total = max_count;
        }
        if (RB_UNLIKELY(total == 0u)) {
            return {};
        }

        const size_type ri = static_cast<size_type>(Base::read_index()); // masked
        const size_type r2e = static_cast<size_type>(cap - ri);
        const size_type first_n = (r2e < total) ? r2e : total;

        regions r{};
        auto *const buf = data();
        r.first.ptr = buf + ri;
        r.first.count = first_n;

        r.second.count = static_cast<size_type>(total - first_n);
        r.second.ptr = (r.second.count != 0u) ? buf : nullptr;

        r.total = total;
        return r;
    }

    // ------------------------------------------------------------------------------------------
    // Producer Operations
    // ------------------------------------------------------------------------------------------
    template <class U,
             typename = std::enable_if_t<std::is_assignable_v<reference, U &&>>>
    RB_FORCEINLINE void
    push(U &&v) noexcept(std::is_nothrow_assignable_v<reference, U &&>) {
        SPSC_ASSERT(!full());
        storage_[Base::write_index()] = std::forward<U>(v);
        Base::increment_head();
    }

    template <class U,
             typename = std::enable_if_t<std::is_assignable_v<reference, U &&>>>
    [[nodiscard]] RB_FORCEINLINE bool
    try_push(U &&v) noexcept(std::is_nothrow_assignable_v<reference, U &&>) {
        if (RB_UNLIKELY(full())) {
            return false;
        }
        storage_[Base::write_index()] = std::forward<U>(v);
        Base::increment_head();
        return true;
    }

    template <class... Args,
             typename = std::enable_if_t<
                 std::is_constructible_v<value_type, Args &&...> &&
                 std::is_assignable_v<reference, value_type>>>
    RB_FORCEINLINE reference emplace(Args &&...args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args &&...> &&
        std::is_nothrow_assignable_v<reference, value_type>) {
        SPSC_ASSERT(!full());
        const size_type wi = Base::write_index();
        reference slot = storage_[wi];
        slot = value_type(std::forward<Args>(args)...);
        Base::increment_head();
        return slot;
    }

    template <class... Args,
             typename = std::enable_if_t<
                 std::is_constructible_v<value_type, Args &&...> &&
                 std::is_assignable_v<reference, value_type>>>
    [[nodiscard]] pointer try_emplace(Args &&...args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args &&...> &&
        std::is_nothrow_assignable_v<reference, value_type>) {
        if (RB_UNLIKELY(full())) {
            return nullptr;
        }
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
        if (RB_UNLIKELY(empty())) {
            return nullptr;
        }
        return &storage_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(empty())) {
            return nullptr;
        }
        return &storage_[Base::read_index()];
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(!empty());
        Base::increment_tail();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(empty())) {
            return false;
        }
        Base::increment_tail();
        return true;
    }

    RB_FORCEINLINE void pop(const size_type n) noexcept {
        SPSC_ASSERT(can_read(n));
        Base::advance_tail(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) {
            return false;
        }
        Base::advance_tail(n);
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE const_reference
    operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx =
            static_cast<size_type>((Base::tail() + i) & Base::mask());
        return storage_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE reference
    operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx =
            static_cast<size_type>((Base::tail() + i) & Base::mask());
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
    // Dynamic-only API (Resize)
    // ------------------------------------------------------------------------------------------
    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool reserve(size_type min_capacity) {
        if (capacity() >= min_capacity) {
            return true;
        }
        return resize(min_capacity);
    }

    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool resize(const size_type requested_capacity) {
        static_assert(::spsc::cap::rb_is_pow2(::spsc::cap::RB_MAX_UNAMBIGUOUS),
                      "[fifo]: RB_MAX_UNAMBIGUOUS must be power of two");

        const size_type old_cap = Base::capacity();
        const size_type old_head = Base::head();
        const size_type old_tail = Base::tail();

        size_type old_size = 0u;
        if (storage_ && old_cap) {
            old_size = static_cast<size_type>(old_head - old_tail);
            if (RB_UNLIKELY(old_size > old_cap)) {
                // Corrupted state: drop queued data to avoid OOB copy.
                Base::clear();
                old_size = 0u;
            }
        }

        // Debug paranoia: if we have storage, geometry must know its capacity.
        SPSC_ASSERT(storage_ == nullptr || old_cap != 0u);

        allocator_type alloc{};

        // Explicit shrink-to-zero: release storage and clear queue.
        if (requested_capacity == 0u) {
            destroy();
            return true;
        }

        // Normalize requested capacity and clamp to sane minimum for mask-based
        // ring.
        const size_type req = (requested_capacity > ::spsc::cap::RB_MAX_UNAMBIGUOUS)
                                  ? ::spsc::cap::RB_MAX_UNAMBIGUOUS
                                  : requested_capacity;

        const size_type req2 = (req < 2u) ? 2u : req;
        const size_type target_cap = ::spsc::cap::rb_next_power2(req2);

        // Optimization: If no growth needed, keep existing storage.
        if (storage_ && old_cap && target_cap <= old_cap) {
            return true;
        }

        // Allocation step (may throw depending on allocator / build mode).
        pointer new_buf = alloc_traits::allocate(alloc, target_cap);
        if (RB_UNLIKELY(!new_buf)) {
            return false;
        }

        // Construct default objects in new buffer.
        SPSC_TRY { std::uninitialized_default_construct_n(new_buf, target_cap); }
        SPSC_CATCH_ALL {
            alloc_traits::deallocate(alloc, new_buf, target_cap);
            SPSC_RETHROW;
        }

        // Data migration step (linearization).
        SPSC_TRY {
            if (storage_ && old_cap && old_size) {
                const size_type old_mask = Base::mask();

                const size_type tail_idx = old_tail & old_mask;
                const size_type to_end = old_cap - tail_idx;
                const size_type first_n = (old_size < to_end) ? old_size : to_end;
                const size_type second_n = old_size - first_n;

                if constexpr (std::is_trivially_copyable_v<value_type>) {
                    std::memcpy(new_buf, storage_ + tail_idx,
                                first_n * sizeof(value_type));
                    if (second_n > 0) {
                        std::memcpy(new_buf + first_n, storage_,
                                    second_n * sizeof(value_type));
                    }
                } else {
                    for (size_type k = 0; k < first_n; ++k) {
                        if constexpr (std::is_move_assignable_v<value_type> &&
                                      (!std::is_copy_assignable_v<value_type> ||
                                       std::is_nothrow_move_assignable_v<value_type>)) {
                            new_buf[k] = std::move(storage_[tail_idx + k]);
                        } else {
                            new_buf[k] = storage_[tail_idx + k];
                        }
                    }
                    for (size_type k = 0; k < second_n; ++k) {
                        if constexpr (std::is_move_assignable_v<value_type> &&
                                      (!std::is_copy_assignable_v<value_type> ||
                                       std::is_nothrow_move_assignable_v<value_type>)) {
                            new_buf[first_n + k] = std::move(storage_[k]);
                        } else {
                            new_buf[first_n + k] = storage_[k];
                        }
                    }
                }
            }
        }
        SPSC_CATCH_ALL {
            // Rollback on failure.
            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                std::destroy_n(new_buf, target_cap);
            }
            alloc_traits::deallocate(alloc, new_buf, target_cap);
            SPSC_RETHROW;
        }

        // Clean up old storage.
        if (storage_ && old_cap) {
            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                std::destroy_n(storage_, old_cap);
            }
            alloc_traits::deallocate(alloc, storage_, old_cap);
        }

        storage_ = new_buf;

        // Reset geometry. init() sets head=0, tail=0.
        const bool ok = Base::init(target_cap);
        if (RB_UNLIKELY(!ok)) {
            // If policy init fails (should not happen), avoid leaving a half-valid
            // object.
            pointer ptr = storage_;
            storage_ = nullptr;
            (void)Base::init(0u);

            if constexpr (!std::is_trivially_destructible_v<value_type>) {
                std::destroy_n(ptr, target_cap);
            }
            alloc_traits::deallocate(alloc, ptr, target_cap);
            return false;
        }

        // We linearized data to the start of new_buf, so head becomes old_size.
        if (old_size) {
            SPSC_ASSERT(old_size <= target_cap);
            Base::advance_head(old_size);
        }

        // Non-concurrent operation: keep shadow caches coherent after changing indices.
        Base::sync_cache();

        return true;
    }

    void destroy() noexcept {
        // Not concurrent with producer/consumer ops.
        if constexpr (kDynamic) {
            pointer ptr = storage_;
            const size_type cap = Base::capacity();

            SPSC_ASSERT((ptr == nullptr) == (cap == 0u));

            storage_ = nullptr;
            (void)Base::init(0u);

            if (ptr != nullptr) {
                allocator_type alloc{};

                // Debug paranoia: storage implies non-zero capacity.
                SPSC_ASSERT(cap != 0u);

                if (RB_LIKELY(cap != 0u)) {
                    if constexpr (!std::is_trivially_destructible_v<value_type>) {
                        std::destroy_n(ptr, cap);
                    }
                    alloc_traits::deallocate(alloc, ptr, cap);
                }
            }
            return;
        }
        Base::clear();
    }

    // ------------------------------------------------------------------------------------------
    // RAII Based API
    // ------------------------------------------------------------------------------------------
    class write_guard {
    public:
        write_guard() noexcept = default;

        explicit write_guard(fifo &q) noexcept
            : q_(&q), ptr_(q.try_claim()), active_(ptr_ != nullptr) {}

        write_guard(const write_guard &) = delete;
        write_guard &operator=(const write_guard &) = delete;

        write_guard(write_guard &&other) noexcept
            : q_(other.q_), ptr_(other.ptr_), active_(other.active_),
            publish_on_destroy_(other.publish_on_destroy_) {
            other.q_ = nullptr;
            other.ptr_ = nullptr;
            other.active_ = false;
            other.publish_on_destroy_ = false;
        }

        write_guard &operator=(write_guard &&) = delete;

        ~write_guard() noexcept {
            if (active_ && q_ && publish_on_destroy_) {
                q_->publish();
            }
        }

        // Arm publishing on scope-exit. Useful after writing through
        // peek()/get()/ref().
        void publish_on_destroy() const noexcept {
            if (active_) {
                publish_on_destroy_ = true;
            }
        }

        // Does NOT arm publishing. Use when you only need the address.
        [[nodiscard]] pointer peek() const noexcept { return ptr_; }

        // Arms publishing, because getting a writable pointer usually means intent
        // to write.
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
        [[nodiscard]] pointer operator->() const noexcept { return get(); }
        explicit operator bool() const noexcept { return active_; }

        void commit() noexcept {
            if (active_ && q_) {
                q_->publish();
            }
            cancel();
        }

        void cancel() noexcept {
            active_ = false;
            publish_on_destroy_ = false;
            q_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        fifo *q_{nullptr};
        pointer ptr_{nullptr};
        bool active_{false};
        mutable bool publish_on_destroy_{false};
    };

    class read_guard {
    public:
        read_guard() noexcept = default;

        explicit read_guard(fifo &q) noexcept
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
        fifo *q_{nullptr};
        pointer ptr_{nullptr};
        bool active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept {
        return write_guard(*this);
    }
    [[nodiscard]] read_guard scoped_read() noexcept { return read_guard(*this); }

private:
    void copy_from(const fifo &other) {
        if constexpr (kDynamic) {
            static_assert(std::is_copy_assignable_v<value_type>,
                          "[spsc::fifo]: copy requires copy-assignable value_type");

            allocator_type alloc{};
            const size_type cap = other.capacity();
            const size_type sz = other.size();

            if (RB_UNLIKELY(sz > cap)) {
                // Corrupted source state: safest is to produce an empty/invalid fifo.
                storage_ = nullptr;
                (void)Base::init(0u);
                return;
            }

            if (cap == 0u || !other.is_valid()) {
                storage_ = nullptr;
                (void)Base::init(0u);
                return;
            }

            pointer new_buf = alloc_traits::allocate(alloc, cap);
            if (!new_buf) {
                storage_ = nullptr;
                (void)Base::init(0u);
                return;
            }

            SPSC_TRY { std::uninitialized_default_construct_n(new_buf, cap); }
            SPSC_CATCH_ALL {
                alloc_traits::deallocate(alloc, new_buf, cap);
                SPSC_RETHROW;
            }

            SPSC_TRY {
                const size_type mask = other.Base::mask();
                const size_type tail = other.Base::tail();

                if constexpr (std::is_trivially_copyable_v<value_type>) {
                    const size_type idx_start = tail & mask;
                    const size_type to_end = cap - idx_start;
                    const size_type first_n = (sz < to_end) ? sz : to_end;
                    const size_type second_n = sz - first_n;

                    std::memcpy(new_buf, other.storage_ + idx_start,
                                first_n * sizeof(value_type));
                    if (second_n > 0) {
                        std::memcpy(new_buf + first_n, other.storage_,
                                    second_n * sizeof(value_type));
                    }
                } else {
                    for (size_type k = 0; k < sz; ++k) {
                        const size_type idx = static_cast<size_type>((tail + k) & mask);
                        new_buf[k] = other.storage_[idx];
                    }
                }
            }
            SPSC_CATCH_ALL {
                if constexpr (!std::is_trivially_destructible_v<value_type>) {
                    std::destroy_n(new_buf, cap);
                }
                alloc_traits::deallocate(alloc, new_buf, cap);
                SPSC_RETHROW;
            }

            storage_ = new_buf;

            const bool ok = Base::init(cap);
            if (RB_UNLIKELY(!ok)) {
                // If policy init fails (should not happen), avoid leaving a half-valid
                // object.
                pointer ptr = storage_;
                storage_ = nullptr;
                (void)Base::init(0u);

                if constexpr (!std::is_trivially_destructible_v<value_type>) {
                    std::destroy_n(ptr, cap);
                }
                alloc_traits::deallocate(alloc, ptr, cap);
                return;
            }

            if (sz) {
                Base::advance_head(sz);
            }
        } else {
            // Static Copy
            static_assert(std::is_copy_assignable_v<value_type>,
                          "[spsc::fifo]: copy requires copy-assignable value_type");

            const size_type cap = Base::capacity();
            const size_type sz = other.size();
            Base::clear();

            if (RB_UNLIKELY(sz > cap)) {
                // Corrupted source: keep *this empty and valid
                return;
            }

            if (sz != 0u) {
                const size_type mask = other.Base::mask();
                const size_type tail = other.Base::tail();

                for (size_type k = 0; k < sz; ++k) {
                    const size_type idx = static_cast<size_type>((tail + k) & mask);
                    storage_[k] = other.storage_[idx];
                }
                Base::advance_head(sz);
            }
        }

        // Non-concurrent operation: keep shadow caches coherent after restoring indices.
        Base::sync_cache();
    }

    void move_from(fifo &&other) noexcept(kNoThrowMoveOps) {
        if constexpr (kDynamic) {
            const size_type cap = other.Base::capacity();
            const size_type head = other.Base::head();
            const size_type tail = other.Base::tail();
            pointer ptr = other.storage_;

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

            // Steal and reset other
            other.storage_ = nullptr;
            (void)other.Base::init(0u);
        } else {
            if constexpr (std::is_move_assignable_v<storage_type>) {
                storage_ = std::move(other.storage_);
            } else {
                storage_ = other.storage_;
            }
            Base::set_head(other.Base::head());
            Base::set_tail(other.Base::tail());

            // Non-concurrent operation: keep shadow caches coherent after restoring indices.
            Base::sync_cache();

            other.Base::clear();
        }
    }

private:
    storage_type storage_{};
};

// ---------------------------------------------------------------------------
// Convenience Aliases
// ---------------------------------------------------------------------------

/**
 * fast_fifo<T, Capacity>:
 * A pre-configured fifo using Atomic counters and Cache-line padding
 * to avoid false sharing. This is the recommended default for
 * high-performance concurrent queues.
 */
template <class T, reg Capacity = 0,
         typename Alloc = ::spsc::alloc::default_alloc>
using fast_fifo = fifo<T, Capacity, ::spsc::policy::CA<>, Alloc>;

} // namespace spsc

#endif /* SPSC_FIFO_HPP_ */
