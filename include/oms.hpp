#pragma once
// oms.hpp — production-grade Order Management System

#include "types.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <optional>
#include <cstdint>
#include <algorithm>

namespace hft {

enum class OmsEventType : uint8_t {
    Submitted,
    Accepted,
    Rejected,
    ReplaceRequested,
    Replaced,
    CancelRequested,
    CancelRejected,
    Cancelled,
    PartiallyFilled,
    Filled,
    DuplicateFillIgnored,
    StateViolation
};

enum class OmsRejectReason : uint8_t {
    None,
    UnknownOrder,
    AlreadyTerminal,
    DuplicateOrderId,
    DuplicateFillId,
    InvalidTransition,
    ExchangeRejected,
    RiskRejected
};

inline const char* oms_event_to_str(OmsEventType t) noexcept {
    switch (t) {
        case OmsEventType::Submitted: return "submitted";
        case OmsEventType::Accepted: return "accepted";
        case OmsEventType::Rejected: return "rejected";
        case OmsEventType::ReplaceRequested: return "replace_requested";
        case OmsEventType::Replaced: return "replaced";
        case OmsEventType::CancelRequested: return "cancel_requested";
        case OmsEventType::CancelRejected: return "cancel_rejected";
        case OmsEventType::Cancelled: return "cancelled";
        case OmsEventType::PartiallyFilled: return "partially_filled";
        case OmsEventType::Filled: return "filled";
        case OmsEventType::DuplicateFillIgnored: return "duplicate_fill_ignored";
        case OmsEventType::StateViolation: return "state_violation";
        default: return "unknown";
    }
}

struct OmsEvent {
    OmsEventType type = OmsEventType::Submitted;
    OmsRejectReason reason = OmsRejectReason::None;
    std::string order_id;
    std::string client_id;
    std::string symbol;
    int64_t ts = 0;
    int32_t version = 0;
    Side side = Side::Unknown;
    double price = 0.0;
    double qty = 0.0;
    double filled_qty = 0.0;
    std::string message;
};

struct OmsStats {
    int64_t submitted = 0;
    int64_t accepted = 0;
    int64_t rejected = 0;
    int64_t replace_requested = 0;
    int64_t replaced = 0;
    int64_t cancel_requested = 0;
    int64_t cancel_rejected = 0;
    int64_t cancelled = 0;
    int64_t partially_filled = 0;
    int64_t filled = 0;
    int64_t duplicate_fills = 0;
    int64_t state_violations = 0;
};

struct ManagedOrder {
    Order order;
    int32_t version = 1;
    bool pending_cancel = false;
    bool pending_replace = false;
    double replace_price = 0.0;
    double replace_qty = 0.0;
    int64_t created_ts = 0;
    int64_t accepted_ts = 0;
    int64_t updated_ts = 0;
    int64_t terminal_ts = 0;
};

class OrderManager {
public:
    bool submit(const Order& order) {
        if (orders_.count(order.order_id)) {
            log_violation(order, OmsRejectReason::DuplicateOrderId, "duplicate_order_id");
            return false;
        }

        ManagedOrder mo;
        mo.order = order;
        mo.order.status = OrderStatus::Open;
        mo.created_ts = order.timestamp;
        mo.updated_ts = order.timestamp;

        orders_[order.order_id] = mo;
        client_to_order_[order.client_id] = order.order_id;

        ++stats_.submitted;
        add_live_index(order.symbol, order.order_id);

        log_event(OmsEventType::Submitted, orders_[order.order_id]);
        return true;
    }

    bool on_ack(const Order& order, bool accepted, int64_t ts, OmsRejectReason reason = OmsRejectReason::None) {
        auto it = orders_.find(order.order_id);

        if (it == orders_.end()) {
            ManagedOrder mo;
            mo.order = order;
            mo.created_ts = order.timestamp;
            mo.updated_ts = ts;
            orders_[order.order_id] = mo;
            it = orders_.find(order.order_id);
        }

        ManagedOrder& mo = it->second;
        mo.updated_ts = ts;

        if (accepted) {
            if (is_terminal(mo.order.status)) {
                log_violation(mo.order, OmsRejectReason::InvalidTransition, "ack_on_terminal_order");
                return false;
            }

            mo.order.status = OrderStatus::Open;
            mo.accepted_ts = ts;
            ++stats_.accepted;
            add_live_index(mo.order.symbol, mo.order.order_id);
            log_event(OmsEventType::Accepted, mo);
            return true;
        }

        mo.order.status = OrderStatus::Rejected;
        mo.terminal_ts = ts;
        ++stats_.rejected;
        remove_live_index(mo.order.symbol, mo.order.order_id);
        log_event(OmsEventType::Rejected, mo, reason);
        return true;
    }

