#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// synthetic.hpp  —  Synthetic tick data generator
//
//  Price process  : Heston stochastic volatility
//                   dS = μ S dt + √V S dW_S
//                   dV = κ(θ−V)dt + ξ√V dW_V      corr(dW_S, dW_V) = ρ
//
//  Spread process : CIR (mean-reverting, always positive)
//                   dσ = k_s(θ_s−σ)dt + ξ_s √σ dW_σ
//
//  Trade arrivals : Hawkes self-exciting point process
//                   λ(t) = λ₀ + α Σ_{t_i<t} exp(−β(t−t_i))
//
//  Regime switch  : 3-state Markov chain (calm / volatile / trending)
//
//  Multi-asset    : Cholesky-correlated price AND vol shocks
// ─────────────────────────────────────────────────────────────────────────────

#include "types.hpp"
#include <vector>
#include <string>
#include <random>
#include <cmath>
#include <functional>
#include <algorithm>
#include <stdexcept>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// AssetParams
// ─────────────────────────────────────────────────────────────────────────────
struct AssetParams {
    std::string symbol;
    double initial_price    = 50'000.0;
    double annual_vol       = 0.80;       // Heston V0 (initial variance = vol²)
    double vol_of_vol       = 0.40;       // κ: vol mean-reversion speed
    double vol_mean         = 0.75;       // θ: long-run vol (variance = vol_mean²)
    double vol_vol          = 0.50;       // ξ: vol-of-vol
    double price_vol_corr   = -0.65;      // ρ: price–vol correlation
    double drift            = 0.0;        // annual drift (μ)
    double tick_size        = 0.01;
    double spread_mean      = 0.50;       // θ_s: long-run spread
    double spread_kappa     = 5.0;        // k_s: spread mean-reversion speed
    double spread_vol       = 0.02;       // ξ_s: spread diffusion
    int    depth_levels     = 20;
    double depth_base_qty   = 1.0;        // qty at best level
    double depth_decay      = 0.6;        // power-law decay per level
    double trade_intensity  = 5.0;        // λ₀: baseline trade arrivals/sec
    double hawkes_alpha     = 0.30;       // α: self-excitation coefficient
    double hawkes_beta      = 2.00;       // β: decay rate per second
    bool   l3_mode          = false;

    // Regime switching
    double regime_probs[3]      = {0.70, 0.20, 0.10};  // calm, volatile, trending
    double regime_vol_mult[3]   = {1.0,  2.5,  1.2};
    double regime_spread_mult[3]= {1.0,  2.0,  1.5};
    double regime_switch_rate   = 0.001;  // per-tick probability of switching

    uint64_t seed = 42;
};

