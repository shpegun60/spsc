#if SPSC_TESTS_WITH_QT

#include <QtTest/QtTest>

#include "test_build_config.hpp"
#include "test_policy_matrix.hpp"

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
#include <new>
#include <random>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#if !defined(NDEBUG)
#  undef SPSC_ASSERT
#  define SPSC_ASSERT(expr) do { if (!(expr)) { std::abort(); } } while (0)
#endif

#include "buffer_pool.hpp"

namespace buffer_pool_test_detail {

#if defined(NDEBUG)
static constexpr int kStateMachineIters = 2500;
#else
static constexpr int kStateMachineIters = 350;
#endif

template <class Tag>
struct alloc_control {
    static inline std::atomic<std::size_t> alloc_calls{0};
    static inline std::atomic<std::size_t> dealloc_calls{0};
    static inline std::atomic<std::size_t> bytes_live{0};
    static inline std::atomic<std::size_t> bytes_peak{0};
    static inline std::atomic<long long> fail_on_call{-1};

    static void reset(const long long fail = -1) noexcept
    {
        alloc_calls.store(0);
        dealloc_calls.store(0);
        bytes_live.store(0);
        bytes_peak.store(0);
        fail_on_call.store(fail);
    }

    static void arm_fail_after_existing(const std::size_t offset) noexcept
    {
        fail_on_call.store(static_cast<long long>(alloc_calls.load() + offset));
    }

    static bool should_fail(const std::size_t call) noexcept
    {
        const long long fail = fail_on_call.load();
        return (fail > 0) && (static_cast<long long>(call) == fail);
    }

    static void on_alloc(const std::size_t bytes) noexcept
    {
        const auto live_now = bytes_live.fetch_add(bytes) + bytes;
        auto peak = bytes_peak.load();
        while ((live_now > peak) && !bytes_peak.compare_exchange_weak(peak, live_now)) {
        }
    }

    static void on_dealloc(const std::size_t bytes) noexcept
    {
        bytes_live.fetch_sub(bytes);
    }
};

template <class T, std::size_t Alignment, class Tag>
class ControlledAlignedAlloc
{
public:
    using value_type = T;
    using pointer = T*;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using is_always_equal = std::true_type;

    ControlledAlignedAlloc() noexcept = default;

    template <class U>
    ControlledAlignedAlloc(const ControlledAlignedAlloc<U, Alignment, Tag>&) noexcept {}

    template <class U>
    struct rebind {
        using other = ControlledAlignedAlloc<U, Alignment, Tag>;
    };

    [[nodiscard]] pointer allocate(const size_type n) noexcept
    {
        if (n == 0u) {
            return nullptr;
        }

        const std::size_t call = alloc_control<Tag>::alloc_calls.fetch_add(1u) + 1u;
        if (alloc_control<Tag>::should_fail(call)) {
            return nullptr;
        }

        constexpr std::size_t kAllocAlign = (Alignment > alignof(T)) ? Alignment : alignof(T);
        const std::size_t bytes = n * sizeof(T);
        void* raw = ::operator new(bytes, std::align_val_t(kAllocAlign), std::nothrow);
        if (raw == nullptr) {
            return nullptr;
        }

        alloc_control<Tag>::on_alloc(bytes);
        return static_cast<pointer>(raw);
    }

    void deallocate(pointer p, const size_type n) noexcept
    {
        if (p == nullptr) {
            return;
        }

        constexpr std::size_t kAllocAlign = (Alignment > alignof(T)) ? Alignment : alignof(T);
        const std::size_t bytes = n * sizeof(T);
        alloc_control<Tag>::dealloc_calls.fetch_add(1u);
        alloc_control<Tag>::on_dealloc(bytes);
        ::operator delete(p, std::align_val_t(kAllocAlign));
    }

    template <class U>
    bool operator==(const ControlledAlignedAlloc<U, Alignment, Tag>&) const noexcept { return true; }

    template <class U>
    bool operator!=(const ControlledAlignedAlloc<U, Alignment, Tag>&) const noexcept { return false; }
};

} // namespace buffer_pool_test_detail

namespace spsc::alloc::detail {

template <class T, std::size_t Alignment, class Tag, class U>
struct allocator_min_alignment<
    ::buffer_pool_test_detail::ControlledAlignedAlloc<T, Alignment, Tag>,
    U,
    void>
    : std::integral_constant<std::size_t, (Alignment > alignof(U)) ? Alignment : alignof(U)> {};

} // namespace spsc::alloc::detail

namespace buffer_pool_test_detail {

struct Cell {
    std::uint32_t value{0u};

    bool operator==(const Cell& other) const noexcept { return value == other.value; }
};

struct TrackedCell {
    static inline std::atomic<int> live{0};
    static inline std::atomic<int> ctor{0};
    static inline std::atomic<int> dtor{0};
    static inline std::atomic<int> copy_ctor{0};
    static inline std::atomic<int> move_ctor{0};
    static inline std::atomic<int> copy_assign{0};
    static inline std::atomic<int> move_assign{0};

    std::uint32_t value{0u};

    TrackedCell() noexcept
    {
        ++live;
        ++ctor;
    }

    TrackedCell(const TrackedCell& other) noexcept
        : value(other.value)
    {
        ++live;
        ++ctor;
        ++copy_ctor;
    }

    TrackedCell(TrackedCell&& other) noexcept
        : value(other.value)
    {
        other.value = 0u;
        ++live;
        ++ctor;
        ++move_ctor;
    }

    TrackedCell& operator=(const TrackedCell& other) noexcept
    {
        value = other.value;
        ++copy_assign;
        return *this;
    }

    TrackedCell& operator=(TrackedCell&& other) noexcept
    {
        value = other.value;
        other.value = 0u;
        ++move_assign;
        return *this;
    }

    ~TrackedCell() noexcept
    {
        ++dtor;
        --live;
    }

