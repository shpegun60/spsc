#include "src/spsc/array_fifo.hpp"
#include "src/spsc/buffer_pool.hpp"
#include "src/spsc/chunk.hpp"
#include "src/spsc/chunk_fifo.hpp"
#include "src/spsc/fifo.hpp"
#include "src/spsc/latest.hpp"
#include "src/spsc/queue.hpp"
#include "src/spsc/typed_pool.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace {

struct bad_argument {};

struct value {
    int payload{0};

    value() noexcept = default;
    explicit value(const int v) noexcept : payload(v) {}
    value(const value&) noexcept = default;
    value& operator=(const value&) noexcept = default;
    value(value&&) noexcept = default;
    value& operator=(value&&) noexcept = default;
};

struct move_only {
    move_only() noexcept = default;
    move_only(const move_only&) = delete;
    move_only& operator=(const move_only&) = delete;
    move_only(move_only&&) noexcept = default;
    move_only& operator=(move_only&&) noexcept = default;
};

#if (SPSC_ENABLE_EXCEPTIONS != 0)
struct throwing_move_only {
    throwing_move_only() noexcept = default;
    throwing_move_only(const throwing_move_only&) = delete;
    throwing_move_only& operator=(const throwing_move_only&) = delete;
    throwing_move_only(throwing_move_only&&) noexcept(false) {}
    throwing_move_only& operator=(throwing_move_only&&) noexcept(false) {
        return *this;
    }
};
#endif

/* fifo copies by assignment, not by T's copy constructor. */
struct copy_assign_only {
    int payload{0};

    copy_assign_only() noexcept = default;
    copy_assign_only(const copy_assign_only&) = delete;
    copy_assign_only& operator=(const copy_assign_only&) noexcept = default;
    copy_assign_only(copy_assign_only&&) noexcept = default;
    copy_assign_only& operator=(copy_assign_only&&) noexcept = default;
};

struct copy_constructible {
    int payload{0};

    copy_constructible() noexcept = default;
    explicit copy_constructible(const int v) noexcept : payload(v) {}
    copy_constructible(const copy_constructible&) noexcept = default;
    copy_constructible& operator=(const copy_constructible&) noexcept = default;
    copy_constructible(copy_constructible&&) noexcept = default;
    copy_constructible& operator=(copy_constructible&&) noexcept = default;
};

/* Deliberately rejects ADL swap while retaining the assignment operations
 * required by static SPSC value storage. */
struct no_adl_swap {
    int payload{0};

    no_adl_swap() noexcept = default;
    no_adl_swap(const no_adl_swap&) noexcept = default;
    no_adl_swap& operator=(const no_adl_swap&) noexcept = default;
    no_adl_swap(no_adl_swap&&) noexcept = default;
    no_adl_swap& operator=(no_adl_swap&&) noexcept = default;

    friend void swap(no_adl_swap&, no_adl_swap&) = delete;
};

struct throwing_move_noexcept_copy {
    static inline unsigned move_assignments{0u};
    static inline unsigned copy_assignments{0u};

    int payload{0};

    throwing_move_noexcept_copy() noexcept = default;
    throwing_move_noexcept_copy(const throwing_move_noexcept_copy&) noexcept = default;
    throwing_move_noexcept_copy(throwing_move_noexcept_copy&&) noexcept(false) = default;

    throwing_move_noexcept_copy&
    operator=(const throwing_move_noexcept_copy& other) noexcept {
        ++copy_assignments;
        payload = other.payload;
        return *this;
    }

    throwing_move_noexcept_copy&
    operator=(throwing_move_noexcept_copy&& other) noexcept(false) {
        ++move_assignments;
        payload = other.payload;
        return *this;
    }

    friend void swap(throwing_move_noexcept_copy&,
                     throwing_move_noexcept_copy&) = delete;
};

struct swap_scratch_probe {
    static inline std::size_t live_count{0u};
    static inline std::size_t peak_live_count{0u};

    int payload{0};

    static void record_construction() noexcept {
        ++live_count;
        if (live_count > peak_live_count) {
            peak_live_count = live_count;
        }
    }

