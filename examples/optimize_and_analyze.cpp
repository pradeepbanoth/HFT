// ─────────────────────────────────────────────────────────────────────────────
// examples/optimize_and_analyze.cpp
//
// Full production workflow:
//   1.  1 hour of Heston BTC/USDT synthetic tick data
//   2.  Walk-forward parameter search (random, 20 trials x 4 folds)
//   3.  Best OOS params applied to the full dataset
//   4.  AnalyticsEngine: risk metrics, cost breakdown, latency histograms
//   5.  CSV export: fills + PnL series written to disk
// ─────────────────────────────────────────────────────────────────────────────

#include "../include/hft.hpp"
#include <iostream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <cmath>

using namespace hft;

// ─── Trial function ───────────────────────────────────────────────────────────

static TrialResult run_trial(
    const ParamMap& p,
    const std::vector<MarketEvent>& data,
    int /*fold*/)
{
    AssetConfig cfg;
    cfg.symbol         = "BTCUSDT";
    cfg.tick_size      = 0.01;
    cfg.lot_size       = 0.001;
    cfg.quote_qty      = 0.01;
    cfg.max_inventory  = p.count("max_inv") ? p.at("max_inv") : 0.10;
    cfg.as_gamma       = p.count("gamma")   ? p.at("gamma")   : 0.12;
    cfg.as_k           = p.count("k")       ? p.at("k")       : 1.5;
    cfg.as_T           = 300.0;
    cfg.min_spread_bps = p.count("min_spd") ? p.at("min_spd") : 1.5;
    cfg.max_spread_bps = 40.0;
    cfg.skew_factor    = p.count("skew")    ? p.at("skew")    : 0.0004;
    cfg.toxicity_pause = 0.85;
    cfg.max_quote_age_ms = 4000.0;
    cfg.min_refresh_us = 400.0;
    cfg.use_glft       = true;

    MultiAssetMarketMaker mm({cfg}, 0.0003);

    FillModelConfig fcfg;
    fcfg.maker_fee = -0.0002; fcfg.taker_fee = 0.0004;
    fcfg.fill_mode = FillMode::FIFO;
    fcfg.ac_gamma  = 1e-6; fcfg.ac_eta = 1e-7;
    fcfg.adverse_penalty = 0.5; fcfg.adverse_thresh = 0.70;

    RiskLimits limits;
    limits.max_position["BTCUSDT"] = 0.25;
    limits.max_drawdown   = 0.12;
    limits.halt_on_breach = true;

    SimEngine engine(mm, binance_colocation(), fcfg,
                     50'000.0, limits, 5'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.01);

    auto stats = engine.run(data);
    auto& ps   = stats.portfolio_summary;

    TrialResult r;
    r.sharpe       = std::isfinite(ps.sharpe) ? ps.sharpe : -999.0;
    r.calmar       = std::isfinite(ps.calmar) ? ps.calmar : -999.0;
    r.pnl          = ps.pnl;
    r.pnl_pct      = ps.pnl_pct;
    r.max_drawdown = ps.max_drawdown;
    r.total_fills  = stats.total_fills;
    r.halted       = stats.halted;
    return r;
}

// ─── Full analysis run ────────────────────────────────────────────────────────

static void analyze(
    const ParamMap& params,
    const std::vector<MarketEvent>& data,
    const std::string& label,
    bool write_csv = false)
{
    AssetConfig cfg;
    cfg.symbol         = "BTCUSDT";
    cfg.tick_size      = 0.01;
    cfg.lot_size       = 0.001;
    cfg.quote_qty      = 0.01;
    cfg.max_inventory  = params.count("max_inv") ? params.at("max_inv") : 0.10;
    cfg.as_gamma       = params.count("gamma")   ? params.at("gamma")   : 0.12;
    cfg.as_k           = params.count("k")       ? params.at("k")       : 1.5;
    cfg.as_T           = 300.0;
    cfg.min_spread_bps = params.count("min_spd") ? params.at("min_spd") : 1.5;
    cfg.max_spread_bps = 40.0;
    cfg.skew_factor    = params.count("skew")    ? params.at("skew")    : 0.0004;
    cfg.toxicity_pause = 0.85;
    cfg.max_quote_age_ms = 4000.0;
    cfg.min_refresh_us = 400.0;
    cfg.use_glft       = true;

    MultiAssetMarketMaker mm({cfg}, 0.0003);

    FillModelConfig fcfg;
    fcfg.maker_fee = -0.0002; fcfg.taker_fee = 0.0004;
    fcfg.fill_mode = FillMode::FIFO;
    fcfg.ac_gamma  = 1e-6; fcfg.ac_eta = 1e-7;
    fcfg.adverse_penalty = 0.5; fcfg.adverse_thresh = 0.70;

    RiskLimits limits;
    limits.max_position["BTCUSDT"] = 0.25;
    limits.max_drawdown   = 0.12;
    limits.halt_on_breach = true;

    SimEngine engine(mm, binance_colocation(), fcfg,
                     50'000.0, limits, 1'000'000'000LL);
    engine.add_symbol("BTCUSDT", 0.01);

    auto stats = engine.run(data);

    // Feed analytics
    AnalyticsEngine ae(3'600'000'000'000LL);   // 1-hour buckets
    ae.ingest(stats, engine.fill_history());
    ae.ingest_pnl_series(engine.pnl_series());
    ae.ingest_latency_samples(
        engine.latency_raw_feed(),
        engine.latency_raw_order()
    );

    ae.print_report(label);

    if (write_csv) {
        // Write fills CSV
        {
            std::ofstream f("fills.csv");
            f << ae.fills_to_csv();
            std::cout << "  [CSV] fills.csv written ("
                      << engine.fill_history().size() << " rows)\n";
        }
        // Write PnL series CSV
        {
            std::ofstream f("pnl_series.csv");
            f << ae.pnl_series_to_csv();
            std::cout << "  [CSV] pnl_series.csv written ("
                      << engine.pnl_series().size() << " rows)\n";
        }
    }
}

