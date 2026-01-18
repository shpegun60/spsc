/*
 * spsc_pool.hpp
 *
 * Modernized SPSC "pool" of fixed-size raw buffers.
 * Structure mirrors spsc::fifo for architectural consistency.
 *
 * The ring stores 'void*' pointers.
 * Each pointer refers to a fixed-size buffer of 'buffer_size()' bytes.
 *
 * Design goals:
 * - Modern:      constexpr, [[nodiscard]], optional span-based bulk API.
 * - Safe:        Dynamic pool can be "invalid"; safe wrappers guard that.
 * - Fast:        Unchecked hot-paths; bulk regions expose contiguous spans.
 * - Hardened:    Corruption guards (used > cap) match fifo's "unbreakable" behavior.
 *
 * Concurrency model:
 * - Single Producer / Single Consumer (wait-free / lock-free depends on Policy).
 * - Producer:    claim, publish, push (memcpy wrapper).
 * - Consumer:    front, pop, consume, claim_read.
 *
 * MEMORY LAYOUT NOTE:
 * - pop() does NOT free or destroy anything (buffers are persistent).
 * - resize(), destroy(), swap(), move/assign, clear() are NOT concurrent with push/pop.
 */

#ifndef SPSC_POOL_HPP_
#define SPSC_POOL_HPP_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <type_traits>
#include <utility>

// Base and utility includes
#include "base/SPSCbase.hpp"        // ::spsc::SPSCbase<Capacity, Policy>, reg
#include "base/spsc_alloc.hpp"      // ::spsc::alloc::default_alloc
#include "base/spsc_snapshot.hpp"   // ::spsc::snapshot_view, ::spsc::snapshot_traits
#include "base/spsc_tools.hpp"      // RB_FORCEINLINE, RB_UNLIKELY, SPSC_* macros (also handles <span>)

namespace spsc {

/* =======================================================================
 * pool<Capacity, Policy, Alloc>
 *
 * Owning Single-Producer Single-Consumer buffer pool.
 * Stores 'void*' pointers to buffers of 'buffer_size()' bytes.
 * Can be static-depth (Capacity != 0) or dynamic-depth (Capacity == 0).
 *
 * Safety notes:
 * - Invalid pool (no storage) behaves like a full+empty queue: it refuses producer ops.
 * - claim_read/claim_write include corruption guards (used > cap) to prevent underflow/UB.
 * ======================================================================= */
template<
    reg Capacity    = 0,
    typename Policy = ::spsc::policy::default_policy,
    typename Alloc  = ::spsc::alloc::default_alloc
    >
class pool : private ::spsc::SPSCbase<Capacity, Policy>
{
    static constexpr bool kDynamic = (Capacity == 0);
    using Base = ::spsc::SPSCbase<Capacity, Policy>;

public:
    // ------------------------------------------------------------------------------------------
    // Type Definitions
    // ------------------------------------------------------------------------------------------
    using value_type      = void*;
    using pointer         = void*;
    using const_pointer   = const void*;
    using reference       = pointer&;
    using const_reference = pointer const&;

    using size_type       = reg;
    using difference_type = std::ptrdiff_t;

    // Allocator types
    using base_allocator_type = Alloc;

    // Allocator for the ring of pointers (void*)
    using slot_allocator_type = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<pointer>;
    using slot_alloc_traits   = std::allocator_traits<slot_allocator_type>;
    using slot_pointer        = typename slot_alloc_traits::pointer;

    // Allocator for the actual data buffers (std::byte)
    using byte_allocator_type = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<std::byte>;
    using byte_alloc_traits   = std::allocator_traits<byte_allocator_type>;
    using byte_pointer        = typename byte_alloc_traits::pointer;

    // Storage for ring slots
    using static_slots  = std::array<pointer, Capacity>;
    using dynamic_slots = pointer*;
    using slots_storage = std::conditional_t<kDynamic, dynamic_slots, static_slots>;