    swap_scratch_probe() noexcept { record_construction(); }

    swap_scratch_probe(const swap_scratch_probe& other) noexcept
        : payload(other.payload) {
        record_construction();
    }

    swap_scratch_probe(swap_scratch_probe&& other) noexcept
        : payload(other.payload) {
        record_construction();
    }

    swap_scratch_probe& operator=(const swap_scratch_probe& other) noexcept {
        payload = other.payload;
        return *this;
    }

    swap_scratch_probe& operator=(swap_scratch_probe&& other) noexcept {
        payload = other.payload;
        return *this;
    }

    ~swap_scratch_probe() noexcept { --live_count; }

    friend void swap(swap_scratch_probe&, swap_scratch_probe&) = delete;
};

struct class_specific_new_value {
    int payload{0};

    class_specific_new_value() noexcept = default;
    explicit class_specific_new_value(const int value) noexcept : payload(value) {}

    static void* operator new(std::size_t) = delete;
    class_specific_new_value* operator&() = delete;
    const class_specific_new_value* operator&() const = delete;
};

struct non_assignable_latest_value {
    non_assignable_latest_value() = default;
    non_assignable_latest_value(const non_assignable_latest_value&) = default;
    non_assignable_latest_value(non_assignable_latest_value&&) = default;

    non_assignable_latest_value&
    operator=(const non_assignable_latest_value&) = delete;

    non_assignable_latest_value&
    operator=(non_assignable_latest_value&&) = delete;

    friend void swap(non_assignable_latest_value&,
                     non_assignable_latest_value&) = delete;
};

struct non_trivial_payload {
    non_trivial_payload() noexcept = default;
    non_trivial_payload(const non_trivial_payload&) noexcept = default;
    ~non_trivial_payload() noexcept {}
};

template<class Q, class Arg, class = void>
struct has_emplace : std::false_type {};

template<class Q, class Arg>
struct has_emplace<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().emplace(std::declval<Arg>()))>>
    : std::true_type {};

template<class Q, class Arg, class = void>
struct has_try_emplace : std::false_type {};

template<class Q, class Arg>
struct has_try_emplace<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().try_emplace(std::declval<Arg>()))>>
    : std::true_type {};

template<class Q, class Arg, class = void>
struct has_emplace_next : std::false_type {};

template<class Q, class Arg>
struct has_emplace_next<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().emplace_next(std::declval<Arg>()))>>
    : std::true_type {};

template<class Q, class Arg, class = void>
struct has_push : std::false_type {};

template<class Q, class Arg>
struct has_push<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().push(std::declval<Arg>()))>>
    : std::true_type {};

template<class Q, class Arg, class = void>
struct has_try_push : std::false_type {};

template<class Q, class Arg>
struct has_try_push<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().try_push(std::declval<Arg>()))>>
    : std::true_type {};

template<class Q, class Arg, class = void>
struct has_push_back : std::false_type {};

template<class Q, class Arg>
struct has_push_back<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().push_back(std::declval<Arg>()))>>
    : std::true_type {};

template<class Q, class Arg, class = void>
struct has_emplace_back : std::false_type {};

template<class Q, class Arg>
struct has_emplace_back<
    Q, Arg,
    std::void_t<decltype(std::declval<Q&>().emplace_back(
        std::declval<Arg>()))>> : std::true_type {};

template<class Q, class = void>
struct exposes_storage : std::false_type {};

template<class Q>
struct exposes_storage<
    Q, std::void_t<decltype(std::declval<Q&>().storage_)>> : std::true_type {};

template<class Q, class = void>
struct exposes_slots : std::false_type {};

template<class Q>
struct exposes_slots<
    Q, std::void_t<decltype(std::declval<Q&>().slots_)>> : std::true_type {};

using static_latest = ::spsc::latest<value, 8u>;
using dynamic_latest = ::spsc::latest<value, 0u>;
using static_chunk = ::spsc::chunk<value, 8u>;
using dynamic_chunk = ::spsc::chunk<value, 0u>;
using typed = ::spsc::typed_pool<value, 8u>;
using move_only_typed = ::spsc::typed_pool<move_only, 8u>;
using object_queue = ::spsc::queue<value, 8u>;
using class_new_queue = ::spsc::queue<class_specific_new_value, 8u>;
using raw_latest = ::spsc::latest<void, 0u>;