    static void reset() noexcept
    {
        live.store(0);
        ctor.store(0);
        dtor.store(0);
        copy_ctor.store(0);
        move_ctor.store(0);
        copy_assign.store(0);
        move_assign.store(0);
    }
};

template <class Q>
constexpr bool kFullyStatic = (Q::static_buffer_size != 0u) && (Q::static_count != 0u);

template <class Q>
constexpr bool kFixedBufferDynamicCount = (Q::static_buffer_size != 0u) && (Q::static_count == 0u);

template <class Q>
constexpr bool kDynamicBufferFixedCount = (Q::static_buffer_size == 0u) && (Q::static_count != 0u);

template <class Q>
constexpr bool kFullyDynamic = (Q::static_buffer_size == 0u) && (Q::static_count == 0u);

template <class Q>
constexpr reg requested_count() noexcept
{
    if constexpr (Q::static_count != 0u) {
        return Q::static_count;
    } else {
        return reg{3u};
    }
}

template <class Q>
constexpr reg requested_size() noexcept
{
    if constexpr (Q::static_buffer_size != 0u) {
        return Q::static_buffer_size;
    } else {
        return reg{4u};
    }
}

template <class Q>
constexpr reg grown_count() noexcept
{
    if constexpr (Q::static_count != 0u) {
        return Q::static_count;
    } else {
        return reg{5u};
    }
}

template <class Q>
constexpr reg grown_size() noexcept
{
    if constexpr (Q::static_buffer_size != 0u) {
        return Q::static_buffer_size;
    } else {
        return reg{7u};
    }
}

template <class Q>
[[nodiscard]] static reg expected_runtime_span_bytes(const reg logical_count) noexcept
{
    using value_type = typename Q::value_type;
    using policy_type = typename Q::policy_type;
    using allocator_type = typename Q::base_allocator_type;

    return static_cast<reg>(::spsc::alloc::round_up_size_for_policy<policy_type, allocator_type>(
        static_cast<reg>(logical_count * sizeof(value_type))));
}

template <class Q>
static bool configure(Q& q, const reg count, const reg size)
{
    if constexpr (kFullyStatic<Q>) {
        Q_UNUSED(q);
        Q_UNUSED(count);
        Q_UNUSED(size);
        return true;
    } else if constexpr (kFixedBufferDynamicCount<Q>) {
        Q_UNUSED(size);
        return q.resize(count);
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        Q_UNUSED(count);
        return q.resize(size);
    } else {
        return q.resize(count, size);
    }
}

template <class Q>
[[nodiscard]] static bool is_empty_state(const Q& q) noexcept
{
    if constexpr (kFullyStatic<Q>) {
        return false;
    } else if constexpr (kFixedBufferDynamicCount<Q>) {
        return q.count() == 0u;
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        return q.size() == 0u;
    } else {
        return (q.count() == 0u) && (q.size() == 0u);
    }
}

template <class Q>
[[nodiscard]] static std::vector<std::uintptr_t> pointer_snapshot(const Q& q)
{
    std::vector<std::uintptr_t> out;
    out.reserve(static_cast<std::size_t>(q.count()));
    for (reg i = 0u; i < q.count(); ++i) {
        out.push_back(reinterpret_cast<std::uintptr_t>(q.data(i)));
    }
    return out;
}

template <class Q>
[[nodiscard]] static std::vector<std::vector<std::uint32_t>> value_snapshot(const Q& q)
{
    std::vector<std::vector<std::uint32_t>> out;
    out.reserve(static_cast<std::size_t>(q.count()));

    for (reg i = 0u; i < q.count(); ++i) {
        std::vector<std::uint32_t> one;
        one.reserve(static_cast<std::size_t>(q.size()));
        const auto* ptr = q.data(i);
        for (reg j = 0u; j < q.size(); ++j) {
            one.push_back((ptr != nullptr) ? ptr[j].value : 0u);
        }
        out.push_back(std::move(one));
    }

    return out;
}

static std::uint32_t pattern_value(const reg i, const reg j, const std::uint32_t base) noexcept;

template <class Q>
static void fill_pattern(Q& q, const std::uint32_t base)
{
    for (reg i = 0u; i < q.count(); ++i) {
        auto* ptr = q.data(i);
        if (ptr == nullptr) {
            continue;
        }

        for (reg j = 0u; j < q.size(); ++j) {
            ptr[j].value = pattern_value(i, j, base);
        }
    }
}

template <class Q>
static void expect_pattern(const Q& q, const std::uint32_t base)
{
    for (reg i = 0u; i < q.count(); ++i) {
        const auto* ptr = q.data(i);
        QVERIFY(ptr != nullptr);
        for (reg j = 0u; j < q.size(); ++j) {
            QCOMPARE(ptr[j].value, pattern_value(i, j, base));
        }
    }
}

template <class Q>
static void verify_invariants(const Q& q, const char* context = nullptr)
{
    Q_UNUSED(context);

    using value_type = typename Q::value_type;
    using size_type = typename Q::size_type;
    using policy_type = typename Q::policy_type;

    QVERIFY(q.is_valid());
    QVERIFY(q.alignment() != 0u);
    QVERIFY((q.alignment() & (q.alignment() - 1u)) == 0u);
    QVERIFY(q.get_allocator() == typename Q::base_allocator_type{});

    if constexpr (kFullyStatic<Q>) {
        QCOMPARE(q.count(), Q::static_count);
        QCOMPARE(q.size(), Q::static_buffer_size);
        QCOMPARE(q.size_bytes(), static_cast<size_type>(sizeof(typename Q::buffer_type)));
        QCOMPARE(q.span_bytes(), static_cast<size_type>(sizeof(typename Q::stored_buffer_type)));
        QCOMPARE(q.alignment(), static_cast<size_type>(alignof(typename Q::stored_buffer_type)));
    } else if constexpr (kFixedBufferDynamicCount<Q>) {
        QCOMPARE(q.size(), Q::static_buffer_size);
        QCOMPARE(q.size_bytes(), static_cast<size_type>(sizeof(typename Q::buffer_type)));
        QCOMPARE(q.span_bytes(), static_cast<size_type>(sizeof(typename Q::stored_buffer_type)));
        QCOMPARE(q.alignment(), static_cast<size_type>(alignof(typename Q::stored_buffer_type)));
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        QCOMPARE(q.count(), Q::static_count);
        QCOMPARE(q.size_bytes(), static_cast<size_type>(q.size() * sizeof(value_type)));
        QCOMPARE(q.span_bytes(), (q.size() == 0u) ? size_type{0u} : expected_runtime_span_bytes<Q>(q.size()));
        QCOMPARE(q.alignment(),
                 static_cast<size_type>(::spsc::alloc::policy_storage_alignment_v<policy_type, value_type>));
    } else {
        QCOMPARE(q.size_bytes(), static_cast<size_type>(q.size() * sizeof(value_type)));
        QCOMPARE(q.span_bytes(), (q.size() == 0u) ? size_type{0u} : expected_runtime_span_bytes<Q>(q.size()));
        QCOMPARE(q.alignment(),
                 static_cast<size_type>(::spsc::alloc::policy_storage_alignment_v<policy_type, value_type>));
    }

    QVERIFY(q.span_bytes() >= q.size_bytes());
    QCOMPARE(q.payload_bytes(), q.size_bytes());
    QCOMPARE(q.cache_span_bytes(), q.span_bytes());
    QCOMPARE(q.storage_alignment(), q.alignment());

    std::vector<std::uintptr_t> seen_ptrs;
    seen_ptrs.reserve(static_cast<std::size_t>(q.count()));

    for (reg i = 0u; i < q.count(); ++i) {
        const auto* ptr = q.data(i);
        if ((q.size() == 0u) || (kFixedBufferDynamicCount<Q> && (q.count() == 0u))) {
            QVERIFY(ptr == nullptr);
            continue;
        }

        QVERIFY(ptr != nullptr);
        QVERIFY((reinterpret_cast<std::uintptr_t>(ptr) % alignof(value_type)) == 0u);
        QVERIFY((reinterpret_cast<std::uintptr_t>(ptr) % q.alignment()) == 0u);
        const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
        QVERIFY(std::find(seen_ptrs.begin(), seen_ptrs.end(), addr) == seen_ptrs.end());
        seen_ptrs.push_back(addr);
        QCOMPARE(q[i], ptr);
    }

    QVERIFY(q.data(q.count()) == nullptr);
}

template <class Q>
static void make_filled(Q& q, const std::uint32_t base)
{
    if constexpr (!kFullyStatic<Q>) {
        QVERIFY(configure(q, requested_count<Q>(), requested_size<Q>()));
    }
    verify_invariants(q);
    fill_pattern(q, base);
    if (!is_empty_state(q)) {
        expect_pattern(q, base);
    }
}

template <class Q>
static void expect_prefix_and_zero_tail(
    const Q& q,
    const std::vector<std::vector<std::uint32_t>>& before)
{
    const reg old_count = static_cast<reg>(before.size());
    const reg old_size = before.empty() ? reg{0u} : static_cast<reg>(before.front().size());
    const reg copy_count = (old_count < q.count()) ? old_count : q.count();
    const reg copy_size = (old_size < q.size()) ? old_size : q.size();

    for (reg i = 0u; i < q.count(); ++i) {
        const auto* ptr = q.data(i);
        if (q.size() == 0u) {
            QVERIFY(ptr == nullptr);
            continue;
        }

        QVERIFY(ptr != nullptr);
        for (reg j = 0u; j < q.size(); ++j) {
            if ((i < copy_count) && (j < copy_size)) {
                QCOMPARE(ptr[j].value, before[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)]);
            } else {
                QCOMPARE(ptr[j].value, std::uint32_t{0u});
            }
        }
    }
}

struct buffer_model {
    reg count{0u};
    reg size{0u};
    std::vector<std::vector<std::uint32_t>> values{};
};

[[nodiscard]] static buffer_model zero_model(const reg count, const reg size)
{
    buffer_model model;
    model.count = count;
    model.size = size;
    model.values.resize(static_cast<std::size_t>(count));
    for (auto& row : model.values) {
        row.resize(static_cast<std::size_t>(size), 0u);
    }
    return model;
}

template <class Q>
[[nodiscard]] static buffer_model empty_model_for()
{
    if constexpr (kFullyStatic<Q>) {
        return zero_model(Q::static_count, Q::static_buffer_size);
    } else if constexpr (kFixedBufferDynamicCount<Q>) {
        return zero_model(0u, Q::static_buffer_size);
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        return zero_model(Q::static_count, 0u);
    } else {
        return zero_model(0u, 0u);
    }
}

template <class Q>
[[nodiscard]] static buffer_model capture_model(const Q& q)
{
    buffer_model model;
    model.count = q.count();
    model.size = q.size();
    model.values = value_snapshot(q);
    return model;
}

template <class Q>
[[nodiscard]] static bool model_describes_empty(const buffer_model& model) noexcept
{
    if constexpr (kFullyStatic<Q>) {
        return false;
    } else if constexpr (kFixedBufferDynamicCount<Q>) {
        return model.count == 0u;
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        return model.size == 0u;
    } else {
        return (model.count == 0u) && (model.size == 0u);
    }
}

static void verify_model_shape(const buffer_model& model)
{
    QCOMPARE(static_cast<reg>(model.values.size()), model.count);
    for (const auto& row : model.values) {
        QCOMPARE(static_cast<reg>(row.size()), model.size);
    }
}

template <class Q>
static void expect_model(const Q& q, const buffer_model& model, const char* context = nullptr)
{
    verify_invariants(q, context);
    verify_model_shape(model);
    QCOMPARE(q.count(), model.count);
    QCOMPARE(q.size(), model.size);
    QVERIFY(value_snapshot(q) == model.values);
    if constexpr (!kFullyStatic<Q>) {
        QCOMPARE(is_empty_state(q), model_describes_empty<Q>(model));
    }
}

