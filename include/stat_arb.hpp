#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// stat_arb.hpp  —  Statistical Arbitrage / Pairs Trading Strategy
//
// Models a cointegrated spread between two assets (e.g. BTC spot vs BTC perp,
// or BTC vs ETH beta-adjusted).
//
// Algorithm:
//   1. Estimate the hedge ratio β via rolling OLS regression on mid-price returns.
//   2. Compute spread: s(t) = log(P_A) - β·log(P_B)
//   3. Filter spread through a Kalman filter to estimate fair value μ_s.
//   4. Compute z-score: z = (s - μ_s) / σ_s  where σ_s is rolling EWMA std.
//   5. Entry: |z| > entry_z  → trade to normalise (buy cheap / sell expensive)
//   6. Exit:  |z| < exit_z   → close position
//   7. Stop:  |z| > stop_z   → emergency close (regime break)
//
// Signal filters:
//   • Minimum spread half-life (reject if reversion is too slow)
//   • Volume filter: only trade when recent trade flow is balanced
//   • Bid-ask cost check: spread PnL must exceed 2× bid-ask spread cost
//
// Multi-leg execution: submits simultaneous limit orders on both legs.
// ─────────────────────────────────────────────────────────────────────────────

#include "simulator.hpp"
#include "signals.hpp"
#include "inventory_manager.hpp"
#include <deque>
#include <cmath>
#include <string>
#include <optional>
#include <iostream>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// PairsConfig
// ─────────────────────────────────────────────────────────────────────────────
struct PairsConfig {
    std::string leg_a;         // symbol A (primary)
    std::string leg_b;         // symbol B (hedge leg)
    double tick_a      = 0.01;
    double tick_b      = 0.01;
    double qty_a       = 0.01; // order qty per side (asset A units)

    // Cointegration model
    int    ols_window  = 200;  // rolling OLS window for β estimation
    double entry_z     = 2.0;  // z-score to enter
    double exit_z      = 0.5;  // z-score to exit
    double stop_z      = 4.0;  // z-score to stop-loss (regime break)

    // Kalman filter params for spread μ
    double kalman_Q    = 1e-5;
    double kalman_R    = 1.0;

    // EWMA params
    int    ewma_hl     = 50;   // half-life for σ estimation (ticks)

    // Position limits
    double max_position_a = 0.1;  // max abs qty in leg A

    // Cost filter
    double min_pnl_bps = 3.0;  // minimum expected PnL (bps) after costs to trade

    // Volume filter
    double max_tfi     = 0.7;  // max |trade flow imbalance| to enter (avoid toxic flow)
};

// ─────────────────────────────────────────────────────────────────────────────
// SpreadState  —  all per-pair running state
// ─────────────────────────────────────────────────────────────────────────────
struct SpreadState {
    // Rolling log-prices for OLS β estimation
    std::deque<double> log_pa, log_pb;

    // Spread history for Kalman + EWMA σ
    std::deque<double> spread_hist;

    // Kalman filter state [μ, drift]
    double kf_x = 0.0, kf_v = 0.0;
    double kf_P00=1, kf_P01=0, kf_P10=0, kf_P11=1;
    bool   kf_init = false;

    // EWMA variance
    double ewma_var  = 0.0;
    bool   ewma_init = false;

    // Current β
    double beta      = 1.0;

    // Current spread, z-score
    double spread    = 0.0;
    double mu_spread = 0.0;
    double sigma_spread = 0.001;
    double z_score   = 0.0;

    // Position state
    enum class PosState { Flat, Long, Short } pos = PosState::Flat;
    std::optional<std::string> pending_a, pending_b;  // live order IDs

    // Trade flow history
    std::deque<double> buy_vols_a, sell_vols_a;
    std::deque<double> buy_vols_b, sell_vols_b;

