#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// orderbook.hpp  —  Full-depth reconstructed order book
//
// PriceLevel: array-backed FIFO queue
//   • parallel std::vector<std::string> ids + std::vector<double> qtys
//   • std::unordered_map<string,size_t> for O(1) index lookup
//   • _head index avoids repeated erasure from the front (amortised O(1) consume)
//   • lazy compaction when head drifts past 256 tombstones
//
// OrderBook:
//   • Bids: std::map<double, double, std::greater<>> (descending)
//   • Asks: std::map<double, double>                 (ascending)
//   • Parallel bid_levels / ask_levels: price → PriceLevel*
//   • L2 → L3 probabilistic reconstruction
//   • Sequence-gap detection, crossed-book recovery, Bybit CRC32 validation
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include <map>
#include <unordered_map>
#include <vector>
#include <string>
#include <optional>
#include <deque>
#include <memory>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <functional>
#include <sstream>
#include <cstdint>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// SequenceTracker
// ─────────────────────────────────────────────────────────────────────────────
class SequenceTracker {
public:
    explicit SequenceTracker(std::string name, int64_t max_gap = 1)
        : name_(std::move(name)), max_gap_(max_gap) {}

    // Returns true if message is valid and in-order.
    bool update(int64_t seq) noexcept {
        if (seq == 0) return true;
        if (last_ < 0) { last_ = seq; return true; }
        if (seq <= last_) { ++dupes_; return false; }  // duplicate
        if (seq - last_ > max_gap_ + 1) {
            ++gaps_;
            // log: gap detected – caller should re-snapshot
            last_ = seq;
            return true;  // continue but signal re-snapshot needed
        }
        last_ = seq;
        return true;
    }

    int64_t gaps()  const noexcept { return gaps_;  }
    int64_t dupes() const noexcept { return dupes_; }
    bool    needs_resync() const noexcept { return gaps_ > 0; }

private:
    std::string name_;
    int64_t     last_    = -1;
    int64_t     max_gap_ = 1;
    int64_t     gaps_    = 0;
    int64_t     dupes_   = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// PriceLevel  —  array-backed FIFO order queue
// ─────────────────────────────────────────────────────────────────────────────
class PriceLevel {
public:
    double price      = 0.0;
    double total_qty  = 0.0;

    explicit PriceLevel(double p) : price(p) {}

    // Non-copyable (owned by unique_ptr in OrderBook)
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&)                 = default;


    
    // ── Mutations ────────────────────────────────────────────────────────────

    void add_order(const std::string& order_id, double qty) {
        if (qty <= 0.0) return;
        auto it = map_.find(order_id);
        if (it != map_.end()) {
            // Re-add: update qty in place (quantity correction, keep position)
            size_t idx = it->second;
            total_qty += qty - qtys_[idx];
            qtys_[idx] = qty;
            return;
        }
        map_[order_id] = ids_.size();
        ids_.push_back(order_id);
        qtys_.push_back(qty);
        total_qty += qty;
    }

    void modify_order(const std::string& order_id, double new_qty) {
        auto it = map_.find(order_id);
        if (it == map_.end()) return;
        size_t idx = it->second;
        double old = qtys_[idx];
        if (new_qty > old) {
            // Increase → lose queue priority (move to back)
            qtys_[idx] = 0.0;
            map_[order_id] = ids_.size();
            ids_.push_back(order_id);
            qtys_.push_back(new_qty);
        } else {
            qtys_[idx] = new_qty;
        }
        total_qty += new_qty - old;
    }

    void delete_order(const std::string& order_id) {
        auto it = map_.find(order_id);
        if (it == map_.end()) return;
        size_t idx = it->second;
        double qty = qtys_[idx];
        qtys_[idx] = 0.0;
        map_.erase(it);
        total_qty = std::max(0.0, total_qty - qty);
    }

    // FIFO consume from front. Returns actually consumed qty.
    double consume_qty(double qty) {
        double remaining = qty;
        while (head_ < ids_.size() && remaining > 1e-12) {
            double q = qtys_[head_];
            if (q <= 1e-12) {
                // Tombstoned slot
                map_.erase(ids_[head_]);
                ++head_;
                continue;
            }
            double take = std::min(q, remaining);
            qtys_[head_] -= take;
            remaining    -= take;
            total_qty     = std::max(0.0, total_qty - take);
            if (qtys_[head_] <= 1e-12) {
                map_.erase(ids_[head_]);
                ++head_;
            }
        }
        // Compact when head has drifted far
        if (head_ > 256 && head_ > ids_.size() / 2) {
            compact();
        }
        return qty - remaining;
    }

