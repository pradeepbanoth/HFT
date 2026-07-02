#pragma once

#include "child_order_manager.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft::execution {

enum class ExecutionGrade : uint8_t {
    Excellent,
    Good,
    Average,
    Poor,
    Failed
};

struct ExecutionQualityConfig {
    double epsilon{1e-9};
    double excellent_slippage_bps{2.0};
    double good_slippage_bps{8.0};
    double average_slippage_bps{20.0};
    double min_good_fill_ratio{0.95};
};

struct ExecutionQualityReport {
    uint64_t parent_id{0};
    std::string symbol;
    ChildOrderSide side{ChildOrderSide::Buy};
    ParentOrderState parent_state{ParentOrderState::Created};
    ExecutionGrade grade{ExecutionGrade::Failed};

    double arrival_price{0.0};
    double decision_price{0.0};
    double average_fill_price{0.0};

    double ordered_qty{0.0};
    double filled_qty{0.0};
    double remaining_qty{0.0};

    double fill_ratio{0.0};
    double reject_ratio{0.0};
    double cancel_ratio{0.0};

    double slippage_bps{0.0};
    double decision_slippage_bps{0.0};
    double implementation_shortfall{0.0};
    double notional{0.0};

    uint64_t child_count{0};
    uint64_t filled_children{0};
    uint64_t rejected_children{0};
    uint64_t cancelled_children{0};
    uint64_t expired_children{0};
    uint64_t working_children{0};

    uint64_t first_submit_ns{0};
    uint64_t first_ack_ns{0};
    uint64_t last_fill_ns{0};
    uint64_t completion_latency_ns{0};
};

struct VenueTcaReport {
    std::string venue;
    std::string symbol;

    uint64_t parent_orders{0};
    uint64_t child_orders{0};
    uint64_t filled_children{0};
    uint64_t rejected_children{0};
    uint64_t cancelled_children{0};
    uint64_t expired_children{0};

    double ordered_qty{0.0};
    double filled_qty{0.0};
    double notional{0.0};

    double fill_ratio{0.0};
    double reject_ratio{0.0};
    double cancel_ratio{0.0};
    double average_fill_price{0.0};
};

struct GlobalTcaSummary {
    uint64_t parent_reports{0};
    uint64_t venue_count{0};

    double total_ordered_qty{0.0};
    double total_filled_qty{0.0};
    double total_notional{0.0};

    double global_fill_ratio{0.0};
    double average_slippage_bps{0.0};
    double average_shortfall{0.0};
};

struct ExecutionQualityMetrics {
    std::atomic<uint64_t> reports_generated{0};
    std::atomic<uint64_t> reports_updated{0};
    std::atomic<uint64_t> venue_updates{0};
};

class ExecutionQualityAnalytics {
public:
    explicit ExecutionQualityAnalytics(ExecutionQualityConfig config = {})
        : config_(config) {}

    std::optional<ExecutionQualityReport> parent_report(
        const ChildOrderManager& manager,
        uint64_t parent_id,
        double arrival_price,
        double decision_price = 0.0
    ) {
        auto parent = manager.parent(parent_id);
        if (!parent.has_value()) return std::nullopt;

        ExecutionQualityReport report;
        report.parent_id = parent->parent_id;
        report.symbol = parent->symbol;
        report.side = parent->side;
        report.parent_state = parent->state;

        report.arrival_price = arrival_price;
        report.decision_price = decision_price > config_.epsilon ? decision_price : arrival_price;
        report.average_fill_price = parent->average_price;

        report.ordered_qty = parent->quantity;
        report.filled_qty = parent->filled_quantity;
        report.remaining_qty = parent->remaining_quantity;

        report.fill_ratio =
            parent->quantity > config_.epsilon
                ? parent->filled_quantity / parent->quantity
                : 0.0;

        report.notional = parent->filled_quantity * parent->average_price;

        report.child_count = parent->child_ids.size();

        for (const auto child_id : parent->child_ids) {
            auto child = manager.child(child_id);
            if (!child.has_value()) continue;

            classify_child(*child, report);
        }

        if (report.child_count > 0) {
            report.reject_ratio =
                static_cast<double>(report.rejected_children) /
                static_cast<double>(report.child_count);

            report.cancel_ratio =
                static_cast<double>(report.cancelled_children) /
                static_cast<double>(report.child_count);
        }

        report.slippage_bps = calc_slippage_bps(
            report.side,
            report.average_fill_price,
            report.arrival_price
        );

        report.decision_slippage_bps = calc_slippage_bps(
            report.side,
            report.average_fill_price,
            report.decision_price
        );

        report.implementation_shortfall =
            calc_shortfall(report.side, report.average_fill_price, report.arrival_price, report.filled_qty);

        if (report.first_submit_ns > 0 &&
            report.last_fill_ns > 0 &&
            report.last_fill_ns >= report.first_submit_ns) {
            report.completion_latency_ns = report.last_fill_ns - report.first_submit_ns;
        }

        report.grade = grade(report);

        const bool existed = parent_reports_.find(parent_id) != parent_reports_.end();
        parent_reports_[parent_id] = report;

        rebuild_venue_stats(manager);

        if (existed) {
            metrics_.reports_updated.fetch_add(1, std::memory_order_relaxed);
        } else {
            metrics_.reports_generated.fetch_add(1, std::memory_order_relaxed);
        }

        return report;
    }

