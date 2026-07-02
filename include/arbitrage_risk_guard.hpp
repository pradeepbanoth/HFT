#pragma once

#include "arbitrage_execution_planner.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace hft::arbitrage {

enum class ArbRiskDecision : uint8_t {
    Approved,
    Rejected
};

enum class ArbRejectCode : uint8_t {
    None,
    KillSwitch,
    PlanNotReady,
    InvalidPlan,
    StaleQuote,
    SuspiciousEdge,
    MaxPlanNotional,
    SymbolExposure,
    VenueExposure,
    VenueConcentration,
    DailyLossLimit,
    InventoryImbalance,
    CooldownActive
};

struct ArbRiskConfig {
    double max_plan_notional{1'000'000.0};
    double max_symbol_exposure{2'000'000.0};
    double max_venue_exposure{1'000'000.0};
    double max_total_exposure{5'000'000.0};

    double max_venue_concentration{0.75};
    double max_inventory_imbalance{100'000.0};

    double min_net_edge_bps{1.0};
    double max_allowed_net_edge_bps{5000.0};

    double max_daily_loss{50'000.0};

    uint64_t max_quote_age_ns{1'000'000'000ULL};
    uint64_t reject_cooldown_ns{250'000'000ULL};
};

struct ArbRiskContext {
    uint64_t now_ns{0};

    uint64_t buy_quote_ts_ns{0};
    uint64_t sell_quote_ts_ns{0};

    // Exposure
    double current_symbol_exposure{0.0};
    double current_buy_venue_exposure{0.0};
    double current_sell_venue_exposure{0.0};
    double total_portfolio_exposure{0.0};

    // Inventory
    double inventory_base{0.0};
    double inventory_quote{0.0};

    // PnL
    double realized_pnl_today{0.0};
    double unrealized_pnl{0.0};
};

struct ArbRiskResult {
    ArbRiskDecision decision{ArbRiskDecision::Rejected};
    ArbRejectCode code{ArbRejectCode::None};
    std::string reason;
};

struct ArbExposureSnapshot {
    double total_exposure{0.0};
    std::unordered_map<std::string, double> symbol_exposure;
    std::unordered_map<std::string, double> venue_exposure;
};

struct ArbRiskMetrics {
    std::atomic<uint64_t> checks{0};
    std::atomic<uint64_t> approved{0};
    std::atomic<uint64_t> rejected{0};

    std::atomic<uint64_t> stale_quotes{0};
    std::atomic<uint64_t> kill_switch_rejects{0};
    std::atomic<uint64_t> notional_rejects{0};
    std::atomic<uint64_t> exposure_rejects{0};
    std::atomic<uint64_t> pnl_rejects{0};
    std::atomic<uint64_t> inventory_rejects{0};
    std::atomic<uint64_t> cooldown_rejects{0};
};

class ArbitrageRiskGuard {
public:
    explicit ArbitrageRiskGuard(ArbRiskConfig config = {})
        : config_(config) {}

    void set_kill_switch(bool enabled) noexcept {
        kill_switch_.store(enabled, std::memory_order_release);
    }

    bool kill_switch_enabled() const noexcept {
        return kill_switch_.load(std::memory_order_acquire);
    }

    ArbRiskResult check(
        const ArbExecutionPlan& plan,
        const ArbRiskContext& ctx
    ) {
        metrics_.checks.fetch_add(1, std::memory_order_relaxed);

        if (kill_switch_enabled()) {
            metrics_.kill_switch_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(ArbRejectCode::KillSwitch, "Kill switch enabled");
        }

        if (cooldown_active(plan.symbol, ctx.now_ns)) {
            metrics_.cooldown_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(ArbRejectCode::CooldownActive, "Symbol cooldown active");
        }

        if (plan.state != ExecutionPlanState::Ready) {
            return reject(ArbRejectCode::PlanNotReady, "Execution plan not ready");
        }

        if (!valid_plan(plan)) {
            return reject(ArbRejectCode::InvalidPlan, "Invalid arbitrage plan");
        }

        if (stale(ctx.now_ns, ctx.buy_quote_ts_ns) ||
            stale(ctx.now_ns, ctx.sell_quote_ts_ns)) {
            metrics_.stale_quotes.fetch_add(1, std::memory_order_relaxed);
            remember_reject(plan.symbol, ctx.now_ns);
            return reject(ArbRejectCode::StaleQuote, "Stale quote");
        }

        if (plan.net_edge_bps < config_.min_net_edge_bps ||
            plan.net_edge_bps > config_.max_allowed_net_edge_bps) {
            remember_reject(plan.symbol, ctx.now_ns);
            return reject(ArbRejectCode::SuspiciousEdge, "Net edge outside allowed range");
        }

        if (daily_loss_exceeded(ctx)) {
            metrics_.pnl_rejects.fetch_add(1, std::memory_order_relaxed);
            set_kill_switch(true);
            return reject(ArbRejectCode::DailyLossLimit, "Daily loss limit exceeded");
        }

        const double plan_notional = std::max(
            plan.buy_leg.notional,
            plan.sell_leg.notional
        );

        if (plan_notional <= 0.0 || plan_notional > config_.max_plan_notional) {
            metrics_.notional_rejects.fetch_add(1, std::memory_order_relaxed);
            remember_reject(plan.symbol, ctx.now_ns);
            return reject(ArbRejectCode::MaxPlanNotional, "Plan notional limit exceeded");
        }

        if (std::abs(ctx.inventory_base * plan.buy_leg.price) >
            config_.max_inventory_imbalance) {
            metrics_.inventory_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(ArbRejectCode::InventoryImbalance, "Inventory imbalance limit exceeded");
        }

        const double projected_total = exposure_.total_exposure + plan_notional;

        if (projected_total > config_.max_total_exposure) {
            metrics_.exposure_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(ArbRejectCode::SymbolExposure, "Total exposure limit exceeded");
        }

        const double projected_symbol =
            exposure_.symbol_exposure[plan.symbol] + plan_notional;

        if (projected_symbol > config_.max_symbol_exposure) {
            metrics_.exposure_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(ArbRejectCode::SymbolExposure, "Symbol exposure limit exceeded");
        }

        const double projected_buy_venue =
            exposure_.venue_exposure[plan.buy_leg.venue] + plan.buy_leg.notional;

        const double projected_sell_venue =
            exposure_.venue_exposure[plan.sell_leg.venue] + plan.sell_leg.notional;

        if (projected_buy_venue > config_.max_venue_exposure ||
            projected_sell_venue > config_.max_venue_exposure) {
            metrics_.exposure_rejects.fetch_add(1, std::memory_order_relaxed);
            return reject(ArbRejectCode::VenueExposure, "Venue exposure limit exceeded");
        }

        if (projected_total > 0.0) {
            const double buy_concentration = projected_buy_venue / projected_total;
            const double sell_concentration = projected_sell_venue / projected_total;

            if (buy_concentration > config_.max_venue_concentration ||
                sell_concentration > config_.max_venue_concentration) {
                metrics_.exposure_rejects.fetch_add(1, std::memory_order_relaxed);
                return reject(ArbRejectCode::VenueConcentration, "Venue concentration limit exceeded");
            }
        }

        metrics_.approved.fetch_add(1, std::memory_order_relaxed);

        return {
            ArbRiskDecision::Approved,
            ArbRejectCode::None,
            "Arbitrage plan approved"
        };
    }

    void reserve_exposure(const ArbExecutionPlan& plan) {
        if (plan.state != ExecutionPlanState::Ready) return;

        const double plan_notional = std::max(
            plan.buy_leg.notional,
            plan.sell_leg.notional
        );

        exposure_.total_exposure += plan_notional;
        exposure_.symbol_exposure[plan.symbol] += plan_notional;
        exposure_.venue_exposure[plan.buy_leg.venue] += plan.buy_leg.notional;
        exposure_.venue_exposure[plan.sell_leg.venue] += plan.sell_leg.notional;
    }

    void release_exposure(const ArbExecutionPlan& plan) {
        if (plan.state != ExecutionPlanState::Ready) return;

        const double plan_notional = std::max(
            plan.buy_leg.notional,
            plan.sell_leg.notional
        );

        subtract_floor_zero(exposure_.total_exposure, plan_notional);
        subtract_floor_zero(exposure_.symbol_exposure[plan.symbol], plan_notional);
        subtract_floor_zero(exposure_.venue_exposure[plan.buy_leg.venue], plan.buy_leg.notional);
        subtract_floor_zero(exposure_.venue_exposure[plan.sell_leg.venue], plan.sell_leg.notional);
    }

    ArbExposureSnapshot exposure_snapshot() const {
        return exposure_;
    }

    const ArbRiskMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    static void subtract_floor_zero(double& value, double amount) noexcept {
        value -= amount;
        if (value < 0.0) value = 0.0;
    }

    bool stale(uint64_t now_ns, uint64_t quote_ts_ns) const noexcept {
        if (now_ns == 0 || quote_ts_ns == 0) return true;
        if (quote_ts_ns > now_ns) return true;
        return now_ns - quote_ts_ns > config_.max_quote_age_ns;
    }

    bool daily_loss_exceeded(const ArbRiskContext& ctx) const noexcept {
        const double total_pnl = ctx.realized_pnl_today + ctx.unrealized_pnl;
        return total_pnl <= -std::abs(config_.max_daily_loss);
    }

    bool valid_plan(const ArbExecutionPlan& plan) const noexcept {
        if (plan.symbol.empty()) return false;
        if (plan.buy_leg.venue.empty() || plan.sell_leg.venue.empty()) return false;
        if (plan.buy_leg.venue == plan.sell_leg.venue) return false;
        if (plan.buy_leg.quantity <= 0.0 || plan.sell_leg.quantity <= 0.0) return false;
        if (plan.buy_leg.price <= 0.0 || plan.sell_leg.price <= 0.0) return false;
        if (std::abs(plan.buy_leg.quantity - plan.sell_leg.quantity) >
            0.000001) {
            return false;
        }
        return true;
    }

    bool cooldown_active(const std::string& symbol, uint64_t now_ns) const {
        auto it = last_reject_ns_.find(symbol);
        if (it == last_reject_ns_.end()) return false;
        if (now_ns <= it->second) return true;
        return now_ns - it->second < config_.reject_cooldown_ns;
    }

    void remember_reject(const std::string& symbol, uint64_t now_ns) {
        if (!symbol.empty() && now_ns > 0) {
            last_reject_ns_[symbol] = now_ns;
        }
    }

    ArbRiskResult reject(ArbRejectCode code, std::string reason) {
        metrics_.rejected.fetch_add(1, std::memory_order_relaxed);
        return {
            ArbRiskDecision::Rejected,
            code,
            std::move(reason)
        };
    }

private:
    ArbRiskConfig config_;
    std::atomic<bool> kill_switch_{false};

    ArbExposureSnapshot exposure_;
    std::unordered_map<std::string, uint64_t> last_reject_ns_;

    ArbRiskMetrics metrics_{};
};

} // namespace hft::arbitrage