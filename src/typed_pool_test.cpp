
/*
 * typed_pool_test.cpp
 *
 * Paranoid API/contract test for spsc::typed_pool.
 *
 * Goals:
 *  - Exercise every public method in typed_pool.hpp at least once.
 *  - Cover policies (P/V/A/CA) across static and dynamic variants.
 *  - Validate invariants aggressively after each operation.
 *  - Stress bulk regions, including wrap-around (split first+second).
 *  - Probe alignment behavior and allocator accounting.
 *  - Include regression checks for shadow-cache correctness (swap/move/resize paths).
 */

#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <deque>
#include <new>      // std::launder
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <unordered_map>
#include <unordered_set>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include "typed_pool.hpp"

namespace spsc_typed_pool_death_detail {

#if !defined(NDEBUG)

static constexpr int kDeathExitCode = 0xAC;

static void sigabrt_handler_(int) noexcept {
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode) {
    // Convert "assert -> abort" into a deterministic exit code so QProcess won't hang on Windows dialogs.
    ::signal(SIGABRT, &sigabrt_handler_);

    using Q = spsc::typed_pool<std::uint32_t, 16u, spsc::policy::P>;

    if (std::strcmp(mode, "pop_empty") == 0) {
        Q q;
        q.pop(); // Must assert: pop on empty.
    } else if (std::strcmp(mode, "front_empty") == 0) {
        Q q;
        (void)q.front(); // Must assert: front on empty.
    } else if (std::strcmp(mode, "publish_full") == 0) {
        Q q;
        for (std::uint32_t i = 0; i < 16u; ++i) {
            const bool ok = q.try_emplace(i);
            if (!ok) {
                std::_Exit(0xEE);
            }
        }
        q.publish(); // Must assert: publish while full (no claimed slot).
    } else if (std::strcmp(mode, "claim_full") == 0) {
        Q q;
        for (std::uint32_t i = 0; i < 16u; ++i) {
            const bool ok = q.try_emplace(i);
            if (!ok) {
                std::_Exit(0xEF);
            }
        }
        (void)q.claim(); // Must assert: claim while full.
    } else if (std::strcmp(mode, "double_emplace") == 0) {
        Q q;
        auto g = q.scoped_write();
        (void)g.emplace(1u);
        (void)g.emplace(2u); // Must assert: double construction.
    } else if (std::strcmp(mode, "commit_unconstructed") == 0) {
        Q q;
        auto g = q.scoped_write();
        g.commit(); // Must assert: publishing an unconstructed slot.
    } else if (std::strcmp(mode, "bulk_double_emplace_next") == 0) {
        Q q;
        auto g = q.scoped_write(1u);
        (void)g.emplace_next(1u);
        (void)g.emplace_next(2u); // Must assert: emplace beyond claimed().
    } else if (std::strcmp(mode, "bulk_arm_publish_unconstructed") == 0) {
        Q q;
        auto g = q.scoped_write(2u);
        g.arm_publish(); // Must assert: no constructed elements.
    } else if (std::strcmp(mode, "consume_foreign_snapshot") == 0) {
        Q q1;
        Q q2;
        if (!q1.try_emplace(1u)) {
            std::_Exit(0xE1);
        }
        if (!q2.try_emplace(2u)) {
            std::_Exit(0xE2);
        }
        const auto snap = q2.make_snapshot();
        q1.consume(snap); // Must assert: foreign snapshot identity.
    } else if (std::strcmp(mode, "pop_n_too_many") == 0) {
        Q q;
        if (!q.try_emplace(1u)) {
            std::_Exit(0xE3);
        }
        q.pop(2u); // Must assert: can_read(2) is false.
    } else {
        std::_Exit(0xF1);
    }

    // If we reach this point, assertions did not fire.
    std::_Exit(0xF0);
}

struct Runner_ {
    Runner_() {
        const char* mode = std::getenv("SPSC_TYPED_POOL_DEATH");
        if (mode && *mode) {
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

#endif // !defined(NDEBUG)

} // namespace spsc_typed_pool_death_detail

namespace {

constexpr reg kSmallCap = 16u;
constexpr reg kMedCap   = 256u;

#if defined(NDEBUG)
constexpr int kFuzzIters       = 35'000;
constexpr int kSwapIters       = 30'000;
constexpr int kThreadIters     = 120'000;
constexpr int kThreadTimeoutMs = 6000;
#else
constexpr int kFuzzIters       = 6'000;
constexpr int kSwapIters       = 6'000;
constexpr int kThreadIters     = 30'000;
constexpr int kThreadTimeoutMs = 15'000;
#endif

static inline bool is_pow2(reg x) noexcept {
    return x != 0u && ((x & (x - 1u)) == 0u);
}

// -------------------------
// Compile-time API smoke
// -------------------------

template <class Q>
static void api_smoke_compile() {
    using value_type = typename Q::value_type;
    using obj_type   = typename Q::object_type;

    static_assert(std::is_same_v<decltype(std::declval<Q&>().is_valid()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().free()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().empty()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().full()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().can_write(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().can_read(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().buffer_size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().buffer_align()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().get_allocator()), typename Q::base_allocator_type>);

    static_assert(std::is_pointer_v<value_type>);
    static_assert(std::is_same_v<std::remove_pointer_t<value_type>, obj_type>);

    // Producer
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_emplace(std::declval<std::uint32_t>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().emplace(std::declval<std::uint32_t>())), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_push(std::declval<obj_type>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().push(std::declval<obj_type>())), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_claim()), value_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim()), value_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish(reg{1})), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write(::spsc::unsafe)), typename Q::regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write(::spsc::unsafe, reg{1})), typename Q::regions>);

    // Consumer
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_front()), obj_type*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().try_front()), const obj_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().front()), obj_type*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().front()), const obj_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop(reg{1})), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read(::spsc::unsafe)), typename Q::regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read(::spsc::unsafe, reg{1})), typename Q::regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().operator[](reg{0})), obj_type*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().operator[](reg{0})), const obj_type*>);

    // Snapshots
    static_assert(std::is_same_v<decltype(std::declval<Q&>().make_snapshot()), typename Q::snapshot>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().make_snapshot()), typename Q::const_snapshot>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().consume(std::declval<const typename Q::snapshot&>())), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_consume(std::declval<const typename Q::snapshot&>())), bool>);

    // RAII
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_write()), typename Q::write_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_read()), typename Q::read_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_write(reg{1})), typename Q::bulk_write_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_read(reg{1})), typename Q::bulk_read_guard>);

    // Const API should compile.
    (void)sizeof(decltype(std::declval<const Q&>().data()));
}

static void api_compile_smoke_all() {
    using QP  = spsc::typed_pool<std::uint32_t, 16u, spsc::policy::P>;
    using QV  = spsc::typed_pool<std::uint32_t, 16u, spsc::policy::V>;
    using QA  = spsc::typed_pool<std::uint32_t, 16u, spsc::policy::A<>>;
    using QCA = spsc::typed_pool<std::uint32_t, 16u, spsc::policy::CA<>>;

    api_smoke_compile<QP>();
    api_smoke_compile<QV>();
    api_smoke_compile<QA>();
    api_smoke_compile<QCA>();
}

// -------------------------
// Test payloads
// -------------------------

struct Blob {
    std::uint32_t seq{0};
    std::uint32_t tag{0xA5A5A5A5u};

    Blob() = default;
    explicit Blob(std::uint32_t s) : seq(s) {}
    Blob(std::uint32_t s, std::uint32_t t) : seq(s), tag(t) {}
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
        if (!p) {
            return;
        }
        const std::size_t bytes = n * sizeof(T);
        AllocStats::on_dealloc(bytes);
        // MinGW does not provide a 4-arg sized+aligned+nothrow delete.
        ::operator delete(p, std::align_val_t(Align));
    }

    template <class U>
    bool operator==(const CountingAlignedAlloc<U, Align>&) const noexcept { return true; }

    template <class U>
    bool operator!=(const CountingAlignedAlloc<U, Align>&) const noexcept { return false; }
};

// -------------------------
// Helpers
// -------------------------

template <class Q>
static void expect_invariants(const Q& q) {
    if (!q.is_valid()) {
        QCOMPARE(q.capacity(), reg{0});
        QCOMPARE(q.size(), reg{0});
        QCOMPARE(q.free(), reg{0});
        QVERIFY(q.empty());
        QVERIFY(q.full()); // invalid treated as full, so producer can't write
        return;
    }

    QVERIFY(q.capacity() >= reg{2});
    QVERIFY(is_pow2(q.capacity()));
    QVERIFY(q.size() <= q.capacity());
    QCOMPARE(q.free(), static_cast<reg>(q.capacity() - q.size()));
    QCOMPARE(q.empty(), q.size() == 0u);
    QCOMPARE(q.full(), q.size() == q.capacity());

    QCOMPARE(q.buffer_size(), static_cast<reg>(sizeof(typename Q::object_type)));
    QCOMPARE(q.buffer_align(), static_cast<reg>(alignof(typename Q::object_type)));

    QVERIFY(q.data() != nullptr);
}

template <class Q>
static void fill_seq(Q& q, std::uint32_t base, reg count) {
    QVERIFY2(q.is_valid(), "fill_seq(): queue invalid");
    QVERIFY2(q.free() >= count, "fill_seq(): not enough free space");

    for (reg i = 0; i < count; ++i) {
        const std::uint32_t v = base + static_cast<std::uint32_t>(i);
        const bool ok = q.try_emplace(v);
        QVERIFY2(ok, "fill_seq(): try_emplace failed unexpectedly");
    }
    expect_invariants(q);
}

