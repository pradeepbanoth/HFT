#pragma once

#include "smart_order_router.hpp"
#include "venue_scoring_engine.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::routing {

struct AdaptiveRouterConfig {
    RouterConfig router_config{};
    VenueScoreConfig score_config{};

    double min_score_to_route{0.20};
    double max_route_slippage_bps{25.0};

    bool allow_partial_if_insufficient{false};
    bool reject_stale_quotes{true};

    uint64_t quote_stale_after_ns{5'000'000'000ULL};
};

struct RouteExplanation {
    std::string venue;
    double score{0.0};
    double price_score{0.0};
    double fee_score{0.0};
    double latency_score{0.0};
    double fill_score{0.0};
    double reject_score{0.0};
    double routed_qty{0.0};
    double route_price{0.0};
    bool selected{false};
    std::string reason;
};

struct AdaptiveRouteResult {
    RouteDecision decision{RouteDecision::Reject};
    std::string reason;

    std::vector<ChildRoute> children;
    std::vector<RouteExplanation> explanations;

    double expected_avg_price{0.0};
    double expected_total_cost{0.0};
    double requested_qty{0.0};
    double routed_qty{0.0};
    double remaining_qty{0.0};
    bool partial{false};
};

struct AdaptiveRouterMetrics {
    std::atomic<uint64_t> route_requests{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> single_venue{0};
    std::atomic<uint64_t> split_venues{0};
    std::atomic<uint64_t> partial_routes{0};
    std::atomic<uint64_t> stale_quotes_filtered{0};
    std::atomic<uint64_t> score_filtered{0};
    std::atomic<uint64_t> slippage_rejects{0};
};

class AdaptiveSmartOrderRouter {
public:
    explicit AdaptiveSmartOrderRouter(AdaptiveRouterConfig config = {})
        : config_(config),
          scorer_(config.score_config) {}

    void update_quote(VenueQuote quote, uint64_t timestamp_ns = 0) {
        const auto k = key(quote.venue, quote.symbol);
        quotes_[k] = std::move(quote);
        quote_ts_[k] = timestamp_ns;
    }

    void record_order_sent(const std::string& venue, const std::string& symbol) {
        scorer_.record_order_sent(venue, symbol);
    }

    void record_fill(
        const std::string& venue,
        const std::string& symbol,
        uint64_t fill_latency_us = 0
    ) {
        scorer_.record_fill(venue, symbol, fill_latency_us);
    }

    void record_reject(const std::string& venue, const std::string& symbol) {
        scorer_.record_reject(venue, symbol);
    }

    void record_cancel(const std::string& venue, const std::string& symbol) {
        scorer_.record_cancel(venue, symbol);
    }

    RouteResult route(const RouterOrder& order) {
        auto advanced = route_advanced(order, 0);

        RouteResult legacy;
        legacy.decision = advanced.decision;
        legacy.reason = advanced.reason;
        legacy.children = advanced.children;
        legacy.expected_avg_price = advanced.expected_avg_price;
        legacy.expected_total_cost = advanced.expected_total_cost;
        return legacy;
    }

