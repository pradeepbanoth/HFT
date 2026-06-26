#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// market_maker.hpp  —  Multi-asset GLFT market-making strategy
//
//  • Guéant–Lehalle–Fernandez-Tapia (2013) optimal quotes
//  • Kalman-filtered fair value (blended with micro-price)
//  • Regime detection via variance-ratio Hurst proxy
//    → trending:       spread × 1.5
//    → mean-reverting: spread × 0.7
//  • PIN proxy toxicity gate: pause quoting when informed flow is high
//  • Stale-quote timeout: hard-cancel any order older than max_quote_age_ms
//  • Depth-proportional quote sizing: qty ≤ depth_fraction × visible level qty
//  • Cross-asset correlation skew (optional correlation matrix)
//  • Rate limiter: min µs between refreshes
// ─────────────────────────────────────────────────────────────────────────────

#include "simulator.hpp"
#include "signals.hpp"
#include <deque>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <cmath>
#include <algorithm>
#include <iostream>

namespace hft {

// ─────────────────────────────────────────────────────────────────────────────
// AssetConfig
// ─────────────────────────────────────────────────────────────────────────────
struct AssetConfig {
    std::string symbol;
    double tick_size        = 0.01;
    double lot_size         = 0.001;
    double quote_qty        = 0.05;
    double max_inventory    = 0.5;
    int    vol_halflife     = 200;       // ticks for EWMA vol
    double as_gamma         = 0.10;
    double as_k             = 1.5;
    double as_T             = 300.0;    // seconds
    double min_spread_bps   = 2.0;
    double max_spread_bps   = 60.0;
    double skew_factor      = 0.0005;
    double toxicity_pause   = 0.75;     // PIN > this → pause quoting
    double max_quote_age_ms = 5000.0;   // hard cancel after this ms
    double min_refresh_us   = 100.0;    // rate limit: min µs between refreshes
    double depth_fraction   = 0.10;     // quote_qty ≤ this × level depth
    double kalman_Q         = 1e-5;
    double kalman_R         = 1.0;
    bool   use_glft         = true;
    int    regime_halflife  = 500;      // ticks for variance-ratio window
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-symbol runtime state
// ─────────────────────────────────────────────────────────────────────────────
struct AssetState {
    static constexpr size_t kMaxHistory = 2000;
    static constexpr size_t kMaxVols    =  500;

    std::deque<double>  mid_history;
    std::deque<double>  buy_vols;
    std::deque<double>  sell_vols;
    std::deque<int64_t> trade_times_ns;
    std::deque<double>  kalman_out;     // last kalman_fair_value output

    std::optional<double>  last_quoted_mid;
    int64_t                last_quote_ts_ns = 0;
    std::optional<std::string> live_bid_id;
    std::optional<std::string> live_ask_id;
    std::unordered_map<std::string, int64_t> quote_placed_ts;

    bool paused = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// MultiAssetMarketMaker
// ─────────────────────────────────────────────────────────────────────────────
class MultiAssetMarketMaker : public Strategy {
public:
    explicit MultiAssetMarketMaker(
        std::vector<AssetConfig>    assets,
        double                      quote_refresh_tol  = 0.0001,
        bool                        enable_cross_hedge = false,
        std::vector<std::vector<double>> correlation   = {}
    )
        : quote_refresh_tol_(quote_refresh_tol)
        , enable_cross_hedge_(enable_cross_hedge)
        , correlation_(std::move(correlation))
    {
        for (auto& a : assets) {
            asset_names_.push_back(a.symbol);
            configs_[a.symbol] = std::move(a);
            states_[a.symbol]  = AssetState{};
        }
    }

    // ── Counters ──────────────────────────────────────────────────────────────
    int64_t quote_count        = 0;
    int64_t cancel_count       = 0;
    int64_t fill_count         = 0;
    int64_t pause_count        = 0;
    int64_t stale_cancel_count = 0;

    // ── Lifecycle ─────────────────────────────────────────────────────────────

    void on_start(SimEngine& engine) override {
        start_ts_ = 0;
    }

    void on_end(SimEngine& engine) override {}

    // ── Book update (main decision point) ────────────────────────────────────

