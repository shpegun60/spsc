/*
 * chunk.hpp
 *
 * High-performance contiguous buffer container.
 *
 * Variants:
 * 1. STATIC CHUNK (ChunkCapacity > 0):
 * - Backed by std::array<T, N>.
 * - Fixed capacity, zero allocation overhead.
 * - Ideal for stack buffers or embedded storage.
 *
 * 2. DYNAMIC CHUNK (ChunkCapacity == 0):
 * - Backed by allocator-managed T[].
 * - "Eager" construction model: all [0..capacity) elements are always constructed.
 * - push() uses assignment (operator=), not construction.
 * - resize() only moves the logical cursor, it implies no construction/destruction cost.
 *
 * Concurrency:
 * - Not thread-safe. Designed to be used as a building block inside
 * synchronized containers (like spsc::fifo) or thread-local buffers.
 */

#ifndef SPSC_CHUNK_HPP_
#define SPSC_CHUNK_HPP_

#include <algorithm>    // std::max
#include <array>
#include <cstddef>      // std::ptrdiff_t
#include <cstring>      // std::memcpy
#include <iterator>     // std::reverse_iterator
#include <memory>       // std::allocator_traits, std::uninitialized_default_construct_n
#include <type_traits>
#include <utility>      // std::move, std::forward, std::swap

// Use core definitions from the SPSC library
#include "basic_types.h"        // reg
#include "base/spsc_alloc.hpp"
#include "base/spsc_tools.hpp" // RB_FORCEINLINE, SPSC_ASSERT, macros

#if SPSC_HAS_SPAN
#include <span>
#endif

namespace spsc {

/*
 * Forward declaration
 */
template<
    class T,
    reg   ChunkCapacity = 0,
    typename Alloc      = ::spsc::alloc::default_alloc
    >
class chunk;

/* =======================================================================
 * STATIC CHUNK: chunk<T, N, Alloc>
 *
 * Fixed-size storage backed by std::array.
 * T must be default-constructible (elements are initialized on creation).
 * ======================================================================= */
template<
    class T,
    reg   ChunkCapacity,
    typename Alloc
    >
class chunk
{
public:
    using value_type             = T;
    using size_type              = reg;
    using difference_type        = std::ptrdiff_t;
    using reference              = value_type&;
    using const_reference        = const value_type&;
    using pointer                = value_type*;
    using const_pointer          = const value_type*;
    using iterator               = pointer;
    using const_iterator         = const_pointer;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    using base_allocator_type    = Alloc;

    static constexpr size_type kCapacity = ChunkCapacity;

    // --------------------------------------------------------------------------
    // Static Assertions
    // --------------------------------------------------------------------------
    static_assert(ChunkCapacity > 0,
                  "[spsc::chunk]: Static capacity must be > 0.");
    static_assert(std::is_default_constructible_v<T>,
                  "[spsc::chunk]: T must be default-constructible (stored in std::array).");
    static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                  "[spsc::chunk]: T must be move- or copy-assignable.");

#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "[spsc::chunk]: no-exceptions mode requires noexcept default constructor.");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                  "[spsc::chunk]: no-exceptions mode requires noexcept assignment.");
#endif

private:
    std::array<value_type, kCapacity> storage_{}; // Elements are always constructed
    size_type                         len_ = 0;   // Active count [0..kCapacity]

public:
    // --------------------------------------------------------------------------
    // Ctors / Assignment
    // --------------------------------------------------------------------------
    chunk() noexcept  = default;
    ~chunk() noexcept = default;

    chunk(const chunk&) = default;
    chunk& operator=(const chunk&) = default;

    chunk(chunk&&) noexcept(std::is_nothrow_move_constructible_v<value_type>) = default;
    chunk& operator=(chunk&&) noexcept(std::is_nothrow_move_assignable_v<value_type>) = default;

    // --------------------------------------------------------------------------
    // Capacity & State
    // --------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept { return len_ == 0; }
    [[nodiscard]] RB_FORCEINLINE bool full()  const noexcept { return len_ >= kCapacity; }
    [[nodiscard]] RB_FORCEINLINE size_type size() const noexcept { return len_; }
    [[nodiscard]] static constexpr size_type capacity() noexcept { return kCapacity; }
    [[nodiscard]] RB_FORCEINLINE size_type free() const noexcept { return static_cast<size_type>(kCapacity - len_); }

