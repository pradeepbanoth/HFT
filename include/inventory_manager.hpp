#pragma once
// inventory_manager.hpp — advanced multi-asset inventory, exposure, hedge engine

#include "types.hpp"

#include <unordered_map>
#include <string>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include <optional>
#include <limits>

namespace hft {

struct Greeks {
    double delta = 0.0;
    double gamma = 0.0;
    double vega = 0.0;
    double theta = 0.0;
    double dollar_delta = 0.0;
};

struct InventoryRiskLimit {
    double max_qty = std::numeric_limits<double>::max();
    double max_notional = std::numeric_limits<double>::max();
    double max_beta_delta = std::numeric_limits<double>::max();
    double target_qty = 0.0;
};

struct AssetInventory {
    std::string symbol;
    double qty = 0.0;
    double avg_cost = 0.0;
    double realized_pnl = 0.0;
    double contract_multiplier = 1.0;
    double beta_vs_ref = 1.0;
    double funding_rate_annual = 0.0;

    int64_t last_update_ts = 0;
    int64_t fill_count = 0;

    void update_on_fill(const FillEvent& fill) {
        double signed_qty = fill.side == Side::Buy ? fill.qty : -fill.qty;
        double new_qty = qty + signed_qty;

        if (std::abs(qty) < 1e-12 || same_sign(qty, signed_qty)) {
            avg_cost = std::abs(new_qty) > 1e-12
                ? (std::abs(qty) * avg_cost + std::abs(signed_qty) * fill.price) / std::abs(new_qty)
                : 0.0;
            qty = new_qty;
        } else {
            double closing_qty = std::min(std::abs(qty), std::abs(signed_qty));
            double pnl_per_unit = qty > 0.0 ? fill.price - avg_cost : avg_cost - fill.price;
            realized_pnl += closing_qty * pnl_per_unit * contract_multiplier;

            qty = new_qty;

            if (std::abs(qty) < 1e-12) {
                qty = 0.0;
                avg_cost = 0.0;
            } else if (!same_sign(qty, signed_qty)) {
                avg_cost = fill.price;
            }
        }

        ++fill_count;
        last_update_ts = fill.timestamp;
    }

    double notional(double price) const noexcept {
        return std::abs(qty) * price * contract_multiplier;
    }

    double signed_notional(double price) const noexcept {
        return qty * price * contract_multiplier;
    }

    double unrealized_pnl(double price) const noexcept {
        if (qty > 0.0) return qty * (price - avg_cost) * contract_multiplier;
        if (qty < 0.0) return std::abs(qty) * (avg_cost - price) * contract_multiplier;
        return 0.0;
    }

    double dollar_delta(double price) const noexcept {
        return qty * price * contract_multiplier;
    }

    double beta_delta(double price) const noexcept {
        return dollar_delta(price) * beta_vs_ref;
    }

    Greeks greeks(double price) const noexcept {
        Greeks g;
        g.delta = qty * contract_multiplier;
        g.dollar_delta = dollar_delta(price);
        return g;
    }

private:
    static bool same_sign(double a, double b) noexcept {
        return (a >= 0.0 && b >= 0.0) || (a <= 0.0 && b <= 0.0);
    }
};

struct HedgeRecommendation {
    std::string symbol;
    Side side = Side::Buy;
    double qty = 0.0;
    double target_delta = 0.0;
    double current_delta = 0.0;
    double urgency = 0.0;
    std::string reason;
};

struct StressResult {
    double shock_pct = 0.0;
    double pnl = 0.0;
    double portfolio_value = 0.0;
};

struct InventorySnapshot {
    double gross_notional = 0.0;
    double net_notional = 0.0;
    double beta_weighted_delta = 0.0;
    double unrealized_pnl = 0.0;
    double realized_pnl = 0.0;
    double concentration_hhi = 0.0;
    int64_t total_fills = 0;
};

struct InventoryManagerConfig {
    std::string reference_symbol = "BTCUSDT";
    double target_beta_delta = 0.0;
    double hedge_threshold_usd = 5'000.0;
    double max_gross_notional = 1'000'000.0;
    double max_concentration_hhi = 0.60;
    double inventory_halflife_s = 300.0;
};

class InventoryManager {
public:
    explicit InventoryManager(InventoryManagerConfig cfg = {})
        : cfg_(std::move(cfg)) {}

