#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// portfolio.hpp  —  Portfolio state, FIFO lot accounting, risk limits
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include <unordered_map>
#include <deque>
#include <vector>
#include <string>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <optional>
#include <limits>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// Lot — single FIFO lot for realized PnL tracking
// ─────────────────────────────────────────────────────────────────────────────
struct Lot {
    double  price = 0.0;
    double  qty   = 0.0;
    int64_t ts    = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// RiskLimits
// ─────────────────────────────────────────────────────────────────────────────
struct RiskLimits {
    std::unordered_map<std::string, double> max_position;  // symbol → max |qty|
    double  max_drawdown    = 0.20;   // fraction of initial capital
    double  max_daily_loss  = 0.05;   // fraction of initial capital
    int32_t max_open_orders = 20;
    bool    halt_on_breach  = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// PortfolioState
// ─────────────────────────────────────────────────────────────────────────────
class PortfolioState {
public:
    explicit PortfolioState(double initial_cash, RiskLimits limits = {})
        : cash_(initial_cash)
        , initial_cash_(initial_cash)
        , limits_(std::move(limits))
        , peak_mtm_(initial_cash)
    {}

    // ── Fill processing ───────────────────────────────────────────────────────

    void update_on_fill(FillEvent& fill) {
        const std::string& sym  = fill.symbol;
        double notional         = fill.qty * fill.price;

        if (fill.side == Side::Buy) {
            positions_[sym] += fill.qty;
            cash_            -= notional;
            lots_[sym].push_back({fill.price, fill.qty, fill.timestamp});
        } else {
            positions_[sym] -= fill.qty;
            cash_            += notional;
            fill.realized_pnl = match_lots(sym, fill.qty, fill.price);
            realized_pnl_[sym] += fill.realized_pnl;
        }

        cash_        -= fill.fee;
        total_fees_  += fill.fee;
        ++fill_count_;
    }

    // ── Mark-to-market ────────────────────────────────────────────────────────

    double mark_to_market(const std::unordered_map<std::string, double>& mid_prices) const {
        double pos_value = 0.0;
        for (auto& [sym, qty] : positions_) {
            auto it = mid_prices.find(sym);
            if (it != mid_prices.end()) pos_value += qty * it->second;
        }
        return cash_ + pos_value;
    }

    // ── Risk checks ───────────────────────────────────────────────────────────

    // Returns list of breached limit descriptions (empty = ok)
    std::vector<std::string> check_risk(
        const std::unordered_map<std::string, double>& mid_prices)
    {
        std::vector<std::string> breaches;
        double mtm = mark_to_market(mid_prices);

        for (auto& [sym, qty] : positions_) {
            auto it = limits_.max_position.find(sym);
            if (it != limits_.max_position.end() && std::abs(qty) > it->second) {
                breaches.push_back("max_position:" + sym);
            }
        }

        double dd = (peak_mtm_ > 1e-12) ? (peak_mtm_ - mtm) / peak_mtm_ : 0.0;
        if (dd > limits_.max_drawdown)
            breaches.push_back("max_drawdown:" + std::to_string(dd));

        double daily_loss_frac = daily_loss_ / std::max(initial_cash_, 1e-12);
        if (daily_loss_frac > limits_.max_daily_loss)
            breaches.push_back("max_daily_loss:" + std::to_string(daily_loss_frac));

        peak_mtm_ = std::max(peak_mtm_, mtm);
        return breaches;
    }

    // ── Snapshot ──────────────────────────────────────────────────────────────

    void snapshot(int64_t ts, const std::unordered_map<std::string, double>& mid_prices) {
        pnl_series_.push_back({ts, mark_to_market(mid_prices)});
    }

    // ── Summary statistics ────────────────────────────────────────────────────

    struct Summary {
        double  pnl;
        double  pnl_pct;
        double  total_fees;
        double  max_drawdown;
        double  sharpe;
        double  calmar;
        int64_t fill_count;
        std::unordered_map<std::string, double> realized_pnl;
        std::unordered_map<std::string, double> positions;
        double  cash;
    };

