// queue_test.cpp
// Paranoid API/contract test for spsc::queue.
//
// Goals:
//  - Exercise public methods for major flavors:
//      * static typed:  spsc::queue<T, Capacity>
//      * dynamic typed: spsc::queue<T, 0> (resize/reserve at runtime)
//  - Cover policy variants: P, V, A<>, CA<>.
//  - Verify object lifetime (placement-new + explicit destructor).
//  - Verify bulk region APIs (claim_write/claim_read) and snapshot APIs.
//  - Stress move/swap/resize and randomized state-machine fuzzing.
//
// Notes:
//  - resize()/clear()/swap() are NOT thread-safe with push/pop. Threaded tests
//    use only producer/consumer operations.
//  - This file intentionally contains redundant checks: it is a contract
//    tripwire.

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <deque>
#include <limits>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(SPSC_ASSERT) && !defined(NDEBUG)
#  define SPSC_ASSERT(expr) do { if(!(expr)) { std::abort(); } } while(0)
#endif

#include "queue.hpp"


// =====================================================================================
// Contract checklist (living documentation)
//
// The goal of this file is to behave like a hostile spec lawyer:
//
//  1) Validate every public API path of spsc::queue<> across policies.
//  2) Validate object lifetime rules (placement-new + explicit destruction).
//  3) Validate wrap-around correctness for indexing, iterators, and bulk regions.
//  4) Validate snapshot safety contracts (identity checks + consume/try_consume).
//  5) Validate dynamic resize migration (order preservation, no leaks).
//  6) Validate RAII guards (commit/cancel, move-only ownership, no double-publish).
//  7) Validate threaded SPSC progress for atomic/cached counters.
//  8) Validate allocator behavior (no leaks, correct dealloc counts).
//
// Notes:
//  - This is intentionally "paranoid" and will look redundant on purpose.
//  - We avoid asserting accidental properties (e.g., "queue must never be full")
//    because correctness does not imply that.
//  - For concurrency tests we only require monotonic progress and FIFO ordering.
//  - For debug builds, SPSC_ASSERT may abort on misuse; such negative tests are
//    kept in the "invalid_inputs" suite and only check safe-return APIs.
//
// If you change queue.hpp behavior, update this checklist and add a regression
// test that fails before the change and passes after.

// -------------------------------------------------------------------------------------
// Suite map (what breaks if you mess up X):
//
//  - static_* / dynamic_* suites
//      * basic push/pop + sizing + can_read/can_write invariants
//      * policy coverage (P/V/A/CA)
//
//  - threaded_* suites
//      * SPSC progress (no deadlock/stall)
//      * FIFO order for queue, including under contention
//
//  - deterministic_interleaving()
//      * tries to catch ordering bugs without relying on timing
//
//  - allocator_accounting()
//      * checks that dynamic resize/destroy returns memory (no leaks)
//      * checks that failed allocate paths do not corrupt state
//
//  - state_machine_fuzz_sweep()
//      * randomized API usage (single-thread) with a reference model
//      * catches subtle off-by-one + wrap-around + snapshot corner cases
//
//  - iterators_* / operator[]
//      * linearization of logical FIFO order, including under wrap-around
//
//  - bulk_regions_* suites
//      * claim_write/claim_read splitting, max_count limiting
//      * publish(n)/pop(n) exactly advance by N
//
//  - raii_guards_* suites
//      * write_guard: no publish unless constructed+armed, no leaks
//      * read_guard: pop-on-destroy unless canceled/committed
//      * move-only ownership does not double-pop/double-publish
//
//  - snapshot_* suites
//      * snapshot identity checks (data pointer + mask)
//      * consume/try_consume correctness
//
// If you add new API to queue.hpp, add it to api_smoke_compile() and add a
// deterministic test that would fail under a plausible regression.
// -------------------------------------------------------------------------------------

// =====================================================================================



namespace spsc_queue_death_detail {

#if !defined(NDEBUG)

static constexpr int kDeathExitCode = 0xAB;

static void sigabrt_handler_(int) noexcept {
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode) {
    // Convert "assert -> abort" into a deterministic exit code so QProcess won't hang on Windows dialogs.
    std::signal(SIGABRT, &sigabrt_handler_);

    using Q = spsc::queue<std::uint32_t, 16u, spsc::policy::P>;

    if (std::strcmp(mode, "pop_empty") == 0) {
        Q q;
        q.pop(); // Must assert: pop on empty.
    } else if (std::strcmp(mode, "publish_full") == 0) {
        Q q;
        for (std::uint32_t i = 0; i < 16u; ++i) {
            const bool ok = q.try_push(i);
            if (!ok) {
                std::_Exit(0xEE);
            }
        }
        q.publish(); // Must assert: publish while full.
    } else if (std::strcmp(mode, "guard_commit_without_construct") == 0) {
        Q q;
        auto g = q.scoped_write();
        g.commit(); // Must assert: commit without constructed value.
    } else if (std::strcmp(mode, "double_emplace") == 0) {
        Q q;
        auto g = q.scoped_write();
        (void)g.emplace(1u);
        (void)g.emplace(2u); // Must assert: double construction.
    } else {
        std::_Exit(0xEF);
    }

    // If we reach this point, assertions did not fire.
    std::_Exit(0xF0);
}

struct Runner_ {
    Runner_() {
        const char* mode = std::getenv("SPSC_QUEUE_DEATH");
        if (mode && *mode) {
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

#endif // !defined(NDEBUG)

} // namespace spsc_queue_death_detail

namespace {

constexpr reg kSmallCap = 16u;
constexpr reg kMedCap   = 256u;
constexpr reg kBigCap   = 1024u;

#if defined(NDEBUG)
constexpr int kFuzzIters        = 40'000;
constexpr int kSwapIters        = 50'000;
constexpr int kThreadIters      = 120'000;
constexpr int kThreadTimeoutMs  = 6000;
#else
constexpr int kFuzzIters        = 8'000;
constexpr int kSwapIters        = 8'000;
constexpr int kThreadIters      = 30'000;
constexpr int kThreadTimeoutMs  = 15'000;
#endif

// -------------------------
// Compile-time API smoke
// -------------------------

template <class Q>
static void api_smoke_compile() {
    using value_type = typename Q::value_type;
    static_assert(std::is_same_v<decltype(std::declval<Q&>().is_valid()), bool>);

    static_assert(std::is_same_v<decltype(std::declval<Q&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().free()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().empty()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().full()), bool>);

    // Producer
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_push(std::declval<value_type>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().push(std::declval<value_type>())), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_emplace(1)), value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().emplace(1)), value_type&>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_claim()), value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim()), value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish(reg{1})), void>);

    // Consumer
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_front()), value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().try_front()), const value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().front()), value_type&>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().front()), const value_type&>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop(reg{1})), void>);

    // Bulk regions
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write()), typename Q::write_regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read()), typename Q::read_regions>);

    // Snapshots
    static_assert(std::is_same_v<decltype(std::declval<Q&>().make_snapshot()), typename Q::snapshot>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().make_snapshot()), typename Q::const_snapshot>);

    // RAII
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_write()), typename Q::write_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_read()), typename Q::read_guard>);
}

// -------------------------
// Test payloads
// -------------------------

struct Blob {
    std::uint32_t seq{0};
    std::uint32_t tag{0xA5A5A5A5u};
};

struct alignas(64) Aligned64 {
    std::uint32_t x{0};
    std::uint32_t y{0};
    std::uint8_t  pad[64 - 8]{};
};

struct Tracked {
    std::uint32_t seq{0};
    std::uint32_t cookie{0xC0FFEEu};

    static inline std::atomic<int> live{0};
    static inline std::atomic<long long> ctor{0};
    static inline std::atomic<long long> dtor{0};
    static inline std::atomic<long long> copy{0};
    static inline std::atomic<long long> move{0};

    Tracked() { ++live; ++ctor; }
    explicit Tracked(std::uint32_t s) : seq(s) { ++live; ++ctor; }

    Tracked(const Tracked& o) : seq(o.seq), cookie(o.cookie) { ++live; ++ctor; ++copy; }

    Tracked(Tracked&& o) noexcept : seq(o.seq), cookie(o.cookie) {
        o.cookie = 0xDEADu;
        ++live; ++ctor; ++move;
    }

