/*
 * pool_view_test.cpp
 *
 * Paranoid API/contract test for spsc::pool_view.
 *
 * Goals:
 *  - Exercise the public API surface of pool_view.hpp (static + dynamic variants).
 *  - Cover policies (P/V/A/CA) across static and dynamic variants.
 *  - Validate invariants aggressively after each operation.
 *  - Stress bulk regions, including wrap-around (split first + second).
 *  - Probe alignment behavior for aligned and deliberately misaligned backing storage.
 *  - Include a real 2-thread SPSC stress for atomic policies.
 *  - Add black-box regression checks for shadow-cache staleness across swap/move/adopt.
 */

#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "pool_view.hpp"

namespace {

#if defined(NDEBUG)
static constexpr int kFuzzIters = 45'000;
static constexpr reg kThreadIters = 250'000u;
static constexpr int kThreadTimeoutMs = 6'000;
#else
static constexpr int kFuzzIters = 400;
static constexpr reg kThreadIters = 40'000u;
static constexpr int kThreadTimeoutMs = 8'000;
#endif

static constexpr reg kBufSz = 64u;

static constexpr bool is_pow2(reg x) noexcept {
    return x && ((x & (x - 1u)) == 0u);
}

template <class... Ts>
struct type_pack final {};

struct Blob final {
    std::uint32_t seq{0};
    std::uint32_t inv{0};
    std::uint64_t salt{0};
    std::array<std::byte, 44> payload{};
};
static_assert(std::is_trivially_copyable_v<Blob>);
static_assert(sizeof(Blob) <= static_cast<std::size_t>(kBufSz));

struct Big final {
    std::array<std::byte, 128> payload{};
};
static_assert(std::is_trivially_copyable_v<Big>);
static_assert(sizeof(Big) > static_cast<std::size_t>(kBufSz));

struct Align16 final {
    alignas(16) std::array<std::byte, 16> x{};
};
static_assert(std::is_trivially_copyable_v<Align16>);

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

static inline void store_to_slot(void* p, const void* src, std::size_t n) noexcept {
    std::memcpy(p, src, n);
}

static inline void load_from_slot(const void* p, void* dst, std::size_t n) noexcept {
    std::memcpy(dst, p, n);
}

static inline void store_to_slot(void* p, const Blob& b) noexcept {
    store_to_slot(p, &b, sizeof(Blob));
}

static inline void load_from_slot(const void* p, Blob& b) noexcept {
    load_from_slot(p, &b, sizeof(Blob));
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

// ------------------------------ storage builders ------------------------------

template <reg Cap, class ByteAlloc = std::allocator<std::byte>>
struct static_storage {
    using byte_vec = std::vector<std::byte, ByteAlloc>;

    reg buf_sz{0u};
    byte_vec bytes{};
    std::array<void*, Cap> slot_table{};

    void init(reg buffer_size) {
        buf_sz = buffer_size;
        bytes.assign(static_cast<std::size_t>(Cap * buffer_size), std::byte{0});
        auto* base = bytes.data();
        for (reg i = 0u; i < Cap; ++i) {
            slot_table[static_cast<std::size_t>(i)] = static_cast<void*>(
                base + static_cast<std::size_t>(i * buffer_size));
        }
    }
};

template <class ByteAlloc = std::allocator<std::byte>>
struct dynamic_storage {
    using byte_vec = std::vector<std::byte, ByteAlloc>;

    reg depth{0u};
    reg buf_sz{0u};
    byte_vec bytes{};
    std::vector<void*> slot_table{};

    void init(reg d, reg buffer_size) {
        depth = d;
        buf_sz = buffer_size;

        bytes.assign(static_cast<std::size_t>(d * buffer_size), std::byte{0});
        slot_table.assign(static_cast<std::size_t>(d), nullptr);

        auto* base = bytes.data();
        for (reg i = 0u; i < d; ++i) {
            slot_table[static_cast<std::size_t>(i)] = static_cast<void*>(
                base + static_cast<std::size_t>(i * buffer_size));
        }
    }
};

// ------------------------------ invariants ------------------------------

template <class Q>
static void verify_invariants(const Q& q) {
    if (!q.is_valid()) {
        QCOMPARE(q.capacity(), reg{0u});
        QCOMPARE(q.size(), reg{0u});
        QCOMPARE(q.free(), reg{0u});
        QVERIFY(q.empty());
        QVERIFY(q.full());

        QCOMPARE(q.buffer_size(), reg{0u});
        QVERIFY(!q.can_write(reg{1u}));
        QVERIFY(!q.can_read(reg{1u}));
        QCOMPARE(q.write_size(), reg{0u});
        QCOMPARE(q.read_size(), reg{0u});

        QVERIFY(q.data() == nullptr);

        const auto st = q.state();
        QCOMPARE(st.head, reg{0u});
        QCOMPARE(st.tail, reg{0u});
        return;
    }

    const reg cap = q.capacity();
    QVERIFY(cap >= reg{2u});
    QVERIFY(is_pow2(cap));
    QVERIFY(cap <= ::spsc::cap::RB_MAX_UNAMBIGUOUS);

    const reg sz = q.size();
    const reg fr = q.free();
    QVERIFY(sz <= cap);
    QVERIFY(fr <= cap);
    QCOMPARE(static_cast<reg>(sz + fr), cap);

    QCOMPARE(q.empty(), (sz == 0u));
    QCOMPARE(q.full(), (sz == cap));

    QCOMPARE(q.can_write(reg{1u}), (fr >= 1u));
    QCOMPARE(q.can_read(reg{1u}), (sz >= 1u));

    QVERIFY(q.write_size() <= fr);
    QVERIFY(q.write_size() <= cap);
    QVERIFY(q.read_size() <= sz);
    QVERIFY(q.read_size() <= cap);

    QVERIFY(q.buffer_size() != 0u);
    QVERIFY(q.data() != nullptr);

    const auto st = q.state();
    QCOMPARE(static_cast<reg>(st.head - st.tail), sz);
}

// ------------------------------ API compile smoke ------------------------------

template <class Q>
static void api_compile_smoke() {
    static_assert(!std::is_copy_constructible_v<Q>, "pool_view must not be copy-constructible");
    static_assert(!std::is_copy_assignable_v<Q>, "pool_view must not be copy-assignable");
    static_assert(std::is_move_constructible_v<Q>, "pool_view must be move-constructible");
    static_assert(std::is_move_assignable_v<Q>, "pool_view must be move-assignable");

    using state_t          = typename Q::state_t;
    using snapshot_t       = typename Q::snapshot;
    using const_snapshot_t = typename Q::const_snapshot;
    using regions_t        = typename Q::regions;
    using write_guard_t    = typename Q::write_guard;
    using read_guard_t     = typename Q::read_guard;

    using smoke_pack = type_pack<
        decltype(std::declval<Q&>().is_valid()),
        decltype(std::declval<Q&>().state()),
        decltype(std::declval<Q&>().capacity()),
        decltype(std::declval<Q&>().size()),
        decltype(std::declval<Q&>().free()),
        decltype(std::declval<Q&>().empty()),
        decltype(std::declval<Q&>().full()),
        decltype(std::declval<Q&>().buffer_size()),
        decltype(std::declval<Q&>().can_write(reg{1u})),
        decltype(std::declval<Q&>().can_read(reg{1u})),
        decltype(std::declval<Q&>().write_size()),
        decltype(std::declval<Q&>().read_size()),
        decltype(std::declval<Q&>().clear()),
        decltype(std::declval<Q&>().reset()),
        decltype(std::declval<Q&>().detach()),
        decltype(std::declval<Q&>().data()),
        decltype(std::declval<const Q&>().data()),
        decltype(std::declval<Q&>().begin()),
        decltype(std::declval<Q&>().end()),
        decltype(std::declval<Q&>().rbegin()),
        decltype(std::declval<Q&>().rend()),
        decltype(std::declval<const Q&>().cbegin()),
        decltype(std::declval<const Q&>().cend()),
        decltype(std::declval<const Q&>().crbegin()),
        decltype(std::declval<const Q&>().crend()),
        decltype(std::declval<Q&>().make_snapshot()),
        decltype(std::declval<const Q&>().make_snapshot()),
        decltype(std::declval<Q&>().consume(std::declval<snapshot_t>())),
        decltype(std::declval<Q&>().try_consume(std::declval<snapshot_t>())),
        decltype(std::declval<Q&>().consume_all()),
        decltype(std::declval<Q&>().claim_write(reg{1u})),
        decltype(std::declval<Q&>().claim_read(reg{1u})),
        decltype(std::declval<Q&>().try_claim()),
        decltype(std::declval<Q&>().claim()),
        decltype(std::declval<Q&>().try_publish()),
        decltype(std::declval<Q&>().publish()),
        decltype(std::declval<Q&>().try_publish(reg{1u})),
        decltype(std::declval<Q&>().publish(reg{1u})),
        decltype(std::declval<Q&>().try_front()),
        decltype(std::declval<const Q&>().try_front()),
        decltype(std::declval<Q&>().front()),
        decltype(std::declval<const Q&>().front()),
        decltype(std::declval<Q&>().try_pop()),
        decltype(std::declval<Q&>().pop()),
        decltype(std::declval<Q&>().try_pop(reg{1u})),
        decltype(std::declval<Q&>().pop(reg{1u})),
        decltype(std::declval<Q&>()[reg{0u}]),
        decltype(std::declval<const Q&>()[reg{0u}]),
        decltype(std::declval<Q&>().scoped_write()),
        decltype(std::declval<Q&>().scoped_read())
        >;

    [[maybe_unused]] smoke_pack smoke{};
    [[maybe_unused]] state_t st{};
    [[maybe_unused]] snapshot_t s1{};
    [[maybe_unused]] const_snapshot_t s2{};
    [[maybe_unused]] regions_t r{};
    [[maybe_unused]] write_guard_t wg{};
    [[maybe_unused]] read_guard_t rg{};

    (void)smoke; (void)st; (void)s1; (void)s2; (void)r; (void)wg; (void)rg;
}

// ------------------------------ state helpers ------------------------------

template <class Q>
static void must_empty(Q& q) {
    while (!q.empty()) {
        QVERIFY(q.try_pop());
    }
    verify_invariants(q);
    QVERIFY(q.empty());
}

// Force head/tail in a non-concurrent way.

template <reg Cap, class Policy>
static void force_state(spsc::pool_view<Cap, Policy>& q,
                        void** slot_table,
                        reg buf_sz,
                        reg head,
                        reg tail)
{
    QVERIFY(q.adopt(slot_table, buf_sz, head, tail));
    QVERIFY(q.is_valid());
    verify_invariants(q);
}

template <class Policy>
static void force_state(spsc::pool_view<0, Policy>& q,
                        void** slot_table,
                        reg depth,
                        reg buf_sz,
                        reg head,
                        reg tail)
{
    QVERIFY(q.adopt(slot_table, depth, buf_sz, head, tail));
    QVERIFY(q.is_valid());
    verify_invariants(q);
}

// ------------------------------ tests: attachment/move/swap ------------------------------

template <class Q>
static void test_static_attachment_state_move_swap() {
    using Policy = typename Q::policy_type;

    static_storage<16> st1;
    st1.init(kBufSz);

    // Default: invalid
    Q q0;
    QVERIFY(!q0.is_valid());
    verify_invariants(q0);

    // Constructor via std::array
    Q q1(st1.slot_table, kBufSz);
    QVERIFY(q1.is_valid());
    verify_invariants(q1);

    // Constructor via pointer + Policy tag
    Q q2(st1.slot_table.data(), kBufSz, Policy{});
    QVERIFY(q2.is_valid());
    verify_invariants(q2);

    // Detach
    q2.detach();
    QVERIFY(!q2.is_valid());
    verify_invariants(q2);

    // Attach back
    QVERIFY(q2.attach(st1.slot_table, kBufSz));
    QVERIFY(q2.is_valid());
    verify_invariants(q2);

    // State + attach(sugar)
    q2.reset();
    QVERIFY(q2.try_push(Blob{}));
    const auto st_saved = q2.state();
    QVERIFY(st_saved.head != st_saved.tail);

    Q q3;
    QVERIFY(!q3.is_valid());
    QVERIFY(q3.attach(st1.slot_table, kBufSz, st_saved));
    QVERIFY(q3.is_valid());
    QCOMPARE(q3.size(), q2.size());

    // Move
    Q q4(std::move(q3));
    QVERIFY(q4.is_valid());
    verify_invariants(q4);
    verify_invariants(q3);

    Q q5;
    q5 = std::move(q4);
    QVERIFY(q5.is_valid());
    verify_invariants(q5);
    verify_invariants(q4);

    // Swap
    static_storage<16> st2;
    st2.init(kBufSz);

    Q qa(st1.slot_table, kBufSz, Policy{});
    Q qb(st2.slot_table, kBufSz, Policy{});

    QVERIFY(qa.try_push(Blob{1u, ~1u, 0u, {}}));
    QVERIFY(qb.try_push(Blob{2u, ~2u, 0u, {}}));

    const auto* a_data = qa.data();
    const auto* b_data = qb.data();

    using std::swap;
    swap(qa, qb);

    QCOMPARE(qa.data(), b_data);
    QCOMPARE(qb.data(), a_data);

    verify_invariants(qa);
    verify_invariants(qb);

    must_empty(qa);
    must_empty(qb);
}

template <class Q>
static void test_dynamic_attachment_state_move_swap() {
    using Policy = typename Q::policy_type;

    dynamic_storage<> st1;
    st1.init(reg{64u}, kBufSz);

    // Default: invalid
    Q q0;
    QVERIFY(!q0.is_valid());
    verify_invariants(q0);

    // Constructor
    Q q1(st1.slot_table.data(), st1.depth, kBufSz);
    QVERIFY(q1.is_valid());
    verify_invariants(q1);

    // Detach + attach
    q1.detach();
    QVERIFY(!q1.is_valid());
    verify_invariants(q1);

    QVERIFY(q1.attach(st1.slot_table.data(), st1.depth, kBufSz));
    QVERIFY(q1.is_valid());
    verify_invariants(q1);

    // State + attach(sugar)
    QVERIFY(q1.try_push(Blob{}));
    const auto st_saved = q1.state();

    Q q2;
    QVERIFY(q2.attach(st1.slot_table.data(), st1.depth, kBufSz, st_saved));
    QVERIFY(q2.is_valid());
    QCOMPARE(q2.size(), q1.size());

    // Move
    Q q3(std::move(q2));
    QVERIFY(q3.is_valid());
    verify_invariants(q3);
    verify_invariants(q2);

    Q q4;
    q4 = std::move(q3);
    QVERIFY(q4.is_valid());
    verify_invariants(q4);
    verify_invariants(q3);

    // Swap (two different storages)
    dynamic_storage<> st2;
    st2.init(reg{128u}, kBufSz);

    Q qa(st1.slot_table.data(), st1.depth, kBufSz, Policy{});
    Q qb(st2.slot_table.data(), st2.depth, kBufSz, Policy{});

    QVERIFY(qa.try_push(Blob{11u, ~11u, 0u, {}}));
    QVERIFY(qb.try_push(Blob{22u, ~22u, 0u, {}}));

    const auto* a_data = qa.data();
    const auto* b_data = qb.data();
    const auto a_bs = qa.buffer_size();
    const auto b_bs = qb.buffer_size();

    using std::swap;
    swap(qa, qb);

    QCOMPARE(qa.data(), b_data);
    QCOMPARE(qb.data(), a_data);
    QCOMPARE(qa.buffer_size(), b_bs);
    QCOMPARE(qb.buffer_size(), a_bs);

    verify_invariants(qa);
    verify_invariants(qb);

    must_empty(qa);
    must_empty(qb);
}

// ------------------------------ tests: introspection/data/iterators ------------------------------

template <class Q>
static void test_introspection_and_data(Q& q) {
    QVERIFY(q.is_valid());
    verify_invariants(q);

    QVERIFY(q.buffer_size() == kBufSz);
    QVERIFY(q.data() != nullptr);

    // Iteration over pointers. Empty -> begin==end.
    QVERIFY(q.begin() == q.end());
    QVERIFY(q.cbegin() == q.cend());
    QVERIFY(q.rbegin() == q.rend());

    // Reserve a single element and then check iterators move.
    std::mt19937 rng(0xC0FFEEu);
    const Blob b = make_blob(1u, rng);
    QVERIFY(q.try_push(b));

    QVERIFY(q.begin() != q.end());
    auto it = q.begin();
    QVERIFY(it != q.end());
    QVERIFY((*it) != nullptr);

    // operator[] points to the same slot pointer as front() for i==0.
    QCOMPARE(q[0u], q.front());

    must_empty(q);
}

template <class Q>
static void test_iterator_ranges(Q& q) {
    QVERIFY(q.is_valid());
    q.reset();

    std::mt19937 rng(0xBBBBu);
    const reg n = 9u;
    for (reg i = 0u; i < n; ++i) {
        QVERIFY(q.try_push(make_blob(10'000u + static_cast<std::uint32_t>(i), rng)));
    }

    // Forward.
    {
        reg i = 0u;
        for (auto p : q) {
            QVERIFY(p != nullptr);
            Blob got{};
            load_from_slot(p, got);
            QVERIFY(got.seq == 10'000u + static_cast<std::uint32_t>(i));
            ++i;
        }
        QCOMPARE(i, n);
    }

    // Reverse.
    {
        reg i = n;
        for (auto it = q.rbegin(); it != q.rend(); ++it) {
            --i;
            auto p = *it;
            QVERIFY(p != nullptr);
            Blob got{};
            load_from_slot(p, got);
            QVERIFY(got.seq == 10'000u + static_cast<std::uint32_t>(i));
        }
        QCOMPARE(i, 0u);
    }

    q.consume_all();
    QVERIFY(q.empty());
}

template <class Q>
static void test_iterator_consistency(Q& q) {
    QVERIFY(q.is_valid());
    q.reset();

    std::mt19937 rng(0xD00Du);
    const reg n = 13u;
    for (reg i = 0u; i < n; ++i) {
        QVERIFY(q.try_push(make_blob(9'000u + static_cast<std::uint32_t>(i), rng)));
    }

    // Snapshot order must match operator[].
    auto s = q.make_snapshot();
    QCOMPARE(static_cast<reg>(s.size()), n);

    auto it = s.begin();
    for (reg i = 0u; i < n; ++i, ++it) {
        const auto p_idx  = q[i];
        const auto p_snap = *it;
        QVERIFY(p_idx == p_snap);
    }

    // begin/end count must match size().
    {
        reg count = 0u;
        for (auto p : q) {
            (void)p;
            ++count;
        }
        QCOMPARE(count, q.size());
    }

    q.consume_all();
    QVERIFY(q.empty());
}

// ------------------------------ tests: push/pop/front/claim ------------------------------

template <class Q>
static void test_push_pop_front_try(Q& q) {
    std::mt19937 rng(0x12345678u);

    // try_front on empty
    QVERIFY(q.try_front() == nullptr);

    // try_push fails when element is too large
    QVERIFY(!q.try_push(Big{}));

    // Single push/pop
    Blob b0 = make_blob(7u, rng);
    QVERIFY(q.try_push(b0));
    verify_invariants(q);

    const void* p = q.front();
    QVERIFY(p != nullptr);
    Blob got{};
    load_from_slot(p, got);
    expect_blob_eq(got, b0);

    QVERIFY(q.try_pop());
    verify_invariants(q);
    QVERIFY(q.empty());

    // claim/publish path
    void* w = q.try_claim();
    QVERIFY(w != nullptr);
    Blob b1 = make_blob(8u, rng);
    store_to_slot(w, b1);
    QVERIFY(q.try_publish());

    const void* r = q.try_front();
    QVERIFY(r != nullptr);
    Blob got1{};
    load_from_slot(r, got1);
    expect_blob_eq(got1, b1);

    q.pop();
    verify_invariants(q);
    QVERIFY(q.empty());
}

// ------------------------------ tests: bulk regions + wrap ------------------------------

template <reg Cap, class Policy>
static void test_bulk_regions(spsc::pool_view<Cap, Policy>& q, void** slots_base) {
    static_assert(Cap != 0u, "static-only overload");

    std::mt19937 rng(0xBADC0DEu);

    // claim_write on empty: should provide full free space
    {
        const auto wr = q.claim_write();
        QCOMPARE(wr.total, q.free());
        QCOMPARE(wr.total, q.capacity());
        QCOMPARE(static_cast<reg>(wr.first.count + wr.second.count), wr.total);
        QVERIFY(wr.first.count != 0u);
    }

    // Fill half via bulk publish
    {
        const reg want = std::min<reg>(q.capacity() / 2u, reg{5u});
        const auto wr = q.claim_write(want);
        QCOMPARE(wr.total, want);

        reg seq = 100u;
        for (reg i = 0u; i < wr.first.count; ++i) {
            void* dst = wr.first.ptr[static_cast<std::size_t>(i)];
            const Blob b = make_blob(static_cast<std::uint32_t>(seq++), rng);
            store_to_slot(dst, b);
        }
        for (reg i = 0u; i < wr.second.count; ++i) {
            void* dst = wr.second.ptr[static_cast<std::size_t>(i)];
            const Blob b = make_blob(static_cast<std::uint32_t>(seq++), rng);
            store_to_slot(dst, b);
        }

        QVERIFY(q.try_publish(wr.total));
        verify_invariants(q);
    }

    // Read back via bulk read and pop
    {
        const auto rd = q.claim_read();
        QCOMPARE(rd.total, q.size());
        QCOMPARE(static_cast<reg>(rd.first.count + rd.second.count), rd.total);
        QVERIFY(rd.total != 0u);

        for (reg i = 0u; i < rd.first.count; ++i) {
            QVERIFY(rd.first.ptr[static_cast<std::size_t>(i)] != nullptr);
        }
        for (reg i = 0u; i < rd.second.count; ++i) {
            QVERIFY(rd.second.ptr[static_cast<std::size_t>(i)] != nullptr);
        }

        q.pop(rd.total);
        verify_invariants(q);
        QVERIFY(q.empty());
    }

    // Force wrap on claim_write: pick head so write_index == cap-1 => w2e == 1.
    {
        const reg cap  = q.capacity();
        const reg head = static_cast<reg>(cap - 1u);
        const reg used = std::min<reg>(3u, static_cast<reg>(cap - 1u));
        const reg tail = static_cast<reg>(head - used);

        force_state(q, slots_base, kBufSz, head, tail);

        const auto wr = q.claim_write();
        QCOMPARE(wr.total, q.free());
        QVERIFY(wr.total >= 1u);

        if (q.free() >= 2u) {
            QCOMPARE(wr.first.count, reg{1u});
            QCOMPARE(static_cast<reg>(wr.second.count), static_cast<reg>(wr.total - 1u));
        }

        q.consume_all();
        verify_invariants(q);
        QVERIFY(q.empty());

        // In single-thread tests we want producer/consumer hints consistent afterwards.
        q.reset();
        verify_invariants(q);
        QVERIFY(q.empty());
    }

    // Force wrap on claim_read: pick tail so read_index == cap-1 => r2e == 1.
    {
        const reg cap  = q.capacity();
        const reg tail = static_cast<reg>(cap - 1u);
        const reg head = static_cast<reg>(tail + 5u); // used == 5

        force_state(q, slots_base, kBufSz, head, tail);

        const auto rd = q.claim_read();
        QCOMPARE(rd.total, q.size());
        QVERIFY(rd.total >= 1u);

        if (q.size() >= 2u) {
            QCOMPARE(rd.first.count, reg{1u});
            QCOMPARE(static_cast<reg>(rd.second.count), static_cast<reg>(rd.total - 1u));
        }

        q.consume_all();
        verify_invariants(q);
        QVERIFY(q.empty());

        q.reset();
        verify_invariants(q);
        QVERIFY(q.empty());
    }
}

template <class Policy>
static void test_bulk_regions(spsc::pool_view<0, Policy>& q, void** slots_base, reg depth) {
    std::mt19937 rng(0xBADC0DEu);

    {
        const auto wr = q.claim_write();
        QCOMPARE(wr.total, q.free());
        QCOMPARE(wr.total, q.capacity());
        QCOMPARE(static_cast<reg>(wr.first.count + wr.second.count), wr.total);
        QVERIFY(wr.first.count != 0u);
    }

    {
        const reg want = std::min<reg>(q.capacity() / 2u, reg{5u});
        const auto wr = q.claim_write(want);
        QCOMPARE(wr.total, want);

        reg seq = 200u;
        for (reg i = 0u; i < wr.first.count; ++i) {
            void* dst = wr.first.ptr[static_cast<std::size_t>(i)];
            const Blob b = make_blob(static_cast<std::uint32_t>(seq++), rng);
            store_to_slot(dst, b);
        }
        for (reg i = 0u; i < wr.second.count; ++i) {
            void* dst = wr.second.ptr[static_cast<std::size_t>(i)];
            const Blob b = make_blob(static_cast<std::uint32_t>(seq++), rng);
            store_to_slot(dst, b);
        }

        QVERIFY(q.try_publish(wr.total));
        verify_invariants(q);
    }

    {
        const auto rd = q.claim_read();
        QCOMPARE(rd.total, q.size());
        QCOMPARE(static_cast<reg>(rd.first.count + rd.second.count), rd.total);
        QVERIFY(rd.total != 0u);

        q.pop(rd.total);
        verify_invariants(q);
        QVERIFY(q.empty());
    }

    // Force wrap on claim_write: pick head so write_index == cap-1 => w2e == 1.
    {
        const reg cap  = q.capacity();
        const reg head = static_cast<reg>(cap - 1u);
        const reg used = std::min<reg>(3u, static_cast<reg>(cap - 1u));
        const reg tail = static_cast<reg>(head - used);

        force_state(q, slots_base, depth, kBufSz, head, tail);

        const auto wr = q.claim_write();
        QCOMPARE(wr.total, q.free());
        QVERIFY(wr.total >= 1u);

        if (q.free() >= 2u) {
            QCOMPARE(wr.first.count, reg{1u});
            QCOMPARE(static_cast<reg>(wr.second.count), static_cast<reg>(wr.total - 1u));
        }

        q.consume_all();
        verify_invariants(q);
        QVERIFY(q.empty());

        q.reset();
        verify_invariants(q);
        QVERIFY(q.empty());
    }

    // Force wrap on claim_read: pick tail so read_index == cap-1 => r2e == 1.
    {
        const reg cap  = q.capacity();
        const reg tail = static_cast<reg>(cap - 1u);
        const reg head = static_cast<reg>(tail + 5u); // used == 5

        force_state(q, slots_base, depth, kBufSz, head, tail);

        const auto rd = q.claim_read();
        QCOMPARE(rd.total, q.size());
        QVERIFY(rd.total >= 1u);

        if (q.size() >= 2u) {
            QCOMPARE(rd.first.count, reg{1u});
            QCOMPARE(static_cast<reg>(rd.second.count), static_cast<reg>(rd.total - 1u));
        }

        q.consume_all();
        verify_invariants(q);
        QVERIFY(q.empty());

        q.reset();
        verify_invariants(q);
        QVERIFY(q.empty());
    }
}

// ------------------------------ tests: snapshots ------------------------------

template <class Q>
static void test_snapshots(Q& q) {
    std::mt19937 rng(0xA11CEu);

    // Make each suite step independent.
    q.reset();
    verify_invariants(q);
    QVERIFY(q.empty());

    // Fill 3 items
    for (std::uint32_t i = 0u; i < 3u; ++i) {
        QVERIFY(q.try_push(make_blob(200u + i, rng)));
    }
    verify_invariants(q);

    // Snapshot
    auto s = q.make_snapshot();
    QVERIFY(s.begin() != s.end());

    // try_consume should succeed (tail matches)
    QVERIFY(q.try_consume(s));
    verify_invariants(q);
    QVERIFY(q.empty());

    // Fill again, then consume_all
    for (std::uint32_t i = 0u; i < 4u; ++i) {
        QVERIFY(q.try_push(make_blob(300u + i, rng)));
    }
    verify_invariants(q);

    q.consume_all();
    verify_invariants(q);
    QVERIFY(q.empty());

    q.reset();
    verify_invariants(q);
    QVERIFY(q.empty());
}

// ------------------------------ tests: RAII guards + alignment paranoia ------------------------------

template <class Q>
static void test_raii_guards_alignment_aligned(Q& q) {
    // Make this test independent from previous ones.
    q.reset();
    verify_invariants(q);
    QVERIFY(q.empty());

    std::mt19937 rng(0x515151u);

    // Slot #1: write a Blob and publish it.
    {
        auto wg = q.scoped_write();
        QVERIFY(wg);

        Blob* b = wg.template as<Blob>();
        QVERIFY(b != nullptr);
        *b = make_blob(777u, rng);

        // Probe alignment without scribbling over the Blob (same memory).
        const Align16* a16_probe = wg.template as<Align16>();
        QVERIFY(a16_probe != nullptr);

        wg.commit();
    }

    // Slot #2: write Align16 payload.
    {
        auto wg = q.scoped_write();
        QVERIFY(wg);

        Align16* a16 = wg.template as<Align16>();
        QVERIFY(a16 != nullptr);
        a16->x.fill(std::byte{0});
        a16->x[0] = std::byte{0x42};

        wg.commit();
    }

    QCOMPARE(q.size(), reg{2u});

    // Read slot #1.
    {
        auto rg = q.scoped_read();
        QVERIFY(rg);

        const Blob* b = rg.template as<Blob>();
        QVERIFY(b != nullptr);
        QCOMPARE(b->seq, 777u);

        rg.commit();
    }

    // Read slot #2.
    {
        auto rg = q.scoped_read();
        QVERIFY(rg);

        const Align16* a16 = rg.template as<Align16>();
        QVERIFY(a16 != nullptr);
        QCOMPARE(a16->x[0], std::byte{0x42});

        rg.commit();
    }

    verify_invariants(q);
    QVERIFY(q.empty());
}

template <class Q>
static void test_raii_guards_alignment_misaligned() {
    using Policy = typename Q::policy_type;

    static_storage<16, misalign_byte_alloc<std::byte>> st;
    st.init(kBufSz);

    // Base pointer must be misaligned for typical alignments.
    QVERIFY((reinterpret_cast<std::uintptr_t>(st.bytes.data()) % 2u) == 1u);

    Q q(st.slot_table, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    std::mt19937 rng(0x616161u);

    {
        auto wg = q.scoped_write();
        QVERIFY(wg);

        // as<T>() must reject misaligned storage.
        QVERIFY(wg.template as<Blob>() == nullptr);
        QVERIFY(wg.template as<Align16>() == nullptr);

        const Blob b = make_blob(888u, rng);
        store_to_slot(wg.get(), b);
        wg.commit();
    }

    {
        auto rg = q.scoped_read();
        QVERIFY(rg);
        QVERIFY(rg.template as<Blob>() == nullptr);

        Blob got{};
        load_from_slot(rg.get(), got);
        QCOMPARE(got.seq, 888u);
        QCOMPARE(got.inv, static_cast<std::uint32_t>(~888u));
        rg.commit();
    }

    verify_invariants(q);
    QVERIFY(q.empty());
}

// ------------------------------ tests: random fuzz ------------------------------

template <class Q>
static void paranoid_random_fuzz(Q& q, std::uint32_t seed, int iters) {
    std::mt19937 rng(seed);
    std::deque<Blob> ref;

    auto do_push = [&]() {
        if (!q.can_write(1u)) {
            return;
        }
        const Blob b = make_blob(static_cast<std::uint32_t>(ref.size() + 1u), rng);

        // Mix push paths.
        const int mode = int(rng() % 3u);
        if (mode == 0) {
            QVERIFY(q.try_push(b));
        } else if (mode == 1) {
            void* p = q.try_claim();
            QVERIFY(p != nullptr);
            store_to_slot(p, b);
            QVERIFY(q.try_publish());
        } else {
            auto wr = q.claim_write(1u);
            QCOMPARE(wr.total, reg{1u});
            void* dst = wr.first.ptr[0];
            store_to_slot(dst, b);
            QVERIFY(q.try_publish(1u));
        }
        ref.push_back(b);
    };

    auto do_pop = [&]() {
        if (!q.can_read(1u)) {
            return;
        }
        const int mode = int(rng() % 3u);
        if (mode == 0) {
            const void* p = q.try_front();
            QVERIFY(p != nullptr);
            Blob got{};
            load_from_slot(p, got);
            expect_blob_eq(got, ref.front());
            QVERIFY(q.try_pop());
            ref.pop_front();
        } else if (mode == 1) {
            auto rd = q.claim_read(1u);
            QCOMPARE(rd.total, reg{1u});
            const void* p = rd.first.ptr[0];
            QVERIFY(p != nullptr);
            Blob got{};
            load_from_slot(p, got);
            expect_blob_eq(got, ref.front());
            QVERIFY(q.try_pop(1u));
            ref.pop_front();
        } else {
            auto rg = q.scoped_read();
            QVERIFY(rg);
            Blob got{};
            load_from_slot(rg.get(), got);
            expect_blob_eq(got, ref.front());
            rg.commit();
            ref.pop_front();
        }
    };

    for (int i = 0; i < iters; ++i) {
        const int op = int(rng() % 8u);
        if (op < 5) {
            do_push();
        } else {
            do_pop();
        }
        verify_invariants(q);
        QCOMPARE(q.size(), static_cast<reg>(ref.size()));
    }

    // Drain
    while (!ref.empty()) {
        do_pop();
    }

    verify_invariants(q);
    QVERIFY(q.empty());
}

// ------------------------------ 2-thread SPSC stress (atomic policies only) ------------------------------

struct SeqMsg final {
    std::uint32_t seq{0};
    std::uint32_t inv{0};
    std::uint64_t salt{0};
    std::array<std::byte, 40> payload{};
};
static_assert(std::is_trivially_copyable_v<SeqMsg>);
static_assert(sizeof(SeqMsg) <= 64u);

static inline SeqMsg make_seq_msg(reg seq) noexcept {
    auto splitmix64 = [](std::uint64_t x) noexcept {
        x += 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    };

    SeqMsg m{};
    m.seq = static_cast<std::uint32_t>(seq);
    m.inv = ~m.seq;
    const std::uint64_t s = splitmix64(static_cast<std::uint64_t>(seq));
    m.salt = s;

    for (std::size_t i = 0; i < m.payload.size(); ++i) {
        m.payload[i] = static_cast<std::byte>((s >> ((i % 8u) * 8u)) & 0xFFu);
    }

    return m;
}

static inline bool msg_matches_seq(const SeqMsg& m, reg seq) noexcept {
    if (m.seq != static_cast<std::uint32_t>(seq)) {
        return false;
    }
    if (m.inv != static_cast<std::uint32_t>(~static_cast<std::uint32_t>(seq))) {
        return false;
    }
    const SeqMsg ref = make_seq_msg(seq);
    return std::memcmp(&m, &ref, sizeof(SeqMsg)) == 0;
}

static inline void backoff_step(reg& spins) {
    ++spins;
    if (spins < 64u) {
        std::this_thread::yield();
    } else {
        std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
}

template <class Q>
static void test_two_thread_spsc_pool_view(Q& q,
                                           reg iters = kThreadIters,
                                           int timeout_ms = kThreadTimeoutMs)
{
    static_assert(std::is_same_v<typename Q::value_type, void*>);

    QVERIFY(q.is_valid());
    QVERIFY(q.buffer_size() >= static_cast<reg>(sizeof(SeqMsg)));

    std::atomic<bool> abort{false};
    std::atomic<bool> prod_done{false};
    std::atomic<bool> cons_done{false};

    std::atomic<int> fail_code{0};
    std::atomic<reg> fail_seq{0u};

    std::atomic<reg> produced{0u};
    std::atomic<reg> consumed{0u};

    auto record_fail = [&](int code, reg seq) {
        int expected = 0;
        fail_code.compare_exchange_strong(expected, code, std::memory_order_relaxed);
        fail_seq.store(seq, std::memory_order_relaxed);
        abort.store(true, std::memory_order_relaxed);
    };

    std::thread producer([&]() {
        reg seq = 0u;
        reg spins = 0u;

        while (seq < iters && !abort.load(std::memory_order_relaxed)) {
            const reg remaining = static_cast<reg>(iters - seq);
            const reg max_batch = std::min<reg>(std::min<reg>(q.free(), 64u), remaining);

            if (max_batch != 0u) {
                const auto wr = q.claim_write(max_batch);
                if (wr.total != 0u) {
                    reg s = seq;

                    for (reg i = 0u; i < wr.first.count; ++i) {
                        void* dst = wr.first.ptr[static_cast<std::size_t>(i)];
                        const SeqMsg m = make_seq_msg(s++);
                        store_to_slot(dst, &m, sizeof(SeqMsg));
                    }
                    for (reg i = 0u; i < wr.second.count; ++i) {
                        void* dst = wr.second.ptr[static_cast<std::size_t>(i)];
                        const SeqMsg m = make_seq_msg(s++);
                        store_to_slot(dst, &m, sizeof(SeqMsg));
                    }

                    if (!q.try_publish(wr.total)) {
                        record_fail(1, seq);
                        break;
                    }

                    produced.fetch_add(wr.total, std::memory_order_relaxed);
                    seq = s;
                    spins = 0u;
                    continue;
                }
            }

            void* p = q.try_claim();
            if (p == nullptr) {
                backoff_step(spins);
                continue;
            }

            const SeqMsg m = make_seq_msg(seq);
            store_to_slot(p, &m, sizeof(SeqMsg));

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

    std::thread consumer([&]() {
        reg expected = 0u;
        reg spins = 0u;

        while (expected < iters && !abort.load(std::memory_order_relaxed)) {
            const reg remaining = static_cast<reg>(iters - expected);
            const reg max_batch = std::min<reg>(std::min<reg>(q.size(), 64u), remaining);

            if (max_batch != 0u) {
                const auto rd = q.claim_read(max_batch);
                if (rd.total != 0u) {
                    reg s = expected;

                    for (reg i = 0u; i < rd.first.count; ++i) {
                        const void* src = rd.first.ptr[static_cast<std::size_t>(i)];
                        SeqMsg got{};
                        load_from_slot(src, &got, sizeof(SeqMsg));
                        if (!msg_matches_seq(got, s++)) {
                            record_fail(3, expected);
                            goto done_cons;
                        }
                    }
                    for (reg i = 0u; i < rd.second.count; ++i) {
                        const void* src = rd.second.ptr[static_cast<std::size_t>(i)];
                        SeqMsg got{};
                        load_from_slot(src, &got, sizeof(SeqMsg));
                        if (!msg_matches_seq(got, s++)) {
                            record_fail(4, expected);
                            goto done_cons;
                        }
                    }

                    if (!q.try_pop(rd.total)) {
                        record_fail(5, expected);
                        goto done_cons;
                    }

                    consumed.fetch_add(rd.total, std::memory_order_relaxed);
                    expected = s;
                    spins = 0u;
                    continue;
                }
            }

            const void* p = q.try_front();
            if (p == nullptr) {
                backoff_step(spins);
                continue;
            }

            SeqMsg got{};
            load_from_slot(p, &got, sizeof(SeqMsg));
            if (!msg_matches_seq(got, expected)) {
                record_fail(6, expected);
                break;
            }

            if (!q.try_pop()) {
                record_fail(7, expected);
                break;
            }

            consumed.fetch_add(1u, std::memory_order_relaxed);
            ++expected;
            spins = 0u;
        }

    done_cons:
        cons_done.store(true, std::memory_order_relaxed);
    });

    const auto t0 = std::chrono::steady_clock::now();
    while (!abort.load(std::memory_order_relaxed)
           && !(prod_done.load(std::memory_order_relaxed)
                && cons_done.load(std::memory_order_relaxed)))
    {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0);
        if (elapsed.count() > timeout_ms) {
            abort.store(true, std::memory_order_relaxed);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    producer.join();
    consumer.join();

    const int code = fail_code.load(std::memory_order_relaxed);
    if (code != 0) {
        const reg seq = fail_seq.load(std::memory_order_relaxed);
        const QString msg = QString("2-thread pool_view SPSC failed: code=%1 seq=%2 produced=%3 consumed=%4 size=%5")
                                .arg(code)
                                .arg(seq)
                                .arg(produced.load())
                                .arg(consumed.load())
                                .arg(q.size());
        QVERIFY2(false, qPrintable(msg));
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    QVERIFY2(prod_done.load(std::memory_order_relaxed)
                 && cons_done.load(std::memory_order_relaxed),
             qPrintable(QString("2-thread pool_view SPSC timeout after %1 ms (produced=%2 consumed=%3 size=%4)")
                            .arg(elapsed.count())
                            .arg(produced.load())
                            .arg(consumed.load())
                            .arg(q.size())));

    QCOMPARE(produced.load(std::memory_order_relaxed), iters);
    QCOMPARE(consumed.load(std::memory_order_relaxed), iters);
    QVERIFY(q.empty());
    verify_invariants(q);
}

// ------------------------------ extra paranoia (black-box) ------------------------------

template <class Q>
static void warm_producer_queries(Q& q) {
    (void)q.write_size();
    (void)q.can_write(1u);
    (void)q.free();
}

template <class Q>
static void fill_full(Q& q, std::uint32_t base_seq) {
    std::mt19937 rng(base_seq ^ 0xA5A5A5A5u);
    const reg cap = q.capacity();

    for (reg i = 0u; i < cap; ++i) {
        const Blob b = make_blob(base_seq + static_cast<std::uint32_t>(i), rng);
        QVERIFY(q.try_push(b));
    }

    verify_invariants(q);
    QVERIFY(q.full());
    QVERIFY(!q.can_write(1u));
    QCOMPARE(q.free(), reg{0u});

    // Full queue must refuse claiming.
    QVERIFY(q.try_claim() == nullptr);
    QVERIFY(!q.try_push(make_blob(base_seq + 0xFFFFu, rng)));
}

template <class Q>
static void drain_expect_seq(Q& q, std::uint32_t base_seq) {
    std::mt19937 rng(base_seq ^ 0xA5A5A5A5u);
    const reg cap = q.capacity();

    for (reg i = 0u; i < cap; ++i) {
        const void* p = q.try_front();
        QVERIFY(p != nullptr);

        Blob got{};
        load_from_slot(p, got);

        const Blob ref = make_blob(base_seq + static_cast<std::uint32_t>(i), rng);
        expect_blob_eq(got, ref);

        QVERIFY(q.try_pop());
    }

    verify_invariants(q);
    QVERIFY(q.empty());
    QVERIFY(!q.full());
}

// 1) swap/move must not leave stale shadow state that allows claim on a full queue.

template <class Policy>
static void test_shadow_swap_move_regression_static() {
    using Q = spsc::pool_view<16, Policy>;

    static_storage<16> st_a;
    static_storage<16> st_b;
    st_a.init(kBufSz);
    st_b.init(kBufSz);

    Q a(st_a.slot_table, kBufSz, Policy{});
    Q b(st_b.slot_table, kBufSz, Policy{});

    QVERIFY(a.is_valid());
    QVERIFY(b.is_valid());

    // Warm producer-side caches on 'a'.
    QVERIFY(a.empty());
    warm_producer_queries(a);

    fill_full(b, 1000u);

    // After swap: 'a' should be full and must refuse claim.
    a.swap(b);
    verify_invariants(a);
    verify_invariants(b);

    QVERIFY(a.full());
    QVERIFY(a.try_claim() == nullptr);
    QVERIFY(b.empty());

    // Move-construct: moved must remain full and refuse claim.
    Q moved(std::move(a));
    QVERIFY(moved.is_valid());
    QVERIFY(moved.full());
    QVERIFY(moved.try_claim() == nullptr);

    drain_expect_seq(moved, 1000u);
}

template <class Policy>
static void test_shadow_swap_move_regression_dynamic() {
    using Q = spsc::pool_view<0, Policy>;

    dynamic_storage<> st_a;
    dynamic_storage<> st_b;
    st_a.init(/*depth*/ 32u, kBufSz);
    st_b.init(/*depth*/ 32u, kBufSz);

    Q a(st_a.slot_table.data(), st_a.depth, kBufSz, Policy{});
    Q b(st_b.slot_table.data(), st_b.depth, kBufSz, Policy{});

    QVERIFY(a.is_valid());
    QVERIFY(b.is_valid());

    QVERIFY(a.empty());
    warm_producer_queries(a);

    fill_full(b, 2000u);

    a.swap(b);
    verify_invariants(a);
    verify_invariants(b);

    QVERIFY(a.full());
    QVERIFY(a.try_claim() == nullptr);
    QVERIFY(b.empty());

    Q moved(std::move(a));
    QVERIFY(moved.is_valid());
    QVERIFY(moved.full());
    QVERIFY(moved.try_claim() == nullptr);

    drain_expect_seq(moved, 2000u);
}

// 2) nullptr slot pointers: high-level APIs must refuse to claim/push to a null backing slot.

template <class Policy>
static void test_null_slot_pointer_defense_static() {
    using Q = spsc::pool_view<16, Policy>;

    static_storage<16> st;
    st.init(kBufSz);

    constexpr reg kNullIndex = 7u;
    st.slot_table[static_cast<std::size_t>(kNullIndex)] = nullptr;

    Q q(st.slot_table, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    // Force write index to the poisoned slot.
    force_state(q, st.slot_table.data(), kBufSz, /*head*/ kNullIndex, /*tail*/ kNullIndex);

    verify_invariants(q);
    QVERIFY(q.empty());

    QVERIFY(q.try_claim() == nullptr);

    std::mt19937 rng(0x1111u);
    QVERIFY(!q.try_push(make_blob(1u, rng)));
    verify_invariants(q);
    QVERIFY(q.empty());

    // Bulk claim exposes the nullptr to the caller.
    const auto wr = q.claim_write(1u);
    QCOMPARE(wr.total, reg{1u});
    QCOMPARE(wr.first.count, reg{1u});
    QVERIFY(wr.first.ptr[0] == nullptr);
}

template <class Policy>
static void test_null_slot_pointer_defense_dynamic() {
    using Q = spsc::pool_view<0, Policy>;

    dynamic_storage<> st;
    st.init(/*depth*/ 32u, kBufSz);

    constexpr reg kNullIndex = 9u;
    st.slot_table[static_cast<std::size_t>(kNullIndex)] = nullptr;

    Q q(st.slot_table.data(), st.depth, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    force_state(q, st.slot_table.data(), st.depth, kBufSz, /*head*/ kNullIndex, /*tail*/ kNullIndex);

    verify_invariants(q);
    QVERIFY(q.empty());

    QVERIFY(q.try_claim() == nullptr);

    std::mt19937 rng(0x2222u);
    QVERIFY(!q.try_push(make_blob(2u, rng)));

    const auto wr = q.claim_write(1u);
    QCOMPARE(wr.total, reg{1u});
    QCOMPARE(wr.first.count, reg{1u});
    QVERIFY(wr.first.ptr[0] == nullptr);
}

// 3) Partial snapshot consume: consume() must advance tail by snapshot size.

template <class Q>
static void test_partial_snapshot_consume(Q& q) {
    QVERIFY(q.is_valid());
    q.reset();

    std::mt19937 rng(0x515151u);
    for (reg i = 0u; i < 10u; ++i) {
        QVERIFY(q.try_push(make_blob(9'000u + static_cast<std::uint32_t>(i), rng)));
    }

    auto s = q.make_snapshot();
    const reg sz = static_cast<reg>(s.size());
    QCOMPARE(sz, reg{10u});

    // Consume half.
    auto it = s.begin();
    for (reg i = 0u; i < 5u; ++i) {
        ++it;
    }
    typename Q::snapshot half{s.begin(), it};

    q.consume(half);
    verify_invariants(q);
    QCOMPARE(q.size(), reg{5u});

    // Remaining items should be the last 5.
    for (reg i = 5u; i < 10u; ++i) {
        const void* p = q.try_front();
        QVERIFY(p != nullptr);
        Blob got{};
        load_from_slot(p, got);
        QVERIFY(got.seq == 9'000u + static_cast<std::uint32_t>(i));
        QVERIFY(q.try_pop());
    }

    verify_invariants(q);
    QVERIFY(q.empty());
}

// 4) Negative snapshot consume: try_consume() must reject mismatched tail.

template <class Q>
static void test_try_consume_rejects_mismatched_tail(Q& q) {
    QVERIFY(q.is_valid());
    q.reset();

    std::mt19937 rng(0x7777u);
    for (reg i = 0u; i < 8u; ++i) {
        QVERIFY(q.try_push(make_blob(7'000u + static_cast<std::uint32_t>(i), rng)));
    }

    auto s = q.make_snapshot();
    QCOMPARE(s.size(), std::size_t{8u});

    // Advance tail, snapshot becomes stale.
    QVERIFY(q.try_pop());
    verify_invariants(q);

    QVERIFY(!q.try_consume(s));

    // A fresh snapshot should be consumable.
    auto s2 = q.make_snapshot();
    QVERIFY(q.try_consume(s2));
    verify_invariants(q);
    QVERIFY(q.empty());
}

// 5) operator[] must match snapshot order.

template <class Q>
static void test_indexing_matches_snapshot(Q& q) {
    QVERIFY(q.is_valid());
    q.reset();

    std::mt19937 rng(0x8888u);
    for (reg i = 0u; i < 12u; ++i) {
        QVERIFY(q.try_push(make_blob(8'000u + static_cast<std::uint32_t>(i), rng)));
    }

    auto s = q.make_snapshot();
    QCOMPARE(static_cast<reg>(s.size()), reg{12u});

    auto it = s.begin();
    for (reg i = 0u; i < 12u; ++i) {
        const void* p_idx  = q[i];
        const void* p_snap = *it;
        QVERIFY(p_idx == p_snap);
        ++it;
    }

    QVERIFY(q.front() == q[0u]);

    q.consume_all();
    QVERIFY(q.empty());
}

// 6) Regions must match contiguous sizes.

template <class Q>
static void test_regions_match_sizes(Q& q) {
    QVERIFY(q.is_valid());
    q.reset();

    std::mt19937 rng(0x9999u);
    for (reg i = 0u; i < 11u; ++i) {
        QVERIFY(q.try_push(make_blob(8'000u + static_cast<std::uint32_t>(i), rng)));
    }

    verify_invariants(q);

    const auto wr = q.claim_write();
    QCOMPARE(wr.total, q.free());
    if (wr.total != 0u) {
        QCOMPARE(static_cast<reg>(wr.first.count), q.write_size());
        QCOMPARE(static_cast<reg>(wr.first.count + wr.second.count), wr.total);
    }

    const auto rd = q.claim_read();
    QCOMPARE(rd.total, q.size());
    if (rd.total != 0u) {
        QCOMPARE(static_cast<reg>(rd.first.count), q.read_size());
        QCOMPARE(static_cast<reg>(rd.first.count + rd.second.count), rd.total);
    }

    q.consume_all();
    verify_invariants(q);
    QVERIFY(q.empty());
}

// 7) Reset/clear idempotence.

template <class Q>
static void test_reset_idempotence(Q& q) {
    QVERIFY(q.is_valid());

    q.reset();
    verify_invariants(q);
    QVERIFY(q.empty());

    q.clear();
    verify_invariants(q);
    QVERIFY(q.empty());

    q.reset();
    verify_invariants(q);
    QVERIFY(q.empty());
}

// 8) adopt/attach(state) must reject corrupted state (size > capacity).

template <class Policy>
static void test_reject_corrupt_state_static() {
    using Q = spsc::pool_view<16, Policy>;

    static_storage<16> st;
    st.init(kBufSz);

    Q q(st.slot_table, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    // used = head - tail = 17 > cap (16)
    QVERIFY(!q.adopt(st.slot_table.data(), kBufSz, /*head*/ 17u, /*tail*/ 0u));
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    // attach with bad state must fail and detach
    Q q2;
    QVERIFY(!q2.attach(st.slot_table, kBufSz, typename Q::state_t{17u, 0u}));
    QVERIFY(!q2.is_valid());
    verify_invariants(q2);
}

template <class Policy>
static void test_reject_corrupt_state_dynamic() {
    using Q = spsc::pool_view<0, Policy>;

    dynamic_storage<> st;
    st.init(reg{32u}, kBufSz);

    Q q(st.slot_table.data(), st.depth, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    // cap is pow2 <= depth, but still >= 2.
    const reg cap = q.capacity();
    QVERIFY(cap >= 2u);

    // used = cap + 1 (corrupted)
    QVERIFY(!q.adopt(st.slot_table.data(), st.depth, kBufSz, /*head*/ cap + 1u, /*tail*/ 0u));
    QVERIFY(!q.is_valid());
    verify_invariants(q);

    Q q2;
    QVERIFY(!q2.attach(st.slot_table.data(), st.depth, kBufSz, typename Q::state_t{cap + 1u, 0u}));
    QVERIFY(!q2.is_valid());
    verify_invariants(q2);
}

// 9) Alignment sweep: for various alignments/offsets, as<T>() must match reality.

template <std::size_t Align>
struct alignas(Align) AlignProbe {
    std::byte payload[8];
};

template <class Policy>
static void test_alignment_sweep_static() {
    using Q = spsc::pool_view<16, Policy>;

    constexpr reg kDepth = 16u;
    constexpr std::size_t kExtra = 64u;

    auto run_one = [&](std::size_t align, std::size_t offset) {
        std::unique_ptr<std::byte[]> raw(new std::byte[static_cast<std::size_t>(kDepth * kBufSz) + kExtra]);
        std::byte* base = raw.get() + offset;

        std::array<void*, kDepth> slot_table{};
        for (std::size_t i = 0; i < kDepth; ++i) {
            slot_table[i] = base + i * static_cast<std::size_t>(kBufSz);
        }

        Q q(slot_table, kBufSz, Policy{});
        QVERIFY(q.is_valid());

        auto g = q.scoped_write();
        QVERIFY(static_cast<bool>(g));
        const void* ptr = g.get();
        QVERIFY(ptr != nullptr);

        const bool expect = (reinterpret_cast<std::uintptr_t>(ptr) % align) == 0u;

        if (align == 2u) {
            QCOMPARE(g.template as<AlignProbe<2u>>() != nullptr, expect);
        } else if (align == 4u) {
            QCOMPARE(g.template as<AlignProbe<4u>>() != nullptr, expect);
        } else if (align == 8u) {
            QCOMPARE(g.template as<AlignProbe<8u>>() != nullptr, expect);
        } else if (align == 16u) {
            QCOMPARE(g.template as<AlignProbe<16u>>() != nullptr, expect);
        } else if (align == 32u) {
            QCOMPARE(g.template as<AlignProbe<32u>>() != nullptr, expect);
        }

        g.cancel();
    };

    const std::array<std::size_t, 5> aligns{2u, 4u, 8u, 16u, 32u};
    for (const auto a : aligns) {
        for (std::size_t off = 0; off < a; ++off) {
            run_one(a, off);
        }
    }
}

template <class Policy>
static void test_alignment_sweep_dynamic() {
    using Q = spsc::pool_view<0, Policy>;

    constexpr reg kDepth = 32u;
    constexpr std::size_t kExtra = 64u;

    auto run_one = [&](std::size_t align, std::size_t offset) {
        std::unique_ptr<std::byte[]> raw(new std::byte[static_cast<std::size_t>(kDepth * kBufSz) + kExtra]);
        std::byte* base = raw.get() + offset;

        std::vector<void*> slot_table(static_cast<std::size_t>(kDepth));
        for (std::size_t i = 0; i < static_cast<std::size_t>(kDepth); ++i) {
            slot_table[i] = base + i * static_cast<std::size_t>(kBufSz);
        }

        Q q(slot_table.data(), kDepth, kBufSz, Policy{});
        QVERIFY(q.is_valid());

        auto g = q.scoped_write();
        QVERIFY(static_cast<bool>(g));
        const void* ptr = g.get();
        QVERIFY(ptr != nullptr);

        const bool expect = (reinterpret_cast<std::uintptr_t>(ptr) % align) == 0u;

        if (align == 2u) {
            QCOMPARE(g.template as<AlignProbe<2u>>() != nullptr, expect);
        } else if (align == 4u) {
            QCOMPARE(g.template as<AlignProbe<4u>>() != nullptr, expect);
        } else if (align == 8u) {
            QCOMPARE(g.template as<AlignProbe<8u>>() != nullptr, expect);
        } else if (align == 16u) {
            QCOMPARE(g.template as<AlignProbe<16u>>() != nullptr, expect);
        } else if (align == 32u) {
            QCOMPARE(g.template as<AlignProbe<32u>>() != nullptr, expect);
        }

        g.cancel();
    };

    const std::array<std::size_t, 5> aligns{2u, 4u, 8u, 16u, 32u};
    for (const auto a : aligns) {
        for (std::size_t off = 0; off < a; ++off) {
            run_one(a, off);
        }
    }
}

// ------------------------------ full suites ------------------------------

template <class Policy>
static void run_static_suite() {
    using Q = spsc::pool_view<16, Policy>;

    api_compile_smoke<Q>();
    test_static_attachment_state_move_swap<Q>();

    static_storage<16> st;
    st.init(kBufSz);

    Q q(st.slot_table, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    test_introspection_and_data(q);
    test_push_pop_front_try(q);
    test_bulk_regions(q, st.slot_table.data());
    test_snapshots(q);
    test_raii_guards_alignment_aligned(q);
    test_raii_guards_alignment_misaligned<Q>();

    test_partial_snapshot_consume(q);
    test_try_consume_rejects_mismatched_tail(q);
    test_indexing_matches_snapshot(q);
    test_iterator_ranges(q);
    test_iterator_consistency(q);
    test_regions_match_sizes(q);
    test_reset_idempotence(q);

    test_shadow_swap_move_regression_static<Policy>();
    test_null_slot_pointer_defense_static<Policy>();
    test_reject_corrupt_state_static<Policy>();
    test_alignment_sweep_static<Policy>();

    paranoid_random_fuzz(q, 0x13579BDFu + static_cast<std::uint32_t>(sizeof(Policy)), kFuzzIters);
}

template <class Policy>
static void run_dynamic_suite() {
    using Q = spsc::pool_view<0, Policy>;

    api_compile_smoke<Q>();
    test_dynamic_attachment_state_move_swap<Q>();

    dynamic_storage<> st;
    st.init(reg{64u}, kBufSz);

    Q q(st.slot_table.data(), st.depth, kBufSz, Policy{});
    QVERIFY(q.is_valid());

    test_introspection_and_data(q);
    test_push_pop_front_try(q);
    test_bulk_regions(q, st.slot_table.data(), st.depth);
    test_snapshots(q);
    test_raii_guards_alignment_aligned(q);

    test_partial_snapshot_consume(q);
    test_try_consume_rejects_mismatched_tail(q);
    test_indexing_matches_snapshot(q);
    test_iterator_ranges(q);
    test_iterator_consistency(q);
    test_regions_match_sizes(q);
    test_reset_idempotence(q);

    test_shadow_swap_move_regression_dynamic<Policy>();
    test_null_slot_pointer_defense_dynamic<Policy>();
    test_reject_corrupt_state_dynamic<Policy>();
    test_alignment_sweep_dynamic<Policy>();

    paranoid_random_fuzz(q, 0x2468ACE0u + static_cast<std::uint32_t>(sizeof(Policy)), kFuzzIters);
}

template <class Policy>
static void run_threaded_suite() {
    {
        using Q = spsc::pool_view<1024, Policy>;
        static_storage<1024> st;
        st.init(static_cast<reg>(sizeof(SeqMsg)));
        Q q(st.slot_table, static_cast<reg>(sizeof(SeqMsg)), Policy{});
        QVERIFY(q.is_valid());
        test_two_thread_spsc_pool_view(q);
    }

    {
        using Q = spsc::pool_view<0, Policy>;
        dynamic_storage<> st;
        st.init(reg{1024u}, static_cast<reg>(sizeof(SeqMsg)));
        Q q(st.slot_table.data(), st.depth, static_cast<reg>(sizeof(SeqMsg)), Policy{});
        QVERIFY(q.is_valid());
        test_two_thread_spsc_pool_view(q);
    }
}

} // namespace

class tst_pool_view_api_paranoid final : public QObject {
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
};

int run_tst_pool_view_api_paranoid(int argc, char** argv)
{
    tst_pool_view_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "pool_view_test.moc"
