/*
 * pool_test.cpp
 *
 * Paranoid API/contract test for spsc::pool.
 *
 * Goals:
 *  - Exercise every public method in pool.hpp at least once.
 *  - Cover policies (P/V/A/CA) across static and dynamic variants.
 *  - Validate invariants aggressively after each operation.
 *  - Stress bulk regions, including wrap-around (split first+second).
 *  - Probe alignment behavior for aligned and deliberately misaligned allocators.
 *  - Include regression checks for shadow-cache correctness (swap/move/resize paths).
 */

#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QProcess>
#include <QProcessEnvironment>

#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include <algorithm> // std::min

#if !defined(SPSC_ASSERT) && !defined(NDEBUG)
#  define SPSC_ASSERT(expr) do { if(!(expr)) { std::abort(); } } while(0)
#endif

#include "pool.hpp"

namespace spsc_pool_death_detail {

#if !defined(NDEBUG)

static constexpr int kDeathExitCode = 0xB0;

static void sigabrt_handler_(int) noexcept {
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode) {
    // Convert "assert -> abort" into a deterministic exit code so QProcess
    // won't hang on Windows crash dialogs.
    std::signal(SIGABRT, &sigabrt_handler_);

    using Q = spsc::pool<16u, spsc::policy::P>;

    auto require_valid = [](Q& q) {
        if (!q.is_valid()) {
            std::_Exit(0xE1);
        }
    };

    if (std::strcmp(mode, "pop_empty") == 0) {
        Q q(reg{64u});
        require_valid(q);
        q.pop(); // Must assert: pop on empty.
    } else if (std::strcmp(mode, "front_empty") == 0) {
        Q q(reg{64u});
        require_valid(q);
        (void)q.front(); // Must assert: front on empty.
    } else if (std::strcmp(mode, "publish_full") == 0) {
        Q q(reg{64u});
        require_valid(q);
        for (std::uint32_t i = 0; i < 16u; ++i) {
            if (!q.try_push(i)) {
                std::_Exit(0xE2);
            }
        }
        q.publish(); // Must assert: publish while full.
    } else if (std::strcmp(mode, "claim_full") == 0) {
        Q q(reg{64u});
        require_valid(q);
        for (std::uint32_t i = 0; i < 16u; ++i) {
            if (!q.try_push(i)) {
                std::_Exit(0xE3);
            }
        }
        (void)q.claim(); // Must assert: claim while full.
    } else if (std::strcmp(mode, "bulk_arm_publish_unwritten") == 0) {
        Q q(reg{64u});
        require_valid(q);
        auto g = q.scoped_write(2u);
        g.arm_publish(); // Must assert: no written elements.
    } else if (std::strcmp(mode, "write_guard_arm_without_slot") == 0) {
        Q q(reg{64u});
        require_valid(q);
        while (!q.full()) {
            const bool ok = q.try_push(std::uint32_t{1u});
            if (!ok) {
                std::_Exit(0xE4);
            }
        }
        auto g = q.scoped_write(); // falsey guard
        g.arm_publish(); // Must assert: invalid guard.
    } else if (std::strcmp(mode, "bulk_get_next_without_claim") == 0) {
        Q q(reg{64u});
        require_valid(q);
        auto g = q.scoped_write(0u); // falsey guard
        (void)g.get_next(); // Must assert: invalid guard.
    } else if (std::strcmp(mode, "bulk_write_next_null_nonzero") == 0) {
        Q q(reg{64u});
        require_valid(q);
        auto g = q.scoped_write(1u);
        (void)g.write_next(nullptr, 1u); // Must assert: non-zero copy from nullptr.
    } else if (std::strcmp(mode, "consume_foreign_snapshot") == 0) {
        Q q1(reg{64u});
        Q q2(reg{64u});
        require_valid(q1);
        require_valid(q2);
        if (!q1.try_push(std::uint32_t{1u})) {
            std::_Exit(0xE5);
        }
        if (!q2.try_push(std::uint32_t{2u})) {
            std::_Exit(0xE6);
        }
        const auto snap = q2.make_snapshot();
        q1.consume(snap); // Must assert: foreign snapshot.
    } else if (std::strcmp(mode, "pop_n_too_many") == 0) {
        Q q(reg{64u});
        require_valid(q);
        if (!q.try_push(std::uint32_t{1u})) {
            std::_Exit(0xE7);
        }
        q.pop(2u); // Must assert: can_read(2) is false.
    } else if (std::strcmp(mode, "publish_n_too_many") == 0) {
        Q q(reg{64u});
        require_valid(q);
        q.publish(static_cast<reg>(q.capacity() + 1u)); // Must assert: can_write(n) is false.
    } else {
        std::_Exit(0xEF);
    }

    // If we reach this point, assertions did not fire.
    std::_Exit(0xF0);
}

struct Runner_ {
    Runner_() {
        const char* mode = std::getenv("SPSC_POOL_DEATH");
        if (mode && *mode) {
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

#endif // !defined(NDEBUG)

} // namespace spsc_pool_death_detail

namespace {

#if defined(NDEBUG)
static constexpr int kFuzzIters = 45000;
#else
static constexpr int kFuzzIters = 200;
#endif

#if defined(NDEBUG)
static constexpr reg kThreadIters = 250000u;
static constexpr int kThreadTimeoutMs = 6000;
#else
static constexpr reg kThreadIters = 40000u;
static constexpr int kThreadTimeoutMs = 8000;
#endif

static constexpr reg kDepth = 16u;
static constexpr reg kBufSz = 64u;

static inline bool is_pow2(reg x) noexcept {
    return x != 0u && ((x & (x - 1u)) == 0u);
}

// Compile-time Capacity extractor for spsc::pool.
template<class Q>
struct pool_capacity;

template<reg C, class P, class A>
struct pool_capacity<::spsc::pool<C, P, A>> : std::integral_constant<reg, C> {};

template<class Q>
inline constexpr reg pool_capacity_v = pool_capacity<std::remove_cv_t<Q>>::value;

// -------------------------
// Compile-time API smoke
// -------------------------

template <class Q>
static void api_smoke_compile() {
    using pointer = typename Q::pointer;

    static_assert(std::is_same_v<decltype(std::declval<Q&>().is_valid()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().capacity()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().free()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().empty()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().full()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().can_write(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().can_read(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().write_size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().read_size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().buffer_size()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().clear()), void>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().get_allocator()), typename Q::base_allocator_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().data()), pointer const*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().data()), pointer const*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().begin()), typename Q::iterator>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().end()), typename Q::iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().begin()), typename Q::const_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().end()), typename Q::const_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().cbegin()), typename Q::const_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().cend()), typename Q::const_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().rbegin()), typename Q::reverse_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().rend()), typename Q::reverse_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().rbegin()), typename Q::const_reverse_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().rend()), typename Q::const_reverse_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().crbegin()), typename Q::const_reverse_iterator>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().crend()), typename Q::const_reverse_iterator>);

    // Producer API
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_push(std::declval<std::uint32_t>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().push(std::declval<std::uint32_t>())), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_push(std::declval<const void*>(), reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().push(std::declval<const void*>(), reg{1})), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_claim()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().template claim_as<std::uint32_t>()), std::uint32_t*>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_write(std::declval<std::uint32_t>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().publish(reg{1})), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_publish(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write(::spsc::unsafe)), typename Q::regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_write(::spsc::unsafe, reg{1})), typename Q::regions>);

    // Consumer API
    static_assert(std::is_same_v<decltype(std::declval<Q&>().front()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().front()), typename Q::const_pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_front()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().try_front()), typename Q::const_pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().template front_as<std::uint32_t>()), std::uint32_t*>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().template try_peek<std::uint32_t>(std::declval<std::uint32_t&>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().pop(reg{1})), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_pop(reg{1})), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read(::spsc::unsafe)), typename Q::regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().claim_read(::spsc::unsafe, reg{1})), typename Q::regions>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().operator[](reg{0})), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().operator[](reg{0})), typename Q::const_pointer>);

    // Snapshots
    static_assert(std::is_same_v<decltype(std::declval<Q&>().make_snapshot()), typename Q::snapshot>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().make_snapshot()), typename Q::const_snapshot>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().consume(std::declval<const typename Q::snapshot&>())), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().try_consume(std::declval<const typename Q::snapshot&>())), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().consume_all()), void>);

    // Lifetime/ownership API
    static_assert(std::is_same_v<decltype(std::declval<Q&>().destroy()), void>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().swap(std::declval<Q&>())), void>);
    if constexpr (requires(Q& q) { q.resize(reg{2u}, reg{64u}); }) {
        static_assert(std::is_same_v<decltype(std::declval<Q&>().resize(reg{2u}, reg{64u})), bool>);
    } else {
        static_assert(std::is_same_v<decltype(std::declval<Q&>().resize(reg{64u})), bool>);
    }

    // RAII guards
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_write()), typename Q::write_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_write(reg{1})), typename Q::bulk_write_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_read()), typename Q::read_guard>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().scoped_read(reg{1})), typename Q::bulk_read_guard>);

    static_assert(std::is_same_v<decltype(std::declval<typename Q::write_guard&>().get()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::write_guard&>().peek()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::write_guard&>().template as<std::uint32_t>()), std::uint32_t*>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_write_guard&>().claimed()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_write_guard&>().constructed()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_write_guard&>().remaining()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_write_guard&>().get_next()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_write_guard&>().template write_next<std::uint32_t>(std::declval<const std::uint32_t&>())), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_write_guard&>().write_next(std::declval<const void*>(), reg{1})), pointer>);

    static_assert(std::is_same_v<decltype(std::declval<typename Q::read_guard&>().get()), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::read_guard&>().template as<std::uint32_t>()), std::uint32_t*>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_read_guard&>().count()), reg>);
    static_assert(std::is_same_v<decltype(std::declval<typename Q::bulk_read_guard&>().regions_view()), const typename Q::regions&>);

#if SPSC_HAS_SPAN
    static_assert(std::is_same_v<decltype(std::declval<Q&>().span()), std::span<std::byte>>);
#endif

    // Const API should compile.
    (void)sizeof(decltype(std::declval<const Q&>().cbegin()));
}

static void api_compile_smoke_all() {
    using SP = spsc::pool<16u, spsc::policy::P>;
    using SV = spsc::pool<16u, spsc::policy::V>;
    using SA = spsc::pool<16u, spsc::policy::A<>>;
    using SC = spsc::pool<16u, spsc::policy::CA<>>;
    using DP = spsc::pool<0u, spsc::policy::P>;
    using DA = spsc::pool<0u, spsc::policy::A<>>;

    api_smoke_compile<SP>();
    api_smoke_compile<SV>();
    api_smoke_compile<SA>();
    api_smoke_compile<SC>();
    api_smoke_compile<DP>();
    api_smoke_compile<DA>();
}

