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
    //
    // NOTE:
    // - Regions are meant for bulk SPSC operations.
    // - The underlying ring stores raw slot pointers (T*), which refer to storage for ONE T.
    // - Reusing the same storage address for placement-new may require std::launder before dereference.
    //
    // To keep the API "hard to misuse", regions expose a launder-on-access view instead of raw T* arrays.
    // This prevents accidental UB from naive `slts[i]->field` on a potentially re-used lifetime.

    class read_region_view {
    public:
        using value_type = T;
        using pointer = T*;

        constexpr read_region_view() noexcept = default;
        constexpr read_region_view(pointer const* slts, size_type count) noexcept
            : slts_(slts), count_(count) {}

        [[nodiscard]] constexpr size_type size() const noexcept { return count_; }
        [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0u; }

        // Returns a laundered pointer to the live object at position i.
        [[nodiscard]] constexpr pointer ptr(size_type i) const noexcept {
            return std::launder(slts_[i]);
        }

        // Convenience: laundered pointer access.
        [[nodiscard]] constexpr pointer operator[](size_type i) const noexcept {
            return ptr(i);
        }

#if SPSC_HAS_SPAN
        // Exposes the raw slot-pointer span for advanced users.
        // Dereferencing raw pointers directly is on the user; prefer ptr()/iterators.
        [[nodiscard]] std::span<pointer const> raw_span() const noexcept {
            return {slts_, count_};
        }
#endif /* SPSC_HAS_SPAN */

        class iterator {
        public:
            using difference_type = std::ptrdiff_t;
            using value_type = T;
            using pointer = T*;
            using reference = T&;
            using iterator_category = std::random_access_iterator_tag;

            constexpr iterator() noexcept = default;
            constexpr explicit iterator(pointer const* p) noexcept : p_(p) {}

            [[nodiscard]] constexpr reference operator*() const noexcept {
                return *std::launder(*p_);
            }
            [[nodiscard]] constexpr pointer operator->() const noexcept {
                return std::launder(*p_);
            }

            constexpr iterator& operator++() noexcept { ++p_; return *this; }
            constexpr iterator operator++(int) noexcept { iterator tmp(*this); ++p_; return tmp; }
            constexpr iterator& operator--() noexcept { --p_; return *this; }
            constexpr iterator operator--(int) noexcept { iterator tmp(*this); --p_; return tmp; }

            constexpr iterator& operator+=(difference_type d) noexcept { p_ += d; return *this; }
            constexpr iterator& operator-=(difference_type d) noexcept { p_ -= d; return *this; }

            [[nodiscard]] constexpr iterator operator+(difference_type d) const noexcept { return iterator(p_ + d); }
            [[nodiscard]] constexpr iterator operator-(difference_type d) const noexcept { return iterator(p_ - d); }

            [[nodiscard]] constexpr difference_type operator-(iterator rhs) const noexcept { return p_ - rhs.p_; }

            [[nodiscard]] constexpr bool operator==(iterator rhs) const noexcept { return p_ == rhs.p_; }
            [[nodiscard]] constexpr bool operator!=(iterator rhs) const noexcept { return p_ != rhs.p_; }
            [[nodiscard]] constexpr bool operator<(iterator rhs) const noexcept { return p_ < rhs.p_; }

        private:
            pointer const* p_{nullptr};
        };

        [[nodiscard]] constexpr iterator begin() const noexcept { return iterator(slts_); }
        [[nodiscard]] constexpr iterator end() const noexcept { return iterator(slts_ + count_); }

    private:
        pointer const* slts_{nullptr};
        size_type count_{0u};
    };

    class write_region_view {
    public:
        using value_type = T;
        using pointer = T*;

        constexpr write_region_view() noexcept = default;
        constexpr write_region_view(pointer* slts, size_type count) noexcept
            : slts_(slts), count_(count) {}

        [[nodiscard]] constexpr size_type size() const noexcept { return count_; }
        [[nodiscard]] constexpr bool empty() const noexcept { return count_ == 0u; }

        // Returns a laundered pointer to the slot storage at position i.
        // The object lifetime begins only after placement-new (construct_at/emplace).
        [[nodiscard]] constexpr pointer storage_ptr(size_type i) const noexcept {
            return std::launder(slts_[i]);
        }

        // Convenience alias.
        [[nodiscard]] constexpr pointer operator[](size_type i) const noexcept {
            return storage_ptr(i);
        }

        template <class... Args>
        [[nodiscard]] pointer construct_at(size_type i, Args&&... args) const {
            static_assert(std::is_constructible_v<T, Args&&...>,
                          "[typed_pool::write_region_view]: T must be constructible from Args...");
            // Construct a T in the slot storage and return a laundered pointer to the live object.
            pointer p = storage_ptr(i);
            p = ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
            return std::launder(p);
        }

#if SPSC_HAS_SPAN
        // Exposes the raw slot-pointer span for advanced users.
        // Writing via raw pointers is on the user; prefer construct_at().
        [[nodiscard]] std::span<pointer> raw_span() const noexcept {
            return {slts_, count_};
        }
#endif /* SPSC_HAS_SPAN */

        class iterator {
        public:
            using difference_type = std::ptrdiff_t;
            using value_type = T;
            using pointer = T*;
            using reference = T&;
            using iterator_category = std::random_access_iterator_tag;

            constexpr iterator() noexcept = default;
            constexpr explicit iterator(pointer* p) noexcept : p_(p) {}

            [[nodiscard]] constexpr reference operator*() const noexcept {
                return *std::launder(*p_);
            }
            [[nodiscard]] constexpr pointer operator->() const noexcept {
                return std::launder(*p_);
            }

            constexpr iterator& operator++() noexcept { ++p_; return *this; }
            constexpr iterator operator++(int) noexcept { iterator tmp(*this); ++p_; return tmp; }

            [[nodiscard]] constexpr bool operator==(iterator rhs) const noexcept { return p_ == rhs.p_; }
            [[nodiscard]] constexpr bool operator!=(iterator rhs) const noexcept { return p_ != rhs.p_; }

        private:
            pointer* p_{nullptr};
        };

        [[nodiscard]] constexpr iterator begin() const noexcept { return iterator(slts_); }
        [[nodiscard]] constexpr iterator end() const noexcept { return iterator(slts_ + count_); }

    private:
        pointer* slts_{nullptr};
        size_type count_{0u};
    };

    struct regions {
        read_region_view first{};
        read_region_view second{};
        size_type total{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return total == 0u; }
    };

    struct write_regions {
        write_region_view first{};
        write_region_view second{};
        size_type total{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return total == 0u; }
    };

    // ------------------------------------------------------------------------------------------
    // Constructors / Destructor
    // ------------------------------------------------------------------------------------------

    typed_pool() noexcept {
        if constexpr (!kDynamic) {
            init_static();
        }
    }

    ~typed_pool() noexcept { destroy(); }

    typed_pool(const typed_pool &other) { copy_from(other); }

    typed_pool &operator=(const typed_pool &other) {
        if (this != &other) {
            destroy();
            copy_from(other);
        }
        return *this;
    }

    typed_pool(typed_pool &&other) noexcept { move_from(std::move(other)); }

    typed_pool &operator=(typed_pool &&other) noexcept {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    // ------------------------------------------------------------------------------------------
    // Basic Properties
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] bool is_valid() const noexcept {
        if constexpr (kDynamic) {
            return slots_ != nullptr && Base::capacity() != 0u;
        } else {
            return this->isAllocated_;
        }
    }

    [[nodiscard]] size_type capacity() const noexcept { return Base::capacity(); }
    [[nodiscard]] size_type size() const noexcept { return static_cast<size_type>(Base::size()); }
    [[nodiscard]] size_type free() const noexcept { return static_cast<size_type>(Base::free()); }

    [[nodiscard]] bool empty() const noexcept { return Base::empty(); }
    [[nodiscard]] bool full() const noexcept { return Base::full(); }

    [[nodiscard]] bool can_write(const size_type n = 1u) const noexcept {
        return Base::can_write(n);
    }
    [[nodiscard]] bool can_read(const size_type n = 1u) const noexcept {
        return Base::can_read(n);
    }

    [[nodiscard]] static constexpr size_type buffer_align() noexcept {
        return static_cast<size_type>(alignof(T));
    }

    [[nodiscard]] size_type buffer_size() const noexcept {
        if (!is_valid()) {
            return 0u;
        }
        // Total object bytes allocated (slots * sizeof(T)).
        // This is the size of the backing object storages, not the pointer ring.
        return static_cast<size_type>(Base::capacity()) * static_cast<size_type>(sizeof(T));
    }

    [[nodiscard]] base_allocator_type get_allocator() const noexcept {
        return base_allocator_type{};
    }

    // Expose ring pointer array (T**) for internal uses/tests.
    [[nodiscard]] pointer *data() noexcept {
        if constexpr (kDynamic) {
            return slots_;
        } else {
            return slots_.data();
        }
    }

    [[nodiscard]] pointer const *data() const noexcept {
        if constexpr (kDynamic) {
            return slots_;
        } else {
            return slots_.data();
        }
    }

    // ------------------------------------------------------------------------------------------
    // Iterators
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] iterator begin() noexcept {
        using it = iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return it(nullptr, 0u, 0u);
        }

        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);
        const size_type m = Base::mask();
        return it(data(), m, t, h);
    }

    [[nodiscard]] iterator end() noexcept {
        using it = iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return it(nullptr, 0u, 0u);
        }

        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);
        const size_type m = Base::mask();
        return it(data(), m, h, h);
    }

    [[nodiscard]] const_iterator begin() const noexcept { return cbegin(); }
    [[nodiscard]] const_iterator end() const noexcept { return cend(); }

    [[nodiscard]] const_iterator cbegin() const noexcept {
        using it = const_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return it(nullptr, 0u, 0u);
        }

        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);
        const size_type m = Base::mask();
        return it(data(), m, t, h);
    }

    [[nodiscard]] const_iterator cend() const noexcept {
        using it = const_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return it(nullptr, 0u, 0u);
        }

        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);
        const size_type m = Base::mask();
        return it(data(), m, h, h);
    }

    [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    [[nodiscard]] const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    // ------------------------------------------------------------------------------------------
    // Snapshots
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] snapshot make_snapshot() noexcept {
        using it = snapshot_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return snapshot(it(nullptr, 0u, 0u), it(nullptr, 0u, 0u));
        }

        // Use a validated "used" snapshot to avoid impossible head<tail ranges under atomics.
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

        const size_type t = static_cast<size_type>(Base::tail());
        const size_type used = static_cast<size_type>(Base::size());
        const size_type h = static_cast<size_type>(t + used);
        const size_type m = Base::mask();
        return const_snapshot(it(data(), m, t), it(data(), m, h));
    }

    [[nodiscard]] bool try_consume(const snapshot &snap) noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return false;
        }

        // Empty snapshot always "consumes" trivially.
        if (snap.empty()) {
            return true;
        }

        // Validate snapshot pointers match current backing.
        if (RB_UNLIKELY(snap.begin().data() != data()) || RB_UNLIKELY(snap.end().data() != data())) {
            return false;
        }

        const size_type cap = Base::capacity();

        // Validate snapshot indices monotonic and within capacity.
        const size_type snap_tail = static_cast<size_type>(snap.begin().index());
        const size_type snap_head = static_cast<size_type>(snap.end().index());
        if (RB_UNLIKELY(snap_head < snap_tail)) {
            return false;
        }

        const size_type cur_tail = static_cast<size_type>(Base::tail());
        const size_type cur_head = static_cast<size_type>(Base::head());

        // Snapshot must start at current tail.
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

    [[nodiscard]] write_regions
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

        write_regions r{};
        r.first = write_region_view(data() + wi, first_n);

        const size_type second_n = static_cast<size_type>(total - first_n);
        r.second = write_region_view((second_n != 0u) ? data() : nullptr, second_n);

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
        r.first = read_region_view(data() + ri, first_n);

        const size_type second_n = static_cast<size_type>(total - first_n);
        r.second = read_region_view((second_n != 0u) ? data() : nullptr, second_n);

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
        pop(1u);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop() noexcept {
        if (RB_UNLIKELY(empty())) {
            return false;
        }
        pop(1u);
        return true;
    }

    RB_FORCEINLINE void pop(const size_type n) noexcept {
        SPSC_ASSERT(can_read(n));
        if constexpr (!std::is_trivially_destructible_v<object_type>) {
            const size_type cap = Base::capacity();
            const size_type mask = cap - 1u;
            const size_type tail = static_cast<size_type>(Base::tail());

            for (size_type k = 0; k < n; ++k) {
                pointer p = std::launder(data()[(tail + k) & mask]);
                detail::destroy_at(p);
            }
        }
        Base::advance_tail(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) {
            return false;
        }
        pop(n);
        return true;
    }

    // Random access by logical position [0..size-1].
    [[nodiscard]] pointer operator[](const size_type pos) noexcept {
        SPSC_ASSERT(pos < size());
        const size_type cap = Base::capacity();
        const size_type mask = cap - 1u;
        const size_type idx = (static_cast<size_type>(Base::tail()) + pos) & mask;
        return object_ptr(idx);
    }

    [[nodiscard]] const_pointer operator[](const size_type pos) const noexcept {
        SPSC_ASSERT(pos < size());
        const size_type cap = Base::capacity();
        const size_type mask = cap - 1u;
        const size_type idx = (static_cast<size_type>(Base::tail()) + pos) & mask;
        return object_ptr(idx);
    }

    // ------------------------------------------------------------------------------------------
    // Mutating Operations (non-concurrent)
    // ------------------------------------------------------------------------------------------

    void clear() noexcept {
        if (RB_UNLIKELY(!is_valid())) {
            return;
        }

        if constexpr (!std::is_trivially_destructible_v<object_type>) {
            while (!empty()) {
                pop();
            }
        } else {
            Base::clear();
        }
    }

    void swap(typed_pool &other) noexcept {
        if (this == &other) {
            return;
        }

        // Non-concurrent operation.
        // Both pools must be in a valid, non-used-by-threads state.
        using std::swap;

        if constexpr (kDynamic) {
            swap(slots_, other.slots_);
            Base::swap_base(other);
        } else {
            // For static, swap pointers and base state. Storage is fixed in-place.
            swap(slots_, other.slots_);
            swap(this->isAllocated_, other.isAllocated_);
            Base::swap_base(other);
        }
    }

    // Dynamic-only: resize capacity. Static-only: returns false for mismatch.
    [[nodiscard]] bool resize(const size_type new_cap) {
        if constexpr (!kDynamic) {
            if (new_cap != Capacity) {
                return false;
            }
            if (!is_valid()) {
                init_static();
            }
            return true;
        } else {
            return resize_dynamic(new_cap);
        }
    }

    void destroy() noexcept {
        if constexpr (kDynamic) {
            if (!slots_) {
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

            // Free all object storages
            object_allocator_type oa{};
            for (size_type i = 0; i < cap; ++i) {
                if (slots_[i]) {
                    object_alloc_traits::deallocate(oa, slots_[i], 1);
                    slots_[i] = nullptr;
                }
            }

            // Free pointer ring
            slot_allocator_type sa{};
            slot_alloc_traits::deallocate(sa, slots_, cap);

            slots_ = nullptr;
            Base::clear();
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
        [[nodiscard]] const_pointer cget() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return active_ && (ptr_ != nullptr); }

        void commit() noexcept {
            if (active_ && p_) {
                p_->pop();
            }
            active_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

        void cancel() noexcept {
            // Cancel means: keep the element, do not pop.
            active_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        typed_pool *p_{nullptr};
        pointer ptr_{nullptr};
        bool active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept { return write_guard(*this); }
    [[nodiscard]] read_guard scoped_read() noexcept { return read_guard(*this); }

private:
    // ------------------------------------------------------------------------------------------
    // Internal helpers
    // ------------------------------------------------------------------------------------------

    [[nodiscard]] RB_FORCEINLINE pointer object_ptr(const size_type masked_index) noexcept {
        // Slots store pointers to storage; dereferencing a reused object lifetime requires launder.
        return std::launder(data()[masked_index]);
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer object_ptr(const size_type masked_index) const noexcept {
        return std::launder(data()[masked_index]);
    }

    void init_static() noexcept {
        // Allocate one storage per slot for static capacity.
        object_allocator_type oa{};
        for (size_type i = 0; i < Capacity; ++i) {
            slots_[i] = object_alloc_traits::allocate(oa, 1);
        }

        this->isAllocated_ = true;
        Base::init(Capacity);
        Base::clear();
    }

    [[nodiscard]] bool resize_dynamic(const size_type new_cap) {
        if (new_cap == 0u) {
            destroy();
            return true;
        }

        if (new_cap < 2u || !::spsc::cap::rb_is_pow2(new_cap) || new_cap > ::spsc::cap::RB_MAX_UNAMBIGUOUS) {
            return false;
        }

        if (!slots_) {
            // Fresh init.
            slot_allocator_type sa{};
            object_allocator_type oa{};

            pointer *new_slots = slot_alloc_traits::allocate(sa, new_cap);
            for (size_type i = 0; i < new_cap; ++i) {
                new_slots[i] = object_alloc_traits::allocate(oa, 1);
            }

            slots_ = new_slots;
            Base::init(new_cap);
            Base::clear();
            return true;
        }

        const size_type old_cap = Base::capacity();
        if (new_cap <= old_cap) {
            // Grow-only policy.
            return (new_cap == old_cap);
        }

        // Migrate pointers in logical order into a new pointer ring.
        slot_allocator_type sa{};
        object_allocator_type oa{};

        pointer *new_slots = slot_alloc_traits::allocate(sa, new_cap);

        const size_type head = Base::head();
        const size_type tail = Base::tail();
        const size_type used = static_cast<size_type>(head - tail);
        const size_type old_mask = old_cap - 1u;

        // Copy existing slot pointers in logical order.
        for (size_type k = 0; k < used; ++k) {
            new_slots[k] = slots_[(tail + k) & old_mask];
        }

        // Copy remaining unused slot pointers after used range.
        // We can keep the old "free" pointers too, preserving already allocated storages.
        for (size_type k = used; k < old_cap; ++k) {
            new_slots[k] = slots_[k & old_mask];
        }

        // Allocate new storages for the extra capacity.
        for (size_type i = old_cap; i < new_cap; ++i) {
            new_slots[i] = object_alloc_traits::allocate(oa, 1);
        }

        // Free old pointer ring (NOT the storages).
        slot_alloc_traits::deallocate(sa, slots_, old_cap);

        slots_ = new_slots;

        // Re-init base to new capacity and remap head/tail.
        Base::init(new_cap);
        Base::set_tail(0u);
        Base::set_head(used);
        return true;
    }

    void copy_from(const typed_pool &other) {
        if (!other.is_valid()) {
            // Leave invalid.
            if constexpr (kDynamic) {
                slots_ = nullptr;
            } else {
                for (auto &p : slots_) {
                    p = nullptr;
                }
                this->isAllocated_ = false;
            }
            Base::clear();
            return;
        }

        const size_type cap = other.capacity();

        if constexpr (kDynamic) {
            // Allocate new ring and storages.
            slot_allocator_type sa{};
            object_allocator_type oa{};
            slots_ = slot_alloc_traits::allocate(sa, cap);
            for (size_type i = 0; i < cap; ++i) {
                slots_[i] = object_alloc_traits::allocate(oa, 1);
            }

            Base::init(cap);
        } else {
            if (!this->isAllocated_) {
                init_static();
            }
        }

        // Copy objects in logical order.
        const size_type used = other.size();
        for (size_type i = 0; i < used; ++i) {
            const_pointer src = other[i];
            pointer dst = data()[i];
            ::new (static_cast<void *>(dst)) T(*src);
        }

        Base::set_tail(0u);
        Base::set_head(used);
    }

    void move_from(typed_pool &&other) noexcept {
        if (!other.is_valid()) {
            if constexpr (kDynamic) {
                slots_ = nullptr;
            } else {
                for (auto &p : slots_) {
                    p = nullptr;
                }
                this->isAllocated_ = false;
            }
            Base::clear();
            return;
        }

        if constexpr (kDynamic) {
            slots_ = other.slots_;
            Base::move_base(std::move(other));
            other.slots_ = nullptr;
            other.Base::clear();
        } else {
            // Static: steal pointer array and base state.
            slots_ = other.slots_;
            this->isAllocated_ = other.isAllocated_;
            Base::move_base(std::move(other));

            // Invalidate other.
            for (auto &p : other.slots_) {
                p = nullptr;
            }
            other.isAllocated_ = false;
            other.Base::clear();
        }
    }

private:
    slots_storage slots_{};
};

} // namespace spsc

#endif /* SPSC_TYPED_POOL_HPP_ */
