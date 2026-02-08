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
#include <thread>
#include <cstdint>
#include <deque>
#include <limits>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#include "fifo_test.h"
#include "fifo.hpp"

namespace {


// Scale fuzz to avoid painfully slow Debug runs.
#if defined(NDEBUG)
static constexpr int kFuzzIters = 55000;
#else
static constexpr int kFuzzIters = 12;
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
    static_assert(std::is_move_constructible_v<WG>);
    static_assert(std::is_move_constructible_v<RG>);

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


} // namespace

class tst_fifo_api_paranoid final : public QObject {
    Q_OBJECT
private slots:
    void static_plain_P()    { run_static_suite<spsc::policy::P>(); }
    void static_volatile_V() { run_static_suite<spsc::policy::V>(); }
    void static_atomic_A()   { run_static_suite<spsc::policy::A<>>(); }
    void static_cached_CA()  { run_static_suite<spsc::policy::CA<>>(); }

    void dynamic_plain_P()    { run_dynamic_suite<spsc::policy::P>(); }
    void dynamic_volatile_V() { run_dynamic_suite<spsc::policy::V>(); }
    void dynamic_atomic_A()   { run_dynamic_suite<spsc::policy::A<>>(); }
    void dynamic_cached_CA()  { run_dynamic_suite<spsc::policy::CA<>>(); }

    void threaded_atomic_A()  { run_threaded_suite<spsc::policy::A<>>(); }
    void threaded_cached_CA() { run_threaded_suite<spsc::policy::CA<>>(); }
    void alignment_sweep()    { alignment_sweep_all(); }
    void regression_matrix() { regression_matrix_all(); }
    void api_smoke()         { api_compile_smoke_all(); }
};

// ------------------------------ runner (no main here) ------------------------------
int run_tst_fifo_api_paranoid(int argc, char** argv)
{
    tst_fifo_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "fifo_test.moc"