static_assert(has_emplace<static_latest, int>::value);
static_assert(has_try_emplace<static_latest, int>::value);
static_assert(!has_emplace<static_latest, bad_argument>::value);
static_assert(!has_try_emplace<static_latest, bad_argument>::value);
static_assert(!has_push<static_latest, bad_argument>::value);
static_assert(!has_try_push<static_latest, bad_argument>::value);

static_assert(has_emplace<dynamic_latest, int>::value);
static_assert(has_try_emplace<dynamic_latest, int>::value);
static_assert(!has_emplace<dynamic_latest, bad_argument>::value);
static_assert(!has_try_emplace<dynamic_latest, bad_argument>::value);
static_assert(!has_push<dynamic_latest, bad_argument>::value);
static_assert(!has_try_push<dynamic_latest, bad_argument>::value);

static_assert(has_emplace<static_chunk, int>::value);
static_assert(has_try_emplace<static_chunk, int>::value);
static_assert(!has_emplace<static_chunk, bad_argument>::value);
static_assert(!has_try_emplace<static_chunk, bad_argument>::value);

static_assert(has_emplace<dynamic_chunk, int>::value);
static_assert(has_try_emplace<dynamic_chunk, int>::value);
static_assert(!has_emplace<dynamic_chunk, bad_argument>::value);
static_assert(!has_try_emplace<dynamic_chunk, bad_argument>::value);
static_assert(has_push_back<dynamic_chunk, value&&>::value);
static_assert(!has_push_back<dynamic_chunk, bad_argument>::value);
static_assert(has_emplace_back<dynamic_chunk, int>::value);
static_assert(!has_emplace_back<dynamic_chunk, bad_argument>::value);

static_assert(has_emplace<typed, int>::value);
static_assert(has_try_emplace<typed, int>::value);
static_assert(!has_emplace<typed, bad_argument>::value);
static_assert(!has_try_emplace<typed, bad_argument>::value);
static_assert(has_push<typed, const value&>::value);
static_assert(has_try_push<typed, value&&>::value);
static_assert(!has_push<move_only_typed, const move_only&>::value);
static_assert(!has_try_push<move_only_typed, const move_only&>::value);
static_assert(has_push<move_only_typed, move_only&&>::value);
static_assert(has_try_push<move_only_typed, move_only&&>::value);

static_assert(has_emplace<typename typed::write_guard, int>::value);
static_assert(!has_emplace<typename typed::write_guard, bad_argument>::value);
static_assert(has_emplace_next<typename typed::bulk_write_guard, int>::value);
static_assert(!has_emplace_next<typename typed::bulk_write_guard, bad_argument>::value);

static_assert(has_emplace<typename object_queue::write_guard, int>::value);
static_assert(!has_emplace<typename object_queue::write_guard, bad_argument>::value);
static_assert(has_emplace_next<typename object_queue::bulk_write_guard, int>::value);
static_assert(!has_emplace_next<typename object_queue::bulk_write_guard, bad_argument>::value);
static_assert(has_push<class_new_queue, int>::value);
static_assert(has_try_push<class_new_queue, int>::value);

static_assert(has_push<raw_latest, std::uint32_t>::value);
static_assert(has_try_push<raw_latest, std::uint32_t>::value);
static_assert(!has_push<raw_latest, non_trivial_payload&>::value);
static_assert(!has_try_push<raw_latest, non_trivial_payload&>::value);

using slot_for_move_only = ::spsc::detail::cache_aligned_slot<move_only, 64u>;
static_assert(!std::is_constructible_v<slot_for_move_only, const move_only&>);
static_assert(std::is_constructible_v<slot_for_move_only, move_only&&>);

