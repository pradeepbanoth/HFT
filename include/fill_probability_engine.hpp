#pragma once

#include "queue_position_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::routing {

struct FillProbabilityConfig {
    double queue_weight{0.40};
    double venue_fill_weight{0.20};
    double reject_weight{0.12};
    double latency_weight{0.10};
    double liquidity_weight{0.10};
    double aggression_weight{0.08};

    double bayes_alpha{2.0};
    double bayes_beta{2.0};

    double max_latency_us{5000.0};
    double min_probability{0.001};
    double max_probability{0.999};
};

struct FillVenueStats {
    uint64_t orders_sent{0};
    uint64_t fills{0};
    uint64_t rejects{0};
    uint64_t cancels{0};

    double total_fill_qty{0.0};
    double total_order_qty{0.0};
    double total_fill_latency_us{0.0};

    double last_probability{0.0};
};

struct FillProbabilityInput {
    VenueQuote quote{};
    QueueInputs queue{};

    double order_qty{0.0};
    double spread_bps{0.0};
    double volatility_bps{0.0};
};

struct FillProbabilityEstimate {
    std::string venue;
    std::string symbol;

    double probability{0.0};
    double confidence{0.0};

    double queue_probability{0.0};
    double venue_fill_score{0.0};
    double reject_score{0.0};
    double latency_score{0.0};
    double liquidity_score{0.0};
    double aggression_score{0.0};
    double adverse_selection_penalty{0.0};

    double expected_fill_time_ms{0.0};
    double expected_fill_qty{0.0};
    double urgency_score{0.0};
};

class FillProbabilityEngine {
public:
    explicit FillProbabilityEngine(
        FillProbabilityConfig config = {},
        QueueModelConfig queue_config = {}
    )
        : config_(config),
          queue_model_(queue_config) {}

    void record_order_sent(
        const std::string& venue,
        const std::string& symbol,
        double qty = 0.0
    ) {
        auto& s = stats_[key(venue, symbol)];
        s.orders_sent += 1;
        s.total_order_qty += std::max(0.0, qty);
    }

    void record_fill(
        const std::string& venue,
        const std::string& symbol,
        double fill_qty = 0.0,
        double fill_latency_us = 0.0
    ) {
        auto& s = stats_[key(venue, symbol)];
        s.fills += 1;
        s.total_fill_qty += std::max(0.0, fill_qty);

        if (fill_latency_us > 0.0) {
            s.total_fill_latency_us += fill_latency_us;
        }
    }

    void record_reject(const std::string& venue, const std::string& symbol) {
        auto& s = stats_[key(venue, symbol)];
        s.rejects += 1;
    }

    void record_cancel(const std::string& venue, const std::string& symbol) {
        auto& s = stats_[key(venue, symbol)];
        s.cancels += 1;
    }

    FillProbabilityEstimate estimate(const FillProbabilityInput& input) {
        FillProbabilityEstimate out;
        out.venue = input.quote.venue;
        out.symbol = input.quote.symbol;

        const auto queue_est = queue_model_.estimate(input.queue);

        out.queue_probability = queue_est.fill_probability;
        out.venue_fill_score = smoothed_fill_score(input.quote.venue, input.quote.symbol);
        out.reject_score = smoothed_reject_score(input.quote.venue, input.quote.symbol);
        out.latency_score = latency_score(input.quote.latency_us);
        out.liquidity_score = liquidity_score(input);
        out.aggression_score = price_aggression_score(input);
        out.adverse_selection_penalty = adverse_selection_penalty(input);

        double raw =
            (config_.queue_weight * out.queue_probability) +
            (config_.venue_fill_weight * out.venue_fill_score) +
            (config_.reject_weight * out.reject_score) +
            (config_.latency_weight * out.latency_score) +
            (config_.liquidity_weight * out.liquidity_score) +
            (config_.aggression_weight * out.aggression_score);

        raw *= (1.0 - out.adverse_selection_penalty);

        out.probability = clamp(
            raw,
            config_.min_probability,
            config_.max_probability
        );

        out.confidence = confidence(input.quote.venue, input.quote.symbol);
        out.expected_fill_time_ms = queue_est.expected_fill_time_ms;
        out.expected_fill_qty = input.queue.order_qty * out.probability;
        out.urgency_score = clamp(
            queue_est.urgency_score + out.adverse_selection_penalty,
            0.0,
            1.0
        );

        stats_[key(input.quote.venue, input.quote.symbol)].last_probability =
            out.probability;

        return out;
    }