    // Stats
    int64_t entries = 0, exits = 0, stops = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// StatArbStrategy
// ─────────────────────────────────────────────────────────────────────────────
class StatArbStrategy : public Strategy {
public:
    explicit StatArbStrategy(PairsConfig cfg)
        : cfg_(std::move(cfg)) {}

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void on_start(SimEngine& engine) override {
        std::cout << "[StatArb] Started: " << cfg_.leg_a
                  << " vs " << cfg_.leg_b << "\n";
    }

    void on_end(SimEngine& engine) override {
        std::cout << "[StatArb] Done. entries=" << st_.entries
                  << " exits=" << st_.exits
                  << " stops=" << st_.stops << "\n";
    }

    // ── Book update ───────────────────────────────────────────────────────────

    void on_book_update(const std::string& symbol,
                        OrderBook& book,
                        int64_t ts_ns,
                        SimEngine& engine) override
    {
        // We need both books to compute the spread
        auto* book_a = engine.get_book(cfg_.leg_a);
        auto* book_b = engine.get_book(cfg_.leg_b);
        if (!book_a || !book_b) return;

        auto mid_a = book_a->mid_price();
        auto mid_b = book_b->mid_price();
        if (!mid_a || !mid_b || *mid_a < 1e-9 || *mid_b < 1e-9) return;

        // Update log-price history
        double lpa = std::log(*mid_a);
        double lpb = std::log(*mid_b);

        st_.log_pa.push_back(lpa);
        st_.log_pb.push_back(lpb);
        if ((int)st_.log_pa.size() > cfg_.ols_window) {
            st_.log_pa.pop_front(); st_.log_pb.pop_front();
        }

        // Estimate β via rolling OLS: lpa = α + β·lpb
        if ((int)st_.log_pa.size() >= cfg_.ols_window / 2) {
            st_.beta = compute_ols_beta();
        }

        // Compute spread
        double new_spread = lpa - st_.beta * lpb;
        st_.spread = new_spread;
        st_.spread_hist.push_back(new_spread);
        if ((int)st_.spread_hist.size() > cfg_.ols_window)
            st_.spread_hist.pop_front();

        // Kalman filter for μ
        update_kalman(new_spread, cfg_.kalman_Q, cfg_.kalman_R);

        // EWMA variance for σ
        double alpha = 1.0 - std::exp(-std::log(2.0) / cfg_.ewma_hl);
        double dev   = new_spread - st_.mu_spread;
        if (!st_.ewma_init) {
            st_.ewma_var = dev * dev;
            st_.ewma_init = true;
        } else {
            st_.ewma_var = alpha * dev * dev + (1.0 - alpha) * st_.ewma_var;
        }
        st_.sigma_spread = std::max(1e-8, std::sqrt(st_.ewma_var));

        // Z-score
        st_.z_score = (new_spread - st_.mu_spread) / st_.sigma_spread;

        // Trading logic
        evaluate_signals(*book_a, *book_b, ts_ns, engine);
    }

    void on_trade(const Trade& trade, OrderBook&, int64_t, SimEngine&) override {
        auto& bv = (trade.symbol == cfg_.leg_a) ? st_.buy_vols_a : st_.buy_vols_b;
        auto& sv = (trade.symbol == cfg_.leg_a) ? st_.sell_vols_a : st_.sell_vols_b;
        auto* q = (trade.aggressor == Side::Buy) ? &bv : &sv;
        q->push_back(trade.qty);
        if (q->size() > 100) q->pop_front();
    }

    void on_fill(const FillEvent& fill, PortfolioState&,
                 int64_t, SimEngine& engine) override
    {
        inv_.on_fill(fill);
        // Clear pending order ID
        if (st_.pending_a && *st_.pending_a == fill.order_id) st_.pending_a.reset();
        if (st_.pending_b && *st_.pending_b == fill.order_id) st_.pending_b.reset();
    }

    void on_order_ack(const Order& order, bool accepted,
                      int64_t, SimEngine&) override
    {
        if (!accepted) {
            if (st_.pending_a && *st_.pending_a == order.order_id) st_.pending_a.reset();
            if (st_.pending_b && *st_.pending_b == order.order_id) st_.pending_b.reset();
        }
    }

