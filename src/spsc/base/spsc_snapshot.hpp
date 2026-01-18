/*
 * snapshot.hpp
 *
 * Lightweight iteration and snapshot utilities for SPSC ring buffers.
 *
 * This header provides:
 *  - detail::ring_iterator<T, Size, Const>
 *  - snapshot_view<T, Size>        (mutable view)
 *  - const_snapshot_view<T, Size>  (read-only view)
 *  - snapshot_traits<T, Size>      (bundles types for containers)
 */

#ifndef SPSC_SNAPSHOT_HPP_
#define SPSC_SNAPSHOT_HPP_

#include <iterator>    // std::bidirectional_iterator_tag
#include <cstddef>     // std::ptrdiff_t
#include <type_traits> // std::conditional_t, std::enable_if_t, std::is_unsigned_v
#include <memory>      // std::addressof

namespace spsc {

namespace detail {

template<class T, class Size, bool Const>
class ring_iterator
{
    static_assert(std::is_unsigned_v<Size>,
                  "[ring_iterator]: Size must be an unsigned integer type");

public:
    using value_type        = T;
    using difference_type   = std::ptrdiff_t;
    using reference         = std::conditional_t<Const, const T&, T&>;
    using pointer           = std::conditional_t<Const, const T*, T*>;
    using iterator_category = std::bidirectional_iterator_tag;

    ring_iterator() noexcept = default;

    ring_iterator(pointer storage, Size mask, Size index) noexcept
        : storage_(storage)
        , mask_(mask)
        , index_(index)
    {}

    // Implicit conversion from non-const to const iterator.
    template<bool C = Const, typename = std::enable_if_t<C>>
    ring_iterator(const ring_iterator<T, Size, false>& other) noexcept
        : storage_(other.storage_)
        , mask_(other.mask_)
        , index_(other.index_)
    {}

    reference operator*() const noexcept {
        return storage_[index_ & mask_];
    }

    pointer operator->() const noexcept {
        return std::addressof(**this);
    }

    ring_iterator& operator++() noexcept {
        ++index_;
        return *this;
    }

    ring_iterator operator++(int) noexcept {
        ring_iterator tmp(*this);
        ++(*this);
        return tmp;
    }

    ring_iterator& operator--() noexcept {
        --index_;
        return *this;
    }

    ring_iterator operator--(int) noexcept {
        ring_iterator tmp(*this);
        --(*this);
        return tmp;
    }


    [[nodiscard]] pointer data() noexcept { return storage_; }
    [[nodiscard]] const T* data() const noexcept { return storage_; }

    [[nodiscard]] Size index() const noexcept { return index_; }
    [[nodiscard]] Size mask() const noexcept { return mask_; }

private:
    template<class, class, bool> friend class ring_iterator;

    pointer storage_{nullptr};
    Size    mask_{0};
    Size    index_{0};
};

// Symmetric comparisons for iterator/const_iterator.
template<class T, class Size, bool C1, bool C2>
inline bool operator==(const ring_iterator<T, Size, C1>& a,
                       const ring_iterator<T, Size, C2>& b) noexcept
{
    return a.data() == b.data()
    && a.index() == b.index()
        && a.mask() == b.mask();
}

template<class T, class Size, bool C1, bool C2>
inline bool operator!=(const ring_iterator<T, Size, C1>& a,
                       const ring_iterator<T, Size, C2>& b) noexcept
{
    return !(a == b);
}

} // namespace detail

// ============================================================================
// snapshot_view<T, Size> (mutable)
// ============================================================================

template<class T, class Size>
class snapshot_view
{
    static_assert(std::is_unsigned_v<Size>,
                  "[snapshot_view]: Size must be an unsigned integer type");

public:
    using value_type      = T;
    using size_type       = Size;
    using iterator        = detail::ring_iterator<T, Size, false>;
    using const_iterator  = detail::ring_iterator<T, Size, true>;

    snapshot_view() noexcept = default;

    snapshot_view(iterator b, iterator e) noexcept
        : begin_(b)
        , end_(e)
    {}

    iterator begin() noexcept { return begin_; }
    iterator end() noexcept { return end_; }

    const_iterator begin() const noexcept { return const_iterator(begin_); }
    const_iterator end() const noexcept { return const_iterator(end_); }

    const_iterator cbegin() const noexcept { return const_iterator(begin_); }
    const_iterator cend() const noexcept { return const_iterator(end_); }

    // Logical size (contract: end >= begin). Returns 0 if violated.
    [[nodiscard]] size_type size() const noexcept {
        const size_type b = begin_.index();
        const size_type e = end_.index();
        return (e >= b) ? static_cast<size_type>(e - b) : size_type{0};
    }

    [[nodiscard]] size_type tail_index() const noexcept { return begin_.index(); }
    [[nodiscard]] size_type head_index() const noexcept { return end_.index(); }

    [[nodiscard]] bool empty() const noexcept {
        return begin_.index() == end_.index();
    }

private:
    iterator begin_{};
    iterator end_{};
};

// ============================================================================
// const_snapshot_view<T, Size> (read-only)
// ============================================================================

template<class T, class Size>
class const_snapshot_view
{
    static_assert(std::is_unsigned_v<Size>,
                  "[const_snapshot_view]: Size must be an unsigned integer type");

public:
    using value_type      = T;
    using size_type       = Size;
    using const_iterator  = detail::ring_iterator<T, Size, true>;
    using iterator        = const_iterator; // Always const, by design.

    const_snapshot_view() noexcept = default;

    const_snapshot_view(const_iterator b, const_iterator e) noexcept
        : begin_(b)
        , end_(e)
    {}

    // Convenience: allow constructing from mutable iterators (implicit conv exists).
    const_snapshot_view(detail::ring_iterator<T, Size, false> b,
                        detail::ring_iterator<T, Size, false> e) noexcept
        : begin_(const_iterator(b))
        , end_(const_iterator(e))
    {}

    const_iterator begin() const noexcept { return begin_; }
    const_iterator end() const noexcept { return end_; }

    const_iterator cbegin() const noexcept { return begin_; }
    const_iterator cend() const noexcept { return end_; }

    [[nodiscard]] size_type size() const noexcept {
        const size_type b = begin_.index();
        const size_type e = end_.index();
        return (e >= b) ? static_cast<size_type>(e - b) : size_type{0};
    }

    [[nodiscard]] size_type tail_index() const noexcept { return begin_.index(); }
    [[nodiscard]] size_type head_index() const noexcept { return end_.index(); }

    [[nodiscard]] bool empty() const noexcept {
        return begin_.index() == end_.index();
    }

private:
    const_iterator begin_{};
    const_iterator end_{};
};

// ============================================================================
// snapshot_traits<T, Size>
// ============================================================================

template<class T, class Size>
struct snapshot_traits
{
    using value_type     = T;
    using size_type      = Size;

    using iterator       = detail::ring_iterator<T, Size, false>;
    using const_iterator = detail::ring_iterator<T, Size, true>;

    using snapshot       = snapshot_view<T, Size>;
    using const_snapshot = const_snapshot_view<T, Size>;
};


} // namespace spsc

#endif /* SPSC_SNAPSHOT_HPP_ */