    // Iterator types (same iterator engine as fifo)
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
    static_assert(std::is_default_constructible_v<base_allocator_type>,
                  "[spsc::pool]: allocator must be default-constructible (used by get_allocator()).");
    static_assert(!kDynamic || slot_alloc_traits::is_always_equal::value,
                  "[spsc::pool]: dynamic pool requires always_equal allocator (stateless).");
    static_assert(!kDynamic || std::is_default_constructible_v<slot_allocator_type>,
                  "[spsc::pool]: dynamic pool requires default-constructible allocator.");
    static_assert(!kDynamic || std::is_same_v<slot_pointer, pointer*>,
                  "[spsc::pool]: dynamic pool requires allocator pointer type void** (raw).");
    static_assert(std::is_same_v<byte_pointer, std::byte*>,
                  "[spsc::pool]: requires byte allocator pointer type std::byte* (raw).");
    static_assert(std::numeric_limits<counter_value>::digits >= 2,
                  "[spsc::pool]: counter type is too narrow.");
    static_assert(::spsc::cap::RB_MAX_UNAMBIGUOUS <= (counter_value(1) << (std::numeric_limits<counter_value>::digits - 1)),
                  "[spsc::pool]: RB_MAX_UNAMBIGUOUS exceeds counter unambiguous range.");
    static_assert(std::is_same_v<counter_value, geometry_value>,
                  "[spsc::pool]: policy counter/geometry value types must match.");
    static_assert(std::is_unsigned_v<counter_value>,
                  "[spsc::pool]: policy counter/geometry value type must be unsigned.");
    static_assert(sizeof(counter_value) >= sizeof(size_type),
                  "[spsc::pool]: counter_type::value_type must be at least as wide as reg.");
    static_assert(std::allocator_traits<byte_allocator_type>::is_always_equal::value,
                  "[spsc::pool]: byte allocator must be always_equal (stateless).");
    static_assert(std::is_default_constructible_v<byte_allocator_type>,
                  "[spsc::pool]: byte allocator must be default-constructible.");
    static_assert(std::is_unsigned_v<size_type>,
                  "[spsc::pool]: reg (size_type) must be unsigned.");
    static_assert(std::is_same_v<value_type, pointer>,
                  "[spsc::pool]: value_type must match pointer.");
    static_assert(std::is_trivially_copyable_v<pointer>,
                  "[spsc::pool]: pointer must be trivially copyable.");
    static_assert(kDynamic || (Capacity >= 2),
                  "[spsc::pool]: static Capacity must be >= 2.");
    static_assert(kDynamic || ::spsc::cap::rb_is_pow2(Capacity),
                  "[spsc::pool]: static Capacity must be power of two.");
    static_assert(kDynamic || (Capacity <= ::spsc::cap::RB_MAX_UNAMBIGUOUS),
                  "[spsc::pool]: static Capacity exceeds RB_MAX_UNAMBIGUOUS.");

    // ------------------------------------------------------------------------------------------
    // Region Structs (Bulk Operations)
    // ------------------------------------------------------------------------------------------
    struct region {
        pointer const* ptr{nullptr};  // read-only access to slot array (prevents ownership corruption)
        size_type      count{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }
#if SPSC_HAS_SPAN
        [[nodiscard]] std::span<pointer const> span() const noexcept { return {ptr, count}; }
#endif /* SPSC_HAS_SPAN */
    };

    struct regions {
        region    first{};
        region    second{};
        size_type total{0u};

        [[nodiscard]] constexpr bool empty() const noexcept { return total == 0u; }
    };

    // ------------------------------------------------------------------------------------------
    // Constructors / Destructor
    // ------------------------------------------------------------------------------------------
    pool() = default;

    // Dynamic: request initial depth + buffer size.
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    explicit pool(const size_type depth, const size_type buffer_size) {
        (void)resize(depth, buffer_size);
    }

    // Static: request buffer size (depth is Capacity).
    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    explicit pool(const size_type buffer_size) {
        (void)resize(buffer_size);
    }

    ~pool() noexcept { destroy(); }

    // Copy semantics
    pool(const pool& other)
        : Base() {
        (void)copy_from(other);
    }

    pool& operator=(const pool& other) {
        if (this == &other) { return *this; }

        // Strong-ish guarantee: if 'other' is valid but we can't allocate, keep *this unchanged.
        pool tmp(other);
        if (other.is_valid() && !tmp.is_valid()) { return *this; }
        swap(tmp);
        return *this;
    }

    // Move semantics
    pool(pool&& other) noexcept
        : Base() {
        move_from(std::move(other));
    }

    pool& operator=(pool&& other) noexcept {
        if (this != &other) {
            destroy();
            move_from(std::move(other));
        }
        return *this;
    }

