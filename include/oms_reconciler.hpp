#pragma once
// oms_reconciler.hpp — advanced OMS journaling, recovery and reconciliation

#include "types.hpp"
#include "oms.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class ExchangeOrderStatus : uint8_t {
    Unknown,
    Open,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected
};

enum class ReconcileSeverity : uint8_t {
    Info,
    Warning,
    Critical
};

enum class ReconcileActionType : uint8_t {
    None,
    ImportMissingExchangeOrder,
    CancelUnexpectedExchangeOrder,
    MarkLocalCancelled,
    MarkLocalFilled,
    CorrectLocalFilledQty,
    CorrectLocalStatus,
    AlertStateMismatch,
    AlertStaleOrder,
    AlertDuplicateClientId
};

struct ExchangeOrderSnapshot {
    std::string venue;
    std::string order_id;
    std::string client_id;
    std::string symbol;
    Side side = Side::Unknown;
    double price = 0.0;
    double qty = 0.0;
    double filled_qty = 0.0;
    ExchangeOrderStatus status = ExchangeOrderStatus::Unknown;
    int64_t exchange_ts = 0;
    int64_t last_update_ts = 0;
};

struct ReconcileAction {
    ReconcileActionType type = ReconcileActionType::None;
    ReconcileSeverity severity = ReconcileSeverity::Info;

    std::string venue;
    std::string order_id;
    std::string client_id;
    std::string symbol;
    std::string message;

    double local_filled = 0.0;
    double exchange_filled = 0.0;
    int64_t local_ts = 0;
    int64_t exchange_ts = 0;
};

struct ReconciliationConfig {
    int64_t stale_live_order_ns = 30'000'000'000LL;
    double qty_tolerance = 1e-9;
    bool cancel_unexpected_exchange_orders = true;
    bool import_missing_exchange_orders = true;
};

struct ReconciliationReport {
    int64_t checked_local = 0;
    int64_t checked_exchange = 0;
    int64_t matched = 0;
    int64_t warnings = 0;
    int64_t critical = 0;
    std::vector<ReconcileAction> actions;

    bool clean() const noexcept { return actions.empty(); }
};

class OmsJournal {
public:
    explicit OmsJournal(std::string path) : path_(std::move(path)) {}

    bool append(const OmsEvent& e) {
        std::ofstream out(path_, std::ios::app);
        if (!out) return false;

        out << static_cast<int>(e.type) << ','
            << static_cast<int>(e.reason) << ','
            << e.ts << ','
            << e.version << ','
            << e.order_id << ','
            << e.client_id << ','
            << e.symbol << ','
            << static_cast<int>(e.side) << ','
            << e.price << ','
            << e.qty << ','
            << e.filled_qty << ','
            << escape(e.message) << '\n';

        return true;
    }

    bool append_all(const std::vector<OmsEvent>& events) {
        bool ok = true;
        for (const auto& e : events) ok = append(e) && ok;
        return ok;
    }

    std::vector<std::string> read_lines() const {
        std::vector<std::string> lines;
        std::ifstream in(path_);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty()) lines.push_back(line);
        }
        return lines;
    }

    const std::string& path() const noexcept { return path_; }

private:
    std::string path_;

    static std::string escape(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) out.push_back(c == ',' ? ';' : c);
        return out;
    }
};

class OmsReconciler {
public:
    explicit OmsReconciler(ReconciliationConfig cfg = {})
        : cfg_(cfg) {}

    ReconciliationReport reconcile(
        const OrderManager& oms,
        const std::vector<ExchangeOrderSnapshot>& exchange_orders,
        int64_t now_ns = 0
    ) const {
        ReconciliationReport r;
        r.checked_local = static_cast<int64_t>(oms.orders().size());
        r.checked_exchange = static_cast<int64_t>(exchange_orders.size());

        auto by_order_id = index_by_order_id(exchange_orders);
        auto by_client_id = index_by_client_id(exchange_orders);

        detect_duplicate_client_ids(r, exchange_orders);

        for (const auto& [oid, mo] : oms.orders()) {
            const Order& local = mo.order;
            const ExchangeOrderSnapshot* ex = find_exchange(local, by_order_id, by_client_id);

            if (!ex) {
                handle_missing_exchange(r, local, mo.updated_ts, now_ns);
                continue;
            }

            ++r.matched;
            compare_pair(r, local, mo.updated_ts, *ex, now_ns);
        }

        for (const auto& ex : exchange_orders) {
            bool exists = false;

            if (!ex.order_id.empty() && oms.get(ex.order_id)) exists = true;

            if (!exists && !ex.client_id.empty()) {
                auto local = oms.order_by_client_id(ex.client_id);
                exists = local.has_value();
            }

            if (!exists && is_exchange_live(ex.status)) {
                push(r, {
                    cfg_.import_missing_exchange_orders
                        ? ReconcileActionType::ImportMissingExchangeOrder
                        : ReconcileActionType::AlertStateMismatch,
                    ReconcileSeverity::Critical,
                    ex.venue,
                    ex.order_id,
                    ex.client_id,
                    ex.symbol,
                    "exchange_live_order_missing_locally",
                    0.0,
                    ex.filled_qty,
                    0,
                    ex.exchange_ts
                });
            }
        }

        finalize_counts(r);
        return r;
    }

