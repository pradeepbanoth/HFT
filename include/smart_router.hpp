#pragma once
// smart_router.hpp — institutional-grade Smart Order Router
//
// Features:
// - Maker/taker routing
// - Best venue / split / cost / quality / balanced routing
// - Venue health scoring
// - Expected fill probability
// - Market impact penalty
// - Reject / latency / slippage / queue penalties
// - Venue concentration control
// - Route explanations for diagnostics
// - Works with current OrderBook API

#include "types.hpp"
#include "orderbook.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <sstream>

namespace hft {

enum class VenueStatus : uint8_t { Online, Degraded, Offline };
enum class VenueTier : uint8_t { Primary, Secondary, Backup, Experimental };

enum class RoutingStyle : uint8_t {
    BestVenue,
    SplitByLiquidity,
    CostOptimized,
    QualityOptimized,
    BalancedBestExecution
};

enum class LiquidityPreference : uint8_t {
    MakerOnly,
    TakerOnly,
    MakerThenTaker,
    TakerThenMaker
};

struct VenueConfig {
    std::string venue;
    VenueTier tier = VenueTier::Secondary;

    double maker_fee = -0.0001;
    double taker_fee = 0.0005;

    double min_order_qty = 0.0;
    double max_order_qty = 1e18;
    double max_route_fraction = 1.0;

    double latency_penalty_bps = 0.0;
    double reject_penalty_mult = 30.0;
    double fill_penalty_mult = 20.0;
    double queue_penalty_mult = 10.0;
    double data_penalty_mult = 12.0;
    double uptime_penalty_mult = 8.0;
    double impact_penalty_mult = 4.0;
    double degradation_penalty_bps = 8.0;

    double qualitative_score = 1.0;

    bool allow_maker = true;
    bool allow_taker = true;
};

struct VenueState {
    VenueStatus status = VenueStatus::Online;

    double recent_reject_rate = 0.0;
    double recent_fill_rate = 1.0;
    double queue_health = 1.0;
    double data_quality = 1.0;
    double uptime_score = 1.0;

    double avg_ack_latency_us = 0.0;
    double last_slippage_bps = 0.0;
    double recent_market_impact_bps = 0.0;

    int64_t last_update_ts = 0;
};

struct RouteRequest {
    std::string symbol;
    Side side = Side::Buy;
    double qty = 0.0;

    double limit_price = 0.0;
    double max_slippage_bps = 25.0;
    double max_venue_cost_bps = 150.0;

    RoutingStyle style = RoutingStyle::BalancedBestExecution;
    LiquidityPreference liquidity = LiquidityPreference::MakerThenTaker;

    bool allow_partial = true;
    double min_child_qty = 0.0;
    double max_child_qty = 1e18;

    double cost_weight = 0.45;
    double quality_weight = 0.25;
    double liquidity_weight = 0.15;
    double fill_probability_weight = 0.15;

    double min_fill_probability = 0.0;
    double max_single_venue_fraction = 1.0;
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
    double expected_spread_cost_bps = 0.0;
    double expected_impact_bps = 0.0;
    double expected_total_cost_bps = 0.0;

    double venue_quality = 0.0;
    double liquidity_score = 0.0;
    double fill_probability = 0.0;
    double final_score = 0.0;

    std::string explanation;
};

struct RouteDecision {
    bool ok = false;
    std::string reason;

    std::vector<RouteLeg> legs;

    double requested_qty = 0.0;
    double routed_qty = 0.0;
    double unfilled_qty = 0.0;

    double expected_avg_price = 0.0;
    double expected_total_cost_bps = 0.0;
    double expected_quality = 0.0;
    double expected_fill_probability = 0.0;
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

        std::vector<std::string> to_remove;
        for (const auto& [k, _] : books_) {
            if (k.rfind(venue + "|", 0) == 0) to_remove.push_back(k);
        }
        for (const auto& k : to_remove) books_.erase(k);
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

        sort_candidates(req, candidates);

        if (req.style == RoutingStyle::BestVenue)
            return route_best(req, candidates);

        return route_split(req, candidates);
    }

private:
    struct Candidate {
        std::string venue;
        bool maker = true;

