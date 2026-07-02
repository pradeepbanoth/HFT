#pragma once

#include "smart_order_router.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::routing {

struct VenueScoreConfig {
    double price_weight{0.30};
    double fee_weight{0.10};
    double latency_weight{0.15};
    double fill_weight{0.20};
    double reject_weight{0.15};
    double liquidity_weight{0.07};
    double spread_weight{0.03};

    double max_latency_us{5000.0};
    double max_fee_bps{25.0};
    double max_spread_bps{50.0};
    double min_score{0.0};

    double ewma_alpha{0.20};
    double default_fill_score{0.70};
    double default_reject_score{1.0};

    uint64_t stale_after_ns{5'000'000'000ULL};
};

struct VenueExecutionStats {
    std::atomic<uint64_t> orders_sent{0};
    std::atomic<uint64_t> fills{0};
    std::atomic<uint64_t> rejects{0};
    std::atomic<uint64_t> cancels{0};
    std::atomic<uint64_t> timeouts{0};

    std::atomic<uint64_t> last_update_ns{0};
    std::atomic<uint64_t> latency_ewma_us_x1000{0};
};

struct VenueScore {
    std::string venue;
    std::string symbol;

    double score{0.0};
    double price_score{0.0};
    double fee_score{0.0};
    double latency_score{0.0};
    double fill_score{0.0};
    double reject_score{0.0};
    double liquidity_score{0.0};
    double spread_score{0.0};

    double effective_price{0.0};
    double available_qty{0.0};
    bool healthy{true};
};

class VenueScoringEngine {
public:
    explicit VenueScoringEngine(VenueScoreConfig config = {})
        : config_(config) {
        normalize_weights();
    }

    void record_order_sent(const std::string& venue, const std::string& symbol, uint64_t now_ns = 0) {
        auto& s = stats_[key(venue, symbol)];
        s.orders_sent.fetch_add(1, std::memory_order_relaxed);
        touch(s, now_ns);
    }

    void record_fill(
        const std::string& venue,
        const std::string& symbol,
        uint64_t fill_latency_us = 0,
        uint64_t now_ns = 0
    ) {
        auto& s = stats_[key(venue, symbol)];
        s.fills.fetch_add(1, std::memory_order_relaxed);

        if (fill_latency_us > 0) {
            update_latency_ewma(s, fill_latency_us);
        }

        touch(s, now_ns);
    }

    void record_reject(const std::string& venue, const std::string& symbol, uint64_t now_ns = 0) {
        auto& s = stats_[key(venue, symbol)];
        s.rejects.fetch_add(1, std::memory_order_relaxed);
        touch(s, now_ns);
    }

    void record_cancel(const std::string& venue, const std::string& symbol, uint64_t now_ns = 0) {
        auto& s = stats_[key(venue, symbol)];
        s.cancels.fetch_add(1, std::memory_order_relaxed);
        touch(s, now_ns);
    }

    void record_timeout(const std::string& venue, const std::string& symbol, uint64_t now_ns = 0) {
        auto& s = stats_[key(venue, symbol)];
        s.timeouts.fetch_add(1, std::memory_order_relaxed);
        touch(s, now_ns);
    }

    std::vector<VenueScore> score_quotes(
        const RouterOrder& order,
        const std::vector<VenueQuote>& quotes,
        uint64_t now_ns = 0
    ) const {
        std::vector<VenueQuote> valid;

        for (const auto& q : quotes) {
            if (!q.healthy) continue;
            if (q.symbol != order.symbol) continue;

            const double px = quote_price(order, q);
            const double qty = quote_qty(order, q);

            if (px <= 0.0 || qty <= 0.0) continue;

            if (order.type == OrderType::Limit) {
                if (order.side == Side::Buy && px > order.limit_price) continue;
                if (order.side == Side::Sell && px < order.limit_price) continue;
            }

            if (is_stats_stale(q.venue, q.symbol, now_ns)) {
                continue;
            }

            valid.push_back(q);
        }

        std::vector<VenueScore> scores;
        scores.reserve(valid.size());

        if (valid.empty()) return scores;

        const auto best_px = best_effective_price(order, valid);
        const auto best_liquidity = best_available_qty(order, valid);

        for (const auto& q : valid) {
            auto score = score_one(order, q, best_px, best_liquidity);

            if (score.score >= config_.min_score) {
                scores.push_back(std::move(score));
            }
        }

        std::sort(
            scores.begin(),
            scores.end(),
            [](const VenueScore& a, const VenueScore& b) {
                if (a.score == b.score) {
                    return a.effective_price < b.effective_price;
                }
                return a.score > b.score;
            }
        );

        return scores;
    }

