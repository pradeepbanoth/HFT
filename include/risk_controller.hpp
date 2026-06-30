#pragma once
// risk_controller.hpp — active institutional portfolio risk controller

#include "portfolio_risk.hpp"
#include "exchange_gateway.hpp"
#include "oms.hpp"
#include "types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class RiskControlActionType : uint8_t {
    None,
    ReduceRisk,
    CancelAllOrders,
    HedgeInventory,
    HaltTrading,
    ResumeTrading,
    AlertOnly
};

enum class RiskActionPriority : uint8_t {
    Low,
    Medium,
    High,
    Emergency
};

struct RiskControlAction {
    RiskControlActionType type = RiskControlActionType::None;
    RiskActionPriority priority = RiskActionPriority::Low;

    std::string symbol;
    Side side = Side::Unknown;
    double qty = 0.0;

    std::string reason;
    std::vector<std::string> breaches;

    int64_t ts = 0;
};

struct RiskGeneratedOrder {
    Order order;
    std::string parent_reason;
    RiskActionPriority priority = RiskActionPriority::Medium;
};

struct RiskControllerConfig {
    bool auto_cancel_on_critical = true;
    bool auto_cancel_on_halt = true;
    bool auto_halt_gateway = true;
    bool generate_hedges = true;

    double hedge_fraction_warning = 0.25;
    double hedge_fraction_critical = 0.50;
    double hedge_fraction_halt = 0.75;

    double min_hedge_qty = 1e-12;
    double max_hedge_qty = 1e18;

    int64_t action_cooldown_ns = 1'000'000'000LL;
    int32_t repeated_breach_escalation_count = 3;
};

struct RiskControllerStats {
    int64_t evaluations = 0;
    int64_t normals = 0;
    int64_t warnings = 0;
    int64_t criticals = 0;
    int64_t halts = 0;

    int64_t actions_generated = 0;
    int64_t cancel_all_actions = 0;
    int64_t hedge_actions = 0;
    int64_t halt_actions = 0;
    int64_t suppressed_by_cooldown = 0;
};

class RiskController {
public:
    explicit RiskController(RiskControllerConfig cfg = {})
        : cfg_(cfg) {}

    std::vector<RiskControlAction> decide(
        const PortfolioRiskReport& report,
        int64_t ts
    ) {
        ++stats_.evaluations;

        update_level_stats(report.level);
        update_breach_counts(report);

        std::vector<RiskControlAction> actions;

        if (report.level == PortfolioRiskLevel::Normal) {
            actions.push_back(make_action(
                RiskControlActionType::None,
                RiskActionPriority::Low,
                "",
                Side::Unknown,
                0.0,
                "risk_normal",
                report,
                ts
            ));
            ++stats_.normals;
            return actions;
        }

        if (report.level == PortfolioRiskLevel::Warning) {
            actions.push_back(make_action(
                RiskControlActionType::ReduceRisk,
                RiskActionPriority::Medium,
                report.max_concentration_symbol,
                Side::Unknown,
                0.0,
                "warning_reduce_risk",
                report,
                ts
            ));
            ++stats_.warnings;
        }

        if (report.level == PortfolioRiskLevel::Critical) {
            if (cfg_.auto_cancel_on_critical) {
                actions.push_back(make_action(
                    RiskControlActionType::CancelAllOrders,
                    RiskActionPriority::High,
                    "",
                    Side::Unknown,
                    0.0,
                    "critical_cancel_all",
                    report,
                    ts
                ));
                ++stats_.cancel_all_actions;
            }

            actions.push_back(make_action(
                RiskControlActionType::ReduceRisk,
                RiskActionPriority::High,
                report.max_concentration_symbol,
                Side::Unknown,
                0.0,
                "critical_reduce_risk",
                report,
                ts
            ));

            ++stats_.criticals;
        }

        if (report.level == PortfolioRiskLevel::Halt) {
            if (cfg_.auto_cancel_on_halt) {
                actions.push_back(make_action(
                    RiskControlActionType::CancelAllOrders,
                    RiskActionPriority::Emergency,
                    "",
                    Side::Unknown,
                    0.0,
                    "halt_cancel_all",
                    report,
                    ts
                ));
                ++stats_.cancel_all_actions;
            }

            actions.push_back(make_action(
                RiskControlActionType::HaltTrading,
                RiskActionPriority::Emergency,
                "",
                Side::Unknown,
                0.0,
                "halt_trading",
                report,
                ts
            ));

            ++stats_.halt_actions;
            ++stats_.halts;
        }

        escalate_repeated_breaches(actions, report, ts);
        filter_cooldown(actions, ts);

        stats_.actions_generated += static_cast<int64_t>(actions.size());
        audit_.insert(audit_.end(), actions.begin(), actions.end());

        return actions;
    }

