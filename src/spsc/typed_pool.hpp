/*
 * spsc_typed_pool.hpp
 *
 * Owning typed SPSC pool of fixed-size slots.
 * Mirrors spsc::pool API/structure for consistency.
 *
 * The ring stores 'T*' pointers.
 * Each pointer refers to raw storage for ONE 'T' (allocated once per slot).
 *
 * Producer:
 * - emplace/push: placement-new into the slot at write index, then advance
 * head.
 * - claim/publish: manual path (claim returns raw storage pointer, user
 * constructs, publish advances).
 *
 * Consumer:
 * - front: returns pointer to a constructed T.
 * - pop: destroys T in-place and advances tail.
 *
 * Resize semantics:
 * - Dynamic (Capacity==0): grow-only (unless depth==0 -> destroy).
 * - On grow, we ONLY rewire/move pointers (no moving/copying of slot memory).
 * Existing slot pointers are migrated into the new pointer ring in logical
 * order. Extra slots get freshly allocated storage.
 *
 * Copy semantics:
 * - Deep copy. Allocates new storages and copy-constructs live objects.
 *
 * Concurrency note:
 * - push/pop are SPSC-safe.
 * - resize/destroy/swap/copy/move/clear are NOT concurrent with push/pop.
 */

#ifndef SPSC_TYPED_POOL_HPP_
#define SPSC_TYPED_POOL_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

// Base and utility includes
#include "base/SPSCbase.hpp"        // ::spsc::SPSCbase<Capacity, Policy>, reg
#include "base/spsc_alloc.hpp"      // ::spsc::alloc::default_alloc
#include "base/spsc_object.hpp"     // ::spsc::detail::destroy_at
#include "base/spsc_snapshot.hpp"   // ::spsc::snapshot_view, ::spsc::snapshot_traits
#include "base/spsc_tools.hpp"      // RB_FORCEINLINE, RB_UNLIKELY, SPSC_* macros (also handles <span>)

