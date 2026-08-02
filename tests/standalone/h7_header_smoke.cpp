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
    using dynamic_queue_type = ::spsc::fifo<std::uint32_t>;

    queue_type queue;
    if (!queue.try_push(42u)) {
        return 1;
    }
    const auto* value = queue.try_front();
    if (value == nullptr || *value != 42u) {
        return 2;
    }
    if (!queue.try_pop()) {
        return 3;
    }

    // Also instantiate dynamic storage: its storage member is a pointer, so
    // this catches a policy alignment that would weaken pointer alignment.
    dynamic_queue_type dynamic_queue{8u};
    if (!dynamic_queue.try_push(7u)) {
        return 4;
    }
    return dynamic_queue.try_pop() ? 0 : 5;
}