        double price = 0.0;
        double available_qty = 0.0;

        double fee_bps = 0.0;
        double spread_cost_bps = 0.0;
        double impact_bps = 0.0;
        double total_cost_bps = 0.0;

        double quality = 0.0;
        double liquidity_score = 0.0;
        double fill_probability = 0.0;
        double final_score = 0.0;

        double min_order_qty = 0.0;
        double max_order_qty = 1e18;
        double max_route_fraction = 1.0;

        std::string explanation;
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

            const auto& st = sit->second;
            if (st.status == VenueStatus::Offline) continue;

            auto bit = books_.find(key(venue, req.symbol));
            if (bit == books_.end() || !bit->second) continue;

            bool try_maker =
                req.liquidity == LiquidityPreference::MakerOnly ||
                req.liquidity == LiquidityPreference::MakerThenTaker ||
                req.liquidity == LiquidityPreference::TakerThenMaker;

            bool try_taker =
                req.liquidity == LiquidityPreference::TakerOnly ||
                req.liquidity == LiquidityPreference::MakerThenTaker ||
                req.liquidity == LiquidityPreference::TakerThenMaker;

            if (try_maker && cfg.allow_maker) {
                auto c = make_candidate(req, cfg, st, *bit->second, true);
                if (c) out.push_back(*c);
            }

            if (try_taker && cfg.allow_taker) {
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
        c.maker = maker;
        c.min_order_qty = cfg.min_order_qty;
        c.max_order_qty = cfg.max_order_qty;
        c.max_route_fraction = cfg.max_route_fraction;

        if (!extract_price_qty(req, book, maker, c.price, c.available_qty))
            return std::nullopt;

        if (c.available_qty <= 1e-12)
            return std::nullopt;

        if (!passes_limit_price(req, c.price))
            return std::nullopt;

        if (!passes_slippage(req, book, c.price))
            return std::nullopt;

        c.fee_bps = (maker ? cfg.maker_fee : cfg.taker_fee) * 10000.0;
        c.spread_cost_bps = maker ? 0.0 : spread_cost(book, c.price);
        c.impact_bps = estimate_impact_bps(req.qty, c.available_qty, st, cfg);

        double reject_penalty = clamp01(st.recent_reject_rate) * cfg.reject_penalty_mult;
        double fill_penalty = (1.0 - clamp01(st.recent_fill_rate)) * cfg.fill_penalty_mult;
        double queue_penalty = maker ? (1.0 - clamp01(st.queue_health)) * cfg.queue_penalty_mult : 0.0;
        double data_penalty = (1.0 - clamp01(st.data_quality)) * cfg.data_penalty_mult;
        double uptime_penalty = (1.0 - clamp01(st.uptime_score)) * cfg.uptime_penalty_mult;
        double degradation_penalty = st.status == VenueStatus::Degraded ? cfg.degradation_penalty_bps : 0.0;
        double slippage_memory_penalty = std::abs(st.last_slippage_bps) * 0.30;

        c.total_cost_bps =
            c.fee_bps +
            c.spread_cost_bps +
            c.impact_bps +
            cfg.latency_penalty_bps +
            reject_penalty +
            fill_penalty +
            queue_penalty +
            data_penalty +
            uptime_penalty +
            degradation_penalty +
            slippage_memory_penalty;

        if (c.total_cost_bps > req.max_venue_cost_bps)
            return std::nullopt;

        c.quality = venue_quality(cfg, st, maker);
        c.liquidity_score = liquidity_score(c.available_qty, req.qty);
        c.fill_probability = expected_fill_probability(cfg, st, maker, c.available_qty, req.qty);

        if (c.fill_probability < req.min_fill_probability)
            return std::nullopt;

        c.final_score =
            req.cost_weight * c.total_cost_bps -
            req.quality_weight * c.quality * 10.0 -
            req.liquidity_weight * c.liquidity_score * 10.0 -
            req.fill_probability_weight * c.fill_probability * 10.0;

        c.explanation = build_explanation(c, cfg, st);
        return c;
    }

