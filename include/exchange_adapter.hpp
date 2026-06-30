#pragma once
// exchange_adapter.hpp — advanced venue-agnostic exchange adapter layer

#include "types.hpp"
#include "exchange.hpp"
#include "exchange_gateway.hpp"
#include "rest_client.hpp"
#include "session_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace hft {

enum class ExchangeErrorCode : uint8_t {
    None,
    Network,
    Authentication,
    PermissionDenied,
    RateLimited,
    InvalidPrice,
    InvalidQuantity,
    InvalidSymbol,
    SymbolNotFound,
    DuplicateOrder,
    OrderNotFound,
    InsufficientBalance,
    InsufficientMargin,
    PostOnlyWouldCross,
    ReduceOnlyViolation,
    UnsupportedFeature,
    ExchangeUnavailable,
    InternalExchange,
    Rejected,
    Unknown
};

// Backward-compatible aliases for older modules/tests.
using ExchangeError = ExchangeErrorCode;
using AdapterError = ExchangeErrorCode;

inline const char* exchange_error_to_str(ExchangeErrorCode e) noexcept {
    switch (e) {
        case ExchangeErrorCode::None: return "none";
        case ExchangeErrorCode::Network: return "network";
        case ExchangeErrorCode::Authentication: return "authentication";
        case ExchangeErrorCode::PermissionDenied: return "permission_denied";
        case ExchangeErrorCode::RateLimited: return "rate_limited";
        case ExchangeErrorCode::InvalidPrice: return "invalid_price";
        case ExchangeErrorCode::InvalidQuantity: return "invalid_quantity";
        case ExchangeErrorCode::InvalidSymbol: return "invalid_symbol";
        case ExchangeErrorCode::SymbolNotFound: return "symbol_not_found";
        case ExchangeErrorCode::DuplicateOrder: return "duplicate_order";
        case ExchangeErrorCode::OrderNotFound: return "order_not_found";
        case ExchangeErrorCode::InsufficientBalance: return "insufficient_balance";
        case ExchangeErrorCode::InsufficientMargin: return "insufficient_margin";
        case ExchangeErrorCode::PostOnlyWouldCross: return "post_only_would_cross";
        case ExchangeErrorCode::ReduceOnlyViolation: return "reduce_only_violation";
        case ExchangeErrorCode::UnsupportedFeature: return "unsupported_feature";
        case ExchangeErrorCode::ExchangeUnavailable: return "exchange_unavailable";
        case ExchangeErrorCode::InternalExchange: return "internal_exchange";
        case ExchangeErrorCode::Rejected: return "rejected";
        default: return "unknown";
    }
}

inline const char* adapter_error_to_str(AdapterError e) noexcept {
    return exchange_error_to_str(e);
}

struct AdapterCapabilities {
    bool supports_market_data = true;
    bool supports_trading = true;
    bool supports_post_only = true;
    bool supports_reduce_only = false;
    bool supports_batch_orders = false;
    bool supports_cancel_replace = true;
    bool supports_self_trade_prevention = false;
    bool supports_oco = false;
    bool supports_trigger_orders = false;
    bool supports_iceberg = false;
    bool supports_gtd = false;
    bool supports_private_stream = true;
};

struct AdapterHealth {
    ExchangeConnectionState state = ExchangeConnectionState::Disconnected;
    int64_t last_event_ns = 0;
    int64_t errors = 0;
    int64_t rejects = 0;
    double error_rate = 0.0;
    double reject_rate = 0.0;
    double avg_ack_latency_us = 0.0;
    double avg_md_latency_us = 0.0;
};

struct AdapterMetrics {
    int64_t md_events = 0;
    int64_t trade_events = 0;
    int64_t order_reports = 0;
    int64_t errors = 0;
    int64_t normalized_symbols = 0;
    int64_t submitted_orders = 0;
    int64_t cancelled_orders = 0;
    int64_t replaced_orders = 0;
    int64_t duplicate_client_ids = 0;
    int64_t validation_rejects = 0;
    double avg_rest_latency_us = 0.0;
    double avg_ack_latency_us = 0.0;
};

struct AdapterErrorEvent {
    ExchangeErrorCode error = ExchangeErrorCode::None;
    std::string venue_code;
    std::string message;

