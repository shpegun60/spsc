#include "latest.hpp"

#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace {

// Actual atomic publication operations, with a quiescent test-only restore
// hook. Only the endpoint counters use this backend; geometry stays plain.
template<class AtomicCounter>
struct seeded_counter : AtomicCounter {
    static inline seeded_counter* endpoints[2]{};
    static inline unsigned constructed = 0u;

    seeded_counter() noexcept {
        if (constructed < 2u) {
            endpoints[constructed++] = this;
        }
    }

    static void reset_registry() noexcept { constructed = 0u; }

    [[nodiscard]] static bool restore_empty(const reg value) noexcept {
        if (constructed != 2u) {
            return false;
        }
        endpoints[0]->store(value);
        endpoints[1]->store(value);
        return true;
    }
};

template<class Q>
[[nodiscard]] bool front_is(Q& q, const std::uint32_t expected) noexcept {
    const auto* slot = q.try_front();
    if (!slot) {
        return false;
    }
    std::uint32_t actual = 0u;
    std::memcpy(&actual, slot, sizeof(actual));
    return actual == expected;
}

template<class Counter, class Q>
[[nodiscard]] bool newest_and_sticky_pop_across_counter_wrap(Q& q) {
    static_assert(std::is_same_v<typename Counter::value_type, reg>);
    const reg near_wrap = std::numeric_limits<reg>::max() - 2u;
    if (!q.is_valid() || !Counter::restore_empty(near_wrap) ||
        !q.try_push(std::uint32_t{1}) || !front_is(q, 1u) ||
        !q.try_push(std::uint32_t{2}) ||
        !q.try_push(std::uint32_t{3}) || !front_is(q, 3u)) {
        return false;
    }
    // Head crossed max(reg) -> 0. A fresh front must replace the
    // still-valid older consumer shadow. A later publish must survive pop().
    if (!q.try_push(std::uint32_t{4}) || !q.full() ||
        q.try_push(std::uint32_t{99}) || !q.try_pop() || q.size() != 1u ||
        !front_is(q, 4u) || !q.try_pop() || !q.empty()) {
        return false;
    }
    for (std::uint32_t value = 5u; value <= 8u; ++value) {
        if (!q.try_push(value)) {
            return false;
        }
    }
    return q.full() && !q.try_push(std::uint32_t{99}) && front_is(q, 8u) &&
           q.try_pop() && q.empty() && q.free() == 4u;
}

template<class AtomicCounter>
[[nodiscard]] bool all_latest_flavors() {
    using Counter = seeded_counter<AtomicCounter>;
    using Policy = spsc::policy::Policy<Counter, spsc::cnt::PlainCounter<reg>>;
    bool ok = true;
    Counter::reset_registry();
    {
        spsc::latest<std::uint32_t, 4u, Policy> q;
        ok = newest_and_sticky_pop_across_counter_wrap<Counter>(q);
    }
    Counter::reset_registry();
    {
        spsc::latest<std::uint32_t, 0u, Policy> q;
        ok = q.init(4u) && newest_and_sticky_pop_across_counter_wrap<Counter>(q) && ok;
    }
    Counter::reset_registry();
    {
        spsc::latest<void, 0u, Policy> q;
        ok = q.init(4u, sizeof(std::uint32_t)) &&
             newest_and_sticky_pop_across_counter_wrap<Counter>(q) && ok;
    }
    return ok;
}

} // namespace

int main() {
    // The allocation-backed flavor rejects volatile payloads; the inline
    // flavor still supports its existing claim/publish assignment workflow.
    spsc::latest<volatile std::uint32_t, 4u, spsc::policy::P> volatile_slots;
    volatile_slots.claim() = 42u;
    volatile_slots.publish();
    const bool volatile_ok = volatile_slots.front() == 42u;
    volatile_slots.pop();
    return volatile_ok && volatile_slots.empty() &&
                   all_latest_flavors<spsc::cnt::FastAtomicCounter<reg>>() &&
                   all_latest_flavors<spsc::cnt::AtomicCounter<reg>>()
               ? 0
               : 1;
}
