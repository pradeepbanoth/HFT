#pragma once

#include "arbitrage_risk_guard.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hft::arbitrage {



enum class ArbLegState : uint8_t {
    Created,
    Submitted,
    Acknowledged,
    PartiallyFilled,
    Filled,
    Rejected,
    CancelPending,
    Cancelled,
    Expired
};

enum class ArbExecutorState : uint8_t {
    Created,
    Submitted,
    Working,
    Completed,
    Rejected,
    Cancelled,
    Expired,
    Imbalanced,
    EmergencyHedgeRequired
};

enum class ArbExecReportType : uint8_t {
    Ack,
    PartialFill,
    Fill,
    Reject,
    CancelAck,
    Expire
};

struct ArbLegSubmission {
    uint64_t plan_id{0};
    uint64_t leg_id{0};
    std::string venue;
    std::string symbol;
    ArbLegSide side{ArbLegSide::Buy};
    double quantity{0.0};
    double price{0.0};
    double notional{0.0};
    uint64_t submit_ts_ns{0};
};

struct ArbExecutionReport {
    uint64_t plan_id{0};
    uint64_t leg_id{0};
    bool is_buy_leg{true};

    ArbExecReportType type{ArbExecReportType::Ack};

    double fill_qty{0.0};
    double fill_price{0.0};

    uint64_t exchange_ts_ns{0};
    uint64_t receive_ts_ns{0};

    std::string reason;

    ArbExecutionReport() = default;

    ArbExecutionReport(
        uint64_t p,
        uint64_t l,
        ArbExecReportType t,
        double fq,
        double fp,
        uint64_t ets,
        uint64_t rts,
        std::string r
    )
        : plan_id(p),
          leg_id(l),
          is_buy_leg(true),
          type(t),
          fill_qty(fq),
          fill_price(fp),
          exchange_ts_ns(ets),
          receive_ts_ns(rts),
          reason(std::move(r)) {}

    ArbExecutionReport(
        uint64_t p,
        bool buy_leg,
        ArbExecReportType t,
        double fq,
        double fp,
        std::string r
    )
        : plan_id(p),
          leg_id(0),
          is_buy_leg(buy_leg),
          type(t),
          fill_qty(fq),
          fill_price(fp),
          exchange_ts_ns(0),
          receive_ts_ns(0),
          reason(std::move(r)) {}
};

struct EmergencyHedgeRequest {
    uint64_t plan_id{0};
    std::string symbol;
    std::string preferred_venue;
    ArbLegSide side{ArbLegSide::Buy};
    double quantity{0.0};
    double reference_price{0.0};
    std::string reason;
};

struct ArbLegRuntime {
    uint64_t leg_id{0};
    std::string venue;
    std::string symbol;
    ArbLegSide side{ArbLegSide::Buy};
    ArbLegState state{ArbLegState::Created};

    double target_qty{0.0};
    double limit_price{0.0};
    double filled_qty{0.0};
    double vwap{0.0};

    uint64_t submit_ts_ns{0};
    uint64_t ack_ts_ns{0};
    uint64_t last_update_ns{0};
};

struct ArbExecutionRecord {
    uint64_t plan_id{0};
    ArbExecutorState state{ArbExecutorState::Created};
    ArbExecutionPlan plan;

    ArbLegRuntime buy_leg;
    ArbLegRuntime sell_leg;

    double realized_gross_pnl{0.0};
    double filled_imbalance{0.0};

    double buy_filled_qty{0.0};
    double sell_filled_qty{0.0};
    double buy_vwap{0.0};
    double sell_vwap{0.0};

    uint64_t create_ts_ns{0};
    uint64_t last_update_ns{0};

    std::optional<EmergencyHedgeRequest> emergency_hedge;
};

