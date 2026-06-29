#pragma once
// exchange_gateway.hpp — production-style gateway abstraction + paper gateway

#include "types.hpp"
#include "oms.hpp"
#include "risk_gateway.hpp"
#include "orderbook.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft {

enum class GatewayStatus : uint8_t {
    Disconnected,
    Connecting,
    Connected,
    Degraded,
    Halted
};

enum class GatewayEventType : uint8_t {
    Connected,
    Disconnected,
    OrderAccepted,
    OrderRejected,
    CancelAccepted,
    CancelRejected,
    ReplaceAccepted,
    ReplaceRejected,
    Fill,
    Heartbeat,
    Error
};

enum class GatewayRejectCode : uint8_t {
    None,
    Disconnected,
    Halted,
    DuplicateClientId,
    RiskRejected,
    UnknownOrder,
    InvalidOrder,
    AlreadyTerminal,
    PriceBand,
    ExchangeRejected,
    InternalError
};

inline const char* gateway_reject_to_str(GatewayRejectCode c) noexcept {
    switch (c) {
        case GatewayRejectCode::None: return "none";
        case GatewayRejectCode::Disconnected: return "disconnected";
        case GatewayRejectCode::Halted: return "halted";
        case GatewayRejectCode::DuplicateClientId: return "duplicate_client_id";
        case GatewayRejectCode::RiskRejected: return "risk_rejected";
        case GatewayRejectCode::UnknownOrder: return "unknown_order";
        case GatewayRejectCode::InvalidOrder: return "invalid_order";
        case GatewayRejectCode::AlreadyTerminal: return "already_terminal";
        case GatewayRejectCode::PriceBand: return "price_band";
        case GatewayRejectCode::ExchangeRejected: return "exchange_rejected";
        case GatewayRejectCode::InternalError: return "internal_error";
        default: return "unknown";
    }
}

struct GatewayEvent {
    GatewayEventType type = GatewayEventType::Heartbeat;
    GatewayRejectCode reject_code = GatewayRejectCode::None;

    std::string venue;
    std::string order_id;
    std::string client_id;
    std::string symbol;
    std::string message;

    int64_t ts = 0;
    double price = 0.0;
    double qty = 0.0;
    double fill_qty = 0.0;
};

struct GatewayAck {
    bool accepted = false;
    GatewayRejectCode reject_code = GatewayRejectCode::None;
    std::string order_id;
    std::string client_id;
    std::string venue;
    std::string message;
    int64_t ts = 0;
};

struct GatewayCancelAck {
    bool accepted = false;
    GatewayRejectCode reject_code = GatewayRejectCode::None;
    std::string order_id;
    std::string venue;
    std::string message;
    int64_t ts = 0;
};

struct GatewayReplaceAck {
    bool accepted = false;
    GatewayRejectCode reject_code = GatewayRejectCode::None;
    std::string order_id;
    std::string venue;
    std::string message;
    int64_t ts = 0;
    double new_price = 0.0;
    double new_qty = 0.0;
};

struct GatewayHealth {
    GatewayStatus status = GatewayStatus::Disconnected;
    int64_t last_heartbeat_ns = 0;
    int64_t reconnects = 0;
    double reject_rate = 0.0;
    double fill_rate = 1.0;
    double avg_ack_latency_us = 0.0;
};

struct GatewayStats {
    int64_t sent_orders = 0;
    int64_t accepted_orders = 0;
    int64_t rejected_orders = 0;

    int64_t cancel_requests = 0;
    int64_t cancel_accepted = 0;
    int64_t cancel_rejected = 0;

    int64_t replace_requests = 0;
    int64_t replace_accepted = 0;
    int64_t replace_rejected = 0;

    int64_t fills = 0;
    int64_t duplicate_client_ids = 0;
    int64_t events_emitted = 0;
};

class IExchangeGateway {
public:
    virtual ~IExchangeGateway() = default;

    virtual GatewayStatus status() const = 0;
    virtual GatewayHealth health() const = 0;
    virtual const std::string& venue() const = 0;

    virtual void connect() = 0;
    virtual void disconnect() = 0;
    virtual void halt(const std::string& reason = "") = 0;
    virtual void resume() = 0;