    Tracked& operator=(const Tracked&) = delete;
    Tracked& operator=(Tracked&&) = delete;

    ~Tracked() {
        cookie = 0xBADC0DEu;
        ++dtor;
        --live;
    }
};

static void tracked_reset() {
    Tracked::live.store(0);
    Tracked::ctor.store(0);
    Tracked::dtor.store(0);
    Tracked::copy.store(0);
    Tracked::move.store(0);
}

// -------------------------
// Counting allocator (stateless)
// -------------------------

struct AllocStats {
    static inline std::atomic<std::size_t> allocs{0};
    static inline std::atomic<std::size_t> deallocs{0};
    static inline std::atomic<std::size_t> bytes_live{0};
    static inline std::atomic<std::size_t> bytes_peak{0};

    static void reset() {
        allocs.store(0);
        deallocs.store(0);
        bytes_live.store(0);
        bytes_peak.store(0);
    }

    static void on_alloc(std::size_t bytes) {
        allocs.fetch_add(1);
        const auto live_now = bytes_live.fetch_add(bytes) + bytes;
        auto peak = bytes_peak.load();
        while (live_now > peak && !bytes_peak.compare_exchange_weak(peak, live_now)) {
            // CAS loop
        }
    }

    static void on_dealloc(std::size_t bytes) {
        deallocs.fetch_add(1);
        bytes_live.fetch_sub(bytes);
    }
};

template <class T, std::size_t Align>
struct CountingAlignedAlloc {
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    CountingAlignedAlloc() = default;

    template <class U>
    CountingAlignedAlloc(const CountingAlignedAlloc<U, Align>&) noexcept {}

    template <class U>
    struct rebind { using other = CountingAlignedAlloc<U, Align>; };

    [[nodiscard]] pointer allocate(size_type n) noexcept {
        if (n == 0) {
            return nullptr;
        }
        const std::size_t bytes = n * sizeof(T);
        void* p = ::operator new(bytes, std::align_val_t(Align), std::nothrow);
        if (!p) {
            return nullptr;
        }
        AllocStats::on_alloc(bytes);
        return static_cast<pointer>(p);
    }

    void deallocate(pointer p, size_type n) noexcept {
        if (!p || n == 0) {
            return;
        }
        const std::size_t bytes = n * sizeof(T);
        AllocStats::on_dealloc(bytes);
        ::operator delete(p, std::align_val_t(Align));
    }
};

// -------------------------
// Small helpers (single-threaded; fail fast, no infinite spins)
// -------------------------

template <class Q>
static void drain_to_vector(Q& q, std::vector<typename Q::value_type>& out) {
    using T = typename Q::value_type;
    while (!q.empty()) {
        T* p = q.try_front();
        QVERIFY(p != nullptr);
        out.push_back(*p);
        q.pop();
    }
}

template <class Q>
static void fill_seq(Q& q, std::uint32_t first, std::uint32_t count) {
    QVERIFY2(q.free() >= static_cast<reg>(count),
             "fill_seq(): not enough free space (would spin forever)");
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t v = first + i;
        if constexpr (std::is_same_v<typename Q::value_type, Blob>) {
            QVERIFY(q.try_push(Blob{v, 0xA5A5A5A5u}));
        } else {
            QVERIFY(q.try_push(typename Q::value_type{v}));
        }
    }
}

template <class Q>
static void check_fifo_exact(Q& q, std::uint32_t first, std::uint32_t count) {
    for (std::uint32_t i = 0; i < count; ++i) {
        auto* p = q.try_front();
        QVERIFY(p != nullptr);
        const std::uint32_t expected = first + i;

        if constexpr (std::is_same_v<typename Q::value_type, Blob>) {
            QCOMPARE(p->seq, expected);
            QCOMPARE(p->tag, 0xA5A5A5A5u);
        } else if constexpr (std::is_same_v<typename Q::value_type, Tracked>) {
            QCOMPARE(p->seq, expected);
            QCOMPARE(p->cookie, 0xC0FFEEu);
        } else {
            QCOMPARE(static_cast<std::uint32_t>(*p), expected);
        }

        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void placement_write_region(Q& q, std::uint32_t& next_seq, reg max_count) {
    auto wr = q.claim_write(max_count);
    if (wr.empty()) {
        return;
    }

    using T = typename Q::value_type;
    auto construct_one = [&](T* dst) {
        if constexpr (std::is_same_v<T, Blob>) {
            new (dst) T{next_seq++, 0xA5A5A5A5u};
        } else {
            new (dst) T{next_seq++};
        }
    };

    T* p1 = wr.first.ptr_uninit();
    for (reg i = 0; i < wr.first.count; ++i) {
        construct_one(p1 + i);
    }

    if (wr.second.count) {
        T* p2 = wr.second.ptr_uninit();
        for (reg i = 0; i < wr.second.count; ++i) {
            construct_one(p2 + i);
        }
    }

    q.publish(wr.total);
}

template <class Q>
static void check_read_regions(Q& q, std::uint32_t& expected_seq, reg max_count) {
    auto rr = q.claim_read(max_count);
    if (rr.empty()) {
        return;
    }

    using T = typename Q::value_type;
    auto check_one = [&](const T& v) {
        if constexpr (std::is_same_v<T, Blob>) {
            QCOMPARE(v.seq, expected_seq);
            QCOMPARE(v.tag, 0xA5A5A5A5u);
        } else if constexpr (std::is_same_v<T, Tracked>) {
            QCOMPARE(v.seq, expected_seq);
            QCOMPARE(v.cookie, 0xC0FFEEu);
        } else {
            QCOMPARE(static_cast<std::uint32_t>(v), expected_seq);
        }
        ++expected_seq;
    };

    for (reg i = 0; i < rr.first.count; ++i) {
        check_one(rr.first.ptr[i]);
    }
    for (reg i = 0; i < rr.second.count; ++i) {
        check_one(rr.second.ptr[i]);
    }

    q.pop(rr.total);
}
// -------------------------
// Suites
// -------------------------

template <class Q>
static void basic_suite(Q& q) {
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 2u);
    QVERIFY(q.empty());
    QVERIFY(!q.full());
    QCOMPARE(q.size(), reg{0});

    // Push a couple.
    q.push(typename Q::value_type{1});
    q.emplace(2);
    QCOMPARE(q.size(), reg{2});

    // Front + operator[] must agree.
    QCOMPARE(q.front().seq, std::uint32_t{1});
    QCOMPARE(q[0].seq, std::uint32_t{1});
    QCOMPARE(q[1].seq, std::uint32_t{2});

    // Pop one.
    q.pop();
    QCOMPARE(q.front().seq, std::uint32_t{2});
    QCOMPARE(q.size(), reg{1});

    // Pop via read_guard.
    {
        auto g = q.scoped_read();
        QVERIFY(bool(g));
        QCOMPARE(g->seq, std::uint32_t{2});
    }
    QVERIFY(q.empty());

    // try_front on empty.
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());

    // Fill to full.
    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) {
        QVERIFY(q.try_push(typename Q::value_type{1000u + static_cast<std::uint32_t>(i)}));
    }
    QVERIFY(q.full());
    QVERIFY(!q.try_push(typename Q::value_type{777}));

    // Drain and verify FIFO order.
    for (reg i = 0; i < cap; ++i) {
        auto* p = q.try_front();
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, 1000u + static_cast<std::uint32_t>(i));
        q.pop();
    }
    QVERIFY(q.empty());

    // claim/publish path.
    {
        auto* slot = q.try_claim();
        QVERIFY(slot != nullptr);
        new (slot) typename Q::value_type{42};
        QVERIFY(q.try_publish());
        QVERIFY(!q.empty());
        QCOMPARE(q.front().seq, std::uint32_t{42});
        q.pop();
    }

    // Scoped write: emplace publishes on scope exit.
    {
        auto g = q.scoped_write();
        QVERIFY(bool(g));
        static_cast<void>(g.emplace(7));

    }
    QVERIFY(!q.empty());
    QCOMPARE(q.front().seq, std::uint32_t{7});
    q.pop();
    QVERIFY(q.empty());

    // Scoped write: construct but do not publish => must destroy.
    if constexpr (!std::is_trivially_destructible_v<typename Q::value_type>) {
        const auto d0 = Tracked::dtor.load();
        {
            auto g = q.scoped_write();
            QVERIFY(bool(g));
            new (g.get()) typename Q::value_type{123};
            g.mark_constructed();
            // no arm_publish
        }
        QVERIFY(q.empty());
        QVERIFY(Tracked::dtor.load() >= d0 + 1);
    }

    // Bulk regions: write some, read some.
    std::uint32_t next_seq = 1;
    std::uint32_t expected = 1;
    for (int k = 0; k < 4; ++k) {
        placement_write_region(q, next_seq, cap);
    }
    while (!q.empty()) {
        check_read_regions(q, expected, cap);
    }
    QCOMPARE(next_seq, expected);

    // Iterator sanity.
    fill_seq(q, 500, 10);
    std::uint32_t iter_seq = 500;
    for (auto& v : q) {
        QCOMPARE(v.seq, iter_seq++);
    }
    QCOMPARE(iter_seq, 510u);
    q.clear();
    QVERIFY(q.empty());
}