    bool request_cancel(const std::string& order_id, int64_t ts) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) {
            ++stats_.cancel_rejected;
            return false;
        }

        ManagedOrder& mo = it->second;
        if (is_terminal(mo.order.status)) {
            ++stats_.cancel_rejected;
            log_event(OmsEventType::CancelRejected, mo, OmsRejectReason::AlreadyTerminal);
            return false;
        }

        mo.pending_cancel = true;
        mo.updated_ts = ts;
        ++stats_.cancel_requested;
        log_event(OmsEventType::CancelRequested, mo);
        return true;
    }

    bool on_cancelled(const std::string& order_id, int64_t ts) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return false;

        ManagedOrder& mo = it->second;
        if (is_terminal(mo.order.status)) {
            log_violation(mo.order, OmsRejectReason::AlreadyTerminal, "cancel_on_terminal");
            return false;
        }

        mo.order.status = OrderStatus::Cancelled;
        mo.pending_cancel = false;
        mo.terminal_ts = ts;
        mo.updated_ts = ts;

        ++stats_.cancelled;
        remove_live_index(mo.order.symbol, order_id);
        log_event(OmsEventType::Cancelled, mo);
        return true;
    }

    bool request_replace(const std::string& order_id, double new_price, double new_qty, int64_t ts) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return false;

        ManagedOrder& mo = it->second;
        if (is_terminal(mo.order.status)) {
            log_event(OmsEventType::StateViolation, mo, OmsRejectReason::AlreadyTerminal);
            return false;
        }

        mo.pending_replace = true;
        mo.replace_price = new_price;
        mo.replace_qty = new_qty;
        mo.updated_ts = ts;

        ++stats_.replace_requested;
        log_event(OmsEventType::ReplaceRequested, mo);
        return true;
    }

    bool on_replaced(const std::string& order_id, double new_price, double new_qty, int64_t ts) {
        auto it = orders_.find(order_id);
        if (it == orders_.end()) return false;

        ManagedOrder& mo = it->second;
        if (is_terminal(mo.order.status)) {
            log_violation(mo.order, OmsRejectReason::AlreadyTerminal, "replace_on_terminal");
            return false;
        }

        mo.order.price = new_price;
        mo.order.qty = new_qty;
        mo.pending_replace = false;
        mo.version++;
        mo.updated_ts = ts;

        ++stats_.replaced;
        log_event(OmsEventType::Replaced, mo);
        return true;
    }

    bool on_fill(const FillEvent& fill) {
        if (!fill.order_id.empty() && !seen_fill_ids_.insert(fill_key(fill)).second) {
            ++stats_.duplicate_fills;
            OmsEvent ev;
            ev.type = OmsEventType::DuplicateFillIgnored;
            ev.order_id = fill.order_id;
            ev.symbol = fill.symbol;
            ev.ts = fill.timestamp;
            ev.price = fill.price;
            ev.qty = fill.qty;
            audit_.push_back(std::move(ev));
            return false;
        }

        auto it = orders_.find(fill.order_id);
        if (it == orders_.end()) return false;

        ManagedOrder& mo = it->second;
        if (is_terminal(mo.order.status)) {
            log_violation(mo.order, OmsRejectReason::AlreadyTerminal, "fill_on_terminal");
            return false;
        }

        double remaining = std::max(0.0, mo.order.qty - mo.order.filled_qty);
        double applied = std::min(fill.qty, remaining);
        mo.order.filled_qty += applied;
        mo.updated_ts = fill.timestamp;

        if (mo.order.filled_qty >= mo.order.qty - 1e-12) {
            mo.order.status = OrderStatus::Filled;
            mo.terminal_ts = fill.timestamp;
            ++stats_.filled;
            remove_live_index(mo.order.symbol, mo.order.order_id);
            log_event(OmsEventType::Filled, mo);
        } else {
            mo.order.status = OrderStatus::Partial;
            ++stats_.partially_filled;
            log_event(OmsEventType::PartiallyFilled, mo);
        }

        return true;
    }

    const ManagedOrder* get_managed(const std::string& order_id) const {
        auto it = orders_.find(order_id);
        return it == orders_.end() ? nullptr : &it->second;
    }

    const Order* get(const std::string& order_id) const {
        auto* mo = get_managed(order_id);
        return mo ? &mo->order : nullptr;
    }

    std::optional<std::string> order_by_client_id(const std::string& client_id) const {
        auto it = client_to_order_.find(client_id);
        if (it == client_to_order_.end()) return std::nullopt;
        return it->second;
    }

    bool is_live(const std::string& order_id) const {
        auto* mo = get_managed(order_id);
        if (!mo) return false;
        return mo->order.status == OrderStatus::Open || mo->order.status == OrderStatus::Partial;
    }

    std::vector<Order> live_orders(const std::string& symbol = "") const {
        std::vector<Order> out;

        if (!symbol.empty()) {
            auto it = live_by_symbol_.find(symbol);
            if (it == live_by_symbol_.end()) return out;
            out.reserve(it->second.size());
            for (const auto& oid : it->second) {
                auto oit = orders_.find(oid);
                if (oit != orders_.end()) out.push_back(oit->second.order);
            }
            return out;
        }

        for (const auto& [id, mo] : orders_) {
            if (mo.order.status == OrderStatus::Open || mo.order.status == OrderStatus::Partial)
                out.push_back(mo.order);
        }

        return out;
    }

    size_t live_count(const std::string& symbol = "") const {
        if (!symbol.empty()) {
            auto it = live_by_symbol_.find(symbol);
            return it == live_by_symbol_.end() ? 0 : it->second.size();
        }

        size_t n = 0;
        for (const auto& [sym, set] : live_by_symbol_) n += set.size();
        return n;
    }

    const OmsStats& stats() const noexcept { return stats_; }
    const std::vector<OmsEvent>& audit_log() const noexcept { return audit_; }
    const std::unordered_map<std::string, ManagedOrder>& orders() const noexcept { return orders_; }