    void on_book_update(const std::string& symbol,
                        OrderBook& book,
                        int64_t ts_ns,
                        SimEngine& engine) override
    {
        auto cfg_it = configs_.find(symbol);
        if (cfg_it == configs_.end()) return;
        if (!book.best_bid() || !book.best_ask()) return;

        const AssetConfig& cfg = cfg_it->second;
        AssetState&        st  = states_[symbol];

        if (start_ts_ == 0) start_ts_ = ts_ns;

        double mid = *book.mid_price();
        st.mid_history.push_back(mid);
        if (st.mid_history.size() > AssetState::kMaxHistory)
            st.mid_history.pop_front();

        // Rate limit
        double elapsed_us = static_cast<double>(ts_ns - st.last_quote_ts_ns) / 1000.0;
        if (elapsed_us < cfg.min_refresh_us && st.last_quoted_mid.has_value()) {
            cancel_stale_quotes(symbol, ts_ns, engine, cfg, st);
            return;
        }

        // Check if mid moved enough to warrant refresh
        if (st.last_quoted_mid.has_value()) {
            double move = std::abs(mid - *st.last_quoted_mid) / mid;
            if (move < quote_refresh_tol_) {
                cancel_stale_quotes(symbol, ts_ns, engine, cfg, st);
                return;
            }
        }

        refresh_quotes(symbol, book, ts_ns, engine, cfg, st);
    }

    void on_trade(const Trade& trade,
                  OrderBook& book,
                  int64_t ts_ns,
                  SimEngine& engine) override
    {
        auto it = states_.find(trade.symbol);
        if (it == states_.end()) return;
        AssetState& st = it->second;

        if (trade.aggressor == Side::Buy) {
            st.buy_vols.push_back(trade.qty);
            if (st.buy_vols.size() > AssetState::kMaxVols) st.buy_vols.pop_front();
        } else {
            st.sell_vols.push_back(trade.qty);
            if (st.sell_vols.size() > AssetState::kMaxVols) st.sell_vols.pop_front();
        }
        st.trade_times_ns.push_back(ts_ns);
        if (st.trade_times_ns.size() > AssetState::kMaxVols) st.trade_times_ns.pop_front();
    }

    void on_fill(const FillEvent& fill,
                 PortfolioState& portfolio,
                 int64_t ts_ns,
                 SimEngine& engine) override
    {
        ++fill_count;
        auto it = states_.find(fill.symbol);
        if (it == states_.end()) return;
        AssetState& st = it->second;

        if (fill.side == Side::Buy)  st.live_bid_id.reset();
        else                          st.live_ask_id.reset();
    }

    void on_order_ack(const Order& order,
                      bool accepted,
                      int64_t ts_ns,
                      SimEngine& engine) override
    {
        if (accepted) return;
        auto it = states_.find(order.symbol);
        if (it == states_.end()) return;
        AssetState& st = it->second;
        if (order.side == Side::Buy)  st.live_bid_id.reset();
        else                           st.live_ask_id.reset();
    }

    void on_risk_breach(const std::vector<std::string>& breaches,
                        int64_t ts_ns,
                        SimEngine& engine) override
    {
        engine.cancel_all();
    }

private:
    std::vector<std::string>                    asset_names_;
    std::unordered_map<std::string, AssetConfig> configs_;
    std::unordered_map<std::string, AssetState>  states_;
    double                                       quote_refresh_tol_;
    bool                                         enable_cross_hedge_;
    std::vector<std::vector<double>>             correlation_;
    int64_t                                      start_ts_ = 0;

    // ── Quote refresh ─────────────────────────────────────────────────────────

