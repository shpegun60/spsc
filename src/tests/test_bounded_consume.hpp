#ifndef SPSC_TEST_BOUNDED_CONSUME_HPP_
#define SPSC_TEST_BOUNDED_CONSUME_HPP_

#include <atomic>
#include <thread>

namespace spsc::test {

struct bounded_consume_value {
    int value{0};

    static inline std::atomic<bool> first_destroyed{false};
    static inline std::atomic<bool> later_published{false};

    bounded_consume_value() noexcept = default;
    explicit bounded_consume_value(const int v) noexcept : value(v) {}
    bounded_consume_value(const bounded_consume_value&) = delete;
    bounded_consume_value& operator=(const bounded_consume_value&) = delete;
    bounded_consume_value(bounded_consume_value&&) = delete;
    bounded_consume_value& operator=(bounded_consume_value&&) = delete;

    ~bounded_consume_value() noexcept {
        if (value == 1) {
            first_destroyed.store(true, std::memory_order_release);
        } else if (value == 2) {
            while (!later_published.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }

    static void reset() noexcept {
        first_destroyed.store(false, std::memory_order_relaxed);
        later_published.store(false, std::memory_order_relaxed);
    }
};

template<class Queue>
[[nodiscard]] bool bounded_consume_snapshot_contract() {
    bounded_consume_value::reset();

    Queue q;
    if (!q.is_valid() || !q.try_emplace(1) || !q.try_emplace(2)) {
        q.destroy();
        return false;
    }

    std::atomic<bool> producer_ok{false};
    std::thread producer([&] {
        while (!bounded_consume_value::first_destroyed.load(
            std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        producer_ok.store(q.try_emplace(3), std::memory_order_relaxed);
        bounded_consume_value::later_published.store(
            true, std::memory_order_release);
    });

    q.consume_all();
    producer.join();

    const auto* remaining = q.try_front();
    const bool ok = producer_ok.load(std::memory_order_relaxed) &&
                    q.size() == 1u && remaining != nullptr &&
                    remaining->value == 3;

    (void)q.try_pop();
    q.destroy();
    return ok;
}

} // namespace spsc::test

#endif // SPSC_TEST_BOUNDED_CONSUME_HPP_