    static void sort_candidates(const RouteRequest& req, std::vector<Candidate>& cands) {
        std::sort(cands.begin(), cands.end(), [&](const Candidate& a, const Candidate& b) {
            if (req.liquidity == LiquidityPreference::MakerThenTaker && a.maker != b.maker)
                return a.maker;
            if (req.liquidity == LiquidityPreference::TakerThenMaker && a.maker != b.maker)
                return !a.maker;

            if (req.style == RoutingStyle::QualityOptimized) {
                if (std::abs(a.quality - b.quality) > 1e-12)
                    return a.quality > b.quality;
            }

            if (req.style == RoutingStyle::SplitByLiquidity) {
                if (std::abs(a.available_qty - b.available_qty) > 1e-12)
                    return a.available_qty > b.available_qty;
            }

            if (std::abs(a.final_score - b.final_score) > 1e-12)
                return a.final_score < b.final_score;

            return a.available_qty > b.available_qty;
        });
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
        for (const auto& lv : depth) s += lv.qty;
        return s;
    }

    static bool passes_limit_price(const RouteRequest& req, double px) {
        if (req.limit_price <= 0.0) return true;
        if (req.side == Side::Buy) return px <= req.limit_price;
        if (req.side == Side::Sell) return px >= req.limit_price;
        return false;
    }

    static bool passes_slippage(const RouteRequest& req, const OrderBook& book, double px) {
        auto mid = book.mid_price();
        if (!mid || *mid <= 1e-12) return true;
        double slip = std::abs(px - *mid) / *mid * 10000.0;
        return slip <= req.max_slippage_bps;
    }

    static double spread_cost(const OrderBook& book, double px) {
        auto mid = book.mid_price();
        if (!mid || *mid <= 1e-12) return 0.0;
        return std::abs(px - *mid) / *mid * 10000.0;
    }

    static double estimate_impact_bps(
        double requested_qty,
        double available_qty,
        const VenueState& st,
        const VenueConfig& cfg
    ) {
        if (available_qty <= 1e-12) return 1e6;
        double participation = requested_qty / available_qty;
        double nonlinear = participation * participation;
        return nonlinear * cfg.impact_penalty_mult + std::abs(st.recent_market_impact_bps) * 0.25;
    }

    static double expected_fill_probability(
        const VenueConfig& cfg,
        const VenueState& st,
        bool maker,
        double available_qty,
        double requested_qty
    ) {
        double base = clamp01(st.recent_fill_rate);
        double liquidity = available_qty > 1e-12
            ? clamp01(available_qty / std::max(requested_qty, 1e-12))
            : 0.0;

        double status =
            st.status == VenueStatus::Online ? 1.0 :
            st.status == VenueStatus::Degraded ? 0.65 : 0.0;

        double maker_adj = maker ? clamp01(st.queue_health) : 1.0;

        double tier_adj = 0.0;
        switch (cfg.tier) {
            case VenueTier::Primary: tier_adj = 1.0; break;
            case VenueTier::Secondary: tier_adj = 0.85; break;
            case VenueTier::Backup: tier_adj = 0.70; break;
            case VenueTier::Experimental: tier_adj = 0.45; break;
        }

        return clamp01(
            0.30 * base +
            0.25 * liquidity +
            0.20 * status +
            0.15 * maker_adj +
            0.10 * tier_adj
        );
    }

    static double venue_quality(const VenueConfig& cfg, const VenueState& st, bool maker) {
        double tier = 0.0;
        switch (cfg.tier) {
            case VenueTier::Primary: tier = 1.0; break;
            case VenueTier::Secondary: tier = 0.75; break;
            case VenueTier::Backup: tier = 0.50; break;
            case VenueTier::Experimental: tier = 0.25; break;
        }

        double status =
            st.status == VenueStatus::Online ? 1.0 :
            st.status == VenueStatus::Degraded ? 0.55 : 0.0;

        double maker_quality = maker ? clamp01(st.queue_health) : 1.0;

        return clamp01(
            0.22 * tier +
            0.18 * cfg.qualitative_score +
            0.18 * status +
            0.14 * clamp01(st.data_quality) +
            0.14 * clamp01(st.uptime_score) +
            0.14 * maker_quality
        );
    }

    static double liquidity_score(double available_qty, double requested_qty) {
        if (requested_qty <= 1e-12) return 0.0;
        return clamp01(available_qty / requested_qty);
    }

