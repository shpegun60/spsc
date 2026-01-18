// latest_test.cpp
// Paranoid API/contract test for spsc::latest.
//
// Goals:
//  - Exercise every public method for all major flavors:
//      * static typed:    spsc::latest<T, Depth>
//      * dynamic typed:   spsc::latest<T, 0> (init at runtime)
//      * dynamic raw:     spsc::latest<void, 0> (per-slot byte buffers)
//  - Cover allocator + alignment variants.
//  - Regression: shadow-cache correctness across move/swap (atomic/cached policies).
//
// Notes:
//  - This test assumes SPSC semantics.
//  - The test intentionally does a lot of redundant checking; it's a contract tripwire.

#include <QtTest/QtTest>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "latest.hpp"

namespace {


template <class Q>
concept has_valid = requires(const Q& q) {
    { q.valid() } -> std::same_as<bool>;
};

template <class Q>
concept has_bytes_per_slot = requires(const Q& q) {
    { q.bytes_per_slot() } -> std::convertible_to<reg>;
};

constexpr reg kSmallCap  = 16u;
constexpr reg kMedCap    = 256u;

#if defined(NDEBUG)
constexpr int kFuzzIters = 12'000;
constexpr int kThreadIters = 60'000;
constexpr int kThreadTimeoutMs = 6000;
#else
constexpr int kFuzzIters = 2'000;
constexpr int kThreadIters = 25'000;
constexpr int kThreadTimeoutMs = 12000;
#endif

// -------------------------
// Compile-time API smoke
// -------------------------

template <class Q>
static void api_smoke_compile() {
    using value_type = typename Q::value_type;
    using size_type  = typename Q::size_type;

    static_assert(std::is_same_v<decltype(std::declval<Q&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().free()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().empty()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().full()), bool>);

    // Producer-side
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_claim()), value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim()), value_type&>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().coalescing_publish()), bool>);

    // Consumer-side
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_front()), value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().try_front()), const value_type*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().front()), value_type&>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().front()), const value_type&>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop()), void>);

    // Utilities
    static_assert(std::is_same_v<decltype(std::declval<Q&>().clear()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().swap(std::declval<Q&>())), void>);

    (void)sizeof(size_type);
}

template <class Q>
static void api_smoke_compile_raw() {
    using size_type = typename Q::size_type;

    static_assert(std::is_same_v<decltype(std::declval<Q&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().bytes_per_slot()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_claim()), void*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim()), void*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_front()), void*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().try_front()), const void*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().front()), void*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().front()), const void*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop()), void>);

    (void)sizeof(size_type);
}

// -------------------------
// Test payloads
// -------------------------

struct Blob {
    std::uint32_t seq{};
    std::uint32_t a{};
    std::uint32_t b{};
    std::uint32_t c{};

    bool operator==(const Blob& o) const noexcept {
        return seq == o.seq && a == o.a && b == o.b && c == o.c;
    }
};

template <std::size_t Align>
struct alignas(Align) OverAligned {
    std::uint32_t seq{};
    std::uint32_t x{};
    std::uint64_t y{};

    bool operator==(const OverAligned& o) const noexcept {
        return seq == o.seq && x == o.x && y == o.y;
    }
};

struct Traced {
    static inline std::atomic<int> alive{0};
    int v{0};

    Traced() noexcept { alive.fetch_add(1, std::memory_order_relaxed); }
    Traced(const Traced& o) noexcept : v(o.v) { alive.fetch_add(1, std::memory_order_relaxed); }
    Traced& operator=(const Traced& o) noexcept {
        v = o.v;
        return *this;
    }
    ~Traced() { alive.fetch_sub(1, std::memory_order_relaxed); }
};

// -------------------------
// Random helpers
// -------------------------

struct Rng {
    std::mt19937 rng;

    explicit Rng(std::uint32_t seed = 0xC0FFEEu) : rng(seed) {}

    std::uint32_t u32(std::uint32_t lo, std::uint32_t hi) {
        std::uniform_int_distribution<std::uint32_t> d(lo, hi);
        return d(rng);
    }

    bool coin() { return (u32(0u, 1u) != 0u); }
};

template <typename Ptr>
static bool is_aligned(Ptr p, std::size_t a) noexcept {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return (v % a) == 0u;
}

// -------------------------
// Minimal model for latest
// -------------------------

template <typename T>
struct LatestModel {
    std::vector<T> q; // stores all pushed values (because latest can buffer)

    void clear() { q.clear(); }

    bool empty() const { return q.empty(); }

    std::size_t size() const { return q.size(); }

    const T* front() const {
        return q.empty() ? nullptr : &q.back();
    }

    // latest.pop() consumes everything
    bool pop() {
        if (q.empty()) {
            return false;
        }
        q.clear();
        return true;
    }
};

// -------------------------
// Generic invariants
// -------------------------

template <class Q>
static void assert_invariants(const Q& q) {
    if constexpr (has_valid<Q>) {
        QVERIFY(q.valid());
    }

    const reg cap = q.capacity();
    const reg sz  = q.size();
    const reg fr  = q.free();

    QVERIFY(sz <= cap);
    QVERIFY(fr <= cap);
    QCOMPARE(static_cast<reg>(sz + fr), cap);

    QCOMPARE(q.empty(), (sz == 0u));
    QCOMPARE(q.full(),  (sz == cap));

    if (q.empty()) {
        QVERIFY(q.try_front() == nullptr);
    } else {
        QVERIFY(q.try_front() != nullptr);
    }
}

// -------------------------
// Typed latest: core tests
// -------------------------

template <class Q>
static void typed_basic_api(Q& q) {
    using T = typename Q::value_type;

    assert_invariants(q);

    // Empty behavior
    QVERIFY(q.try_claim() != nullptr || q.full());
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());

    // claim/publish/front
    {
        T& s = q.claim();
        s = T{};
        s.seq = 1u;
        q.publish();

        assert_invariants(q);
        QCOMPARE(q.size(), reg{1u});

        const T& f = q.front();
        QCOMPARE(f.seq, 1u);
    }

    // Another push without consuming: latest must show last element.
    {
        T& s = q.claim();
        s = T{};
        s.seq = 2u;
        q.publish();

        assert_invariants(q);
        QCOMPARE(q.size(), reg{2u});

        const T& f = q.front();
        QCOMPARE(f.seq, 2u);
    }

    // pop consumes everything
    QVERIFY(q.try_pop());
    assert_invariants(q);
    QVERIFY(q.empty());

    // clear is idempotent
    q.clear();
    q.clear();
    assert_invariants(q);
}

template <class Q>
static void typed_try_api(Q& q) {
    using T = typename Q::value_type;

    q.clear();
    assert_invariants(q);

    // try_claim/try_publish
    T* s = q.try_claim();
    QVERIFY(s != nullptr);
    s->seq = 10u;
    QVERIFY(q.try_publish());
    QCOMPARE(q.front().seq, 10u);

    // try_front
    const T* f = q.try_front();
    QVERIFY(f != nullptr);
    QCOMPARE(f->seq, 10u);

    // try_pop
    QVERIFY(q.try_pop());
    QVERIFY(!q.try_pop());
    QVERIFY(q.try_front() == nullptr);
    assert_invariants(q);
}

