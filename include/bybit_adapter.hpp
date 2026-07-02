#pragma once
// bybit_adapter.hpp — advanced Bybit V5 paper/live-ready adapter

#include "types.hpp"
#include "exchange_gateway.hpp"
#include "oms.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class BybitCategory : uint8_t { Spot, Linear, Inverse, Option };
enum class BybitPositionMode : uint8_t { OneWay, Hedge };
enum class BybitTimeInForce : uint8_t { GTC, IOC, FOK, PostOnly };
enum class BybitOrderFlag : uint8_t { None, ReduceOnly, CloseOnTrigger };

enum class BybitError : int32_t {
    None = 0,
    InvalidRequest = 10001,
    AuthFailed = 10003,
    RateLimited = 10006,
    OrderNotFound = 110001,
    InvalidPrice = 110003,
    InsufficientBalance = 110004,
    InvalidQuantity = 110007,
    DuplicateOrder = 110030,
    Network = -9000
};

struct BybitCapabilities {
    bool supports_post_only = true;
    bool supports_reduce_only = true;
    bool supports_batch_orders = true;
    bool supports_self_trade_prevention = true;
    bool supports_oco = false;
    bool supports_trigger_orders = true;
    bool supports_iceberg = false;
    bool supports_gtd = true;
    bool supports_position_mode = true;
};

struct BybitOrderOptions {
    BybitTimeInForce tif = BybitTimeInForce::GTC;
    BybitOrderFlag flag = BybitOrderFlag::None;
    bool reduce_only = false;
    bool close_on_trigger = false;
    int position_idx = 0;
};

struct BybitSubscription {
    std::string topic;
    std::string symbol;
    int depth = 0;
};

struct BybitAdapterConfig {
    BybitCategory category = BybitCategory::Spot;
    BybitPositionMode position_mode = BybitPositionMode::OneWay;
    std::string venue = "bybit";
    bool paper_mode = true;
    bool testnet = true;
    bool verbose = false;
};

class BybitAdapter {
public:
    BybitAdapter(
        BybitAdapterConfig cfg,
        PaperExchangeGateway* gateway = nullptr,
        OrderManager* oms = nullptr
    )
        : cfg_(std::move(cfg)), gateway_(gateway), oms_(oms) {}

    void set_gateway(PaperExchangeGateway* gateway) noexcept { gateway_ = gateway; }
    void set_oms(OrderManager* oms) noexcept { oms_ = oms; }
    void set_callbacks(AdapterCallbacks cb) { cb_ = std::move(cb); }

    bool connect() {
        connected_ = true;
        authenticated_ = cfg_.paper_mode;
        if (gateway_) gateway_->connect();
        return true;
    }

    void disconnect() {
        connected_ = false;
        authenticated_ = false;
        if (gateway_) gateway_->disconnect();
        if (cb_.on_disconnect) cb_.on_disconnect();
    }

    bool reconnect() {
        ++metrics_.reconnects;
        disconnect();
        connected_ = true;
        authenticated_ = cfg_.paper_mode;
        if (gateway_) gateway_->connect();
        if (cb_.on_reconnect) cb_.on_reconnect();
        return true;
    }

    bool is_connected() const noexcept { return connected_; }
    bool authenticated() const noexcept { return authenticated_; }

    void heartbeat() {
        last_heartbeat_ns_ = now_ns();
        if (gateway_) gateway_->heartbeat();
        if (cb_.on_heartbeat) cb_.on_heartbeat();
    }

    bool subscribe_orderbook(const std::string& internal_symbol, int depth = 50) {
        const std::string ext = symbol_to_external(internal_symbol);
        BybitSubscription sub;
        sub.topic = "orderbook." + std::to_string(depth) + "." + ext;
        sub.symbol = normalize_to_internal(ext);
        sub.depth = depth;
        subscriptions_[sub.topic] = sub;
        return true;
    }

    bool subscribe_trades(const std::string& internal_symbol) {
        const std::string ext = symbol_to_external(internal_symbol);
        BybitSubscription sub;
        sub.topic = "publicTrade." + ext;
        sub.symbol = normalize_to_internal(ext);
        subscriptions_[sub.topic] = sub;
        return true;
    }

