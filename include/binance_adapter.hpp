#pragma once
// binance_adapter.hpp — advanced Binance adapter: paper/live-ready, filters, rate limits, metrics

#include "types.hpp"
#include "exchange_adapter.hpp"
#include "exchange_gateway.hpp"
#include "oms.hpp"
#include "risk_gateway.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hft {

enum class BinanceMarketType : uint8_t { Spot, FuturesUSDM };
enum class AdapterMode : uint8_t { Paper, LiveReady };

struct BinanceSymbolFilter {
    std::string symbol;
    double tick_size = 0.01;
    double lot_size = 0.000001;
    double min_qty = 0.0;
    double max_qty = 1e18;
    double min_notional = 0.0;
    double max_notional = 1e18;
};

struct BinanceCapabilities {
    bool supports_post_only = true;
    bool supports_reduce_only = true;
    bool supports_batch_orders = true;
    bool supports_self_trade_prevention = true;
    bool supports_oco = true;
    bool supports_trigger_orders = true;
    bool supports_iceberg = true;
    bool supports_gtd = true;
};

struct BinanceAdapterConfig {
    BinanceMarketType market_type = BinanceMarketType::Spot;
    AdapterMode mode = AdapterMode::Paper;
    std::string venue = "binance";
    bool testnet = true;
    bool verbose = false;
    int64_t max_orders_per_second = 20;
    int64_t max_cancel_per_second = 20;
};

struct AdapterLatencyMetrics {
    double rest_mean_us = 0.0;
    double ws_mean_us = 0.0;
    double ack_mean_us = 0.0;
    double cancel_mean_us = 0.0;
    double replace_mean_us = 0.0;
    int64_t rest_count = 0;
    int64_t ws_count = 0;
    int64_t ack_count = 0;
    int64_t cancel_count = 0;
    int64_t replace_count = 0;
    int64_t sequence_gaps = 0;
    int64_t reconnects = 0;
};

struct BinanceAdapterStats {
    int64_t submitted_orders = 0;
    int64_t accepted_orders = 0;
    int64_t rejected_orders = 0;
    int64_t cancelled_orders = 0;
    int64_t replaced_orders = 0;
    int64_t fills = 0;
    int64_t book_events = 0;
    int64_t trade_events = 0;
    int64_t errors = 0;
};

struct AdapterCallbacks {
    std::function<void(const L2Update&)> on_book;
    std::function<void(const Trade&)> on_trade;
    std::function<void(const Order&, bool)> on_ack;
    std::function<void(const FillEvent&)> on_fill;
    std::function<void(const std::string&)> on_cancel;
    std::function<void(const AdapterErrorEvent&)> on_error;
    std::function<void()> on_disconnect;
    std::function<void()> on_reconnect;
    std::function<void()> on_heartbeat;
};

class BinanceAdapter {
public:
    BinanceAdapter(
        BinanceAdapterConfig cfg = {},
        PaperExchangeGateway* gateway = nullptr,
        OrderManager* oms = nullptr
    )
        : cfg_(std::move(cfg)), gateway_(gateway), oms_(oms) {}

    void set_gateway(PaperExchangeGateway* g) noexcept { gateway_ = g; }
    void set_oms(OrderManager* o) noexcept { oms_ = o; }
    void set_callbacks(AdapterCallbacks cb) { cb_ = std::move(cb); }

    bool connect() {
        connected_ = true;
        authenticated_ = (cfg_.mode == AdapterMode::Paper);
        if (gateway_) gateway_->connect();
        return true;
    }

    void disconnect() {
        connected_ = false;
        authenticated_ = false;
        if (gateway_) gateway_->disconnect();
        safe_call([&]{ if (cb_.on_disconnect) cb_.on_disconnect(); });
    }

    bool reconnect() {
        ++latency_.reconnects;
        disconnect();
        connected_ = true;
        authenticated_ = (cfg_.mode == AdapterMode::Paper);
        if (gateway_) gateway_->connect();
        safe_call([&]{ if (cb_.on_reconnect) cb_.on_reconnect(); });
        return true;
    }

    bool is_connected() const noexcept { return connected_; }
    bool authenticated() const noexcept { return authenticated_; }

    void heartbeat() {
        last_heartbeat_ns_ = now_ns();
        if (gateway_) gateway_->heartbeat();
        safe_call([&]{ if (cb_.on_heartbeat) cb_.on_heartbeat(); });
    }

    void set_symbol_filter(BinanceSymbolFilter f) {
        f.symbol = normalize_to_internal(f.symbol);
        filters_[f.symbol] = std::move(f);
    }