template <class Q>
static void typed_push_emplace(Q& q) {
    using T = typename Q::value_type;

    q.clear();

    // push
    T t{};
    t.seq = 123u;
    q.push(t);
    QCOMPARE(q.front().seq, 123u);

    // emplace
    q.emplace(T{.seq = 777u});
    QCOMPARE(q.front().seq, 777u);

    // try_push/try_emplace
    q.clear();
    QVERIFY(q.try_push(t));
    QCOMPARE(q.front().seq, 123u);
    QVERIFY(q.try_emplace(T{.seq = 9u}));
    QCOMPARE(q.front().seq, 9u);

    assert_invariants(q);
}

template <class Q>
static void typed_coalescing_publish(Q& q) {
    using T = typename Q::value_type;

    q.clear();
    const reg cap = q.capacity();
    QVERIFY(cap >= 4u);

    // Semantics in latest.hpp: coalescing_publish advances head only when
    // the queue has at least 3 free slots, leaving >=2 slots of slack.
    // (Condition: used <= cap - 3)

    // Fill to cap - 3: free == 3
    for (reg i = 0; i < cap - 3u; ++i) {
        T& s = q.claim();
        s.seq = static_cast<std::uint32_t>(i + 1u);
        s.a   = s.seq ^ 0x11111111u;
        s.b   = s.seq + 0x2222u;
        s.c   = ~s.seq;
        q.publish();
    }
    QCOMPARE(q.size(), static_cast<reg>(cap - 3u));

    // free == 3 -> coalescing_publish should succeed and leave free == 2
    {
        T& s = q.claim();
        s.seq = 0xAAu;
        s.a   = s.seq ^ 0x11111111u;
        s.b   = s.seq + 0x2222u;
        s.c   = ~s.seq;

        const bool ok = q.coalescing_publish();
        QVERIFY(ok);
        QCOMPARE(q.size(), static_cast<reg>(cap - 2u));
        QCOMPARE(q.front().seq, 0xAAu);
    }

    // free == 2 -> coalescing_publish should refuse (it would reduce slack below 2)
    {
        T& s = q.claim();
        s.seq = 0xBBu;
        s.a   = s.seq ^ 0x11111111u;
        s.b   = s.seq + 0x2222u;
        s.c   = ~s.seq;

        const bool ok = q.coalescing_publish();
        QVERIFY(!ok);

        // Not published: size and visible front unchanged.
        QCOMPARE(q.size(), static_cast<reg>(cap - 2u));
        QCOMPARE(q.front().seq, 0xAAu);

        // A regular publish still works.
        q.publish();
        QCOMPARE(q.size(), static_cast<reg>(cap - 1u));
        QCOMPARE(q.front().seq, 0xBBu);
    }

    // free == 1 -> coalescing_publish must also refuse
    {
        T& s = q.claim();
        s.seq = 0xCCu;
        s.a   = s.seq ^ 0x11111111u;
        s.b   = s.seq + 0x2222u;
        s.c   = ~s.seq;

        const bool ok = q.coalescing_publish();
        QVERIFY(!ok);

        q.publish();
        QCOMPARE(q.size(), cap);
        QCOMPARE(q.front().seq, 0xCCu);
        QVERIFY(q.full());
    }

    // Consume all and verify empty.
    q.pop();
    QVERIFY(q.empty());
}

template <class Q>
static void typed_fuzz(Q& q, std::uint32_t seed) {
    using T = typename Q::value_type;

    q.clear();
    LatestModel<T> m;
    Rng r(seed);

    const reg cap = q.capacity();

    for (int it = 0; it < kFuzzIters; ++it) {
        const std::uint32_t op = r.u32(0u, 9u);

        if (op <= 5u) {
            // push (blocking if full, to keep model deterministic)
            T v{};
            v.seq = static_cast<std::uint32_t>(it + 1);
            v.a   = r.u32(0u, 0xFFFFu);
            v.b   = r.u32(0u, 0xFFFFu);
            v.c   = r.u32(0u, 0xFFFFu);

            while (q.full()) {
                QVERIFY(q.try_pop());
                m.pop();
            }

            QVERIFY(q.try_push(v));
            m.q.push_back(v);
        } else if (op == 6u) {
            // pop
            const bool q_ok = q.try_pop();
            const bool m_ok = m.pop();
            QCOMPARE(q_ok, m_ok);
        } else if (op == 7u) {
            // clear
            q.clear();
            m.clear();
        } else if (op == 8u) {
            // front check
            const T* fq = q.try_front();
            const T* fm = m.front();
            if (fm == nullptr) {
                QVERIFY(fq == nullptr);
            } else {
                QVERIFY(fq != nullptr);
                QCOMPARE(fq->seq, fm->seq);
            }
        } else {
            // coalescing_publish path
            if (!q.full()) {
                T* s = q.try_claim();
                QVERIFY(s != nullptr);
                T v{};
                v.seq = static_cast<std::uint32_t>(0xF0000000u ^ static_cast<std::uint32_t>(it));
                *s = v;

                if (q.coalescing_publish()) {
                    m.q.push_back(v);
                } else {
                    // Not published -> discard the speculative slot write from model.
                }
            }
        }

        // Invariants
        assert_invariants(q);
        QVERIFY(q.size() <= cap);

        // Model vs q visibility
        const T* fq = q.try_front();
        const T* fm = m.front();
        if (fm == nullptr) {
            QVERIFY(fq == nullptr);
        } else {
            QVERIFY(fq != nullptr);
            QCOMPARE(fq->seq, fm->seq);
        }

        // Model vs size: should match exactly.
        QCOMPARE(q.size(), static_cast<reg>(m.size()));
    }
}

// -------------------------
// Raw latest: core tests
// -------------------------

template <class Q>
static void raw_basic_api(Q& q) {
    assert_invariants(q);

    QVERIFY(q.bytes_per_slot() > 0u);

    // Empty
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());

    // push/ front
    {
        Blob b{};
        b.seq = 1u;
        b.a   = 2u;
        b.b   = 3u;
        b.c   = 4u;

        q.push(b);
        const void* f = q.front();
        QVERIFY(f != nullptr);

        Blob out{};
        std::memcpy(&out, f, sizeof(out));
        QCOMPARE(out.seq, 1u);
        QCOMPARE(out.a, 2u);
        QCOMPARE(out.b, 3u);
        QCOMPARE(out.c, 4u);

        QCOMPARE(q.size(), reg{1u});
    }

    // Another push
    {
        Blob b{};
        b.seq = 7u;
        q.push(b);

        Blob out{};
        std::memcpy(&out, q.front(), sizeof(out));
        QCOMPARE(out.seq, 7u);
        QCOMPARE(q.size(), reg{2u});
    }

    // pop consumes all
    QVERIFY(q.try_pop());
    QVERIFY(q.empty());

    q.clear();
    QVERIFY(q.empty());
}

template <class Q>
static void raw_try_api(Q& q) {
    q.clear();

    QVERIFY(q.try_front() == nullptr);

    // try_claim/try_publish
    void* s = q.try_claim();
    QVERIFY(s != nullptr);
    Blob b{};
    b.seq = 42u;
    std::memcpy(s, &b, sizeof(b));
    QVERIFY(q.try_publish());

    Blob out{};
    std::memcpy(&out, q.front(), sizeof(out));
    QCOMPARE(out.seq, 42u);

    QVERIFY(q.try_pop());
    QVERIFY(!q.try_pop());
}

