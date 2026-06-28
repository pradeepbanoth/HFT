#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// fast_book.hpp  —  SIMD-accelerated flat-array order book
//
// Problem with std::map<double,double>:
//   Every lookup traverses a red-black tree with scattered heap nodes.
//   Each node access is a pointer dereference → cache miss at ~100ns.
//   A book with 100 levels = 100–200 cache misses per update.
//
// Solution: sorted flat arrays with AVX2 search
//   bids_prices_[0..N] and bids_qtys_[0..N] are contiguous 64-byte aligned.
//   Price search uses AVX2 to compare 4 doubles at once → 4× fewer iterations.
//   Insert/erase shift memory (memmove) — fast for N≤200 because it's sequential.
//
// Design:
//   FastBook mirrors the OrderBook API but internally stores prices in
//   descending (bids) and ascending (asks) sorted arrays.
//   It is used *alongside* OrderBook for the fast top-of-book path:
//     - FastBook handles best_bid/ask, spread, imbalance, depth_arrays
//     - OrderBook handles L3 queue tracking for fill simulation
//
// Capacity: kMaxLevels = 256 (fits in 4KB per side, one memory page)
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include <cstddef>
#include <cstring>
#include <cmath>
#include <optional>
#include <immintrin.h>   // AVX2
#include <algorithm>
#include <cassert>
#include <array>
#include <chrono>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// AVX2 utilities
// ─────────────────────────────────────────────────────────────────────────────
namespace simd {

// Search sorted descending array for `target`. Returns index or -1.
// Uses AVX2 to test 4 doubles per cycle.
inline int find_desc(const double* arr, int n, double target) noexcept {
#ifdef __AVX2__
    __m256d vt = _mm256_set1_pd(target);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        __m256d va = _mm256_loadu_pd(arr + i);
        // Compare equal: all 4 lanes
        __m256d eq = _mm256_cmp_pd(va, vt, _CMP_EQ_OQ);
        int mask = _mm256_movemask_pd(eq);
        if (mask) {
            // __builtin_ctz gives position of first set bit
            return i + __builtin_ctz(mask);
        }
    }
    for (; i < n; ++i) if (arr[i] == target) return i;
    return -1;
#else
    for (int i = 0; i < n; ++i) if (arr[i] == target) return i;
    return -1;
#endif
}

// Search sorted ascending array for `target`. Same SIMD logic.
inline int find_asc(const double* arr, int n, double target) noexcept {
    return find_desc(arr, n, target);  // same bytewise op
}