template <class Policy>
static void run_static_suite() {
    using Q = spsc::queue<Tracked, kSmallCap, Policy>;
    api_smoke_compile<Q>();

    tracked_reset();
    {
        Q q;
        QVERIFY(q.is_valid());
        QCOMPARE(q.capacity(), kSmallCap);

        basic_suite(q);

        // Snapshot + consume.
        fill_seq(q, 1, 12);
        auto snap = q.make_snapshot();
        QCOMPARE(reg(snap.size()), reg{12});
        q.consume(snap);
        QVERIFY(q.empty());

        // Swap with another.
        fill_seq(q, 100, 5);
        Q other;
        fill_seq(other, 200, 3);
        q.swap(other);
        check_fifo_exact(q, 200, 3);
        check_fifo_exact(other, 100, 5);

        // Move: moved-from static becomes invalid.
        fill_seq(other, 10, 4);
        Q moved{std::move(other)};
        QVERIFY(moved.is_valid());
        QVERIFY(!other.is_valid());
        check_fifo_exact(moved, 10, 4);

        // Explicit destroy must be idempotent.
        moved.destroy();
        QVERIFY(!moved.is_valid());
        moved.destroy();
        QVERIFY(!moved.is_valid());
    }

    // After scope, all objects must have been destroyed.
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

template <class Policy>
static void run_dynamic_suite() {
    using Q = spsc::queue<Tracked, 0, Policy>;
    api_smoke_compile<Q>();

    tracked_reset();
    {
        Q q;
        QVERIFY(!q.is_valid());
        QCOMPARE(q.capacity(), reg{0});
        QVERIFY(q.empty());

        // Invalid operations must be safe.
        QVERIFY(q.try_front() == nullptr);
        QVERIFY(!q.try_pop());
        q.clear();

        // resize(0) is a no-op success.
        QVERIFY(q.resize(0u));
        QVERIFY(!q.is_valid());

        QVERIFY(q.resize(kSmallCap));
        QVERIFY(q.is_valid());
        QCOMPARE(q.capacity(), kSmallCap);

        basic_suite(q);

        // Migration: keep order.
        {
            constexpr std::uint32_t kMigCount = 20u;
            const std::uint32_t cap0 = static_cast<std::uint32_t>(q.capacity());
            const std::uint32_t batch1 = (cap0 < kMigCount) ? cap0 : kMigCount;

            fill_seq(q, 1u, batch1);

            QVERIFY(q.resize(kMedCap));
            QVERIFY(q.is_valid());
            QVERIFY(q.capacity() >= kMedCap);

            if (batch1 < kMigCount) {
                fill_seq(q, 1u + batch1, kMigCount - batch1);
            }

            QCOMPARE(q.size(), reg{kMigCount});
            check_fifo_exact(q, 1u, kMigCount);
        }

        // reserve should keep larger capacity.
        QVERIFY(q.reserve(128));
        QVERIFY(q.capacity() >= 128u);

        // try_consume must reject mismatched snapshots.
        fill_seq(q, 10, 10);
        auto snap = q.make_snapshot();
        Q other;
        QVERIFY(other.resize(64));
        QVERIFY(!other.try_consume(snap));
        QVERIFY(q.try_consume(snap));
        QVERIFY(q.empty());

        q.destroy();
        QVERIFY(!q.is_valid());
    }

    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

template <class Policy>
static void run_threaded_suite(const char* name) {
    using Q = spsc::queue<Blob, 0, Policy>;

    Q q;
    QVERIFY2(q.resize(kBigCap), name);
    QVERIFY2(q.is_valid(), name);

    std::atomic<bool> abort{false};
    std::atomic<bool> prod_done{false};

    auto should_abort = [&]() -> bool {
        return abort.load(std::memory_order_relaxed);
    };

    std::thread prod([&]() {
        for (int i = 1; i <= kThreadIters && !should_abort(); ++i) {
            Blob b{static_cast<std::uint32_t>(i), 0xA5A5A5A5u};
            while (!q.try_push(b)) {
                if (should_abort()) {
                    return;
                }
                std::this_thread::yield();
            }
        }
        prod_done.store(true, std::memory_order_release);
    });

    std::thread cons([&]() {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(kThreadTimeoutMs);
        int expected = 1;
        while (expected <= kThreadIters) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            Blob* p = q.try_front();
            if (!p) {
                if (prod_done.load(std::memory_order_acquire) && q.empty()) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
                continue;
            }

            if (p->seq != static_cast<std::uint32_t>(expected)) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            q.pop();
            ++expected;
        }
    });

    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), name);
    QVERIFY2(q.empty(), name);
}

// A small deterministic interleaving test that catches common lifetime bugs.
// This is single-threaded but tries to mimic typical producer/consumer steps.

template <class Q>
static void deterministic_interleaving(Q& q) {
    QVERIFY(q.is_valid());
    QVERIFY(q.empty());

    // Contract: claim() does NOT reserve. Until publish(), repeated claims return the same slot.
    auto* s0 = q.try_claim();
    QVERIFY(s0 != nullptr);
    new (s0) typename Q::value_type{1};

    auto* s0_again = q.try_claim();
    QVERIFY(s0_again != nullptr);
    QCOMPARE(reinterpret_cast<void*>(s0_again), reinterpret_cast<void*>(s0));

    // Publish the constructed value.
    QVERIFY(q.try_publish());
    QVERIFY(!q.empty());
    QCOMPARE(q.front().seq, std::uint32_t{1});

    // Consumer pops it.
    q.pop();
    QVERIFY(q.empty());

    // Next element: claim -> construct -> publish (one-at-a-time).
    auto* s1 = q.try_claim();
    QVERIFY(s1 != nullptr);
    new (s1) typename Q::value_type{2};

    QVERIFY(q.try_publish());
    QVERIFY(!q.empty());
    QCOMPARE(q.front().seq, std::uint32_t{2});
    q.pop();
    QVERIFY(q.empty());
}

// -------------------------
// Randomized state-machine fuzz (single-thread)
// -------------------------

// Full → empty cycle used by stress tests:
//  - Fill the queue to full
//  - Verify try_push fails when full
//  - Drain and verify FIFO order for the items we produced

template <class Q>
static void full_empty_cycle_u32(Q& q, std::uint32_t base) {
    using T = typename Q::value_type;
    static_assert(std::is_same_v<T, std::uint32_t>, "Expected queue<uint32_t>.");

    const reg cap = q.capacity();
    QVERIFY(cap > 0u);

    const reg s0 = q.size();
    QVERIFY(s0 <= cap);

    const reg fill = cap - s0;

    // Fill to full.
    for (reg i = 0; i < fill; ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(i));
        QVERIFY(q.try_push(v));
    }

    QCOMPARE(q.size(), cap);
    QVERIFY(q.full());

    // Must fail when full.
    {
        const std::uint32_t extra = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(fill));
        QVERIFY(!q.try_push(extra));
    }

    // Drain: existing prefix (unknown values).
    for (reg i = 0; i < s0; ++i) {
        auto* p = q.try_front();
        QVERIFY(p != nullptr);
        q.pop();
    }

    // Drain: suffix we just wrote.
    for (reg i = 0; i < fill; ++i) {
        auto* p = q.try_front();
        QVERIFY(p != nullptr);
        const std::uint32_t out = *p;
        q.pop();
        const std::uint32_t exp = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(i));
        QCOMPARE(out, exp);
    }

    QVERIFY(q.empty());
    QCOMPARE(q.size(), reg{0});
}