    void refresh_quotes(const std::string& symbol,
                        OrderBook& book,
                        int64_t ts_ns,
                        SimEngine& engine,
                        const AssetConfig& cfg,
                        AssetState& st)
    {
        // ① Kalman fair value
        std::vector<double> mids(st.mid_history.begin(), st.mid_history.end());
        double fair = mids.back();
        if (mids.size() >= 5) {
            std::vector<double> kfv(mids.size());
            signals::kalman_fair_value(mids.data(), mids.size(),
                                       cfg.kalman_Q, cfg.kalman_R, kfv.data());
            fair = kfv.back();
        }

        // ② Micro-price (OBI-weighted)
        auto bid_lv = book.bid_depth(10);
        auto ask_lv = book.ask_depth(10);
        size_t nd   = std::min(bid_lv.size(), ask_lv.size());
        if (nd == 0) return;

        std::vector<double> bp(nd), bq(nd), ap(nd), aq(nd);
        for (size_t i = 0; i < nd; ++i) {
            bp[i] = bid_lv[i].price; bq[i] = bid_lv[i].qty;
            ap[i] = ask_lv[i].price; aq[i] = ask_lv[i].qty;
        }
        double mp = signals::micro_price(bp.data(), bq.data(),
                                         ap.data(), aq.data(),
                                         std::min(nd, size_t(5)));
        // Blend Kalman and micro-price
        double fv = 0.6 * fair + 0.4 * mp;

        // ③ EWMA volatility + regime multiplier
        double vol = mids.size() > 10
            ? signals::ewma_vol(mids.data(), mids.size(), cfg.vol_halflife)
            : 0.001;
        double regime_mult = compute_regime_mult(mids, cfg);
        double eff_vol = vol * regime_mult;

        // ④ GLFT / A-S quotes
        double inv  = engine.portfolio().position(symbol);
        double dt   = static_cast<double>(ts_ns - start_ts_) / 1e9;

        double bid_adj, ask_adj;
        if (cfg.use_glft) {
            auto q = signals::glft_optimal_quotes(eff_vol, cfg.as_gamma, inv,
                                                  cfg.max_inventory, cfg.as_T, dt, cfg.as_k);
            bid_adj = q[0]; ask_adj = q[1];
        } else {
            auto q = signals::as_optimal_quotes(eff_vol, cfg.as_gamma, inv,
                                                cfg.as_T, dt, cfg.as_k);
            bid_adj = q[0] - q[1]; ask_adj = q[0] + q[1];
        }

        // ⑤ Inventory skew
        auto sk = signals::inventory_skew_quotes(fv, inv, cfg.max_inventory, cfg.skew_factor);
        bid_adj += sk[0]; ask_adj += sk[1];

        // ⑥ Cross-asset correlation skew
        if (enable_cross_hedge_ && !correlation_.empty()) {
            auto sym_it = std::find(asset_names_.begin(), asset_names_.end(), symbol);
            if (sym_it != asset_names_.end()) {
                size_t idx = std::distance(asset_names_.begin(), sym_it);
                for (size_t j = 0; j < asset_names_.size(); ++j) {
                    if (j == idx) continue;
                    if (idx >= correlation_.size() || j >= correlation_[idx].size()) continue;
                    double corr      = correlation_[idx][j];
                    double other_inv = engine.portfolio().position(asset_names_[j]);
                    auto other_it    = configs_.find(asset_names_[j]);
                    if (other_it == configs_.end()) continue;
                    double other_max = other_it->second.max_inventory;
                    double cross     = corr * (other_inv / std::max(other_max, 1e-12))
                                     * cfg.skew_factor * fv;
                    bid_adj -= cross; ask_adj -= cross;
                }
            }
        }

        // ⑦ Clamp spread
        double raw_bid = fv + bid_adj;
        double raw_ask = fv + ask_adj;
        double min_hs  = fv * (cfg.min_spread_bps / 10000.0);
        double max_hs  = fv * (cfg.max_spread_bps / 10000.0);
        double cur_hs  = (raw_ask - raw_bid) / 2.0;
        if (cur_hs < min_hs) { raw_bid = fv - min_hs; raw_ask = fv + min_hs; }
        if (cur_hs > max_hs) { raw_bid = fv - max_hs; raw_ask = fv + max_hs; }

        // ⑧ Toxicity gate (PIN proxy)
        std::vector<double> bv(st.buy_vols.begin(),  st.buy_vols.end());
        std::vector<double> sv(st.sell_vols.begin(), st.sell_vols.end());
        if (bv.size() > 5 && sv.size() > 5) {
            double tox = signals::pin_proxy(bv.data(), bv.size(), sv.data(), sv.size());
            if (tox > cfg.toxicity_pause) {
                if (!st.paused) { st.paused = true; ++pause_count; }
                cancel_stale_quotes(symbol, ts_ns, engine, cfg, st);
                return;
            }
            st.paused = false;
        }

        // ⑨ Round to tick
        double bid_p = signals::round_to_tick(raw_bid, cfg.tick_size);
        double ask_p = signals::round_to_tick(raw_ask, cfg.tick_size);

        // Guard: never cross inside spread
        double best_ask = *book.best_ask();
        double best_bid = *book.best_bid();
        if (bid_p >= best_ask) bid_p = best_ask - cfg.tick_size;
        if (ask_p <= best_bid) ask_p = best_bid + cfg.tick_size;
        if (bid_p <= 0.0 || ask_p <= bid_p) return;

        // ⑩ Depth-proportional quote sizing
        double bid_avail = bid_lv.empty() ? cfg.quote_qty : bid_lv[0].qty;
        double ask_avail = ask_lv.empty() ? cfg.quote_qty : ask_lv[0].qty;
        double bid_qty = std::clamp(bid_avail * cfg.depth_fraction, cfg.lot_size, cfg.quote_qty);
        double ask_qty = std::clamp(ask_avail * cfg.depth_fraction, cfg.lot_size, cfg.quote_qty);

        // ⑪ Cancel stale quotes
        const auto& open = engine.open_orders();

        auto try_cancel = [&](std::optional<std::string>& oid, double new_price) {
            if (!oid) return;
            auto it = open.find(*oid);
            if (it == open.end()) { oid.reset(); return; }
            if (std::abs(it->second.price - new_price) > cfg.tick_size * 0.5) {
                engine.cancel(*oid);
                ++cancel_count;
                oid.reset();
            }
        };
        try_cancel(st.live_bid_id, bid_p);
        try_cancel(st.live_ask_id, ask_p);

        // ⑫ Submit fresh quotes (inventory guard)
        if (inv < cfg.max_inventory && !st.live_bid_id) {
            std::string oid = engine.submit_limit(symbol, Side::Buy, bid_p, bid_qty, true);
            st.live_bid_id       = oid;
            st.quote_placed_ts[oid] = ts_ns;
            ++quote_count;
        }
        if (inv > -cfg.max_inventory && !st.live_ask_id) {
            std::string oid = engine.submit_limit(symbol, Side::Sell, ask_p, ask_qty, true);
            st.live_ask_id       = oid;
            st.quote_placed_ts[oid] = ts_ns;
            ++quote_count;
        }

        st.last_quoted_mid  = mp;
        st.last_quote_ts_ns = ts_ns;
    }