struct ArbExecutorConfig {
    double max_leg_imbalance_qty{0.000001};
    uint64_t ack_timeout_ns{1'000'000'000ULL};
    uint64_t fill_timeout_ns{5'000'000'000ULL};
    bool auto_hedge_on_reject{true};
    bool auto_hedge_on_timeout{true};
};

struct ArbExecutionSnapshot {
    uint64_t plan_id{0};
    ArbExecutorState state{ArbExecutorState::Created};

    double buy_filled_qty{0.0};
    double sell_filled_qty{0.0};
    double buy_vwap{0.0};
    double sell_vwap{0.0};
    double imbalance{0.0};
    double realized_gross_pnl{0.0};

    bool hedge_required{false};
    std::string hedge_reason;
};

struct ArbExecutorMetrics {
    std::atomic<uint64_t> plans_submitted{0};
    std::atomic<uint64_t> plans_completed{0};
    std::atomic<uint64_t> reports_processed{0};
    std::atomic<uint64_t> acks{0};
    std::atomic<uint64_t> fills{0};
    std::atomic<uint64_t> partial_fills{0};
    std::atomic<uint64_t> rejects{0};
    std::atomic<uint64_t> cancels{0};
    std::atomic<uint64_t> expiries{0};
    std::atomic<uint64_t> imbalances{0};
    std::atomic<uint64_t> emergency_hedges{0};
    std::atomic<uint64_t> unknown_reports{0};
};

class ArbitrageExecutor {
public:
    explicit ArbitrageExecutor(ArbExecutorConfig config = {})
        : config_(config) {}

    std::vector<ArbLegSubmission> submit_plan(const ArbExecutionPlan& plan) {
        std::vector<ArbLegSubmission> out;

        if (plan.state != ExecutionPlanState::Ready) {
            return out;
        }

        const uint64_t now = arb_now_ns();
        const uint64_t plan_id = next_plan_id_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t buy_leg_id = next_leg_id_.fetch_add(1, std::memory_order_relaxed);
        const uint64_t sell_leg_id = next_leg_id_.fetch_add(1, std::memory_order_relaxed);

        ArbExecutionRecord record;
        record.plan_id = plan_id;
        record.plan = plan;
        record.state = ArbExecutorState::Submitted;
        record.create_ts_ns = now;
        record.last_update_ns = now;

        record.buy_leg = make_runtime_leg(buy_leg_id, plan.buy_leg, ArbLegSide::Buy, now);
        record.sell_leg = make_runtime_leg(sell_leg_id, plan.sell_leg, ArbLegSide::Sell, now);

        sync_compat_fields(record);

        records_[plan_id] = record;
        leg_to_plan_[buy_leg_id] = plan_id;
        leg_to_plan_[sell_leg_id] = plan_id;

        out.push_back(make_submission(plan_id, record.buy_leg));
        out.push_back(make_submission(plan_id, record.sell_leg));

        metrics_.plans_submitted.fetch_add(1, std::memory_order_relaxed);
        return out;
    }

    bool on_report(ArbExecutionReport report) {
        if (report.receive_ts_ns == 0) {
            report.receive_ts_ns = arb_now_ns();
        }

        auto record_it = records_.find(report.plan_id);

        if (record_it == records_.end()) {
            metrics_.unknown_reports.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto& record = record_it->second;
        ArbLegRuntime* leg = nullptr;

        if (report.leg_id != 0) {
            if (record.buy_leg.leg_id == report.leg_id) {
                leg = &record.buy_leg;
            } else if (record.sell_leg.leg_id == report.leg_id) {
                leg = &record.sell_leg;
            }
        } else {
            leg = report.is_buy_leg ? &record.buy_leg : &record.sell_leg;
        }

        if (!leg) {
            metrics_.unknown_reports.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        metrics_.reports_processed.fetch_add(1, std::memory_order_relaxed);

        apply_report(record, *leg, report);
        recompute_record(record);
        sync_compat_fields(record);

        return true;
    }

    std::vector<EmergencyHedgeRequest> check_timeouts(uint64_t now_ns = arb_now_ns()) {
        std::vector<EmergencyHedgeRequest> hedges;

        for (auto& [_, record] : records_) {
            check_leg_timeout(record, record.buy_leg, now_ns);
            check_leg_timeout(record, record.sell_leg, now_ns);
            recompute_record(record);
            sync_compat_fields(record);

            if (record.emergency_hedge.has_value()) {
                hedges.push_back(*record.emergency_hedge);
            }
        }

        return hedges;
    }

    std::optional<ArbExecutionRecord> record(uint64_t plan_id) const {
        auto it = records_.find(plan_id);
        if (it == records_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<ArbExecutionSnapshot> snapshot(uint64_t plan_id) const {
        auto it = records_.find(plan_id);
        if (it == records_.end()) return std::nullopt;

        const auto& r = it->second;

        ArbExecutionSnapshot s;
        s.plan_id = r.plan_id;
        s.state = r.state;
        s.buy_filled_qty = r.buy_leg.filled_qty;
        s.sell_filled_qty = r.sell_leg.filled_qty;
        s.buy_vwap = r.buy_leg.vwap;
        s.sell_vwap = r.sell_leg.vwap;
        s.imbalance = r.filled_imbalance;
        s.realized_gross_pnl = r.realized_gross_pnl;
        s.hedge_required = r.emergency_hedge.has_value();

        if (r.emergency_hedge) {
            s.hedge_reason = r.emergency_hedge->reason;
        }

        return s;
    }

    const ArbExecutorMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    static ArbLegRuntime make_runtime_leg(
        uint64_t leg_id,
        const ArbExecutionLeg& source,
        ArbLegSide side,
        uint64_t now
    ) {
        ArbLegRuntime leg;
        leg.leg_id = leg_id;
        leg.venue = source.venue;
        leg.symbol = source.symbol;
        leg.side = side;
        leg.state = ArbLegState::Submitted;
        leg.target_qty = source.quantity;
        leg.limit_price = source.price;
        leg.submit_ts_ns = now;
        leg.last_update_ns = now;
        return leg;
    }

    static ArbLegSubmission make_submission(
        uint64_t plan_id,
        const ArbLegRuntime& leg
    ) {
        return {
            plan_id,
            leg.leg_id,
            leg.venue,
            leg.symbol,
            leg.side,
            leg.target_qty,
            leg.limit_price,
            leg.target_qty * leg.limit_price,
            leg.submit_ts_ns
        };
    }

    void apply_report(
        ArbExecutionRecord& record,
        ArbLegRuntime& leg,
        const ArbExecutionReport& report
    ) {
        leg.last_update_ns = report.receive_ts_ns;
        record.last_update_ns = report.receive_ts_ns;

        switch (report.type) {
            case ArbExecReportType::Ack:
                if (leg.state == ArbLegState::Submitted ||
                    leg.state == ArbLegState::Created) {
                    leg.state = ArbLegState::Acknowledged;
                    leg.ack_ts_ns = report.receive_ts_ns;
                    metrics_.acks.fetch_add(1, std::memory_order_relaxed);
                }
                break;

            case ArbExecReportType::PartialFill:
                apply_fill(leg, report.fill_qty, report.fill_price);
                if (leg.state != ArbLegState::Filled) {
                    leg.state = ArbLegState::PartiallyFilled;
                }
                metrics_.partial_fills.fetch_add(1, std::memory_order_relaxed);
                break;

            case ArbExecReportType::Fill:
                apply_fill(leg, report.fill_qty, report.fill_price);
                leg.state = ArbLegState::Filled;
                metrics_.fills.fetch_add(1, std::memory_order_relaxed);
                break;

            case ArbExecReportType::Reject:
                leg.state = ArbLegState::Rejected;
                metrics_.rejects.fetch_add(1, std::memory_order_relaxed);

                if (config_.auto_hedge_on_reject) {
                    maybe_create_hedge(record, "Leg rejected");
                }
                break;

            case ArbExecReportType::CancelAck:
                leg.state = ArbLegState::Cancelled;
                metrics_.cancels.fetch_add(1, std::memory_order_relaxed);
                maybe_create_hedge(record, "Leg cancelled");
                break;

            case ArbExecReportType::Expire:
                leg.state = ArbLegState::Expired;
                metrics_.expiries.fetch_add(1, std::memory_order_relaxed);

                if (config_.auto_hedge_on_timeout) {
                    maybe_create_hedge(record, "Leg expired");
                }
                break;
        }
    }

    static void apply_fill(
        ArbLegRuntime& leg,
        double fill_qty,
        double fill_price
    ) {
        if (fill_qty <= 0.0 || fill_price <= 0.0) return;

        const double remaining = std::max(0.0, leg.target_qty - leg.filled_qty);
        const double accepted_qty = std::min(fill_qty, remaining);

        if (accepted_qty <= 0.0) return;

        const double old_notional = leg.vwap * leg.filled_qty;
        const double new_notional = accepted_qty * fill_price;

        leg.filled_qty += accepted_qty;
        leg.vwap = (old_notional + new_notional) / leg.filled_qty;

        if (std::abs(leg.filled_qty - leg.target_qty) <= 1e-12) {
            leg.state = ArbLegState::Filled;
        }
    }

    void recompute_record(ArbExecutionRecord& record) {
        record.filled_imbalance =
            std::abs(record.buy_leg.filled_qty - record.sell_leg.filled_qty);

        record.realized_gross_pnl =
            (record.sell_leg.vwap * record.sell_leg.filled_qty) -
            (record.buy_leg.vwap * record.buy_leg.filled_qty);

        const bool buy_filled = record.buy_leg.state == ArbLegState::Filled;
        const bool sell_filled = record.sell_leg.state == ArbLegState::Filled;

        const bool any_rejected =
            record.buy_leg.state == ArbLegState::Rejected ||
            record.sell_leg.state == ArbLegState::Rejected;

        const bool any_cancelled =
            record.buy_leg.state == ArbLegState::Cancelled ||
            record.sell_leg.state == ArbLegState::Cancelled;

        const bool any_expired =
            record.buy_leg.state == ArbLegState::Expired ||
            record.sell_leg.state == ArbLegState::Expired;

        if (buy_filled && sell_filled) {
            if (record.filled_imbalance <= config_.max_leg_imbalance_qty) {
                if (record.state != ArbExecutorState::Completed) {
                    record.state = ArbExecutorState::Completed;
                    metrics_.plans_completed.fetch_add(1, std::memory_order_relaxed);
                }
                return;
            }

            metrics_.imbalances.fetch_add(1, std::memory_order_relaxed);
            maybe_create_hedge(record, "Final fill imbalance");
            return;
        }

        if (record.emergency_hedge.has_value()) {
            record.state = ArbExecutorState::EmergencyHedgeRequired;
            return;
        }

        if (any_rejected) {
            record.state = ArbExecutorState::Rejected;
            return;
        }

        if (any_cancelled) {
            record.state = ArbExecutorState::Cancelled;
            return;
        }

        if (any_expired) {
            record.state = ArbExecutorState::Expired;
            return;
        }

        if (record.buy_leg.filled_qty > 0.0 || record.sell_leg.filled_qty > 0.0) {
            record.state = ArbExecutorState::Working;
        } else {
            record.state = ArbExecutorState::Submitted;
        }

        if (record.filled_imbalance > config_.max_leg_imbalance_qty) {
            metrics_.imbalances.fetch_add(1, std::memory_order_relaxed);
            maybe_create_hedge(record, "Live fill imbalance");
        }
    }

    void check_leg_timeout(
        ArbExecutionRecord& record,
        ArbLegRuntime& leg,
        uint64_t now_ns
    ) {
        if (is_terminal(leg.state)) return;

        if (leg.state == ArbLegState::Submitted &&
            now_ns > leg.submit_ts_ns &&
            now_ns - leg.submit_ts_ns > config_.ack_timeout_ns) {
            leg.state = ArbLegState::Expired;
            metrics_.expiries.fetch_add(1, std::memory_order_relaxed);

            if (config_.auto_hedge_on_timeout) {
                maybe_create_hedge(record, "Ack timeout");
            }

            return;
        }

        if ((leg.state == ArbLegState::Acknowledged ||
             leg.state == ArbLegState::PartiallyFilled) &&
            now_ns > leg.last_update_ns &&
            now_ns - leg.last_update_ns > config_.fill_timeout_ns) {
            leg.state = ArbLegState::Expired;
            metrics_.expiries.fetch_add(1, std::memory_order_relaxed);

            if (config_.auto_hedge_on_timeout) {
                maybe_create_hedge(record, "Fill timeout");
            }
        }
    }

    static bool is_terminal(ArbLegState state) noexcept {
        return state == ArbLegState::Filled ||
               state == ArbLegState::Rejected ||
               state == ArbLegState::Cancelled ||
               state == ArbLegState::Expired;
    }

    static void sync_compat_fields(ArbExecutionRecord& record) {
        record.buy_filled_qty = record.buy_leg.filled_qty;
        record.sell_filled_qty = record.sell_leg.filled_qty;
        record.buy_vwap = record.buy_leg.vwap;
        record.sell_vwap = record.sell_leg.vwap;
    }

    void maybe_create_hedge(
        ArbExecutionRecord& record,
        std::string reason
    ) {
        if (record.emergency_hedge.has_value()) return;

        const double diff = record.buy_leg.filled_qty - record.sell_leg.filled_qty;

        if (std::abs(diff) <= config_.max_leg_imbalance_qty) return;

        EmergencyHedgeRequest hedge;
        hedge.plan_id = record.plan_id;
        hedge.symbol = record.plan.symbol;
        hedge.quantity = std::abs(diff);
        hedge.reason = std::move(reason);

        if (diff > 0.0) {
            hedge.side = ArbLegSide::Sell;
            hedge.preferred_venue = record.sell_leg.venue;
            hedge.reference_price = record.sell_leg.limit_price;
        } else {
            hedge.side = ArbLegSide::Buy;
            hedge.preferred_venue = record.buy_leg.venue;
            hedge.reference_price = record.buy_leg.limit_price;
        }

        record.emergency_hedge = hedge;
        record.state = ArbExecutorState::EmergencyHedgeRequired;

        metrics_.emergency_hedges.fetch_add(1, std::memory_order_relaxed);
    }

private:
    ArbExecutorConfig config_;

    std::atomic<uint64_t> next_plan_id_{1};
    std::atomic<uint64_t> next_leg_id_{1};

    std::unordered_map<uint64_t, ArbExecutionRecord> records_;
    std::unordered_map<uint64_t, uint64_t> leg_to_plan_;

    ArbExecutorMetrics metrics_{};
};

} // namespace hft::arbitrage