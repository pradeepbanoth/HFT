#pragma once

#include "exchange_bus.hpp"
#include "lock_free_market_data_cache.hpp"
#include "gap_recovery_engine.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft::marketdata {

using TimestampNs = uint64_t;

inline TimestampNs md_now_ns() noexcept {
    return static_cast<TimestampNs>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

enum class MdPacketType : uint8_t {
    Unknown,
    Snapshot,
    L2Update,
    Trade,
    Heartbeat,
    Reset
};

enum class MdSide : uint8_t {
    Bid,
    Ask
};

enum class FeedState : uint8_t {
    Cold,
    Live,
    GapDetected,
    SnapshotPending,
    Recovering,
    Stale,
    Failed
};

enum class SequenceStatus : uint8_t {
    FirstPacket,
    Valid,
    Duplicate,
    Old,
    Gap,
    BufferedOutOfOrder
};

struct RawMarketPacket {
    std::string venue;
    std::string symbol;
    MdPacketType type{MdPacketType::Unknown};

    uint64_t sequence{0};
    uint64_t exchange_ts_ns{0};
    uint64_t receive_ts_ns{0};

    MdSide side{MdSide::Bid};
    double price{0.0};
    double qty{0.0};

    bool is_trade_buy{false};
};

struct BookLevel {
    double price{0.0};
    double qty{0.0};
};

struct BookSnapshot {
    std::string symbol;
    uint64_t last_sequence{0};
    std::vector<BookLevel> bids;
    std::vector<BookLevel> asks;
};

struct FeedMetricsSnapshot {
    uint64_t packets_received{0};
    uint64_t packets_applied{0};
    uint64_t packets_dropped{0};
    uint64_t packets_duplicate{0};
    uint64_t packets_buffered{0};
    uint64_t sequence_gaps{0};
    uint64_t snapshot_requests{0};
    uint64_t heartbeats{0};
    uint64_t stale_events{0};

    double avg_latency_us{0.0};
    double max_latency_us{0.0};
    double packet_loss_rate{0.0};
};

struct FeedMetrics {
    std::atomic<uint64_t> packets_received{0};
    std::atomic<uint64_t> packets_applied{0};
    std::atomic<uint64_t> packets_dropped{0};
    std::atomic<uint64_t> packets_duplicate{0};
    std::atomic<uint64_t> packets_buffered{0};
    std::atomic<uint64_t> sequence_gaps{0};
    std::atomic<uint64_t> snapshot_requests{0};
    std::atomic<uint64_t> heartbeats{0};
    std::atomic<uint64_t> stale_events{0};

    std::atomic<uint64_t> latency_count{0};
    std::atomic<uint64_t> latency_total_ns{0};
    std::atomic<uint64_t> latency_max_ns{0};

    void observe_latency(uint64_t latency_ns) noexcept {
        latency_count.fetch_add(1, std::memory_order_relaxed);
        latency_total_ns.fetch_add(latency_ns, std::memory_order_relaxed);

        auto cur = latency_max_ns.load(std::memory_order_relaxed);
        while (latency_ns > cur &&
               !latency_max_ns.compare_exchange_weak(
                   cur,
                   latency_ns,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed
               )) {}
    }

    FeedMetricsSnapshot snapshot() const noexcept {
        FeedMetricsSnapshot s;

        s.packets_received = packets_received.load(std::memory_order_relaxed);
        s.packets_applied = packets_applied.load(std::memory_order_relaxed);
        s.packets_dropped = packets_dropped.load(std::memory_order_relaxed);
        s.packets_duplicate = packets_duplicate.load(std::memory_order_relaxed);
        s.packets_buffered = packets_buffered.load(std::memory_order_relaxed);
        s.sequence_gaps = sequence_gaps.load(std::memory_order_relaxed);
        s.snapshot_requests = snapshot_requests.load(std::memory_order_relaxed);
        s.heartbeats = heartbeats.load(std::memory_order_relaxed);
        s.stale_events = stale_events.load(std::memory_order_relaxed);

        const auto n = latency_count.load(std::memory_order_relaxed);
        const auto total = latency_total_ns.load(std::memory_order_relaxed);
        const auto maxv = latency_max_ns.load(std::memory_order_relaxed);

        s.avg_latency_us = n == 0 ? 0.0 : static_cast<double>(total) / n / 1000.0;
        s.max_latency_us = static_cast<double>(maxv) / 1000.0;

        if (s.packets_received > 0) {
            s.packet_loss_rate =
                static_cast<double>(s.packets_dropped) /
                static_cast<double>(s.packets_received);
        }

        return s;
    }
};

class SymbolNormalizer {
public:
    void add_mapping(std::string external, std::string internal) {
        std::scoped_lock lock(mutex_);
        mappings_[std::move(external)] = std::move(internal);
    }

    std::string normalize(const std::string& external) const {
        std::scoped_lock lock(mutex_);
        auto it = mappings_.find(external);
        return it == mappings_.end() ? external : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::string> mappings_;
};

class DuplicateFilter {
public:
    explicit DuplicateFilter(std::size_t max_seen = 4096)
        : max_seen_(max_seen) {}

    bool seen_or_insert(const std::string& key, uint64_t sequence) {
        std::scoped_lock lock(mutex_);

        auto& state = states_[key];

        if (state.seen.contains(sequence)) {
            return true;
        }

        state.seen.insert(sequence);
        state.order.push_back(sequence);

        while (state.order.size() > max_seen_) {
            state.seen.erase(state.order.front());
            state.order.pop_front();
        }

        return false;
    }

private:
    struct State {
        std::unordered_set<uint64_t> seen;
        std::deque<uint64_t> order;
    };

    mutable std::mutex mutex_;
    std::size_t max_seen_;
    std::unordered_map<std::string, State> states_;
};

class SequenceValidator {
public:
    SequenceStatus validate(const std::string& key, uint64_t sequence) {
        std::scoped_lock lock(mutex_);

        auto& last = last_sequence_[key];

        if (last == 0) {
            last = sequence;
            return SequenceStatus::FirstPacket;
        }

        if (sequence <= last) {
            return sequence == last ? SequenceStatus::Duplicate : SequenceStatus::Old;
        }

        if (sequence == last + 1) {
            last = sequence;
            return SequenceStatus::Valid;
        }

        return SequenceStatus::Gap;
    }

    void force_advance(const std::string& key, uint64_t sequence) {
        std::scoped_lock lock(mutex_);
        auto& last = last_sequence_[key];
        if (sequence > last) last = sequence;
    }

    uint64_t last_sequence(const std::string& key) const {
        std::scoped_lock lock(mutex_);
        auto it = last_sequence_.find(key);
        return it == last_sequence_.end() ? 0 : it->second;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, uint64_t> last_sequence_;
};

class OutOfOrderBuffer {
public:
    explicit OutOfOrderBuffer(std::size_t max_depth = 1024)
        : max_depth_(max_depth) {}

    bool buffer(const std::string& key, RawMarketPacket packet) {
        std::scoped_lock lock(mutex_);

        auto& q = buffers_[key];

        if (q.size() >= max_depth_) {
            return false;
        }

        q.emplace(packet.sequence, std::move(packet));
        return true;
    }

    std::vector<RawMarketPacket> pop_ready(const std::string& key, uint64_t last_sequence) {
        std::scoped_lock lock(mutex_);

        std::vector<RawMarketPacket> ready;

        auto it = buffers_.find(key);
        if (it == buffers_.end()) return ready;

        auto& q = it->second;
        auto expected = last_sequence + 1;

        while (!q.empty()) {
            auto first = q.begin();

            if (first->first != expected) break;

            ready.push_back(std::move(first->second));
            q.erase(first);
            ++expected;
        }

        return ready;
    }

    void clear(const std::string& key) {
        std::scoped_lock lock(mutex_);
        buffers_.erase(key);
    }

private:
    mutable std::mutex mutex_;
    std::size_t max_depth_;
    std::unordered_map<std::string, std::map<uint64_t, RawMarketPacket>> buffers_;
};

class SnapshotManager {
public:
    void request(const std::string& key) {
        std::scoped_lock lock(mutex_);
        pending_.insert(key);
        states_[key] = FeedState::SnapshotPending;
    }

    void synced(const std::string& key) {
        std::scoped_lock lock(mutex_);
        pending_.erase(key);
        states_[key] = FeedState::Live;
    }

    bool pending(const std::string& key) const {
        std::scoped_lock lock(mutex_);
        return pending_.contains(key);
    }

    FeedState state(const std::string& key) const {
        std::scoped_lock lock(mutex_);
        auto it = states_.find(key);
        return it == states_.end() ? FeedState::Cold : it->second;
    }

    void set_state(const std::string& key, FeedState state) {
        std::scoped_lock lock(mutex_);
        states_[key] = state;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> pending_;
    std::unordered_map<std::string, FeedState> states_;
};

class FeedHealthMonitor {
public:
    explicit FeedHealthMonitor(uint64_t heartbeat_timeout_ns = 5'000'000'000ULL)
        : heartbeat_timeout_ns_(heartbeat_timeout_ns) {}

    void mark_seen(const std::string& key) {
        std::scoped_lock lock(mutex_);
        last_seen_[key] = md_now_ns();
    }

    bool stale(const std::string& key) const {
        std::scoped_lock lock(mutex_);

        auto it = last_seen_.find(key);
        if (it == last_seen_.end()) return true;

        return md_now_ns() - it->second > heartbeat_timeout_ns_;
    }

    void set_timeout_ns(uint64_t timeout_ns) noexcept {
        heartbeat_timeout_ns_ = timeout_ns;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TimestampNs> last_seen_;
    uint64_t heartbeat_timeout_ns_;
};

class BookCache {
public:
    void reset(const std::string& symbol) {
        std::scoped_lock lock(mutex_);
        books_[symbol] = Book{};
    }

    void apply_l2(
        const std::string& symbol,
        MdSide side,
        double price,
        double qty,
        uint64_t sequence
    ) {
        std::scoped_lock lock(mutex_);

        auto& book = books_[symbol];
        auto& levels = side == MdSide::Bid ? book.bids : book.asks;

        if (qty <= 0.0) {
            levels.erase(price);
        } else {
            levels[price] = qty;
        }

        book.last_sequence = sequence;
    }

    BookSnapshot snapshot(const std::string& symbol, std::size_t depth = 10) const {
        std::scoped_lock lock(mutex_);

        BookSnapshot out;
        out.symbol = symbol;

        auto it = books_.find(symbol);
        if (it == books_.end()) return out;

        out.last_sequence = it->second.last_sequence;

        for (auto rit = it->second.bids.rbegin(); rit != it->second.bids.rend(); ++rit) {
            if (out.bids.size() >= depth) break;
            out.bids.push_back({rit->first, rit->second});
        }

        for (const auto& [price, qty] : it->second.asks) {
            if (out.asks.size() >= depth) break;
            out.asks.push_back({price, qty});
        }

        return out;
    }

private:
    struct Book {
        uint64_t last_sequence{0};
        std::map<double, double> bids;
        std::map<double, double> asks;
    };

    mutable std::mutex mutex_;
    std::unordered_map<std::string, Book> books_;
};

class MarketDataEngine {
public:
    explicit MarketDataEngine(hft::exchange::ExchangeBus* bus = nullptr)
        : bus_(bus) {}

    void add_symbol_mapping(const std::string& external, const std::string& internal) {
        normalizer_.add_mapping(external, internal);
    }

    void set_heartbeat_timeout_ns(uint64_t timeout_ns) {
        health_.set_timeout_ns(timeout_ns);
    }

    bool on_packet(RawMarketPacket packet) {
        metrics_.packets_received.fetch_add(1, std::memory_order_relaxed);

        if (packet.receive_ts_ns == 0) {
            packet.receive_ts_ns = md_now_ns();
        }

        observe_latency(packet);

        const auto symbol = normalizer_.normalize(packet.symbol);
        const auto key = make_key(packet.venue, symbol);

        health_.mark_seen(key);

        if (dedupe_.seen_or_insert(key, packet.sequence)) {
            metrics_.packets_duplicate.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (packet.type == MdPacketType::Heartbeat) {
            metrics_.heartbeats.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (packet.type == MdPacketType::Reset) {
            reset_symbol(key, symbol);
            return true;
        }

        const auto status = sequence_.validate(key, packet.sequence);

        if (status == SequenceStatus::Old || status == SequenceStatus::Duplicate) {
            metrics_.packets_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        if (status == SequenceStatus::Gap) {
            metrics_.sequence_gaps.fetch_add(1, std::memory_order_relaxed);
            metrics_.snapshot_requests.fetch_add(1, std::memory_order_relaxed);

            snapshots_.request(key);

            if (!ooo_.buffer(key, packet)) {
                metrics_.packets_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                metrics_.packets_buffered.fetch_add(1, std::memory_order_relaxed);
            }

            return true;
        }

       

        const bool applied = apply_packet(key, symbol, packet);

        if (!applied) {
            metrics_.packets_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        drain_ready_buffer(key, symbol);
        return true;
    }

    std::optional<TopOfBookSnapshot> top_of_book(const std::string& symbol) const {
        return md_cache_.top_of_book(symbol);
    }

    std::vector<CachedTrade> recent_trades(const std::string& symbol) const {
        return md_cache_.recent_trades(symbol);
    }

    std::optional<CachedTrade> latest_trade(const std::string& symbol) const {
        return md_cache_.latest_trade(symbol);
    }

    MarketDataCacheStatsSnapshot cache_stats() const noexcept {
        return md_cache_.stats_snapshot();
    }

    BookSnapshot book(const std::string& symbol, std::size_t depth = 10) const {
        return book_cache_.snapshot(symbol, depth);
    }

    bool snapshot_pending(const std::string& venue, const std::string& symbol) const {
        return snapshots_.pending(make_key(venue, normalizer_.normalize(symbol)));
    }

    bool stale(const std::string& venue, const std::string& symbol) const {
        return health_.stale(make_key(venue, normalizer_.normalize(symbol)));
    }

    FeedState state(const std::string& venue, const std::string& symbol) const {
        return snapshots_.state(make_key(venue, normalizer_.normalize(symbol)));
    }

    FeedMetricsSnapshot metrics_snapshot() const noexcept {
        return metrics_.snapshot();
    }

private:
    static std::string make_key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    void reset_symbol(const std::string& key, const std::string& symbol) {
        book_cache_.reset(symbol);
        ooo_.clear(key);
        snapshots_.request(key);
        metrics_.snapshot_requests.fetch_add(1, std::memory_order_relaxed);
    }

    bool apply_packet(
        const std::string& key,
        const std::string& symbol,
        const RawMarketPacket& packet
    ) {
        if (packet.type == MdPacketType::Snapshot) {
            book_cache_.reset(symbol);
            snapshots_.synced(key);
            ooo_.clear(key);
            metrics_.packets_applied.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (packet.type == MdPacketType::L2Update) {
            book_cache_.apply_l2(symbol, packet.side, packet.price, packet.qty, packet.sequence);

            if (packet.side == MdSide::Bid) {
                md_cache_.update_bid(
                    symbol,
                    packet.price,
                    packet.qty,
                    packet.sequence,
                    packet.receive_ts_ns,
                    packet.exchange_ts_ns
                );
            } else {
                md_cache_.update_ask(
                    symbol,
                    packet.price,
                    packet.qty,
                    packet.sequence,
                    packet.receive_ts_ns,
                    packet.exchange_ts_ns
                );
            }

            sequence_.force_advance(key, packet.sequence);
            snapshots_.set_state(key, FeedState::Live);
            publish_l2(symbol, packet);
            metrics_.packets_applied.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        if (packet.type == MdPacketType::Trade) {
            md_cache_.update_trade(
                symbol,
                CachedTrade{
                    packet.price,
                    packet.qty,
                    packet.is_trade_buy,
                    packet.sequence,
                    packet.exchange_ts_ns,
                    packet.receive_ts_ns
                }
            );

            sequence_.force_advance(key, packet.sequence);
            publish_trade(symbol, packet);
            metrics_.packets_applied.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        return false;
    }

    void drain_ready_buffer(const std::string& key, const std::string& symbol) {
        while (true) {
            const auto last = sequence_.last_sequence(key);
            auto ready = ooo_.pop_ready(key, last);

            if (ready.empty()) break;

            for (const auto& packet : ready) {
                apply_packet(key, symbol, packet);
            }
        }
    }

    void observe_latency(const RawMarketPacket& packet) noexcept {
        if (packet.exchange_ts_ns == 0 || packet.receive_ts_ns <= packet.exchange_ts_ns) return;
        metrics_.observe_latency(packet.receive_ts_ns - packet.exchange_ts_ns);
    }

    void publish_l2(const std::string& symbol, const RawMarketPacket& packet) {
        if (!bus_ || !bus_->running()) return;

        hft::exchange::L2Update event;
        event.symbol = symbol;
        event.price = packet.price;
        event.qty = packet.qty;
        event.is_bid = packet.side == MdSide::Bid;
        event.level = 0;

        bus_->publish(
            hft::exchange::ExchangeEventType::L2Update,
            std::move(event),
            hft::exchange::EventPriority::High,
            0,
            0,
            packet.sequence
        );
    }

    void publish_trade(const std::string& symbol, const RawMarketPacket& packet) {
        if (!bus_ || !bus_->running()) return;

        hft::exchange::TradeEvent event;
        event.symbol = symbol;
        event.price = packet.price;
        event.qty = packet.qty;
        event.aggressor_buy = packet.is_trade_buy;

        bus_->publish(
            hft::exchange::ExchangeEventType::Trade,
            std::move(event),
            hft::exchange::EventPriority::High,
            0,
            0,
            packet.sequence
        );
    }

private:
    hft::exchange::ExchangeBus* bus_{nullptr};

    SymbolNormalizer normalizer_;
    DuplicateFilter dedupe_;
    SequenceValidator sequence_;
    OutOfOrderBuffer ooo_;
    SnapshotManager snapshots_;
    FeedHealthMonitor health_;
    BookCache book_cache_;
    LockFreeMarketDataCache<2048> md_cache_;

    FeedMetrics metrics_{};
};

} // namespace hft::marketdata