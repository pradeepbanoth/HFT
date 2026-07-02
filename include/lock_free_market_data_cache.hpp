#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::marketdata {

constexpr std::size_t MD_CACHE_LINE = 64;

inline uint64_t cache_now_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );
}

struct alignas(MD_CACHE_LINE) AtomicTopOfBook {
    std::atomic<uint64_t> version{0};

    std::atomic<double> bid_price{0.0};
    std::atomic<double> bid_qty{0.0};
    std::atomic<double> ask_price{0.0};
    std::atomic<double> ask_qty{0.0};

    std::atomic<uint64_t> bid_sequence{0};
    std::atomic<uint64_t> ask_sequence{0};
    std::atomic<uint64_t> last_sequence{0};

    std::atomic<uint64_t> update_ts_ns{0};
    std::atomic<uint64_t> exchange_ts_ns{0};
};

struct TopOfBookSnapshot {
    std::string symbol;

    double bid_price{0.0};
    double bid_qty{0.0};
    double ask_price{0.0};
    double ask_qty{0.0};

    uint64_t bid_sequence{0};
    uint64_t ask_sequence{0};
    uint64_t last_sequence{0};

    uint64_t update_ts_ns{0};
    uint64_t exchange_ts_ns{0};
    uint64_t version{0};

    bool valid() const noexcept {
        return bid_price > 0.0 &&
               ask_price > 0.0 &&
               bid_qty > 0.0 &&
               ask_qty > 0.0 &&
               bid_price <= ask_price;
    }

    bool crossed() const noexcept {
        return bid_price > 0.0 && ask_price > 0.0 && bid_price > ask_price;
    }

    double mid() const noexcept {
        return valid() ? (bid_price + ask_price) * 0.5 : 0.0;
    }

    double spread() const noexcept {
        return valid() ? ask_price - bid_price : 0.0;
    }

    double micro_price() const noexcept {
        const double total = bid_qty + ask_qty;
        if (!valid() || total <= 0.0) return 0.0;
        return ((ask_price * bid_qty) + (bid_price * ask_qty)) / total;
    }

    double imbalance() const noexcept {
        const double total = bid_qty + ask_qty;
        if (!valid() || total <= 0.0) return 0.0;
        return (bid_qty - ask_qty) / total;
    }

    bool stale(uint64_t now_ns, uint64_t max_age_ns) const noexcept {
        return update_ts_ns == 0 || now_ns - update_ts_ns > max_age_ns;
    }
};

struct CachedTrade {
    double price{0.0};
    double qty{0.0};
    bool aggressor_buy{false};
    uint64_t sequence{0};
    uint64_t exchange_ts_ns{0};
    uint64_t receive_ts_ns{0};
};

template <std::size_t Capacity>
class alignas(MD_CACHE_LINE) RecentTradeRing {
    static_assert(Capacity >= 2, "RecentTradeRing capacity must be >= 2");

public:
    void push(const CachedTrade& trade) noexcept {
        const uint64_t pos = write_pos_.fetch_add(1, std::memory_order_acq_rel);
        trades_[pos % Capacity] = trade;

        const std::size_t new_size =
            static_cast<std::size_t>((pos + 1) > Capacity ? Capacity : (pos + 1));

        size_.store(new_size, std::memory_order_release);
    }

    std::vector<CachedTrade> snapshot() const {
        const uint64_t write = write_pos_.load(std::memory_order_acquire);
        const std::size_t size = size_.load(std::memory_order_acquire);

        std::vector<CachedTrade> out;
        out.reserve(size);

        const uint64_t start = write >= size ? write - size : 0;

        for (uint64_t i = start; i < write; ++i) {
            out.push_back(trades_[i % Capacity]);
        }

        return out;
    }

    std::optional<CachedTrade> latest() const noexcept {
        const uint64_t write = write_pos_.load(std::memory_order_acquire);
        if (write == 0) return std::nullopt;
        return trades_[(write - 1) % Capacity];
    }

    std::size_t size() const noexcept {
        return size_.load(std::memory_order_acquire);
    }

private:
    std::array<CachedTrade, Capacity> trades_{};