using noncopyable_fifo = ::spsc::fifo<move_only, 8u>;
using assign_copy_fifo = ::spsc::fifo<copy_assign_only, 8u>;
using dynamic_assign_copy_fifo = ::spsc::fifo<copy_assign_only, 0u>;
using noncopyable_typed = ::spsc::typed_pool<move_only, 8u>;
using copyable_typed = ::spsc::typed_pool<copy_constructible, 8u>;
using dynamic_copyable_typed = ::spsc::typed_pool<copy_constructible, 0u>;
using noncopyable_array_fifo = ::spsc::array_fifo<move_only, 2u, 8u>;
using noncopyable_chunk_fifo = ::spsc::chunk_fifo<move_only, 2u, 8u>;

static_assert(!std::is_copy_constructible_v<noncopyable_fifo>);
static_assert(!std::is_copy_assignable_v<noncopyable_fifo>);
static_assert(std::is_copy_constructible_v<assign_copy_fifo>);
static_assert(std::is_copy_assignable_v<assign_copy_fifo>);
static_assert(std::is_copy_constructible_v<dynamic_assign_copy_fifo>);
static_assert(std::is_copy_assignable_v<dynamic_assign_copy_fifo>);
static_assert(!std::is_copy_constructible_v<noncopyable_typed>);
static_assert(!std::is_copy_assignable_v<noncopyable_typed>);
static_assert(std::is_copy_constructible_v<copyable_typed>);
static_assert(std::is_copy_assignable_v<copyable_typed>);
static_assert(std::is_copy_constructible_v<dynamic_copyable_typed>);
static_assert(std::is_copy_assignable_v<dynamic_copyable_typed>);
static_assert(!std::is_copy_constructible_v<noncopyable_array_fifo>);
static_assert(!std::is_copy_assignable_v<noncopyable_array_fifo>);
static_assert(!std::is_copy_constructible_v<noncopyable_chunk_fifo>);
static_assert(!std::is_copy_assignable_v<noncopyable_chunk_fifo>);
static_assert(!exposes_storage<assign_copy_fifo>::value);
static_assert(!exposes_slots<copyable_typed>::value);

#if (SPSC_ENABLE_EXCEPTIONS != 0)
using throwing_move_fifo = ::spsc::fifo<throwing_move_only, 8u>;
using non_assignable_latest = ::spsc::latest<non_assignable_latest_value, 8u>;
using non_assignable_aligned_slot =
    ::spsc::detail::cache_aligned_slot<non_assignable_latest_value, 64u>;
static_assert(!std::is_copy_constructible_v<throwing_move_fifo>);
static_assert(!std::is_copy_assignable_v<throwing_move_fifo>);
static_assert(std::is_move_constructible_v<throwing_move_fifo>);
static_assert(!std::is_nothrow_move_constructible_v<throwing_move_fifo>);
static_assert(std::is_same_v<
              decltype(std::move_if_noexcept(
                  std::declval<throwing_move_fifo&>())),
              throwing_move_fifo&&>);
static_assert(!std::is_move_constructible_v<non_assignable_latest>);
static_assert(!std::is_move_assignable_v<non_assignable_latest>);
static_assert(!std::is_swappable_v<non_assignable_latest>);
static_assert(!std::is_swappable_v<non_assignable_aligned_slot>);
#endif

using swap_fifo = ::spsc::fifo<no_adl_swap, 8u>;
using swap_latest = ::spsc::latest<no_adl_swap, 8u>;
using swap_chunk = ::spsc::chunk<no_adl_swap, 8u>;
using swap_pool = ::spsc::buffer_pool<no_adl_swap, 2u, 8u>;
using swap_array_fifo = ::spsc::array_fifo<no_adl_swap, 2u, 8u>;
using swap_chunk_fifo = ::spsc::chunk_fifo<no_adl_swap, 2u, 8u>;

static_assert(!std::is_swappable_v<no_adl_swap>);
static_assert(std::is_swappable_v<swap_fifo>);
static_assert(std::is_swappable_v<swap_latest>);
static_assert(std::is_move_constructible_v<swap_latest>);
static_assert(std::is_move_assignable_v<swap_latest>);
using swap_latest_member_pointer =
    void (swap_latest::*)(swap_latest&) noexcept;