    void swap(pool& other) noexcept {
        if (this == &other) { return; }

        const size_type a_cap  = Base::capacity();
        const size_type a_head = Base::head();
        const size_type a_tail = Base::tail();
        const size_type a_bs   = bufferSize_.load();

        const size_type b_cap  = other.Base::capacity();
        const size_type b_head = other.Base::head();
        const size_type b_tail = other.Base::tail();
        const size_type b_bs   = other.bufferSize_.load();

        if constexpr (kDynamic) {
            // Keep invariants: storage first, then geometry.
            SPSC_ASSERT((slots_ == nullptr) == (a_cap == 0u));
            SPSC_ASSERT((other.slots_ == nullptr) == (b_cap == 0u));

            std::swap(slots_, other.slots_);

            // Manual swap for geometry_type (std::swap fails if type is atomic)
            bufferSize_.store(b_bs);
            other.bufferSize_.store(a_bs);

            const bool ok1 = (b_cap != 0u) ? Base::init(b_cap, b_head, b_tail)
                                           : Base::init(0u);
            const bool ok2 = (a_cap != 0u) ? other.Base::init(a_cap, a_head, a_tail)
                                           : other.Base::init(0u);

            if (RB_UNLIKELY(!ok1 || !ok2)) {
                // [FIXED ROLLBACK LOGIC]
                // 1. Roll back storage pointer
                std::swap(slots_, other.slots_);

                // 2. Roll back buffer sizes (force restore original values)
                bufferSize_.store(a_bs);
                other.bufferSize_.store(b_bs);

                // 3. Roll back Base geometry
                const bool rb1 = (a_cap != 0u) ? Base::init(a_cap, a_head, a_tail)
                                               : Base::init(0u);
                const bool rb2 = (b_cap != 0u) ? other.Base::init(b_cap, b_head, b_tail)
                                               : other.Base::init(0u);

                SPSC_ASSERT(rb1 && rb2);
                (void)rb1; (void)rb2;
            }

            // Keep invariant: detached dynamic pool has zero geometry.
            if (slots_ == nullptr) { (void)Base::init(0u); }
            if (other.slots_ == nullptr) { (void)other.Base::init(0u); }
        } else {
            // Static-depth: no geometry init, just swap storage + counters.
            std::swap(slots_, other.slots_);

            // Manual swap for geometry_type
            bufferSize_.store(b_bs);
            other.bufferSize_.store(a_bs);

            // Non-concurrent by contract: restore state via Base::init() to keep shadow caches consistent.
            const bool ok1 = Base::init(b_head, b_tail);
            const bool ok2 = other.Base::init(a_head, a_tail);
            SPSC_ASSERT(ok1 && ok2);
            (void)ok1; (void)ok2;
        }
    }

    friend void swap(pool& a, pool& b) noexcept { a.swap(b); }

    // ------------------------------------------------------------------------------------------
    // Validity & Safe Introspection
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool is_valid() const noexcept {
        if (RB_UNLIKELY(bufferSize_.load() == 0u)) { return false; }
        if constexpr (kDynamic) {
            return (slots_ != nullptr) && (Base::capacity() != 0u);
        } else {
            // Static pool is considered valid only after buffers are allocated (bufferSize_ != 0).
            return true;
        }
    }

    [[nodiscard]] size_type capacity() const noexcept { return is_valid() ? Base::capacity() : 0u; }
    [[nodiscard]] size_type size()     const noexcept { return is_valid() ? Base::size() : 0u; }
    [[nodiscard]] bool      empty()    const noexcept { return !is_valid() || Base::empty(); }
    [[nodiscard]] bool      full()     const noexcept { return !is_valid() || Base::full(); }
    [[nodiscard]] size_type free()     const noexcept { return is_valid() ? Base::free() : 0u; }
    [[nodiscard]] size_type buffer_size() const noexcept { return bufferSize_.load(); }
    [[nodiscard]] bool can_write(size_type n = 1u) const noexcept { return is_valid() && Base::can_write(n); }
    [[nodiscard]] bool can_read (size_type n = 1u) const noexcept { return is_valid() && Base::can_read(n); }
    [[nodiscard]] size_type write_size() const noexcept { return is_valid() ? Base::write_size() : 0u; }
    [[nodiscard]] size_type read_size()  const noexcept { return is_valid() ? Base::read_size()  : 0u; }
    void clear() noexcept { Base::clear(); }
    [[nodiscard]] base_allocator_type get_allocator() const noexcept { return {}; }

    // ------------------------------------------------------------------------------------------
    // Access to the ring of pointers (void**)
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer const* data() noexcept {
        // Expose slots as read-only to prevent users from corrupting the pointer ring.
        if constexpr (kDynamic) { return slots_; }
        else                    { return slots_.data(); }
    }

    [[nodiscard]] RB_FORCEINLINE pointer const* data() const noexcept {
        if constexpr (kDynamic) { return slots_; }
        else                    { return slots_.data(); }
    }

    // ------------------------------------------------------------------------------------------
    // Iteration API (Consumer Side Only)
    // ------------------------------------------------------------------------------------------
    iterator begin() noexcept {
        if (RB_UNLIKELY(!is_valid())) { return iterator(nullptr, 0u, 0u); }
        return iterator(data(), Base::mask(), Base::tail());
    }

    iterator end() noexcept {
        if (RB_UNLIKELY(!is_valid())) { return iterator(nullptr, 0u, 0u); }
        return iterator(data(), Base::mask(), Base::head());
    }

