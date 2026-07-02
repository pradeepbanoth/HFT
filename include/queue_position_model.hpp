#pragma once

#include "smart_order_router.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::routing {

enum class LiquidityEventType : uint8_t {
    Trade,
    Cancel,
    Add,
    Replace
};

struct QueueModelConfig {
    double latency_decay_us{5000.0};
    double trade_intensity_decay{0.08};
    double cancel_intensity_decay{0.05};
    double adverse_selection_weight{0.25};
    double queue_depletion_weight{0.35};
    double aggression_weight{0.25};
    double latency_weight{0.15};
    double min_fill_probability{0.001};
    double max_fill_probability{0.999};
};

struct LiquidityEvent {
    std::string venue;
    std::string symbol;
    LiquidityEventType type{LiquidityEventType::Trade};
    Side side{Side::Buy};
    double qty{0.0};
    double price{0.0};
    uint64_t timestamp_ns{0};
};

struct QueueInputs {
    std::string venue;
    std::string symbol;

    Side side{Side::Buy};

    double order_qty{0.0};
    double order_price{0.0};

    double best_bid{0.0};
    double best_ask{0.0};
    double bid_qty_ahead{0.0};
    double ask_qty_ahead{0.0};

    double recent_trade_qty{0.0};
    double recent_trade_rate_per_sec{0.0};

    double latency_us{0.0};
};

struct QueueEstimate {
    std::string venue;
    std::string symbol;

    double queue_ahead_qty{0.0};
    double queue_position_qty{0.0};

    double queue_depletion_score{0.0};
    double aggression_score{0.0};
    double latency_score{0.0};
    double adverse_selection_risk{0.0};

    double fill_probability{0.0};
    double expected_fill_time_ms{0.0};
    double urgency_score{0.0};
    double maker_quality_score{0.0};
};

struct VenueMicrostructureStats {
    std::atomic<uint64_t> trades{0};
    std::atomic<uint64_t> cancels{0};
    std::atomic<uint64_t> adds{0};
    std::atomic<uint64_t> replaces{0};

    std::atomic<uint64_t> trade_qty_scaled{0};
    std::atomic<uint64_t> cancel_qty_scaled{0};
    std::atomic<uint64_t> add_qty_scaled{0};

    std::atomic<uint64_t> aggressive_buy_trades{0};
    std::atomic<uint64_t> aggressive_sell_trades{0};
};

struct QueueModelMetricsSnapshot {
    uint64_t venues_tracked{0};
    uint64_t total_trades{0};
    uint64_t total_cancels{0};
    uint64_t total_adds{0};
    uint64_t total_replaces{0};
};

class QueuePositionModel {
public:
    explicit QueuePositionModel(QueueModelConfig config = {})
        : config_(config) {}