template <class Q>
static void raw_fuzz(Q& q, std::uint32_t seed) {
    q.clear();
    LatestModel<Blob> m;
    Rng r(seed);

    const reg cap = q.capacity();

    for (int it = 0; it < kFuzzIters; ++it) {
        const std::uint32_t op = r.u32(0u, 8u);

        if (op <= 4u) {
            // push
            Blob v{};
            v.seq = static_cast<std::uint32_t>(it + 1);
            v.a   = r.u32(0u, 0xFFFFu);
            v.b   = r.u32(0u, 0xFFFFu);
            v.c   = r.u32(0u, 0xFFFFu);

            while (q.full()) {
                QVERIFY(q.try_pop());
                m.pop();
            }

            QVERIFY(q.try_push(v));
            m.q.push_back(v);
        } else if (op == 5u) {
            const bool q_ok = q.try_pop();
            const bool m_ok = m.pop();
            QCOMPARE(q_ok, m_ok);
        } else if (op == 6u) {
            q.clear();
            m.clear();
        } else if (op == 7u) {
            const void* fq = q.try_front();
            const Blob* fm = m.front();
            if (fm == nullptr) {
                QVERIFY(fq == nullptr);
            } else {
                QVERIFY(fq != nullptr);
                Blob out{};
                std::memcpy(&out, fq, sizeof(out));
                QCOMPARE(out.seq, fm->seq);
            }
        } else {
            // claim + coalescing_publish
            if (!q.full()) {
                void* slot = q.try_claim();
                QVERIFY(slot != nullptr);
                Blob v{};
                v.seq = static_cast<std::uint32_t>(0xA0000000u ^ static_cast<std::uint32_t>(it));
                std::memcpy(slot, &v, sizeof(v));

                if (q.coalescing_publish()) {
                    m.q.push_back(v);
                }
            }
        }

        assert_invariants(q);
        QVERIFY(q.size() <= cap);

        const void* fq = q.try_front();
        const Blob* fm = m.front();
        if (fm == nullptr) {
            QVERIFY(fq == nullptr);
        } else {
            QVERIFY(fq != nullptr);
            Blob out{};
            std::memcpy(&out, fq, sizeof(out));
            QCOMPARE(out.seq, fm->seq);
        }

        QCOMPARE(q.size(), static_cast<reg>(m.size()));
    }
}

// -------------------------
// Shadow regressions (static typed)
// -------------------------

template <class Policy>
static void shadow_regression_static_swap_and_move() {
    using Q = spsc::latest<Blob, kSmallCap, Policy>;

    Q a;
    Q b;

    // Step 1: advance head/tail to 90 on a
    for (std::uint32_t i = 0; i < 90u; ++i) {
        Blob v{};
        v.seq = i + 1u;
        a.push(v);
        QVERIFY(a.try_pop());
    }

    // Force producer shadow tail in 'a' to refresh to current tail by making the queue full
    for (std::uint32_t i = 0; i < kSmallCap; ++i) {
        Blob v{};
        v.seq = 1000u + i;
        a.push(v);
    }
    QVERIFY(a.full());

    // Step 2: prepare b with tail 84, head 100 (full), so b.tail < a.prod_shadow_tail is plausible
    for (std::uint32_t i = 0; i < 84u; ++i) {
        Blob v{};
        v.seq = 2000u + i;
        b.push(v);
        QVERIFY(b.try_pop());
    }
    for (std::uint32_t i = 0; i < kSmallCap; ++i) {
        Blob v{};
        v.seq = 3000u + i;
        b.push(v);
    }
    QVERIFY(b.full());

    // Now swap. If shadows are not re-synced, a may think it is NOT full and allow an extra push.
    a.swap(b);

    if constexpr (has_valid<Q>) {
        QVERIFY(a.valid());
        QVERIFY(b.valid());
    }

    QVERIFY(a.full());

    Blob vv{};
    vv.seq = 0xDEADBEEFu;

    // Must refuse: full means no claim/publish.
    QVERIFY(!a.try_push(vv));

    // Move assignment should keep shadows consistent too.
    Q c;
    c = std::move(a);
    QVERIFY(c.full());
    QVERIFY(!c.try_push(vv));
}

// -------------------------
// Alignment + allocator checks
// -------------------------

template <class Policy>
static void alignment_typed_dynamic_default_alloc() {
    using T = OverAligned<64>;
    using Q = spsc::latest<T, 0u, Policy, spsc::alloc::default_alloc>;

    Q q;
    QVERIFY(q.init(kMedCap));

    QVERIFY(is_aligned(q.data(), 64u));

    // Spot check front() alignment too.
    q.push(T{.seq = 1u, .x = 2u, .y = 3u});
    QVERIFY(is_aligned(&q.front(), 64u));
}

template <class Policy, std::size_t Align>
static void alignment_typed_dynamic_aligned_alloc() {
    using T = OverAligned<Align>;
    using A = spsc::alloc::aligned_allocator<std::byte, Align, spsc::alloc::fail_mode::returns_null>;
    using Q = spsc::latest<T, 0u, Policy, A>;

    Q q;
    QVERIFY(q.init(kMedCap));

    QVERIFY(is_aligned(q.data(), Align));

    q.push(T{.seq = 1u, .x = 2u, .y = 3u});
    QVERIFY(is_aligned(&q.front(), Align));
}

template <class Policy, std::size_t Align>
static void alignment_raw_dynamic_aligned_alloc() {
    using A = spsc::alloc::aligned_allocator<std::byte, Align, spsc::alloc::fail_mode::returns_null>;
    using Q = spsc::latest<void, 0u, Policy, A>;

    Q q;
    QVERIFY(q.init(kMedCap, sizeof(OverAligned<Align>)));

    void* s = q.claim();
    QVERIFY(s != nullptr);
    QVERIFY(is_aligned(s, Align));

    OverAligned<Align> v{};
    v.seq = 1u;
    std::memcpy(s, &v, sizeof(v));
    q.publish();

    const void* f = q.front();
    QVERIFY(f != nullptr);
    QVERIFY(is_aligned(f, Align));
}

template <class Policy>
static void alignment_raw_dynamic_default_alloc_max_align() {
    using Q = spsc::latest<void, 0u, Policy, spsc::alloc::default_alloc>;

    Q q;
    QVERIFY(q.init(kMedCap, sizeof(Blob)));

    void* s = q.claim();
    QVERIFY(s != nullptr);

    // Fundamental guarantee: operator new provides at least max_align_t alignment.
    QVERIFY(is_aligned(s, alignof(std::max_align_t)));
}

// -------------------------
// Dynamic typed lifecycle test
// -------------------------

template <class Policy>
static void traced_lifecycle_dynamic() {
    using Q = spsc::latest<Traced, 0u, Policy>;

    const int before = Traced::alive.load(std::memory_order_relaxed);

    {
        Q q;
        QVERIFY(q.init(64u));

        const int during = Traced::alive.load(std::memory_order_relaxed);
        QVERIFY(during >= before + 64);

        q.destroy();

        const int after_destroy = Traced::alive.load(std::memory_order_relaxed);
        QCOMPARE(after_destroy, before);
    }

    const int after_scope = Traced::alive.load(std::memory_order_relaxed);
    QCOMPARE(after_scope, before);
}

// -------------------------
// Threaded SPSC test
// -------------------------

static inline void spin_pause(unsigned& spins) noexcept {
    if ((++spins & 0xFFu) == 0u) {
        std::this_thread::yield();
    }
}

