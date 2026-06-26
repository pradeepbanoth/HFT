#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// signals.hpp  —  Production signal library
//
// All functions are inline and operate on raw pointer/span arguments to avoid
// heap allocation in the hot path.  No external dependencies.
//
// Signals:
//   Book microstructure: micro_price, book_imbalance, multi_level_pressure, vwap_cost
//   Volatility: ewma_vol, realised_vol, parkinson_vol, garman_klass_vol
//   Fair value:  as_optimal_quotes, glft_optimal_quotes, roll_spread, kalman_fair_value
//   Trade flow:  trade_flow_imbalance, kyle_lambda, hawkes_intensity, pin_proxy
//   Inventory:   inventory_skew_quotes, delta_hedge_qty
//   Utils:       round_to_tick, spread_bps, exponential_decay_weights
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstddef>
#include <algorithm>
#include <numeric>
#include <limits>
#include <cassert>
#include <array>

namespace hft::signals {

static constexpr double kEps = 1e-12;

// ─────────────────────────────────────────────────────────────────────────────
// Book microstructure
// ─────────────────────────────────────────────────────────────────────────────

// OBI-weighted mid-price  — Gould & Cont (2013)
inline double micro_price(
    const double* bid_prices, const double* bid_qtys,
    const double* ask_prices, const double* ask_qtys,
    size_t levels) noexcept
{
    double bv = 0.0, av = 0.0;
    for (size_t i = 0; i < levels; ++i) { bv += bid_qtys[i]; av += ask_qtys[i]; }
    double total = bv + av;
    if (total < kEps) return (bid_prices[0] + ask_prices[0]) * 0.5;
    double imb = bv / total;
    return bid_prices[0] * imb + ask_prices[0] * (1.0 - imb);
}

// (bid_vol − ask_vol) / total ∈ [−1, 1]
inline double book_imbalance(
    const double* bid_qtys, const double* ask_qtys, size_t levels) noexcept
{
    double bv = 0.0, av = 0.0;
    for (size_t i = 0; i < levels; ++i) { bv += bid_qtys[i]; av += ask_qtys[i]; }
    double t = bv + av;
    return t > kEps ? (bv - av) / t : 0.0;
}

// Per-level imbalance vector (writes into `out`, length `levels`)
inline void multi_level_pressure(
    const double* bid_qtys, const double* ask_qtys,
    size_t levels, double* out) noexcept
{
    for (size_t i = 0; i < levels; ++i) {
        double bq = bid_qtys[i], aq = ask_qtys[i];
        double t  = bq + aq;
        out[i]    = (t > kEps) ? (bq - aq) / t : 0.0;
    }
}

// Walk-the-book VWAP cost to fill `target_qty`. Returns -1.0 on insufficient depth.
inline double vwap_cost(
    const double* prices, const double* qtys, size_t n, double target_qty) noexcept
{
    double rem = target_qty, cost = 0.0;
    for (size_t i = 0; i < n && rem > kEps; ++i) {
        double take = std::min(qtys[i], rem);
        cost += take * prices[i];
        rem  -= take;
    }
    return rem <= kEps ? cost / target_qty : -1.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Volatility
// ─────────────────────────────────────────────────────────────────────────────

// Log-return realised volatility (std dev of log returns)
inline double realised_vol(const double* mids, size_t n) noexcept {
    if (n < 2) return 0.0;
    double mean = 0.0, count = static_cast<double>(n - 1);
    // First pass: compute mean log-return
    for (size_t i = 0; i < n - 1; ++i) {
        if (mids[i] > kEps) mean += std::log(mids[i + 1] / mids[i]);
    }
    mean /= count;
    // Second pass: variance
    double var = 0.0;
    for (size_t i = 0; i < n - 1; ++i) {
        double r = (mids[i] > kEps) ? std::log(mids[i + 1] / mids[i]) : 0.0;
        double d = r - mean;
        var += d * d;
    }
    return std::sqrt(var / count);
}

// RiskMetrics EWMA volatility
inline double ewma_vol(const double* mids, size_t n, int halflife) noexcept {
    if (n < 2) return 0.0;
    double alpha = 1.0 - std::exp(-std::log(2.0) / halflife);
    double var   = 0.0;
    for (size_t i = 1; i < n; ++i) {
        double r = (mids[i - 1] > kEps) ? std::log(mids[i] / mids[i - 1]) : 0.0;
        var      = alpha * r * r + (1.0 - alpha) * var;
    }
    return std::sqrt(var);
}

// Parkinson (1980) high-low range estimator
inline double parkinson_vol(const double* highs, const double* lows, size_t n) noexcept {
    if (n < 1) return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if (lows[i] > kEps) {
            double r = std::log(highs[i] / lows[i]);
            s += r * r;
        }
    }
    return std::sqrt(s / (4.0 * n * std::log(2.0)));
}

// Garman-Klass (1980) OHLC volatility estimator
inline double garman_klass_vol(
    const double* opens, const double* highs,
    const double* lows,  const double* closes, size_t n) noexcept
{
    if (n < 1) return 0.0;
    const double c = 2.0 * std::log(2.0) - 1.0;
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) {
        if (opens[i] > kEps && lows[i] > kEps) {
            double hl2 = std::log(highs[i] / lows[i]);
            double co2 = std::log(closes[i] / opens[i]);
            s += 0.5 * hl2 * hl2 - c * co2 * co2;
        }
    }
    return std::sqrt(s / n);
}

