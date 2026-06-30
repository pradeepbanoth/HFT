#pragma once
// exchange.hpp — advanced normalized exchange abstraction + paper exchange

#include "types.hpp"
#include "exchange_gateway.hpp"
#include "orderbook.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace hft {

enum class ExchangeKind : uint8_t {
    Paper,
    Binance,
    Bybit,
    OKX,
    Deribit,
    Coinbase,
    Unknown
};

enum class ExchangeConnectionState : uint8_t {
    Disconnected,
    Connecting,
    Authenticating,
    Connected,
    Recovering,
    Degraded,
    Halted
};

enum class ExchangeCapability : uint32_t {
    MarketData    = 1u << 0,
    Trading       = 1u << 1,
    CancelReplace = 1u << 2,
    BatchOrders   = 1u << 3,
    PrivateStream = 1u << 4,
    PublicStream  = 1u << 5,
    Positions     = 1u << 6,
    Balances      = 1u << 7,
    Recovery      = 1u << 8
};

inline uint32_t capability_bit(ExchangeCapability c) noexcept {
    return static_cast<uint32_t>(c);
}

struct ExchangeCredentials {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;
    bool testnet = true;
};

struct ExchangeSymbolInfo {
    std::string symbol;
    std::string base;
    std::string quote;

    double tick_size = 1e-8;
    double lot_size = 1e-8;
    double min_qty = 0.0;
    double max_qty = 1e18;
    double min_notional = 0.0;

    bool active = true;
};

struct ExchangeLimits {
    int64_t rest_orders_per_second = 10;
    int64_t rest_requests_per_second = 50;
    int64_t ws_subscriptions_per_second = 5;
    int64_t max_open_orders = 500;
};

struct ExchangeHealth {
    ExchangeConnectionState state = ExchangeConnectionState::Disconnected;

    int64_t last_public_msg_ns = 0;
    int64_t last_private_msg_ns = 0;
    int64_t last_heartbeat_ns = 0;

    int64_t reconnect_count = 0;
    int64_t sequence_gaps = 0;
    int64_t dropped_messages = 0;

    double avg_public_latency_us = 0.0;
    double avg_private_latency_us = 0.0;
};

struct ExchangeStats {
    int64_t public_messages = 0;
    int64_t private_messages = 0;
    int64_t orders_sent = 0;
    int64_t cancels_sent = 0;
    int64_t replaces_sent = 0;
    int64_t rejects = 0;
    int64_t reconnects = 0;
    int64_t recoveries = 0;
};

struct ExchangeOrderRequest {
    std::string client_id;
    std::string symbol;

    Side side = Side::Buy;
    OrderType type = OrderType::Limit;

    double price = 0.0;
    double qty = 0.0;

    bool post_only = false;
    bool reduce_only = false;
};

struct ExchangeCancelRequest {
    std::string order_id;
    std::string client_id;
    std::string symbol;
};

struct ExchangeReplaceRequest {
    std::string order_id;
    std::string client_id;
    std::string symbol;
    double new_price = 0.0;
    double new_qty = 0.0;
};

struct ExchangeBalance {
    std::string asset;
    double free = 0.0;
    double locked = 0.0;

    double total() const noexcept {
        return free + locked;
    }
};

struct ExchangePosition {
    std::string symbol;
    Side side = Side::Unknown;
    double qty = 0.0;
    double entry_price = 0.0;
    double mark_price = 0.0;
    double unrealized_pnl = 0.0;
};

struct ExchangeExecutionReport {
    std::string venue;
    std::string order_id;
    std::string client_id;
    std::string symbol;

    OrderStatus status = OrderStatus::Open;
    Side side = Side::Unknown;

    double price = 0.0;
    double qty = 0.0;
    double filled_qty = 0.0;
    double last_fill_price = 0.0;
    double last_fill_qty = 0.0;
    double fee = 0.0;