static_assert(std::is_same_v<decltype(&swap_latest::swap),
                             swap_latest_member_pointer>);
static_assert(std::is_swappable_v<swap_chunk>);
static_assert(std::is_swappable_v<swap_pool>);
static_assert(std::is_swappable_v<swap_array_fifo>);
static_assert(std::is_swappable_v<swap_chunk_fifo>);
static_assert(!std::is_swappable_v<throwing_move_noexcept_copy>);
static_assert(!::spsc::detail::fallback_value_swap_uses_move_v<
              throwing_move_noexcept_copy>);
static_assert(::spsc::detail::value_swap_noexcept_v<
              throwing_move_noexcept_copy>);
using scratch_array = std::array<swap_scratch_probe, 8u>;
static_assert(::spsc::detail::value_swappable_v<scratch_array>);
static_assert(::spsc::detail::value_swap_noexcept_v<scratch_array>);
using zero_non_assignable_array =
    std::array<non_assignable_latest_value, 0u>;
static_assert(::spsc::detail::value_swappable_v<zero_non_assignable_array>);
static_assert(::spsc::detail::value_swap_noexcept_v<zero_non_assignable_array>);
using aligned_scratch_slot =
    ::spsc::detail::cache_aligned_slot<scratch_array, 64u>;
using aligned_scratch_storage = std::array<aligned_scratch_slot, 2u>;
static_assert(std::is_swappable_v<aligned_scratch_slot>);
static_assert(std::is_nothrow_swappable_v<aligned_scratch_slot>);
static_assert(::spsc::detail::value_swap_noexcept_v<aligned_scratch_storage>);

struct custom_counter {
    using value_type = reg;

    value_type value{0u};

    constexpr custom_counter() noexcept = default;
    void store(const value_type next) noexcept { value = next; }
    [[nodiscard]] value_type load() const noexcept { return value; }
    [[nodiscard]] value_type load_relaxed() const noexcept { return value; }
    void add(const value_type delta) noexcept { value += delta; }
    void inc() noexcept { ++value; }
};

struct custom_counter_without_relaxed_load {
    using value_type = reg;

    value_type value{0u};

    constexpr custom_counter_without_relaxed_load() noexcept = default;
    void store(const value_type next) noexcept { value = next; }
    [[nodiscard]] value_type load() const noexcept { return value; }
    void add(const value_type delta) noexcept { value += delta; }
    void inc() noexcept { ++value; }
};

struct no_value_counter {
    void store(const reg) noexcept {}
    [[nodiscard]] reg load() const noexcept { return 0u; }
    void add(const reg) noexcept {}
    void inc() noexcept {}
};

struct throwing_counter {
    using value_type = reg;

    void store(const value_type) noexcept {}
    [[nodiscard]] value_type load() const noexcept { return 0u; }
    void add(const value_type) noexcept {}
    void inc() {}
};

struct wrong_relaxed_counter {
    using value_type = reg;

    void store(const value_type) noexcept {}
    [[nodiscard]] value_type load() const noexcept { return 0u; }
    [[nodiscard]] int load_relaxed() const { return 0; }
    void add(const value_type) noexcept {}
    void inc() noexcept {}
};

struct signed_counter {
    using value_type = int;

    void store(const value_type) noexcept {}
    [[nodiscard]] value_type load() const noexcept { return 0; }
    void add(const value_type) noexcept {}
    void inc() noexcept {}
};

using custom_policy = ::spsc::policy::Policy<custom_counter>;
using custom_policy_without_relaxed_load =
    ::spsc::policy::Policy<custom_counter_without_relaxed_load>;

struct custom_aligned_policy : custom_policy {
    static constexpr reg allocator_alignment = 32u;
};

enum class custom_alignment : unsigned { bytes = 32u };

struct custom_enum_aligned_policy : custom_policy {
    static constexpr custom_alignment allocator_alignment =
        custom_alignment::bytes;
};

static_assert(::spsc::policy::detail::is_counter_like_v<custom_counter>);
static_assert(::spsc::policy::detail::is_counter_like_v<
              custom_counter_without_relaxed_load>);