    bool ok() const noexcept {
        return error == ExchangeErrorCode::None;
    }
};

struct AdapterOrderResult {
    bool ok = false;
    ExchangeErrorCode error = ExchangeErrorCode::None;
    GatewayAck ack;
    std::string message;
};

struct AdapterCancelResult {
    bool ok = false;
    ExchangeErrorCode error = ExchangeErrorCode::None;
    GatewayCancelAck ack;
    std::string message;
};

struct AdapterReplaceResult {
    bool ok = false;
    ExchangeErrorCode error = ExchangeErrorCode::None;
    GatewayReplaceAck ack;
    std::string message;
};

using AdapterMarketCallback = std::function<void(const MarketEvent&)>;
using AdapterExecutionCallback = std::function<void(const ExchangeExecutionReport&)>;
using AdapterErrorCallback = std::function<void(const AdapterErrorEvent&)>;

class SymbolNormalizer {
public:
    void add_mapping(std::string internal_symbol, std::string external_symbol) {
        internal_to_external_[internal_symbol] = external_symbol;
        external_to_internal_[external_symbol] = internal_symbol;
    }

    std::string to_external(const std::string& internal_symbol) const {
        auto it = internal_to_external_.find(internal_symbol);
        return it == internal_to_external_.end() ? internal_symbol : it->second;
    }

    std::string to_internal(const std::string& external_symbol) const {
        auto it = external_to_internal_.find(external_symbol);
        return it == external_to_internal_.end() ? external_symbol : it->second;
    }

    bool has_internal(const std::string& s) const {
        return internal_to_external_.count(s) > 0;
    }

private:
    std::unordered_map<std::string, std::string> internal_to_external_;
    std::unordered_map<std::string, std::string> external_to_internal_;
};

class ExchangeErrorTranslator {
public:
    void add_mapping(std::string venue_code, ExchangeErrorCode error) {
        code_map_[std::move(venue_code)] = error;
    }

    ExchangeErrorCode translate(const std::string& venue_code, const std::string& message) const {
        auto it = code_map_.find(venue_code);
        if (it != code_map_.end()) return it->second;

        std::string m = message;
        std::transform(m.begin(), m.end(), m.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });

        if (venue_code == "429" || contains(m, "rate")) return ExchangeErrorCode::RateLimited;
        if (venue_code == "401" || venue_code == "403" || contains(m, "auth")) return ExchangeErrorCode::Authentication;
        if (venue_code == "404" || contains(m, "not found")) return ExchangeErrorCode::OrderNotFound;
        if (contains(m, "price")) return ExchangeErrorCode::InvalidPrice;
        if (contains(m, "quantity") || contains(m, "qty")) return ExchangeErrorCode::InvalidQuantity;
        if (contains(m, "symbol")) return ExchangeErrorCode::InvalidSymbol;
        if (contains(m, "balance")) return ExchangeErrorCode::InsufficientBalance;
        if (contains(m, "margin")) return ExchangeErrorCode::InsufficientMargin;
        if (contains(m, "duplicate")) return ExchangeErrorCode::DuplicateOrder;
        if (contains(m, "post") && contains(m, "cross")) return ExchangeErrorCode::PostOnlyWouldCross;
        if (contains(m, "reduce")) return ExchangeErrorCode::ReduceOnlyViolation;
        if (contains(m, "unavailable")) return ExchangeErrorCode::ExchangeUnavailable;

        return ExchangeErrorCode::Unknown;
    }

private:
    std::unordered_map<std::string, ExchangeErrorCode> code_map_;

    static bool contains(const std::string& haystack, const std::string& needle) {
        return haystack.find(needle) != std::string::npos;
    }
};

class IExchangeAdapter {
public:
    virtual ~IExchangeAdapter() = default;

    virtual const std::string& venue() const = 0;
    virtual ExchangeKind kind() const = 0;
    virtual AdapterCapabilities capabilities() const = 0;
    virtual AdapterMetrics metrics() const = 0;
    virtual AdapterHealth health() const = 0;

    virtual bool connect() = 0;
    virtual void disconnect() = 0;
    virtual bool authenticate(const ExchangeCredentials& credentials) = 0;
    virtual bool recover() = 0;

    virtual bool subscribe_orderbook(const std::string& symbol, int depth = 50) = 0;
    virtual bool subscribe_trades(const std::string& symbol) = 0;
    virtual bool unsubscribe(const std::string& symbol) = 0;