    bool subscribe_ticker(const std::string& internal_symbol) {
        const std::string ext = symbol_to_external(internal_symbol);
        BybitSubscription sub;
        sub.topic = "tickers." + ext;
        sub.symbol = normalize_to_internal(ext);
        subscriptions_[sub.topic] = sub;
        return true;
    }

    bool unsubscribe(const std::string& internal_symbol) {
        const std::string ext = symbol_to_external(internal_symbol);
        std::vector<std::string> remove;

        for (const auto& [topic, sub] : subscriptions_) {
            if (sub.symbol == normalize_to_internal(ext) || topic.find(ext) != std::string::npos)
                remove.push_back(topic);
        }

        for (const auto& t : remove) subscriptions_.erase(t);
        return !remove.empty();
    }

    GatewayAck submit_order(Order order, BybitOrderOptions options = {}) {
        auto t0 = now_ns();

        if (!connected_)
            return reject_ack(order, AdapterError::Network, "adapter_disconnected");

        if (order.order_id.empty()) order.order_id = next_order_id();
        if (order.client_id.empty()) order.client_id = order.order_id;

        order.symbol = normalize_to_internal(order.symbol);

        auto err = validate_order(order, options);
        if (err != AdapterError::None)
            return reject_ack(order, err, adapter_error_to_str(err));

        GatewayAck ack;

        if (gateway_) {
            ack = gateway_->send_order(order);
            if (!ack.accepted)
                fire_error(gateway_to_adapter_error(ack.reject_code), ack.message);
        } else {
            ack.accepted = true;
            ack.order_id = order.order_id;
            ack.client_id = order.client_id;
            ack.venue = cfg_.venue;
            ack.ts = now_ns();

            if (oms_) {
                oms_->submit(order);
                oms_->on_ack(order, true, ack.ts);
            }
        }

        bybit_options_[order.order_id] = options;
        update_mean(metrics_.ack_mean_us, metrics_.ack_count, ns_to_us(now_ns() - t0));

        if (cb_.on_ack) cb_.on_ack(order, ack.accepted);
        return ack;
    }

    GatewayCancelAck cancel_order(const std::string& order_id) {
        auto t0 = now_ns();

        GatewayCancelAck ack;
        if (!connected_) {
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::Disconnected;
            ack.message = "adapter_disconnected";
            ack.order_id = order_id;
            ack.venue = cfg_.venue;
            ack.ts = now_ns();
            fire_error(AdapterError::Network, ack.message);
            return ack;
        }

        if (gateway_) {
            ack = gateway_->cancel_order(order_id);
        } else {
            ack.accepted = true;
            ack.order_id = order_id;
            ack.venue = cfg_.venue;
            ack.ts = now_ns();
            if (oms_) oms_->on_cancelled(order_id, ack.ts);
        }

        update_mean(metrics_.cancel_mean_us, metrics_.cancel_count, ns_to_us(now_ns() - t0));

        if (ack.accepted) {
            bybit_options_.erase(order_id);
            if (cb_.on_cancel) cb_.on_cancel(order_id);
        } else {
            fire_error(gateway_to_adapter_error(ack.reject_code), ack.message);
        }

        return ack;
    }

    GatewayReplaceAck replace_order(const std::string& order_id, double new_price, double new_qty) {
        auto t0 = now_ns();

        if (!connected_) {
            GatewayReplaceAck ack;
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::Disconnected;
            ack.message = "adapter_disconnected";
            ack.order_id = order_id;
            ack.venue = cfg_.venue;
            ack.ts = now_ns();
            fire_error(AdapterError::Network, ack.message);
            return ack;
        }

        GatewayReplaceAck ack;

        if (gateway_) {
            ack = gateway_->replace_order(order_id, new_price, new_qty);
        } else {
            ack.accepted = true;
            ack.order_id = order_id;
            ack.venue = cfg_.venue;
            ack.ts = now_ns();
            ack.new_price = new_price;
            ack.new_qty = new_qty;

            if (oms_) {
                oms_->request_replace(order_id, new_price, new_qty, ack.ts);
                oms_->on_replaced(order_id, new_price, new_qty, ack.ts);
            }
        }

        update_mean(metrics_.rest_mean_us, metrics_.rest_count, ns_to_us(now_ns() - t0));
        return ack;
    }