    std::optional<ExecutionQualityReport> cached_parent_report(uint64_t parent_id) const {
        auto it = parent_reports_.find(parent_id);
        if (it == parent_reports_.end()) return std::nullopt;
        return it->second;
    }

    std::vector<ExecutionQualityReport> parent_reports() const {
        std::vector<ExecutionQualityReport> out;
        out.reserve(parent_reports_.size());

        for (const auto& [_, report] : parent_reports_) {
            out.push_back(report);
        }

        std::sort(
            out.begin(),
            out.end(),
            [](const ExecutionQualityReport& a, const ExecutionQualityReport& b) {
                return a.parent_id < b.parent_id;
            }
        );

        return out;
    }

    std::vector<VenueTcaReport> venue_reports() const {
        std::vector<VenueTcaReport> reports;
        reports.reserve(venues_.size());

        for (const auto& [_, stats] : venues_) {
            reports.push_back(to_venue_report(stats));
        }

        std::sort(
            reports.begin(),
            reports.end(),
            [](const VenueTcaReport& a, const VenueTcaReport& b) {
                if (std::abs(a.fill_ratio - b.fill_ratio) > 1e-12) {
                    return a.fill_ratio > b.fill_ratio;
                }
                return a.reject_ratio < b.reject_ratio;
            }
        );

        return reports;
    }

    std::optional<VenueTcaReport> venue_report(
        const std::string& venue,
        const std::string& symbol
    ) const {
        auto it = venues_.find(key(venue, symbol));
        if (it == venues_.end()) return std::nullopt;
        return to_venue_report(it->second);
    }

    std::optional<VenueTcaReport> best_venue() const {
        auto reports = venue_reports();
        if (reports.empty()) return std::nullopt;
        return reports.front();
    }

    std::optional<VenueTcaReport> worst_venue() const {
        auto reports = venue_reports();
        if (reports.empty()) return std::nullopt;
        return reports.back();
    }

    GlobalTcaSummary summary() const {
        GlobalTcaSummary out;
        out.parent_reports = parent_reports_.size();
        out.venue_count = venues_.size();

        double total_slippage = 0.0;
        double total_shortfall = 0.0;

        for (const auto& [_, report] : parent_reports_) {
            out.total_ordered_qty += report.ordered_qty;
            out.total_filled_qty += report.filled_qty;
            out.total_notional += report.notional;
            total_slippage += report.slippage_bps;
            total_shortfall += report.implementation_shortfall;
        }

        out.global_fill_ratio =
            out.total_ordered_qty > config_.epsilon
                ? out.total_filled_qty / out.total_ordered_qty
                : 0.0;

        if (!parent_reports_.empty()) {
            out.average_slippage_bps =
                total_slippage / static_cast<double>(parent_reports_.size());

            out.average_shortfall =
                total_shortfall / static_cast<double>(parent_reports_.size());
        }

        return out;
    }

    const ExecutionQualityMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    struct VenueStats {
        std::string venue;
        std::string symbol;

        std::unordered_set<uint64_t> parent_ids;

        uint64_t child_orders{0};
        uint64_t filled_children{0};
        uint64_t rejected_children{0};
        uint64_t cancelled_children{0};
        uint64_t expired_children{0};

        double ordered_qty{0.0};
        double filled_qty{0.0};
        double notional{0.0};
    };

    static std::string key(const std::string& venue, const std::string& symbol) {
        return venue + ":" + symbol;
    }

    static double calc_slippage_bps(
        ChildOrderSide side,
        double fill_price,
        double reference_price
    ) noexcept {
        if (fill_price <= 0.0 || reference_price <= 0.0) return 0.0;

        const double diff =
            side == ChildOrderSide::Buy
                ? fill_price - reference_price
                : reference_price - fill_price;

        return diff / reference_price * 10000.0;
    }

    static double calc_shortfall(
        ChildOrderSide side,
        double fill_price,
        double reference_price,
        double qty
    ) noexcept {
        if (fill_price <= 0.0 || reference_price <= 0.0 || qty <= 0.0) return 0.0;

        const double diff =
            side == ChildOrderSide::Buy
                ? fill_price - reference_price
                : reference_price - fill_price;

        return diff * qty;
    }

