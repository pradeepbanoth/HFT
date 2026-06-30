#pragma once
// portfolio_risk.hpp — advanced portfolio risk engine
// Features:
// - Parametric VaR / CVaR
// - Monte Carlo VaR / CVaR
// - Stress testing
// - Margins / collateral usage
// - Concentration risk
// - Liquidity liquidation risk
// - Factor exposure
// - Risk contribution per asset
// - Kill-switch action recommendation

#include "types.hpp"
#include "portfolio.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace hft {

enum class PortfolioRiskLevel : uint8_t {
    Normal,
    Warning,
    Critical,
    Halt
};

enum class RiskAction : uint8_t {
    None,
    ReduceRisk,
    CancelAllOrders,
    HedgeInventory,
    HaltTrading
};

struct RiskScenario {
    std::string name;
    std::unordered_map<std::string, double> shock_pct;
};

struct FactorExposure {
    std::string factor;
    std::unordered_map<std::string, double> beta;
};

struct AssetRiskInput {
    std::string symbol;
    double price = 0.0;
    double daily_vol = 0.02;
    double liquidity_usd = 1e9;
    double margin_rate = 0.10;
    double liquidation_participation = 0.10;
};

struct AssetRiskContribution {
    std::string symbol;
    double exposure = 0.0;
    double abs_exposure = 0.0;
    double var_contribution = 0.0;
    double risk_pct = 0.0;
    double concentration = 0.0;
    double liquidity_days = 0.0;
};

struct PortfolioRiskLimits {
    double max_gross_exposure = 1e18;
    double max_net_exposure = 1e18;
    double max_leverage = 5.0;
    double max_var_95 = 1e18;
    double max_cvar_95 = 1e18;
    double max_mc_var_99 = 1e18;
    double max_margin_usage = 0.80;
    double max_concentration = 0.50;
    double max_drawdown = 0.20;
    double max_liquidity_days = 3.0;
    double max_stress_loss_frac = 0.15;
    bool halt_on_critical = true;
};

struct PortfolioRiskReport {
    PortfolioRiskLevel level = PortfolioRiskLevel::Normal;
    RiskAction action = RiskAction::None;

    double equity = 0.0;
    double cash = 0.0;

    double gross_exposure = 0.0;
    double net_exposure = 0.0;
    double leverage = 0.0;

    double parametric_var_95 = 0.0;
    double parametric_cvar_95 = 0.0;
    double monte_carlo_var_99 = 0.0;
    double monte_carlo_cvar_99 = 0.0;
    double portfolio_vol_usd = 0.0;
    double portfolio_vol_pct = 0.0;

    double margin_required = 0.0;
    double margin_usage = 0.0;

    double max_concentration = 0.0;
    std::string max_concentration_symbol;

    double liquidity_days = 0.0;
    double max_drawdown = 0.0;

    double worst_stress_loss = 0.0;
    double worst_stress_loss_frac = 0.0;
    std::string worst_stress_name;

    std::unordered_map<std::string, double> factor_exposures;
    std::vector<AssetRiskContribution> contributions;
    std::vector<std::string> breaches;
};

class PortfolioRiskEngine {
public:
    explicit PortfolioRiskEngine(PortfolioRiskLimits limits = {}, uint64_t seed = 42)
        : limits_(limits), rng_(seed) {}

    void set_limits(PortfolioRiskLimits limits) {
        limits_ = limits;
    }

    void set_asset_input(const AssetRiskInput& input) {
        assets_[input.symbol] = input;
    }

    void set_correlation(const std::string& a, const std::string& b, double corr) {
        double c = std::clamp(corr, -1.0, 1.0);
        corr_[key(a, b)] = c;
        corr_[key(b, a)] = c;
    }

    void add_scenario(RiskScenario scenario) {
        scenarios_.push_back(std::move(scenario));
    }

    void clear_scenarios() {
        scenarios_.clear();
    }

    void add_factor(FactorExposure factor) {
        factors_.push_back(std::move(factor));
    }

    void clear_factors() {
        factors_.clear();
    }