    void record_event(const LiquidityEvent& event) {
        if (event.venue.empty() || event.symbol.empty() || event.qty <= 0.0) return;

        auto& s = stats_[key(event.venue, event.symbol)];
        const auto qty_scaled = scale_qty(event.qty);

        switch (event.type) {
            case LiquidityEventType::Trade:
                s.trades.fetch_add(1, std::memory_order_relaxed);
                s.trade_qty_scaled.fetch_add(qty_scaled, std::memory_order_relaxed);

                if (event.side == Side::Buy) {
                    s.aggressive_buy_trades.fetch_add(1, std::memory_order_relaxed);
                } else {
                    s.aggressive_sell_trades.fetch_add(1, std::memory_order_relaxed);
                }
                break;

            case LiquidityEventType::Cancel:
                s.cancels.fetch_add(1, std::memory_order_relaxed);
                s.cancel_qty_scaled.fetch_add(qty_scaled, std::memory_order_relaxed);
                break;

            case LiquidityEventType::Add:
                s.adds.fetch_add(1, std::memory_order_relaxed);
                s.add_qty_scaled.fetch_add(qty_scaled, std::memory_order_relaxed);
                break;

            case LiquidityEventType::Replace:
                s.replaces.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    QueueEstimate estimate(const QueueInputs& in) const {
        QueueEstimate out;
        out.venue = in.venue;
        out.symbol = in.symbol;

        if (in.order_qty <= 0.0 || in.order_price <= 0.0) return out;

        out.queue_ahead_qty = queue_ahead(in);
        out.queue_position_qty = out.queue_ahead_qty + in.order_qty;

        out.aggression_score = calc_aggression_score(in);
        out.latency_score = calc_latency_score(in.latency_us);
        out.queue_depletion_score = calc_queue_depletion_score(in, out.queue_position_qty);
        out.adverse_selection_risk = calc_adverse_selection_risk(in);

        const double raw =
            (config_.queue_depletion_weight * out.queue_depletion_score) +
            (config_.aggression_weight * out.aggression_score) +
            (config_.latency_weight * out.latency_score) -
            (config_.adverse_selection_weight * out.adverse_selection_risk);

        out.fill_probability = clamp(
            raw,
            config_.min_fill_probability,
            config_.max_fill_probability
        );

        out.expected_fill_time_ms = calc_expected_fill_time_ms(
            out.queue_position_qty,
            effective_trade_rate(in),
            out.fill_probability
        );

        out.urgency_score = clamp(
            (1.0 - out.fill_probability) +
            out.adverse_selection_risk +
            (1.0 - out.latency_score),
            0.0,
            1.0
        );

        out.maker_quality_score = clamp(
            out.fill_probability *
            (1.0 - out.adverse_selection_risk) *
            out.latency_score,
            0.0,
            1.0
        );

        return out;
    }

    std::vector<QueueEstimate> rank_by_fill_probability(
        const std::vector<QueueInputs>& inputs
    ) const {
        auto estimates = estimate_all(inputs);

        std::sort(
            estimates.begin(),
            estimates.end(),
            [](const QueueEstimate& a, const QueueEstimate& b) {
                return a.fill_probability > b.fill_probability;
            }
        );

        return estimates;
    }

    std::vector<QueueEstimate> rank_by_maker_quality(
        const std::vector<QueueInputs>& inputs
    ) const {
        auto estimates = estimate_all(inputs);

        std::sort(
            estimates.begin(),
            estimates.end(),
            [](const QueueEstimate& a, const QueueEstimate& b) {
                return a.maker_quality_score > b.maker_quality_score;
            }
        );

        return estimates;
    }

    std::optional<QueueEstimate> best_fill_candidate(
        const std::vector<QueueInputs>& inputs
    ) const {
        auto ranked = rank_by_fill_probability(inputs);
        if (ranked.empty()) return std::nullopt;
        return ranked.front();
    }

    const VenueMicrostructureStats* venue_stats(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = stats_.find(key(venue, symbol));
        if (it == stats_.end()) return nullptr;
        return &it->second;
    }

    QueueModelMetricsSnapshot metrics_snapshot() const {
        QueueModelMetricsSnapshot snap;
        snap.venues_tracked = stats_.size();

        for (const auto& [_, s] : stats_) {
            snap.total_trades += s.trades.load(std::memory_order_relaxed);
            snap.total_cancels += s.cancels.load(std::memory_order_relaxed);
            snap.total_adds += s.adds.load(std::memory_order_relaxed);
            snap.total_replaces += s.replaces.load(std::memory_order_relaxed);
        }

        return snap;
    }

private:
    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    static uint64_t scale_qty(double qty) noexcept {
        return static_cast<uint64_t>(std::max(0.0, qty) * 1'000'000.0);
    }

    static double unscale_qty(uint64_t qty) noexcept {
        return static_cast<double>(qty) / 1'000'000.0;
    }

    static double clamp(double v, double lo, double hi) noexcept {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    std::vector<QueueEstimate> estimate_all(const std::vector<QueueInputs>& inputs) const {
        std::vector<QueueEstimate> estimates;
        estimates.reserve(inputs.size());

        for (const auto& input : inputs) {
            estimates.push_back(estimate(input));
        }

        return estimates;
    }

    double queue_ahead(const QueueInputs& in) const noexcept {
        if (in.side == Side::Buy) {
            if (in.best_ask > 0.0 && in.order_price >= in.best_ask) return 0.0;
            if (in.best_bid > 0.0 && in.order_price >= in.best_bid) return in.bid_qty_ahead;
            return in.bid_qty_ahead + in.order_qty;
        }

        if (in.best_bid > 0.0 && in.order_price <= in.best_bid) return 0.0;
        if (in.best_ask > 0.0 && in.order_price <= in.best_ask) return in.ask_qty_ahead;
        return in.ask_qty_ahead + in.order_qty;
    }

    double calc_aggression_score(const QueueInputs& in) const noexcept {
        if (in.side == Side::Buy) {
            if (in.best_ask > 0.0 && in.order_price >= in.best_ask) return 1.0;
            if (in.best_bid > 0.0 && in.order_price >= in.best_bid) return 0.65;
            return 0.25;
        }

        if (in.best_bid > 0.0 && in.order_price <= in.best_bid) return 1.0;
        if (in.best_ask > 0.0 && in.order_price <= in.best_ask) return 0.65;
        return 0.25;
    }

    double calc_latency_score(double latency_us) const noexcept {
        if (latency_us <= 0.0) return 1.0;
        return clamp(std::exp(-latency_us / config_.latency_decay_us), 0.0, 1.0);
    }

    double effective_trade_rate(const QueueInputs& in) const {
        double rate = std::max(0.0, in.recent_trade_rate_per_sec);

        auto it = stats_.find(key(in.venue, in.symbol));
        if (it == stats_.end()) return std::max(rate, 0.01);

        const double trades = static_cast<double>(
            it->second.trades.load(std::memory_order_relaxed)
        );

        const double stat_rate_proxy = std::log1p(trades);

        return std::max(rate, stat_rate_proxy);
    }

    double calc_queue_depletion_score(
        const QueueInputs& in,
        double queue_position_qty
    ) const {
        const double recent_qty = std::max(0.0, in.recent_trade_qty);

        auto it = stats_.find(key(in.venue, in.symbol));

        double traded_qty = recent_qty;
        double cancelled_qty = 0.0;
        double added_qty = 0.0;

        if (it != stats_.end()) {
            traded_qty += unscale_qty(
                it->second.trade_qty_scaled.load(std::memory_order_relaxed)
            );

            cancelled_qty += unscale_qty(
                it->second.cancel_qty_scaled.load(std::memory_order_relaxed)
            );

            added_qty += unscale_qty(
                it->second.add_qty_scaled.load(std::memory_order_relaxed)
            );
        }

        const double depletion =
            traded_qty +
            (config_.cancel_intensity_decay * cancelled_qty);

        const double replenishment =
            config_.trade_intensity_decay * added_qty;

        const double effective_depletion = std::max(0.0, depletion - replenishment);

        if (queue_position_qty <= 0.0) return 1.0;

        return clamp(
            effective_depletion / (effective_depletion + queue_position_qty),
            0.0,
            1.0
        );
    }

    double calc_adverse_selection_risk(const QueueInputs& in) const {
        auto it = stats_.find(key(in.venue, in.symbol));

        double buy_pressure = 0.0;
        double sell_pressure = 0.0;

        if (it != stats_.end()) {
            buy_pressure = static_cast<double>(
                it->second.aggressive_buy_trades.load(std::memory_order_relaxed)
            );

            sell_pressure = static_cast<double>(
                it->second.aggressive_sell_trades.load(std::memory_order_relaxed)
            );
        }

        const double total = buy_pressure + sell_pressure;

        if (total <= 0.0) {
            return 0.10;
        }

        const double imbalance = (buy_pressure - sell_pressure) / total;

        if (in.side == Side::Buy) {
            return clamp(std::max(0.0, imbalance), 0.0, 1.0);
        }

        return clamp(std::max(0.0, -imbalance), 0.0, 1.0);
    }

    double calc_expected_fill_time_ms(
        double queue_position_qty,
        double trade_rate_per_sec,
        double fill_probability
    ) const noexcept {
        if (trade_rate_per_sec <= 0.0 || fill_probability <= 0.0) {
            return 1'000'000.0;
        }

        const double seconds =
            queue_position_qty /
            std::max(1e-9, trade_rate_per_sec * fill_probability);

        return seconds * 1000.0;
    }

private:
    QueueModelConfig config_;
    std::unordered_map<std::string, VenueMicrostructureStats> stats_;
};

} // namespace hft::routing