    const_iterator begin()  const noexcept { return cbegin(); }
    const_iterator end()    const noexcept { return cend(); }

    const_iterator cbegin() const noexcept {
        if (RB_UNLIKELY(!is_valid())) { return const_iterator(nullptr, 0u, 0u); }
        return const_iterator(data(), Base::mask(), Base::tail());
    }

    const_iterator cend() const noexcept {
        if (RB_UNLIKELY(!is_valid())) { return const_iterator(nullptr, 0u, 0u); }
        return const_iterator(data(), Base::mask(), Base::head());
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
        const auto t = Base::tail();
        const auto h = Base::head();
        const auto m = Base::mask();
        return snapshot(it(data(), m, t), it(data(), m, h));
    }

    [[nodiscard]] const_snapshot make_snapshot() const noexcept {
        using it = const_snapshot_iterator;
        if (RB_UNLIKELY(!is_valid())) {
            return const_snapshot(it(nullptr, 0u, 0u), it(nullptr, 0u, 0u));
        }
        const auto t = Base::tail();
        const auto h = Base::head();
        const auto m = Base::mask();
        return const_snapshot(it(data(), m, t), it(data(), m, h));
    }

    template<class Snap>
    void consume(const Snap& s) noexcept {
        SPSC_ASSERT(is_valid());
        SPSC_ASSERT(s.begin().data() == data());
        SPSC_ASSERT(s.begin().mask() == Base::mask());
        SPSC_ASSERT(static_cast<size_type>(s.tail_index()) == Base::tail());
        Base::set_tail(static_cast<size_type>(s.head_index()));
    }

    template<class Snap>
    [[nodiscard]] bool try_consume(const Snap& s) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return false; }

        const size_type cap = Base::capacity();

        const size_type snap_tail = static_cast<size_type>(s.tail_index());
        const size_type snap_head = static_cast<size_type>(s.head_index());

        const size_type cur_tail  = Base::tail();
        const size_type cur_head  = Base::head();

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

        // Ensure snap_head is not "ahead" of current head in modulo sense.
        const size_type head_delta = static_cast<size_type>(cur_head - snap_head);
        if (RB_UNLIKELY(head_delta > cap)) { return false; }