    // ── Stale-quote timeout ───────────────────────────────────────────────────

    void cancel_stale_quotes(const std::string& symbol,
                              int64_t ts_ns,
                              SimEngine& engine,
                              const AssetConfig& cfg,
                              AssetState& st)
    {
        int64_t max_age_ns = static_cast<int64_t>(cfg.max_quote_age_ms * 1e6);
        for (auto* oid_opt : {&st.live_bid_id, &st.live_ask_id}) {
            if (!oid_opt->has_value()) continue;
            const std::string& oid = **oid_opt;
            auto pt = st.quote_placed_ts.find(oid);
            if (pt != st.quote_placed_ts.end() && ts_ns - pt->second > max_age_ns) {
                engine.cancel(oid);
                ++stale_cancel_count;
                ++cancel_count;
                oid_opt->reset();
            }
        }
    }

    // ── Regime multiplier via variance-ratio Hurst proxy ─────────────────────

    double compute_regime_mult(const std::vector<double>& mids,
                               const AssetConfig& cfg) const noexcept
    {
        size_t n = std::min(mids.size(), static_cast<size_t>(cfg.regime_halflife));
        if (n < 20) return 1.0;
        const double* m = mids.data() + mids.size() - n;
        size_t k        = std::max(size_t(4), n / 8);
        double vr       = signals::variance_ratio(m, n, k);
        if (vr > 1.2) return 1.5;   // trending  → widen
        if (vr < 0.8) return 0.7;   // mean-rev  → tighten
        return 1.0;
    }
};

} // namespace hft