    std::optional<VenueScore> best_venue(
        const RouterOrder& order,
        const std::vector<VenueQuote>& quotes,
        uint64_t now_ns = 0
    ) const {
        auto scores = score_quotes(order, quotes, now_ns);
        if (scores.empty()) return std::nullopt;
        return scores.front();
    }

    std::vector<VenueQuote> rank_quotes(
        const RouterOrder& order,
        const std::vector<VenueQuote>& quotes,
        uint64_t now_ns = 0
    ) const {
        auto scores = score_quotes(order, quotes, now_ns);

        std::vector<VenueQuote> ranked;
        ranked.reserve(scores.size());

        for (const auto& score : scores) {
            auto it = std::find_if(
                quotes.begin(),
                quotes.end(),
                [&](const VenueQuote& q) {
                    return q.venue == score.venue && q.symbol == score.symbol;
                }
            );

            if (it != quotes.end()) {
                ranked.push_back(*it);
            }
        }

        return ranked;
    }

    const VenueExecutionStats* stats_for(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = stats_.find(key(venue, symbol));
        if (it == stats_.end()) return nullptr;
        return &it->second;
    }

private:
    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    static double clamp01(double value) noexcept {
        if (value < 0.0) return 0.0;
        if (value > 1.0) return 1.0;
        return value;
    }

    static double quote_price(const RouterOrder& order, const VenueQuote& q) noexcept {
        return order.side == Side::Buy ? q.ask : q.bid;
    }

    static double quote_qty(const RouterOrder& order, const VenueQuote& q) noexcept {
        return order.side == Side::Buy ? q.ask_qty : q.bid_qty;
    }

    void normalize_weights() {
        const double total =
            config_.price_weight +
            config_.fee_weight +
            config_.latency_weight +
            config_.fill_weight +
            config_.reject_weight +
            config_.liquidity_weight +
            config_.spread_weight;

        if (total <= 0.0) return;

        config_.price_weight /= total;
        config_.fee_weight /= total;
        config_.latency_weight /= total;
        config_.fill_weight /= total;
        config_.reject_weight /= total;
        config_.liquidity_weight /= total;
        config_.spread_weight /= total;
    }

    void touch(VenueExecutionStats& stats, uint64_t now_ns) {
        if (now_ns > 0) {
            stats.last_update_ns.store(now_ns, std::memory_order_relaxed);
        }
    }

    void update_latency_ewma(VenueExecutionStats& stats, uint64_t latency_us) {
        const auto scaled = latency_us * 1000ULL;
        auto current = stats.latency_ewma_us_x1000.load(std::memory_order_relaxed);

        if (current == 0) {
            stats.latency_ewma_us_x1000.store(scaled, std::memory_order_relaxed);
            return;
        }

        const double updated =
            (config_.ewma_alpha * static_cast<double>(scaled)) +
            ((1.0 - config_.ewma_alpha) * static_cast<double>(current));

        stats.latency_ewma_us_x1000.store(
            static_cast<uint64_t>(updated),
            std::memory_order_relaxed
        );
    }

    bool is_stats_stale(
        const std::string& venue,
        const std::string& symbol,
        uint64_t now_ns
    ) const {
        if (now_ns == 0) return false;

        auto it = stats_.find(key(venue, symbol));
        if (it == stats_.end()) return false;

        const auto last = it->second.last_update_ns.load(std::memory_order_relaxed);
        if (last == 0) return false;
        if (now_ns < last) return false;

        return now_ns - last > config_.stale_after_ns;
    }

    double effective_price(const RouterOrder& order, const VenueQuote& q) const {
        const double raw = quote_price(order, q);
        const double fee_mult = q.fee_bps / 10000.0;

        if (order.side == Side::Buy) {
            return raw * (1.0 + fee_mult);
        }

        return raw * (1.0 - fee_mult);
    }

    double best_effective_price(
        const RouterOrder& order,
        const std::vector<VenueQuote>& quotes
    ) const {
        double best = order.side == Side::Buy
            ? std::numeric_limits<double>::max()
            : 0.0;

        for (const auto& q : quotes) {
            const auto px = effective_price(order, q);

            if (order.side == Side::Buy) {
                best = std::min(best, px);
            } else {
                best = std::max(best, px);
            }
        }

        return best;
    }