[[nodiscard]] static buffer_model resized_model_like(
    const buffer_model& before,
    const reg new_count,
    const reg new_size)
{
    buffer_model model = zero_model(new_count, new_size);
    const reg copy_count = (before.count < new_count) ? before.count : new_count;
    const reg copy_size = (before.size < new_size) ? before.size : new_size;

    for (reg i = 0u; i < copy_count; ++i) {
        for (reg j = 0u; j < copy_size; ++j) {
            model.values[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] =
                before.values[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)];
        }
    }

    return model;
}

static std::uint32_t pattern_value(const reg i, const reg j, const std::uint32_t base) noexcept
{
    return base + static_cast<std::uint32_t>(i * 101u) + static_cast<std::uint32_t>(j * 7u);
}

static void fill_model_pattern(buffer_model& model, const std::uint32_t base)
{
    verify_model_shape(model);
    for (reg i = 0u; i < model.count; ++i) {
        for (reg j = 0u; j < model.size; ++j) {
            model.values[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = pattern_value(i, j, base);
        }
    }
}

template <class Q>
static void api_smoke_compile()
{
    using size_type = typename Q::size_type;
    using pointer = typename Q::pointer;
    using const_pointer = typename Q::const_pointer;

    static_assert(std::is_same_v<decltype(std::declval<const Q&>().get_allocator()), typename Q::base_allocator_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().is_valid()), bool>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().data(size_type{})), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().data(size_type{})), const_pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().operator[](size_type{})), pointer>);
    static_assert(std::is_same_v<decltype(std::declval<const Q&>().operator[](size_type{})), const_pointer>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().alignment()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().span_bytes()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().size_bytes()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().storage_alignment()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().cache_span_bytes()), size_type>);
    static_assert(std::is_same_v<decltype(std::declval<Q&>().payload_bytes()), size_type>);

    if constexpr (::spsc::test::has_destroy_method<Q>::value) {
        static_assert(std::is_same_v<decltype(std::declval<Q&>().destroy()), void>);
    }
    if constexpr (::spsc::test::has_swap_method<Q>::value) {
        static_assert(std::is_same_v<decltype(std::declval<Q&>().swap(std::declval<Q&>())), void>);
    }
    if constexpr (::spsc::test::has_resize_two<Q>::value) {
        static_assert(std::is_same_v<decltype(std::declval<Q&>().resize(size_type{}, size_type{})), bool>);
    } else if constexpr (::spsc::test::has_resize_one<Q>::value) {
        static_assert(std::is_same_v<decltype(std::declval<Q&>().resize(size_type{})), bool>);
    }
}

static void api_compile_smoke_all()
{
    using SP = ::spsc::buffer_pool<Cell, 5u, 3u, ::spsc::policy::P>;
    using SV = ::spsc::buffer_pool<Cell, 5u, 3u, ::spsc::policy::V>;
    using SA = ::spsc::buffer_pool<Cell, 5u, 3u, ::spsc::policy::A<>>;
    using SC = ::spsc::buffer_pool<Cell, 5u, 3u, ::spsc::policy::CA<>>;
    using FP = ::spsc::buffer_pool<Cell, 5u, 0u, ::spsc::policy::P>;
    using FC = ::spsc::buffer_pool<Cell, 0u, 3u, ::spsc::policy::P>;
    using DP = ::spsc::buffer_pool<Cell, 0u, 0u, ::spsc::policy::P>;

    api_smoke_compile<SP>();
    api_smoke_compile<SV>();
    api_smoke_compile<SA>();
    api_smoke_compile<SC>();
    api_smoke_compile<FP>();
    api_smoke_compile<FC>();
    api_smoke_compile<DP>();

    static_assert(std::is_same_v<
        ::spsc::static_buffer_pool<Cell, 5u, 3u>,
        ::spsc::buffer_pool<Cell, 5u, 3u>>);
    static_assert(std::is_same_v<
        ::spsc::fixed_buffer_pool<Cell, 5u>,
        ::spsc::buffer_pool<Cell, 5u, 0u>>);
    static_assert(std::is_same_v<
        ::spsc::fixed_count_buffer_pool<Cell, 3u>,
        ::spsc::buffer_pool<Cell, 0u, 3u>>);
    static_assert(std::is_same_v<
        ::spsc::dynamic_buffer_pool<Cell>,
        ::spsc::buffer_pool<Cell, 0u, 0u>>);

    ::spsc::test::for_each_extended_nonthreaded_policy([](auto policy_tag) {
        using Policy = typename decltype(policy_tag)::type;
        api_smoke_compile<::spsc::buffer_pool<Cell, 5u, 3u, Policy>>();
        api_smoke_compile<::spsc::buffer_pool<Cell, 5u, 0u, Policy>>();
        api_smoke_compile<::spsc::buffer_pool<Cell, 0u, 3u, Policy>>();
        api_smoke_compile<::spsc::buffer_pool<Cell, 0u, 0u, Policy>>();
    });
}

template <class Q>
static void ctor_contracts_suite()
{
    Q q_default{};
    verify_invariants(q_default);
    if constexpr (!kFullyStatic<Q>) {
        QVERIFY(is_empty_state(q_default));
    }

    if constexpr (kFixedBufferDynamicCount<Q>) {
        Q q{reg{4u}};
        verify_invariants(q);
        QCOMPARE(q.count(), reg{4u});

        Q q_zero{reg{0u}};
        verify_invariants(q_zero);
        QVERIFY(is_empty_state(q_zero));
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        Q q{reg{6u}};
        verify_invariants(q);
        QCOMPARE(q.size(), reg{6u});

        Q q_zero{reg{0u}};
        verify_invariants(q_zero);
        QVERIFY(is_empty_state(q_zero));
    } else if constexpr (kFullyDynamic<Q>) {
        Q q{reg{4u}, reg{6u}};
        verify_invariants(q);
        QCOMPARE(q.count(), reg{4u});
        QCOMPARE(q.size(), reg{6u});

        Q q_zero0{reg{0u}, reg{0u}};
        Q q_zero1{reg{4u}, reg{0u}};
        Q q_zero2{reg{0u}, reg{6u}};
        verify_invariants(q_zero0);
        verify_invariants(q_zero1);
        verify_invariants(q_zero2);
        QVERIFY(is_empty_state(q_zero0));
        QVERIFY(is_empty_state(q_zero1));
        QVERIFY(is_empty_state(q_zero2));
    }
}

template <class Q>
static void indexing_suite()
{
    Q q{};
    make_filled(q, 100u);

    for (reg i = 0u; i < q.count(); ++i) {
        auto* p = q.data(i);
        QVERIFY(p != nullptr);
        QCOMPARE(q[i], p);
        for (reg j = 0u; j < q.size(); ++j) {
            QCOMPARE(q[i][j].value,
                     100u + static_cast<std::uint32_t>(i * 101u) + static_cast<std::uint32_t>(j * 7u));
        }
    }

    QVERIFY(q.data(q.count()) == nullptr);

    if constexpr (!kFullyStatic<Q>) {
        q.destroy();
        verify_invariants(q);
        QVERIFY(is_empty_state(q));
        QVERIFY(q.data(0u) == nullptr);
    }
}

template <class Q>
static void copy_move_swap_suite()
{
    Q src{};
    make_filled(src, 1000u);
    const auto src_values = value_snapshot(src);

    Q copy{src};
    verify_invariants(copy);
    QVERIFY(value_snapshot(copy) == src_values);
    for (reg i = 0u; i < copy.count(); ++i) {
        if (copy.size() != 0u) {
            QVERIFY(copy.data(i) != src.data(i));
        }
    }

    Q assigned{};
    make_filled(assigned, 7000u);
    assigned = src;
    verify_invariants(assigned);
    QVERIFY(value_snapshot(assigned) == src_values);

    if constexpr (!kFullyStatic<Q>) {
        Q empty{};
        verify_invariants(empty);
        QVERIFY(is_empty_state(empty));
        assigned = empty;
        verify_invariants(assigned);
        QVERIFY(is_empty_state(assigned));
    }

    Q moved{std::move(copy)};
    verify_invariants(moved);
    QVERIFY(value_snapshot(moved) == src_values);
    if constexpr (!kFullyStatic<Q>) {
        verify_invariants(copy);
        QVERIFY(is_empty_state(copy));
    }

    Q move_assigned{};
    make_filled(move_assigned, 9000u);
    move_assigned = std::move(src);
    verify_invariants(move_assigned);
    QVERIFY(value_snapshot(move_assigned) == src_values);
    if constexpr (!kFullyStatic<Q>) {
        verify_invariants(src);
        QVERIFY(is_empty_state(src));
    }

    Q a{};
    Q b{};
    make_filled(a, 111u);
    make_filled(b, 222u);
    const auto snap_a = value_snapshot(a);
    const auto snap_b = value_snapshot(b);

    a.swap(b);
    verify_invariants(a);
    verify_invariants(b);
    QVERIFY(value_snapshot(a) == snap_b);
    QVERIFY(value_snapshot(b) == snap_a);

    swap(a, b);
    QVERIFY(value_snapshot(a) == snap_a);
    QVERIFY(value_snapshot(b) == snap_b);

    const auto before_self = value_snapshot(a);
    a.swap(a);
    verify_invariants(a);
    QVERIFY(value_snapshot(a) == before_self);

    if constexpr (!kFullyStatic<Q>) {
        Q empty{};
        const auto full_before = value_snapshot(a);
        a.swap(empty);
        verify_invariants(a);
        verify_invariants(empty);
        QVERIFY(is_empty_state(a));
        QVERIFY(value_snapshot(empty) == full_before);

        swap(a, empty);
        verify_invariants(a);
        verify_invariants(empty);
        QVERIFY(value_snapshot(a) == full_before);
        QVERIFY(is_empty_state(empty));
    }
}

