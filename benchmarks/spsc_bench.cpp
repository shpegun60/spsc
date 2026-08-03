/*
 * Reproducible SPSC baseline harness.
 *
 * This executable intentionally reports measurements; it does not make a
 * performance claim. See benchmarks/README.md for workload equivalence and
 * interpretation rules.
 */

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#  include <immintrin.h>
#  define SPSC_BENCH_HAS_X86_PAUSE 1
#else
#  define SPSC_BENCH_HAS_X86_PAUSE 0
#endif

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#  include <time.h>
#endif

#include "basic_types.h"
#include "spsc/fifo.hpp"
#include "spsc/queue.hpp"
#include "rigtorp/SPSCQueue.h"

namespace spsc_bench {

using clock_type = std::chrono::steady_clock;

enum class workload {
    steady,
    boundary
};

inline constexpr unsigned benchmark_format_version = 2u;
inline constexpr std::size_t minimum_ranking_samples = 9u;
inline constexpr double maximum_paired_ratio_cv = 0.10;
inline constexpr double minimum_endpoint_cpu_occupancy = 0.80;
inline constexpr double maximum_direction_ratio_spread = 0.15;
inline constexpr double parity_ratio_lower = 0.95;
inline constexpr double parity_ratio_upper = 1.05;

struct affinity_pair {
    int producer_cpu{-1};
    int consumer_cpu{-1};
    bool topology_auto{false};
};

struct affinity_run {
    const char *name{"forward"};
    affinity_pair affinity{};
};

struct options {
    std::uint64_t items{20'000'000u};
    unsigned samples{9u};
    unsigned warmup{2u};
    std::size_t capacity{1024u};
    affinity_pair affinity{};
    bool bidirectional_affinity{true};
    std::string suite{"all"};
    std::string output{};
    std::string commit{"unknown"};
};

struct endpoint_metrics {
    std::uint64_t completed{0u};
    std::uint64_t full_events{0u};
    std::uint64_t empty_events{0u};
    std::uint64_t checksum{0u};
    std::uint64_t lifetime_sink{0u};
    std::uint64_t cpu_time_ns{0u};
    std::uint64_t cpu_cycles{0u};
    bool affinity_applied{false};
    bool sequence_ok{true};
};

struct sample_result {
    std::uint64_t items{0u};
    std::uint64_t duration_ns{0u};
    std::uint64_t producer_full_events{0u};
    std::uint64_t consumer_empty_events{0u};
    std::uint64_t checksum{0u};
    std::uint64_t lifetime_sink{0u};
    std::uint64_t producer_cpu_time_ns{0u};
    std::uint64_t consumer_cpu_time_ns{0u};
    std::uint64_t producer_cpu_cycles{0u};
    std::uint64_t consumer_cpu_cycles{0u};
    bool producer_affinity_applied{false};
    bool consumer_affinity_applied{false};
    bool verified{false};

    [[nodiscard]] double transfers_per_second() const noexcept {
        if (duration_ns == 0u) {
            return 0.0;
        }
        return static_cast<double>(items) * 1'000'000'000.0 /
               static_cast<double>(duration_ns);
    }

    [[nodiscard]] double minimum_cpu_occupancy() const noexcept {
        if (duration_ns == 0u) {
            return 0.0;
        }
        const double duration = static_cast<double>(duration_ns);
        const double producer = static_cast<double>(producer_cpu_time_ns) / duration;
        const double consumer = static_cast<double>(consumer_cpu_time_ns) / duration;
        return std::min(producer, consumer);
    }

    [[nodiscard]] double retries_per_transfer() const noexcept {
        if (items == 0u) {
            return 0.0;
        }
        return static_cast<double>(producer_full_events + consumer_empty_events) /
               static_cast<double>(items);
    }
};

[[noreturn]] static void usage(const char *message = nullptr) {
    if (message != nullptr) {
        std::cerr << "error: " << message << '\n';
    }
    std::cerr
        << "usage: spsc_bench [options]\n"
        << "  --items N                 item transfers per sample (default 20000000)\n"
        << "  --samples N               measured samples per case (default 9)\n"
        << "  --warmup N                discarded warm-up samples per case (default 2)\n"
        << "  --capacity N              one of 64, 256, 1024, 4096 (default 1024)\n"
        << "  --affinity auto|P,C|none  producer/consumer logical CPU selection\n"
        << "  --directions forward|both affinity directions (default both)\n"
        << "  --suite all|queue|fifo|policy\n"
        << "  --output PATH             JSONL output path (stdout when omitted)\n"
        << "  --commit SHA              source revision recorded in each result\n";
    std::exit(message == nullptr ? 0 : 2);
}

[[nodiscard]] static std::uint64_t parse_u64(const char *text,
                                               const char *option_name) {
    if (text == nullptr || *text == '\0' || *text == '-') {
        usage(option_name);
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        usage(option_name);
    }
    return static_cast<std::uint64_t>(value);
}

[[nodiscard]] static unsigned parse_unsigned(const char *text,
                                              const char *option_name) {
    const std::uint64_t value = parse_u64(text, option_name);
    if (value > static_cast<std::uint64_t>(std::numeric_limits<unsigned>::max())) {
        usage(option_name);
    }
    return static_cast<unsigned>(value);
}

[[nodiscard]] static int parse_int(const std::string &text,
                                    const char *option_name) {
    if (text.empty()) {
        usage(option_name);
    }
    errno = 0;
    char *end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        value < static_cast<long>(std::numeric_limits<int>::min()) ||
        value > static_cast<long>(std::numeric_limits<int>::max())) {
        usage(option_name);
    }
    return static_cast<int>(value);
}

[[nodiscard]] static affinity_pair parse_affinity(const std::string &value) {
    if (value == "none") {
        return {};
    }
    if (value == "auto") {
        affinity_pair result{};
        result.topology_auto = true;
        return result;
    }
    const std::size_t comma = value.find(',');
    if (comma == std::string::npos || value.find(',', comma + 1u) != std::string::npos) {
        usage("--affinity must be P,C or none");
    }
    affinity_pair result{};
    result.producer_cpu = parse_int(value.substr(0u, comma), "--affinity");
    result.consumer_cpu = parse_int(value.substr(comma + 1u), "--affinity");
    if (result.producer_cpu < 0 || result.consumer_cpu < 0) {
        usage("--affinity CPU indices must be non-negative");
    }
    return result;
}

[[nodiscard]] static bool supported_capacity(const std::size_t capacity) noexcept {
    return capacity == 64u || capacity == 256u || capacity == 1024u ||
           capacity == 4096u;
}

[[nodiscard]] static options parse_options(const int argc, char **argv) {
    options result{};
    for (int i = 1; i < argc; ++i) {
        const std::string option = argv[i];
        const auto next_value = [&]() -> const char * {
            if (i + 1 >= argc) {
                usage(option.c_str());
            }
            return argv[++i];
        };

        if (option == "--help" || option == "-h") {
            usage();
        } else if (option == "--items") {
            result.items = parse_u64(next_value(), "--items");
        } else if (option == "--samples") {
            result.samples = parse_unsigned(next_value(), "--samples");
        } else if (option == "--warmup") {
            result.warmup = parse_unsigned(next_value(), "--warmup");
        } else if (option == "--capacity") {
            result.capacity = static_cast<std::size_t>(parse_u64(next_value(), "--capacity"));
        } else if (option == "--affinity") {
            result.affinity = parse_affinity(next_value());
        } else if (option == "--directions") {
            const std::string directions = next_value();
            if (directions != "forward" && directions != "both") {
                usage("--directions must be forward or both");
            }
            result.bidirectional_affinity = directions == "both";
        } else if (option == "--suite") {
            result.suite = next_value();
        } else if (option == "--output") {
            result.output = next_value();
        } else if (option == "--commit") {
            result.commit = next_value();
        } else {
            usage(option.c_str());
        }
    }

    if (result.items == 0u || result.samples == 0u || !supported_capacity(result.capacity)) {
        usage("items/samples/capacity are invalid");
    }
    if (result.suite != "all" && result.suite != "queue" &&
        result.suite != "fifo" && result.suite != "policy") {
        usage("--suite must be all, queue, fifo, or policy");
    }
    if (result.items < result.capacity) {
        usage("--items must be at least one full capacity");
    }
    return result;
}

