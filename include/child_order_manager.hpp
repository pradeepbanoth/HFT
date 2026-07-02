#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft::execution {

enum class ParentOrderState : uint8_t {
    Created,
    Working,
    PartiallyFilled,
    Filled,
    CancelPending,
    Cancelled,
    Rejected,
    Expired
};

enum class ChildOrderState : uint8_t {
    Created,
    Submitted,
    Acknowledged,
    PartiallyFilled,
    Filled,
    CancelPending,
    Cancelled,
    ReplacePending,
    Replaced,
    Rejected,
    Expired
};

enum class ChildOrderSide : uint8_t {
    Buy,
    Sell
};

enum class ExecutionReportType : uint8_t {
    Ack,
    PartialFill,
    Fill,
    Reject,
    CancelAck,
    ReplaceAck,
    Expired
};

struct ChildOrderRoute {
    std::string venue;
    double qty{0.0};
    double price{0.0};
};

struct ParentCreateRequest {
    std::string symbol;
    ChildOrderSide side{ChildOrderSide::Buy};
    double quantity{0.0};
    uint64_t timestamp_ns{0};
};

struct ExecutionReport {
    ExecutionReportType type{ExecutionReportType::Ack};

    uint64_t child_id{0};
    std::string client_order_id;
    std::string venue_order_id;

    double fill_qty{0.0};
    double fill_price{0.0};

    double new_qty{0.0};
    double new_price{0.0};

    std::string reason;
    uint64_t timestamp_ns{0};
};

struct ParentOrder {
    uint64_t parent_id{0};
    std::string symbol;
    ChildOrderSide side{ChildOrderSide::Buy};

    double quantity{0.0};
    double filled_quantity{0.0};
    double remaining_quantity{0.0};
    double average_price{0.0};

    ParentOrderState state{ParentOrderState::Created};

    uint64_t create_time_ns{0};
    uint64_t last_update_ns{0};

    std::vector<uint64_t> child_ids;
};

struct ChildOrder {
    uint64_t child_id{0};
    uint64_t parent_id{0};

    std::string venue;
    std::string symbol;
    ChildOrderSide side{ChildOrderSide::Buy};

    double quantity{0.0};
    double filled_quantity{0.0};
    double remaining_quantity{0.0};
    double average_price{0.0};
    double limit_price{0.0};

    ChildOrderState state{ChildOrderState::Created};

    uint64_t create_time_ns{0};
    uint64_t submit_time_ns{0};
    uint64_t ack_time_ns{0};
    uint64_t last_update_ns{0};

    uint32_t retry_count{0};

    std::string client_order_id;
    std::string venue_order_id;
    std::string reject_reason;
};

struct ChildOrderMetrics {
    std::atomic<uint64_t> parents_created{0};
    std::atomic<uint64_t> children_created{0};
    std::atomic<uint64_t> children_submitted{0};
    std::atomic<uint64_t> children_acknowledged{0};

    std::atomic<uint64_t> partial_fills{0};
    std::atomic<uint64_t> full_fills{0};
    std::atomic<uint64_t> rejects{0};
    std::atomic<uint64_t> cancels{0};
    std::atomic<uint64_t> replaces{0};
    std::atomic<uint64_t> expiries{0};
    std::atomic<uint64_t> retries{0};

    std::atomic<uint64_t> ack_latency_total_ns{0};
    std::atomic<uint64_t> ack_latency_count{0};
    std::atomic<uint64_t> fill_latency_total_ns{0};
    std::atomic<uint64_t> fill_latency_count{0};
};

struct ChildOrderMetricsSnapshot {
    uint64_t parents_created{0};
    uint64_t children_created{0};
    uint64_t children_submitted{0};
    uint64_t children_acknowledged{0};

    uint64_t partial_fills{0};
    uint64_t full_fills{0};
    uint64_t rejects{0};
    uint64_t cancels{0};
    uint64_t replaces{0};
    uint64_t expiries{0};
    uint64_t retries{0};

    double avg_ack_latency_us{0.0};
    double avg_fill_latency_us{0.0};
};