    int cancel_all(const std::string& symbol = "") {
        int n = 0;

        if (!gateway_) return 0;

        std::vector<std::string> ids;
        for (const auto& [id, o] : gateway_->live_orders()) {
            if (symbol.empty() || o.symbol == normalize_to_internal(symbol))
                ids.push_back(id);
        }

        for (const auto& id : ids) {
            auto ack = cancel_order(id);
            if (ack.accepted) ++n;
        }

        return n;
    }

    int batch_cancel(const std::vector<std::string>& order_ids) {
        int n = 0;
        for (const auto& id : order_ids) {
            auto ack = cancel_order(id);
            if (ack.accepted) ++n;
        }
        return n;
    }

    std::vector<GatewayAck> batch_submit(const std::vector<Order>& orders) {
        std::vector<GatewayAck> out;
        out.reserve(orders.size());

        for (auto o : orders)
            out.push_back(submit_order(std::move(o)));

        return out;
    }

    std::vector<Order> open_orders(const std::string& symbol = "") const {
        if (!gateway_) return {};

        std::vector<Order> out;
        const std::string norm = normalize_to_internal(symbol);

        for (const auto& [id, o] : gateway_->live_orders()) {
            if (symbol.empty() || o.symbol == norm)
                out.push_back(o);
        }

        return out;
    }

    std::unordered_map<std::string, double> balances() const { return balances_; }
    std::unordered_map<std::string, double> positions() const { return positions_; }
    std::vector<FillEvent> fills() const { return fills_; }

    void inject_book_update(L2Update upd) {
        upd.symbol = normalize_to_internal(upd.symbol);

        int64_t prev = last_seq_[upd.symbol];
        if (upd.seq > 0 && prev > 0 && upd.seq != prev + 1)
            ++metrics_.sequence_gaps;

        if (upd.seq > 0) last_seq_[upd.symbol] = upd.seq;

        if (cb_.on_book) cb_.on_book(upd);
        update_mean(metrics_.ws_mean_us, metrics_.ws_count, 1.0);
    }

    void inject_trade(Trade trade) {
        trade.symbol = normalize_to_internal(trade.symbol);
        if (cb_.on_trade) cb_.on_trade(trade);
        update_mean(metrics_.ws_mean_us, metrics_.ws_count, 1.0);
    }

    void inject_fill(FillEvent fill) {
        fill.symbol = normalize_to_internal(fill.symbol);
        fills_.push_back(fill);

        if (gateway_) gateway_->inject_fill(fill);
        else if (oms_) oms_->on_fill(fill);

        if (cb_.on_fill) cb_.on_fill(fill);
    }

    void update_balance(const std::string& asset, double amount) {
        balances_[asset] = amount;
    }

    void update_position(const std::string& symbol, double qty) {
        positions_[normalize_to_internal(symbol)] = qty;
    }

    bool set_position_mode(BybitPositionMode mode) {
        cfg_.position_mode = mode;
        return true;
    }

    BybitPositionMode position_mode() const noexcept {
        return cfg_.position_mode;
    }

    BybitCapabilities capabilities() const noexcept { return {}; }
    const AdapterLatencyMetrics& metrics() const noexcept { return metrics_; }
    const std::unordered_map<std::string, BybitSubscription>& subscriptions() const noexcept { return subscriptions_; }

    static std::string category_to_str(BybitCategory c) {
        switch (c) {
            case BybitCategory::Spot: return "spot";
            case BybitCategory::Linear: return "linear";
            case BybitCategory::Inverse: return "inverse";
            case BybitCategory::Option: return "option";
            default: return "spot";
        }
    }

    static std::string symbol_to_external(std::string internal_symbol) {
        return normalize_to_internal(internal_symbol);
    }

    static std::string normalize_to_internal(std::string s) {
        std::string out;
        out.reserve(s.size());

        for (char c : s) {
            if (c == '-' || c == '_' || c == '/') continue;
            out.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }

        if (out == "XBTUSDT") out = "BTCUSDT";
        if (out == "XBTUSD") out = "BTCUSD";
        return out;
    }