template <class Q>
static void exercise_self_move_assignment(Q& q)
{
    Q* alias = &q;
    q = std::move(*alias);
}

template <class Q>
static void copy_move_aliasing_corner_cases_suite()
{
    Q self{};
    make_filled(self, 5151u);
    const auto self_values = value_snapshot(self);
    const auto self_ptrs = pointer_snapshot(self);

    self = self;
    verify_invariants(self);
    QVERIFY(value_snapshot(self) == self_values);
    QVERIFY(pointer_snapshot(self) == self_ptrs);

    exercise_self_move_assignment(self);
    verify_invariants(self);
    QVERIFY(value_snapshot(self) == self_values);
    QVERIFY(pointer_snapshot(self) == self_ptrs);

    if constexpr (!kFullyStatic<Q>) {
        Q empty{};
        verify_invariants(empty);
        QVERIFY(is_empty_state(empty));

        Q src{};
        Q dst{};
        make_filled(src, 6161u);
        make_filled(dst, 7171u);

        const auto src_values = value_snapshot(src);
        const auto src_ptrs = pointer_snapshot(src);

        dst = std::move(src);
        verify_invariants(dst);
        verify_invariants(src);
        QVERIFY(value_snapshot(dst) == src_values);
        QVERIFY(pointer_snapshot(dst) == src_ptrs);
        QVERIFY(is_empty_state(src));

        Q copy_of_moved{src};
        verify_invariants(copy_of_moved);
        QVERIFY(is_empty_state(copy_of_moved));

        Q assigned_from_moved{};
        make_filled(assigned_from_moved, 8181u);
        assigned_from_moved = src;
        verify_invariants(assigned_from_moved);
        QVERIFY(is_empty_state(assigned_from_moved));

        src = dst;
        verify_invariants(src);
        QVERIFY(value_snapshot(src) == src_values);
        if (src.size() != 0u) {
            for (reg i = 0u; i < src.count(); ++i) {
                QVERIFY(src.data(i) != dst.data(i));
            }
        }

        const auto rebound_values = value_snapshot(src);
        const auto rebound_ptrs = pointer_snapshot(src);
        dst = std::move(src);
        verify_invariants(dst);
        verify_invariants(src);
        QVERIFY(value_snapshot(dst) == rebound_values);
        QVERIFY(pointer_snapshot(dst) == rebound_ptrs);
        QVERIFY(is_empty_state(src));

        Q moved_empty{std::move(empty)};
        verify_invariants(moved_empty);
        verify_invariants(empty);
        QVERIFY(is_empty_state(moved_empty));
        QVERIFY(is_empty_state(empty));

        dst = std::move(empty);
        verify_invariants(dst);
        verify_invariants(empty);
        QVERIFY(is_empty_state(dst));
        QVERIFY(is_empty_state(empty));
    }
}

template <class Q>
static void resize_suite()
{
    static_assert(!kFullyStatic<Q>);

    Q q{};
    make_filled(q, 321u);

    const reg same_count = requested_count<Q>();
    const reg same_size = requested_size<Q>();
    const auto before_ptrs = pointer_snapshot(q);
    const auto before_vals = value_snapshot(q);
    QVERIFY(configure(q, same_count, same_size));
    verify_invariants(q);
    QVERIFY(pointer_snapshot(q) == before_ptrs);
    QVERIFY(value_snapshot(q) == before_vals);

    const auto old_values = value_snapshot(q);
    QVERIFY(configure(q, grown_count<Q>(), grown_size<Q>()));
    verify_invariants(q);
    expect_prefix_and_zero_tail(q, old_values);

    fill_pattern(q, 777u);
    const auto grown_values = value_snapshot(q);
    const reg shrink_count = (requested_count<Q>() > 1u) ? requested_count<Q>() - 1u : requested_count<Q>();
    const reg shrink_size = (requested_size<Q>() > 1u) ? requested_size<Q>() - 1u : requested_size<Q>();
    QVERIFY(configure(q, shrink_count, shrink_size));
    verify_invariants(q);
    expect_prefix_and_zero_tail(q, grown_values);

    if constexpr (kFixedBufferDynamicCount<Q>) {
        QVERIFY(q.resize(0u));
    } else if constexpr (kDynamicBufferFixedCount<Q>) {
        QVERIFY(q.resize(0u));
    } else {
        QVERIFY(q.resize(0u, 0u));
        QVERIFY(q.resize(0u, grown_size<Q>()));
        verify_invariants(q);
        QVERIFY(is_empty_state(q));
    }

    verify_invariants(q);
    QVERIFY(is_empty_state(q));

    q.destroy();
    verify_invariants(q);
    QVERIFY(is_empty_state(q));
    q.destroy();
    verify_invariants(q);
    QVERIFY(is_empty_state(q));

    make_filled(q, 999u);
    verify_invariants(q);
    expect_pattern(q, 999u);
}

template <class Q>
static void tracked_lifetime_suite()
{
    TrackedCell::reset();

    {
        Q q{};
        if constexpr (!kFullyStatic<Q>) {
            QVERIFY(configure(q, requested_count<Q>(), requested_size<Q>()));
        }

        verify_invariants(q);
        QCOMPARE(TrackedCell::live.load(), static_cast<int>(q.count() * q.size()));

        {
            Q copy{q};
            verify_invariants(copy);
            QCOMPARE(TrackedCell::live.load(), static_cast<int>((q.count() * q.size()) + (copy.count() * copy.size())));
        }

        if constexpr (!kFullyStatic<Q>) {
            q.destroy();
            verify_invariants(q);
            QVERIFY(is_empty_state(q));
            QCOMPARE(TrackedCell::live.load(), 0);
        }
    }

    QCOMPARE(TrackedCell::live.load(), 0);
    QCOMPARE(TrackedCell::ctor.load(), TrackedCell::dtor.load());
}

template <class Tag>
static void failure_ctor_suite_fixed_buffer_dynamic_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        5u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset(1);
    Q q{reg{4u}};
    verify_invariants(q);
    QVERIFY(is_empty_state(q));
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), std::size_t{0u});

    alloc_control<Tag>::reset();
    QVERIFY(configure(q, requested_count<Q>(), requested_size<Q>()));
    verify_invariants(q);
    fill_pattern(q, 410u);
    expect_pattern(q, 410u);
    QVERIFY(alloc_control<Tag>::bytes_live.load() > std::size_t{0u});
}

template <class Tag>
static void failure_ctor_suite_dynamic_buffer_fixed_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        3u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset(1);
    Q q{reg{4u}};
    verify_invariants(q);
    QVERIFY(is_empty_state(q));
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), std::size_t{0u});

    alloc_control<Tag>::reset();
    QVERIFY(configure(q, requested_count<Q>(), requested_size<Q>()));
    verify_invariants(q);
    fill_pattern(q, 420u);
    expect_pattern(q, 420u);
    QVERIFY(alloc_control<Tag>::bytes_live.load() > std::size_t{0u});
}

template <class Tag>
static void failure_ctor_suite_fully_dynamic()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset(2);
    Q q{reg{3u}, reg{4u}};
    verify_invariants(q);
    QVERIFY(is_empty_state(q));
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), std::size_t{0u});

    alloc_control<Tag>::reset();
    QVERIFY(configure(q, requested_count<Q>(), requested_size<Q>()));
    verify_invariants(q);
    fill_pattern(q, 430u);
    expect_pattern(q, 430u);
    QVERIFY(alloc_control<Tag>::bytes_live.load() > std::size_t{0u});
}