template <class Q>
static void threaded_spsc_latest(Q& q) {
    using T = typename Q::value_type;

    q.clear();

    std::atomic<bool> start{false};
    std::atomic<bool> prod_done{false};
    std::atomic<bool> abort{false};
    std::atomic<int>  fail{0};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kThreadTimeoutMs);
    auto timed_out = [&]() noexcept {
        return std::chrono::steady_clock::now() > deadline;
    };

    std::thread prod([&]() {
        while (!start.load(std::memory_order_acquire)) {
            if (timed_out()) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(10, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        }

        for (std::uint32_t seq = 1u; seq <= static_cast<std::uint32_t>(kThreadIters); ++seq) {
            if (abort.load(std::memory_order_relaxed)) {
                return;
            }

            T* slot = nullptr;
            unsigned spins = 0;
            while ((slot = q.try_claim()) == nullptr) {
                if (timed_out()) {
                    abort.store(true, std::memory_order_relaxed);
                    fail.store(11, std::memory_order_relaxed);
                    return;
                }
                spin_pause(spins);
            }

            slot->seq = seq;
            slot->a   = seq ^ 0x11111111u;
            slot->b   = seq + 0x2222u;
            slot->c   = ~seq;

            q.publish();
        }

        prod_done.store(true, std::memory_order_release);
    });

    std::thread cons([&]() {
        start.store(true, std::memory_order_release);

        std::uint32_t last = 0u;
        for (;;) {
            if (abort.load(std::memory_order_relaxed)) {
                return;
            }
            if (timed_out()) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(20, std::memory_order_relaxed);
                return;
            }

            const T* f = q.try_front();
            if (f == nullptr) {
                if (prod_done.load(std::memory_order_acquire) &&
                    last >= static_cast<std::uint32_t>(kThreadIters)) {
                    return;
                }
                std::this_thread::yield();
                continue;
            }

            const std::uint32_t seq = f->seq;
            if (seq < last) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(21, std::memory_order_relaxed);
                return;
            }
            if (f->a != (seq ^ 0x11111111u) || f->b != (seq + 0x2222u) || f->c != ~seq) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(22, std::memory_order_relaxed);
                return;
            }

            last = seq;

            // latest::pop() collapses everything up to the head snapshot used by try_front().
            // This is intentional: we only require monotonicity and eventual observation of the final sequence.
            q.pop();

            if (prod_done.load(std::memory_order_acquire) &&
                last >= static_cast<std::uint32_t>(kThreadIters)) {
                return;
            }
        }
    });

    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), "threaded_spsc_latest: timed out or invariant failed");
    QCOMPARE(fail.load(std::memory_order_relaxed), 0);
    QVERIFY(q.empty());
}

// Raw threaded: store u32 counters via memcpy.

template <class Q>
static void threaded_spsc_latest_raw(Q& q) {
    q.clear();

    std::atomic<bool> start{false};
    std::atomic<bool> prod_done{false};
    std::atomic<bool> abort{false};
    std::atomic<int>  fail{0};

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kThreadTimeoutMs);
    auto timed_out = [&]() noexcept {
        return std::chrono::steady_clock::now() > deadline;
    };

    std::thread prod([&]() {
        while (!start.load(std::memory_order_acquire)) {
            if (timed_out()) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(30, std::memory_order_relaxed);
                return;
            }
            std::this_thread::yield();
        }

        for (std::uint32_t seq = 1u; seq <= static_cast<std::uint32_t>(kThreadIters); ++seq) {
            if (abort.load(std::memory_order_relaxed)) {
                return;
            }

            const std::uint32_t v = seq;
            unsigned spins = 0;
            while (!q.try_push(v)) {
                if (timed_out()) {
                    abort.store(true, std::memory_order_relaxed);
                    fail.store(31, std::memory_order_relaxed);
                    return;
                }
                spin_pause(spins);
            }
        }

        prod_done.store(true, std::memory_order_release);
    });

    std::thread cons([&]() {
        start.store(true, std::memory_order_release);

        std::uint32_t last = 0u;
        for (;;) {
            if (abort.load(std::memory_order_relaxed)) {
                return;
            }
            if (timed_out()) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(40, std::memory_order_relaxed);
                return;
            }

            const void* f = q.try_front();
            if (f == nullptr) {
                if (prod_done.load(std::memory_order_acquire) &&
                    last >= static_cast<std::uint32_t>(kThreadIters)) {
                    return;
                }
                std::this_thread::yield();
                continue;
            }

            std::uint32_t v{};
            std::memcpy(&v, f, sizeof(v));

            if (v < last) {
                abort.store(true, std::memory_order_relaxed);
                fail.store(41, std::memory_order_relaxed);
                return;
            }

            last = v;
            q.pop();

            if (prod_done.load(std::memory_order_acquire) &&
                last >= static_cast<std::uint32_t>(kThreadIters)) {
                return;
            }
        }
    });

    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), "threaded_spsc_latest_raw: timed out or invariant failed");
    QCOMPARE(fail.load(std::memory_order_relaxed), 0);
    QVERIFY(q.empty());
}

// -------------------------
// Suite runners
// -------------------------

template <class Policy>
static void run_static_typed_suite() {
    using Q = spsc::latest<Blob, kSmallCap, Policy>;

    Q q;
    if constexpr (has_valid<Q>) {
        QVERIFY(q.valid());
    }
    QCOMPARE(q.capacity(), kSmallCap);

    typed_basic_api(q);
    typed_try_api(q);
    typed_push_emplace(q);
    typed_coalescing_publish(q);
    typed_fuzz(q, 0x1111u);

    // Shadow regression only meaningful for shadow-enabled policies, but safe to run always.
    shadow_regression_static_swap_and_move<Policy>();
}

template <class Policy>
static void run_dynamic_typed_suite() {
    using Q = spsc::latest<Blob, 0u, Policy>;

    Q q;
    QVERIFY(q.init(kMedCap));
    QCOMPARE(q.capacity(), kMedCap);

    typed_basic_api(q);
    typed_try_api(q);
    typed_push_emplace(q);
    typed_coalescing_publish(q);
    typed_fuzz(q, 0x2222u);

    // move/swap basics
    {
        Q other;
        QVERIFY(other.init(128u));

        q.push(Blob{.seq = 7u});
        other.push(Blob{.seq = 9u});

        q.swap(other);
        QCOMPARE(q.front().seq, 9u);
        QCOMPARE(other.front().seq, 7u);

        Q moved = std::move(q);
        QVERIFY(!q.valid());
        QVERIFY(moved.valid());
        QCOMPARE(moved.front().seq, 9u);
    }
}

template <class Policy>
static void run_dynamic_raw_suite() {
    using Q = spsc::latest<void, 0u, Policy>;

    Q q;
    QVERIFY(q.init(kMedCap, sizeof(Blob)));

    raw_basic_api(q);
    raw_try_api(q);
    raw_fuzz(q, 0x3333u);

    // move/swap basics
    {
        Q other;
        QVERIFY(other.init(128u, sizeof(Blob)));

        Blob a{.seq = 1u};
        Blob b{.seq = 2u};
        q.push(a);
        other.push(b);

        q.swap(other);

        Blob out{};
        std::memcpy(&out, q.front(), sizeof(out));
        QCOMPARE(out.seq, 2u);

        Q moved = std::move(q);
        QVERIFY(!q.valid());
        QVERIFY(moved.valid());
        std::memcpy(&out, moved.front(), sizeof(out));
        QCOMPARE(out.seq, 2u);
    }
}



// -------------------------
// Deterministic interleavings (sticky snapshot contract)
// -------------------------