static_assert(!::spsc::cnt::counter_has_relaxed_load_v<
              custom_counter_without_relaxed_load>);
static_assert(!::spsc::policy::detail::is_counter_like_v<no_value_counter>);
static_assert(!::spsc::policy::detail::is_counter_like_v<throwing_counter>);
static_assert(!::spsc::policy::detail::is_counter_like_v<wrong_relaxed_counter>);
static_assert(!::spsc::policy::detail::is_counter_like_v<signed_counter>);
static_assert(::spsc::alloc::policy_allocator_alignment_v<custom_aligned_policy> == 32u);
static_assert(::spsc::alloc::policy_allocator_alignment_v<
                  custom_enum_aligned_policy> == 32u);

using custom_counter_fifo = ::spsc::fifo<value, 8u, custom_policy>;
using custom_counter_without_relaxed_fifo =
    ::spsc::fifo<value, 8u, custom_policy_without_relaxed_load>;

template<typename T>
void copy_assign_through_alias(T& value)
{
    const T& alias = value;
    value = alias;
}

bool verify_value_swap_runtime()
{
    zero_non_assignable_array zero_lhs{};
    zero_non_assignable_array zero_rhs{};
    ::spsc::detail::swap_value(zero_lhs, zero_rhs);

    throwing_move_noexcept_copy lhs;
    throwing_move_noexcept_copy rhs;
    lhs.payload = 41;
    rhs.payload = 43;
    throwing_move_noexcept_copy::move_assignments = 0u;
    throwing_move_noexcept_copy::copy_assignments = 0u;

    ::spsc::detail::swap_value(lhs, rhs);
    if (lhs.payload != 43 || rhs.payload != 41 ||
        throwing_move_noexcept_copy::move_assignments != 0u ||
        throwing_move_noexcept_copy::copy_assignments != 3u) {
        return false;
    }

    throwing_move_noexcept_copy::move_assignments = 0u;
    throwing_move_noexcept_copy::copy_assignments = 0u;
    ::spsc::detail::swap_value(lhs, lhs);
    if (lhs.payload != 43 ||
        throwing_move_noexcept_copy::move_assignments != 0u ||
        throwing_move_noexcept_copy::copy_assignments != 0u) {
        return false;
    }

    bool bounded_array_scratch = false;
    {
        scratch_array array_lhs{};
        scratch_array array_rhs{};
        const std::size_t baseline_live = swap_scratch_probe::live_count;
        swap_scratch_probe::peak_live_count = baseline_live;
        array_lhs[0].payload = 47;
        array_rhs[0].payload = 53;

        ::spsc::detail::swap_value(array_lhs, array_rhs);
        bounded_array_scratch =
            baseline_live == (2u * array_lhs.size()) &&
            swap_scratch_probe::peak_live_count == (baseline_live + 1u) &&
            array_lhs[0].payload == 53 && array_rhs[0].payload == 47;
    }

    if (!bounded_array_scratch || swap_scratch_probe::live_count != 0u) {
        return false;
    }

    bool bounded_aligned_scratch = false;
    {
        aligned_scratch_storage storage_lhs{};
        aligned_scratch_storage storage_rhs{};
        const std::size_t baseline_live = swap_scratch_probe::live_count;
        swap_scratch_probe::peak_live_count = baseline_live;
        storage_lhs[0][0].payload = 61;
        storage_rhs[0][0].payload = 67;

        ::spsc::detail::swap_value(storage_lhs, storage_rhs);
        bounded_aligned_scratch =
            baseline_live ==
                (2u * storage_lhs.size() * storage_lhs[0].size()) &&
            swap_scratch_probe::peak_live_count == (baseline_live + 1u) &&
            storage_lhs[0][0].payload == 67 &&
            storage_rhs[0][0].payload == 61;
    }

    return bounded_aligned_scratch && swap_scratch_probe::live_count == 0u;
}

