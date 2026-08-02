#include <cstdint>

#include "fifo.hpp"

static_assert(sizeof(reg) == 4u,
              "H6 32-bit matrix must be compiled for a genuine 32-bit reg domain");
static_assert(SPSC_ENABLE_SHADOW_INDICES == 1,
              "H6 32-bit matrix intentionally exercises the atomic shadow gate");

using h6_policy = spsc::policy::CFA<>;
static_assert(spsc::detail::rb_use_shadow_v<h6_policy> ==
                  (SPSC_SHADOW_ALLOW_32BIT != 0),
              "32-bit shadow eligibility must exactly follow SPSC_SHADOW_ALLOW_32BIT");

namespace {

bool fifo_round_trip() noexcept
{
    spsc::fifo<std::uint32_t, 8u, h6_policy> q;

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
    return fifo_round_trip() ? 0 : 1;
}