[[nodiscard]] static std::string json_quote(const std::string &value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (ch < 0x20u) {
                output << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                       << static_cast<unsigned>(ch) << std::dec << std::setfill(' ');
            } else {
                output << static_cast<char>(ch);
            }
            break;
        }
    }
    output << '"';
    return output.str();
}

[[nodiscard]] static std::string environment_or(const char *name,
                                                  const char *fallback = "unknown") {
#if defined(_MSC_VER)
    char *value = nullptr;
    std::size_t size = 0u;
    if (::_dupenv_s(&value, &size, name) != 0 || value == nullptr || *value == '\0') {
        std::free(value);
        return fallback;
    }
    const std::string result(value);
    std::free(value);
    return result;
#else
    const char *value = std::getenv(name);
    return (value != nullptr && *value != '\0') ? value : fallback;
#endif
}

[[nodiscard]] static std::string compiler_description() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return std::string("msvc ") + std::to_string(_MSC_VER);
#else
    return "unknown compiler";
#endif
}

[[nodiscard]] static std::string platform_description() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

[[nodiscard]] static bool pin_current_thread(const int cpu) noexcept {
    if (cpu < 0) {
        return false;
    }
#if defined(_WIN32)
    if (cpu >= static_cast<int>(sizeof(DWORD_PTR) * CHAR_BIT)) {
        return false;
    }
    const DWORD_PTR mask = static_cast<DWORD_PTR>(1u) << static_cast<unsigned>(cpu);
    return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0u;
#elif defined(__linux__)
    if (cpu >= CPU_SETSIZE) {
        return false;
    }
    cpu_set_t set{};
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

[[nodiscard]] static const char *affinity_selection_name(const affinity_pair affinity) noexcept {
    if (affinity.topology_auto) {
        return "topology_auto";
    }
    return (affinity.producer_cpu >= 0 && affinity.consumer_cpu >= 0) ? "explicit" : "none";
}

#if defined(_WIN32)
[[nodiscard]] static int first_cpu_in_mask(const KAFFINITY mask) noexcept {
    for (int cpu = 0; cpu < static_cast<int>(sizeof(KAFFINITY) * CHAR_BIT); ++cpu) {
        const KAFFINITY bit = static_cast<KAFFINITY>(1u) << static_cast<unsigned>(cpu);
        if ((mask & bit) != 0u) {
            return cpu;
        }
    }
    return -1;
}

[[nodiscard]] static unsigned cpu_count_in_mask(KAFFINITY mask) noexcept {
    unsigned count = 0u;
    while (mask != 0u) {
        count += static_cast<unsigned>(mask & static_cast<KAFFINITY>(1u));
        mask >>= 1u;
    }
    return count;
}

[[nodiscard]] static affinity_pair select_topology_affinity() {
    DWORD buffer_bytes = 0u;
    const BOOL initial_ok = ::GetLogicalProcessorInformationEx(
        RelationProcessorCore, nullptr, &buffer_bytes);
    if (initial_ok != FALSE || ::GetLastError() != ERROR_INSUFFICIENT_BUFFER ||
        buffer_bytes == 0u) {
        throw std::runtime_error("unable to query processor-core topology for auto affinity");
    }

    std::vector<unsigned char> buffer(buffer_bytes);
    auto *first = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data());
    if (::GetLogicalProcessorInformationEx(RelationProcessorCore, first, &buffer_bytes) == FALSE) {
        throw std::runtime_error("processor-core topology query failed for auto affinity");
    }

    struct core_candidate {
        int cpu{-1};
        unsigned logical_threads{0u};
    };
    std::vector<core_candidate> candidates;

    DWORD offset = 0u;
    while (offset < buffer_bytes) {
        const auto *entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(
            buffer.data() + offset);
        if (entry->Size == 0u || entry->Size > (buffer_bytes - offset)) {
            throw std::runtime_error("processor-core topology returned an invalid record");
        }

        if (entry->Relationship == RelationProcessorCore) {
            const PROCESSOR_RELATIONSHIP &core = entry->Processor;
            for (WORD group_index = 0u; group_index < core.GroupCount; ++group_index) {
                const GROUP_AFFINITY group = core.GroupMask[group_index];
                // SetThreadAffinityMask is group-zero only. A caller that needs
                // another processor group can still pass an explicit pair.
                if (group.Group != 0u) {
                    continue;
                }
                const int cpu = first_cpu_in_mask(group.Mask);
                if (cpu >= 0) {
                    candidates.push_back({cpu, cpu_count_in_mask(group.Mask)});
                }
            }
        }
        offset += entry->Size;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const core_candidate &left, const core_candidate &right) {
                  return left.cpu < right.cpu;
              });
    if (candidates.size() < 2u) {
        throw std::runtime_error("auto affinity needs two distinct physical cores in processor group zero");
    }

    // On hybrid Intel systems a core record with two logical threads identifies
    // a P-core. Select two distinct such cores first, never sibling threads.
    std::vector<int> selected;
    for (const core_candidate &candidate : candidates) {
        if (candidate.logical_threads > 1u && selected.size() < 2u) {
            selected.push_back(candidate.cpu);
        }
    }
    for (const core_candidate &candidate : candidates) {
        if (selected.size() >= 2u) {
            break;
        }
        if (std::find(selected.begin(), selected.end(), candidate.cpu) == selected.end()) {
            selected.push_back(candidate.cpu);
        }
    }
    if (selected.size() != 2u) {
        throw std::runtime_error("auto affinity could not select two distinct physical cores");
    }
    return {selected[0], selected[1], true};
}
#else
[[nodiscard]] static affinity_pair select_topology_affinity() {
    if (std::thread::hardware_concurrency() < 2u) {
        throw std::runtime_error("auto affinity needs at least two logical CPUs");
    }
    // Linux/other callers retain an explicit override; topology probing is
    // currently implemented for the canonical Windows runner.
    return {0, 1, true};
}
#endif

[[nodiscard]] static affinity_pair resolve_affinity(const affinity_pair requested) {
    return requested.topology_auto ? select_topology_affinity() : requested;
}

[[nodiscard]] static const char *affinity_direction_protocol_name(
    const options &input) noexcept {
    return input.bidirectional_affinity ? "both" : "forward";
}

[[nodiscard]] static std::vector<affinity_run> make_affinity_runs(
    const options &input) {
    std::vector<affinity_run> runs;
    runs.push_back({"forward", input.affinity});
    if (input.bidirectional_affinity && input.affinity.producer_cpu >= 0 &&
        input.affinity.consumer_cpu >= 0 &&
        input.affinity.producer_cpu != input.affinity.consumer_cpu) {
        affinity_pair reversed = input.affinity;
        std::swap(reversed.producer_cpu, reversed.consumer_cpu);
        runs.push_back({"reverse", reversed});
    }
    return runs;
}

[[nodiscard]] static std::uint64_t current_thread_cpu_time_ns() noexcept {
#if defined(_WIN32)
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (::GetThreadTimes(::GetCurrentThread(), &created, &exited, &kernel, &user) == FALSE) {
        return 0u;
    }
    ULARGE_INTEGER kernel_ticks{};
    kernel_ticks.LowPart = kernel.dwLowDateTime;
    kernel_ticks.HighPart = kernel.dwHighDateTime;
    ULARGE_INTEGER user_ticks{};
    user_ticks.LowPart = user.dwLowDateTime;
    user_ticks.HighPart = user.dwHighDateTime;
    // FILETIME ticks are 100 ns.
    return (kernel_ticks.QuadPart + user_ticks.QuadPart) * 100u;
#elif defined(__linux__)
    timespec value{};
    if (::clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) {
        return 0u;
    }
    return static_cast<std::uint64_t>(value.tv_sec) * 1'000'000'000u +
           static_cast<std::uint64_t>(value.tv_nsec);
#else
    return 0u;
#endif
}

