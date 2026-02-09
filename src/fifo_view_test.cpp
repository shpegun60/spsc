#include <QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>    // std::memcpy
#include <deque>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>     // std::swap
#include <new>         // std::construct_at, std::destroy_at
#include <vector>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include "fifo_view_test.h"

#if !defined(SPSC_ASSERT) && !defined(NDEBUG)
#  define SPSC_ASSERT(expr) do { if(!(expr)) { std::abort(); } } while(0)
#endif

#include "fifo_view.hpp"

// This test intentionally instantiates/uses the entire public API surface of
// fifo_view.hpp (static + dynamic variants) and stresses edge cases.
// If it fails, it's your code. Not Qt.

namespace spsc_fifo_view_death_detail {

#if !defined(NDEBUG)

static constexpr int kDeathExitCode = 0xAF;

static void sigabrt_handler_(int) noexcept {
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode) {
    std::signal(SIGABRT, &sigabrt_handler_);

    std::array<std::uint32_t, 16u> a{};
    std::array<std::uint32_t, 16u> b{};
    using Q = spsc::fifo_view<std::uint32_t, 16u, spsc::policy::P>;

    if (std::strcmp(mode, "pop_empty") == 0) {
        Q q(a);
        q.pop(); // Must assert: pop on empty.
    } else if (std::strcmp(mode, "front_empty") == 0) {
        Q q(a);
        (void)q.front(); // Must assert: front on empty.
    } else if (std::strcmp(mode, "publish_full") == 0) {
        Q q(a);
        for (std::uint32_t i = 0; i < 16u; ++i) {
            if (!q.try_push(i)) {
                std::_Exit(0xE1);
            }
        }
        q.publish(); // Must assert: publish while full.
    } else if (std::strcmp(mode, "claim_full") == 0) {
        Q q(a);
        for (std::uint32_t i = 0; i < 16u; ++i) {
            if (!q.try_push(i)) {
                std::_Exit(0xE2);
            }
        }
        (void)q.claim(); // Must assert: claim while full.
    } else if (std::strcmp(mode, "bulk_double_emplace_next") == 0) {
        Q q(a);
        auto g = q.scoped_write(1u);
        (void)g.emplace_next(1u);
        (void)g.emplace_next(2u); // Must assert: writing past claimed.
    } else if (std::strcmp(mode, "bulk_arm_publish_unwritten") == 0) {
        Q q(a);
        auto g = q.scoped_write(2u);
        g.arm_publish(); // Must assert: no written elements.
    } else if (std::strcmp(mode, "consume_foreign_snapshot") == 0) {
        Q q1(a);
        Q q2(b);
        if (!q1.try_push(1u)) {
            std::_Exit(0xE3);
        }
        if (!q2.try_push(2u)) {
            std::_Exit(0xE4);
        }
        const auto snap = q2.make_snapshot();
        q1.consume(snap); // Must assert: foreign snapshot identity.
    } else if (std::strcmp(mode, "pop_n_too_many") == 0) {
        Q q(a);
        if (!q.try_push(1u)) {
            std::_Exit(0xE5);
        }
        q.pop(2u); // Must assert: can_read(2) is false.
    } else {
        std::_Exit(0xEF);
    }

    std::_Exit(0xF0);
}

struct Runner_ {
    Runner_() {
        const char* mode = std::getenv("SPSC_FIFO_VIEW_DEATH");
        if (mode && *mode) {
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

#endif // !defined(NDEBUG)

} // namespace spsc_fifo_view_death_detail

namespace {

#if defined(NDEBUG)
constexpr int kFuzzIters = 50'000;
constexpr int kSwapIters = 30'000;
constexpr int kThreadIters = 250'000;
constexpr int kThreadTimeoutMs = 6000;
#else
constexpr int kFuzzIters = 8'000;
constexpr int kSwapIters = 8'000;
constexpr int kThreadIters = 30'000;
constexpr int kThreadTimeoutMs = 15'000;
#endif

static constexpr bool is_pow2(reg x) noexcept {
    return x && ((x & (x - 1u)) == 0u);
}

struct Traced {
    int v{0};

    Traced() noexcept = default;
    explicit Traced(int x) noexcept : v(x) {}

    Traced(const Traced&) = default;
    Traced& operator=(const Traced&) = default;

    ~Traced() noexcept = default;
};

struct LifecycleTracked {
    static inline std::atomic<int> live{0};
    static inline std::atomic<int> ctor{0};
    static inline std::atomic<int> dtor{0};

    int v{0};

    LifecycleTracked() noexcept {
        ctor.fetch_add(1, std::memory_order_relaxed);
        live.fetch_add(1, std::memory_order_relaxed);
    }
    explicit LifecycleTracked(int x) noexcept : v(x) {
        ctor.fetch_add(1, std::memory_order_relaxed);
        live.fetch_add(1, std::memory_order_relaxed);
    }
    LifecycleTracked(const LifecycleTracked& o) noexcept : v(o.v) {
        ctor.fetch_add(1, std::memory_order_relaxed);
        live.fetch_add(1, std::memory_order_relaxed);
    }
    LifecycleTracked& operator=(const LifecycleTracked&) noexcept = default;
    ~LifecycleTracked() noexcept {
        dtor.fetch_add(1, std::memory_order_relaxed);
        live.fetch_sub(1, std::memory_order_relaxed);
    }

    static void reset() noexcept {
        live.store(0, std::memory_order_relaxed);
        ctor.store(0, std::memory_order_relaxed);
        dtor.store(0, std::memory_order_relaxed);
    }
};

template <class... Ts>
struct type_pack final {};

template <class Q>
static void verify_invariants(const Q& q) {
    if (!q.is_valid()) {
        QCOMPARE(q.capacity(), reg{0u});
        QCOMPARE(q.size(), reg{0u});
        QCOMPARE(q.free(), reg{0u});
        QVERIFY(q.empty());
        QVERIFY(q.full());               // Invalid view is treated as non-writable by design.
        QVERIFY(!q.can_write(reg{1u}));
        QVERIFY(!q.can_read(reg{1u}));
        QCOMPARE(q.write_size(), reg{0u});
        QCOMPARE(q.read_size(), reg{0u});

        const auto st = q.state();
        QCOMPARE(st.head, reg{0u});
        QCOMPARE(st.tail, reg{0u});

#if SPSC_HAS_SPAN
        auto sp = q.span();
        QCOMPARE(static_cast<reg>(sp.size()), reg{0u});
#endif
        return;
    }

    const reg cap = q.capacity();
    QVERIFY(cap >= reg{1u});
    QVERIFY(is_pow2(cap));
    QVERIFY(cap <= ::spsc::cap::RB_MAX_UNAMBIGUOUS);

    const reg sz = q.size();
    const reg fr = q.free();

    QVERIFY(sz <= cap);
    QVERIFY(fr <= cap);
    QCOMPARE(sz + fr, cap);

    QCOMPARE(q.empty(), (sz == 0u));
    QCOMPARE(q.full(), (sz == cap));

    QCOMPARE(q.can_write(reg{1u}), (fr >= 1u));
    QCOMPARE(q.can_read(reg{1u}), (sz >= 1u));

    QVERIFY(q.write_size() <= fr);
    QVERIFY(q.write_size() <= cap);
    QVERIFY(q.read_size() <= sz);
    QVERIFY(q.read_size() <= cap);

    QVERIFY(q.data() != nullptr);

    const auto st = q.state();
    QCOMPARE(static_cast<reg>(st.head - st.tail), sz);
}

// Warm up producer/consumer shadow caches by calling query APIs a few times.
// This is intentionally side-effect-free in terms of visible queue state.
template<class Q>
static void shadow_warm_queries(Q& q) {
    // These calls exercise the shadow-refresh paths in SPSCbase when enabled.
    for (int i = 0; i < 8; ++i) {
        (void)q.capacity();
        (void)q.size();
        (void)q.free();
        (void)q.empty();
        (void)q.full();
        (void)q.can_write();
        (void)q.can_read();
        (void)q.write_size();
        (void)q.read_size();
        (void)q.claim_read(1u);
        (void)q.claim_write(1u);
        (void)q.make_snapshot();
    }
}

template <class Q>
static void api_compile_smoke() {
    static_assert(!std::is_copy_constructible_v<Q>, "fifo_view must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<Q>, "fifo_view must not be copy-assignable");
    static_assert(std::is_move_constructible_v<Q>, "fifo_view must be move-constructible");
    static_assert(std::is_move_assignable_v<Q>, "fifo_view must be move-assignable");

    using snapshot_t       = typename Q::snapshot;
    using const_snapshot_t = typename Q::const_snapshot;
    using state_t          = typename Q::state_t;

    [[maybe_unused]] type_pack<
        decltype(std::declval<Q&>().is_valid()),
        decltype(std::declval<Q&>().capacity()),
        decltype(std::declval<Q&>().size()),
        decltype(std::declval<Q&>().free()),
        decltype(std::declval<Q&>().empty()),
        decltype(std::declval<Q&>().full()),
        decltype(std::declval<Q&>().can_write(reg{1u})),
        decltype(std::declval<Q&>().can_read(reg{1u})),
        decltype(std::declval<Q&>().write_size()),
        decltype(std::declval<Q&>().read_size()),
        decltype(std::declval<Q&>().data()),
        decltype(std::declval<const Q&>().data()),
        decltype(std::declval<Q&>().state()),
        decltype(std::declval<const Q&>().state()),
        decltype(std::declval<Q&>().clear()),
        decltype(std::declval<Q&>().reset()),
        decltype(std::declval<Q&>().detach()),
        decltype(std::declval<Q&>().swap(std::declval<Q&>())),
        decltype(swap(std::declval<Q&>(), std::declval<Q&>())),

        decltype(std::declval<Q&>().push(std::declval<const Traced&>())),
        decltype(std::declval<Q&>().try_push(std::declval<const Traced&>())),
        decltype(std::declval<Q&>().try_emplace(1)),
        decltype(std::declval<Q&>().emplace(1)),
        decltype(std::declval<Q&>().front()),
        decltype(std::declval<const Q&>().front()),
        decltype(std::declval<Q&>().try_front()),
        decltype(std::declval<const Q&>().try_front()),
        decltype(std::declval<Q&>().pop()),
        decltype(std::declval<Q&>().pop(reg{1u})),
        decltype(std::declval<Q&>().try_pop()),
        decltype(std::declval<Q&>().try_pop(reg{1u})),

        decltype(std::declval<Q&>().claim()),
        decltype(std::declval<Q&>().publish()),
        decltype(std::declval<Q&>().publish(reg{1u})),
        decltype(std::declval<Q&>().try_claim()),
        decltype(std::declval<Q&>().try_publish()),
        decltype(std::declval<Q&>().try_publish(reg{1u})),
        decltype(std::declval<Q&>().claim_write()),
        decltype(std::declval<Q&>().claim_write(reg{1u})),
        decltype(std::declval<Q&>().claim_write(::spsc::unsafe)),
        decltype(std::declval<Q&>().claim_write(::spsc::unsafe, reg{1u})),
        decltype(std::declval<Q&>().claim_read()),
        decltype(std::declval<Q&>().claim_read(reg{1u})),
        decltype(std::declval<Q&>().claim_read(::spsc::unsafe)),
        decltype(std::declval<Q&>().claim_read(::spsc::unsafe, reg{1u})),

        decltype(std::declval<Q&>().make_snapshot()),
        decltype(std::declval<const Q&>().make_snapshot()),
        decltype(std::declval<Q&>().try_consume(std::declval<const snapshot_t&>())),
        decltype(std::declval<Q&>().try_consume(std::declval<const const_snapshot_t&>())),
        decltype(std::declval<Q&>().consume(std::declval<const snapshot_t&>())),
        decltype(std::declval<Q&>().consume_all()),

        decltype(std::declval<Q&>().scoped_write()),
        decltype(std::declval<Q&>().scoped_write(reg{1u})),
        decltype(std::declval<Q&>().scoped_read()),
        decltype(std::declval<Q&>().scoped_read(reg{1u})),

        state_t
        > _api{};

    using WG = decltype(std::declval<Q&>().scoped_write());
    using RG = decltype(std::declval<Q&>().scoped_read());
    using BWG = decltype(std::declval<Q&>().scoped_write(reg{1u}));
    using BRG = decltype(std::declval<Q&>().scoped_read(reg{1u}));
    static_assert(std::is_move_constructible_v<WG>);
    static_assert(std::is_move_constructible_v<RG>);
    static_assert(std::is_move_constructible_v<BWG>);
    static_assert(std::is_move_constructible_v<BRG>);

#if SPSC_HAS_SPAN
    [[maybe_unused]] type_pack<
        decltype(std::declval<Q&>().span()),
        decltype(std::declval<const Q&>().span())
        > _span_api{};
#endif
}

template <class Q>
static void test_invalid_view_behavior(Q& q) {
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    QVERIFY(!q.try_push(Traced{1}));
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(!q.try_publish());

    {
        auto rr = q.claim_read();
        QVERIFY(rr.empty());
        rr = q.claim_read(reg{8u});
        QVERIFY(rr.empty());

        auto wr = q.claim_write();
        QVERIFY(wr.empty());
        wr = q.claim_write(reg{8u});
        QVERIFY(wr.empty());
    }

#if SPSC_HAS_SPAN
    {
        auto sp = q.span();
        QCOMPARE(static_cast<reg>(sp.size()), reg{0u});
        const Q& cq = q;
        auto csp = cq.span();
        QCOMPARE(static_cast<reg>(csp.size()), reg{0u});
    }
#endif

    {
        auto rg = q.scoped_read();
        QVERIFY(!static_cast<bool>(rg));
        auto wg = q.scoped_write();
        QVERIFY(!static_cast<bool>(wg));

        auto brg = q.scoped_read(reg{8u});
        QVERIFY(!static_cast<bool>(brg));
        auto bwg = q.scoped_write(reg{8u});
        QVERIFY(!static_cast<bool>(bwg));
    }

    {
        auto s = q.make_snapshot();
        QVERIFY(s.size() == 0u);
        QVERIFY(!q.try_consume(s));
    }

    q.detach();
    QVERIFY(!q.is_valid());
    q.reset();
    QVERIFY(!q.is_valid());
    verify_invariants(q);
}

template <class Q>
static void test_introspection_and_data(Q& q) {
    verify_invariants(q);

    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() > 0u);
    QVERIFY(q.data() != nullptr);

    const Q& cq = q;
    QVERIFY(cq.data() != nullptr);

#if SPSC_HAS_SPAN
    auto sp = q.span();
    QCOMPARE(static_cast<reg>(sp.size()), q.capacity());
    auto csp = cq.span();
    QCOMPARE(static_cast<reg>(csp.size()), q.capacity());
#endif
}

template <class Q>
static void test_push_pop_front_try(Q& q) {
    q.clear();
    verify_invariants(q);

    QVERIFY(q.empty());
    QVERIFY(!q.try_pop());
    QVERIFY(q.try_front() == nullptr);

    QVERIFY(q.try_push(Traced{1}));
    QVERIFY(!q.empty());
    QCOMPARE(q.front().v, 1);
    QCOMPARE(q.try_front()->v, 1);

    QVERIFY(q.try_push(Traced{2}));
    QCOMPARE(q.size(), reg{2u});
    QCOMPARE(q.front().v, 1);

    q.pop();
    QCOMPARE(q.front().v, 2);
    q.pop();
    QVERIFY(q.empty());
    verify_invariants(q);

    QVERIFY(!q.try_pop(reg{1u}));
    QVERIFY(q.try_push(Traced{10}));
    QVERIFY(q.try_push(Traced{11}));
    QVERIFY(q.try_pop(reg{2u}));
    QVERIFY(q.empty());

    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) {
        QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
    }
    QVERIFY(q.full());
    QVERIFY(!q.try_push(Traced{999}));
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(!q.try_publish());
    QVERIFY(q.claim_write().empty());

    for (reg i = 0; i < cap; ++i) {
        QCOMPARE(q.front().v, static_cast<int>(i));
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_emplace(Q& q) {
    q.clear();

    auto& r = q.emplace(7);
    QCOMPARE(r.v, 7);
    QCOMPARE(q.front().v, 7);
    q.pop();
    QVERIFY(q.empty());

    auto* p = q.try_emplace(9);
    QVERIFY(p != nullptr);
    QCOMPARE(p->v, 9);
    QCOMPARE(q.front().v, 9);
    q.pop();
    QVERIFY(q.empty());

    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) QVERIFY(q.try_push(Traced{1}));
    QVERIFY(q.full());
    QVERIFY(q.try_emplace(123) == nullptr);
    q.clear();
}

template <class Q>
static void test_claim_publish(Q& q) {
    q.clear();

    auto& slot = q.claim();
    slot = Traced{7};
    q.publish();
    QCOMPARE(q.front().v, 7);

    q.pop();
    QVERIFY(q.empty());

    auto* w = q.try_claim();
    QVERIFY(w != nullptr);
    *w = Traced{8};
    QVERIFY(q.try_publish());
    QCOMPARE(q.front().v, 8);
    q.pop();
    QVERIFY(q.empty());

    {
        const reg too_big = q.free() + reg{1u};
        QVERIFY(!q.try_publish(too_big));
        QVERIFY(q.empty());
    }

    {
        const reg n = reg{4u};
        QVERIFY(q.free() >= n);

        auto wr = q.claim_write(n);
        QCOMPARE(wr.total, n);

        int v = 1000;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

        QVERIFY(q.try_publish(wr.total));
        QCOMPARE(q.size(), n);

        auto rr = q.claim_read(reg{10u});
        QCOMPARE(rr.total, n);
        q.pop(rr.total);
        QVERIFY(q.empty());
    }
}

template <class Q>
static void test_indexing_and_iterators(Q& q) {
    q.clear();

    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
    q.pop(cap - 3u);
    QVERIFY(q.try_push(Traced{1000}));
    QVERIFY(q.try_push(Traced{1001}));
    QVERIFY(q.try_push(Traced{1002}));
    QCOMPARE(q.size(), reg{6u});

    QCOMPARE(q[0u].v, static_cast<int>(cap - 3u));
    QCOMPARE(q[1u].v, static_cast<int>(cap - 2u));
    QCOMPARE(q[2u].v, static_cast<int>(cap - 1u));
    QCOMPARE(q[3u].v, 1000);

    {
        const Q& cq = q;
        QCOMPARE(cq[0u].v, static_cast<int>(cap - 3u));
        QCOMPARE(cq[3u].v, 1000);
    }

    {
        std::vector<int> got;
        for (auto it = q.begin(); it != q.end(); ++it) got.push_back(it->v);
        QCOMPARE(static_cast<int>(got.size()), 6);
        QCOMPARE(got.front(), static_cast<int>(cap - 3u));
        QCOMPARE(got.back(), 1002);
    }

    {
        const Q& cq = q;
        QCOMPARE(cq.cbegin()->v, static_cast<int>(cap - 3u));
    }

    {
        std::vector<int> got;
        for (auto it = q.rbegin(); it != q.rend(); ++it) got.push_back(it->v);
        QCOMPARE(got.front(), 1002);
        QCOMPARE(got.back(), static_cast<int>(cap - 3u));
    }

    q.clear();
    QVERIFY(q.empty());

    {
        const Q& cq = q;
        QVERIFY(cq.cbegin() == cq.cend());
        QVERIFY(cq.crbegin() == cq.crend());
    }
}

template <class Q>
static void test_bulk_regions(Q& q) {
    q.clear();

    {
        auto rr = q.claim_read();
        QVERIFY(rr.empty());
        rr = q.claim_read(reg{10u});
        QVERIFY(rr.empty());
    }

    {
        auto wr0 = q.claim_write(reg{0u});
        QVERIFY(wr0.empty());

        auto rr0 = q.claim_read(reg{0u});
        QVERIFY(rr0.empty());
    }

    {
        auto wr = q.claim_write(reg{5u});
        QVERIFY(wr.total <= q.free());
        QVERIFY(wr.total > 0u);
        QVERIFY(wr.first.ptr != nullptr);
        QVERIFY(wr.first.count > 0u);

        int v = 10;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

        q.publish(wr.total);
        QCOMPARE(q.size(), wr.total);
    }

    {
        auto rr = q.claim_read();
        QVERIFY(rr.total > 0u);
        std::vector<int> got;
        got.reserve(rr.total);
        for (reg i = 0; i < rr.first.count; ++i) got.push_back(rr.first.ptr[i].v);
        for (reg i = 0; i < rr.second.count; ++i) got.push_back(rr.second.ptr[i].v);

        for (int i = 1; i < static_cast<int>(got.size()); ++i) {
            QCOMPARE(got[i], got[i - 1] + 1);
        }

        q.pop(rr.total);
        QVERIFY(q.empty());
    }

    {
        q.clear();
        auto wr = q.claim_write(q.capacity() * 2u);
        QVERIFY(!wr.empty());
        QCOMPARE(wr.total, q.free());
        QCOMPARE(wr.first.count + wr.second.count, wr.total);
    }

    {
        q.clear();
        auto wr = q.claim_write();
        QVERIFY(!wr.empty());
        QCOMPARE(wr.total, q.free());
        QCOMPARE(wr.first.count + wr.second.count, wr.total);
        QVERIFY(wr.total <= q.capacity());
    }

    {
        q.clear();
        const reg N = reg{6u};
        const reg K = reg{3u};
        QVERIFY(q.free() >= N);

        auto wr = q.claim_write(N);
        QCOMPARE(wr.total, N);

        int v = 500;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

        q.publish(reg{0u});
        QCOMPARE(q.size(), reg{0u});

        q.publish(K);
        QCOMPARE(q.size(), K);

        auto rr = q.claim_read(reg{10u});
        QCOMPARE(rr.total, K);

        std::vector<int> got;
        got.reserve(rr.total);
        for (reg i = 0; i < rr.first.count; ++i) got.push_back(rr.first.ptr[i].v);
        for (reg i = 0; i < rr.second.count; ++i) got.push_back(rr.second.ptr[i].v);
        QCOMPARE(static_cast<int>(got.size()), 3);
        QCOMPARE(got[0], 500);
        QCOMPARE(got[1], 501);
        QCOMPARE(got[2], 502);

        q.pop(rr.total);
        QVERIFY(q.empty());
    }

    {
        q.clear();
        const reg N = reg{6u};
        const reg K = reg{3u};
        QVERIFY(q.free() >= N);

        auto wr = q.claim_write(N);
        QCOMPARE(wr.total, N);

        int v = 700;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};
        q.publish(N);
        QCOMPARE(q.size(), N);

        auto rr = q.claim_read(N);
        QCOMPARE(rr.total, N);

        q.pop(K);
        QCOMPARE(q.size(), N - K);
        QCOMPARE(q.front().v, 703);

        for (int expect = 703; expect <= 705; ++expect) {
            QCOMPARE(q.front().v, expect);
            q.pop();
        }
        QVERIFY(q.empty());
    }

    {
        const reg cap = q.capacity();
        QVERIFY(cap >= 8u);
        q.clear();

        const reg fill = cap / 2u;
        for (reg i = 0; i < fill; ++i) QVERIFY(q.try_push(Traced{100 + static_cast<int>(i)}));
        q.pop();

        auto wr = q.claim_write();
        QVERIFY(wr.total > 0u);
        QVERIFY(wr.first.count > 0u);
        QVERIFY(wr.second.count > 0u);
        QCOMPARE(wr.first.count + wr.second.count, wr.total);

        QCOMPARE(q.write_size(), wr.first.count);
    }

    {
        const reg cap = q.capacity();
        QVERIFY(cap >= 8u);
        q.clear();

        for (reg i = 0; i < cap; ++i) QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
        q.pop(cap - 2u);
        QVERIFY(q.try_push(Traced{2000}));
        QVERIFY(q.try_push(Traced{2001}));
        QVERIFY(q.try_push(Traced{2002}));
        QVERIFY(q.try_push(Traced{2003}));

        auto rr = q.claim_read();
        QCOMPARE(rr.total, reg{6u});
        QCOMPARE(q.read_size(), rr.first.count);
        QVERIFY(rr.first.count > 0u);
        QVERIFY(rr.second.count > 0u);

        q.pop(rr.total);
        QVERIFY(q.empty());
    }
}

template <class Q>
static void test_snapshots(Q& q) {
    q.clear();

    for (int i = 0; i < 10; ++i) QVERIFY(q.try_push(Traced{i}));

    auto s = q.make_snapshot();
    QVERIFY(s.size() == 10u);

    {
        const Q& cq = q;
        auto cs = cq.make_snapshot(); // will instantiate const make_snapshot()
        QCOMPARE(cs.size(), reg{10u});
        int last = -1;
        for (auto it = cs.begin(); it != cs.end(); ++it) last = it->v;
        QCOMPARE(last, 9);
    }

    {
        int expected = 0;
        for (auto it = s.begin(); it != s.end(); ++it) {
            QCOMPARE(it->v, expected++);
        }
    }

    QVERIFY(q.try_consume(s));
    QVERIFY(q.empty());

    {
        for (int i = 0; i < 3; ++i) QVERIFY(q.try_push(Traced{200 + i}));
        auto s3 = q.make_snapshot();
        q.consume(s3);
        QVERIFY(q.empty());
    }

    for (int i = 0; i < 4; ++i) QVERIFY(q.try_push(Traced{100 + i}));
    auto s2 = q.make_snapshot();
    q.pop();
    QVERIFY(!q.try_consume(s2));

    {
        using V = typename Q::value_type;
        using P = typename Q::policy_type;

        std::array<V, 16> other_buf{};
        spsc::fifo_view<V, 16, P> other(other_buf, P{});
        QVERIFY(other.is_valid());
        QVERIFY(other.try_push(Traced{1}));
        auto os = other.make_snapshot();
        QVERIFY(!q.try_consume(os));
    }

    q.clear();
    QVERIFY(q.try_push(Traced{7}));
    QVERIFY(q.try_push(Traced{8}));
    q.consume_all();
    QVERIFY(q.empty());
}

template <class Q>
static void test_raii_guards(Q& q) {
    q.clear();

    {
        auto rg = q.scoped_read();
        QVERIFY(!static_cast<bool>(rg));
    }

    {
        QVERIFY(q.try_push(Traced{9}));
        {
            auto rg = q.scoped_read();
            QVERIFY(static_cast<bool>(rg));
            QCOMPARE(rg->v, 9);
        }
        QVERIFY(q.empty());
    }

    {
        auto wg = q.scoped_write();
        QVERIFY(static_cast<bool>(wg));
        auto* p = wg.peek();
        QVERIFY(p != nullptr);
        *p = Traced{1};
        wg.cancel();
    }
    QVERIFY(q.empty());

    {
        auto wg = q.scoped_write();
        QVERIFY(static_cast<bool>(wg));
        (*wg).v = 2;
    }
    QCOMPARE(q.size(), reg{1u});
    QCOMPARE(q.front().v, 2);

    {
        auto rg = q.scoped_read();
        QVERIFY(static_cast<bool>(rg));
        QCOMPARE((*rg).v, 2);
        rg.cancel();
    }
    QCOMPARE(q.size(), reg{1u});

    {
        auto rg = q.scoped_read();
        QVERIFY(static_cast<bool>(rg));
        rg.commit();
    }
    QVERIFY(q.empty());

    {
        auto wg = q.scoped_write();
        QVERIFY(static_cast<bool>(wg));
        wg->v = 3;
    }
    QCOMPARE(q.front().v, 3);
    q.pop();
    QVERIFY(q.empty());

    {
        auto wg1 = q.scoped_write();
        QVERIFY(static_cast<bool>(wg1));
        auto wg2 = std::move(wg1);
        QVERIFY(static_cast<bool>(wg2));
        wg2.ref() = Traced{42};
        wg2.commit();
    }
    QCOMPARE(q.front().v, 42);
    q.pop();
    QVERIFY(q.empty());

    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
    {
        auto wg = q.scoped_write();
        QVERIFY(!static_cast<bool>(wg));
    }
    q.clear();
}

template <class Q>
static void test_bulk_raii_overloads(Q& q) {
    q.clear();

    {
        auto bw = q.scoped_write(5u);
        QVERIFY(static_cast<bool>(bw));
        QCOMPARE(bw.claimed(), reg{5u});
        QCOMPARE(bw.constructed(), reg{0u});
        QCOMPARE(bw.remaining(), reg{5u});

        auto* p1 = bw.emplace_next(10);
        auto* p2 = bw.emplace_next(11);
        QVERIFY(p1 != nullptr);
        QVERIFY(p2 != nullptr);
        QCOMPARE(bw.constructed(), reg{2u});
        QCOMPARE(bw.remaining(), reg{3u});

        bw.disarm_publish();
    }
    QVERIFY(q.empty());

    {
        auto bw = q.scoped_write(1u);
        QVERIFY(static_cast<bool>(bw));
        auto* slot = bw.get_next();
        QVERIFY(slot != nullptr);
        slot->v = 99;
        bw.mark_written();
        QCOMPARE(bw.constructed(), reg{1u});
        bw.commit();
    }
    QCOMPARE(q.size(), reg{1u});
    QCOMPARE(q.front().v, 99);
    q.pop();
    QVERIFY(q.empty());

    {
        auto bw = q.scoped_write(4u);
        QVERIFY(static_cast<bool>(bw));
        for (int v = 100; v < 104; ++v) {
            auto* p = bw.emplace_next(v);
            QVERIFY(p != nullptr);
            QCOMPARE(p->v, v);
        }
        QCOMPARE(bw.constructed(), reg{4u});
        bw.commit();
        QVERIFY(!static_cast<bool>(bw));
    }
    QCOMPARE(q.size(), reg{4u});

    {
        auto bw1 = q.scoped_write(2u);
        QVERIFY(static_cast<bool>(bw1));
        auto bw2 = std::move(bw1);
        QVERIFY(!static_cast<bool>(bw1));
        QVERIFY(static_cast<bool>(bw2));
        (void)bw2.write_next(Traced{104});
        (void)bw2.write_next(Traced{105});
        bw2.arm_publish();
    }
    QCOMPARE(q.size(), reg{6u});

    {
        auto br = q.scoped_read(3u);
        QVERIFY(static_cast<bool>(br));
        QCOMPARE(br.count(), reg{3u});
        const auto& r = br.regions_view();

        std::vector<int> vals;
        vals.reserve(static_cast<std::size_t>(r.total));
        for (reg i = 0; i < r.first.count; ++i) {
            vals.push_back(r.first.ptr[i].v);
        }
        for (reg i = 0; i < r.second.count; ++i) {
            vals.push_back(r.second.ptr[i].v);
        }
        QCOMPARE(vals.size(), std::size_t{3u});
        QCOMPARE(vals[0], 100);
        QCOMPARE(vals[1], 101);
        QCOMPARE(vals[2], 102);

        br.cancel();
        QVERIFY(!static_cast<bool>(br));
    }
    QCOMPARE(q.size(), reg{6u});

    {
        auto br = q.scoped_read(4u);
        QVERIFY(static_cast<bool>(br));
        QCOMPARE(br.count(), reg{4u});
        br.commit();
        QVERIFY(!static_cast<bool>(br));
    }
    QCOMPARE(q.size(), reg{2u});
    QCOMPARE(q.front().v, 104);

    {
        auto br1 = q.scoped_read(2u);
        QVERIFY(static_cast<bool>(br1));
        auto br2 = std::move(br1);
        QVERIFY(!static_cast<bool>(br1));
        QVERIFY(static_cast<bool>(br2));
        QCOMPARE(br2.count(), reg{2u});
    }
    QVERIFY(q.empty());

    {
        auto bw = q.scoped_write(0u);
        QVERIFY(!static_cast<bool>(bw));
    }
    {
        auto br = q.scoped_read(0u);
        QVERIFY(!static_cast<bool>(br));
    }
}

template <class Q>
static void paranoid_random_fuzz(Q& q, int seed, int iters) {
    q.clear();
    std::deque<int> model;
    std::mt19937 rng(static_cast<std::uint32_t>(seed));

    auto push_one = [&](int value) {
        if (q.try_push(Traced{value})) {
            model.push_back(value);
        } else {
            QVERIFY(q.full());
            QVERIFY(!q.can_write(reg{1u}));
        }
    };

    auto pop_one = [&]() {
        if (model.empty()) {
            QVERIFY(q.empty());
            QVERIFY(!q.try_pop());
            return;
        }
        QCOMPARE(q.front().v, model.front());
        q.pop();
        model.pop_front();
    };

    for (int step = 0; step < iters; ++step) {
        verify_invariants(q);

        QCOMPARE(q.size(), static_cast<reg>(model.size()));
        QCOMPARE(q.empty(), model.empty());

        const int op = static_cast<int>(rng() % 10u);

        switch (op) {
        case 0: { push_one(static_cast<int>(rng() & 0x7FFFu)); } break;
        case 1: { pop_one(); } break;

        case 2: { // bulk write
            const reg want = static_cast<reg>((rng() % 8u) + 1u);
            auto wr = q.claim_write(want);
            if (wr.total == 0u) {
                QVERIFY(q.full() || !q.is_valid());
                break;
            }
            int base = static_cast<int>(rng() & 0x3FFFu);
            int v = base;
            for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
            for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

            q.publish(wr.total);
            for (reg i = 0; i < wr.total; ++i) model.push_back(base + static_cast<int>(i));
        } break;

        case 3: { // bulk read
            const reg want = static_cast<reg>((rng() % 8u) + 1u);
            auto rr = q.claim_read(want);
            if (rr.total == 0u) {
                QVERIFY(model.empty());
                QVERIFY(q.empty());
                break;
            }
            reg consumed = 0u;
            for (reg i = 0; i < rr.first.count; ++i) {
                QCOMPARE(rr.first.ptr[i].v, model[static_cast<std::size_t>(consumed)]);
                ++consumed;
            }
            for (reg i = 0; i < rr.second.count; ++i) {
                QCOMPARE(rr.second.ptr[i].v, model[static_cast<std::size_t>(consumed)]);
                ++consumed;
            }
            QCOMPARE(consumed, rr.total);

            q.pop(rr.total);
            for (reg i = 0; i < rr.total; ++i) model.pop_front();
        } break;

        case 4: { // snapshot verify
            auto s = q.make_snapshot();
            QCOMPARE(static_cast<std::size_t>(s.size()), model.size());
            std::size_t i = 0;
            for (auto it = s.begin(); it != s.end(); ++it, ++i) {
                QCOMPARE(it->v, model[i]);
            }
        } break;

        case 5: { // snapshot consume deterministic in single-thread
            auto s = q.make_snapshot();
            QVERIFY(q.try_consume(s));
            model.clear();
            QVERIFY(q.empty());
        } break;

        case 6: { // guards
            if (!q.full()) {
                auto wg = q.scoped_write();
                if (wg) {
                    const int v = static_cast<int>(rng() & 0x1FFFu);
                    wg.ref() = Traced{v};
                    if ((rng() & 1u) == 0u) { wg.commit(); model.push_back(v); }
                    else { wg.cancel(); }
                }
            }
            if (!q.empty()) {
                auto rg = q.scoped_read();
                if (rg) {
                    QCOMPARE(rg->v, model.front());
                    if ((rng() & 1u) == 0u) { rg.commit(); model.pop_front(); }
                    else { rg.cancel(); }
                }
            }
        } break;

        case 7: { // clear/reset
            if ((rng() & 15u) == 0u) { q.clear(); model.clear(); }
            if ((rng() & 63u) == 0u) { q.reset(); model.clear(); }
        } break;

        case 8: { // front/try_front
            if (model.empty()) {
                QVERIFY(q.try_front() == nullptr);
            } else {
                auto* p = q.try_front();
                QVERIFY(p != nullptr);
                QCOMPARE(p->v, model.front());
            }
        } break;

        case 9: { // try_pop(n)
            const reg n = static_cast<reg>((rng() % 4u) + 1u);
            const bool ok = q.try_pop(n);
            if (!ok) {
                QVERIFY(model.size() < static_cast<std::size_t>(n));
            } else {
                for (reg i = 0; i < n; ++i) model.pop_front();
            }
        } break;

        default:
            break;
        }
    }

    while (!model.empty()) {
        QCOMPARE(q.front().v, model.front());
        q.pop();
        model.pop_front();
    }
    QVERIFY(q.empty());
    verify_invariants(q);
}


// ------------------------------ shadow cache regressions ------------------------------

template <class Q>
struct fifo_view_type_traits;

template <class T, reg Cap, class Policy>
struct fifo_view_type_traits<::spsc::fifo_view<T, Cap, Policy>> {
    using value_type  = T;
    using policy_type = Policy;
    static constexpr reg kCap = Cap;
    static constexpr bool kDynamic = (Cap == 0u);
};

template <class Q>
static void shadow_warm(Q& q) {
    // Touch both consumer-side and producer-side paths so shadows become non-trivial.
    (void)q.empty();
    (void)q.read_size();
    (void)q.can_read(reg{1u});

    (void)q.full();
    (void)q.write_size();
    (void)q.can_write(reg{1u});
}

template <class Q>
static void test_shadow_sync_regressions_impl(std::false_type /*static_cap*/) {
    using Traits = fifo_view_type_traits<Q>;
    using V = typename Traits::value_type;
    using P = typename Traits::policy_type;

    // Swap regression (static).
    {
        std::array<V, static_cast<std::size_t>(Traits::kCap)> bufA{};
        std::array<V, static_cast<std::size_t>(Traits::kCap)> bufB{};

        Q a(bufA, P{});
        Q b(bufB, P{});
        QVERIFY(a.is_valid());
        QVERIFY(b.is_valid());

        QVERIFY(a.try_push(Traced{1}));
        QVERIFY(a.try_push(Traced{2}));
        shadow_warm(a);
        verify_invariants(a);

        QVERIFY(b.try_push(Traced{9}));
        shadow_warm(b);
        verify_invariants(b);

        a.swap(b);
        verify_invariants(a);
        verify_invariants(b);

        QCOMPARE(a.front().v, 9);
        a.pop();
        QVERIFY(a.empty());

        QCOMPARE(b.front().v, 1);
        b.pop();
        QCOMPARE(b.front().v, 2);
        b.pop();
        QVERIFY(b.empty());
    }

    // Move regression (static).
    {
        std::array<V, static_cast<std::size_t>(Traits::kCap)> bufC{};

        Q msrc(bufC, P{});
        QVERIFY(msrc.try_push(Traced{7}));
        QVERIFY(msrc.try_push(Traced{8}));
        shadow_warm(msrc);
        verify_invariants(msrc);

        Q mdst(std::move(msrc));
        verify_invariants(mdst);

        QCOMPARE(mdst.front().v, 7);
        mdst.pop();
        QCOMPARE(mdst.front().v, 8);
        mdst.pop();
        QVERIFY(mdst.empty());
        verify_invariants(mdst);

#if !defined(__clang_analyzer__)
        QVERIFY(!msrc.is_valid());
#endif
    }
}

template <class Q>
static void test_shadow_sync_regressions_impl(std::true_type /*dynamic_cap*/) {
    using V = typename fifo_view_type_traits<Q>::value_type;
    using P = typename fifo_view_type_traits<Q>::policy_type;

    // Swap regression (dynamic).
    {
        std::vector<V> bufA(64);
        std::vector<V> bufB(64);

        Q a(bufA.data(), static_cast<reg>(bufA.size()), P{});
        Q b(bufB.data(), static_cast<reg>(bufB.size()), P{});
        QVERIFY(a.is_valid());
        QVERIFY(b.is_valid());
        QCOMPARE(a.capacity(), reg{64u});
        QCOMPARE(b.capacity(), reg{64u});

        QVERIFY(a.try_push(Traced{1}));
        QVERIFY(a.try_push(Traced{2}));
        shadow_warm(a);
        verify_invariants(a);

        QVERIFY(b.try_push(Traced{9}));
        shadow_warm(b);
        verify_invariants(b);

        a.swap(b);
        verify_invariants(a);
        verify_invariants(b);

        QCOMPARE(a.front().v, 9);
        a.pop();
        QVERIFY(a.empty());

        QCOMPARE(b.front().v, 1);
        b.pop();
        QCOMPARE(b.front().v, 2);
        b.pop();
        QVERIFY(b.empty());
    }

    // Move regression (dynamic).
    {
        std::vector<V> bufC(64);

        Q msrc(bufC.data(), static_cast<reg>(bufC.size()), P{});
        QVERIFY(msrc.try_push(Traced{7}));
        QVERIFY(msrc.try_push(Traced{8}));
        shadow_warm(msrc);
        verify_invariants(msrc);

        Q mdst(std::move(msrc));
        verify_invariants(mdst);

        QCOMPARE(mdst.front().v, 7);
        mdst.pop();
        QCOMPARE(mdst.front().v, 8);
        mdst.pop();
        QVERIFY(mdst.empty());
        verify_invariants(mdst);

#if !defined(__clang_analyzer__)
        QVERIFY(!msrc.is_valid());
#endif
    }
}

template <class Q>
static void test_shadow_sync_regressions() {
    using P = typename fifo_view_type_traits<Q>::policy_type;

    // Only meaningful when shadow indices are enabled for this policy.
    if constexpr (!::spsc::detail::rb_use_shadow_v<P>) {
        return;
    }

    test_shadow_sync_regressions_impl<Q>(std::bool_constant<fifo_view_type_traits<Q>::kDynamic>{});
}

// ------------------------------ fifo_view specific: attachment/state ------------------------------

template <class Q>
static void test_static_attachment_state_move_swap() {
    using V = typename Q::value_type;
    using P = typename Q::policy_type;

    std::array<V, 16> bufA{};
    std::array<V, 16> bufB{};

    // CTAD sanity (static)
    {
        auto q1 = spsc::fifo_view(bufA, P{});
        static_assert(std::is_same_v<decltype(q1), spsc::fifo_view<V, 16, P>>);
        (void)q1;
    }

    Q q;
    QVERIFY(!q.is_valid());
    test_invalid_view_behavior(q);

    QVERIFY(q.attach(bufA));
    QVERIFY(q.is_valid());
    QCOMPARE(q.capacity(), reg{16u});
    verify_invariants(q);

    QVERIFY(q.try_push(Traced{1}));
    QVERIFY(q.try_push(Traced{2}));
    QVERIFY(q.try_push(Traced{3}));
    const auto st = q.state();
    QCOMPARE(static_cast<reg>(st.head - st.tail), reg{3u});

    q.detach();
    QVERIFY(!q.is_valid());

    QVERIFY(q.attach(bufA.data(), st));
    QVERIFY(q.is_valid());
    QCOMPARE(q.size(), reg{3u});
    QCOMPARE(q.front().v, 1);
    q.pop();
    QCOMPARE(q.front().v, 2);
    q.pop();
    QCOMPARE(q.front().v, 3);
    q.pop();
    QVERIFY(q.empty());

    q.detach();
    QVERIFY(!q.is_valid());

    bufA[15] = Traced{100};
    bufA[0]  = Traced{101};
    bufA[1]  = Traced{102};

    QVERIFY(q.adopt(bufA.data(), reg{18u}, reg{15u}));
    QVERIFY(q.is_valid());
    QCOMPARE(q.size(), reg{3u});
    QCOMPARE(q.front().v, 100);
    q.pop();
    QCOMPARE(q.front().v, 101);
    q.pop();
    QCOMPARE(q.front().v, 102);
    q.pop();
    QVERIFY(q.empty());

    QVERIFY(!q.adopt(bufA.data(), reg{100u}, reg{0u}));
    QVERIFY(!q.is_valid());

    Q a(bufA, P{});
    Q b(bufB, P{});

    QVERIFY(a.try_push(Traced{1}));
    QVERIFY(a.try_push(Traced{2}));
    QVERIFY(b.try_push(Traced{9}));

    a.swap(b);

    QCOMPARE(a.front().v, 9);
    a.pop();
    QVERIFY(a.empty());

    QCOMPARE(b.front().v, 1);
    b.pop();
    QCOMPARE(b.front().v, 2);
    b.pop();
    QVERIFY(b.empty());

    Q inv;
    QVERIFY(!inv.is_valid());

    QVERIFY(a.attach(bufA));
    QVERIFY(a.try_push(Traced{77}));
    swap(a, inv);

    QVERIFY(!a.is_valid());
    QVERIFY(inv.is_valid());
    QCOMPARE(inv.front().v, 77);
    inv.pop();

    Q msrc(bufA, P{});
    QVERIFY(msrc.try_push(Traced{5}));
    auto* p_old = msrc.data();

    Q mdst(std::move(msrc));
    QVERIFY(mdst.is_valid());
    QCOMPARE(mdst.data(), p_old);
    QCOMPARE(mdst.front().v, 5);

#if !defined(__clang_analyzer__)
    QVERIFY(!msrc.is_valid());
#endif
}

template <class Q>
static void test_dynamic_attachment_state_move_swap() {
    using V = typename Q::value_type;
    using P = typename Q::policy_type;

    std::vector<V> bufA(24); // Non-pow2 -> fifo_view floors to pow2 (16)
    std::vector<V> bufB(64);

    // CTAD sanity (dynamic)
    {
        auto q1 = spsc::fifo_view(bufB.data(), reg{64u}, P{});
        static_assert(std::is_same_v<decltype(q1), spsc::fifo_view<V, 0u, P>>);
        (void)q1;
    }

    Q q;
    QVERIFY(!q.is_valid());
    test_invalid_view_behavior(q);

    QVERIFY(!q.attach(nullptr, reg{16u}));
    QVERIFY(!q.is_valid());

    QVERIFY(!q.attach(bufA.data(), reg{0u}));
    QVERIFY(!q.is_valid());

    QVERIFY(q.attach(bufA.data(), static_cast<reg>(bufA.size())));
    QVERIFY(q.is_valid());
    QCOMPARE(q.capacity(), reg{16u});
    verify_invariants(q);

    QVERIFY(q.try_push(Traced{1}));
    QVERIFY(q.try_push(Traced{2}));
    QVERIFY(q.try_push(Traced{3}));
    const auto st = q.state();
    QCOMPARE(static_cast<reg>(st.head - st.tail), reg{3u});

    q.detach();
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    // Dynamic attach(buffer, state) without capacity must fail (by design).
    QVERIFY(!q.attach(bufA.data(), st));
    QVERIFY(!q.is_valid());

    QVERIFY(q.adopt(bufA.data(), static_cast<reg>(bufA.size()), st.head, st.tail));
    QVERIFY(q.is_valid());
    QCOMPARE(q.capacity(), reg{16u});

    QCOMPARE(q.front().v, 1);
    q.pop();
    QCOMPARE(q.front().v, 2);
    q.pop();
    QCOMPARE(q.front().v, 3);
    q.pop();
    QVERIFY(q.empty());

    QVERIFY(!q.adopt(bufA.data(), static_cast<reg>(bufA.size()), reg{100u}, reg{0u}));
    QVERIFY(!q.is_valid());

    Q a(bufA.data(), static_cast<reg>(bufA.size()), P{});
    Q b(bufB.data(), static_cast<reg>(bufB.size()), P{});
    QVERIFY(a.is_valid());
    QVERIFY(b.is_valid());
    QCOMPARE(a.capacity(), reg{16u});
    QCOMPARE(b.capacity(), reg{64u});

    QVERIFY(a.try_push(Traced{1}));
    QVERIFY(a.try_push(Traced{2}));
    QVERIFY(b.try_push(Traced{9}));

    a.swap(b);

    QCOMPARE(a.capacity(), reg{64u});
    QCOMPARE(b.capacity(), reg{16u});

    QCOMPARE(a.front().v, 9);
    a.pop();
    QVERIFY(a.empty());

    QCOMPARE(b.front().v, 1);
    b.pop();
    QCOMPARE(b.front().v, 2);
    b.pop();
    QVERIFY(b.empty());

    Q inv;
    QVERIFY(!inv.is_valid());

    QVERIFY(a.attach(bufB.data(), static_cast<reg>(bufB.size())));
    QVERIFY(a.try_push(Traced{77}));
    swap(a, inv);

    QVERIFY(!a.is_valid());
    QVERIFY(inv.is_valid());
    QCOMPARE(inv.front().v, 77);
    inv.pop();

    Q msrc(bufB.data(), static_cast<reg>(bufB.size()), P{});
    QVERIFY(msrc.try_push(Traced{5}));
    const auto cap_old = msrc.capacity();
    auto* p_old = msrc.data();

    Q mdst(std::move(msrc));
    QVERIFY(mdst.is_valid());
    QCOMPARE(mdst.data(), p_old);
    QCOMPARE(mdst.capacity(), cap_old);
    QCOMPARE(mdst.front().v, 5);

#if !defined(__clang_analyzer__)
    QVERIFY(!msrc.is_valid());
#endif
}



// ------------------------------ ALIGNMENT SWEEP + SHADOW REGRESSIONS (EXTRA HARD) ------------------------------

namespace align_sweep {

// A trivially copyable payload with configurable alignment.
// We never dereference misaligned storage: the fifo_view must reject it (constructor/attach).

template<std::size_t Align>
struct alignas(Align) Blob final {
    // Keep members byte-aligned so we can legally request Align=2.
    // Payload is validated via memcpy to avoid UB on exotic alignments.
    std::array<std::byte, 32> payload{};

    RB_FORCEINLINE void set(std::uint32_t s, std::uint32_t t) noexcept {
        const std::uint32_t seq = s;
        const std::uint32_t inv = ~s;
        const std::uint32_t tag = t;
        const std::uint32_t pad = s ^ t ^ 0xA5A5A5A5u;
        std::memcpy(payload.data() + 0,  &seq, sizeof(seq));
        std::memcpy(payload.data() + 4,  &inv, sizeof(inv));
        std::memcpy(payload.data() + 8,  &tag, sizeof(tag));
        std::memcpy(payload.data() + 12, &pad, sizeof(pad));
    }

    RB_FORCEINLINE void verify(std::uint32_t s, std::uint32_t t) const noexcept {
        std::uint32_t seq = 0, inv = 0, tag = 0, pad = 0;
        std::memcpy(&seq, payload.data() + 0,  sizeof(seq));
        std::memcpy(&inv, payload.data() + 4,  sizeof(inv));
        std::memcpy(&tag, payload.data() + 8,  sizeof(tag));
        std::memcpy(&pad, payload.data() + 12, sizeof(pad));
        QCOMPARE(seq, s);
        QCOMPARE(inv, ~s);
        QCOMPARE(tag, t);
        QCOMPARE(pad, (s ^ t ^ 0xA5A5A5A5u));
    }
};

template<class T>
static void construct_n(T* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        std::construct_at(p + i);
    }
}

template<class T>
static void destroy_n(T* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        std::destroy_at(p + i);
    }
}

// Run a small write/read cycle to ensure the queue actually works on this storage.
template<class Q>
static void sanity_rw(Q& q) {
    verify_invariants(q);

    // Fill half.
    const reg cap = q.capacity();
    const reg n   = cap / 2u;

    for (reg i = 0; i < n; ++i) {
        auto* slot = q.try_claim();
        QVERIFY(slot != nullptr);
        slot->set(1000u + i, 0x11111111u);
        QVERIFY(q.try_publish());
    }

    // Drain.
    for (reg i = 0; i < n; ++i) {
        auto* front = q.try_front();
        QVERIFY(front != nullptr);
        front->verify(1000u + i, 0x11111111u);
        QVERIFY(q.try_pop());
    }

    QVERIFY(q.empty());
    verify_invariants(q);
}

// Alignment sweep for STATIC views: ctor + attach() must reject misaligned pointers.
template<class Policy, std::size_t Align>
static void sweep_static_one_align() {
    using T = Blob<Align>;
    using Q = spsc::fifo_view<T, 32, Policy>;

    // Raw bytes with extra room for offset games.
    const std::size_t raw_bytes = (32u * sizeof(T)) + (Align * 4u);
    std::unique_ptr<std::byte[]> raw(new std::byte[raw_bytes]);

    for (std::size_t off = 0; off < (Align * 2u); ++off) {
        auto* base = raw.get() + off;
        auto* buf  = reinterpret_cast<T*>(base);

        const bool aligned = ((reinterpret_cast<std::uintptr_t>(buf) & (Align - 1u)) == 0u);

        if (aligned) {
            construct_n(buf, 32u);

            // ctor path
            {
                Q q(buf, Policy{});
                QVERIFY(q.is_valid());
                QCOMPARE(q.capacity(), reg{32u});
                sanity_rw(q);
            }

            // attach path
            {
                Q q;
                QVERIFY(!q.is_valid());
                QVERIFY(q.attach(buf));
                QVERIFY(q.is_valid());
                QCOMPARE(q.capacity(), reg{32u});
                sanity_rw(q);
            }

            destroy_n(buf, 32u);
        } else {
            // ctor must not accept misaligned storage.
            {
                Q q(buf, Policy{});
                QVERIFY(!q.is_valid());
            }

            // attach must refuse.
            {
                Q q;
                QVERIFY(!q.is_valid());
                QVERIFY(!q.attach(buf));
                QVERIFY(!q.is_valid());
            }
        }
    }
}

// Alignment sweep for DYNAMIC views: ctor + attach() + adopt() must reject misaligned pointers.
template<class Policy, std::size_t Align>
static void sweep_dynamic_one_align() {
    using T = Blob<Align>;
    using Q = spsc::fifo_view<T, 0, Policy>;

    constexpr reg kReq = 64u; // power-of-two for predictability

    const std::size_t raw_bytes = (kReq * sizeof(T)) + (Align * 4u);
    std::unique_ptr<std::byte[]> raw(new std::byte[raw_bytes]);

    for (std::size_t off = 0; off < (Align * 2u); ++off) {
        auto* base = raw.get() + off;
        auto* buf  = reinterpret_cast<T*>(base);

        const bool aligned = ((reinterpret_cast<std::uintptr_t>(buf) & (Align - 1u)) == 0u);

        if (aligned) {
            construct_n(buf, kReq);

            // ctor path
            {
                Q q(buf, kReq, Policy{});
                QVERIFY(q.is_valid());
                QCOMPARE(q.capacity(), kReq);
                sanity_rw(q);
            }

            // attach path
            {
                Q q;
                QVERIFY(!q.is_valid());
                QVERIFY(q.attach(buf, kReq));
                QVERIFY(q.is_valid());
                QCOMPARE(q.capacity(), kReq);
                sanity_rw(q);
            }

            // adopt path (restore full state)
            {
                // First make buffer contain a known pattern.
                Q src(buf, kReq, Policy{});
                QVERIFY(src.is_valid());

                // Put 10 items, then pop all, leaving old bytes in buffer.
                for (reg i = 0; i < 10u; ++i) {
                    auto* slot = src.try_claim();
                    QVERIFY(slot != nullptr);
                    slot->set(2000u + i, 0x22222222u);
                    QVERIFY(src.try_publish());
                }
                for (reg i = 0; i < 10u; ++i) {
                    QVERIFY(src.try_pop());
                }
                QVERIFY(src.empty());

                // Now adopt state that re-exposes those 10 items: head=10, tail=0.
                Q q;
                QVERIFY(!q.is_valid());
                QVERIFY(q.adopt(buf, kReq, 10u, 0u));
                QVERIFY(q.is_valid());
                QCOMPARE(q.size(), reg{10u});

                for (reg i = 0; i < 10u; ++i) {
                    auto* front = q.try_front();
                    QVERIFY(front != nullptr);
                    front->verify(2000u + i, 0x22222222u);
                    QVERIFY(q.try_pop());
                }
                QVERIFY(q.empty());
            }

            destroy_n(buf, kReq);
        } else {
            {
                Q q(buf, kReq, Policy{});
                QVERIFY(!q.is_valid());
            }
            {
                Q q;
                QVERIFY(!q.is_valid());
                QVERIFY(!q.attach(buf, kReq));
                QVERIFY(!q.is_valid());
            }
            {
                Q q;
                QVERIFY(!q.is_valid());
                QVERIFY(!q.adopt(buf, kReq, 0u, 0u));
                QVERIFY(!q.is_valid());
            }
        }
    }
}

// Bundle: run multiple alignments.
template<class Policy>
static void run_static_alignment_sweep() {
    sweep_static_one_align<Policy, 2>();
    sweep_static_one_align<Policy, 4>();
    sweep_static_one_align<Policy, 8>();
    sweep_static_one_align<Policy, 16>();
    sweep_static_one_align<Policy, 32>();
    sweep_static_one_align<Policy, 64>();
    sweep_static_one_align<Policy, 128>();
}

template<class Policy>
static void run_dynamic_alignment_sweep() {
    sweep_dynamic_one_align<Policy, 2>();
    sweep_dynamic_one_align<Policy, 4>();
    sweep_dynamic_one_align<Policy, 8>();
    sweep_dynamic_one_align<Policy, 16>();
    sweep_dynamic_one_align<Policy, 32>();
    sweep_dynamic_one_align<Policy, 64>();
    sweep_dynamic_one_align<Policy, 128>();
}

} // namespace align_sweep

// -------------------------------------------------------------------------
// Extra invariants: claim_read/claim_write must match read_size/write_size.
// -------------------------------------------------------------------------

template<class Q>
static void test_region_vs_size_hints(Q& q) {
    verify_invariants(q);

    // Empty.
    QCOMPARE(q.read_size(), reg{0u});
    auto rr0 = q.claim_read();
    QCOMPARE(rr0.total, reg{0u});

    // Write some items.
    for (reg i = 0; i < 7u; ++i) {
        QVERIFY(q.try_push(Traced{static_cast<int>(900 + i)}));
    }

    // Contiguous read is read_size().
    const reg rsz = q.read_size();
    auto rr = q.claim_read();
    QCOMPARE(rr.first.count, rsz);
    QVERIFY(rr.total >= rr.first.count);

    // Pop 3.
    QVERIFY(q.try_pop(3u));

    // write_size() must match claim_write().
    const reg wsz = q.write_size();
    auto wr = q.claim_write();
    QCOMPARE(wr.first.count, wsz);
    QVERIFY(wr.total >= wr.first.count);

    // Cleanup.
    QVERIFY(q.try_pop(q.size()));
    QVERIFY(q.empty());
}

// -------------------------------------------------------------------------
// Partial snapshot consume: consume(snapshot{subrange}) must advance tail by snapshot size.
// -------------------------------------------------------------------------

template<class Q>
static void test_partial_snapshot_consume(Q& q) {
    q.clear();
    QVERIFY(q.empty());

    for (reg i = 0; i < 10u; ++i) {
        QVERIFY(q.try_push(Traced{static_cast<int>(10000 + i)}));
    }
    QCOMPARE(q.size(), reg{10u});

    auto s = q.make_snapshot();
    QCOMPARE(s.size(), reg{10u});

    // Take first 4 items.
    auto it = s.begin();
    for (int k = 0; k < 4; ++k) { ++it; }
    typename Q::snapshot first4{s.begin(), it};

    QCOMPARE(first4.size(), reg{4u});
    QVERIFY(!first4.empty());

    q.consume(first4);
    QCOMPARE(q.size(), reg{6u});

    // Remaining items must start at 10004.
    auto* f = q.try_front();
    QVERIFY(f != nullptr);
    QCOMPARE(f->v, 10004);

    // Drain.
    for (reg i = 0; i < 6u; ++i) {
        QVERIFY(q.try_pop());
    }
    QVERIFY(q.empty());
}

// -------------------------------------------------------------------------
// Shadow regression: swap/move with FULL queues must not allow spurious claim.
// -------------------------------------------------------------------------

template<class Policy>
static void test_shadow_full_swap_move_claim_regression_static() {
    using Q = spsc::fifo_view<Traced, 16, Policy>;

    if constexpr (!spsc::detail::rb_use_shadow_v<Policy>) {
        return;
    }

    std::array<Traced, 16> buf_a{};
    std::array<Traced, 16> buf_b{};

    Q a(buf_a, Policy{});
    Q b(buf_b, Policy{});
    QVERIFY(a.is_valid());
    QVERIFY(b.is_valid());

    // Warm A on empty so producer shadow caches tail=0.
    shadow_warm_queries(a);

    // Fill B to full.
    for (reg i = 0; i < b.capacity(); ++i) {
        auto* s = b.try_claim();
        QVERIFY(s != nullptr);
        *s = Traced{static_cast<int>(i)};
        QVERIFY(b.try_publish());
    }
    QVERIFY(b.full());
    QVERIFY(b.try_claim() == nullptr);

    // Warm B on full.
    shadow_warm_queries(b);

    // Swap: A must become full and still refuse claim.
    using std::swap;
    swap(a, b);
    QVERIFY(a.full());
    QVERIFY(a.try_claim() == nullptr);

    // B must become empty and accept claim.
    QVERIFY(b.empty());
    auto* slot = b.try_claim();
    QVERIFY(slot != nullptr);
    *slot = Traced{12345};
    QVERIFY(b.try_publish());
    QVERIFY(b.try_pop());
    QVERIFY(b.empty());

    // Move: moved queue must preserve "full" refusal.
    Q moved(std::move(a));
    QVERIFY(moved.is_valid());
    QVERIFY(moved.full());
    QVERIFY(moved.try_claim() == nullptr);
}

template<class Policy>
static void test_shadow_full_swap_move_claim_regression_dynamic() {
    using Q = spsc::fifo_view<Traced, 0, Policy>;

    if constexpr (!spsc::detail::rb_use_shadow_v<Policy>) {
        return;
    }

    std::vector<Traced> buf_a(64);
    std::vector<Traced> buf_b(64);

    Q a(buf_a.data(), static_cast<reg>(buf_a.size()), Policy{});
    Q b(buf_b.data(), static_cast<reg>(buf_b.size()), Policy{});
    QVERIFY(a.is_valid());
    QVERIFY(b.is_valid());

    // Warm A on empty.
    shadow_warm_queries(a);

    // Fill B to full.
    const reg cap = b.capacity();
    for (reg i = 0; i < cap; ++i) {
        auto* s = b.try_claim();
        QVERIFY(s != nullptr);
        *s = Traced{static_cast<int>(i)};
        QVERIFY(b.try_publish());
    }
    QVERIFY(b.full());
    QVERIFY(b.try_claim() == nullptr);

    // Warm B on full.
    shadow_warm_queries(b);

    // Swap and validate.
    using std::swap;
    swap(a, b);
    QVERIFY(a.full());
    QVERIFY(a.try_claim() == nullptr);

    QVERIFY(b.empty());
    auto* slot = b.try_claim();
    QVERIFY(slot != nullptr);
    *slot = Traced{54321};
    QVERIFY(b.try_publish());
    QVERIFY(b.try_pop());
    QVERIFY(b.empty());

    Q moved(std::move(a));
    QVERIFY(moved.is_valid());
    QVERIFY(moved.full());
    QVERIFY(moved.try_claim() == nullptr);
}

// Shadow regression: adopt() must reset shadow caches; otherwise producer can overwrite.
// -------------------------------------------------------------------------

template<class Policy>
static void test_shadow_adopt_overwrite_regression_static() {
    using Q = spsc::fifo_view<std::uint32_t, 16, Policy>;

    if constexpr (!spsc::detail::rb_use_shadow_v<Policy>) {
        return;
    }

    std::array<std::uint32_t, 16> buf{};
    Q q(buf, Policy{});
    QVERIFY(q.is_valid());

    // 1) Fill with 0..15.
    for (std::uint32_t i = 0; i < 16u; ++i) {
        QVERIFY(q.try_push(i));
    }
    QVERIFY(q.full());

    // 2) Drain all (tail becomes 16).
    for (std::uint32_t i = 0; i < 16u; ++i) {
        QVERIFY(q.try_pop());
    }
    QVERIFY(q.empty());

    // 3) Warm producer queries with tail high (16).
    shadow_warm_queries(q);

    // 4) Adopt FULL state head=16, tail=0 (re-expose old items).
    QVERIFY(q.adopt(buf.data(), 16u, 0u));
    QVERIFY(q.full());

    // 5) Must refuse claim (otherwise overwrite possible).
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(!q.can_write());

    // 6) Verify data is intact.
    for (std::uint32_t i = 0; i < 16u; ++i) {
        auto* f = q.try_front();
        QVERIFY(f != nullptr);
        QCOMPARE(*f, i);
        QVERIFY(q.try_pop());
    }
    QVERIFY(q.empty());
}

template<class Policy>
static void test_shadow_adopt_overwrite_regression_dynamic() {
    using Q = spsc::fifo_view<std::uint32_t, 0, Policy>;

    if constexpr (!spsc::detail::rb_use_shadow_v<Policy>) {
        return;
    }

    std::vector<std::uint32_t> buf(64u);
    Q q(buf.data(), static_cast<reg>(buf.size()), Policy{});
    QVERIFY(q.is_valid());

    const reg cap = q.capacity();

    // 1) Fill full.
    for (reg i = 0; i < cap; ++i) {
        QVERIFY(q.try_push(static_cast<std::uint32_t>(i)));
    }
    QVERIFY(q.full());

    // 2) Drain all (tail becomes cap).
    QVERIFY(q.try_pop(cap));
    QVERIFY(q.empty());

    // 3) Warm producer queries with tail high.
    shadow_warm_queries(q);

    // 4) Adopt FULL state head=cap, tail=0.
    QVERIFY(q.adopt(buf.data(), static_cast<reg>(buf.size()), cap, 0u));
    QVERIFY(q.full());

    // 5) Must refuse claim.
    QVERIFY(q.try_claim() == nullptr);

    // 6) Verify data is intact.
    for (reg i = 0; i < cap; ++i) {
        auto* f = q.try_front();
        QVERIFY(f != nullptr);
        QCOMPARE(*f, static_cast<std::uint32_t>(i));
        QVERIFY(q.try_pop());
    }
    QVERIFY(q.empty());
}

// -------------------------------------------------------------------------
// 2-thread SPSC stress on fifo_view (atomic backends only).
// -------------------------------------------------------------------------

template<class Policy>
static void threaded_spsc_stress_fifo_view() {
    using Q = spsc::fifo_view<std::uint32_t, 0, Policy>;

    // Only makes sense for atomic policies.
    static_assert(Policy::counter_type::is_atomic,
                  "threaded_spsc_stress_fifo_view requires atomic backend");

    constexpr reg kBufCap = 4096u;
    constexpr std::uint32_t kIters = 250000u;

    std::vector<std::uint32_t> storage(kBufCap);
    Q q(storage.data(), kBufCap, Policy{});
    QVERIFY(q.is_valid());

    std::atomic<bool> start{false};
    std::atomic<bool> done_prod{false};
    std::atomic<bool> done_cons{false};

    std::thread prod([&]() {
        while (!start.load(std::memory_order_acquire)) {}

        std::uint32_t v = 1u;
        while (v <= kIters) {
            auto* slot = q.try_claim();
            if (!slot) {
                continue;
            }
            *slot = v;
            if (q.try_publish()) {
                ++v;
            }
        }
        done_prod.store(true, std::memory_order_release);
    });

    std::thread cons([&]() {
        while (!start.load(std::memory_order_acquire)) {}

        std::uint32_t expect = 1u;
        while (expect <= kIters) {
            auto* front = q.try_front();
            if (!front) {
                continue;
            }
            const std::uint32_t got = *front;
            if (got == expect) {
                QVERIFY(q.try_pop());
                ++expect;
            }
        }
        done_cons.store(true, std::memory_order_release);
    });

    start.store(true, std::memory_order_release);

    prod.join();
    cons.join();

    QVERIFY(done_prod.load(std::memory_order_acquire));
    QVERIFY(done_cons.load(std::memory_order_acquire));
    QVERIFY(q.empty());
}


// ------------------------------ full suites ------------------------------

template <class Policy>
static void run_static_suite() {
    using Q = spsc::fifo_view<Traced, 16, Policy>;

    api_compile_smoke<Q>();
    test_static_attachment_state_move_swap<Q>();
    test_shadow_sync_regressions<Q>();
    test_shadow_full_swap_move_claim_regression_static<Policy>();

    // Extra-hard: alignment sweep (ctor/attach reject misaligned storage)
    align_sweep::run_static_alignment_sweep<Policy>();
    // Extra-hard: shadow adoption overwrite regression
    test_shadow_adopt_overwrite_regression_static<Policy>();


    std::array<Traced, 16> buf{};
    Q q(buf, Policy{});
    QVERIFY(q.is_valid());

    test_introspection_and_data(q);
    test_push_pop_front_try(q);
    test_emplace(q);
    test_claim_publish(q);
    test_indexing_and_iterators(q);
    test_bulk_regions(q);
    test_snapshots(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);

    // Extra invariants and partial snapshot consume.
    test_region_vs_size_hints(q);
    test_partial_snapshot_consume(q);

    paranoid_random_fuzz(q, 0x123456 + int(sizeof(Policy)), kFuzzIters);
}

template <class Policy>
static void run_dynamic_suite() {
    using Q = spsc::fifo_view<Traced, 0, Policy>;

    api_compile_smoke<Q>();
    test_dynamic_attachment_state_move_swap<Q>();
    test_shadow_sync_regressions<Q>();
    test_shadow_full_swap_move_claim_regression_dynamic<Policy>();

    // Extra-hard: alignment sweep (ctor/attach/adopt reject misaligned storage)
    align_sweep::run_dynamic_alignment_sweep<Policy>();
    // Extra-hard: shadow adoption overwrite regression
    test_shadow_adopt_overwrite_regression_dynamic<Policy>();


    std::vector<Traced> buf(64);
    Q q(buf.data(), static_cast<reg>(buf.size()), Policy{});
    QVERIFY(q.is_valid());

    test_introspection_and_data(q);
    test_push_pop_front_try(q);
    test_emplace(q);
    test_claim_publish(q);
    test_indexing_and_iterators(q);
    test_bulk_regions(q);
    test_snapshots(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);

    // Extra invariants and partial snapshot consume.
    test_region_vs_size_hints(q);
    test_partial_snapshot_consume(q);

    paranoid_random_fuzz(q, 0xBADC0DE + int(sizeof(Policy)), kFuzzIters);
}



template <class Policy>
static void run_threaded_suite() {
    // Uses dynamic fifo_view with atomic counter backends only.
    threaded_spsc_stress_fifo_view<Policy>();
}

template <class Q>
static void deterministic_interleaving_suite(Q& q) {
    using T = typename Q::value_type;
    static_assert(std::is_same_v<T, std::uint32_t>, "deterministic_interleaving_suite expects fifo_view<uint32_t>.");

    QVERIFY(q.is_valid());
    q.clear();
    QVERIFY(q.empty());

    auto* s0 = q.try_claim();
    QVERIFY(s0 != nullptr);
    *s0 = 1u;

    auto* s0_again = q.try_claim();
    QVERIFY(s0_again != nullptr);
    QCOMPARE(reinterpret_cast<void*>(s0_again), reinterpret_cast<void*>(s0));

    QVERIFY(q.try_publish());
    QVERIFY(!q.empty());
    QCOMPARE(q.front(), std::uint32_t{1u});

    q.pop();
    QVERIFY(q.empty());

    auto* s1 = q.try_claim();
    QVERIFY(s1 != nullptr);
    *s1 = 2u;
    QVERIFY(q.try_publish());
    QCOMPARE(q.front(), std::uint32_t{2u});
    q.pop();
    QVERIFY(q.empty());
}

template <class Policy>
static void run_threaded_snapshot_suite(const char* name) {
    using Q = spsc::fifo_view<std::uint32_t, 0u, Policy>;

    std::vector<std::uint32_t> storage(4096u);
    Q q(storage.data(), static_cast<reg>(storage.size()), Policy{});
    QVERIFY2(q.is_valid(), name);

    std::atomic<bool> abort{false};
    std::atomic<bool> prod_done{false};
    std::atomic<int> fail{0};

    auto should_abort = [&]() -> bool {
        return abort.load(std::memory_order_relaxed);
    };

    std::thread prod([&]() {
        for (int i = 1; i <= kThreadIters && !should_abort(); ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(i);
            while (!q.try_push(v)) {
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
        std::uint32_t expected = 1u;

        while (expected <= static_cast<std::uint32_t>(kThreadIters) && !should_abort()) {
            if (std::chrono::steady_clock::now() > deadline) {
                fail.store(1, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            auto snap = q.make_snapshot();
            const reg sz = snap.size();
            if (sz == 0u) {
                if (!(snap.begin() == snap.end())) {
                    fail.store(2, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                if (prod_done.load(std::memory_order_acquire) && q.empty() &&
                    expected <= static_cast<std::uint32_t>(kThreadIters)) {
                    fail.store(3, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
                continue;
            }

            const reg cap = q.capacity();
            reg cnt = 0u;
            std::uint32_t cur = expected;
            for (auto it = snap.begin(); it != snap.end(); ++it) {
                if (cnt > cap) {
                    fail.store(4, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                if (*it != cur) {
                    fail.store(5, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                ++cur;
                ++cnt;
            }

            if (cnt != sz) {
                fail.store(6, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            if (!q.try_consume(snap)) {
                fail.store(7, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            expected += static_cast<std::uint32_t>(cnt);
        }
    });

    prod.join();
    cons.join();

    QVERIFY2(fail.load(std::memory_order_relaxed) == 0, name);
    QVERIFY2(!abort.load(std::memory_order_relaxed), name);
    QVERIFY2(q.empty(), name);
}

template <class Policy>
static void run_threaded_bulk_regions_suite(const char* name) {
    using Q = spsc::fifo_view<std::uint32_t, 0u, Policy>;

    std::vector<std::uint32_t> storage(2048u);
    Q q(storage.data(), static_cast<reg>(storage.size()), Policy{});
    QVERIFY2(q.is_valid(), name);

    std::atomic<bool> start{false};
    std::atomic<bool> done{false};
    std::atomic<bool> abort{false};

    constexpr std::uint32_t kMaxBatch = 8u;

    std::thread prod([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t seq = 1u;
        std::uint32_t rng = 0x1234567u;
        auto next_u32 = [&] {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            return rng;
        };

        while (seq <= static_cast<std::uint32_t>(kThreadIters) &&
               !abort.load(std::memory_order_relaxed)) {
            const reg want = static_cast<reg>((next_u32() % kMaxBatch) + 1u);
            auto wr = q.claim_write(::spsc::unsafe, want);
            if (wr.total == 0u) {
                std::this_thread::yield();
                continue;
            }

            reg written = 0u;
            for (reg i = 0u; i < wr.first.count && seq <= static_cast<std::uint32_t>(kThreadIters); ++i) {
                wr.first.ptr[i] = seq++;
                ++written;
            }
            for (reg i = 0u; i < wr.second.count && seq <= static_cast<std::uint32_t>(kThreadIters); ++i) {
                wr.second.ptr[i] = seq++;
                ++written;
            }

            if (written != 0u) {
                q.publish(written);
            }
        }

        done.store(true, std::memory_order_release);
    });

    std::thread cons([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        std::uint32_t expect = 1u;
        const auto t0 = std::chrono::steady_clock::now();
        while (expect <= static_cast<std::uint32_t>(kThreadIters) &&
               !abort.load(std::memory_order_relaxed)) {
            auto rr = q.claim_read(::spsc::unsafe, kMaxBatch);
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

            auto check_span = [&](const std::uint32_t* p, const reg n) {
                for (reg i = 0u; i < n; ++i) {
                    if (p[i] != expect) {
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
}

static void api_compile_smoke_all() {
    using QP  = spsc::fifo_view<Traced, 16u, spsc::policy::P>;
    using QV  = spsc::fifo_view<Traced, 16u, spsc::policy::V>;
    using QA  = spsc::fifo_view<Traced, 16u, spsc::policy::A<>>;
    using QCA = spsc::fifo_view<Traced, 16u, spsc::policy::CA<>>;
    using QD  = spsc::fifo_view<Traced, 0u, spsc::policy::P>;

    api_compile_smoke<QP>();
    api_compile_smoke<QV>();
    api_compile_smoke<QA>();
    api_compile_smoke<QCA>();
    api_compile_smoke<QD>();
}

static void allocator_accounting_suite() {
    using Q = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::P>;

    std::vector<std::uint32_t> a(64u);
    std::vector<std::uint32_t> b(64u);

    Q q(a.data(), static_cast<reg>(a.size()));
    QVERIFY(q.is_valid());
    QCOMPARE(q.data(), a.data());
    QCOMPARE(q.capacity(), reg{64u});

    q.detach();
    QVERIFY(!q.is_valid());
    QCOMPARE(q.data(), static_cast<std::uint32_t*>(nullptr));

    QVERIFY(q.attach(a.data(), static_cast<reg>(a.size())));
    QCOMPARE(q.data(), a.data());
    QVERIFY(q.try_push(1u));
    QVERIFY(q.try_push(2u));

    auto st = q.state();
    QVERIFY(q.adopt(a.data(), static_cast<reg>(a.size()), st.head, st.tail));
    QCOMPARE(q.data(), a.data());
    QCOMPARE(q.size(), reg{2u});
    QCOMPARE(q.front(), std::uint32_t{1u});

    Q other(b.data(), static_cast<reg>(b.size()));
    QVERIFY(other.is_valid());
    QVERIFY(other.try_push(99u));

    q.swap(other);
    QCOMPARE(q.data(), b.data());
    QCOMPARE(other.data(), a.data());
    QCOMPARE(q.front(), std::uint32_t{99u});
    QCOMPARE(other.front(), std::uint32_t{1u});
}

static void resize_migration_order_suite() {
    using Q = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::P>;

    std::vector<std::uint32_t> src(64u);
    std::vector<std::uint32_t> dst(128u);

    Q q(src.data(), static_cast<reg>(src.size()));
    QVERIFY(q.is_valid());

    // Create wrapped logical sequence in source storage.
    for (std::uint32_t i = 1u; i <= 40u; ++i) {
        QVERIFY(q.try_push(i));
    }
    for (std::uint32_t i = 0u; i < 17u; ++i) {
        q.pop();
    }
    for (std::uint32_t i = 41u; i <= 70u; ++i) {
        QVERIFY(q.try_push(i));
    }

    // External migration for fifo_view:
    // copy logical FIFO order into destination and adopt matching state.
    const auto snap = q.make_snapshot();
    reg migrated = 0u;
    for (auto it = snap.begin(); it != snap.end(); ++it) {
        dst[static_cast<std::size_t>(migrated++)] = *it;
    }
    QVERIFY(migrated != 0u);

    QVERIFY(q.adopt(dst.data(), static_cast<reg>(dst.size()), migrated, reg{0u}));
    QVERIFY(q.is_valid());
    QCOMPARE(q.data(), dst.data());
    QCOMPARE(q.size(), migrated);

    for (std::uint32_t i = 18u; i <= 70u; ++i) {
        QCOMPARE(q.front(), i);
        q.pop();
    }
    QVERIFY(q.empty());
}

static void dynamic_capacity_sweep_suite() {
    using Q = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());

    for (reg req = 0u; req < 512u; req += 7u) {
        std::vector<std::uint32_t> storage(static_cast<std::size_t>(req == 0u ? 1u : req));
        const bool ok = q.attach(req == 0u ? nullptr : storage.data(), req);
        if (req < 2u) {
            QVERIFY(!ok);
            QVERIFY(!q.is_valid());
            continue;
        }

        QVERIFY(ok);
        QVERIFY(q.is_valid());
        const reg cap = q.capacity();
        QVERIFY(cap >= 2u);
        QVERIFY((cap & (cap - 1u)) == 0u);
        QVERIFY(cap <= req);

        q.clear();
        const reg n = (cap < 5u) ? cap : 5u;
        for (reg i = 0u; i < n; ++i) {
            QVERIFY(q.try_push(static_cast<std::uint32_t>(i + 1u)));
        }
        for (reg i = 0u; i < n; ++i) {
            QCOMPARE(q.front(), static_cast<std::uint32_t>(i + 1u));
            q.pop();
        }
        QVERIFY(q.empty());
        q.detach();
    }
}

static void move_swap_stress_suite() {
    using Q = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::P>;

    std::vector<std::uint32_t> a_buf(128u);
    std::vector<std::uint32_t> b_buf(128u);

    Q a(a_buf.data(), static_cast<reg>(a_buf.size()));
    Q b(b_buf.data(), static_cast<reg>(b_buf.size()));
    QVERIFY(a.is_valid());
    QVERIFY(b.is_valid());

    std::uint32_t seq = 1u;
    for (int i = 0; i < kSwapIters; ++i) {
        if (!a.full()) {
            static_cast<void>(a.try_push(seq++));
        }
        if (!b.full()) {
            static_cast<void>(b.try_push(seq++));
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

        if ((i & 3) == 0) {
            if (!a.empty()) {
                a.pop();
            }
            if (!b.empty()) {
                b.pop();
            }
        }
    }
}

static void snapshot_try_consume_contract_suite() {
    std::array<Traced, 64u> buf{};
    using Q = spsc::fifo_view<Traced, 64u, spsc::policy::P>;
    Q q(buf);

    for (int i = 0; i < 10; ++i) {
        QVERIFY(q.try_push(Traced{i + 1}));
    }
    auto snap = q.make_snapshot();
    q.pop();
    QVERIFY(!q.try_consume(snap));

    auto snap2 = q.make_snapshot();
    QVERIFY(q.try_consume(snap2));
    QVERIFY(q.empty());
}

static void snapshot_iteration_contract_suite() {
    std::array<Traced, 64u> buf{};
    using Q = spsc::fifo_view<Traced, 64u, spsc::policy::P>;
    Q q(buf);
    QVERIFY(q.is_valid());

    q.clear();
    for (int i = 1; i <= 12; ++i) {
        QVERIFY(q.try_push(Traced{i}));
    }

    auto snap = q.make_snapshot();
    QCOMPARE(snap.size(), reg{12u});

    int expect = 1;
    reg count = 0u;
    for (auto it = snap.begin(); it != snap.end(); ++it) {
        QCOMPARE(it->v, expect++);
        ++count;
    }
    QCOMPARE(count, reg{12u});

    QVERIFY(q.try_consume(snap));
    QVERIFY(q.empty());
}

static void alignment_sweep_all() {
    align_sweep::run_static_alignment_sweep<spsc::policy::P>();
    align_sweep::run_static_alignment_sweep<spsc::policy::V>();
    align_sweep::run_static_alignment_sweep<spsc::policy::A<>>();
    align_sweep::run_static_alignment_sweep<spsc::policy::CA<>>();

    align_sweep::run_dynamic_alignment_sweep<spsc::policy::P>();
    align_sweep::run_dynamic_alignment_sweep<spsc::policy::V>();
    align_sweep::run_dynamic_alignment_sweep<spsc::policy::A<>>();
    align_sweep::run_dynamic_alignment_sweep<spsc::policy::CA<>>();
}

template <class Q>
static void bulk_regions_max_count_suite(Q& q) {
    q.clear();
    auto wr = q.claim_write(::spsc::unsafe, 7u);
    QCOMPARE(wr.total, reg{7u});
    QVERIFY(wr.first.count > 0u);
    QCOMPARE(wr.first.count + wr.second.count, wr.total);
}

template <class Q>
static void bulk_read_max_count_suite(Q& q) {
    q.clear();
    for (std::uint32_t i = 1u; i <= 30u; ++i) {
        QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
    }
    auto rr = q.claim_read(::spsc::unsafe, 7u);
    QCOMPARE(rr.total, reg{7u});

    int expect = 1;
    for (reg i = 0u; i < rr.first.count; ++i) {
        QCOMPARE(rr.first.ptr[i].v, expect++);
    }
    for (reg i = 0u; i < rr.second.count; ++i) {
        QCOMPARE(rr.second.ptr[i].v, expect++);
    }
    q.pop(rr.total);
}

template <class Q>
static void guard_move_semantics_suite(Q& q) {
    q.clear();
    QVERIFY(q.empty());

    {
        auto w1 = q.scoped_write();
        QVERIFY(w1);
        w1.ref() = Traced{1};
        auto w2 = std::move(w1);
        QVERIFY(!w1);
        QVERIFY(w2);
        w2.commit();
    }
    QCOMPARE(q.size(), reg{1u});

    {
        auto r1 = q.scoped_read();
        QVERIFY(r1);
        auto r2 = std::move(r1);
        QVERIFY(!r1);
        QVERIFY(r2);
        QCOMPARE(r2->v, 1);
    }
    QVERIFY(q.empty());
}

template <class Q>
static void snapshot_consume_suite(Q& q) {
    q.clear();
    for (int i = 0; i < 12; ++i) {
        QVERIFY(q.try_push(Traced{i + 1}));
    }
    auto snap = q.make_snapshot();
    q.consume(snap);
    QVERIFY(q.empty());
}

static void snapshot_invalid_view_suite() {
    using Q = spsc::fifo_view<Traced, 0u, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());
    auto s = q.make_snapshot();
    QCOMPARE(s.size(), reg{0u});
    QVERIFY(s.begin() == s.end());
    QVERIFY(!q.try_consume(s));
}

static void reserve_resize_edge_cases_dynamic_suite() {
    using Q = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());

    std::vector<std::uint32_t> tiny(1u);
    QVERIFY(!q.attach(nullptr, reg{64u}));
    QVERIFY(q.attach(tiny.data(), static_cast<reg>(tiny.size())));
    QVERIFY(q.is_valid());
    QCOMPARE(q.capacity(), reg{1u});
    QVERIFY(q.try_push(123u));
    QVERIFY(!q.try_push(456u));
    QCOMPARE(q.front(), std::uint32_t{123u});
    q.pop();
    QVERIFY(q.empty());
    q.detach();
    QVERIFY(!q.is_valid());

    std::vector<std::uint32_t> odd(3u);
    QVERIFY(q.attach(odd.data(), static_cast<reg>(odd.size())));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 2u);
    QVERIFY(q.capacity() <= static_cast<reg>(odd.size()));
    QVERIFY((q.capacity() & (q.capacity() - 1u)) == 0u);

    q.detach();
    QVERIFY(!q.is_valid());

    std::vector<std::uint32_t> buf(64u);
    QVERIFY(q.attach(buf.data(), static_cast<reg>(buf.size())));
    QVERIFY(q.try_push(1u));
    QVERIFY(q.try_push(2u));
    const auto st = q.state();

    QVERIFY(!q.adopt(nullptr, static_cast<reg>(buf.size()), st.head, st.tail));
    QVERIFY(!q.adopt(buf.data(), reg{1u}, st.head, st.tail));
    QVERIFY(q.adopt(buf.data(), static_cast<reg>(buf.size()), st.head, st.tail));
    QCOMPARE(q.front(), std::uint32_t{1u});
    q.pop();
    QCOMPARE(q.front(), std::uint32_t{2u});
    q.pop();
    QVERIFY(q.empty());
}

static void consume_all_contract_suite() {
    {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        for (int i = 0; i < 20; ++i) {
            QVERIFY(q.try_push(Traced{i}));
        }
        QVERIFY(!q.empty());
        q.consume_all();
        QVERIFY(q.empty());
    }
}

static void dynamic_move_contract_suite() {
    using Q = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::P>;

    std::vector<std::uint32_t> a(128u);
    std::vector<std::uint32_t> b(128u);

    Q src(a.data(), static_cast<reg>(a.size()));
    for (std::uint32_t i = 1u; i <= 20u; ++i) {
        QVERIFY(src.try_push(i));
    }

    Q mid(std::move(src));
    QVERIFY(!src.is_valid());
    QVERIFY(mid.is_valid());
    QCOMPARE(mid.data(), a.data());
    QCOMPARE(mid.size(), reg{20u});
    QCOMPARE(mid.front(), std::uint32_t{1u});

    Q dst(b.data(), static_cast<reg>(b.size()));
    QVERIFY(dst.try_push(999u));
    dst = std::move(mid);
    QVERIFY(!mid.is_valid());
    QVERIFY(dst.is_valid());
    QCOMPARE(dst.data(), a.data());
    QCOMPARE(dst.size(), reg{20u});

    for (std::uint32_t i = 1u; i <= 20u; ++i) {
        QCOMPARE(dst.front(), i);
        dst.pop();
    }
    QVERIFY(dst.empty());
}

static void stress_cached_ca_transitions_suite() {
    using QD = spsc::fifo_view<std::uint32_t, 0u, spsc::policy::CA<>>;
    using QS = spsc::fifo_view<std::uint32_t, 64u, spsc::policy::CA<>>;

    {
        std::array<std::uint32_t, 64u> a_buf{};
        std::array<std::uint32_t, 64u> b_buf{};
        QS a(a_buf);
        QS b(b_buf);

        for (std::uint32_t i = 1u; i <= 17u; ++i) {
            QVERIFY(a.try_push(i));
        }
        for (std::uint32_t i = 100u; i < 121u; ++i) {
            QVERIFY(b.try_push(i));
        }

        a.swap(b);
        QCOMPARE(a.front(), std::uint32_t{100u});
        QCOMPARE(b.front(), std::uint32_t{1u});
    }

    {
        std::vector<std::uint32_t> storage(128u);
        QD q(storage.data(), static_cast<reg>(storage.size()));
        QVERIFY(q.is_valid());
        for (std::uint32_t i = 1u; i <= 48u; ++i) {
            QVERIFY(q.try_push(i));
        }
        auto st = q.state();
        q.detach();
        QVERIFY(!q.is_valid());
        QVERIFY(q.adopt(storage.data(), static_cast<reg>(storage.size()), st.head, st.tail));
        QVERIFY(q.is_valid());
        QCOMPARE(q.front(), std::uint32_t{1u});
    }
}

static void regression_matrix_all() {
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 16u, spsc::policy::P>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 16u, spsc::policy::V>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 16u, spsc::policy::A<>>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 16u, spsc::policy::CA<>>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 0u, spsc::policy::P>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 0u, spsc::policy::V>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 0u, spsc::policy::A<>>>();
    test_shadow_sync_regressions<spsc::fifo_view<Traced, 0u, spsc::policy::CA<>>>();

    test_shadow_full_swap_move_claim_regression_static<spsc::policy::P>();
    test_shadow_full_swap_move_claim_regression_static<spsc::policy::V>();
    test_shadow_full_swap_move_claim_regression_static<spsc::policy::A<>>();
    test_shadow_full_swap_move_claim_regression_static<spsc::policy::CA<>>();
    test_shadow_full_swap_move_claim_regression_dynamic<spsc::policy::P>();
    test_shadow_full_swap_move_claim_regression_dynamic<spsc::policy::V>();
    test_shadow_full_swap_move_claim_regression_dynamic<spsc::policy::A<>>();
    test_shadow_full_swap_move_claim_regression_dynamic<spsc::policy::CA<>>();

    test_shadow_adopt_overwrite_regression_static<spsc::policy::P>();
    test_shadow_adopt_overwrite_regression_static<spsc::policy::V>();
    test_shadow_adopt_overwrite_regression_static<spsc::policy::A<>>();
    test_shadow_adopt_overwrite_regression_static<spsc::policy::CA<>>();
    test_shadow_adopt_overwrite_regression_dynamic<spsc::policy::P>();
    test_shadow_adopt_overwrite_regression_dynamic<spsc::policy::V>();
    test_shadow_adopt_overwrite_regression_dynamic<spsc::policy::A<>>();
    test_shadow_adopt_overwrite_regression_dynamic<spsc::policy::CA<>>();
}

static void lifecycle_traced_suite() {
    LifecycleTracked::reset();
    {
        std::array<LifecycleTracked, 64u> s_buf{};
        const int base_live = LifecycleTracked::live.load(std::memory_order_relaxed);
        QVERIFY(base_live >= 64);

        spsc::fifo_view<LifecycleTracked, 64u, spsc::policy::P> q(s_buf);
        QVERIFY(q.is_valid());

        for (int i = 0; i < 20; ++i) {
            QVERIFY(q.try_push(LifecycleTracked{i + 1}));
            QVERIFY(LifecycleTracked::live.load(std::memory_order_relaxed) >= base_live);
        }
        q.consume_all();
        QVERIFY(q.empty());
        QCOMPARE(LifecycleTracked::live.load(std::memory_order_relaxed), base_live);
    }
    QCOMPARE(LifecycleTracked::live.load(std::memory_order_relaxed), 0);
    QCOMPARE(LifecycleTracked::ctor.load(std::memory_order_relaxed),
             LifecycleTracked::dtor.load(std::memory_order_relaxed));
}

static void state_machine_fuzz_sweep_suite() {
    {
        std::array<Traced, 16u> s_buf{};
        spsc::fifo_view<Traced, 16u, spsc::policy::P> q(s_buf);
        paranoid_random_fuzz(q, 0x123456, kFuzzIters / 2);
    }
    {
        std::vector<Traced> d_buf(64u);
        spsc::fifo_view<Traced, 0u, spsc::policy::CA<>> q(d_buf.data(), static_cast<reg>(d_buf.size()));
        paranoid_random_fuzz(q, 0xBADC0DE, kFuzzIters / 2);
    }
}

static void death_tests_debug_only_suite() {
#if !defined(NDEBUG)
    auto expect_death = [&](const char* mode) {
        QProcess p;
        p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments(QStringList{});

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SPSC_FIFO_VIEW_DEATH", QString::fromLatin1(mode));
        p.setProcessEnvironment(env);

        p.start();
        QVERIFY2(p.waitForStarted(1500), "Death child failed to start.");

        if (!p.waitForFinished(8000)) {
            p.kill();
            QVERIFY2(false, "Death child did not finish (possible crash dialog)." );
        }

        const int code = p.exitCode();
        QVERIFY2(code == spsc_fifo_view_death_detail::kDeathExitCode,
                 "Expected assertion death (SIGABRT -> kDeathExitCode)." );
    };

    expect_death("pop_empty");
    expect_death("front_empty");
    expect_death("publish_full");
    expect_death("claim_full");
    expect_death("bulk_double_emplace_next");
    expect_death("bulk_arm_publish_unwritten");
    expect_death("consume_foreign_snapshot");
    expect_death("pop_n_too_many");
#else
    QSKIP("Death tests are debug-only (assertions disabled)." );
#endif
}

} // namespace

class tst_fifo_view_api_paranoid final : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() { api_compile_smoke_all(); }

    void static_plain_P()    { run_static_suite<spsc::policy::P>(); }
    void static_volatile_V() { run_static_suite<spsc::policy::V>(); }
    void static_atomic_A()   { run_static_suite<spsc::policy::A<>>(); }
    void static_cached_CA()  { run_static_suite<spsc::policy::CA<>>(); }

    void dynamic_plain_P()    { run_dynamic_suite<spsc::policy::P>(); }
    void dynamic_volatile_V() { run_dynamic_suite<spsc::policy::V>(); }
    void dynamic_atomic_A()   { run_dynamic_suite<spsc::policy::A<>>(); }
    void dynamic_cached_CA()  { run_dynamic_suite<spsc::policy::CA<>>(); }

    void deterministic_interleaving() {
        std::array<std::uint32_t, 64u> buf{};
        spsc::fifo_view<std::uint32_t, 64u, spsc::policy::P> q(buf);
        deterministic_interleaving_suite(q);
    }

    void threaded_atomic_A()  { run_threaded_suite<spsc::policy::A<>>(); }
    void threaded_cached_CA() { run_threaded_suite<spsc::policy::CA<>>(); }

    void threaded_snapshot_atomic_A() {
        run_threaded_snapshot_suite<spsc::policy::A<>>("threaded_snapshot_atomic");
    }
    void threaded_snapshot_cached_CA() {
        run_threaded_snapshot_suite<spsc::policy::CA<>>("threaded_snapshot_cached");
    }

    void allocator_accounting() { allocator_accounting_suite(); }

    void invalid_inputs() {
        spsc::fifo_view<Traced, 0u, spsc::policy::P> q;
        test_invalid_view_behavior(q);
    }

    void dynamic_capacity_sweep()   { dynamic_capacity_sweep_suite(); }
    void move_swap_stress()         { move_swap_stress_suite(); }
    void state_machine_fuzz_sweep() { state_machine_fuzz_sweep_suite(); }
    void resize_migration_order()   { resize_migration_order_suite(); }
    void snapshot_try_consume_contract() { snapshot_try_consume_contract_suite(); }

    void bulk_regions_wraparound() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        test_bulk_regions(q);
    }

    void raii_guards_static() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        test_raii_guards(q);
    }

    void raii_guards_dynamic() {
        std::vector<Traced> buf(64u);
        spsc::fifo_view<Traced, 0u, spsc::policy::P> q(buf.data(), static_cast<reg>(buf.size()));
        test_raii_guards(q);
    }

    void iterators_and_indexing_static() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        test_indexing_and_iterators(q);
    }

    void iterators_wraparound_static() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        q.clear();
        const reg cap = q.capacity();
        for (reg i = 0u; i < cap; ++i) {
            QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
        }
        q.pop(cap - 4u);
        QVERIFY(q.try_push(Traced{1000}));
        QVERIFY(q.try_push(Traced{1001}));
        QVERIFY(q.try_push(Traced{1002}));
        QVERIFY(q.try_push(Traced{1003}));

        std::vector<int> got;
        for (const auto& v : q) {
            got.push_back(v.v);
        }
        QCOMPARE(got.size(), std::size_t{8u});
        QCOMPARE(got.front(), static_cast<int>(cap - 4u));
        QCOMPARE(got.back(), 1003);
    }

    void bulk_raii_overloads_static() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        test_bulk_raii_overloads(q);
    }

    void bulk_raii_overloads_dynamic() {
        std::vector<Traced> buf(64u);
        spsc::fifo_view<Traced, 0u, spsc::policy::P> q(buf.data(), static_cast<reg>(buf.size()));
        test_bulk_raii_overloads(q);
    }

    void snapshot_iteration_contract() { snapshot_iteration_contract_suite(); }

    void bulk_regions_max_count_static() {
        std::array<std::uint32_t, 64u> buf{};
        spsc::fifo_view<std::uint32_t, 64u, spsc::policy::P> q(buf);
        bulk_regions_max_count_suite(q);
    }

    void bulk_read_max_count_static() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        bulk_read_max_count_suite(q);
    }

    void guard_move_semantics_static() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        guard_move_semantics_suite(q);
    }

    void reserve_resize_edge_cases_dynamic() { reserve_resize_edge_cases_dynamic_suite(); }

    void snapshot_consume_contract() {
        std::array<Traced, 64u> buf{};
        spsc::fifo_view<Traced, 64u, spsc::policy::P> q(buf);
        snapshot_consume_suite(q);
    }

    void snapshot_invalid_view() { snapshot_invalid_view_suite(); }
    void snapshot_invalid_queue() { snapshot_invalid_view_suite(); }
    void dynamic_move_contract() { dynamic_move_contract_suite(); }
    void consume_all_contract()  { consume_all_contract_suite(); }

    void threaded_bulk_regions_atomic_A() {
        run_threaded_bulk_regions_suite<spsc::policy::A<>>("threaded_fifo_view_bulk_atomic");
    }
    void threaded_bulk_regions_cached_CA() {
        run_threaded_bulk_regions_suite<spsc::policy::CA<>>("threaded_fifo_view_bulk_cached");
    }

    void alignment_sweep() { alignment_sweep_all(); }
    void stress_cached_ca_transitions() { stress_cached_ca_transitions_suite(); }
    void regression_matrix() { regression_matrix_all(); }
    void api_smoke() { api_compile_smoke_all(); }
    void death_tests_debug_only() { death_tests_debug_only_suite(); }
    void lifecycle_traced() { lifecycle_traced_suite(); }
    void cleanupTestCase() {}
};

int run_tst_fifo_view_api_paranoid(int argc, char** argv)
{
    tst_fifo_view_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "fifo_view_test.moc"
