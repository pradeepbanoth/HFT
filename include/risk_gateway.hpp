#pragma once
// risk_gateway.hpp — high-performance pre-trade risk gateway

#include "types.hpp"
#include "portfolio.hpp"
#include "orderbook.hpp"

#include <unordered_map>
#include <string>
#include <vector>
#include <cmath>
#include <algorithm>
#include <chrono>

namespace hft {

enum class RiskRejectCode : uint8_t {
    None,
    KillSwitchActive,
    UnknownSymbol,
    InvalidQty,
    InvalidPrice,
    MaxOrderQty,
    MaxOrderNotional,
    MaxPosition,
    MaxGrossExposure,
    MaxNetExposure,
    MaxOpenOrders,
    PriceBand,
    PostOnlyWouldCross,
    RateLimit
};

inline const char* reject_code_to_str(RiskRejectCode c) noexcept {
    switch (c) {
        case RiskRejectCode::None: return "none";
        case RiskRejectCode::KillSwitchActive: return "kill_switch_active";
        case RiskRejectCode::UnknownSymbol: return "unknown_symbol";
        case RiskRejectCode::InvalidQty: return "invalid_qty";
        case RiskRejectCode::InvalidPrice: return "invalid_price";
        case RiskRejectCode::MaxOrderQty: return "max_order_qty";
        case RiskRejectCode::MaxOrderNotional: return "max_order_notional";
        case RiskRejectCode::MaxPosition: return "max_position";
        case RiskRejectCode::MaxGrossExposure: return "max_gross_exposure";
        case RiskRejectCode::MaxNetExposure: return "max_net_exposure";
        case RiskRejectCode::MaxOpenOrders: return "max_open_orders";
        case RiskRejectCode::PriceBand: return "price_band";
        case RiskRejectCode::PostOnlyWouldCross: return "post_only_would_cross";
        case RiskRejectCode::RateLimit: return "rate_limit";
        default: return "unknown";
    }
}

struct RiskDecision {
    bool allowed = true;
    RiskRejectCode code = RiskRejectCode::None;
    std::string reason;

    static RiskDecision ok() {
        return {};
    }

    static RiskDecision reject(RiskRejectCode c, std::string why = {}) {
        RiskDecision d;
        d.allowed = false;
        d.code = c;
        d.reason = why.empty() ? reject_code_to_str(c) : std::move(why);
        return d;
    }
};

struct SymbolRiskLimits {
    double max_position = 1e18;
    double max_order_qty = 1e18;
    double max_order_notional = 1e18;
    double max_price_deviation_bps = 100.0;
    double min_price = 0.0;
    double max_price = 1e18;
    double tick_size = 1e-8;
    double lot_size = 1e-12;
    int64_t max_orders_per_second = 1000;
};

struct GlobalRiskLimits {
    double max_gross_exposure = 1e18;
    double max_net_exposure = 1e18;
    int32_t max_open_orders = 1000;
    bool enforce_post_only = true;
};

struct RiskGatewayStats {
    int64_t checks = 0;
    int64_t accepted = 0;
    int64_t rejected = 0;
    int64_t kill_switch_rejects = 0;
    int64_t rate_limit_rejects = 0;
    int64_t fat_finger_rejects = 0;
};

class RiskGateway {
public:
    explicit RiskGateway(GlobalRiskLimits global = {})
        : global_(global) {}

    void set_symbol_limits(const std::string& symbol, SymbolRiskLimits limits) {
        symbol_limits_[symbol] = limits;
        buckets_[symbol] = TokenBucket{};
    }

    void set_kill_switch(bool enabled) noexcept {
        kill_switch_ = enabled;
    }

    bool kill_switch() const noexcept {
        return kill_switch_;
    }

    const RiskGatewayStats& stats() const noexcept {
        return stats_;
    }

    RiskDecision check_order(
        const Order& order,
        const PortfolioState& portfolio,
        const std::unordered_map<std::string, Order>& open_orders,
        const OrderBook* book,
        int64_t now_ns
    ) {
        ++stats_.checks;

        if (kill_switch_) {
            ++stats_.rejected;
            ++stats_.kill_switch_rejects;
            return RiskDecision::reject(RiskRejectCode::KillSwitchActive);
        }

        auto lit = symbol_limits_.find(order.symbol);
        if (lit == symbol_limits_.end()) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::UnknownSymbol);
        }

        const SymbolRiskLimits& lim = lit->second;

