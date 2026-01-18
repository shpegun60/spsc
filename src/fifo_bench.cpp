/*
 * fifo_bench.cpp
 *
 * Compact benchmarks for spsc::fifo.
 *
 * Goals:
 *  - QtCore-only (no QtTest/testlib).
 *  - No CSV.
 *  - Compact console output.
 *  - ST benches for each policy.
 *  - MT benches for atomic-ish policies.
 *  - Snapshot speed included.
 */

#include <QDebug>
#include <QString>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

#include "fifo_bench.h"
#include "fifo.hpp"

namespace {

// ------------------------------ knobs ------------------------------

static constexpr reg kCapStatic = reg{4096u};
static constexpr reg kCapDynamic = reg{4096u};
static constexpr reg kBulkChunk = reg{32u};

#if defined(NDEBUG)
static constexpr int kOpsST = 2'000'000;
static constexpr int kOpsTryST = 2'000'000;
static constexpr int kOpsRAII = 600'000;
static constexpr int kOpsMT = 2'000'000;
static constexpr int kRoundsBulk = 25'000;
static constexpr int kRoundsBulkMT = 25'000;
static constexpr int kSnapshotMakeIters = 400'000;
static constexpr int kSnapshotIterIters = 80'000;
static constexpr int kTimeoutMsMT = 6'000;
#else
static constexpr int kOpsST = 400'000;
static constexpr int kOpsTryST = 400'000;
static constexpr int kOpsRAII = 120'000;
static constexpr int kOpsMT = 300'000;
static constexpr int kRoundsBulk = 6'000;
static constexpr int kRoundsBulkMT = 6'000;
static constexpr int kSnapshotMakeIters = 120'000;
static constexpr int kSnapshotIterIters = 20'000;
static constexpr int kTimeoutMsMT = 15'000;
#endif

// A global sink prevents the optimizer from "being helpful".
static volatile std::uint64_t g_sink = 0u;

struct cell final {
    bool ok = false;
    double v = 0.0; // M/s (or Msnap/s)
};

struct row final {
    std::string label;
    cell pp{};
    cell tr{};
    cell bulk{};
    cell raii{};
    cell snap_mk{};
    cell snap_it{};
    cell mt_try{};
    cell mt_bulk{};
};

struct scoped_timer final {
    using clock = std::chrono::steady_clock;
    clock::time_point t0{clock::now()};

    std::uint64_t elapsed_ns() const noexcept {
        const auto dt = clock::now() - t0;
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(dt).count());
    }
};

static double to_m_per_s(std::uint64_t elapsed_ns, std::uint64_t items) noexcept {
    if (elapsed_ns == 0u || items == 0u) return 0.0;
    const double sec = static_cast<double>(elapsed_ns) * 1e-9;
    return (static_cast<double>(items) / sec) * 1e-6;
}

// ------------------------------ ST benches ------------------------------

template <class Q>
static cell bench_st_push_pop(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;
    q.clear();

    const reg warm = q.capacity() / 2u;
    for (reg i = 0; i < warm; ++i) q.push(static_cast<int>(i));

    scoped_timer tm;
    for (int i = 0; i < kOpsST; ++i) {
        const int x = q.front();
        q.pop();
        q.push(x + 1);
    }

    out.ok = true;
    out.v = to_m_per_s(tm.elapsed_ns(), static_cast<std::uint64_t>(kOpsST));
    return out;
}

template <class Q>
static cell bench_st_try(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;
    q.clear();

    q.push(123);

    scoped_timer tm;
    for (int i = 0; i < kOpsTryST; ++i) {
        while (!q.try_pop()) {
            // ST: if this spins, something is broken.
        }
        while (!q.try_push(i)) {
            // ST: if this spins, something is broken.
        }
    }

    out.ok = true;
    out.v = to_m_per_s(tm.elapsed_ns(), static_cast<std::uint64_t>(kOpsTryST));
    return out;
}

