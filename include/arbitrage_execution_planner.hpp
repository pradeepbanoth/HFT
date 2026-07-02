#pragma once

#include "cross_exchange_arbitrage.hpp"
#include "smart_order_router.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace hft::arbitrage {

enum class HedgePolicy : uint8_t {
    ImmediateBothLegs,
    BuyFirstThenHedge,
    SellFirstThenHedge
};

enum class ExecutionUrgency : uint8_t {
    Passive,
    Normal,
    Aggressive,
    Immediate
};

enum class ExecutionPlanState : uint8_t {
    Rejected,
    Ready
};

enum class PlanRejectCode : uint8_t {
    None,
    NotExecutable,
    InvalidPriceOrQty,
    ProfitTooSmall,
    NotionalTooLarge,
    EdgeTooSmall,
    QuantityTooSmall,
    LegImbalance,
    SelfTradeRisk
};

struct ArbExecutionConfig {
    double max_notional{1'000'000.0};
    double min_expected_profit{0.0};
    double min_net_edge_bps{1.0};
    double min_qty{0.000001};
    double max_leg_qty_imbalance{0.000001};

    uint64_t plan_ttl_ns{250'000'000};
    HedgePolicy hedge_policy{HedgePolicy::ImmediateBothLegs};
    ExecutionUrgency urgency{ExecutionUrgency::Immediate};

    bool prevent_same_venue_cross{true};
    bool allow_partial_execution{false};
};

struct ArbExecutionLeg {
    uint64_t leg_id{0};
    uint64_t plan_id{0};

    std::string venue;
    std::string symbol;

    ArbLegSide side{ArbLegSide::Buy};

    double quantity{0.0};
    double price{0.0};
    double notional{0.0};

    uint8_t priority{0};
    bool hedge_leg{false};
};

struct ArbExecutionPlan {
    ExecutionPlanState state{ExecutionPlanState::Rejected};
    PlanRejectCode reject_code{PlanRejectCode::None};
    std::string reason;

    uint64_t plan_id{0};
    uint64_t created_ns{0};
    uint64_t expires_ns{0};

    std::string symbol;

    double quantity{0.0};
    double expected_profit{0.0};
    double net_edge_bps{0.0};
    double gross_edge_bps{0.0};
    double expected_buy_notional{0.0};
    double expected_sell_notional{0.0};

    HedgePolicy hedge_policy{HedgePolicy::ImmediateBothLegs};
    ExecutionUrgency urgency{ExecutionUrgency::Immediate};

    ArbExecutionLeg buy_leg;
    ArbExecutionLeg sell_leg;

    std::vector<hft::routing::RouterOrder> sor_orders;
};

struct ArbExecutionPlannerMetrics {
    std::atomic<uint64_t> plans_requested{0};
    std::atomic<uint64_t> plans_ready{0};
    std::atomic<uint64_t> plans_rejected{0};
    std::atomic<uint64_t> notional_rejects{0};
    std::atomic<uint64_t> edge_rejects{0};
    std::atomic<uint64_t> profit_rejects{0};
    std::atomic<uint64_t> self_trade_rejects{0};
};

class ArbitrageExecutionPlanner {
public:
    explicit ArbitrageExecutionPlanner(ArbExecutionConfig config = {})
        : config_(config) {}

    ArbExecutionPlan build_plan(const ArbitrageOpportunity& opp) {
        metrics_.plans_requested.fetch_add(1, std::memory_order_relaxed);

        const uint64_t plan_id = next_plan_id_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t now = arb_now_ns();

        if (opp.quantity <= 0.0 || opp.buy_price <= 0.0 || opp.sell_price <= 0.0) {
            return reject(plan_id, now, PlanRejectCode::InvalidPriceOrQty, "Invalid price or quantity");
        }

        if (opp.quantity < config_.min_qty) {
            return reject(plan_id, now, PlanRejectCode::QuantityTooSmall, "Quantity below minimum");
        }

        if (opp.net_edge_bps < config_.min_net_edge_bps) {
            metrics_.edge_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(plan_id, now, PlanRejectCode::EdgeTooSmall, "Net edge below planner threshold");
        }

        if (opp.expected_profit < config_.min_expected_profit) {
            metrics_.profit_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(plan_id, now, PlanRejectCode::ProfitTooSmall, "Expected profit below threshold");
        }

        if (config_.prevent_same_venue_cross && opp.buy_venue == opp.sell_venue) {
            metrics_.self_trade_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(plan_id, now, PlanRejectCode::SelfTradeRisk, "Buy and sell venue are identical");
        }

        const double buy_notional = opp.quantity * opp.buy_price;
        const double sell_notional = opp.quantity * opp.sell_price;

        if (buy_notional > config_.max_notional || sell_notional > config_.max_notional) {
            metrics_.notional_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(plan_id, now, PlanRejectCode::NotionalTooLarge, "Notional limit exceeded");
        }

        const double qty_imbalance = std::abs(opp.quantity - opp.quantity);

        if (qty_imbalance > config_.max_leg_qty_imbalance) {
            return reject(plan_id, now, PlanRejectCode::LegImbalance, "Leg quantity imbalance");
        }

        ArbExecutionPlan plan;
        plan.state = ExecutionPlanState::Ready;
        plan.reject_code = PlanRejectCode::None;
        plan.reason = "Execution plan ready";

        plan.plan_id = plan_id;
        plan.created_ns = now;
        plan.expires_ns = now + config_.plan_ttl_ns;

        plan.symbol = opp.symbol;
        plan.quantity = opp.quantity;
        plan.expected_profit = opp.expected_profit;
        plan.net_edge_bps = opp.net_edge_bps;
        plan.gross_edge_bps = opp.gross_edge_bps;
        plan.expected_buy_notional = buy_notional;
        plan.expected_sell_notional = sell_notional;

        plan.hedge_policy = config_.hedge_policy;
        plan.urgency = config_.urgency;

        plan.buy_leg = make_leg(
            plan_id,
            1,
            opp.buy_venue,
            opp.symbol,
            ArbLegSide::Buy,
            opp.quantity,
            opp.buy_price,
            buy_notional,
            buy_priority(),
            is_buy_hedge_leg()
        );

        plan.sell_leg = make_leg(
            plan_id,
            2,
            opp.sell_venue,
            opp.symbol,
            ArbLegSide::Sell,
            opp.quantity,
            opp.sell_price,
            sell_notional,
            sell_priority(),
            is_sell_hedge_leg()
        );

        add_sor_orders(plan);

        metrics_.plans_ready.fetch_add(1, std::memory_order_relaxed);
        return plan;
    }