    virtual AdapterOrderResult submit_order(const ExchangeOrderRequest& req) = 0;
    virtual AdapterCancelResult cancel_order(const ExchangeCancelRequest& req) = 0;
    virtual AdapterReplaceResult replace_order(const ExchangeReplaceRequest& req) = 0;
    virtual int cancel_all(const std::string& symbol = "") = 0;

    virtual std::vector<ExchangeBalance> balances() const = 0;
    virtual std::vector<ExchangePosition> positions() const = 0;
    virtual std::vector<Order> open_orders(const std::string& symbol = "") const = 0;

    virtual std::string to_external_symbol(const std::string& internal_symbol) const = 0;
    virtual std::string to_internal_symbol(const std::string& external_symbol) const = 0;

    virtual ExchangeErrorCode translate_error(const std::string& venue_code, const std::string& message) const = 0;

    virtual void set_market_callback(AdapterMarketCallback cb) = 0;
    virtual void set_execution_callback(AdapterExecutionCallback cb) = 0;
    virtual void set_error_callback(AdapterErrorCallback cb) = 0;
};

class GenericExchangeAdapter : public IExchangeAdapter {
public:
    explicit GenericExchangeAdapter(IExchange& exchange)
        : exchange_(exchange)
    {
        install_default_error_mappings();

        exchange_.set_market_data_callback([this](const MarketEvent& e) {
            ++metrics_.md_events;
            health_.last_event_ns = now_ns();

            auto out = normalize_market_event(e);
            if (std::holds_alternative<Trade>(out)) ++metrics_.trade_events;

            if (market_cb_) market_cb_(out);
        });

        exchange_.set_execution_report_callback([this](const ExchangeExecutionReport& r) {
            ++metrics_.order_reports;
            health_.last_event_ns = now_ns();

            auto out = normalize_report(r);
            update_reject_rate(out.status != OrderStatus::Rejected);

            if (exec_cb_) exec_cb_(out);
        });

        exchange_.set_state_callback([this](ExchangeConnectionState s) {
            health_.state = s;
            health_.last_event_ns = now_ns();
        });
    }

    const std::string& venue() const override {
        return exchange_.venue();
    }

    ExchangeKind kind() const override {
        return exchange_.kind();
    }

    AdapterCapabilities capabilities() const override {
        AdapterCapabilities c;
        c.supports_market_data = exchange_.has_capability(ExchangeCapability::MarketData);
        c.supports_trading = exchange_.has_capability(ExchangeCapability::Trading);
        c.supports_cancel_replace = exchange_.has_capability(ExchangeCapability::CancelReplace);
        c.supports_batch_orders = exchange_.has_capability(ExchangeCapability::BatchOrders);
        c.supports_private_stream = exchange_.has_capability(ExchangeCapability::PrivateStream);
        return c;
    }

    AdapterMetrics metrics() const override { return metrics_; }
    AdapterHealth health() const override { return health_; }

    bool connect() override {
        bool ok = exchange_.connect();
        health_.state = exchange_.state();
        return ok;
    }

    void disconnect() override {
        exchange_.disconnect();
        health_.state = exchange_.state();
    }

    bool authenticate(const ExchangeCredentials& credentials) override {
        bool ok = exchange_.authenticate(credentials);
        if (!ok) emit_error(ExchangeErrorCode::Authentication, "auth_failed", "authentication failed");
        return ok;
    }

    bool recover() override {
        bool ok = exchange_.recover();
        health_.state = exchange_.state();
        return ok;
    }

    bool subscribe_orderbook(const std::string& symbol, int depth = 50) override {
        if (!capabilities().supports_market_data) {
            emit_error(ExchangeErrorCode::UnsupportedFeature, "unsupported", "market data unsupported");
            return false;
        }
        return exchange_.subscribe_book(normalizer_.to_external(symbol), depth);
    }

    bool subscribe_trades(const std::string& symbol) override {
        if (!capabilities().supports_market_data) {
            emit_error(ExchangeErrorCode::UnsupportedFeature, "unsupported", "market data unsupported");
            return false;
        }
        return exchange_.subscribe_trades(normalizer_.to_external(symbol));
    }