private:
    std::unordered_map<std::string, ManagedOrder> orders_;
    std::unordered_map<std::string, std::string> client_to_order_;
    std::unordered_map<std::string, std::unordered_set<std::string>> live_by_symbol_;
    std::unordered_set<std::string> seen_fill_ids_;
    std::vector<OmsEvent> audit_;
    OmsStats stats_;

    static bool is_terminal(OrderStatus s) noexcept {
        return s == OrderStatus::Filled ||
               s == OrderStatus::Cancelled ||
               s == OrderStatus::Rejected;
    }

    static std::string fill_key(const FillEvent& f) {
        return f.order_id + "|" + std::to_string(f.timestamp) + "|" +
               std::to_string(f.price) + "|" + std::to_string(f.qty);
    }

    void add_live_index(const std::string& symbol, const std::string& order_id) {
        live_by_symbol_[symbol].insert(order_id);
    }

    void remove_live_index(const std::string& symbol, const std::string& order_id) {
        auto it = live_by_symbol_.find(symbol);
        if (it == live_by_symbol_.end()) return;
        it->second.erase(order_id);
        if (it->second.empty()) live_by_symbol_.erase(it);
    }

    void log_event(
        OmsEventType type,
        const ManagedOrder& mo,
        OmsRejectReason reason = OmsRejectReason::None,
        std::string message = {}
    ) {
        OmsEvent ev;
        ev.type = type;
        ev.reason = reason;
        ev.order_id = mo.order.order_id;
        ev.client_id = mo.order.client_id;
        ev.symbol = mo.order.symbol;
        ev.ts = mo.updated_ts;
        ev.version = mo.version;
        ev.side = mo.order.side;
        ev.price = mo.order.price;
        ev.qty = mo.order.qty;
        ev.filled_qty = mo.order.filled_qty;
        ev.message = std::move(message);
        audit_.push_back(std::move(ev));
    }

    void log_violation(const Order& order, OmsRejectReason reason, std::string message) {
        ++stats_.state_violations;

        OmsEvent ev;
        ev.type = OmsEventType::StateViolation;
        ev.reason = reason;
        ev.order_id = order.order_id;
        ev.client_id = order.client_id;
        ev.symbol = order.symbol;
        ev.ts = order.timestamp;
        ev.side = order.side;
        ev.price = order.price;
        ev.qty = order.qty;
        ev.filled_qty = order.filled_qty;
        ev.message = std::move(message);

        audit_.push_back(std::move(ev));
    }
};

} // namespace hft