[[nodiscard]] static std::uint64_t current_thread_cpu_cycles() noexcept {
#if defined(_WIN32)
    ULONG64 cycles = 0u;
    return ::QueryThreadCycleTime(::GetCurrentThread(), &cycles) != FALSE
               ? static_cast<std::uint64_t>(cycles)
               : 0u;
#else
    // A portable per-thread cycle counter is not available in C++17. CPU time
    // remains present on Linux; the JSON metadata identifies cycles as absent.
    return 0u;
#endif
}

[[nodiscard]] static const char *thread_cycle_counter_name() noexcept {
#if defined(_WIN32)
    return "QueryThreadCycleTime";
#else
    return "unavailable";
#endif
}

static void cpu_relax() noexcept {
#if SPSC_BENCH_HAS_X86_PAUSE
    _mm_pause();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

static void backoff(const std::uint64_t iteration) noexcept {
    (void)iteration;
    // A retry must not periodically become a scheduler yield. Different
    // queues naturally observe different counts of transient full/empty
    // probes; yielding after a fixed count turns that implementation detail
    // into a large and non-repeatable Windows scheduling effect.
    cpu_relax();
}

class persistent_parallel_executor {
public:
    using task_type = std::function<void(endpoint_metrics &)>;

    explicit persistent_parallel_executor(const affinity_pair affinity)
        : affinity_(affinity) {
        try {
            producer_thread_ = std::thread([this] { worker(true); });
            consumer_thread_ = std::thread([this] { worker(false); });
        } catch (...) {
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                stop_ = true;
            }
            state_changed_.notify_all();
            if (producer_thread_.joinable()) {
                producer_thread_.join();
            }
            if (consumer_thread_.joinable()) {
                consumer_thread_.join();
            }
            throw;
        }
        std::unique_lock<std::mutex> lock(mutex_);
        state_changed_.wait(lock, [this] { return initialized_workers_ == 2u; });
    }

    persistent_parallel_executor(const persistent_parallel_executor &) = delete;
    persistent_parallel_executor &operator=(const persistent_parallel_executor &) = delete;

    ~persistent_parallel_executor() {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        state_changed_.notify_all();
        if (producer_thread_.joinable()) {
            producer_thread_.join();
        }
        if (consumer_thread_.joinable()) {
            consumer_thread_.join();
        }
    }

    [[nodiscard]] sample_result run(task_type producer_task,
                                    task_type consumer_task) {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            producer_metrics_ = {};
            consumer_metrics_ = {};
            producer_metrics_.affinity_applied = producer_affinity_applied_;
            consumer_metrics_.affinity_applied = consumer_affinity_applied_;
            producer_error_ = nullptr;
            consumer_error_ = nullptr;
            producer_task_ = std::move(producer_task);
            consumer_task_ = std::move(consumer_task);
            ready_workers_ = 0u;
            completed_workers_ = 0u;
            go_.store(false, std::memory_order_relaxed);
            ++generation_;
        }
        state_changed_.notify_all();

        {
            std::unique_lock<std::mutex> lock(mutex_);
            state_changed_.wait(lock, [this] { return ready_workers_ == 2u; });
        }

        const auto started = clock_type::now();
        go_.store(true, std::memory_order_release);

        {
            std::unique_lock<std::mutex> lock(mutex_);
            state_changed_.wait(lock, [this] { return completed_workers_ == 2u; });
        }
        const auto ended = std::max(producer_ended_, consumer_ended_);

        if (producer_error_ != nullptr) {
            std::rethrow_exception(producer_error_);
        }
        if (consumer_error_ != nullptr) {
            std::rethrow_exception(consumer_error_);
        }

        sample_result result{};
        result.duration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(ended - started).count());
        result.producer_full_events = producer_metrics_.full_events;
        result.consumer_empty_events = consumer_metrics_.empty_events;
        result.checksum = consumer_metrics_.checksum;
        result.lifetime_sink = consumer_metrics_.lifetime_sink;
        result.producer_cpu_time_ns = producer_metrics_.cpu_time_ns;
        result.consumer_cpu_time_ns = consumer_metrics_.cpu_time_ns;
        result.producer_cpu_cycles = producer_metrics_.cpu_cycles;
        result.consumer_cpu_cycles = consumer_metrics_.cpu_cycles;
        result.producer_affinity_applied = producer_metrics_.affinity_applied;
        result.consumer_affinity_applied = consumer_metrics_.affinity_applied;
        result.verified = producer_metrics_.sequence_ok && consumer_metrics_.sequence_ok &&
                          producer_metrics_.completed == consumer_metrics_.completed;
        return result;
    }

private:
    void worker(const bool producer_side) noexcept {
        endpoint_metrics &metrics = producer_side ? producer_metrics_ : consumer_metrics_;
        const int cpu = producer_side ? affinity_.producer_cpu : affinity_.consumer_cpu;
        const bool affinity_applied = pin_current_thread(cpu);
        unsigned observed_generation = 0u;

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (producer_side) {
                producer_affinity_applied_ = affinity_applied;
            } else {
                consumer_affinity_applied_ = affinity_applied;
            }
            ++initialized_workers_;
        }
        state_changed_.notify_all();

        for (;;) {
            task_type *task = nullptr;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                state_changed_.wait(lock, [this, observed_generation] {
                    return stop_ || generation_ != observed_generation;
                });
                if (stop_) {
                    return;
                }
                observed_generation = generation_;
                task = producer_side ? &producer_task_ : &consumer_task_;
                ++ready_workers_;
            }
            state_changed_.notify_all();

            while (!go_.load(std::memory_order_acquire)) {
                cpu_relax();
            }

            const std::uint64_t started_cpu = current_thread_cpu_time_ns();
            const std::uint64_t started_cycles = current_thread_cpu_cycles();
            try {
                (*task)(metrics);
            } catch (...) {
                if (producer_side) {
                    producer_error_ = std::current_exception();
                } else {
                    consumer_error_ = std::current_exception();
                }
            }
            const std::uint64_t ended_cycles = current_thread_cpu_cycles();
            const std::uint64_t ended_cpu = current_thread_cpu_time_ns();
            const auto ended_wall = clock_type::now();
            metrics.cpu_time_ns = ended_cpu >= started_cpu ? ended_cpu - started_cpu : 0u;
            metrics.cpu_cycles = ended_cycles >= started_cycles ? ended_cycles - started_cycles : 0u;

            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (producer_side) {
                    producer_ended_ = ended_wall;
                } else {
                    consumer_ended_ = ended_wall;
                }
                ++completed_workers_;
            }
            state_changed_.notify_all();
        }
    }

    affinity_pair affinity_{};
    std::mutex mutex_{};
    std::condition_variable state_changed_{};
    std::atomic<bool> go_{false};
    bool stop_{false};
    unsigned initialized_workers_{0u};
    unsigned generation_{0u};
    unsigned ready_workers_{0u};
    unsigned completed_workers_{0u};
    bool producer_affinity_applied_{false};
    bool consumer_affinity_applied_{false};
    endpoint_metrics producer_metrics_{};
    endpoint_metrics consumer_metrics_{};
    task_type producer_task_{};
    task_type consumer_task_{};
    std::exception_ptr producer_error_{};
    std::exception_ptr consumer_error_{};
    clock_type::time_point producer_ended_{};
    clock_type::time_point consumer_ended_{};
    std::thread producer_thread_;
    std::thread consumer_thread_;
};

thread_local volatile std::uint64_t lifetime_sink = 0u;

struct lifetime_payload {
    std::uint64_t value{0u};

    explicit lifetime_payload(const std::uint64_t input) noexcept : value(input) {}
    lifetime_payload(const lifetime_payload &) = default;
    lifetime_payload(lifetime_payload &&) noexcept = default;
    lifetime_payload &operator=(const lifetime_payload &) = default;
    lifetime_payload &operator=(lifetime_payload &&) noexcept = default;