    PortfolioRiskReport evaluate(
        const PortfolioState& portfolio,
        const std::unordered_map<std::string, double>& prices,
        int mc_paths = 5000
    ) {
        PortfolioRiskReport r;
        r.cash = portfolio.cash();
        r.equity = portfolio.mark_to_market(prices);

        const auto& pos = portfolio.positions();

        compute_exposures(pos, prices, r);
        compute_margin(pos, prices, r);
        compute_concentration_and_liquidity(pos, prices, r);
        compute_parametric_var(pos, prices, r);
        compute_monte_carlo_var(pos, prices, r, mc_paths);
        compute_drawdown(portfolio.pnl_series(), r);
        compute_stress(pos, prices, r);
        compute_factor_exposures(pos, prices, r);
        compute_contributions(pos, prices, r);
        apply_limits(r);

        return r;
    }

private:
    PortfolioRiskLimits limits_;
    std::unordered_map<std::string, AssetRiskInput> assets_;
    std::unordered_map<std::string, double> corr_;
    std::vector<RiskScenario> scenarios_;
    std::vector<FactorExposure> factors_;
    std::mt19937_64 rng_;

    static std::string key(const std::string& a, const std::string& b) {
        return a + "|" + b;
    }

    double price_of(
        const std::string& sym,
        const std::unordered_map<std::string, double>& prices
    ) const {
        auto pit = prices.find(sym);
        if (pit != prices.end()) return pit->second;

        auto ait = assets_.find(sym);
        if (ait != assets_.end()) return ait->second.price;

        return 0.0;
    }

    double vol_of(const std::string& sym) const {
        auto it = assets_.find(sym);
        return it != assets_.end() ? it->second.daily_vol : 0.02;
    }

    double liquidity_of(const std::string& sym) const {
        auto it = assets_.find(sym);
        return it != assets_.end() ? std::max(1.0, it->second.liquidity_usd) : 1e9;
    }

    double participation_of(const std::string& sym) const {
        auto it = assets_.find(sym);
        return it != assets_.end()
            ? std::clamp(it->second.liquidation_participation, 0.001, 1.0)
            : 0.10;
    }

    double margin_rate_of(const std::string& sym) const {
        auto it = assets_.find(sym);
        return it != assets_.end() ? it->second.margin_rate : 0.10;
    }

    double corr_of(const std::string& a, const std::string& b) const {
        if (a == b) return 1.0;
        auto it = corr_.find(key(a, b));
        return it != corr_.end() ? it->second : 0.0;
    }

    void compute_exposures(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        for (const auto& [sym, qty] : pos) {
            double exp = qty * price_of(sym, prices);
            r.net_exposure += exp;
            r.gross_exposure += std::abs(exp);
        }

        r.leverage = r.equity > 1e-12 ? r.gross_exposure / r.equity : 0.0;
    }

    void compute_margin(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        for (const auto& [sym, qty] : pos) {
            double notional = std::abs(qty * price_of(sym, prices));
            r.margin_required += notional * margin_rate_of(sym);
        }

        r.margin_usage = r.equity > 1e-12 ? r.margin_required / r.equity : 1e18;
    }

    void compute_concentration_and_liquidity(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        if (r.gross_exposure <= 1e-12) return;

        for (const auto& [sym, qty] : pos) {
            double notional = std::abs(qty * price_of(sym, prices));
            double concentration = notional / r.gross_exposure;

            if (concentration > r.max_concentration) {
                r.max_concentration = concentration;
                r.max_concentration_symbol = sym;
            }

            double daily_liq_capacity = liquidity_of(sym) * participation_of(sym);
            double days = notional / std::max(1.0, daily_liq_capacity);
            r.liquidity_days = std::max(r.liquidity_days, days);
        }
    }