        Base::set_tail(snap_head);
        return true;
    }

    void consume_all() noexcept { Base::sync_tail_to_head(); }

    // ------------------------------------------------------------------------------------------
    // Bulk / Regions
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] regions claim_write(const size_type max_count = static_cast<size_type>(~size_type(0))) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return {}; }

        const size_type cap  = Base::capacity();
        const size_type head = Base::head();
        const size_type tail = Base::tail();

        const size_type used = static_cast<size_type>(head - tail);
        if (RB_UNLIKELY(used > cap)) { return {}; } // corruption guard (prevents underflow)

        size_type total = static_cast<size_type>(cap - used);

        if (max_count < total) { total = max_count; }
        if (RB_UNLIKELY(total == 0u)) { return {}; }

        const size_type mask    = Base::mask();
        const size_type wi      = static_cast<size_type>(head & mask);
        const size_type w2e     = static_cast<size_type>(cap - wi);
        const size_type first_n = (w2e < total) ? w2e : total;

        regions r{};
        r.first.ptr  = data() + wi;
        r.first.count  = first_n;
        r.second.ptr = data();
        r.second.count = static_cast<size_type>(total - first_n);
        r.total        = total;
        return r;
    }

    [[nodiscard]] regions claim_read(const size_type max_count = static_cast<size_type>(~size_type(0))) noexcept {
        if (RB_UNLIKELY(!is_valid())) { return {}; }

        const size_type cap  = Base::capacity();
        const size_type head = Base::head();
        const size_type tail = Base::tail();

        size_type total = static_cast<size_type>(head - tail);
        if (RB_UNLIKELY(total > cap)) { return {}; } // corruption guard

        if (max_count < total) { total = max_count; }
        if (RB_UNLIKELY(total == 0u)) { return {}; }

        const size_type mask    = Base::mask();
        const size_type ri      = static_cast<size_type>(tail & mask);
        const size_type r2e     = static_cast<size_type>(cap - ri);
        const size_type first_n = (r2e < total) ? r2e : total;

        regions r{};
        r.first.ptr  = data() + ri;
        r.first.count  = first_n;
        r.second.ptr = data();
        r.second.count = static_cast<size_type>(total - first_n);
        r.total        = total;
        return r;
    }

    // ------------------------------------------------------------------------------------------
    // Producer Operations
    // ------------------------------------------------------------------------------------------
    template<class U>
    RB_FORCEINLINE void push(const U& v) noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool]: U must be trivially copyable");
        SPSC_ASSERT(!full());
        SPSC_ASSERT(sizeof(U) <= bufferSize_.load());

        pointer dst = slots_[Base::write_index()];
        std::memcpy(dst, &v, sizeof(U));
        Base::increment_head();
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE bool try_push(const U& v) noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool]: U must be trivially copyable");
        if (RB_UNLIKELY(full())) { return false; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { return false; }

        pointer dst = slots_[Base::write_index()];
        std::memcpy(dst, &v, sizeof(U));
        Base::increment_head();
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE pointer claim() noexcept {
        SPSC_ASSERT(!full());
        return slots_[Base::write_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_claim() noexcept {
        if (RB_UNLIKELY(full())) { return nullptr; }
        return slots_[Base::write_index()];
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE U* claim_as() noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool]: U must be trivially copyable");
        pointer p = try_claim();
        if (RB_UNLIKELY(!p)) { return nullptr; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { return nullptr; }
        if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) != 0u)) { return nullptr; }
        return static_cast<U*>(p);
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
        SPSC_ASSERT(n <= free());
        Base::advance_head(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_publish(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_write(n))) { return false; }
        Base::advance_head(n);
        return true;
    }

    // --------------------------------------------------------------------------
    // Raw Buffer Push API
    // --------------------------------------------------------------------------

    /*
     * push(data, size)
     * Copies up to 'buffer_size()' bytes from 'data'.
     *
     * Behavior:
     * - Truncates input if size > buffer_size() (writes only what fits).
     * - Asserts !full().
     */
    RB_FORCEINLINE void push(const void* data, const size_type size) noexcept {
        SPSC_ASSERT(!full());

        const size_type bufferSize = bufferSize_.load();
        // Clamp copy size to buffer capacity (saturation)
        const size_type copy_n = (size < bufferSize) ? size : bufferSize;

        pointer dst = slots_[Base::write_index()];
        std::memcpy(dst, data, copy_n);
        Base::increment_head();
    }

    /*
     * try_push(data, size)
     * Copies up to 'buffer_size()' bytes if the queue is not full.
     * Returns false ONLY if the queue is full.
     */
    [[nodiscard]] RB_FORCEINLINE bool try_push(const void* data, const size_type size) noexcept {
        if (RB_UNLIKELY(full())) { return false; }

        const size_type bufferSize = bufferSize_.load();
        // Clamp copy size to buffer capacity (saturation)
        const size_type copy_n = (size < bufferSize) ? size : bufferSize;

        pointer dst = slots_[Base::write_index()];
        std::memcpy(dst, data, copy_n);
        Base::increment_head();
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Consumer Operations
    // ------------------------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer front() noexcept {
        SPSC_ASSERT(!empty());
        return slots_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer front() const noexcept {
        SPSC_ASSERT(!empty());
        return slots_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE pointer try_front() noexcept {
        if (RB_UNLIKELY(empty())) { return nullptr; }
        return slots_[Base::read_index()];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer try_front() const noexcept {
        if (RB_UNLIKELY(empty())) { return nullptr; }
        return slots_[Base::read_index()];
    }

    template<class U>
    [[nodiscard]] RB_FORCEINLINE U* front_as() noexcept {
        static_assert(std::is_trivially_copyable_v<U>, "[pool]: U must be trivially copyable");
        pointer p = try_front();
        if (RB_UNLIKELY(!p)) { return nullptr; }
        if (RB_UNLIKELY(sizeof(U) > bufferSize_.load())) { return nullptr; }
        if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) != 0u)) { return nullptr; }
        return static_cast<U*>(p);
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
        SPSC_ASSERT(n <= size());
        Base::advance_tail(n);
    }

    [[nodiscard]] RB_FORCEINLINE bool try_pop(const size_type n) noexcept {
        if (RB_UNLIKELY(!can_read(n))) { return false; }
        Base::advance_tail(n);
        return true;
    }

    [[nodiscard]] RB_FORCEINLINE pointer operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx = static_cast<size_type>((Base::tail() + i) & Base::mask());
        return slots_[idx];
    }

    [[nodiscard]] RB_FORCEINLINE const_pointer operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < size());
        const size_type idx = static_cast<size_type>((Base::tail() + i) & Base::mask());
        return slots_[idx];
    }

#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<std::byte> span() noexcept {
        pointer p = try_front();
        if (!p) { return {}; }
        return std::span<std::byte>(static_cast<std::byte*>(p), bufferSize_.load());
    }