    bool unsubscribe(const std::string& symbol) override {
        return exchange_.unsubscribe(normalizer_.to_external(symbol));
    }

    AdapterOrderResult submit_order(const ExchangeOrderRequest& req) override {
        ++metrics_.submitted_orders;

        auto valid = validate_order(req);
        if (!valid.ok()) {
            ++metrics_.validation_rejects;
            emit_error(valid.error, valid.venue_code, valid.message);
            return AdapterOrderResult{false, valid.error, {}, valid.message};
        }

        ExchangeOrderRequest r = req;
        r.symbol = normalizer_.to_external(req.symbol);

        if (!r.client_id.empty()) {
            if (!seen_client_ids_.insert(r.client_id).second) {
                ++metrics_.duplicate_client_ids;
                emit_error(ExchangeErrorCode::DuplicateOrder, "duplicate_client_id", "duplicate client id");
                return AdapterOrderResult{false, ExchangeErrorCode::DuplicateOrder, {}, "duplicate_client_id"};
            }
        }

        auto t0 = std::chrono::high_resolution_clock::now();
        GatewayAck ack = exchange_.submit_order(r);
        update_latency(t0, metrics_.avg_ack_latency_us, health_.avg_ack_latency_us);

        AdapterOrderResult out;
        out.ok = ack.accepted;
        out.ack = ack;
        out.message = ack.message;
        out.error = ack.accepted ? ExchangeErrorCode::None : translate_gateway_error(ack.reject_code, ack.message);

        update_reject_rate(out.ok);

        if (!out.ok)
            emit_error(out.error, std::to_string(static_cast<int>(ack.reject_code)), ack.message);

        return out;
    }

    AdapterCancelResult cancel_order(const ExchangeCancelRequest& req) override {
        ++metrics_.cancelled_orders;

        ExchangeCancelRequest r = req;
        r.symbol = normalizer_.to_external(req.symbol);

        auto t0 = std::chrono::high_resolution_clock::now();
        GatewayCancelAck ack = exchange_.cancel_order(r);
        update_latency(t0, metrics_.avg_ack_latency_us, health_.avg_ack_latency_us);

        AdapterCancelResult out;
        out.ok = ack.accepted;
        out.ack = ack;
        out.message = ack.message;
        out.error = ack.accepted ? ExchangeErrorCode::None : translate_gateway_error(ack.reject_code, ack.message);

        update_reject_rate(out.ok);

        if (!out.ok)
            emit_error(out.error, std::to_string(static_cast<int>(ack.reject_code)), ack.message);

        return out;
    }

    AdapterReplaceResult replace_order(const ExchangeReplaceRequest& req) override {
        ++metrics_.replaced_orders;

        if (!capabilities().supports_cancel_replace) {
            emit_error(ExchangeErrorCode::UnsupportedFeature, "unsupported", "replace unsupported");
            return AdapterReplaceResult{false, ExchangeErrorCode::UnsupportedFeature, {}, "replace_unsupported"};
        }

        if (req.new_price <= 0.0 || req.new_qty <= 0.0) {
            ++metrics_.validation_rejects;
            return AdapterReplaceResult{false, ExchangeErrorCode::InvalidQuantity, {}, "invalid_replace"};
        }

        ExchangeReplaceRequest r = req;
        r.symbol = normalizer_.to_external(req.symbol);

        auto t0 = std::chrono::high_resolution_clock::now();
        GatewayReplaceAck ack = exchange_.replace_order(r);
        update_latency(t0, metrics_.avg_ack_latency_us, health_.avg_ack_latency_us);

        AdapterReplaceResult out;
        out.ok = ack.accepted;
        out.ack = ack;
        out.message = ack.message;
        out.error = ack.accepted ? ExchangeErrorCode::None : translate_gateway_error(ack.reject_code, ack.message);

        update_reject_rate(out.ok);

        if (!out.ok)
            emit_error(out.error, std::to_string(static_cast<int>(ack.reject_code)), ack.message);

        return out;
    }

    int cancel_all(const std::string& symbol = "") override {
        return exchange_.cancel_all(symbol.empty() ? "" : normalizer_.to_external(symbol));
    }

    std::vector<ExchangeBalance> balances() const override {
        return exchange_.balances();
    }