    ~lifetime_payload() noexcept {
        lifetime_sink = (lifetime_sink << 1u) ^ (value + 0x9e3779b97f4a7c15ull);
    }
};

template <reg Capacity, class Policy>
struct fifo_adapter {
    using item_type = std::uint64_t;
    static constexpr bool validates_lifetime = false;
    spsc::fifo<item_type, Capacity, Policy> queue{};

    explicit fifo_adapter(const std::size_t) noexcept {}

    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(queue.capacity());
    }
    [[nodiscard]] bool try_push(const std::uint64_t value) noexcept {
        return queue.try_push(value);
    }
    [[nodiscard]] item_type *try_front() noexcept { return queue.try_front(); }
    void pop() noexcept { queue.pop(); }
    [[nodiscard]] static std::uint64_t value_of(const item_type value) noexcept { return value; }
    static void begin_consumer() noexcept {}
    [[nodiscard]] static std::uint64_t end_consumer() noexcept { return 0u; }
};

template <reg Capacity, class Policy>
struct spsc_queue_adapter {
    using item_type = lifetime_payload;
    static constexpr bool validates_lifetime = true;
    spsc::queue<item_type, Capacity, Policy> queue{};

    explicit spsc_queue_adapter(const std::size_t) {}

    [[nodiscard]] std::size_t capacity() const noexcept {
        return static_cast<std::size_t>(queue.capacity());
    }
    [[nodiscard]] bool try_push(const std::uint64_t value) {
        return queue.try_emplace(value) != nullptr;
    }
    [[nodiscard]] item_type *try_front() noexcept { return queue.try_front(); }
    void pop() noexcept { queue.pop(); }
    [[nodiscard]] static std::uint64_t value_of(const item_type &value) noexcept {
        return value.value;
    }
    static void begin_consumer() noexcept { lifetime_sink = 0u; }
    [[nodiscard]] static std::uint64_t end_consumer() noexcept { return lifetime_sink; }
};

template <reg Capacity>
struct rigtorp_queue_adapter {
    using item_type = lifetime_payload;
    static constexpr bool validates_lifetime = true;
    rigtorp::SPSCQueue<item_type> queue;

    explicit rigtorp_queue_adapter(const std::size_t requested_capacity)
        : queue(requested_capacity) {}

    [[nodiscard]] std::size_t capacity() const noexcept { return queue.capacity(); }
    [[nodiscard]] bool try_push(const std::uint64_t value) {
        return queue.try_emplace(value);
    }
    [[nodiscard]] item_type *try_front() noexcept { return queue.front(); }
    void pop() noexcept { queue.pop(); }
    [[nodiscard]] static std::uint64_t value_of(const item_type &value) noexcept {
        return value.value;
    }
    static void begin_consumer() noexcept { lifetime_sink = 0u; }
    [[nodiscard]] static std::uint64_t end_consumer() noexcept { return lifetime_sink; }
};

template <class Adapter>
static void pretouch_queue(Adapter &queue) {
    const std::size_t capacity = queue.capacity();
    Adapter::begin_consumer();
    for (std::size_t index = 0u; index < capacity; ++index) {
        if (!queue.try_push(static_cast<std::uint64_t>(index + 1u))) {
            throw std::runtime_error("benchmark pre-touch could not fill the queue");
        }
    }
    for (std::size_t index = 0u; index < capacity; ++index) {
        if (queue.try_front() == nullptr) {
            throw std::runtime_error("benchmark pre-touch could not drain the queue");
        }
        queue.pop();
    }
    (void)Adapter::end_consumer();
}

[[nodiscard]] static std::uint64_t expected_checksum(const std::uint64_t count) noexcept {
    return (count & 1u) == 0u ? (count / 2u) * (count + 1u)
                              : count * ((count + 1u) / 2u);
}

[[nodiscard]] static std::uint64_t expected_lifetime_sink(const std::uint64_t count) noexcept {
    // The recurrence is linear over uint64_t. Contributions older than 64
    // destructions have shifted out, so only the final 64 values are needed.
    if (count == 0u) {
        return 0u;
    }
    const std::uint64_t first = count > 64u ? count - 63u : 1u;
    std::uint64_t result = 0u;
    for (std::uint64_t value = first;; ++value) {
        result = (result << 1u) ^ (value + 0x9e3779b97f4a7c15ull);
        if (value == count) {
            break;
        }
    }
    return result;
}

template <class Adapter>
[[nodiscard]] static sample_result measure_steady(
    const options &input,
    persistent_parallel_executor &executor) {
    Adapter queue(input.capacity);
    if (queue.capacity() != input.capacity) {
        throw std::runtime_error("benchmark queue did not expose the requested usable capacity");
    }
    pretouch_queue(queue);

    const std::uint64_t item_count = input.items;
    sample_result result = executor.run(
        [&](endpoint_metrics &metrics) {
            for (std::uint64_t sequence = 1u; sequence <= item_count; ++sequence) {
                while (!queue.try_push(sequence)) {
                    ++metrics.full_events;
                    backoff(metrics.full_events);
                }
                ++metrics.completed;
            }
        },
        [&](endpoint_metrics &metrics) {
            Adapter::begin_consumer();
            std::uint64_t expected = 1u;
            while (metrics.completed < item_count) {
                auto *item = queue.try_front();
                if (item == nullptr) {
                    ++metrics.empty_events;
                    backoff(metrics.empty_events);
                    continue;
                }
                const std::uint64_t value = Adapter::value_of(*item);
                metrics.sequence_ok = metrics.sequence_ok && value == expected;
                metrics.checksum += value;
                queue.pop();
                ++metrics.completed;
                ++expected;
            }
            metrics.lifetime_sink = Adapter::end_consumer();
        });

    result.items = item_count;
    result.verified = result.verified && result.checksum == expected_checksum(item_count);
    if constexpr (Adapter::validates_lifetime) {
        result.verified = result.verified &&
                          result.lifetime_sink == expected_lifetime_sink(item_count);
    }
    return result;
}

template <class Adapter>
[[nodiscard]] static sample_result measure_boundary(
    const options &input,
    persistent_parallel_executor &executor) {
    Adapter queue(input.capacity);
    if (queue.capacity() != input.capacity) {
        throw std::runtime_error("benchmark queue did not expose the requested usable capacity");
    }
    pretouch_queue(queue);

    const std::uint64_t item_count =
        (input.items / static_cast<std::uint64_t>(input.capacity)) *
        static_cast<std::uint64_t>(input.capacity);
    if (item_count == 0u) {
        throw std::runtime_error("boundary workload needs at least one complete round");
    }
    const std::uint64_t rounds = item_count / static_cast<std::uint64_t>(input.capacity);
    std::atomic<std::uint64_t> producer_round{0u};
    std::atomic<std::uint64_t> consumer_round{0u};
    std::atomic<std::size_t> ready_items{0u};

    sample_result result = executor.run(
        [&](endpoint_metrics &metrics) {
            std::uint64_t sequence = 1u;
            for (std::uint64_t round = 0u; round < rounds; ++round) {
                std::size_t published = 0u;
                for (std::size_t i = 0u; i < input.capacity; ++i) {
                    if (!queue.try_push(sequence)) {
                        metrics.sequence_ok = false;
                        break;
                    }
                    ++sequence;
                    ++published;
                    ++metrics.completed;
                }

                // This is intentionally a failed full-boundary probe. If it
                // unexpectedly succeeds, make the sample invalid but still
                // hand the additional element to the consumer so it cannot hang.
                if (!queue.try_push(0xffffffffffffffffull)) {
                    ++metrics.full_events;
                } else {
                    metrics.sequence_ok = false;
                    ++published;
                    ++metrics.completed;
                }

                ready_items.store(published, std::memory_order_release);
                producer_round.store(round + 1u, std::memory_order_release);
                while (consumer_round.load(std::memory_order_acquire) != round + 1u) {
                    backoff(round);
                }
            }
        },
        [&](endpoint_metrics &metrics) {
            Adapter::begin_consumer();
            std::uint64_t expected = 1u;
            for (std::uint64_t round = 0u; round < rounds; ++round) {
                while (producer_round.load(std::memory_order_acquire) != round + 1u) {
                    backoff(round);
                }
                const std::size_t count = ready_items.load(std::memory_order_acquire);
                if (count != input.capacity) {
                    metrics.sequence_ok = false;
                }
                for (std::size_t i = 0u; i < count; ++i) {
                    auto *item = queue.try_front();
                    while (item == nullptr) {
                        ++metrics.empty_events;
                        backoff(metrics.empty_events);
                        item = queue.try_front();
                    }
                    const std::uint64_t value = Adapter::value_of(*item);
                    metrics.sequence_ok = metrics.sequence_ok && value == expected;
                    metrics.checksum += value;
                    queue.pop();
                    ++metrics.completed;
                    ++expected;
                }

                if (queue.try_front() == nullptr) {
                    ++metrics.empty_events;
                } else {
                    metrics.sequence_ok = false;
                }
                consumer_round.store(round + 1u, std::memory_order_release);
            }
            metrics.lifetime_sink = Adapter::end_consumer();
        });

    result.items = item_count;
    result.verified = result.verified && result.checksum == expected_checksum(item_count);
    if constexpr (Adapter::validates_lifetime) {
        result.verified = result.verified &&
                          result.lifetime_sink == expected_lifetime_sink(item_count);
    }
    return result;
}

