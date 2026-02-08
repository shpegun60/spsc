/*
 * spsc_regions.hpp
 *
 * Shared POD types for "bulk" APIs (claim_read/claim_write) across SPSC containers.
 *
 * Why this exists:
 * - Multiple buffers used to copy-paste the same tiny region structs.
 * - These structs are part of public API, so they should stay simple and stable.
 */

#ifndef SPSC_BASE_SPSC_REGIONS_HPP_
#define SPSC_BASE_SPSC_REGIONS_HPP_

#include <cstddef>
#include <type_traits>

// Provides SPSC_HAS_SPAN and includes <span> when available.
#include "spsc_tools.hpp"

namespace spsc {

// Tag type for explicitly unsafe low-level APIs (claim_read/claim_write).
// Use: obj.claim_write(spsc::unsafe, n);
struct unsafe_t { explicit constexpr unsafe_t() = default; };
inline constexpr unsafe_t unsafe{};

namespace bulk {

// Generic pair of regions (wrap split).
template <class RegionT, class SizeT>
struct region_pair {
    RegionT first{};
    RegionT second{};
    SizeT   total{0u};

    [[nodiscard]] constexpr bool empty() const noexcept { return total == 0u; }
};

// ---------------------------------------------------------------------------------------------
// Initialized contiguous region (e.g. FIFO read, queue read)
// ---------------------------------------------------------------------------------------------
template <class T, class SizeT>
struct init_region {
    T    *ptr{nullptr};
    SizeT count{0u};

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }

#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<T> span() const noexcept { return {ptr, static_cast<std::size_t>(count)}; }
#endif /* SPSC_HAS_SPAN */
};

// ---------------------------------------------------------------------------------------------
// Uninitialized contiguous region (raw storage) (e.g. queue write)
// ---------------------------------------------------------------------------------------------
template <class T, class SizeT>
struct uninit_region {
    std::byte *raw{nullptr};
    SizeT      count{0u};

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }

    // Explicitly named to discourage assignment without starting lifetime.
    [[nodiscard]] T *ptr_uninit() const noexcept { return reinterpret_cast<T *>(raw); }

#if SPSC_HAS_SPAN
    // Raw bytes view (safe for uninitialized storage).
    [[nodiscard]] std::span<std::byte> bytes() const noexcept {
        return {raw, static_cast<std::size_t>(count) * sizeof(T)};
    }
#endif /* SPSC_HAS_SPAN */
};

// ---------------------------------------------------------------------------------------------
// Generic contiguous region: PtrT is a pointer-to-element (e.g. T*, const T*, void* const*).
// ---------------------------------------------------------------------------------------------
template <class PtrT, class SizeT>
struct region {
    static_assert(std::is_pointer_v<PtrT>, "spsc::bulk::region requires PtrT to be a pointer type");

    PtrT  ptr{nullptr};
    SizeT count{0u};

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }

#if SPSC_HAS_SPAN
    using span_value_type = std::remove_pointer_t<PtrT>;
    [[nodiscard]] std::span<span_value_type> span() const noexcept {
        return {ptr, static_cast<std::size_t>(count)};
    }
#endif /* SPSC_HAS_SPAN */
};

// Convenience: pair of generic regions.
template <class PtrT, class SizeT>
using regions = region_pair<region<PtrT, SizeT>, SizeT>;

// ---------------------------------------------------------------------------------------------
// Slot-list region: used by pool / pool_view / typed_pool.
// ElemPtr is the element pointer type stored in the slot array (e.g. T*).
// The region itself is a span over that slot array.
// ---------------------------------------------------------------------------------------------
template <class ElemPtr, class SizeT>
struct slot_region {
    static_assert(std::is_pointer_v<ElemPtr>, "spsc::bulk::slot_region expects ElemPtr to be a pointer type (e.g. T*)");

    ElemPtr const *ptr{nullptr};
    SizeT          count{0u};

    [[nodiscard]] constexpr bool empty() const noexcept { return count == 0u; }

#if SPSC_HAS_SPAN
    [[nodiscard]] std::span<ElemPtr const> span() const noexcept {
        return {ptr, static_cast<std::size_t>(count)};
    }
#endif /* SPSC_HAS_SPAN */
};

template <class ElemPtr, class SizeT>
using slot_regions = region_pair<slot_region<ElemPtr, SizeT>, SizeT>;

} // namespace bulk
} // namespace spsc

#endif /* SPSC_BASE_SPSC_REGIONS_HPP_ */