    // Returns qty ahead of our order in FIFO queue. O(n) scan.
    double qty_ahead_of(const std::string& our_id) const {
        double ahead = 0.0;
        for (size_t i = head_; i < ids_.size(); ++i) {
            if (qtys_[i] <= 1e-12) continue;
            if (ids_[i] == our_id) return ahead;
            ahead += qtys_[i];
        }
        return ahead;  // not found → assume at back
    }

    // Count of live orders
    size_t live_count() const noexcept {
        size_t n = 0;
        for (size_t i = head_; i < ids_.size(); ++i)
            if (qtys_[i] > 1e-12) ++n;
        return n;
    }

    bool empty() const noexcept { return total_qty <= 1e-12; }

private:
    std::vector<std::string>              ids_;
    std::vector<double>                   qtys_;
    std::unordered_map<std::string, size_t> map_;
    size_t                                head_ = 0;

    void compact() {
        std::vector<std::string> new_ids;
        std::vector<double>      new_qtys;
        std::unordered_map<std::string, size_t> new_map;
        for (size_t i = head_; i < ids_.size(); ++i) {
            if (qtys_[i] > 1e-12) {
                new_map[ids_[i]] = new_ids.size();
                new_ids.push_back(ids_[i]);
                new_qtys.push_back(qtys_[i]);
            }
        }
        ids_   = std::move(new_ids);
        qtys_  = std::move(new_qtys);
        map_   = std::move(new_map);
        head_  = 0;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// OrderBook
// ─────────────────────────────────────────────────────────────────────────────
class OrderBook {
public:
        void clear() {
    bids_.clear();
    asks_.clear();

    bid_levels_.clear();
    ask_levels_.clear();

    last_update_ts_ = 0;
    sequence_ = 0;
    cross_events_ = 0;
    total_buy_vol_ = 0.0;
    total_sell_vol_ = 0.0;

    trade_head_ = 0;
    trade_count_ = 0;

    std::fill(trade_ring_.begin(), trade_ring_.end(), Trade{});
}
    // Bids: descending by price (highest first)
    using BidMap = std::map<double, double, std::greater<double>>;
    // Asks: ascending by price (lowest first)
    using AskMap = std::map<double, double>;

    explicit OrderBook(
        std::string symbol,
        double      tick_size        = 1e-8,
        bool        check_integrity  = false,
        bool        ts_monotonic     = true
    )
        : symbol_(std::move(symbol))
        , tick_size_(tick_size)
        , check_integrity_(check_integrity)
        , ts_monotonic_(ts_monotonic)
        , seq_tracker_(symbol_)
        , trade_ring_(kTradeRingSize)
    {}

    // Non-copyable
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;


    
    // ── L2 Feed ──────────────────────────────────────────────────────────────

    void apply_l2(const L2Update& upd) {
        if (ts_monotonic_ && upd.timestamp < last_update_ts_) return;
        if (upd.seq && !seq_tracker_.update(upd.seq)) return;

        bool     is_bid = (upd.side == BookSide::Bid);
        BidMap&  bid_book = bids_;
        AskMap&  ask_book = asks_;
        auto&    levels   = is_bid ? bid_levels_ : ask_levels_;
        double   price    = upd.price;

        if (upd.qty <= 1e-12) {
            // Remove level
            levels.erase(price);
            if (is_bid) bid_book.erase(price);
            else        ask_book.erase(price);
        } else {
            auto it = levels.find(price);
            if (it == levels.end()) {
                levels.emplace(price, std::make_unique<PriceLevel>(price));
                it = levels.find(price);
            }
            PriceLevel& lvl   = *it->second;
            double      delta = upd.qty - lvl.total_qty;
            if (delta > 1e-12) {
                // Qty increased: append synthetic order at back
                std::string synth_id = "_l2_" + std::to_string(sequence_);
                lvl.add_order(synth_id, delta);
            } else if (delta < -1e-12) {
                // Qty decreased: FIFO consume from front
                lvl.consume_qty(-delta);
            }
            lvl.total_qty = upd.qty;  // authoritative
            if (is_bid) bid_book[price] = upd.qty;
            else        ask_book[price] = upd.qty;
        }

        post_update(upd.timestamp);
        if (check_integrity_) assert_integrity();
    }

    // ── L3 Feed ──────────────────────────────────────────────────────────────

    void apply_l3(const L3Update& upd) {
        if (ts_monotonic_ && upd.timestamp < last_update_ts_) return;
        if (upd.seq && !seq_tracker_.update(upd.seq)) return;

        bool    is_bid = (upd.side == Side::Buy);
        auto&   levels = is_bid ? bid_levels_ : ask_levels_;
        double  price  = upd.price;

        switch (upd.event) {
            case L3Event::Add: {
                if (levels.find(price) == levels.end())
                    levels.emplace(price, std::make_unique<PriceLevel>(price));
                PriceLevel& lvl = *levels[price];
                lvl.add_order(upd.order_id, upd.qty);
                if (is_bid) bids_[price] = lvl.total_qty;
                else        asks_[price] = lvl.total_qty;
                break;
            }
            case L3Event::Modify: {
                auto it = levels.find(price);
                if (it != levels.end()) {
                    it->second->modify_order(upd.order_id, upd.qty);
                    double tq = it->second->total_qty;
                    if (tq <= 1e-12) {
                        levels.erase(it);
                        if (is_bid) bids_.erase(price);
                        else        asks_.erase(price);
                    } else {
                        if (is_bid) bids_[price] = tq;
                        else        asks_[price] = tq;
                    }
                }
                break;
            }
            case L3Event::Delete: {
                auto it = levels.find(price);
                if (it != levels.end()) {
                    it->second->delete_order(upd.order_id);
                    double tq = it->second->total_qty;
                    if (tq <= 1e-12) {
                        levels.erase(it);
                        if (is_bid) bids_.erase(price);
                        else        asks_.erase(price);
                    } else {
                        if (is_bid) bids_[price] = tq;
                        else        asks_[price] = tq;
                    }
                }
                break;
            }
            case L3Event::Trade: {
                auto it = levels.find(price);
                if (it != levels.end()) {
                    it->second->consume_qty(upd.qty);
                    double tq = it->second->total_qty;
                    if (tq <= 1e-12) {
                        levels.erase(it);
                        if (is_bid) bids_.erase(price);
                        else        asks_.erase(price);
                    } else {
                        if (is_bid) bids_[price] = tq;
                        else        asks_[price] = tq;
                    }
                }
                break;
            }
        }

        post_update(upd.timestamp);
    }

    // ── Our Orders ────────────────────────────────────────────────────────────

    void register_our_order(Order& order) {
        bool    is_bid = (order.side == Side::Buy);
        auto&   levels = is_bid ? bid_levels_ : ask_levels_;
        double  price  = order.price;

        if (levels.find(price) == levels.end())
            levels.emplace(price, std::make_unique<PriceLevel>(price));
        PriceLevel& lvl  = *levels[price];
        order.queue_ahead = lvl.total_qty;
        order.queue_pos   = static_cast<int32_t>(lvl.live_count());
        lvl.add_order(order.order_id, order.qty);
        if (is_bid) bids_[price] = lvl.total_qty;
        else        asks_[price] = lvl.total_qty;
    }

    void cancel_our_order(const Order& order) {
        bool    is_bid = (order.side == Side::Buy);
        auto&   levels = is_bid ? bid_levels_ : ask_levels_;
        double  price  = order.price;

        auto it = levels.find(price);
        if (it != levels.end()) {
            it->second->delete_order(order.order_id);
            if (it->second->empty()) {
                levels.erase(it);
                if (is_bid) bids_.erase(price);
                else        asks_.erase(price);
            } else {
                double tq = it->second->total_qty;
                if (is_bid) bids_[price] = tq;
                else        asks_[price] = tq;
            }
        }
    }

    double qty_ahead_of_order(const Order& order) const {
        bool    is_bid = (order.side == Side::Buy);
        const auto& levels = is_bid ? bid_levels_ : ask_levels_;
        auto it = levels.find(order.price);
        if (it == levels.end()) return 0.0;
        return it->second->qty_ahead_of(order.order_id);
    }

    // Consume qty from a level (used by fill simulator)
    void consume_level_qty(double price, bool is_bid, double qty) {
        auto& levels = is_bid ? bid_levels_ : ask_levels_;
        auto  it     = levels.find(price);
        if (it == levels.end()) return;
        it->second->consume_qty(qty);
        if (it->second->empty()) {
            levels.erase(it);
            if (is_bid) bids_.erase(price);
            else        asks_.erase(price);
        } else {
            double tq = it->second->total_qty;
            if (is_bid) bids_[price] = tq;
            else        asks_[price] = tq;
        }
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    std::optional<double> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    std::optional<double> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    std::optional<double> mid_price() const noexcept {
        auto bb = best_bid(), ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return (*bb + *ba) * 0.5;
    }

    std::optional<double> spread() const noexcept {
        auto bb = best_bid(), ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return *ba - *bb;
    }

    std::optional<double> spread_bps() const noexcept {
        auto bb = best_bid(), ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        double mid = (*bb + *ba) * 0.5;
        if (mid <= 1e-12) return std::nullopt;
        return std::optional<double>{(*ba - *bb) / mid * 10000.0};
    }

    // Fill `out` with up to `n` bid levels (descending price)
    std::vector<DepthLevel> bid_depth(size_t n = 10) const {
        std::vector<DepthLevel> out;
        out.reserve(std::min(n, bids_.size()));
        for (auto it = bids_.begin(); it != bids_.end() && out.size() < n; ++it)
            out.push_back({it->first, it->second});
        return out;
    }

    // Fill `out` with up to `n` ask levels (ascending price)
    std::vector<DepthLevel> ask_depth(size_t n = 10) const {
        std::vector<DepthLevel> out;
        out.reserve(std::min(n, asks_.size()));
        for (auto it = asks_.begin(); it != asks_.end() && out.size() < n; ++it)
            out.push_back({it->first, it->second});
        return out;
    }

    double total_bid_qty(size_t levels = 10) const noexcept {
    double s = 0.0;
    size_t n = 0;
    for (auto it = bids_.begin(); it != bids_.end() && n < levels; ++it, ++n)
        s += it->second;
    return s;
     }

    double total_ask_qty(size_t levels = 10) const noexcept {
    double s = 0.0;
    size_t n = 0;
    for (auto it = asks_.begin(); it != asks_.end() && n < levels; ++it, ++n)
        s += it->second;
    return s;
    }

    double imbalance(size_t levels = 5) const noexcept {
        double bq = 0.0, aq = 0.0;
        size_t n = 0;
        for (auto it = bids_.begin(); it != bids_.end() && n < levels; ++it, ++n)
            bq += it->second;
        n = 0;
        for (auto it = asks_.begin(); it != asks_.end() && n < levels; ++it, ++n)
            aq += it->second;
        double t = bq + aq;
        return t > 1e-12 ? (bq - aq) / t : 0.0;
    }

    double pressure_ratio(size_t levels = 5) const noexcept {
        double bq = 0.0, aq = 0.0;
        size_t n = 0;
        for (auto it = bids_.begin(); it != bids_.end() && n < levels; ++it, ++n) bq += it->second;
        n = 0;
        for (auto it = asks_.begin(); it != asks_.end() && n < levels; ++it, ++n) aq += it->second;
        return aq > 1e-12 ? bq / aq : 1e18;
    }

    std::optional<double> vwap_to_fill(Side side, double target_qty) const {
        double rem = target_qty, cost = 0.0;
        auto sweep = (side == Side::Buy) ? ask_depth(200) : bid_depth(200);
        for (auto& lv : sweep) {
            double take  = std::min(lv.qty, rem);
            cost += take * lv.price;
            rem  -= take;
            if (rem <= 1e-12) break;
        }
        if (rem > 1e-12) return std::nullopt;
        return cost / target_qty;
    }

    void record_trade(const Trade& t) {
        trade_ring_[trade_head_] = t;
        trade_head_ = (trade_head_ + 1) % kTradeRingSize;
        if (trade_count_ < kTradeRingSize) ++trade_count_;
        if (t.aggressor == Side::Buy) total_buy_vol_  += t.qty;
        else                          total_sell_vol_ += t.qty;
    }

    // Buy vol / total vol over last `window` trades.  0.5 = balanced.
    double trade_flow_ratio(size_t window = 500) const noexcept {
        size_t n    = std::min(window, trade_count_);
        if (n == 0) return 0.5;
        double buy  = 0.0, total = 0.0;
        for (size_t i = 0; i < n; ++i) {
            // Walk backwards from head
            size_t idx = (trade_head_ + kTradeRingSize - 1 - i) % kTradeRingSize;
            const Trade& t = trade_ring_[idx];
            total += t.qty;
            if (t.aggressor == Side::Buy) buy += t.qty;
        }
        return total > 1e-12 ? buy / total : 0.5;
    }

    // ── Bybit CRC32 checksum validation ──────────────────────────────────────

    bool validate_bybit_checksum(uint32_t checksum, size_t levels = 25) const {
        auto bids = bid_depth(levels);
        auto asks = ask_depth(levels);
        size_t n  = std::min({bids.size(), asks.size(), levels});

        std::ostringstream oss;
        bool first = true;
        for (size_t i = 0; i < n; ++i) {
            auto append = [&](double v) {
                if (!first) oss << '|';
                // Trim trailing zeros for CRC string matching
                char buf[64];
                snprintf(buf, sizeof(buf), "%.10g", v);
                oss << buf;
                first = false;
            };
            append(bids[i].price); append(bids[i].qty);
            append(asks[i].price); append(asks[i].qty);
        }
        std::string raw  = oss.str();
        uint32_t    crc  = crc32_compute(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
        return crc == checksum;
    }

    // ── Accessors for fill simulator (direct level access) ────────────────────

    PriceLevel* get_level(double price, bool is_bid) const {
        const auto& levels = is_bid ? bid_levels_ : ask_levels_;
        auto it = levels.find(price);
        return (it != levels.end()) ? it->second.get() : nullptr;
    }

    // ── State accessors ───────────────────────────────────────────────────────

    const std::string& symbol()       const noexcept { return symbol_; }
    int64_t  last_update_ts()         const noexcept { return last_update_ts_; }
    int64_t  sequence()               const noexcept { return sequence_; }
    int64_t  cross_events()           const noexcept { return cross_events_; }
    bool     needs_resync()           const noexcept { return seq_tracker_.needs_resync(); }
    double   total_buy_vol()          const noexcept { return total_buy_vol_; }
    double   total_sell_vol()         const noexcept { return total_sell_vol_; }

    // Book maps: price → aggregate qty.  Exposed for signal layer and testing.
    BidMap bids_;   // descending (highest bid first)
    AskMap asks_;   // ascending  (lowest ask first)

private:
    static constexpr size_t kTradeRingSize = 50'000;

    std::string        symbol_;
    double             tick_size_       = 1e-8;
    bool               check_integrity_ = false;
    bool               ts_monotonic_    = true;
    SequenceTracker    seq_tracker_;

    // Owned PriceLevels: price → unique_ptr<PriceLevel>
    std::unordered_map<double, std::unique_ptr<PriceLevel>> bid_levels_;
    std::unordered_map<double, std::unique_ptr<PriceLevel>> ask_levels_;

    int64_t  last_update_ts_ = 0;
    int64_t  sequence_       = 0;
    int64_t  cross_events_   = 0;
    double   total_buy_vol_  = 0.0;
    double   total_sell_vol_ = 0.0;

    // Ring buffer for recent trades (heap-allocated to avoid stack overflow)
    std::vector<Trade> trade_ring_;
    size_t trade_head_  = 0;
    size_t trade_count_ = 0;

    void post_update(int64_t ts) {
        last_update_ts_ = std::max(last_update_ts_, ts);
        ++sequence_;
        check_crossed();
    }

    void check_crossed() {
        auto bb = best_bid(), ba = best_ask();
        if (!bb || !ba) return;
        if (*bb >= *ba) {
            ++cross_events_;
            // Remove the stale top-bid level
            double offending = *bb;
            bid_levels_.erase(offending);
            bids_.erase(offending);
        }
    }

    void assert_integrity() const {
        for (auto& [price, lvl] : bid_levels_) {
            auto it = bids_.find(price);
            [[maybe_unused]] double book_qty = (it != bids_.end()) ? it->second : 0.0;
            assert(std::abs(lvl->total_qty - book_qty) < 1e-6);
        }
        for (auto& [price, lvl] : ask_levels_) {
            auto it = asks_.find(price);
            [[maybe_unused]] double book_qty = (it != asks_.end()) ? it->second : 0.0;
            assert(std::abs(lvl->total_qty - book_qty) < 1e-6);
        }
        auto bb = best_bid(), ba = best_ask();
        if (bb && ba) assert(*bb < *ba);
    }

    // Portable CRC32 (no external dependency)
    static uint32_t crc32_compute(const uint8_t* data, size_t len) noexcept {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int j = 0; j < 8; ++j)
                crc = (crc >> 1) ^ (0xEDB88320u & ~((crc & 1) - 1));
        }
        return crc ^ 0xFFFFFFFFu;
    }
};

} // namespace hft