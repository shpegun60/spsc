#include <cstdint>
#include <limits>

#include "fifo.hpp"

static_assert(sizeof(reg) == 4u,
              "H6 32-bit matrix must be compiled for a genuine 32-bit reg domain");
static_assert(SPSC_ENABLE_SHADOW_INDICES == 1,
              "H6 32-bit matrix intentionally exercises the atomic shadow gate");

using h6_fast_policy = spsc::policy::CFA<>;
using h6_strict_policy = spsc::policy::CA<>;
static_assert(spsc::detail::rb_use_shadow_v<h6_fast_policy> ==
                  (SPSC_SHADOW_ALLOW_32BIT != 0),
              "32-bit fast shadow eligibility must follow SPSC_SHADOW_ALLOW_32BIT");
static_assert(spsc::detail::rb_use_shadow_v<h6_strict_policy> ==
                  (SPSC_SHADOW_ALLOW_32BIT != 0),
              "32-bit strict shadow eligibility must follow SPSC_SHADOW_ALLOW_32BIT");

namespace {

template<reg Capacity, class Policy>
class shadow_alias_probe final
    : private spsc::SPSCbase<Capacity, Policy>
{
    using Base = spsc::SPSCbase<Capacity, Policy>;

    static constexpr reg kCapacity = 8u;
    static constexpr reg kMax = std::numeric_limits<reg>::max();

    void prepare(const reg head, const reg tail) noexcept
    {
        Base::clear(); // keep both endpoint-owned shadows ancient at zero
        Base::set_head(head);
        Base::set_tail(tail);
    }

public:
    shadow_alias_probe() noexcept
        : Base()
    {
        if constexpr (Capacity == 0u) {
            (void)Base::init(kCapacity);
        }
    }

    [[nodiscard]] bool unchecked_single_producer_refreshes() noexcept
    {
        const reg tail = static_cast<reg>(kMax - 4u); // 0xFFFF'FFFB
        prepare(2u, tail);                            // used == 7
        Base::producer_commit_owner(2u);              // head == 3, full
        return Base::producer_full_cached();
    }

    [[nodiscard]] bool unchecked_single_consumer_refreshes() noexcept
    {
        const reg head = static_cast<reg>(kMax - 3u); // 0xFFFF'FFFC
        const reg tail = static_cast<reg>(kMax - 4u); // one readable slot
        prepare(head, tail);
        Base::consumer_commit_owner(tail);            // tail == head, empty
        return Base::consumer_empty_cached();
    }

    [[nodiscard]] bool unchecked_bulk_producer_refreshes() noexcept
    {
        const reg tail = static_cast<reg>(kMax - 4u);
        prepare(1u, tail);                            // used == 6
        Base::advance_head_unchecked(2u);             // head == 3, full
        return Base::producer_full_cached();
    }

    [[nodiscard]] bool unchecked_bulk_consumer_refreshes() noexcept
    {
        const reg head = static_cast<reg>(kMax - 3u);
        const reg tail = static_cast<reg>(kMax - 5u); // two readable slots
        prepare(head, tail);
        Base::advance_tail_unchecked(2u);             // tail == head, empty
        return Base::consumer_empty_cached();
    }

    [[nodiscard]] bool checked_paths_remain_valid() noexcept
    {
        const reg full_tail = static_cast<reg>(kMax - 4u);
        prepare(2u, full_tail);                       // used == 7
        const auto write = Base::producer_single_snapshot();
        if (!write.available) {
            return false;
        }
        Base::producer_commit_single(write);
        if (!Base::producer_full_cached()) {
            return false;
        }

        const reg empty_head = static_cast<reg>(kMax - 3u);
        const reg empty_tail = static_cast<reg>(kMax - 4u);
        prepare(empty_head, empty_tail);              // one readable slot
        const auto read = Base::consumer_single_snapshot();
        if (!read.available) {
            return false;
        }
        Base::consumer_commit_single(read);
        if (!Base::consumer_empty_cached()) {
            return false;
        }

        prepare(1u, full_tail);                       // six occupied slots
        if (!Base::producer_can_write_cached(2u)) {
            return false;
        }
        Base::advance_head_checked(2u);
        if (!Base::producer_full_cached()) {
            return false;
        }

        const reg two_tail = static_cast<reg>(kMax - 5u);
        prepare(empty_head, two_tail);                // two readable slots
        if (!Base::consumer_can_read_cached(2u)) {
            return false;
        }
        Base::advance_tail_checked(2u);
        return Base::consumer_empty_cached();
    }
};

template<reg Capacity, class Policy>
[[nodiscard]] bool hostile_shadow_alias_suite() noexcept
{
    shadow_alias_probe<Capacity, Policy> producer_single;
    shadow_alias_probe<Capacity, Policy> consumer_single;
    shadow_alias_probe<Capacity, Policy> producer_bulk;
    shadow_alias_probe<Capacity, Policy> consumer_bulk;
    shadow_alias_probe<Capacity, Policy> checked;

    return producer_single.unchecked_single_producer_refreshes() &&
           consumer_single.unchecked_single_consumer_refreshes() &&
           producer_bulk.unchecked_bulk_producer_refreshes() &&
           consumer_bulk.unchecked_bulk_consumer_refreshes() &&
           checked.checked_paths_remain_valid();
}

template<class Policy>
bool fifo_round_trip() noexcept
{
    spsc::fifo<std::uint32_t, 8u, Policy> q;

    for (std::uint32_t value = 0u; value < 8u; ++value) {
        if (!q.try_push(value)) {
            return false;
        }
    }
    if (!q.full() || q.try_push(99u)) {
        return false;
    }

    for (std::uint32_t expected = 0u; expected < 4u; ++expected) {
        const auto* front = q.try_front();
        if (!front || *front != expected) {
            return false;
        }
        q.pop();
    }

    for (std::uint32_t value = 8u; value < 12u; ++value) {
        if (!q.try_push(value)) {
            return false;
        }
    }

    for (std::uint32_t expected = 4u; expected < 12u; ++expected) {
        const auto* front = q.try_front();
        if (!front || *front != expected) {
            return false;
        }
        q.pop();
    }

    return q.empty() && q.size() == 0u && q.free() == q.capacity();
}

} // namespace

int main()
{
    return fifo_round_trip<h6_fast_policy>() &&
                   fifo_round_trip<h6_strict_policy>() &&
                   hostile_shadow_alias_suite<8u, h6_fast_policy>() &&
                   hostile_shadow_alias_suite<0u, h6_fast_policy>() &&
                   hostile_shadow_alias_suite<8u, h6_strict_policy>() &&
                   hostile_shadow_alias_suite<0u, h6_strict_policy>()
               ? 0
               : 1;
}
