#include "fifo.hpp"

struct invalid_alignment_policy : spsc::policy::P {
#if defined(SPSC_TEST_NEGATIVE_POLICY_ALIGNMENT)
    static constexpr int allocator_alignment = -8;
#elif defined(SPSC_TEST_NONINTEGRAL_POLICY_ALIGNMENT)
    static constexpr double allocator_alignment = 32.0;
#else
    static constexpr reg allocator_alignment = 24u;
#endif
};

using rejected_fifo = spsc::fifo<int, 8u, invalid_alignment_policy>;

int main()
{
    rejected_fifo q;
    return q.empty() ? 0 : 1;
}