enum class Op : std::uint8_t {
    TryPush,
    TryEmplace,
    ClaimPublish,
    RegionPublish,
    TryPop,
    PopN,
    SnapshotConsume,
    Clear,
    Resize,
    Swap
};

static Op pick_op(std::mt19937& rng) {
    std::uniform_int_distribution<int> d(0, 9);
    return static_cast<Op>(d(rng));
}

template <class Policy>
static void run_state_machine_fuzz(bool dynamic) {
    using T = Tracked;
    using QStatic = spsc::queue<T, kSmallCap, Policy>;
    using QDynamic = spsc::queue<T, 0, Policy>;

    tracked_reset();
    std::mt19937 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> val(1, 1'000'000);

    std::deque<std::uint32_t> model;

    auto check_equal = [&](auto& q) {
        QCOMPARE(q.size(), reg(model.size()));
        for (reg i = 0; i < q.size(); ++i) {
            QCOMPARE(q[i].seq, model[static_cast<std::size_t>(i)]);
        }
        if (!model.empty()) {
            QCOMPARE(q.front().seq, model.front());
        }
    };

    auto do_one = [&](auto& q, auto& other) {
        Op op = pick_op(rng);
        switch (op) {
        case Op::TryPush: {
            const std::uint32_t x = static_cast<std::uint32_t>(val(rng));
            const bool ok = q.try_push(T{x});
            if (ok) {
                model.push_back(x);
            }
            break;
        }
        case Op::TryEmplace: {
            const std::uint32_t x = static_cast<std::uint32_t>(val(rng));
            T* p = q.try_emplace(x);
            if (p) {
                model.push_back(x);
            }
            break;
        }
        case Op::ClaimPublish: {
            if (q.full()) {
                break;
            }
            const std::uint32_t x = static_cast<std::uint32_t>(val(rng));
            T* slot = q.try_claim();
            QVERIFY(slot != nullptr);
            new (slot) T{x};
            QVERIFY(q.try_publish());
            model.push_back(x);
            break;
        }
        case Op::RegionPublish: {
            if (!q.can_write(1)) {
                break;
            }
            const reg maxn = static_cast<reg>(1 + (rng() % 7));
            auto wr = q.claim_write(maxn);
            if (wr.empty()) {
                break;
            }
            T* p1 = wr.first.ptr_uninit();
            for (reg i = 0; i < wr.first.count; ++i) {
                const std::uint32_t x = static_cast<std::uint32_t>(val(rng));
                new (p1 + i) T{x};
                model.push_back(x);
            }
            if (wr.second.count) {
                T* p2 = wr.second.ptr_uninit();
                for (reg i = 0; i < wr.second.count; ++i) {
                    const std::uint32_t x = static_cast<std::uint32_t>(val(rng));
                    new (p2 + i) T{x};
                    model.push_back(x);
                }
            }
            q.publish(wr.total);
            break;
        }
        case Op::TryPop: {
            if (model.empty()) {
                QVERIFY(q.try_front() == nullptr);
                QVERIFY(!q.try_pop());
            } else {
                QVERIFY(q.try_front() != nullptr);
                QVERIFY(q.try_pop());
                model.pop_front();
            }
            break;
        }
        case Op::PopN: {
            if (model.empty()) {
                break;
            }
            const reg n = static_cast<reg>(1 + (rng() % 5));
            const reg can = q.can_read(n) ? n : q.size();
            q.pop(can);
            for (reg i = 0; i < can; ++i) {
                model.pop_front();
            }
            break;
        }
        case Op::SnapshotConsume: {
            auto snap = q.make_snapshot();
            const reg snap_sz = static_cast<reg>(snap.size());
            if (snap_sz == 0u) {
                break;
            }
            // Consume the snapshot.
            QVERIFY(q.try_consume(snap));
            for (reg i = 0; i < snap_sz; ++i) {
                model.pop_front();
            }
            break;
        }
        case Op::Clear: {
            q.clear();
            model.clear();
            break;
        }
        case Op::Resize: {
            if constexpr (std::is_same_v<std::decay_t<decltype(q)>, QDynamic>) {
                // Resize only when dynamic and single-threaded.
                const reg req = static_cast<reg>(2u << (rng() % 9));
                QVERIFY(q.resize(req));
                QVERIFY(q.is_valid());
                QVERIFY(q.capacity() >= 2u);
                // Model unchanged.
            }
            break;
        }
        case Op::Swap: {
            // Only safe in single-thread.
            q.swap(other);
            model.clear();
            // After swap, q content is whatever other had; since we don't
            // track both models, make it empty by clearing both.
            q.clear();
            other.clear();
            break;
        }
        default:
            break;
        }

        check_equal(q);
    };

    if (dynamic) {
        QDynamic q;
        QDynamic other;
        QVERIFY(q.resize(kSmallCap));
        QVERIFY(other.resize(kSmallCap));
        for (int i = 0; i < kFuzzIters; ++i) {
            do_one(q, other);
        }
        q.destroy();
        other.destroy();
    } else {
        QStatic q;
        QStatic other;
        for (int i = 0; i < kFuzzIters; ++i) {
            do_one(q, other);
        }
        q.destroy();
        other.destroy();
    }

    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

// -------------------------
// Tests
// -------------------------


// =====================================================================================
// Additional paranoid coverage (2026-01):
//  - RAII guards (write_guard/read_guard) semantics
//  - Iterator/indexing contracts under wrap-around
//  - Snapshot iteration and stability checks
//  - Bulk regions with max_count + publish(n)/pop(n) correctness
//  - Threaded bulk-region producer/consumer stress (atomic/cached)
// =====================================================================================

template <class Q>
static void check_iterators_and_indexing(Q& q, std::uint32_t first, std::uint32_t count) {
    using T = typename Q::value_type;

    QCOMPARE(q.size(), static_cast<reg>(count));

    // operator[] must linearize the logical FIFO order.
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto& v = q[static_cast<reg>(i)];
        if constexpr (std::is_same_v<T, Blob>) {
            QCOMPARE(v.seq, first + i);
            QCOMPARE(v.tag, 0xA5A5A5A5u);
        } else {
            QCOMPARE(v.seq, first + i);
        }
    }

    // Forward iterators
    {
        std::uint32_t exp = first;
        reg n = 0;
        for (auto it = q.begin(); it != q.end(); ++it) {
            const auto& v = *it;
            if constexpr (std::is_same_v<T, Blob>) {
                QCOMPARE(v.seq, exp);
            } else {
                QCOMPARE(v.seq, exp);
            }
            ++exp;
            ++n;
        }
        QCOMPARE(n, q.size());
        QCOMPARE(exp, first + count);
    }

    // Const iterators
    {
        const Q& cq = q;
        std::uint32_t exp = first;
        reg n = 0;
        for (auto it = cq.cbegin(); it != cq.cend(); ++it) {
            const auto& v = *it;
            QCOMPARE(v.seq, exp);
            ++exp;
            ++n;
        }
        QCOMPARE(n, cq.size());
        QCOMPARE(exp, first + count);
    }

    // Reverse iterators
    {
        std::uint32_t exp = first + count;
        reg n = 0;
        for (auto it = q.rbegin(); it != q.rend(); ++it) {
            const auto& v = *it;
            --exp;
            QCOMPARE(v.seq, exp);
            ++n;
        }
        QCOMPARE(n, q.size());
        QCOMPARE(exp, first);
    }
}