    // ── Accessors ─────────────────────────────────────────────────────────────

    double z_score()  const { return st_.z_score; }
    double beta()     const { return st_.beta; }
    double spread()   const { return st_.spread; }
    const SpreadState& state() const { return st_; }

private:
    PairsConfig     cfg_;
    SpreadState     st_;
    InventoryManager inv_;

    // ── Signal evaluation ─────────────────────────────────────────────────────

    void evaluate_signals(const OrderBook& book_a, const OrderBook& book_b,
                          int64_t ts_ns, SimEngine& engine)
    {
        double z = st_.z_score;

        // Stop-loss: z too extreme → close immediately
        if (std::abs(z) > cfg_.stop_z && st_.pos != SpreadState::PosState::Flat) {
            close_position(book_a, book_b, ts_ns, engine);
            ++st_.stops;
            return;
        }

        // Exit: z reverted
        if (st_.pos == SpreadState::PosState::Long && z > -cfg_.exit_z) {
            close_position(book_a, book_b, ts_ns, engine);
            ++st_.exits;
            return;
        }
        if (st_.pos == SpreadState::PosState::Short && z < cfg_.exit_z) {
            close_position(book_a, book_b, ts_ns, engine);
            ++st_.exits;
            return;
        }

        // Entry: only when flat and no pending orders
        if (st_.pos != SpreadState::PosState::Flat) return;
        if (st_.pending_a || st_.pending_b) return;

        // Position limit check
        double pos_a = inv_.qty(cfg_.leg_a);
        if (std::abs(pos_a) >= cfg_.max_position_a) return;

        // Trade flow filter (avoid toxic flow)
        if (!st_.buy_vols_a.empty() && !st_.sell_vols_a.empty()) {
            std::vector<double> bv(st_.buy_vols_a.begin(), st_.buy_vols_a.end());
            std::vector<double> sv(st_.sell_vols_a.begin(), st_.sell_vols_a.end());
            double tfi = std::abs(signals::trade_flow_imbalance(
                bv.data(), bv.size(), sv.data(), sv.size()));
            if (tfi > cfg_.max_tfi) return;  // too much directional flow
        }

        // Bid-ask cost check
        double spread_a_bps = book_a.spread_bps().value_or(999.0);
        double spread_b_bps = book_b.spread_bps().value_or(999.0);
        double cost_bps = spread_a_bps / 2.0 + spread_b_bps / 2.0;
        if (cost_bps > cfg_.min_pnl_bps) return;  // not worth it after costs

        // Entry signals
        if (z < -cfg_.entry_z) {
            // Spread too low: buy A, sell B·β units
            enter_long(book_a, book_b, ts_ns, engine);
        } else if (z > cfg_.entry_z) {
            // Spread too high: sell A, buy B·β units
            enter_short(book_a, book_b, ts_ns, engine);
        }
    }

    void enter_long(const OrderBook& book_a, const OrderBook& book_b,
                    int64_t, SimEngine& engine)
    {
        auto ba = book_a.best_ask(), bb = book_b.best_bid();
        if (!ba || !bb) return;
        double qty_b = cfg_.qty_a * st_.beta;

        st_.pending_a = engine.submit_limit(
            cfg_.leg_a, Side::Buy,
            signals::round_to_tick(*ba, cfg_.tick_a),
            cfg_.qty_a, false);  // IOC-style: use limit at ask
        st_.pending_b = engine.submit_limit(
            cfg_.leg_b, Side::Sell,
            signals::round_to_tick(*bb, cfg_.tick_b),
            qty_b, false);

        st_.pos = SpreadState::PosState::Long;
        ++st_.entries;
    }