    std::string report_to_csv(const ReconciliationReport& r) const {
        std::ostringstream oss;
        oss << "severity,action,venue,order_id,client_id,symbol,message,local_filled,exchange_filled,local_ts,exchange_ts\n";

        for (const auto& a : r.actions) {
            oss << severity_to_str(a.severity) << ','
                << action_to_str(a.type) << ','
                << a.venue << ','
                << a.order_id << ','
                << a.client_id << ','
                << a.symbol << ','
                << safe(a.message) << ','
                << a.local_filled << ','
                << a.exchange_filled << ','
                << a.local_ts << ','
                << a.exchange_ts << '\n';
        }

        return oss.str();
    }

private:
    ReconciliationConfig cfg_;

    using SnapIndex = std::unordered_map<std::string, const ExchangeOrderSnapshot*>;

    static SnapIndex index_by_order_id(const std::vector<ExchangeOrderSnapshot>& snaps) {
        SnapIndex m;
        for (const auto& s : snaps)
            if (!s.order_id.empty()) m[s.order_id] = &s;
        return m;
    }

    static SnapIndex index_by_client_id(const std::vector<ExchangeOrderSnapshot>& snaps) {
        SnapIndex m;
        for (const auto& s : snaps)
            if (!s.client_id.empty()) m[s.client_id] = &s;
        return m;
    }

    static const ExchangeOrderSnapshot* find_exchange(
        const Order& local,
        const SnapIndex& by_order_id,
        const SnapIndex& by_client_id
    ) {
        auto it = by_order_id.find(local.order_id);
        if (it != by_order_id.end()) return it->second;

        if (!local.client_id.empty()) {
            auto cit = by_client_id.find(local.client_id);
            if (cit != by_client_id.end()) return cit->second;
        }

        return nullptr;
    }

    void handle_missing_exchange(
        ReconciliationReport& r,
        const Order& local,
        int64_t local_ts,
        int64_t now_ns
    ) const {
        if (is_live(local.status)) {
            push(r, {
                ReconcileActionType::AlertStateMismatch,
                ReconcileSeverity::Critical,
                "",
                local.order_id,
                local.client_id,
                local.symbol,
                "local_live_order_missing_on_exchange",
                local.filled_qty,
                0.0,
                local_ts,
                0
            });
        }

        if (now_ns > 0 && is_live(local.status) && now_ns - local_ts > cfg_.stale_live_order_ns) {
            push(r, {
                ReconcileActionType::AlertStaleOrder,
                ReconcileSeverity::Warning,
                "",
                local.order_id,
                local.client_id,
                local.symbol,
                "local_order_stale_without_exchange_confirmation",
                local.filled_qty,
                0.0,
                local_ts,
                0
            });
        }
    }

    void compare_pair(
        ReconciliationReport& r,
        const Order& local,
        int64_t local_ts,
        const ExchangeOrderSnapshot& ex,
        int64_t now_ns
    ) const {
        if (std::abs(local.filled_qty - ex.filled_qty) > cfg_.qty_tolerance) {
            push(r, {
                ReconcileActionType::CorrectLocalFilledQty,
                ReconcileSeverity::Critical,
                ex.venue,
                local.order_id,
                local.client_id,
                local.symbol,
                "filled_qty_mismatch",
                local.filled_qty,
                ex.filled_qty,
                local_ts,
                ex.exchange_ts
            });
        }

        if (is_live(local.status) && ex.status == ExchangeOrderStatus::Cancelled) {
            push(r, {
                ReconcileActionType::MarkLocalCancelled,
                ReconcileSeverity::Critical,
                ex.venue,
                local.order_id,
                local.client_id,
                local.symbol,
                "exchange_cancelled_but_local_live",
                local.filled_qty,
                ex.filled_qty,
                local_ts,
                ex.exchange_ts
            });
        }

        if (is_live(local.status) && ex.status == ExchangeOrderStatus::Filled) {
            push(r, {
                ReconcileActionType::MarkLocalFilled,
                ReconcileSeverity::Critical,
                ex.venue,
                local.order_id,
                local.client_id,
                local.symbol,
                "exchange_filled_but_local_live",
                local.filled_qty,
                ex.filled_qty,
                local_ts,
                ex.exchange_ts
            });
        }

        if (is_terminal(local.status) && is_exchange_live(ex.status)) {
            push(r, {
                cfg_.cancel_unexpected_exchange_orders
                    ? ReconcileActionType::CancelUnexpectedExchangeOrder
                    : ReconcileActionType::AlertStateMismatch,
                ReconcileSeverity::Critical,
                ex.venue,
                local.order_id,
                local.client_id,
                local.symbol,
                "local_terminal_but_exchange_live",
                local.filled_qty,
                ex.filled_qty,
                local_ts,
                ex.exchange_ts
            });
        }

        if (now_ns > 0 && is_live(local.status)) {
            int64_t last = std::max(local_ts, ex.last_update_ts);
            if (last > 0 && now_ns - last > cfg_.stale_live_order_ns) {
                push(r, {
                    ReconcileActionType::AlertStaleOrder,
                    ReconcileSeverity::Warning,
                    ex.venue,
                    local.order_id,
                    local.client_id,
                    local.symbol,
                    "live_order_stale",
                    local.filled_qty,
                    ex.filled_qty,
                    local_ts,
                    ex.exchange_ts
                });
            }
        }
    }