// Lower bound in descending array (first index where arr[i] <= target)
inline int lower_bound_desc(const double* arr, int n, double target) noexcept {
    // Binary search for insertion point in descending order
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (arr[mid] > target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

// Lower bound in ascending array (first index where arr[i] >= target)
inline int lower_bound_asc(const double* arr, int n, double target) noexcept {
    int lo = 0, hi = n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (arr[mid] < target) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

} // namespace simd

// ─────────────────────────────────────────────────────────────────────────────
// FlatLevel  —  price + qty in one cache-line-friendly pair
// ─────────────────────────────────────────────────────────────────────────────
struct alignas(16) FlatLevel {
    double price = 0.0;
    double qty   = 0.0;
};

// ─────────────────────────────────────────────────────────────────────────────
// FastBook
// ─────────────────────────────────────────────────────────────────────────────
class FastBook {
public:
    static constexpr int kMaxLevels = 256;

    explicit FastBook(std::string symbol, double tick_size = 1e-8)
        : symbol_(std::move(symbol)), tick_size_(tick_size)
    {
        std::memset(bid_prices_, 0, sizeof(bid_prices_));
        std::memset(bid_qtys_,   0, sizeof(bid_qtys_));
        std::memset(ask_prices_, 0, sizeof(ask_prices_));
        std::memset(ask_qtys_,   0, sizeof(ask_qtys_));
    }

    // ── L2 apply ─────────────────────────────────────────────────────────────

    void apply_l2(const L2Update& upd) noexcept {
        if (upd.side == BookSide::Bid) {
            upsert_desc(bid_prices_, bid_qtys_, n_bids_, upd.price, upd.qty);
        } else {
            upsert_asc(ask_prices_, ask_qtys_, n_asks_, upd.price, upd.qty);
        }
        last_ts_ = upd.timestamp;
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    std::optional<double> best_bid() const noexcept {
        return n_bids_ > 0 ? std::optional<double>{bid_prices_[0]} : std::nullopt;
    }
    std::optional<double> best_ask() const noexcept {
        return n_asks_ > 0 ? std::optional<double>{ask_prices_[0]} : std::nullopt;
    }
    std::optional<double> mid_price() const noexcept {
        if (n_bids_ == 0 || n_asks_ == 0) return std::nullopt;
        return (bid_prices_[0] + ask_prices_[0]) * 0.5;
    }
    std::optional<double> spread() const noexcept {
        if (n_bids_ == 0 || n_asks_ == 0) return std::nullopt;
        return ask_prices_[0] - bid_prices_[0];
    }
    std::optional<double> spread_bps() const noexcept {
        auto s = spread();
        auto m = mid_price();
        if (!s || !m || *m < 1e-12) return std::nullopt;
        return *s / *m * 10000.0;
    }

    int n_bids() const noexcept { return n_bids_; }
    int n_asks() const noexcept { return n_asks_; }

    // Fill arrays for SIMD signal functions — no allocation
    void fill_depth_arrays(
        double* bp, double* bq,
        double* ap, double* aq,
        int levels) const noexcept
    {
        int nb = std::min(n_bids_, levels);
        int na = std::min(n_asks_, levels);
        std::memcpy(bp, bid_prices_, nb * sizeof(double));
        std::memcpy(bq, bid_qtys_,   nb * sizeof(double));
        std::memcpy(ap, ask_prices_, na * sizeof(double));
        std::memcpy(aq, ask_qtys_,   na * sizeof(double));
        // Zero-pad remaining
        for (int i = nb; i < levels; ++i) { bp[i] = 0; bq[i] = 0; }
        for (int i = na; i < levels; ++i) { ap[i] = 0; aq[i] = 0; }
    }

    // OBI-weighted micro-price (inline, no function call overhead)
    double micro_price(int levels = 5) const noexcept {
        if (n_bids_ == 0 || n_asks_ == 0) return 0.0;
        int n = std::min({n_bids_, n_asks_, levels});
        double bv = 0.0, av = 0.0;
        for (int i = 0; i < n; ++i) { bv += bid_qtys_[i]; av += ask_qtys_[i]; }
        double total = bv + av;
        if (total < 1e-12) return (bid_prices_[0] + ask_prices_[0]) * 0.5;
        return bid_prices_[0] * (bv / total) + ask_prices_[0] * (av / total);
    }

    double imbalance(int levels = 5) const noexcept {
        int n = std::min({n_bids_, n_asks_, levels});
        double bv = 0.0, av = 0.0;
        for (int i = 0; i < n; ++i) { bv += bid_qtys_[i]; av += ask_qtys_[i]; }
        double t = bv + av;
        return t > 1e-12 ? (bv - av) / t : 0.0;
    }

    // VWAP cost to fill target_qty on the given side
    double vwap_cost(Side side, double target_qty) const noexcept {
        const double* prices = (side == Side::Buy) ? ask_prices_ : bid_prices_;
        const double* qtys   = (side == Side::Buy) ? ask_qtys_   : bid_qtys_;
        int n                = (side == Side::Buy) ? n_asks_      : n_bids_;
        double rem = target_qty, cost = 0.0;
        for (int i = 0; i < n && rem > 1e-12; ++i) {
            double take = std::min(qtys[i], rem);
            cost += take * prices[i];
            rem  -= take;
        }
        return rem <= 1e-12 ? cost / target_qty : -1.0;
    }

    // Total visible bid/ask qty across N levels
    double total_bid_qty(int levels = 10) const noexcept {
        double s = 0.0;
        for (int i = 0; i < std::min(n_bids_, levels); ++i) s += bid_qtys_[i];
        return s;
    }
    double total_ask_qty(int levels = 10) const noexcept {
        double s = 0.0;
        for (int i = 0; i < std::min(n_asks_, levels); ++i) s += ask_qtys_[i];
        return s;
    }

    void clear() noexcept { n_bids_ = 0; n_asks_ = 0; }
    const std::string& symbol() const noexcept { return symbol_; }
    int64_t last_ts() const noexcept { return last_ts_; }

private:
    std::string symbol_;
    double      tick_size_;
    int64_t     last_ts_ = 0;

    // Cache-line aligned flat arrays
    alignas(64) double bid_prices_[kMaxLevels];
    alignas(64) double bid_qtys_  [kMaxLevels];
    alignas(64) double ask_prices_[kMaxLevels];
    alignas(64) double ask_qtys_  [kMaxLevels];

    int n_bids_ = 0;
    int n_asks_ = 0;

    // ── Sorted array operations ───────────────────────────────────────────────

    // Upsert into descending-sorted array (bids)
    void upsert_desc(double* prices, double* qtys, int& n,
                     double price, double qty) noexcept
    {
        // Check for existing level first (AVX2 search)
        int idx = simd::find_desc(prices, n, price);
        if (idx >= 0) {
            // Level exists
            if (qty <= 1e-12) {
                // Remove: shift left
                int move = n - idx - 1;
                if (move > 0) {
                    std::memmove(prices + idx, prices + idx + 1, move * sizeof(double));
                    std::memmove(qtys   + idx, qtys   + idx + 1, move * sizeof(double));
                }
                --n;
            } else {
                qtys[idx] = qty;
            }
            return;
        }
        if (qty <= 1e-12) return;   // remove non-existent level — no-op

        // Find insertion point (binary search)
        int ins = simd::lower_bound_desc(prices, n, price);
        if (n < kMaxLevels) {
            // Shift right to make room
            int move = n - ins;
            if (move > 0) {
                std::memmove(prices + ins + 1, prices + ins, move * sizeof(double));
                std::memmove(qtys   + ins + 1, qtys   + ins, move * sizeof(double));
            }
            prices[ins] = price;
            qtys[ins]   = qty;
            ++n;
        } else if (ins < kMaxLevels) {
            // Array full: insert and drop worst (last) bid (lowest price)
            int move = kMaxLevels - ins - 1;
            std::memmove(prices + ins + 1, prices + ins, move * sizeof(double));
            std::memmove(qtys   + ins + 1, qtys   + ins, move * sizeof(double));
            prices[ins] = price;
            qtys[ins]   = qty;
            // n stays at kMaxLevels
        }
    }

    // Upsert into ascending-sorted array (asks)
    void upsert_asc(double* prices, double* qtys, int& n,
                    double price, double qty) noexcept
    {
        int idx = simd::find_asc(prices, n, price);
        if (idx >= 0) {
            if (qty <= 1e-12) {
                int move = n - idx - 1;
                if (move > 0) {
                    std::memmove(prices + idx, prices + idx + 1, move * sizeof(double));
                    std::memmove(qtys   + idx, qtys   + idx + 1, move * sizeof(double));
                }
                --n;
            } else {
                qtys[idx] = qty;
            }
            return;
        }
        if (qty <= 1e-12) return;

        int ins = simd::lower_bound_asc(prices, n, price);
        if (n < kMaxLevels) {
            int move = n - ins;
            if (move > 0) {
                std::memmove(prices + ins + 1, prices + ins, move * sizeof(double));
                std::memmove(qtys   + ins + 1, qtys   + ins, move * sizeof(double));
            }
            prices[ins] = price;
            qtys[ins]   = qty;
            ++n;
        } else if (ins < kMaxLevels) {
            int move = kMaxLevels - ins - 1;
            std::memmove(prices + ins + 1, prices + ins, move * sizeof(double));
            std::memmove(qtys   + ins + 1, qtys   + ins, move * sizeof(double));
            prices[ins] = price;
            qtys[ins]   = qty;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// FastBookBenchmark  —  measures update throughput
// ─────────────────────────────────────────────────────────────────────────────
struct FastBookBenchmark {
    double updates_per_second = 0.0;
    double ns_per_update      = 0.0;
    double mid_price_sample   = 0.0;
};

inline FastBookBenchmark benchmark_fast_book(int n_updates = 1'000'000) {
    FastBook book("BTCUSDT", 0.01);
    // Pre-build a book with 20 levels
    for (int i = 0; i < 20; ++i) {
        L2Update b; b.symbol="BTCUSDT"; b.side=BookSide::Bid;
        b.price=43500.0-i*0.01; b.qty=1.0+i*0.1; b.timestamp=i;
        book.apply_l2(b);
        L2Update a; a.symbol="BTCUSDT"; a.side=BookSide::Ask;
        a.price=43501.0+i*0.01; a.qty=1.0+i*0.1; a.timestamp=i;
        book.apply_l2(a);
    }

    auto t0 = std::chrono::high_resolution_clock::now();
    volatile double sink = 0.0;
    for (int i = 0; i < n_updates; ++i) {
        // Simulate alternating bid/ask updates at best levels
        double price = 43500.0 + (i % 5) * 0.01;
        double qty   = 1.0 + (i % 10) * 0.1;
        L2Update u; u.symbol="BTCUSDT";
        u.side = (i % 2 == 0) ? BookSide::Bid : BookSide::Ask;
        u.price = price; u.qty = qty; u.timestamp = i;
        book.apply_l2(u);
        sink += book.micro_price();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1-t0).count();

    return {
        n_updates / elapsed,
        elapsed / n_updates * 1e9,
        static_cast<double>(sink) / n_updates
    };
}

} // namespace hft