    alignas(MD_CACHE_LINE) std::atomic<uint64_t> write_pos_{0};
    alignas(MD_CACHE_LINE) std::atomic<std::size_t> size_{0};
};

struct MarketDataCacheStatsSnapshot {
    uint64_t symbols_created{0};
    uint64_t tob_updates{0};
    uint64_t bid_updates{0};
    uint64_t ask_updates{0};
    uint64_t trade_updates{0};
    uint64_t snapshot_reads{0};
    uint64_t failed_snapshot_reads{0};
    uint64_t stale_reads{0};
    uint64_t crossed_books{0};
};

struct MarketDataCacheStats {
    std::atomic<uint64_t> symbols_created{0};
    std::atomic<uint64_t> tob_updates{0};
    std::atomic<uint64_t> bid_updates{0};
    std::atomic<uint64_t> ask_updates{0};
    std::atomic<uint64_t> trade_updates{0};
    std::atomic<uint64_t> snapshot_reads{0};
    std::atomic<uint64_t> failed_snapshot_reads{0};
    std::atomic<uint64_t> stale_reads{0};
    std::atomic<uint64_t> crossed_books{0};

    MarketDataCacheStatsSnapshot snapshot() const noexcept {
        return {
            symbols_created.load(std::memory_order_relaxed),
            tob_updates.load(std::memory_order_relaxed),
            bid_updates.load(std::memory_order_relaxed),
            ask_updates.load(std::memory_order_relaxed),
            trade_updates.load(std::memory_order_relaxed),
            snapshot_reads.load(std::memory_order_relaxed),
            failed_snapshot_reads.load(std::memory_order_relaxed),
            stale_reads.load(std::memory_order_relaxed),
            crossed_books.load(std::memory_order_relaxed)
        };
    }
};