template <class Q>
static void check_fifo_exact(Q& q, std::uint32_t base, reg count) {
    QVERIFY(q.is_valid());
    QCOMPARE(q.size(), count);
    for (reg i = 0; i < count; ++i) {
        auto* p = q.try_front();
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, base + static_cast<std::uint32_t>(i));
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void check_iterators_and_indexing(Q& q, std::uint32_t base, reg count) {
    QVERIFY(q.is_valid());
    QCOMPARE(q.size(), count);

    // operator[]
    for (reg i = 0; i < count; ++i) {
        auto* p = q[static_cast<typename Q::size_type>(i)];
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, base + static_cast<std::uint32_t>(i));
    }

    // iterators yield raw pointers (slots). Launder before reading.
    reg i = 0;
    for (auto it = q.begin(); it != q.end(); ++it, ++i) {
        auto* p = std::launder(*it);
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, base + static_cast<std::uint32_t>(i));
    }
    QCOMPARE(i, count);

    // reverse iterators
    {
        reg n = 0u;
        std::uint32_t exp = base + static_cast<std::uint32_t>(count);
        for (auto it = q.rbegin(); it != q.rend(); ++it) {
            auto* p = std::launder(*it);
            QVERIFY(p != nullptr);
            --exp;
            QCOMPARE(p->seq, exp);
            ++n;
        }
        QCOMPARE(n, count);
        QCOMPARE(exp, base);
    }

    // const reverse iterators
    {
        const Q& cq = q;
        reg n = 0u;
        std::uint32_t exp = base + static_cast<std::uint32_t>(count);
        for (auto it = cq.crbegin(); it != cq.crend(); ++it) {
            const auto* p = std::launder(*it);
            QVERIFY(p != nullptr);
            --exp;
            QCOMPARE(p->seq, exp);
            ++n;
        }
        QCOMPARE(n, count);
        QCOMPARE(exp, base);
    }
}

template <class Q>
static void deterministic_interleaving_suite() {
    Q q;
    expect_invariants(q);

    QVERIFY(q.try_emplace(1u));
    QVERIFY(q.try_emplace(2u));
    QVERIFY(q.try_emplace(3u));

    QCOMPARE(q.front()->seq, std::uint32_t{1});
    q.pop();

    QVERIFY(q.try_emplace(4u));
    QCOMPARE(q.front()->seq, std::uint32_t{2});
    q.pop();

    QCOMPARE(q.front()->seq, std::uint32_t{3});
    q.pop();

    QCOMPARE(q.front()->seq, std::uint32_t{4});
    q.pop();

    QVERIFY(q.empty());
}

template <class Q>
static void bulk_regions_wraparound_suite(Q& q) {
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();

    const reg cap = q.capacity();
    QVERIFY(cap >= 16u);

    // Step 1: push cap-5, pop cap-7 to put tail near the end.
    fill_seq(q, 1u, static_cast<reg>(cap - 5u));
    for (reg i = 0; i < static_cast<reg>(cap - 7u); ++i) {
        q.pop();
    }

    // Step 2: claim_write more than write_to_end_capacity to force split.
    const reg want = 12u;
    QVERIFY(q.free() >= want);

    auto wr = q.claim_write(::spsc::unsafe, want);
    QVERIFY(!wr.empty());
    QCOMPARE(wr.total, want);
    QVERIFY(wr.first.count > 0u);
    QVERIFY(wr.second.count > 0u);

    std::uint32_t seq = 100u;
    for (reg i = 0; i < wr.first.count; ++i) {
        T* dst = wr.first.ptr[i];
        ::new (static_cast<void*>(dst)) T(seq++);
    }
    for (reg i = 0; i < wr.second.count; ++i) {
        T* dst = wr.second.ptr[i];
        ::new (static_cast<void*>(dst)) T(seq++);
    }
    q.publish(wr.total);

    // Step 3: snapshot the final order, then drain and compare.
    auto snap = q.make_snapshot();
    QVERIFY(static_cast<reg>(snap.size()) == q.size());

    std::deque<std::uint32_t> expect;
    for (auto* p_raw : snap) {
        auto* p = std::launder(p_raw);
        expect.push_back(p->seq);
    }
    for (std::size_t i = 0; i < expect.size(); ++i) {
        auto* p = q.front();
        QCOMPARE(p->seq, expect[i]);
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void bulk_regions_max_count_suite(Q& q) {
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();

    const reg cap = q.capacity();
    const reg max_req = cap * 10u;

    auto wr = q.claim_write(::spsc::unsafe, max_req);
    QCOMPARE(wr.total, q.free());

    std::uint32_t seq = 1u;
    for (reg i = 0; i < wr.first.count; ++i) {
        ::new (static_cast<void*>(wr.first.ptr[i])) T(seq++);
    }
    for (reg i = 0; i < wr.second.count; ++i) {
        ::new (static_cast<void*>(wr.second.ptr[i])) T(seq++);
    }
    q.publish(wr.total);

    QVERIFY(q.full());
    QCOMPARE(q.size(), cap);

    auto rd = q.claim_read(::spsc::unsafe, max_req);
    QCOMPARE(rd.total, cap);

    reg seen = 0;
    for (reg i = 0; i < rd.first.count; ++i, ++seen) {
        auto* p = std::launder(rd.first.ptr[i]);
        QCOMPARE(p->seq, static_cast<std::uint32_t>(1u + seen));
    }
    for (reg i = 0; i < rd.second.count; ++i, ++seen) {
        auto* p = std::launder(rd.second.ptr[i]);
        QCOMPARE(p->seq, static_cast<std::uint32_t>(1u + seen));
    }
    q.pop(rd.total);
    QVERIFY(q.empty());
}

template <class Q>
static void bulk_read_max_count_suite(Q& q) {
    QVERIFY(q.is_valid());
    q.clear();

    fill_seq(q, 10u, 20u);

    auto rd = q.claim_read(::spsc::unsafe, 3u);
    QCOMPARE(rd.total, reg{3});
    QCOMPARE(rd.first.count + rd.second.count, rd.total);

    reg idx = 0;
    for (reg i = 0; i < rd.first.count; ++i, ++idx) {
        auto* p = std::launder(rd.first.ptr[i]);
        QCOMPARE(p->seq, static_cast<std::uint32_t>(10u + idx));
    }
    for (reg i = 0; i < rd.second.count; ++i, ++idx) {
        auto* p = std::launder(rd.second.ptr[i]);
        QCOMPARE(p->seq, static_cast<std::uint32_t>(10u + idx));
    }

    q.pop(rd.total);
    QCOMPARE(q.size(), reg{17});
    check_fifo_exact(q, 13u, 17u);
}

template <class Q>
static void snapshot_iteration_contract_suite(Q& q) {
    QVERIFY(q.is_valid());
    q.clear();

    fill_seq(q, 1u, 40u);

    auto snap = q.make_snapshot();
    QCOMPARE(static_cast<reg>(snap.size()), q.size());

    reg i = 0;
    for (auto* p_raw : snap) {
        auto* p = std::launder(p_raw);
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, static_cast<std::uint32_t>(1u + i));
        ++i;
    }
    QCOMPARE(i, q.size());
}

template <class Q>
static void snapshot_try_consume_contract_suite() {
    Q q;
    QVERIFY(q.is_valid());
    fill_seq(q, 1u, 32u);

    auto snap = q.make_snapshot();
    QVERIFY(!snap.empty());

    Q other;
    QVERIFY(other.is_valid());
    fill_seq(other, 100u, 4u);
    auto other_snap = other.make_snapshot();
    QVERIFY(!q.try_consume(other_snap));
    QCOMPARE(q.size(), reg{32});

    QVERIFY(q.try_consume(snap));
    QVERIFY(q.empty());
}

template <class Q>
static void snapshot_consume_suite(Q& q) {
    QVERIFY(q.is_valid());
    q.clear();

    fill_seq(q, 1u, 20u);
    auto snap = q.make_snapshot();
    q.consume(snap);
    QVERIFY(q.empty());

    fill_seq(q, 100u, 7u);
    auto snap2 = q.make_snapshot();
    QVERIFY(q.try_consume(snap2));
    QVERIFY(q.empty());
}

static void snapshot_invalid_pool_suite() {
    using Q = spsc::typed_pool<Blob, 0, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());

    auto snap = q.make_snapshot();
    QVERIFY(snap.empty());
    QVERIFY(!q.try_consume(snap));
}

template <class Q>
static void raii_guards_suite(Q& q) {
    QVERIFY(q.is_valid());
    q.clear();

    {
        auto g = q.scoped_write();
        QVERIFY(g);
        (void)g.emplace(1u);
        g.publish_on_destroy();
    }
    QCOMPARE(q.size(), reg{1});
    QCOMPARE(q.front()->seq, std::uint32_t{1});
    q.pop();

    {
        auto g = q.scoped_write();
        (void)g.emplace(2u);
        g.cancel();
    }
    QVERIFY(q.empty());

    fill_seq(q, 10u, 3u);
    {
        auto g = q.scoped_read();
        QVERIFY(g);
        auto* p = g.get();
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, std::uint32_t{10});
    }
    QCOMPARE(q.front()->seq, std::uint32_t{11});
    q.pop();
    q.pop();
    QVERIFY(q.empty());
}

template <class Q>
static void guard_move_semantics_suite(Q& q) {
    QVERIFY(q.is_valid());
    q.clear();

    {
        auto g1 = q.scoped_write();
        QVERIFY(g1);
        auto g2 = std::move(g1);
        QVERIFY(g2);
        QVERIFY(!g1);

        (void)g2.emplace(5u);
        g2.publish_on_destroy();
    }
    QCOMPARE(q.front()->seq, std::uint32_t{5});
    q.pop();

    fill_seq(q, 100u, 2u);
    {
        auto r1 = q.scoped_read();
        QVERIFY(r1);
        auto r2 = std::move(r1);
        QVERIFY(r2);
        QVERIFY(!r1);

        QCOMPARE(r2.get()->seq, std::uint32_t{100});
    }
    QCOMPARE(q.front()->seq, std::uint32_t{101});
    q.pop();
    QVERIFY(q.empty());
}