    bool expired(const ArbExecutionPlan& plan, uint64_t now_ns = arb_now_ns()) const noexcept {
        return plan.expires_ns > 0 && now_ns > plan.expires_ns;
    }

    const ArbExecutionPlannerMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    ArbExecutionPlan reject(
        uint64_t plan_id,
        uint64_t now,
        PlanRejectCode code,
        std::string reason
    ) {
        metrics_.plans_rejected.fetch_add(1, std::memory_order_relaxed);

        ArbExecutionPlan plan;
        plan.state = ExecutionPlanState::Rejected;
        plan.reject_code = code;
        plan.reason = std::move(reason);
        plan.plan_id = plan_id;
        plan.created_ns = now;
        plan.expires_ns = now;
        return plan;
    }

    ArbExecutionLeg make_leg(
        uint64_t plan_id,
        uint64_t leg_offset,
        std::string venue,
        std::string symbol,
        ArbLegSide side,
        double qty,
        double price,
        double notional,
        uint8_t priority,
        bool hedge_leg
    ) const {
        ArbExecutionLeg leg;
        leg.plan_id = plan_id;
        leg.leg_id = plan_id * 10 + leg_offset;
        leg.venue = std::move(venue);
        leg.symbol = std::move(symbol);
        leg.side = side;
        leg.quantity = qty;
        leg.price = price;
        leg.notional = notional;
        leg.priority = priority;
        leg.hedge_leg = hedge_leg;
        return leg;
    }

    uint8_t buy_priority() const noexcept {
        switch (config_.hedge_policy) {
            case HedgePolicy::ImmediateBothLegs: return 0;
            case HedgePolicy::BuyFirstThenHedge: return 0;
            case HedgePolicy::SellFirstThenHedge: return 1;
        }
        return 0;
    }

    uint8_t sell_priority() const noexcept {
        switch (config_.hedge_policy) {
            case HedgePolicy::ImmediateBothLegs: return 0;
            case HedgePolicy::BuyFirstThenHedge: return 1;
            case HedgePolicy::SellFirstThenHedge: return 0;
        }
        return 0;
    }

    bool is_buy_hedge_leg() const noexcept {
        return config_.hedge_policy == HedgePolicy::SellFirstThenHedge;
    }

    bool is_sell_hedge_leg() const noexcept {
        return config_.hedge_policy == HedgePolicy::BuyFirstThenHedge;
    }

    hft::routing::RouterOrder to_sor_order(const ArbExecutionLeg& leg) const {
        hft::routing::RouterOrder order;
        order.symbol = leg.symbol;
        order.qty = leg.quantity;
        order.limit_price = leg.price;
        order.type = hft::routing::OrderType::Limit;
        order.client_ts_ns = arb_now_ns();

        order.side =
            leg.side == ArbLegSide::Buy
                ? hft::routing::Side::Buy
                : hft::routing::Side::Sell;

        return order;
    }

    void add_sor_orders(ArbExecutionPlan& plan) const {
        if (plan.buy_leg.priority <= plan.sell_leg.priority) {
            plan.sor_orders.push_back(to_sor_order(plan.buy_leg));
            plan.sor_orders.push_back(to_sor_order(plan.sell_leg));
        } else {
            plan.sor_orders.push_back(to_sor_order(plan.sell_leg));
            plan.sor_orders.push_back(to_sor_order(plan.buy_leg));
        }
    }

private:
    ArbExecutionConfig config_;
    std::atomic<uint64_t> next_plan_id_{1};
    ArbExecutionPlannerMetrics metrics_{};
};

} // namespace hft::arbitrage