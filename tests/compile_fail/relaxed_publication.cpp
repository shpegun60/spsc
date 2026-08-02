#include <cstdint>

#include "fifo.hpp"

// This translation unit must never compile. Atomic index publication needs an
// acquire-capable reader and a release-capable writer; relaxed_orders is kept
// only as an explicitly rejected marker for externally synchronized experiments.
using relaxed_fifo = spsc::fifo<
    std::uint32_t,
    8u,
    spsc::policy::A<spsc::cnt::relaxed_orders>>;

int main()
{
    relaxed_fifo q;
    return q.empty() ? 0 : 1;
}