// ─── main ─────────────────────────────────────────────────────────────────────

int main() {
    std::cout << "===========================================================\n";
    std::cout << " HFT Optimization + Walk-Forward + Analytics Pipeline\n";
    std::cout << "===========================================================\n\n";

    // ── 1. Generate data ──────────────────────────────────────────────────────
    std::cout << "Step 1: Generating 1h of Heston BTC/USDT tick data...\n";

    AssetParams ap;
    ap.symbol          = "BTCUSDT";
    ap.initial_price   = 43'500.0;
    ap.annual_vol      = 0.75;
    ap.vol_of_vol      = 0.40;
    ap.vol_mean        = 0.75;
    ap.vol_vol         = 0.50;
    ap.tick_size       = 0.01;
    ap.spread_mean     = 0.50;
    ap.depth_levels    = 10;
    ap.trade_intensity = 8.0;
    ap.hawkes_alpha    = 0.30;
    ap.hawkes_beta     = 2.00;
    ap.regime_switch_rate = 0.002;
    ap.seed            = 42;

    auto t_gen0 = std::chrono::high_resolution_clock::now();
    SyntheticGenerator gen(ap);
    auto all_data = gen.generate(300.0, 1000);   // 1h @ 100µs intervals
    auto t_gen1   = std::chrono::high_resolution_clock::now();

    {
        int nl2 = 0, ntr = 0;
        for (auto& e : all_data)
            std::visit([&](const auto& ev){
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T,L2Update>) ++nl2;
                if constexpr (std::is_same_v<T,Trade>)    ++ntr;
            }, e);
        std::cout << "  Generated " << all_data.size() << " events in "
                  << std::fixed << std::setprecision(2)
                  << std::chrono::duration<double>(t_gen1-t_gen0).count() << "s\n"
                  << "  L2 updates: " << nl2 << "  Trades: " << ntr << "\n\n";
    }

    // ── 2. Walk-forward optimisation ─────────────────────────────────────────
    std::cout << "Step 2: Walk-forward parameter optimisation (20 trials x 4 folds)...\n\n";

    std::vector<ParamRange> ranges = {
        {"gamma",   0.05,  0.30},
        {"k",       1.0,   2.5 },
        {"min_spd", 1.0,   4.0 },
        {"max_inv", 0.05,  0.20},
        {"skew",    0.0002, 0.001},
    };

    Optimizer::Config opt_cfg;
    opt_cfg.n_threads   = 1;   // increase to hardware_concurrency() for speed
    opt_cfg.verbose     = true;
    opt_cfg.print_top_n = 3;
    opt_cfg.objective   = obj_sharpe_with_fillrate;

    WalkForwardConfig wf_cfg;
    wf_cfg.train_frac = 0.70;
    wf_cfg.test_frac  = 0.30;
    wf_cfg.n_folds    = 4;
    wf_cfg.anchored   = false;

    Optimizer opt(opt_cfg);
    auto t_opt0 = std::chrono::high_resolution_clock::now();
    auto wfr    = opt.walk_forward(ranges, all_data, run_trial, wf_cfg, 20);
    auto t_opt1 = std::chrono::high_resolution_clock::now();

    std::cout << "\n--------------------------------------------------\n";
    std::cout << "Walk-forward complete in "
              << std::setprecision(1)
              << std::chrono::duration<double>(t_opt1-t_opt0).count() << "s\n";
    std::cout << "  OOS Sharpe mean  : "
              << std::setprecision(3) << wfr.oos_sharpe_mean << "\n";
    std::cout << "  OOS PnL mean     : $"
              << std::setprecision(4) << wfr.oos_pnl_mean << "\n";
    std::cout << "  OOS consistency  : "
              << std::setprecision(1) << wfr.oos_consistency * 100 << "% profitable folds\n";
    std::cout << "  Best OOS params  : ";
    for (auto& [k, v] : wfr.best_params)
        std::cout << k << "=" << std::setprecision(4) << v << " ";
    std::cout << "\n--------------------------------------------------\n";

    // ── 3. Baseline analysis (default params) ─────────────────────────────────
    std::cout << "Step 3: Full analytics - baseline (default params)...\n";
    ParamMap default_params = {
        {"gamma",0.12},{"k",1.5},{"min_spd",1.5},{"max_inv",0.10},{"skew",0.0004}
    };
    analyze(default_params, all_data, "Baseline  (default params)", false);

    // ── 4. Optimized analysis (best walk-forward params) ──────────────────────
    if (!wfr.best_params.empty()) {
        std::cout << "Step 4: Full analytics - optimized (best walk-forward params)...\n";
        analyze(wfr.best_params, all_data,
                "Optimized (best walk-forward params)", true);
    }

    // ── 5. FastBook throughput benchmark ──────────────────────────────────────
    std::cout << "\nStep 5: FastBook SIMD throughput benchmark...\n";
    auto bench = benchmark_fast_book(2'000'000);
    std::cout << "  Throughput : " << std::fixed << std::setprecision(2)
              << bench.updates_per_second / 1e6 << "M updates/sec\n";
    std::cout << "  ns/update  : " << bench.ns_per_update << " ns\n";

    std::cout << "\n[Done]\n";
    return 0;
}