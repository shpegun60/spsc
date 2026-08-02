#include "src/spsc/fifo.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

namespace {

struct message {
    std::uint32_t sequence;
    std::uint32_t complement;
};

constexpr std::uint32_t kItems = 20000u;
constexpr auto kTimeout = std::chrono::seconds(15);

[[nodiscard]] constexpr message make_message(const std::uint32_t sequence) noexcept {
    return {sequence, ~sequence};
}

[[nodiscard]] constexpr bool matches(const message& value,
                                     const std::uint32_t sequence) noexcept {
    return value.sequence == sequence && value.complement == ~sequence;
}

void spin_pause() noexcept {
    std::this_thread::yield();
}

} // namespace

int main() {
    using queue_type = ::spsc::fifo<message, 1024u, ::spsc::policy::CFA<>>;

    queue_type queue;
    std::atomic<bool> abort{false};
    std::atomic<bool> start{false};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};
    std::atomic<int> workers_ready{0};
    std::atomic<int> failure{0};
    std::atomic<std::uint32_t> observations{0u};

    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    const auto record_failure = [&](const int code) noexcept {
        int expected = 0;
        failure.compare_exchange_strong(expected, code, std::memory_order_relaxed);
        abort.store(true, std::memory_order_relaxed);
    };
    const auto await_start = [&]() noexcept {
        workers_ready.fetch_add(1, std::memory_order_relaxed);
        while (!abort.load(std::memory_order_relaxed) &&
               !start.load(std::memory_order_relaxed)) {
            spin_pause();
        }
    };

    std::thread producer([&] {
        await_start();
        for (std::uint32_t sequence = 1u;
             sequence <= kItems && !abort.load(std::memory_order_relaxed);
             ++sequence) {
            while (!abort.load(std::memory_order_relaxed) &&
                   !queue.try_push(make_message(sequence))) {
                spin_pause();
            }
        }
        producer_done.store(true, std::memory_order_relaxed);
    });

    std::thread consumer([&] {
        await_start();
        for (std::uint32_t expected = 1u;
             expected <= kItems && !abort.load(std::memory_order_relaxed);
             ++expected) {
            const message* value = nullptr;
            while (!abort.load(std::memory_order_relaxed) &&
                   (value = queue.try_front()) == nullptr) {
                spin_pause();
            }
            if (abort.load(std::memory_order_relaxed)) {
                break;
            }
            if (!matches(*value, expected)) {
                record_failure(1);
                break;
            }
            if (!queue.try_pop()) {
                record_failure(2);
                break;
            }
        }
        consumer_done.store(true, std::memory_order_relaxed);
    });

    std::thread observer([&] {
        await_start();
        if (abort.load(std::memory_order_relaxed)) {
            return;
        }
        do {
            const auto capacity = queue.capacity();
            const auto size = queue.size();
            const auto free = queue.free();
            const auto writable = queue.write_size();
            const auto readable = queue.read_size();
            (void)queue.empty();
            (void)queue.full();
            (void)queue.can_write(1u);
            (void)queue.can_read(1u);

            if (size > capacity || free > capacity ||
                writable > capacity || readable > capacity) {
                record_failure(3);
                break;
            }
            observations.fetch_add(1u, std::memory_order_relaxed);
        } while (!abort.load(std::memory_order_relaxed) &&
                 !(producer_done.load(std::memory_order_relaxed) &&
                   consumer_done.load(std::memory_order_relaxed)));
    });

    while (workers_ready.load(std::memory_order_relaxed) != 3 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (workers_ready.load(std::memory_order_relaxed) != 3) {
        record_failure(4);
    }
    start.store(true, std::memory_order_relaxed);

    while (!abort.load(std::memory_order_relaxed) &&
           !(producer_done.load(std::memory_order_relaxed) &&
             consumer_done.load(std::memory_order_relaxed)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!(producer_done.load(std::memory_order_relaxed) &&
          consumer_done.load(std::memory_order_relaxed))) {
        record_failure(5);
    }

    abort.store(true, std::memory_order_relaxed);
    producer.join();
    consumer.join();
    observer.join();

    if (failure.load(std::memory_order_relaxed) != 0 ||
        observations.load(std::memory_order_relaxed) == 0u || !queue.empty()) {
        return 1;
    }
    return 0;
}