template <class Q>
static void iterators_wraparound_suite(Q& q) {
    using T = typename Q::value_type;
    const reg cap = q.capacity();
    QVERIFY(cap >= 16u);

    // Fill close to cap, pop some, then push again to force wrap.
    const std::uint32_t batch1 = static_cast<std::uint32_t>(cap - 3u);
    fill_seq(q, 1, batch1);
    QCOMPARE(q.size(), static_cast<reg>(batch1));

    const std::uint32_t pop1 = static_cast<std::uint32_t>(cap / 2u);
    q.pop(static_cast<reg>(pop1));

    const std::uint32_t batch2 = static_cast<std::uint32_t>(cap / 2u);
    fill_seq(q, 1000, batch2);

    // Expected logical order: [1+pop1 .. batch1] then [1000 .. 1000+batch2-1]
    const std::uint32_t tail_first = 1u + pop1;
    const std::uint32_t tail_count = batch1 - pop1;

    // Build expected sequence list.
    std::vector<std::uint32_t> expected;
    expected.reserve(static_cast<std::size_t>(tail_count + batch2));
    for (std::uint32_t v = tail_first; v < (1u + batch1); ++v) {
        expected.push_back(v);
    }
    for (std::uint32_t v = 1000u; v < (1000u + batch2); ++v) {
        expected.push_back(v);
    }

    QCOMPARE(q.size(), static_cast<reg>(expected.size()));

    // operator[] must match expected.
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto& v = q[static_cast<reg>(i)];
        QCOMPARE(v.seq, expected[i]);
    }

    // Forward iterators must match expected.
    {
        std::size_t i = 0;
        for (auto it = q.begin(); it != q.end(); ++it, ++i) {
            QCOMPARE(it->seq, expected[i]);
        }
        QCOMPARE(i, expected.size());
    }

    // Reverse iterators must match expected reversed.
    {
        std::size_t i = expected.size();
        for (auto it = q.rbegin(); it != q.rend(); ++it) {
            --i;
            QCOMPARE(it->seq, expected[i]);
        }
        QCOMPARE(i, std::size_t{0});
    }

    // Drain without leaking lifetimes.
    if constexpr (!std::is_trivially_destructible_v<T>) {
        q.clear();
    } else {
        q.consume_all();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void raii_guard_suite(Q& q) {
    using T = typename Q::value_type;

    // -----------------------
    // write_guard
    // -----------------------
    {
        auto g = q.scoped_write();
        QVERIFY(bool(g));

        // Construct but DO NOT publish: destructor must destroy and NOT advance.
        if constexpr (std::is_same_v<T, Tracked>) {
            static_cast<void>(g.emplace(123u));

            // publish_on_destroy_ is armed by emplace(), so cancel it explicitly.
            g.cancel();
            QVERIFY(q.empty());
            QCOMPARE(Tracked::live.load(), 0);
        } else {
            // For trivially destructible types, still validate that cancel doesn't publish.
            static_cast<void>(g.emplace(Blob{123u, 0xA5A5A5A5u}));

            g.cancel();
            QVERIFY(q.empty());
        }
    }

    // Construct and publish via scope-exit.
    {
        auto g = q.scoped_write();
        QVERIFY(bool(g));
        if constexpr (std::is_same_v<T, Tracked>) {
            static_cast<void>(g.emplace(1u));

        } else {
            static_cast<void>(g.emplace(Blob{1u, 0xA5A5A5A5u}));

        }
        // Destructor should publish.
    }
    QCOMPARE(q.size(), reg{1});

    // Construct and commit explicitly.
    {
        auto g = q.scoped_write();
        QVERIFY(bool(g));
        if constexpr (std::is_same_v<T, Tracked>) {
            static_cast<void>(g.emplace(2u));

        } else {
            static_cast<void>(g.emplace(Blob{2u, 0xA5A5A5A5u}));

        }
        g.commit();
        QVERIFY(!bool(g));
    }
    QCOMPARE(q.size(), reg{2});

    // Manual placement + mark_constructed + arm_publish.
    {
        auto g = q.scoped_write();
        QVERIFY(bool(g));
        T* p = g.get();
        QVERIFY(p != nullptr);
        new (p) T{};
        if constexpr (std::is_same_v<T, Tracked>) {
            p->seq = 3u;
        } else {
            p->seq = 3u;
            p->tag = 0xA5A5A5A5u;
        }
        g.mark_constructed();
        g.arm_publish();
        // Destructor should publish.
    }
    QCOMPARE(q.size(), reg{3});

    // -----------------------
    // read_guard
    // -----------------------
    {
        auto rg = q.scoped_read();
        QVERIFY(bool(rg));
        QCOMPARE(rg.get()->seq, 1u);
        // cancel: must NOT pop
        rg.cancel();
    }
    QCOMPARE(q.size(), reg{3});

    {
        auto rg = q.scoped_read();
        QVERIFY(bool(rg));
        QCOMPARE(rg.get()->seq, 1u);
        // commit: pop now
        rg.commit();
        QVERIFY(!bool(rg));
    }
    QCOMPARE(q.size(), reg{2});

    // Destructor auto-pop
    {
        auto rg = q.scoped_read();
        QVERIFY(bool(rg));
        QCOMPARE(rg.get()->seq, 2u);
    }
    QCOMPARE(q.size(), reg{1});

    // Drain remaining.
    q.pop();
    QVERIFY(q.empty());
}

template <class Q>
static void bulk_regions_max_count_suite(Q& q) {
    using T = typename Q::value_type;

    const reg cap = q.capacity();
    QVERIFY(cap >= 16u);

    // Ask for a small max_count even when free is larger.
    const reg want = 7u;
    auto wr = q.claim_write(want);
    QVERIFY(!wr.empty());
    QCOMPARE(wr.total, want);

    // Construct exactly wr.total objects.
    std::uint32_t seq = 10;
    if (wr.first.count) {
        T* p = wr.first.ptr_uninit();
        for (reg i = 0; i < wr.first.count; ++i) {
            if constexpr (std::is_same_v<T, Blob>) {
                new (p + i) T{seq++, 0xA5A5A5A5u};
            } else {
                new (p + i) T{seq++};
            }
        }
    }
    if (wr.second.count) {
        T* p = wr.second.ptr_uninit();
        for (reg i = 0; i < wr.second.count; ++i) {
            if constexpr (std::is_same_v<T, Blob>) {
                new (p + i) T{seq++, 0xA5A5A5A5u};
            } else {
                new (p + i) T{seq++};
            }
        }
    }

    // publish(n) must advance by exactly want.
    q.publish(wr.total);
    QCOMPARE(q.size(), want);

    // claim_read with smaller max_count must limit.
    auto rr = q.claim_read(3u);
    QVERIFY(!rr.empty());
    QCOMPARE(rr.total, reg{3});

    // pop(3) must destroy exactly 3 objects for non-trivial.
    q.pop(rr.total);
    QCOMPARE(q.size(), reg{want - 3u});

    // try_publish(n) must fail if not enough free.
    const reg free_now = q.free();
    QVERIFY(!q.try_publish(static_cast<reg>(free_now + 1u)));

    q.clear();
    QVERIFY(q.empty());
}


// -----------------------------------------------------------------------------------------
// More paranoid coverage: reserve/resize edge cases, guard move semantics, claim_read limits.
// -----------------------------------------------------------------------------------------

static void reserve_resize_edge_cases_suite() {
    using Q = spsc::queue<Blob, 0, spsc::policy::P>;
    Q q;

    // reserve() and resize() on invalid queue
    QVERIFY(!q.is_valid());
    QVERIFY(q.reserve(2));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 2u);

    const reg cap_min = q.capacity();

    // reserve below current must be no-op
    QVERIFY(q.reserve(1));
    QCOMPARE(q.capacity(), cap_min);

    // resize below current must be no-op
    QVERIFY(q.resize(2));
    QCOMPARE(q.capacity(), cap_min);

    // Ensure enough room for subsequent content-preservation checks.
    QVERIFY(q.reserve(16));
    QVERIFY(q.capacity() >= 16u);
    const reg cap0 = q.capacity();

    // push some items, then reserve larger
    fill_seq(q, 1, 10);
    const reg sz_before = q.size();
    QVERIFY(q.reserve(cap0 * 4u));
    QVERIFY(q.capacity() >= cap0 * 4u);
    QCOMPARE(q.size(), sz_before);
    check_fifo_exact(q, 1, static_cast<std::uint32_t>(sz_before));

    // resize(0) must destroy and become invalid
    QVERIFY(q.resize(0));
    QVERIFY(!q.is_valid());
    QCOMPARE(q.capacity(), reg{0});
    QVERIFY(q.empty());

    // reserve again should allocate
    QVERIFY(q.reserve(64));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 64u);

    q.destroy();
    QVERIFY(!q.is_valid());
}