    AdaptiveRouteResult route_advanced(
        const RouterOrder& order,
        uint64_t now_ns = 0
    ) {
        metrics_.route_requests.fetch_add(1, std::memory_order_relaxed);

        AdaptiveRouteResult result;
        result.requested_qty = order.qty;

        if (order.symbol.empty() || order.qty <= 0.0) {
            return reject(result, "Invalid order");
        }

        auto quotes = collect_quotes(order.symbol, now_ns);
        if (quotes.empty()) {
            return reject(result, "No usable quotes");
        }

        auto scores = scorer_.score_quotes(order, quotes);
        if (scores.empty()) {
            return reject(result, "No scored venue available");
        }

        std::vector<VenueScore> filtered_scores;
        filtered_scores.reserve(scores.size());

        for (const auto& score : scores) {
            RouteExplanation explanation;
            explanation.venue = score.venue;
            explanation.score = score.score;
            explanation.price_score = score.price_score;
            explanation.fee_score = score.fee_score;
            explanation.latency_score = score.latency_score;
            explanation.fill_score = score.fill_score;
            explanation.reject_score = score.reject_score;

            if (score.score < config_.min_score_to_route) {
                explanation.selected = false;
                explanation.reason = "Filtered by min venue score";
                result.explanations.push_back(std::move(explanation));
                metrics_.score_filtered.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            explanation.selected = true;
            explanation.reason = "Eligible";
            result.explanations.push_back(std::move(explanation));
            filtered_scores.push_back(score);
        }

        if (filtered_scores.empty()) {
            return reject(result, "All venues filtered by score");
        }

        auto ranked_quotes = quotes_from_scores(filtered_scores);
        return route_ranked(order, ranked_quotes, result);
    }

    const VenueScoringEngine& scorer() const noexcept {
        return scorer_;
    }

    const AdaptiveRouterMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    AdaptiveRouteResult reject(AdaptiveRouteResult result, std::string reason) {
        metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
        result.decision = RouteDecision::Reject;
        result.reason = std::move(reason);
        return result;
    }

    bool quote_is_stale(const std::string& k, uint64_t now_ns) const {
        if (!config_.reject_stale_quotes) return false;
        if (now_ns == 0) return false;

        auto it = quote_ts_.find(k);
        if (it == quote_ts_.end()) return false;
        if (it->second == 0) return false;
        if (now_ns < it->second) return false;

        return now_ns - it->second > config_.quote_stale_after_ns;
    }

    std::vector<VenueQuote> collect_quotes(
        const std::string& symbol,
        uint64_t now_ns
    ) {
        std::vector<VenueQuote> out;

        for (const auto& [k, q] : quotes_) {
            if (q.symbol != symbol) continue;
            if (!q.healthy) continue;

            if (quote_is_stale(k, now_ns)) {
                metrics_.stale_quotes_filtered.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            out.push_back(q);
        }

        return out;
    }

    std::vector<VenueQuote> quotes_from_scores(
        const std::vector<VenueScore>& scores
    ) const {
        std::vector<VenueQuote> ranked;
        ranked.reserve(scores.size());

        for (const auto& score : scores) {
            auto it = quotes_.find(key(score.venue, score.symbol));
            if (it != quotes_.end()) {
                ranked.push_back(it->second);
            }
        }

        return ranked;
    }

    double available_qty(const RouterOrder& order, const VenueQuote& q) const {
        return order.side == Side::Buy ? q.ask_qty : q.bid_qty;
    }

    double raw_price(const RouterOrder& order, const VenueQuote& q) const {
        return order.side == Side::Buy ? q.ask : q.bid;
    }

    bool price_allowed(const RouterOrder& order, double px) const {
        if (order.type != OrderType::Limit) return true;

        if (order.side == Side::Buy) {
            return px <= order.limit_price;
        }

        return px >= order.limit_price;
    }

    double effective_price(const RouterOrder& order, const VenueQuote& q) const {
        const double px = raw_price(order, q);
        const double fee_mult = q.fee_bps / 10000.0;
        const double latency_penalty = q.latency_us * 0.00000001;

        if (order.side == Side::Buy) {
            return px * (1.0 + fee_mult + latency_penalty);
        }

        return px * (1.0 - fee_mult - latency_penalty);
    }

    bool violates_slippage(
        const RouterOrder& order,
        double avg_price
    ) const {
        if (order.type != OrderType::Limit || order.limit_price <= 0.0) {
            return false;
        }

        const double diff =
            order.side == Side::Buy
                ? avg_price - order.limit_price
                : order.limit_price - avg_price;

        const double bps = diff / order.limit_price * 10000.0;
        return bps > config_.max_route_slippage_bps;
    }

    void mark_selected_explanation(
        AdaptiveRouteResult& result,
        const std::string& venue,
        double qty,
        double px
    ) {
        for (auto& explanation : result.explanations) {
            if (explanation.venue == venue && explanation.selected) {
                explanation.routed_qty += qty;
                explanation.route_price = px;
                explanation.reason = "Selected for route";
                return;
            }
        }
    }

    AdaptiveRouteResult route_ranked(
        const RouterOrder& order,
        const std::vector<VenueQuote>& ranked_quotes,
        AdaptiveRouteResult result
    ) {
        result.reason = "Adaptive scored route";

        double remaining = order.qty;
        double total_qty = 0.0;
        double total_cost = 0.0;

        for (const auto& q : ranked_quotes) {
            if (remaining <= config_.router_config.min_child_qty) break;

            const double px = raw_price(order, q);
            const double available = available_qty(order, q);

            if (available <= 0.0 || px <= 0.0) continue;

            if (!price_allowed(order, px)) {
                continue;
            }

            const double child_qty = std::min(remaining, available);

            if (child_qty < config_.router_config.min_child_qty) {
                continue;
            }

            const double cost = child_qty * effective_price(order, q);

            result.children.push_back({
                q.venue,
                child_qty,
                px,
                cost
            });

            mark_selected_explanation(result, q.venue, child_qty, px);

            remaining -= child_qty;
            total_qty += child_qty;
            total_cost += cost;
        }

        result.routed_qty = total_qty;
        result.remaining_qty = std::max(0.0, order.qty - total_qty);
        result.expected_total_cost = total_cost;
        result.expected_avg_price = total_qty > 0.0 ? total_cost / total_qty : 0.0;

        if (result.children.empty()) {
            return reject(result, "No executable scored venue");
        }

        if (violates_slippage(order, result.expected_avg_price)) {
            metrics_.slippage_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(result, "Route slippage limit exceeded");
        }

        if (result.remaining_qty > config_.router_config.min_child_qty) {
            if (!config_.allow_partial_if_insufficient) {
                return reject(result, "Insufficient scored liquidity");
            }

            result.partial = true;
            metrics_.partial_routes.fetch_add(1, std::memory_order_relaxed);
        }

        result.decision =
            result.children.size() == 1
                ? RouteDecision::SingleVenue
                : RouteDecision::SplitVenues;

        if (result.decision == RouteDecision::SingleVenue) {
            metrics_.single_venue.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics_.split_venues.fetch_add(1, std::memory_order_relaxed);
        }

        return result;
    }

private:
    AdaptiveRouterConfig config_;
    VenueScoringEngine scorer_;

    std::unordered_map<std::string, VenueQuote> quotes_;
    std::unordered_map<std::string, uint64_t> quote_ts_;

    AdaptiveRouterMetrics metrics_{};
};

} // namespace hft::routing