template <class Q>
static void allocator_and_bulk_raii_overloads_suite(Q& q) {
    using T = typename Q::object_type;
    static_assert(std::is_same_v<T, Blob>,
                  "allocator_and_bulk_raii_overloads_suite expects typed_pool<Blob, ...>.");

    [[maybe_unused]] auto alloc = q.get_allocator();

    q.clear();

    // bulk_write_guard: construct then disarm => no publish.
    {
        auto bw = q.scoped_write(5u);
        QVERIFY(bw);
        QCOMPARE(bw.claimed(), reg{5});
        QCOMPARE(bw.constructed(), reg{0});
        QCOMPARE(bw.remaining(), reg{5});

        static_cast<void>(bw.emplace_next(1u, 0xA5A5A5A5u));
        static_cast<void>(bw.emplace_next(2u, 0xA5A5A5A5u));
        QCOMPARE(bw.constructed(), reg{2});
        QCOMPARE(bw.remaining(), reg{3});

        bw.disarm_publish();
    }
    QVERIFY(q.empty());

    // bulk_write_guard: commit publishes exactly constructed().
    {
        auto bw = q.scoped_write(4u);
        QVERIFY(bw);
        for (std::uint32_t seq = 10u; seq < 14u; ++seq) {
            auto* p = bw.emplace_next(seq, 0xA5A5A5A5u);
            QVERIFY(p != nullptr);
        }
        bw.commit();
        QVERIFY(!bw);
    }
    QCOMPARE(q.size(), reg{4});

    // bulk_write_guard move semantics + destructor publish.
    {
        auto bw1 = q.scoped_write(2u);
        QVERIFY(bw1);
        auto bw2 = std::move(bw1);
        QVERIFY(!bw1);
        QVERIFY(bw2);
        static_cast<void>(bw2.emplace_next(14u, 0xA5A5A5A5u));
        static_cast<void>(bw2.emplace_next(15u, 0xA5A5A5A5u));
        bw2.arm_publish();
    }
    QCOMPARE(q.size(), reg{6});

    // bulk_read_guard: inspect first()/second(), then cancel (must not pop).
    {
        auto br = q.scoped_read(3u);
        QVERIFY(br);
        QCOMPARE(br.count(), reg{3});

        auto f = br.first();
        auto s = br.second();
#if SPSC_HAS_SPAN
        const auto f_raw = f.raw_span();
        const auto s_raw = s.raw_span();
        QCOMPARE(f_raw.size(), static_cast<std::size_t>(f.size()));
        QCOMPARE(s_raw.size(), static_cast<std::size_t>(s.size()));
#endif

        std::vector<std::uint32_t> seqs;
        seqs.reserve(static_cast<std::size_t>(br.count()));
        for (reg i = 0; i < f.size(); ++i) {
            auto* p = f.ptr(i);
            QVERIFY(p != nullptr);
            seqs.push_back(p->seq);
        }
        for (reg i = 0; i < s.size(); ++i) {
            auto* p = s.ptr(i);
            QVERIFY(p != nullptr);
            seqs.push_back(p->seq);
        }

        QCOMPARE(seqs.size(), std::size_t{3});
        QCOMPARE(seqs[0], 10u);
        QCOMPARE(seqs[1], 11u);
        QCOMPARE(seqs[2], 12u);

        br.cancel();
        QVERIFY(!br);
    }
    QCOMPARE(q.size(), reg{6});

    // bulk_read_guard: commit pops exactly count().
    {
        auto br = q.scoped_read(4u);
        QVERIFY(br);
        QCOMPARE(br.count(), reg{4});
        br.commit();
        QVERIFY(!br);
    }
    QCOMPARE(q.size(), reg{2});
    QCOMPARE(q.front()->seq, std::uint32_t{14});

    // bulk_read_guard move semantics + destructor pop.
    {
        auto br1 = q.scoped_read(2u);
        QVERIFY(br1);
        auto br2 = std::move(br1);
        QVERIFY(!br1);
        QVERIFY(br2);
        QCOMPARE(br2.count(), reg{2});
    }
    QVERIFY(q.empty());

    // max_count=0 must produce inactive guards.
    {
        auto bw = q.scoped_write(0u);
        QVERIFY(!bw);
    }
    {
        auto br = q.scoped_read(0u);
        QVERIFY(!br);
    }
}

template <class Policy>
static void run_static_suite() {
    {
        spsc::typed_pool<Blob, 64u, Policy> q;
        QVERIFY(q.is_valid());
        expect_invariants(q);

        fill_seq(q, 1u, 20u);
        check_iterators_and_indexing(q, 1u, 20u);

        snapshot_iteration_contract_suite(q);
        snapshot_consume_suite(q);

        bulk_regions_wraparound_suite(q);
        bulk_regions_max_count_suite(q);
        bulk_read_max_count_suite(q);

        raii_guards_suite(q);
        q.destroy();
    }

    tracked_reset();
    {
        spsc::typed_pool<Tracked, 64u, Policy> q;
        QVERIFY(q.is_valid());
        fill_seq(q, 1u, 40u);

        QVERIFY(q.try_pop(7u));
        QCOMPARE(q.size(), reg{33});

        q.consume_all();
        QVERIFY(q.empty());

        raii_guards_suite(q);
        guard_move_semantics_suite(q);

        q.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

template <class Policy>
static void run_dynamic_suite() {
    using Q = spsc::typed_pool<Blob, 0u, Policy>;
    Q q;

    QVERIFY(!q.is_valid());
    expect_invariants(q);

    QVERIFY(q.resize(64u));
    QVERIFY(q.is_valid());
    expect_invariants(q);

    fill_seq(q, 1u, 20u);
    QCOMPARE(q.size(), reg{20});

    QVERIFY(q.resize(128u));
    QVERIFY(q.is_valid());
    QCOMPARE(q.size(), reg{20});
    check_iterators_and_indexing(q, 1u, 20u);

    QVERIFY(q.resize(0u));
    QVERIFY(!q.is_valid());
    expect_invariants(q);

    QVERIFY(q.resize(32u));
    fill_seq(q, 100u, 10u);
    check_fifo_exact(q, 100u, 10u);

    q.destroy();
}

template <class Policy>
static void run_threaded_suite() {
    using Q = spsc::typed_pool<Blob, 0u, Policy>;

    Q q;
    QVERIFY(q.resize(1024u));

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> abort{false};

    std::thread prod([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (int i = 0; i < kThreadIters && !abort.load(); ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(i + 1);
            while (!q.try_emplace(v)) {
                if (abort.load()) return;
                std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });

    std::thread cons([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t expected = 1u;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kThreadTimeoutMs);

        while (!done.load(std::memory_order_acquire) || !q.empty()) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true);
                return;
            }

            auto* p = q.try_front();
            if (!p) {
                std::this_thread::yield();
                continue;
            }

            if (p->seq != expected) {
                abort.store(true);
                return;
            }
            q.pop();
            ++expected;
        }
    });

    start.store(true, std::memory_order_release);
    prod.join();
    cons.join();

    QVERIFY2(!abort.load(), "threaded_suite: consumer observed wrong sequence or timed out");
    QVERIFY(q.empty());
    q.destroy();
}

static void invalid_inputs_suite() {
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());
    expect_invariants(q);

    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());

    auto wr = q.claim_write(::spsc::unsafe);
    QVERIFY(wr.empty());
    auto rd = q.claim_read(::spsc::unsafe);
    QVERIFY(rd.empty());

    auto snap = q.make_snapshot();
    QVERIFY(snap.empty());

    QVERIFY(!q.try_consume(snap));
}

static void allocator_accounting_suite() {
    using Alloc = CountingAlignedAlloc<std::byte, 64u>;
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P, Alloc>;

    AllocStats::reset();
    {
        Q q;
        QVERIFY(!q.is_valid());

        QVERIFY(q.resize(64u));
        QVERIFY(q.is_valid());

        const reg cap1 = q.capacity();
        const auto allocs1 = AllocStats::allocs.load();
        QVERIFY2(allocs1 >= 1u + static_cast<unsigned long long>(cap1),
                 "Expected: 1 (pointer ring) + cap (object slots) allocations after first resize");
        QVERIFY(AllocStats::bytes_live.load() > 0u);

        fill_seq(q, 1u, 20u);
        QVERIFY(q.resize(256u));
        QVERIFY(q.is_valid());

        const reg cap2 = q.capacity();
        const auto allocs2 = AllocStats::allocs.load();

        // typed_pool reuses existing object storages on growth:
        // +1 allocation for new pointer ring, +(cap2-cap1) new object storages.
        const unsigned long long expected_min_allocs2 =
            static_cast<unsigned long long>(allocs1) +
            1ull + static_cast<unsigned long long>(cap2 - cap1);

        QVERIFY2(allocs2 >= expected_min_allocs2,
                 "Expected allocations to increase by at least: 1 (new ring) + (cap2-cap1) (new storages)");
        q.destroy();
    }

    QVERIFY(AllocStats::bytes_live.load() == 0u);
    QVERIFY(AllocStats::allocs.load() == AllocStats::deallocs.load());
}


static void dynamic_capacity_sweep_suite() {
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P>;
    for (reg cap = 2u; cap <= 1024u; cap <<= 1u) {
        Q q;
        QVERIFY(q.resize(cap));
        QCOMPARE(q.capacity(), cap);
        QVERIFY(q.free() == cap);

        const reg push_n = std::min<reg>(cap, 64u);
        fill_seq(q, 1u, push_n);
        check_fifo_exact(q, 1u, push_n);

        q.destroy();
    }
}

static void resize_migration_order_suite() {
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P>;
    Q q;
    QVERIFY(q.resize(64u));
    fill_seq(q, 1u, 50u);

    q.pop(17u);
    QCOMPARE(q.front()->seq, std::uint32_t{18});
    QVERIFY(q.resize(256u));
    QCOMPARE(q.size(), reg{33});
    check_fifo_exact(q, 18u, 33u);

    q.destroy();
}