static void dynamic_capacity_sweep_suite() {
    using Q = spsc::queue<Blob, 0, spsc::policy::P>;
    Q q;

    // Sweep a bunch of capacities, always expecting pow2 and >=2.
    for (reg req = 0; req < 2048; req += 7) {
        const reg want = (req == 0u) ? 0u : req;
        QVERIFY(q.resize(want));
        if (want == 0u) {
            QVERIFY(!q.is_valid());
            QCOMPARE(q.capacity(), reg{0});
            continue;
        }
        QVERIFY(q.is_valid());
        const reg cap = q.capacity();
        QVERIFY(cap >= 2u);
        QVERIFY((cap & (cap - 1u)) == 0u); // power of two

        // Basic operation sanity.
        q.clear();
        fill_seq(q, 1, 5);
        check_fifo_exact(q, 1, 5);
        QVERIFY(q.empty());
    }

    q.destroy();
}

template <class Q>
static void bulk_read_max_count_suite(Q& q) {
    using T = typename Q::value_type;

    // Fill some data
    fill_seq(q, 1, 30);
    QCOMPARE(q.size(), reg{30});

    // claim_read with max_count must cap regions
    {
        auto rr = q.claim_read(7);
        QCOMPARE(rr.total, reg{7});
        QVERIFY(rr.first.count > 0u);
        // Validate the claimed values without popping.
        for (reg i = 0; i < rr.first.count; ++i) {
            QCOMPARE(rr.first.ptr[static_cast<std::size_t>(i)].seq, 1u + static_cast<std::uint32_t>(i));
        }
        if (rr.second.count) {
            for (reg i = 0; i < rr.second.count; ++i) {
                QCOMPARE(rr.second.ptr[static_cast<std::size_t>(i)].seq,
                         1u + static_cast<std::uint32_t>(rr.first.count + i));
            }
        }

        // Pop exactly what we claimed.
        q.pop(rr.total);
    }

    // Remaining size
    QCOMPARE(q.size(), reg{23});

    // drain and verify order continues
    check_fifo_exact(q, 8, 23);
    q.destroy();

    // For non-trivial, destruction is handled by pop/destroy.
    if constexpr (!std::is_trivially_destructible_v<T>) {
        // no-op
        ;
    }
}

template <class Q>
static void guard_move_semantics_suite(Q& q) {
    using T = typename Q::value_type;
    static_assert(std::is_same_v<T, Tracked>, "guard_move_semantics_suite expects Tracked");

    // Ensure queue is empty
    q.clear();
    QVERIFY(q.empty());

    // Move a write_guard: only the last owner should publish.
    {
        auto g1 = q.scoped_write();
        QVERIFY(g1);
        QVERIFY(g1.get() != nullptr);
        new (g1.get()) T(1);
        g1.mark_constructed();
        g1.arm_publish();

        auto g2 = std::move(g1);
        QVERIFY(g2);
        // g1 is moved-from and must not publish/destroy.
    }
    QCOMPARE(q.size(), reg{1});
    QCOMPARE(q.front().seq, 1u);

    // Move a read_guard: only the last owner should pop.
    {
        auto r1 = q.scoped_read();
        QVERIFY(r1);
        QCOMPARE(r1->seq, 1u);
        auto r2 = std::move(r1);
        QVERIFY(r2);
        // r2 destructor will pop.
    }
    QVERIFY(q.empty());
}


// ----------------------------------------
// Snapshot consume + invalid snapshot cases
// ----------------------------------------

template <class Q>
static void snapshot_consume_suite(Q& q) {
    using T = typename Q::value_type;

    q.clear();
    QVERIFY(q.empty());

    fill_seq(q, 1, 12);
    QCOMPARE(q.size(), reg{12});

    auto snap = q.make_snapshot();
    QCOMPARE(static_cast<reg>(snap.size()), reg{12});

    // consume(snapshot) must pop exactly snapshot size.
    q.consume(snap);
    QVERIFY(q.empty());

    // Refill and take a const snapshot.
    fill_seq(q, 100, 5);
    const Q& cq = q;
    auto csnap = cq.make_snapshot();
    QCOMPARE(static_cast<reg>(csnap.size()), reg{5});

    // Iterating const snapshot
    std::uint32_t exp = 100;
    for (auto it = csnap.begin(); it != csnap.end(); ++it) {
        QCOMPARE(it->seq, exp++);
    }

    q.clear();
    QVERIFY(q.empty());

    if constexpr (!std::is_trivially_destructible_v<T>) {
        // Ensure we didn't leak.
        ;
    }
}

static void snapshot_invalid_queue_suite() {
    using Q = spsc::queue<Blob, 0, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());

    auto s = q.make_snapshot();
    QCOMPARE(static_cast<reg>(s.size()), reg{0});
    QVERIFY(s.begin() == s.end());

    // try_consume on invalid queue must fail.
    QVERIFY(!q.try_consume(s));
}

// ----------------------------------------
// Dynamic move semantics contract
// ----------------------------------------

static void dynamic_move_contract_suite() {
    using Q = spsc::queue<Tracked, 0, spsc::policy::P>;
    tracked_reset();

    Q a;
    QVERIFY(a.resize(64));
    fill_seq(a, 1, 20);

    Q b{std::move(a)};

    // Moved-from dynamic queue becomes invalid (storage_ == nullptr).
    QVERIFY(!a.is_valid());
    QVERIFY(b.is_valid());
    QCOMPARE(b.size(), reg{20});

    // Verify FIFO content survived (non-destructive).
    check_iterators_and_indexing(b, 1u, 20u);

    // Move-assign into a new queue.
    Q c;
    QVERIFY(c.resize(32));
    fill_seq(c, 1000, 5);

    c = std::move(b);
    QVERIFY(!b.is_valid());
    QVERIFY(c.is_valid());

    // c should now contain the 20 elements (not the old 5).
    QCOMPARE(c.size(), reg{20});
    check_fifo_exact(c, 1, 20);

    c.destroy();
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

// ----------------------------------------
// raw_bytes() / data() sanity (if span enabled)
// ----------------------------------------

template <class Q>
static void raw_bytes_suite(Q& q) {
    QVERIFY(q.is_valid());
    QVERIFY(q.data() != nullptr);

#if SPSC_HAS_SPAN
    auto bytes = q.raw_bytes();
    QVERIFY(bytes.data() != nullptr);
    QCOMPARE(bytes.size(), static_cast<std::size_t>(q.capacity()) * sizeof(typename Q::value_type));

    auto cbytes = const_cast<const Q&>(q).raw_bytes();
    QCOMPARE(cbytes.size(), bytes.size());
#endif
}

template <class Policy>
static void run_threaded_bulk_regions_suite(const char* name) {
    using Q = spsc::queue<Blob, 0, Policy>;

    Q q;
    QVERIFY(q.resize(1024));

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> abort{false};

    constexpr std::uint32_t kMaxBatch = 8;

    std::thread prod([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t seq = 1;
        std::uint32_t rng = 0x1234567u;

        auto next_u32 = [&] {
            // xorshift32
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };

        while (seq <= static_cast<std::uint32_t>(kThreadIters) && !abort.load(std::memory_order_relaxed)) {
            const reg want = static_cast<reg>((next_u32() % kMaxBatch) + 1u);
            auto wr = q.claim_write(want);
            if (wr.total == 0u) {
                std::this_thread::yield();
                continue;
            }

            // Construct wr.total blobs.
            reg constructed = 0u;
            if (wr.first.count) {
                Blob* p = wr.first.ptr_uninit();
                for (reg i = 0; i < wr.first.count && seq <= static_cast<std::uint32_t>(kThreadIters); ++i) {
                    new (p + i) Blob{seq++, 0xA5A5A5A5u};
                    ++constructed;
                }
            }
            if (wr.second.count) {
                Blob* p = wr.second.ptr_uninit();
                for (reg i = 0; i < wr.second.count && seq <= static_cast<std::uint32_t>(kThreadIters); ++i) {
                    new (p + i) Blob{seq++, 0xA5A5A5A5u};
                    ++constructed;
                }
            }

            if (constructed == 0u) {
                // Nothing constructed (should only happen at tail end).
                continue;
            }

            // Publish exactly what we constructed.
            q.publish(constructed);
        }

        done.store(true, std::memory_order_release);
    });

    std::thread cons([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t expect = 1;
        auto t0 = std::chrono::steady_clock::now();

        while (expect <= static_cast<std::uint32_t>(kThreadIters) && !abort.load(std::memory_order_relaxed)) {
            auto rr = q.claim_read(kMaxBatch);
            if (rr.total == 0u) {
                if (done.load(std::memory_order_acquire) && q.empty()) {
                    abort.store(true, std::memory_order_relaxed);
                    break;
                }
                const auto now = std::chrono::steady_clock::now();
                const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - t0).count();
                if (ms > kThreadTimeoutMs) {
                    abort.store(true, std::memory_order_relaxed);
                    break;
                }
                std::this_thread::yield();
                continue;
            }

            auto check_span = [&](Blob* p, reg n) {
                for (reg i = 0; i < n; ++i) {
                    if (p[i].seq != expect) {
                        abort.store(true, std::memory_order_relaxed);
                        return;
                    }
                    if (p[i].tag != 0xA5A5A5A5u) {
                        abort.store(true, std::memory_order_relaxed);
                        return;
                    }
                    ++expect;
                }
            };

            if (rr.first.count) {
                check_span(rr.first.ptr, rr.first.count);
            }
            if (rr.second.count) {
                check_span(rr.second.ptr, rr.second.count);
            }

            q.pop(rr.total);
        }
    });

    start.store(true, std::memory_order_release);
    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), name);

    q.destroy();
}