class ChildOrderManager {
public:
    uint64_t create_parent(const ParentCreateRequest& request) {
        if (request.symbol.empty() || request.quantity <= 0.0) return 0;

        const auto parent_id = next_parent_id_.fetch_add(1, std::memory_order_relaxed);

        ParentOrder parent;
        parent.parent_id = parent_id;
        parent.symbol = request.symbol;
        parent.side = request.side;
        parent.quantity = request.quantity;
        parent.remaining_quantity = request.quantity;
        parent.create_time_ns = request.timestamp_ns;
        parent.last_update_ns = request.timestamp_ns;

        parents_[parent_id] = std::move(parent);
        metrics_.parents_created.fetch_add(1, std::memory_order_relaxed);

        return parent_id;
    }

    std::vector<uint64_t> create_children(
        uint64_t parent_id,
        const std::vector<ChildOrderRoute>& routes,
        uint64_t timestamp_ns
    ) {
        std::vector<uint64_t> ids;

        auto parent_it = parents_.find(parent_id);
        if (parent_it == parents_.end()) return ids;

        auto& parent = parent_it->second;

        for (const auto& route : routes) {
            if (route.venue.empty() || route.qty <= 0.0 || route.price <= 0.0) continue;

            const auto child_id = next_child_id_.fetch_add(1, std::memory_order_relaxed);

            ChildOrder child;
            child.child_id = child_id;
            child.parent_id = parent_id;
            child.venue = route.venue;
            child.symbol = parent.symbol;
            child.side = parent.side;
            child.quantity = route.qty;
            child.remaining_quantity = route.qty;
            child.limit_price = route.price;
            child.create_time_ns = timestamp_ns;
            child.last_update_ns = timestamp_ns;
            child.client_order_id = make_client_order_id(parent_id, child_id);

            client_to_child_[child.client_order_id] = child_id;

            children_[child_id] = std::move(child);
            parent.child_ids.push_back(child_id);
            ids.push_back(child_id);

            metrics_.children_created.fetch_add(1, std::memory_order_relaxed);
        }

        if (!ids.empty()) {
            parent.state = ParentOrderState::Working;
            parent.last_update_ns = timestamp_ns;
        }

        return ids;
    }