template <class Tag>
static void failure_copy_ctor_suite_fixed_buffer_dynamic_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        5u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q src;
    make_filled(src, 5100u);
    const auto src_values = value_snapshot(src);
    const auto src_ptrs = pointer_snapshot(src);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(1u);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(is_empty_state(copy));
        QVERIFY(copy.data(0u) == nullptr);
    }
    QVERIFY(value_snapshot(src) == src_values);
    QVERIFY(pointer_snapshot(src) == src_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(value_snapshot(copy) == src_values);
        if (copy.size() != 0u) {
            for (reg i = 0u; i < copy.count(); ++i) {
                QVERIFY(copy.data(i) != src.data(i));
            }
        }
    }
}

template <class Tag>
static void failure_copy_ctor_suite_dynamic_buffer_fixed_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        3u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q src;
    make_filled(src, 5200u);
    const auto src_values = value_snapshot(src);
    const auto src_ptrs = pointer_snapshot(src);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(2u);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(is_empty_state(copy));
        QVERIFY(copy.data(0u) == nullptr);
    }
    QVERIFY(value_snapshot(src) == src_values);
    QVERIFY(pointer_snapshot(src) == src_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(value_snapshot(copy) == src_values);
        for (reg i = 0u; i < copy.count(); ++i) {
            QVERIFY(copy.data(i) != src.data(i));
        }
    }
}

template <class Tag>
static void failure_copy_ctor_suite_fully_dynamic()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q src;
    make_filled(src, 5300u);
    const auto src_values = value_snapshot(src);
    const auto src_ptrs = pointer_snapshot(src);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(1u);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(is_empty_state(copy));
        QVERIFY(copy.data(0u) == nullptr);
    }
    QVERIFY(value_snapshot(src) == src_values);
    QVERIFY(pointer_snapshot(src) == src_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::arm_fail_after_existing(2u);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(is_empty_state(copy));
        QVERIFY(copy.data(0u) == nullptr);
    }
    QVERIFY(value_snapshot(src) == src_values);
    QVERIFY(pointer_snapshot(src) == src_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    {
        Q copy{src};
        verify_invariants(copy);
        QVERIFY(value_snapshot(copy) == src_values);
        for (reg i = 0u; i < copy.count(); ++i) {
            QVERIFY(copy.data(i) != src.data(i));
        }
    }
}

template <class Tag>
static void failure_resize_suite_fixed_buffer_dynamic_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        5u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q q;
    make_filled(q, 111u);
    const auto before_values = value_snapshot(q);
    const auto before_ptrs = pointer_snapshot(q);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(1u);
    QVERIFY(!q.resize(reg{7u}));
    verify_invariants(q);
    QVERIFY(value_snapshot(q) == before_values);
    QVERIFY(pointer_snapshot(q) == before_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    QVERIFY(configure(q, grown_count<Q>(), grown_size<Q>()));
    verify_invariants(q);
    expect_prefix_and_zero_tail(q, before_values);
}

template <class Tag>
static void failure_resize_suite_dynamic_buffer_fixed_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        3u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q q;
    make_filled(q, 222u);
    const auto before_values = value_snapshot(q);
    const auto before_ptrs = pointer_snapshot(q);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(2u);
    QVERIFY(!q.resize(reg{7u}));
    verify_invariants(q);
    QVERIFY(value_snapshot(q) == before_values);
    QVERIFY(pointer_snapshot(q) == before_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    QVERIFY(configure(q, grown_count<Q>(), grown_size<Q>()));
    verify_invariants(q);
    expect_prefix_and_zero_tail(q, before_values);
}

template <class Tag>
static void failure_resize_suite_fully_dynamic()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q q;
    make_filled(q, 333u);
    const auto before_values = value_snapshot(q);
    const auto before_ptrs = pointer_snapshot(q);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(2u);
    QVERIFY(!q.resize(reg{6u}, reg{7u}));
    verify_invariants(q);
    QVERIFY(value_snapshot(q) == before_values);
    QVERIFY(pointer_snapshot(q) == before_ptrs);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    QVERIFY(configure(q, grown_count<Q>(), grown_size<Q>()));
    verify_invariants(q);
    expect_prefix_and_zero_tail(q, before_values);
}

template <class Tag>
static void failure_copy_assignment_suite_fixed_buffer_dynamic_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        5u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q src;
    Q dst;
    make_filled(src, 1000u);
    make_filled(dst, 2000u);
    const auto dst_before = value_snapshot(dst);
    const auto ptrs_before = pointer_snapshot(dst);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(1u);
    dst = src;
    verify_invariants(dst);
    QVERIFY(value_snapshot(dst) == dst_before);
    QVERIFY(pointer_snapshot(dst) == ptrs_before);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    dst = src;
    verify_invariants(dst);
    QVERIFY(value_snapshot(dst) == value_snapshot(src));
}

template <class Tag>
static void failure_copy_assignment_suite_dynamic_buffer_fixed_count()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        3u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q src;
    Q dst;
    make_filled(src, 3000u);
    make_filled(dst, 4000u);
    const auto dst_before = value_snapshot(dst);
    const auto ptrs_before = pointer_snapshot(dst);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(2u);
    dst = src;
    verify_invariants(dst);
    QVERIFY(value_snapshot(dst) == dst_before);
    QVERIFY(pointer_snapshot(dst) == ptrs_before);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    dst = src;
    verify_invariants(dst);
    QVERIFY(value_snapshot(dst) == value_snapshot(src));
}

template <class Tag>
static void failure_copy_assignment_suite_fully_dynamic()
{
    using Q = ::spsc::buffer_pool<
        Cell,
        0u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, Tag>>;

    alloc_control<Tag>::reset();
    Q src;
    Q dst;
    make_filled(src, 5000u);
    make_filled(dst, 6000u);
    const auto dst_before = value_snapshot(dst);
    const auto ptrs_before = pointer_snapshot(dst);
    const auto live_before = alloc_control<Tag>::bytes_live.load();

    alloc_control<Tag>::arm_fail_after_existing(2u);
    dst = src;
    verify_invariants(dst);
    QVERIFY(value_snapshot(dst) == dst_before);
    QVERIFY(pointer_snapshot(dst) == ptrs_before);
    QCOMPARE(alloc_control<Tag>::bytes_live.load(), live_before);

    alloc_control<Tag>::fail_on_call.store(-1);
    dst = src;
    verify_invariants(dst);
    QVERIFY(value_snapshot(dst) == value_snapshot(src));
}

static void allocator_accounting_suite()
{
    struct FixedCountTag {};
    struct RuntimeCountTag {};
    struct DynamicTag {};

    using QFixedCount = ::spsc::buffer_pool<
        Cell,
        5u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, FixedCountTag>>;
    using QRuntimeCount = ::spsc::buffer_pool<
        Cell,
        0u,
        3u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, RuntimeCountTag>>;
    using QDynamic = ::spsc::buffer_pool<
        Cell,
        0u,
        0u,
        ::spsc::policy::CA<>,
        ControlledAlignedAlloc<Cell, 64u, DynamicTag>>;

    alloc_control<FixedCountTag>::reset();
    {
        QFixedCount q;
        QVERIFY(q.resize(4u));
        verify_invariants(q);
        QCOMPARE(alloc_control<FixedCountTag>::alloc_calls.load(), std::size_t{1u});
        QCOMPARE(alloc_control<FixedCountTag>::dealloc_calls.load(), std::size_t{0u});
        QCOMPARE(alloc_control<FixedCountTag>::bytes_live.load(),
                 static_cast<std::size_t>(q.count()) * static_cast<std::size_t>(q.span_bytes()));
        q.destroy();
        QCOMPARE(alloc_control<FixedCountTag>::bytes_live.load(), std::size_t{0u});
        QCOMPARE(alloc_control<FixedCountTag>::dealloc_calls.load(), std::size_t{1u});
    }

    alloc_control<RuntimeCountTag>::reset();
    {
        QRuntimeCount q;
        QVERIFY(q.resize(4u));
        verify_invariants(q);
        // One transient pointer table plus one persistent allocation per
        // fixed-count payload. The table is released before resize returns.
        QCOMPARE(alloc_control<RuntimeCountTag>::alloc_calls.load(), std::size_t{4u});
        QCOMPARE(alloc_control<RuntimeCountTag>::dealloc_calls.load(), std::size_t{1u});
        QCOMPARE(alloc_control<RuntimeCountTag>::bytes_live.load(),
                 static_cast<std::size_t>(q.count()) * static_cast<std::size_t>(q.span_bytes()));
        q.destroy();
        QCOMPARE(alloc_control<RuntimeCountTag>::bytes_live.load(), std::size_t{0u});
        QCOMPARE(alloc_control<RuntimeCountTag>::dealloc_calls.load(), std::size_t{4u});
    }

    alloc_control<DynamicTag>::reset();
    {
        QDynamic q;
        QVERIFY(q.resize(4u, 4u));
        verify_invariants(q);
        QCOMPARE(alloc_control<DynamicTag>::alloc_calls.load(), std::size_t{5u});
        QCOMPARE(alloc_control<DynamicTag>::dealloc_calls.load(), std::size_t{0u});
        const std::size_t expected_live =
            static_cast<std::size_t>(q.count()) * static_cast<std::size_t>(q.span_bytes())
            + static_cast<std::size_t>(q.count()) * sizeof(typename QDynamic::pointer);
        QCOMPARE(alloc_control<DynamicTag>::bytes_live.load(), expected_live);
        q.destroy();
        QCOMPARE(alloc_control<DynamicTag>::bytes_live.load(), std::size_t{0u});
        QCOMPARE(alloc_control<DynamicTag>::dealloc_calls.load(), std::size_t{5u});
    }
}