template <class Q>
static cell bench_st_bulk(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;
    q.clear();

    if (q.capacity() < (kBulkChunk * 2u)) return out;

    volatile int sink = 0;
    std::uint64_t items_done = 0u;

    scoped_timer tm;
    for (int r = 0; r < kRoundsBulk; ++r) {
        auto wr = q.claim_write(kBulkChunk);
        if (wr.total == 0u) continue;

        int v = r;
        for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = v++;
        for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = v++;
        q.publish(wr.total);

        auto rr = q.claim_read(wr.total);
        for (reg i = 0; i < rr.first.count; ++i) sink ^= rr.first.ptr[i];
        for (reg i = 0; i < rr.second.count; ++i) sink ^= rr.second.ptr[i];
        q.pop(rr.total);

        items_done += static_cast<std::uint64_t>(rr.total);
    }

    g_sink ^= static_cast<std::uint64_t>(sink);

    out.ok = (items_done != 0u);
    out.v = to_m_per_s(tm.elapsed_ns(), items_done);
    return out;
}

template <class Q>
static cell bench_st_raii(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;
    q.clear();

    volatile int sink = 0;

    scoped_timer tm;
    for (int i = 0; i < kOpsRAII; ++i) {
        {
            auto wg = q.scoped_write();
            if (!wg) continue;
            wg.ref() = i;
            wg.commit();
        }
        {
            auto rg = q.scoped_read();
            if (!rg) continue;
            sink ^= rg.ref();
            rg.commit();
        }
    }

    g_sink ^= static_cast<std::uint64_t>(sink);

    out.ok = true;
    out.v = to_m_per_s(tm.elapsed_ns(), static_cast<std::uint64_t>(kOpsRAII));
    return out;
}

// Snapshot make only.
//
// Important: a smart optimizer may try to hoist/collapse the snapshot loop if the
// queue never changes. We add a tiny periodic pop+push to keep head/tail moving.

template <class Q>
static cell bench_st_snapshot_make(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;
    q.clear();

    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) q.push(static_cast<int>(i));

    std::uint64_t checksum = 0u;

    scoped_timer tm;
    for (int it = 0; it < kSnapshotMakeIters; ++it) {
        const auto s = q.make_snapshot();
        checksum += static_cast<std::uint64_t>(s.size());
        checksum += static_cast<std::uint64_t>(s.head_index());
        checksum += static_cast<std::uint64_t>(s.tail_index()) * 3u;

        if ((it & 255) == 0) {
            // Keep the queue evolving with negligible overhead.
            const int x = q.front();
            q.pop();
            q.push(x + 1);
        }
    }

    g_sink += checksum;

    out.ok = true;
    out.v = to_m_per_s(tm.elapsed_ns(), static_cast<std::uint64_t>(kSnapshotMakeIters));
    return out;
}

// Snapshot make + iterate all elements.

template <class Q>
static cell bench_st_snapshot_iter(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;
    q.clear();

    const reg cap = q.capacity();
    for (reg i = 0; i < cap; ++i) q.push(static_cast<int>(i));

    std::uint64_t checksum = 0u;
    std::uint64_t elems_total = 0u;

    scoped_timer tm;
    for (int it = 0; it < kSnapshotIterIters; ++it) {
        const auto s = q.make_snapshot();

        // Mix in the loop counter to make algebraic collapse annoying.
        const std::uint64_t salt = static_cast<std::uint64_t>(it + 1);

        for (auto p = s.begin(); p != s.end(); ++p) {
            checksum = (checksum * 1315423911u) + static_cast<std::uint64_t>(*p) + salt;
        }

        elems_total += static_cast<std::uint64_t>(s.size());

        if ((it & 127) == 0) {
            // Keep the queue evolving a bit so snapshot boundaries are not constant.
            const int x = q.front();
            q.pop();
            q.push(x + 1);
        }
    }

    g_sink += checksum;

    out.ok = (elems_total != 0u);
    out.v = to_m_per_s(tm.elapsed_ns(), elems_total);
    return out;
}

// ------------------------------ MT benches ------------------------------

struct spin_backoff final {
    std::uint32_t spins = 0u;

    void pause() noexcept {
        ++spins;
        if ((spins & 0x7FFu) == 0u) {
            std::this_thread::yield();
        } else {
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }

    void reset() noexcept { spins = 0u; }
};

template <class Q>
static cell bench_mt_try(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;

    q.clear();

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};

