/*
 * fifo_test.cpp
 *
 * Paranoid API/contract test for spsc::fifo.
 *
 * Goals:
 *  - Exercise EVERY public constructor and method in fifo.hpp at least once.
 *  - Cover all built-in policies (P/V/A/CA) without inventing new ones.
 *  - Cover both static (Capacity != 0) and dynamic (Capacity == 0) variants.
 *  - Stress with deterministic pseudo-random sequences to catch wrap/index bugs.
 *
 * Notes:
 *  - This is single-threaded validation (correctness + invariants), not a formal memory-order proof.
 *  - QMake: add `QT += testlib` (NOT `test`).
 */

#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <deque>
#include <limits>
#include <new>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include "fifo_test.h"

#if !defined(SPSC_ASSERT) && !defined(NDEBUG)
#  define SPSC_ASSERT(expr) do { if(!(expr)) { std::abort(); } } while(0)
#endif

#include "fifo.hpp"

namespace spsc_fifo_death_detail {

#if !defined(NDEBUG)

static constexpr int kDeathExitCode = 0xAD;

static void sigabrt_handler_(int) noexcept {
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode) {
    std::signal(SIGABRT, &sigabrt_handler_);

    using Q = spsc::fifo<std::uint32_t, 16u, spsc::policy::P>;

    if (std::strcmp(mode, "pop_empty") == 0) {
        Q q;
        q.pop(); // Must assert: pop on empty.
    } else if (std::strcmp(mode, "front_empty") == 0) {
        Q q;
        (void)q.front(); // Must assert: front on empty.
    } else if (std::strcmp(mode, "publish_full") == 0) {
        Q q;
        for (std::uint32_t i = 0; i < 16u; ++i) {
            if (!q.try_push(i)) {
                std::_Exit(0xE1);
            }
        }
        q.publish(); // Must assert: publish while full.
    } else if (std::strcmp(mode, "claim_full") == 0) {
        Q q;
        for (std::uint32_t i = 0; i < 16u; ++i) {
            if (!q.try_push(i)) {
                std::_Exit(0xE2);
            }
        }
        (void)q.claim(); // Must assert: claim while full.
    } else if (std::strcmp(mode, "bulk_double_emplace_next") == 0) {
        Q q;
        auto g = q.scoped_write(1u);
        (void)g.emplace_next(1u);
        (void)g.emplace_next(2u); // Must assert: writing past claimed.
    } else if (std::strcmp(mode, "bulk_arm_publish_unwritten") == 0) {
        Q q;
        auto g = q.scoped_write(2u);
        g.arm_publish(); // Must assert: no written elements.
    } else if (std::strcmp(mode, "consume_foreign_snapshot") == 0) {
        Q q1;
        Q q2;
        if (!q1.try_push(1u)) {
            std::_Exit(0xE3);
        }
        if (!q2.try_push(2u)) {
            std::_Exit(0xE4);
        }
        const auto snap = q2.make_snapshot();
        q1.consume(snap); // Must assert: foreign snapshot identity.
    } else if (std::strcmp(mode, "pop_n_too_many") == 0) {
        Q q;
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
        const char* mode = std::getenv("SPSC_FIFO_DEATH");
        if (mode && *mode) {
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

#endif // !defined(NDEBUG)

} // namespace spsc_fifo_death_detail

namespace {


// Scale fuzz to avoid painfully slow Debug runs.
#if defined(NDEBUG)
static constexpr int kFuzzIters = 55000;
static constexpr int kSwapIters = 30000;
#else
static constexpr int kFuzzIters = 6000;
static constexpr int kSwapIters = 6000;
#endif

// Threaded stress is only valid for atomic backends (A/CA).
#if defined(NDEBUG)
static constexpr int kThreadIters      = 250000;
static constexpr int kThreadTimeoutMs = 3500;
#else
static constexpr int kThreadIters      = 20000;
static constexpr int kThreadTimeoutMs = 6000;
#endif

// ------------------------------ tiny types to catch "assignment" mistakes ------------------------------

struct Traced final {
    int v{0};
    std::uint32_t tag{0};

    Traced() noexcept = default;
    explicit Traced(int x) noexcept : v(x), tag(0xC0FFEEu) {}

    Traced(const Traced&) noexcept = default;
    Traced& operator=(const Traced&) noexcept = default;

    Traced(Traced&& o) noexcept : v(o.v), tag(o.tag) { o.v = -777; }
    Traced& operator=(Traced&& o) noexcept {
        if (this != &o) {
            v = o.v;
            tag = o.tag;
            o.v = -777;
        }
        return *this;
    }

    ~Traced() noexcept = default;
};

struct Tracked final {
    std::uint32_t seq{0u};
    std::uint32_t cookie{0u};

    static inline std::atomic<int> live{0};
    static inline std::atomic<int> ctor{0};
    static inline std::atomic<int> dtor{0};
    static inline std::atomic<int> copy{0};
    static inline std::atomic<int> move{0};

    Tracked() noexcept : seq(0u), cookie(0xA5A5A5A5u) {
        ++live;
        ++ctor;
    }

    explicit Tracked(const std::uint32_t s) noexcept
        : seq(s), cookie(s ^ 0xA5A5A5A5u) {
        ++live;
        ++ctor;
    }

    Tracked(const Tracked& other) noexcept
        : seq(other.seq), cookie(other.cookie) {
        ++live;
        ++ctor;
        ++copy;
    }

    Tracked& operator=(const Tracked& other) noexcept {
        if (this != &other) {
            seq = other.seq;
            cookie = other.cookie;
        }
        ++copy;
        return *this;
    }

    Tracked(Tracked&& other) noexcept
        : seq(other.seq), cookie(other.cookie) {
        other.cookie = 0xDEADu;
        ++live;
        ++ctor;
        ++move;
    }

    Tracked& operator=(Tracked&& other) noexcept {
        if (this != &other) {
            seq = other.seq;
            cookie = other.cookie;
            other.cookie = 0xDEADu;
        }
        ++move;
        return *this;
    }

    ~Tracked() noexcept {
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

    static void on_alloc(const std::size_t bytes) {
        allocs.fetch_add(1);
        const auto live_now = bytes_live.fetch_add(bytes) + bytes;
        auto peak = bytes_peak.load();
        while (live_now > peak && !bytes_peak.compare_exchange_weak(peak, live_now)) {
            // CAS loop
        }
    }

    static void on_dealloc(const std::size_t bytes) {
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

    [[nodiscard]] pointer allocate(const size_type n) noexcept {
        if (n == 0u) {
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

    void deallocate(pointer p, const size_type n) noexcept {
        if (!p || n == 0u) {
            return;
        }
        const std::size_t bytes = n * sizeof(T);
        AllocStats::on_dealloc(bytes);
        ::operator delete(p, std::align_val_t(Align));
    }

    template <class U>
    bool operator==(const CountingAlignedAlloc<U, Align>&) const noexcept { return true; }

    template <class U>
    bool operator!=(const CountingAlignedAlloc<U, Align>&) const noexcept { return false; }
};

static inline bool is_pow2(reg x) noexcept {
    return x != 0u && ((x & (x - 1u)) == 0u);
}

template <class Q>
static void verify_invariants(const Q& q) {
    if (!q.is_valid()) {
        QCOMPARE(q.capacity(), reg{0u});
        QCOMPARE(q.size(), reg{0u});
        QCOMPARE(q.free(), reg{0u});
        QVERIFY(q.empty());
        QVERIFY(q.full()); // invalid behaves as "cannot push"
        QVERIFY(!q.can_write(reg{1u}));
        return;
    }

    QVERIFY(is_pow2(q.capacity()));
    QVERIFY(q.capacity() >= 2u);

    QVERIFY(q.size() <= q.capacity());
    QVERIFY(q.free() <= q.capacity());
    QCOMPARE(q.size() + q.free(), q.capacity());

    // read/write predicates
    QVERIFY(q.can_read(q.size()));
    QVERIFY(q.can_write(q.free()));
    QVERIFY(!q.can_read(q.size() + 1u));
    QVERIFY(!q.can_write(q.free() + 1u));

    // read_size/write_size are contiguous region lengths (<= total size/free)
    QVERIFY(q.read_size() <= q.size());
    QVERIFY(q.write_size() <= q.free());

    // empty/full consistency
    if (q.empty()) {
        QCOMPARE(q.size(), reg{0u});
    }
    if (q.full()) {
        QCOMPARE(q.free(), reg{0u});
    }
}

// ------------------------------ compile-time API smoke (C++17) ------------------------------

template <class, class = void>
struct has_reserve : std::false_type {};
template <class Q>
struct has_reserve<Q, std::void_t<decltype(std::declval<Q&>().reserve(reg{1u}))>> : std::true_type {};

template <class, class = void>
struct has_resize : std::false_type {};
template <class Q>
struct has_resize<Q, std::void_t<decltype(std::declval<Q&>().resize(typename Q::size_type{2u}))>> : std::true_type {};

template <class... Ts>
struct type_pack final {};

template <class X>
struct is_readonly_deref : std::false_type {};
template <class U>
struct is_readonly_deref<U const&> : std::true_type {};
template <class U>
struct is_readonly_deref<U const*> : std::true_type {};
template <class U>
struct is_readonly_deref<U const* const> : std::true_type {};
template <class U>
struct is_readonly_deref<U const&&> : std::true_type {};

template <class Q>
static void api_compile_smoke() {
    using T = typename Q::value_type;
    using size_type = typename Q::size_type;

    static_assert(std::is_same<decltype(std::declval<const Q&>().is_valid()), bool>::value, "is_valid()");
    static_assert(std::is_same<decltype(std::declval<const Q&>().capacity()), size_type>::value, "capacity()");
    static_assert(std::is_same<decltype(std::declval<const Q&>().size()), size_type>::value, "size()");
    static_assert(std::is_same<decltype(std::declval<const Q&>().free()), size_type>::value, "free()");

    // Compile-time proof that const_snapshot is read-only (const ref or const ptr).
    {
        using CS = typename Q::const_snapshot;
        using CSIt = decltype(std::declval<const CS&>().begin());
        using CSDeref = decltype(*std::declval<CSIt>());
        static_assert(is_readonly_deref<CSDeref>::value, "const_snapshot must be read-only (const T& or const T*)");
    }

    // Force instantiation of the entire API surface without -Wunused-local-typedefs warnings.
    [[maybe_unused]] type_pack<
        decltype(std::declval<Q&>().clear()),
        decltype(std::declval<Q&>().destroy()),

        decltype(std::declval<Q&>().push(T{})),
        decltype(std::declval<Q&>().try_push(T{})),
        decltype(std::declval<Q&>().emplace()),
        decltype(std::declval<Q&>().try_emplace()),

        decltype(std::declval<Q&>().claim()),
        decltype(std::declval<Q&>().try_claim()),
        decltype(std::declval<Q&>().publish()),
        decltype(std::declval<Q&>().try_publish()),
        decltype(std::declval<Q&>().publish(size_type{1u})),
        decltype(std::declval<Q&>().try_publish(size_type{1u})),

        decltype(std::declval<Q&>().front()),
        decltype(std::declval<const Q&>().front()),
        decltype(std::declval<Q&>().try_front()),
        decltype(std::declval<const Q&>().try_front()),

        decltype(std::declval<Q&>().pop()),
        decltype(std::declval<Q&>().try_pop()),
        decltype(std::declval<Q&>().pop(size_type{1u})),
        decltype(std::declval<Q&>().try_pop(size_type{1u})),

        decltype(std::declval<Q&>()[size_type{0u}]),

        decltype(std::declval<Q&>().claim_write(::spsc::unsafe)),
        decltype(std::declval<Q&>().claim_write(::spsc::unsafe, size_type{1u})),
        decltype(std::declval<Q&>().claim_read(::spsc::unsafe)),
        decltype(std::declval<Q&>().claim_read(::spsc::unsafe, size_type{1u})),

        decltype(std::declval<Q&>().make_snapshot()),
        decltype(std::declval<const Q&>().make_snapshot()),
        decltype(std::declval<Q&>().consume(std::declval<typename Q::snapshot>())),
        decltype(std::declval<Q&>().try_consume(std::declval<typename Q::snapshot>())),
        decltype(std::declval<Q&>().consume_all()),

        decltype(std::declval<Q&>().scoped_write()),
        decltype(std::declval<Q&>().scoped_read()),
        decltype(std::declval<Q&>().scoped_write(size_type{1u})),
        decltype(std::declval<Q&>().scoped_read(size_type{1u})),

#if SPSC_HAS_SPAN
        decltype(std::declval<Q&>().span()),
        decltype(std::declval<const Q&>().span()),
#endif

        decltype(std::declval<Q&>().begin()),
        decltype(std::declval<Q&>().end()),
        decltype(std::declval<const Q&>().cbegin()),
        decltype(std::declval<const Q&>().cend()),
        decltype(std::declval<Q&>().rbegin()),
        decltype(std::declval<Q&>().rend()),
        decltype(std::declval<const Q&>().crbegin()),
        decltype(std::declval<const Q&>().crend())
        > _api{};

    if constexpr (has_resize<Q>::value) {
        [[maybe_unused]] type_pack<decltype(std::declval<Q&>().resize(size_type{2u}))> _resize{};
    }
    if constexpr (has_reserve<Q>::value) {
        [[maybe_unused]] type_pack<decltype(std::declval<Q&>().reserve(size_type{2u}))> _reserve{};
    }
}

// ------------------------------ small helpers ------------------------------

template <class Q>
static void ensure_valid(Q& q, reg cap) {
    if constexpr (has_resize<Q>::value) {
        if (!q.is_valid()) {
            QVERIFY(q.resize(cap));
        }
    }
    QVERIFY(q.is_valid());
}

template <class Q>
static std::vector<int> drain_values(Q& q) {
    std::vector<int> out;
    out.reserve(static_cast<std::size_t>(q.size()));
    while (!q.empty()) {
        out.push_back(q.front().v);
        q.pop();
    }
    return out;
}

template <class Q>
static void fill_values(Q& q, std::initializer_list<int> vals) {
    for (int v : vals) {
        QVERIFY(q.try_push(Traced{v}));
    }
}

template <class Q>
static void fill_seq_tracked(Q& q, const std::uint32_t first, const std::uint32_t count) {
    for (std::uint32_t i = 0u; i < count; ++i) {
        QVERIFY(q.try_push(Tracked{first + i}));
    }
}

template <class Q>
static void check_fifo_exact_tracked(Q& q, const std::uint32_t first, const std::uint32_t count) {
    for (std::uint32_t i = 0u; i < count; ++i) {
        QVERIFY(!q.empty());
        QCOMPARE(q.front().seq, first + i);
        QCOMPARE(q.front().cookie, (first + i) ^ 0xA5A5A5A5u);
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void fill_seq_u32(Q& q, const std::uint32_t first, const std::uint32_t count) {
    for (std::uint32_t i = 0u; i < count; ++i) {
        QVERIFY(q.try_push(first + i));
    }
}

template <class Q>
static void check_fifo_exact_u32(Q& q, const std::uint32_t first, const std::uint32_t count) {
    for (std::uint32_t i = 0u; i < count; ++i) {
        QVERIFY(!q.empty());
        QCOMPARE(q.front(), first + i);
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void full_empty_cycle_u32(Q& q, const std::uint32_t base) {
    using T = typename Q::value_type;
    static_assert(std::is_same_v<T, std::uint32_t>, "Expected fifo<uint32_t>.");

    const reg cap = q.capacity();
    QVERIFY(cap > 0u);

    const reg s0 = q.size();
    QVERIFY(s0 <= cap);

    const reg fill = cap - s0;
    for (reg i = 0u; i < fill; ++i) {
        const auto v = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(i));
        QVERIFY(q.try_push(v));
    }

    QCOMPARE(q.size(), cap);
    QVERIFY(q.full());

    {
        const auto extra = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(fill));
        QVERIFY(!q.try_push(extra));
    }

    for (reg i = 0u; i < s0; ++i) {
        QVERIFY(q.try_front() != nullptr);
        q.pop();
    }

    for (reg i = 0u; i < fill; ++i) {
        auto* p = q.try_front();
        QVERIFY(p != nullptr);
        const auto out = *p;
        q.pop();
        const auto exp = static_cast<std::uint32_t>(base + static_cast<std::uint32_t>(i));
        QCOMPARE(out, exp);
    }

    QVERIFY(q.empty());
    QCOMPARE(q.size(), reg{0u});
}

// ------------------------------ per-method tests ------------------------------

template <class Q>
static void test_shadow_sync_regressions() {
    using Policy = typename Q::policy_type;

    // Only meaningful when shadows are enabled for this policy.
    if constexpr (!::spsc::detail::rb_use_shadow_v<Policy>) {
        return;
    }

    Q a;
    Q b;

    ensure_valid(a, reg{16u});
    ensure_valid(b, reg{16u});

    a.clear();
    b.clear();

    // Fill 'a' and warm both producer+consumer paths so shadows are non-trivial.
    QVERIFY(a.try_push(Traced{1}));
    QVERIFY(a.try_push(Traced{2}));
    (void)a.empty();      // consumer path
    (void)a.read_size();  // consumer path
    (void)a.full();       // producer path
    (void)a.write_size(); // producer path
    verify_invariants(a);

    // Warm 'b' too (keep it empty but touch both paths).
    (void)b.empty();
    (void)b.read_size();
    (void)b.full();
    (void)b.write_size();
    verify_invariants(b);

    // Swap regression: if swap doesn't sync shadow with new head/tail, invariants can break.
    a.swap(b);
    verify_invariants(a);
    verify_invariants(b);

    QVERIFY(a.empty());
    QVERIFY(!b.empty());
    QCOMPARE(b.front().v, 1);
    b.pop();
    QCOMPARE(b.front().v, 2);
    b.pop();
    QVERIFY(b.empty());
    verify_invariants(b);

    // Move regression: move a warmed, non-empty queue.
    QVERIFY(b.try_push(Traced{7}));
    QVERIFY(b.try_push(Traced{8}));
    (void)b.empty();
    (void)b.read_size();
    (void)b.write_size();
    verify_invariants(b);

    Q moved(std::move(b));
    verify_invariants(moved);

    QCOMPARE(moved.front().v, 7);
    moved.pop();
    QCOMPARE(moved.front().v, 8);
    moved.pop();
    QVERIFY(moved.empty());
    verify_invariants(moved);
}


template <class Q>
static void test_constructors_and_assignment() {
    api_compile_smoke<Q>();

    // default ctor
    Q a;
    verify_invariants(a);

    // copy/move/swap basics (may be "invalid" for dynamic until initialized elsewhere)
    Q b(a);
    verify_invariants(b);

    Q c;
    c = b;
    verify_invariants(c);

    Q d(std::move(c));
    verify_invariants(d);

    Q e;
    e = std::move(d);
    verify_invariants(e);

    // swap (including std::swap ADL)
    using std::swap;
    swap(a, e);
    verify_invariants(a);
    verify_invariants(e);

    // explicit member swap
    a.swap(e);
    verify_invariants(a);
    verify_invariants(e);

    // ----------------- non-empty / non-trivial state checks -----------------
    // This catches "forgot to swap head/tail", "copied only storage", etc.
    {
        Q x;
        Q y;

        ensure_valid(x, reg{16u});
        ensure_valid(y, reg{32u});

        x.clear();
        y.clear();

        fill_values(x, {1, 2, 3, 4});
        fill_values(y, {100, 101});

        const auto x_before = drain_values(x); // destructive read to capture expected
        const auto y_before = drain_values(y);
        QVERIFY(x.empty());
        QVERIFY(y.empty());

        // Refill and swap for real
        fill_values(x, {1, 2, 3, 4});
        fill_values(y, {100, 101});

        using std::swap;
        swap(x, y);

        // After swap, sequences must swap as well.
        auto y_after = drain_values(x);
        auto x_after = drain_values(y);

        QCOMPARE(y_after, y_before);
        QCOMPARE(x_after, x_before);
    }

    // copy-assign into an already-valid, non-empty queue (common bug zone)
    {
        Q src;
        Q dst;

        ensure_valid(src, reg{16u});
        ensure_valid(dst, reg{8u});

        src.clear();
        dst.clear();

        fill_values(src, {7, 8, 9});
        fill_values(dst, {111, 222});

        dst = src;
        QVERIFY(dst.is_valid());
        QCOMPARE(dst.size(), src.size());

        auto got = drain_values(dst);
        QCOMPARE(got, std::vector<int>({7, 8, 9}));
    }

    // move-assign into an already-valid queue (steal must not leak/alias)
    {
        Q src;
        Q dst;

        ensure_valid(src, reg{16u});
        ensure_valid(dst, reg{16u});

        src.clear();
        dst.clear();

        fill_values(src, {55, 56});
        fill_values(dst, {999});

        dst = std::move(src);
        QVERIFY(dst.is_valid());
        QCOMPARE(drain_values(dst), std::vector<int>({55, 56}));
    }
}

template <class Q>
static void test_introspection_and_data(Q& q) {
    verify_invariants(q);

    // get_allocator() should exist and be usable
    [[maybe_unused]] auto alloc = q.get_allocator();

    // data() / const data()
    if (q.is_valid()) {
        QVERIFY(q.data() != nullptr);
        const Q& cq = q;
        QVERIFY(cq.data() != nullptr);
    } else {
        QCOMPARE(q.data(), nullptr);
        const Q& cq = q;
        QCOMPARE(cq.data(), nullptr);
    }

#if SPSC_HAS_SPAN
    if (q.is_valid()) {
        auto sp = q.span();
        QCOMPARE(static_cast<reg>(sp.size()), q.capacity());
        QVERIFY(sp.data() == q.data());

        const Q& cq = q;
        auto csp = cq.span();
        QCOMPARE(static_cast<reg>(csp.size()), cq.capacity());
        QVERIFY(csp.data() == cq.data());
    }
#endif

    // clear()
    q.clear();
    QVERIFY(q.empty());

    // Empty iterator sanity on a valid buffer.
    if (q.is_valid()) {
        QVERIFY(q.begin() == q.end());
        const Q& cq = q;
        QVERIFY(cq.cbegin() == cq.cend());
        QVERIFY(q.rbegin() == q.rend());
        QVERIFY(cq.crbegin() == cq.crend());
    }

    // destroy() on static queues must NOT invalidate, it should behave like clear().
    if constexpr (!has_resize<Q>::value) {
        QVERIFY(q.is_valid());
        QVERIFY(q.try_push(Traced{123}));
        q.destroy();
        QVERIFY(q.is_valid());
        QVERIFY(q.empty());
        QVERIFY(q.capacity() != 0u);
    }

    verify_invariants(q);
}

template <class Q>
static void test_push_pop_front_try(Q& q) {
    q.clear();
    verify_invariants(q);

    // empty cases
    QCOMPARE(q.try_front(), nullptr);
    QVERIFY(!q.try_pop());
    QVERIFY(!q.try_pop(reg{1u}));

    // try_pop(0) must succeed and be a no-op (even on empty)
    {
        const auto sz = q.size();
        QVERIFY(q.try_pop(reg{0u}));
        QCOMPARE(q.size(), sz);
    }

    // pop(0) must be a no-op
    q.pop(reg{0u});
    QVERIFY(q.empty());

    // push / front / pop
    q.push(Traced{1});
    QVERIFY(!q.empty());
    QCOMPARE(q.front().v, 1);

    // pop(0) must not change front/size
    {
        const auto sz = q.size();
        q.pop(reg{0u});
        QCOMPARE(q.size(), sz);
        QCOMPARE(q.front().v, 1);
    }

    QCOMPARE(static_cast<const Q&>(q).front().v, 1);
    q.pop();
    QVERIFY(q.empty());

    // try_push / try_front / try_pop
    QVERIFY(q.try_push(Traced{2}));
    auto* p = q.try_front();
    QVERIFY(p != nullptr);
    QCOMPARE(p->v, 2);
    QVERIFY(q.try_pop());
    QVERIFY(q.empty());

    // pop(n) / try_pop(n)
    QVERIFY(q.try_push(Traced{10}));
    QVERIFY(q.try_push(Traced{11}));
    QVERIFY(q.try_push(Traced{12}));
    QVERIFY(!q.try_pop(reg{4u}));
    q.pop(reg{2u});
    QCOMPARE(q.front().v, 12);
    q.pop();
    QVERIFY(q.empty());

    verify_invariants(q);
}

template <class Q>
static void test_emplace(Q& q) {
    q.clear();

    auto& r = q.emplace(123);
    QCOMPARE(r.v, 123);
    QCOMPARE(q.front().v, 123);
    q.pop();
    QVERIFY(q.empty());

    auto* p = q.try_emplace(456);
    QVERIFY(p != nullptr);
    QCOMPARE(p->v, 456);
    q.pop();
    QVERIFY(q.empty());
}

template <class Q>
static void test_claim_publish(Q& q) {
    q.clear();

    // publish(0) must be a no-op
    {
        const auto sz = q.size();
        q.publish(reg{0u});
        QCOMPARE(q.size(), sz);
    }

    // try_publish(0) must succeed and be a no-op
    {
        const auto sz = q.size();
        QVERIFY(q.try_publish(reg{0u}));
        QCOMPARE(q.size(), sz);
    }

    // claim/publish single
    {
        auto& slot = q.claim();
        slot = Traced{7};
        q.publish();
    }
    QCOMPARE(q.size(), reg{1u});
    QCOMPARE(q.front().v, 7);

    // try_claim/try_publish single
    q.pop();
    QVERIFY(q.empty());

    auto* w = q.try_claim();
    QVERIFY(w != nullptr);
    *w = Traced{8};
    QVERIFY(q.try_publish());
    QCOMPARE(q.front().v, 8);
    q.pop();
    QVERIFY(q.empty());

    // try_publish(n) failure path
    {
        const reg too_big = q.free() + reg{1u};
        QVERIFY(!q.try_publish(too_big));
        QVERIFY(q.empty());
    }

    // try_publish(n) runtime path via bulk claim_write
    {
        const reg n = reg{4u};
        QVERIFY(q.free() >= n);

        auto wr = q.claim_write(::spsc::unsafe, n);
        QCOMPARE(wr.total, n);

        int v = 1000;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

        QVERIFY(q.try_publish(wr.total));
        QCOMPARE(q.size(), n);

        auto rr = q.claim_read(::spsc::unsafe, reg{10u});
        QCOMPARE(rr.total, n);
        q.pop(rr.total);
        QVERIFY(q.empty());
    }

    // fill to full and validate try_ behavior
    const reg cap = q.capacity();
    QVERIFY(cap >= 2u);
    for (reg i = 0; i < cap; ++i) {
        QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
    }
    QVERIFY(q.full());
    QVERIFY(!q.can_write(reg{1u}));

    QVERIFY(!q.try_push(Traced{999}));
    QCOMPARE(q.try_claim(), nullptr);
    QVERIFY(!q.try_publish());
    auto wr = q.claim_write(::spsc::unsafe);
    QVERIFY(wr.empty());

    // drain
    for (reg i = 0; i < cap; ++i) {
        QCOMPARE(q.front().v, static_cast<int>(i));
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_indexing_and_iterators(Q& q) {
    q.clear();

    // force wrap: fill, pop some, push again
    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
    q.pop(cap - 3u); // leave 3 at the "end"
    QVERIFY(q.try_push(Traced{1000}));
    QVERIFY(q.try_push(Traced{1001}));
    QVERIFY(q.try_push(Traced{1002}));
    QVERIFY(q.size() == 6u);

    // operator[]
    QCOMPARE(q[0u].v, static_cast<int>(cap - 3u));
    QCOMPARE(q[1u].v, static_cast<int>(cap - 2u));
    QCOMPARE(q[2u].v, static_cast<int>(cap - 1u));
    QCOMPARE(q[3u].v, 1000);

    // const operator[]
    {
        const Q& cq = q;
        QCOMPARE(cq[0u].v, static_cast<int>(cap - 3u));
        QCOMPARE(cq[3u].v, 1000);
    }

    // begin/end forward order
    {
        std::vector<int> got;
        for (auto it = q.begin(); it != q.end(); it++) got.push_back(it->v);
        QCOMPARE(static_cast<int>(got.size()), 6);
        QCOMPARE(got.front(), static_cast<int>(cap - 3u));
        QCOMPARE(got.back(), 1002);
    }

    // const iteration
    {
        const Q& cq = q;
        int first = cq.cbegin()->v;
        QCOMPARE(first, static_cast<int>(cap - 3u));
    }

    // reverse iteration
    {
        std::vector<int> got;
        for (auto it = q.rbegin(); it != q.rend(); it++) got.push_back(it->v);
        QCOMPARE(got.front(), 1002);
        QCOMPARE(got.back(), static_cast<int>(cap - 3u));
    }

    q.clear();
    QVERIFY(q.empty());

    // const iterator sanity on empty
    {
        const Q& cq = q;
        QVERIFY(cq.cbegin() == cq.cend());
        QVERIFY(cq.crbegin() == cq.crend());
    }
}

template <class Q>
static void test_bulk_regions(Q& q) {
    q.clear();

    // empty read regions
    {
        auto rr = q.claim_read(::spsc::unsafe);
        QVERIFY(rr.empty());
        rr = q.claim_read(::spsc::unsafe, reg{10u});
        QVERIFY(rr.empty());
    }

    // max_count == 0 must always yield empty regions
    {
        auto wr0 = q.claim_write(::spsc::unsafe, reg{0u});
        QVERIFY(wr0.empty());

        auto rr0 = q.claim_read(::spsc::unsafe, reg{0u});
        QVERIFY(rr0.empty());
    }

    // want > available should clamp to what exists
    {
        auto rr = q.claim_read(::spsc::unsafe, reg{999u});
        QVERIFY(rr.empty());
    }

    // write region on empty
    {
        auto wr = q.claim_write(::spsc::unsafe, reg{5u});
        QVERIFY(wr.total <= q.free());
        QVERIFY(wr.total > 0u);
        QVERIFY(wr.first.ptr != nullptr);
        QVERIFY(wr.first.count > 0u);

        // write monotonic
        int v = 10;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

        q.publish(wr.total);
        QCOMPARE(q.size(), wr.total);
    }

    // read region should match what we wrote
    {
        auto rr = q.claim_read(::spsc::unsafe);
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

    // want > free should clamp to free
    {
        q.clear();
        auto wr = q.claim_write(::spsc::unsafe, q.capacity() * 2u);
        QVERIFY(!wr.empty());
        QCOMPARE(wr.total, q.free());
        QCOMPARE(wr.first.count + wr.second.count, wr.total);
    }

    // claim_write() default max_count should return up to free() (possibly split).
    {
        q.clear();
        auto wr = q.claim_write(::spsc::unsafe);
        QVERIFY(!wr.empty());
        QCOMPARE(wr.total, q.free());
        QCOMPARE(wr.first.count + wr.second.count, wr.total);
        QVERIFY(wr.total <= q.capacity());
    }

    // partial publish: claim_write(N) then publish(K < N) should expose only K items
    {
        q.clear();
        const reg N = reg{6u};
        const reg K = reg{3u};
        QVERIFY(q.free() >= N);

        auto wr = q.claim_write(::spsc::unsafe, N);
        QCOMPARE(wr.total, N);

        int v = 500;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};

        // publish(0) must not change size
        q.publish(reg{0u});
        QCOMPARE(q.size(), reg{0u});

        q.publish(K);
        QCOMPARE(q.size(), K);

        auto rr = q.claim_read(::spsc::unsafe, reg{10u});
        QCOMPARE(rr.total, K);

        // must see only first K values [500..502]
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

    // partial pop: claim_read(N) then pop(K < N) should leave remaining items intact
    {
        q.clear();
        const reg N = reg{6u};
        const reg K = reg{3u};
        QVERIFY(q.free() >= N);

        auto wr = q.claim_write(::spsc::unsafe, N);
        QCOMPARE(wr.total, N);

        int v = 700;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = Traced{v++};
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = Traced{v++};
        q.publish(N);
        QCOMPARE(q.size(), N);

        auto rr = q.claim_read(::spsc::unsafe, N);
        QCOMPARE(rr.total, N);

        q.pop(K);
        QCOMPARE(q.size(), N - K);
        QCOMPARE(q.front().v, 703);

        // remaining must be [703..705]
        for (int expect = 703; expect <= 705; ++expect) {
            QCOMPARE(q.front().v, expect);
            q.pop();
        }
        QVERIFY(q.empty());
    }

    // force wrap split for claim_write (deterministic)
    {
        const reg cap = q.capacity();
        QVERIFY(cap >= 8u);
        q.clear();

        // Make tail_phys < head_phys (used contiguous), so free region splits.
        const reg fill = cap / 2u;
        for (reg i = 0; i < fill; ++i) QVERIFY(q.try_push(Traced{100 + static_cast<int>(i)}));
        q.pop(); // tail_phys becomes 1

        auto wr = q.claim_write(::spsc::unsafe);
        QVERIFY(wr.total > 0u);
        QVERIFY(wr.first.count > 0u);
        QVERIFY(wr.second.count > 0u);
        QCOMPARE(wr.first.count + wr.second.count, wr.total);

        // write_size() should match the contiguous first region.
        QCOMPARE(q.write_size(), wr.first.count);
    }

    // force wrap split for claim_read (deterministic)
    {
        const reg cap = q.capacity();
        QVERIFY(cap >= 8u);
        q.clear(); // reset head/tail to 0 so wrap scenario is deterministic

        // fill full
        for (reg i = 0; i < cap; ++i) QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
        q.pop(cap - 2u); // leave 2
        QVERIFY(q.try_push(Traced{2000}));
        QVERIFY(q.try_push(Traced{2001}));
        QVERIFY(q.try_push(Traced{2002}));
        QVERIFY(q.try_push(Traced{2003}));

        auto rr = q.claim_read(::spsc::unsafe);
        QCOMPARE(rr.total, reg{6u});
        // read_size() should match the contiguous first region.
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

    // fill and snapshot
    for (int i = 0; i < 10; ++i) QVERIFY(q.try_push(Traced{i}));

    auto s = q.make_snapshot();
    QVERIFY(s.size() == 10u);

    // const_snapshot should mirror snapshot content and be read-only
    {
        const Q& cq = q;
        auto cs = cq.make_snapshot();
        QCOMPARE(cs.size(), reg{10u});
        int last = -1;
        for (auto it = cs.begin(); it != cs.end(); ++it) last = it->v;
        QCOMPARE(last, 9);
    }

    // verify snapshot values
    {
        int expected = 0;
        for (auto it = s.begin(); it != s.end(); it++) {
            QCOMPARE(it->v, expected++);
        }
    }

    // try_consume should succeed
    QVERIFY(q.try_consume(s));
    QVERIFY(q.empty());

    // consume() (non-try) should accept a fresh snapshot at current tail
    {
        for (int i = 0; i < 3; ++i) QVERIFY(q.try_push(Traced{200 + i}));
        auto s3 = q.make_snapshot();
        q.consume(s3);
        QVERIFY(q.empty());
    }

    // stale snapshot should fail
    for (int i = 0; i < 4; ++i) QVERIFY(q.try_push(Traced{100 + i}));
    auto s2 = q.make_snapshot();
    q.pop(); // consumer advanced
    QVERIFY(!q.try_consume(s2));

    // snapshot from another queue should fail
    Q other;
    if constexpr (has_resize<Q>::value) {
        // Ensure 'other' is valid for dynamic.
        ensure_valid(other, reg{16u});
    }
    if (other.is_valid()) {
        other.clear();
        QVERIFY(other.try_push(Traced{1}));
        auto os = other.make_snapshot();
        QVERIFY(!q.try_consume(os));
    }

    // consume_all
    q.clear();
    QVERIFY(q.try_push(Traced{7}));
    QVERIFY(q.try_push(Traced{8}));
    q.consume_all();
    QVERIFY(q.empty());
}

template <class Q>
static void test_raii_guards(Q& q) {
    q.clear();

    // scoped_read on empty should be falsey
    {
        auto rg = q.scoped_read();
        QVERIFY(!static_cast<bool>(rg));
    }

    // scoped_read on non-empty: operator-> and auto-pop on destructor
    {
        QVERIFY(q.try_push(Traced{9}));
        {
            auto rg = q.scoped_read();
            QVERIFY(static_cast<bool>(rg));
            QCOMPARE(rg->v, 9);
            // no commit(): destructor should pop
        }
        QVERIFY(q.empty());
    }

    // scoped_write cancel path
    {
        auto wg = q.scoped_write();
        QVERIFY(static_cast<bool>(wg));
        auto* p = wg.peek();
        QVERIFY(p != nullptr);
        *p = Traced{1};
        wg.cancel();
    }
    QVERIFY(q.empty());

    // scoped_write publish-on-destroy (via operator*)
    {
        auto wg = q.scoped_write();
        QVERIFY(static_cast<bool>(wg));
        (*wg).v = 2; // arms publish-on-destroy
    }
    QCOMPARE(q.size(), reg{1u});
    QCOMPARE(q.front().v, 2);

    // scoped_read cancel keeps the element
    {
        auto rg = q.scoped_read();
        QVERIFY(static_cast<bool>(rg));
        QCOMPARE((*rg).v, 2);
        rg.cancel();
    }
    QCOMPARE(q.size(), reg{1u});

    // scoped_read commit pops
    {
        auto rg = q.scoped_read();
        QVERIFY(static_cast<bool>(rg));
        rg.commit();
    }
    QVERIFY(q.empty());

    // scoped_write publish-on-destroy (via operator->)
    {
        auto wg = q.scoped_write();
        QVERIFY(static_cast<bool>(wg));
        wg->v = 3;
    }
    QCOMPARE(q.front().v, 3);
    q.pop();
    QVERIFY(q.empty());

    // guard move correctness
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

    // when full, scoped_write should be falsey
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

    // bulk_write_guard: write + disarm => no publish.
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

    // bulk_write_guard: manual pointer path via get_next() + mark_written().
    {
        auto bw = q.scoped_write(1u);
        QVERIFY(static_cast<bool>(bw));
        auto* slot = bw.get_next();
        QVERIFY(slot != nullptr);
        slot->v = 99;
        slot->tag = 0xC0FFEEu;
        bw.mark_written();
        QCOMPARE(bw.constructed(), reg{1u});
        bw.commit();
    }
    QCOMPARE(q.size(), reg{1u});
    QCOMPARE(q.front().v, 99);
    QCOMPARE(q.front().tag, std::uint32_t{0xC0FFEEu});
    q.pop();
    QVERIFY(q.empty());

    // bulk_write_guard: explicit commit publishes exactly N written elements.
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

    // bulk_write_guard move semantics + destructor publish.
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

    // bulk_read_guard: inspect + cancel (must not pop).
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

    // bulk_read_guard: commit pops exactly requested count.
    {
        auto br = q.scoped_read(4u);
        QVERIFY(static_cast<bool>(br));
        QCOMPARE(br.count(), reg{4u});
        br.commit();
        QVERIFY(!static_cast<bool>(br));
    }
    QCOMPARE(q.size(), reg{2u});
    QCOMPARE(q.front().v, 104);

    // bulk_read_guard move semantics + destructor pop.
    {
        auto br1 = q.scoped_read(2u);
        QVERIFY(static_cast<bool>(br1));
        auto br2 = std::move(br1);
        QVERIFY(!static_cast<bool>(br1));
        QVERIFY(static_cast<bool>(br2));
        QCOMPARE(br2.count(), reg{2u});
    }
    QVERIFY(q.empty());

    // max_count=0 => inactive guards.
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

        // Keep the reference model and the queue in lock-step. If this fails, stop immediately.
        QCOMPARE(q.size(), static_cast<reg>(model.size()));
        QCOMPARE(q.empty(), model.empty());

        const int op = static_cast<int>(rng() % 10u);

        switch (op) {
        case 0: { // try_push
            push_one(static_cast<int>(rng() & 0x7FFFu));
        } break;

        case 1: { // pop
            pop_one();
        } break;

        case 2: { // bulk write
            const reg want = static_cast<reg>((rng() % 8u) + 1u);
            auto wr = q.claim_write(::spsc::unsafe, want);
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
            auto rr = q.claim_read(::spsc::unsafe, want);
            if (rr.total == 0u) {
                QVERIFY(model.empty());
                QVERIFY(q.empty());
                break;
            }
            // verify equals model
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
            for (auto it = s.begin(); it != s.end(); it++, ++i) {
                QCOMPARE(it->v, model[i]);
            }
        } break;

        case 5: { // snapshot consume must be deterministic in single-thread
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
                    // 50% commit, 50% cancel
                    if ((rng() & 1u) == 0u) {
                        wg.commit();
                        model.push_back(v);
                    } else {
                        wg.cancel();
                    }
                }
            }
            if (!q.empty()) {
                auto rg = q.scoped_read();
                if (rg) {
                    QCOMPARE(rg->v, model.front());
                    if ((rng() & 1u) == 0u) {
                        rg.commit();
                        model.pop_front();
                    } else {
                        rg.cancel();
                    }
                }
            }
        } break;

        case 7: { // clear
            if ((rng() & 7u) == 0u) {
                q.clear();
                model.clear();
            }
        } break;

        case 8: { // front/try_front cross-check
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

    // final drain must match
    while (!model.empty()) {
        QCOMPARE(q.front().v, model.front());
        q.pop();
        model.pop_front();
    }
    QVERIFY(q.empty());
    verify_invariants(q);
}

template <class Q>
static void test_invalid_queue_behavior(Q& q) {
    // This helper is intended for dynamic fifo (Capacity==0) BEFORE initialization.
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    // All producer/consumer "try" operations should be inert on invalid queues.
    QVERIFY(!q.try_push(Traced{1}));
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(!q.try_publish());

    // Bulk API should return empty regions.
    {
        auto rr = q.claim_read(::spsc::unsafe);
        QVERIFY(rr.empty());
        rr = q.claim_read(::spsc::unsafe, reg{8u});
        QVERIFY(rr.empty());

        auto wr = q.claim_write(::spsc::unsafe);
        QVERIFY(wr.empty());
        wr = q.claim_write(::spsc::unsafe, reg{8u});
        QVERIFY(wr.empty());
    }

#if SPSC_HAS_SPAN
    // span() on invalid must be empty (data() is expected to be nullptr)
    {
        auto sp = q.span();
        QCOMPARE(static_cast<reg>(sp.size()), reg{0u});
        const Q& cq = q;
        auto csp = cq.span();
        QCOMPARE(static_cast<reg>(csp.size()), reg{0u});
    }
#endif

    // RAII guards should be falsey.
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

    // Snapshot on invalid should be empty and non-consumable.
    {
        auto s = q.make_snapshot();
        QVERIFY(s.size() == 0u);
        QVERIFY(!q.try_consume(s));
    }
}

// ------------------------------ dynamic-only paranoia ------------------------------

template <class Q>
static void test_dynamic_resize_reserve_destroy() {
    // default invalid
    Q q;
    verify_invariants(q);

    // reserve() on invalid should allocate (via resize)
    QVERIFY(q.reserve(1u));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 2u);
    auto* p_init = q.data();
    QVERIFY(p_init != nullptr);
    // no-grow reserve must not reallocate
    QVERIFY(q.reserve(1u));
    QCOMPARE(q.data(), p_init);
    q.destroy();
    verify_invariants(q);

    // ctor(requested_capacity)
    Q q2(reg{7u});
    QVERIFY(q2.is_valid());
    QVERIFY(q2.capacity() >= 7u);
    QVERIFY(is_pow2(q2.capacity()));
    verify_invariants(q2);

    // resize creates/grows
    QVERIFY(q.resize(8u));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 8u);
    QVERIFY(is_pow2(q.capacity()));

    // destroy() on live queue must invalidate and release storage
    {
        QVERIFY(q.resize(8u));
        QVERIFY(q.is_valid());
        QVERIFY(q.try_push(Traced{123}));
        auto* p_live = q.data();
        QVERIFY(p_live != nullptr);
        q.destroy();
        verify_invariants(q);
        // must be invalid and storage freed
        QVERIFY(!q.is_valid());
        QCOMPARE(q.data(), nullptr);
    }

    // restore for further tests
    QVERIFY(q.resize(8u));
    QVERIFY(q.is_valid());

    // data migration on grow (order must remain, including wrapped layout)
    {
        // Fill to capacity, then create a wrapped layout by popping some and pushing more.
        const reg cap0 = q.capacity();
        QVERIFY(cap0 >= 8u);

        for (reg i = 0; i < cap0; ++i) {
            QVERIFY(q.try_push(Traced{static_cast<int>(i)}));
        }

        // Remove 3 -> tail moves into the middle
        q.pop(reg{3u});

        // Push 3 more -> ring is full again, but the logical sequence is wrapped
        QVERIFY(q.try_push(Traced{2000}));
        QVERIFY(q.try_push(Traced{2001}));
        QVERIFY(q.try_push(Traced{2002}));

        const reg cap_before = q.capacity();
        QVERIFY(q.resize(cap_before + 1u)); // next_pow2 -> forces growth (e.g. 8 -> 16)
        QVERIFY(q.capacity() > cap_before);

        // Expected order after wrap + grow
        const int expected[] = {3, 4, 5, 6, 7, 2000, 2001, 2002};
        for (int v : expected) {
            QCOMPARE(q.front().v, v);
            q.pop();
        }
        QVERIFY(q.empty());
    }

    // reserve should not shrink
    const reg cap0 = q.capacity();
    QVERIFY(q.reserve(4u));
    QCOMPARE(q.capacity(), cap0);
    // no-grow reserve must not reallocate
    auto* p_before = q.data();
    QVERIFY(p_before != nullptr);
    QVERIFY(q.reserve(cap0));
    QCOMPARE(q.data(), p_before);

    // resize smaller should not shrink (grow-only policy)
    QVERIFY(q.resize(2u));
    QCOMPARE(q.capacity(), cap0);
    // no-grow resize must not reallocate
    QCOMPARE(q.data(), p_before);

    // destroy makes it invalid
    q.destroy();
    verify_invariants(q);

    // resize(0) is explicit destroy-like behavior
    QVERIFY(q.resize(16u));
    QVERIFY(q.is_valid());
    QVERIFY(q.resize(0u));
    verify_invariants(q);
}

// ------------------------------ full suites ------------------------------

template <class Policy>
static void run_static_suite() {
    using Q = spsc::fifo<Traced, 16, Policy>;

    test_constructors_and_assignment<Q>();

    Q q;
    QVERIFY(q.is_valid());
    test_introspection_and_data(q);
    test_shadow_sync_regressions<Q>();
    test_push_pop_front_try(q);
    test_emplace(q);
    test_claim_publish(q);
    test_indexing_and_iterators(q);
    test_bulk_regions(q);
    test_snapshots(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);

    paranoid_random_fuzz(q, /*seed*/ 0x123456 + int(sizeof(Policy)), /*iters*/ kFuzzIters);
}

template <class Policy>
static void run_dynamic_suite() {
    using Q = spsc::fifo<Traced, 0, Policy>;

    test_dynamic_resize_reserve_destroy<Q>();

    Q q;
    QVERIFY(!q.is_valid());
    test_invalid_queue_behavior(q);
    test_shadow_sync_regressions<Q>();

    // basic suite after init
    QVERIFY(q.resize(16u));
    QVERIFY(q.is_valid());

    test_constructors_and_assignment<Q>();

    test_introspection_and_data(q);
    test_push_pop_front_try(q);
    test_emplace(q);
    test_claim_publish(q);
    test_indexing_and_iterators(q);
    test_bulk_regions(q);
    test_snapshots(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);

    paranoid_random_fuzz(q, /*seed*/ 0xBADC0DE + int(sizeof(Policy)), /*iters*/ kFuzzIters);

    // copy must not alias memory (dynamic) - ctor
    {
        Q a;
        QVERIFY(a.resize(8u));
        QVERIFY(a.try_push(Traced{1}));
        Q b(a);
        QVERIFY(b.is_valid());
        QVERIFY(a.data() != b.data());
        QCOMPARE(b.front().v, 1);
        b.pop();
    }

    // copy-assign between two VALID queues with different capacities must not alias
    {
        Q rhs;
        QVERIFY(rhs.resize(32u));
        QVERIFY(rhs.try_push(Traced{10}));
        QVERIFY(rhs.try_push(Traced{11}));
        QVERIFY(rhs.try_push(Traced{12}));

        Q lhs;
        QVERIFY(lhs.resize(8u));
        QVERIFY(lhs.try_push(Traced{999}));

        lhs = rhs;
        QVERIFY(lhs.is_valid());
        QCOMPARE(lhs.size(), rhs.size());
        QVERIFY(lhs.data() != rhs.data());

        // Must preserve order/content
        QCOMPARE(lhs.front().v, 10);
        lhs.pop();
        QCOMPARE(lhs.front().v, 11);
        lhs.pop();
        QCOMPARE(lhs.front().v, 12);
        lhs.pop();
        QVERIFY(lhs.empty());
    }

    // move should steal and make other invalid (dynamic)
    {
        Q a;
        QVERIFY(a.resize(8u));
        QVERIFY(a.try_push(Traced{9}));
        auto* p_old = a.data();

        Q b(std::move(a));
        QVERIFY(b.is_valid());
        QVERIFY(b.data() == p_old);
        QCOMPARE(b.front().v, 9);

#if !defined(__clang_analyzer__)
        QVERIFY(!a.is_valid());
        QCOMPARE(a.data(), nullptr);
#endif
    }
}



// -------------------------------------------------------------------------------------------------
// Two-thread SPSC stress (atomic backends only).
//
// Goal:
//  - Validate the hot-path contract under real concurrency (producer/consumer threads).
//  - Check strict monotonicity + data integrity (no corruption, no duplication, no holes).
//
// Notes:
//  - Plain/Volatile counters are intentionally excluded (not multi-thread safe).
//  - Conservative on Windows schedulers: bounded waiting + timeout.
// -------------------------------------------------------------------------------------------------

struct ThreadMsg final {
    std::uint32_t seq{0u};
    std::uint32_t cookie{0u};
    std::uint32_t pad0{0u};
    std::uint32_t pad1{0u};
};

static RB_FORCEINLINE ThreadMsg make_msg(std::uint32_t seq) noexcept {
    ThreadMsg m{};
    m.seq    = seq;
    m.cookie = seq ^ 0xA5A5'5A5Au;
    m.pad0   = seq * 3u + 1u;
    m.pad1   = seq * 7u + 2u;
    return m;
}

static RB_FORCEINLINE bool msg_matches(const ThreadMsg& m, std::uint32_t expected_seq) noexcept {
    if (m.seq != expected_seq) {
        return false;
    }
    if (m.cookie != (expected_seq ^ 0xA5A5'5A5Au)) {
        return false;
    }
    if (m.pad0 != (expected_seq * 3u + 1u)) {
        return false;
    }
    if (m.pad1 != (expected_seq * 7u + 2u)) {
        return false;
    }
    return true;
}

static RB_FORCEINLINE void backoff_step(std::uint32_t& spins) noexcept {
    ++spins;
    if ((spins & 0xFFu) == 0u) {
        std::this_thread::yield();
    }
}

template <class Q>
static void threaded_spsc_stress_fifo(Q& q, const int iters = kThreadIters, const int timeout_ms = kThreadTimeoutMs) {
    static_assert(std::is_same_v<typename Q::value_type, ThreadMsg>,
                  "threaded_spsc_stress_fifo: Q::value_type must be ThreadMsg");

    QVERIFY(q.is_valid());
    QVERIFY(q.empty());

    std::atomic<bool> abort{false};
    std::atomic<int> fail_code{0};
    std::atomic<std::uint32_t> fail_seq{0u};
    std::atomic<std::uint32_t> produced{0u};
    std::atomic<std::uint32_t> consumed{0u};

    std::atomic<bool> prod_done{false};
    std::atomic<bool> cons_done{false};

    auto record_fail = [&](int code, std::uint32_t seq) noexcept {
        int expected = 0;
        if (fail_code.compare_exchange_strong(expected, code)) {
            fail_seq.store(seq, std::memory_order_relaxed);
        }
        abort.store(true, std::memory_order_relaxed);
    };

    std::thread producer([&] {
        std::uint32_t spins = 0u;
        std::uint32_t seq = 1u;

        while (!abort.load(std::memory_order_relaxed) && seq <= static_cast<std::uint32_t>(iters)) {
            auto* slot = q.try_claim();
            if (slot == nullptr) {
                backoff_step(spins);
                continue;
            }

            *slot = make_msg(seq);

            if (!q.try_publish()) {
                record_fail(1, seq);
                break;
            }

            produced.fetch_add(1u, std::memory_order_relaxed);
            ++seq;
            spins = 0u;
        }

        prod_done.store(true, std::memory_order_relaxed);
    });

    std::thread consumer([&] {
        std::uint32_t spins = 0u;
        std::uint32_t expected = 1u;

        while (!abort.load(std::memory_order_relaxed) && expected <= static_cast<std::uint32_t>(iters)) {
            const auto* f = q.try_front();
            if (f == nullptr) {
                backoff_step(spins);
                continue;
            }

            if (!msg_matches(*f, expected)) {
                record_fail(2, expected);
                break;
            }

            if (!q.try_pop()) {
                record_fail(3, expected);
                break;
            }

            consumed.fetch_add(1u, std::memory_order_relaxed);
            ++expected;
            spins = 0u;
        }

        cons_done.store(true, std::memory_order_relaxed);
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (!abort.load(std::memory_order_relaxed)
           && !(prod_done.load(std::memory_order_relaxed) && cons_done.load(std::memory_order_relaxed))) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        if (elapsed.count() > timeout_ms) {
            abort.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (producer.joinable()) {
        producer.join();
    }
    if (consumer.joinable()) {
        consumer.join();
    }

    const int code = fail_code.load(std::memory_order_relaxed);
    if (code != 0) {
        const std::uint32_t seq = fail_seq.load(std::memory_order_relaxed);
        const QString msg = QString("2-thread fifo stress failed: code=%1 seq=%2 produced=%3 consumed=%4 size=%5")
                                .arg(code)
                                .arg(seq)
                                .arg(produced.load())
                                .arg(consumed.load())
                                .arg(q.size());
        QVERIFY2(false, qPrintable(msg));
    }

    QCOMPARE(static_cast<int>(produced.load(std::memory_order_relaxed)), iters);
    QCOMPARE(static_cast<int>(consumed.load(std::memory_order_relaxed)), iters);
    QVERIFY(q.empty());
    verify_invariants(q);
}

template <typename Policy>
static void run_threaded_suite() {
    using Qs = spsc::fifo<ThreadMsg, 4096u, Policy>;
    using Qd = spsc::fifo<ThreadMsg, 0u, Policy>;

    {
        Qs q;
        threaded_spsc_stress_fifo(q);
    }

    {
        Qd q;
        QVERIFY(q.resize(4096u));
        threaded_spsc_stress_fifo(q);
    }
}

template <class Q>
static void deterministic_interleaving_suite(Q& q) {
    using T = typename Q::value_type;
    static_assert(std::is_same_v<T, std::uint32_t>, "deterministic_interleaving_suite expects fifo<uint32_t>.");

    QVERIFY(q.is_valid());
    q.clear();
    QVERIFY(q.empty());

    auto* s0 = q.try_claim();
    QVERIFY(s0 != nullptr);
    *s0 = 1u;

    // claim() must not reserve: repeated claim points to the same slot until publish().
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
    using Q = spsc::fifo<ThreadMsg, 0u, Policy>;

    Q q;
    QVERIFY2(q.resize(1024u), name);
    QVERIFY2(q.is_valid(), name);

    std::atomic<bool> abort{false};
    std::atomic<bool> prod_done{false};
    std::atomic<int> fail{0};

    auto should_abort = [&]() -> bool {
        return abort.load(std::memory_order_relaxed);
    };

    std::thread prod([&]() {
        for (int i = 1; i <= kThreadIters && !should_abort(); ++i) {
            const auto msg = make_msg(static_cast<std::uint32_t>(i));
            while (!q.try_push(msg)) {
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

            // Iterator must not loop forever under concurrent producer activity.
            {
                reg steps = 0u;
                for (const auto& v : q) {
                    (void)v;
                    if (++steps > q.capacity()) {
                        fail.store(2, std::memory_order_relaxed);
                        abort.store(true, std::memory_order_relaxed);
                        return;
                    }
                }
            }

            auto snap = q.make_snapshot();
            const reg sz = snap.size();

            if (sz == 0u) {
                if (!(snap.begin() == snap.end())) {
                    fail.store(3, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                if (prod_done.load(std::memory_order_acquire) && q.empty() &&
                    expected <= static_cast<std::uint32_t>(kThreadIters)) {
                    fail.store(4, std::memory_order_relaxed);
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
                    fail.store(5, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                if (!msg_matches(*it, cur)) {
                    fail.store(6, std::memory_order_relaxed);
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                ++cur;
                ++cnt;
            }

            if (cnt != sz) {
                fail.store(7, std::memory_order_relaxed);
                abort.store(true, std::memory_order_relaxed);
                return;
            }

            if (!q.try_consume(snap)) {
                fail.store(8, std::memory_order_relaxed);
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
    q.destroy();
}

template <class Policy>
static void run_threaded_bulk_regions_suite(const char* name) {
    using Q = spsc::fifo<ThreadMsg, 0u, Policy>;

    Q q;
    QVERIFY2(q.resize(1024u), name);

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
                wr.first.ptr[i] = make_msg(seq++);
                ++written;
            }
            for (reg i = 0u; i < wr.second.count && seq <= static_cast<std::uint32_t>(kThreadIters); ++i) {
                wr.second.ptr[i] = make_msg(seq++);
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

            auto check_span = [&](const ThreadMsg* p, const reg n) {
                for (reg i = 0u; i < n; ++i) {
                    if (!msg_matches(p[i], expect)) {
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

static void allocator_accounting_suite() {
    using Q = spsc::fifo<Tracked, 0u, spsc::policy::P,
                         CountingAlignedAlloc<Tracked, alignof(Tracked)>>;
    tracked_reset();
    AllocStats::reset();

    {
        Q q;
        QVERIFY(!q.is_valid());
        QVERIFY(q.resize(64u));
        QVERIFY(q.is_valid());
        QCOMPARE(AllocStats::allocs.load(), std::size_t{1});
        QVERIFY(AllocStats::bytes_live.load() >= 64u * sizeof(Tracked));

        fill_seq_tracked(q, 1u, 10u);
        QVERIFY(q.resize(256u));
        QVERIFY(q.capacity() >= 256u);
        QVERIFY(AllocStats::allocs.load() >= std::size_t{2});
        QVERIFY(AllocStats::deallocs.load() >= std::size_t{1});

        q.destroy();
        QVERIFY(!q.is_valid());
    }

    QCOMPARE(AllocStats::bytes_live.load(), std::size_t{0});
    QCOMPARE(AllocStats::allocs.load(), AllocStats::deallocs.load());
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void dynamic_capacity_sweep_suite() {
    using Q = spsc::fifo<std::uint32_t, 0u, spsc::policy::P>;
    Q q;

    for (reg req = 0u; req < 2048u; req += 7u) {
        const reg want = (req == 0u) ? 0u : req;
        QVERIFY(q.resize(want));

        if (want == 0u) {
            QVERIFY(!q.is_valid());
            QCOMPARE(q.capacity(), reg{0u});
            continue;
        }

        QVERIFY(q.is_valid());
        const reg cap = q.capacity();
        QVERIFY(cap >= 2u);
        QVERIFY((cap & (cap - 1u)) == 0u);

        q.clear();
        const std::uint32_t n = static_cast<std::uint32_t>((cap < 5u) ? cap : 5u);
        fill_seq_u32(q, 1u, n);
        check_fifo_exact_u32(q, 1u, n);
    }

    q.destroy();
}

static void move_swap_stress_suite() {
    using Q = spsc::fifo<Tracked, 0u, spsc::policy::P>;
    tracked_reset();

    Q a;
    Q b;
    QVERIFY(a.resize(128u));
    QVERIFY(b.resize(128u));

    std::uint32_t seq = 1u;
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

enum class FuzzOp : std::uint8_t {
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

static FuzzOp pick_fuzz_op(std::mt19937& rng) {
    std::uniform_int_distribution<int> d(0, 9);
    return static_cast<FuzzOp>(d(rng));
}

template <class Policy>
static void run_state_machine_fuzz(const bool dynamic) {
    using QStatic = spsc::fifo<Tracked, 64u, Policy>;
    using QDynamic = spsc::fifo<Tracked, 0u, Policy>;

    tracked_reset();
    std::mt19937 rng(0xC0FFEEu + static_cast<std::uint32_t>(sizeof(Policy) * 17u) + (dynamic ? 1u : 0u));
    std::uniform_int_distribution<int> val(1, 1'000'000);
    std::deque<std::uint32_t> model;

    auto check_equal = [&](auto& q) {
        QCOMPARE(q.size(), reg(model.size()));
        for (reg i = 0u; i < q.size(); ++i) {
            QCOMPARE(q[i].seq, model[static_cast<std::size_t>(i)]);
            QCOMPARE(q[i].cookie, model[static_cast<std::size_t>(i)] ^ 0xA5A5A5A5u);
        }
        if (!model.empty()) {
            QCOMPARE(q.front().seq, model.front());
            QCOMPARE(q.front().cookie, model.front() ^ 0xA5A5A5A5u);
        }
    };

    auto do_one = [&](auto& q, auto& other) {
        const auto op = pick_fuzz_op(rng);
        switch (op) {
        case FuzzOp::TryPush: {
            const auto x = static_cast<std::uint32_t>(val(rng));
            if (q.try_push(Tracked{x})) {
                model.push_back(x);
            }
            break;
        }
        case FuzzOp::TryEmplace: {
            const auto x = static_cast<std::uint32_t>(val(rng));
            auto* p = q.try_emplace(x);
            if (p != nullptr) {
                model.push_back(x);
            }
            break;
        }
        case FuzzOp::ClaimPublish: {
            if (q.full()) {
                break;
            }
            const auto x = static_cast<std::uint32_t>(val(rng));
            auto* slot = q.try_claim();
            QVERIFY(slot != nullptr);
            *slot = Tracked{x};
            QVERIFY(q.try_publish());
            model.push_back(x);
            break;
        }
        case FuzzOp::RegionPublish: {
            if (!q.can_write(1u)) {
                break;
            }
            const reg maxn = static_cast<reg>(1u + (rng() % 7u));
            auto wr = q.claim_write(::spsc::unsafe, maxn);
            if (wr.empty()) {
                break;
            }
            for (reg i = 0u; i < wr.first.count; ++i) {
                const auto x = static_cast<std::uint32_t>(val(rng));
                wr.first.ptr[i] = Tracked{x};
                model.push_back(x);
            }
            for (reg i = 0u; i < wr.second.count; ++i) {
                const auto x = static_cast<std::uint32_t>(val(rng));
                wr.second.ptr[i] = Tracked{x};
                model.push_back(x);
            }
            q.publish(wr.total);
            break;
        }
        case FuzzOp::TryPop: {
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
        case FuzzOp::PopN: {
            if (model.empty()) {
                break;
            }
            const reg n = static_cast<reg>(1u + (rng() % 5u));
            const reg can = q.can_read(n) ? n : q.size();
            q.pop(can);
            for (reg i = 0u; i < can; ++i) {
                model.pop_front();
            }
            break;
        }
        case FuzzOp::SnapshotConsume: {
            auto snap = q.make_snapshot();
            const reg snap_sz = static_cast<reg>(snap.size());
            if (snap_sz == 0u) {
                break;
            }
            QVERIFY(q.try_consume(snap));
            for (reg i = 0u; i < snap_sz; ++i) {
                model.pop_front();
            }
            break;
        }
        case FuzzOp::Clear: {
            q.clear();
            model.clear();
            break;
        }
        case FuzzOp::Resize: {
            if constexpr (std::is_same_v<std::decay_t<decltype(q)>, QDynamic>) {
                const reg req = static_cast<reg>(2u << (rng() % 9u));
                QVERIFY(q.resize(req));
                QVERIFY(q.is_valid());
                QVERIFY(q.capacity() >= 2u);
            }
            break;
        }
        case FuzzOp::Swap: {
            q.swap(other);
            model.clear();
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
        QVERIFY(q.resize(64u));
        QVERIFY(other.resize(64u));
        for (int i = 0; i < kFuzzIters; ++i) {
            do_one(q, other);
        }
        q.destroy();
        other.destroy();
    } else {
        {
            QStatic q;
            QStatic other;
            for (int i = 0; i < kFuzzIters; ++i) {
                do_one(q, other);
            }
        }
    }

    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void state_machine_fuzz_sweep_suite() {
    run_state_machine_fuzz<spsc::policy::P>(false);
    run_state_machine_fuzz<spsc::policy::A<>>(false);
    run_state_machine_fuzz<spsc::policy::P>(true);
    run_state_machine_fuzz<spsc::policy::CA<>>(true);
}

static void resize_migration_order_suite() {
    using Q = spsc::fifo<Tracked, 0u, spsc::policy::P>;
    tracked_reset();

    Q q;
    QVERIFY(q.resize(32u));

    fill_seq_tracked(q, 1u, 25u);
    QVERIFY(q.resize(256u));
    QCOMPARE(q.size(), reg{25u});
    check_fifo_exact_tracked(q, 1u, 25u);

    q.destroy();
    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void snapshot_try_consume_contract_suite() {
    using Q = spsc::fifo<Tracked, 64u, spsc::policy::P>;
    tracked_reset();

    {
        Q q;
        fill_seq_tracked(q, 1u, 10u);
        auto snap = q.make_snapshot();

        q.pop();
        QVERIFY(!q.try_consume(snap));

        auto snap2 = q.make_snapshot();
        QVERIFY(q.try_consume(snap2));
        QVERIFY(q.empty());
    }

    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void snapshot_iteration_contract_suite() {
    using Q = spsc::fifo<std::uint32_t, 64u, spsc::policy::P>;
    Q q;
    fill_seq_u32(q, 1u, 30u);

    auto snap = q.make_snapshot();
    reg n = 0u;
    std::uint32_t exp = 1u;
    for (auto it = snap.begin(); it != snap.end(); ++it) {
        QCOMPARE(*it, exp++);
        ++n;
    }
    QCOMPARE(n, reg{30u});

    q.pop(10u);
    fill_seq_u32(q, 1000u, 10u);

    n = 0u;
    exp = 1u;
    for (auto it = snap.begin(); it != snap.end(); ++it) {
        QCOMPARE(*it, exp++);
        ++n;
    }
    QCOMPARE(n, reg{30u});
    QVERIFY(!q.try_consume(snap));
}

template <class Q>
static void bulk_regions_max_count_suite(Q& q) {
    q.clear();
    const reg limit = 7u;
    auto wr = q.claim_write(::spsc::unsafe, limit);
    QCOMPARE(wr.total, limit);
    QVERIFY(wr.first.count > 0u);
    QCOMPARE(wr.first.count + wr.second.count, wr.total);

    std::uint32_t v = 1u;
    for (reg i = 0u; i < wr.first.count; ++i) {
        wr.first.ptr[i] = v++;
    }
    for (reg i = 0u; i < wr.second.count; ++i) {
        wr.second.ptr[i] = v++;
    }

    q.publish(wr.total);
    QCOMPARE(q.size(), limit);
    check_fifo_exact_u32(q, 1u, static_cast<std::uint32_t>(limit));
}

template <class Q>
static void bulk_read_max_count_suite(Q& q) {
    q.clear();
    fill_seq_u32(q, 1u, 30u);
    QCOMPARE(q.size(), reg{30u});

    auto rr = q.claim_read(::spsc::unsafe, 7u);
    QCOMPARE(rr.total, reg{7u});

    std::uint32_t expected = 1u;
    for (reg i = 0u; i < rr.first.count; ++i) {
        QCOMPARE(rr.first.ptr[i], expected++);
    }
    for (reg i = 0u; i < rr.second.count; ++i) {
        QCOMPARE(rr.second.ptr[i], expected++);
    }

    q.pop(rr.total);
    QCOMPARE(q.size(), reg{23u});
    check_fifo_exact_u32(q, 8u, 23u);
}

template <class Q>
static void guard_move_semantics_suite(Q& q) {
    q.clear();
    QVERIFY(q.empty());

    {
        auto w1 = q.scoped_write();
        QVERIFY(w1);
        w1.ref() = Tracked{1u};

        auto w2 = std::move(w1);
        QVERIFY(!w1);
        QVERIFY(w2);
        w2.commit();
    }
    QCOMPARE(q.size(), reg{1u});
    QCOMPARE(q.front().seq, 1u);

    {
        auto r1 = q.scoped_read();
        QVERIFY(r1);
        auto r2 = std::move(r1);
        QVERIFY(!r1);
        QVERIFY(r2);
        QCOMPARE(r2->seq, 1u);
    }
    QVERIFY(q.empty());
}

static void reserve_resize_edge_cases_dynamic_suite() {
    using Q = spsc::fifo<std::uint32_t, 0u, spsc::policy::P>;
    Q q;

    QVERIFY(!q.is_valid());
    QVERIFY(q.reserve(1u));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 2u);

    const reg cap_min = q.capacity();
    QVERIFY(q.reserve(1u));
    QCOMPARE(q.capacity(), cap_min);

    QVERIFY(q.resize(2u));
    QCOMPARE(q.capacity(), cap_min);

    QVERIFY(q.reserve(16u));
    QVERIFY(q.capacity() >= 16u);
    const reg cap0 = q.capacity();

    fill_seq_u32(q, 1u, 10u);
    const reg sz_before = q.size();
    QVERIFY(q.reserve(cap0 * 4u));
    QVERIFY(q.capacity() >= cap0 * 4u);
    QCOMPARE(q.size(), sz_before);
    check_fifo_exact_u32(q, 1u, static_cast<std::uint32_t>(sz_before));

    QVERIFY(q.resize(0u));
    QVERIFY(!q.is_valid());
    QCOMPARE(q.capacity(), reg{0u});
    QVERIFY(q.empty());

    QVERIFY(q.reserve(64u));
    QVERIFY(q.is_valid());
    QVERIFY(q.capacity() >= 64u);

    q.destroy();
    QVERIFY(!q.is_valid());
}

template <class Q>
static void snapshot_consume_suite(Q& q) {
    q.clear();
    QVERIFY(q.empty());

    fill_seq_u32(q, 1u, 12u);
    QCOMPARE(q.size(), reg{12u});

    auto snap = q.make_snapshot();
    QCOMPARE(static_cast<reg>(snap.size()), reg{12u});
    q.consume(snap);
    QVERIFY(q.empty());

    fill_seq_u32(q, 100u, 5u);
    const Q& cq = q;
    auto csnap = cq.make_snapshot();
    QCOMPARE(static_cast<reg>(csnap.size()), reg{5u});

    std::uint32_t exp = 100u;
    for (auto it = csnap.begin(); it != csnap.end(); ++it) {
        QCOMPARE(*it, exp++);
    }

    q.clear();
    QVERIFY(q.empty());
}

static void snapshot_invalid_queue_suite() {
    using Q = spsc::fifo<std::uint32_t, 0u, spsc::policy::P>;
    Q q;
    QVERIFY(!q.is_valid());

    auto snap = q.make_snapshot();
    QCOMPARE(static_cast<reg>(snap.size()), reg{0u});
    QVERIFY(snap.begin() == snap.end());

    QVERIFY(!q.try_consume(snap));
}

static void dynamic_move_contract_suite() {
    using Q = spsc::fifo<Tracked, 0u, spsc::policy::P>;
    tracked_reset();

    {
        Q a;
        QVERIFY(a.resize(64u));
        fill_seq_tracked(a, 1u, 20u);

        Q b{std::move(a)};
        QVERIFY(!a.is_valid());
        QVERIFY(b.is_valid());
        QCOMPARE(b.size(), reg{20u});
        for (reg i = 0u; i < b.size(); ++i) {
            QCOMPARE(b[i].seq, static_cast<std::uint32_t>(i + 1u));
        }

        Q c;
        QVERIFY(c.resize(32u));
        fill_seq_tracked(c, 1000u, 5u);

        c = std::move(b);
        QVERIFY(!b.is_valid());
        QVERIFY(c.is_valid());
        QCOMPARE(c.size(), reg{20u});
        check_fifo_exact_tracked(c, 1u, 20u);

        c.destroy();
    }

    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void consume_all_contract_suite() {
    {
        spsc::fifo<std::uint32_t, 64u, spsc::policy::P> q;
        fill_seq_u32(q, 1u, 40u);
        QVERIFY(!q.empty());
        q.consume_all();
        QVERIFY(q.empty());
    }

    tracked_reset();
    {
        spsc::fifo<Tracked, 64u, spsc::policy::P> q;
        fill_seq_tracked(q, 1u, 40u);
        QVERIFY(!q.empty());
        q.consume_all();
        QVERIFY(q.empty());
    }

    QCOMPARE(Tracked::live.load(), 0);
    QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
}

static void stress_cached_ca_transitions_suite() {
    using QS = spsc::fifo<std::uint32_t, 64u, spsc::policy::CA<>>;
    using QD = spsc::fifo<std::uint32_t, 0u, spsc::policy::CA<>>;

    {
        QS a;
        QS b;

        for (std::uint32_t i = 1u; i <= 17u; ++i) {
            QVERIFY(a.try_push(i));
        }
        for (std::uint32_t i = 100u; i < 121u; ++i) {
            QVERIFY(b.try_push(i));
        }

        a.swap(b);
        QCOMPARE(a.front(), std::uint32_t{100u});
        QCOMPARE(b.front(), std::uint32_t{1u});

        full_empty_cycle_u32(a, 1000u);
        full_empty_cycle_u32(b, 2000u);
    }

    {
        QS src;
        for (std::uint32_t i = 1u; i <= 33u; ++i) {
            QVERIFY(src.try_push(i));
        }

        QS dst(std::move(src));
        QCOMPARE(dst.size(), reg{33u});
        full_empty_cycle_u32(dst, 3000u);
    }

    {
        QS a;
        QS b;

        for (std::uint32_t i = 10u; i < 30u; ++i) {
            QVERIFY(a.try_push(i));
        }
        for (std::uint32_t i = 200u; i < 210u; ++i) {
            QVERIFY(b.try_push(i));
        }

        a = std::move(b);
        QCOMPARE(a.front(), std::uint32_t{200u});
        full_empty_cycle_u32(a, 4000u);
    }

    {
        QD q;
        QVERIFY(q.resize(64u));
        for (std::uint32_t i = 1u; i <= 48u; ++i) {
            QVERIFY(q.try_push(i));
        }

        QVERIFY(q.resize(128u));
        full_empty_cycle_u32(q, 5000u);

        QVERIFY(q.resize(64u));
        for (std::uint32_t i = 1u; i <= 10u; ++i) {
            QVERIFY(q.try_push(6000u + i));
        }

        QVERIFY(q.resize(16u));
        full_empty_cycle_u32(q, 7000u);
    }
}

static void lifecycle_traced_suite() {
    tracked_reset();
    {
        spsc::fifo<Tracked, 64u, spsc::policy::P> q;
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

// -------------------------------------------------------------------------------------------------
// Alignment sweep (owning fifo): backing storage must satisfy alignof(T) even for over-aligned T.
//
// We do NOT test "bad allocators" here. If the allocator violates the C++ allocator contract
// (returns misaligned storage for T), any container can be forced into UB.
// -------------------------------------------------------------------------------------------------

template <std::size_t Align>
struct alignas(Align) AlignBlob final {
    std::uint32_t seq{0u};
    std::uint32_t cookie{0u};
    std::uint8_t  payload[32]{};
};

template <class T>
static void expect_ptr_aligned_to(const T* p) {
    const std::uintptr_t u = reinterpret_cast<std::uintptr_t>(p);
    QVERIFY((u % alignof(T)) == 0u);
}

template <class Q>
static void alignment_smoke_push_pop(Q& q) {
    QVERIFY(q.is_valid());
    expect_ptr_aligned_to(q.data());

    for (std::uint32_t i = 1u; i <= 128u; ++i) {
        auto* s = q.try_claim();
        QVERIFY(s != nullptr);

        s->seq    = i;
        s->cookie = i ^ 0xC0FF'EE01u;

        QVERIFY(q.try_publish());

        const auto* f = q.try_front();
        QVERIFY(f != nullptr);
        QCOMPARE(f->seq, i);
        QCOMPARE(f->cookie, i ^ 0xC0FF'EE01u);

        q.pop();
        QVERIFY(q.empty());
    }

    verify_invariants(q);
}

template <typename Policy, std::size_t Align>
static void sweep_static_one_align() {
    using T = AlignBlob<Align>;
    using Q = spsc::fifo<T, 256u, Policy>;

    Q q;
    alignment_smoke_push_pop(q);
}

template <typename Policy, std::size_t Align>
static void sweep_dynamic_one_align() {
    using T = AlignBlob<Align>;
    using Q = spsc::fifo<T, 0u, Policy, spsc::alloc::align_alloc<Align>>;

    Q q;
    QVERIFY(q.resize(256u));
    alignment_smoke_push_pop(q);
}

template <typename Policy>
static void run_alignment_sweep_static() {
    sweep_static_one_align<Policy, 4u>();
    sweep_static_one_align<Policy, 8u>();
    sweep_static_one_align<Policy, 16u>();
    sweep_static_one_align<Policy, 32u>();
    sweep_static_one_align<Policy, 64u>();
    sweep_static_one_align<Policy, 128u>();
    sweep_static_one_align<Policy, 256u>();
}

template <typename Policy>
static void run_alignment_sweep_dynamic() {
    sweep_dynamic_one_align<Policy, 4u>();
    sweep_dynamic_one_align<Policy, 8u>();
    sweep_dynamic_one_align<Policy, 16u>();
    sweep_dynamic_one_align<Policy, 32u>();
    sweep_dynamic_one_align<Policy, 64u>();
    sweep_dynamic_one_align<Policy, 128u>();
    sweep_dynamic_one_align<Policy, 256u>();
}

static void alignment_sweep_all() {
    run_alignment_sweep_static<spsc::policy::P>();
    run_alignment_sweep_static<spsc::policy::V>();
    run_alignment_sweep_static<spsc::policy::A<>>();
    run_alignment_sweep_static<spsc::policy::CA<>>();

    run_alignment_sweep_dynamic<spsc::policy::P>();
    run_alignment_sweep_dynamic<spsc::policy::V>();
    run_alignment_sweep_dynamic<spsc::policy::A<>>();
    run_alignment_sweep_dynamic<spsc::policy::CA<>>();
}


// ------------------------------ regression matrix (many capacities, many APIs) ------------------------------

template <class Policy>
static void regression_matrix_one_policy() {
    using Q = spsc::fifo<std::uint32_t, 0, Policy>;

#if defined(NDEBUG)
    constexpr reg caps[] = {2u, 4u, 8u, 16u, 32u, 64u, 128u, 256u, 512u, 1024u};
#else
    constexpr reg caps[] = {2u, 4u, 8u, 16u, 32u, 64u};
#endif

    for (const reg cap : caps) {
        Q q;
        QVERIFY(q.resize(cap));
        QVERIFY(q.is_valid());
        QCOMPARE(q.capacity(), cap);

        // 1) Fill to full and confirm the boundary.
        for (reg i = 0; i < cap; ++i) {
            QVERIFY(q.try_push(static_cast<std::uint32_t>(i + 1u)));
        }
        QVERIFY(q.full());
        QVERIFY(!q.try_push(0xDEADBEEFu));
        QCOMPARE(q.size(), cap);
        QCOMPARE(q.free(), reg{0u});

        // 2) Drain and confirm ordering.
        for (reg i = 0; i < cap; ++i) {
            const std::uint32_t v = q.front();
            QCOMPARE(v, static_cast<std::uint32_t>(i + 1u));
            q.pop();
        }
        QVERIFY(q.empty());
        QVERIFY(!q.try_pop());
        QCOMPARE(q.size(), reg{0u});
        QCOMPARE(q.free(), cap);

        // 3) Deterministic wrap: push 2, pop 1, then fill to full.
        QVERIFY(q.try_push(1u));
        QVERIFY(q.try_push(2u));
        q.pop();

        for (reg i = 0; i < cap - 1u; ++i) {
            QVERIFY(q.try_push(static_cast<std::uint32_t>(3u + i)));
        }
        QVERIFY(q.full());

        // Expected sequence: 2,3,4,...,cap+1
        {
            std::uint32_t expected = 2u;
            while (!q.empty()) {
                const std::uint32_t v = q.front();
                QCOMPARE(v, expected++);
                q.pop();
            }
            QCOMPARE(expected, static_cast<std::uint32_t>(cap + 2u));
        }
        QVERIFY(q.empty());

        // 4) Bulk claim/publish/read/pop: exercise split regions.
        if (cap >= 8u) {
            // Create a non-zero tail index first.
            QVERIFY(q.try_push(11u));
            QVERIFY(q.try_push(12u));
            QVERIFY(q.try_push(13u));
            q.pop();
            q.pop();

            // Now claim almost full to likely split.
            const reg want = cap - 1u;
            auto wr = q.claim_write(::spsc::unsafe, want);
            QCOMPARE(wr.first.count + wr.second.count, wr.total);
            QCOMPARE(wr.total, want);

            std::uint32_t v = 1000u;
            for (reg i = 0; i < wr.first.count; ++i) {
                wr.first.ptr[i] = v++;
            }
            for (reg i = 0; i < wr.second.count; ++i) {
                wr.second.ptr[i] = v++;
            }

            q.publish(wr.total);
            QCOMPARE(q.size(), cap);
            QVERIFY(q.full());

            auto rd = q.claim_read(::spsc::unsafe);
            QCOMPARE(rd.first.count + rd.second.count, rd.total);
            QCOMPARE(rd.total, cap);

            // Linear verification across split read regions.
            std::uint32_t expected_first = 13u;
            std::uint32_t expected_bulk = 1000u;

            bool first_seen = false;
            for (reg i = 0; i < rd.first.count; ++i) {
                const std::uint32_t got = rd.first.ptr[i];
                if (!first_seen) {
                    QCOMPARE(got, expected_first);
                    first_seen = true;
                } else {
                    QCOMPARE(got, expected_bulk++);
                }
            }
            for (reg i = 0; i < rd.second.count; ++i) {
                const std::uint32_t got = rd.second.ptr[i];
                if (!first_seen) {
                    QCOMPARE(got, expected_first);
                    first_seen = true;
                } else {
                    QCOMPARE(got, expected_bulk++);
                }
            }
            QVERIFY(first_seen);
            QCOMPARE(expected_bulk, static_cast<std::uint32_t>(1000u + (cap - 1u)));

            q.pop(rd.total);
            QVERIFY(q.empty());
        }

        // 5) Destroy and re-init should leave a sane, empty queue.
        q.destroy();
        QVERIFY(!q.is_valid());
        QVERIFY(q.empty());
        QVERIFY(q.full());
        QCOMPARE(q.capacity(), reg{0u});
    }
}

static void regression_matrix_all() {
    regression_matrix_one_policy<spsc::policy::P>();
    regression_matrix_one_policy<spsc::policy::V>();
    regression_matrix_one_policy<spsc::policy::A<>>();
    regression_matrix_one_policy<spsc::policy::CA<>>();
}

// ------------------------------ API compile smoke (coverage without runtime cost) ------------------------------

template <class Q>
static void api_compile_smoke_one() {
    using value_type = typename Q::value_type;
    using size_type  = typename Q::size_type;
    using pointer    = typename Q::pointer;
    using regions     = typename Q::regions;

    // Make sure the "hot" API surface exists and types line up.
    static_assert(std::is_same_v<decltype(std::declval<Q&>().capacity()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().size()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().free()), size_type>);

    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_push(std::declval<value_type>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_claim()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_front()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop()), bool>);

    static_assert(std::is_void_v<decltype(std::declval<Q&>().publish())>);
    static_assert(std::is_void_v<decltype(std::declval<Q&>().publish(size_type{1u}))>);
    static_assert(std::is_void_v<decltype(std::declval<Q&>().pop())>);
    static_assert(std::is_void_v<decltype(std::declval<Q&>().pop(size_type{1u}))>);

    // Bulk regions.
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write(::spsc::unsafe)), regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write(::spsc::unsafe, size_type{1u})), regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read(::spsc::unsafe)), regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read(::spsc::unsafe, size_type{1u})), regions>);

    using Snap = decltype(std::declval<Q&>().make_snapshot());
    static_assert(std::is_void_v<decltype(std::declval<Q&>().consume(std::declval<const Snap&>()))>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_consume(std::declval<const Snap&>())), bool>);

    // Scoped guards should be movable.
    using WG = decltype(std::declval<Q&>().scoped_write());
    using RG = decltype(std::declval<Q&>().scoped_read());
    using BWG = decltype(std::declval<Q&>().scoped_write(size_type{1u}));
    using BRG = decltype(std::declval<Q&>().scoped_read(size_type{1u}));
    static_assert(std::is_move_constructible_v<WG>);
    static_assert(std::is_move_constructible_v<RG>);
    static_assert(std::is_move_constructible_v<BWG>);
    static_assert(std::is_move_constructible_v<BRG>);

    // Const API.
    (void)sizeof(decltype(std::declval<const Q&>().empty()));
    (void)sizeof(decltype(std::declval<const Q&>().full()));
    (void)sizeof(decltype(std::declval<const Q&>().can_write(size_type{1u})));
    (void)sizeof(decltype(std::declval<const Q&>().can_read(size_type{1u})));
    (void)sizeof(decltype(std::declval<const Q&>().read_size()));
    (void)sizeof(decltype(std::declval<const Q&>().write_size()));
}

static void api_compile_smoke_all() {
    using QP  = spsc::fifo<Traced, 16u, spsc::policy::P>;
    using QV  = spsc::fifo<Traced, 16u, spsc::policy::V>;
    using QA  = spsc::fifo<Traced, 16u, spsc::policy::A<>>;
    using QCA = spsc::fifo<Traced, 16u, spsc::policy::CA<>>;

    api_compile_smoke_one<QP>();
    api_compile_smoke_one<QV>();
    api_compile_smoke_one<QA>();
    api_compile_smoke_one<QCA>();
}

static void death_tests_debug_only_suite() {
#if !defined(NDEBUG)
    auto expect_death = [&](const char* mode) {
        QProcess p;
        p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments(QStringList{});

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SPSC_FIFO_DEATH", QString::fromLatin1(mode));
        p.setProcessEnvironment(env);

        p.start();
        QVERIFY2(p.waitForStarted(1500), "Death child failed to start.");

        if (!p.waitForFinished(8000)) {
            p.kill();
            QVERIFY2(false, "Death child did not finish (possible crash dialog)." );
        }

        const int code = p.exitCode();
        QVERIFY2(code == spsc_fifo_death_detail::kDeathExitCode,
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

class tst_fifo_api_paranoid final : public QObject {
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
        spsc::fifo<std::uint32_t, 64u, spsc::policy::P> q;
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
        spsc::fifo<Traced, 0u, spsc::policy::P> q;
        test_invalid_queue_behavior(q);
    }

    void dynamic_capacity_sweep()   { dynamic_capacity_sweep_suite(); }
    void move_swap_stress()         { move_swap_stress_suite(); }
    void state_machine_fuzz_sweep() { state_machine_fuzz_sweep_suite(); }
    void resize_migration_order()   { resize_migration_order_suite(); }
    void snapshot_try_consume_contract() { snapshot_try_consume_contract_suite(); }

    void bulk_regions_wraparound() {
        spsc::fifo<Traced, 64u, spsc::policy::P> q;
        test_bulk_regions(q);
    }

    void raii_guards_static() {
        spsc::fifo<Traced, 64u, spsc::policy::P> q;
        test_raii_guards(q);
    }

    void raii_guards_dynamic() {
        spsc::fifo<Traced, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(64u));
        test_raii_guards(q);
        q.destroy();
    }

    void iterators_and_indexing_static() {
        spsc::fifo<Traced, 64u, spsc::policy::P> q;
        test_indexing_and_iterators(q);
    }

    void iterators_wraparound_static() {
        spsc::fifo<Traced, 64u, spsc::policy::P> q;
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
        spsc::fifo<Traced, 64u, spsc::policy::P> q;
        test_bulk_raii_overloads(q);
    }

    void bulk_raii_overloads_dynamic() {
        spsc::fifo<Traced, 0u, spsc::policy::P> q;
        QVERIFY(q.resize(64u));
        test_bulk_raii_overloads(q);
        q.destroy();
    }

    void snapshot_iteration_contract() { snapshot_iteration_contract_suite(); }

    void bulk_regions_max_count_static() {
        spsc::fifo<std::uint32_t, 64u, spsc::policy::P> q;
        bulk_regions_max_count_suite(q);
    }

    void bulk_read_max_count_static() {
        spsc::fifo<std::uint32_t, 64u, spsc::policy::P> q;
        bulk_read_max_count_suite(q);
    }

    void guard_move_semantics_static() {
        tracked_reset();
        {
            spsc::fifo<Tracked, 64u, spsc::policy::P> q;
            guard_move_semantics_suite(q);
        }
        QCOMPARE(Tracked::live.load(), 0);
        QCOMPARE(Tracked::ctor.load(), Tracked::dtor.load());
    }

    void reserve_resize_edge_cases_dynamic() { reserve_resize_edge_cases_dynamic_suite(); }

    void snapshot_consume_contract() {
        spsc::fifo<std::uint32_t, 64u, spsc::policy::P> q;
        snapshot_consume_suite(q);
    }

    void snapshot_invalid_queue() { snapshot_invalid_queue_suite(); }
    void dynamic_move_contract()  { dynamic_move_contract_suite(); }
    void consume_all_contract()   { consume_all_contract_suite(); }

    void threaded_bulk_regions_atomic_A() {
        run_threaded_bulk_regions_suite<spsc::policy::A<>>("threaded_fifo_bulk_atomic");
    }
    void threaded_bulk_regions_cached_CA() {
        run_threaded_bulk_regions_suite<spsc::policy::CA<>>("threaded_fifo_bulk_cached");
    }

    void alignment_sweep() { alignment_sweep_all(); }
    void stress_cached_ca_transitions() { stress_cached_ca_transitions_suite(); }
    void regression_matrix() { regression_matrix_all(); }
    void api_smoke() { api_compile_smoke_all(); }
    void death_tests_debug_only() { death_tests_debug_only_suite(); }
    void lifecycle_traced() { lifecycle_traced_suite(); }
    void cleanupTestCase() {}
};

// ------------------------------ runner (no main here) ------------------------------
int run_tst_fifo_api_paranoid(int argc, char** argv)
{
    tst_fifo_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "fifo_test.moc"