static void overflow_suite()
{
    using QFixedCount = ::spsc::buffer_pool<Cell, 0u, 3u, ::spsc::policy::P>;
    using QDynamic = ::spsc::buffer_pool<Cell, 0u, 0u, ::spsc::policy::P>;

    const reg overflow_size =
        static_cast<reg>((std::numeric_limits<reg>::max)() / sizeof(Cell)) + reg{1u};

    {
        QFixedCount q;
        make_filled(q, 111u);
        const auto before = value_snapshot(q);
        QVERIFY(!q.resize(overflow_size));
        verify_invariants(q);
        QVERIFY(value_snapshot(q) == before);
    }

    {
        QDynamic q;
        make_filled(q, 222u);
        const auto before = value_snapshot(q);
        QVERIFY(!q.resize(requested_count<QDynamic>(), overflow_size));
        verify_invariants(q);
        QVERIFY(value_snapshot(q) == before);
    }
}

static void alignment_suite_default()
{
    using QStaticP = ::spsc::buffer_pool<std::byte, 13u, 3u, ::spsc::policy::P>;
    using QStaticCA = ::spsc::buffer_pool<std::byte, 13u, 3u, ::spsc::policy::CA<>>;
    using QRuntimeCA = ::spsc::buffer_pool<std::byte, 0u, 3u, ::spsc::policy::CA<>>;
    using QDynamicCA = ::spsc::buffer_pool<std::byte, 0u, 0u, ::spsc::policy::CA<>>;

    constexpr auto kCAAlign = static_cast<reg>(
        ::spsc::alloc::policy_allocator_alignment_v<::spsc::policy::CA<>>);
    constexpr auto kCAStorageSpan13 = static_cast<reg>(sizeof(typename QStaticCA::stored_buffer_type));
    constexpr auto kCAAllocatedSpan13 = static_cast<reg>(
        ::spsc::alloc::round_up_size_for_policy<
            ::spsc::policy::CA<>,
            typename QStaticCA::base_allocator_type>(13u));

    static_assert(QStaticP::size_bytes() == 13u);
    static_assert(QStaticP::span_bytes() == 13u);
    static_assert(kCAAlign == ::spsc::hw::cacheline_bytes);
    static_assert(QStaticCA::span_bytes() == kCAStorageSpan13);
    static_assert(QStaticCA::alignment() == kCAAlign);

    QRuntimeCA q1{13u};
    verify_invariants(q1);
    QCOMPARE(q1.size_bytes(), reg{13u});
    QCOMPARE(q1.span_bytes(), kCAAllocatedSpan13);
    for (reg i = 0u; i < q1.count(); ++i) {
        QVERIFY((reinterpret_cast<std::uintptr_t>(q1.data(i)) % kCAAlign) == 0u);
    }

    QDynamicCA q2{4u, 13u};
    verify_invariants(q2);
    QCOMPARE(q2.size_bytes(), reg{13u});
    QCOMPARE(q2.span_bytes(), kCAAllocatedSpan13);
    for (reg i = 0u; i < q2.count(); ++i) {
        QVERIFY((reinterpret_cast<std::uintptr_t>(q2.data(i)) % kCAAlign) == 0u);
    }
}

static void alignment_suite_aligned_allocator_128()
{
    using QFixedBuffer = ::spsc::buffer_pool<std::byte, 13u, 0u, ::spsc::policy::P, ::spsc::alloc::align_alloc<128u>>;
    using QFixedCount = ::spsc::buffer_pool<std::byte, 0u, 3u, ::spsc::policy::P, ::spsc::alloc::align_alloc<128u>>;
    using QDynamic = ::spsc::buffer_pool<std::byte, 0u, 0u, ::spsc::policy::P, ::spsc::alloc::align_alloc<128u>>;

    {
        QFixedBuffer q;
        QVERIFY(q.resize(4u));
        verify_invariants(q);
        QCOMPARE(q.span_bytes(), reg{13u});
        QVERIFY((reinterpret_cast<std::uintptr_t>(q.data(0u)) % 128u) == 0u);
    }

    {
        QFixedCount q{13u};
        verify_invariants(q);
        QCOMPARE(q.span_bytes(), reg{13u});
        for (reg i = 0u; i < q.count(); ++i) {
            QVERIFY((reinterpret_cast<std::uintptr_t>(q.data(i)) % 128u) == 0u);
        }
    }

    {
        QDynamic q{4u, 13u};
        verify_invariants(q);
        QCOMPARE(q.span_bytes(), reg{13u});
        for (reg i = 0u; i < q.count(); ++i) {
            QVERIFY((reinterpret_cast<std::uintptr_t>(q.data(i)) % 128u) == 0u);
        }
    }
}

static void alignment_rounding_matrix_suite()
{
    constexpr std::array<reg, 16u> kSizes{
        0u, 1u, 2u, 3u, 7u, 8u, 15u, 16u, 31u, 32u, 63u, 64u, 65u, 127u, 128u, 129u};

    using QFixedP = ::spsc::buffer_pool<std::byte, 0u, 3u, ::spsc::policy::P>;
    using QFixedCA = ::spsc::buffer_pool<std::byte, 0u, 3u, ::spsc::policy::CA<>>;
    using QDynamicP = ::spsc::buffer_pool<std::byte, 0u, 0u, ::spsc::policy::P>;
    using QDynamicCA = ::spsc::buffer_pool<std::byte, 0u, 0u, ::spsc::policy::CA<>>;
    using QDynamicP128 =
        ::spsc::buffer_pool<std::byte, 0u, 0u, ::spsc::policy::P, ::spsc::alloc::align_alloc<128u>>;

    for (const reg logical_size : kSizes) {
        {
            QFixedP q{logical_size};
            verify_invariants(q);
            QCOMPARE(q.size_bytes(), q.size());
            QCOMPARE(q.span_bytes(), q.size_bytes());
        }

        {
            QFixedCA q{logical_size};
            verify_invariants(q);
            QCOMPARE(q.size_bytes(), q.size());
            QCOMPARE(q.span_bytes(),
                     (q.size() == 0u) ? reg{0u} : expected_runtime_span_bytes<QFixedCA>(q.size()));
            if (q.size() != 0u) {
                for (reg i = 0u; i < q.count(); ++i) {
                    QVERIFY((reinterpret_cast<std::uintptr_t>(q.data(i)) % q.alignment()) == 0u);
                }
            }
        }

        {
            QDynamicP q{reg{4u}, logical_size};
            verify_invariants(q);
            QCOMPARE(q.size_bytes(), q.size());
            QCOMPARE(q.span_bytes(), q.size_bytes());
        }

        {
            QDynamicCA q{reg{4u}, logical_size};
            verify_invariants(q);
            QCOMPARE(q.size_bytes(), q.size());
            QCOMPARE(q.span_bytes(),
                     (q.size() == 0u) ? reg{0u} : expected_runtime_span_bytes<QDynamicCA>(q.size()));
            if (q.size() != 0u) {
                for (reg i = 0u; i < q.count(); ++i) {
                    QVERIFY((reinterpret_cast<std::uintptr_t>(q.data(i)) % q.alignment()) == 0u);
                }
            }
        }

        {
            QDynamicP128 q{reg{4u}, logical_size};
            verify_invariants(q);
            QCOMPARE(q.size_bytes(), q.size());
            QCOMPARE(q.span_bytes(), q.size_bytes());
            if (q.size() != 0u) {
                for (reg i = 0u; i < q.count(); ++i) {
                    QVERIFY((reinterpret_cast<std::uintptr_t>(q.data(i)) % 128u) == 0u);
                }
            }
        }
    }
}

