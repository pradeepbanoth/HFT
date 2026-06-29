#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// risk_manager.hpp  —  Pre-trade and post-trade risk controls
//
// Pre-trade checks (called before order submission):
//   • Fat-finger:  reject orders > max_order_notional USD
//   • Price sanity: reject if price deviates > max_price_deviation_bps from mid
//   • Order rate:  reject if > max_orders_per_second in a rolling 1s window
//   • Daily volume: reject if cumulative traded notional > max_daily_notional
//   • Position limit: reject if fill would breach max_position_qty
//   • Margin check: reject if estimated margin > available_margin
//
// Post-trade circuit breakers (checked after each fill):
//   • Max daily loss: halt all trading if daily PnL < -max_daily_loss_usd
//   • Max drawdown:   halt if MTM < peak × (1 - max_drawdown_pct)
//   • Consecutive losses: halt after N consecutive losing trades
//
// All checks are O(1) with nanosecond timestamps.
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include "orderbook.hpp"
#include <string>
#include <unordered_map>
#include <deque>
#include <vector>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <functional>
#include <algorithm>
#include <cstdio>
#include <utility>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// RiskViolation
// ─────────────────────────────────────────────────────────────────────────────
enum class ViolationKind : uint8_t {
    FatFinger,
    PriceSanity,
    OrderRateLimit,
    DailyVolumeLimit,
    PositionLimit,
    MarginInsufficient,
    DailyLossLimit,
    MaxDrawdown,
    ConsecutiveLosses,
    ManualHalt,
};

inline const char* violation_name(ViolationKind k) noexcept {
    switch(k) {
        case ViolationKind::FatFinger:          return "FatFinger";
        case ViolationKind::PriceSanity:        return "PriceSanity";
        case ViolationKind::OrderRateLimit:     return "OrderRateLimit";
        case ViolationKind::DailyVolumeLimit:   return "DailyVolumeLimit";
        case ViolationKind::PositionLimit:      return "PositionLimit";
        case ViolationKind::MarginInsufficient: return "MarginInsufficient";
        case ViolationKind::DailyLossLimit:     return "DailyLossLimit";
        case ViolationKind::MaxDrawdown:        return "MaxDrawdown";
        case ViolationKind::ConsecutiveLosses:  return "ConsecutiveLosses";
        case ViolationKind::ManualHalt:         return "ManualHalt";
        default:                                return "Unknown";
    }
}

struct RiskViolation {
    ViolationKind kind;
    std::string   detail;
    int64_t       ts_ns = 0;