        if (order.qty <= 0.0 || !std::isfinite(order.qty)) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::InvalidQty);
        }

        if (order.order_type != OrderType::Market) {
            if (order.price <= 0.0 || !std::isfinite(order.price)) {
                ++stats_.rejected;
                return RiskDecision::reject(RiskRejectCode::InvalidPrice);
            }

            if (order.price < lim.min_price || order.price > lim.max_price) {
                ++stats_.rejected;
                ++stats_.fat_finger_rejects;
                return RiskDecision::reject(RiskRejectCode::InvalidPrice);
            }

            if (!price_on_tick(order.price, lim.tick_size)) {
                ++stats_.rejected;
                return RiskDecision::reject(RiskRejectCode::InvalidPrice, "price_not_on_tick");
            }
        }

        if (!qty_on_lot(order.qty, lim.lot_size)) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::InvalidQty, "qty_not_on_lot");
        }

        if (!rate_limit_ok(order.symbol, lim.max_orders_per_second, now_ns)) {
            ++stats_.rejected;
            ++stats_.rate_limit_rejects;
            return RiskDecision::reject(RiskRejectCode::RateLimit);
        }

        if (order.qty > lim.max_order_qty) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::MaxOrderQty);
        }

        double ref_price = reference_price(order, book);
        double notional = order.qty * ref_price;

        if (notional > lim.max_order_notional) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::MaxOrderNotional);
        }

        if (book && order.order_type != OrderType::Market) {
            auto mid = book->mid_price();
            if (mid && *mid > 1e-12) {
                double dev_bps = std::abs(order.price - *mid) / *mid * 10000.0;
                if (dev_bps > lim.max_price_deviation_bps) {
                    ++stats_.rejected;
                    ++stats_.fat_finger_rejects;
                    return RiskDecision::reject(RiskRejectCode::PriceBand);
                }
            }

            if (global_.enforce_post_only && order.order_type != OrderType::Market) {
                if (would_cross(order, *book)) {
                     ++stats_.rejected;
                     return RiskDecision::reject(RiskRejectCode::PostOnlyWouldCross);
               }
           }
        }

        double current_pos = portfolio.position(order.symbol);
        double projected_pos = current_pos + signed_qty(order.side, order.qty);

        if (std::abs(projected_pos) > lim.max_position) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::MaxPosition);
        }

        if (static_cast<int32_t>(open_orders.size()) >= global_.max_open_orders) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::MaxOpenOrders);
        }

        auto exposure = projected_exposure(order, portfolio, open_orders, ref_price);

        if (exposure.gross > global_.max_gross_exposure) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::MaxGrossExposure);
        }

        if (std::abs(exposure.net) > global_.max_net_exposure) {
            ++stats_.rejected;
            return RiskDecision::reject(RiskRejectCode::MaxNetExposure);
        }

        ++stats_.accepted;
        return RiskDecision::ok();
    }

private:
    struct TokenBucket {
        int64_t window_start_ns = 0;
        int64_t count = 0;
    };

    struct Exposure {
        double gross = 0.0;
        double net = 0.0;
    };

    GlobalRiskLimits global_;
    std::unordered_map<std::string, SymbolRiskLimits> symbol_limits_;
    std::unordered_map<std::string, TokenBucket> buckets_;
    RiskGatewayStats stats_;
    bool kill_switch_ = false;

    static double signed_qty(Side side, double qty) noexcept {
        return side == Side::Buy ? qty : -qty;
    }

    static bool price_on_tick(double price, double tick) noexcept {
        if (tick <= 0.0) return true;
        double n = price / tick;
        return std::abs(n - std::round(n)) < 1e-6;
    }

    static bool qty_on_lot(double qty, double lot) noexcept {
        if (lot <= 0.0) return true;
        double n = qty / lot;
        return std::abs(n - std::round(n)) < 1e-6;
    }

    static bool would_cross(const Order& order, const OrderBook& book) noexcept {
        if (order.side == Side::Buy) {
            auto ask = book.best_ask();
            return ask && order.price >= *ask;
        }

        if (order.side == Side::Sell) {
            auto bid = book.best_bid();
            return bid && order.price <= *bid;
        }

        return true;
    }

    static double reference_price(const Order& order, const OrderBook* book) noexcept {
        if (order.order_type != OrderType::Market && order.price > 0.0) {
            return order.price;
        }

        if (book) {
            auto mid = book->mid_price();
            if (mid) return *mid;

            if (order.side == Side::Buy) {
                auto ask = book->best_ask();
                if (ask) return *ask;
            } else {
                auto bid = book->best_bid();
                if (bid) return *bid;
            }
        }

        return std::max(order.price, 1.0);
    }

    bool rate_limit_ok(const std::string& symbol, int64_t max_ops, int64_t now_ns) {
        if (max_ops <= 0) return false;

        constexpr int64_t kWindowNs = 1'000'000'000LL;

        auto& b = buckets_[symbol];
        if (b.window_start_ns == 0 || now_ns - b.window_start_ns >= kWindowNs) {
            b.window_start_ns = now_ns;
            b.count = 0;
        }

        if (b.count >= max_ops) return false;
        ++b.count;
        return true;
    }

    Exposure projected_exposure(
        const Order& incoming,
        const PortfolioState& portfolio,
        const std::unordered_map<std::string, Order>& open_orders,
        double incoming_ref_price
    ) const {
        Exposure e;

        for (const auto& [sym, qty] : portfolio.positions()) {
            auto lit = symbol_limits_.find(sym);
            double px = incoming_ref_price;
            if (sym != incoming.symbol && lit != symbol_limits_.end()) {
                px = std::max(lit->second.min_price, 1.0);
            }

            double value = qty * px;
            e.net += value;
            e.gross += std::abs(value);
        }

        e.net += signed_qty(incoming.side, incoming.qty) * incoming_ref_price;
        e.gross += std::abs(incoming.qty * incoming_ref_price);

        for (const auto& [oid, o] : open_orders) {
            auto lit = symbol_limits_.find(o.symbol);
            double px = o.price > 0.0 ? o.price : incoming_ref_price;
            if (o.symbol != incoming.symbol && lit != symbol_limits_.end()) {
                px = std::max(lit->second.min_price, 1.0);
            }

            double v = signed_qty(o.side, o.qty - o.filled_qty) * px;
            e.net += v;
            e.gross += std::abs(v);
        }

        return e;
    }
};

} // namespace hft