    void clear() noexcept { len_ = 0; }

    // Manually adjust logical size (e.g., after direct DMA write).
    [[nodiscard]] bool resize(const size_type n) noexcept {
        len_ = (n < kCapacity) ? n : kCapacity;
        return true;
    }

    // Explicit name for clamping resize (same behavior as resize()).
    [[nodiscard]] bool resize_clamp(const size_type n) noexcept { return resize(n); }

    // Resize without clamping; does not modify size if n > capacity.
    [[nodiscard]] bool try_resize(const size_type n) noexcept {
        if (n > kCapacity) { return false; }
        len_ = n;
        return true;
    }

    // Accept externally-written size (e.g., DMA) without allocation.
    void commit_size(const size_type n) noexcept {
        SPSC_ASSERT(n <= kCapacity);
        len_ = n;
    }

    // --------------------------------------------------------------------------
    // Data Access
    // --------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer data() noexcept { return storage_.data(); }
    [[nodiscard]] RB_FORCEINLINE const_pointer data() const noexcept { return storage_.data(); }


#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<value_type> used_span() noexcept { return { data(), size() }; }
    [[nodiscard]] std::span<const value_type> used_span() const noexcept { return { data(), size() }; }

    [[nodiscard]] std::span<value_type> cap_span() noexcept { return { data(), capacity() }; }
    [[nodiscard]] std::span<const value_type> cap_span() const noexcept { return { data(), capacity() }; }
#endif

    [[nodiscard]] RB_FORCEINLINE reference operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < kCapacity);
        return storage_[i];
    }
    [[nodiscard]] RB_FORCEINLINE const_reference operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < kCapacity);
        return storage_[i];
    }

    [[nodiscard]] RB_FORCEINLINE reference front() noexcept { SPSC_ASSERT(!empty()); return storage_[0]; }
    [[nodiscard]] RB_FORCEINLINE const_reference front() const noexcept { SPSC_ASSERT(!empty()); return storage_[0]; }

    [[nodiscard]] RB_FORCEINLINE reference back() noexcept { SPSC_ASSERT(!empty()); return storage_[len_ - 1]; }
    [[nodiscard]] RB_FORCEINLINE const_reference back() const noexcept { SPSC_ASSERT(!empty()); return storage_[len_ - 1]; }

    [[nodiscard]] pointer try_front() noexcept {
        return empty() ? nullptr : &storage_[0];
    }
    [[nodiscard]] const_pointer try_front() const noexcept {
        return empty() ? nullptr : &storage_[0];
    }

    [[nodiscard]] pointer try_back() noexcept {
        return empty() ? nullptr : &storage_[len_ - 1];
    }
    [[nodiscard]] const_pointer try_back() const noexcept {
        return empty() ? nullptr : &storage_[len_ - 1];
    }
    void pop_back() noexcept {
        SPSC_ASSERT(!empty());
        --len_;
    }


    [[nodiscard]] bool try_pop_back() noexcept {
        if (empty()) { return false; }
        --len_;
        return true;
    }

    void pop_back_n(size_type n) noexcept {
        SPSC_ASSERT(n <= len_);
        len_ = static_cast<size_type>(len_ - n);
    }
    // --------------------------------------------------------------------------
    // Producer Interface
    // --------------------------------------------------------------------------
    template<class U, typename = std::enable_if_t<std::is_assignable_v<reference, U&&>>>
    void push(U&& v) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        SPSC_ASSERT(!full());
        storage_[len_] = std::forward<U>(v);
        ++len_;
    }

    template<class U, typename = std::enable_if_t<std::is_assignable_v<reference, U&&>>>
    [[nodiscard]] bool try_push(U&& v) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        if (RB_UNLIKELY(full())) { return false; }
        storage_[len_] = std::forward<U>(v);
        ++len_;
        return true;
    }

    template<class... Args>
    reference emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        SPSC_ASSERT(!full());
        reference slot = storage_[len_];
        // Note: Logic implies assignment because slots are already constructed.
        slot = value_type(std::forward<Args>(args)...);
        ++len_;
        return slot;
    }

    template<class... Args>
    [[nodiscard]] pointer try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        if (RB_UNLIKELY(full())) { return nullptr; }
        reference slot = storage_[len_];
        slot = value_type(std::forward<Args>(args)...);
        ++len_;
        return &slot;
    }

    // --------------------------------------------------------------------------
    // Iterators & Swap
    // --------------------------------------------------------------------------
    [[nodiscard]] iterator begin() noexcept { return storage_.data(); }
    [[nodiscard]] iterator end()   noexcept { return storage_.data() + len_; }
    [[nodiscard]] const_iterator begin() const noexcept { return storage_.data(); }
    [[nodiscard]] const_iterator end()   const noexcept { return storage_.data() + len_; }
    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] const_iterator cend()   const noexcept { return end(); }

    [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] reverse_iterator rend()   noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] const_reverse_iterator rend()   const noexcept { return const_reverse_iterator(begin()); }

    void swap(chunk& other) noexcept {
        using std::swap;
        swap(storage_, other.storage_);
        swap(len_, other.len_);
    }

    friend void swap(chunk& a, chunk& b) noexcept { a.swap(b); }
};