[[nodiscard]] static const char *workload_name(const workload value) noexcept {
    return value == workload::steady ? "steady" : "boundary";
}

struct rate_statistics {
    double minimum{0.0};
    double median{0.0};
    double mean{0.0};
    double maximum{0.0};
    double sample_standard_deviation{0.0};
};

[[nodiscard]] static double coefficient_of_variation(
    const rate_statistics &statistics) noexcept {
    return statistics.mean != 0.0
               ? statistics.sample_standard_deviation / std::abs(statistics.mean)
               : std::numeric_limits<double>::infinity();
}

struct paired_direction_result {
    std::string direction{};
    std::string gate_reason{};
    rate_statistics ratio_statistics{};
    double ratio_cv{0.0};
    double minimum_cpu_occupancy{0.0};
    double spsc_median_retries_per_transfer{0.0};
    double rigtorp_median_retries_per_transfer{0.0};
    std::size_t samples{0u};
    bool verified{false};
    bool affinity_verified{false};
    bool stable{false};
};

[[nodiscard]] static rate_statistics summarize_rates(const std::vector<double> &rates) {
    if (rates.empty()) {
        throw std::runtime_error("cannot summarize an empty benchmark sample set");
    }

    std::vector<double> sorted = rates;
    std::sort(sorted.begin(), sorted.end());
    const double mean = std::accumulate(rates.begin(), rates.end(), 0.0) /
                        static_cast<double>(rates.size());
    const std::size_t middle = sorted.size() / 2u;
    const double median = (sorted.size() & 1u) != 0u
                              ? sorted[middle]
                              : (sorted[middle - 1u] + sorted[middle]) * 0.5;

    double squared_error_sum = 0.0;
    for (const double rate : rates) {
        const double error = rate - mean;
        squared_error_sum += error * error;
    }
    const double sample_standard_deviation = rates.size() > 1u
                                                ? std::sqrt(squared_error_sum /
                                                            static_cast<double>(rates.size() - 1u))
                                                : 0.0;
    return {sorted.front(), median, mean, sorted.back(), sample_standard_deviation};
}

class jsonl_writer {
public:
    explicit jsonl_writer(const std::string &path) {
        if (path.empty()) {
            output_ = &std::cout;
        } else {
            file_.open(path, std::ios::out | std::ios::trunc);
            if (!file_) {
                throw std::runtime_error("cannot open benchmark output: " + path);
            }
            output_ = &file_;
        }
    }

    template <reg Capacity>
    void write_metadata(const options &input) {
        using queue_a = spsc::queue<lifetime_payload, Capacity, spsc::policy::A<>>;
        using queue_fa = spsc::queue<lifetime_payload, Capacity, spsc::policy::FA<>>;
        using queue_aa = spsc::queue<lifetime_payload, Capacity, spsc::policy::AA<>>;
        using queue_ca = spsc::queue<lifetime_payload, Capacity, spsc::policy::CA<>>;
        using queue_cfa = spsc::queue<lifetime_payload, Capacity, spsc::policy::CFA<>>;
        using queue_caa = spsc::queue<lifetime_payload, Capacity, spsc::policy::CAA<>>;
        using fifo_a = spsc::fifo<std::uint64_t, Capacity, spsc::policy::A<>>;
        using fifo_fa = spsc::fifo<std::uint64_t, Capacity, spsc::policy::FA<>>;
        using fifo_aa = spsc::fifo<std::uint64_t, Capacity, spsc::policy::AA<>>;
        using fifo_ca = spsc::fifo<std::uint64_t, Capacity, spsc::policy::CA<>>;
        using fifo_cfa = spsc::fifo<std::uint64_t, Capacity, spsc::policy::CFA<>>;
        using fifo_caa = spsc::fifo<std::uint64_t, Capacity, spsc::policy::CAA<>>;

        auto &out = *output_;
        out << "{\"kind\":\"metadata\",\"format_version\":" << benchmark_format_version
            << ",\"commit\":" << json_quote(input.commit)
            << ",\"platform\":" << json_quote(platform_description())
            << ",\"compiler\":" << json_quote(compiler_description())
            << ",\"compiler_path\":" << json_quote(environment_or("SPSC_BENCH_COMPILER_PATH"))
            << ",\"build_flags\":" << json_quote(environment_or("SPSC_BENCH_BUILD_FLAGS"))
            << ",\"hardware_threads\":" << std::thread::hardware_concurrency()
            << ",\"capacity\":" << Capacity
            << ",\"items_per_sample\":" << input.items
            << ",\"samples_per_case\":" << input.samples
            << ",\"warmup_per_case\":" << input.warmup
            << ",\"affinity_selection\":" << json_quote(affinity_selection_name(input.affinity))
            << ",\"affinity_resolved\":[" << input.affinity.producer_cpu << ','
            << input.affinity.consumer_cpu << ']'
            << ",\"affinity_directions\":"
            << json_quote(affinity_direction_protocol_name(input))
            << ",\"worker_lifecycle\":\"persistent_per_workload_case\""
            << ",\"retry_backoff\":\"cpu_relax\""
            << ",\"thread_cycle_counter\":" << json_quote(thread_cycle_counter_name())
            << ",\"ranking_gate\":{"
            << "\"minimum_samples\":" << minimum_ranking_samples
            << ",\"maximum_paired_ratio_cv\":" << maximum_paired_ratio_cv
            << ",\"minimum_endpoint_cpu_occupancy\":"
            << minimum_endpoint_cpu_occupancy
            << ",\"maximum_direction_ratio_spread\":"
            << maximum_direction_ratio_spread
            << ",\"parity_ratio\":[" << parity_ratio_lower << ','
            << parity_ratio_upper << "]}"
            << ",\"boundary_batch_size\":" << Capacity
            << ",\"spsc_cacheline_bytes\":" << SPSC_CACHELINE_BYTES
            << ",\"shadow_indices_enabled\":" << SPSC_ENABLE_SHADOW_INDICES
            << ",\"shadow_allow_32bit\":" << SPSC_SHADOW_ALLOW_32BIT
            << ",\"payload_layout\":{"
            << "\"queue_item\":[" << sizeof(lifetime_payload) << ','
            << alignof(lifetime_payload) << ']'
            << ",\"fifo_item\":[" << sizeof(std::uint64_t) << ','
            << alignof(std::uint64_t) << "]}"
            << ",\"type_layout\":{"
            << "\"queue_a\":[" << sizeof(queue_a) << ',' << alignof(queue_a) << ']'
            << ",\"queue_fa\":[" << sizeof(queue_fa) << ',' << alignof(queue_fa) << ']'
            << ",\"queue_aa\":[" << sizeof(queue_aa) << ',' << alignof(queue_aa) << ']'
            << ",\"queue_ca\":[" << sizeof(queue_ca) << ',' << alignof(queue_ca) << ']'
            << ",\"queue_cfa\":[" << sizeof(queue_cfa) << ',' << alignof(queue_cfa) << ']'
            << ",\"queue_caa\":[" << sizeof(queue_caa) << ',' << alignof(queue_caa) << ']'
            << ",\"fifo_a\":[" << sizeof(fifo_a) << ',' << alignof(fifo_a) << ']'
            << ",\"fifo_fa\":[" << sizeof(fifo_fa) << ',' << alignof(fifo_fa) << ']'
            << ",\"fifo_aa\":[" << sizeof(fifo_aa) << ',' << alignof(fifo_aa) << ']'
            << ",\"fifo_ca\":[" << sizeof(fifo_ca) << ',' << alignof(fifo_ca) << ']'
            << ",\"fifo_cfa\":[" << sizeof(fifo_cfa) << ',' << alignof(fifo_cfa) << ']'
            << ",\"fifo_caa\":[" << sizeof(fifo_caa) << ',' << alignof(fifo_caa) << ']'
            << ",\"rigtorp_queue\":[" << sizeof(rigtorp::SPSCQueue<lifetime_payload>)
            << ',' << alignof(rigtorp::SPSCQueue<lifetime_payload>) << "]}}\n";
        out.flush();
    }