bool verify_queue_placement_new_runtime()
{
    class_new_queue queue;
    if (!queue.try_push(59)) {
        return false;
    }
    queue.push(61);

    const class_specific_new_value* value_ptr = queue.try_front();
    if (value_ptr == nullptr || value_ptr->payload != 59) {
        return false;
    }

    if (!queue.try_pop()) {
        return false;
    }

    value_ptr = queue.try_front();
    if (value_ptr == nullptr || value_ptr->payload != 61 || !queue.try_pop()) {
        return false;
    }

    class_specific_new_value& emplaced = queue.emplace(71);
    if (emplaced.payload != 71 || !queue.try_pop()) {
        return false;
    }

    value_ptr = queue.try_emplace(73);
    if (value_ptr == nullptr || value_ptr->payload != 73 || !queue.try_pop()) {
        return false;
    }

    class_specific_new_value* claimed = queue.claim();
    (void)::new (static_cast<void*>(claimed)) class_specific_new_value(79);
    queue.publish();
    value_ptr = queue.try_front();
    if (value_ptr == nullptr || value_ptr->payload != 79 || !queue.try_pop()) {
        return false;
    }

    claimed = queue.try_claim();
    if (claimed == nullptr) {
        return false;
    }
    (void)::new (static_cast<void*>(claimed)) class_specific_new_value(83);
    if (!queue.try_publish()) {
        return false;
    }
    value_ptr = queue.try_front();
    return value_ptr != nullptr && value_ptr->payload == 83 && queue.try_pop();
}