    int64_t exchange_ts = 0;
    int64_t recv_ts = 0;

    std::string message;
};

using MarketDataCallback = std::function<void(const MarketEvent&)>;
using ExecutionReportCallback = std::function<void(const ExchangeExecutionReport&)>;
using ExchangeStateCallback = std::function<void(ExchangeConnectionState)>;

class IExchange {
public:
    virtual ~IExchange() = default;

    virtual ExchangeKind kind() const = 0;
    virtual const std::string& venue() const = 0;
    virtual uint32_t capabilities() const = 0;

    bool has_capability(ExchangeCapability c) const {
        return (capabilities() & capability_bit(c)) != 0;
    }

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool authenticate(const ExchangeCredentials& credentials) = 0;
    virtual bool recover() = 0;

    virtual ExchangeConnectionState state() const = 0;
    virtual ExchangeHealth health() const = 0;
    virtual ExchangeStats stats() const = 0;

    virtual bool subscribe_book(const std::string& symbol, int depth = 50) = 0;
    virtual bool subscribe_trades(const std::string& symbol) = 0;
    virtual bool unsubscribe(const std::string& symbol) = 0;

    virtual GatewayAck submit_order(const ExchangeOrderRequest& req) = 0;
    virtual GatewayCancelAck cancel_order(const ExchangeCancelRequest& req) = 0;
    virtual GatewayReplaceAck replace_order(const ExchangeReplaceRequest& req) = 0;
    virtual int cancel_all(const std::string& symbol = "") = 0;

    virtual std::vector<ExchangeBalance> balances() const = 0;
    virtual std::vector<ExchangePosition> positions() const = 0;
    virtual std::vector<Order> open_orders(const std::string& symbol = "") const = 0;

    virtual std::optional<ExchangeSymbolInfo> symbol_info(const std::string& symbol) const = 0;
    virtual ExchangeLimits limits() const = 0;

    virtual void set_market_data_callback(MarketDataCallback cb) = 0;
    virtual void set_execution_report_callback(ExecutionReportCallback cb) = 0;
    virtual void set_state_callback(ExchangeStateCallback cb) = 0;
};

class PaperExchange final : public IExchange {
public:
    explicit PaperExchange(std::string venue = "paper")
        : venue_(std::move(venue))
        , gateway_(venue_)
    {}

    ExchangeKind kind() const override {
        return ExchangeKind::Paper;
    }

    const std::string& venue() const override {
        return venue_;
    }

    uint32_t capabilities() const override {
        return capability_bit(ExchangeCapability::MarketData) |
               capability_bit(ExchangeCapability::Trading) |
               capability_bit(ExchangeCapability::CancelReplace) |
               capability_bit(ExchangeCapability::BatchOrders) |
               capability_bit(ExchangeCapability::PrivateStream) |
               capability_bit(ExchangeCapability::PublicStream) |
               capability_bit(ExchangeCapability::Positions) |
               capability_bit(ExchangeCapability::Balances) |
               capability_bit(ExchangeCapability::Recovery);
    }

    bool connect() override {
        state_ = ExchangeConnectionState::Connected;
        gateway_.connect();
        health_.state = state_;
        health_.last_heartbeat_ns = now_ns();
        notify_state();
        return true;
    }

    void disconnect() override {
        state_ = ExchangeConnectionState::Disconnected;
        gateway_.disconnect();
        health_.state = state_;
        notify_state();
    }

    bool authenticate(const ExchangeCredentials&) override {
        if (state_ == ExchangeConnectionState::Disconnected)
            return false;

        state_ = ExchangeConnectionState::Connected;
        health_.state = state_;
        notify_state();
        return true;
    }

    bool recover() override {
        ++stats_.recoveries;

        state_ = ExchangeConnectionState::Recovering;
        health_.state = state_;
        notify_state();

        state_ = ExchangeConnectionState::Connected;
        health_.state = state_;
        notify_state();
        return true;
    }