class tst_queue_api_paranoid : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Compile-time smoke for representative instantiations.
        api_smoke_compile<spsc::queue<Blob, kSmallCap, spsc::policy::P>>();
        api_smoke_compile<spsc::queue<Tracked, 0, spsc::policy::A<>>>();
    }

    void static_plain_P()    { run_static_suite<spsc::policy::P>(); }
    void static_volatile_V() { run_static_suite<spsc::policy::V>(); }
    void static_atomic_A()   { run_static_suite<spsc::policy::A<>>(); }
    void static_cached_CA()  { run_static_suite<spsc::policy::CA<>>(); }

    void dynamic_plain_P()    { run_dynamic_suite<spsc::policy::P>(); }
    void dynamic_volatile_V() { run_dynamic_suite<spsc::policy::V>(); }
    void dynamic_atomic_A()   { run_dynamic_suite<spsc::policy::A<>>(); }
    void dynamic_cached_CA()  { run_dynamic_suite<spsc::policy::CA<>>(); }

    void deterministic_interleaving() {
        spsc::queue<Tracked, 64, spsc::policy::P> q;
        tracked_reset();
        ::deterministic_interleaving(q);
        q.destroy();
        QCOMPARE(Tracked::live.load(), 0);
    }

    void threaded_atomic_A()  { run_threaded_suite<spsc::policy::A<>>("threaded_queue_atomic"); }
    void threaded_cached_CA() { run_threaded_suite<spsc::policy::CA<>>("threaded_queue_cached"); }

    void allocator_accounting() {
        using Q = spsc::queue<Tracked, 0, spsc::policy::P, CountingAlignedAlloc<std::byte, alignof(Tracked)>>;
        tracked_reset();
        AllocStats::reset();

        {
            Q q;
            QVERIFY(!q.is_valid());
            QVERIFY(q.resize(64));
            QVERIFY(q.is_valid());
            QCOMPARE(AllocStats::allocs.load(), std::size_t{1});
            QVERIFY(AllocStats::bytes_live.load() >= 64u * sizeof(Tracked));

            fill_seq(q, 1, 10);
            QVERIFY(q.resize(256));
            QVERIFY(q.capacity() >= 256u);
            // One more alloc, and one dealloc for the old buffer.
            QVERIFY(AllocStats::allocs.load() >= std::size_t{2});
            QVERIFY(AllocStats::deallocs.load() >= std::size_t{1});

            q.destroy();
            QVERIFY(!q.is_valid());
        }

        // All memory released.
        QCOMPARE(AllocStats::bytes_live.load(), std::size_t{0});
        QCOMPARE(AllocStats::allocs.load(), AllocStats::deallocs.load());
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void invalid_inputs() {
        using Q = spsc::queue<Blob, 0, spsc::policy::P>;
        Q q;
        QVERIFY(!q.is_valid());
        QCOMPARE(q.capacity(), reg{0});
        QVERIFY(q.empty());
        QVERIFY(q.full());
        QVERIFY(q.try_front() == nullptr);
        QVERIFY(!q.try_pop());
        QVERIFY(q.claim_read().empty());
        QVERIFY(q.claim_write().empty());
        q.clear();
        q.destroy();
        QVERIFY(!q.is_valid());

        // resize(1) must clamp to at least 2.
        QVERIFY(q.resize(1u));
        QVERIFY(q.is_valid());
        QVERIFY(q.capacity() >= 2u);
        q.destroy();
    }
    void dynamic_capacity_sweep() { dynamic_capacity_sweep_suite(); }


    void move_swap_stress() {
        using Q = spsc::queue<Tracked, 0, spsc::policy::P>;
        tracked_reset();

        Q a;
        Q b;
        QVERIFY(a.resize(128));
        QVERIFY(b.resize(128));

        std::uint32_t seq = 1;

        for (int i = 0; i < kSwapIters; ++i) {
            if (!a.full()) {
                static_cast<void>(a.try_push(Tracked{seq++}));
            }
            if (!b.full()) {
                static_cast<void>(b.try_push(Tracked{seq++}));
            }

            if ((i & 7) == 0) {
                a.swap(b);
            } else if ((i & 31) == 0) {
                Q tmp{std::move(a)};
                a = std::move(b);
                b = std::move(tmp);
            }

            QVERIFY(a.is_valid());
            QVERIFY(b.is_valid());

            // Random drain a bit.
            if ((i & 3) == 0) {
                if (!a.empty()) {
                    a.pop();
                }
                if (!b.empty()) {
                    b.pop();
                }
            }
        }

        a.destroy();
        b.destroy();
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void state_machine_fuzz_sweep() {
        run_state_machine_fuzz<spsc::policy::P>(false);
        run_state_machine_fuzz<spsc::policy::A<>>(false);
        run_state_machine_fuzz<spsc::policy::P>(true);
        run_state_machine_fuzz<spsc::policy::CA<>>(true);
    }

    void resize_migration_order() {
        using Q = spsc::queue<Tracked, 0, spsc::policy::P>;
        tracked_reset();

        Q q;
        QVERIFY(q.resize(32));

        fill_seq(q, 1, 25);
        QVERIFY(q.resize(256));
        QCOMPARE(q.size(), reg{25});
        check_fifo_exact(q, 1, 25);

        q.destroy();
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void snapshot_try_consume_contract() {
        using Q = spsc::queue<Tracked, 64, spsc::policy::P>;
        tracked_reset();

        Q q;
        fill_seq(q, 1, 10);
        auto snap = q.make_snapshot();

        // If consumer advanced, try_consume must fail.
        q.pop();
        QVERIFY(!q.try_consume(snap));

        // Fresh snapshot works.
        auto snap2 = q.make_snapshot();
        QVERIFY(q.try_consume(snap2));
        QVERIFY(q.empty());

        q.destroy();
        QCOMPARE(Tracked::live.load(), 0);
    }

    void bulk_regions_wraparound() {
        using Q = spsc::queue<Blob, 64, spsc::policy::P>;
        Q q;
        QVERIFY(q.is_valid());

        // Force wrap: push 50, pop 40, then region-write that continues the sequence.
        fill_seq(q, 1, 50);
        q.pop(40);

        std::uint32_t next = 51;
        placement_write_region(q, next, 40);

        // Now drain and verify monotonic.
        std::uint32_t exp = 41;
        while (!q.empty()) {
            check_read_regions(q, exp, 64);
        }

        // We drained: 41..50 (10 items) + 51..90 (40 items) => next expected is 91.
        QCOMPARE(exp, 91u);
        q.destroy();
    }



    void raii_guards_static() {
        tracked_reset();
        {
            spsc::queue<Tracked, 64, spsc::policy::P> q;
            QVERIFY(q.is_valid());
            raii_guard_suite(q);
            q.destroy();
        }
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void raii_guards_dynamic() {
        tracked_reset();
        {
            spsc::queue<Tracked, 0, spsc::policy::P> q;
            QVERIFY(q.resize(64));
            raii_guard_suite(q);
            q.destroy();
        }
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void iterators_and_indexing_static() {
        spsc::queue<Blob, 64, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        fill_seq(q, 1, 20);
        check_iterators_and_indexing(q, 1, 20);
        q.destroy();
    }

    void iterators_wraparound_static() {
        spsc::queue<Blob, 64, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        iterators_wraparound_suite(q);
        q.destroy();
    }

    void snapshot_iteration_contract() {
        using Q = spsc::queue<Blob, 64, spsc::policy::P>;
        Q q;
        QVERIFY(q.is_valid());
        fill_seq(q, 1, 30);

        auto s = q.make_snapshot();
        // Snapshot must view exactly the items present at capture time.
        reg n = 0;
        std::uint32_t exp = 1;
        for (auto it = s.begin(); it != s.end(); ++it) {
            QCOMPARE(it->seq, exp++);
            ++n;
        }
        QCOMPARE(n, reg{30});

        // Mutating the queue after snapshot must not mutate the snapshot view.
        q.pop(10);
        fill_seq(q, 1000, 10);

        n = 0;
        exp = 1;
        for (auto it = s.begin(); it != s.end(); ++it) {
            QCOMPARE(it->seq, exp++);
            ++n;
        }
        QCOMPARE(n, reg{30});

        // try_consume must fail because consumer advanced.
        QVERIFY(!q.try_consume(s));

        q.destroy();
    }

    void bulk_regions_max_count_static() {
        using Q = spsc::queue<Blob, 64, spsc::policy::P>;
        Q q;
        QVERIFY(q.is_valid());
        bulk_regions_max_count_suite(q);
        q.destroy();
    }


    void bulk_read_max_count_static() {
        spsc::queue<Blob, 64, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        bulk_read_max_count_suite(q);
    }

    void guard_move_semantics_static() {
        tracked_reset();
        spsc::queue<Tracked, 64, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        guard_move_semantics_suite(q);
        q.destroy();
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void reserve_resize_edge_cases_dynamic() {
        reserve_resize_edge_cases_suite();
    }

    void snapshot_consume_contract() {
        spsc::queue<Blob, 64, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        snapshot_consume_suite(q);
        q.destroy();
    }

    void snapshot_invalid_queue() {
        snapshot_invalid_queue_suite();
    }

    void dynamic_move_contract() {
        dynamic_move_contract_suite();
    }

    void raw_bytes_contract() {
        {
            spsc::queue<Blob, 64, spsc::policy::P> q;
            raw_bytes_suite(q);
            q.destroy();
        }
        {
            spsc::queue<Blob, 0, spsc::policy::P> q;
            QVERIFY(q.resize(128));
            raw_bytes_suite(q);
            q.destroy();
        }
    }

    void consume_all_contract() {
        // Trivial path
        {
            spsc::queue<Blob, 64, spsc::policy::P> q;
            fill_seq(q, 1, 40);
            QVERIFY(!q.empty());
            q.consume_all();
            QVERIFY(q.empty());
            q.destroy();
        }

        // Non-trivial path must destroy elements.
        tracked_reset();
        {
            spsc::queue<Tracked, 64, spsc::policy::P> q;
            fill_seq(q, 1, 40);
            QVERIFY(Tracked::live.load() > 0);
            q.consume_all();
            QVERIFY(q.empty());
            q.destroy();
        }
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void threaded_bulk_regions_atomic_A()  { run_threaded_bulk_regions_suite<spsc::policy::A<>>("threaded_queue_bulk_atomic"); }
    void threaded_bulk_regions_cached_CA() { run_threaded_bulk_regions_suite<spsc::policy::CA<>>("threaded_queue_bulk_cached"); }

    void alignment_sweep() {
        {
            spsc::queue<Aligned64, 64, spsc::policy::P> q;
            QVERIFY(q.is_valid());
            auto addr = reinterpret_cast<std::uintptr_t>(q.data());
            QVERIFY((addr % alignof(Aligned64)) == 0u);
        }
        {
            spsc::queue<Aligned64, 0, spsc::policy::P> q;
            QVERIFY(q.resize(128));
            auto addr = reinterpret_cast<std::uintptr_t>(q.data());
            QVERIFY((addr % alignof(Aligned64)) == 0u);
            q.destroy();
        }
    }


    void stress_cached_ca_transitions() {
        using QS = spsc::queue<std::uint32_t, 64u, spsc::policy::CA<>>;
        using QD = spsc::queue<std::uint32_t, 0u, spsc::policy::CA<>>;

        // Swap: non-empty <-> non-empty, then immediately: push-to-full + pop-to-empty.
        {
            QS a;
            QS b;

            for (std::uint32_t i = 1; i <= 17; ++i) {
                QVERIFY(a.try_push(i));
            }
            for (std::uint32_t i = 100; i < 121; ++i) {
                QVERIFY(b.try_push(i));
            }

            a.swap(b);

            QCOMPARE(a.front(), std::uint32_t{100});
            QCOMPARE(b.front(), std::uint32_t{1});

            full_empty_cycle_u32(a, 1000u);
            full_empty_cycle_u32(b, 2000u);
        }

        // Move ctor: source non-empty.
        {
            QS src;
            for (std::uint32_t i = 1; i <= 33; ++i) {
                QVERIFY(src.try_push(i));
            }

            QS dst(std::move(src));
            QCOMPARE(dst.size(), reg{33});

            full_empty_cycle_u32(dst, 3000u);
        }

        // Move assign into non-empty.
        {
            QS a;
            QS b;

            for (std::uint32_t i = 10; i < 30; ++i) {
                QVERIFY(a.try_push(i));
            }
            for (std::uint32_t i = 200; i < 210; ++i) {
                QVERIFY(b.try_push(i));
            }

            a = std::move(b);

            QCOMPARE(a.front(), std::uint32_t{200});
            full_empty_cycle_u32(a, 4000u);
        }

        // Resize migration: grow then shrink, always followed by a full->empty cycle.
        {
            QD q;

            QVERIFY(q.resize(64));
            for (std::uint32_t i = 1; i <= 48; ++i) {
                QVERIFY(q.try_push(i));
            }

            QVERIFY(q.resize(128));
            full_empty_cycle_u32(q, 5000u);

            QVERIFY(q.resize(64));
            for (std::uint32_t i = 1; i <= 10; ++i) {
                QVERIFY(q.try_push(6000u + i));
            }

            QVERIFY(q.resize(16));
            full_empty_cycle_u32(q, 7000u);
        }
    }

    void death_tests_debug_only() {
#if !defined(NDEBUG)
        auto expect_death = [&](const char* mode) {
            QProcess p;
            p.setProgram(QCoreApplication::applicationFilePath());
            p.setArguments(QStringList{});

            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("SPSC_QUEUE_DEATH", QString::fromLatin1(mode));
            p.setProcessEnvironment(env);

            p.start();
            QVERIFY2(p.waitForStarted(1500), "Death child failed to start.");

            if (!p.waitForFinished(8000)) {
                p.kill();
                QVERIFY2(false, "Death child did not finish (possible crash dialog)." );
            }

            const int code = p.exitCode();
            QVERIFY2(code != 0, "Expected non-zero exit code from death child.");
        };

        expect_death("pop_empty");
        expect_death("publish_full");
        expect_death("guard_commit_without_construct");
        expect_death("double_emplace");
#else
        QSKIP("Death tests are debug-only (assertions disabled)." );
#endif
    }
    void lifecycle_traced() {
        tracked_reset();
        {
            spsc::queue<Tracked, 64, spsc::policy::P> q;
            for (int i = 0; i < 200; ++i) {
                if (!q.full()) {
                    QVERIFY(q.try_push(Tracked{static_cast<std::uint32_t>(i)}));
                }
                if ((i & 1) == 0 && !q.empty()) {
                    q.pop();
                }
            }
            q.clear();
            QVERIFY(q.empty());
        }
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }
};

} // namespace


int run_tst_queue_api_paranoid(int argc, char** argv) {
    tst_queue_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "queue_test.moc"