    std::atomic<std::uint64_t> consumed{0};
    std::uint64_t checksum = 0u;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMsMT);

    auto producer = [&]() {
        spin_backoff bk;
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) bk.pause();
        bk.reset();

        for (int i = 0; i < kOpsMT; ) {
            if (stop.load(std::memory_order_relaxed)) break;

            if (q.try_push(i)) {
                ++i;
                bk.reset();
            } else {
                if (std::chrono::steady_clock::now() > deadline) {
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                bk.pause();
            }
        }
    };

    auto consumer = [&]() {
        spin_backoff bk;
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) bk.pause();
        bk.reset();

        for (int i = 0; i < kOpsMT; ) {
            if (stop.load(std::memory_order_relaxed)) break;

            if (auto* p = q.try_front()) {
                checksum += static_cast<std::uint64_t>(*p);
                q.pop();
                consumed.fetch_add(1, std::memory_order_relaxed);
                ++i;
                bk.reset();
            } else {
                if (std::chrono::steady_clock::now() > deadline) {
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                bk.pause();
            }
        }
    };

    while (ready.load(std::memory_order_acquire) != 0) {
        ready.store(0, std::memory_order_release);
        break;
    }

    std::thread tp(producer);
    std::thread tc(consumer);

    while (ready.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }

    scoped_timer tm;
    go.store(true, std::memory_order_release);

    tp.join();
    tc.join();

    g_sink += checksum;

    const std::uint64_t done = consumed.load(std::memory_order_relaxed);
    out.ok = (done != 0u) && !stop.load(std::memory_order_relaxed);
    out.v = to_m_per_s(tm.elapsed_ns(), done);
    return out;
}

template <class Q>
static cell bench_mt_bulk(Q& q) {
    cell out{};
    if (!q.is_valid()) return out;

    q.clear();

    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::atomic<bool> stop{false};

    std::atomic<std::uint64_t> consumed{0};
    std::uint64_t checksum = 0u;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMsMT);

    auto producer = [&]() {
        spin_backoff bk;
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) bk.pause();
        bk.reset();

        int v = 1;
        std::uint64_t sent = 0u;
        while (sent < static_cast<std::uint64_t>(kRoundsBulkMT) * static_cast<std::uint64_t>(kBulkChunk)) {
            if (stop.load(std::memory_order_relaxed)) break;

            auto wr = q.claim_write(kBulkChunk);
            if (wr.total == 0u) {
                if (std::chrono::steady_clock::now() > deadline) {
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                bk.pause();
                continue;
            }

            for (reg i = 0; i < wr.first.count; ++i) wr.first.ptr[i] = v++;
            for (reg i = 0; i < wr.second.count; ++i) wr.second.ptr[i] = v++;

            q.publish(wr.total);
            sent += static_cast<std::uint64_t>(wr.total);
            bk.reset();
        }
    };

    auto consumer = [&]() {
        spin_backoff bk;
        ready.fetch_add(1, std::memory_order_release);
        while (!go.load(std::memory_order_acquire)) bk.pause();
        bk.reset();

        std::uint64_t got = 0u;
        const std::uint64_t need = static_cast<std::uint64_t>(kRoundsBulkMT) * static_cast<std::uint64_t>(kBulkChunk);

        while (got < need) {
            if (stop.load(std::memory_order_relaxed)) break;

            auto rr = q.claim_read(kBulkChunk);
            if (rr.total == 0u) {
                if (std::chrono::steady_clock::now() > deadline) {
                    stop.store(true, std::memory_order_relaxed);
                    break;
                }
                bk.pause();
                continue;
            }

            for (reg i = 0; i < rr.first.count; ++i) checksum += static_cast<std::uint64_t>(rr.first.ptr[i]);
            for (reg i = 0; i < rr.second.count; ++i) checksum += static_cast<std::uint64_t>(rr.second.ptr[i]);

            q.pop(rr.total);

            got += static_cast<std::uint64_t>(rr.total);
            consumed.fetch_add(static_cast<std::uint64_t>(rr.total), std::memory_order_relaxed);
            bk.reset();
        }
    };

    std::thread tp(producer);
    std::thread tc(consumer);

    while (ready.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }

    scoped_timer tm;
    go.store(true, std::memory_order_release);

    tp.join();
    tc.join();

    g_sink += checksum;

    const std::uint64_t done = consumed.load(std::memory_order_relaxed);
    out.ok = (done != 0u) && !stop.load(std::memory_order_relaxed);
    out.v = to_m_per_s(tm.elapsed_ns(), done);
    return out;
}

// ------------------------------ formatting ------------------------------

static std::string fmt_cell(const cell& c, int width = 8) {
    std::ostringstream oss;
    oss.setf(std::ios::fixed);
    oss << std::setw(width);
    if (!c.ok) {
        oss << "-";
    } else {
        oss << std::setprecision(3) << c.v;
    }
    return oss.str();
}