    virtual GatewayAck send_order(const Order& order) = 0;
    virtual GatewayCancelAck cancel_order(const std::string& order_id) = 0;
    virtual GatewayReplaceAck replace_order(
        const std::string& order_id,
        double new_price,
        double new_qty
    ) = 0;

    virtual bool poll_event(GatewayEvent& out) = 0;
    virtual const GatewayStats& stats() const = 0;
};

class PaperExchangeGateway final : public IExchangeGateway {
public:
    using FillCallback = std::function<void(const FillEvent&)>;
    using EventCallback = std::function<void(const GatewayEvent&)>;

    explicit PaperExchangeGateway(
        std::string venue,
        OrderManager* oms = nullptr,
        RiskGateway* risk = nullptr,
        PortfolioState* portfolio = nullptr
    )
        : venue_(std::move(venue))
        , oms_(oms)
        , risk_(risk)
        , portfolio_(portfolio)
    {
        health_.status = GatewayStatus::Connected;
        health_.last_heartbeat_ns = now_ns();
    }

    GatewayStatus status() const override { return health_.status; }
    GatewayHealth health() const override { return health_; }
    const std::string& venue() const override { return venue_; }
    const GatewayStats& stats() const override { return stats_; }

    void set_oms(OrderManager* oms) noexcept { oms_ = oms; }
    void set_risk_gateway(RiskGateway* risk) noexcept { risk_ = risk; }
    void set_portfolio(PortfolioState* portfolio) noexcept { portfolio_ = portfolio; }
    void set_fill_callback(FillCallback cb) { fill_cb_ = std::move(cb); }
    void set_event_callback(EventCallback cb) { event_cb_ = std::move(cb); }

    void update_book(const std::string& symbol, OrderBook* book) {
        books_[symbol] = book;
    }

    void connect() override {
        health_.status = GatewayStatus::Connected;
        health_.last_heartbeat_ns = now_ns();
        emit({GatewayEventType::Connected, GatewayRejectCode::None, venue_, "", "", "", "connected", now_ns()});
    }

    void disconnect() override {
        health_.status = GatewayStatus::Disconnected;
        emit({GatewayEventType::Disconnected, GatewayRejectCode::None, venue_, "", "", "", "disconnected", now_ns()});
    }

    void halt(const std::string& reason = "") override {
        health_.status = GatewayStatus::Halted;
        emit({GatewayEventType::Error, GatewayRejectCode::Halted, venue_, "", "", "", reason, now_ns()});
    }

    void resume() override {
        if (health_.status == GatewayStatus::Halted || health_.status == GatewayStatus::Disconnected)
            health_.status = GatewayStatus::Connected;
    }

    GatewayAck send_order(const Order& order) override {
        ++stats_.sent_orders;

        GatewayAck ack;
        ack.order_id = order.order_id;
        ack.client_id = order.client_id;
        ack.venue = venue_;
        ack.ts = now_ns();

        auto reject = [&](GatewayRejectCode code, const std::string& msg) {
            ++stats_.rejected_orders;
            ack.accepted = false;
            ack.reject_code = code;
            ack.message = msg;
            update_reject_rate(false);
            if (oms_) oms_->on_ack(order, false, ack.ts, to_oms_reason(code));
            emit_order_event(GatewayEventType::OrderRejected, order, code, msg, ack.ts);
            return ack;
        };

        if (health_.status == GatewayStatus::Disconnected)
            return reject(GatewayRejectCode::Disconnected, "gateway_disconnected");

        if (health_.status == GatewayStatus::Halted)
            return reject(GatewayRejectCode::Halted, "gateway_halted");

        if (!valid_order(order))
            return reject(GatewayRejectCode::InvalidOrder, "invalid_order");

        if (!order.client_id.empty()) {
            if (!seen_client_ids_.insert(order.client_id).second) {
                ++stats_.duplicate_client_ids;
                return reject(GatewayRejectCode::DuplicateClientId, "duplicate_client_id");
            }
        }

        if (risk_ && portfolio_) {
            OrderBook* book = nullptr;
            auto bit = books_.find(order.symbol);
            if (bit != books_.end()) book = bit->second;

            RiskDecision rd = risk_->check_order(order, *portfolio_, live_orders_, book, ack.ts);
            if (!rd.allowed)
                return reject(GatewayRejectCode::RiskRejected, rd.reason);
        }

        live_orders_[order.order_id] = order;

        if (oms_) {
            oms_->submit(order);
            oms_->on_ack(order, true, ack.ts);
        }

        ++stats_.accepted_orders;
        ack.accepted = true;
        update_reject_rate(true);
        emit_order_event(GatewayEventType::OrderAccepted, order, GatewayRejectCode::None, "", ack.ts);

        return ack;
    }

