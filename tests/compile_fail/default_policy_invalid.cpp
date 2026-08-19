#include "base/spsc_policy.hpp"

// The compile-fail harness supplies SPSC_DEFAULT_POLICY_ATOMIC=2. The legacy
// override is intentionally binary: undefined selects the v3 FA<> default,
// while explicit 0 and 1 preserve the historical P and A<> meanings.
int main()
{
    return 0;
}