namespace spsc {

namespace detail {

template <reg Capacity> struct typed_pool_base {
    bool isAllocated_{false};
};

template <> struct typed_pool_base<0> {};

} // namespace detail

/* =======================================================================
 * typed_pool<T, Capacity, Policy, Alloc>
 * ======================================================================= */
template <class T, reg Capacity = 0,
         typename Policy = ::spsc::policy::default_policy,
         typename Alloc = ::spsc::alloc::default_alloc>
class typed_pool : public detail::typed_pool_base<Capacity>,
                   private ::spsc::SPSCbase<Capacity, Policy> {
    static constexpr bool kDynamic = (Capacity == 0);
    using Base = ::spsc::SPSCbase<Capacity, Policy>;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    // Ring element type (same role as pool::value_type / pool::pointer).
    using object_type = T;
    using pointer = T *;             // points to the object storage
    using const_pointer = const T *; // points to a const object
    using value_type = pointer;      // ring stores pointers

    // Convenience aliases (not used by the ring itself).
    using object_reference = T &;
    using object_const_reference = const T &;

    using size_type = reg;
    using difference_type = std::ptrdiff_t;

    // Allocator types
    using base_allocator_type = Alloc;

    // Allocator for the ring of pointers (T*)
    using slot_allocator_type = typename std::allocator_traits<
        base_allocator_type>::template rebind_alloc<pointer>;
    using slot_alloc_traits = std::allocator_traits<slot_allocator_type>;
    using slot_pointer = typename slot_alloc_traits::pointer;

    // Allocator for the actual slot storage (T)
    using object_allocator_type = typename std::allocator_traits<
        base_allocator_type>::template rebind_alloc<T>;
    using object_alloc_traits = std::allocator_traits<object_allocator_type>;
    using object_pointer = typename object_alloc_traits::pointer;

    // Storage for ring slots
    using static_slots = std::array<pointer, Capacity>;
    using dynamic_slots = pointer *;
    using slots_storage =
        std::conditional_t<kDynamic, dynamic_slots, static_slots>;

    // Iterator types (same iterator engine as fifo)
    using iterator = ::spsc::detail::ring_iterator<std::add_const_t<value_type>,
                                                   size_type, false>;
    using const_iterator =
        ::spsc::detail::ring_iterator<std::add_const_t<value_type>, size_type,
                                      true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // Snapshot types
    using snapshot_traits =
        ::spsc::snapshot_traits<std::add_const_t<value_type>, size_type>;
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
    static_assert(std::is_default_constructible_v<base_allocator_type>,
                  "[spsc::typed_pool]: allocator must be default-constructible "
                  "(used by get_allocator()).");
    static_assert(!kDynamic || slot_alloc_traits::is_always_equal::value,
                  "[spsc::typed_pool]: dynamic typed_pool requires always_equal "
                  "allocator (stateless).");
    static_assert(!kDynamic ||
                      std::is_default_constructible_v<slot_allocator_type>,
                  "[spsc::typed_pool]: dynamic typed_pool requires "
                  "default-constructible allocator.");
    static_assert(!kDynamic || std::is_same_v<slot_pointer, pointer *>,
                  "[spsc::typed_pool]: dynamic typed_pool requires allocator "
                  "pointer type T** (raw).");
    static_assert(
        std::is_same_v<object_pointer, pointer>,
        "[spsc::typed_pool]: requires object allocator pointer type T* (raw).");
    static_assert(std::numeric_limits<counter_value>::digits >= 2,
                  "[spsc::typed_pool]: counter type is too narrow.");
    static_assert(::spsc::cap::RB_MAX_UNAMBIGUOUS <=
                      (counter_value(1)
                       << (std::numeric_limits<counter_value>::digits - 1)),
                  "[spsc::typed_pool]: RB_MAX_UNAMBIGUOUS exceeds counter "
                  "unambiguous range.");
    static_assert(
        std::is_same_v<counter_value, geometry_value>,
        "[spsc::typed_pool]: policy counter/geometry value types must match.");
    static_assert(std::is_unsigned_v<counter_value>,
                  "[spsc::typed_pool]: policy counter/geometry value type must "
                  "be unsigned.");
    static_assert(sizeof(counter_value) >= sizeof(size_type),
                  "[spsc::typed_pool]: counter_type::value_type must be at least "
                  "as wide as reg.");
    static_assert(std::is_unsigned_v<size_type>,
                  "[spsc::typed_pool]: reg (size_type) must be unsigned.");
    static_assert(kDynamic || (Capacity >= 2),
                  "[spsc::typed_pool]: static Capacity must be >= 2.");
    static_assert(kDynamic || ::spsc::cap::rb_is_pow2(Capacity),
                  "[spsc::typed_pool]: static Capacity must be power of two.");
    static_assert(
        kDynamic || (Capacity <= ::spsc::cap::RB_MAX_UNAMBIGUOUS),
        "[spsc::typed_pool]: static Capacity exceeds RB_MAX_UNAMBIGUOUS.");
    static_assert(
        object_alloc_traits::is_always_equal::value,
        "[spsc::typed_pool]: object allocator must be always_equal (stateless).");
    static_assert(
        std::is_default_constructible_v<object_allocator_type>,
        "[spsc::typed_pool]: object allocator must be default-constructible.");
    static_assert(std::is_same_v<value_type, pointer>,
                  "[spsc::typed_pool]: value_type must match pointer.");
    static_assert(std::is_trivially_copyable_v<pointer>,
                  "[spsc::typed_pool]: pointer must be trivially copyable.");

    // ------------------------------------------------------------------------------------------
    // Region Structs (Bulk Operations)
    // ------------------------------------------------------------------------------------------
    struct region {
        pointer const *slts{nullptr};
        size_type count{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }
#if SPSC_HAS_SPAN
        [[nodiscard]] std::span<pointer const> span() const noexcept {
            return {slts, count};
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

    typed_pool() {
        if constexpr (!kDynamic) {
            (void)resize();
        }
    }

    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    explicit typed_pool(const size_type depth) {
        (void)resize(depth);
    }

    ~typed_pool() noexcept { destroy(); }

    // Copy semantics
    typed_pool(const typed_pool &other) : Base() { (void)copy_from(other); }

    typed_pool &operator=(const typed_pool &other) {
        if (this == &other) {
            return *this;
        }

        typed_pool tmp(other);
        if (other.is_valid() && !tmp.is_valid()) {
            return *this;
        }
        swap(tmp);
        return *this;
    }

    // Move semantics
    typed_pool(typed_pool &&other) noexcept : Base() {
        move_from(std::move(other));
    }

    typed_pool &operator=(typed_pool &&other) noexcept {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    void swap(typed_pool &other) noexcept {
        if (this == &other) {
            return;
        }

        const size_type a_cap = Base::capacity();
        const size_type a_head = Base::head();
        const size_type a_tail = Base::tail();

        const size_type b_cap = other.Base::capacity();
        const size_type b_head = other.Base::head();
        const size_type b_tail = other.Base::tail();

        if constexpr (kDynamic) {
            SPSC_ASSERT((slots_ == nullptr) == (a_cap == 0u));
            SPSC_ASSERT((other.slots_ == nullptr) == (b_cap == 0u));

            std::swap(slots_, other.slots_);

            const bool ok1 =
                (b_cap != 0u) ? Base::init(b_cap, b_head, b_tail) : Base::init(0u);
            const bool ok2 = (a_cap != 0u) ? other.Base::init(a_cap, a_head, a_tail)
                                           : other.Base::init(0u);

            if (RB_UNLIKELY(!ok1 || !ok2)) {
                std::swap(slots_, other.slots_);
                (void)Base::init(a_cap, a_head, a_tail);
                (void)other.Base::init(b_cap, b_head, b_tail);
            }
        } else {
            std::swap(slots_, other.slots_);
            std::swap(this->isAllocated_, other.isAllocated_);

            // Capacity is fixed for both instances.
            [[maybe_unused]] const bool ok1 = Base::init(b_head, b_tail);
            [[maybe_unused]] const bool ok2 = other.Base::init(a_head, a_tail);
            SPSC_ASSERT(ok1 && ok2);
        }
    }

    // ------------------------------------------------------------------------------------------
    // Introspection
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] constexpr base_allocator_type get_allocator() const noexcept {
        return base_allocator_type{};
    }

    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        if constexpr (kDynamic) {
            SPSC_ASSERT((slots_ == nullptr) == (Base::capacity() == 0u));
            return slots_ != nullptr;
        } else {
            return this->isAllocated_;
        }
    }

    [[nodiscard]] RB_FORCEINLINE size_type capacity() const noexcept {
        return is_valid() ? Base::capacity() : 0u;
    }
    [[nodiscard]] RB_FORCEINLINE size_type size() const noexcept {
        return is_valid() ? Base::size() : 0u;
    }
    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept {
        return !is_valid() || Base::empty();
    }
    [[nodiscard]] RB_FORCEINLINE bool full() const noexcept {
        return !is_valid() || Base::full();
    }
    [[nodiscard]] RB_FORCEINLINE size_type free() const noexcept {
        return is_valid() ? Base::free() : 0u;
    }

    [[nodiscard]] RB_FORCEINLINE bool
    can_write(const size_type n = 1u) const noexcept {
        return is_valid() && Base::can_write(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool
    can_read(const size_type n = 1u) const noexcept {
        return is_valid() && Base::can_read(n);
    }

    // Pool compatibility helpers
    [[nodiscard]] static constexpr size_type buffer_size() noexcept {
        return static_cast<size_type>(sizeof(T));
    }

    [[nodiscard]] static constexpr size_type buffer_align() noexcept {
        return static_cast<size_type>(alignof(T));
    }

    // ------------------------------------------------------------------------------------------
    // Direct storage access
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] RB_FORCEINLINE pointer const *data() noexcept {
        // Expose slots as read-only to prevent users from corrupting the pointer
        // ring.
        if constexpr (kDynamic) {
            return slots_;
        } else {
            return slots_.data();
        }
    }

    [[nodiscard]] RB_FORCEINLINE pointer const *data() const noexcept {
        if constexpr (kDynamic) {
            return slots_;
        } else {
            return slots_.data();
        }
    }

    // ------------------------------------------------------------------------------------------
    // Iteration API (Consumer Side Only)
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

        // Use a validated "used" snapshot to avoid impossible head<tail ranges
        // under atomic backends.
        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);

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
        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);

        const size_type m = Base::mask();
        return const_snapshot(it(data(), m, t), it(data(), m, h));
    }