    ExchangeConnectionState state() const override {
        return state_;
    }

    ExchangeHealth health() const override {
        ExchangeHealth h = health_;
        auto gh = gateway_.health();
        h.last_heartbeat_ns = gh.last_heartbeat_ns;
        h.reconnect_count = gh.reconnects;
        h.avg_private_latency_us = gh.avg_ack_latency_us;
        return h;
    }

    ExchangeStats stats() const override {
        return stats_;
    }

    bool subscribe_book(const std::string& symbol, int depth = 50) override {
        if (!is_known_symbol(symbol)) return false;
        subscribed_books_[symbol] = depth;
        return true;
    }

    bool subscribe_trades(const std::string& symbol) override {
        if (!is_known_symbol(symbol)) return false;
        subscribed_trades_[symbol] = true;
        return true;
    }

    bool unsubscribe(const std::string& symbol) override {
        subscribed_books_.erase(symbol);
        subscribed_trades_.erase(symbol);
        return true;
    }

    GatewayAck submit_order(const ExchangeOrderRequest& req) override {
        ++stats_.orders_sent;

        Order order;
        order.order_id = req.client_id.empty()
            ? "paper_" + std::to_string(++order_seq_)
            : req.client_id;
        order.client_id = req.client_id;
        order.symbol = req.symbol;
        order.side = req.side;
        order.order_type = req.post_only ? OrderType::PostOnly : req.type;
        order.price = normalize_price(req.symbol, req.price);
        order.qty = normalize_qty(req.symbol, req.qty);
        order.timestamp = now_ns();

        auto ack = gateway_.send_order(order);
        if (!ack.accepted) ++stats_.rejects;

        emit_report_from_ack(order, ack);
        ++stats_.private_messages;

        return ack;
    }

    GatewayCancelAck cancel_order(const ExchangeCancelRequest& req) override {
        ++stats_.cancels_sent;
        auto ack = gateway_.cancel_order(req.order_id);
        if (!ack.accepted) ++stats_.rejects;
        return ack;
    }

    GatewayReplaceAck replace_order(const ExchangeReplaceRequest& req) override {
        ++stats_.replaces_sent;

        double px = normalize_price(req.symbol, req.new_price);
        double qty = normalize_qty(req.symbol, req.new_qty);

        auto ack = gateway_.replace_order(req.order_id, px, qty);
        if (!ack.accepted) ++stats_.rejects;
        return ack;
    }

    int cancel_all(const std::string& symbol = "") override {
        auto orders = gateway_.live_orders();
        int n = 0;

        for (const auto& [oid, order] : orders) {
            if (symbol.empty() || order.symbol == symbol) {
                gateway_.cancel_order(oid);
                ++n;
            }
        }

        return n;
    }

    std::vector<ExchangeBalance> balances() const override {
        return balances_;
    }

    std::vector<ExchangePosition> positions() const override {
        return positions_;
    }

    std::vector<Order> open_orders(const std::string& symbol = "") const override {
        std::vector<Order> out;

        for (const auto& [id, order] : gateway_.live_orders()) {
            if (symbol.empty() || order.symbol == symbol)
                out.push_back(order);
        }

        return out;
    }

    std::optional<ExchangeSymbolInfo> symbol_info(const std::string& symbol) const override {
        auto it = symbols_.find(symbol);
        if (it == symbols_.end()) return std::nullopt;
        return it->second;
    }

    ExchangeLimits limits() const override {
        return limits_;
    }

    void set_market_data_callback(MarketDataCallback cb) override {
        md_cb_ = std::move(cb);
    }

    void set_execution_report_callback(ExecutionReportCallback cb) override {
        er_cb_ = std::move(cb);
    }

    void set_state_callback(ExchangeStateCallback cb) override {
        state_cb_ = std::move(cb);
    }

    void add_symbol(ExchangeSymbolInfo info) {
        symbols_[info.symbol] = std::move(info);
    }