    void write_sample(const options &input,
                      const char *implementation,
                      const char *policy,
                      const workload mode,
                      const affinity_run &affinity,
                      const unsigned sample_index,
                      const sample_result &sample,
                      const int pair_index = -1,
                      const int order_in_pair = -1) {
        auto &out = *output_;
        out << std::setprecision(17)
            << "{\"kind\":\"sample\",\"format_version\":" << benchmark_format_version
            << ",\"commit\":" << json_quote(input.commit)
            << ",\"implementation\":" << json_quote(implementation)
            << ",\"policy\":" << json_quote(policy)
            << ",\"workload\":" << json_quote(workload_name(mode))
            << ",\"direction\":" << json_quote(affinity.name)
            << ",\"sample\":" << sample_index
            << ",\"items\":" << sample.items
            << ",\"duration_ns\":" << sample.duration_ns
            << ",\"transfers_per_second\":" << sample.transfers_per_second()
            << ",\"producer_full_events\":" << sample.producer_full_events
            << ",\"consumer_empty_events\":" << sample.consumer_empty_events
            << ",\"checksum\":" << sample.checksum
            << ",\"lifetime_sink\":" << sample.lifetime_sink
            << ",\"producer_cpu_time_ns\":" << sample.producer_cpu_time_ns
            << ",\"consumer_cpu_time_ns\":" << sample.consumer_cpu_time_ns
            << ",\"producer_cpu_cycles\":" << sample.producer_cpu_cycles
            << ",\"consumer_cpu_cycles\":" << sample.consumer_cpu_cycles
            << ",\"minimum_cpu_occupancy\":" << sample.minimum_cpu_occupancy()
            << ",\"retries_per_transfer\":" << sample.retries_per_transfer()
            << ",\"verified\":" << (sample.verified ? "true" : "false")
            << (pair_index >= 0 ? ",\"pair_index\":" + std::to_string(pair_index) : "")
            << (order_in_pair >= 0 ? ",\"order_in_pair\":" + std::to_string(order_in_pair) : "")
            << ",\"affinity\":{\"producer_cpu\":" << affinity.affinity.producer_cpu
            << ",\"consumer_cpu\":" << affinity.affinity.consumer_cpu
            << ",\"producer_applied\":" << (sample.producer_affinity_applied ? "true" : "false")
            << ",\"consumer_applied\":" << (sample.consumer_affinity_applied ? "true" : "false")
            << "}}\n";
        out.flush();
    }

    void write_summary(const options &input,
                       const char *implementation,
                       const char *policy,
                       const workload mode,
                       const affinity_run &affinity,
                       const std::vector<sample_result> &samples) {
        std::vector<double> rates;
        std::vector<double> retries;
        rates.reserve(samples.size());
        retries.reserve(samples.size());
        bool verified = true;
        double minimum_cpu_occupancy = std::numeric_limits<double>::infinity();
        std::uint64_t full_events = 0u;
        std::uint64_t empty_events = 0u;
        for (const sample_result &sample : samples) {
            rates.push_back(sample.transfers_per_second());
            retries.push_back(sample.retries_per_transfer());
            minimum_cpu_occupancy =
                std::min(minimum_cpu_occupancy, sample.minimum_cpu_occupancy());
            verified = verified && sample.verified;
            full_events += sample.producer_full_events;
            empty_events += sample.consumer_empty_events;
        }
        const rate_statistics statistics = summarize_rates(rates);
        const rate_statistics retry_statistics = summarize_rates(retries);

        auto &out = *output_;
        out << std::setprecision(17)
            << "{\"kind\":\"summary\",\"format_version\":" << benchmark_format_version
            << ",\"commit\":" << json_quote(input.commit)
            << ",\"implementation\":" << json_quote(implementation)
            << ",\"policy\":" << json_quote(policy)
            << ",\"workload\":" << json_quote(workload_name(mode))
            << ",\"direction\":" << json_quote(affinity.name)
            << ",\"samples\":" << samples.size()
            << ",\"min_transfers_per_second\":" << statistics.minimum
            << ",\"median_transfers_per_second\":" << statistics.median
            << ",\"mean_transfers_per_second\":" << statistics.mean
            << ",\"max_transfers_per_second\":" << statistics.maximum
            << ",\"sample_standard_deviation\":" << statistics.sample_standard_deviation
            << ",\"rate_coefficient_of_variation\":"
            << coefficient_of_variation(statistics)
            << ",\"median_retries_per_transfer\":" << retry_statistics.median
            << ",\"minimum_cpu_occupancy\":" << minimum_cpu_occupancy
            << ",\"producer_full_events_total\":" << full_events
            << ",\"consumer_empty_events_total\":" << empty_events
            << ",\"verified\":" << (verified ? "true" : "false")
            << "}\n";
        out.flush();
    }