bool verify_copy_and_swap_runtime()
{
    assign_copy_fifo fifo_source;
    (void)fifo_source.try_push(copy_assign_only{});
    fifo_source.front().payload = 17;
    assign_copy_fifo fifo_copy(fifo_source);
    if (fifo_copy.size() != 1u || fifo_copy.front().payload != 17) {
        return false;
    }
    assign_copy_fifo fifo_assigned;
    fifo_assigned = fifo_source;
    copy_assign_through_alias(fifo_assigned);
    if (fifo_assigned.size() != 1u || fifo_assigned.front().payload != 17) {
        return false;
    }

    dynamic_assign_copy_fifo dynamic_fifo_source(8u);
    if (!dynamic_fifo_source.try_push(copy_assign_only{})) {
        return false;
    }
    dynamic_fifo_source.front().payload = 19;
    dynamic_assign_copy_fifo dynamic_fifo_copy(dynamic_fifo_source);
    dynamic_assign_copy_fifo dynamic_fifo_assigned(4u);
    dynamic_fifo_assigned = dynamic_fifo_source;
    if (dynamic_fifo_copy.size() != 1u ||
        dynamic_fifo_copy.front().payload != 19 ||
        dynamic_fifo_assigned.size() != 1u ||
        dynamic_fifo_assigned.front().payload != 19) {
        return false;
    }

    copyable_typed typed_source;
    (void)typed_source.try_emplace(23);
    copyable_typed typed_copy(typed_source);
    if (typed_copy.size() != 1u || typed_copy.try_front() == nullptr ||
        typed_copy.try_front()->payload != 23) {
        return false;
    }
    copyable_typed typed_assigned;
    typed_assigned = typed_source;
    copy_assign_through_alias(typed_assigned);
    if (typed_assigned.size() != 1u || typed_assigned.try_front() == nullptr ||
        typed_assigned.try_front()->payload != 23) {
        return false;
    }

    dynamic_copyable_typed dynamic_typed_source(8u);
    if (!dynamic_typed_source.try_emplace(29)) {
        return false;
    }
    dynamic_copyable_typed dynamic_typed_copy(dynamic_typed_source);
    dynamic_copyable_typed dynamic_typed_assigned(4u);
    dynamic_typed_assigned = dynamic_typed_source;
    if (dynamic_typed_copy.size() != 1u ||
        dynamic_typed_copy.try_front() == nullptr ||
        dynamic_typed_copy.try_front()->payload != 29 ||
        dynamic_typed_assigned.size() != 1u ||
        dynamic_typed_assigned.try_front() == nullptr ||
        dynamic_typed_assigned.try_front()->payload != 29) {
        return false;
    }

    swap_fifo fifo_a;
    swap_fifo fifo_b;
    no_adl_swap fifo_value_a;
    no_adl_swap fifo_value_b;
    no_adl_swap fifo_value_c;
    fifo_value_a.payload = 1;
    fifo_value_b.payload = 2;
    fifo_value_c.payload = 9;
    (void)fifo_a.try_push(std::move(fifo_value_a));
    (void)fifo_b.try_push(std::move(fifo_value_b));
    (void)fifo_b.try_push(std::move(fifo_value_c));
    fifo_a.swap(fifo_b);
    if (fifo_a.size() != 2u || fifo_b.size() != 1u ||
        fifo_a.front().payload != 2 || fifo_b.front().payload != 1) {
        return false;
    }

    swap_latest latest_a;
    swap_latest latest_b;
    no_adl_swap latest_value_a;
    no_adl_swap latest_value_b;
    latest_value_a.payload = 3;
    latest_value_b.payload = 4;
    (void)latest_a.try_push(std::move(latest_value_a));
    (void)latest_b.try_push(std::move(latest_value_b));
    latest_a.swap(latest_b);
    if (latest_a.try_front() == nullptr || latest_b.try_front() == nullptr ||
        latest_a.try_front()->payload != 4 || latest_b.try_front()->payload != 3) {
        return false;
    }
    swap_latest latest_c(std::move(latest_a));
    if (latest_c.try_front() == nullptr || latest_c.try_front()->payload != 4) {
        return false;
    }
    latest_c = std::move(latest_b);
    if (latest_c.try_front() == nullptr || latest_c.try_front()->payload != 3) {
        return false;
    }

    swap_chunk chunk_a;
    swap_chunk chunk_b;
    no_adl_swap chunk_value_a;
    no_adl_swap chunk_value_b;
    no_adl_swap chunk_value_c;
    chunk_value_a.payload = 5;
    chunk_value_b.payload = 6;
    chunk_value_c.payload = 10;
    (void)chunk_a.try_push(std::move(chunk_value_a));
    (void)chunk_b.try_push(std::move(chunk_value_b));
    (void)chunk_b.try_push(std::move(chunk_value_c));
    chunk_a.swap(chunk_b);
    if (chunk_a.size() != 2u || chunk_b.size() != 1u ||
        chunk_a.front().payload != 6 || chunk_b.front().payload != 5) {
        return false;
    }

    swap_pool pool_a;
    swap_pool pool_b;
    pool_a.data(0u)[0].payload = 7;
    pool_b.data(0u)[0].payload = 8;
    pool_a.swap(pool_b);
    if (pool_a.data(0u)[0].payload != 8 || pool_b.data(0u)[0].payload != 7) {
        return false;
    }

    swap_array_fifo array_fifo_a;
    swap_array_fifo array_fifo_b;
    array_fifo_a.data()[0][0].payload = 11;
    array_fifo_b.data()[0][0].payload = 12;
    array_fifo_a.swap(array_fifo_b);
    if (array_fifo_a.data()[0][0].payload != 12 ||
        array_fifo_b.data()[0][0].payload != 11) {
        return false;
    }

    swap_chunk_fifo chunk_fifo_a;
    swap_chunk_fifo chunk_fifo_b;
    chunk_fifo_a.data()[0].data()[0].payload = 13;
    chunk_fifo_b.data()[0].data()[0].payload = 14;
    chunk_fifo_a.swap(chunk_fifo_b);
    if (chunk_fifo_a.data()[0].data()[0].payload != 14 ||
        chunk_fifo_b.data()[0].data()[0].payload != 13) {
        return false;
    }

    custom_counter_fifo custom_queue;
    (void)custom_queue.try_push(value{31});
    custom_counter_without_relaxed_fifo fallback_queue;
    (void)fallback_queue.try_push(value{37});
    return custom_queue.try_front() != nullptr &&
           custom_queue.try_front()->payload == 31 &&
           fallback_queue.try_front() != nullptr &&
           fallback_queue.try_front()->payload == 37;
}

} // namespace

int main()
{
    return verify_value_swap_runtime() &&
                   verify_queue_placement_new_runtime() &&
                   verify_copy_and_swap_runtime()
               ? 0
               : 1;
}