static void move_swap_stress_suite() {
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P>;
    std::mt19937 rng(0xC0FFEEu);

    Q a;
    Q b;
    QVERIFY(a.resize(64u));
    QVERIFY(b.resize(128u));

    std::deque<std::uint32_t> ra;
    std::deque<std::uint32_t> rb;

    auto refill = [&](Q& q, std::deque<std::uint32_t>& ref, std::uint32_t base, reg n) {
        q.clear();
        ref.clear();
        for (reg i = 0; i < n; ++i) {
            const std::uint32_t v = base + static_cast<std::uint32_t>(i);
            QVERIFY(q.try_emplace(v));
            ref.push_back(v);
        }
    };

    refill(a, ra, 1u, 20u);
    refill(b, rb, 1000u, 33u);

    for (int it = 0; it < kSwapIters; ++it) {
        const int op = static_cast<int>(rng() % 6u);
        switch (op) {
        case 0:
            a.swap(b);
            std::swap(ra, rb);
            break;
        case 1: {
            Q tmp(std::move(a));
            QVERIFY(!a.is_valid());
            a = std::move(tmp);
            QVERIFY(a.is_valid());
            break;
        }
        case 2:
            if (!ra.empty()) {
                QCOMPARE(a.front()->seq, ra.front());
                a.pop();
                ra.pop_front();
            }
            break;
        case 3:
            if (a.free() > 0u) {
                const std::uint32_t v = static_cast<std::uint32_t>(rng());
                QVERIFY(a.try_emplace(v));
                ra.push_back(v);
            }
            break;
        case 4:
            if (!rb.empty()) {
                QCOMPARE(b.front()->seq, rb.front());
                b.pop();
                rb.pop_front();
            }
            break;
        case 5:
            if (b.free() > 0u) {
                const std::uint32_t v = static_cast<std::uint32_t>(rng());
                QVERIFY(b.try_emplace(v));
                rb.push_back(v);
            }
            break;
        default:
            break;
        }

        if (a.is_valid()) {
            QCOMPARE(a.size(), static_cast<reg>(ra.size()));
            if (!ra.empty()) {
                QCOMPARE(a.front()->seq, ra.front());
            }
        }
        if (b.is_valid()) {
            QCOMPARE(b.size(), static_cast<reg>(rb.size()));
            if (!rb.empty()) {
                QCOMPARE(b.front()->seq, rb.front());
            }
        }
    }

    a.destroy();
    b.destroy();
}

static void state_machine_fuzz_sweep_suite() {
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P>;
    std::mt19937 rng(0x12345678u);

    Q q;
    QVERIFY(q.resize(256u));
    std::deque<std::uint32_t> ref;
    std::uint32_t next = 1u;


    std::unordered_map<std::uint32_t, const void*> prev_ptr_by_seq;
    auto check_one = [&]() {
        if (!ref.empty()) {
            QCOMPARE(q.front()->seq, ref.front());
        }
        QCOMPARE(q.size(), static_cast<reg>(ref.size()));


        // Pointer set invariants + per-object pointer stability (no resize in this fuzz).
        {
            std::unordered_map<std::uint32_t, const void*> cur_ptr_by_seq;
            std::unordered_set<const void*> ptr_set;

            QCOMPARE(q.size(), static_cast<reg>(ref.size()));
            for (reg i = 0; i < q.size(); ++i) {
                const auto* p = q[i];
                QVERIFY(p != nullptr);

                // Pointers for concurrently live objects must be unique.
                QVERIFY(ptr_set.insert(static_cast<const void*>(p)).second);

                const std::uint32_t seq = p->seq;
                QCOMPARE(seq, ref[static_cast<std::size_t>(i)]);

                cur_ptr_by_seq.emplace(seq, static_cast<const void*>(p));

                if (auto it = prev_ptr_by_seq.find(seq); it != prev_ptr_by_seq.end()) {
                    QCOMPARE(it->second, static_cast<const void*>(p));
                }
            }

            prev_ptr_by_seq.swap(cur_ptr_by_seq);
        }

        expect_invariants(q);
    };

    for (int it = 0; it < kFuzzIters; ++it) {
        const int op = static_cast<int>(rng() % 10u);

        switch (op) {
        case 0:
            if (q.free() > 0u) {
                QVERIFY(q.try_emplace(next));
                ref.push_back(next++);
            }
            break;

        case 1:
            if (!ref.empty()) {
                QCOMPARE(q.front()->seq, ref.front());
                q.pop();
                ref.pop_front();
            }
            break;

        case 2: {
            const reg max_req = static_cast<reg>((rng() % 8u) + 1u);
            auto wr = q.claim_write(::spsc::unsafe, max_req);
            if (!wr.empty()) {
                reg filled = 0;
                for (reg i = 0; i < wr.first.count; ++i, ++filled) {
                    ::new (static_cast<void*>(wr.first.ptr[i])) Blob(next);
                    ref.push_back(next++);
                }
                for (reg i = 0; i < wr.second.count; ++i, ++filled) {
                    ::new (static_cast<void*>(wr.second.ptr[i])) Blob(next);
                    ref.push_back(next++);
                }
                QCOMPARE(filled, wr.total);
                q.publish(wr.total);
            }
            break;
        }

        case 3: {
            const reg max_req = static_cast<reg>((rng() % 8u) + 1u);
            auto rd = q.claim_read(::spsc::unsafe, max_req);
            if (!rd.empty()) {
                reg got = 0;
                for (reg i = 0; i < rd.first.count; ++i, ++got) {
                    QCOMPARE(std::launder(rd.first.ptr[i])->seq, ref[got]);
                }
                for (reg i = 0; i < rd.second.count; ++i, ++got) {
                    QCOMPARE(std::launder(rd.second.ptr[i])->seq, ref[got]);
                }
                q.pop(rd.total);
                for (reg i = 0; i < rd.total; ++i) {
                    ref.pop_front();
                }
            }
            break;
        }

        case 4: {
            auto snap = q.make_snapshot();
            QCOMPARE(static_cast<std::size_t>(snap.size()), ref.size());
            std::size_t i = 0;
            for (auto* p_raw : snap) {
                QCOMPARE(std::launder(p_raw)->seq, ref[i]);
                ++i;
            }
            break;
        }

        case 5:
            if ((rng() & 0x7u) == 0u) {
                q.clear();
                ref.clear();
            }
            break;

        case 6:
            if ((rng() & 0xFFu) == 0u) {
                const reg cur = q.capacity();
                const reg next_cap = (cur < 4096u) ? (cur << 1u) : cur;
                QVERIFY(q.resize(next_cap));
            }
            break;

        case 7:
            if ((rng() & 0x3FFu) == 0u) {
                q.destroy();
                ref.clear();
                QVERIFY(!q.is_valid());
                QVERIFY(q.resize(256u));
            }
            break;

        default:
            break;
        }

        check_one();
    }

    while (!ref.empty()) {
        QCOMPARE(q.front()->seq, ref.front());
        q.pop();
        ref.pop_front();
    }
    QVERIFY(q.empty());
    q.destroy();
}

static void resize_edge_cases_dynamic_suite() {
    using Q = spsc::typed_pool<Blob, 0u, spsc::policy::P>;
    Q q;

    QVERIFY(!q.is_valid());

    // typed_pool normalizes depth:
    //  - depth==0: destroy
    //  - depth<2: rounds up to 2
    //  - non-pow2: rounds up to next power-of-two
    QVERIFY(q.resize(3u));
    QVERIFY(q.is_valid());
    QCOMPARE(q.capacity(), reg{4});

    QVERIFY(q.resize(8u));
    QCOMPARE(q.capacity(), reg{8});

    QVERIFY(q.resize(8u));
    QCOMPARE(q.capacity(), reg{8});

    // Never shrink for a valid instance (keeps current capacity).
    QVERIFY(q.resize(4u));
    QCOMPARE(q.capacity(), reg{8});

    QVERIFY(q.resize(0u));
    QVERIFY(!q.is_valid());
}


static void dynamic_move_contract_suite() {
    using Q = spsc::typed_pool<Tracked, 0u, spsc::policy::P>;
    tracked_reset();

    Q a;
    QVERIFY(a.resize(64u));
    fill_seq(a, 1u, 20u);

    Q b{std::move(a)};

    QVERIFY(!a.is_valid());
    QVERIFY(b.is_valid());
    QCOMPARE(b.size(), reg{20});

    check_iterators_and_indexing(b, 1u, 20u);

    Q c;
    QVERIFY(c.resize(32u));
    fill_seq(c, 1000u, 5u);

    c = std::move(b);
    QVERIFY(!b.is_valid());
    QVERIFY(c.is_valid());

    QCOMPARE(c.size(), reg{20});
    check_fifo_exact(c, 1u, 20u);

    c.destroy();
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void consume_all_contract_suite() {
    {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        fill_seq(q, 1u, 40u);
        QVERIFY(!q.empty());
        q.consume_all();
        QVERIFY(q.empty());
        q.destroy();
    }

    tracked_reset();
    {
        spsc::typed_pool<Tracked, 64u, spsc::policy::P> q;
        fill_seq(q, 1u, 40u);
        QVERIFY(Tracked::live.load() > 0);
        q.consume_all();
        QVERIFY(q.empty());
        q.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

template <class Policy>
static void run_threaded_bulk_regions_suite(const char* /*name*/) {
    using Q = spsc::typed_pool<Blob, 0u, Policy>;

    Q q;
    QVERIFY(q.resize(1024u));

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> abort{false};

    constexpr std::uint32_t kMaxBatch = 8;

    std::thread prod([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t seq = 1u;
        for (int it = 0; it < kThreadIters && !abort.load(); ++it) {
            const reg want = static_cast<reg>((seq % kMaxBatch) + 1u);
            auto wr = q.claim_write(::spsc::unsafe, want);
            if (wr.empty()) {
                std::this_thread::yield();
                continue;
            }

            std::uint32_t local = seq;
            for (reg i = 0; i < wr.first.count; ++i) {
                ::new (static_cast<void*>(wr.first.ptr[i])) Blob(local++);
            }
            for (reg i = 0; i < wr.second.count; ++i) {
                ::new (static_cast<void*>(wr.second.ptr[i])) Blob(local++);
            }
            q.publish(wr.total);
            seq = local;
        }

        done.store(true, std::memory_order_release);
    });

    std::thread cons([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t expected = 1u;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kThreadTimeoutMs);

        while (!done.load(std::memory_order_acquire) || !q.empty()) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true);
                return;
            }

            auto rd = q.claim_read(::spsc::unsafe, kMaxBatch);
            if (rd.empty()) {
                std::this_thread::yield();
                continue;
            }

            for (reg i = 0; i < rd.first.count; ++i) {
                auto* p = std::launder(rd.first.ptr[i]);
                if (p->seq != expected) {
                    abort.store(true);
                    return;
                }
                ++expected;
            }
            for (reg i = 0; i < rd.second.count; ++i) {
                auto* p = std::launder(rd.second.ptr[i]);
                if (p->seq != expected) {
                    abort.store(true);
                    return;
                }
                ++expected;
            }

            q.pop(rd.total);
        }
    });

    start.store(true, std::memory_order_release);
    prod.join();
    cons.join();

    QVERIFY2(!abort.load(), "threaded_bulk_regions_suite: consumer observed wrong sequence or timed out");
    QVERIFY(q.empty());
    q.destroy();
}

