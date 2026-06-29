#pragma once
// smart_router.hpp — advanced multi-venue smart order router

#include "types.hpp"
#include "orderbook.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class VenueStatus : uint8_t { Online, Degraded, Offline };
enum class RoutingStyle : uint8_t { BestVenue, SplitByLiquidity, CostOptimized };
enum class LiquidityPreference : uint8_t { MakerOnly, TakerOnly, MakerThenTaker };

struct VenueConfig {
    std::string venue;

    double maker_fee = -0.0001;
    double taker_fee = 0.0005;

    double max_order_qty = 1e18;
    double min_order_qty = 0.0;

    double latency_penalty_bps = 0.0;
    double reject_penalty_mult = 20.0;
    double fill_penalty_mult = 10.0;

    bool allow_maker = true;
    bool allow_taker = true;
};

struct VenueState {
    VenueStatus status = VenueStatus::Online;

    double recent_reject_rate = 0.0;
    double recent_fill_rate = 1.0;
    double avg_ack_latency_us = 0.0;
    double queue_health = 1.0;

    int64_t last_update_ts = 0;
};

struct RouteRequest {
    std::string symbol;
    Side side = Side::Buy;

    double qty = 0.0;
    double limit_price = 0.0;

    RoutingStyle style = RoutingStyle::CostOptimized;
    LiquidityPreference liquidity = LiquidityPreference::MakerThenTaker;

    bool allow_partial = true;
    double max_child_qty = 1e18;
    double min_child_qty = 0.0;

    double max_slippage_bps = 20.0;
    double max_venue_cost_bps = 100.0;
};

struct RouteLeg {
    std::string venue;
    std::string symbol;

    Side side = Side::Buy;
    OrderType order_type = OrderType::Limit;

    double price = 0.0;
    double qty = 0.0;

    bool post_only = true;

    double expected_fee_bps = 0.0;
    double expected_cost_bps = 0.0;
    double venue_score = 0.0;
};

struct RouteDecision {
    bool ok = false;
    std::string reason;

    std::vector<RouteLeg> legs;

    double requested_qty = 0.0;
    double routed_qty = 0.0;
    double unfilled_qty = 0.0;

    double expected_avg_price = 0.0;
    double expected_cost_bps = 0.0;
};

class SmartOrderRouter {
public:
    void add_venue(VenueConfig cfg) {
        states_[cfg.venue] = VenueState{};
        venues_[cfg.venue] = std::move(cfg);
    }

    void remove_venue(const std::string& venue) {
        venues_.erase(venue);
        states_.erase(venue);

        std::vector<std::string> erase_keys;
        for (const auto& [k, _] : books_) {
            if (k.rfind(venue + "|", 0) == 0)
                erase_keys.push_back(k);
        }

        for (const auto& k : erase_keys)
            books_.erase(k);
    }

    void update_state(const std::string& venue, VenueState state) {
        states_[venue] = state;
    }

    void update_book(const std::string& venue, const std::string& symbol, OrderBook* book) {
        books_[key(venue, symbol)] = book;
    }

    RouteDecision route(const RouteRequest& req) const {
        if (req.symbol.empty() || req.qty <= 0.0 || !std::isfinite(req.qty))
            return fail(req, "invalid_request");

        auto candidates = collect_candidates(req);

        if (candidates.empty())
            return fail(req, "no_route_candidates");

        std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
                if (std::abs(a.score_bps - b.score_bps) > 1e-12)
                    return a.score_bps < b.score_bps;
                return a.available_qty > b.available_qty;
            });

        if (req.style == RoutingStyle::BestVenue)
            return route_best(req, candidates);

        return route_split(req, candidates);
    }