    static double clamp01(double x) {
        return std::clamp(x, 0.0, 1.0);
    }

    static std::string build_explanation(
        const Candidate& c,
        const VenueConfig&,
        const VenueState& st
    ) {
        std::ostringstream oss;
        oss << (c.maker ? "maker" : "taker")
            << " cost=" << c.total_cost_bps
            << "bps quality=" << c.quality
            << " fillProb=" << c.fill_probability
            << " liq=" << c.available_qty
            << " reject=" << st.recent_reject_rate
            << " fillRate=" << st.recent_fill_rate;
        return oss.str();
    }

    RouteDecision route_best(const RouteRequest& req, const std::vector<Candidate>& cands) const {
        RouteDecision d;
        d.ok = true;
        d.requested_qty = req.qty;

        const auto& c = cands.front();

        double max_by_fraction = req.qty * std::min(req.max_single_venue_fraction, c.max_route_fraction);
        double q = std::min({req.qty, c.available_qty, c.max_order_qty, req.max_child_qty, max_by_fraction});
        double min_q = std::max(req.min_child_qty, c.min_order_qty);

        if (q < min_q)
            return fail(req, "best_venue_below_min_qty");

        RouteLeg leg;
        fill_leg(req, c, q, leg);
        d.legs.push_back(leg);

        d.routed_qty = q;
        d.unfilled_qty = req.qty - q;
        d.expected_avg_price = c.price;
        d.expected_total_cost_bps = c.total_cost_bps;
        d.expected_quality = c.quality;
        d.expected_fill_probability = c.fill_probability;

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
        double quality_weighted = 0.0;
        double fill_prob_weighted = 0.0;

        std::unordered_map<std::string, double> venue_used;

        for (const auto& c : cands) {
            if (remaining <= 1e-12) break;

            double max_by_fraction = req.qty * std::min(req.max_single_venue_fraction, c.max_route_fraction);
            double already = venue_used[c.venue];
            double venue_capacity_left = std::max(0.0, max_by_fraction - already);

            double q = std::min({
                remaining,
                c.available_qty,
                c.max_order_qty,
                req.max_child_qty,
                venue_capacity_left
            });

            double min_q = std::max(req.min_child_qty, c.min_order_qty);
            if (q < min_q) continue;

            RouteLeg leg;
            fill_leg(req, c, q, leg);
            d.legs.push_back(leg);

            venue_used[c.venue] += q;
            remaining -= q;
            d.routed_qty += q;

            notional += q * c.price;
            cost_weighted += q * c.total_cost_bps;
            quality_weighted += q * c.quality;
            fill_prob_weighted += q * c.fill_probability;
        }

        d.unfilled_qty = remaining;

        if (d.routed_qty <= 1e-12)
            return fail(req, "no_liquidity_after_filters");

        if (remaining > 1e-12 && !req.allow_partial)
            return fail(req, "insufficient_liquidity_no_partial");

        if (remaining > 1e-12)
            d.reason = "partial_route";

        d.expected_avg_price = notional / d.routed_qty;
        d.expected_total_cost_bps = cost_weighted / d.routed_qty;
        d.expected_quality = quality_weighted / d.routed_qty;
        d.expected_fill_probability = fill_prob_weighted / d.routed_qty;

        return d;
    }

    static void fill_leg(const RouteRequest& req, const Candidate& c, double qty, RouteLeg& leg) {
        leg.venue = c.venue;
        leg.symbol = req.symbol;
        leg.side = req.side;
        leg.order_type = OrderType::Limit;
        leg.price = c.price;
        leg.qty = qty;
        leg.post_only = c.maker;

        leg.expected_fee_bps = c.fee_bps;
        leg.expected_spread_cost_bps = c.spread_cost_bps;
        leg.expected_impact_bps = c.impact_bps;
        leg.expected_total_cost_bps = c.total_cost_bps;

        leg.venue_quality = c.quality;
        leg.liquidity_score = c.liquidity_score;
        leg.fill_probability = c.fill_probability;
        leg.final_score = c.final_score;
        leg.explanation = c.explanation;
    }
};

} // namespace hft