    BinanceSymbolFilter symbol_filter(const std::string& symbol) const {
        auto s = normalize_to_internal(symbol);
        auto it = filters_.find(s);
        if (it != filters_.end()) return it->second;
        return BinanceSymbolFilter{.symbol = s};
    }

    bool subscribe_orderbook(const std::string& symbol, int depth = 20) {
        subscriptions_[normalize_to_internal(symbol) + "|book"] = depth;
        return true;
    }

    bool subscribe_trades(const std::string& symbol) {
        subscriptions_[normalize_to_internal(symbol) + "|trades"] = 1;
        return true;
    }

    bool subscribe_ticker(const std::string& symbol) {
        subscriptions_[normalize_to_internal(symbol) + "|ticker"] = 1;
        return true;
    }

    bool unsubscribe(const std::string& symbol) {
        erase_prefix(normalize_to_internal(symbol) + "|");
        return true;
    }

    GatewayAck submit_order(Order order) {
        ++stats_.submitted_orders;
        auto t0 = now_ns();

        if (!connected_) return reject_ack(order, AdapterError::Network, "not_connected", "not_connected");

        if (!rate_limit(order_bucket_, cfg_.max_orders_per_second, t0))
            return reject_ack(order, AdapterError::RateLimited, "order_rate_limit", "rate_limit");

        if (order.order_id.empty()) order.order_id = next_order_id();
        if (order.client_id.empty()) order.client_id = order.order_id;

        order.symbol = normalize_to_internal(order.symbol);

        auto err = validate_order(order);
        if (err != AdapterError::None)
            return reject_ack(order, err, adapter_error_to_str(err), "validation");

        if (!seen_client_ids_.insert(order.client_id).second)
            return reject_ack(order, AdapterError::DuplicateOrder, "duplicate_client_id", "duplicate_client_id");

        GatewayAck ack;

        if (gateway_) {
            ack = gateway_->send_order(order);
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

        if (ack.accepted) {
            ++stats_.accepted_orders;
        } else {
            ++stats_.rejected_orders;
            fire_error(gateway_to_adapter_error(ack.reject_code), ack.message, std::to_string(static_cast<int>(ack.reject_code)));
        }

        update_mean(latency_.ack_mean_us, latency_.ack_count, ns_to_us(now_ns() - t0));
        safe_call([&]{ if (cb_.on_ack) cb_.on_ack(order, ack.accepted); });
        return ack;
    }

    GatewayCancelAck cancel_order(const std::string& order_id) {
        auto t0 = now_ns();

        GatewayCancelAck ack;
        ack.order_id = order_id;
        ack.venue = cfg_.venue;
        ack.ts = now_ns();

        if (!connected_) {
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::Disconnected;
            ack.message = "not_connected";
            fire_error(AdapterError::Network, ack.message, "not_connected");
            return ack;
        }

        if (!rate_limit(cancel_bucket_, cfg_.max_cancel_per_second, t0)) {
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::ExchangeRejected;
            ack.message = "cancel_rate_limit";
            fire_error(AdapterError::RateLimited, ack.message, "rate_limit");
            return ack;
        }

        if (gateway_) {
            ack = gateway_->cancel_order(order_id);
        } else {
            ack.accepted = true;
            if (oms_) oms_->on_cancelled(order_id, ack.ts);
        }

        if (ack.accepted) {
            ++stats_.cancelled_orders;
            safe_call([&]{ if (cb_.on_cancel) cb_.on_cancel(order_id); });
        } else {
            fire_error(gateway_to_adapter_error(ack.reject_code), ack.message, std::to_string(static_cast<int>(ack.reject_code)));
        }

        update_mean(latency_.cancel_mean_us, latency_.cancel_count, ns_to_us(now_ns() - t0));
        return ack;
    }

    GatewayReplaceAck replace_order(const std::string& order_id, double new_price, double new_qty) {
        auto t0 = now_ns();

        GatewayReplaceAck ack;
        ack.order_id = order_id;
        ack.venue = cfg_.venue;
        ack.ts = now_ns();
        ack.new_price = new_price;
        ack.new_qty = new_qty;

        if (!connected_) {
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::Disconnected;
            ack.message = "not_connected";
            fire_error(AdapterError::Network, ack.message, "not_connected");
            return ack;
        }

        if (new_price <= 0.0 || !std::isfinite(new_price)) {
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::InvalidOrder;
            ack.message = "invalid_price";
            fire_error(AdapterError::InvalidPrice, ack.message, "invalid_price");
            return ack;
        }

        if (new_qty <= 0.0 || !std::isfinite(new_qty)) {
            ack.accepted = false;
            ack.reject_code = GatewayRejectCode::InvalidOrder;
            ack.message = "invalid_qty";
            fire_error(AdapterError::InvalidQuantity, ack.message, "invalid_qty");
            return ack;
        }

        if (gateway_) {
            ack = gateway_->replace_order(order_id, new_price, new_qty);
        } else {
            ack.accepted = true;

            if (oms_) {
                oms_->request_replace(order_id, new_price, new_qty, ack.ts);
                oms_->on_replaced(order_id, new_price, new_qty, ack.ts);
            }
        }

        if (ack.accepted) {
            ++stats_.replaced_orders;
        } else {
            fire_error(gateway_to_adapter_error(ack.reject_code), ack.message, std::to_string(static_cast<int>(ack.reject_code)));
        }

        update_mean(latency_.replace_mean_us, latency_.replace_count, ns_to_us(now_ns() - t0));
        return ack;
    }

    int cancel_all(const std::string& symbol = "") {
        if (!gateway_) return 0;

        int n = 0;
        std::vector<std::string> ids;
        std::string sym = normalize_to_internal(symbol);

        for (const auto& [id, o] : gateway_->live_orders()) {
            if (sym.empty() || o.symbol == sym) ids.push_back(id);
        }

        for (const auto& id : ids) {
            if (cancel_order(id).accepted) ++n;
        }

        return n;
    }

    std::vector<Order> open_orders(const std::string& symbol = "") const {
        std::vector<Order> out;
        if (!gateway_) return out;

        std::string sym = normalize_to_internal(symbol);

        for (const auto& [id, o] : gateway_->live_orders()) {
            if (sym.empty() || o.symbol == sym) out.push_back(o);
        }

        return out;
    }

    void update_balance(const std::string& asset, double value) {
        balances_[asset] = value;
    }

    void update_position(const std::string& symbol, double value) {
        positions_[normalize_to_internal(symbol)] = value;
    }

    std::unordered_map<std::string, double> balances() const { return balances_; }
    std::unordered_map<std::string, double> positions() const { return positions_; }
    std::vector<FillEvent> fills() const { return fills_; }

    void inject_book_update(L2Update upd) {
        upd.symbol = normalize_to_internal(upd.symbol);

        auto& prev = last_seq_[upd.symbol];
        if (upd.seq > 0 && prev > 0 && upd.seq != prev + 1) ++latency_.sequence_gaps;
        if (upd.seq > 0) prev = upd.seq;

        ++stats_.book_events;
        update_mean(latency_.ws_mean_us, latency_.ws_count, 1.0);

        safe_call([&]{ if (cb_.on_book) cb_.on_book(upd); });
    }

    void inject_trade(Trade trade) {
        trade.symbol = normalize_to_internal(trade.symbol);
        ++stats_.trade_events;
        update_mean(latency_.ws_mean_us, latency_.ws_count, 1.0);

        safe_call([&]{ if (cb_.on_trade) cb_.on_trade(trade); });
    }

    void inject_fill(FillEvent fill) {
        fill.symbol = normalize_to_internal(fill.symbol);
        fills_.push_back(fill);
        ++stats_.fills;

        if (gateway_) gateway_->inject_fill(fill);
        else if (oms_) oms_->on_fill(fill);

        safe_call([&]{ if (cb_.on_fill) cb_.on_fill(fill); });
    }

    BinanceCapabilities capabilities() const noexcept { return {}; }
    const AdapterLatencyMetrics& metrics() const noexcept { return latency_; }
    const BinanceAdapterStats& stats() const noexcept { return stats_; }

    static std::string symbol_to_external(std::string internal_symbol) {
        return normalize_to_internal(std::move(internal_symbol));
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

    static ExchangeErrorCode translate_binance_error(int code) noexcept {
        switch (code) {
            case -1013: return AdapterError::InvalidPrice;
            case -1014: return AdapterError::InvalidQuantity;
            case -2010: return AdapterError::ExchangeUnavailable;
            case -2011: return AdapterError::OrderNotFound;
            case -2019: return AdapterError::InsufficientMargin;
            case -1003: return AdapterError::RateLimited;
            case -1022: return AdapterError::Authentication;
            default: return AdapterError::InternalExchange;
        }
    }

private:
    struct Bucket {
        int64_t window_start_ns = 0;
        int64_t count = 0;
    };

    BinanceAdapterConfig cfg_;
    PaperExchangeGateway* gateway_ = nullptr;
    OrderManager* oms_ = nullptr;
    AdapterCallbacks cb_;

    bool connected_ = false;
    bool authenticated_ = false;

    int64_t order_counter_ = 0;
    int64_t last_heartbeat_ns_ = 0;

    Bucket order_bucket_;
    Bucket cancel_bucket_;

    std::unordered_set<std::string> seen_client_ids_;
    std::unordered_map<std::string, BinanceSymbolFilter> filters_;
    std::unordered_map<std::string, int> subscriptions_;
    std::unordered_map<std::string, int64_t> last_seq_;
    std::unordered_map<std::string, double> balances_;
    std::unordered_map<std::string, double> positions_;
    std::vector<FillEvent> fills_;

    AdapterLatencyMetrics latency_;
    BinanceAdapterStats stats_;

    std::string next_order_id() {
        return "BN_" + std::to_string(++order_counter_);
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

    static bool multiple_of(double x, double step) noexcept {
        if (step <= 0.0) return true;
        double n = x / step;
        return std::abs(n - std::round(n)) < 1e-7;
    }

    bool rate_limit(Bucket& b, int64_t max_ops, int64_t now) {
        if (max_ops <= 0) return false;

        constexpr int64_t W = 1'000'000'000LL;

        if (b.window_start_ns == 0 || now - b.window_start_ns >= W) {
            b.window_start_ns = now;
            b.count = 0;
        }

        if (b.count >= max_ops) return false;

        ++b.count;
        return true;
    }

    void erase_prefix(const std::string& prefix) {
        std::vector<std::string> remove;

        for (const auto& [k, v] : subscriptions_) {
            if (k.rfind(prefix, 0) == 0) remove.push_back(k);
        }

        for (const auto& k : remove) subscriptions_.erase(k);
    }

    ExchangeErrorCode validate_order(const Order& o) const {
        if (o.symbol.empty()) return AdapterError::SymbolNotFound;
        if (o.side == Side::Unknown) return AdapterError::Rejected;

        if (o.qty <= 0.0 || !std::isfinite(o.qty))
            return AdapterError::InvalidQuantity;

        auto f = symbol_filter(o.symbol);

        if (o.qty < f.min_qty || o.qty > f.max_qty || !multiple_of(o.qty, f.lot_size))
            return AdapterError::InvalidQuantity;

        if (o.order_type != OrderType::Market) {
            if (o.price <= 0.0 || !std::isfinite(o.price))
                return AdapterError::InvalidPrice;

            if (!multiple_of(o.price, f.tick_size))
                return AdapterError::InvalidPrice;

            double notional = o.price * o.qty;
            if (notional < f.min_notional || notional > f.max_notional)
                return AdapterError::Rejected;
        }

        return AdapterError::None;
    }

    GatewayAck reject_ack(
        const Order& order,
        AdapterError err,
        const std::string& msg,
        const std::string& code
    ) {
        ++stats_.rejected_orders;

        GatewayAck ack;
        ack.accepted = false;
        ack.order_id = order.order_id;
        ack.client_id = order.client_id;
        ack.venue = cfg_.venue;
        ack.message = msg;
        ack.ts = now_ns();

        switch (err) {
            case AdapterError::Network:
                ack.reject_code = GatewayRejectCode::Disconnected;
                break;
            case AdapterError::DuplicateOrder:
                ack.reject_code = GatewayRejectCode::DuplicateClientId;
                break;
            case AdapterError::InvalidPrice:
            case AdapterError::InvalidQuantity:
            case AdapterError::InvalidSymbol:
            case AdapterError::SymbolNotFound:
                ack.reject_code = GatewayRejectCode::InvalidOrder;
                break;
            default:
                ack.reject_code = GatewayRejectCode::ExchangeRejected;
                break;
        }

        fire_error(err, msg, code);
        safe_call([&]{ if (cb_.on_ack) cb_.on_ack(order, false); });

        return ack;
    }

    static ExchangeErrorCode gateway_to_adapter_error(GatewayRejectCode c) noexcept {
        switch (c) {
            case GatewayRejectCode::Disconnected: return AdapterError::Network;
            case GatewayRejectCode::DuplicateClientId: return AdapterError::DuplicateOrder;
            case GatewayRejectCode::RiskRejected: return AdapterError::Rejected;
            case GatewayRejectCode::UnknownOrder: return AdapterError::OrderNotFound;
            case GatewayRejectCode::InvalidOrder: return AdapterError::Rejected;
            case GatewayRejectCode::Halted: return AdapterError::ExchangeUnavailable;
            default: return AdapterError::InternalExchange;
        }
    }

    void fire_error(AdapterError e, const std::string& msg, const std::string& code = "adapter") {
        ++stats_.errors;
        safe_call([&] {
            if (cb_.on_error) cb_.on_error(AdapterErrorEvent{e, code, msg});
        });
    }

    template<typename Fn>
    void safe_call(Fn&& fn) const noexcept {
        try {
            fn();
        } catch (...) {
        }
    }
};

} // namespace hft