    std::vector<RiskGeneratedOrder> generate_hedge_orders(
        const PortfolioRiskReport& report,
        const PortfolioState& portfolio,
        const std::unordered_map<std::string, double>& prices,
        int64_t ts
    ) {
        std::vector<RiskGeneratedOrder> orders;

        if (!cfg_.generate_hedges) return orders;
        if (report.level == PortfolioRiskLevel::Normal) return orders;

        double hedge_fraction = hedge_fraction_for(report.level);
        if (hedge_fraction <= 0.0) return orders;

        const auto& pos = portfolio.positions();

        for (const auto& c : report.contributions) {
            auto it = pos.find(c.symbol);
            if (it == pos.end()) continue;

            double position = it->second;
            if (std::abs(position) <= cfg_.min_hedge_qty) continue;

            double qty = std::abs(position) * hedge_fraction;
            qty = std::clamp(qty, cfg_.min_hedge_qty, cfg_.max_hedge_qty);

            auto pit = prices.find(c.symbol);
            double px = pit != prices.end() ? pit->second : 0.0;
            if (px <= 0.0) continue;

            Order o;
            o.order_id = "risk_hedge_" + c.symbol + "_" + std::to_string(ts);
            o.client_id = o.order_id;
            o.symbol = c.symbol;
            o.side = position > 0.0 ? Side::Sell : Side::Buy;
            o.price = px;
            o.qty = qty;
            o.timestamp = ts;
            o.order_type = OrderType::Market;

            RiskGeneratedOrder go;
            go.order = o;
            go.parent_reason = "risk_hedge_" + c.symbol;
            go.priority = priority_for(report.level);

            orders.push_back(std::move(go));
            ++stats_.hedge_actions;

            if (report.level == PortfolioRiskLevel::Warning)
                break;
        }

        return orders;
    }

    void apply_gateway_actions(
        const std::vector<RiskControlAction>& actions,
        IExchangeGateway& gateway
    ) {
        for (const auto& a : actions) {
            if (a.type == RiskControlActionType::HaltTrading && cfg_.auto_halt_gateway) {
                gateway.halt(a.reason);
            }
        }
    }

    void apply_cancel_actions_to_gateway(
        const std::vector<RiskControlAction>& actions,
        PaperExchangeGateway& gateway
    ) {
        bool should_cancel = false;
        for (const auto& a : actions) {
            if (a.type == RiskControlActionType::CancelAllOrders) {
                should_cancel = true;
                break;
            }
        }

        if (!should_cancel) return;

        std::vector<std::string> ids;
        for (const auto& [id, order] : gateway.live_orders()) {
            ids.push_back(id);
        }

        for (const auto& id : ids) {
            gateway.cancel_order(id);
        }
    }

    const RiskControllerStats& stats() const noexcept {
        return stats_;
    }

    const std::vector<RiskControlAction>& audit_log() const noexcept {
        return audit_;
    }

private:
    RiskControllerConfig cfg_;
    RiskControllerStats stats_;

    std::unordered_map<std::string, int32_t> breach_counts_;
    std::unordered_map<std::string, int64_t> last_action_ts_;
    std::vector<RiskControlAction> audit_;

    void update_level_stats(PortfolioRiskLevel) {}

    void update_breach_counts(const PortfolioRiskReport& report) {
        for (const auto& b : report.breaches)
            ++breach_counts_[b];
    }

    RiskControlAction make_action(
        RiskControlActionType type,
        RiskActionPriority priority,
        std::string symbol,
        Side side,
        double qty,
        std::string reason,
        const PortfolioRiskReport& report,
        int64_t ts
    ) const {
        RiskControlAction a;
        a.type = type;
        a.priority = priority;
        a.symbol = std::move(symbol);
        a.side = side;
        a.qty = qty;
        a.reason = std::move(reason);
        a.breaches = report.breaches;
        a.ts = ts;
        return a;
    }

    void escalate_repeated_breaches(
        std::vector<RiskControlAction>& actions,
        const PortfolioRiskReport& report,
        int64_t ts
    ) {
        for (const auto& b : report.breaches) {
            auto it = breach_counts_.find(b);
            if (it == breach_counts_.end()) continue;

            if (it->second >= cfg_.repeated_breach_escalation_count) {
                bool already_halt = std::any_of(actions.begin(), actions.end(),
                    [](const RiskControlAction& a) {
                        return a.type == RiskControlActionType::HaltTrading;
                    });

                if (!already_halt) {
                    actions.push_back(make_action(
                        RiskControlActionType::HaltTrading,
                        RiskActionPriority::Emergency,
                        "",
                        Side::Unknown,
                        0.0,
                        "repeated_breach_escalation:" + b,
                        report,
                        ts
                    ));
                    ++stats_.halt_actions;
                }
            }
        }
    }

    void filter_cooldown(std::vector<RiskControlAction>& actions, int64_t ts) {
        std::vector<RiskControlAction> filtered;
        filtered.reserve(actions.size());

        for (const auto& a : actions) {
            std::string k = action_key(a);
            auto it = last_action_ts_.find(k);

            if (it != last_action_ts_.end() &&
                ts - it->second < cfg_.action_cooldown_ns &&
                a.priority != RiskActionPriority::Emergency) {
                ++stats_.suppressed_by_cooldown;
                continue;
            }

            last_action_ts_[k] = ts;
            filtered.push_back(a);
        }

        actions = std::move(filtered);
    }

    static std::string action_key(const RiskControlAction& a) {
        return std::to_string(static_cast<int>(a.type)) + "|" + a.symbol + "|" + a.reason;
    }

    static double hedge_fraction_for(PortfolioRiskLevel level) {
        switch (level) {
            case PortfolioRiskLevel::Warning: return 0.25;
            case PortfolioRiskLevel::Critical: return 0.50;
            case PortfolioRiskLevel::Halt: return 0.75;
            default: return 0.0;
        }
    }

    static RiskActionPriority priority_for(PortfolioRiskLevel level) {
        switch (level) {
            case PortfolioRiskLevel::Warning: return RiskActionPriority::Medium;
            case PortfolioRiskLevel::Critical: return RiskActionPriority::High;
            case PortfolioRiskLevel::Halt: return RiskActionPriority::Emergency;
            default: return RiskActionPriority::Low;
        }
    }
};

} // namespace hft