    std::string to_string() const {
        std::ostringstream oss;
        oss << "[RiskViolation:" << violation_name(kind) << "] " << detail
            << " @ " << ts_ns << "ns";
        return oss.str();
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// RiskManagerConfig
// ─────────────────────────────────────────────────────────────────────────────
struct RiskManagerConfig {
    // Pre-trade
    double  max_order_notional_usd   = 50'000.0;  // single-order notional cap
    double  max_price_deviation_bps  = 50.0;       // max deviation from mid (bps)
    int     max_orders_per_second    = 200;        // rate limit
    double  max_daily_notional_usd   = 10'000'000.0;
    std::unordered_map<std::string, double> max_position_qty;  // sym → max |qty|
    double  available_margin_usd     = 1'000'000.0; // available collateral
    double  margin_rate              = 0.10;         // 10% initial margin

    // Post-trade circuit breakers
    double  max_daily_loss_usd       = 5'000.0;
    double  max_drawdown_pct         = 0.10;      // 10% from peak
    int     max_consecutive_losses   = 10;

    // Alert callback (optional — called on any violation)
    std::function<void(const RiskViolation&)> on_violation;

    bool    halt_on_circuit_breaker  = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// RiskManager
// ─────────────────────────────────────────────────────────────────────────────
class RiskManager {
public:
    explicit RiskManager(RiskManagerConfig cfg = {})
        : cfg_(std::move(cfg)) {}

    // ── Pre-trade checks ──────────────────────────────────────────────────────

    // Returns empty vector if order is acceptable, or list of violations.
    // Pass book=nullptr to skip price sanity check.
    std::vector<RiskViolation> check_order(
        const std::string& symbol,
        Side               side,
        double             price,
        double             qty,
        int64_t            ts_ns,
        const OrderBook*   book = nullptr) const
    {
        if (halted_) {
            return { {ViolationKind::ManualHalt, "System halted", ts_ns} };
        }

        std::vector<RiskViolation> violations;
        double notional = price * qty;

        // 1. Fat-finger
        if (notional > cfg_.max_order_notional_usd) {
            violations.push_back({
                ViolationKind::FatFinger,
                "order notional $" + fmt(notional) + " > limit $"
                + fmt(cfg_.max_order_notional_usd),
                ts_ns
            });
        }

        // 2. Price sanity vs mid
        if (book && cfg_.max_price_deviation_bps > 0) {
            auto mid = book->mid_price();
            if (mid && *mid > 1e-9) {
                double deviation_bps = std::abs(price - *mid) / *mid * 10000.0;
                if (deviation_bps > cfg_.max_price_deviation_bps) {
                    violations.push_back({
                        ViolationKind::PriceSanity,
                        "price " + fmt(price) + " deviates " + fmt(deviation_bps)
                        + "bps from mid " + fmt(*mid),
                        ts_ns
                    });
                }
            }
        }

        // 3. Order rate limit (rolling 1-second window)
        prune_order_timestamps(ts_ns);
        if ((int)order_timestamps_.size() >= cfg_.max_orders_per_second) {
            violations.push_back({
                ViolationKind::OrderRateLimit,
                std::to_string(order_timestamps_.size()) + " orders in last second >= limit "
                + std::to_string(cfg_.max_orders_per_second),
                ts_ns
            });
        }

        // 4. Daily volume
        if (daily_notional_ + notional > cfg_.max_daily_notional_usd) {
            violations.push_back({
                ViolationKind::DailyVolumeLimit,
                "daily notional $" + fmt(daily_notional_ + notional)
                + " > limit $" + fmt(cfg_.max_daily_notional_usd),
                ts_ns
            });
        }

        // 5. Position limit
        auto pit = cfg_.max_position_qty.find(symbol);
        if (pit != cfg_.max_position_qty.end()) {
            double current = positions_.count(symbol) ? positions_.at(symbol) : 0.0;
            double projected = current + (side == Side::Buy ? qty : -qty);
            if (std::abs(projected) > pit->second) {
                violations.push_back({
                    ViolationKind::PositionLimit,
                    symbol + " projected qty " + fmt(projected)
                    + " > limit " + fmt(pit->second),
                    ts_ns
                });
            }
        }

        // 6. Margin check
        double margin_required = notional * cfg_.margin_rate;
        if (margin_required > cfg_.available_margin_usd - used_margin_) {
            violations.push_back({
                ViolationKind::MarginInsufficient,
                "margin required $" + fmt(margin_required)
                + " > available $" + fmt(cfg_.available_margin_usd - used_margin_),
                ts_ns
            });
        }

        // Fire alert callback
        for (auto& v : violations) {
            if (cfg_.on_violation) cfg_.on_violation(v);
        }
        return violations;
    }

    bool is_order_ok(const std::string& symbol, Side side,
                     double price, double qty, int64_t ts_ns,
                     const OrderBook* book = nullptr) const
    {
        return check_order(symbol, side, price, qty, ts_ns, book).empty();
    }

    // ── Order accepted (call after exchange ack) ──────────────────────────────

    void on_order_sent(const std::string& /*symbol*/, double price, double qty,
                       int64_t ts_ns)
    {
        order_timestamps_.push_back(ts_ns);
        used_margin_ += price * qty * cfg_.margin_rate;
        ++orders_sent_total_;
    }

    // ── Fill notification ─────────────────────────────────────────────────────

    // Returns list of circuit-breaker violations (empty = ok).
    std::vector<RiskViolation> on_fill(const FillEvent& fill,
                                        double current_mtm,
                                        int64_t ts_ns)
    {
        // Update state
        double notional = fill.qty * fill.price;
        daily_notional_ += notional;
        used_margin_    -= notional * cfg_.margin_rate * 0.5; // approximate release
        used_margin_     = std::max(0.0, used_margin_);

        double sign = (fill.side == Side::Buy) ? 1.0 : -1.0;
        positions_[fill.symbol] += sign * fill.qty;

        // Daily PnL tracking
        daily_pnl_ += fill.realized_pnl - fill.fee;
        peak_mtm_   = std::max(peak_mtm_, current_mtm);

        // Consecutive losses
        if (fill.realized_pnl - fill.fee < 0) {
            ++consecutive_losses_;
        } else {
            consecutive_losses_ = 0;
        }

        // Circuit breaker checks
        std::vector<RiskViolation> violations;

        if (daily_pnl_ < -cfg_.max_daily_loss_usd) {
            violations.push_back({
                ViolationKind::DailyLossLimit,
                "daily PnL $" + fmt(daily_pnl_) + " < -$" + fmt(cfg_.max_daily_loss_usd),
                ts_ns
            });
        }

        double dd = peak_mtm_ > 1e-9 ? (peak_mtm_ - current_mtm) / peak_mtm_ : 0.0;
        if (dd > cfg_.max_drawdown_pct) {
            violations.push_back({
                ViolationKind::MaxDrawdown,
                "drawdown " + fmt(dd * 100) + "% > limit " + fmt(cfg_.max_drawdown_pct * 100) + "%",
                ts_ns
            });
        }

        if (consecutive_losses_ >= cfg_.max_consecutive_losses) {
            violations.push_back({
                ViolationKind::ConsecutiveLosses,
                std::to_string(consecutive_losses_) + " consecutive losses >= limit "
                + std::to_string(cfg_.max_consecutive_losses),
                ts_ns
            });
        }

        for (auto& v : violations) {
            if (cfg_.on_violation) cfg_.on_violation(v);
            if (cfg_.halt_on_circuit_breaker) halted_ = true;
        }
        return violations;
    }

    // ── Reset (e.g., start of new trading day) ────────────────────────────────

    void reset_daily() {
        daily_notional_    = 0.0;
        daily_pnl_         = 0.0;
        consecutive_losses_= 0;
        order_timestamps_.clear();
    }

    void manual_halt()   { halted_ = true; }
    void manual_resume() { halted_ = false; }

    // ── Accessors ─────────────────────────────────────────────────────────────

    bool   is_halted()            const { return halted_; }
    double daily_pnl()            const { return daily_pnl_; }
    double daily_notional()       const { return daily_notional_; }
    int    consecutive_losses()   const { return consecutive_losses_; }
    int    orders_last_second(int64_t ts_ns) const {
        prune_order_timestamps(ts_ns);
        return static_cast<int>(order_timestamps_.size());
    }
    double used_margin()          const { return used_margin_; }
    double available_margin()     const { return cfg_.available_margin_usd - used_margin_; }

    struct Summary {
        bool   halted;
        double daily_pnl;
        double daily_notional;
        int    consecutive_losses;
        double used_margin;
        int64_t orders_sent_today;
        std::unordered_map<std::string, double> positions;
    };

    Summary summary() const {
        return {
            halted_, daily_pnl_, daily_notional_,
            consecutive_losses_, used_margin_,
            static_cast<int64_t>(orders_sent_total_),
            positions_
        };
    }

    const RiskManagerConfig& config() const { return cfg_; }

private:
    RiskManagerConfig                          cfg_;
    mutable std::deque<int64_t>                order_timestamps_;  // ns
    std::unordered_map<std::string, double>    positions_;
    double  daily_notional_     = 0.0;
    double  daily_pnl_          = 0.0;
    double  peak_mtm_           = 0.0;
    double  used_margin_        = 0.0;
    int     consecutive_losses_ = 0;
    int64_t orders_sent_total_  = 0;
    bool    halted_             = false;

    void prune_order_timestamps(int64_t ts_ns) const {
        int64_t cutoff = ts_ns - 1'000'000'000LL;  // 1 second ago
        while (!order_timestamps_.empty() && order_timestamps_.front() < cutoff)
            order_timestamps_.pop_front();
    }

    static std::string fmt(double v) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.4f", v);
        return buf;
    }
};

} // namespace hft