// ─────────────────────────────────────────────────────────────────────────────
// Cholesky decomposition (in-place, n×n row-major)
// Returns false if matrix is not positive definite
// ─────────────────────────────────────────────────────────────────────────────
inline bool cholesky(std::vector<double>& A, int n) {
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j <= i; ++j) {
            double s = A[i * n + j];
            for (int k = 0; k < j; ++k)
                s -= A[i * n + k] * A[j * n + k];
            if (i == j) {
                if (s <= 0.0) return false;
                A[i * n + j] = std::sqrt(s);
            } else {
                A[i * n + j] = s / A[j * n + j];
                A[j * n + i] = 0.0;   // zero upper triangle
            }
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// SyntheticGenerator  —  single-asset feed generator
// ─────────────────────────────────────────────────────────────────────────────
class SyntheticGenerator {
public:
    explicit SyntheticGenerator(AssetParams params)
        : p_(std::move(params))
        , rng_(p_.seed)
        , normal_(0.0, 1.0)
        , uniform_(0.0, 1.0)
    {}

    // Returns a flat vector of MarketEvent (L2Update + Trade).
    // Call this once; then feed the vector to SimEngine::run().
    std::vector<MarketEvent> generate(double duration_s,
                                      int    tick_interval_us = 100)
    {
        std::vector<MarketEvent> out;
        double  dt        = tick_interval_us * 1e-6;
        int64_t n_ticks   = static_cast<int64_t>(duration_s / dt);
        out.reserve(static_cast<size_t>(n_ticks) * (p_.depth_levels * 2 + 2));

        // Heston state
        double S   = p_.initial_price;
        double V   = p_.annual_vol * p_.annual_vol;
        double kap = p_.vol_of_vol;
        double tht = p_.vol_mean * p_.vol_mean;
        double xi  = p_.vol_vol;
        double rho = p_.price_vol_corr;

        // CIR spread state
        double spr    = p_.spread_mean;

        // Hawkes intensity state
        double lam        = p_.trade_intensity;
        double last_trade = 0.0;

        // Regime (0=calm,1=volatile,2=trending)
        int    regime = 0;
        double trend  = 0.0;

        // Sigma (annualised vol → per-tick)
        double annfac = std::sqrt(dt / (365.0 * 86400.0));

        static const int64_t kBaseNs = 1'700'000'000LL * 1'000'000'000LL;
        int64_t ts_ns = kBaseNs;
        int64_t trade_ctr = 0;

        for (int64_t tick = 0; tick < n_ticks; ++tick) {
            ts_ns += tick_interval_us * 1000LL;
            double t_sec = tick * dt;

            // ── Regime switch ────────────────────────────────────────────────
            if (uniform_(rng_) < p_.regime_switch_rate) {
                double r = uniform_(rng_);
                double acc = 0.0;
                for (int i = 0; i < 3; ++i) {
                    acc += p_.regime_probs[i];
                    if (r < acc) { regime = i; break; }
                }
                trend = 0.0;
            }
            double vol_m = p_.regime_vol_mult[regime];
            double spr_m = p_.regime_spread_mult[regime];

            // ── Heston step ──────────────────────────────────────────────────
            double z1 = normal_(rng_);
            double z2 = rho * z1 + std::sqrt(std::max(1.0 - rho * rho, 0.0)) * normal_(rng_);

            V = std::max(V + kap * (tht - V) * dt
                         + xi * std::sqrt(std::max(V, 0.0) * dt) * z2,
                         1e-10);

            double inst_vol = std::sqrt(V) * vol_m;
            double mu_dt    = p_.drift * dt + trend * dt;
            S = S * std::exp(mu_dt + inst_vol * annfac / dt * std::sqrt(dt) * z1);
            // Simplified: S *= exp(drift*dt + sqrt(V)*vol_m * sqrt(dt) * z1)
            S = std::max(S, p_.tick_size);

            if (regime == 2) {
                trend = 0.95 * trend + 0.05 * (z1 * inst_vol);
            }

            // ── CIR spread step ──────────────────────────────────────────────
            double zs = normal_(rng_);
            spr = std::max(
                p_.tick_size,
                spr + p_.spread_kappa * (p_.spread_mean - spr) * dt
                    + p_.spread_vol * spr_m * std::sqrt(std::max(spr, 0.0) * dt) * zs
            );

            double half_spr = spr / 2.0;
            double bid_top  = round_price(S - half_spr, p_.tick_size);
            double ask_top  = round_price(S + half_spr, p_.tick_size);
            if (ask_top <= bid_top) ask_top = bid_top + p_.tick_size;

            // ── Emit book levels ─────────────────────────────────────────────
            if (p_.l3_mode) {
                emit_l3_levels(out, ts_ns, bid_top, ask_top, tick);
            } else {
                emit_l2_levels(out, ts_ns, bid_top, ask_top);
            }

            // ── Hawkes trade arrivals ────────────────────────────────────────
            double elapsed = t_sec - last_trade;
            double cur_lam = lam + p_.hawkes_alpha
                             * std::exp(-p_.hawkes_beta * elapsed);
            double expected = cur_lam * dt;
            // Poisson draw
            double L   = std::exp(-expected);
            int    k   = 0;
            double pp  = 1.0;
            do { pp *= uniform_(rng_); ++k; } while (pp > L);
            int n_trades = k - 1;

            for (int i = 0; i < n_trades; ++i) {
                Side aggressor = (uniform_(rng_) < 0.5) ? Side::Buy : Side::Sell;
                double tp      = (aggressor == Side::Buy) ? ask_top : bid_top;
                double qty     = p_.depth_base_qty * (0.5 + expo_draw());

                // Price impact nudge
                double impact = qty * 2e-5 * S;
                S += (aggressor == Side::Buy) ? impact : -impact;

                Trade t;
                t.trade_id  = p_.symbol + "_t_" + std::to_string(++trade_ctr);
                t.symbol    = p_.symbol;
                t.side      = aggressor;
                t.price     = tp;
                t.qty       = qty;
                t.timestamp = ts_ns + 500;
                t.aggressor = aggressor;
                out.push_back(std::move(t));
                last_trade = t_sec;
            }
        }
        return out;
    }

private:
    AssetParams             p_;
    std::mt19937_64         rng_;
    std::normal_distribution<double> normal_;
    std::uniform_real_distribution<double> uniform_;

    static double round_price(double p, double tick) noexcept {
        return std::round(p / tick) * tick;
    }

    double expo_draw() {
        // Exponential with mean 0.5
        return -0.5 * std::log(std::max(uniform_(rng_), 1e-15));
    }

    double lognorm_qty(double base) {
        double u = uniform_(rng_);
        u = std::max(u, 1e-15);
        return base * std::exp(0.3 * (-2.0 * std::log(u)));  // approx log-normal
    }

    void emit_l2_levels(std::vector<MarketEvent>& out,
                        int64_t ts_ns,
                        double bid_top, double ask_top)
    {
        for (int i = 0; i < p_.depth_levels; ++i) {
            double noise = std::exp(0.3 * normal_(rng_));
            double decay = std::pow(1.0 / (1.0 + i), p_.depth_decay);
            double qty   = std::max(p_.tick_size, p_.depth_base_qty * decay * noise);

            L2Update bid;
            bid.symbol    = p_.symbol;
            bid.side      = BookSide::Bid;
            bid.price     = bid_top - i * p_.tick_size;
            bid.qty       = qty;
            bid.timestamp = ts_ns;
            out.push_back(bid);

            L2Update ask;
            ask.symbol    = p_.symbol;
            ask.side      = BookSide::Ask;
            ask.price     = ask_top + i * p_.tick_size;
            ask.qty       = qty;
            ask.timestamp = ts_ns;
            out.push_back(ask);
        }
    }

    void emit_l3_levels(std::vector<MarketEvent>& out,
                        int64_t ts_ns,
                        double bid_top, double ask_top,
                        int64_t tick_id)
    {
        for (int i = 0; i < p_.depth_levels; ++i) {
            double decay   = std::pow(1.0 / (1.0 + i), p_.depth_decay);
            double qty_lvl = std::max(p_.tick_size,
                                      p_.depth_base_qty * decay
                                      * std::exp(0.3 * normal_(rng_)));
            int n_orders = std::max(1, static_cast<int>(
                std::poisson_distribution<int>(2.0 + i * 0.5)(rng_)));

            for (int j = 0; j < n_orders; ++j) {
                auto make_id = [&](char side_ch) {
                    return p_.symbol + "_" + side_ch
                         + std::to_string(tick_id * 1000 + i * 10 + j);
                };

                L3Update bid;
                bid.symbol    = p_.symbol;
                bid.event     = L3Event::Add;
                bid.order_id  = make_id('b');
                bid.side      = Side::Buy;
                bid.price     = bid_top - i * p_.tick_size;
                bid.qty       = qty_lvl / n_orders;
                bid.timestamp = ts_ns;
                out.push_back(bid);

                L3Update ask;
                ask.symbol    = p_.symbol;
                ask.event     = L3Event::Add;
                ask.order_id  = make_id('a');
                ask.side      = Side::Sell;
                ask.price     = ask_top + i * p_.tick_size;
                ask.qty       = qty_lvl / n_orders;
                ask.timestamp = ts_ns;
                out.push_back(ask);
            }
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// CorrelatedGenerator  —  multi-asset feed with Cholesky-correlated shocks
// ─────────────────────────────────────────────────────────────────────────────
class CorrelatedGenerator {
public:
    CorrelatedGenerator(
        std::vector<AssetParams>  assets,
        std::vector<double>       price_corr_flat,   // n×n row-major
        std::vector<double>       vol_corr_flat = {}  // n×n, defaults to price_corr
    )
        : assets_(std::move(assets))
        , rng_(assets_.empty() ? 42ULL : assets_[0].seed)
        , normal_(0.0, 1.0)
        , uniform_(0.0, 1.0)
    {
        int n = static_cast<int>(assets_.size());
        price_L_ = price_corr_flat.empty()
                 ? identity(n) : price_corr_flat;
        vol_L_   = vol_corr_flat.empty()
                 ? price_L_ : vol_corr_flat;

        if (!cholesky(price_L_, n) || !cholesky(vol_L_, n)) {
            throw std::runtime_error(
                "CorrelatedGenerator: correlation matrix is not positive definite");
        }
    }

    std::vector<MarketEvent> generate(double duration_s,
                                      int    tick_interval_us = 100)
    {
        int    n    = static_cast<int>(assets_.size());
        double dt   = tick_interval_us * 1e-6;
        int64_t nt  = static_cast<int64_t>(duration_s / dt);

        std::vector<MarketEvent> out;
        out.reserve(static_cast<size_t>(nt) * n
                    * static_cast<size_t>(assets_.empty() ? 1
                                         : assets_[0].depth_levels * 2 + 2));

        // Per-asset Heston state
        std::vector<double> S(n), V(n), spr(n);
        std::vector<int>    regime(n, 0);
        std::vector<double> trend(n, 0.0);
        std::vector<double> last_trade(n, 0.0);
        std::vector<int64_t> trade_ctr(n, 0);

        for (int i = 0; i < n; ++i) {
            S[i]   = assets_[i].initial_price;
            V[i]   = assets_[i].annual_vol * assets_[i].annual_vol;
            spr[i] = assets_[i].spread_mean;
        }

        static const int64_t kBaseNs = 1'700'000'000LL * 1'000'000'000LL;
        int64_t ts_ns = kBaseNs;

        for (int64_t tick = 0; tick < nt; ++tick) {
            ts_ns += tick_interval_us * 1000LL;
            double t_sec = tick * dt;

            // Draw correlated standard normals
            std::vector<double> raw_price(n), raw_vol(n);
            for (int i = 0; i < n; ++i) {
                raw_price[i] = normal_(rng_);
                raw_vol[i]   = normal_(rng_);
            }
            std::vector<double> z_price(n, 0.0), z_vol(n, 0.0);
            // L is lower-triangular: z = L * raw
            for (int i = 0; i < n; ++i) {
                for (int j = 0; j <= i; ++j) {
                    z_price[i] += price_L_[i * n + j] * raw_price[j];
                    z_vol[i]   += vol_L_[i * n + j]   * raw_vol[j];
                }
            }

            for (int i = 0; i < n; ++i) {
                const AssetParams& p = assets_[i];

                // Regime switch
                if (uniform_(rng_) < p.regime_switch_rate) {
                    double r = uniform_(rng_), acc = 0.0;
                    for (int ri = 0; ri < 3; ++ri) {
                        acc += p.regime_probs[ri];
                        if (r < acc) { regime[i] = ri; break; }
                    }
                    trend[i] = 0.0;
                }
                double vm = p.regime_vol_mult[regime[i]];
                double sm = p.regime_spread_mult[regime[i]];

                // Heston step
                double rho = p.price_vol_corr;
                double zp  = z_price[i];
                double zv  = rho * zp + std::sqrt(std::max(1.0 - rho*rho, 0.0)) * z_vol[i];

                double kap = p.vol_of_vol, tht = p.vol_mean*p.vol_mean, xi = p.vol_vol;
                V[i] = std::max(V[i] + kap*(tht - V[i])*dt
                                + xi * std::sqrt(std::max(V[i], 0.0)*dt) * zv,
                                1e-10);

                double inst_v = std::sqrt(V[i]) * vm;
                S[i] = std::max(S[i] * std::exp(p.drift*dt + trend[i]*dt
                                + inst_v * std::sqrt(dt) * zp), p.tick_size);

                if (regime[i] == 2) trend[i] = 0.95*trend[i] + 0.05*(zp*inst_v);

                // CIR spread
                double zs = normal_(rng_);
                spr[i] = std::max(p.tick_size,
                                  spr[i] + p.spread_kappa*(p.spread_mean - spr[i])*dt
                                  + p.spread_vol * sm
                                  * std::sqrt(std::max(spr[i],0.0)*dt) * zs);

                double bid_top = std::round((S[i]-spr[i]/2.0)/p.tick_size)*p.tick_size;
                double ask_top = std::round((S[i]+spr[i]/2.0)/p.tick_size)*p.tick_size;
                if (ask_top <= bid_top) ask_top = bid_top + p.tick_size;

                // L2 levels
                for (int lv = 0; lv < p.depth_levels; ++lv) {
                    double decay = std::pow(1.0/(1.0+lv), p.depth_decay);
                    double qty   = std::max(p.tick_size,
                                            p.depth_base_qty * decay
                                            * std::exp(0.25*normal_(rng_)));

                    L2Update b; b.symbol=p.symbol; b.side=BookSide::Bid;
                    b.price=bid_top-lv*p.tick_size; b.qty=qty; b.timestamp=ts_ns;
                    out.push_back(b);

                    L2Update a; a.symbol=p.symbol; a.side=BookSide::Ask;
                    a.price=ask_top+lv*p.tick_size; a.qty=qty; a.timestamp=ts_ns;
                    out.push_back(a);
                }

                // Hawkes trades
                double elapsed = t_sec - last_trade[i];
                double cur_lam = p.trade_intensity
                               + p.hawkes_alpha * std::exp(-p.hawkes_beta*elapsed);
                if (uniform_(rng_) < cur_lam * dt) {
                    Side agg = (uniform_(rng_) < 0.5) ? Side::Buy : Side::Sell;
                    Trade t;
                    t.trade_id  = p.symbol + "_ct_" + std::to_string(++trade_ctr[i]);
                    t.symbol    = p.symbol;
                    t.side      = agg;
                    t.price     = (agg==Side::Buy) ? ask_top : bid_top;
                    t.qty       = p.depth_base_qty*(0.5-0.5*std::log(
                                      std::max(uniform_(rng_),1e-15)));
                    t.timestamp = ts_ns + 500;
                    t.aggressor = agg;
                    out.push_back(t);
                    last_trade[i] = t_sec;
                }
            }
        }
        return out;
    }

private:
    std::vector<AssetParams> assets_;
    std::mt19937_64          rng_;
    std::normal_distribution<double>       normal_;
    std::uniform_real_distribution<double> uniform_;
    std::vector<double>      price_L_;  // Cholesky factor
    std::vector<double>      vol_L_;

    static std::vector<double> identity(int n) {
        std::vector<double> I(n*n, 0.0);
        for (int i = 0; i < n; ++i) I[i*n+i] = 1.0;
        return I;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Merge multiple MarketEvent vectors in timestamp order (k-way merge)
// ─────────────────────────────────────────────────────────────────────────────
inline std::vector<MarketEvent>
merge_event_streams(std::vector<std::vector<MarketEvent>> streams)
{
    // Sort each stream
    for (auto& s : streams)
        std::stable_sort(s.begin(), s.end(), [](const MarketEvent& a, const MarketEvent& b){
            return event_timestamp(a) < event_timestamp(b);
        });

    size_t total = 0;
    for (auto& s : streams) total += s.size();

    std::vector<MarketEvent> out;
    out.reserve(total);

    // k-way merge with index tracking
    std::vector<size_t> idx(streams.size(), 0);
    while (true) {
        int64_t  min_ts = std::numeric_limits<int64_t>::max();
        int      best   = -1;
        for (size_t k = 0; k < streams.size(); ++k) {
            if (idx[k] < streams[k].size()) {
                int64_t ts = event_timestamp(streams[k][idx[k]]);
                if (ts < min_ts) { min_ts = ts; best = static_cast<int>(k); }
            }
        }
        if (best < 0) break;
        out.push_back(std::move(streams[best][idx[best]++]));
    }
    return out;
}

} // namespace hft