// ─────────────────────────────────────────────────────────────────────────────
// Fair value / price discovery
// ─────────────────────────────────────────────────────────────────────────────

// Avellaneda–Stoikov (2008) optimal quotes
// Returns {reservation_price_adj, optimal_half_spread}
inline std::array<double, 2> as_optimal_quotes(
    double sigma, double gamma, double q, double T, double dt, double k) noexcept
{
    double tau        = std::max(T - dt, 1e-6);
    double r_adj      = -gamma * sigma * sigma * q * tau;
    double half_spread = gamma * sigma * sigma * tau / 2.0
                       + std::log(1.0 + gamma / k) / gamma;
    return { r_adj, std::max(half_spread, 0.0) };
}

// Guéant–Lehalle–Fernandez-Tapia (2013) inventory-bounded quotes
// Returns {bid_adj, ask_adj} relative to fair value
inline std::array<double, 2> glft_optimal_quotes(
    double sigma, double gamma, double q, double q_max,
    double T, double dt, double k) noexcept
{
    double tau       = std::max(T - dt, 1e-6);
    double r         = -gamma * sigma * sigma * q * tau;
    double inv_ratio = std::abs(q) / std::max(q_max, kEps);
    double spread_adj = 1.0 + inv_ratio * inv_ratio;
    double half_spread = (gamma * sigma * sigma * tau / 2.0
                        + std::log(1.0 + gamma / k) / gamma) * spread_adj;
    return { r - half_spread, r + half_spread };
}

// Roll (1984) effective spread estimator.
// Returns full spread (half = result / 2)
inline double roll_spread(const double* mids, size_t n) noexcept {
    if (n < 3) return 0.0;
    double cov = 0.0;
    for (size_t i = 1; i < n - 1; ++i) {
        double d1 = mids[i]     - mids[i - 1];
        double d2 = mids[i + 1] - mids[i];
        cov += d1 * d2;
    }
    cov /= static_cast<double>(n - 2);
    return cov < 0.0 ? 2.0 * std::sqrt(-cov) : 0.0;
}

