#ifndef SPSC_TEST_SPSC_LAYOUT_HPP
#define SPSC_TEST_SPSC_LAYOUT_HPP

/*
 * Friend-only layout probe for SPSCbase H2/H5 tests.
 *
 * This header is test-only. Production SPSCbase exposes no state or layout API;
 * it names the probe as a friend without changing its class definition by macro.
 */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "base/SPSCbase.hpp"
#include "base/spsc_counter.hpp"
#include "base/spsc_policy.hpp"

namespace spsc::test_layout {

struct address_range {
    std::uintptr_t begin{0u};
    std::uintptr_t end{0u};
};

[[nodiscard]] inline address_range address_range_of(const void *address,
                                                     const std::size_t bytes) noexcept {
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    return {begin, static_cast<std::uintptr_t>(begin + bytes)};
}

[[nodiscard]] inline bool ranges_share_cache_line(const address_range a,
                                                   const address_range b) noexcept {
    if (a.begin == a.end || b.begin == b.end) {
        return false;
    }

    const auto last_a = static_cast<std::uintptr_t>(a.end - 1u);
    const auto last_b = static_cast<std::uintptr_t>(b.end - 1u);
    const auto first_line_a = a.begin / SPSC_CACHELINE_BYTES;
    const auto last_line_a = last_a / SPSC_CACHELINE_BYTES;
    const auto first_line_b = b.begin / SPSC_CACHELINE_BYTES;
    const auto last_line_b = last_b / SPSC_CACHELINE_BYTES;
    return !(last_line_a < first_line_b || last_line_b < first_line_a);
}

struct index_layout_snapshot {
    bool shadows_enabled{false};
    const void *head{nullptr};
    const void *tail{nullptr};
    const void *prod_shadow_tail{nullptr};
    const void *cons_shadow_head{nullptr};
    const void *producer_block{nullptr};
    const void *consumer_block{nullptr};
    std::size_t counter_bytes{0u};
    std::size_t counter_alignment{0u};
    std::size_t producer_block_bytes{0u};
    std::size_t consumer_block_bytes{0u};
};

/* Public test subject with no extra data members. Passing its base subobject to
 * the friend probe exercises the real SPSCbase storage layout for both static
 * and dynamic geometry.
 */
template<reg C, class PolicyT>
class layout_subject final : public ::spsc::SPSCbase<C, PolicyT> {
    using Base = ::spsc::SPSCbase<C, PolicyT>;

public:
    layout_subject() noexcept = default;

    template<reg C_ = C, std::enable_if_t<C_ == 0u, int> = 0>
    explicit layout_subject(const reg capacity) noexcept
        : Base(capacity) {}
};

/* Test-only public surface for H3. It makes the otherwise protected direct
 * observations callable without exposing anything in the production API, and
 * separately exposes the endpoint-local cached helpers so tests can establish
 * a non-trivial shadow state before exercising an observer query.
 */
template<reg C, class PolicyT>
class observer_subject final : public ::spsc::SPSCbase<C, PolicyT> {
    using Base = ::spsc::SPSCbase<C, PolicyT>;

public:
    observer_subject() noexcept = default;

    template<reg C_ = C, std::enable_if_t<C_ == 0u, int> = 0>
    explicit observer_subject(const reg capacity) noexcept
        : Base(capacity) {}

    using Base::can_read;
    using Base::can_write;
    using Base::empty;
    using Base::free;
    using Base::full;
    using Base::read_size;
    using Base::size;
    using Base::write_size;

    [[nodiscard]] bool producer_full_cached_for_test() const noexcept {
        return Base::producer_full_cached();
    }

    [[nodiscard]] bool consumer_empty_cached_for_test() const noexcept {
        return Base::consumer_empty_cached();
    }