    void classify_child(const ChildOrder& child, ExecutionQualityReport& report) const {
        if (child.state == ChildOrderState::Filled) ++report.filled_children;
        else if (child.state == ChildOrderState::Rejected) ++report.rejected_children;
        else if (child.state == ChildOrderState::Cancelled) ++report.cancelled_children;
        else if (child.state == ChildOrderState::Expired) ++report.expired_children;
        else ++report.working_children;

        if (child.submit_time_ns > 0 &&
            (report.first_submit_ns == 0 || child.submit_time_ns < report.first_submit_ns)) {
            report.first_submit_ns = child.submit_time_ns;
        }

        if (child.ack_time_ns > 0 &&
            (report.first_ack_ns == 0 || child.ack_time_ns < report.first_ack_ns)) {
            report.first_ack_ns = child.ack_time_ns;
        }

        if (child.filled_quantity > config_.epsilon && child.last_update_ns > report.last_fill_ns) {
            report.last_fill_ns = child.last_update_ns;
        }
    }

    ExecutionGrade grade(const ExecutionQualityReport& report) const noexcept {
        if (report.fill_ratio <= config_.epsilon) return ExecutionGrade::Failed;

        const double abs_slippage = std::abs(report.slippage_bps);

        if (report.fill_ratio >= 0.999 &&
            abs_slippage <= config_.excellent_slippage_bps &&
            report.reject_ratio <= 0.0) {
            return ExecutionGrade::Excellent;
        }

        if (report.fill_ratio >= config_.min_good_fill_ratio &&
            abs_slippage <= config_.good_slippage_bps) {
            return ExecutionGrade::Good;
        }

        if (report.fill_ratio >= 0.50 &&
            abs_slippage <= config_.average_slippage_bps) {
            return ExecutionGrade::Average;
        }

        return ExecutionGrade::Poor;
    }

    void rebuild_venue_stats(const ChildOrderManager& manager) {
        venues_.clear();

        for (const auto& [parent_id, report] : parent_reports_) {
            auto parent = manager.parent(parent_id);
            if (!parent.has_value()) continue;

            for (const auto child_id : parent->child_ids) {
                auto child = manager.child(child_id);
                if (!child.has_value()) continue;

                update_venue_stats(parent_id, *child);
            }
        }
    }

    void update_venue_stats(uint64_t parent_id, const ChildOrder& child) {
        auto& stats = venues_[key(child.venue, child.symbol)];

        stats.venue = child.venue;
        stats.symbol = child.symbol;
        stats.parent_ids.insert(parent_id);

        stats.child_orders += 1;
        stats.ordered_qty += child.quantity;
        stats.filled_qty += child.filled_quantity;
        stats.notional += child.filled_quantity * child.average_price;

        if (child.state == ChildOrderState::Filled) ++stats.filled_children;
        if (child.state == ChildOrderState::Rejected) ++stats.rejected_children;
        if (child.state == ChildOrderState::Cancelled) ++stats.cancelled_children;
        if (child.state == ChildOrderState::Expired) ++stats.expired_children;

        metrics_.venue_updates.fetch_add(1, std::memory_order_relaxed);
    }

    VenueTcaReport to_venue_report(const VenueStats& stats) const {
        VenueTcaReport report;
        report.venue = stats.venue;
        report.symbol = stats.symbol;

        report.parent_orders = stats.parent_ids.size();
        report.child_orders = stats.child_orders;
        report.filled_children = stats.filled_children;
        report.rejected_children = stats.rejected_children;
        report.cancelled_children = stats.cancelled_children;
        report.expired_children = stats.expired_children;

        report.ordered_qty = stats.ordered_qty;
        report.filled_qty = stats.filled_qty;
        report.notional = stats.notional;

        report.fill_ratio =
            stats.ordered_qty > config_.epsilon
                ? stats.filled_qty / stats.ordered_qty
                : 0.0;

        report.reject_ratio =
            stats.child_orders > 0
                ? static_cast<double>(stats.rejected_children) /
                      static_cast<double>(stats.child_orders)
                : 0.0;

        report.cancel_ratio =
            stats.child_orders > 0
                ? static_cast<double>(stats.cancelled_children) /
                      static_cast<double>(stats.child_orders)
                : 0.0;

        report.average_fill_price =
            stats.filled_qty > config_.epsilon
                ? stats.notional / stats.filled_qty
                : 0.0;

        return report;
    }

private:
    ExecutionQualityConfig config_;

    std::unordered_map<uint64_t, ExecutionQualityReport> parent_reports_;
    std::unordered_map<std::string, VenueStats> venues_;

    ExecutionQualityMetrics metrics_{};
};

} // namespace hft::execution