    [[nodiscard]] paired_direction_result write_paired_summary(
        const options &input,
        const workload mode,
        const affinity_run &affinity,
        const std::vector<sample_result> &spsc_samples,
        const std::vector<sample_result> &rigtorp_samples) {
        if (spsc_samples.size() != rigtorp_samples.size() || spsc_samples.empty()) {
            throw std::runtime_error("paired benchmark samples are inconsistent");
        }

        std::vector<double> ratios;
        std::vector<double> spsc_retries;
        std::vector<double> rigtorp_retries;
        ratios.reserve(spsc_samples.size());
        spsc_retries.reserve(spsc_samples.size());
        rigtorp_retries.reserve(rigtorp_samples.size());
        bool verified = true;
        bool affinity_verified = affinity.affinity.producer_cpu >= 0 &&
                                 affinity.affinity.consumer_cpu >= 0 &&
                                 affinity.affinity.producer_cpu !=
                                     affinity.affinity.consumer_cpu;
        double minimum_cpu_occupancy = std::numeric_limits<double>::infinity();
        for (std::size_t index = 0u; index < spsc_samples.size(); ++index) {
            const double spsc_rate = spsc_samples[index].transfers_per_second();
            if (spsc_rate == 0.0) {
                throw std::runtime_error("paired benchmark produced a zero SPSC rate");
            }
            ratios.push_back(rigtorp_samples[index].transfers_per_second() / spsc_rate);
            spsc_retries.push_back(spsc_samples[index].retries_per_transfer());
            rigtorp_retries.push_back(rigtorp_samples[index].retries_per_transfer());
            minimum_cpu_occupancy = std::min(
                minimum_cpu_occupancy,
                std::min(spsc_samples[index].minimum_cpu_occupancy(),
                         rigtorp_samples[index].minimum_cpu_occupancy()));
            verified = verified && spsc_samples[index].verified && rigtorp_samples[index].verified;
            affinity_verified = affinity_verified &&
                                spsc_samples[index].producer_affinity_applied &&
                                spsc_samples[index].consumer_affinity_applied &&
                                rigtorp_samples[index].producer_affinity_applied &&
                                rigtorp_samples[index].consumer_affinity_applied;
        }
        const rate_statistics statistics = summarize_rates(ratios);
        const double ratio_cv = coefficient_of_variation(statistics);
        const double spsc_median_retries = summarize_rates(spsc_retries).median;
        const double rigtorp_median_retries = summarize_rates(rigtorp_retries).median;
        std::string gate_reason = "passed";
        if (ratios.size() < minimum_ranking_samples) {
            gate_reason = "insufficient_samples";
        } else if (!verified) {
            gate_reason = "unverified_sample";
        } else if (!affinity_verified) {
            gate_reason = "affinity_not_applied";
        } else if (ratio_cv > maximum_paired_ratio_cv) {
            gate_reason = "paired_ratio_variation";
        } else if (minimum_cpu_occupancy < minimum_endpoint_cpu_occupancy) {
            gate_reason = "low_cpu_occupancy";
        }
        const bool stable = gate_reason == "passed";

        auto &out = *output_;
        out << std::setprecision(17)
            << "{\"kind\":\"paired_summary\",\"format_version\":"
            << benchmark_format_version
            << ",\"commit\":" << json_quote(input.commit)
            << ",\"workload\":" << json_quote(workload_name(mode))
            << ",\"direction\":" << json_quote(affinity.name)
            << ",\"affinity\":[" << affinity.affinity.producer_cpu << ','
            << affinity.affinity.consumer_cpu << ']'
            << ",\"numerator\":\"rigtorp::SPSCQueue\""
            << ",\"denominator\":\"spsc::queue<CFA>\""
            << ",\"samples\":" << ratios.size()
            << ",\"min_rate_ratio\":" << statistics.minimum
            << ",\"median_rate_ratio\":" << statistics.median
            << ",\"mean_rate_ratio\":" << statistics.mean
            << ",\"max_rate_ratio\":" << statistics.maximum
            << ",\"sample_standard_deviation\":" << statistics.sample_standard_deviation
            << ",\"ratio_coefficient_of_variation\":" << ratio_cv
            << ",\"minimum_cpu_occupancy\":" << minimum_cpu_occupancy
            << ",\"spsc_median_retries_per_transfer\":" << spsc_median_retries
            << ",\"rigtorp_median_retries_per_transfer\":" << rigtorp_median_retries
            << ",\"affinity_verified\":" << (affinity_verified ? "true" : "false")
            << ",\"gate_reason\":" << json_quote(gate_reason)
            << ",\"ranking_eligible\":" << (stable ? "true" : "false")
            << ",\"verified\":" << (verified ? "true" : "false")
            << "}\n";
        out.flush();

        return {affinity.name,
                gate_reason,
                statistics,
                ratio_cv,
                minimum_cpu_occupancy,
                spsc_median_retries,
                rigtorp_median_retries,
                ratios.size(),
                verified,
                affinity_verified,
                stable};
    }

    void write_comparison_summary(
        const options &input,
        const workload mode,
        const std::vector<paired_direction_result> &directions) {
        std::string status = "inconclusive";
        std::string reason = "bidirectional_affinity_required";
        bool ranking_eligible = false;
        double geometric_mean_ratio = 0.0;
        double direction_ratio_spread = 0.0;
        std::string forward_gate_reason = "missing";
        std::string reverse_gate_reason = "missing";

        if (!directions.empty()) {
            forward_gate_reason = directions.front().gate_reason;
        }
        if (directions.size() == 2u) {
            const paired_direction_result &forward = directions[0];
            const paired_direction_result &reverse = directions[1];
            reverse_gate_reason = reverse.gate_reason;
            const double first_ratio = forward.ratio_statistics.median;
            const double second_ratio = reverse.ratio_statistics.median;
            geometric_mean_ratio = std::sqrt(first_ratio * second_ratio);
            const double lower = std::min(first_ratio, second_ratio);
            const double upper = std::max(first_ratio, second_ratio);
            direction_ratio_spread = lower != 0.0 ? (upper / lower) - 1.0
                                                  : std::numeric_limits<double>::infinity();

            if (!forward.stable && !reverse.stable) {
                reason = "both_direction_sample_gates_failed";
            } else if (!forward.stable) {
                reason = "forward_" + forward.gate_reason;
            } else if (!reverse.stable) {
                reason = "reverse_" + reverse.gate_reason;
            } else if (direction_ratio_spread > maximum_direction_ratio_spread) {
                reason = "direction_sensitive_result";
            } else {
                const bool both_spsc = first_ratio < parity_ratio_lower &&
                                       second_ratio < parity_ratio_lower;
                const bool both_rigtorp = first_ratio > parity_ratio_upper &&
                                          second_ratio > parity_ratio_upper;
                const bool both_parity = first_ratio >= parity_ratio_lower &&
                                         first_ratio <= parity_ratio_upper &&
                                         second_ratio >= parity_ratio_lower &&
                                         second_ratio <= parity_ratio_upper;
                if (both_spsc) {
                    status = "spsc_faster";
                    reason = "both_directions_agree";
                    ranking_eligible = true;
                } else if (both_rigtorp) {
                    status = "rigtorp_faster";
                    reason = "both_directions_agree";
                    ranking_eligible = true;
                } else if (both_parity) {
                    status = "parity";
                    reason = "both_directions_within_parity_band";
                    ranking_eligible = true;
                } else {
                    reason = "direction_classification_disagrees";
                }
            }
        }

        auto &out = *output_;
        out << std::setprecision(17)
            << "{\"kind\":\"comparison_summary\",\"format_version\":"
            << benchmark_format_version
            << ",\"commit\":" << json_quote(input.commit)
            << ",\"workload\":" << json_quote(workload_name(mode))
            << ",\"status\":" << json_quote(status)
            << ",\"reason\":" << json_quote(reason)
            << ",\"ranking_eligible\":" << (ranking_eligible ? "true" : "false")
            << ",\"geometric_mean_rate_ratio\":" << geometric_mean_ratio
            << ",\"direction_ratio_spread\":" << direction_ratio_spread
            << ",\"directions\":" << directions.size()
            << ",\"forward_gate_reason\":" << json_quote(forward_gate_reason)
            << ",\"reverse_gate_reason\":" << json_quote(reverse_gate_reason)
            << ",\"ratio_semantics\":\"rigtorp_over_spsc\""
            << "}\n";
        out.flush();
    }

private:
    std::ofstream file_{};
    std::ostream *output_{nullptr};
};

template <class Adapter>
[[nodiscard]] static sample_result run_measurement(const options &input,
                                                     const workload mode,
                                                     persistent_parallel_executor &executor) {
    return mode == workload::steady ? measure_steady<Adapter>(input, executor)
                                     : measure_boundary<Adapter>(input, executor);
}

struct execution_context {
    affinity_run affinity{};
    std::unique_ptr<persistent_parallel_executor> executor{};
};

[[nodiscard]] static std::vector<execution_context> make_execution_contexts(
    const options &input) {
    std::vector<execution_context> contexts;
    for (const affinity_run &affinity : make_affinity_runs(input)) {
        contexts.push_back(
            {affinity, std::make_unique<persistent_parallel_executor>(affinity.affinity)});
    }
    return contexts;
}

template <class Fn>
static void for_each_direction(const std::size_t count,
                               const unsigned iteration,
                               Fn &&function) {
    for (std::size_t offset = 0u; offset < count; ++offset) {
        const std::size_t index = (iteration & 1u) == 0u
                                      ? offset
                                      : static_cast<std::size_t>(count - offset - 1u);
        function(index);
    }
}