/* =======================================================================
 * DYNAMIC CHUNK: chunk<T, 0, Alloc>
 *
 * Resizable storage backed by Alloc.
 * Maintains invariant: [0..cap_) are valid constructed objects.
 * ======================================================================= */
template<class T, typename Alloc>
class chunk<T, 0, Alloc>
{
public:
    using value_type             = T;
    using size_type              = reg;
    using difference_type        = std::ptrdiff_t;
    using reference              = value_type&;
    using const_reference        = const value_type&;
    using pointer                = value_type*;
    using const_pointer          = const value_type*;
    using iterator               = pointer;
    using const_iterator         = const_pointer;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    using base_allocator_type    = Alloc;
    using allocator_type         = typename std::allocator_traits<base_allocator_type>
        ::template rebind_alloc<value_type>;
    using alloc_traits           = std::allocator_traits<allocator_type>;
    using alloc_pointer          = typename alloc_traits::pointer;

    static constexpr size_type kCapacity = 0; // Marker for dynamic

private:
    pointer   storage_ = nullptr;
    size_type len_     = 0;
    size_type cap_     = 0;

public:
    // --------------------------------------------------------------------------
    // Static Assertions
    // --------------------------------------------------------------------------
    static_assert(alloc_traits::is_always_equal::value,
                  "[spsc::chunk]: dynamic chunk requires stateless allocator.");
    static_assert(std::is_same_v<alloc_pointer, pointer>,
                  "[spsc::chunk]: allocator must return raw pointers (T*).");
    static_assert(std::is_default_constructible_v<T>,
                  "[spsc::chunk]: T must be default-constructible (eager initialization).");
    static_assert(std::is_move_assignable_v<T> || std::is_copy_assignable_v<T>,
                  "[spsc::chunk]: T must be assignable.");

#if (SPSC_ENABLE_EXCEPTIONS == 0)
    static_assert(std::is_nothrow_default_constructible_v<T>,
                  "[spsc::chunk]: no-exceptions mode requires noexcept default constructor.");
    static_assert(std::is_nothrow_move_assignable_v<T> || std::is_nothrow_copy_assignable_v<T>,
                  "[spsc::chunk]: no-exceptions mode requires noexcept assignment.");
#endif

    // --------------------------------------------------------------------------
    // Ctors / Assignment
    // --------------------------------------------------------------------------
    chunk() noexcept = default;

    ~chunk() noexcept {
        release_storage(storage_, cap_);
    }

    // Move-only
    chunk(const chunk&)            = delete;
    chunk& operator=(const chunk&) = delete;

    chunk(chunk&& other) noexcept
        : storage_(other.storage_)
        , len_(other.len_)
        , cap_(other.cap_)
    {
        other.storage_ = nullptr;
        other.len_     = 0;
        other.cap_     = 0;
    }

    chunk& operator=(chunk&& other) noexcept {
        if (this != &other) {
            release_storage(storage_, cap_);
            storage_ = other.storage_;
            len_     = other.len_;
            cap_     = other.cap_;

            other.storage_ = nullptr;
            other.len_     = 0;
            other.cap_     = 0;
        }
        return *this;
    }