    void set_indices_for_test(const reg head, const reg tail) noexcept {
        Base::set_head(head);
        Base::set_tail(tail);
    }
};

/* Test-only public surface for H5 counter-wrap checks. It reaches protected
 * SPSCbase operations through derivation, so production headers gain neither a
 * test switch nor a public raw-index API.
 */
template<reg C, class PolicyT>
class counter_wrap_subject final : public ::spsc::SPSCbase<C, PolicyT> {
    using Base = ::spsc::SPSCbase<C, PolicyT>;

public:
    counter_wrap_subject() noexcept = default;

    template<reg C_ = C, std::enable_if_t<C_ == 0u, int> = 0>
    explicit counter_wrap_subject(const reg capacity) noexcept
        : Base(capacity) {}

    using Base::can_read;
    using Base::can_write;
    using Base::empty;
    using Base::free;
    using Base::full;
    using Base::read_size;
    using Base::size;
    using Base::write_size;

    [[nodiscard]] bool restore_indices_for_test(const reg head,
                                                const reg tail) noexcept {
        if constexpr (C == 0u) {
            return Base::init(Base::capacity(), head, tail);
        } else {
            return Base::init(head, tail);
        }
    }

    void set_indices_unchecked_for_test(const reg head, const reg tail) noexcept {
        Base::set_head(head);
        Base::set_tail(tail);
    }

    [[nodiscard]] reg head_for_test() const noexcept { return Base::head(); }
    [[nodiscard]] reg tail_for_test() const noexcept { return Base::tail(); }
    [[nodiscard]] reg write_index_for_test() const noexcept { return Base::write_index(); }
    [[nodiscard]] reg read_index_for_test() const noexcept { return Base::read_index(); }

    [[nodiscard]] bool producer_full_cached_for_test() const noexcept {
        return Base::producer_full_cached();
    }
    [[nodiscard]] bool consumer_empty_cached_for_test() const noexcept {
        return Base::consumer_empty_cached();
    }
    [[nodiscard]] reg producer_write_size_cached_for_test() const noexcept {
        return Base::producer_write_size_cached();
    }
    [[nodiscard]] reg consumer_read_size_cached_for_test() const noexcept {
        return Base::consumer_read_size_cached();
    }

    void increment_head_for_test() noexcept { Base::increment_head(); }
    void increment_tail_for_test() noexcept { Base::increment_tail(); }
};

inline constexpr reg kSubcachelineAtomicAlignment =
    static_cast<reg>(alignof(typename ::spsc::policy::A<>::counter_type));

using subcacheline_atomic_policy =
    ::spsc::policy::CacheAligned<::spsc::policy::A<>, kSubcachelineAtomicAlignment>;

static_assert(kSubcachelineAtomicAlignment < SPSC_CACHELINE_BYTES,
              "The H2 sub-cacheline test requires an atomic counter naturally smaller than a cache line");

inline constexpr std::size_t kOverAlignedCounterAlignment =
    static_cast<std::size_t>(SPSC_CACHELINE_BYTES) * 2u;

static_assert((kOverAlignedCounterAlignment & (kOverAlignedCounterAlignment - 1u)) == 0u,
              "The over-aligned counter test requires a power-of-two alignment");

struct alignas(kOverAlignedCounterAlignment) over_aligned_atomic_counter {
    static constexpr bool is_atomic = true;
    using value_type = reg;

    std::atomic<reg> value{0u};