struct Blob final {
    std::uint32_t seq{0};
    std::uint32_t inv{0};
    std::uint64_t salt{0};
    std::array<std::byte, 44> payload{};
};
static_assert(std::is_trivially_copyable_v<Blob>);
static_assert(sizeof(Blob) <= kBufSz);

static inline Blob make_blob(std::uint32_t seq, std::mt19937& rng) noexcept {
    Blob b{};
    b.seq  = seq;
    b.inv  = ~seq;
    b.salt = (static_cast<std::uint64_t>(rng()) << 32) ^ rng();
    for (auto& x : b.payload) {
        x = static_cast<std::byte>(rng() & 0xFF);
    }
    return b;
}

static inline void load_blob_from_slot(const void* p, Blob& out) noexcept {
    std::memcpy(&out, p, sizeof(Blob));
}

static inline void store_blob_to_slot(void* p, const Blob& in) noexcept {
    std::memcpy(p, &in, sizeof(Blob));
}

static inline void expect_blob_eq(const Blob& a, const Blob& b) {
    QCOMPARE(a.seq, b.seq);
    QCOMPARE(a.inv, b.inv);
    QCOMPARE(a.salt, b.salt);
    QCOMPARE(a.payload, b.payload);
}

// Deliberately misalign only std::byte allocations (buffer storage), but keep
// rebind allocations (pointer arrays) aligned and well-formed.

template <class T>
struct misalign_byte_alloc {
    using value_type = T;

    misalign_byte_alloc() noexcept = default;

    template <class U>
    misalign_byte_alloc(const misalign_byte_alloc<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t n) {
        std::allocator<T> base;
        if constexpr (std::is_same_v<T, std::byte>) {
            // Allocate +1 byte and shift by 1 to break typical power-of-two alignments.
            auto* raw = base.allocate(n + 1u);
            return reinterpret_cast<T*>(reinterpret_cast<std::byte*>(raw) + 1u);
        } else {
            return base.allocate(n);
        }
    }

    void deallocate(T* p, std::size_t n) noexcept {
        std::allocator<T> base;
        if constexpr (std::is_same_v<T, std::byte>) {
            auto* raw = reinterpret_cast<T*>(reinterpret_cast<std::byte*>(p) - 1u);
            base.deallocate(raw, n + 1u);
        } else {
            base.deallocate(p, n);
        }
    }

    template <class U>
    bool operator==(const misalign_byte_alloc<U>&) const noexcept { return true; }

    template <class U>
    bool operator!=(const misalign_byte_alloc<U>&) const noexcept { return false; }
};

// Resize helper that works for both static and dynamic pools.

template <class Q>
static bool resize_to(Q& q, reg depth, reg buf_sz) {
    if constexpr (requires(Q& x) { x.resize(depth, buf_sz); }) {
        return q.resize(depth, buf_sz);
    } else {
        (void)depth;
        return q.resize(buf_sz);
    }
}

template <class Q>
static void ensure_valid(Q& q, reg depth = kDepth, reg buf_sz = kBufSz) {
    QVERIFY(resize_to(q, depth, buf_sz));
    QVERIFY(q.is_valid());
}

// region accessors (some older versions used different field names).

template <typename R>
constexpr auto region_ptr(const R& r) {
    if constexpr (requires { r.ptr; }) {
        return r.ptr;
    } else if constexpr (requires { r.span(); }) {
        return r.span().data();
    } else if constexpr (requires { r.data(); }) {
        return r.data();
    } else {
        static_assert(sizeof(R) == 0, "Region must expose ptr/data/span");
    }
}


template <class R>
static reg region_count(const R& r) {
    if constexpr (requires { r.count; }) {
        return static_cast<reg>(r.count);
    } else {
        return static_cast<reg>(r.n);
    }
}

template <class Q, class U>
static const U* safe_try_front_as(const Q& q) noexcept {
    static_assert(std::is_trivially_copyable_v<U>);

    const auto p = q.try_front();
    if (p == nullptr) {
        return nullptr;
    }

    if (sizeof(U) > static_cast<std::size_t>(q.buffer_size())) {
        return nullptr;
    }

    if ((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) != 0u) {
        return nullptr;
    }

    return reinterpret_cast<const U*>(p);
}


template <class Q>
static void verify_invariants(Q& q, const char* context = nullptr) {
    Q_UNUSED(context);
    const auto& cq = static_cast<const Q&>(q);

    constexpr reg kStaticCap = pool_capacity_v<Q>;

    if (!cq.is_valid()) {
        QCOMPARE(cq.capacity(), reg{0u});
        QCOMPARE(cq.size(), reg{0u});
        QVERIFY(cq.empty());
        QVERIFY(cq.full());
        QCOMPARE(cq.buffer_size(), reg{0u});

        // Dynamic pool: data() must be null. Static pool: data() is stable but entries must be null.
        if constexpr (kStaticCap == 0u) {
            QVERIFY(cq.data() == nullptr);
        } else {
            QVERIFY(cq.data() != nullptr);
            for (reg i = 0; i < kStaticCap; ++i) {
                QVERIFY(cq.data()[i] == nullptr);
            }
        }

        QCOMPARE(cq.read_size(), reg{0u});
        QCOMPARE(cq.write_size(), reg{0u});

        QVERIFY(q.try_front() == nullptr);
        QVERIFY(q.try_claim() == nullptr);
        QVERIFY(!q.try_publish());
        QVERIFY(!q.try_pop());
        QVERIFY(!q.try_consume(cq.make_snapshot()));

        auto wr = q.claim_write(::spsc::unsafe);
        QVERIFY(wr.total == 0u);
        auto rd = q.claim_read(::spsc::unsafe);
        QVERIFY(rd.total == 0u);

        auto bw = q.scoped_write(reg{8u});
        QVERIFY(!static_cast<bool>(bw));
        auto br = q.scoped_read(reg{8u});
        QVERIFY(!static_cast<bool>(br));
        return;
    }

    const reg cap = cq.capacity();
    QVERIFY(cap != 0u);
    QVERIFY(is_pow2(cap));

    QVERIFY(cq.buffer_size() != 0u);

    const reg sz = cq.size();
    QVERIFY(sz <= cap);

    QVERIFY(cq.free() == (cap - sz));
    QVERIFY(cq.empty() == (sz == 0u));
    QVERIFY(cq.full() == (sz == cap));

    QVERIFY(cq.can_read(0u));
    QVERIFY(cq.can_write(0u));

    QVERIFY(cq.can_read(sz));
    QVERIFY(cq.can_write(cap - sz));

    QVERIFY(!cq.can_read(sz + 1u));
    QVERIFY(!cq.can_write((cap - sz) + 1u));

    const reg rs = cq.read_size();
    const reg ws = cq.write_size();

    QVERIFY(rs <= sz);
    QVERIFY(ws <= (cap - sz));

    if (cq.empty()) {
        QCOMPARE(rs, reg{0u});
        QVERIFY(ws > 0u);
    }

    if (cq.full()) {
        QCOMPARE(ws, reg{0u});
        QVERIFY(rs > 0u);
    }

    // Slot pointer table must exist and point to real storages.
    auto slot_ptrs = cq.data();
    if (slot_ptrs == nullptr) {
        QFAIL("data() returned nullptr for a valid pool");
        return;
    }
    for (reg i = 0; i < cap; ++i) {
        QVERIFY(slot_ptrs[i] != nullptr);
    }

    // operator[] contract: 0 <= i < size.
    for (reg i = 0; i < sz; ++i) {
        QVERIFY(cq[i] != nullptr);
    }

    // Snapshot belongs to this pool.
    {
        const auto snap = cq.make_snapshot();
        // These are assert-checked in pool.hpp, but verify anyway.
        QCOMPARE(snap.begin().data(), cq.data());
        const reg expectedMask = (cap != 0u) ? (cap - 1u) : 0u;
        QCOMPARE(snap.begin().mask(), expectedMask);
    }
}

template <class Q>
static void fill_and_drain_basic(Q& q) {
    verify_invariants(q);

    std::mt19937 rng(0xC0FFEEu);

    // try_push / try_front / try_pop
    for (reg i = 0; i < q.capacity(); ++i) {
        Blob b = make_blob(static_cast<std::uint32_t>(i + 1u), rng);
        QVERIFY(q.try_push(b));
        verify_invariants(q);
    }
    QVERIFY(q.full());

    // push() (precondition: not full)
    {
        QVERIFY(q.try_pop());
        Blob b = make_blob(0xAA55u, rng);
        q.push(b);
        QVERIFY(q.full());
    }

    // front() / pop() (precondition: not empty)
    {
        Blob got{};
        load_blob_from_slot(q.front(), got);
        QVERIFY(got.seq != 0u);
        q.pop();
        verify_invariants(q);
    }

    // Drain the rest.
    while (!q.empty()) {
        Blob got{};
        load_blob_from_slot(q.front(), got);
        QVERIFY(got.inv == ~got.seq);
        q.pop();
    }

    QVERIFY(q.empty());
    verify_invariants(q);

    // consume_all()
    for (reg i = 0; i < q.capacity() / 2u; ++i) {
        Blob b = make_blob(0x1000u + static_cast<std::uint32_t>(i), rng);
        QVERIFY(q.try_push(b));
    }
    QVERIFY(!q.empty());
    q.consume_all();
    QVERIFY(q.empty());
    verify_invariants(q);
}

