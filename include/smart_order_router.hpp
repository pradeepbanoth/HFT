#pragma once

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

enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Market, Limit };
enum class RouteDecision : uint8_t { Reject, SingleVenue, SplitVenues };

struct RouterOrder {
    std::string symbol;
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};
    double qty{0.0};
    double limit_price{0.0};
    uint64_t client_ts_ns{0};
};

struct VenueQuote {
    std::string venue;
    std::string symbol;
    double bid{0.0};
    double ask{0.0};
    double bid_qty{0.0};
    double ask_qty{0.0};
    double fee_bps{0.0};
    double latency_us{0.0};
    bool healthy{true};
};

struct ChildRoute {
    std::string venue;
    double qty{0.0};
    double price{0.0};
    double expected_cost{0.0};
};

struct RouteResult {
    RouteDecision decision{RouteDecision::Reject};
    std::string reason;
    std::vector<ChildRoute> children;
    double expected_avg_price{0.0};
    double expected_total_cost{0.0};
};

struct RouterConfig {
    double max_slippage_bps{25.0};
    double min_child_qty{0.000001};
    bool allow_split{true};
};

struct RouterMetrics {
    std::atomic<uint64_t> route_requests{0};
    std::atomic<uint64_t> rejected{0};
    std::atomic<uint64_t> single_venue{0};
    std::atomic<uint64_t> split_venues{0};
};

class SmartOrderRouter {
public:
    explicit SmartOrderRouter(RouterConfig config = {})
        : config_(config) {}

    void update_quote(VenueQuote quote) {
        quotes_[key(quote.venue, quote.symbol)] = std::move(quote);
    }

    RouteResult route(const RouterOrder& order) {
        metrics_.route_requests.fetch_add(1, std::memory_order_relaxed);

        if (order.symbol.empty() || order.qty <= 0.0) {
            return reject("Invalid order");
        }

        auto candidates = collect_candidates(order);

        if (candidates.empty()) {
            return reject("No healthy venue liquidity");
        }

        std::sort(
            candidates.begin(),
            candidates.end(),
            [&](const VenueQuote& a, const VenueQuote& b) {
                return effective_price(order, a) < effective_price(order, b);
            }
        );

        if (!config_.allow_split) {
            return route_single(order, candidates.front());
        }

        return route_split(order, candidates);
    }

    const RouterMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    RouteResult reject(std::string reason) {
        metrics_.rejected.fetch_add(1, std::memory_order_relaxed);

        RouteResult result;
        result.decision = RouteDecision::Reject;
        result.reason = std::move(reason);
        return result;
    }

    std::vector<VenueQuote> collect_candidates(const RouterOrder& order) const {
        std::vector<VenueQuote> out;

        for (const auto& [_, q] : quotes_) {
            if (!q.healthy) continue;
            if (q.symbol != order.symbol) continue;

            const double available = order.side == Side::Buy ? q.ask_qty : q.bid_qty;
            const double px = order.side == Side::Buy ? q.ask : q.bid;

            if (available <= 0.0 || px <= 0.0) continue;

            if (order.type == OrderType::Limit) {
                if (order.side == Side::Buy && px > order.limit_price) continue;
                if (order.side == Side::Sell && px < order.limit_price) continue;
            }

            out.push_back(q);
        }

        return out;
    }

    double effective_price(const RouterOrder& order, const VenueQuote& q) const {
        const double raw = order.side == Side::Buy ? q.ask : q.bid;
        const double fee_mult = q.fee_bps / 10000.0;
        const double latency_penalty = q.latency_us * 0.00000001;

        if (order.side == Side::Buy) {
            return raw * (1.0 + fee_mult + latency_penalty);
        }

        return raw * (1.0 - fee_mult - latency_penalty);
    }

    double available_qty(const RouterOrder& order, const VenueQuote& q) const {
        return order.side == Side::Buy ? q.ask_qty : q.bid_qty;
    }

    bool violates_slippage(const RouterOrder& order, double avg_price) const {
        if (order.type != OrderType::Limit || order.limit_price <= 0.0) {
            return false;
        }

        const double diff =
            order.side == Side::Buy
                ? avg_price - order.limit_price
                : order.limit_price - avg_price;

        const double bps = diff / order.limit_price * 10000.0;
        return bps > config_.max_slippage_bps;
    }

    RouteResult route_single(const RouterOrder& order, const VenueQuote& q) {
        const double qty = std::min(order.qty, available_qty(order, q));

        if (qty < order.qty) {
            return reject("Insufficient liquidity on best venue");
        }

        const double px = order.side == Side::Buy ? q.ask : q.bid;
        const double cost = qty * effective_price(order, q);

        RouteResult result;
        result.decision = RouteDecision::SingleVenue;
        result.reason = "Single venue route";
        result.children.push_back({q.venue, qty, px, cost});
        result.expected_total_cost = cost;
        result.expected_avg_price = cost / qty;

        metrics_.single_venue.fetch_add(1, std::memory_order_relaxed);
        return result;
    }

    RouteResult route_split(const RouterOrder& order, const std::vector<VenueQuote>& candidates) {
        RouteResult result;
        result.reason = "Split venue route";

        double remaining = order.qty;
        double total_cost = 0.0;
        double total_qty = 0.0;

        for (const auto& q : candidates) {
            if (remaining <= 0.0) break;

            const double child_qty = std::min(remaining, available_qty(order, q));

            if (child_qty < config_.min_child_qty) continue;

            const double px = order.side == Side::Buy ? q.ask : q.bid;
            const double cost = child_qty * effective_price(order, q);

            result.children.push_back({q.venue, child_qty, px, cost});

            remaining -= child_qty;
            total_qty += child_qty;
            total_cost += cost;
        }

        if (remaining > config_.min_child_qty) {
            return reject("Insufficient aggregate liquidity");
        }

        result.expected_total_cost = total_cost;
        result.expected_avg_price = total_qty > 0.0 ? total_cost / total_qty : 0.0;

        if (violates_slippage(order, result.expected_avg_price)) {
            return reject("Slippage limit exceeded");
        }

        result.decision =
            result.children.size() == 1
                ? RouteDecision::SingleVenue
                : hft::routing::RouteDecision::SplitVenues;

        if (result.decision == RouteDecision::SingleVenue) {
            metrics_.single_venue.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics_.split_venues.fetch_add(1, std::memory_order_relaxed);
        }

        return result;
    }

private:
    RouterConfig config_;
    std::unordered_map<std::string, VenueQuote> quotes_;
    RouterMetrics metrics_{};
};

} // namespace hft::routing