    std::vector<FillProbabilityEstimate> rank(
        const std::vector<FillProbabilityInput>& inputs
    ) {
        std::vector<FillProbabilityEstimate> estimates;
        estimates.reserve(inputs.size());

        for (const auto& input : inputs) {
            estimates.push_back(estimate(input));
        }

        std::sort(
            estimates.begin(),
            estimates.end(),
            [](const FillProbabilityEstimate& a, const FillProbabilityEstimate& b) {
                if (std::abs(a.probability - b.probability) > 1e-9) {
                    return a.probability > b.probability;
                }

                return a.expected_fill_time_ms < b.expected_fill_time_ms;
            }
        );

        return estimates;
    }

    std::optional<FillProbabilityEstimate> best(
        const std::vector<FillProbabilityInput>& inputs
    ) {
        auto ranked = rank(inputs);
        if (ranked.empty()) return std::nullopt;
        return ranked.front();
    }

    std::optional<FillVenueStats> stats_for(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = stats_.find(key(venue, symbol));
        if (it == stats_.end()) return std::nullopt;
        return it->second;
    }

private:
    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    static double clamp(double v, double lo, double hi) noexcept {
        if (v < lo) return lo;
        if (v > hi) return hi;
        return v;
    }

    double smoothed_fill_score(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = stats_.find(key(venue, symbol));

        if (it == stats_.end()) {
            return config_.bayes_alpha /
                (config_.bayes_alpha + config_.bayes_beta);
        }

        const auto& s = it->second;

        return clamp(
            (static_cast<double>(s.fills) + config_.bayes_alpha) /
                (static_cast<double>(s.orders_sent) +
                 config_.bayes_alpha +
                 config_.bayes_beta),
            0.0,
            1.0
        );
    }

    double smoothed_reject_score(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = stats_.find(key(venue, symbol));

        if (it == stats_.end()) return 1.0;

        const auto& s = it->second;

        const double reject_rate =
            (static_cast<double>(s.rejects) + 1.0) /
            (static_cast<double>(s.orders_sent) + 2.0);

        return clamp(1.0 - reject_rate, 0.0, 1.0);
    }

    double confidence(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = stats_.find(key(venue, symbol));
        if (it == stats_.end()) return 0.10;

        const auto n = static_cast<double>(it->second.orders_sent);
        return clamp(std::log1p(n) / std::log(101.0), 0.10, 1.0);
    }

    double latency_score(double latency_us) const noexcept {
        if (latency_us <= 0.0) return 1.0;
        return clamp(std::exp(-latency_us / config_.max_latency_us), 0.0, 1.0);
    }

    double liquidity_score(const FillProbabilityInput& input) const noexcept {
        const auto& q = input.quote;

        const double available =
            input.queue.side == Side::Buy ? q.ask_qty : q.bid_qty;

        const double needed =
            input.order_qty > 0.0 ? input.order_qty : input.queue.order_qty;

        if (needed <= 0.0) return 0.0;

        return clamp(available / needed, 0.0, 1.0);
    }

    double price_aggression_score(const FillProbabilityInput& input) const noexcept {
        const auto& q = input.quote;
        const auto& qi = input.queue;

        if (qi.side == Side::Buy) {
            if (qi.order_price >= q.ask && q.ask > 0.0) return 1.0;
            if (qi.order_price >= q.bid && q.bid > 0.0) return 0.65;
            return 0.25;
        }

        if (qi.order_price <= q.bid && q.bid > 0.0) return 1.0;
        if (qi.order_price <= q.ask && q.ask > 0.0) return 0.65;
        return 0.25;
    }

    double adverse_selection_penalty(const FillProbabilityInput& input) const noexcept {
        const double spread_component =
            clamp(input.spread_bps / 100.0, 0.0, 0.30);

        const double volatility_component =
            clamp(input.volatility_bps / 200.0, 0.0, 0.40);

        const double latency_component =
            clamp(input.quote.latency_us / 20'000.0, 0.0, 0.30);

        return clamp(
            spread_component + volatility_component + latency_component,
            0.0,
            0.75
        );
    }

private:
    FillProbabilityConfig config_;
    QueuePositionModel queue_model_;
    std::unordered_map<std::string, FillVenueStats> stats_;
};

} // namespace hft::routing