    template <class Snap>
    void consume(const Snap& s) noexcept {
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

        if constexpr (!std::is_trivially_destructible_v<object_type>) {
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
        r.first.slts = data() + wi;
        r.first.count = first_n;

        r.second.count = static_cast<size_type>(total - first_n);
        r.second.slts = (r.second.count != 0u) ? data() : nullptr;

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
        r.first.slts = data() + ri;
        r.first.count = first_n;

        r.second.count = static_cast<size_type>(total - first_n);
        r.second.slts = (r.second.count != 0u) ? data() : nullptr;

        r.total = total;
        return r;
    }


    // ------------------------------------------------------------------------------------------
    // Producer Operations
    // ------------------------------------------------------------------------------------------

    template <class... Args> void emplace(Args &&...args) {
        static_assert(std::is_constructible_v<T, Args &&...>,
                      "[typed_pool]: T must be constructible from Args...");
        SPSC_ASSERT(!full());

        // Get raw storage pointer (no launder needed for raw storage)
        pointer dst = data()[Base::write_index()];
        ::new (static_cast<void *>(dst)) T(std::forward<Args>(args)...);
        Base::increment_head();
    }

    template <class... Args> [[nodiscard]] bool try_emplace(Args &&...args) {
        static_assert(std::is_constructible_v<T, Args &&...>,
                      "[typed_pool]: T must be constructible from Args...");
        if (RB_UNLIKELY(full())) {
            return false;
        }

        pointer dst = data()[Base::write_index()];
        ::new (static_cast<void *>(dst)) T(std::forward<Args>(args)...);
        Base::increment_head();
        return true;
    }

    void push(const T &v) { emplace(v); }
    void push(T &&v) { emplace(std::move(v)); }

    [[nodiscard]] bool try_push(const T &v) { return try_emplace(v); }
    [[nodiscard]] bool try_push(T &&v) { return try_emplace(std::move(v)); }

    // Manual population: claim returns raw storage pointer (uninitialized).
    [[nodiscard]] RB_FORCEINLINE pointer claim() noexcept {
        SPSC_ASSERT(!full());
        return data()[Base::write_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(full())) {
            return nullptr;
        }
        return data()[Base::write_index()];
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

    [[nodiscard]] RB_FORCEINLINE pointer front() noexcept {
        SPSC_ASSERT(!empty());
        return object_ptr(Base::read_index());
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer front() const noexcept {
        SPSC_ASSERT(!empty());
        return object_ptr(Base::read_index());
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(empty())) {
            return nullptr;
        }
        return object_ptr(Base::read_index());
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(empty())) {
            return nullptr;
        }
        return object_ptr(Base::read_index());
    }

    RB_FORCEINLINE void pop() noexcept {
        SPSC_ASSERT(!empty());

        pointer p = object_ptr(Base::read_index());
        detail::destroy_at(p);
        Base::increment_tail();
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(empty())) {
            return false;
        }

        pointer p = object_ptr(Base::read_index());
        detail::destroy_at(p);
        Base::increment_tail();
        return true;
    }