    void register_asset(
        const std::string& symbol,
        double contract_multiplier = 1.0,
        double beta_vs_ref = 1.0,
        InventoryRiskLimit limit = {}
    ) {
        auto& inv = inventory_[symbol];
        inv.symbol = symbol;
        inv.contract_multiplier = contract_multiplier;
        inv.beta_vs_ref = beta_vs_ref;
        limits_[symbol] = limit;
    }

    void set_beta(const std::string& symbol, double beta) {
        inventory_[symbol].symbol = symbol;
        inventory_[symbol].beta_vs_ref = beta;
    }

    void set_funding_rate(const std::string& symbol, double annual_rate) {
        inventory_[symbol].symbol = symbol;
        inventory_[symbol].funding_rate_annual = annual_rate;
    }

    void on_fill(const FillEvent& fill) {
        auto& inv = inventory_[fill.symbol];
        if (inv.symbol.empty()) inv.symbol = fill.symbol;

        inv.update_on_fill(fill);

        double signed_qty = fill.side == Side::Buy ? fill.qty : -fill.qty;
        auto& buf = velocity_[fill.symbol];
        buf.push_back({fill.timestamp, signed_qty});
        if (buf.size() > 128) buf.pop_front();
    }

    InventorySnapshot snapshot(const std::unordered_map<std::string, double>& mids) const {
        InventorySnapshot s;

        std::vector<double> notionals;
        double gross = 0.0;

        for (const auto& [sym, inv] : inventory_) {
            auto it = mids.find(sym);
            if (it == mids.end()) continue;

            double px = it->second;
            double n = inv.notional(px);
            gross += n;
            s.net_notional += inv.signed_notional(px);
            s.beta_weighted_delta += inv.beta_delta(px);
            s.unrealized_pnl += inv.unrealized_pnl(px);
            s.realized_pnl += inv.realized_pnl;
            s.total_fills += inv.fill_count;
            notionals.push_back(n);
        }

        s.gross_notional = gross;

        if (gross > 1e-12) {
            double hhi = 0.0;
            for (double n : notionals) {
                double w = n / gross;
                hhi += w * w;
            }
            s.concentration_hhi = hhi;
        }

        return s;
    }

    std::vector<std::string> check_limits(const std::unordered_map<std::string, double>& mids) const {
        std::vector<std::string> breaches;

        auto snap = snapshot(mids);

        if (snap.gross_notional > cfg_.max_gross_notional)
            breaches.push_back("inventory:max_gross_notional");

        if (snap.concentration_hhi > cfg_.max_concentration_hhi)
            breaches.push_back("inventory:concentration_hhi");

        for (const auto& [sym, inv] : inventory_) {
            auto lit = limits_.find(sym);
            if (lit == limits_.end()) continue;

            const auto& lim = lit->second;
            auto mit = mids.find(sym);
            double px = mit == mids.end() ? inv.avg_cost : mit->second;

            if (std::abs(inv.qty) > lim.max_qty)
                breaches.push_back("inventory:max_qty:" + sym);

            if (inv.notional(px) > lim.max_notional)
                breaches.push_back("inventory:max_notional:" + sym);

            if (std::abs(inv.beta_delta(px)) > lim.max_beta_delta)
                breaches.push_back("inventory:max_beta_delta:" + sym);
        }

        return breaches;
    }