    // --------------------------------------------------------------------------
    // Capacity & State
    // --------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE bool empty() const noexcept { return len_ == 0; }
    [[nodiscard]] RB_FORCEINLINE bool full()  const noexcept { return len_ >= cap_; }
    [[nodiscard]] RB_FORCEINLINE size_type size() const noexcept { return len_; }
    [[nodiscard]] RB_FORCEINLINE size_type capacity() const noexcept { return cap_; }
    [[nodiscard]] RB_FORCEINLINE size_type free() const noexcept { return static_cast<size_type>(cap_ - len_); }

    void clear() noexcept { len_ = 0; }

    // --------------------------------------------------------------------------
    // Reserve / Resize
    // --------------------------------------------------------------------------

    // Ensures capacity >= new_cap.
    // NOTE: Eagerly default-constructs new elements to allow unchecked writes.
    [[nodiscard]] bool reserve(const size_type new_cap) {
        if (new_cap <= cap_) { return true; }

        allocator_type alloc{};
        pointer new_storage = alloc_traits::allocate(alloc, new_cap);
        if (RB_UNLIKELY(!new_storage)) { return false; }

        // 1. Construct everything in new buffer (exception-safe)
        size_type constructed = 0;
        SPSC_TRY {
            for (; constructed < new_cap; ++constructed) {
                alloc_traits::construct(alloc, new_storage + constructed);
            }
        } SPSC_CATCH_ALL {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy_n(new_storage, constructed);
            }
            alloc_traits::deallocate(alloc, new_storage, new_cap);
            SPSC_RETHROW;
        }

        // 2. Migrate existing data (Move or Copy)
        SPSC_TRY {
            if constexpr (std::is_trivially_copyable_v<T>) {
                if (len_ > 0) {
                    std::memcpy(new_storage, storage_, len_ * sizeof(T));
                }
            } else {
                for (size_type i = 0; i < len_; ++i) {
                    if constexpr (std::is_move_assignable_v<T>) {
                        new_storage[i] = std::move(storage_[i]);
                    } else {
                        new_storage[i] = storage_[i];
                    }
                }
            }
        } SPSC_CATCH_ALL {
            // Rollback: destroy new buffer, keep old
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy_n(new_storage, new_cap);
            }
            alloc_traits::deallocate(alloc, new_storage, new_cap);
            SPSC_RETHROW;
        }

        // 3. Cleanup old
        release_storage(storage_, cap_);

        storage_ = new_storage;
        cap_     = new_cap;
        return true;
    }

    // Adjust logical size. Allocates if n > capacity.
    [[nodiscard]] bool resize(const size_type n) {
        if (n > cap_) {
            // Heuristic: +50% + small padding
            const size_type grow_cap = static_cast<size_type>(n + (n >> 1) + 8);
            if (!reserve(grow_cap)) { return false; }
        }
        len_ = n;
        return true;
    }


    // Accept externally-written size without allocation (e.g., DMA into data()).
    void commit_size(const size_type n) noexcept {
        SPSC_ASSERT(n <= cap_);
        len_ = n;
    }
    // --------------------------------------------------------------------------
    // Data Access
    // --------------------------------------------------------------------------
    [[nodiscard]] RB_FORCEINLINE pointer data() noexcept { return storage_; }
    [[nodiscard]] RB_FORCEINLINE const_pointer data() const noexcept { return storage_; }


#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<value_type> used_span() noexcept { return { data(), size() }; }
    [[nodiscard]] std::span<const value_type> used_span() const noexcept { return { data(), size() }; }

    [[nodiscard]] std::span<value_type> cap_span() noexcept { return { data(), capacity() }; }
    [[nodiscard]] std::span<const value_type> cap_span() const noexcept { return { data(), capacity() }; }
