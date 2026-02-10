// chunk_test.cpp
// Paranoid API/contract tests for spsc::chunk.

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(SPSC_ASSERT) && !defined(NDEBUG)
#  define SPSC_ASSERT(expr) do { if(!(expr)) { std::abort(); } } while(0)
#endif

#include "chunk.hpp"

namespace spsc_chunk_death_detail {

#if !defined(NDEBUG)

static constexpr int kDeathExitCode = 0xB2;

static void sigabrt_handler_(int) noexcept {
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode) {
    std::signal(SIGABRT, &sigabrt_handler_);

    using QStatic = spsc::chunk<std::uint32_t, 4u>;
    using QDyn    = spsc::chunk<std::uint32_t, 0u>;

    if (std::strcmp(mode, "static_front_empty") == 0) {
        QStatic q;
        (void)q.front();
    } else if (std::strcmp(mode, "static_back_empty") == 0) {
        QStatic q;
        (void)q.back();
    } else if (std::strcmp(mode, "static_pop_back_empty") == 0) {
        QStatic q;
        q.pop_back();
    } else if (std::strcmp(mode, "static_push_full") == 0) {
        QStatic q;
        for (std::uint32_t i = 0; i < 4u; ++i) {
            const bool ok = q.try_push(i);
            if (!ok) {
                std::_Exit(0xE1);
            }
        }
        q.push(5u);
    } else if (std::strcmp(mode, "static_pop_back_n_too_many") == 0) {
        QStatic q;
        if (!q.try_push(1u)) {
            std::_Exit(0xE2);
        }
        q.pop_back_n(2u);
    } else if (std::strcmp(mode, "static_commit_size_over_cap") == 0) {
        QStatic q;
        q.commit_size(5u);
    } else if (std::strcmp(mode, "dynamic_front_empty") == 0) {
        QDyn q;
        (void)q.front();
    } else if (std::strcmp(mode, "dynamic_pop_back_empty") == 0) {
        QDyn q;
        q.pop_back();
    } else if (std::strcmp(mode, "dynamic_push_without_reserve") == 0) {
        QDyn q;
        q.push(1u);
    } else if (std::strcmp(mode, "dynamic_commit_size_over_cap") == 0) {
        QDyn q;
        if (!q.reserve(4u)) {
            std::_Exit(0xE3);
        }
        q.commit_size(5u);
    } else {
        std::_Exit(0xEF);
    }

    std::_Exit(0xF0);
}

struct Runner_ {
    Runner_() {
        const char* mode = std::getenv("SPSC_CHUNK_DEATH");
        if (mode && *mode) {
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

#endif // !defined(NDEBUG)

} // namespace spsc_chunk_death_detail

namespace {

constexpr reg kStaticCap = 16u;

struct Blob {
    std::uint32_t seq{};
    std::uint32_t tag{};

    bool operator==(const Blob& o) const noexcept {
        return seq == o.seq && tag == o.tag;
    }
};

template <std::size_t Align>
struct alignas(Align) OverAligned {
    std::uint64_t a{};
    std::uint32_t b{};
};

template <typename Ptr>
static bool is_aligned(Ptr p, std::size_t a) noexcept {
    const auto v = reinterpret_cast<std::uintptr_t>(p);
    return (v % a) == 0u;
}

struct Rng {
    std::mt19937 gen;
    explicit Rng(std::uint32_t seed) : gen(seed) {}

    std::uint32_t u32(std::uint32_t lo, std::uint32_t hi) {
        std::uniform_int_distribution<std::uint32_t> d(lo, hi);
        return d(gen);
    }
};

static Blob make_blob(const std::uint32_t seq, Rng& rng) {
    Blob b{};
    b.seq = seq;
    b.tag = rng.u32(0u, 0x00FFFFFFu) ^ (seq * 0x9E3779B9u);
    return b;
}

template <class Q>
static void assert_invariants(const Q& q) {
    const reg cap = q.capacity();
    const reg sz  = q.size();
    const reg fr  = q.free();

    QVERIFY2(sz <= cap, "size() must not exceed capacity()");
    QVERIFY2(fr <= cap, "free() must not exceed capacity()");
    QCOMPARE(static_cast<reg>(sz + fr), cap);
    QCOMPARE(q.empty(), sz == 0u);
    QCOMPARE(q.full(), sz == cap);

    if (sz == 0u) {
        QVERIFY(q.try_front() == nullptr);
        QVERIFY(q.try_back() == nullptr);
    } else {
        QVERIFY(q.try_front() != nullptr);
        QVERIFY(q.try_back() != nullptr);
    }
}

template <class Q>
static void compare_model_prefix(const Q& q, const std::vector<Blob>& shadow, const reg logical_size) {
    QCOMPARE(q.size(), logical_size);
    QVERIFY(static_cast<std::size_t>(logical_size) <= shadow.size());
    for (reg i = 0; i < logical_size; ++i) {
        QCOMPARE(q[i].seq, shadow[static_cast<std::size_t>(i)].seq);
        QCOMPARE(q[i].tag, shadow[static_cast<std::size_t>(i)].tag);
    }
}

static void api_smoke_compile() {
    using QS = spsc::chunk<Blob, 8u>;
    using QD = spsc::chunk<Blob, 0u>;

    static_assert(std::is_copy_constructible_v<QS>);
    static_assert(std::is_copy_assignable_v<QS>);
    static_assert(std::is_move_constructible_v<QS>);
    static_assert(std::is_move_assignable_v<QS>);

    static_assert(!std::is_copy_constructible_v<QD>);
    static_assert(!std::is_copy_assignable_v<QD>);
    static_assert(std::is_move_constructible_v<QD>);
    static_assert(std::is_move_assignable_v<QD>);

    static_assert(std::is_same_v<decltype(std::declval<QS&>().size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<QS&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<QS&>().resize(reg{})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<QS&>().try_resize(reg{})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<QS&>().try_push(std::declval<Blob>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<QS&>().try_emplace(std::declval<Blob>())), Blob*>);

    static_assert(std::is_same_v<decltype(std::declval<QD&>().size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<QD&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<QD&>().reserve(reg{})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<QD&>().resize(reg{})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<QD&>().try_push(std::declval<Blob>())), bool>);
}

static void static_contract_suite() {
    using Q = spsc::chunk<Blob, kStaticCap>;

    Q q;
    assert_invariants(q);
    QCOMPARE(q.capacity(), kStaticCap);
    QVERIFY(q.empty());
    QVERIFY(!q.full());
    QCOMPARE(q.free(), kStaticCap);

    QVERIFY(q.try_front() == nullptr);
    QVERIFY(q.try_back() == nullptr);
    QVERIFY(!q.try_pop_back());

    q.push(Blob{.seq = 1u, .tag = 101u});
    q.emplace(Blob{.seq = 2u, .tag = 202u});
    Blob* p3 = q.try_emplace(Blob{.seq = 3u, .tag = 303u});
    QVERIFY(p3 != nullptr);
    QCOMPARE(p3->seq, 3u);

    assert_invariants(q);
    QCOMPARE(q.size(), reg{3u});
    QCOMPARE(q.front().seq, 1u);
    QCOMPARE(q.back().seq, 3u);

    // operator[] is allowed up to capacity(), not only size().
    q[7] = Blob{.seq = 77u, .tag = 707u};
    QCOMPARE(q.size(), reg{3u});

    {
        std::vector<std::uint32_t> seqs;
        for (const auto& v : q) { seqs.push_back(v.seq); }
        QCOMPARE(seqs.size(), std::size_t{3u});
        QCOMPARE(seqs[0], 1u);
        QCOMPARE(seqs[1], 2u);
        QCOMPARE(seqs[2], 3u);
    }
    {
        std::vector<std::uint32_t> rev;
        for (auto it = q.rbegin(); it != q.rend(); ++it) { rev.push_back(it->seq); }
        QCOMPARE(rev.size(), std::size_t{3u});
        QCOMPARE(rev[0], 3u);
        QCOMPARE(rev[1], 2u);
        QCOMPARE(rev[2], 1u);
    }

    q.pop_back();
    QCOMPARE(q.size(), reg{2u});
    QCOMPARE(q.back().seq, 2u);

    q.pop_back_n(2u);
    QVERIFY(q.empty());
    QVERIFY(!q.try_pop_back());

    QVERIFY(q.try_resize(5u));
    QCOMPARE(q.size(), reg{5u});
    QVERIFY(!q.try_resize(kStaticCap + 1u));
    QCOMPARE(q.size(), reg{5u});

    q.commit_size(3u);
    QCOMPARE(q.size(), reg{3u});

    QVERIFY(q.resize(999u)); // clamp to capacity
    QCOMPARE(q.size(), kStaticCap);
    QVERIFY(q.full());
    QVERIFY(!q.try_push(Blob{.seq = 999u, .tag = 999u}));

    q.clear();
    assert_invariants(q);

    Q a;
    Q b;
    a.push(Blob{.seq = 10u, .tag = 110u});
    a.push(Blob{.seq = 20u, .tag = 220u});
    b.push(Blob{.seq = 90u, .tag = 990u});

    Q c = a; // copy
    QCOMPARE(c.size(), reg{2u});
    QCOMPARE(c.front().seq, 10u);
    QCOMPARE(c.back().seq, 20u);

    Q moved = std::move(c);
    QCOMPARE(moved.size(), reg{2u});
    QCOMPARE(moved.front().seq, 10u);

    a.swap(b);
    QCOMPARE(a.size(), reg{1u});
    QCOMPARE(a.front().seq, 90u);
    QCOMPARE(b.size(), reg{2u});
    QCOMPARE(b.front().seq, 10u);

    Q assigned;
    assigned = std::move(moved);
    QCOMPARE(assigned.size(), reg{2u});
    QCOMPARE(assigned.front().seq, 10u);
}

template <class Alloc>
static void dynamic_contract_suite() {
    using Q = spsc::chunk<Blob, 0u, Alloc>;

    Q q;
    assert_invariants(q);
    QCOMPARE(q.capacity(), reg{0u});
    QCOMPARE(q.size(), reg{0u});
    QVERIFY(q.full()); // cap=0 => full by definition
    QVERIFY(!q.try_push(Blob{.seq = 1u, .tag = 11u}));
    QVERIFY(q.try_emplace(Blob{.seq = 1u, .tag = 11u}) == nullptr);

    QVERIFY(q.reserve(6u));
    QVERIFY(q.capacity() >= 6u);
    assert_invariants(q);

    QVERIFY(q.try_push(Blob{.seq = 1u, .tag = 11u}));
    q.push(Blob{.seq = 2u, .tag = 22u});
    q.emplace(Blob{.seq = 3u, .tag = 33u});

    QCOMPARE(q.size(), reg{3u});
    QCOMPARE(q.front().seq, 1u);
    QCOMPARE(q.back().seq, 3u);

    QVERIFY(q.resize(12u));
    QVERIFY(q.capacity() >= 12u);
    QCOMPARE(q.size(), reg{12u});
    QCOMPARE(q[0].seq, 1u);
    q[11] = Blob{.seq = 99u, .tag = 999u};
    QCOMPARE(q.back().seq, 99u);

    q.commit_size(4u);
    QCOMPARE(q.size(), reg{4u});
    q.pop_back_n(2u);
    QCOMPARE(q.size(), reg{2u});
    QCOMPARE(q.back().seq, 2u);

    q.commit_size(q.capacity());
    QVERIFY(q.full());
    QVERIFY(!q.try_push(Blob{.seq = 123u, .tag = 123u}));

    q.clear();
    assert_invariants(q);

    Q a;
    Q b;
    QVERIFY(a.reserve(8u));
    QVERIFY(b.reserve(4u));
    a.push(Blob{.seq = 7u, .tag = 70u});
    a.push(Blob{.seq = 8u, .tag = 80u});
    b.push(Blob{.seq = 1u, .tag = 10u});

    a.swap(b);
    QCOMPARE(a.size(), reg{1u});
    QCOMPARE(a.front().seq, 1u);
    QCOMPARE(b.size(), reg{2u});
    QCOMPARE(b.front().seq, 7u);

    Q moved = std::move(a);
    QCOMPARE(moved.size(), reg{1u});
    QCOMPARE(moved.front().seq, 1u);
    QCOMPARE(a.capacity(), reg{0u});
    QCOMPARE(a.size(), reg{0u});
    QVERIFY(a.full());

    Q assigned;
    QVERIFY(assigned.reserve(2u));
    assigned = std::move(b);
    QCOMPARE(assigned.size(), reg{2u});
    QCOMPARE(assigned.front().seq, 7u);
    QCOMPARE(b.capacity(), reg{0u});
    QCOMPARE(b.size(), reg{0u});
}

static void dynamic_resize_overflow_guard_suite() {
    using Q = spsc::chunk<Blob, 0u>;

    Q q;
    QVERIFY(q.reserve(8u));
    QVERIFY(q.try_push(Blob{.seq = 1u, .tag = 10u}));
    QVERIFY(q.try_push(Blob{.seq = 2u, .tag = 20u}));
    const reg old_cap  = q.capacity();
    const reg old_size = q.size();

    bool ok = false;
    const reg huge = std::numeric_limits<reg>::max();
    SPSC_TRY {
        ok = q.resize(huge);
    } SPSC_CATCH_ALL {
        ok = false;
    }

    if (ok) {
        QVERIFY2(q.capacity() >= huge, "If resize succeeds, capacity must satisfy requested size.");
        QCOMPARE(q.size(), huge);
    } else {
        QCOMPARE(q.capacity(), old_cap);
        QCOMPARE(q.size(), old_size);
        QCOMPARE(q.front().seq, 1u);
        QCOMPARE(q.back().seq, 2u);
    }
}

template <class Q>
static void fuzz_suite_static(Q& q, const std::uint32_t seed) {
    Rng rng(seed);
    std::vector<Blob> shadow(static_cast<std::size_t>(q.capacity()));
    reg logical_size = 0u;
    std::uint32_t seq = 1u;

    q.clear();
    logical_size = 0u;

    for (int i = 0; i < 4000; ++i) {
        const auto op = rng.u32(0u, 7u);

        if (op <= 1u) {
            Blob b = make_blob(seq++, rng);
            if (logical_size < q.capacity()) {
                QVERIFY(q.try_push(b));
                shadow[static_cast<std::size_t>(logical_size)] = b;
                ++logical_size;
            } else {
                QVERIFY(!q.try_push(b));
            }
        } else if (op == 2u) {
            const bool ok = q.try_pop_back();
            const bool mk = (logical_size != 0u);
            if (mk) { --logical_size; }
            QCOMPARE(ok, mk);
        } else if (op == 3u) {
            const reg n = static_cast<reg>(rng.u32(0u, static_cast<std::uint32_t>(q.capacity() * 2u)));
            QVERIFY(q.resize(n));
            logical_size = std::min<reg>(n, q.capacity());
        } else if (op == 4u) {
            const reg n = static_cast<reg>(rng.u32(0u, static_cast<std::uint32_t>(q.capacity() + 2u)));
            const bool ok = q.try_resize(n);
            if (n <= q.capacity()) {
                QVERIFY(ok);
                logical_size = n;
            } else {
                QVERIFY(!ok);
            }
        } else if (op == 5u) {
            q.clear();
            logical_size = 0u;
        } else if (op == 6u) {
            const reg n = static_cast<reg>(rng.u32(0u, static_cast<std::uint32_t>(q.capacity())));
            q.commit_size(n);
            logical_size = n;
        } else {
            if (q.capacity() != 0u) {
                const reg idx = static_cast<reg>(rng.u32(0u, static_cast<std::uint32_t>(q.capacity() - 1u)));
                const Blob b = make_blob(seq++, rng);
                q[idx] = b;
                shadow[static_cast<std::size_t>(idx)] = b;
            }
        }

        assert_invariants(q);
        compare_model_prefix(q, shadow, logical_size);
    }
}

template <class Q>
static void fuzz_suite_dynamic(Q& q, const std::uint32_t seed) {
    Rng rng(seed);
    std::vector<Blob> shadow;
    reg logical_size = 0u;
    std::uint32_t seq = 1u;

    auto sync_shadow_with_realloc = [&](const reg old_cap, const reg old_logical) {
        const reg new_cap = q.capacity();
        if (new_cap > old_cap) {
            std::vector<Blob> grown(static_cast<std::size_t>(new_cap));
            const reg copy_n = std::min<reg>(old_logical, old_cap);
            for (reg i = 0; i < copy_n; ++i) {
                grown[static_cast<std::size_t>(i)] = shadow[static_cast<std::size_t>(i)];
            }
            shadow.swap(grown);
        } else if (new_cap < old_cap) {
            shadow.resize(static_cast<std::size_t>(new_cap));
        } else if (shadow.size() != static_cast<std::size_t>(new_cap)) {
            shadow.resize(static_cast<std::size_t>(new_cap));
        }
        if (logical_size > new_cap) {
            logical_size = new_cap;
        }
    };

    shadow.resize(static_cast<std::size_t>(q.capacity()));

    for (int i = 0; i < 5000; ++i) {
        const auto op = rng.u32(0u, 8u);

        if (op == 0u) {
            const reg req = static_cast<reg>(rng.u32(0u, 96u));
            const reg old_cap = q.capacity();
            const reg old_logical = logical_size;
            bool ok = false;
            SPSC_TRY { ok = q.reserve(req); } SPSC_CATCH_ALL { ok = false; }
            if (ok) {
                sync_shadow_with_realloc(old_cap, old_logical);
            }
        } else if (op == 1u) {
            const reg n = static_cast<reg>(rng.u32(0u, 96u));
            const reg old_cap = q.capacity();
            const reg old_logical = logical_size;
            bool ok = false;
            SPSC_TRY { ok = q.resize(n); } SPSC_CATCH_ALL { ok = false; }
            if (ok) {
                sync_shadow_with_realloc(old_cap, old_logical);
                logical_size = q.size();
            }
        } else if (op == 2u) {
            const Blob b = make_blob(seq++, rng);
            const bool ok = q.try_push(b);
            if (ok) {
                QVERIFY(logical_size < q.capacity());
                QVERIFY(static_cast<std::size_t>(logical_size) < shadow.size());
                shadow[static_cast<std::size_t>(logical_size)] = b;
                ++logical_size;
            } else {
                QCOMPARE(logical_size, q.capacity());
            }
        } else if (op == 3u) {
            const bool ok = q.try_pop_back();
            const bool mk = (logical_size != 0u);
            if (mk) { --logical_size; }
            QCOMPARE(ok, mk);
        } else if (op == 4u) {
            if (logical_size != 0u) {
                const reg max_n = q.size();
                const reg n = static_cast<reg>(rng.u32(1u, static_cast<std::uint32_t>(max_n)));
                q.pop_back_n(n);
                logical_size = static_cast<reg>(logical_size - n);
            } else {
                QVERIFY(!q.try_pop_back());
            }
        } else if (op == 5u) {
            q.clear();
            logical_size = 0u;
        } else if (op == 6u) {
            const reg cap = q.capacity();
            if (cap != 0u) {
                const reg n = static_cast<reg>(rng.u32(0u, static_cast<std::uint32_t>(cap)));
                q.commit_size(n);
                logical_size = n;
            } else {
                q.commit_size(0u);
                logical_size = 0u;
            }
        } else if (op == 7u) {
            const reg cap = q.capacity();
            if (cap != 0u) {
                const reg idx = static_cast<reg>(rng.u32(0u, static_cast<std::uint32_t>(cap - 1u)));
                const Blob b = make_blob(seq++, rng);
                q[idx] = b;
                shadow[static_cast<std::size_t>(idx)] = b;
            }
        } else {
            if (logical_size != 0u) {
                QCOMPARE(q.front().seq, shadow[0].seq);
                QCOMPARE(q.back().seq, shadow[static_cast<std::size_t>(logical_size - 1u)].seq);
            } else {
                QVERIFY(q.try_front() == nullptr);
            }
        }

        QCOMPARE(static_cast<reg>(shadow.size()), q.capacity());
        assert_invariants(q);
        compare_model_prefix(q, shadow, logical_size);
    }
}

struct CountingAllocatorState {
    static inline std::atomic<std::size_t> alloc_calls{0};
    static inline std::atomic<std::size_t> dealloc_calls{0};
    static inline std::atomic<std::size_t> bytes_live{0};
};

template <typename T>
struct CountingAllocator {
    using value_type = T;
    using is_always_equal = std::true_type;

    template <typename U>
    struct rebind { using other = CountingAllocator<U>; };

    CountingAllocator() noexcept = default;

    template <typename U>
    CountingAllocator(const CountingAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        CountingAllocatorState::alloc_calls.fetch_add(1, std::memory_order_relaxed);
        CountingAllocatorState::bytes_live.fetch_add(n * sizeof(T), std::memory_order_relaxed);
        return std::allocator<T>{}.allocate(n);
    }

    void deallocate(T* p, std::size_t n) noexcept {
        CountingAllocatorState::dealloc_calls.fetch_add(1, std::memory_order_relaxed);
        CountingAllocatorState::bytes_live.fetch_sub(n * sizeof(T), std::memory_order_relaxed);
        std::allocator<T>{}.deallocate(p, n);
    }

    template <typename U>
    bool operator==(const CountingAllocator<U>&) const noexcept { return true; }
    template <typename U>
    bool operator!=(const CountingAllocator<U>&) const noexcept { return false; }
};

static void reset_alloc_counters() {
    CountingAllocatorState::alloc_calls.store(0, std::memory_order_relaxed);
    CountingAllocatorState::dealloc_calls.store(0, std::memory_order_relaxed);
    CountingAllocatorState::bytes_live.store(0, std::memory_order_relaxed);
}

static void allocator_accounting_dynamic_suite() {
    using Q = spsc::chunk<Blob, 0u, CountingAllocator<std::byte>>;

    reset_alloc_counters();

    {
        Q q;
        QVERIFY(q.reserve(16u));
        QVERIFY(CountingAllocatorState::alloc_calls.load(std::memory_order_relaxed) >= 1u);
        QVERIFY(CountingAllocatorState::bytes_live.load(std::memory_order_relaxed) >= sizeof(Blob) * 16u);

        QVERIFY(q.try_push(Blob{.seq = 1u, .tag = 11u}));
        QVERIFY(q.try_push(Blob{.seq = 2u, .tag = 22u}));
        QCOMPARE(q.front().seq, 1u);
        QCOMPARE(q.back().seq, 2u);

        QVERIFY(q.resize(64u));
        QVERIFY(q.capacity() >= 64u);

        Q moved = std::move(q);
        QCOMPARE(q.capacity(), reg{0u});
        QCOMPARE(moved.size(), reg{64u});
        moved.clear();
    }

    QCOMPARE(CountingAllocatorState::bytes_live.load(std::memory_order_relaxed), std::size_t{0});
    QCOMPARE(CountingAllocatorState::alloc_calls.load(std::memory_order_relaxed),
             CountingAllocatorState::dealloc_calls.load(std::memory_order_relaxed));
}

static void alignment_sweep_suite() {
    {
        using Q = spsc::chunk<OverAligned<64>, 8u>;
        Q q;
        QVERIFY(is_aligned(q.data(), alignof(OverAligned<64>)));
        q.push(OverAligned<64>{});
        QVERIFY(is_aligned(q.try_front(), alignof(OverAligned<64>)));
    }

    {
        using Q = spsc::chunk<OverAligned<128>, 0u>;
        Q q;
        QVERIFY(q.reserve(8u));
        QVERIFY(is_aligned(q.data(), alignof(OverAligned<128>)));
        q.push(OverAligned<128>{});
        QVERIFY(is_aligned(q.try_front(), alignof(OverAligned<128>)));
    }

    {
        using Q = spsc::chunk<OverAligned<64>, 0u, spsc::alloc::align_alloc<64>>;
        Q q;
        QVERIFY(q.reserve(8u));
        QVERIFY(is_aligned(q.data(), alignof(OverAligned<64>)));
    }
}

#if SPSC_HAS_SPAN
static void span_contract_suite() {
    {
        spsc::chunk<Blob, 8u> q;
        q.push(Blob{.seq = 1u, .tag = 1u});
        q.push(Blob{.seq = 2u, .tag = 2u});
        auto used = q.used_span();
        auto cap  = q.cap_span();
        QCOMPARE(used.size(), std::size_t{2u});
        QCOMPARE(cap.size(), std::size_t{8u});
        QCOMPARE(used[0].seq, 1u);
        QCOMPARE(used[1].seq, 2u);
    }
    {
        spsc::chunk<Blob, 0u> q;
        QVERIFY(q.reserve(12u));
        q.push(Blob{.seq = 3u, .tag = 3u});
        auto used = q.used_span();
        auto cap  = q.cap_span();
        QCOMPARE(used.size(), std::size_t{1u});
        QVERIFY(cap.size() >= std::size_t{12u});
        QCOMPARE(used[0].seq, 3u);
    }
}
#endif

class tst_chunk_api_paranoid final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        api_smoke_compile();
    }

    void static_contract() {
        static_contract_suite();
    }

    void dynamic_contract() {
        dynamic_contract_suite<spsc::alloc::default_alloc>();
        dynamic_contract_suite<spsc::alloc::align_alloc<64>>();
    }

    void dynamic_resize_overflow_guard() {
        dynamic_resize_overflow_guard_suite();
    }

    void static_fuzz() {
        spsc::chunk<Blob, kStaticCap> q;
        fuzz_suite_static(q, 0xC001u);
    }

    void dynamic_fuzz() {
        spsc::chunk<Blob, 0u> q;
        fuzz_suite_dynamic(q, 0xD002u);
    }

    void allocator_accounting() {
        allocator_accounting_dynamic_suite();
    }

    void alignment_sweep() {
        alignment_sweep_suite();
    }

#if SPSC_HAS_SPAN
    void span_contract() {
        span_contract_suite();
    }
#endif

    void death_tests_debug_only() {
#if !defined(NDEBUG)
        auto expect_death = [&](const char* mode) {
            QProcess p;
            p.setProgram(QCoreApplication::applicationFilePath());
            p.setArguments(QStringList{});

            QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
            env.insert("SPSC_CHUNK_DEATH", QString::fromLatin1(mode));
            p.setProcessEnvironment(env);

            p.start();
            QVERIFY2(p.waitForStarted(1500), "Death child failed to start.");

            if (!p.waitForFinished(8000)) {
                p.kill();
                QVERIFY2(false, "Death child did not finish (possible crash dialog).");
            }

            const int code = p.exitCode();
            QVERIFY2(code == spsc_chunk_death_detail::kDeathExitCode,
                     "Expected assertion death (SIGABRT -> kDeathExitCode).");
        };

        expect_death("static_front_empty");
        expect_death("static_back_empty");
        expect_death("static_pop_back_empty");
        expect_death("static_push_full");
        expect_death("static_pop_back_n_too_many");
        expect_death("static_commit_size_over_cap");
        expect_death("dynamic_front_empty");
        expect_death("dynamic_pop_back_empty");
        expect_death("dynamic_push_without_reserve");
        expect_death("dynamic_commit_size_over_cap");
#else
        QSKIP("Death tests are debug-only (assertions disabled).");
#endif
    }
};

} // namespace

int run_tst_chunk_api_paranoid(int argc, char** argv) {
    tst_chunk_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "chunk_test.moc"