// 2-state Kalman filter on mid-price series.
// State: [price, trend]. Writes filtered values into `out` (length n).
inline void kalman_fair_value(
    const double* mids, size_t n,
    double process_noise, double obs_noise,
    double* out) noexcept
{
    if (n == 0) return;
    double x = mids[0], v = 0.0;
    // 2×2 covariance matrix stored flat [P00, P01, P10, P11]
    double P00 = 1.0, P01 = 0.0, P10 = 0.0, P11 = 1.0;
    double Q   = process_noise;
    double R   = obs_noise;
    out[0]     = x;

    for (size_t i = 1; i < n; ++i) {
        // Predict
        double xp   = x + v;
        double vp   = v;
        double pp00 = P00 + P10 + P01 + P11 + Q;
        double pp01 = P01 + P11 + Q * 0.1;
        double pp10 = P10 + P11 + Q * 0.1;
        double pp11 = P11 + Q * 0.1;

        // Innovation
        double z = mids[i];
        double y = z - xp;
        double S = pp00 + R;

        // Kalman gain
        double K0 = pp00 / S;
        double K1 = pp10 / S;

        // Update
        x    = xp + K0 * y;
        v    = vp + K1 * y;
        P00  = (1.0 - K0) * pp00;
        P01  = (1.0 - K0) * pp01;
        P10  = pp10 - K1 * pp00;
        P11  = pp11 - K1 * pp01;

        out[i] = x;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Trade flow / toxicity
// ─────────────────────────────────────────────────────────────────────────────

// (buy_vol − sell_vol) / total ∈ [−1, 1]
inline double trade_flow_imbalance(
    const double* buy_vols, size_t nb,
    const double* sell_vols, size_t ns) noexcept
{
    double tb = 0.0, ts = 0.0;
    for (size_t i = 0; i < nb; ++i) tb += buy_vols[i];
    for (size_t i = 0; i < ns; ++i) ts += sell_vols[i];
    double total = tb + ts;
    return total > kEps ? (tb - ts) / total : 0.0;
}

// Kyle's lambda: price impact per unit signed flow (OLS with decay weights)
inline double kyle_lambda(
    const double* price_changes,
    const double* signed_volumes,
    const double* weights,
    size_t n) noexcept
{
    double sxy = 0.0, sxx = 0.0;
    for (size_t i = 0; i < n; ++i) {
        double w  = weights ? weights[i] : 1.0;
        sxy += w * signed_volumes[i] * price_changes[i];
        sxx += w * signed_volumes[i] * signed_volumes[i];
    }
    return sxx > kEps ? sxy / sxx : 0.0;
}

// Hawkes process instantaneous intensity
// λ(t) = μ + α * Σ_{t_i < t} exp(−β * (t − t_i))
// event_times_ns: arrival times in nanoseconds
inline double hawkes_intensity(
    const int64_t* event_times_ns, size_t n,
    int64_t current_ns,
    double mu = 0.5, double alpha = 0.3, double beta_per_ns = 5e-10) noexcept
{
    // beta_per_ns = 1/2e9 → 2-second half-life
    double intensity = mu;
    for (size_t i = 0; i < n; ++i) {
        double dt_ns = static_cast<double>(current_ns - event_times_ns[i]);
        if (dt_ns > 0.0)
            intensity += alpha * std::exp(-beta_per_ns * dt_ns);
    }
    return intensity;
}

// PIN proxy — fast closed-form approximation of Easley et al. (1996)
// PIN ≈ |buy_vol - sell_vol| / total. Range [0, 1].
inline double pin_proxy(
    const double* buy_vols, size_t nb,
    const double* sell_vols, size_t ns) noexcept
{
    double tb = 0.0, ts = 0.0;
    for (size_t i = 0; i < nb; ++i) tb += buy_vols[i];
    for (size_t i = 0; i < ns; ++i) ts += sell_vols[i];
    double total = tb + ts;
    return total > kEps ? std::abs(tb - ts) / total : 0.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inventory / risk
// ─────────────────────────────────────────────────────────────────────────────

// Linear inventory skew. Returns {bid_adj, ask_adj}.
inline std::array<double, 2> inventory_skew_quotes(
    double mid, double inventory,
    double max_inventory, double skew_factor) noexcept
{
    double ratio  = std::max(-1.0, std::min(1.0,
                        max_inventory > kEps ? inventory / max_inventory : 0.0));
    double half   = mid * skew_factor;
    return { -half - ratio * half, half - ratio * half };
}

// ─────────────────────────────────────────────────────────────────────────────
// Utilities
// ─────────────────────────────────────────────────────────────────────────────

inline double round_to_tick(double price, double tick_size) noexcept {
    return std::round(price / tick_size) * tick_size;
}

inline double spread_bps(double bid, double ask) noexcept {
    double mid = (bid + ask) * 0.5;
    return mid > kEps ? (ask - bid) / mid * 10000.0 : 0.0;
}

// Fill `out` (length n) with normalised exponential decay weights
inline void exponential_decay_weights(size_t n, int halflife, double* out) noexcept {
    double alpha = std::log(2.0) / halflife;
    double sum   = 0.0;
    for (size_t i = 0; i < n; ++i) {
        out[i] = std::exp(-alpha * static_cast<double>(n - 1 - i));
        sum   += out[i];
    }
    if (sum > kEps) for (size_t i = 0; i < n; ++i) out[i] /= sum;
}

// Variance-ratio Hurst proxy for regime detection.
// VR > 1.2 → trending, VR < 0.8 → mean-reverting.
inline double variance_ratio(const double* mids, size_t n, size_t k) noexcept {
    if (n < k + 2 || k < 2) return 1.0;
    // 1-step variance
    double var1 = 0.0;
    for (size_t i = 1; i < n; ++i) {
        double r = mids[i] - mids[i - 1];
        var1 += r * r;
    }
    var1 /= static_cast<double>(n - 1);
    // k-step variance
    double vark = 0.0;
    size_t nk = n - k;
    for (size_t i = 0; i < nk; ++i) {
        double r = mids[i + k] - mids[i];
        vark += r * r;
    }
    vark /= static_cast<double>(nk);
    return var1 > kEps ? vark / (k * var1) : 1.0;
}

} // namespace hft::signals