    void compute_parametric_var(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        std::vector<std::pair<std::string, double>> exp;
        for (const auto& [sym, qty] : pos) {
            double e = qty * price_of(sym, prices);
            if (std::abs(e) > 1e-12) exp.push_back({sym, e});
        }

        double variance = 0.0;
        for (size_t i = 0; i < exp.size(); ++i) {
            for (size_t j = 0; j < exp.size(); ++j) {
                const auto& [si, ei] = exp[i];
                const auto& [sj, ej] = exp[j];

                variance += ei * ej * vol_of(si) * vol_of(sj) * corr_of(si, sj);
            }
        }

        variance = std::max(0.0, variance);
        double sigma = std::sqrt(variance);

        r.portfolio_vol_usd = sigma;
        r.portfolio_vol_pct = r.equity > 1e-12 ? sigma / r.equity : 0.0;
        r.parametric_var_95 = 1.645 * sigma;
        r.parametric_cvar_95 = 2.063 * sigma;
    }

    void compute_monte_carlo_var(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r,
        int paths
    ) {
        if (paths <= 0 || pos.empty()) return;

        std::vector<std::string> syms;
        std::vector<double> exposures;
        for (const auto& [sym, qty] : pos) {
            double e = qty * price_of(sym, prices);
            if (std::abs(e) > 1e-12) {
                syms.push_back(sym);
                exposures.push_back(e);
            }
        }

        if (syms.empty()) return;

        std::normal_distribution<double> nd(0.0, 1.0);
        std::vector<double> losses;
        losses.reserve(paths);

        for (int p = 0; p < paths; ++p) {
            double pnl = 0.0;

            for (size_t i = 0; i < syms.size(); ++i) {
                double z = nd(rng_);

                // Simple correlation approximation:
                // blend idiosyncratic with average correlated market shock.
                double common = nd(rng_);
                double avg_corr = average_abs_corr(syms[i], syms);

                double blended_z =
                    std::sqrt(std::clamp(avg_corr, 0.0, 0.95)) * common +
                    std::sqrt(1.0 - std::clamp(avg_corr, 0.0, 0.95)) * z;

                double ret = vol_of(syms[i]) * blended_z;
                pnl += exposures[i] * ret;
            }

            losses.push_back(-pnl);
        }

        std::sort(losses.begin(), losses.end());

        size_t var_idx = static_cast<size_t>(0.99 * losses.size());
        if (var_idx >= losses.size()) var_idx = losses.size() - 1;

        r.monte_carlo_var_99 = std::max(0.0, losses[var_idx]);

        double tail_sum = 0.0;
        int tail_n = 0;
        for (size_t i = var_idx; i < losses.size(); ++i) {
            tail_sum += losses[i];
            ++tail_n;
        }

        r.monte_carlo_cvar_99 = tail_n > 0 ? std::max(0.0, tail_sum / tail_n) : 0.0;
    }

    double average_abs_corr(const std::string& sym, const std::vector<std::string>& syms) const {
        if (syms.size() <= 1) return 0.0;

        double s = 0.0;
        int n = 0;

        for (const auto& other : syms) {
            if (other == sym) continue;
            s += std::abs(corr_of(sym, other));
            ++n;
        }

        return n > 0 ? s / n : 0.0;
    }

    void compute_drawdown(
        const std::vector<std::pair<int64_t, double>>& pnl,
        PortfolioRiskReport& r
    ) const {
        if (pnl.empty()) return;

        double peak = pnl.front().second;
        double worst = 0.0;

        for (const auto& [ts, v] : pnl) {
            peak = std::max(peak, v);
            if (peak > 1e-12)
                worst = std::min(worst, (v - peak) / peak);
        }

        r.max_drawdown = std::abs(worst);
    }

    void compute_stress(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        for (const auto& sc : scenarios_) {
            double pnl = 0.0;

            for (const auto& [sym, qty] : pos) {
                double shock = 0.0;
                auto it = sc.shock_pct.find(sym);
                if (it != sc.shock_pct.end()) shock = it->second;

                pnl += qty * price_of(sym, prices) * shock;
            }

            double loss = std::max(0.0, -pnl);
            if (loss > r.worst_stress_loss) {
                r.worst_stress_loss = loss;
                r.worst_stress_name = sc.name;
            }
        }

        r.worst_stress_loss_frac =
            r.equity > 1e-12 ? r.worst_stress_loss / r.equity : 0.0;
    }