    std::vector<ExchangePosition> positions() const override {
        auto p = exchange_.positions();
        for (auto& x : p) x.symbol = normalizer_.to_internal(x.symbol);
        return p;
    }

    std::vector<Order> open_orders(const std::string& symbol = "") const override {
        auto orders = exchange_.open_orders(symbol.empty() ? "" : normalizer_.to_external(symbol));
        for (auto& o : orders) o.symbol = normalizer_.to_internal(o.symbol);
        return orders;
    }

    std::string to_external_symbol(const std::string& internal_symbol) const override {
        return normalizer_.to_external(internal_symbol);
    }

    std::string to_internal_symbol(const std::string& external_symbol) const override {
        return normalizer_.to_internal(external_symbol);
    }

    ExchangeErrorCode translate_error(const std::string& venue_code, const std::string& message) const override {
        return translator_.translate(venue_code, message);
    }

    void set_market_callback(AdapterMarketCallback cb) override {
        market_cb_ = std::move(cb);
    }

    void set_execution_callback(AdapterExecutionCallback cb) override {
        exec_cb_ = std::move(cb);
    }

    void set_error_callback(AdapterErrorCallback cb) override {
        error_cb_ = std::move(cb);
    }

    void add_symbol_mapping(const std::string& internal_symbol, const std::string& external_symbol) {
        normalizer_.add_mapping(internal_symbol, external_symbol);
    }

    void add_error_mapping(const std::string& venue_code, ExchangeErrorCode error) {
        translator_.add_mapping(venue_code, error);
    }

protected:
    IExchange& exchange_;
    SymbolNormalizer normalizer_;
    ExchangeErrorTranslator translator_;

    AdapterMetrics metrics_;
    AdapterHealth health_;

    AdapterMarketCallback market_cb_;
    AdapterExecutionCallback exec_cb_;
    AdapterErrorCallback error_cb_;

    std::unordered_set<std::string> seen_client_ids_;

    void install_default_error_mappings() {
        translator_.add_mapping("-2010", ExchangeErrorCode::ExchangeUnavailable);
        translator_.add_mapping("-2011", ExchangeErrorCode::OrderNotFound);
        translator_.add_mapping("-1013", ExchangeErrorCode::InvalidQuantity);
        translator_.add_mapping("-1021", ExchangeErrorCode::Authentication);
        translator_.add_mapping("10001", ExchangeErrorCode::InvalidQuantity);
        translator_.add_mapping("10003", ExchangeErrorCode::Authentication);
        translator_.add_mapping("10006", ExchangeErrorCode::RateLimited);
        translator_.add_mapping("110001", ExchangeErrorCode::OrderNotFound);
        translator_.add_mapping("51008", ExchangeErrorCode::InsufficientBalance);
    }

    AdapterErrorEvent validate_order(const ExchangeOrderRequest& req) const {
        if (!capabilities().supports_trading)
            return {ExchangeErrorCode::UnsupportedFeature, "unsupported", "trading unsupported"};

        if (req.symbol.empty())
            return {ExchangeErrorCode::InvalidSymbol, "invalid_symbol", "empty symbol"};

        if (req.side == Side::Unknown)
            return {ExchangeErrorCode::InvalidQuantity, "invalid_side", "unknown side"};

        if (req.qty <= 0.0 || !std::isfinite(req.qty))
            return {ExchangeErrorCode::InvalidQuantity, "invalid_qty", "invalid qty"};

        if (req.type != OrderType::Market && (req.price <= 0.0 || !std::isfinite(req.price)))
            return {ExchangeErrorCode::InvalidPrice, "invalid_price", "invalid price"};

        if (req.post_only && !capabilities().supports_post_only)
            return {ExchangeErrorCode::UnsupportedFeature, "unsupported", "post only unsupported"};

        if (req.reduce_only && !capabilities().supports_reduce_only)
            return {ExchangeErrorCode::UnsupportedFeature, "unsupported", "reduce only unsupported"};

        auto info = exchange_.symbol_info(normalizer_.to_external(req.symbol));
        if (info) {
            if (!on_grid(req.qty, info->lot_size))
                return {ExchangeErrorCode::InvalidQuantity, "lot_size", "quantity not on lot grid"};

            if (req.type != OrderType::Market && !on_grid(req.price, info->tick_size))
                return {ExchangeErrorCode::InvalidPrice, "tick_size", "price not on tick grid"};

            if (req.qty < info->min_qty || req.qty > info->max_qty)
                return {ExchangeErrorCode::InvalidQuantity, "qty_bounds", "qty outside bounds"};

            double notional = req.qty * std::max(req.price, 1.0);
            if (notional < info->min_notional)
                return {ExchangeErrorCode::InvalidQuantity, "min_notional", "notional below minimum"};
        }

        return {};
    }

