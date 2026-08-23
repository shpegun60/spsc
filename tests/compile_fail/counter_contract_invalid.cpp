#include "base/SPSCbase.hpp"

/* Deliberately matches the old shallow store/load/add/inc shape while omitting
 * value_type.  Policy<> must reject it at the extension boundary. */
struct incomplete_counter {
    void store(const reg) noexcept {}
    [[nodiscard]] reg load() const noexcept { return 0u; }
    void add(const reg) noexcept {}
    void inc() noexcept {}
};

struct throwing_conversion_proxy {
    operator reg() const noexcept(false) { return 0u; }
};

struct throwing_load_conversion_counter {
    using value_type = reg;

    void store(const value_type) noexcept {}
    [[nodiscard]] throwing_conversion_proxy load() const noexcept { return {}; }
    [[nodiscard]] value_type load_relaxed() const noexcept { return 0u; }
    void add(const value_type) noexcept {}
    void inc() noexcept {}
};

struct throwing_relaxed_conversion_counter {
    using value_type = reg;

    void store(const value_type) noexcept {}
    [[nodiscard]] value_type load() const noexcept { return 0u; }
    [[nodiscard]] throwing_conversion_proxy load_relaxed() const noexcept {
        return {};
    }
    void add(const value_type) noexcept {}
    void inc() noexcept {}
};

struct throwing_operation_counter {
    using value_type = reg;

    void store(value_type) noexcept(false) {}
    [[nodiscard]] value_type load() const noexcept(false) { return 0u; }
    [[nodiscard]] value_type load_relaxed() const noexcept(false) { return 0u; }
    void add(value_type) noexcept(false) {}
    void inc() noexcept(false) {}
};

#if defined(SPSC_TEST_THROWING_LOAD_CONVERSION)
using invalid_counter = throwing_load_conversion_counter;
#elif defined(SPSC_TEST_THROWING_RELAXED_CONVERSION)
using invalid_counter = throwing_relaxed_conversion_counter;
#else
using invalid_counter = incomplete_counter;
#endif

#if defined(SPSC_TEST_CACHELINE_UNDERLYING_COUNTER)
using rejected_policy =
    spsc::cnt::CachelineCounter<throwing_operation_counter, 64u>;
#elif defined(SPSC_TEST_DIRECT_POLICY_COUNTER)
struct direct_counter_policy {
    using counter_type = invalid_counter;
    using geometry_type = spsc::cnt::PlainCounter<reg>;
};
using rejected_policy = spsc::SPSCbase<8u, direct_counter_policy>;
#elif defined(SPSC_TEST_DIRECT_POLICY_GEOMETRY)
struct direct_geometry_policy {
    using counter_type = spsc::cnt::PlainCounter<reg>;
    using geometry_type = invalid_counter;
};
using rejected_policy = spsc::cap::CapacityCtrl<0u, direct_geometry_policy>;
#else
using rejected_policy = spsc::policy::Policy<invalid_counter>;
#endif

int main()
{
    return sizeof(rejected_policy);
}