template <std::size_t RecentTradeCapacity = 2048>
class LockFreeMarketDataCache {
private:
    struct alignas(MD_CACHE_LINE) SymbolCache {
        AtomicTopOfBook tob;
        RecentTradeRing<RecentTradeCapacity> trades;
    };

public:
    void update_bid(
        const std::string& symbol,
        double price,
        double qty,
        uint64_t sequence,
        uint64_t receive_ts_ns,
        uint64_t exchange_ts_ns = 0
    ) {
        auto& cache = get_or_create(symbol);

        begin_write(cache);
        cache.tob.bid_price.store(price, std::memory_order_release);
        cache.tob.bid_qty.store(qty, std::memory_order_release);
        cache.tob.bid_sequence.store(sequence, std::memory_order_release);
        cache.tob.last_sequence.store(sequence, std::memory_order_release);
        cache.tob.update_ts_ns.store(receive_ts_ns, std::memory_order_release);
        cache.tob.exchange_ts_ns.store(exchange_ts_ns, std::memory_order_release);
        end_write(cache);

        stats_.tob_updates.fetch_add(1, std::memory_order_relaxed);
        stats_.bid_updates.fetch_add(1, std::memory_order_relaxed);

        auto snap = top_of_book(symbol);
        if (snap && snap->crossed()) {
            stats_.crossed_books.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void update_ask(
        const std::string& symbol,
        double price,
        double qty,
        uint64_t sequence,
        uint64_t receive_ts_ns,
        uint64_t exchange_ts_ns = 0
    ) {
        auto& cache = get_or_create(symbol);

        begin_write(cache);
        cache.tob.ask_price.store(price, std::memory_order_release);
        cache.tob.ask_qty.store(qty, std::memory_order_release);
        cache.tob.ask_sequence.store(sequence, std::memory_order_release);
        cache.tob.last_sequence.store(sequence, std::memory_order_release);
        cache.tob.update_ts_ns.store(receive_ts_ns, std::memory_order_release);
        cache.tob.exchange_ts_ns.store(exchange_ts_ns, std::memory_order_release);
        end_write(cache);

        stats_.tob_updates.fetch_add(1, std::memory_order_relaxed);
        stats_.ask_updates.fetch_add(1, std::memory_order_relaxed);

        auto snap = top_of_book(symbol);
        if (snap && snap->crossed()) {
            stats_.crossed_books.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void update_trade(const std::string& symbol, const CachedTrade& trade) {
        auto& cache = get_or_create(symbol);
        cache.trades.push(trade);
        stats_.trade_updates.fetch_add(1, std::memory_order_relaxed);
    }

    std::optional<TopOfBookSnapshot> top_of_book(
        const std::string& symbol,
        uint64_t stale_after_ns = 0
    ) const {
        SymbolCache* cache = find(symbol);

        if (!cache) {
            stats_.failed_snapshot_reads.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        TopOfBookSnapshot out;
        out.symbol = symbol;

        for (int attempt = 0; attempt < 5; ++attempt) {
            const uint64_t v1 = cache->tob.version.load(std::memory_order_acquire);

            if (v1 & 1ULL) continue;

            out.bid_price = cache->tob.bid_price.load(std::memory_order_acquire);
            out.bid_qty = cache->tob.bid_qty.load(std::memory_order_acquire);
            out.ask_price = cache->tob.ask_price.load(std::memory_order_acquire);
            out.ask_qty = cache->tob.ask_qty.load(std::memory_order_acquire);

            out.bid_sequence = cache->tob.bid_sequence.load(std::memory_order_acquire);
            out.ask_sequence = cache->tob.ask_sequence.load(std::memory_order_acquire);
            out.last_sequence = cache->tob.last_sequence.load(std::memory_order_acquire);

            out.update_ts_ns = cache->tob.update_ts_ns.load(std::memory_order_acquire);
            out.exchange_ts_ns = cache->tob.exchange_ts_ns.load(std::memory_order_acquire);

            const uint64_t v2 = cache->tob.version.load(std::memory_order_acquire);

            if (v1 == v2 && !(v2 & 1ULL)) {
                out.version = v2;
                stats_.snapshot_reads.fetch_add(1, std::memory_order_relaxed);

                if (stale_after_ns > 0 && out.stale(cache_now_ns(), stale_after_ns)) {
                    stats_.stale_reads.fetch_add(1, std::memory_order_relaxed);
                    return std::nullopt;
                }

                return out;
            }
        }

        stats_.failed_snapshot_reads.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    std::optional<CachedTrade> latest_trade(const std::string& symbol) const {
        SymbolCache* cache = find(symbol);
        if (!cache) return std::nullopt;
        return cache->trades.latest();
    }

    std::vector<CachedTrade> recent_trades(const std::string& symbol) const {
        SymbolCache* cache = find(symbol);
        if (!cache) return {};
        return cache->trades.snapshot();
    }

    bool has_symbol(const std::string& symbol) const {
        return find(symbol) != nullptr;
    }

    std::size_t symbol_count() const {
        std::scoped_lock lock(mutex_);
        return symbols_.size();
    }

    MarketDataCacheStatsSnapshot stats_snapshot() const noexcept {
        return stats_.snapshot();
    }

private:
    static void begin_write(SymbolCache& cache) noexcept {
        cache.tob.version.fetch_add(1, std::memory_order_acq_rel);
    }

    static void end_write(SymbolCache& cache) noexcept {
        cache.tob.version.fetch_add(1, std::memory_order_release);
    }

    SymbolCache& get_or_create(const std::string& symbol) {
        std::scoped_lock lock(mutex_);

        auto it = symbols_.find(symbol);
        if (it != symbols_.end()) {
            return *it->second;
        }

        auto ptr = std::make_unique<SymbolCache>();
        auto [inserted, _] = symbols_.emplace(symbol, std::move(ptr));

        stats_.symbols_created.fetch_add(1, std::memory_order_relaxed);

        return *inserted->second;
    }

    SymbolCache* find(const std::string& symbol) const {
        std::scoped_lock lock(mutex_);

        auto it = symbols_.find(symbol);
        if (it == symbols_.end()) return nullptr;

        return it->second.get();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unique_ptr<SymbolCache>> symbols_;
    mutable MarketDataCacheStats stats_{};
};

} // namespace hft::marketdata