    void compute_factor_exposures(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        for (const auto& factor : factors_) {
            double exposure = 0.0;

            for (const auto& [sym, qty] : pos) {
                auto bit = factor.beta.find(sym);
                double beta = bit != factor.beta.end() ? bit->second : 0.0;
                exposure += qty * price_of(sym, prices) * beta;
            }

            r.factor_exposures[factor.factor] = exposure;
        }
    }

    void compute_contributions(
        const std::unordered_map<std::string, double>& pos,
        const std::unordered_map<std::string, double>& prices,
        PortfolioRiskReport& r
    ) const {
        double total_abs = std::max(1e-12, r.gross_exposure);
        double total_var = std::max(1e-12, r.parametric_var_95);

        for (const auto& [sym, qty] : pos) {
            double exp = qty * price_of(sym, prices);
            double abs_exp = std::abs(exp);
            double standalone_var = 1.645 * abs_exp * vol_of(sym);

            AssetRiskContribution c;
            c.symbol = sym;
            c.exposure = exp;
            c.abs_exposure = abs_exp;
            c.var_contribution = standalone_var;
            c.risk_pct = standalone_var / total_var;
            c.concentration = abs_exp / total_abs;

            double daily_liq_capacity = liquidity_of(sym) * participation_of(sym);
            c.liquidity_days = abs_exp / std::max(1.0, daily_liq_capacity);

            r.contributions.push_back(c);
        }

        std::sort(r.contributions.begin(), r.contributions.end(),
            [](const auto& a, const auto& b) {
                return a.var_contribution > b.var_contribution;
            });
    }

    void apply_limits(PortfolioRiskReport& r) const {
        auto breach = [&](const std::string& b, PortfolioRiskLevel lvl) {
            r.breaches.push_back(b);
            if (static_cast<int>(lvl) > static_cast<int>(r.level))
                r.level = lvl;
        };

        if (r.gross_exposure > limits_.max_gross_exposure)
            breach("max_gross_exposure", PortfolioRiskLevel::Critical);

        if (std::abs(r.net_exposure) > limits_.max_net_exposure)
            breach("max_net_exposure", PortfolioRiskLevel::Critical);

        if (r.leverage > limits_.max_leverage)
            breach("max_leverage", PortfolioRiskLevel::Critical);

        if (r.parametric_var_95 > limits_.max_var_95)
            breach("max_parametric_var_95", PortfolioRiskLevel::Critical);

        if (r.parametric_cvar_95 > limits_.max_cvar_95)
            breach("max_parametric_cvar_95", PortfolioRiskLevel::Critical);

        if (r.monte_carlo_var_99 > limits_.max_mc_var_99)
            breach("max_monte_carlo_var_99", PortfolioRiskLevel::Critical);

        if (r.margin_usage > limits_.max_margin_usage)
            breach("max_margin_usage", PortfolioRiskLevel::Critical);

        if (r.max_concentration > limits_.max_concentration)
            breach("max_concentration:" + r.max_concentration_symbol, PortfolioRiskLevel::Warning);

        if (r.max_drawdown > limits_.max_drawdown)
            breach("max_drawdown", PortfolioRiskLevel::Critical);

        if (r.liquidity_days > limits_.max_liquidity_days)
            breach("max_liquidity_days", PortfolioRiskLevel::Warning);

        if (r.worst_stress_loss_frac > limits_.max_stress_loss_frac)
            breach("max_stress_loss", PortfolioRiskLevel::Critical);

        if (limits_.halt_on_critical && r.level == PortfolioRiskLevel::Critical)
            r.level = PortfolioRiskLevel::Halt;

        if (r.level == PortfolioRiskLevel::Normal)
            r.action = RiskAction::None;
        else if (r.level == PortfolioRiskLevel::Warning)
            r.action = RiskAction::ReduceRisk;
        else if (r.level == PortfolioRiskLevel::Critical)
            r.action = RiskAction::CancelAllOrders;
        else
            r.action = RiskAction::HaltTrading;
    }
};

} // namespace hft