static void alignment_sweep_suite() {
    {
        spsc::typed_pool<Aligned64, 64u, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        for (reg i = 0; i < q.capacity(); ++i) {
            const auto addr = reinterpret_cast<std::uintptr_t>(q.data()[i]);
            QVERIFY((addr % alignof(Aligned64)) == 0u);
        }
    }
    {
        spsc::typed_pool<Aligned64, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(128u));
        for (reg i = 0; i < q.capacity(); ++i) {
            const auto addr = reinterpret_cast<std::uintptr_t>(q.data()[i]);
            QVERIFY((addr % alignof(Aligned64)) == 0u);
        }
        q.destroy();
    }
}

static void full_empty_cycle_u32(spsc::typed_pool<std::uint32_t, 64u, spsc::policy::CA<>>& q, std::uint32_t base) {
    q.clear();
    for (std::uint32_t i = 0; i < 64u; ++i) {
        QVERIFY(q.try_emplace(base + i));
    }
    QVERIFY(q.full());
    for (std::uint32_t i = 0; i < 64u; ++i) {
        QCOMPARE(*q.front(), base + i);
        q.pop();
    }
    QVERIFY(q.empty());
}

static void stress_cached_ca_transitions_suite() {
    using QS = spsc::typed_pool<std::uint32_t, 64u, spsc::policy::CA<>>;
    using QD = spsc::typed_pool<std::uint32_t, 0u,  spsc::policy::CA<>>;

    {
        QS a;
        QS b;

        for (std::uint32_t i = 1; i <= 17; ++i) {
            QVERIFY(a.try_emplace(i));
        }
        for (std::uint32_t i = 100; i < 121; ++i) {
            QVERIFY(b.try_emplace(i));
        }

        a.swap(b);

        QCOMPARE(*a.front(), std::uint32_t{100});
        QCOMPARE(*b.front(), std::uint32_t{1});

        full_empty_cycle_u32(a, 1000u);
        full_empty_cycle_u32(b, 2000u);
    }

    {
        QS src;
        for (std::uint32_t i = 1; i <= 33; ++i) {
            QVERIFY(src.try_emplace(i));
        }

        QS dst(std::move(src));
        QVERIFY(!src.is_valid());
        QCOMPARE(dst.size(), reg{33});

        full_empty_cycle_u32(dst, 3000u);
    }

    {
        QS a;
        QS b;

        for (std::uint32_t i = 10; i < 30; ++i) {
            QVERIFY(a.try_emplace(i));
        }
        for (std::uint32_t i = 200; i < 210; ++i) {
            QVERIFY(b.try_emplace(i));
        }

        a = std::move(b);
        QVERIFY(!b.is_valid());
        QCOMPARE(*a.front(), std::uint32_t{200});
        full_empty_cycle_u32(a, 4000u);
    }

    {
        QD q;

        QVERIFY(q.resize(64u));
        for (std::uint32_t i = 1; i <= 48; ++i) {
            QVERIFY(q.try_emplace(i));
        }

        QVERIFY(q.resize(128u));
        for (std::uint32_t i = 0; i < 128u - q.size(); ++i) {
            QVERIFY(q.try_emplace(5000u + i));
        }
        while (!q.empty()) {
            q.pop();
        }
        QVERIFY(q.empty());

        QVERIFY(q.resize(0u));
        QVERIFY(!q.is_valid());
    }
}