#endif

    [[nodiscard]] RB_FORCEINLINE reference operator[](const size_type i) noexcept {
        SPSC_ASSERT(i < cap_); // allow access up to capacity for raw writes
        return storage_[i];
    }
    [[nodiscard]] RB_FORCEINLINE const_reference operator[](const size_type i) const noexcept {
        SPSC_ASSERT(i < cap_);
        return storage_[i];
    }

    [[nodiscard]] RB_FORCEINLINE reference front() noexcept { SPSC_ASSERT(!empty()); return storage_[0]; }
    [[nodiscard]] RB_FORCEINLINE const_reference front() const noexcept { SPSC_ASSERT(!empty()); return storage_[0]; }

    [[nodiscard]] RB_FORCEINLINE reference back() noexcept { SPSC_ASSERT(!empty()); return storage_[len_ - 1]; }
    [[nodiscard]] RB_FORCEINLINE const_reference back() const noexcept { SPSC_ASSERT(!empty()); return storage_[len_ - 1]; }

    [[nodiscard]] pointer try_front() noexcept {
        return empty() ? nullptr : &storage_[0];
    }
    [[nodiscard]] const_pointer try_front() const noexcept {
        return empty() ? nullptr : &storage_[0];
    }

    [[nodiscard]] pointer try_back() noexcept {
        return empty() ? nullptr : &storage_[len_ - 1];
    }
    [[nodiscard]] const_pointer try_back() const noexcept {
        return empty() ? nullptr : &storage_[len_ - 1];
    }
    void pop_back() noexcept {
        SPSC_ASSERT(!empty());
        --len_;
    }


    [[nodiscard]] bool try_pop_back() noexcept {
        if (empty()) { return false; }
        --len_;
        return true;
    }

    void pop_back_n(size_type n) noexcept {
        SPSC_ASSERT(n <= len_);
        len_ = static_cast<size_type>(len_ - n);
    }
    // --------------------------------------------------------------------------
    // Producer Interface
    // --------------------------------------------------------------------------
    template<class U, typename = std::enable_if_t<std::is_assignable_v<reference, U&&>>>
    void push(U&& v) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        SPSC_ASSERT(!full());
        storage_[len_] = std::forward<U>(v);
        ++len_;
    }

    template<class U, typename = std::enable_if_t<std::is_assignable_v<reference, U&&>>>
    [[nodiscard]] bool try_push(U&& v) noexcept(std::is_nothrow_assignable_v<reference, U&&>) {
        if (RB_UNLIKELY(full())) { return false; }
        storage_[len_] = std::forward<U>(v);
        ++len_;
        return true;
    }

    template<class... Args>
    reference emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        SPSC_ASSERT(!full());
        reference slot = storage_[len_];
        slot = value_type(std::forward<Args>(args)...);
        ++len_;
        return slot;
    }

    template<class... Args>
    [[nodiscard]] pointer try_emplace(Args&&... args) noexcept(
        std::is_nothrow_constructible_v<value_type, Args&&...> &&
        std::is_nothrow_assignable_v<reference, value_type>
        ) {
        if (RB_UNLIKELY(full())) { return nullptr; }
        reference slot = storage_[len_];
        slot = value_type(std::forward<Args>(args)...);
        ++len_;
        return &slot;
    }

    // --------------------------------------------------------------------------
    // STL-style aliases
    // --------------------------------------------------------------------------
    template<class U> void push_back(U&& v) { push(std::forward<U>(v)); }
    template<class... Args> void emplace_back(Args&&... args) { (void)emplace(std::forward<Args>(args)...); }

    // --------------------------------------------------------------------------
    // Iterators & Swap
    // --------------------------------------------------------------------------
    [[nodiscard]] iterator begin() noexcept { return storage_; }
    [[nodiscard]] iterator end()   noexcept { return storage_ ? storage_ + len_ : nullptr; }
    [[nodiscard]] const_iterator begin() const noexcept { return storage_; }
    [[nodiscard]] const_iterator end()   const noexcept { return storage_ ? storage_ + len_ : nullptr; }
    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] const_iterator cend()   const noexcept { return end(); }

    [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] reverse_iterator rend()   noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] const_reverse_iterator rend()   const noexcept { return const_reverse_iterator(begin()); }

    void swap(chunk& other) noexcept {
        using std::swap;
        swap(storage_, other.storage_);
        swap(len_, other.len_);
        swap(cap_, other.cap_);
    }

    friend void swap(chunk& a, chunk& b) noexcept { a.swap(b); }

private:
    void release_storage(pointer p, size_type cap) noexcept {
        if (p && cap) {
            if constexpr (!std::is_trivially_destructible_v<T>) {
                std::destroy_n(p, cap);
            }
            allocator_type alloc{};
            alloc_traits::deallocate(alloc, p, cap);
        }
    }
};

template<class T, class Alloc = ::spsc::alloc::default_alloc>
using dyn_chunk = chunk<T, 0, Alloc>;

} // namespace spsc

#endif /* SPSC_CHUNK_HPP_ */