private:
    struct Candidate {
        std::string venue;
        bool post_only = true;

        double price = 0.0;
        double available_qty = 0.0;
        double score_bps = 0.0;
        double fee_bps = 0.0;

        double min_order_qty = 0.0;
        double max_order_qty = 1e18;
    };

    std::unordered_map<std::string, VenueConfig> venues_;
    std::unordered_map<std::string, VenueState> states_;
    std::unordered_map<std::string, OrderBook*> books_;

    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + "|" + symbol;
    }

    static RouteDecision fail(const RouteRequest& req, std::string reason) {
        RouteDecision d;
        d.ok = false;
        d.reason = std::move(reason);
        d.requested_qty = req.qty;
        d.unfilled_qty = req.qty;
        return d;
    }

    std::vector<Candidate> collect_candidates(const RouteRequest& req) const {
        std::vector<Candidate> out;

        for (const auto& [venue, cfg] : venues_) {
            auto sit = states_.find(venue);
            if (sit == states_.end()) continue;

            const VenueState& st = sit->second;
            if (st.status == VenueStatus::Offline) continue;

            auto bit = books_.find(key(venue, req.symbol));
            if (bit == books_.end() || !bit->second) continue;

            if (req.liquidity != LiquidityPreference::TakerOnly && cfg.allow_maker) {
                auto c = make_candidate(req, cfg, st, *bit->second, true);
                if (c) out.push_back(*c);
            }

            if (req.liquidity != LiquidityPreference::MakerOnly && cfg.allow_taker) {
                auto c = make_candidate(req, cfg, st, *bit->second, false);
                if (c) out.push_back(*c);
            }
        }

        return out;
    }

    static std::optional<Candidate> make_candidate(
        const RouteRequest& req,
        const VenueConfig& cfg,
        const VenueState& st,
        const OrderBook& book,
        bool maker
    ) {
        Candidate c;
        c.venue = cfg.venue;
        c.post_only = maker;
        c.min_order_qty = cfg.min_order_qty;
        c.max_order_qty = cfg.max_order_qty;

        if (!extract_price_qty(req, book, maker, c.price, c.available_qty))
            return std::nullopt;

        if (c.available_qty <= 1e-12)
            return std::nullopt;

        if (!passes_limit_price(req, c.price))
            return std::nullopt;

        if (!passes_slippage(req, book, c.price))
            return std::nullopt;

        c.fee_bps = (maker ? cfg.maker_fee : cfg.taker_fee) * 10000.0;

        double reject_penalty = st.recent_reject_rate * cfg.reject_penalty_mult;
        double fill_penalty = (1.0 - std::clamp(st.recent_fill_rate, 0.0, 1.0)) * cfg.fill_penalty_mult;
        double degraded_penalty = st.status == VenueStatus::Degraded ? 5.0 : 0.0;
        double queue_penalty = maker ? (1.0 - std::clamp(st.queue_health, 0.0, 1.0)) * 5.0 : 0.0;

        double taker_spread_cost = 0.0;
        if (!maker) {
            auto mid = book.mid_price();
            if (mid && *mid > 1e-12)
                taker_spread_cost = std::abs(c.price - *mid) / *mid * 10000.0;
        }

        c.score_bps =
            c.fee_bps +
            taker_spread_cost +
            cfg.latency_penalty_bps +
            reject_penalty +
            fill_penalty +
            degraded_penalty +
            queue_penalty;

        if (c.score_bps > req.max_venue_cost_bps)
            return std::nullopt;

        return c;
    }

    static bool extract_price_qty(
        const RouteRequest& req,
        const OrderBook& book,
        bool maker,
        double& price,
        double& qty
    ) {
        if (req.side == Side::Buy) {
            if (maker) {
                auto bid = book.best_bid();
                if (!bid) return false;
                price = *bid;
                qty = sum_qty(book.bid_depth(5));
            } else {
                auto ask = book.best_ask();
                if (!ask) return false;
                price = *ask;
                qty = sum_qty(book.ask_depth(5));
            }
            return true;
        }

        if (req.side == Side::Sell) {
            if (maker) {
                auto ask = book.best_ask();
                if (!ask) return false;
                price = *ask;
                qty = sum_qty(book.ask_depth(5));
            } else {
                auto bid = book.best_bid();
                if (!bid) return false;
                price = *bid;
                qty = sum_qty(book.bid_depth(5));
            }
            return true;
        }

        return false;
    }

    static double sum_qty(const std::vector<DepthLevel>& depth) {
        double s = 0.0;
        for (const auto& lv : depth)
            s += lv.qty;
        return s;
    }

    static bool passes_limit_price(const RouteRequest& req, double px) {
        if (req.limit_price <= 0.0) return true;

        if (req.side == Side::Buy)
            return px <= req.limit_price;

        if (req.side == Side::Sell)
            return px >= req.limit_price;

        return false;
    }

    static bool passes_slippage(const RouteRequest& req, const OrderBook& book, double px) {
        auto mid = book.mid_price();
        if (!mid || *mid <= 1e-12) return true;

        double slip = std::abs(px - *mid) / *mid * 10000.0;
        return slip <= req.max_slippage_bps;
    }

    RouteDecision route_best(const RouteRequest& req, const std::vector<Candidate>& cands) const {
        RouteDecision d;
        d.ok = true;
        d.requested_qty = req.qty;

        const Candidate& c = cands.front();
        double q = std::min({req.qty, c.available_qty, c.max_order_qty, req.max_child_qty});

        if (q < std::max(req.min_child_qty, c.min_order_qty))
            return fail(req, "best_venue_below_min_qty");

        RouteLeg leg;
        leg.venue = c.venue;
        leg.symbol = req.symbol;
        leg.side = req.side;
        leg.price = c.price;
        leg.qty = q;
        leg.post_only = c.post_only;
        leg.order_type = OrderType::Limit;
        leg.expected_fee_bps = c.fee_bps;
        leg.expected_cost_bps = c.score_bps;
        leg.venue_score = c.score_bps;

        d.legs.push_back(leg);
        d.routed_qty = q;
        d.unfilled_qty = req.qty - q;
        d.expected_avg_price = c.price;
        d.expected_cost_bps = c.score_bps;

        if (d.unfilled_qty > 1e-12 && !req.allow_partial)
            return fail(req, "insufficient_liquidity_best_venue");

        if (d.unfilled_qty > 1e-12)
            d.reason = "partial_best_venue";

        return d;
    }

    RouteDecision route_split(const RouteRequest& req, const std::vector<Candidate>& cands) const {
        RouteDecision d;
        d.ok = true;
        d.requested_qty = req.qty;

        double remaining = req.qty;
        double notional = 0.0;
        double cost_weighted = 0.0;

        for (const auto& c : cands) {
            if (remaining <= 1e-12) break;

            double q = std::min({remaining, c.available_qty, c.max_order_qty, req.max_child_qty});
            double min_q = std::max(req.min_child_qty, c.min_order_qty);

            if (q < min_q) continue;

            RouteLeg leg;
            leg.venue = c.venue;
            leg.symbol = req.symbol;
            leg.side = req.side;
            leg.price = c.price;
            leg.qty = q;
            leg.post_only = c.post_only;
            leg.order_type = OrderType::Limit;
            leg.expected_fee_bps = c.fee_bps;
            leg.expected_cost_bps = c.score_bps;
            leg.venue_score = c.score_bps;

            d.legs.push_back(leg);

            remaining -= q;
            d.routed_qty += q;
            notional += q * c.price;
            cost_weighted += q * c.score_bps;

            if (req.style == RoutingStyle::BestVenue)
                break;
        }

        d.unfilled_qty = remaining;

        if (d.routed_qty <= 1e-12)
            return fail(req, "no_liquidity_after_filters");

        if (remaining > 1e-12 && !req.allow_partial)
            return fail(req, "insufficient_liquidity_no_partial");

        if (remaining > 1e-12)
            d.reason = "partial_route";

        d.expected_avg_price = notional / d.routed_qty;
        d.expected_cost_bps = cost_weighted / d.routed_qty;

        return d;
    }
};

} // namespace hft