template <class Q>
static void test_claim_publish_path(Q& q) {
    std::mt19937 rng(123u);

    // claim()/publish() hot path.
    while (!q.full()) {
        auto* p = q.claim();
        QVERIFY(p != nullptr);
        const Blob b = make_blob(0xB000u + static_cast<std::uint32_t>(q.size()), rng);
        store_blob_to_slot(p, b);
        q.publish();
    }
    QVERIFY(q.full());

    // try_claim()/try_publish() on full.
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(!q.try_publish());

    // Read out using try_front/try_pop.
    while (!q.empty()) {
        const void* p = q.try_front();
        QVERIFY(p != nullptr);
        Blob got{};
        load_blob_from_slot(p, got);
        QVERIFY(got.inv == ~got.seq);
        QVERIFY(q.try_pop());
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_raw_push_truncation(Q& q) {
    std::mt19937 rng(77u);

    q.consume_all();
    QVERIFY(q.empty());

    struct Big final {
        std::array<std::byte, 256> bytes{};
    };
    static_assert(std::is_trivially_copyable_v<Big>);

    Big big{};
    for (auto& x : big.bytes) {
        x = static_cast<std::byte>(rng() & 0xFF);
    }

    // push(raw): success path with truncation.
    {
        const reg before = q.size();
        q.push(&big, static_cast<reg>(sizeof(big)));
        QCOMPARE(q.size(), static_cast<reg>(before + 1u));

        std::vector<std::byte> tmp(q.buffer_size());
        std::memcpy(tmp.data(), q.front(), tmp.size());
        QCOMPARE(std::memcmp(tmp.data(), &big, tmp.size()), 0);

        q.pop();
        QCOMPARE(q.size(), before);
    }

    QVERIFY(q.try_push(&big, sizeof(big)));

    // Must be truncated to buffer_size.
    std::vector<std::byte> tmp(q.buffer_size());
    std::memcpy(tmp.data(), q.front(), tmp.size());

    QCOMPARE(tmp.size(), static_cast<std::size_t>(q.buffer_size()));
    QCOMPARE(std::memcmp(tmp.data(), &big, tmp.size()), 0);

    q.pop();
    QVERIFY(q.empty());

    // Null source with non-zero copy size must fail safely in try_push.
    {
        const reg before = q.size();
        QVERIFY(!q.try_push(nullptr, reg{1u}));
        QCOMPARE(q.size(), before);
    }

    // Zero-byte write is a valid no-op copy into one claimed slot.
    {
        const reg before = q.size();
        QVERIFY(q.try_push(nullptr, reg{0u}));
        QCOMPARE(q.size(), static_cast<reg>(before + 1u));
        q.pop();
        QCOMPARE(q.size(), before);
    }
}

template <class Q>
static void test_iteration_and_indexing(Q& q) {
    std::mt19937 rng(0xFEEDu);

    q.consume_all();
    QVERIFY(q.empty());

    constexpr reg n = 7u;
    for (reg i = 0; i < n; ++i) {
        Blob b = make_blob(static_cast<std::uint32_t>(0x9000u + i), rng);
        QVERIFY(q.try_push(b));
    }

    // operator[] order == iterator order.
    {
        reg i = 0;
        for (auto it = q.begin(); it != q.end(); ++it, ++i) {
            Blob a{};
            Blob b{};
            load_blob_from_slot(*it, a);
            load_blob_from_slot(q[i], b);
            expect_blob_eq(a, b);
        }
        QCOMPARE(i, q.size());
    }

    // reverse iterators.
    {
        reg count = 0;
        for (auto it = q.rbegin(); it != q.rend(); ++it) {
            QVERIFY(*it != nullptr);
            ++count;
        }
        QCOMPARE(count, q.size());
    }

    // Snapshot iteration.
    {
        const auto snap = q.make_snapshot();
        reg count = 0;
        for (auto it = snap.begin(); it != snap.end(); ++it) {
            QVERIFY(*it != nullptr);
            ++count;
        }
        QCOMPARE(count, q.size());

        // consume(snapshot)
        q.consume(snap);
        QVERIFY(q.empty());
    }

    verify_invariants(q);
}

template <class Q>
static void test_snapshot_contracts(Q& q) {
    std::mt19937 rng(42u);

    q.consume_all();
    QVERIFY(q.empty());

    QVERIFY(q.try_push(make_blob(1u, rng)));
    QVERIFY(q.try_push(make_blob(2u, rng)));

    const auto snap = q.make_snapshot();

    // Snapshot must not be consumable by another pool.
    {
        Q other;
        ensure_valid(other);
        QVERIFY(!other.try_consume(snap));
    }

    // Snapshot becomes stale if tail changes.
    QVERIFY(q.try_pop());
    QVERIFY(!q.try_consume(snap));

    // Fresh snapshot should be consumable.
    const auto snap2 = q.make_snapshot();
    QVERIFY(q.try_consume(snap2));
    QVERIFY(q.empty());
}

template <class Q>
static void test_zero_count_contracts(Q& q) {
    q.consume_all();
    const reg before_size = q.size();
    const reg before_free = q.free();

    // Zero-count variants must be no-ops and always valid.
    QVERIFY(q.try_publish(0u));
    q.publish(0u);
    QVERIFY(q.try_pop(0u));
    q.pop(0u);

    const auto wr0 = q.claim_write(::spsc::unsafe, 0u);
    const auto rd0 = q.claim_read(::spsc::unsafe, 0u);
    QCOMPARE(wr0.total, reg{0u});
    QCOMPARE(rd0.total, reg{0u});

    auto bw0 = q.scoped_write(reg{0u});
    auto br0 = q.scoped_read(reg{0u});
    QVERIFY(!static_cast<bool>(bw0));
    QVERIFY(!static_cast<bool>(br0));

    QCOMPARE(q.size(), before_size);
    QCOMPARE(q.free(), before_free);
}

template <class Q>
static void test_typed_view_contracts(Q& q) {
    std::mt19937 rng(0x5EEDu);
    q.consume_all();
    QVERIFY(q.empty());

    Blob out{};
    QVERIFY(!q.try_peek(out));

    // try_write/try_peek/readback contract.
    const Blob first = make_blob(0x1111u, rng);
    QVERIFY(q.try_write(first));
    QVERIFY(q.try_peek(out));
    expect_blob_eq(out, first);

    auto* f0 = q.template front_as<Blob>();
    QVERIFY(f0 != nullptr);
    expect_blob_eq(*f0, first);
    q.pop();
    QVERIFY(q.empty());

    // claim_as/front_as oversize should fail safely.
    struct TooBig final {
        std::array<std::byte, 4096> bytes{};
    };
    if (sizeof(TooBig) > static_cast<std::size_t>(q.buffer_size())) {
        QVERIFY(q.template claim_as<TooBig>() == nullptr);
        QVERIFY(q.template front_as<TooBig>() == nullptr);
    }

    // write_guard::as<U>() must arm publish-on-destroy.
    {
        const reg before = q.size();
        {
            auto g = q.scoped_write();
            QVERIFY(static_cast<bool>(g) == (!q.full()));
            if (g) {
                auto* p = g.template as<Blob>();
                QVERIFY(p != nullptr);
                const Blob b = make_blob(0x2222u, rng);
                store_blob_to_slot(p, b);
            }
        }
        QCOMPARE(q.size(), static_cast<reg>(before + (before < q.capacity() ? 1u : 0u)));
    }

    // read_guard::as<U>() typed view contract.
    {
        auto g = q.scoped_read();
        QVERIFY(static_cast<bool>(g));
        auto* p = g.template as<Blob>();
        QVERIFY(p != nullptr);
        QCOMPARE(p->seq, std::uint32_t{0x2222u});
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_bulk_limit_contracts(Q& q) {
    std::mt19937 rng(0xCAFEu);
    q.consume_all();

    // claim_write max_count limit.
    {
        const auto wr = q.claim_write(::spsc::unsafe, 1u);
        QVERIFY(wr.total <= 1u);
    }

    // Fill few and verify claim_read max_count limit.
    for (reg i = 0; i < 4u && !q.full(); ++i) {
        QVERIFY(q.try_push(make_blob(0x3300u + static_cast<std::uint32_t>(i), rng)));
    }

    const reg before = q.size();
    {
        auto br = q.scoped_read(1u);
        QVERIFY(static_cast<bool>(br) == (before != 0u));
        if (br) {
            QCOMPARE(br.count(), reg{1u});
            auto rv = br.regions_view();
            QCOMPARE(rv.total, reg{1u});
            QCOMPARE(static_cast<reg>(region_count(rv.first) + region_count(rv.second)), reg{1u});
        }
    }
    QCOMPARE(q.size(), static_cast<reg>(before ? (before - 1u) : 0u));

    // bulk_write emplace_next path (alias to write_next for pool).
    {
        auto bw = q.scoped_write(2u);
        QVERIFY(static_cast<bool>(bw));
        Blob b = make_blob(0x4400u, rng);
        QVERIFY(bw.emplace_next(b) != nullptr);
        bw.arm_publish();
    }
    QVERIFY(!q.empty());
    q.consume_all();
    QVERIFY(q.empty());
}

template <class Q>
static void test_raii_guards(Q& q) {
    std::mt19937 rng(99u);
    q.consume_all();

    // write_guard: default destruction must NOT publish.
    {
        const reg before = q.size();
        auto g = q.scoped_write();
        if (g) {
            auto* p = g.peek();
            QVERIFY(p != nullptr);
            QCOMPARE(p, g.get());
            store_blob_to_slot(p, make_blob(0xA001u, rng));
        }
        // Not armed => no publish.
        QCOMPARE(q.size(), before);
    }

    // write_guard: publish_on_destroy publishes exactly one (on scope exit).
    {
        const reg before = q.size();
        const reg expected = static_cast<reg>(before + (before != q.capacity() ? 1u : 0u));
        {
            auto g = q.scoped_write();
            QVERIFY(static_cast<bool>(g) == (!q.full()));
            if (g) {
                auto* p = g.get();
                QVERIFY(p != nullptr);
                store_blob_to_slot(p, make_blob(0xA002u, rng));
                g.arm_publish();
            }
        }
        QCOMPARE(q.size(), expected);
    }

    // write_guard: as<Blob>() must arm auto-publish.
    {
        const reg before = q.size();
        const reg expected = static_cast<reg>(before + (before != q.capacity() ? 1u : 0u));
        {
            auto g = q.scoped_write();
            QVERIFY(static_cast<bool>(g) == (!q.full()));
            if (g) {
                auto* p = g.template as<Blob>();
                QVERIFY(p != nullptr);
                *p = make_blob(0xA0022u, rng);
            }
        }
        QCOMPARE(q.size(), expected);
    }

    // write_guard: disarm_publish() cancels previously armed auto-publish.
    {
        const reg before = q.size();
        {
            auto g = q.scoped_write();
            if (g) {
                auto* p = g.get();
                QVERIFY(p != nullptr);
                store_blob_to_slot(p, make_blob(0xA0021u, rng));
                g.arm_publish();
                g.disarm_publish();
            }
        }
        QCOMPARE(q.size(), before);
    }

    // write_guard: commit() publishes immediately.
    {
        const reg before = q.size();
        auto g = q.scoped_write();
        if (g) {
            auto* p = g.get();
            QVERIFY(p != nullptr);
            store_blob_to_slot(p, make_blob(0xA003u, rng));
            g.commit();
            QCOMPARE(q.size(), static_cast<reg>(before + 1u));
        }
    }

    // write_guard: cancel path.
    {
        const reg before = q.size();
        auto g = q.scoped_write();
        if (g) {
            auto* p = g.get();
            QVERIFY(p != nullptr);
            store_blob_to_slot(p, make_blob(0xA004u, rng));
            g.publish_on_destroy();
            g.cancel();
        }
        QCOMPARE(q.size(), before);
    }

    // read_guard: default destruction MUST pop exactly one when active.
    {
        // Ensure there is at least one element.
        if (!q.full()) {
            Blob b = make_blob(0xB100u, rng);
            QVERIFY(q.try_push(b));
        }
        const reg before = q.size();
        {
            auto g = q.scoped_read();
            QVERIFY(static_cast<bool>(g) == (before != 0u));
            if (g) {
                auto* p = g.get();
                QVERIFY(p != nullptr);
                Blob got{};
                load_blob_from_slot(p, got);
                QVERIFY(got.inv == ~got.seq);
            }
        }
        QCOMPARE(q.size(), static_cast<reg>(before - (before ? 1u : 0u)));
    }

    // read_guard: as<Blob>() typed access path.
    {
        if (!q.full()) {
            Blob b = make_blob(0xB1001u, rng);
            QVERIFY(q.try_push(b));
        }
        const reg before = q.size();
        {
            auto g = q.scoped_read();
            QVERIFY(static_cast<bool>(g) == (before != 0u));
            if (g) {
                auto* p = g.template as<Blob>();
                QVERIFY(p != nullptr);
                QVERIFY(p->inv == ~p->seq);
            }
        }
        QCOMPARE(q.size(), static_cast<reg>(before - (before ? 1u : 0u)));
    }

    // read_guard: cancel prevents pop.
    {
        // Ensure one element.
        if (!q.full()) {
            Blob b = make_blob(0xB101u, rng);
            QVERIFY(q.try_push(b));
        }
        const reg before = q.size();
        {
            auto g = q.scoped_read();
            if (g) {
                g.cancel();
            }
        }
        QCOMPARE(q.size(), before);
    }

    // read_guard: commit pops now, destructor must not pop twice.
    {
        if (!q.full()) {
            Blob b = make_blob(0xB102u, rng);
            QVERIFY(q.try_push(b));
        }
        const reg before = q.size();
        {
            auto g = q.scoped_read();
            if (g) {
                g.commit();
            }
        }
        QCOMPARE(q.size(), static_cast<reg>(before - (before ? 1u : 0u)));
    }

    verify_invariants(q);

    // Drain.
    while (!q.empty()) {
        q.pop();
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_bulk_raii_overloads(Q& q) {
    std::mt19937 rng(0xBEEFu);
    q.consume_all();
    QVERIFY(q.empty());

    const reg cap = q.capacity();
    QVERIFY(cap > 0u);

    const reg want = (cap < 6u) ? cap : reg{6u};
    {
        auto bw = q.scoped_write(want);
        QVERIFY(static_cast<bool>(bw));
        QCOMPARE(bw.claimed(), want);
        QCOMPARE(bw.constructed(), reg{0u});
        QCOMPARE(bw.remaining(), want);

        for (reg i = 0u; i < want; ++i) {
            auto* slot = bw.peek_next();
            QVERIFY(slot != nullptr);
            QCOMPARE(slot, bw.get_next());
            store_blob_to_slot(slot, make_blob(1000u + static_cast<std::uint32_t>(i), rng));
            bw.mark_written();
        }

        QCOMPARE(bw.constructed(), want);
        QCOMPARE(bw.remaining(), reg{0u});
        bw.publish_on_destroy();
    }
    QCOMPARE(q.size(), want);

    {
        auto br = q.scoped_read(want);
        QVERIFY(static_cast<bool>(br));
        QCOMPARE(br.count(), want);

        reg seen = 0u;
        for (reg i = 0u; i < br.first().size(); ++i) {
            Blob b{};
            load_blob_from_slot(br.first()[i], b);
            QCOMPARE(b.seq, 1000u + static_cast<std::uint32_t>(seen++));
        }
        for (reg i = 0u; i < br.second().size(); ++i) {
            Blob b{};
            load_blob_from_slot(br.second()[i], b);
            QCOMPARE(b.seq, 1000u + static_cast<std::uint32_t>(seen++));
        }
        QCOMPARE(seen, want);
    }
    QVERIFY(q.empty());

    // cancel() must not publish anything.
    {
        const reg before = q.size();
        auto bw = q.scoped_write(reg{2u});
        QVERIFY(static_cast<bool>(bw));
        auto* slot = bw.get_next();
        QVERIFY(slot != nullptr);
        store_blob_to_slot(slot, make_blob(2001u, rng));
        bw.mark_written();
        bw.cancel();
        QCOMPARE(q.size(), before);
    }

    // commit() publishes exactly written amount.
    {
        auto bw = q.scoped_write(reg{3u});
        QVERIFY(static_cast<bool>(bw));
        for (reg i = 0u; i < reg{3u}; ++i) {
            auto* slot = bw.get_next();
            QVERIFY(slot != nullptr);
            store_blob_to_slot(slot, make_blob(3000u + static_cast<std::uint32_t>(i), rng));
            bw.mark_written();
        }
        bw.commit();
    }
    QCOMPARE(q.size(), reg{3u});

    // bulk read cancel keeps queue intact.
    {
        const reg before = q.size();
        auto br = q.scoped_read(reg{2u});
        QVERIFY(static_cast<bool>(br));
        br.cancel();
        QCOMPARE(q.size(), before);
    }

    // bulk read commit pops claimed amount.
    {
        auto br = q.scoped_read(reg{2u});
        QVERIFY(static_cast<bool>(br));
        QCOMPARE(br.count(), reg{2u});
        br.commit();
    }
    QCOMPARE(q.size(), reg{1u});
    q.pop();
    QVERIFY(q.empty());

    // raw write_next(ptr,size) path + disarm/arm controls.
    {
        Blob b = make_blob(0x3100u, rng);
        auto bw = q.scoped_write(reg{1u});
        QVERIFY(static_cast<bool>(bw));
        QVERIFY(bw.write_next(&b, static_cast<reg>(sizeof(b))) != nullptr);
        bw.disarm_publish();
    }
    QVERIFY(q.empty());

    {
        Blob b = make_blob(0x3200u, rng);
        auto bw = q.scoped_write(reg{1u});
        QVERIFY(static_cast<bool>(bw));
        QVERIFY(bw.write_next(&b, static_cast<reg>(sizeof(b))) != nullptr);
        bw.arm_publish();
    }
    QCOMPARE(q.size(), reg{1u});
    {
        auto br = q.scoped_read(reg{1u});
        QVERIFY(static_cast<bool>(br));
        Blob got{};
        load_blob_from_slot(br.first()[0u], got);
        QCOMPARE(got.seq, 0x3200u);
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_guard_move_semantics(Q& q) {
    std::mt19937 rng(0xC0DEu);
    q.consume_all();

    {
        auto w1 = q.scoped_write();
        QVERIFY(static_cast<bool>(w1));
        auto w2 = std::move(w1);
        QVERIFY(!static_cast<bool>(w1));
        QVERIFY(static_cast<bool>(w2));
        auto* slot = w2.get();
        QVERIFY(slot != nullptr);
        store_blob_to_slot(slot, make_blob(0x7001u, rng));
        w2.publish_on_destroy();
    }
    QCOMPARE(q.size(), reg{1u});

    {
        auto r1 = q.scoped_read();
        QVERIFY(static_cast<bool>(r1));
        auto r2 = std::move(r1);
        QVERIFY(!static_cast<bool>(r1));
        QVERIFY(static_cast<bool>(r2));
    }
    QVERIFY(q.empty());

    {
        auto bw1 = q.scoped_write(reg{2u});
        QVERIFY(static_cast<bool>(bw1));
        auto bw2 = std::move(bw1);
        QVERIFY(!static_cast<bool>(bw1));
        QVERIFY(static_cast<bool>(bw2));

        auto* s0 = bw2.get_next();
        QVERIFY(s0 != nullptr);
        store_blob_to_slot(s0, make_blob(0x7002u, rng));
        bw2.mark_written();
        bw2.commit();
    }
    QCOMPARE(q.size(), reg{1u});

    {
        auto br1 = q.scoped_read(reg{1u});
        QVERIFY(static_cast<bool>(br1));
        auto br2 = std::move(br1);
        QVERIFY(!static_cast<bool>(br1));
        QVERIFY(static_cast<bool>(br2));
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_api_coverage_smoke(Q& q) {
    verify_invariants(q);

    // Basic accessors.
    (void)q.get_allocator();
    (void)q.capacity();
    (void)q.buffer_size();
    (void)q.size();
    (void)q.free();
    (void)q.empty();
    (void)q.full();

    // Const iteration helpers.
    const auto& cq = static_cast<const Q&>(q);
    (void)cq.cbegin();
    (void)cq.cend();
    (void)cq.crbegin();
    (void)cq.crend();

    // Guard overload surface (single + bulk).
    {
        auto bw = q.scoped_write(reg{2u});
        (void)static_cast<bool>(bw);
        if (bw) {
            (void)bw.claimed();
            (void)bw.constructed();
            (void)bw.remaining();
            bw.cancel();
        }

        auto br = q.scoped_read(reg{2u});
        (void)static_cast<bool>(br);
        if (br) {
            (void)br.count();
            (void)br.regions_view();
            br.cancel();
        }
    }

    // clear() must keep validity but drop size.
    {
        std::mt19937 rng(0xA5u);
        if (!q.full()) {
            QVERIFY(q.try_push(make_blob(0xFACEu, rng)));
        }
        QVERIFY(q.is_valid());
        q.clear();
        QVERIFY(q.is_valid());
        QVERIFY(q.empty());
    }

    // Typed views: try_* variants must never assert.
    struct alignas(8) A8 { std::uint32_t a; std::uint32_t b; };
    static_assert(std::is_trivially_copyable_v<A8>);

    {
        q.consume_all();
        QVERIFY(q.empty());

#if SPSC_HAS_SPAN
        QVERIFY(q.span().empty());
#endif

        auto* p = q.template claim_as<A8>();
        if (p != nullptr) {
            p->a = 0x11223344u;
            p->b = 0x55667788u;
            QVERIFY(q.try_publish());
            QVERIFY(!q.empty());

#if SPSC_HAS_SPAN
            {
                auto sp = q.span();
                QCOMPARE(static_cast<reg>(sp.size()), q.buffer_size());
                QCOMPARE(sp.data(), static_cast<std::byte*>(q.front()));
            }
#endif

            // try_front_as is const and must be safe.
            auto* f = safe_try_front_as<Q, A8>(cq);
            QVERIFY(f != nullptr);
            QCOMPARE(f->a, 0x11223344u);
            QCOMPARE(f->b, 0x55667788u);

            // try_pop(count)
            QVERIFY(q.try_pop(1u));
            QVERIFY(q.empty());
        }
    }

    // Bulk regions + try_publish(count) / try_pop(count).
    {
        q.consume_all();
        QVERIFY(q.empty());

        const reg want = 3u;
        auto wr = q.claim_write(::spsc::unsafe, want);
        QVERIFY(wr.total <= want);

        std::mt19937 rng(0xB6u);
        reg wrote = 0;
        const auto fill = [&](const auto& r) {
            const reg n = region_count(r);
            auto p = region_ptr(r);
            for (reg i = 0; i < n; ++i) {
                store_blob_to_slot(p[i], make_blob(0xC0DE0000u + wrote, rng));
                ++wrote;
            }
        };
        fill(wr.first);
        fill(wr.second);

        QVERIFY(q.try_publish(wr.total));
        QCOMPARE(q.size(), wr.total);

        // try_publish too much must fail.
        if (q.free() != 0u) {
            QVERIFY(!q.try_publish(static_cast<reg>(q.free() + 1u)));
        }

        auto rd = q.claim_read(::spsc::unsafe, want);
        QVERIFY(rd.total <= want);
        QVERIFY(rd.total <= q.size());

        // try_pop too much must fail.
        if (q.size() != 0u) {
            QVERIFY(!q.try_pop(static_cast<reg>(q.size() + 1u)));
        }

        QVERIFY(q.try_pop(rd.total));
        QVERIFY(q.empty());
    }

    verify_invariants(q);
}

template <class Q>
static void test_ctor_contracts() {
    // buffer_size == 0 must yield an invalid pool.
    if constexpr (pool_capacity_v<Q> != 0u) {
        Q bad(reg{0u});
        QVERIFY(!bad.is_valid());
        verify_invariants(bad);
    } else {
        {
            Q bad_depth0(reg{0u}, reg{kBufSz});
            QVERIFY(!bad_depth0.is_valid());
            verify_invariants(bad_depth0);
        }
        {
            Q bad_buf0(reg{8u}, reg{0u});
            QVERIFY(!bad_buf0.is_valid());
            verify_invariants(bad_buf0);
        }

        // Depth must round up to pow2 and be >= requested.
        Q q(reg{17u}, reg{kBufSz});
        QVERIFY(q.is_valid());
        QVERIFY(is_pow2(q.capacity()));
        QVERIFY(q.capacity() >= 17u);
        QCOMPARE(q.buffer_size(), reg{kBufSz});
        verify_invariants(q);
    }
}

// The requested "anti-human" churn: force wrap-around split regions.

template <class Q>
static void test_wraparound_bulk_regions() {
    Q q;
    ensure_valid(q);

    const reg cap = q.capacity();
    QVERIFY(cap >= 8u);

    std::mt19937 rng(7u);

    // Step 1: advance indices to make head/tail land near the end of the ring,
    // while keeping the queue empty.
    const reg churn_n = (cap - 2u);
    for (reg i = 0; i < churn_n; ++i) {
        QVERIFY(q.try_push(make_blob(static_cast<std::uint32_t>(i + 1u), rng)));
        q.pop();
    }
    QVERIFY(q.empty());

    // Step 2: claim_write must split (first + second).
    {
        auto wr = q.claim_write(::spsc::unsafe, 4u);
        QVERIFY(wr.total == 4u);
        QCOMPARE(region_count(wr.first), reg{2u});
        QCOMPARE(region_count(wr.second), reg{2u});

        // Fill in physical order (first region then second).
        reg seq = 0;
        auto p1 = region_ptr(wr.first);
        auto p2 = region_ptr(wr.second);
        for (reg i = 0; i < region_count(wr.first); ++i) {
            store_blob_to_slot(p1[i], make_blob(0xF000u + static_cast<std::uint32_t>(seq++), rng));
        }
        for (reg i = 0; i < region_count(wr.second); ++i) {
            store_blob_to_slot(p2[i], make_blob(0xF000u + static_cast<std::uint32_t>(seq++), rng));
        }

        q.publish(wr.total);
        QCOMPARE(q.size(), reg{4u});
    }

    // Step 3: claim_read must also split at the same boundary.
    {
        auto rd = q.claim_read(::spsc::unsafe, 4u);
        QVERIFY(rd.total == 4u);
        QCOMPARE(region_count(rd.first), reg{2u});
        QCOMPARE(region_count(rd.second), reg{2u});

        // Validate logical order across wrap.
        std::vector<Blob> got;
        got.reserve(4u);

        auto r1 = region_ptr(rd.first);
        auto r2 = region_ptr(rd.second);

        for (reg i = 0; i < region_count(rd.first); ++i) {
            Blob b{};
            load_blob_from_slot(r1[i], b);
            got.push_back(b);
        }
        for (reg i = 0; i < region_count(rd.second); ++i) {
            Blob b{};
            load_blob_from_slot(r2[i], b);
            got.push_back(b);
        }

        QCOMPARE(got.size(), std::size_t{4u});
        for (std::size_t i = 0; i < got.size(); ++i) {
            QVERIFY(got[i].seq != 0u);
        }

        q.pop(rd.total);
        QVERIFY(q.empty());
    }

    // Read-wrap variant: land tail near end, then push more than r2e.
    {
        Q q2;
        ensure_valid(q2);

        const reg churn2 = (cap - 3u);
        for (reg i = 0; i < churn2; ++i) {
            QVERIFY(q2.try_push(make_blob(static_cast<std::uint32_t>(0x200u + i), rng)));
            q2.pop();
        }
        QVERIFY(q2.empty());

        for (reg i = 0; i < 5u; ++i) {
            QVERIFY(q2.try_push(make_blob(static_cast<std::uint32_t>(0x300u + i), rng)));
        }

        auto rd = q2.claim_read(::spsc::unsafe, 5u);
        QVERIFY(rd.total == 5u);
        QCOMPARE(region_count(rd.first), reg{3u});
        QCOMPARE(region_count(rd.second), reg{2u});

        q2.pop(rd.total);
        QVERIFY(q2.empty());
    }
}

template <class Q>
static void fuzz_ops(Q& q) {
    std::mt19937 rng(0x1234567u);
    std::deque<Blob> model;

    auto pop_one = [&] {
        QVERIFY(!model.empty());
        Blob got{};
        load_blob_from_slot(q.front(), got);
        expect_blob_eq(got, model.front());
        q.pop();
        model.pop_front();
    };

    for (int it = 0; it < kFuzzIters; ++it) {
        verify_invariants(q);

        const int op = static_cast<int>(rng() % 9u);

        switch (op) {
        case 0: {
            // try_push
            if (!q.full()) {
                Blob b = make_blob(static_cast<std::uint32_t>(it + 1u), rng);
                QVERIFY(q.try_push(b));
                model.push_back(b);
            } else {
                Blob b = make_blob(static_cast<std::uint32_t>(it + 1u), rng);
                QVERIFY(!q.try_push(b));
            }
        } break;

        case 1: {
            // try_pop
            if (!model.empty()) {
                QVERIFY(q.try_pop());
                model.pop_front();
            } else {
                QVERIFY(!q.try_pop());
            }
        } break;

        case 2: {
            // claim/publish
            if (!q.full()) {
                void* p = q.try_claim();
                QVERIFY(p != nullptr);
                Blob b = make_blob(static_cast<std::uint32_t>(0x4000u + it), rng);
                store_blob_to_slot(p, b);
                QVERIFY(q.try_publish());
                model.push_back(b);
            } else {
                QVERIFY(q.try_claim() == nullptr);
                QVERIFY(!q.try_publish());
            }
        } break;

        case 3: {
            // Bulk claim_write/publish
            const reg want = static_cast<reg>((rng() % 6u) + 1u);
            auto wr = q.claim_write(::spsc::unsafe, want);
            QVERIFY(wr.total <= want);
            QVERIFY(wr.total <= q.free());

            reg wrote = 0;
            const auto fill = [&](const auto& r) {
                const reg n = region_count(r);
                auto p = region_ptr(r);
                for (reg i = 0; i < n; ++i) {
                    Blob b = make_blob(static_cast<std::uint32_t>(0x8000u + it * 7 + wrote), rng);
                    store_blob_to_slot(p[i], b);
                    model.push_back(b);
                    ++wrote;
                }
            };
            fill(wr.first);
            fill(wr.second);

            if (wr.total != 0u) {
                q.publish(wr.total);
            }
        } break;

        case 4: {
            // Bulk claim_read/pop
            const reg want = static_cast<reg>((rng() % 6u) + 1u);
            auto rd = q.claim_read(::spsc::unsafe, want);
            QVERIFY(rd.total <= want);
            QVERIFY(rd.total <= q.size());

            reg seen = 0;
            const auto check = [&](const auto& r) {
                const reg n = region_count(r);
                auto p = region_ptr(r);
                for (reg i = 0; i < n; ++i) {
                    QVERIFY(!model.empty());
                    Blob got{};
                    load_blob_from_slot(p[i], got);
                    expect_blob_eq(got, model.front());
                    model.pop_front();
                    ++seen;
                }
            };
            check(rd.first);
            check(rd.second);

            if (rd.total != 0u) {
                q.pop(rd.total);
            }
            QCOMPARE(seen, rd.total);
        } break;

        case 5: {
            // Snapshot try_consume
            const auto snap = q.make_snapshot();
            const reg snap_sz = q.size();
            QVERIFY(q.try_consume(snap));
            QVERIFY(q.size() == 0u);
            model.clear();
            QVERIFY(snap_sz == snap.size());
        } break;

        case 6: {
            // consume_all
            q.consume_all();
            model.clear();
        } break;

        case 7: {
            // Pop one with exact model compare.
            if (!model.empty()) {
                pop_one();
            }
        } break;

        case 8: {
            // No-op reads.
            (void)q.write_size();
            (void)q.read_size();
            (void)q.empty();
            (void)q.full();
        } break;

        default: break;
        }

        // Model must match queue size exactly.
        QCOMPARE(static_cast<reg>(model.size()), q.size());
    }

    // Drain and validate.
    while (!q.empty()) {
        if (!model.empty()) {
            pop_one();
        } else {
            q.pop();
        }
    }
    QVERIFY(q.empty());
}

template <class Q>
static void test_copy_move_semantics() {
    std::mt19937 rng(0xBADC0DEu);

    Q a;
    ensure_valid(a);
    a.consume_all();

    // Fill half.
    const reg n = a.capacity() / 2u;
    std::deque<Blob> model;
    for (reg i = 0; i < n; ++i) {
        Blob b = make_blob(0xCAFE0000u + static_cast<std::uint32_t>(i), rng);
        QVERIFY(a.try_push(b));
        model.push_back(b);
    }

    // Copy ctor.
    Q b(a);
    QVERIFY(b.is_valid() == a.is_valid());
    QCOMPARE(b.size(), a.size());
    for (reg i = 0; i < b.size(); ++i) {
        Blob got{};
        load_blob_from_slot(b[i], got);
        expect_blob_eq(got, model[static_cast<std::size_t>(i)]);
    }

    // Copy assignment.
    Q c;
    ensure_valid(c);
    c = a;
    QCOMPARE(c.size(), a.size());

    // Move ctor.
    Q moved(std::move(a));
    QVERIFY(moved.is_valid());
    QCOMPARE(moved.size(), n);
    // Using moved-from objects is valid but unspecified in general; the pool
    // contract currently makes it invalid/empty, but static analyzers flag any
    // post-move use. Keep the check in normal builds, skip under clang analyzer.
#if !defined(__clang_analyzer__)
    QVERIFY(!a.is_valid());
#endif

    // Move assignment.
    Q d;
    ensure_valid(d);
    d = std::move(moved);
    QVERIFY(d.is_valid());
    QCOMPARE(d.size(), n);

    // swap + ADL swap.
    Q e;
    ensure_valid(e);
    e.consume_all();
    QVERIFY(e.empty());

    d.swap(e);
    QVERIFY(d.empty());
    QCOMPARE(e.size(), n);

    using std::swap;
    swap(d, e);
    QCOMPARE(d.size(), n);
    QVERIFY(e.empty());

    verify_invariants(d);
    verify_invariants(e);
}

template <class Q>
static void test_resize_semantics() {
    std::mt19937 rng(0x123u);

    Q q;
    ensure_valid(q);
    q.consume_all();

    // Fill some data.
    const reg n = (q.capacity() >= 8u) ? 7u : (q.capacity() / 2u);
    std::deque<Blob> model;
    for (reg i = 0; i < n; ++i) {
        Blob b = make_blob(0xDADA0000u + static_cast<std::uint32_t>(i), rng);
        QVERIFY(q.try_push(b));
        model.push_back(b);
    }

    const reg old_cap = q.capacity();
    const reg old_bs  = q.buffer_size();

    // Resize with smaller buffer size must not shrink.
    QVERIFY(resize_to(q, old_cap, (old_bs > 1u) ? (old_bs - 1u) : old_bs));
    QCOMPARE(q.capacity(), old_cap);
    QCOMPARE(q.buffer_size(), old_bs);
    QCOMPARE(q.size(), n);

    // Grow buffer size.
    const reg new_bs = static_cast<reg>(old_bs + 32u);
    QVERIFY(resize_to(q, old_cap, new_bs));
    QCOMPARE(q.capacity(), old_cap);
    QCOMPARE(q.buffer_size(), new_bs);
    QCOMPARE(q.size(), n);

    // Validate migrated data.
    for (reg i = 0; i < q.size(); ++i) {
        Blob got{};
        load_blob_from_slot(q[i], got);
        expect_blob_eq(got, model[static_cast<std::size_t>(i)]);
    }

    // Dynamic pools: request depth growth must increase capacity to next pow2 (or keep old if already >=).
    if constexpr (requires(Q& x) { x.resize(reg{}, reg{}); }) {
        const reg want_depth = static_cast<reg>(old_cap + 3u);
        QVERIFY(q.resize(want_depth, new_bs));
        QVERIFY(q.capacity() >= old_cap);
        QVERIFY(is_pow2(q.capacity()));
    }

    // Shrink-to-zero should fully destroy.
    if constexpr (requires(Q& x) { x.resize(reg{}, reg{}); }) {
        QVERIFY(q.resize(0u, 0u));
    } else {
        QVERIFY(q.resize(0u));
    }
    QVERIFY(!q.is_valid());
    verify_invariants(q);
}

template <class Q>
static void test_shadow_swap_regression() {
    // Two pools with different head/tail histories.
    Q a;
    Q b;
    ensure_valid(a);
    ensure_valid(b);

    std::mt19937 rng(12345u);

    auto churn = [&](Q& qq, int cycles) {
        for (int i = 0; i < cycles; ++i) {
            Blob x = make_blob(static_cast<std::uint32_t>(i + 1u), rng);
            QVERIFY(qq.try_push(x));
            qq.pop();
        }
        verify_invariants(qq);
    };

    churn(a, 120);
    churn(b, 77);

    // Make b full and a empty.
    while (!b.full()) {
        Blob x = make_blob(0xABCD0000u + static_cast<std::uint32_t>(b.size()), rng);
        QVERIFY(b.try_push(x));
    }
    QVERIFY(b.full());
    QVERIFY(a.empty());

    // Warm up cached paths.
    (void)a.write_size();
    (void)a.read_size();
    (void)b.write_size();
    (void)b.read_size();

    // Swap (regression target: shadow cache must be re-initialized).
    a.swap(b);

    QVERIFY(a.full());
    QVERIFY(!a.can_write(1u));
    QVERIFY(a.read_size() > 0u);

    QVERIFY(b.empty());
    QVERIFY(!b.can_read(1u));
    QVERIFY(b.write_size() > 0u);

    verify_invariants(a);
    verify_invariants(b);
}

// Alignment tests.

template <std::size_t Align>
struct alignas(Align) AlignedWord {
    std::uint64_t x;
    std::uint64_t y;
};

template <std::size_t Align>
static constexpr bool kIsPow2Align = (Align != 0u) && ((Align & (Align - 1u)) == 0u);
static_assert(alignof(AlignedWord<128u>) == 128u);
static_assert((sizeof(AlignedWord<128u>) % 128u) == 0u);
static_assert(sizeof(AlignedWord<128u>) >= 16u);

template <class Q, std::size_t Align>
static void test_alignment_behavior(bool require_claim_as_success) {
    static_assert(kIsPow2Align<Align>);
    using U = AlignedWord<Align>;
    static_assert(std::is_trivially_copyable_v<U>);
    static_assert(alignof(U) == Align);
    static_assert((sizeof(U) % alignof(U)) == 0u);

    Q q;

    const reg buf_sz = require_claim_as_success
                           ? static_cast<reg>(std::max(sizeof(U), std::size_t{kBufSz}))
                           : kBufSz;

    ensure_valid(q, kDepth, buf_sz);

    // Post-resize contract: must be valid and empty (do NOT assume head/tail == 0).
    QVERIFY(q.is_valid());
    QVERIFY(q.empty());
    QVERIFY(!q.full());
    QCOMPARE(q.size(), reg{0u});
    QCOMPARE(q.read_size(), reg{0u});
    QCOMPARE(q.free(), q.capacity());
    QVERIFY(q.try_front() == nullptr);

    // Global invariant sweep right after init/resize.
    verify_invariants(q, "alignment post-resize");

    auto* slot_ptrs = q.data();
    QVERIFY2(slot_ptrs != nullptr, "pool::data() returned nullptr for a valid pool");

    // Slots must exist; when we require alignment, every slot must satisfy it.
    for (reg i = 0; i < q.capacity(); ++i) {
        const auto slot = slot_ptrs[i];
        QVERIFY2(slot != nullptr, "pool::data()[i] returned nullptr for a valid pool");

        if (require_claim_as_success) {
            const auto addr = reinterpret_cast<std::uintptr_t>(slot);
            QVERIFY2((addr % Align) == 0u, "Slot pointer is not aligned as requested");
        }
    }

    // Empty-read contract: claim_read() must be zero and must not mutate state.
    {
        const reg before_sz   = q.size();
        const reg before_free = q.free();

        auto rd0 = q.claim_read(::spsc::unsafe, q.capacity());
        QCOMPARE(rd0.total, reg{0u});
        QCOMPARE(region_count(rd0.first), reg{0u});
        QCOMPARE(region_count(rd0.second), reg{0u});

        QCOMPARE(q.size(), before_sz);
        QCOMPARE(q.free(), before_free);
        QVERIFY(q.empty());
        QVERIFY(q.try_front() == nullptr);
    }

    // Empty-write contract: claim_write(cap) must cover all slots and must not mutate state.
    {
        const reg before_sz   = q.size();
        const reg before_free = q.free();

        auto wr0 = q.claim_write(::spsc::unsafe, q.capacity());
        QCOMPARE(wr0.total, q.capacity());
        QCOMPARE(static_cast<reg>(region_count(wr0.first) + region_count(wr0.second)), q.capacity());

        // Contiguous write_size() must match the first region length (physical ring layout).
        QCOMPARE(region_count(wr0.first), q.write_size());

        QCOMPARE(q.size(), before_sz);
        QCOMPARE(q.free(), before_free);
        QVERIFY(q.empty());
        QVERIFY(q.try_front() == nullptr);
    }

    // Churn: move head/tail to physical index == (cap - 1) while staying empty.
    // This must not assume initial head/tail are zero.
    if (q.capacity() >= 4u) {
        const reg cap  = q.capacity();
        const reg mask = static_cast<reg>(cap - 1u);

        // On empty queue: claim_write(cap) returns a split that reveals current head index.
        // n1 == contiguous slots to end == cap - head_ix.
        const auto wr_seed = q.claim_write(::spsc::unsafe, cap);
        QCOMPARE(wr_seed.total, cap);

        const reg n1_seed = region_count(wr_seed.first);
        const reg head_ix = static_cast<reg>((cap - n1_seed) & mask);

        // Steps needed to reach index (cap - 1) from head_ix (mod cap).
        const reg target_ix = mask;
        const reg steps = static_cast<reg>((target_ix - head_ix) & mask);

        std::mt19937 rng(1u);
        for (reg i = 0; i < steps; ++i) {
            QVERIFY(q.try_push(make_blob(static_cast<std::uint32_t>(i + 1u), rng)));
            QVERIFY(q.try_pop());
        }

        QVERIFY(q.empty());
        QCOMPARE(q.size(), reg{0u});
        QCOMPARE(q.read_size(), reg{0u});
        QCOMPARE(q.free(), q.capacity());
        QVERIFY(q.try_front() == nullptr);

        verify_invariants(q, "alignment churn-to-boundary");
    }

    // Deterministic wrap layout on empty queue at boundary:
    // head_ix == cap - 1 => claim_write(cap) splits 1 + (cap - 1).
    if (q.capacity() >= 4u) {
        const reg cap = q.capacity();
        auto wr = q.claim_write(::spsc::unsafe, cap);
        QCOMPARE(wr.total, cap);

        const reg n1 = region_count(wr.first);
        const reg n2 = region_count(wr.second);

        QCOMPARE(static_cast<reg>(n1 + n2), cap);
        QCOMPARE(n1, reg{1u});
        QCOMPARE(n2, static_cast<reg>(cap - 1u));

        // write_size() should equal the contiguous chunk to end (== 1 here).
        QCOMPARE(q.write_size(), reg{1u});
        QCOMPARE(q.write_size(), n1);
    }

    // Non-required scenario: if U does not fit into slot, claim_as must fail anyway.
    // Return early to avoid depending on claim_as behavior when it cannot possibly succeed.
    if (!require_claim_as_success) {
        if (sizeof(U) > static_cast<std::size_t>(q.buffer_size())) {
            return;
        }
    }

    auto* p = q.template claim_as<U>();

    if (require_claim_as_success) {
        QVERIFY(p != nullptr);
        QVERIFY((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) == 0u);

        // Write + publish exactly one.
        p->x = 0x1122334455667788ull;
        p->y = 0x99AABBCCDDEEFF00ull;
        q.publish();

        QVERIFY(!q.empty());
        QCOMPARE(q.size(), reg{1u});
        QCOMPARE(q.free(), static_cast<reg>(q.capacity() - 1u));
        QVERIFY(q.try_front() != nullptr);

        // Bulk read of 1 must work and must not split.
        {
            auto rd = q.claim_read(::spsc::unsafe, 1u);
            QCOMPARE(rd.total, reg{1u});
            QCOMPARE(region_count(rd.first), reg{1u});
            QCOMPARE(region_count(rd.second), reg{0u});
        }

        // Typed front view must succeed.
        auto* f = q.template front_as<U>();
        QVERIFY(f != nullptr);
        QCOMPARE(f->x, 0x1122334455667788ull);
        QCOMPARE(f->y, 0x99AABBCCDDEEFF00ull);

        q.pop();
        QVERIFY(q.empty());
        QCOMPARE(q.size(), reg{0u});
        QCOMPARE(q.free(), q.capacity());
        QVERIFY(q.try_front() == nullptr);

        verify_invariants(q, "alignment required-path done");
        return;
    }

    // Non-required: accept both outcomes (aligned or not), but stay safe.
    if (p != nullptr) {
        QVERIFY((reinterpret_cast<std::uintptr_t>(p) % alignof(U)) == 0u);

        p->x = 0xA5A5A5A5A5A5A5A5ull;
        p->y = 0x5A5A5A5A5A5A5A5Aull;
        q.publish();

        QVERIFY(!q.empty());
        QCOMPARE(q.size(), reg{1u});

        const auto& cq = static_cast<const Q&>(q);
        auto* f = safe_try_front_as<Q, U>(cq);
        QVERIFY(f != nullptr);

        QCOMPARE(f->x, 0xA5A5A5A5A5A5A5A5ull);
        QCOMPARE(f->y, 0x5A5A5A5A5A5A5A5Aull);

        q.pop();
        QVERIFY(q.empty());
        QCOMPARE(q.size(), reg{0u});
        verify_invariants(q, "alignment nonrequired-path done");
    }
}




static inline std::uint64_t splitmix64_next(std::uint64_t& x) noexcept {
    x += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = x;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static inline Blob make_seq_blob(reg seq) noexcept {
    Blob b{};
    b.seq = static_cast<std::uint32_t>(seq);
    b.inv = static_cast<std::uint32_t>(~static_cast<std::uint32_t>(seq));

    std::uint64_t x = 0xD1B54A32D192ED03ull ^ static_cast<std::uint64_t>(seq);
    b.salt = splitmix64_next(x);

    for (auto& e : b.payload) {
        e = static_cast<std::byte>(splitmix64_next(x) & 0xFFu);
    }
    return b;
}

static inline bool blob_matches_seq(const Blob& got, reg seq) noexcept {
    const Blob exp = make_seq_blob(seq);
    return (std::memcmp(&got, &exp, sizeof(Blob)) == 0);
}

static inline void backoff_step(std::uint32_t& spins) noexcept {
    ++spins;
    if ((spins & 0xFFu) == 0u) {
        std::this_thread::yield();
    }
}

template <class Q>
static void test_two_thread_spsc(Q& q, const reg iters = kThreadIters, const int timeout_ms = kThreadTimeoutMs) {
    // This is a real 2-thread SPSC stress: producer writes slots, consumer reads slots.
    // It is meant for atomic-backed policies only.

    std::atomic<bool> abort{false};
    std::atomic<int> fail_code{0};
    std::atomic<reg> fail_seq{0u};
    std::atomic<reg> produced{0u};
    std::atomic<reg> consumed{0u};
    std::atomic<bool> prod_done{false};
    std::atomic<bool> cons_done{false};

    auto record_fail = [&](int code, reg seq) noexcept {
        int expected = 0;
        if (fail_code.compare_exchange_strong(expected, code)) {
            fail_seq.store(seq, std::memory_order_relaxed);
        }
        abort.store(true, std::memory_order_relaxed);
    };

    std::thread producer([&] {
        std::uint32_t spins = 0u;
        std::uint32_t prng = 0x1234567u;
        reg seq = 1u;

        while (!abort.load(std::memory_order_relaxed) && seq <= iters) {
            // xorshift32-ish
            prng ^= prng << 13;
            prng ^= prng >> 17;
            prng ^= prng << 5;

            const bool burst = ((prng & 0x7Fu) == 0u);
            if (burst) {
                const reg want = std::min<reg>(4u, static_cast<reg>(iters - seq + 1u));
                auto wr = q.claim_write(::spsc::unsafe, want);
                if (wr.total == 0u) {
                    backoff_step(spins);
                    continue;
                }

                reg cur = seq;

                auto write_region = [&](const auto& r) {
                    const reg n = region_count(r);
                    if (n == 0u) { return; }
                    auto slot_ptrs = region_ptr(r);
                    for (reg i = 0u; i < n; ++i) {
                        const Blob b = make_seq_blob(cur++);
                        store_blob_to_slot(slot_ptrs[i], b);
                    }
                };

                write_region(wr.first);
                write_region(wr.second);

                if (!q.try_publish(wr.total)) {
                    record_fail(1, seq);
                    break;
                }
                produced.fetch_add(wr.total, std::memory_order_relaxed);
                seq += wr.total;
                spins = 0u;
                continue;
            }

            void* p = q.try_claim();
            if (p == nullptr) {
                backoff_step(spins);
                continue;
            }

            const Blob b = make_seq_blob(seq);
            store_blob_to_slot(p, b);

            if (!q.try_publish()) {
                record_fail(2, seq);
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
        std::uint32_t prng = 0x89ABCDEFu;
        reg expected = 1u;

        while (!abort.load(std::memory_order_relaxed) && expected <= iters) {
            prng ^= prng << 13;
            prng ^= prng >> 17;
            prng ^= prng << 5;

            const bool burst = ((prng & 0x3Fu) == 0u);
            if (burst) {
                const reg want = std::min<reg>(4u, static_cast<reg>(iters - expected + 1u));
                auto rd = q.claim_read(::spsc::unsafe, want);
                if (rd.total == 0u) {
                    backoff_step(spins);
                    continue;
                }

                reg cur = expected;

                auto read_region = [&](const auto& r) -> bool {
                    const reg n = region_count(r);
                    if (n == 0u) { return true; }
                    auto slot_ptrs = region_ptr(r);
                    for (reg i = 0u; i < n; ++i) {
                        Blob got{};
                        load_blob_from_slot(slot_ptrs[i], got);
                        if (!blob_matches_seq(got, cur)) {
                            record_fail(3, cur);
                            return false;
                        }
                        ++cur;
                    }
                    return true;
                };

                if (!read_region(rd.first) || !read_region(rd.second)) {
                    cons_done.store(true, std::memory_order_relaxed);
                    return;
                }

                if (!q.try_pop(rd.total)) {
                    record_fail(4, expected);
                    break;
                }
                consumed.fetch_add(rd.total, std::memory_order_relaxed);
                expected += rd.total;
                spins = 0u;
                continue;
            }

            const void* f = q.try_front();
            if (f == nullptr) {
                backoff_step(spins);
                continue;
            }

            Blob got{};
            load_blob_from_slot(f, got);
            if (!blob_matches_seq(got, expected)) {
                record_fail(5, expected);
                break;
            }

            if (!q.try_pop()) {
                record_fail(6, expected);
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
        const reg seq = fail_seq.load(std::memory_order_relaxed);
        const QString msg = QString("2-thread SPSC failed: code=%1 seq=%2 produced=%3 consumed=%4 size=%5")
                                .arg(code)
                                .arg(seq)
                                .arg(produced.load())
                                .arg(consumed.load())
                                .arg(q.size());
        QVERIFY2(false, qPrintable(msg));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    QVERIFY2(prod_done.load(std::memory_order_relaxed) && cons_done.load(std::memory_order_relaxed),
             qPrintable(QString("2-thread SPSC timeout after %1 ms (produced=%2 consumed=%3 size=%4)")
                            .arg(elapsed.count())
                            .arg(produced.load())
                            .arg(consumed.load())
                            .arg(q.size())));

    QCOMPARE(produced.load(std::memory_order_relaxed), iters);
    QCOMPARE(consumed.load(std::memory_order_relaxed), iters);
    QVERIFY(q.empty());
    verify_invariants(q);
}

static void dynamic_capacity_sweep_suite() {
    using Q = spsc::pool<0u, spsc::policy::P>;

    constexpr reg depths[] = {1u, 2u, 3u, 7u, 8u, 9u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u, 65u};
    constexpr reg buf_sizes[] = {1u, 2u, 3u, 8u, 16u, 64u, 127u};

    std::mt19937 rng(0xD00Du);

    for (reg d : depths) {
        for (reg bs : buf_sizes) {
            Q q;
            QVERIFY(q.resize(d, bs));
            QVERIFY(q.is_valid());
            QVERIFY(is_pow2(q.capacity()));
            QVERIFY(q.capacity() >= std::max<reg>(2u, d));
            QVERIFY(q.buffer_size() >= bs);
            verify_invariants(q);

            const reg n = std::min<reg>(q.capacity(), 5u);
            if (q.buffer_size() >= static_cast<reg>(sizeof(Blob))) {
                for (reg i = 0; i < n; ++i) {
                    QVERIFY(q.try_push(make_blob(0x5000u + static_cast<std::uint32_t>(i), rng)));
                }
                QCOMPARE(q.size(), n);

                for (reg i = 0; i < n; ++i) {
                    Blob b{};
                    load_blob_from_slot(q.front(), b);
                    QCOMPARE(b.seq, 0x5000u + static_cast<std::uint32_t>(i));
                    q.pop();
                }
                QVERIFY(q.empty());
            } else {
                // Blob does not fit: typed try_push(Blob) must fail without mutating state.
                for (reg i = 0; i < n; ++i) {
                    QVERIFY(!q.try_push(make_blob(0x5000u + static_cast<std::uint32_t>(i), rng)));
                }
                QCOMPARE(q.size(), reg{0u});

                // But small payloads must still work.
                for (reg i = 0; i < n; ++i) {
                    QVERIFY(q.try_push(static_cast<std::uint8_t>(i + 1u)));
                }
                QCOMPARE(q.size(), n);

                for (reg i = 0; i < n; ++i) {
                    std::uint8_t v = 0u;
                    QVERIFY(q.try_peek(v));
                    QCOMPARE(v, static_cast<std::uint8_t>(i + 1u));
                    q.pop();
                }

                // Raw-byte API accepts bigger source and truncates.
                const Blob b = make_blob(0x5A5Au, rng);
                QVERIFY(q.try_push(&b, static_cast<reg>(sizeof(Blob))));
                QVERIFY(!q.empty());
                q.pop();
                QVERIFY(q.empty());
            }
            verify_invariants(q);
        }
    }
}

static void death_tests_debug_only_suite() {
#if !defined(NDEBUG)
    auto expect_death = [&](const char* mode) {
        QProcess p;
        p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments(QStringList{});

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("SPSC_POOL_DEATH", QString::fromLatin1(mode));
        p.setProcessEnvironment(env);

        p.start();
        QVERIFY2(p.waitForStarted(1500), "Death child failed to start.");

        if (!p.waitForFinished(8000)) {
            p.kill();
            QVERIFY2(false, "Death child did not finish (possible crash dialog).");
        }

        const int code = p.exitCode();
        QVERIFY2(code == spsc_pool_death_detail::kDeathExitCode,
                 "Expected assertion death (SIGABRT -> kDeathExitCode).");
    };

    expect_death("pop_empty");
    expect_death("front_empty");
    expect_death("publish_full");
    expect_death("claim_full");
    expect_death("bulk_arm_publish_unwritten");
    expect_death("write_guard_arm_without_slot");
    expect_death("bulk_get_next_without_claim");
    expect_death("bulk_write_next_null_nonzero");
    expect_death("consume_foreign_snapshot");
    expect_death("pop_n_too_many");
    expect_death("publish_n_too_many");
#else
    QSKIP("Death tests are debug-only (assertions disabled).");
#endif
}

} // namespace

// NOTE: Some projects declare this class in pool_test.h; others don't.
// Keeping it here matches the self-contained style.

class tst_pool_api_paranoid final : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { api_compile_smoke_all(); }

    void static_plain_P();
    void static_volatile_V();
    void static_atomic_A();
    void static_cached_CA();

    void dynamic_plain_P();
    void dynamic_volatile_V();
    void dynamic_atomic_A();
    void dynamic_cached_CA();

    void threaded_atomic_A();
    void threaded_cached_CA();

    void dynamic_capacity_sweep();
    void death_tests_debug_only();

    void alignment_default_alloc();
    void alignment_align_alloc_64();
    void alignment_align_alloc_128();

    void alignment_align_alloc_256();
    void alignment_align_alloc_32();
};

// ------------------------------ Static pools ------------------------------

void tst_pool_api_paranoid::static_plain_P() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::P>;

    test_ctor_contracts<Q>();

    // ctor(buffer_size)
    {
        Q q(reg{kBufSz});
        QVERIFY(q.is_valid());

        test_api_coverage_smoke(q);

        fill_and_drain_basic(q);
        test_claim_publish_path(q);
        test_raw_push_truncation(q);
        test_iteration_and_indexing(q);
        test_snapshot_contracts(q);
        test_zero_count_contracts(q);
        test_typed_view_contracts(q);
        test_bulk_limit_contracts(q);
        test_raii_guards(q);
        test_bulk_raii_overloads(q);
        test_guard_move_semantics(q);
        fuzz_ops(q);
    }

    // default ctor + resize()
    {
        Q q;
        QVERIFY(!q.is_valid());
        ensure_valid(q);
        fill_and_drain_basic(q);
        fuzz_ops(q);
    }

    test_wraparound_bulk_regions<Q>();
    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::static_volatile_V() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::V>;

    test_ctor_contracts<Q>();

    Q q(reg{kBufSz});
    QVERIFY(q.is_valid());

    test_api_coverage_smoke(q);

    fill_and_drain_basic(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);
    test_wraparound_bulk_regions<Q>();
    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::static_atomic_A() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::A<>>;

    test_ctor_contracts<Q>();

    Q q(reg{kBufSz});
    QVERIFY(q.is_valid());

    test_api_coverage_smoke(q);

    fill_and_drain_basic(q);
    test_claim_publish_path(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);

    test_shadow_swap_regression<Q>();
    test_wraparound_bulk_regions<Q>();
    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::static_cached_CA() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::CA<>>;

    test_ctor_contracts<Q>();

    Q q(reg{kBufSz});
    QVERIFY(q.is_valid());

    test_api_coverage_smoke(q);

    fill_and_drain_basic(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);

    test_shadow_swap_regression<Q>();
    test_wraparound_bulk_regions<Q>();
    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

// ------------------------------ Dynamic pools ------------------------------

void tst_pool_api_paranoid::dynamic_plain_P() {
    using Q = ::spsc::pool<0u, ::spsc::policy::P>;

    test_ctor_contracts<Q>();

    Q q;
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    ensure_valid(q);

    test_api_coverage_smoke(q);
    fill_and_drain_basic(q);
    test_claim_publish_path(q);
    test_raw_push_truncation(q);
    test_iteration_and_indexing(q);
    test_snapshot_contracts(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);

    test_shadow_swap_regression<Q>();
    test_wraparound_bulk_regions<Q>();

    // destroy()
    q.destroy();
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::dynamic_volatile_V() {
    using Q = ::spsc::pool<0u, ::spsc::policy::V>;

    test_ctor_contracts<Q>();

    Q q;
    ensure_valid(q);

    test_api_coverage_smoke(q);

    fill_and_drain_basic(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);

    test_wraparound_bulk_regions<Q>();

    q.destroy();
    QVERIFY(!q.is_valid());

    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::dynamic_atomic_A() {
    using Q = ::spsc::pool<0u, ::spsc::policy::A<>>;

    test_ctor_contracts<Q>();

    Q q;
    ensure_valid(q);

    test_api_coverage_smoke(q);

    fill_and_drain_basic(q);
    test_claim_publish_path(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);

    test_shadow_swap_regression<Q>();
    test_wraparound_bulk_regions<Q>();

    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::dynamic_cached_CA() {
    using Q = ::spsc::pool<0u, ::spsc::policy::CA<>>;

    test_ctor_contracts<Q>();

    Q q;
    ensure_valid(q);

    test_api_coverage_smoke(q);

    fill_and_drain_basic(q);
    test_zero_count_contracts(q);
    test_typed_view_contracts(q);
    test_bulk_limit_contracts(q);
    test_raii_guards(q);
    test_bulk_raii_overloads(q);
    test_guard_move_semantics(q);
    fuzz_ops(q);

    test_shadow_swap_regression<Q>();
    test_wraparound_bulk_regions<Q>();

    test_copy_move_semantics<Q>();
    test_resize_semantics<Q>();
}

void tst_pool_api_paranoid::threaded_atomic_A() {
    using Qs = ::spsc::pool<kDepth, ::spsc::policy::A<>>;
    {
        Qs q;
        ensure_valid(q);
        test_two_thread_spsc(q);
        verify_invariants(q, "threaded atomic (static)");
    }

    using Qd = ::spsc::pool<0u, ::spsc::policy::A<>>;
    {
        Qd q;
        ensure_valid(q);
        test_two_thread_spsc(q);
        verify_invariants(q, "threaded atomic (dynamic)");
    }
}

void tst_pool_api_paranoid::threaded_cached_CA() {
    using Qs = ::spsc::pool<kDepth, ::spsc::policy::CA<>>;
    {
        Qs q;
        ensure_valid(q);
        test_two_thread_spsc(q);
        verify_invariants(q, "threaded cached (static)");
    }

    using Qd = ::spsc::pool<0u, ::spsc::policy::CA<>>;
    {
        Qd q;
        ensure_valid(q);
        test_two_thread_spsc(q);
        verify_invariants(q, "threaded cached (dynamic)");
    }
}

void tst_pool_api_paranoid::dynamic_capacity_sweep() {
    dynamic_capacity_sweep_suite();
}

void tst_pool_api_paranoid::death_tests_debug_only() {
    death_tests_debug_only_suite();
}

// ------------------------------ Alignment suites ------------------------------

void tst_pool_api_paranoid::alignment_default_alloc() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::P>;
    test_alignment_behavior<Q, 64u>(false);
    test_alignment_behavior<Q, 128u>(false);

    // Intentionally misaligned allocator: claim_as<alignas(64)> must fail.
    using QMis = ::spsc::pool<kDepth, ::spsc::policy::P, misalign_byte_alloc<std::byte>>;
    {
        QMis q;
        ensure_valid(q);

        using A64 = AlignedWord<64u>;

        auto* p = q.template claim_as<A64>();
        QVERIFY(p == nullptr);

        auto* p2 = q.template claim_as<AlignedWord<128u>>();
        QVERIFY(p2 == nullptr);

        // Raw byte API must still work.
        std::mt19937 rng(11u);
        Blob b = make_blob(0x1111u, rng);
        QVERIFY(q.try_push(b));
        Blob got{};
        load_blob_from_slot(q.front(), got);
        expect_blob_eq(got, b);
        q.pop();
        QVERIFY(q.empty());
    }
}

void tst_pool_api_paranoid::alignment_align_alloc_64() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::P, ::spsc::alloc::align_alloc<64u>>;
    test_alignment_behavior<Q, 64u>(true);
    test_alignment_behavior<Q, 32u>(true);
}

void tst_pool_api_paranoid::alignment_align_alloc_128() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::P, ::spsc::alloc::align_alloc<128u>>;
    test_alignment_behavior<Q, 128u>(true);

    // Stronger alignment implies weaker ones.
    test_alignment_behavior<Q, 64u>(true);
}

void tst_pool_api_paranoid::alignment_align_alloc_256() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::P, ::spsc::alloc::align_alloc<256u>>;
    test_alignment_behavior<Q, 256u>(true);

    // Stronger alignment implies weaker ones.
    test_alignment_behavior<Q, 128u>(true);
}
void tst_pool_api_paranoid::alignment_align_alloc_32() {
    using Q = ::spsc::pool<kDepth, ::spsc::policy::P, ::spsc::alloc::align_alloc<32u>>;
    test_alignment_behavior<Q, 32u>(true);
}
// ------------------------------ Test runner ------------------------------

void run_tst_pool_api_paranoid(bool verbose) {
    tst_pool_api_paranoid tc;

    if (verbose) {
        QTest::qExec(&tc, {" ", "-vs"});
    } else {
        QTest::qExec(&tc);
    }
}

#include "pool_test.moc"