static void death_tests_debug_only_suite() {
#if !defined(NDEBUG)
    auto expect_death = [&](const char* mode) {
        QProcess p;
        p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments(QStringList{});

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SPSC_TYPED_POOL_DEATH", QString::fromLatin1(mode));
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
    expect_death("front_empty");
    expect_death("publish_full");
    expect_death("claim_full");
    expect_death("double_emplace");
    expect_death("commit_unconstructed");
    expect_death("bulk_double_emplace_next");
    expect_death("bulk_arm_publish_unconstructed");
    expect_death("consume_foreign_snapshot");
    expect_death("pop_n_too_many");
#else
    QSKIP("Death tests are debug-only (assertions disabled)." );
#endif
}

static void lifecycle_traced_suite() {
    tracked_reset();
    {
        spsc::typed_pool<Tracked, 64u, spsc::policy::P> q;

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
        q.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

} // namespace



// ==============================================================================================
// Extra-heavy contract tests (requested):
//  - More bulk-region variants (partial publish, staggered wrap).
//  - Canonical regression "matrix" scripts (deterministic, model-based).
//  - Pointer stability / migration properties.
//  - "No double-claim without publish" semantics.
// ==============================================================================================

template <class Regions>
static auto wr_ptr_at_(const Regions& wr, const reg pos) -> decltype(wr.first.ptr[0])
{
    // Order is always: first region, then second region.
    if (pos < wr.first.count) {
        return wr.first.ptr[pos];
    }
    return wr.second.ptr[pos - wr.first.count];
}

template <class Q>
static std::vector<typename Q::pointer> collect_slot_ptrs_(const Q& q)
{
    std::vector<typename Q::pointer> out;
    out.reserve(q.capacity());
    auto* d = q.data();
    for (reg i = 0; i < q.capacity(); ++i) {
        out.push_back(d[i]);
    }
    return out;
}

template <class Q>
static void assert_slot_ptrs_stable_(const Q& q, const std::vector<typename Q::pointer>& base)
{
    QCOMPARE(static_cast<reg>(base.size()), q.capacity());
    for (reg i = 0; i < q.capacity(); ++i) {
        QCOMPARE(q.data()[i], base[static_cast<std::size_t>(i)]);
    }
}

template <class Q>
static void no_double_claim_without_publish_suite(Q& q)
{
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();
    QCOMPARE(q.size(), reg{0});

    const reg free0 = q.free();

    // Single-slot claim must not reserve. Calling claim twice must return the same slot.
    T* p1 = q.try_claim();
    QVERIFY(p1 != nullptr);

    T* p2 = q.try_claim();
    QVERIFY(p2 != nullptr);

    QCOMPARE(p1, p2);
    QCOMPARE(q.size(), reg{0});
    QCOMPARE(q.free(), free0);

    // Construct, still not visible until publish.
    ::new (static_cast<void*>(p1)) T(std::uint32_t{123});

    QCOMPARE(q.size(), reg{0});
    QCOMPARE(q.free(), free0);

    q.publish();
    QCOMPARE(q.size(), reg{1});
    QCOMPARE(q.front()->seq, std::uint32_t{123});

    q.pop();
    QVERIFY(q.empty());

    // Bulk claim_write must also not reserve until publish(n).
    if (q.capacity() >= 8u) {
        q.clear();
        QCOMPARE(q.size(), reg{0});
        const reg want = 8u;
        QVERIFY(q.free() >= want);

        auto wr1 = q.claim_write(::spsc::unsafe, want);
        auto wr2 = q.claim_write(::spsc::unsafe, want);
        QCOMPARE(wr1.total, want);
        QCOMPARE(wr2.total, want);

        for (reg i = 0; i < wr1.first.count; ++i) {
            QCOMPARE(wr1.first.ptr[i], wr2.first.ptr[i]);
        }
        for (reg i = 0; i < wr1.second.count; ++i) {
            QCOMPARE(wr1.second.ptr[i], wr2.second.ptr[i]);
        }

        // Construct ONLY first k objects, publish(k).
        const reg k = 2u;
        reg done = 0u;

        for (reg i = 0; i < wr1.first.count && done < k; ++i) {
            ::new (static_cast<void*>(wr1.first.ptr[i])) T(static_cast<std::uint32_t>(1u + done));
            ++done;
        }
        for (reg i = 0; i < wr1.second.count && done < k; ++i) {
            ::new (static_cast<void*>(wr1.second.ptr[i])) T(static_cast<std::uint32_t>(1u + done));
            ++done;
        }

        QCOMPARE(done, k);
        q.publish(k);

        QCOMPARE(q.size(), k);
        QCOMPARE(q.front()->seq, std::uint32_t{1});
        q.pop();
        QCOMPARE(q.front()->seq, std::uint32_t{2});
        q.pop();
        QVERIFY(q.empty());
    }
}

template <class Q>
static void bulk_regions_partial_publish_suite(Q& q)
{
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();

    // Prepare wrap-around: move write index close to the end.
    // We leave 2 items alive so we can also verify order preservation.
    const reg cap = q.capacity();
    QVERIFY(cap >= 16u);

    const std::uint32_t base = 1u;
    const reg fill_n = cap - 5u;
    const reg pop_n  = cap - 7u;

    fill_seq(q, base, fill_n);
    q.pop(pop_n);

    QCOMPARE(q.size(), reg{2});
    const std::uint32_t old1 = base + static_cast<std::uint32_t>(pop_n);
    const std::uint32_t old2 = base + static_cast<std::uint32_t>(pop_n + 1u);
    QCOMPARE(q[0]->seq, old1);
    QCOMPARE(q[1]->seq, old2);

    // Claim a chunk that must split into two regions.
    const reg want = 12u;
    QVERIFY(q.free() >= want);

    auto wr = q.claim_write(::spsc::unsafe, want);
    QCOMPARE(wr.total, want);
    QVERIFY(wr.second.count > 0u);

    // Publish only a prefix.
    const reg k = 5u;
    QVERIFY(k < wr.total);

    std::uint32_t seq = 100u;
    reg constructed = 0u;
    for (reg i = 0; i < wr.first.count && constructed < k; ++i) {
        ::new (static_cast<void*>(wr.first.ptr[i])) T(seq++);
        ++constructed;
    }
    for (reg i = 0; i < wr.second.count && constructed < k; ++i) {
        ::new (static_cast<void*>(wr.second.ptr[i])) T(seq++);
        ++constructed;
    }
    QCOMPARE(constructed, k);

    q.publish(k);

    // Validate order: old items first, then published prefix.
    QCOMPARE(q.size(), reg{2 + k});
    QCOMPARE(q.front()->seq, old1); q.pop();
    QCOMPARE(q.front()->seq, old2); q.pop();

    for (reg i = 0; i < k; ++i) {
        QCOMPARE(q.front()->seq, std::uint32_t{100u + static_cast<std::uint32_t>(i)});
        q.pop();
    }
    QVERIFY(q.empty());

    // Now verify the "unpublished suffix" is still writable and starts right after the prefix.
    const reg remain = want - k;
    QVERIFY(q.free() >= remain);

    auto wr2 = q.claim_write(::spsc::unsafe, remain);
    QCOMPARE(wr2.total, remain);

    for (reg i = 0; i < remain; ++i) {
        QCOMPARE(wr_ptr_at_(wr2, i), wr_ptr_at_(wr, k + i));
    }

    // Don't publish anything here; just leave (claim does not reserve).
}

template <class Q>
static void bulk_regions_staggered_wrap_suite(Q& q)
{
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();

    const reg cap = q.capacity();
    QVERIFY(cap >= 16u);

    // Force a split claim_write by moving write index near end.
    fill_seq(q, 1u, cap - 2u);
    q.pop(cap - 4u); // leave 2 alive, write index near end

    const reg want = 10u;
    QVERIFY(q.free() >= want);

    auto wr = q.claim_write(::spsc::unsafe, want);
    QCOMPARE(wr.total, want);
    QVERIFY(wr.second.count > 0u);

    const reg n1 = wr.first.count;   // to end
    const reg n2 = wr.second.count;  // wrap part
    QVERIFY(n1 > 0u);
    QVERIFY(n2 > 0u);
    QCOMPARE(n1 + n2, want);

    // Publish exactly the first region (finish at end, wrap head to 0).
    std::uint32_t seq = 500u;
    for (reg i = 0; i < n1; ++i) {
        ::new (static_cast<void*>(wr.first.ptr[i])) T(seq++);
    }
    q.publish(n1);

    // Next claim should start at 0 and match the second region pointers.
    auto wr2 = q.claim_write(::spsc::unsafe, n2);
    QCOMPARE(wr2.total, n2);
    QCOMPARE(wr2.second.count, reg{0});
    QCOMPARE(wr2.first.count, n2);

    for (reg i = 0; i < n2; ++i) {
        QCOMPARE(wr2.first.ptr[i], wr.second.ptr[i]);
    }

    for (reg i = 0; i < n2; ++i) {
        ::new (static_cast<void*>(wr2.first.ptr[i])) T(seq++);
    }
    q.publish(n2);

    // Drain and validate that the wrap inserted items are in strict FIFO order.
    // First drain the 2 survivors from the pre-fill:
    QCOMPARE(q.front()->seq, static_cast<std::uint32_t>(cap - 3u)); q.pop();
    QCOMPARE(q.front()->seq, static_cast<std::uint32_t>(cap - 2u)); q.pop();

    // Then our published sequences:
    for (reg i = 0; i < want; ++i) {
        QCOMPARE(q.front()->seq, std::uint32_t{500u + static_cast<std::uint32_t>(i)});
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void bulk_regions_partial_publish_matrix_suite(Q& q)
{
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    const reg cap = q.capacity();
    QVERIFY(cap >= 16u);

    // Make sure we split: after fill(cap-2) head is at cap-2 => only 2 slots to the end.
    // Keep 4 survivors alive so we also validate FIFO order relative to existing items.
    const std::uint32_t base = 1u;

    const std::array<reg, 5> wants = { reg{3}, reg{4}, reg{5}, reg{7}, reg{9} };

    for (reg want : wants) {
        q.clear();
        fill_seq(q, base, cap - 2u);
        q.pop(cap - 6u); // 4 survivors

        QCOMPARE(q.size(), reg{4});

        QVERIFY(q.free() >= want);
        auto wr = q.claim_write(::spsc::unsafe, want);
        QCOMPARE(wr.total, want);
        QVERIFY(wr.second.count > 0u);

        // Try several prefixes: 1, mid, last.
        const std::array<reg, 3> prefixes = { reg{1}, reg{want / 2u}, reg{want - 1u} };

        for (reg k : prefixes) {
            q.clear();
            fill_seq(q, base, cap - 2u);
            q.pop(cap - 6u);

            QVERIFY(q.free() >= want);
            auto wrk = q.claim_write(::spsc::unsafe, want);
            QCOMPARE(wrk.total, want);
            QVERIFY(wrk.second.count > 0u);
            QVERIFY(k >= 1u && k < want);

            // Construct only k, publish(k).
            std::uint32_t seq = 1000u;
            reg done = 0u;
            for (reg i = 0; i < wrk.first.count && done < k; ++i) {
                ::new (static_cast<void*>(wrk.first.ptr[i])) T(seq++);
                ++done;
            }
            for (reg i = 0; i < wrk.second.count && done < k; ++i) {
                ::new (static_cast<void*>(wrk.second.ptr[i])) T(seq++);
                ++done;
            }
            QCOMPARE(done, k);
            q.publish(k);

            // Drain survivors first (their exact values are deterministic).
            for (reg i = 0; i < 4u; ++i) {
                QVERIFY(q.front() != nullptr);
                q.pop();
            }

            // Then our published prefix.
            for (reg i = 0; i < k; ++i) {
                QCOMPARE(q.front()->seq, std::uint32_t{1000u + static_cast<std::uint32_t>(i)});
                q.pop();
            }
            QVERIFY(q.empty());
        }
    }
}



template <class Q>
static void pointer_stability_suite_static(Q& q)
{
    QVERIFY(q.is_valid());
    q.clear();

    const auto base = collect_slot_ptrs_(q);

    // Exercise operations that must NOT change physical slot addresses.
    fill_seq(q, 1u, 20u);
    q.pop(10u);
    fill_seq(q, 1000u, 5u);
    q.consume_all();

    assert_slot_ptrs_stable_(q, base);

    // Pointers returned by front()/operator[] must come from that stable set.
    fill_seq(q, 7u, 12u);

    std::unordered_set<typename Q::pointer> set(base.begin(), base.end());
    QVERIFY(set.size() == static_cast<std::size_t>(q.capacity()));

    for (reg i = 0; i < q.size(); ++i) {
        QVERIFY(set.count(q[i]) != 0u);
    }

    q.consume_all();
    assert_slot_ptrs_stable_(q, base);
}

template <class Q>
static void pointer_migration_suite_dynamic(Q& q)
{
    using P = typename Q::pointer;

    QVERIFY(q.is_valid());
    q.clear();

    const auto base = collect_slot_ptrs_(q);
    std::unordered_set<P> base_set(base.begin(), base.end());
    QVERIFY(base_set.size() == static_cast<std::size_t>(q.capacity()));

    // Make some live objects and remember their physical pointers in FIFO order.
    fill_seq(q, 1u, 12u);

    std::vector<P> live;
    live.reserve(q.size());
    for (reg i = 0; i < q.size(); ++i) {
        live.push_back(q[i]);
    }

    // Grow: pointers may reorder, but all old pointers must still exist exactly once.
    const reg old_cap = q.capacity();
    QVERIFY(q.resize(old_cap * 2u));
    const reg new_cap = q.capacity();
    QVERIFY(new_cap >= old_cap * 2u);

    const auto after = collect_slot_ptrs_(q);
    std::unordered_set<P> after_set(after.begin(), after.end());
    QVERIFY(after_set.size() == static_cast<std::size_t>(new_cap));

    for (auto* p : base) {
        QVERIFY(after_set.count(p) != 0u);
    }

    // Live objects must keep their addresses and FIFO order after resize.
    QCOMPARE(q.size(), reg{12});
    for (reg i = 0; i < 12u; ++i) {
        QCOMPARE(q[i], live[static_cast<std::size_t>(i)]);
        QCOMPARE(q[i]->seq, std::uint32_t{1u + static_cast<std::uint32_t>(i)});
    }

    q.consume_all();
}

template <class Q>
static void regression_matrix_suite(Q& q, const char* tag)
{
    Q_UNUSED(tag);

    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();

    std::deque<std::uint32_t> model;

    auto check_model = [&]() {
        QCOMPARE(q.size(), static_cast<reg>(model.size()));
        if (!model.empty()) {
            QCOMPARE(q.front()->seq, model.front());
            QCOMPARE(q[q.size() - 1]->seq, model.back());
        }
        for (reg i = 0; i < q.size(); ++i) {
            QCOMPARE(q[i]->seq, model[static_cast<std::size_t>(i)]);
        }
    };

    auto do_push = [&](std::uint32_t base, reg n) {
        QVERIFY(q.free() >= n);
        for (reg i = 0; i < n; ++i) {
            q.emplace(base + static_cast<std::uint32_t>(i));
            model.push_back(base + static_cast<std::uint32_t>(i));
        }
        check_model();
    };

    auto do_pop = [&](reg n) {
        QVERIFY(q.size() >= n);
        for (reg i = 0; i < n; ++i) {
            QCOMPARE(q.front()->seq, model.front());
            q.pop();
            model.pop_front();
        }
        check_model();
    };

    auto do_bulk = [&](std::uint32_t base, reg n) {
        QVERIFY(q.free() >= n);
        auto wr = q.claim_write(::spsc::unsafe, n);
        QCOMPARE(wr.total, n);

        std::uint32_t seq = base;
        for (reg i = 0; i < wr.first.count; ++i) {
            ::new (static_cast<void*>(wr.first.ptr[i])) T(seq++);
        }
        for (reg i = 0; i < wr.second.count; ++i) {
            ::new (static_cast<void*>(wr.second.ptr[i])) T(seq++);
        }
        q.publish(n);

        for (reg i = 0; i < n; ++i) {
            model.push_back(base + static_cast<std::uint32_t>(i));
        }
        check_model();
    };

    auto do_bulk_partial = [&](std::uint32_t base, reg n, reg k) {
        QVERIFY(q.free() >= n);
        QVERIFY(k <= n);
        auto wr = q.claim_write(::spsc::unsafe, n);
        QCOMPARE(wr.total, n);

        std::uint32_t seq = base;
        reg done = 0u;
        for (reg i = 0; i < wr.first.count && done < k; ++i) {
            ::new (static_cast<void*>(wr.first.ptr[i])) T(seq++);
            ++done;
        }
        for (reg i = 0; i < wr.second.count && done < k; ++i) {
            ::new (static_cast<void*>(wr.second.ptr[i])) T(seq++);
            ++done;
        }
        QCOMPARE(done, k);
        q.publish(k);

        for (reg i = 0; i < k; ++i) {
            model.push_back(base + static_cast<std::uint32_t>(i));
        }
        check_model();
    };

    const reg cap = q.capacity();
    QVERIFY(cap >= 8u);

    // Matrix over a few canonical prefill patterns to force wrap-around.
    const std::array<reg, 8> pre_fills = { reg{0}, reg{1}, reg{2}, reg{3}, reg{5}, reg{cap/2}, reg{cap-2}, reg{cap-1} };

    std::uint32_t seq_base = 1u;

    for (reg pre : pre_fills) {
        q.clear();
        model.clear();
        check_model();

        if (pre != 0u) {
            do_push(seq_base, pre);
            seq_base += static_cast<std::uint32_t>(pre);
        }

        // Pop a variable amount (including 0) to move tail.
        const reg pop1 = (pre >= 3u) ? 2u : 0u;
        if (pop1 != 0u) {
            do_pop(pop1);
        }

        // Force a bulk wrap insert.
        const reg room = q.free();
        const reg bulk_n = (room >= 6u) ? 6u : (room >= 4u ? 4u : 0u);
        if (bulk_n != 0u) {
            do_bulk(seq_base, bulk_n);
            seq_base += static_cast<std::uint32_t>(bulk_n);
        }

        // Partial bulk publish to test "prefix visibility".
        const reg room2 = q.free();
        if (room2 >= 8u) {
            do_bulk_partial(seq_base, 8u, 5u);
            seq_base += 5u;
        }

        // A couple of single pushes/pops to scramble indices.
        if (q.free() >= 3u) {
            do_push(seq_base, 3u);
            seq_base += 3u;
        }
        if (q.size() >= 4u) {
            do_pop(4u);
        }

        // Drain everything and ensure model matches.
        do_pop(q.size());
        QVERIFY(q.empty());
        QVERIFY(model.empty());
    }
}





template <class Q>
static void claim_read_consume_scenarios_suite(Q& q)
{
    using T = typename Q::object_type;

    QVERIFY(q.is_valid());
    q.clear();

    // Scenario 0: empty => claim_read() must be empty.
    {
        const auto r = q.claim_read(::spsc::unsafe);
        QCOMPARE(r.total, reg{0});
        QVERIFY(r.first.empty());
        QVERIFY(r.second.empty());
    }

    // Scenario 1: claim_read(max_count) exposes only a prefix, and pop(prefix) consumes exactly that.
    {
        for (std::uint32_t i = 1; i <= 6; ++i) {
            q.emplace(i);
        }
        QCOMPARE(q.size(), reg{6});

        const auto r = q.claim_read(::spsc::unsafe, 3u);
        QCOMPARE(r.total, reg{3});
        QCOMPARE(r.first.count + r.second.count, r.total);

        // Verify exposed pointers match operator[] order for the prefix.
        std::vector<std::uint32_t> seen;
        seen.reserve(static_cast<std::size_t>(r.total));
        for (reg i = 0; i < static_cast<reg>(r.first.count); ++i) {
            QVERIFY(r.first.ptr[i] != nullptr);
            seen.push_back(r.first.ptr[i]->seq);
        }
        for (reg i = 0; i < static_cast<reg>(r.second.count); ++i) {
            QVERIFY(r.second.ptr[i] != nullptr);
            seen.push_back(r.second.ptr[i]->seq);
        }
        QCOMPARE(seen.size(), static_cast<std::size_t>(r.total));
        QCOMPARE(seen[0], std::uint32_t{1});
        QCOMPARE(seen[1], std::uint32_t{2});
        QCOMPARE(seen[2], std::uint32_t{3});

        q.pop(r.total);
        QCOMPARE(q.size(), reg{3});
        QCOMPARE(q[0]->seq, std::uint32_t{4});
        QCOMPARE(q[1]->seq, std::uint32_t{5});
        QCOMPARE(q[2]->seq, std::uint32_t{6});
    }

    // Scenario 2: force wrap so claim_read() splits into two regions; pop(total) consumes all.
    {
        const reg cap = q.capacity();
        QVERIFY(cap >= 8);

        q.clear();
        const reg fill1 = cap - 2;
        for (std::uint32_t i = 1; i <= static_cast<std::uint32_t>(fill1); ++i) {
            q.emplace(1000u + i);
        }
        q.pop(fill1 - 3); // leave 3 items, tail is near end now.

        // Push more to wrap head.
        q.emplace(2001u);
        q.emplace(2002u);
        q.emplace(2003u);

        QVERIFY(q.size() >= reg{6});

        const auto r = q.claim_read(::spsc::unsafe);
        QCOMPARE(r.total, q.size());
        QCOMPARE(r.first.count + r.second.count, r.total);

        // Must expose all items in FIFO order.
        std::vector<std::uint32_t> seqs;
        seqs.reserve(static_cast<std::size_t>(r.total));
        for (reg i = 0; i < static_cast<reg>(r.first.count); ++i) {
            seqs.push_back(r.first.ptr[i]->seq);
        }
        for (reg i = 0; i < static_cast<reg>(r.second.count); ++i) {
            seqs.push_back(r.second.ptr[i]->seq);
        }

        QCOMPARE(seqs.size(), static_cast<std::size_t>(r.total));
        for (reg i = 0; i < r.total; ++i) {
            QCOMPARE(seqs[static_cast<std::size_t>(i)], q[i]->seq);
        }

        q.pop(r.total);
        QVERIFY(q.empty());
    }

    // Scenario 3: claim_read(max_count=0) must be empty and must not change state.
    {
        q.clear();
        q.emplace(42u);
        q.emplace(43u);

        const auto r = q.claim_read(::spsc::unsafe, 0u);
        QCOMPARE(r.total, reg{0});
        QVERIFY(r.first.empty());
        QVERIFY(r.second.empty());

        QCOMPARE(q.size(), reg{2});
        QCOMPARE(q.front()->seq, std::uint32_t{42});
    }

    q.consume_all();
    QVERIFY(q.empty());
    (void)sizeof(T);
}


// ------------------------------------------------------------------------------------------
// Copy semantics (copy ctor / copy assign / self-assign)
// ------------------------------------------------------------------------------------------

template <class Q>
static std::vector<std::uint32_t> drain_seq(Q& q) {
    std::vector<std::uint32_t> out;
    out.reserve(static_cast<std::size_t>(q.size()));
    while (!q.empty()) {
        out.push_back(q.front()->seq);
        q.pop();
    }
    return out;
}

template <class Q>
static void fill_wrap_scenario(Q& q) {
    // Build a wrapped (tail!=0 and head wrapped) live set to verify copy preserves order.
    q.clear();
    fill_seq(q, 1u, 50u);
    q.pop(40u);             // leaves [41..50]
    fill_seq(q, 1000u, 30u); // appends [1000..1029], forces wrap for cap=64
    QCOMPARE(q.size(), reg{40});
}

static void copy_semantics_static_suite() {
    using Q = spsc::typed_pool<Tracked, 64u, spsc::policy::P>;

    tracked_reset();
    {
        Q a;
        QVERIFY(a.is_valid());
        fill_wrap_scenario(a);

        const auto copy_before = Tracked::copy.load();
        Q b(a); // copy ctor
        QVERIFY(b.is_valid());
        QCOMPARE(b.size(), a.size());

        auto a_seq = drain_seq(a);
        auto b_seq = drain_seq(b);
        QCOMPARE(a_seq, b_seq);

        // Expect at least one copy per live element.
        QVERIFY2(Tracked::copy.load() >= copy_before + 40, "copy ctor did not copy-construct expected number of elements");

        a.destroy();
        b.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());

    tracked_reset();
    {
        Q src;
        Q dst;
        QVERIFY(src.is_valid());
        QVERIFY(dst.is_valid());

        fill_wrap_scenario(src);
        fill_seq(dst, 77u, 10u);

        const auto copy_before = Tracked::copy.load();
        dst = src; // copy assign
        QCOMPARE(dst.size(), src.size());

        // Self-assign must be a no-op.
        const auto dst_size_before = dst.size();
        dst = dst;
        QCOMPARE(dst.size(), dst_size_before);

        auto src_seq = drain_seq(src);
        auto dst_seq = drain_seq(dst);
        QCOMPARE(src_seq, dst_seq);

        QVERIFY2(Tracked::copy.load() >= copy_before + 40, "copy assign did not copy-construct expected number of elements");

        src.destroy();
        dst.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void copy_semantics_dynamic_suite() {
    using Q = spsc::typed_pool<Tracked, 0u, spsc::policy::P>;

    tracked_reset();
    {
        Q a;
        QVERIFY(a.resize(64u));
        fill_wrap_scenario(a);

        const auto copy_before = Tracked::copy.load();
        Q b(a); // copy ctor
        QVERIFY(b.is_valid());
        QCOMPARE(b.size(), a.size());

        auto a_seq = drain_seq(a);
        auto b_seq = drain_seq(b);
        QCOMPARE(a_seq, b_seq);

        QVERIFY2(Tracked::copy.load() >= copy_before + 40, "dynamic copy ctor did not copy-construct expected number of elements");

        a.destroy();
        b.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());

    tracked_reset();
    {
        Q src;
        Q dst;
        QVERIFY(src.resize(64u));
        QVERIFY(dst.resize(64u));

        fill_wrap_scenario(src);
        fill_seq(dst, 77u, 10u);

        const auto copy_before = Tracked::copy.load();
        dst = src; // copy assign
        QCOMPARE(dst.size(), src.size());

        const auto dst_size_before = dst.size();
        dst = dst; // self-assign
        QCOMPARE(dst.size(), dst_size_before);

        auto src_seq = drain_seq(src);
        auto dst_seq = drain_seq(dst);
        QCOMPARE(src_seq, dst_seq);

        QVERIFY2(Tracked::copy.load() >= copy_before + 40, "dynamic copy assign did not copy-construct expected number of elements");

        src.destroy();
        dst.destroy();
    }
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

// ------------------------------------------------------------------------------------------
// MT snapshot + try_consume stress (targets head<tail snapshot hazards)
// ------------------------------------------------------------------------------------------

template <class Policy>
static void run_threaded_snapshot_try_consume_suite() {
    using Q = spsc::typed_pool<Blob, 0u, Policy>;

    Q q;
    QVERIFY(q.resize(64u));

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> abort{false};

    std::thread prod([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t seq = 1u;
        for (int it = 0; it < kThreadIters && !abort.load(std::memory_order_relaxed); ++it) {
            while (!q.try_emplace(seq)) {
                if (abort.load(std::memory_order_relaxed)) return;
                std::this_thread::yield();
            }
            ++seq;
        }
        done.store(true, std::memory_order_release);
    });

    std::thread cons([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t expected = 1u;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kThreadTimeoutMs);

        while (!done.load(std::memory_order_acquire) || !q.empty()) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            // Regression: container iterators must always terminate under concurrent producer activity.
            {
                reg steps = 0u;
                for (auto* p : q) {
                    (void)p;
                    if (++steps > q.capacity()) {
                        abort.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            }

            const auto snap = q.make_snapshot();

            const auto tail = static_cast<reg>(snap.tail_index());
            const auto head = static_cast<reg>(snap.head_index());
            const auto used = static_cast<reg>(head - tail);

            // If this trips, snapshot is inconsistent (typical head<tail hazard).
            if (used > q.capacity()) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }
            if (used == 0u) {
                // If snapshot says "0", iterators must reflect that too.
                if (snap.begin() != snap.end()) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
                continue;
            }

            reg seen = 0u;
            for (auto* raw : snap) {
                if (++seen > q.capacity()) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                if (!raw) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                auto* p = std::launder(raw);
                if (p->seq != expected) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                ++expected;
            }
            if (seen != used) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            if (!q.try_consume(snap)) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }
        }
    });

    start.store(true, std::memory_order_release);
    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), "threaded_snapshot_try_consume_suite: inconsistent snapshot or sequence mismatch");
    QVERIFY(q.empty());
    q.destroy();
}


class tst_typed_pool_api_paranoid final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { api_compile_smoke_all(); }

    void copy_semantics_static()  { copy_semantics_static_suite(); }
    void copy_semantics_dynamic() { copy_semantics_dynamic_suite(); }

    void threaded_snapshot_atomic_A()  { run_threaded_snapshot_try_consume_suite<spsc::policy::A<>>(); }
    void threaded_snapshot_cached_CA() { run_threaded_snapshot_try_consume_suite<spsc::policy::CA<>>(); }


    void static_plain_P()    { run_static_suite<spsc::policy::P>(); }
    void static_volatile_V() { run_static_suite<spsc::policy::V>(); }
    void static_atomic_A()   { run_static_suite<spsc::policy::A<>>(); }
    void static_cached_CA()  { run_static_suite<spsc::policy::CA<>>(); }

    void dynamic_plain_P()    { run_dynamic_suite<spsc::policy::P>(); }
    void dynamic_volatile_V() { run_dynamic_suite<spsc::policy::V>(); }
    void dynamic_atomic_A()   { run_dynamic_suite<spsc::policy::A<>>(); }
    void dynamic_cached_CA()  { run_dynamic_suite<spsc::policy::CA<>>(); }

    void deterministic_interleaving() {
        deterministic_interleaving_suite<spsc::typed_pool<Blob, 64u, spsc::policy::P>>();
    }

    void threaded_atomic_A()  { run_threaded_suite<spsc::policy::A<>>(); }
    void threaded_cached_CA() { run_threaded_suite<spsc::policy::CA<>>(); }

    void allocator_accounting() { allocator_accounting_suite(); }
    void invalid_inputs()       { invalid_inputs_suite(); }

    void dynamic_capacity_sweep()   { dynamic_capacity_sweep_suite(); }
    void move_swap_stress()         { move_swap_stress_suite(); }
    void state_machine_fuzz_sweep() { state_machine_fuzz_sweep_suite(); }
    void resize_migration_order()   { resize_migration_order_suite(); }

    void snapshot_try_consume_contract() { snapshot_try_consume_contract_suite<spsc::typed_pool<Blob, 64u, spsc::policy::P>>(); }
    void bulk_regions_wraparound()       { spsc::typed_pool<Blob, 64u, spsc::policy::P> q; bulk_regions_wraparound_suite(q); q.destroy(); }


    void no_double_claim_without_publish() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        no_double_claim_without_publish_suite(q);
        q.destroy();
    }

    void bulk_regions_partial_publish() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        bulk_regions_partial_publish_suite(q);
        q.destroy();
    }

    void bulk_regions_staggered_wrap() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        bulk_regions_staggered_wrap_suite(q);
        q.destroy();
    }


    void bulk_regions_partial_publish_matrix() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        bulk_regions_partial_publish_matrix_suite(q);
        q.destroy();
    }

    void pointer_migration_dynamic_resize_CA() {
        spsc::typed_pool<Blob, 0u, spsc::policy::CA<>> q;
        QVERIFY(q.resize(64u));
        pointer_migration_suite_dynamic(q);
        q.destroy();
    }

    void regression_matrix_static_A() {
        spsc::typed_pool<Blob, 16u, spsc::policy::A<>> q;
        regression_matrix_suite(q, "static_A");
        q.destroy();
    }

    void regression_matrix_dynamic_CA() {
        spsc::typed_pool<Blob, 0u, spsc::policy::CA<>> q;
        QVERIFY(q.resize(16u));
        regression_matrix_suite(q, "dynamic_CA");
        q.destroy();
    }


    void pointer_stability_static() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        pointer_stability_suite_static(q);
        q.destroy();
    }

    void pointer_stability_dynamic_noresize() {
        spsc::typed_pool<Blob, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(64u));
        pointer_stability_suite_static(q);
        q.destroy();
    }

    void pointer_migration_dynamic_resize() {
        spsc::typed_pool<Blob, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(64u));
        pointer_migration_suite_dynamic(q);
        q.destroy();
    }

    void regression_matrix_static_P() {
        spsc::typed_pool<Blob, 16u, spsc::policy::P> q;
        regression_matrix_suite(q, "static_P");
        q.destroy();
    }

    void regression_matrix_static_CA() {
        spsc::typed_pool<Blob, 16u, spsc::policy::CA<>> q;
        regression_matrix_suite(q, "static_CA");
        q.destroy();
    }

    void regression_matrix_dynamic_P() {
        spsc::typed_pool<Blob, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(16u));
        regression_matrix_suite(q, "dynamic_P");
        q.destroy();
    }





    void claim_read_consume_scenarios() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        claim_read_consume_scenarios_suite(q);
        q.destroy();

        spsc::typed_pool<Blob, 0u, spsc::policy::CA<>> q2;
        QVERIFY(q2.resize(64u));
        claim_read_consume_scenarios_suite(q2);
        q2.destroy();
    }

    void raii_guards_static()  { spsc::typed_pool<Tracked, 64u, spsc::policy::P> q; tracked_reset(); raii_guards_suite(q); q.destroy(); QCOMPARE(Tracked::live.load(), 0); QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load()); }
    void raii_guards_dynamic() { spsc::typed_pool<Tracked, 0u,  spsc::policy::P> q; tracked_reset(); QVERIFY(q.resize(64u)); raii_guards_suite(q); q.destroy(); QCOMPARE(Tracked::live.load(), 0); QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load()); }

    void iterators_and_indexing_static() { spsc::typed_pool<Blob, 64u, spsc::policy::P> q; fill_seq(q, 1u, 20u); check_iterators_and_indexing(q, 1u, 20u); q.destroy(); }

    void iterators_wraparound_static() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        fill_seq(q, 1u, 62u);
        q.pop(60u);
        fill_seq(q, 1000u, 10u);
        // Now the queue contains [61,62,1000..1009]
        QCOMPARE(q.size(), reg{12});
        QCOMPARE(q[0]->seq, std::uint32_t{61});
        QCOMPARE(q[1]->seq, std::uint32_t{62});
        QCOMPARE(q[2]->seq, std::uint32_t{1000});
        q.destroy();
    }

    void allocator_and_bulk_raii_overloads_static() {
        spsc::typed_pool<Blob, 64u, spsc::policy::P> q;
        QVERIFY(q.is_valid());
        allocator_and_bulk_raii_overloads_suite(q);
        q.destroy();
    }

    void allocator_and_bulk_raii_overloads_dynamic() {
        spsc::typed_pool<Blob, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(64u));
        allocator_and_bulk_raii_overloads_suite(q);
        q.destroy();
    }

    void snapshot_iteration_contract() { spsc::typed_pool<Blob, 64u, spsc::policy::P> q; snapshot_iteration_contract_suite(q); q.destroy(); }

    void bulk_regions_max_count_static() { spsc::typed_pool<Blob, 64u, spsc::policy::P> q; bulk_regions_max_count_suite(q); q.destroy(); }
    void bulk_read_max_count_static()    { spsc::typed_pool<Blob, 64u, spsc::policy::P> q; bulk_read_max_count_suite(q); q.destroy(); }

    void guard_move_semantics_static() {
        tracked_reset();
        spsc::typed_pool<Tracked, 64u, spsc::policy::P> q;
        guard_move_semantics_suite(q);
        q.destroy();
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void resize_edge_cases_dynamic() { resize_edge_cases_dynamic_suite(); }

    void snapshot_consume_contract() { spsc::typed_pool<Blob, 64u, spsc::policy::P> q; snapshot_consume_suite(q); q.destroy(); }
    void snapshot_invalid_pool()     { snapshot_invalid_pool_suite(); }

    void dynamic_move_contract() { dynamic_move_contract_suite(); }

    void consume_all_contract() { consume_all_contract_suite(); }

    void threaded_bulk_regions_atomic_A()  { run_threaded_bulk_regions_suite<spsc::policy::A<>>("threaded_typed_pool_bulk_atomic"); }
    void threaded_bulk_regions_cached_CA() { run_threaded_bulk_regions_suite<spsc::policy::CA<>>("threaded_typed_pool_bulk_cached"); }

    void alignment_sweep() { alignment_sweep_suite(); }
    void stress_cached_ca_transitions() { stress_cached_ca_transitions_suite(); }

    void death_tests_debug_only() { death_tests_debug_only_suite(); }
    void lifecycle_traced()       { lifecycle_traced_suite(); }

    void cleanupTestCase() {}
};

int run_tst_typed_pool_api_paranoid(int argc, char** argv)
{
    tst_typed_pool_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "typed_pool_test.moc"