    GatewayCancelAck cancel_order(const std::string& order_id) override {
        ++stats_.cancel_requests;

        GatewayCancelAck ack;
        ack.order_id = order_id;
        ack.venue = venue_;
        ack.ts = now_ns();

        auto it = live_orders_.find(order_id);
        if (it == live_orders_.end()) {
            ++stats_.cancel_rejected;
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::UnknownOrder;
            ack.message = "unknown_order";
            emit_cancel_event(GatewayEventType::CancelRejected, order_id, ack.reject_code, ack.message, ack.ts);
            return ack;
        }

        if (oms_) {
            oms_->request_cancel(order_id, ack.ts);
            oms_->on_cancelled(order_id, ack.ts);
        }

        live_orders_.erase(it);

        ++stats_.cancel_accepted;
        ack.accepted = true;
        emit_cancel_event(GatewayEventType::CancelAccepted, order_id, GatewayRejectCode::None, "", ack.ts);
        return ack;
    }

    GatewayReplaceAck replace_order(
        const std::string& order_id,
        double new_price,
        double new_qty
    ) override {
        ++stats_.replace_requests;

        GatewayReplaceAck ack;
        ack.order_id = order_id;
        ack.venue = venue_;
        ack.ts = now_ns();
        ack.new_price = new_price;
        ack.new_qty = new_qty;

        auto it = live_orders_.find(order_id);
        if (it == live_orders_.end()) {
            ++stats_.replace_rejected;
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::UnknownOrder;
            ack.message = "unknown_order";
            emit_cancel_event(GatewayEventType::ReplaceRejected, order_id, ack.reject_code, ack.message, ack.ts);
            return ack;
        }

        if (new_price <= 0.0 || new_qty <= 0.0 || !std::isfinite(new_price) || !std::isfinite(new_qty)) {
            ++stats_.replace_rejected;
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::InvalidOrder;
            ack.message = "invalid_replace";
            emit_cancel_event(GatewayEventType::ReplaceRejected, order_id, ack.reject_code, ack.message, ack.ts);
            return ack;
        }

        Order replacement = it->second;
        replacement.price = new_price;
        replacement.qty = new_qty;
        replacement.timestamp = ack.ts;

        if (risk_ && portfolio_) {
            OrderBook* book = nullptr;
            auto bit = books_.find(replacement.symbol);
            if (bit != books_.end()) book = bit->second;

            auto temp_open = live_orders_;
            temp_open.erase(order_id);

            RiskDecision rd = risk_->check_order(replacement, *portfolio_, temp_open, book, ack.ts);
            if (!rd.allowed) {
                ++stats_.replace_rejected;
                ack.accepted = false;
                ack.reject_code = GatewayRejectCode::RiskRejected;
                ack.message = rd.reason;
                emit_cancel_event(GatewayEventType::ReplaceRejected, order_id, ack.reject_code, ack.message, ack.ts);
                return ack;
            }
        }

        it->second.price = new_price;
        it->second.qty = new_qty;
        it->second.timestamp = ack.ts;

        if (oms_) {
            oms_->request_replace(order_id, new_price, new_qty, ack.ts);
            oms_->on_replaced(order_id, new_price, new_qty, ack.ts);
        }

        ++stats_.replace_accepted;
        ack.accepted = true;
        emit_cancel_event(GatewayEventType::ReplaceAccepted, order_id, GatewayRejectCode::None, "", ack.ts);
        return ack;
    }