static void emit_header() {
    qInfo().noquote() << "\n=== spsc::fifo bench (compact) ===";

    {
        std::ostringstream oss;
        oss << "cap_static=" << static_cast<unsigned>(kCapStatic)
            << " cap_dynamic=" << static_cast<unsigned>(kCapDynamic)
            << " bulk_chunk=" << static_cast<unsigned>(kBulkChunk);
        qInfo().noquote() << QString::fromStdString(oss.str());
    }

    qInfo().noquote()
        << "\nFORMAT: policy/variant | pp M/s | try M/s | bulk M/s | raii M/s | snap_mk Msnap/s | snap_it Melem/s | mt_try M/s | mt_bulk M/s";
}

static void emit_row(const row& r) {
    std::ostringstream oss;
    oss << std::left << std::setw(10) << r.label << " | "
        << fmt_cell(r.pp) << " | "
        << fmt_cell(r.tr) << " | "
        << fmt_cell(r.bulk) << " | "
        << fmt_cell(r.raii) << " | "
        << fmt_cell(r.snap_mk, 12) << " | "
        << fmt_cell(r.snap_it, 12) << " | "
        << fmt_cell(r.mt_try) << " | "
        << fmt_cell(r.mt_bulk);

    qInfo().noquote() << QString::fromStdString(oss.str());
}

// ------------------------------ suite runner ------------------------------

template <class Q>
static void run_st_suite(row& r, Q& q) {
    r.pp = bench_st_push_pop(q);
    r.tr = bench_st_try(q);
    r.bulk = bench_st_bulk(q);
    r.raii = bench_st_raii(q);
    r.snap_mk = bench_st_snapshot_make(q);
    r.snap_it = bench_st_snapshot_iter(q);
}

template <class Q>
static void run_mt_suite(row& r, Q& q) {
    r.mt_try = bench_mt_try(q);
    r.mt_bulk = bench_mt_bulk(q);
}

} // namespace

int run_fifo_bench() {
    emit_header();

    // P
    {
        using Policy = ::spsc::policy::P;

        ::spsc::fifo<int, kCapStatic, Policy> qs;
        row rs{};
        rs.label = "P/static";
        run_st_suite(rs, qs);
        emit_row(rs);

        ::spsc::fifo<int, 0, Policy> qd;
        (void)qd.resize(kCapDynamic);
        row rd{};
        rd.label = "P/dynamic";
        run_st_suite(rd, qd);
        emit_row(rd);
    }

    // V
    {
        using Policy = ::spsc::policy::V;

        ::spsc::fifo<int, kCapStatic, Policy> qs;
        row rs{};
        rs.label = "V/static";
        run_st_suite(rs, qs);
        emit_row(rs);

        ::spsc::fifo<int, 0, Policy> qd;
        (void)qd.resize(kCapDynamic);
        row rd{};
        rd.label = "V/dynamic";
        run_st_suite(rd, qd);
        emit_row(rd);
    }

    // A (atomic)
    {
        using Policy = ::spsc::policy::A<>;

        ::spsc::fifo<int, kCapStatic, Policy> qs;
        row rs{};
        rs.label = "A/static";
        run_st_suite(rs, qs);
        run_mt_suite(rs, qs);
        emit_row(rs);

        ::spsc::fifo<int, 0, Policy> qd;
        (void)qd.resize(kCapDynamic);
        row rd{};
        rd.label = "A/dynamic";
        run_st_suite(rd, qd);
        run_mt_suite(rd, qd);
        emit_row(rd);
    }

    // CA (cacheline-aligned atomic)
    {
        using Policy = ::spsc::policy::CA<>;

        ::spsc::fifo<int, kCapStatic, Policy> qs;
        row rs{};
        rs.label = "CA/static";
        run_st_suite(rs, qs);
        run_mt_suite(rs, qs);
        emit_row(rs);

        ::spsc::fifo<int, 0, Policy> qd;
        (void)qd.resize(kCapDynamic);
        row rd{};
        rd.label = "CA/dynamic";
        run_st_suite(rd, qd);
        run_mt_suite(rd, qd);
        emit_row(rd);
    }

    // Touch the sink to keep the compiler honest.
    if (g_sink == 0xFFFFFFFFFFFFFFFFull) {
        qWarning().noquote() << "[fifo_bench] sink hit the impossible";
    }

    return 0;
}