    RB_FORCEINLINE void pop(const size_type n) noexcept {
        SPSC_ASSERT(can_read(n));

        for (size_type k = 0; k < n; ++k) {
            pointer p = object_ptr((Base::tail() + k) & Base::mask());
            detail::destroy_at(p);
        }
        Base::advance_tail(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) {
            return false;
        }

        for (size_type k = 0; k < n; ++k) {
            pointer p = object_ptr((Base::tail() + k) & Base::mask());
            detail::destroy_at(p);
        }
        Base::advance_tail(n);
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE pointer operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx =
            static_cast<size_type>((Base::tail() + i) & Base::mask());
        return object_ptr(idx);
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer
    operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx =
            static_cast<size_type>((Base::tail() + i) & Base::mask());
        return object_ptr(idx);
    }

    void clear() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }
        consume_all();
        Base::clear();
    }

    // ------------------------------------------------------------------------------------------
    // Resize / Destroy
    // ------------------------------------------------------------------------------------------

    template <size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool resize(size_type depth) {
        return reallocate_impl(depth);
    }

    template <size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool resize() {
        // For static typed_pool, "resize" means: allocate once if not allocated.
        if (is_valid()) {
            return true;
        }
        return allocate_static_storage();
    }

    void destroy() noexcept {
        if constexpr (kDynamic) {
            pointer *ptr = slots_;
            const size_type cap = Base::capacity();
            const size_type head = Base::head();
            const size_type tail = Base::tail();

            SPSC_ASSERT((ptr == nullptr) == (cap == 0u));

            slots_ = nullptr;
            (void)Base::init(0u);

            if (!ptr || cap == 0u) {
                return;
            }

            // Best-effort: destroy live objects if state looks sane.
            const size_type used = static_cast<size_type>(head - tail);
            if (used <= cap) {
                const size_type mask = cap - 1u;
                for (size_type k = 0; k < used; ++k) {
                    // Manual launder for destruction
                    pointer p = std::launder(ptr[(tail + k) & mask]);
                    detail::destroy_at(p);
                }
            }

            // Free all storages
            object_allocator_type oa{};
            for (size_type i = 0; i < cap; ++i) {
                if (ptr[i]) {
                    object_alloc_traits::deallocate(oa, ptr[i], 1);
                }
            }

            // Free pointer ring
            slot_allocator_type sa{};
            slot_alloc_traits::deallocate(sa, ptr, cap);
        } else {
            if (!this->isAllocated_) {
                return;
            }

            // Destroy live objects if state looks sane.
            const size_type cap = Base::capacity();
            const size_type head = Base::head();
            const size_type tail = Base::tail();

            const size_type used = static_cast<size_type>(head - tail);
            if (used <= cap && cap != 0u) {
                const size_type mask = cap - 1u;
                for (size_type k = 0; k < used; ++k) {
                    pointer p = std::launder(slots_[(tail + k) & mask]);
                    detail::destroy_at(p);
                }
            }

            object_allocator_type oa{};
            for (size_type i = 0; i < Capacity; ++i) {
                if (slots_[i]) {
                    object_alloc_traits::deallocate(oa, slots_[i], 1);
                    slots_[i] = nullptr;
                }
            }

            this->isAllocated_ = false;
            Base::clear();
        }
    }
    // ------------------------------------------------------------------------------------------
    // RAII Based API
    // ------------------------------------------------------------------------------------------

    class write_guard {
    public:
        write_guard() noexcept = default;

        explicit write_guard(typed_pool &p) noexcept
            : p_(&p), ptr_(p.try_claim()) {}

        write_guard(const write_guard &) = delete;
        write_guard &operator=(const write_guard &) = delete;

        write_guard(write_guard &&other) noexcept
            : p_(other.p_), ptr_(other.ptr_),
            publish_on_destroy_(other.publish_on_destroy_),
            constructed_(other.constructed_) {
            other.p_ = nullptr;
            other.ptr_ = nullptr;
            other.publish_on_destroy_ = false;
            other.constructed_ = false;
        }

        write_guard &operator=(write_guard &&) = delete;
        ~write_guard() noexcept {
            if (!p_ || !ptr_) {
                return;
            }

            if (publish_on_destroy_ && constructed_) {
                // Object becomes visible to the consumer.
                p_->publish();
            } else if (constructed_) {
                // Constructed but not published: destroy safely.
                ::spsc::detail::destroy_at(std::launder(ptr_));
            }
        }

        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        explicit operator bool() const noexcept {
            return (p_ != nullptr) && (ptr_ != nullptr);
        }

        template <class... Args> pointer emplace(Args &&...args) {
            static_assert(
                std::is_constructible_v<T, Args &&...>,
                "[typed_pool::write_guard]: T must be constructible from Args...");
            SPSC_ASSERT(p_ && ptr_);
            SPSC_ASSERT(!constructed_);

            // Placement-new returns a fresh pointer to the newly created object.
            // Reusing an old T* may require std::launder per the standard rules.
            ptr_ = ::new (static_cast<void *>(ptr_)) T(std::forward<Args>(args)...);
            ptr_ = std::launder(ptr_);

            constructed_ = true;
            publish_on_destroy_ = true;
            return ptr_;
        }

        // Manual path: call this AFTER placement-new to mark the slot as containing
        // a live object.
        void mark_constructed() noexcept {
            SPSC_ASSERT(p_ && ptr_);
            SPSC_ASSERT(!constructed_);
            ptr_ = std::launder(ptr_);
            constructed_ = true;
        }

        // Arm publishing on scope exit.
        void arm_publish() noexcept {
            SPSC_ASSERT(p_ && ptr_);
            SPSC_ASSERT(constructed_ && "arm_publish() requires a constructed object; call mark_constructed() after placement-new");
            publish_on_destroy_ = true;
        }

        void publish_on_destroy() noexcept {
            SPSC_ASSERT(p_ && ptr_);
            SPSC_ASSERT(constructed_ && "publish_on_destroy() requires a constructed object; call mark_constructed() after placement-new");
            publish_on_destroy_ = true;
        }

        void commit() noexcept {
            if (p_ && ptr_) {
                SPSC_ASSERT(constructed_ && "write_guard::commit() publishing an unconstructed slot");
                if (constructed_) {
                    p_->publish();
                }
            }

            // After publish, consumer owns destruction.
            publish_on_destroy_ = false;
            constructed_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }
        void cancel() noexcept {
            // Cancel means: do NOT publish, and if constructed -> destroy.
            if (p_ && ptr_ && constructed_) {
                ::spsc::detail::destroy_at(std::launder(ptr_));
            }
            publish_on_destroy_ = false;
            constructed_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        typed_pool *p_{nullptr};
        pointer ptr_{nullptr};
        bool publish_on_destroy_{false};
        bool constructed_{false};
    };

    class read_guard {
    public:
        read_guard() noexcept = default;

        explicit read_guard(typed_pool &p) noexcept
            : p_(&p), ptr_(p.try_front()), active_(ptr_ != nullptr) {}

        read_guard(const read_guard &) = delete;
        read_guard &operator=(const read_guard &) = delete;

        read_guard(read_guard &&other) noexcept
            : p_(other.p_), ptr_(other.ptr_), active_(other.active_) {
            other.p_ = nullptr;
            other.ptr_ = nullptr;
            other.active_ = false;
        }

        read_guard &operator=(read_guard &&) = delete;

        ~read_guard() noexcept {
            if (active_ && p_) {
                p_->pop();
            }
        }

        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        [[nodiscard]] object_reference ref() const noexcept {
            SPSC_ASSERT(ptr_ != nullptr);
            return *ptr_;
        }
        [[nodiscard]] object_reference operator*() const noexcept { return ref(); }
        [[nodiscard]] pointer operator->() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return active_; }
        void commit() noexcept {
            if (active_ && p_) {
                p_->pop();
            }
            cancel();
        }

        void cancel() noexcept {
            active_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        typed_pool *p_{nullptr};
        pointer ptr_{nullptr};
        bool active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept {
        return write_guard(*this);
    }
    [[nodiscard]] read_guard scoped_read() noexcept { return read_guard(*this); }

private:
    // ------------------------------------------------------------------------------------------
    // Allocation helpers
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] static pointer alloc_one() {
        object_allocator_type oa{};
        return object_alloc_traits::allocate(oa, 1);
    }

    static void free_one(pointer p) noexcept {
        if (!p) {
            return;
        }
        object_allocator_type oa{};
        object_alloc_traits::deallocate(oa, p, 1);
    }

    // Helper: obtain pointer to LIVE object (for consumer / internal copy).
    // Launder is required because storage was reused via placement new.
    [[nodiscard]] RB_FORCEINLINE pointer
    object_ptr(size_type index) const noexcept {
        return std::launder(slots_[index]);
    }

    [[nodiscard]] bool allocate_static_storage() {
        static_assert(!kDynamic,
                      "allocate_static_storage() is for static Capacity only");

        slots_.fill(nullptr);

        size_type allocated = 0u;

        SPSC_TRY {
            for (; allocated < Capacity; ++allocated) {
                pointer p = alloc_one();
                if (RB_UNLIKELY(!p)) {
                    break;
                }
                slots_[allocated] = p;
            }
        }
        SPSC_CATCH_ALL {
            for (size_type i = 0; i < allocated; ++i) {
                free_one(slots_[i]);
                slots_[i] = nullptr;
            }
            SPSC_RETHROW;
        }

        if (RB_UNLIKELY(allocated != Capacity)) {
            for (size_type i = 0; i < allocated; ++i) {
                free_one(slots_[i]);
                slots_[i] = nullptr;
            }
            this->isAllocated_ = false;
            Base::clear();
            return false;
        }

        this->isAllocated_ = true;
        Base::clear();
        return true;
    }

    [[nodiscard]] bool reallocate_impl(size_type requested_depth) {
        static_assert(kDynamic, "reallocate_impl() is for dynamic Capacity only");
        static_assert(::spsc::cap::rb_is_pow2(::spsc::cap::RB_MAX_UNAMBIGUOUS),
                      "[typed_pool]: RB_MAX_UNAMBIGUOUS must be power of two");

        // Snapshot old state (defensive: fail closed on corrupted state)
        pointer const *old_slots = nullptr;
        size_type old_cap = 0u;
        size_type old_head = 0u;
        size_type old_tail = 0u;
        size_type old_size = 0u;

        if (is_valid()) {
            old_slots = data();
            old_cap = Base::capacity();
            old_head = Base::head();
            old_tail = Base::tail();
            old_size = static_cast<size_type>(old_head - old_tail);

            if (RB_UNLIKELY(old_cap != 0u && old_size > old_cap)) {
                destroy();
                old_slots = nullptr;
                old_cap = 0u;
                old_head = 0u;
                old_tail = 0u;
                old_size = 0u;
            }
        }

        // Explicit shrink-to-zero: release storage and clear queue.
        if (requested_depth == 0u) {
            destroy();
            return true;
        }

        // Normalize depth and enforce pow2 geometry
        size_type target_depth = requested_depth;

        if (target_depth < 2u) {
            target_depth = 2u;
        }
        if (target_depth > ::spsc::cap::RB_MAX_UNAMBIGUOUS) {
            target_depth = ::spsc::cap::RB_MAX_UNAMBIGUOUS;
        }
        target_depth = ::spsc::cap::rb_next_power2(target_depth);

        // Never shrink depth for a valid queue
        if (is_valid() && old_cap != 0u && target_depth < old_cap) {
            target_depth = old_cap;
        }

        // Optimization: if no growth needed, keep existing.
        if (is_valid() && old_cap != 0u && target_depth <= old_cap) {
            return true;
        }

        pointer *new_slots = nullptr;
        size_type allocated_extra = 0u;

        // Allocate pointer ring + extra storages first (strong-ish guarantee).
        SPSC_TRY {
            slot_allocator_type sa{};
            new_slots = slot_alloc_traits::allocate(sa, target_depth);
            if (RB_LIKELY(new_slots != nullptr)) {
                for (size_type i = 0; i < target_depth; ++i) {
                    new_slots[i] = nullptr;
                }
            }

            if (RB_LIKELY(new_slots != nullptr)) {
                for (size_type i = old_cap; i < target_depth; ++i) {
                    pointer p = alloc_one();
                    if (RB_UNLIKELY(!p)) {
                        break;
                    }
                    new_slots[i] = p;
                    ++allocated_extra;
                }
            }
        }
        SPSC_CATCH_ALL {
            // Rollback extras + ring
            if (new_slots != nullptr) {
                for (size_type i = old_cap; i < old_cap + allocated_extra; ++i) {
                    free_one(new_slots[i]);
                    new_slots[i] = nullptr;
                }
                slot_allocator_type sa{};
                slot_alloc_traits::deallocate(sa, new_slots, target_depth);
            }
            SPSC_RETHROW;
        }

        if (RB_UNLIKELY(new_slots == nullptr)) {
            return false;
        }
        if (RB_UNLIKELY((target_depth > old_cap) &&
                        (allocated_extra != (target_depth - old_cap)))) {
            for (size_type i = old_cap; i < old_cap + allocated_extra; ++i) {
                free_one(new_slots[i]);
                new_slots[i] = nullptr;
            }
            slot_allocator_type sa{};
            slot_alloc_traits::deallocate(sa, new_slots, target_depth);
            return false;
        }

        // Migrate pointers (active first in logical order, then the rest).
        size_type migrated = 0u;
        if (is_valid() && old_slots && old_cap) {
            // Active pointers in logical order
            const size_type old_mask = old_cap - 1u;
            for (size_type k = 0; k < old_size; ++k) {
                const size_type idx = static_cast<size_type>((old_tail + k) & old_mask);
                // Launder not needed for pointer migration itself, but safer if T* is
                // opaque
                new_slots[k] = old_slots[idx];
            }
            migrated = old_size;

            // Free pointers (anything not in the active physical ranges)
            const size_type tail_idx = static_cast<size_type>(old_tail & old_mask);
            const size_type to_end = static_cast<size_type>(old_cap - tail_idx);
            const size_type first_n = (old_size < to_end) ? old_size : to_end;
            const size_type second_n = static_cast<size_type>(old_size - first_n);

            auto in_active = [&](size_type i) noexcept {
                const bool in_first =
                    (first_n != 0u) && (i >= tail_idx) && (i < (tail_idx + first_n));
                const bool in_second = (second_n != 0u) && (i < second_n);
                return in_first || in_second;
            };

            size_type pos = migrated;
            for (size_type i = 0; i < old_cap; ++i) {
                if (in_active(i)) {
                    continue;
                }
                new_slots[pos++] = old_slots[i];
            }

            SPSC_ASSERT(pos == old_cap);
        } else {
            // No old storage: the ring is just the newly allocated pointers.
            migrated = 0u;
        }

        // Drop old pointer ring (storages stay alive, now referenced by new ring)
        if (slots_ != nullptr && old_cap != 0u) {
            slot_allocator_type sa{};
            slot_alloc_traits::deallocate(sa, slots_, old_cap);
        }

        // Commit new storage
        slots_ = new_slots;

        const bool ok = Base::init(target_depth, migrated, 0u);
        if (RB_UNLIKELY(!ok)) {
            // Should not happen, but keep the object safe.
            pointer *ptr = slots_;
            slots_ = nullptr;
            (void)Base::init(0u);

            // Free all storages.
            object_allocator_type oa{};
            for (size_type i = 0; i < target_depth; ++i) {
                if (ptr[i]) {
                    object_alloc_traits::deallocate(oa, ptr[i], 1);
                }
            }

            slot_allocator_type sa{};
            slot_alloc_traits::deallocate(sa, ptr, target_depth);
            return false;
        }

        return true;
    }

    [[nodiscard]] bool copy_from(const typed_pool &other) {
        static_assert(std::is_copy_constructible_v<T>,
                      "[typed_pool]: T must be copy-constructible for copying");

        if (!other.is_valid()) {
            destroy();
            return true;
        }

        const size_type target_depth = other.capacity();

        // Allocate fresh storage (do not reuse old pointers).
        typed_pool tmp;

        if constexpr (kDynamic) {
            if (!tmp.resize(target_depth)) {
                destroy();
                return false;
            }
        } else {
            (void)target_depth;
            if (!tmp.is_valid() && !tmp.allocate_static_storage()) {
                destroy();
                return false;
            }
        }

        if (!tmp.is_valid()) {
            destroy();
            return false;
        }

        const size_type sz = other.size();
        const size_type cap = tmp.capacity();
        if (RB_UNLIKELY(sz > cap)) {
            // Corrupted other state: keep tmp empty.
            tmp.Base::clear();
            swap(tmp);
            return true;
        }

        // Copy-construct live objects into tmp[0..sz-1]
        size_type constructed = 0u;
        SPSC_TRY {
            const size_type m = other.Base::mask();
            const size_type t = other.Base::tail();

            for (; constructed < sz; ++constructed) {
                // Must use object_ptr() on source to launder the pointer
                pointer src = other.object_ptr((t + constructed) & m);
                pointer dst = tmp.data()[constructed];
                ::new (static_cast<void *>(dst)) T(*src);
            }
        }
        SPSC_CATCH_ALL {
            for (size_type k = 0; k < constructed; ++k) {
                detail::destroy_at(tmp.data()[k]);
            }
            // tmp will free memory in its destructor.
            SPSC_RETHROW;
        }

        // Set geometry: [0..sz-1] live, tail=0
        tmp.Base::clear();
        tmp.Base::set_tail(0u);
        tmp.Base::set_head(sz);
        tmp.Base::sync_cache();

        swap(tmp);
        return true;
    }

    void move_from(typed_pool &&other) noexcept {
        if constexpr (kDynamic) {
            slots_ = other.slots_;
            other.slots_ = nullptr;

            const size_type cap = other.Base::capacity();
            const size_type head = other.Base::head();
            const size_type tail = other.Base::tail();

            if (cap != 0u) {
                (void)Base::init(cap, head, tail);
            } else {
                (void)Base::init(0u);
            }
            (void)other.Base::init(0u);
        } else {
            slots_ = other.slots_;
            this->isAllocated_ = other.isAllocated_;

            Base::set_head(other.Base::head());
            Base::set_tail(other.Base::tail());
            Base::sync_cache();

            other.slots_.fill(nullptr);
            other.isAllocated_ = false;
            other.Base::clear();
        }
    }

private:
    // ------------------------------------------------------------------------------------------
    // Storage
    // ------------------------------------------------------------------------------------------
    slots_storage slots_{};
};

} // namespace spsc

#endif /* SPSC_TYPED_POOL_HPP_ */
