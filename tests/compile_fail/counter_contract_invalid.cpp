#include "base/SPSCbase.hpp"

/* Deliberately matches the old shallow store/load/add/inc shape while omitting
 * value_type.  Policy<> must reject it at the extension boundary. */
struct incomplete_counter {
    void store(const reg) noexcept {}
    [[nodiscard]] reg load() const noexcept { return 0u; }
    void add(const reg) noexcept {}
    void inc() noexcept {}
};

#if defined(SPSC_TEST_DIRECT_POLICY_COUNTER)
struct direct_counter_policy {
    using counter_type = incomplete_counter;
    using geometry_type = spsc::cnt::PlainCounter<reg>;
};
using rejected_policy = spsc::SPSCbase<8u, direct_counter_policy>;
#elif defined(SPSC_TEST_DIRECT_POLICY_GEOMETRY)
struct direct_geometry_policy {
    using counter_type = spsc::cnt::PlainCounter<reg>;
    using geometry_type = incomplete_counter;
};
using rejected_policy = spsc::cap::CapacityCtrl<0u, direct_geometry_policy>;
#else
using rejected_policy = spsc::policy::Policy<incomplete_counter>;
#endif

int main()
{
    return sizeof(rejected_policy);
}