    void inject_fill(FillEvent fill) {
        ++stats_.fills;

        auto it = live_orders_.find(fill.order_id);
        if (it == live_orders_.end()) return;

        if (portfolio_) portfolio_->update_on_fill(fill);
        if (oms_) oms_->on_fill(fill);
        if (fill_cb_) fill_cb_(fill);

        emit_fill_event(fill);

        const Order* o = oms_ ? oms_->get(fill.order_id) : nullptr;
        if ((o && o->status == OrderStatus::Filled) || fill.qty >= it->second.qty - it->second.filled_qty - 1e-12) {
            live_orders_.erase(fill.order_id);
        }

        update_fill_rate(true);
    }

    void heartbeat() {
        health_.last_heartbeat_ns = now_ns();
        emit({GatewayEventType::Heartbeat, GatewayRejectCode::None, venue_, "", "", "", "heartbeat", health_.last_heartbeat_ns});
    }

    bool poll_event(GatewayEvent& out) override {
        if (events_.empty()) return false;
        out = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    const std::unordered_map<std::string, Order>& live_orders() const {
        return live_orders_;
    }

private:
    std::string venue_;
    GatewayHealth health_;
    GatewayStats stats_;

    OrderManager* oms_ = nullptr;
    RiskGateway* risk_ = nullptr;
    PortfolioState* portfolio_ = nullptr;

    FillCallback fill_cb_;
    EventCallback event_cb_;

    std::unordered_map<std::string, Order> live_orders_;
    std::unordered_map<std::string, OrderBook*> books_;
    std::unordered_set<std::string> seen_client_ids_;
    std::deque<GatewayEvent> events_;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    static bool valid_order(const Order& o) {
        if (o.order_id.empty() || o.symbol.empty()) return false;
        if (o.side == Side::Unknown) return false;
        if (o.qty <= 0.0 || !std::isfinite(o.qty)) return false;
        if (o.order_type != OrderType::Market && (o.price <= 0.0 || !std::isfinite(o.price))) return false;
        return true;
    }

    static OmsRejectReason to_oms_reason(GatewayRejectCode code) {
        switch (code) {
            case GatewayRejectCode::RiskRejected: return OmsRejectReason::RiskRejected;
            case GatewayRejectCode::DuplicateClientId: return OmsRejectReason::DuplicateOrderId;
            default: return OmsRejectReason::ExchangeRejected;
        }
    }

    void emit(GatewayEvent ev) {
        ++stats_.events_emitted;
        events_.push_back(ev);
        if (event_cb_) event_cb_(ev);
    }

    void emit_order_event(
        GatewayEventType type,
        const Order& order,
        GatewayRejectCode code,
        const std::string& msg,
        int64_t ts
    ) {
        GatewayEvent ev;
        ev.type = type;
        ev.reject_code = code;
        ev.venue = venue_;
        ev.order_id = order.order_id;
        ev.client_id = order.client_id;
        ev.symbol = order.symbol;
        ev.message = msg;
        ev.ts = ts;
        ev.price = order.price;
        ev.qty = order.qty;
        emit(std::move(ev));
    }

    void emit_cancel_event(
        GatewayEventType type,
        const std::string& order_id,
        GatewayRejectCode code,
        const std::string& msg,
        int64_t ts
    ) {
        GatewayEvent ev;
        ev.type = type;
        ev.reject_code = code;
        ev.venue = venue_;
        ev.order_id = order_id;
        ev.message = msg;
        ev.ts = ts;
        emit(std::move(ev));
    }

    void emit_fill_event(const FillEvent& fill) {
        GatewayEvent ev;
        ev.type = GatewayEventType::Fill;
        ev.reject_code = GatewayRejectCode::None;
        ev.venue = venue_;
        ev.order_id = fill.order_id;
        ev.symbol = fill.symbol;
        ev.ts = fill.timestamp;
        ev.price = fill.price;
        ev.qty = fill.qty;
        ev.fill_qty = fill.qty;
        emit(std::move(ev));
    }

    void update_reject_rate(bool accepted) {
        double x = accepted ? 0.0 : 1.0;
        health_.reject_rate = health_.reject_rate * 0.98 + x * 0.02;
    }

    void update_fill_rate(bool filled) {
        double x = filled ? 1.0 : 0.0;
        health_.fill_rate = health_.fill_rate * 0.98 + x * 0.02;
    }
};

} // namespace hft