#endif /* SPSC_HAS_SPAN */

    // ------------------------------------------------------------------------------------------
    // Resize / Destroy
    // ------------------------------------------------------------------------------------------

    // Dynamic-only: resize depth + buffer_size (grow-only unless depth==0 or buffer_size==0).
    template<size_type C = Capacity, typename = std::enable_if_t<C == 0>>
    [[nodiscard]] bool resize(size_type depth, size_type buffer_size) {
        return reallocate_impl(depth, buffer_size);
    }

    // Static-only: (re)allocate buffers (grow-only unless buffer_size==0).
    template<size_type C = Capacity, typename = std::enable_if_t<C != 0>>
    [[nodiscard]] bool resize(size_type buffer_size) {
        return reallocate_impl(Capacity, buffer_size);
    }

    void destroy() noexcept {
        if constexpr (kDynamic) {
            pointer* ptr = slots_;
            const size_type cap = Base::capacity();
            const size_type bs  = bufferSize_.load();

            SPSC_ASSERT((ptr == nullptr) == (cap == 0u));

            slots_      = nullptr;
            bufferSize_.store(0u);
            (void)Base::init(0u);

            if (ptr != nullptr && cap != 0u && bs != 0u) {
                free_buffers(ptr, cap, bs);
                slot_allocator_type sa{};
                slot_alloc_traits::deallocate(sa, ptr, cap);
            }
            return;
        } else {
            const size_type bufferSize = bufferSize_.load();
            if (bufferSize != 0u) {
                free_buffers(slots_.data(), Capacity, bufferSize);
            }
            slots_.fill(nullptr);
            bufferSize_.store(0u);
            Base::clear();
        }
    }
    // ------------------------------------------------------------------------------------------
    // RAII Based API
    // ------------------------------------------------------------------------------------------

    class write_guard {
    public:
        write_guard() noexcept = default;

        explicit write_guard(pool& p) noexcept
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
            // Publish ONLY if explicitly armed.
            if (p_ && ptr_ && publish_on_destroy_) {
                p_->publish();
            }
        }

        [[nodiscard]] pointer get() const noexcept { return ptr_; }
        explicit operator bool() const noexcept { return (p_ != nullptr) && (ptr_ != nullptr); }

        template<class U>
        [[nodiscard]] U* as() const noexcept {
            if (RB_UNLIKELY(!ptr_ || !p_)) { return nullptr; }
            static_assert(std::is_trivially_copyable_v<U>, "[pool::guard]: U must be trivially copyable");
            if (RB_UNLIKELY(sizeof(U) > p_->buffer_size())) { return nullptr; }
            if (RB_UNLIKELY((reinterpret_cast<std::uintptr_t>(ptr_) % alignof(U)) != 0u)) { return nullptr; }
            return static_cast<U*>(ptr_);
        }

        // Manual path: user wrote bytes via get()/as<U>(), then arms publish on scope exit.
        void publish_on_destroy() noexcept {
            SPSC_ASSERT(p_ && ptr_);
            publish_on_destroy_ = true;
        }

        // Explicit publish now.
        void commit() noexcept {
            if (p_ && ptr_) {
                p_->publish();
            }
            cancel();
        }

        // Cancel means: do NOT publish.
        void cancel() noexcept {
            publish_on_destroy_ = false;
            p_ = nullptr;
            ptr_ = nullptr;
        }

    private:
        pool* p_{nullptr};
        pointer ptr_{nullptr};
        bool    publish_on_destroy_{false};
    };

    class read_guard {
    public:
        read_guard() noexcept = default;

        explicit read_guard(pool& p) noexcept
            : p_(&p), ptr_(p.try_front()), active_(ptr_ != nullptr) {}

        read_guard(const read_guard&)            = delete;
        read_guard& operator=(const read_guard&) = delete;

        read_guard(read_guard&& other) noexcept
            : p_(other.p_), ptr_(other.ptr_), active_(other.active_) {
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
            static_assert(std::is_trivially_copyable_v<U>, "[pool::guard]: U must be trivially copyable");
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
        pool* p_{nullptr};
        pointer ptr_{nullptr};
        bool    active_{false};
    };

    [[nodiscard]] write_guard scoped_write() noexcept { return write_guard(*this); }
    [[nodiscard]] read_guard  scoped_read()  noexcept { return read_guard(*this); }

private:
    // ------------------------------------------------------------------------------------------
    // Allocation helpers
    // ------------------------------------------------------------------------------------------
    static void free_buffers(pointer* ptr, size_type depth, size_type buffer_size) noexcept {
        if (RB_UNLIKELY(!ptr || depth == 0u || buffer_size == 0u)) { return; }

        byte_allocator_type ba{};
        for (size_type i = 0; i < depth; ++i) {
            if (ptr[i]) {
                auto* p = static_cast<std::byte*>(ptr[i]);
                byte_alloc_traits::deallocate(ba, p, buffer_size);
            }
        }
    }

    // Core reallocation logic shared by constructors and resize.
    [[nodiscard]] bool reallocate_impl(size_type requested_depth, size_type requested_buffer_size) {
        static_assert(::spsc::cap::rb_is_pow2(::spsc::cap::RB_MAX_UNAMBIGUOUS),
                      "[pool]: RB_MAX_UNAMBIGUOUS must be power of two");

        // Snapshot old state (defensive: handle corrupted state)
        pointer const* old_slots = data();
        size_type old_cap  = Base::capacity();
        size_type old_head = Base::head();
        size_type old_tail = Base::tail();
        size_type old_bs   = bufferSize_.load();
        size_type old_size = 0u;

        if (is_valid()) {
            old_size = static_cast<size_type>(old_head - old_tail);
            if (RB_UNLIKELY(old_cap != 0u && old_size > old_cap)) {
                // Corrupted state: drop queued data to avoid OOB migration.
                Base::clear();
                old_head = 0u;
                old_tail = 0u;
                old_size = 0u;
            }
        }

        // Explicit shrink-to-zero: release storage and clear queue.
        if (requested_depth == 0u || requested_buffer_size == 0u) {
            destroy();
            return true;
        }

        // Normalize depth and enforce pow2 geometry
        size_type target_depth = requested_depth;

        if (target_depth < 2u) { target_depth = 2u; }
        if (target_depth > ::spsc::cap::RB_MAX_UNAMBIGUOUS) { target_depth = ::spsc::cap::RB_MAX_UNAMBIGUOUS; }
        target_depth = ::spsc::cap::rb_next_power2(target_depth);

        if constexpr (!kDynamic) {
            target_depth = Capacity;
        }

        // Never shrink depth for a valid queue
        if (is_valid() && old_cap != 0u && target_depth < old_cap) { target_depth = old_cap; }

        // Optimization: if no growth needed (depth and buffer size), keep existing.
        if (is_valid() && old_cap != 0u && target_depth <= old_cap && requested_buffer_size <= old_bs) {
            return true;
        }

        pointer* new_slots = nullptr;
        static_slots new_static{};

        byte_allocator_type ba{};
        size_type allocated = 0u;

        // Allocate slots + buffers inside one TRY so any throw rolls back cleanly.
        SPSC_TRY {
            if constexpr (kDynamic) {
                slot_allocator_type sa{};
                new_slots = slot_alloc_traits::allocate(sa, target_depth);
                if (RB_LIKELY(new_slots != nullptr)) {
                    for (size_type i = 0; i < target_depth; ++i) { new_slots[i] = nullptr; }
                }
            } else {
                new_static.fill(nullptr);
                new_slots = new_static.data();
            }

            if (RB_LIKELY(new_slots != nullptr)) {
                for (; allocated < target_depth; ++allocated) {
                    auto* buf = byte_alloc_traits::allocate(ba, requested_buffer_size);
                    if (RB_UNLIKELY(!buf)) { break; }
                    new_slots[allocated] = static_cast<pointer>(buf);
                }
            }
        } SPSC_CATCH_ALL {
            free_buffers(new_slots, allocated, requested_buffer_size);
            if constexpr (kDynamic) {
                if (new_slots != nullptr) {
                    slot_allocator_type sa{};
                    slot_alloc_traits::deallocate(sa, new_slots, target_depth);
                }
            }
            SPSC_RETHROW;
        }

        if (RB_UNLIKELY(new_slots == nullptr)) { return false; }

        if (RB_UNLIKELY(allocated != target_depth)) {
            free_buffers(new_slots, allocated, requested_buffer_size);
            if constexpr (kDynamic) {
                slot_allocator_type sa{};
                slot_alloc_traits::deallocate(sa, new_slots, target_depth);
            }
            return false;
        }

        // Migrate queued data (linearize into [0..old_size-1])
        size_type migrated = 0u;
        if (is_valid() && old_slots && old_cap && old_size) {
            const size_type copy_len = (old_bs < requested_buffer_size) ? old_bs : requested_buffer_size;

            const size_type old_mask = Base::mask();
            const size_type tail_idx = static_cast<size_type>(old_tail & old_mask);
            const size_type to_end   = static_cast<size_type>(old_cap - tail_idx);
            const size_type first_n  = (old_size < to_end) ? old_size : to_end;
            const size_type second_n = static_cast<size_type>(old_size - first_n);

            if (copy_len > 0u) {
                for (size_type k = 0; k < first_n; ++k) {
                    std::memcpy(new_slots[k], old_slots[tail_idx + k], copy_len);
                }
                for (size_type k = 0; k < second_n; ++k) {
                    std::memcpy(new_slots[first_n + k], old_slots[k], copy_len);
                }
            }
            migrated = old_size;
        }

        // Drop old storage
        destroy();

        // Commit new storage
        bufferSize_.store(requested_buffer_size);

        if constexpr (kDynamic) {
            slots_ = new_slots;
            const bool ok = Base::init(target_depth, migrated, 0u);

            if (RB_UNLIKELY(!ok)) {
                // If policy init fails (should not happen), avoid leaving a half-valid object.
                pointer* ptr = slots_;
                slots_ = nullptr;
                bufferSize_.store(0u);
                (void)Base::init(0u);

                free_buffers(ptr, target_depth, requested_buffer_size);
                slot_allocator_type sa{};
                slot_alloc_traits::deallocate(sa, ptr, target_depth);
                return false;
            }
        } else {
            slots_ = new_static;
            // Non-concurrent commit: restore state via Base::init() (tail=0, head=migrated).
            const bool ok = Base::init(migrated, 0u);
            SPSC_ASSERT(ok);
            (void)ok;
        }

        return true;
    }

    [[nodiscard]] bool copy_from(const pool& other) {
        if (!other.is_valid()) {
            destroy();
            return true;
        }

        // Allocate using other's geometry and buffer size
        if constexpr (kDynamic) {
            if (!reallocate_impl(other.capacity(), other.buffer_size())) { destroy(); return false; }
        } else {
            if (!reallocate_impl(Capacity, other.buffer_size())) { destroy(); return false; }
        }

        if (!is_valid()) { destroy(); return false; }

        const size_type sz = other.size();
        if (sz == 0u) { Base::clear(); return true; }

        const size_type cap = Base::capacity();
        if (RB_UNLIKELY(sz > cap)) { Base::clear(); return true; }

        const size_type m = other.Base::mask();
        const size_type t = other.Base::tail();

        const size_type bufferSize = bufferSize_.load();
        const size_type copy_len = (bufferSize < other.buffer_size()) ? bufferSize : other.buffer_size();

        // Copy queued buffers into 0..sz-1 (linearized)
        for (size_type k = 0; k < sz; ++k) {
            pointer src = other.data()[(t + k) & m];
            pointer dst = slots_[k];
            if (copy_len > 0u) {
                std::memcpy(dst, src, copy_len);
            }
        }

        // Non-concurrent commit: restore state via Base::init() (tail=0, head=sz).
        bool ok = false;
        if constexpr (kDynamic) {
            ok = Base::init(Base::capacity(), sz, 0u);
        } else {
            ok = Base::init(sz, 0u);
        }
        SPSC_ASSERT(ok);
        (void)ok;
        return true;
    }

    void move_from(pool&& other) noexcept {
        if constexpr (kDynamic) {
            const size_type cap  = other.Base::capacity();
            const size_type head = other.Base::head();
            const size_type tail = other.Base::tail();
            pointer* ptr  = other.slots_;
            const size_type bs   = other.bufferSize_.load();

            // Keep invariants: either fully-valid (ptr+cap+bs) or fully-empty.
            if (RB_UNLIKELY((ptr == nullptr) != (cap == 0u))) {
                // Invariant broken: do not touch 'other' or you'll lose the pointer in release builds.
                slots_      = nullptr;
                bufferSize_.store(0u);
                (void)Base::init(0u);
                return;
            }
            if (RB_UNLIKELY((bs == 0u) != (cap == 0u))) {
                // Buffer size invariant broken: same policy as above.
                slots_      = nullptr;
                bufferSize_.store(0u);
                (void)Base::init(0u);
                return;
            }

            if (ptr != nullptr) {
                const bool ok = Base::init(cap, head, tail);
                if (RB_UNLIKELY(!ok)) {
                    // Corrupted source geometry: leave other untouched, poison *this*.
                    slots_      = nullptr;
                    bufferSize_.store(0u);
                    (void)Base::init(0u);
                    return;
                }

                slots_      = ptr;
                bufferSize_.store(bs);
            } else {
                slots_      = nullptr;
                bufferSize_.store(0u);
                (void)Base::init(0u);
            }

            // Steal and reset other
            other.slots_      = nullptr;
            other.bufferSize_.store(0u);
            (void)other.Base::init(0u);
        } else {
            slots_      = other.slots_;
            bufferSize_.store(other.bufferSize_.load());

            // Non-concurrent move: restore state via Base::init() to keep shadow caches consistent.
            const bool ok = Base::init(other.Base::head(), other.Base::tail());
            SPSC_ASSERT(ok);
            (void)ok;

            other.slots_.fill(nullptr);
            other.bufferSize_.store(0u);
            other.Base::clear();
        }
    }

private:
    slots_storage 	slots_{};     // void** (dynamic) or array<void*, Capacity> (static)
    geometry_type  	bufferSize_{};
};

} // namespace spsc

#endif /* SPSC_POOL_HPP_ */
