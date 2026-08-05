#include "fifo.hpp"
#include "spsc/base/spsc_slot_wrap.hpp"
#include "spsc/buffer_pool.hpp"

#include <cstdint>

int main()
{
    ::spsc::fifo<std::uint32_t, 8u, ::spsc::policy::CFA<>> queue;

    if (!queue.try_push(42u)) {
        return 1;
    }

    const auto* value = queue.try_front();
    if (value == nullptr || *value != 42u) {
        return 2;
    }

    return queue.try_pop() ? 0 : 3;
}