    static AdapterError translate_bybit_error(int code) noexcept {
        switch (static_cast<BybitError>(code)) {
            case BybitError::AuthFailed: return AdapterError::Authentication;
            case BybitError::InsufficientBalance: return AdapterError::InsufficientMargin;
            case BybitError::OrderNotFound: return AdapterError::OrderNotFound;
            case BybitError::DuplicateOrder: return AdapterError::DuplicateOrder;
            case BybitError::RateLimited: return AdapterError::RateLimited;
            case BybitError::InvalidPrice: return AdapterError::InvalidPrice;
            case BybitError::InvalidQuantity: return AdapterError::InvalidQuantity;
            case BybitError::Network: return AdapterError::Network;
            case BybitError::InvalidRequest: return AdapterError::Rejected;
            default: return AdapterError::InternalExchange;
        }
    }

private:
    BybitAdapterConfig cfg_;
    PaperExchangeGateway* gateway_ = nullptr;
    OrderManager* oms_ = nullptr;
    AdapterCallbacks cb_;

    bool connected_ = false;
    bool authenticated_ = false;
    int64_t order_counter_ = 0;
    int64_t last_heartbeat_ns_ = 0;

    std::unordered_map<std::string, BybitSubscription> subscriptions_;
    std::unordered_map<std::string, int64_t> last_seq_;
    std::unordered_map<std::string, double> balances_;
    std::unordered_map<std::string, double> positions_;
    std::unordered_map<std::string, BybitOrderOptions> bybit_options_;
    std::vector<FillEvent> fills_;

    AdapterLatencyMetrics metrics_;

    std::string next_order_id() {
        return "BY_" + std::to_string(++order_counter_);
    }

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }

    static double ns_to_us(int64_t ns) noexcept {
        return static_cast<double>(ns) / 1000.0;
    }

    static void update_mean(double& mean, int64_t& count, double sample) {
        ++count;
        mean += (sample - mean) / static_cast<double>(count);
    }

    AdapterError validate_order(const Order& o, const BybitOrderOptions& opt) const noexcept {
        if (o.symbol.empty() || o.side == Side::Unknown)
            return AdapterError::Rejected;

        if (o.qty <= 0.0 || !std::isfinite(o.qty))
            return AdapterError::InvalidQuantity;

        if (o.order_type != OrderType::Market) {
            if (o.price <= 0.0 || !std::isfinite(o.price))
                return AdapterError::InvalidPrice;
        }

        if (cfg_.category == BybitCategory::Spot && (opt.reduce_only || opt.close_on_trigger))
            return AdapterError::Rejected;

        if (cfg_.position_mode == BybitPositionMode::OneWay && opt.position_idx != 0)
            return AdapterError::Rejected;

        return AdapterError::None;
    }

    GatewayAck reject_ack(const Order& order, AdapterError err, const std::string& msg) {
        GatewayAck ack;
        ack.accepted = false;
        ack.order_id = order.order_id;
        ack.client_id = order.client_id;
        ack.venue = cfg_.venue;
        ack.message = msg;
        ack.ts = now_ns();

        switch (err) {
            case AdapterError::Network: ack.reject_code = GatewayRejectCode::Disconnected; break;
            case AdapterError::DuplicateOrder: ack.reject_code = GatewayRejectCode::DuplicateClientId; break;
            case AdapterError::InvalidPrice:
            case AdapterError::InvalidQuantity: ack.reject_code = GatewayRejectCode::InvalidOrder; break;
            default: ack.reject_code = GatewayRejectCode::ExchangeRejected; break;
        }

        fire_error(err, msg);
        if (cb_.on_ack) cb_.on_ack(order, false);
        return ack;
    }

    static AdapterError gateway_to_adapter_error(GatewayRejectCode c) noexcept {
        switch (c) {
            case GatewayRejectCode::Disconnected: return AdapterError::Network;
            case GatewayRejectCode::DuplicateClientId: return AdapterError::DuplicateOrder;
            case GatewayRejectCode::RiskRejected: return AdapterError::Rejected;
            case GatewayRejectCode::UnknownOrder: return AdapterError::OrderNotFound;
            case GatewayRejectCode::InvalidOrder: return AdapterError::Rejected;
            default: return AdapterError::InternalExchange;
        }
    }

    void fire_error(AdapterError e, const std::string& msg) {
    if (cb_.on_error) {
        AdapterErrorEvent ev;
        ev.error = e;
        ev.message = msg;
        cb_.on_error(ev);
    }
  }
};

} // namespace hft