    std::vector<HedgeRecommendation> hedge_recommendations(
        const std::unordered_map<std::string, double>& mids
    ) const {
        std::vector<HedgeRecommendation> out;
        auto snap = snapshot(mids);

        double imbalance = snap.beta_weighted_delta - cfg_.target_beta_delta;
        if (std::abs(imbalance) < cfg_.hedge_threshold_usd) return out;

        auto ref = mids.find(cfg_.reference_symbol);
        if (ref == mids.end() || ref->second <= 1e-12) return out;

        double hedge_qty = std::abs(imbalance) / ref->second;

        HedgeRecommendation h;
        h.symbol = cfg_.reference_symbol;
        h.side = imbalance > 0.0 ? Side::Sell : Side::Buy;
        h.qty = hedge_qty;
        h.current_delta = snap.beta_weighted_delta;
        h.target_delta = cfg_.target_beta_delta;
        h.urgency = std::min(1.0, std::abs(imbalance) / std::max(cfg_.hedge_threshold_usd * 5.0, 1.0));
        h.reason = "beta_delta_rebalance";

        out.push_back(std::move(h));
        return out;
    }

    double inventory_decay_trade(const std::string& symbol, double elapsed_s) const {
        auto it = inventory_.find(symbol);
        if (it == inventory_.end()) return 0.0;

        double current = it->second.qty;
        double target = current * std::exp(-std::log(2.0) * elapsed_s / cfg_.inventory_halflife_s);
        return target - current;
    }

    double position_velocity(const std::string& symbol, int64_t lookback_ns = 60'000'000'000LL) const {
        auto it = velocity_.find(symbol);
        if (it == velocity_.end() || it->second.empty()) return 0.0;

        int64_t latest = it->second.back().first;
        double sum = 0.0;
        int n = 0;

        for (auto rit = it->second.rbegin(); rit != it->second.rend(); ++rit) {
            if (latest - rit->first > lookback_ns) break;
            sum += rit->second;
            ++n;
        }

        return n > 0 ? sum / n : 0.0;
    }

    std::vector<StressResult> stress_test(
        const std::unordered_map<std::string, double>& mids,
        const std::vector<double>& shocks = {-0.20, -0.10, -0.05, 0.05, 0.10, 0.20}
    ) const {
        std::vector<StressResult> out;
        out.reserve(shocks.size());

        auto base = snapshot(mids);
        double base_value = base.unrealized_pnl + base.realized_pnl;

        for (double shock : shocks) {
            std::unordered_map<std::string, double> shocked = mids;
            for (auto& [sym, px] : shocked) px *= (1.0 + shock);

            auto s = snapshot(shocked);

            StressResult r;
            r.shock_pct = shock * 100.0;
            r.portfolio_value = s.unrealized_pnl + s.realized_pnl;
            r.pnl = r.portfolio_value - base_value;
            out.push_back(r);
        }

        return out;
    }

    double funding_pnl_estimate(
        const std::unordered_map<std::string, double>& mids,
        double horizon_days
    ) const {
        double pnl = 0.0;
        for (const auto& [sym, inv] : inventory_) {
            auto it = mids.find(sym);
            if (it == mids.end()) continue;
            pnl += inv.signed_notional(it->second) * inv.funding_rate_annual * horizon_days / 365.0;
        }
        return pnl;
    }

    const AssetInventory* get(const std::string& symbol) const {
        auto it = inventory_.find(symbol);
        return it == inventory_.end() ? nullptr : &it->second;
    }

    double qty(const std::string& symbol) const {
        auto* inv = get(symbol);
        return inv ? inv->qty : 0.0;
    }

    const auto& all() const noexcept { return inventory_; }
    const InventoryManagerConfig& config() const noexcept { return cfg_; }

private:
    InventoryManagerConfig cfg_;
    std::unordered_map<std::string, AssetInventory> inventory_;
    std::unordered_map<std::string, InventoryRiskLimit> limits_;
    std::unordered_map<std::string, std::deque<std::pair<int64_t, double>>> velocity_;
};

} // namespace hft