static void extended_policy_smoke_suite()
{
    ::spsc::test::for_each_extended_nonthreaded_policy([](auto policy_tag) {
        using Policy = typename decltype(policy_tag)::type;
        using QS = ::spsc::buffer_pool<Cell, 5u, 3u, Policy>;
        using QF = ::spsc::buffer_pool<Cell, 5u, 0u, Policy>;
        using QC = ::spsc::buffer_pool<Cell, 0u, 3u, Policy>;
        using QD = ::spsc::buffer_pool<Cell, 0u, 0u, Policy>;

        QS qs;
        verify_invariants(qs);
        fill_pattern(qs, 10u);
        expect_pattern(qs, 10u);

        QF qf;
        QVERIFY(qf.resize(3u));
        verify_invariants(qf);
        fill_pattern(qf, 20u);
        expect_pattern(qf, 20u);

        QC qc;
        QVERIFY(qc.resize(4u));
        verify_invariants(qc);
        fill_pattern(qc, 30u);
        expect_pattern(qc, 30u);

        QD qd;
        QVERIFY(qd.resize(3u, 4u));
        verify_invariants(qd);
        fill_pattern(qd, 40u);
        expect_pattern(qd, 40u);
    });
}

template <class Q>
static void state_machine_fuzz_suite(const std::uint32_t seed)
{
    static_assert(!kFullyStatic<Q>);

    std::mt19937 rng(seed);
    Q a{};
    Q b{};
    buffer_model model_a = empty_model_for<Q>();
    buffer_model model_b = empty_model_for<Q>();

    expect_model(a, model_a, "initial a");
    expect_model(b, model_b, "initial b");

    const auto random_count = [&]() -> reg {
        if constexpr (kFixedBufferDynamicCount<Q> || kFullyDynamic<Q>) {
            return static_cast<reg>(rng() % 7u);
        } else {
            return Q::static_count;
        }
    };

    const auto random_size = [&]() -> reg {
        if constexpr (kDynamicBufferFixedCount<Q> || kFullyDynamic<Q>) {
            return static_cast<reg>(rng() % 9u);
        } else {
            return Q::static_buffer_size;
        }
    };

    const auto do_resize = [&](Q& q, buffer_model& model) {
        const buffer_model before = model;
        QVERIFY(configure(q, random_count(), random_size()));
        model = resized_model_like(before, q.count(), q.size());
        expect_model(q, model, "resize");
    };

    const auto do_mutate = [&](Q& q, buffer_model& model) {
        if ((model.count == 0u) || (model.size == 0u)) {
            expect_model(q, model, "mutate-empty");
            return;
        }

        const reg i = static_cast<reg>(rng() % static_cast<std::uint32_t>(model.count));
        const reg j = static_cast<reg>(rng() % static_cast<std::uint32_t>(model.size));
        const std::uint32_t value = rng();
        q[i][j].value = value;
        model.values[static_cast<std::size_t>(i)][static_cast<std::size_t>(j)] = value;
        expect_model(q, model, "mutate");
    };

    const auto do_fill = [&](Q& q, buffer_model& model) {
        const std::uint32_t base = rng();
        fill_pattern(q, base);
        fill_model_pattern(model, base);
        expect_model(q, model, "fill");
    };

    const auto do_copy_ctor_probe = [&](const Q& q, const buffer_model& model) {
        Q copy{q};
        expect_model(copy, model, "copy-ctor");
    };

    const auto do_copy_assign = [&](Q& dst, buffer_model& dst_model, const Q& src, const buffer_model& src_model) {
        dst = src;
        dst_model = src_model;
        expect_model(dst, dst_model, "copy-assign");
    };

    const auto do_move_roundtrip = [&](Q& q, buffer_model& model) {
        const buffer_model before = model;
        const buffer_model empty = empty_model_for<Q>();
        Q tmp{std::move(q)};
        expect_model(tmp, before, "move-ctor tmp");
        expect_model(q, empty, "move-ctor src");

        q = std::move(tmp);
        model = before;
        expect_model(q, model, "move-roundtrip dst");
        expect_model(tmp, empty, "move-roundtrip tmp");
    };

    const auto do_move_assign = [&](Q& dst, buffer_model& dst_model, Q& src, buffer_model& src_model) {
        const buffer_model src_before = src_model;
        const buffer_model empty = empty_model_for<Q>();
        dst = std::move(src);
        dst_model = src_before;
        src_model = empty;
        expect_model(dst, dst_model, "move-assign dst");
        expect_model(src, src_model, "move-assign src");
    };

    const auto do_same_resize = [&](Q& q, buffer_model& model) {
        const auto ptrs_before = pointer_snapshot(q);
        const buffer_model before = model;
        QVERIFY(configure(q, q.count(), q.size()));
        expect_model(q, before, "same-resize");
        QVERIFY(pointer_snapshot(q) == ptrs_before);
    };

    for (int it = 0; it < kStateMachineIters; ++it) {
        const bool pick_a = (rng() & 1u) == 0u;
        Q& q = pick_a ? a : b;
        Q& other = pick_a ? b : a;
        buffer_model& model = pick_a ? model_a : model_b;
        buffer_model& other_model = pick_a ? model_b : model_a;

        switch (rng() % 11u) {
        case 0u:
            do_resize(q, model);
            break;
        case 1u:
            q.destroy();
            model = empty_model_for<Q>();
            expect_model(q, model, "destroy");
            break;
        case 2u:
            do_mutate(q, model);
            break;
        case 3u:
            do_fill(q, model);
            break;
        case 4u:
            if ((rng() & 1u) == 0u) {
                do_copy_assign(q, model, other, other_model);
            } else {
                do_copy_assign(other, other_model, q, model);
            }
            break;
        case 5u:
            do_copy_ctor_probe(q, model);
            do_copy_ctor_probe(other, other_model);
            break;
        case 6u:
            q.swap(other);
            std::swap(model, other_model);
            expect_model(q, model, "swap selected");
            expect_model(other, other_model, "swap other");
            break;
        case 7u:
            q = q;
            q.swap(q);
            expect_model(q, model, "self-ops");
            break;
        case 8u:
            do_move_roundtrip(q, model);
            break;
        case 9u:
            if ((rng() & 1u) == 0u) {
                do_move_assign(q, model, other, other_model);
            } else {
                do_move_assign(other, other_model, q, model);
            }
            break;
        case 10u:
            do_same_resize(q, model);
            break;
        default:
            Q_UNREACHABLE();
        }

        expect_model(a, model_a, "post-a");
        expect_model(b, model_b, "post-b");
    }
}

static void state_machine_fuzz_sweep_suite()
{
    state_machine_fuzz_suite<::spsc::buffer_pool<Cell, 5u, 0u, ::spsc::policy::P>>(0xB001u);
    state_machine_fuzz_suite<::spsc::buffer_pool<Cell, 5u, 0u, ::spsc::policy::CA<>>>(0xB002u);
    state_machine_fuzz_suite<::spsc::buffer_pool<Cell, 0u, 3u, ::spsc::policy::P>>(0xB003u);
    state_machine_fuzz_suite<::spsc::buffer_pool<Cell, 0u, 3u, ::spsc::policy::CA<>>>(0xB004u);
    state_machine_fuzz_suite<::spsc::buffer_pool<Cell, 0u, 0u, ::spsc::policy::P>>(0xB005u);
    state_machine_fuzz_suite<::spsc::buffer_pool<Cell, 0u, 0u, ::spsc::policy::CA<>>>(0xB006u);
}

#if !defined(NDEBUG)
namespace spsc_buffer_pool_death_detail {

static constexpr int kDeathExitCode = 0xB4;

static void sigabrt_handler_(int) noexcept
{
    std::_Exit(kDeathExitCode);
}

[[noreturn]] static void run_case_(const char* mode)
{
    std::signal(SIGABRT, &sigabrt_handler_);

    if (std::strcmp(mode, "fixed_static_oob") == 0) {
        using Q = ::spsc::buffer_pool<Cell, 5u, 3u, ::spsc::policy::P>;
        Q q;
        (void)q[3u];
    } else if (std::strcmp(mode, "fixed_buffer_empty_index") == 0) {
        using Q = ::spsc::buffer_pool<Cell, 5u, 0u, ::spsc::policy::P>;
        Q q;
        (void)q[0u];
    } else if (std::strcmp(mode, "fixed_count_empty_index") == 0) {
        using Q = ::spsc::buffer_pool<Cell, 0u, 3u, ::spsc::policy::P>;
        Q q;
        (void)q[0u];
    } else if (std::strcmp(mode, "fully_dynamic_empty_index") == 0) {
        using Q = ::spsc::buffer_pool<Cell, 0u, 0u, ::spsc::policy::P>;
        Q q;
        (void)q[0u];
    } else {
        std::_Exit(0xEF);
    }

    std::_Exit(0xF0);
}

struct Runner_ {
    Runner_()
    {
        const char* mode = std::getenv("SPSC_BUFFER_POOL_DEATH");
        if (mode && *mode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            run_case_(mode);
        }
    }
};

static const Runner_ g_runner_{};

} // namespace spsc_buffer_pool_death_detail