    static void detect_duplicate_client_ids(
        ReconciliationReport& r,
        const std::vector<ExchangeOrderSnapshot>& snaps
    ) {
        std::unordered_map<std::string, int> seen;

        for (const auto& s : snaps) {
            if (s.client_id.empty()) continue;
            int n = ++seen[s.client_id];

            if (n == 2) {
                push(r, {
                    ReconcileActionType::AlertDuplicateClientId,
                    ReconcileSeverity::Critical,
                    s.venue,
                    s.order_id,
                    s.client_id,
                    s.symbol,
                    "duplicate_client_id_on_exchange",
                    0.0,
                    s.filled_qty,
                    0,
                    s.exchange_ts
                });
            }
        }
    }

    static bool is_live(OrderStatus s) noexcept {
        return s == OrderStatus::Open || s == OrderStatus::Partial;
    }

    static bool is_terminal(OrderStatus s) noexcept {
        return s == OrderStatus::Filled ||
               s == OrderStatus::Cancelled ||
               s == OrderStatus::Rejected;
    }

    static bool is_exchange_live(ExchangeOrderStatus s) noexcept {
        return s == ExchangeOrderStatus::Open ||
               s == ExchangeOrderStatus::PartiallyFilled;
    }

    static void push(ReconciliationReport& r, ReconcileAction a) {
        r.actions.push_back(std::move(a));
    }

    static void finalize_counts(ReconciliationReport& r) {
        r.warnings = 0;
        r.critical = 0;

        for (const auto& a : r.actions) {
            if (a.severity == ReconcileSeverity::Warning) ++r.warnings;
            if (a.severity == ReconcileSeverity::Critical) ++r.critical;
        }
    }

    static const char* severity_to_str(ReconcileSeverity s) noexcept {
        switch (s) {
            case ReconcileSeverity::Info: return "info";
            case ReconcileSeverity::Warning: return "warning";
            case ReconcileSeverity::Critical: return "critical";
            default: return "unknown";
        }
    }

    static const char* action_to_str(ReconcileActionType t) noexcept {
        switch (t) {
            case ReconcileActionType::None: return "none";
            case ReconcileActionType::ImportMissingExchangeOrder: return "import_missing_exchange_order";
            case ReconcileActionType::CancelUnexpectedExchangeOrder: return "cancel_unexpected_exchange_order";
            case ReconcileActionType::MarkLocalCancelled: return "mark_local_cancelled";
            case ReconcileActionType::MarkLocalFilled: return "mark_local_filled";
            case ReconcileActionType::CorrectLocalFilledQty: return "correct_local_filled_qty";
            case ReconcileActionType::CorrectLocalStatus: return "correct_local_status";
            case ReconcileActionType::AlertStateMismatch: return "alert_state_mismatch";
            case ReconcileActionType::AlertStaleOrder: return "alert_stale_order";
            case ReconcileActionType::AlertDuplicateClientId: return "alert_duplicate_client_id";
            default: return "unknown";
        }
    }

    static std::string safe(std::string s) {
        for (char& c : s)
            if (c == ',') c = ';';
        return s;
    }
};

inline const char* reconcile_action_to_str(ReconcileActionType t) noexcept {
    switch (t) {
        case ReconcileActionType::None: return "none";
        case ReconcileActionType::ImportMissingExchangeOrder: return "import_missing_exchange_order";
        case ReconcileActionType::CancelUnexpectedExchangeOrder: return "cancel_unexpected_exchange_order";
        case ReconcileActionType::MarkLocalCancelled: return "mark_local_cancelled";
        case ReconcileActionType::MarkLocalFilled: return "mark_local_filled";
        case ReconcileActionType::CorrectLocalFilledQty: return "correct_local_filled_qty";
        case ReconcileActionType::CorrectLocalStatus: return "correct_local_status";
        case ReconcileActionType::AlertStateMismatch: return "alert_state_mismatch";
        case ReconcileActionType::AlertStaleOrder: return "alert_stale_order";
        case ReconcileActionType::AlertDuplicateClientId: return "alert_duplicate_client_id";
        default: return "unknown";
    }
}

} // namespace hft