    void set_limits(ExchangeLimits limits) {
        limits_ = limits;
    }

    void set_balances(std::vector<ExchangeBalance> balances) {
        balances_ = std::move(balances);
    }

    void set_positions(std::vector<ExchangePosition> positions) {
        positions_ = std::move(positions);
    }

    void update_book(const std::string& symbol, OrderBook* book) {
        gateway_.update_book(symbol, book);
    }

    void inject_market_event(const MarketEvent& event) {
        ++stats_.public_messages;
        health_.last_public_msg_ns = now_ns();

        if (md_cb_) md_cb_(event);
    }

    void inject_fill(FillEvent fill) {
        gateway_.inject_fill(fill);

        ExchangeExecutionReport er;
        er.venue = venue_;
        er.order_id = fill.order_id;
        er.client_id = fill.order_id;
        er.symbol = fill.symbol;
        er.side = fill.side;
        er.status = OrderStatus::Filled;
        er.last_fill_price = fill.price;
        er.last_fill_qty = fill.qty;
        er.filled_qty = fill.qty;
        er.fee = fill.fee;
        er.exchange_ts = fill.timestamp;
        er.recv_ts = now_ns();

        ++stats_.private_messages;
        health_.last_private_msg_ns = er.recv_ts;

        if (er_cb_) er_cb_(er);
    }

    void heartbeat() {
        gateway_.heartbeat();
        health_.last_heartbeat_ns = now_ns();
    }

    PaperExchangeGateway& gateway() noexcept {
        return gateway_;
    }

private:
    std::string venue_;
    PaperExchangeGateway gateway_;

    ExchangeConnectionState state_ = ExchangeConnectionState::Disconnected;
    ExchangeHealth health_;
    ExchangeStats stats_;
    ExchangeLimits limits_;

    std::unordered_map<std::string, ExchangeSymbolInfo> symbols_;
    std::unordered_map<std::string, int> subscribed_books_;
    std::unordered_map<std::string, bool> subscribed_trades_;

    std::vector<ExchangeBalance> balances_;
    std::vector<ExchangePosition> positions_;

    MarketDataCallback md_cb_;
    ExecutionReportCallback er_cb_;
    ExchangeStateCallback state_cb_;

    int64_t order_seq_ = 0;

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    bool is_known_symbol(const std::string& symbol) const {
        auto it = symbols_.find(symbol);
        return it != symbols_.end() && it->second.active;
    }

    double normalize_price(const std::string& symbol, double price) const {
        auto info = symbol_info(symbol);
        if (!info || price <= 0.0) return price;

        double tick = info->tick_size;
        if (tick <= 0.0) return price;

        return std::round(price / tick) * tick;
    }

    double normalize_qty(const std::string& symbol, double qty) const {
        auto info = symbol_info(symbol);
        if (!info || qty <= 0.0) return qty;

        double lot = info->lot_size;
        if (lot <= 0.0) return qty;

        double rounded = std::floor(qty / lot) * lot;
        rounded = std::max(rounded, info->min_qty);
        rounded = std::min(rounded, info->max_qty);
        return rounded;
    }

    void notify_state() {
        if (state_cb_) state_cb_(state_);
    }

    void emit_report_from_ack(const Order& order, const GatewayAck& ack) {
        if (!er_cb_) return;

        ExchangeExecutionReport er;
        er.venue = venue_;
        er.order_id = order.order_id;
        er.client_id = order.client_id;
        er.symbol = order.symbol;
        er.side = order.side;
        er.price = order.price;
        er.qty = order.qty;
        er.exchange_ts = ack.ts;
        er.recv_ts = ack.ts;
        er.status = ack.accepted ? OrderStatus::Open : OrderStatus::Rejected;
        er.message = ack.message;

        health_.last_private_msg_ns = er.recv_ts;

        er_cb_(er);
    }
};

} // namespace hft