#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

#include "fifo.hpp"
#include "fifo_view.hpp"
#include "pool.hpp"
#include "pool_view.hpp"
#include "queue.hpp"
#include "typed_pool.hpp"

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

template<bool SingleWriter>
class counting_atomic_counter
{
public:
    static constexpr bool is_atomic = true;
    static constexpr bool is_single_writer = SingleWriter;
    using value_type = reg;

    static inline reg synchronized_loads = 0u;

    static void reset_load_count() noexcept { synchronized_loads = 0u; }

    void store(const value_type value) noexcept { value_ = value; }
    [[nodiscard]] value_type load() const noexcept
    {
        ++synchronized_loads;
        return value_;
    }
    [[nodiscard]] value_type load_relaxed() const noexcept { return value_; }
    void add(const value_type value) noexcept { value_ += value; }
    void inc() noexcept { ++value_; }

private:
    value_type value_{0u};
};

template<bool SingleWriter>
using counting_policy = spsc::policy::Policy<
    counting_atomic_counter<SingleWriter>, spsc::policy::PlainCounter<reg>>;

template<reg Capacity, class Policy>
class shadow_alias_probe final
    : private spsc::SPSCbase<Capacity, Policy>
{
    using Base = spsc::SPSCbase<Capacity, Policy>;

    static constexpr reg kCapacity = 8u;
    static constexpr reg kMax = std::numeric_limits<reg>::max();

    void prepare_stale(const reg head, const reg tail) noexcept
    {
        Base::clear(); // keep both endpoint-owned shadows ancient at zero
        Base::set_head(head);
        Base::set_tail(tail);
    }

    void prepare_synced(const reg head, const reg tail) noexcept
    {
        prepare_stale(head, tail);
        Base::sync_cache();
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
        prepare_stale(2u, tail);                      // used == 7
        Base::producer_commit_owner(2u);              // head == 3, full
        return Base::producer_full_cached();
    }

    [[nodiscard]] bool unchecked_single_consumer_refreshes() noexcept
    {
        const reg head = static_cast<reg>(kMax - 3u); // 0xFFFF'FFFC
        const reg tail = static_cast<reg>(kMax - 4u); // one readable slot
        prepare_stale(head, tail);
        Base::consumer_commit_owner(tail);            // tail == head, empty
        return Base::consumer_empty_cached();
    }

    [[nodiscard]] bool unchecked_bulk_producer_refreshes() noexcept
    {
        const reg tail = static_cast<reg>(kMax - 4u);
        prepare_stale(1u, tail);                      // used == 6
        Base::advance_head_unchecked(2u);             // head == 3, full
        return Base::producer_full_cached();
    }

    [[nodiscard]] bool unchecked_bulk_consumer_refreshes() noexcept
    {
        const reg head = static_cast<reg>(kMax - 3u);
        const reg tail = static_cast<reg>(kMax - 5u); // two readable slots
        prepare_stale(head, tail);
        Base::advance_tail_unchecked(2u);             // tail == head, empty
        return Base::consumer_empty_cached();
    }

    [[nodiscard]] bool checked_paths_remain_valid() noexcept
    {
        const reg full_tail = static_cast<reg>(kMax - 4u);
        prepare_synced(2u, full_tail);                // used == 7
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
        prepare_synced(empty_head, empty_tail);       // one readable slot
        const auto read = Base::consumer_single_snapshot();
        if (!read.available) {
            return false;
        }
        Base::consumer_commit_single(read);
        if (!Base::consumer_empty_cached()) {
            return false;
        }

        prepare_synced(1u, full_tail);                // six occupied slots
        if (!Base::producer_can_write_cached(2u)) {
            return false;
        }
        Base::advance_head_checked(2u);
        if (!Base::producer_full_cached()) {
            return false;
        }

        const reg two_tail = static_cast<reg>(kMax - 5u);
        prepare_synced(empty_head, two_tail);         // two readable slots
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

template<bool SingleWriter>
bool checked_queue_paths_preserve_shadow() noexcept
{
    using policy_type = counting_policy<SingleWriter>;
    using counter_type = typename policy_type::counter_type;
    spsc::queue<std::uint32_t, 8u, policy_type> q;

    if (!q.is_valid() || !q.try_push(10u) || !q.try_push(11u) ||
        !q.try_push(12u) || !q.try_pop(2u)) {
        return false;
    }

    counter_type::reset_load_count();
    const auto* remaining = q.try_front();
    if (!remaining || *remaining != 12u) {
        return false;
    }
    if constexpr (spsc::detail::rb_use_shadow_v<policy_type>) {
        if (counter_type::synchronized_loads != 0u) {
            return false;
        }
    }
    if (!q.try_pop()) {
        return false;
    }

    if (!q.try_push(20u) || !q.try_push(21u)) {
        return false;
    }
    const auto snapshot = q.make_snapshot();
    if (!q.try_push(22u) || !q.try_consume(snapshot)) {
        return false;
    }

    counter_type::reset_load_count();
    const auto* after_snapshot = q.try_front();
    if (!after_snapshot || *after_snapshot != 22u) {
        return false;
    }
    if constexpr (spsc::detail::rb_use_shadow_v<policy_type>) {
        if (counter_type::synchronized_loads != 0u) {
            return false;
        }
    }

    return q.try_pop() && q.empty();
}

// Shared scenario for every snapshot-consuming container: publish two
// elements, snapshot them, publish a third, then commit the snapshot through
// checked try_consume(). The commit must preserve the consumer shadow proven
// by the validation, so the following try_front() must not need a
// synchronized head reload.
template<class Policy, class Container, class Push, class FrontIs>
bool checked_consume_preserves_shadow_case(Container& c, Push push,
                                           FrontIs front_is) noexcept
{
    using counter_type = typename Policy::counter_type;

    if (!push(c, 20u) || !push(c, 21u)) {
        return false;
    }
    const auto snapshot = c.make_snapshot();
    if (!push(c, 22u) || !c.try_consume(snapshot)) {
        return false;
    }

    counter_type::reset_load_count();
    if (!front_is(c, 22u)) {
        return false;
    }
    if constexpr (spsc::detail::rb_use_shadow_v<Policy>) {
        if (counter_type::synchronized_loads != 0u) {
            return false;
        }
    }

    return c.try_pop(1u) && c.empty();
}

template<bool SingleWriter>
bool checked_consume_paths_preserve_shadow() noexcept
{
    using policy_type = counting_policy<SingleWriter>;

    const auto push_value = [](auto& c, const std::uint32_t v) noexcept {
        return c.try_push(v);
    };
    const auto typed_front_is = [](auto& c, const std::uint32_t v) noexcept {
        const auto* front = c.try_front();
        return front != nullptr && *front == v;
    };
    const auto raw_front_is = [](auto& c, const std::uint32_t v) noexcept {
        const void* front = c.try_front();
        if (front == nullptr) {
            return false;
        }
        std::uint32_t out{};
        std::memcpy(&out, front, sizeof(out));
        return out == v;
    };

    spsc::fifo<std::uint32_t, 8u, policy_type> f;
    if (!f.is_valid() ||
        !checked_consume_preserves_shadow_case<policy_type>(f, push_value,
                                                            typed_front_is)) {
        return false;
    }

    std::array<std::uint32_t, 8u> view_storage{};
    spsc::fifo_view<std::uint32_t, 8u, policy_type> fv;
    if (!fv.attach(view_storage) ||
        !checked_consume_preserves_shadow_case<policy_type>(fv, push_value,
                                                            typed_front_is)) {
        return false;
    }

    spsc::pool<8u, policy_type> p{
        static_cast<reg>(sizeof(std::uint32_t))};
    if (!p.is_valid() ||
        !checked_consume_preserves_shadow_case<policy_type>(p, push_value,
                                                            raw_front_is)) {
        return false;
    }

    std::array<std::array<unsigned char, sizeof(std::uint32_t)>, 8u> payload{};
    void* slots[8u] = {};
    for (std::size_t i = 0u; i < 8u; ++i) {
        slots[i] = payload[i].data();
    }
    spsc::pool_view<8u, policy_type> pv;
    if (!pv.attach(slots, static_cast<reg>(sizeof(std::uint32_t))) ||
        !checked_consume_preserves_shadow_case<policy_type>(pv, push_value,
                                                            raw_front_is)) {
        return false;
    }

    spsc::typed_pool<std::uint32_t, 8u, policy_type> tp;
    if (!tp.is_valid() ||
        !checked_consume_preserves_shadow_case<policy_type>(tp, push_value,
                                                            typed_front_is)) {
        return false;
    }

    return true;
}

template<class Policy>
[[nodiscard]] bool h6_loads_clean() noexcept
{
    if constexpr (spsc::detail::rb_use_shadow_v<Policy>) {
        return Policy::counter_type::synchronized_loads == 0u;
    } else {
        return true;
    }
}

// Single RAII guards acquire through checked claim/front, so their commit
// must also stay on the checked path: after a guard commits, the next guard
// acquisition on the same endpoint must be served from the preserved shadow
// without a synchronized opposite-counter reload.
template<bool SingleWriter>
bool checked_single_guards_preserve_shadow() noexcept
{
    using policy_type = counting_policy<SingleWriter>;
    using counter_type = typename policy_type::counter_type;

    // fifo: assignment-based guards.
    {
        spsc::fifo<std::uint32_t, 8u, policy_type> f;
        {
            auto g = f.scoped_write();
            if (!g) { return false; }
            *g = 10u;
            g.commit();
        }
        counter_type::reset_load_count();
        {
            auto g = f.scoped_write();
            if (!g) { return false; }
            *g = 11u;
            g.commit();
        }
        {
            auto g = f.scoped_write();
            if (!g) { return false; }
            g.cancel();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }

        if (f.try_front() == nullptr) { return false; } // warm consumer shadow
        counter_type::reset_load_count();
        {
            auto g = f.scoped_read();
            if (!g || *g != 10u) { return false; }
            g.commit();
        }
        {
            auto g = f.scoped_read();
            if (!g || *g != 11u) { return false; }
            g.cancel();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }
        if (!f.try_pop() || !f.empty()) { return false; }
    }

    // fifo_view: same guard family over external storage.
    {
        std::array<std::uint32_t, 8u> storage{};
        spsc::fifo_view<std::uint32_t, 8u, policy_type> v;
        if (!v.attach(storage)) { return false; }
        {
            auto g = v.scoped_write();
            if (!g) { return false; }
            *g = 20u;
            g.commit();
        }
        counter_type::reset_load_count();
        {
            auto g = v.scoped_write();
            if (!g) { return false; }
            *g = 21u;
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }

        if (v.try_front() == nullptr) { return false; }
        counter_type::reset_load_count();
        {
            auto g = v.scoped_read();
            if (!g || *g != 20u) { return false; }
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }
        if (!v.try_pop() || !v.empty()) { return false; }
    }

    // pool: raw byte slots.
    {
        spsc::pool<8u, policy_type> p{
            static_cast<reg>(sizeof(std::uint32_t))};
        if (!p.is_valid()) { return false; }
        const auto write_value = [](auto& guard, const std::uint32_t value) {
            std::memcpy(guard.get(), &value, sizeof(value));
        };
        const auto read_value = [](auto& guard) {
            std::uint32_t out{};
            std::memcpy(&out, guard.get(), sizeof(out));
            return out;
        };
        {
            auto g = p.scoped_write();
            if (!g) { return false; }
            write_value(g, 30u);
            g.commit();
        }
        counter_type::reset_load_count();
        {
            auto g = p.scoped_write();
            if (!g) { return false; }
            write_value(g, 31u);
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }

        if (p.try_front() == nullptr) { return false; }
        counter_type::reset_load_count();
        {
            auto g = p.scoped_read();
            if (!g || read_value(g) != 30u) { return false; }
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }
        if (!p.try_pop() || !p.empty()) { return false; }
    }

    // typed_pool: guard-managed object lifetime.
    {
        spsc::typed_pool<std::uint32_t, 8u, policy_type> tp;
        if (!tp.is_valid()) { return false; }
        {
            auto g = tp.scoped_write();
            if (!g) { return false; }
            (void)g.emplace(40u);
            g.commit();
        }
        counter_type::reset_load_count();
        {
            auto g = tp.scoped_write();
            if (!g) { return false; }
            (void)g.emplace(41u);
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }

        if (tp.try_front() == nullptr) { return false; }
        counter_type::reset_load_count();
        {
            auto g = tp.scoped_read();
            if (!g || *g.get() != 40u) { return false; }
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }
        if (!tp.try_pop() || !tp.empty()) { return false; }
    }

    // queue: manual-lifetime guards via emplace.
    {
        spsc::queue<std::uint32_t, 8u, policy_type> q;
        if (!q.is_valid()) { return false; }
        {
            auto g = q.scoped_write();
            if (!g) { return false; }
            (void)g.emplace(50u);
            g.commit();
        }
        counter_type::reset_load_count();
        {
            auto g = q.scoped_write();
            if (!g) { return false; }
            (void)g.emplace(51u);
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }

        if (q.try_front() == nullptr) { return false; }
        counter_type::reset_load_count();
        {
            auto g = q.scoped_read();
            if (!g || *g.get() != 50u) { return false; }
            g.commit();
        }
        if (!h6_loads_clean<policy_type>()) { return false; }
        if (!q.try_pop() || !q.empty()) { return false; }
    }

    return true;
}

} // namespace

int main()
{
    return fifo_round_trip<h6_fast_policy>() &&
                   fifo_round_trip<h6_strict_policy>() &&
                   hostile_shadow_alias_suite<8u, h6_fast_policy>() &&
                   hostile_shadow_alias_suite<0u, h6_fast_policy>() &&
                   hostile_shadow_alias_suite<8u, h6_strict_policy>() &&
                   hostile_shadow_alias_suite<0u, h6_strict_policy>() &&
                   checked_queue_paths_preserve_shadow<true>() &&
                   checked_queue_paths_preserve_shadow<false>() &&
                   checked_consume_paths_preserve_shadow<true>() &&
                   checked_consume_paths_preserve_shadow<false>() &&
                   checked_single_guards_preserve_shadow<true>() &&
                   checked_single_guards_preserve_shadow<false>()
               ? 0
               : 1;
}