    void store(const reg next) noexcept {
        value.store(next, std::memory_order_release);
    }
    [[nodiscard]] reg load() const noexcept {
        return value.load(std::memory_order_acquire);
    }
    void add(const reg delta) noexcept {
        (void)value.fetch_add(delta, std::memory_order_acq_rel);
    }
    void inc() noexcept {
        (void)value.fetch_add(1u, std::memory_order_acq_rel);
    }
};

using over_aligned_atomic_policy =
    ::spsc::policy::Policy<over_aligned_atomic_counter,
                           ::spsc::cnt::PlainCounter<reg>>;

static_assert(alignof(over_aligned_atomic_counter) > SPSC_CACHELINE_BYTES,
              "The custom atomic counter must exercise an over-cacheline alignment");

inline constexpr bool kExpectedAtomicShadow =
    (SPSC_ENABLE_SHADOW_INDICES != 0) &&
    ((std::numeric_limits<reg>::digits >= 64) || (SPSC_SHADOW_ALLOW_32BIT != 0));

static_assert(!::spsc::detail::rb_use_shadow_v<::spsc::policy::P>);
static_assert(!::spsc::detail::rb_use_shadow_v<::spsc::policy::V>);
static_assert(!::spsc::detail::rb_use_shadow_v<::spsc::policy::VV>);
static_assert(!::spsc::detail::rb_use_shadow_v<::spsc::policy::CP>);
static_assert(!::spsc::detail::rb_use_shadow_v<::spsc::policy::CV>);
static_assert(!::spsc::detail::rb_use_shadow_v<::spsc::policy::CVV>);

static_assert(::spsc::detail::rb_use_shadow_v<::spsc::policy::A<>> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<::spsc::policy::FA<>> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<::spsc::policy::AA<>> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<::spsc::policy::CA<>> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<::spsc::policy::CFA<>> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<::spsc::policy::CAA<>> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<subcacheline_atomic_policy> ==
                  kExpectedAtomicShadow);
static_assert(::spsc::detail::rb_use_shadow_v<over_aligned_atomic_policy> ==
                  kExpectedAtomicShadow);

} // namespace spsc::test_layout

namespace spsc::detail {

template<reg C, class PolicyT>
struct spscbase_layout_probe {
    using base_type = ::spsc::SPSCbase<C, PolicyT>;
    using storage_type = typename base_type::IndexStorage;
    using counter_type = typename PolicyT::counter_type;

    static constexpr bool kUseShadow = base_type::kUseShadow;

    static_assert(storage_type::kHasShadows == kUseShadow,
                  "Probe and storage must agree on shadow presence");
    static_assert(!kUseShadow || !std::is_standard_layout<counter_type>::value ||
                      std::is_standard_layout<storage_type>::value,
                  "Standard-layout counters require standard-layout storage");

    [[nodiscard]] static ::spsc::test_layout::index_layout_snapshot
    inspect(const base_type& base) noexcept {
        const auto& indices = base._indices;

        if constexpr (kUseShadow) {
            using producer_block_type = typename storage_type::producer_block_type;
            using consumer_block_type = typename storage_type::consumer_block_type;

            return {
                true,
                static_cast<const void*>(&indices.producer.head),
                static_cast<const void*>(&indices.consumer.tail),
                static_cast<const void*>(&indices.producer.prod_shadow_tail),
                static_cast<const void*>(&indices.consumer.cons_shadow_head),
                static_cast<const void*>(&indices.producer),
                static_cast<const void*>(&indices.consumer),
                sizeof(counter_type),
                alignof(counter_type),
                sizeof(producer_block_type),
                sizeof(consumer_block_type)
            };
        } else {
            return {
                false,
                static_cast<const void*>(&indices.head),
                static_cast<const void*>(&indices.tail),
                nullptr,
                nullptr,
                nullptr,
                nullptr,
                sizeof(counter_type),
                alignof(counter_type),
                0u,
                0u
            };
        }
    }

    [[nodiscard]] static reg producer_shadow_value(const base_type& base) noexcept {
        if constexpr (kUseShadow) {
            return base._indices.prod_shadow_tail();
        } else {
            (void)base;
            return 0u;
        }
    }

    [[nodiscard]] static reg consumer_shadow_value(const base_type& base) noexcept {
        if constexpr (kUseShadow) {
            return base._indices.cons_shadow_head();
        } else {
            (void)base;
            return 0u;
        }
    }
};

} // namespace spsc::detail

#endif // SPSC_TEST_SPSC_LAYOUT_HPP