static inline bool spin_until(const std::chrono::steady_clock::time_point deadline,
                              const std::atomic<int>& state,
                              const int want_at_least) {
    unsigned spins = 0;
    while (state.load(std::memory_order_acquire) < want_at_least) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        spin_pause(spins);
    }
    return true;
}

template <class Q>
static void deterministic_snapshot_preserves_future_typed(Q& q) {
    using T = typename Q::value_type;

    q.clear();

    // State machine:
    //  0: init
    //  1: producer published seq=1
    //  2: consumer observed seq=1 (try_front)
    //  3: producer published seq=2 (after consumer observed seq=1)
    //  4: consumer popped snapshot (must NOT drop seq=2)
    //  5: consumer observed seq=2
    //  6: done

    std::atomic<int> state{0};
    std::atomic<bool> abort{false};

    T got1{};
    T got2{};
    bool ok1 = false;
    bool ok2 = false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);

    std::thread prod([&]() {
        // Publish seq=1.
        {
            T* s = nullptr;
            unsigned spins = 0;
            while ((s = q.try_claim()) == nullptr) {
                if (std::chrono::steady_clock::now() > deadline) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                spin_pause(spins);
            }
            s->seq = 1u;
            s->a   = 0xAAAA0001u;
            s->b   = 0xBBBB0001u;
            s->c   = 0xCCCC0001u;
            q.publish();
        }

        state.store(1, std::memory_order_release);

        // Wait until consumer has snapshotted head for seq=1.
        if (!spin_until(deadline, state, 2)) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        // Publish seq=2 AFTER consumer observed seq=1.
        {
            T* s = nullptr;
            unsigned spins = 0;
            while ((s = q.try_claim()) == nullptr) {
                if (std::chrono::steady_clock::now() > deadline) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                spin_pause(spins);
            }
            s->seq = 2u;
            s->a   = 0xAAAA0002u;
            s->b   = 0xBBBB0002u;
            s->c   = 0xCCCC0002u;
            q.publish();
        }

        state.store(3, std::memory_order_release);

        // Wait for completion.
        (void)spin_until(deadline, state, 6);
    });

    std::thread cons([&]() {
        // Wait for seq=1 to exist.
        if (!spin_until(deadline, state, 1)) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        const T* f1 = nullptr;
        unsigned spins = 0;
        while ((f1 = q.try_front()) == nullptr) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }
            spin_pause(spins);
        }

        got1 = *f1;
        ok1 = (got1.seq == 1u) && (got1.a == 0xAAAA0001u) && (got1.b == 0xBBBB0001u) && (got1.c == 0xCCCC0001u);

        // Signal: snapshot taken.
        state.store(2, std::memory_order_release);

        // Wait until producer publishes seq=2.
        if (!spin_until(deadline, state, 3)) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        // Pop MUST only consume up to the snapshot taken above (seq=1), not current head.
        q.pop();
        state.store(4, std::memory_order_release);

        // Now we MUST be able to see seq=2.
        const T* f2 = nullptr;
        spins = 0;
        while ((f2 = q.try_front()) == nullptr) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }
            spin_pause(spins);
        }

        got2 = *f2;
        ok2 = (got2.seq == 2u) && (got2.a == 0xAAAA0002u) && (got2.b == 0xBBBB0002u) && (got2.c == 0xCCCC0002u);

        state.store(5, std::memory_order_release);

        q.pop();
        if (!q.empty()) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        state.store(6, std::memory_order_release);
    });

    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), "deterministic_snapshot_preserves_future_typed: timed out or invariant failed");
    QVERIFY2(ok1, "deterministic_snapshot_preserves_future_typed: consumer did not observe seq=1 correctly");
    QVERIFY2(ok2, "deterministic_snapshot_preserves_future_typed: pop() dropped a future publish (seq=2)");
}


template <class Q>
static void deterministic_snapshot_preserves_future_raw(Q& q) {
    q.clear();

    std::atomic<int> state{0};
    std::atomic<bool> abort{false};

    std::uint32_t v1 = 0u;
    std::uint32_t v2 = 0u;
    bool ok1 = false;
    bool ok2 = false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);

    std::thread prod([&]() {
        // Publish 1.
        {
            unsigned spins = 0;
            const std::uint32_t v = 1u;
            while (!q.try_push(v)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                spin_pause(spins);
            }
        }
        state.store(1, std::memory_order_release);

        if (!spin_until(deadline, state, 2)) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        // Publish 2 after consumer observed 1.
        {
            unsigned spins = 0;
            const std::uint32_t v = 2u;
            while (!q.try_push(v)) {
                if (std::chrono::steady_clock::now() > deadline) {
                    abort.store(true, std::memory_order_relaxed);
                    return;
                }
                spin_pause(spins);
            }
        }
        state.store(3, std::memory_order_release);

        (void)spin_until(deadline, state, 6);
    });

    std::thread cons([&]() {
        if (!spin_until(deadline, state, 1)) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        const void* f1 = nullptr;
        unsigned spins = 0;
        while ((f1 = q.try_front()) == nullptr) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }
            spin_pause(spins);
        }

        std::memcpy(&v1, f1, sizeof(v1));
        ok1 = (v1 == 1u);

        state.store(2, std::memory_order_release);

        if (!spin_until(deadline, state, 3)) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        q.pop();
        state.store(4, std::memory_order_release);

        const void* f2 = nullptr;
        spins = 0;
        while ((f2 = q.try_front()) == nullptr) {
            if (std::chrono::steady_clock::now() > deadline) {
                abort.store(true, std::memory_order_relaxed);
                return;
            }
            spin_pause(spins);
        }

        std::memcpy(&v2, f2, sizeof(v2));
        ok2 = (v2 == 2u);

        state.store(5, std::memory_order_release);

        q.pop();
        if (!q.empty()) {
            abort.store(true, std::memory_order_relaxed);
            return;
        }

        state.store(6, std::memory_order_release);
    });

    prod.join();
    cons.join();

    QVERIFY2(!abort.load(std::memory_order_relaxed), "deterministic_snapshot_preserves_future_raw: timed out or invariant failed");
    QVERIFY2(ok1, "deterministic_snapshot_preserves_future_raw: consumer did not observe v=1 correctly");
    QVERIFY2(ok2, "deterministic_snapshot_preserves_future_raw: pop() dropped a future publish (v=2)");
}

// -------------------------
// Invalid inputs + safe no-ops
// -------------------------

template <class Q>
static void invalid_default_constructed_typed() {
    Q q;

    if constexpr (has_valid<Q>) {
        QVERIFY(!q.valid());
    }

    QCOMPARE(q.capacity(), reg{0u});
    QCOMPARE(q.free(), reg{0u});
    QCOMPARE(q.size(), reg{0u});
    QVERIFY(q.empty());
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());

    // clear() must be a safe no-op.
    q.clear();
    QVERIFY(q.empty());
}

template <class Q>
static void invalid_default_constructed_raw() {
    Q q;

    if constexpr (has_valid<Q>) {
        QVERIFY(!q.valid());
    }

    QCOMPARE(q.capacity(), reg{0u});
    QCOMPARE(q.bytes_per_slot(), reg{0u});
    QCOMPARE(q.size(), reg{0u});
    QVERIFY(q.empty());
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(q.try_front() == nullptr);
    QVERIFY(!q.try_pop());

    q.clear();
    QVERIFY(q.empty());
}

// -------------------------
// Counting allocator (leak tripwire)
// -------------------------