template <class Adapter>
static void run_case(jsonl_writer &writer,
                     const options &input,
                      const char *implementation,
                      const char *policy,
                      const workload mode) {
    std::vector<execution_context> contexts = make_execution_contexts(input);
    for (unsigned warmup = 0u; warmup < input.warmup; ++warmup) {
        for_each_direction(contexts.size(), warmup, [&](const std::size_t direction) {
            (void)run_measurement<Adapter>(input, mode, *contexts[direction].executor);
        });
    }

    std::vector<std::vector<sample_result>> samples(contexts.size());
    for (std::vector<sample_result> &direction_samples : samples) {
        direction_samples.reserve(input.samples);
    }
    bool all_verified = true;
    for (unsigned sample = 0u; sample < input.samples; ++sample) {
        for_each_direction(contexts.size(), sample, [&](const std::size_t direction) {
            sample_result result = run_measurement<Adapter>(
                input, mode, *contexts[direction].executor);
            writer.write_sample(input, implementation, policy, mode,
                                contexts[direction].affinity, sample, result);
            all_verified = all_verified && result.verified;
            samples[direction].push_back(result);
        });
    }
    for (std::size_t direction = 0u; direction < contexts.size(); ++direction) {
        writer.write_summary(input, implementation, policy, mode,
                             contexts[direction].affinity, samples[direction]);
    }
    if (!all_verified) {
        throw std::runtime_error("benchmark correctness check failed");
    }
}

template <reg Capacity>
static void run_paired_queue_case(jsonl_writer &writer,
                                  const options &input,
                                  const workload mode) {
    using spsc_adapter = spsc_queue_adapter<Capacity, spsc::policy::CFA<>>;
    using rigtorp_adapter = rigtorp_queue_adapter<Capacity>;

    struct paired_context {
        execution_context execution{};
        std::vector<sample_result> spsc_samples{};
        std::vector<sample_result> rigtorp_samples{};
    };

    std::vector<execution_context> executions = make_execution_contexts(input);
    std::vector<paired_context> contexts;
    contexts.reserve(executions.size());
    for (execution_context &execution : executions) {
        paired_context context{};
        context.execution = std::move(execution);
        context.spsc_samples.reserve(input.samples);
        context.rigtorp_samples.reserve(input.samples);
        contexts.push_back(std::move(context));
    }

    const auto spsc_first_for = [mode](const unsigned pair_index,
                                       const std::size_t direction) noexcept {
        // Alternate execution order. Starting the boundary phase in the
        // opposite direction, and offsetting the reverse-affinity case, keeps
        // implementation order balanced across both endpoint assignments.
        const unsigned phase_bias = mode == workload::boundary ? 1u : 0u;
        return ((pair_index + phase_bias + static_cast<unsigned>(direction)) & 1u) == 0u;
    };
    const auto run_pair = [&](paired_context &context,
                              const std::size_t direction,
                              const unsigned pair_index,
                              const bool record) {
        sample_result spsc_result{};
        sample_result rigtorp_result{};
        const bool spsc_first = spsc_first_for(pair_index, direction);
        if (spsc_first) {
            spsc_result = run_measurement<spsc_adapter>(
                input, mode, *context.execution.executor);
            if (record) {
                writer.write_sample(input, "spsc::queue", "CFA", mode,
                                    context.execution.affinity, pair_index,
                                    spsc_result, static_cast<int>(pair_index), 0);
            }
            rigtorp_result = run_measurement<rigtorp_adapter>(
                input, mode, *context.execution.executor);
            if (record) {
                writer.write_sample(input, "rigtorp::SPSCQueue", "rigtorp-v1.1", mode,
                                    context.execution.affinity, pair_index, rigtorp_result,
                                    static_cast<int>(pair_index), 1);
            }
        } else {
            rigtorp_result = run_measurement<rigtorp_adapter>(
                input, mode, *context.execution.executor);
            if (record) {
                writer.write_sample(input, "rigtorp::SPSCQueue", "rigtorp-v1.1", mode,
                                    context.execution.affinity, pair_index, rigtorp_result,
                                    static_cast<int>(pair_index), 0);
            }
            spsc_result = run_measurement<spsc_adapter>(
                input, mode, *context.execution.executor);
            if (record) {
                writer.write_sample(input, "spsc::queue", "CFA", mode,
                                    context.execution.affinity, pair_index,
                                    spsc_result, static_cast<int>(pair_index), 1);
            }
        }
        if (record) {
            context.spsc_samples.push_back(spsc_result);
            context.rigtorp_samples.push_back(rigtorp_result);
        }
    };

    for (unsigned warmup = 0u; warmup < input.warmup; ++warmup) {
        for_each_direction(contexts.size(), warmup, [&](const std::size_t direction) {
            run_pair(contexts[direction], direction, warmup, false);
        });
    }

    for (unsigned sample = 0u; sample < input.samples; ++sample) {
        for_each_direction(contexts.size(), sample, [&](const std::size_t direction) {
            run_pair(contexts[direction], direction, sample, true);
        });
    }

    const auto has_unverified = [](const std::vector<sample_result> &samples) {
        return std::any_of(samples.begin(), samples.end(),
                           [](const sample_result &sample) { return !sample.verified; });
    };

    std::vector<paired_direction_result> direction_summaries;
    direction_summaries.reserve(contexts.size());
    for (paired_context &context : contexts) {
        writer.write_summary(input, "spsc::queue", "CFA", mode,
                             context.execution.affinity, context.spsc_samples);
        writer.write_summary(input, "rigtorp::SPSCQueue", "rigtorp-v1.1", mode,
                             context.execution.affinity, context.rigtorp_samples);
        direction_summaries.push_back(writer.write_paired_summary(
            input, mode, context.execution.affinity,
            context.spsc_samples, context.rigtorp_samples));

        if (has_unverified(context.spsc_samples) ||
            has_unverified(context.rigtorp_samples)) {
            throw std::runtime_error("paired queue benchmark correctness check failed");
        }
    }
    writer.write_comparison_summary(input, mode, direction_summaries);
}

template <reg Capacity>
static void run_capacity(jsonl_writer &writer, const options &input) {
    writer.write_metadata<Capacity>(input);

    const auto run_queue = [&](const workload mode) {
        run_paired_queue_case<Capacity>(writer, input, mode);
    };
    const auto run_fifo = [&](const workload mode) {
        run_case<fifo_adapter<Capacity, spsc::policy::CFA<>>>(
            writer, input, "spsc::fifo", "CFA", mode);
    };
    const auto run_policy = [&](const workload mode) {
        run_case<fifo_adapter<Capacity, spsc::policy::A<>>>(
            writer, input, "spsc::fifo", "A", mode);
        run_case<fifo_adapter<Capacity, spsc::policy::FA<>>>(
            writer, input, "spsc::fifo", "FA", mode);
        run_case<fifo_adapter<Capacity, spsc::policy::CA<>>>(
            writer, input, "spsc::fifo", "CA", mode);
        run_case<fifo_adapter<Capacity, spsc::policy::CFA<>>>(
            writer, input, "spsc::fifo", "CFA", mode);
    };

    for (const workload mode : {workload::steady, workload::boundary}) {
        if (input.suite == "all" || input.suite == "queue") {
            run_queue(mode);
        }
        if (input.suite == "fifo") {
            run_fifo(mode);
        }
        if (input.suite == "all" || input.suite == "policy") {
            run_policy(mode);
        }
    }
}

static void dispatch_capacity(jsonl_writer &writer, const options &input) {
    switch (input.capacity) {
    case 64u: run_capacity<64u>(writer, input); break;
    case 256u: run_capacity<256u>(writer, input); break;
    case 1024u: run_capacity<1024u>(writer, input); break;
    case 4096u: run_capacity<4096u>(writer, input); break;
    default: usage("unsupported capacity");
    }
}

} // namespace spsc_bench

int main(int argc, char **argv) {
    try {
        spsc_bench::options input = spsc_bench::parse_options(argc, argv);
        input.affinity = spsc_bench::resolve_affinity(input.affinity);
        spsc_bench::jsonl_writer writer(input.output);
        spsc_bench::dispatch_capacity(writer, input);
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