    double best_available_qty(
        const RouterOrder& order,
        const std::vector<VenueQuote>& quotes
    ) const {
        double best = 0.0;

        for (const auto& q : quotes) {
            best = std::max(best, quote_qty(order, q));
        }

        return best;
    }

    double price_score(
        const RouterOrder& order,
        const VenueQuote& q,
        double best_px
    ) const {
        const double px = effective_price(order, q);
        if (px <= 0.0 || best_px <= 0.0) return 0.0;

        if (order.side == Side::Buy) {
            return clamp01(best_px / px);
        }

        return clamp01(px / best_px);
    }

    double fee_score(const VenueQuote& q) const {
        if (config_.max_fee_bps <= 0.0) return 1.0;
        return clamp01(1.0 - (q.fee_bps / config_.max_fee_bps));
    }

    double latency_score(const VenueQuote& q) const {
        double latency = q.latency_us;

        auto it = stats_.find(key(q.venue, q.symbol));
        if (it != stats_.end()) {
            const auto ewma = it->second.latency_ewma_us_x1000.load(std::memory_order_relaxed);
            if (ewma > 0) {
                latency = static_cast<double>(ewma) / 1000.0;
            }
        }

        if (config_.max_latency_us <= 0.0) return 1.0;
        return clamp01(1.0 - (latency / config_.max_latency_us));
    }

    double fill_score(const VenueQuote& q) const {
        auto it = stats_.find(key(q.venue, q.symbol));
        if (it == stats_.end()) return config_.default_fill_score;

        const auto sent = it->second.orders_sent.load(std::memory_order_relaxed);
        const auto fills = it->second.fills.load(std::memory_order_relaxed);

        if (sent == 0) return config_.default_fill_score;

        return clamp01(static_cast<double>(fills) / static_cast<double>(sent));
    }

    double reject_score(const VenueQuote& q) const {
        auto it = stats_.find(key(q.venue, q.symbol));
        if (it == stats_.end()) return config_.default_reject_score;

        const auto sent = it->second.orders_sent.load(std::memory_order_relaxed);
        const auto rejects = it->second.rejects.load(std::memory_order_relaxed);
        const auto timeouts = it->second.timeouts.load(std::memory_order_relaxed);

        if (sent == 0) return config_.default_reject_score;

        const double bad_rate =
            static_cast<double>(rejects + timeouts) / static_cast<double>(sent);

        return clamp01(1.0 - bad_rate);
    }

    double liquidity_score(
        const RouterOrder& order,
        const VenueQuote& q,
        double best_liquidity
    ) const {
        if (best_liquidity <= 0.0) return 0.0;

        const double qty = quote_qty(order, q);
        return clamp01(qty / best_liquidity);
    }

    double spread_score(const VenueQuote& q) const {
        if (q.bid <= 0.0 || q.ask <= 0.0 || q.ask < q.bid) return 0.0;

        const double mid = (q.bid + q.ask) * 0.5;
        if (mid <= 0.0) return 0.0;

        const double spread_bps = ((q.ask - q.bid) / mid) * 10000.0;

        if (config_.max_spread_bps <= 0.0) return 1.0;
        return clamp01(1.0 - (spread_bps / config_.max_spread_bps));
    }

    VenueScore score_one(
        const RouterOrder& order,
        const VenueQuote& q,
        double best_px,
        double best_liquidity
    ) const {
        VenueScore out;
        out.venue = q.venue;
        out.symbol = q.symbol;
        out.effective_price = effective_price(order, q);
        out.available_qty = quote_qty(order, q);
        out.healthy = q.healthy;

        out.price_score = price_score(order, q, best_px);
        out.fee_score = fee_score(q);
        out.latency_score = latency_score(q);
        out.fill_score = fill_score(q);
        out.reject_score = reject_score(q);
        out.liquidity_score = liquidity_score(order, q, best_liquidity);
        out.spread_score = spread_score(q);

        out.score =
            config_.price_weight * out.price_score +
            config_.fee_weight * out.fee_score +
            config_.latency_weight * out.latency_score +
            config_.fill_weight * out.fill_score +
            config_.reject_weight * out.reject_score +
            config_.liquidity_weight * out.liquidity_score +
            config_.spread_weight * out.spread_score;

        return out;
    }

private:
    VenueScoreConfig config_;
    std::unordered_map<std::string, VenueExecutionStats> stats_;
};

} // namespace hft::routing