template <typename T>
struct CountingAllocator {
    using value_type = T;

    // latest requires stateless allocators.
    using is_always_equal = std::true_type;

    static inline std::atomic<std::size_t> alloc_calls{0};
    static inline std::atomic<std::size_t> dealloc_calls{0};
    static inline std::atomic<std::size_t> bytes_live{0};

    CountingAllocator() noexcept = default;

    template <typename U>
    CountingAllocator(const CountingAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        alloc_calls.fetch_add(1, std::memory_order_relaxed);
        bytes_live.fetch_add(n * sizeof(T), std::memory_order_relaxed);
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        dealloc_calls.fetch_add(1, std::memory_order_relaxed);
        bytes_live.fetch_sub(n * sizeof(T), std::memory_order_relaxed);
        std::allocator<T>{}.deallocate(p, n);
    }

    template <typename U>
    bool operator==(const CountingAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const CountingAllocator<U>&) const noexcept { return false; }
};

template <typename T>
static void reset_alloc_counters() {
    CountingAllocator<T>::alloc_calls.store(0, std::memory_order_relaxed);
    CountingAllocator<T>::dealloc_calls.store(0, std::memory_order_relaxed);
    CountingAllocator<T>::bytes_live.store(0, std::memory_order_relaxed);
}

template <class Policy>
static void allocator_accounting_typed_dynamic() {
    using A = CountingAllocator<Blob>;
    using Q = spsc::latest<Blob, 0u, Policy, A>;

    reset_alloc_counters<Blob>();

    {
        Q q;
        QVERIFY(q.init(128u));

        QVERIFY(A::alloc_calls.load(std::memory_order_relaxed) >= 1u);
        QVERIFY(A::bytes_live.load(std::memory_order_relaxed) >= sizeof(Blob) * 128u);

        q.push(Blob{.seq = 1u});
        q.push(Blob{.seq = 2u});
        QCOMPARE(q.front().seq, 2u);

        q.destroy();
        QVERIFY(!q.valid());
    }

    // All allocations must be paired by deallocations after scope end.
    QCOMPARE(A::bytes_live.load(std::memory_order_relaxed), std::size_t{0});
    QCOMPARE(A::alloc_calls.load(std::memory_order_relaxed), A::dealloc_calls.load(std::memory_order_relaxed));
}

template <class Policy>
static void allocator_accounting_raw_dynamic() {
    using BA = CountingAllocator<std::byte>;
    using SA = CountingAllocator<void*>;
    using Q  = spsc::latest<void, 0u, Policy, BA>;

    reset_alloc_counters<std::byte>();
    reset_alloc_counters<void*>();

    {
        Q q;
        QVERIFY(q.init(64u, sizeof(Blob)));

        // For raw: we expect slot array (void**) + per-slot buffers.
        QVERIFY(SA::alloc_calls.load(std::memory_order_relaxed) >= 1u);
        QVERIFY(BA::alloc_calls.load(std::memory_order_relaxed) >= 64u);
        QVERIFY(BA::bytes_live.load(std::memory_order_relaxed) >= sizeof(Blob) * 64u);

        Blob b{.seq = 123u};
        q.push(b);

        Blob out{};
        std::memcpy(&out, q.front(), sizeof(out));
        QCOMPARE(out.seq, 123u);

        q.destroy();
        QVERIFY(!q.valid());
    }

    // Bytes
    QCOMPARE(BA::bytes_live.load(std::memory_order_relaxed), std::size_t{0});
    QCOMPARE(BA::alloc_calls.load(std::memory_order_relaxed), BA::dealloc_calls.load(std::memory_order_relaxed));

    // Slots
    QCOMPARE(SA::bytes_live.load(std::memory_order_relaxed), std::size_t{0});
    QCOMPARE(SA::alloc_calls.load(std::memory_order_relaxed), SA::dealloc_calls.load(std::memory_order_relaxed));
}

// -------------------------
// State-machine fuzz (model-backed)
// -------------------------

enum class OpKind : std::uint8_t {
    TryClaim,
    Claim,
    Publish,
    TryPublish,
    TryFront,
    Front,
    TryPop,
    Pop,
    Clear,
};

template <class Q>
static void typed_state_machine_fuzz(Q& q, std::uint32_t seed) {
    using T = typename Q::value_type;

    Rng rng(seed);
    LatestModel<T> m;

    q.clear();
    m.clear();

    // A slightly different fuzzer than typed_fuzz():
    // it explicitly models the "claimed but not published" state.
    bool has_claimed = false;
    T* claimed_ptr = nullptr;

    for (int i = 0; i < kFuzzIters; ++i) {
        assert_invariants(q);

        const auto r = rng.u32(0u, 99u);
        OpKind op{};
        if (r < 15u) op = OpKind::TryClaim;
        else if (r < 25u) op = OpKind::Claim;
        else if (r < 40u) op = OpKind::Publish;
        else if (r < 50u) op = OpKind::TryPublish;
        else if (r < 65u) op = OpKind::TryFront;
        else if (r < 75u) op = OpKind::Front;
        else if (r < 85u) op = OpKind::TryPop;
        else if (r < 95u) op = OpKind::Pop;
        else op = OpKind::Clear;

        switch (op) {
        case OpKind::TryClaim: {
            T* p = q.try_claim();
            if (!has_claimed) {
                // We accept nullptr when full.
                if (p != nullptr) {
                    has_claimed = true;
                    claimed_ptr = p;
                    p->seq = 0u;
                }
            } else {
                // If already claimed, calling try_claim again should not create a new slot.
                // It may return nullptr or the same pointer, but must never yield a different live slot.
                if (p != nullptr) {
                    QVERIFY(p == claimed_ptr);
                }
            }
        } break;

        case OpKind::Claim: {
            if (q.full()) {
                break;
            }
            if (!has_claimed) {
                T& r = q.claim();
                has_claimed = true;
                claimed_ptr = &r;
                r.seq = 0u;
            }
        } break;

        case OpKind::Publish: {
            if (!has_claimed) {
                break;
            }
            const std::uint32_t seq = static_cast<std::uint32_t>(m.size() + 1u + rng.u32(0u, 3u));
            claimed_ptr->seq = seq;
            claimed_ptr->a   = seq ^ 0x11111111u;
            claimed_ptr->b   = seq + 0x2222u;
            claimed_ptr->c   = ~seq;
            q.publish();
            m.q.push_back(*claimed_ptr);
            has_claimed = false;
            claimed_ptr = nullptr;
        } break;

        case OpKind::TryPublish: {
            if (!has_claimed) {
                break;
            }
            const std::uint32_t seq = static_cast<std::uint32_t>(m.size() + 1u);
            claimed_ptr->seq = seq;
            claimed_ptr->a   = seq ^ 0x11111111u;
            claimed_ptr->b   = seq + 0x2222u;
            claimed_ptr->c   = ~seq;

            if (q.try_publish()) {
                m.q.push_back(*claimed_ptr);
                has_claimed = false;
                claimed_ptr = nullptr;
            }
        } break;

        case OpKind::TryFront: {
            const T* f = q.try_front();
            const T* mf = m.front();
            if (mf == nullptr) {
                QVERIFY(f == nullptr);
            } else {
                QVERIFY(f != nullptr);
                QCOMPARE(f->seq, mf->seq);
            }
        } break;

        case OpKind::Front: {
            if (q.empty()) {
                break;
            }
            const T& f = q.front();
            const T* mf = m.front();
            QVERIFY(mf != nullptr);
            QCOMPARE(f.seq, mf->seq);
        } break;

        case OpKind::TryPop: {
            const bool ok = q.try_pop();
            const bool mok = m.pop();
            QCOMPARE(ok, mok);
        } break;

        case OpKind::Pop: {
            if (q.empty()) {
                break;
            }
            q.pop();
            const bool mok = m.pop();
            QVERIFY(mok);
        } break;

        case OpKind::Clear: {
            q.clear();
            m.clear();
            has_claimed = false;
            claimed_ptr = nullptr;
        } break;

        default:
            break;
        }

        // Cross-check size/empty with the model.
        QCOMPARE(q.empty(), m.empty());
        QCOMPARE(static_cast<std::size_t>(q.size()), m.size());
    }

    // Ensure we can drain.
    q.clear();
    QVERIFY(q.empty());
}

