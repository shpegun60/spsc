#include "src/spsc/array_fifo.hpp"
#include "src/spsc/buffer_pool.hpp"
#include "src/spsc/chunk.hpp"
#include "src/spsc/chunk_fifo.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/fifo_view.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/pool.hpp"
#include "src/spsc/pool_view.hpp"
#include "src/spsc/queue.hpp"
#include "src/spsc/typed_pool.hpp"

#include <cstdint>

int main() {
    using queue_type = ::spsc::fifo<std::uint32_t, 8u, ::spsc::policy::CFA<>>;

    queue_type queue;
    if (!queue.try_push(42u)) {
        return 1;
    }
    const auto* value = queue.try_front();
    if (value == nullptr || *value != 42u) {
        return 2;
    }
    return queue.try_pop() ? 0 : 3;
}