static void death_tests_debug_only_suite()
{
    QString blockedReason;

    const auto expect_death = [&](const char* mode) {
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SPSC_BUFFER_POOL_DEATH"), QString::fromUtf8(mode));
        p.setProcessEnvironment(env);
        p.setProgram(QCoreApplication::applicationFilePath());
        p.setArguments(QCoreApplication::arguments().mid(1));
        p.start();

        const bool started = p.waitForStarted(5000);
#if defined(Q_OS_WIN)
        if (!started) {
            const QString err = p.errorString();
            if (p.error() == QProcess::FailedToStart
                || err.contains(QStringLiteral("Access is denied"), Qt::CaseInsensitive)
                || err.contains(QStringLiteral("CreateFile failed"), Qt::CaseInsensitive)) {
                blockedReason = QStringLiteral("Death child launch blocked by environment: %1").arg(err);
                return;
            }
            QVERIFY2(false, qPrintable(QStringLiteral("Death child failed to start: %1").arg(err)));
        }
#else
        QVERIFY2(started, "Death child failed to start.");
#endif

        if (!p.waitForFinished(8000)) {
            p.kill();
            QVERIFY2(false, "Death child did not finish.");
        }

        const int code = p.exitCode();
        const QString detail = QStringLiteral("Expected assertion death. exit=%1 status=%2 error=%3")
                                   .arg(code)
                                   .arg(static_cast<int>(p.exitStatus()))
                                   .arg(p.errorString());
        QVERIFY2(code == spsc_buffer_pool_death_detail::kDeathExitCode, qPrintable(detail));
    };

#define SPSC_EXPECT_DEATH_CASE(mode_literal) \
    do { expect_death(mode_literal); if (!blockedReason.isEmpty()) { QSKIP(qPrintable(blockedReason)); } } while (0)

    SPSC_EXPECT_DEATH_CASE("fixed_static_oob");
    SPSC_EXPECT_DEATH_CASE("fixed_buffer_empty_index");
    SPSC_EXPECT_DEATH_CASE("fixed_count_empty_index");
    SPSC_EXPECT_DEATH_CASE("fully_dynamic_empty_index");

#undef SPSC_EXPECT_DEATH_CASE
}
#endif

} // namespace buffer_pool_test_detail

class tst_buffer_pool_api_paranoid final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        SPSC_TEST_VERIFY_BUILD_CONFIG("buffer_pool");
        buffer_pool_test_detail::api_compile_smoke_all();
    }

    void static_plain_P()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 3u, ::spsc::policy::P>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
    }

    void static_atomic_A()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 3u, ::spsc::policy::A<>>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
    }

    void static_cached_CA()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 3u, ::spsc::policy::CA<>>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
    }

    void fixed_buffer_dynamic_count_plain_P()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 0u, ::spsc::policy::P>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
        buffer_pool_test_detail::resize_suite<Q>();
    }

    void fixed_buffer_dynamic_count_cached_CA()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 0u, ::spsc::policy::CA<>>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
        buffer_pool_test_detail::resize_suite<Q>();
    }

    void dynamic_buffer_fixed_count_plain_P()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 3u, ::spsc::policy::P>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
        buffer_pool_test_detail::resize_suite<Q>();
    }

    void dynamic_buffer_fixed_count_cached_CA()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 3u, ::spsc::policy::CA<>>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
        buffer_pool_test_detail::resize_suite<Q>();
    }

    void fully_dynamic_plain_P()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 0u, ::spsc::policy::P>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
        buffer_pool_test_detail::resize_suite<Q>();
    }

    void fully_dynamic_cached_CA()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 0u, ::spsc::policy::CA<>>;
        buffer_pool_test_detail::ctor_contracts_suite<Q>();
        buffer_pool_test_detail::indexing_suite<Q>();
        buffer_pool_test_detail::copy_move_swap_suite<Q>();
        buffer_pool_test_detail::resize_suite<Q>();
    }

    void tracked_lifetime_static()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::TrackedCell, 5u, 3u, ::spsc::policy::P>;
        buffer_pool_test_detail::tracked_lifetime_suite<Q>();
    }

    void tracked_lifetime_fixed_buffer_dynamic_count()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::TrackedCell, 5u, 0u, ::spsc::policy::P>;
        buffer_pool_test_detail::tracked_lifetime_suite<Q>();
    }

    void tracked_lifetime_dynamic_buffer_fixed_count()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::TrackedCell, 0u, 3u, ::spsc::policy::P>;
        buffer_pool_test_detail::tracked_lifetime_suite<Q>();
    }

    void tracked_lifetime_fully_dynamic()
    {
        using Q = ::spsc::buffer_pool<buffer_pool_test_detail::TrackedCell, 0u, 0u, ::spsc::policy::P>;
        buffer_pool_test_detail::tracked_lifetime_suite<Q>();
    }

    void alignment_default_alloc() { buffer_pool_test_detail::alignment_suite_default(); }
    void alignment_align_alloc_128() { buffer_pool_test_detail::alignment_suite_aligned_allocator_128(); }
    void alignment_rounding_matrix() { buffer_pool_test_detail::alignment_rounding_matrix_suite(); }

    void allocator_accounting() { buffer_pool_test_detail::allocator_accounting_suite(); }

    void copy_move_aliasing_corner_cases()
    {
        using QS = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 3u, ::spsc::policy::P>;
        using QF = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 0u, ::spsc::policy::P>;
        using QFC = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 5u, 0u, ::spsc::policy::CA<>>;
        using QC = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 3u, ::spsc::policy::P>;
        using QCC = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 3u, ::spsc::policy::CA<>>;
        using QD = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 0u, ::spsc::policy::P>;
        using QDC = ::spsc::buffer_pool<buffer_pool_test_detail::Cell, 0u, 0u, ::spsc::policy::CA<>>;

        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QS>();
        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QF>();
        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QFC>();
        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QC>();
        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QCC>();
        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QD>();
        buffer_pool_test_detail::copy_move_aliasing_corner_cases_suite<QDC>();
    }

    void failure_ctor_paths()
    {
        struct FixedCtorTag {};
        struct RuntimeCtorTag {};
        struct DynamicCtorTag {};
        buffer_pool_test_detail::failure_ctor_suite_fixed_buffer_dynamic_count<FixedCtorTag>();
        buffer_pool_test_detail::failure_ctor_suite_dynamic_buffer_fixed_count<RuntimeCtorTag>();
        buffer_pool_test_detail::failure_ctor_suite_fully_dynamic<DynamicCtorTag>();
    }

    void failure_copy_constructor_paths()
    {
        struct FixedCopyCtorTag {};
        struct RuntimeCopyCtorTag {};
        struct DynamicCopyCtorTag {};
        buffer_pool_test_detail::failure_copy_ctor_suite_fixed_buffer_dynamic_count<FixedCopyCtorTag>();
        buffer_pool_test_detail::failure_copy_ctor_suite_dynamic_buffer_fixed_count<RuntimeCopyCtorTag>();
        buffer_pool_test_detail::failure_copy_ctor_suite_fully_dynamic<DynamicCopyCtorTag>();
    }

    void failure_resize_paths()
    {
        struct FixedResizeTag {};
        struct RuntimeResizeTag {};
        struct DynamicResizeTag {};
        buffer_pool_test_detail::failure_resize_suite_fixed_buffer_dynamic_count<FixedResizeTag>();
        buffer_pool_test_detail::failure_resize_suite_dynamic_buffer_fixed_count<RuntimeResizeTag>();
        buffer_pool_test_detail::failure_resize_suite_fully_dynamic<DynamicResizeTag>();
    }

    void failure_copy_assignment_paths()
    {
        struct FixedCopyTag {};
        struct RuntimeCopyTag {};
        struct DynamicCopyTag {};
        buffer_pool_test_detail::failure_copy_assignment_suite_fixed_buffer_dynamic_count<FixedCopyTag>();
        buffer_pool_test_detail::failure_copy_assignment_suite_dynamic_buffer_fixed_count<RuntimeCopyTag>();
        buffer_pool_test_detail::failure_copy_assignment_suite_fully_dynamic<DynamicCopyTag>();
    }

    void overflow_paths() { buffer_pool_test_detail::overflow_suite(); }

    void extended_policy_smoke() { buffer_pool_test_detail::extended_policy_smoke_suite(); }
    void state_machine_fuzz_sweep() { buffer_pool_test_detail::state_machine_fuzz_sweep_suite(); }

    void death_tests_debug_only()
    {
#if !defined(NDEBUG)
        buffer_pool_test_detail::death_tests_debug_only_suite();
#else
        QSKIP("Death tests are debug-only (assertions disabled).");
#endif
    }
};

int run_tst_buffer_pool_api_paranoid(int argc, char** argv)
{
    tst_buffer_pool_api_paranoid tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "buffer_pool_test.moc"

#endif // SPSC_TESTS_WITH_QT