template <class Q>
static void raw_state_machine_fuzz(Q& q, std::uint32_t seed) {
    Rng rng(seed);
    LatestModel<std::uint32_t> m;

    q.clear();
    m.clear();

    bool has_claimed = false;
    void* claimed_ptr = nullptr;

    for (int i = 0; i < kFuzzIters; ++i) {
        assert_invariants(q);

        const auto r = rng.u32(0u, 99u);
        OpKind op{};
        if (r < 15u) op = OpKind::TryClaim;
        else if (r < 25u) op = OpKind::Claim;
        else if (r < 40u) op = OpKind::Publish;
        else if (r < 50u) op = OpKind::TryPublish;
        else if (r < 65u) op = OpKind::TryFront;
        else if (r < 75u) op = OpKind::Front;
        else if (r < 85u) op = OpKind::TryPop;
        else if (r < 95u) op = OpKind::Pop;
        else op = OpKind::Clear;

        switch (op) {
        case OpKind::TryClaim: {
            void* p = q.try_claim();
            if (!has_claimed) {
                if (p != nullptr) {
                    has_claimed = true;
                    claimed_ptr = p;
                    const std::uint32_t z = 0u;
                    std::memcpy(p, &z, sizeof(z));
                }
            } else {
                if (p != nullptr) {
                    QVERIFY(p == claimed_ptr);
                }
            }
        } break;

        case OpKind::Claim: {
            if (q.full()) {
                break;
            }
            if (!has_claimed) {
                void* p = q.claim();
                QVERIFY(p != nullptr);
                has_claimed = true;
                claimed_ptr = p;
                const std::uint32_t z = 0u;
                std::memcpy(p, &z, sizeof(z));
            }
        } break;

        case OpKind::Publish: {
            if (!has_claimed) {
                break;
            }
            const std::uint32_t v = static_cast<std::uint32_t>(m.size() + 1u + rng.u32(0u, 3u));
            std::memcpy(claimed_ptr, &v, sizeof(v));
            q.publish();
            m.q.push_back(v);
            has_claimed = false;
            claimed_ptr = nullptr;
        } break;

        case OpKind::TryPublish: {
            if (!has_claimed) {
                break;
            }
            const std::uint32_t v = static_cast<std::uint32_t>(m.size() + 1u);
            std::memcpy(claimed_ptr, &v, sizeof(v));
            if (q.try_publish()) {
                m.q.push_back(v);
                has_claimed = false;
                claimed_ptr = nullptr;
            }
        } break;

        case OpKind::TryFront: {
            const void* f = q.try_front();
            const std::uint32_t* mf = m.front();
            if (mf == nullptr) {
                QVERIFY(f == nullptr);
            } else {
                QVERIFY(f != nullptr);
                std::uint32_t got{};
                std::memcpy(&got, f, sizeof(got));
                QCOMPARE(got, *mf);
            }
        } break;

        case OpKind::Front: {
            if (q.empty()) {
                break;
            }
            const void* f = q.front();
            QVERIFY(f != nullptr);
            const std::uint32_t* mf = m.front();
            QVERIFY(mf != nullptr);
            std::uint32_t got{};
            std::memcpy(&got, f, sizeof(got));
            QCOMPARE(got, *mf);
        } break;

        case OpKind::TryPop: {
            const bool ok = q.try_pop();
            const bool mok = m.pop();
            QCOMPARE(ok, mok);
        } break;

        case OpKind::Pop: {
            if (q.empty()) {
                break;
            }
            q.pop();
            const bool mok = m.pop();
            QVERIFY(mok);
        } break;

        case OpKind::Clear: {
            q.clear();
            m.clear();
            has_claimed = false;
            claimed_ptr = nullptr;
        } break;

        default:
            break;
        }

        QCOMPARE(q.empty(), m.empty());
        QCOMPARE(static_cast<std::size_t>(q.size()), m.size());
    }

    q.clear();
    QVERIFY(q.empty());
}

// -------------------------
// Move/swap stress
// -------------------------

template <class Policy>
static void move_swap_stress_typed_dynamic() {
    using Q = spsc::latest<Blob, 0u, Policy>;

    Q a;
    Q b;
    QVERIFY(a.init(128u));
    QVERIFY(b.init(128u));

    for (int i = 0; i < 5000; ++i) {
        // Keep progress: if backlog reaches capacity, acknowledge it and continue.
        // This also exercises full->empty transitions under move/swap churn.
        if (a.full()) {
            a.pop();
        }
        if (b.full()) {
            b.pop();
        }

        a.push(Blob{.seq = static_cast<std::uint32_t>(i * 2 + 1)});
        b.push(Blob{.seq = static_cast<std::uint32_t>(i * 2 + 2)});

        a.swap(b);
        QVERIFY(!a.empty());
        QVERIFY(!b.empty());

        // Move-construct and move-assign in alternating pattern.
        Q tmp = std::move(a);
        QVERIFY(!tmp.empty());
        // tmp.full() is allowed here: if the consumer acknowledges rarely, backlog can reach capacity.
        // The important contract is that the moved-to object is valid and retains its state.
        QVERIFY(tmp.valid());

        a = std::move(b);
        QVERIFY(a.valid());
        QVERIFY(!a.empty());

        b = std::move(tmp);
        QVERIFY(b.valid());
        QVERIFY(!b.empty());

        // Drain occasionally.
        if ((i & 0x7F) == 0) {
            a.clear();
            b.clear();
            QVERIFY(a.empty());
            QVERIFY(b.empty());
        }
    }
}

template <class Policy>
static void raw_bytes_per_slot_contract() {
    using Q = spsc::latest<void, 0u, Policy>;

    Q q;
    QVERIFY(q.init(32u, 4u));

    // Pushing a larger trivially-copyable type must fail.
    struct Big {
        std::uint64_t a;
        std::uint64_t b;
    };

    Big x{1u, 2u};
    QVERIFY(!q.try_push(x));

    // Pushing u32 fits.
    const std::uint32_t v = 0xAABBCCDDu;
    QVERIFY(q.try_push(v));

    std::uint32_t out{};
    std::memcpy(&out, q.front(), sizeof(out));
    QCOMPARE(out, v);
}
// -------------------------
// QtTest harness
// -------------------------