    bool mark_submitted(uint64_t child_id, uint64_t timestamp_ns) {
        auto* child = find_child(child_id);
        if (!child) return false;

        if (child->state != ChildOrderState::Created &&
            child->state != ChildOrderState::Replaced) {
            return false;
        }

        child->state = ChildOrderState::Submitted;
        child->submit_time_ns = timestamp_ns;
        child->last_update_ns = timestamp_ns;

        metrics_.children_submitted.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool acknowledge(
        uint64_t child_id,
        std::string venue_order_id,
        uint64_t timestamp_ns
    ) {
        auto* child = find_child(child_id);
        if (!child) return false;

        if (child->state != ChildOrderState::Submitted &&
            child->state != ChildOrderState::Created &&
            child->state != ChildOrderState::Replaced) {
            return false;
        }

        child->venue_order_id = std::move(venue_order_id);
        if (!child->venue_order_id.empty()) {
            venue_to_child_[child->venue_order_id] = child_id;
        }

        child->state = ChildOrderState::Acknowledged;
        child->ack_time_ns = timestamp_ns;
        child->last_update_ns = timestamp_ns;

        if (child->submit_time_ns > 0 && timestamp_ns >= child->submit_time_ns) {
            metrics_.ack_latency_total_ns.fetch_add(
                timestamp_ns - child->submit_time_ns,
                std::memory_order_relaxed
            );
            metrics_.ack_latency_count.fetch_add(1, std::memory_order_relaxed);
        }

        metrics_.children_acknowledged.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool apply_fill(
        uint64_t child_id,
        double fill_qty,
        double fill_price,
        uint64_t timestamp_ns
    ) {
        auto* child = find_child(child_id);
        if (!child || fill_qty <= 0.0 || fill_price <= 0.0) return false;
        if (is_terminal(child->state)) return false;

        const double accepted_qty = std::min(fill_qty, child->remaining_quantity);
        if (accepted_qty <= 0.0) return false;

        child->average_price = combine_vwap(
            child->average_price,
            child->filled_quantity,
            fill_price,
            accepted_qty
        );

        child->filled_quantity += accepted_qty;
        child->remaining_quantity = std::max(0.0, child->quantity - child->filled_quantity);
        child->last_update_ns = timestamp_ns;

        if (child->submit_time_ns > 0 && timestamp_ns >= child->submit_time_ns) {
            metrics_.fill_latency_total_ns.fetch_add(
                timestamp_ns - child->submit_time_ns,
                std::memory_order_relaxed
            );
            metrics_.fill_latency_count.fetch_add(1, std::memory_order_relaxed);
        }

        if (child->remaining_quantity <= epsilon_) {
            child->state = ChildOrderState::Filled;
            metrics_.full_fills.fetch_add(1, std::memory_order_relaxed);
        } else {
            child->state = ChildOrderState::PartiallyFilled;
            metrics_.partial_fills.fetch_add(1, std::memory_order_relaxed);
        }

        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool reject(uint64_t child_id, std::string reason, uint64_t timestamp_ns) {
        auto* child = find_child(child_id);
        if (!child || is_terminal(child->state)) return false;

        child->state = ChildOrderState::Rejected;
        child->reject_reason = std::move(reason);
        child->last_update_ns = timestamp_ns;

        metrics_.rejects.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool mark_cancel_pending(uint64_t child_id, uint64_t timestamp_ns) {
        auto* child = find_child(child_id);
        if (!child || is_terminal(child->state)) return false;

        child->state = ChildOrderState::CancelPending;
        child->last_update_ns = timestamp_ns;

        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool cancel_ack(uint64_t child_id, uint64_t timestamp_ns) {
        auto* child = find_child(child_id);
        if (!child) return false;

        if (child->state != ChildOrderState::CancelPending &&
            child->state != ChildOrderState::Acknowledged &&
            child->state != ChildOrderState::PartiallyFilled &&
            child->state != ChildOrderState::Submitted) {
            return false;
        }

        child->state = ChildOrderState::Cancelled;
        child->last_update_ns = timestamp_ns;

        metrics_.cancels.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool mark_replace_pending(uint64_t child_id, uint64_t timestamp_ns) {
    auto* child = find_child(child_id);
    if (!child || is_terminal(child->state)) return false;

    if (child->state == ChildOrderState::CancelPending) {
        return false;
    }

    child->state = ChildOrderState::ReplacePending;
        child->last_update_ns = timestamp_ns;

        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool replace_ack(
        uint64_t child_id,
        double new_qty,
        double new_price,
        uint64_t timestamp_ns
    ) {
        auto* child = find_child(child_id);
        if (!child) return false;

        if (child->state != ChildOrderState::ReplacePending) return false;
        if (new_qty <= 0.0 || new_price <= 0.0) return false;
        if (new_qty + epsilon_ < child->filled_quantity) return false;

        child->quantity = new_qty;
        child->remaining_quantity = std::max(0.0, new_qty - child->filled_quantity);
        child->limit_price = new_price;
        child->state = ChildOrderState::Replaced;
        child->last_update_ns = timestamp_ns;

        metrics_.replaces.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool expire_child(uint64_t child_id, uint64_t timestamp_ns) {
        auto* child = find_child(child_id);
        if (!child || is_terminal(child->state)) return false;

        child->state = ChildOrderState::Expired;
        child->last_update_ns = timestamp_ns;

        metrics_.expiries.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    std::vector<uint64_t> expire_stale_children(
        uint64_t now_ns,
        uint64_t timeout_ns
    ) {
        std::vector<uint64_t> expired;

        for (auto& [child_id, child] : children_) {
            if (is_terminal(child.state)) continue;
            if (child.last_update_ns == 0) continue;
            if (now_ns < child.last_update_ns) continue;

            if (now_ns - child.last_update_ns >= timeout_ns) {
                child.state = ChildOrderState::Expired;
                child.last_update_ns = now_ns;
                expired.push_back(child_id);
                metrics_.expiries.fetch_add(1, std::memory_order_relaxed);
                recompute_parent(child.parent_id, now_ns);
            }
        }

        return expired;
    }

    bool increment_retry(uint64_t child_id, uint64_t timestamp_ns) {
        auto* child = find_child(child_id);
        if (!child) return false;

        child->retry_count += 1;
        child->last_update_ns = timestamp_ns;

        metrics_.retries.fetch_add(1, std::memory_order_relaxed);
        recompute_parent(child->parent_id, timestamp_ns);
        return true;
    }

    bool on_execution_report(const ExecutionReport& report) {
        const auto child_id = resolve_child_id(report);
        if (child_id == 0) return false;

        switch (report.type) {
            case ExecutionReportType::Ack:
                return acknowledge(child_id, report.venue_order_id, report.timestamp_ns);

            case ExecutionReportType::PartialFill:
                return apply_fill(child_id, report.fill_qty, report.fill_price, report.timestamp_ns);

            case ExecutionReportType::Fill:
                return apply_fill(child_id, report.fill_qty, report.fill_price, report.timestamp_ns);

            case ExecutionReportType::Reject:
                return reject(child_id, report.reason, report.timestamp_ns);

            case ExecutionReportType::CancelAck:
                return cancel_ack(child_id, report.timestamp_ns);

            case ExecutionReportType::ReplaceAck:
                return replace_ack(child_id, report.new_qty, report.new_price, report.timestamp_ns);

            case ExecutionReportType::Expired:
                return expire_child(child_id, report.timestamp_ns);
        }

        return false;
    }

    std::optional<ParentOrder> parent(uint64_t parent_id) const {
        auto it = parents_.find(parent_id);
        if (it == parents_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<ChildOrder> child(uint64_t child_id) const {
        auto it = children_.find(child_id);
        if (it == children_.end()) return std::nullopt;
        return it->second;
    }

    std::optional<ChildOrder> child_by_client_order_id(const std::string& id) const {
        auto it = client_to_child_.find(id);
        if (it == client_to_child_.end()) return std::nullopt;
        return child(it->second);
    }

    std::optional<ChildOrder> child_by_venue_order_id(const std::string& id) const {
        auto it = venue_to_child_.find(id);
        if (it == venue_to_child_.end()) return std::nullopt;
        return child(it->second);
    }

    const ChildOrderMetrics& metrics() const noexcept {
        return metrics_;
    }

    ChildOrderMetricsSnapshot metrics_snapshot() const noexcept {
        ChildOrderMetricsSnapshot out;

        out.parents_created = metrics_.parents_created.load(std::memory_order_relaxed);
        out.children_created = metrics_.children_created.load(std::memory_order_relaxed);
        out.children_submitted = metrics_.children_submitted.load(std::memory_order_relaxed);
        out.children_acknowledged = metrics_.children_acknowledged.load(std::memory_order_relaxed);

        out.partial_fills = metrics_.partial_fills.load(std::memory_order_relaxed);
        out.full_fills = metrics_.full_fills.load(std::memory_order_relaxed);
        out.rejects = metrics_.rejects.load(std::memory_order_relaxed);
        out.cancels = metrics_.cancels.load(std::memory_order_relaxed);
        out.replaces = metrics_.replaces.load(std::memory_order_relaxed);
        out.expiries = metrics_.expiries.load(std::memory_order_relaxed);
        out.retries = metrics_.retries.load(std::memory_order_relaxed);

        const auto ack_count = metrics_.ack_latency_count.load(std::memory_order_relaxed);
        const auto ack_total = metrics_.ack_latency_total_ns.load(std::memory_order_relaxed);

        const auto fill_count = metrics_.fill_latency_count.load(std::memory_order_relaxed);
        const auto fill_total = metrics_.fill_latency_total_ns.load(std::memory_order_relaxed);

        out.avg_ack_latency_us =
            ack_count == 0 ? 0.0 : static_cast<double>(ack_total) / ack_count / 1000.0;

        out.avg_fill_latency_us =
            fill_count == 0 ? 0.0 : static_cast<double>(fill_total) / fill_count / 1000.0;

        return out;
    }

private:
    uint64_t resolve_child_id(const ExecutionReport& report) const {
        if (report.child_id != 0) return report.child_id;

        if (!report.client_order_id.empty()) {
            auto it = client_to_child_.find(report.client_order_id);
            if (it != client_to_child_.end()) return it->second;
        }

        if (!report.venue_order_id.empty()) {
            auto it = venue_to_child_.find(report.venue_order_id);
            if (it != venue_to_child_.end()) return it->second;
        }

        return 0;
    }

    static std::string make_client_order_id(uint64_t parent_id, uint64_t child_id) {
        return "COM-" + std::to_string(parent_id) + "-" + std::to_string(child_id);
    }

    static bool is_terminal(ChildOrderState state) noexcept {
        return state == ChildOrderState::Filled ||
               state == ChildOrderState::Cancelled ||
               state == ChildOrderState::Rejected ||
               state == ChildOrderState::Expired;
    }

    static double combine_vwap(
        double old_avg,
        double old_qty,
        double new_price,
        double new_qty
    ) noexcept {
        const double total_qty = old_qty + new_qty;
        if (total_qty <= 0.0) return 0.0;
        return ((old_avg * old_qty) + (new_price * new_qty)) / total_qty;
    }

    ChildOrder* find_child(uint64_t child_id) {
        auto it = children_.find(child_id);
        if (it == children_.end()) return nullptr;
        return &it->second;
    }

    void recompute_parent(uint64_t parent_id, uint64_t timestamp_ns) {
        auto parent_it = parents_.find(parent_id);
        if (parent_it == parents_.end()) return;

        auto& parent = parent_it->second;

        double filled_qty = 0.0;
        double notional = 0.0;

        bool any_active = false;
        bool any_partial = false;
        bool any_cancel_pending = false;
        bool all_terminal = !parent.child_ids.empty();

        uint64_t rejected_count = 0;
        uint64_t cancelled_count = 0;
        uint64_t expired_count = 0;

        for (const auto child_id : parent.child_ids) {
            auto child_it = children_.find(child_id);
            if (child_it == children_.end()) continue;

            const auto& child = child_it->second;

            filled_qty += child.filled_quantity;
            notional += child.filled_quantity * child.average_price;

            if (child.state == ChildOrderState::PartiallyFilled) {
                any_partial = true;
            }

            if (child.state == ChildOrderState::CancelPending) {
                any_cancel_pending = true;
            }

            if (!is_terminal(child.state)) {
                any_active = true;
                all_terminal = false;
            }

            if (child.state == ChildOrderState::Rejected) ++rejected_count;
            if (child.state == ChildOrderState::Cancelled) ++cancelled_count;
            if (child.state == ChildOrderState::Expired) ++expired_count;
        }

        parent.filled_quantity = filled_qty;
        parent.remaining_quantity = std::max(0.0, parent.quantity - filled_qty);
        parent.average_price = filled_qty > 0.0 ? notional / filled_qty : 0.0;
        parent.last_update_ns = timestamp_ns;

        if (parent.remaining_quantity <= epsilon_) {
            parent.state = ParentOrderState::Filled;
            return;
        }

        if (any_cancel_pending) {
            parent.state = ParentOrderState::CancelPending;
            return;
        }

        if (filled_qty > epsilon_ || any_partial) {
            parent.state = ParentOrderState::PartiallyFilled;
            return;
        }

        if (any_active) {
            parent.state = ParentOrderState::Working;
            return;
        }

        if (all_terminal && expired_count > 0) {
            parent.state = ParentOrderState::Expired;
            return;
        }

        if (all_terminal && rejected_count > 0) {
            parent.state = ParentOrderState::Rejected;
            return;
        }

        if (all_terminal && cancelled_count > 0) {
            parent.state = ParentOrderState::Cancelled;
            return;
        }

        parent.state = ParentOrderState::Working;
    }

private:
    static constexpr double epsilon_{1e-9};

    std::atomic<uint64_t> next_parent_id_{1};
    std::atomic<uint64_t> next_child_id_{1};

    std::unordered_map<uint64_t, ParentOrder> parents_;
    std::unordered_map<uint64_t, ChildOrder> children_;

    std::unordered_map<std::string, uint64_t> client_to_child_;
    std::unordered_map<std::string, uint64_t> venue_to_child_;

    ChildOrderMetrics metrics_{};
};

} // namespace hft::execution