    static bool on_grid(double x, double grid) noexcept {
        if (grid <= 0.0) return true;
        double n = x / grid;
        return std::abs(n - std::round(n)) < 1e-6;
    }

    ExchangeErrorCode translate_gateway_error(GatewayRejectCode code, const std::string& message) const {
        switch (code) {
            case GatewayRejectCode::None: return ExchangeErrorCode::None;
            case GatewayRejectCode::Disconnected: return ExchangeErrorCode::Network;
            case GatewayRejectCode::Halted: return ExchangeErrorCode::ExchangeUnavailable;
            case GatewayRejectCode::DuplicateClientId: return ExchangeErrorCode::DuplicateOrder;
            case GatewayRejectCode::RiskRejected: return ExchangeErrorCode::InternalExchange;
            case GatewayRejectCode::UnknownOrder: return ExchangeErrorCode::OrderNotFound;
            case GatewayRejectCode::InvalidOrder: return translator_.translate("400", message);
            case GatewayRejectCode::PriceBand: return ExchangeErrorCode::InvalidPrice;
            case GatewayRejectCode::ExchangeRejected: return translator_.translate("exchange", message);
            default: return ExchangeErrorCode::Unknown;
        }
    }

    void emit_error(ExchangeErrorCode e, const std::string& code, const std::string& msg) {
        ++metrics_.errors;
        ++health_.errors;
        update_error_rate(false);
        if (error_cb_) error_cb_(AdapterErrorEvent{e, code, msg});
    }

    MarketEvent normalize_market_event(const MarketEvent& e) {
        MarketEvent out = e;
        auto t0 = std::chrono::high_resolution_clock::now();

        std::visit([this](auto& ev) {
            ev.symbol = normalizer_.to_internal(ev.symbol);
            ++metrics_.normalized_symbols;
        }, out);

        update_latency(t0, metrics_.avg_rest_latency_us, health_.avg_md_latency_us);
        return out;
    }

    ExchangeExecutionReport normalize_report(ExchangeExecutionReport r) {
        r.symbol = normalizer_.to_internal(r.symbol);
        ++metrics_.normalized_symbols;
        return r;
    }

    void update_reject_rate(bool accepted) {
        double x = accepted ? 0.0 : 1.0;
        health_.reject_rate = health_.reject_rate * 0.98 + x * 0.02;
        if (!accepted) {
            ++health_.rejects;
            update_error_rate(false);
        }
    }

    void update_error_rate(bool ok) {
        double x = ok ? 0.0 : 1.0;
        health_.error_rate = health_.error_rate * 0.98 + x * 0.02;
    }

    static void update_latency(
        std::chrono::high_resolution_clock::time_point t0,
        double& metric,
        double& health_metric
    ) {
        auto t1 = std::chrono::high_resolution_clock::now();
        double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        metric = metric * 0.98 + us * 0.02;
        health_metric = health_metric * 0.98 + us * 0.02;
    }

    static int64_t now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count();
    }
};

class PaperExchangeAdapter final : public GenericExchangeAdapter {
public:
    explicit PaperExchangeAdapter(PaperExchange& ex)
        : GenericExchangeAdapter(ex)
        , paper_(ex)
    {}

    PaperExchange& paper_exchange() noexcept {
        return paper_;
    }

    AdapterCapabilities capabilities() const override {
        AdapterCapabilities c = GenericExchangeAdapter::capabilities();
        c.supports_market_data = true;
        c.supports_trading = true;
        c.supports_cancel_replace = true;
        c.supports_post_only = true;
        return c;
    }

    bool subscribe_orderbook(const std::string& symbol, int depth = 50) override {
        (void)symbol;
        (void)depth;
        return true;
    }

    bool subscribe_trades(const std::string& symbol) override {
        (void)symbol;
        return true;
    }

private:
    PaperExchange& paper_;
};

} // namespace hft