class tst_latest_api_paranoid : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Basic compile-time sanity.
        static_assert(std::is_same_v<decltype(std::declval<spsc::latest<Blob, 0u>&>().try_pop()), bool>);
        static_assert(std::is_same_v<decltype(std::declval<spsc::latest<void, 0u>&>().try_pop()), bool>);
    }

    void static_plain_P()    { run_static_typed_suite<spsc::policy::P>(); }
    void static_volatile_V() { run_static_typed_suite<spsc::policy::V>(); }
    void static_atomic_A()   { run_static_typed_suite<spsc::policy::A<>>(); }
    void static_cached_CA()  { run_static_typed_suite<spsc::policy::CA<>>(); }

    void dynamic_typed_plain_P()    { run_dynamic_typed_suite<spsc::policy::P>(); }
    void dynamic_typed_volatile_V() { run_dynamic_typed_suite<spsc::policy::V>(); }
    void dynamic_typed_atomic_A()   { run_dynamic_typed_suite<spsc::policy::A<>>(); }
    void dynamic_typed_cached_CA()  { run_dynamic_typed_suite<spsc::policy::CA<>>(); }

    void dynamic_raw_plain_P()    { run_dynamic_raw_suite<spsc::policy::P>(); }
    void dynamic_raw_volatile_V() { run_dynamic_raw_suite<spsc::policy::V>(); }
    void dynamic_raw_atomic_A()   { run_dynamic_raw_suite<spsc::policy::A<>>(); }
    void dynamic_raw_cached_CA()  { run_dynamic_raw_suite<spsc::policy::CA<>>(); }

    void threaded_atomic_A() {
        {
            spsc::latest<Blob, 0u, spsc::policy::A<>> q;
            QVERIFY(q.init(1024u));
            threaded_spsc_latest(q);
        }
        {
            spsc::latest<void, 0u, spsc::policy::A<>> q;
            QVERIFY(q.init(1024u, sizeof(std::uint32_t)));
            threaded_spsc_latest_raw(q);
        }
    }

    void threaded_cached_CA() {
        {
            spsc::latest<Blob, 0u, spsc::policy::CA<>> q;
            QVERIFY(q.init(1024u));
            threaded_spsc_latest(q);
        }
        {
            spsc::latest<void, 0u, spsc::policy::CA<>> q;
            QVERIFY(q.init(1024u, sizeof(std::uint32_t)));
            threaded_spsc_latest_raw(q);
        }
    }



    void deterministic_snapshot_interleaving() {
        {
            spsc::latest<Blob, 0u, spsc::policy::A<>> q;
            QVERIFY(q.init(8u));
            deterministic_snapshot_preserves_future_typed(q);
        }
        {
            spsc::latest<void, 0u, spsc::policy::A<>> q;
            QVERIFY(q.init(8u, sizeof(std::uint32_t)));
            deterministic_snapshot_preserves_future_raw(q);
        }
        {
            spsc::latest<Blob, 0u, spsc::policy::CA<>> q;
            QVERIFY(q.init(8u));
            deterministic_snapshot_preserves_future_typed(q);
        }
        {
            spsc::latest<void, 0u, spsc::policy::CA<>> q;
            QVERIFY(q.init(8u, sizeof(std::uint32_t)));
            deterministic_snapshot_preserves_future_raw(q);
        }
    }

    void invalid_inputs() {
        invalid_default_constructed_typed<spsc::latest<Blob, 0u, spsc::policy::P>>();
        invalid_default_constructed_typed<spsc::latest<Blob, 0u, spsc::policy::A<>>>();
        invalid_default_constructed_typed<spsc::latest<Blob, 0u, spsc::policy::CA<>>>();

        invalid_default_constructed_raw<spsc::latest<void, 0u, spsc::policy::P>>();
        invalid_default_constructed_raw<spsc::latest<void, 0u, spsc::policy::A<>>>();
        invalid_default_constructed_raw<spsc::latest<void, 0u, spsc::policy::CA<>>>();
    }

    void allocator_accounting() {
        allocator_accounting_typed_dynamic<spsc::policy::P>();
        allocator_accounting_typed_dynamic<spsc::policy::A<>>();
        allocator_accounting_typed_dynamic<spsc::policy::CA<>>();

        allocator_accounting_raw_dynamic<spsc::policy::P>();
        allocator_accounting_raw_dynamic<spsc::policy::A<>>();
        allocator_accounting_raw_dynamic<spsc::policy::CA<>>();
    }

    void state_machine_fuzz_sweep() {
        {
            spsc::latest<Blob, kSmallCap, spsc::policy::P> q;
            typed_state_machine_fuzz(q, 0xABC1u);
        }
        {
            spsc::latest<Blob, 0u, spsc::policy::A<>> q;
            QVERIFY(q.init(128u));
            typed_state_machine_fuzz(q, 0xABC2u);
        }
        {
            spsc::latest<void, 0u, spsc::policy::CA<>> q;
            QVERIFY(q.init(128u, sizeof(std::uint32_t)));
            raw_state_machine_fuzz(q, 0xABC3u);
        }
    }

    void move_swap_stress() {
        move_swap_stress_typed_dynamic<spsc::policy::P>();
        move_swap_stress_typed_dynamic<spsc::policy::A<>>();
        move_swap_stress_typed_dynamic<spsc::policy::CA<>>();
    }

    void raw_bytes_per_slot() {
        raw_bytes_per_slot_contract<spsc::policy::P>();
        raw_bytes_per_slot_contract<spsc::policy::A<>>();
        raw_bytes_per_slot_contract<spsc::policy::CA<>>();
    }
    void alignment_sweep() {
        // typed dynamic
        alignment_typed_dynamic_default_alloc<spsc::policy::P>();
        alignment_typed_dynamic_default_alloc<spsc::policy::A<>>();
        alignment_typed_dynamic_default_alloc<spsc::policy::CA<>>();

        alignment_typed_dynamic_aligned_alloc<spsc::policy::P, 64>();
        alignment_typed_dynamic_aligned_alloc<spsc::policy::A<>, 64>();
        alignment_typed_dynamic_aligned_alloc<spsc::policy::CA<>, 64>();

        alignment_typed_dynamic_aligned_alloc<spsc::policy::A<>, 128>();
        alignment_typed_dynamic_aligned_alloc<spsc::policy::CA<>, 128>();

        // raw dynamic
        alignment_raw_dynamic_default_alloc_max_align<spsc::policy::P>();
        alignment_raw_dynamic_default_alloc_max_align<spsc::policy::A<>>();

        alignment_raw_dynamic_aligned_alloc<spsc::policy::P, 32>();
        alignment_raw_dynamic_aligned_alloc<spsc::policy::A<>, 32>();
        alignment_raw_dynamic_aligned_alloc<spsc::policy::CA<>, 32>();

        alignment_raw_dynamic_aligned_alloc<spsc::policy::A<>, 64>();
        alignment_raw_dynamic_aligned_alloc<spsc::policy::CA<>, 64>();

        alignment_raw_dynamic_aligned_alloc<spsc::policy::A<>, 128>();
        alignment_raw_dynamic_aligned_alloc<spsc::policy::CA<>, 128>();
    }

    void lifecycle_traced() {
        traced_lifecycle_dynamic<spsc::policy::P>();
        traced_lifecycle_dynamic<spsc::policy::A<>>();
        traced_lifecycle_dynamic<spsc::policy::CA<>>();
    }

    void cleanupTestCase() {
        // Ensure no Traced leaks after all tests.
        QCOMPARE(Traced::alive.load(std::memory_order_relaxed), 0);
    }
};

} // namespace

int run_tst_latest_api_paranoid(int argc, char** argv) {
    tst_latest_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "latest_test.moc"