    Summary summary() const {
        std::vector<double> vals;
        vals.reserve(pnl_series_.size());
        for (auto& [ts, v] : pnl_series_) vals.push_back(v);

        double final_mtm = vals.empty() ? cash_ : vals.back();
        double pnl       = final_mtm - initial_cash_;
        double pnl_pct   = pnl / std::max(initial_cash_, 1e-12);

        double dd      = max_drawdown(vals);
        double sharpe  = sharpe_ratio(vals);
        double calmar  = (dd < -1e-12) ? pnl_pct / std::abs(dd) : std::numeric_limits<double>::quiet_NaN();

        Summary s;
        s.pnl           = pnl;
        s.pnl_pct       = pnl_pct * 100.0;
        s.total_fees    = total_fees_;
        s.max_drawdown  = dd;
        s.sharpe        = sharpe;
        s.calmar        = calmar;
        s.fill_count    = fill_count_;
        s.realized_pnl  = realized_pnl_;
        s.positions     = positions_;
        s.cash          = cash_;
        return s;
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    double cash()          const noexcept { return cash_; }
    double initial_cash()  const noexcept { return initial_cash_; }
    double total_fees()    const noexcept { return total_fees_; }
    int64_t fill_count()   const noexcept { return fill_count_; }
    bool   halted()        const noexcept { return halted_; }
    void   set_halted(bool h)   noexcept { halted_ = h; }

    double position(const std::string& sym) const {
        auto it = positions_.find(sym);
        return it != positions_.end() ? it->second : 0.0;
    }

    const std::unordered_map<std::string, double>& positions() const { return positions_; }
    const std::vector<std::pair<int64_t,double>>&  pnl_series() const { return pnl_series_; }
    const RiskLimits& limits() const noexcept { return limits_; }

private:
    double   cash_;
    double   initial_cash_;
    RiskLimits limits_;

    std::unordered_map<std::string, double>            positions_;
    std::unordered_map<std::string, std::deque<Lot>>   lots_;
    std::unordered_map<std::string, double>            realized_pnl_;

    double   total_fees_  = 0.0;
    int64_t  fill_count_  = 0;
    bool     halted_      = false;
    double   daily_loss_  = 0.0;
    mutable double peak_mtm_;

    std::vector<std::pair<int64_t, double>> pnl_series_;

    double match_lots(const std::string& sym, double sell_qty, double sell_price) {
        auto& lots = lots_[sym];
        double rem = sell_qty, pnl = 0.0;
        while (rem > 1e-12 && !lots.empty()) {
            Lot& lot  = lots.front();
            double take = std::min(lot.qty, rem);
            pnl        += take * (sell_price - lot.price);
            lot.qty    -= take;
            rem        -= take;
            if (lot.qty <= 1e-12) lots.pop_front();
        }
        return pnl;
    }

    static double max_drawdown(const std::vector<double>& vals) {
        if (vals.size() < 2) return 0.0;
        double peak = vals[0], dd = 0.0;
        for (double v : vals) {
            peak = std::max(peak, v);
            if (peak > 1e-12) dd = std::min(dd, (v - peak) / peak);
        }
        return dd;
    }

    static double sharpe_ratio(const std::vector<double>& vals) {
        if (vals.size() < 3) return std::numeric_limits<double>::quiet_NaN();
        std::vector<double> rets;
        rets.reserve(vals.size() - 1);
        for (size_t i = 1; i < vals.size(); ++i) {
            double prev = std::abs(vals[i - 1]) > 1e-9 ? vals[i - 1] : 1e-9;
            rets.push_back((vals[i] - vals[i - 1]) / prev);
        }
        double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
        double var  = 0.0;
        for (double r : rets) var += (r - mean) * (r - mean);
        var /= rets.size();
        double sd = std::sqrt(var);
        return sd > 1e-12 ? mean / sd * std::sqrt(static_cast<double>(rets.size())) : 0.0;
    }
};

} // namespace hft