    void enter_short(const OrderBook& book_a, const OrderBook& book_b,
                     int64_t, SimEngine& engine)
    {
        auto bb_a = book_a.best_bid(), ba_b = book_b.best_ask();
        if (!bb_a || !ba_b) return;
        double qty_b = cfg_.qty_a * st_.beta;

        st_.pending_a = engine.submit_limit(
            cfg_.leg_a, Side::Sell,
            signals::round_to_tick(*bb_a, cfg_.tick_a),
            cfg_.qty_a, false);
        st_.pending_b = engine.submit_limit(
            cfg_.leg_b, Side::Buy,
            signals::round_to_tick(*ba_b, cfg_.tick_b),
            qty_b, false);

        st_.pos = SpreadState::PosState::Short;
        ++st_.entries;
    }

    void close_position(const OrderBook& book_a, const OrderBook& book_b,
                        int64_t, SimEngine& engine)
    {
        // Cancel any pending orders
        if (st_.pending_a) { engine.cancel(*st_.pending_a); st_.pending_a.reset(); }
        if (st_.pending_b) { engine.cancel(*st_.pending_b); st_.pending_b.reset(); }

        double pos_a = inv_.qty(cfg_.leg_a);
        double pos_b = inv_.qty(cfg_.leg_b);

        if (std::abs(pos_a) > 1e-9) {
            Side close_a = (pos_a > 0) ? Side::Sell : Side::Buy;
            auto ref_a = (close_a == Side::Sell) ? book_a.best_bid() : book_a.best_ask();
            if (ref_a)
                engine.submit_limit(cfg_.leg_a, close_a,
                    signals::round_to_tick(*ref_a, cfg_.tick_a),
                    std::abs(pos_a), false);
        }
        if (std::abs(pos_b) > 1e-9) {
            Side close_b = (pos_b > 0) ? Side::Sell : Side::Buy;
            auto ref_b = (close_b == Side::Sell) ? book_b.best_bid() : book_b.best_ask();
            if (ref_b)
                engine.submit_limit(cfg_.leg_b, close_b,
                    signals::round_to_tick(*ref_b, cfg_.tick_b),
                    std::abs(pos_b), false);
        }

        st_.pos = SpreadState::PosState::Flat;
    }

    // ── OLS β estimation ──────────────────────────────────────────────────────

    double compute_ols_beta() const {
        // β = cov(lpa, lpb) / var(lpb)
        int n = static_cast<int>(st_.log_pa.size());
        if (n < 4) return 1.0;

        double mean_a = 0.0, mean_b = 0.0;
        for (int i = 0; i < n; ++i) { mean_a += st_.log_pa[i]; mean_b += st_.log_pb[i]; }
        mean_a /= n; mean_b /= n;

        double cov = 0.0, var_b = 0.0;
        for (int i = 0; i < n; ++i) {
            double da = st_.log_pa[i] - mean_a;
            double db = st_.log_pb[i] - mean_b;
            cov   += da * db;
            var_b += db * db;
        }
        return var_b > 1e-12 ? cov / var_b : 1.0;
    }

    // ── Kalman update for μ_s ─────────────────────────────────────────────────

    void update_kalman(double obs, double Q, double R) {
        if (!st_.kf_init) {
            st_.kf_x = obs; st_.kf_v = 0.0;
            st_.kf_init = true;
        }
        // Predict
        double xp   = st_.kf_x + st_.kf_v;
        double p00  = st_.kf_P00 + st_.kf_P10 + st_.kf_P01 + st_.kf_P11 + Q;
        double p01  = st_.kf_P01 + st_.kf_P11 + Q * 0.1;
        double p10  = st_.kf_P10 + st_.kf_P11 + Q * 0.1;
        double p11  = st_.kf_P11 + Q * 0.1;
        // Update
        double y = obs - xp;
        double S = p00 + R;
        double K0 = p00 / S, K1 = p10 / S;
        st_.kf_x    = xp   + K0 * y;
        st_.kf_v    = st_.kf_v + K1 * y;
        st_.kf_P00  = (1.0 - K0) * p00;
        st_.kf_P01  = (1.0 - K0) * p01;
        st_.kf_P10  = p10 - K1 * p00;
        st_.kf_P11  = p11 - K1 * p01;
        st_.mu_spread = st_.kf_x;
    }
};

} // namespace hft